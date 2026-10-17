/*
 * hal_pwm_audio board backend: Sonora AK512 and Nano AK506 (dsPIC33AK).
 *
 *   APP_PWM_OUTPUT == 1  MPQ6612A: PG5/DMA4 mono on B or XPRO1,
 *                        optional PG6/DMA5 B+XPRO1 stereo,
 *                        48/96/192 kHz carrier, nSLEEP/nFAULT from mpq6612a_board.h.
 *   APP_PWM_OUTPUT == 0  two MIC4607 half-bridge drivers, stereo: PG5 (L) and
 *                        PG6 (R, triggered by PG5), DMA4/DMA5, 96 kHz carrier,
 *                        each duty held for two carrier periods.
 *
 * Duty is in PG Q4 counts. DMA4 HALF/DONE refills the half it just finished
 * from one core block, for both channels at once; DMA5 runs in lockstep from
 * the same start and has no interrupt of its own.
 */
#include "app_specific_config_defs.h"
#include "nora_pwm_audio_backend.h"

#if NORA_PWM_AUDIO_ENABLE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xc.h>

#include "board/audio/mpq6612a_board.h"
#include "board/audio/pwm_audio_board.h"
#include "board/clock/sonora_clock.h"
#include "nora_dma.h"
#include "nora_gpio.h"
#include "nora_pwm.h"
#include "pwm_audio_dma_buffer.h"   /* PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 */
#include "timer_app.h"
#include <stdio.h>

#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
#error "hal_pwm_audio drives one L/R output; the secondary pair is legacy-only"
#endif

#define BOARD_FACTOR      PWM_AUDIO_UPSAMPLE_FACTOR
#define BOARD_PERIOD_Q4   PWM_AUDIO_PERIOD_COUNT_Q4
#define BOARD_NEUTRAL_Q4  (BOARD_PERIOD_Q4 / 2u)
#define BOARD_HALF_WORDS  (NORA_PWM_AUDIO_FRAMES_PER_BLOCK * BOARD_FACTOR)
#define BOARD_DMA_WORDS   (2u * BOARD_HALF_WORDS)

#if APP_PWM_OUTPUT == 1
#define BOARD_CHANNELS (1u + MPQ6612A_STEREO)
#if PWM_AUDIO_SCOPE_ONLY
#define BOARD_MIN_DUTY_Q4 256u
#else
/* MPQ6612A tMON typ 200 ns; 500 ns initial headroom, rounded UP to whole
 * PWM clocks (the legacy float path rounded down: 399 -> 499.6 ns). */
#define BOARD_MIN_DUTY_Q4 ((((SONORA_CLOCK_PWM_MCLK_HZ) + 1999999u) / 2000000u) << 4)
#endif
#define BOARD_L_RP_H MPQ6612A_IN1_RP
#define BOARD_L_RP_L MPQ6612A_IN2_RP
#if MPQ6612A_STEREO
#define BOARD_R_RP_H MPQ6612A_RIGHT_IN1_RP
#define BOARD_R_RP_L MPQ6612A_RIGHT_IN2_RP
#endif
#define BOARD_DEAD_TIME   0u    /* integrated bridge handles shoot-through */
#else
#define BOARD_CHANNELS 2u
#define BOARD_MIN_DUTY_Q4 256u  /* 0x100: dsPIC33AK512 minimum PG register value */
/* PGxDT plain ticks (not Q4) at 798.72 MHz, ~20 ns. Estimate carried over
 * from the legacy path; see classic_audio_pwm.c. */
#define BOARD_DEAD_TIME   16u
/* Legacy MIC4607 pin map, unchanged. */
#define BOARD_L_RP_H ((nora_gpio_rp_t)28u)
#define BOARD_L_RP_L ((nora_gpio_rp_t)97u)
#define BOARD_R_RP_H ((nora_gpio_rp_t)82u)
#define BOARD_R_RP_L ((nora_gpio_rp_t)98u)
#endif

#if NORA_PWM_AUDIO_MAX_CHANNELS != BOARD_CHANNELS
#error "nora_pwm_audio_conf.h channel count does not match this board"
#endif
#if (BOARD_PERIOD_Q4 % 2u) != 0u || BOARD_MIN_DUTY_Q4 * 2u >= BOARD_PERIOD_Q4
#error "PWM period / minimum pulse cannot form a v1 modulator"
#endif

static volatile uint32_t s_duty_l[BOARD_DMA_WORDS] __attribute__((aligned(4)));
#if BOARD_CHANNELS == 2
static volatile uint32_t s_duty_r[BOARD_DMA_WORDS] __attribute__((aligned(4)));
#endif
static uint8_t s_channels;
static bool s_hw_up;              /* PG/DMA configured since the last stop */
static volatile bool s_released; /* outputs follow the PWM */
static bool s_lab_mode;           /* explicit *cp55xx power-stage bypass */
#if APP_PWM_OUTPUT == 1
#define STAGE_ROUTE_LEFT  0x01u
#if MPQ6612A_STEREO
#define STAGE_ROUTE_RIGHT 0x02u
#define STAGE_ROUTE_ALL   (STAGE_ROUTE_LEFT | STAGE_ROUTE_RIGHT)
#else
#define STAGE_ROUTE_ALL   STAGE_ROUTE_LEFT
#endif
static uint8_t s_route_mask;      /* routes woken and released on this start */
#endif
static volatile uint32_t s_half, s_done, s_dma_errors, s_phase_errors, s_overlaps;
#if PWM_AUDIO_SCOPE_ONLY
static volatile uint32_t s_duty_min[BOARD_CHANNELS], s_duty_max[BOARD_CHANNELS];
#endif

#if APP_PWM_OUTPUT == 1
static void stage_sleep_all(void)
{
    (void)nora_gpio_rp_clear(MPQ6612A_SLEEP_RP);
#if MPQ6612A_STEREO
    (void)nora_gpio_rp_clear(MPQ6612A_RIGHT_SLEEP_RP);
#endif
}

static uint8_t stage_requested_mask(void)
{
    uint8_t mask = STAGE_ROUTE_LEFT;
#if MPQ6612A_STEREO
    if (s_channels == 2u) mask |= STAGE_ROUTE_RIGHT;
#endif
    return mask;
}

/* Probe the board's external nFAULT pull-up while nSLEEP is Low. The dsPIC
 * pull-down is 10 kOhm, the same as the MikroBUS board's pull-up, so it cannot
 * provide a valid logic level. Instead, discharge the open-drain net briefly,
 * return it to input and allow the external pull-up 2 us to restore High. */
static bool stage_probe_route(nora_gpio_rp_t fault_rp, uint8_t route,
                              uint8_t *available)
{
    if (!nora_gpio_rp_config_digital_output(fault_rp, false)) return false;
    delay_us(2u);
    if (!nora_gpio_rp_config_digital_input(fault_rp)) return false;
    delay_us(2u);
    if (nora_gpio_rp_read(fault_rp) == NORA_GPIO_LEVEL_HIGH)
        *available |= route;
    return true;
}

/* A Low result is either an absent board or an asserted fault; both are kept
 * asleep and overridden. High is a route-presence proxy, not proof of VIN. */
static bool stage_available_mask(uint8_t requested, uint8_t *available)
{
    *available = 0u;
    if ((requested & STAGE_ROUTE_LEFT) != 0u &&
        !stage_probe_route(MPQ6612A_FAULT_RP, STAGE_ROUTE_LEFT, available))
        return false;
#if MPQ6612A_STEREO
    if ((requested & STAGE_ROUTE_RIGHT) != 0u &&
        !stage_probe_route(MPQ6612A_RIGHT_FAULT_RP, STAGE_ROUTE_RIGHT, available))
        return false;
#endif
    return true;
}

static void stage_wake_routes(uint8_t mask)
{
    if ((mask & STAGE_ROUTE_LEFT) != 0u)
        (void)nora_gpio_rp_set(MPQ6612A_SLEEP_RP);
#if MPQ6612A_STEREO
    if ((mask & STAGE_ROUTE_RIGHT) != 0u)
        (void)nora_gpio_rp_set(MPQ6612A_RIGHT_SLEEP_RP);
#endif
}

static bool stage_fault(void)
{
#if PWM_AUDIO_SCOPE_ONLY
    return false; /* Diagnostics are inactive while both bridges sleep. */
#else
    if (s_lab_mode) return false;
    if ((s_route_mask & STAGE_ROUTE_LEFT) != 0u &&
        nora_gpio_rp_read(MPQ6612A_FAULT_RP) != NORA_GPIO_LEVEL_HIGH)
        return true;
#if MPQ6612A_STEREO
    if ((s_route_mask & STAGE_ROUTE_RIGHT) != 0u &&
        nora_gpio_rp_read(MPQ6612A_RIGHT_FAULT_RP) != NORA_GPIO_LEVEL_HIGH)
        return true;
#endif
    return false;
#endif
}
#endif

static void fill_neutral(volatile uint32_t *buf, uint16_t from, uint16_t count)
{
    uint16_t i;
    for (i = 0u; i < count; ++i) buf[from + i] = BOARD_NEUTRAL_Q4;
}

/* EMERGENCY_SAFE: no waits, callable from any ISR. A foreground override
 * read-modify-write it interrupts may be lost; the stage is already off by
 * then on MPQ, and both pins are forced again on the next stop(). */
static void board_emergency_safe(void)
{
    s_released = false;
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();   /* both bridges off first */
    s_route_mask = 0u;
#endif
    (void)nora_pwm_generator_override(NORA_PWM_GEN_5, true, false, false);
#if BOARD_CHANNELS == 2
    (void)nora_pwm_generator_override(NORA_PWM_GEN_6, true, false, false);
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
}

/* The completed half must be inactive on every DMA before and after copy.
 * Both channels use PG5's event, with at most one word arbitration distance. */
static bool dma_copy_window(uint16_t offset, bool reserve)
{
    uint32_t left = (nora_dma_read_src(NORA_DMA_CHANNEL_4) -
                     (uint32_t)s_duty_l) / sizeof(uint32_t);
    if (left >= BOARD_DMA_WORDS || left / BOARD_HALF_WORDS == offset / BOARD_HALF_WORDS)
        return false;
#if BOARD_CHANNELS == 2
    if (s_channels == 2u) {
        uint32_t right = (nora_dma_read_src(NORA_DMA_CHANNEL_5) -
                          (uint32_t)s_duty_r) / sizeof(uint32_t);
        uint32_t distance = left > right ? left - right : right - left;
        if (right >= BOARD_DMA_WORDS || (distance > 1u && distance != BOARD_DMA_WORDS - 1u)) {
            ++s_phase_errors;
            return false;
        }
        if (right / BOARD_HALF_WORDS == offset / BOARD_HALF_WORDS) return false;
        if (reserve && BOARD_HALF_WORDS - right % BOARD_HALF_WORDS <= 4u) return false;
    }
#endif
    return !reserve || BOARD_HALF_WORDS - left % BOARD_HALF_WORDS > 4u;
}

static void refill_half(uint16_t offset)
{
    const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS];
    bool have_data;
    uint16_t i, k, w;
    /* DMA4's IRQ can beat DMA5's last transfer by a few bus cycles. */
    unsigned attempt = 0;
    while (!dma_copy_window(offset, true) && ++attempt < 32u) {}
    if (attempt == 32u) {
        ++s_overlaps;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    have_data = nora_pwm_audio_core_take(duty);
    w = offset;
#if PWM_AUDIO_SCOPE_ONLY
    for (unsigned ch = 0; ch < BOARD_CHANNELS; ++ch) {
        s_duty_min[ch] = BOARD_PERIOD_Q4;
        s_duty_max[ch] = 0;
    }
#endif
    for (i = 0u; i < NORA_PWM_AUDIO_FRAMES_PER_BLOCK; ++i) {
        uint32_t l = have_data ? duty[0][i] : BOARD_NEUTRAL_Q4;
#if BOARD_CHANNELS == 2
        uint32_t r = have_data && s_channels == 2u ? duty[1][i] : BOARD_NEUTRAL_Q4;
#endif
        for (k = 0u; k < BOARD_FACTOR; ++k, ++w) {
            s_duty_l[w] = l;
#if BOARD_CHANNELS == 2
            s_duty_r[w] = r;
#endif
        }
#if PWM_AUDIO_SCOPE_ONLY
        if (l < s_duty_min[0]) s_duty_min[0] = l;
        if (l > s_duty_max[0]) s_duty_max[0] = l;
#if BOARD_CHANNELS == 2
        if (r < s_duty_min[1]) s_duty_min[1] = r;
        if (r > s_duty_max[1]) s_duty_max[1] = r;
#endif
#endif
    }
    if (have_data) nora_pwm_audio_core_release();
    if (!dma_copy_window(offset, false)) {
        ++s_overlaps;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
    }
}

void __attribute__((interrupt, context)) _DMA4Interrupt(void)
{
    nora_dma_status_t status = nora_dma_isr_snapshot(NORA_DMA_CHANNEL_4);
    nora_dma_half_t half;
    bool bad = nora_dma_status_has_overrun(status) ||
               nora_dma_status_has_half_done_conflict(status);
#if BOARD_CHANNELS == 2
    if (s_channels == 2u) {
        nora_dma_status_t status_r = nora_dma_read_status(NORA_DMA_CHANNEL_5);
        nora_dma_clear_status(NORA_DMA_CHANNEL_5);
        bad = bad || nora_dma_status_has_overrun(status_r);
    }
#endif
    if (bad) {
        ++s_dma_errors;
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    half = nora_dma_half_from_status(status);
    if (half == NORA_DMA_HALF_FIRST) { ++s_half; refill_half(0u); }
    else if (half == NORA_DMA_HALF_SECOND) { ++s_done; refill_half(BOARD_HALF_WORDS); }
}

static bool board_dma_config(nora_dma_channel_t ch, nora_dma_trigger_t trigger,
                             volatile uint32_t *duty_buf, volatile uint32_t *pg_dc,
                             bool irq)
{
    nora_dma_channel_cfg_t cfg = { 0 };

    cfg.src          = (volatile void *)duty_buf;
    cfg.dst          = (volatile void *)pg_dc;
    cfg.count        = BOARD_DMA_WORDS;
    cfg.src_mode     = NORA_DMA_ADDR_INCREMENT;
    cfg.dst_mode     = NORA_DMA_ADDR_FIXED;
    cfg.size         = NORA_DMA_SIZE_WORD;
    cfg.tr_mode      = NORA_DMA_TRMODE_REPEAT_ONESHOT;
    cfg.reload_count = true;
    cfg.reload_src   = true;
    cfg.reload_dst   = false;
    cfg.half_int_en  = irq;
    cfg.done_int_en  = irq;
    cfg.trigger      = trigger;
    cfg.irq_priority_set = false;   /* as the legacy path */
    cfg.irq_enable   = irq;
    return nora_dma_channel_config(ch, &cfg) && nora_dma_channel_enable(ch, true);
}

static bool board_pg_init(nora_pwm_generator_id_t id, nora_gpio_rp_t rp_h,
                          nora_gpio_rp_t rp_l, bool follow_pg5)
{
    nora_pwm_generator_cfg_t cfg = { 0 };

    cfg.period            = BOARD_PERIOD_Q4;
    cfg.duty_init         = BOARD_NEUTRAL_Q4;
    cfg.dead_time_high    = BOARD_DEAD_TIME;
    cfg.dead_time_low     = BOARD_DEAD_TIME;
    cfg.rp_h              = rp_h;
    cfg.rp_l              = rp_l;
    cfg.pen_h             = true;
    cfg.pen_l             = true;
    cfg.output_mode       = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows   = follow_pg5;
    cfg.trigger_master    = NORA_PWM_GEN_5;
    cfg.is_module_master  = true;
    /* Both pins low (MPQ: coast Z/Z; MIC4607: both FETs off) until release. */
    cfg.start_overridden  = true;
    cfg.start_disabled    = true;
    return nora_pwm_generator_init(id, &cfg);
}

/* Preserve the proven CODEC XTAL -> REFI1 -> PLL2 -> CLKGEN5 PWM clock. */
static bool board_clock_ready(void)
{
    static bool prepared;
    if (!prepared) {
        nora_clock_status_t detail;
        if (sonora_clock_pwm_prepare(&detail) != SONORA_CLOCK_PWM_OK ||
            !nora_pwm_module_init(NORA_PWM_MCLK_HIGH_FREQ)) return false;
        prepared = true;
    }
    return OSCCTRLbits.PLL2RDY && CLK5CONbits.CLKRDY;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_init(uint8_t channel_count,
                                                    nora_pwm_audio_modulator_t *mod)
{
    if (channel_count == 0u || channel_count > BOARD_CHANNELS) return NORA_PWM_AUDIO_UNSUPPORTED;
#if APP_PWM_OUTPUT == 1
    /* Keep the bridge asleep from here on, whatever happens next. */
    if (!nora_gpio_rp_config_digital_output(MPQ6612A_SLEEP_RP, false) ||
        !nora_gpio_rp_config_digital_input(MPQ6612A_FAULT_RP)) {
        stage_sleep_all();
        return NORA_PWM_AUDIO_FAULT;
    }
#if MPQ6612A_STEREO
    if (!nora_gpio_rp_config_digital_output(MPQ6612A_RIGHT_SLEEP_RP, false) ||
        !nora_gpio_rp_config_digital_input(MPQ6612A_RIGHT_FAULT_RP)) {
        stage_sleep_all();
        return NORA_PWM_AUDIO_FAULT;
    }
    if (channel_count == 1u &&
        (!nora_gpio_rp_config_digital_output(BOARD_R_RP_H, false) ||
         !nora_gpio_rp_config_digital_output(BOARD_R_RP_L, false))) return NORA_PWM_AUDIO_FAULT;
#endif
#else
    /* A stereo board run mono: hold the unused R driver inputs low. */
    if (channel_count == 1u &&
        (!nora_gpio_rp_config_digital_output(BOARD_R_RP_H, false) ||
         !nora_gpio_rp_config_digital_output(BOARD_R_RP_L, false))) {
        return NORA_PWM_AUDIO_FAULT;
    }
#endif
    if (!nora_pwm_audio_modulator_setup(mod, BOARD_PERIOD_Q4, BOARD_MIN_DUTY_Q4)) {
        return NORA_PWM_AUDIO_FAULT;
    }
    s_channels = channel_count;
    return NORA_PWM_AUDIO_OK;
}

nora_pwm_audio_result_t nora_pwm_audio_backend_start(void)
{
    bool ok;
    nora_pwm_audio_status_t st;
    s_released = false;
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();
    s_route_mask = stage_requested_mask();
#if !PWM_AUDIO_SCOPE_ONLY
    if (!s_lab_mode) {
        uint8_t available;
        if (!stage_available_mask(s_route_mask, &available)) {
            s_route_mask = 0u;
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
            return NORA_PWM_AUDIO_FAULT;
        }
        s_route_mask = available;
    }
    if (s_route_mask == 0u) {
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
        return NORA_PWM_AUDIO_FAULT;
    }
#endif
#endif
    /* Own all resources before any partial failure can occur. */
    s_hw_up = true;
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
#if BOARD_CHANNELS == 2
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
    fill_neutral(s_duty_l, 0u, BOARD_DMA_WORDS);
#if BOARD_CHANNELS == 2
    fill_neutral(s_duty_r, 0u, BOARD_DMA_WORDS);
#endif
    if (!board_clock_ready()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
        return NORA_PWM_AUDIO_FAULT;
    }
    ok = board_pg_init(NORA_PWM_GEN_5, BOARD_L_RP_H, BOARD_L_RP_L, false);
#if BOARD_CHANNELS == 2
    if (ok && s_channels == 2u) {
        ok = board_pg_init(NORA_PWM_GEN_6, BOARD_R_RP_H, BOARD_R_RP_L, true) &&
             board_dma_config(NORA_DMA_CHANNEL_5, NORA_DMA_TRIGGER_PWM_GEN5,
                              s_duty_r, &PG6DC, false);
    }
#endif
    if (ok) ok = board_dma_config(NORA_DMA_CHANNEL_4, NORA_DMA_TRIGGER_PWM_GEN5,
                                 s_duty_l, &PG5DC, true);
#if BOARD_CHANNELS == 2
    if (ok && s_channels == 2u) ok = nora_pwm_generator_run(NORA_PWM_GEN_6, true);
#endif
    if (ok) ok = nora_pwm_generator_run(NORA_PWM_GEN_5, true);
    if (!ok) goto fail;
#if APP_PWM_OUTPUT == 1
    if (!PWM_AUDIO_SCOPE_ONLY || s_lab_mode) {
        nora_pwm_audio_get_status(&st);
        if (st.fault_flags) goto fail;
        stage_wake_routes(s_route_mask);
        delay_us(1000u); /* coast wake: do not enter low-IQ brake */
        if (stage_fault()) {
            board_emergency_safe();
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
            goto fail;
        }
    }
#endif
    nora_pwm_audio_get_status(&st);
    if (st.fault_flags) goto fail;
#if BOARD_CHANNELS == 2
#if APP_PWM_OUTPUT == 1
    if (s_channels == 2u && (s_route_mask & STAGE_ROUTE_RIGHT) != 0u &&
#else
    if (s_channels == 2u &&
#endif
        !nora_pwm_generator_override(NORA_PWM_GEN_6, false, false, true)) goto fail;
#endif
#if APP_PWM_OUTPUT == 1
    if ((s_route_mask & STAGE_ROUTE_LEFT) != 0u &&
        !nora_pwm_generator_override(NORA_PWM_GEN_5, false, false, true)) goto fail;
#else
    if (!nora_pwm_generator_override(NORA_PWM_GEN_5, false, false, true)) goto fail;
#endif
    nora_pwm_audio_get_status(&st);
    if (st.fault_flags) goto fail;
    pwm_audio_board_service(); /* establish progress baseline before release */
    s_released = true;
    return NORA_PWM_AUDIO_OK;
fail:
    board_emergency_safe();
    nora_pwm_audio_backend_stop();
    return NORA_PWM_AUDIO_FAULT;
}

void nora_pwm_audio_backend_stop(void)
{
    s_released = false; /* core has stopped producer before calling backend */
#if APP_PWM_OUTPUT == 1
    if (!s_hw_up) { stage_sleep_all(); s_route_mask = 0u; return; }
#else
    if (!s_hw_up) return;
#endif
    s_hw_up = false;
    (void)nora_pwm_generator_override(NORA_PWM_GEN_5, true,
                                     APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY, true);
#if BOARD_CHANNELS == 2
    (void)nora_pwm_generator_override(NORA_PWM_GEN_6, true,
                                     APP_PWM_OUTPUT == 1 && !PWM_AUDIO_SCOPE_ONLY, true);
#endif
    delay_us(50u);
#if APP_PWM_OUTPUT == 1
    stage_sleep_all();
    s_route_mask = 0u;
    delay_us(1000u);
#endif
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
#if BOARD_CHANNELS == 2
    (void)nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    (void)nora_pwm_generator_run(NORA_PWM_GEN_6, false);
#endif
    (void)nora_pwm_generator_run(NORA_PWM_GEN_5, false);
}

void pwm_audio_board_service(void)
{
    static uint32_t last_ms, last_events;
    uint32_t now = GetTicks();
    if (!s_released) { last_ms = now; last_events = s_half + s_done; return; }
    if (!PG5CONbits.ON || !DMA4CHbits.CHEN
#if BOARD_CHANNELS == 2
        || (s_channels == 2u && (!PG6CONbits.ON || !DMA5CHbits.CHEN))
#endif
        ) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
        return;
    }
    if ((uint32_t)(now - last_ms) >= 1000u) {
        uint32_t events = s_half + s_done;
        if (events == last_events) {
            board_emergency_safe();
            nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_DMA);
            return;
        }
        last_events = events;
        last_ms = now;
    }
    if (!OSCCTRLbits.PLL2RDY || !CLK5CONbits.CLKRDY) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_CLOCK);
    }
#if APP_PWM_OUTPUT == 1
    if (s_released && stage_fault()) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
    }
#if PWM_AUDIO_SCOPE_ONLY
    if (s_released && !s_lab_mode &&
        (nora_gpio_rp_read(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW ||
         nora_gpio_rp_read_output(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
#if MPQ6612A_STEREO
         || nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
         || nora_gpio_rp_read_output(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
#endif
        )) {
        board_emergency_safe();
        nora_pwm_audio_core_fault(NORA_PWM_AUDIO_FAULT_STAGE);
    }
#endif
#endif
}

void pwm_audio_board_set_lab_mode(bool enabled)
{
    s_lab_mode = enabled;
}

void pwm_audio_board_control_print(void)
{
    nora_pwm_audio_status_t st;
    nora_pwm_audio_get_status(&st);
    printf(" PWMcontrol lab=%u pg=%u dma=%u fault=%lu written=%lu underrun=%lu",
        (unsigned)s_lab_mode, (unsigned)PG5CONbits.ON,
        (unsigned)DMA4CHbits.CHEN, (unsigned long)st.fault_flags,
        (unsigned long)st.written_blocks, (unsigned long)st.underrun_blocks);
#if APP_PWM_OUTPUT == 1
    printf(" active=0x%X nSLEEP=%u nFAULT=%u", (unsigned)s_route_mask,
        (unsigned)(nora_gpio_rp_read_output(MPQ6612A_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)(nora_gpio_rp_read(MPQ6612A_FAULT_RP) == NORA_GPIO_LEVEL_HIGH));
#if MPQ6612A_STEREO
    printf(" nSLEEP_R=%u nFAULT_R=%u",
        (unsigned)(nora_gpio_rp_read_output(MPQ6612A_RIGHT_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)(nora_gpio_rp_read(MPQ6612A_RIGHT_FAULT_RP) == NORA_GPIO_LEVEL_HIGH));
#endif
#endif
    printf(" carrier=%lu per=%lu pll2=%u clk5=%u err=%lu overlap=%lu phase_err=%lu\n",
        (unsigned long)(SAMPLE_RATE * BOARD_FACTOR), (unsigned long)PG5PER,
        (unsigned)OSCCTRLbits.PLL2RDY, (unsigned)CLK5CONbits.CLKRDY,
        (unsigned long)s_dma_errors, (unsigned long)s_overlaps, (unsigned long)s_phase_errors);
}

void pwm_audio_board_scope_print(bool running, bool neutral)
{
#if PWM_AUDIO_SCOPE_ONLY
    static uint32_t last_ms;
    uint32_t now = GetTicks();
    nora_pwm_audio_status_t st;
    if ((uint32_t)(now - last_ms) < 1000u) return;
    last_ms = now;
    nora_pwm_audio_get_status(&st);
    printf(" PWMhal ms=%lu ready=%u pll2=%u clk5=%u pg=%u dma=%u half=%lu done=%lu err=%lu overlap=%lu phase_err=%lu written=%lu rejected=%lu underrun=%lu fault=%lu source=%s\n",
        (unsigned long)now, (unsigned)(running && s_released),
        (unsigned)OSCCTRLbits.PLL2RDY, (unsigned)CLK5CONbits.CLKRDY,
        (unsigned)PG5CONbits.ON, (unsigned)DMA4CHbits.CHEN,
        (unsigned long)s_half, (unsigned long)s_done, (unsigned long)s_dma_errors,
        (unsigned long)s_overlaps, (unsigned long)s_phase_errors,
        (unsigned long)st.written_blocks, (unsigned long)st.rejected_blocks,
        (unsigned long)st.underrun_blocks, (unsigned long)st.fault_flags,
        neutral ? "neutral" : "audio");
    printf(" PWMhalL sleep=%u duty=%lu/%lu per=%lu route=%s\n",
        (unsigned)(nora_gpio_rp_read(MPQ6612A_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned long)s_duty_min[0], (unsigned long)s_duty_max[0], (unsigned long)BOARD_PERIOD_Q4,
        MPQ6612A_ENABLE_MIKROB ? "mikrob" : "xpro1");
#if MPQ6612A_STEREO
    printf(" PWMhalR sleep=%u pg=%u dma=%u duty=%lu/%lu\n",
        (unsigned)(nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)PG6CONbits.ON, (unsigned)DMA5CHbits.CHEN,
        (unsigned long)s_duty_min[1], (unsigned long)s_duty_max[1]);
#endif
#else
    (void)running; (void)neutral;
#endif
}

#else
typedef int pwm_audio_board_ak_unused_t;   /* ISO C: no empty translation unit */
#endif /* NORA_PWM_AUDIO_ENABLE */
