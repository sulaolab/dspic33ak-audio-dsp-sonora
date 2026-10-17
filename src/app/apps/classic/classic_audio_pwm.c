#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_audio_pwm.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <xc.h>

#include "classic_audio_pwm.h"
#include "timer_app.h"
#include "board/audio/mpq6612a_board.h"
#include "nora_pwm.h"   /* hal_pwm: PG generator register HAL */
#include "nora_dma.h"   /* hal_dma: DMA channel register HAL */
#include "apps/shared/float_conversion.h"
#include "pwm_audio_dma_buffer.h"
#if PWM_AUDIO_SCOPE_ONLY
#include "audio_transport/audio_transport_snapshot.h"
#endif

#if APP_USE_PWM_AUDIO && APP_PWM_AUDIO_HAL
#include "nora_pwm_audio.h"
#include "board/audio/pwm_audio_board.h"
#include "board/audio/nora_pwm_audio_conf.h"

/* hal_pwm_audio path. The block is converted with Post_Gain_PWM applied
 * before saturation, then copied by the HAL; the scratch is reused at once. */
static int32_t s_pwm_q31_l[APP_BLOCK_FRAMES];
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
static int32_t s_pwm_q31_r[APP_BLOCK_FRAMES];
#endif

static bool s_hal_running;
#if PWM_AUDIO_SCOPE_ONLY
static bool s_hal_neutral;
#endif

void classic_audio_pwm_init(void)
{
    const nora_pwm_audio_config_t cfg = {
        .sample_rate_hz   = SAMPLE_RATE,
        .frames_per_block = APP_BLOCK_FRAMES,
        .channel_count    = NORA_PWM_AUDIO_MAX_CHANNELS,
    };
    nora_pwm_audio_result_t r = nora_pwm_audio_init(&cfg);
    if (r == NORA_PWM_AUDIO_OK) r = nora_pwm_audio_start();
    s_hal_running = r == NORA_PWM_AUDIO_OK;
    if (r != NORA_PWM_AUDIO_OK) {
        printf(" WARNING: classic_audio_pwm_init: hal_pwm_audio result %d\n", (int)r);
    }
}

bool classic_audio_pwm_start(void)
{
    s_hal_running = nora_pwm_audio_start() == NORA_PWM_AUDIO_OK;
    return s_hal_running;
}

void classic_audio_pwm_stop(void)
{
    s_hal_running = false;
    nora_pwm_audio_stop();
}

bool classic_audio_pwm_fault_active(void)
{
    nora_pwm_audio_status_t st;
    pwm_audio_board_service();
    nora_pwm_audio_get_status(&st);
    return st.fault_flags != 0u;
}

bool classic_audio_pwm_lab_control(bool enabled)
{
    classic_audio_pwm_stop();
    pwm_audio_board_set_lab_mode(enabled);
    if (enabled) {
        /* init clears a fault latched by startup without a powered PCB. */
        classic_audio_pwm_init();
        return s_hal_running;
    }
    return true;
}

void classic_audio_pwm_control_print(void)
{
    pwm_audio_board_control_print();
}

void classic_audio_pwm_hal_process( const float* in, int num_proc_ch )
{
    nora_pwm_audio_block_t block;
    if (!in || num_proc_ch < NORA_PWM_AUDIO_MAX_CHANNELS) return;
    /* 2ch: L = slot 0, R = slot 1 (as process_left/right_primary). */
    convert_float_to_pwm_q31( in, 0u, s_pwm_q31_l, APP_BLOCK_FRAMES );
    block.left_q31 = s_pwm_q31_l;
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
    convert_float_to_pwm_q31( in, 1u, s_pwm_q31_r, APP_BLOCK_FRAMES );
    block.right_q31 = s_pwm_q31_r;
#else
    block.right_q31 = NULL;
#endif
    (void)nora_pwm_audio_write( &block );   /* BUSY/NOT_READY: counted by the HAL */
}

/* Scope uses the completed TDM TX Q31 values, without applying PWM gain again. */
void classic_audio_pwm_scope_process_pcm(const int32_t* pcm, unsigned stride)
{
#if PWM_AUDIO_SCOPE_ONLY
    nora_pwm_audio_block_t block = {s_pwm_q31_l, NULL};
    if (!pcm || stride < NORA_PWM_AUDIO_MAX_CHANNELS) return;
    for (unsigned i = 0; i < APP_BLOCK_FRAMES; ++i) {
        s_pwm_q31_l[i] = s_hal_neutral ? 0 : pcm[i * stride];
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
        s_pwm_q31_r[i] = s_hal_neutral ? 0 : pcm[i * stride + 1u];
#endif
    }
#if NORA_PWM_AUDIO_MAX_CHANNELS == 2
    block.right_q31 = s_pwm_q31_r;
#endif
    (void)nora_pwm_audio_write(&block);
#else
    (void)pcm; (void)stride;
#endif
}
void classic_audio_pwm_scope_set_silence(bool silence)
{
#if PWM_AUDIO_SCOPE_ONLY
    s_hal_neutral = silence;
#else
    (void)silence;
#endif
}
void classic_audio_pwm_scope_service(void) { pwm_audio_board_service(); }
void classic_audio_pwm_scope_print(void)
{
#if PWM_AUDIO_SCOPE_ONLY
    pwm_audio_board_scope_print(s_hal_running, s_hal_neutral);
#endif
}

void classic_audio_pwm_process_left_primary( float* in, int n ) { (void)in; (void)n; }
void classic_audio_pwm_process_right_primary( float* in, int n ) { (void)in; (void)n; }
void classic_audio_pwm_process_left_secondary( float* in, int n ) { (void)in; (void)n; }
void classic_audio_pwm_process_right_secondary( float* in, int n ) { (void)in; (void)n; }

#else /* legacy direct float->duty path */

void classic_audio_pwm_hal_process( const float* in, int num_proc_ch )
{
    (void)in; (void)num_proc_ch;
}

#if (APP_TARGET == APP_TARGET_AK512) || (defined(APP_TARGET_AK506) && APP_TARGET == APP_TARGET_AK506)

#define PG_PER_COUNT     PWM_AUDIO_PERIOD_COUNT_Q4
#define PG_DC_INIT       (PG_PER_COUNT / 2u)

/* Dead time, PGxDT (plain ticks @ 798.72MHz master clock, NOT Q4 -- unlike
 * PER/DC): 1 tick ~= 1.252ns.
 *
 * ESTIMATE, NOT HW-VALIDATED. No FET driver IC is selected yet. 16 ticks ~=
 * 20.03ns sits at the safer/larger end of the ~10-20ns typical for a
 * Class-D-style H-bridge driver at this switching speed -- favors avoiding
 * shoot-through over maximizing duty headroom, appropriate for a placeholder
 * nothing has measured. Re-check against the chosen driver's datasheet
 * before first power-up.
 *
 * The previous placeholder (8 ticks) came with a "2us -> 1597 ticks" target
 * comment left over from when the carrier was 48kHz (PWM_AUDIO_UPSAMPLE_
 * FACTOR 1, ~16640 raw ticks/period): at 480kHz (factor 10, ~1664 raw
 * ticks/period) that target would consume ~96% of the period and leave no
 * usable duty range. 16 ticks costs ~1% of the period per edge instead. */
#define AUDIO_DEAD_TIME  (16u)

static volatile bool s_pwm_ready;
static bool s_pwm_initialized;
#if APP_PWM_OUTPUT == 1
static bool s_pwm_stopped = true;
static volatile bool s_pwm_stream_fault;
#endif
#if PWM_AUDIO_SCOPE_ONLY
typedef struct {
    uint32_t half, done, dma_errors, pcm_peak, pcm_blocks;
    uint32_t duty_min, duty_max, writes, write_overlap;
} pwm_scope_channel_t;
static volatile pwm_scope_channel_t s_scope[1u + MPQ6612A_STEREO];
static uint32_t s_scope_failures;
static volatile bool s_scope_silence;
#endif

static void pwm_dma_observe(unsigned channel, nora_dma_status_t status)
{
#if APP_PWM_OUTPUT == 1
    if (channel < 4u || channel >= 5u + MPQ6612A_STEREO) return;
    bool error = nora_dma_status_has_overrun(status) ||
                 nora_dma_status_has_half_done_conflict(status);
    if (error) s_pwm_stream_fault = true;
#if PWM_AUDIO_SCOPE_ONLY
    unsigned index = channel - 4u;
    if (error) ++s_scope[index].dma_errors;
    if (nora_dma_half_from_status(status) == NORA_DMA_HALF_FIRST) ++s_scope[index].half;
    if (nora_dma_half_from_status(status) == NORA_DMA_HALF_SECOND) ++s_scope[index].done;
#endif
#else
    (void)channel; (void)status;
#endif
}
static bool pwm5_init(void);
static bool pwm6_init(void);
static bool pwm7_init(void);
static bool pwm8_init(void);

volatile uint32_t
    g_pwm_audio_pg5_duty[PWM_AUDIO_DUTY_WORD_COUNT] __attribute__((aligned(4)));
volatile uint32_t
    g_pwm_audio_pg6_duty[PWM_AUDIO_DUTY_WORD_COUNT] __attribute__((aligned(4)));
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
volatile uint32_t
    g_pwm_audio_pg7_duty[PWM_AUDIO_DUTY_WORD_COUNT] __attribute__((aligned(4)));
volatile uint32_t
    g_pwm_audio_pg8_duty[PWM_AUDIO_DUTY_WORD_COUNT] __attribute__((aligned(4)));
#endif

#if APP_PWM_OUTPUT == 1
typedef struct {
    nora_pwm_generator_id_t generator;
    nora_dma_channel_t dma;
    nora_dma_trigger_t trigger;
    nora_gpio_rp_t sleep, fault;
    volatile uint32_t *buffer, *dc;
} mpq_channel_t;
static const mpq_channel_t s_mpq[1u + MPQ6612A_STEREO] = {
    {NORA_PWM_GEN_5, NORA_DMA_CHANNEL_4, NORA_DMA_TRIGGER_PWM_GEN5,
     MPQ6612A_SLEEP_RP, MPQ6612A_FAULT_RP, g_pwm_audio_pg5_duty, &PG5DC},
#if MPQ6612A_STEREO
    {NORA_PWM_GEN_6, NORA_DMA_CHANNEL_5, NORA_DMA_TRIGGER_PWM_GEN5,
     MPQ6612A_RIGHT_SLEEP_RP, MPQ6612A_RIGHT_FAULT_RP, g_pwm_audio_pg6_duty, &PG6DC},
#endif
};
#define MPQ_CHANNEL_COUNT (1u + MPQ6612A_STEREO)

static void mpq_sleep_all(void)
{
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch)
        nora_gpio_rp_clear(s_mpq[ch].sleep);
}

/* Also valid after partial init/start failure: attempt every resource cleanup. */
static void mpq_disable_all(void)
{
    s_pwm_ready = false;
    mpq_sleep_all();
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch) {
        nora_dma_channel_enable(s_mpq[ch].dma, false);
        nora_pwm_generator_enable(s_mpq[ch].generator, false);
        nora_pwm_generator_pair_override(s_mpq[ch].generator, true, false, false);
    }
    s_pwm_stopped = true;
}
#endif

#if MPQ6612A_STEREO
#define PWM_HALF_WORDS (APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR)
/* Convert before acquiring the free DMA half. The final copy is only 64 words
 * at the initial 32-frame/48-kHz recipe. Four carrier intervals of headroom
 * leave >83 us for this copy; ISR preemption/overlap is still checked below. */
#define PWM_COMMIT_RESERVE_WORDS (4u)
static int32_t s_stereo_stage[2][PWM_HALF_WORDS];
static volatile uint32_t s_stereo_skips, s_stereo_phase_errors;

static bool stereo_commit(void)
{
    unsigned position[2], half[2];
    for (unsigned ch = 0; ch < 2u; ++ch) {
        uint32_t base = (uint32_t)s_mpq[ch].buffer;
        uint32_t src = nora_dma_read_src(s_mpq[ch].dma);
        if (src < base || src >= base + PWM_AUDIO_DUTY_WORD_COUNT * 4u) {
            ++s_stereo_skips; return false; // reload in progress
        }
        position[ch] = (src - base) / 4u;
        half[ch] = position[ch] / PWM_HALF_WORDS;
    }
    // A trigger services DMA4/5 serially. A one-word transient is permitted;
    // a persistent larger offset would pair different audio sample indices.
    unsigned distance = position[0] > position[1] ? position[0] - position[1] :
                                                   position[1] - position[0];
    if (half[0] != half[1]) { ++s_stereo_skips; return false; }
    if (distance > 1u) {
        ++s_stereo_phase_errors; s_pwm_stream_fault = true; return false;
    }
    for (unsigned ch = 0; ch < 2u; ++ch) {
        if (PWM_HALF_WORDS - position[ch] % PWM_HALF_WORDS <= PWM_COMMIT_RESERVE_WORDS) {
            ++s_stereo_skips; return false;
        }
    }
    unsigned target = 1u - half[0];
    for (unsigned i = 0; i < PWM_HALF_WORDS; ++i) {
        s_mpq[0].buffer[target * PWM_HALF_WORDS + i] = (uint32_t)s_stereo_stage[0][i];
        s_mpq[1].buffer[target * PWM_HALF_WORDS + i] = (uint32_t)s_stereo_stage[1][i];
    }
    for (unsigned ch = 0; ch < 2u; ++ch) {
        uint32_t src = nora_dma_read_src(s_mpq[ch].dma);
        uint32_t boundary = (uint32_t)&s_mpq[ch].buffer[PWM_HALF_WORDS];
        bool dma_second = src >= boundary;
        if (dma_second == (target != 0u)) {
            s_pwm_stream_fault = true;
#if PWM_AUDIO_SCOPE_ONLY
            ++s_scope[ch].write_overlap;
#endif
        }
#if PWM_AUDIO_SCOPE_ONLY
        uint32_t minimum = PG_PER_COUNT, maximum = 0;
        for (unsigned i = 0; i < PWM_HALF_WORDS; ++i) {
            uint32_t duty = (uint32_t)s_stereo_stage[ch][i];
            if (duty < minimum) minimum = duty;
            if (duty > maximum) maximum = duty;
        }
        s_scope[ch].duty_min = minimum;
        s_scope[ch].duty_max = maximum;
        ++s_scope[ch].writes;
#endif
    }
    return !s_pwm_stream_fault;
}
#endif

/* PG status/interrupt handling is out of hal_pwm's scope (R8/R9: HAL owns no
 * ISR vector); unchanged from the pre-hal_pwm implementation. */
void __attribute__( ( interrupt, context ) ) _PWM5Interrupt(void)
{
    uint32_t pg_stat;
    _PWM5IF = 0;
    pg_stat = PG5STAT;
    PG5STAT = 0;
    (void)pg_stat;
}

void __attribute__( ( interrupt, context ) ) _PWM6Interrupt(void)
{
    uint32_t pg_stat;
    _PWM6IF = 0;
    pg_stat = PG6STAT;
    PG6STAT = 0;
    (void)pg_stat;
}

void __attribute__( ( interrupt, context ) ) _PWM7Interrupt(void)
{
    uint32_t pg_stat;
    _PWM7IF = 0;
    pg_stat = PG7STAT;
    PG7STAT = 0;
    (void)pg_stat;
}

void __attribute__( ( interrupt, context ) ) _PWM8Interrupt(void)
{
    uint32_t pg_stat;
    _PWM8IF = 0;
    pg_stat = PG8STAT;
    PG8STAT = 0;
    (void)pg_stat;
}

#if APP_USE_PWM_AUDIO
static volatile uint32_t s_pwm_audio_breakpoint;

/* Ping-pong half tracking through nora_dma's pure predicates instead of raw
 * DMAxSTAT bit masks -- hal_dma still owns no callback/buffer policy (R8);
 * this ISR is exactly the "consumer decides what a half means" caller its
 * design boundaries describe. FIRST/SECOND mirror the original HALF/DONE
 * branches one-for-one (both are still no-op breakpoint anchors). */
#define DEFINE_PWM_AUDIO_DMA_ISR(channel) \
    void __attribute__( ( interrupt, context ) ) \
        _DMA##channel##Interrupt(void) \
    { \
        nora_dma_status_t nora_dma_status = \
            nora_dma_isr_snapshot(NORA_DMA_CHANNEL_##channel); \
        nora_dma_half_t nora_dma_half = nora_dma_half_from_status(nora_dma_status); \
        pwm_dma_observe(channel, nora_dma_status); \
        if( nora_dma_half == NORA_DMA_HALF_FIRST ) \
        { \
            s_pwm_audio_breakpoint = s_pwm_audio_breakpoint; \
        } \
        if( nora_dma_half == NORA_DMA_HALF_SECOND ) \
        { \
            s_pwm_audio_breakpoint = s_pwm_audio_breakpoint; \
        } \
    }

DEFINE_PWM_AUDIO_DMA_ISR(4)
DEFINE_PWM_AUDIO_DMA_ISR(5)
DEFINE_PWM_AUDIO_DMA_ISR(6)
DEFINE_PWM_AUDIO_DMA_ISR(7)

#undef DEFINE_PWM_AUDIO_DMA_ISR
#endif /* APP_USE_PWM_AUDIO */

void classic_audio_pwm_init(void)
{
#if APP_USE_PWM_AUDIO
    /* Routing conflicts are rejected by the configuration gates. */
    bool ok = true;
    if (s_pwm_initialized) return;
#if defined(APP_TARGET_AK506) && APP_TARGET == APP_TARGET_AK506
    // Nano has no RGB LED consumer to prepare the shared PWM clock.
    nora_clock_status_t clock_detail;
    if (sonora_clock_pwm_prepare(&clock_detail) != SONORA_CLOCK_PWM_OK ||
        !nora_pwm_module_init(NORA_PWM_MCLK_HIGH_FREQ)) {
        printf(" WARNING: Nano PWM clock init failed\n");
        return;
    }
#endif
#if APP_PWM_OUTPUT == 1
    // Put both bridges to sleep before routing either PWM input.
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch) {
        if (!nora_gpio_rp_config_digital_output(s_mpq[ch].sleep, false)) {
            mpq_disable_all(); return;
        }
    }
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch) {
        if (!nora_gpio_rp_config_digital_input(s_mpq[ch].fault)) {
            mpq_disable_all(); return;
        }
    }
#endif
    for (unsigned i = 0; i < PWM_AUDIO_DUTY_WORD_COUNT; ++i) {
        g_pwm_audio_pg5_duty[i] = PG_DC_INIT;
        g_pwm_audio_pg6_duty[i] = PG_DC_INIT;
    }
    ok = pwm5_init() && ok;
#if (APP_PWM_OUTPUT == 0) || MPQ6612A_STEREO
    ok = pwm6_init() && ok;
#endif
    ok = pwm7_init() && ok;
    ok = pwm8_init() && ok;
    s_pwm_initialized = ok;
#if APP_PWM_OUTPUT == 1
    if (ok) ok = classic_audio_pwm_start();
    else mpq_disable_all();
#else
    s_pwm_ready = ok;
#endif
    if( !ok )
    {
        printf(" WARNING: classic_audio_pwm_init: one or more PWM/DMA channels failed to init\n");
    }
#endif
}

void classic_audio_pwm_process_left_primary( float* in, int num_proc_ch )
{
    if (!s_pwm_ready) return;
#if MPQ6612A_STEREO && !PWM_AUDIO_SCOPE_ONLY
    if (!in || num_proc_ch < 2) return;
    for (unsigned ch = 0; ch < 2u; ++ch)
        convert_float_to_pwm_20bit(in, num_proc_ch, ch, s_stereo_stage[ch],
            APP_BLOCK_FRAMES, PWM_AUDIO_UPSAMPLE_FACTOR, PG_PER_COUNT);
    stereo_commit();
    return; // Both channels committed from the same application block.
#endif
    const uint32_t half_pwm_addr =
        (uint32_t)&g_pwm_audio_pg5_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    int32_t* dest_ptr;

    if( nora_dma_read_src(NORA_DMA_CHANNEL_4) >= half_pwm_addr )
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg5_duty[0];
    }
    else
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg5_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    }

    /* 2ch: L. 4ch: L1. */
#if PWM_AUDIO_SCOPE_ONLY
    if (s_scope_silence) {
        for (unsigned i = 0; i < APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR; ++i)
            dest_ptr[i] = PG_DC_INIT;
    } else
#endif
    convert_float_to_pwm_20bit(
        in, num_proc_ch, 0u, dest_ptr, APP_BLOCK_FRAMES,
        PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 );
#if PWM_AUDIO_SCOPE_ONLY
    // Observe the written half, never rewrite the half DMA is consuming.
    uint32_t minimum = PG_PER_COUNT, maximum = 0;
    for (unsigned i = 0; i < APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR; ++i) {
        uint32_t duty = (uint32_t)dest_ptr[i];
        if (duty < minimum) minimum = duty;
        if (duty > maximum) maximum = duty;
    }
    s_scope[0].duty_min = minimum;
    s_scope[0].duty_max = maximum;
    ++s_scope[0].writes;
    const bool dma_in_second = nora_dma_read_src(NORA_DMA_CHANNEL_4) >= half_pwm_addr;
    const bool wrote_first = dest_ptr == (int32_t*)&g_pwm_audio_pg5_duty[0];
    if (dma_in_second != wrote_first) ++s_scope[0].write_overlap;
#endif
}

void classic_audio_pwm_process_right_primary( float* in, int num_proc_ch )
{
#if APP_PWM_OUTPUT == 1
    (void)in; (void)num_proc_ch; return;
#endif
    if (!s_pwm_ready) return;
    const uint32_t half_pwm_addr =
        (uint32_t)&g_pwm_audio_pg6_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    int32_t* dest_ptr;

    if( nora_dma_read_src(NORA_DMA_CHANNEL_5) >= half_pwm_addr )
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg6_duty[0];
    }
    else
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg6_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    }

    /* 2ch: R. 4ch: R1. */
    convert_float_to_pwm_20bit(
        in, num_proc_ch, 1u, dest_ptr, APP_BLOCK_FRAMES,
        PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 );
}

void classic_audio_pwm_process_left_secondary( float* in, int num_proc_ch )
{
    if (!s_pwm_ready) return;
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
    const uint32_t half_pwm_addr =
        (uint32_t)&g_pwm_audio_pg7_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    int32_t* dest_ptr;
    const uint8_t slot = ( num_proc_ch >= 4 ) ? 2u : 0u;

    if( nora_dma_read_src(NORA_DMA_CHANNEL_6) >= half_pwm_addr )
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg7_duty[0];
    }
    else
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg7_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    }

    /* 2ch: mirror L. 4ch: L2. */
    convert_float_to_pwm_20bit(
        in, num_proc_ch, slot, dest_ptr, APP_BLOCK_FRAMES,
        PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 );
#else
    (void)in;
    (void)num_proc_ch;
#endif
}

void classic_audio_pwm_process_right_secondary( float* in, int num_proc_ch )
{
    if (!s_pwm_ready) return;
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
    const uint32_t half_pwm_addr =
        (uint32_t)&g_pwm_audio_pg8_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    int32_t* dest_ptr;
    const uint8_t slot = ( num_proc_ch >= 4 ) ? 3u : 1u;

    if( nora_dma_read_src(NORA_DMA_CHANNEL_7) >= half_pwm_addr )
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg8_duty[0];
    }
    else
    {
        dest_ptr = (int32_t*)&g_pwm_audio_pg8_duty[
            APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    }

    /* 2ch: mirror R. 4ch: R2. */
    convert_float_to_pwm_20bit(
        in, num_proc_ch, slot, dest_ptr, APP_BLOCK_FRAMES,
        PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 );
#else
    (void)in;
    (void)num_proc_ch;
#endif
}

#if (APP_TARGET == APP_TARGET_AK512) || (defined(APP_TARGET_AK506) && APP_TARGET == APP_TARGET_AK506)

/* Common ping-pong DMA shape for all four channels: 32-bit words, source
 * increments through the duty buffer, destination is the fixed PGxDC
 * register, repeated one-shot + full reload so the transfer re-arms forever,
 * half/done interrupts drive the ping-pong buffer swap in the ISRs above. */
static bool pwm_audio_dma_config( nora_dma_channel_t ch, nora_dma_trigger_t trigger,
                                   volatile uint32_t *duty_buf, volatile uint32_t *pg_dc )
{
    nora_dma_channel_cfg_t cfg = { 0 };

    cfg.src           = (volatile void*)duty_buf;
    cfg.dst            = (volatile void*)pg_dc;
    cfg.count          = PWM_AUDIO_DUTY_WORD_COUNT;
    cfg.src_mode       = NORA_DMA_ADDR_INCREMENT;
    cfg.dst_mode       = NORA_DMA_ADDR_FIXED;
    cfg.size           = NORA_DMA_SIZE_WORD;
    cfg.tr_mode        = NORA_DMA_TRMODE_REPEAT_ONESHOT;
    cfg.reload_count   = true;
    cfg.reload_src     = true;
    cfg.reload_dst     = false;
    cfg.half_int_en    = true;
    cfg.done_int_en    = true;
    cfg.trigger        = trigger;
    cfg.irq_priority_set = false;
    cfg.irq_enable     = true;

    if( !nora_dma_channel_config(ch, &cfg) )
    {
        return false;
    }
    return nora_dma_channel_enable(ch, APP_PWM_OUTPUT == 0);
}

static bool pwm5_init(void)
{
//
// location assignment
//
// DIM-P33  RP29  CVDAN28/CVDTX12/CMP3D/RP29/RB12
// DIM-P35  RP90  RP90/RF9
// DIM-P37  RP89  RP89/RF8
// DIM-P39  RP92  RP92/RF11
    nora_pwm_generator_cfg_t cfg = { 0 };

    if( !pwm_audio_dma_config( NORA_DMA_CHANNEL_4, NORA_DMA_TRIGGER_PWM_GEN5,
                                &g_pwm_audio_pg5_duty[0], &PG5DC ) )
    {
        return false;
    }

    cfg.period          = PG_PER_COUNT;
    cfg.duty_init        = PG_DC_INIT;
#if APP_PWM_OUTPUT == 1
    // Integrated bridge handles shoot-through; complementary A/B, no host dead time.
    cfg.dead_time_high = 0;
    cfg.dead_time_low = 0;
    cfg.rp_h = MPQ6612A_IN1_RP;
    cfg.rp_l = MPQ6612A_IN2_RP;
    cfg.start_disabled = true;
#else
    cfg.dead_time_high   = AUDIO_DEAD_TIME;
    cfg.dead_time_low    = AUDIO_DEAD_TIME;
    cfg.rp_h             = (nora_gpio_rp_t)28u;
    cfg.rp_l             = (nora_gpio_rp_t)97u;
#endif
    cfg.pen_h            = true;
    cfg.pen_l            = true;
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = false;
    cfg.is_module_master = true;

    return nora_pwm_generator_init(NORA_PWM_GEN_5, &cfg);
}

bool classic_audio_pwm_fault_active(void)
{
#if APP_PWM_OUTPUT == 1
    if (s_pwm_stream_fault) return true;
#if !PWM_AUDIO_SCOPE_ONLY
    // Ignore sleeping IC diagnostics, including after an idempotent stop.
    if (!s_pwm_stopped) {
        for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch)
            if (nora_gpio_rp_read(s_mpq[ch].fault) != NORA_GPIO_LEVEL_HIGH) return true;
    }
#endif
#endif
    return false;
}

bool classic_audio_pwm_start(void)
{
#if APP_PWM_OUTPUT == 1
    if (s_pwm_ready) return true;
    if (!s_pwm_initialized) return false;
    mpq_disable_all();
    s_pwm_stream_fault = false;
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch) {
        for (unsigned i = 0; i < PWM_AUDIO_DUTY_WORD_COUNT; ++i)
            s_mpq[ch].buffer[i] = PG_DC_INIT;
        nora_pwm_generator_set_duty(s_mpq[ch].generator, PG_DC_INIT);
        if (!nora_pwm_generator_pair_override(s_mpq[ch].generator, true, false, false) ||
            !pwm_audio_dma_config(s_mpq[ch].dma, s_mpq[ch].trigger,
                                  s_mpq[ch].buffer, s_mpq[ch].dc)) goto fail;
    }
    // Arm both streams with PG5 stopped. Enable PG6 first: its first SOC is
    // PG5's first EOC. Both DMAs use that same PG5 event, sample zero together.
    for (unsigned ch = MPQ_CHANNEL_COUNT; ch-- > 0u;) {
        if (!nora_dma_channel_enable(s_mpq[ch].dma, true) ||
            !nora_pwm_generator_enable(s_mpq[ch].generator, true)) goto fail;
    }
    s_pwm_stopped = false;
#if !PWM_AUDIO_SCOPE_ONLY
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch)
        nora_gpio_rp_set(s_mpq[ch].sleep);
    delay_us(1000); // tDELAY max 500 us; coast (Z/Z), never 1-ms low-IQ brake
    if (classic_audio_pwm_fault_active()) goto fail;
#endif
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch)
        if (!nora_pwm_generator_pair_override(s_mpq[ch].generator, false, false, true)) goto fail;
    delay_us(50); // >2 carrier periods before producer writes audio
    s_pwm_ready = true;
    return true;
fail:
    mpq_disable_all();
    return false;
#else
    return s_pwm_ready;
#endif
}

void classic_audio_pwm_stop(void)
{
#if APP_PWM_OUTPUT == 1
    s_pwm_ready = false; // Stop producer before touching either channel.
    if (!s_pwm_initialized || s_pwm_stopped) { mpq_sleep_all(); return; }
    for (unsigned ch = 0; ch < MPQ_CHANNEL_COUNT; ++ch)
        nora_pwm_generator_pair_override(s_mpq[ch].generator, true,
                                         !PWM_AUDIO_SCOPE_ONLY, true);
#if !PWM_AUDIO_SCOPE_ONLY
    delay_us(50); // short brake, far below the 1-ms low-IQ brake threshold
    mpq_sleep_all();
    delay_us(1000);
#endif
    mpq_disable_all();
#elif APP_USE_PWM_AUDIO
    /* Legacy MIC4607 route: terminal stop, including the optional pair. */
    s_pwm_ready = false;
    nora_dma_channel_enable(NORA_DMA_CHANNEL_4, false);
    nora_dma_channel_enable(NORA_DMA_CHANNEL_5, false);
    nora_pwm_generator_enable(NORA_PWM_GEN_5, false);
    nora_pwm_generator_enable(NORA_PWM_GEN_6, false);
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
    nora_dma_channel_enable(NORA_DMA_CHANNEL_6, false);
    nora_dma_channel_enable(NORA_DMA_CHANNEL_7, false);
    nora_pwm_generator_enable(NORA_PWM_GEN_7, false);
    nora_pwm_generator_enable(NORA_PWM_GEN_8, false);
#endif
#endif
}

void classic_audio_pwm_scope_process_pcm(const int32_t* pcm, unsigned stride)
{
#if PWM_AUDIO_SCOPE_ONLY
    if (!pcm || stride < 1u + MPQ6612A_STEREO) return;
    for (unsigned ch = 0; ch < 1u + MPQ6612A_STEREO; ++ch) {
        uint32_t peak = 0;
        for (unsigned i = 0; i < APP_BLOCK_FRAMES; ++i) {
            int32_t sample = pcm[i * stride + ch];
            uint32_t magnitude = sample < 0 ? 0u - (uint32_t)sample : (uint32_t)sample;
            if (magnitude > peak) peak = magnitude;
        }
        s_scope[ch].pcm_peak = peak;
        ++s_scope[ch].pcm_blocks;
    }
    if (!s_pwm_ready) return;
#if MPQ6612A_STEREO
    for (unsigned ch = 0; ch < 2u; ++ch) {
        if (s_scope_silence) {
            for (unsigned i = 0; i < PWM_HALF_WORDS; ++i) s_stereo_stage[ch][i] = PG_DC_INIT;
        } else {
            convert_codec_q31_to_pwm_20bit(pcm + ch, stride, s_stereo_stage[ch],
                APP_BLOCK_FRAMES, PWM_AUDIO_UPSAMPLE_FACTOR, PG_PER_COUNT);
        }
    }
    stereo_commit();
#else
    const uint32_t half_pwm_addr = (uint32_t)&g_pwm_audio_pg5_duty[
        APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    int32_t* dest_ptr = nora_dma_read_src(NORA_DMA_CHANNEL_4) >= half_pwm_addr ?
        (int32_t*)&g_pwm_audio_pg5_duty[0] :
        (int32_t*)&g_pwm_audio_pg5_duty[APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR];
    if (s_scope_silence) {
        for (unsigned i = 0; i < APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR; ++i)
            dest_ptr[i] = PG_DC_INIT;
    } else {
        convert_codec_q31_to_pwm_20bit(pcm, stride, dest_ptr, APP_BLOCK_FRAMES,
            PWM_AUDIO_UPSAMPLE_FACTOR, PG_PER_COUNT);
    }
    uint32_t minimum = PG_PER_COUNT, maximum = 0;
    for (unsigned i = 0; i < APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR; ++i) {
        uint32_t duty = (uint32_t)dest_ptr[i];
        if (duty < minimum) minimum = duty;
        if (duty > maximum) maximum = duty;
    }
    s_scope[0].duty_min = minimum;
    s_scope[0].duty_max = maximum;
    ++s_scope[0].writes;
    bool dma_second = nora_dma_read_src(NORA_DMA_CHANNEL_4) >= half_pwm_addr;
    bool wrote_first = dest_ptr == (int32_t*)&g_pwm_audio_pg5_duty[0];
    if (dma_second != wrote_first) {
        ++s_scope[0].write_overlap; s_pwm_stream_fault = true;
    }
#endif
#else
    (void)pcm; (void)stride;
#endif
}

void classic_audio_pwm_scope_set_silence(bool silence)
{
#if PWM_AUDIO_SCOPE_ONLY
    s_scope_silence = silence;
#else
    (void)silence;
#endif
}

void classic_audio_pwm_scope_service(void)
{
#if PWM_AUDIO_SCOPE_ONLY
    static uint32_t last_ms, last_events[1u + MPQ6612A_STEREO];
    static bool monitoring;
    uint32_t now = GetTicks();
    // Check the output latch as well as the pin: readback alone is not a
    // physical nSLEEP measurement. A failure always returns to sleep/coast.
    if (s_pwm_ready && (nora_gpio_rp_read(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW ||
        nora_gpio_rp_read_output(MPQ6612A_SLEEP_RP) != NORA_GPIO_LEVEL_LOW ||
        !OSCCTRLbits.PLL2RDY || !CLK5CONbits.CLKRDY ||
        !PG5CONbits.ON || !DMA4CHbits.CHEN || s_pwm_stream_fault
#if MPQ6612A_STEREO
        || nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
        || nora_gpio_rp_read_output(MPQ6612A_RIGHT_SLEEP_RP) != NORA_GPIO_LEVEL_LOW
        || !PG6CONbits.ON || !DMA5CHbits.CHEN
#endif
        )) {
        ++s_scope_failures;
        classic_audio_pwm_stop();
    }
    if (!s_pwm_ready) { monitoring = false; return; }
    if (!monitoring) {
        // Boot/codec setup already consumed >1 s. Start the deadline when
        // PWM starts, rather than comparing its first event with boot time.
        last_ms = now;
        for (unsigned ch = 0; ch < 1u + MPQ6612A_STEREO; ++ch)
            last_events[ch] = s_scope[ch].half + s_scope[ch].done;
        monitoring = true;
        return;
    }
    if ((uint32_t)(now - last_ms) < 1000u) return;
    for (unsigned ch = 0; ch < 1u + MPQ6612A_STEREO; ++ch) {
        uint32_t events = s_scope[ch].half + s_scope[ch].done;
        if (s_pwm_ready && events == last_events[ch]) {
            ++s_scope_failures;
            classic_audio_pwm_stop();
        }
        last_events[ch] = events;
    }
    last_ms = now;
#endif
}

void classic_audio_pwm_scope_print(void)
{
#if PWM_AUDIO_SCOPE_ONLY
    static uint32_t last_ms;
    uint32_t now = GetTicks();
    if ((uint32_t)(now - last_ms) < 1000u) return;
    last_ms = now;
#define SCOPE_STR2(x) #x
#define SCOPE_STR(x) SCOPE_STR2(x)
    printf(" PWMscope id=%s ms=%lu ready=%u sleep=%u pll2=%u clk5=%u pg=%u dma=%u cnt=%lu half=%lu done=%lu err=%lu fail=%lu\n",
        SCOPE_STR(PWM_AUDIO_SCOPE_TAG), (unsigned long)now, (unsigned)s_pwm_ready,
        (unsigned)(nora_gpio_rp_read(MPQ6612A_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)OSCCTRLbits.PLL2RDY, (unsigned)CLK5CONbits.CLKRDY,
        (unsigned)PG5CONbits.ON, (unsigned)DMA4CHbits.CHEN,
        (unsigned long)nora_dma_read_count(NORA_DMA_CHANNEL_4),
        (unsigned long)s_scope[0].half, (unsigned long)s_scope[0].done,
        (unsigned long)s_scope[0].dma_errors, (unsigned long)s_scope_failures);
    audio_transport_snapshot_t snapshot = {0};
    audio_transport_snapshot_get(&snapshot);
    const audio_transport_leg_snapshot_t* leg = &snapshot.legs[0];
    printf(" PWMscope pcmL_q31=%lu pcm_blk=%lu duty=%lu/%lu per=%lu wr=%lu overlap=%lu tdm=%u/%lu miss=%lu rtf=%lu/%lu/%lu source=%s map=tdm_tx_q31\n",
        (unsigned long)s_scope[0].pcm_peak, (unsigned long)s_scope[0].pcm_blocks,
        (unsigned long)s_scope[0].duty_min, (unsigned long)s_scope[0].duty_max,
        (unsigned long)PG_PER_COUNT, (unsigned long)s_scope[0].writes,
        (unsigned long)s_scope[0].write_overlap, (unsigned)leg->running,
        (unsigned long)leg->block_count, (unsigned long)leg->deadline_miss_count,
        (unsigned long)leg->rx_overrun_block_count, (unsigned long)leg->tx_underrun_block_count,
        (unsigned long)leg->frame_error_block_count, s_scope_silence ? "neutral" : "audio");
#if MPQ6612A_STEREO
    printf(" PWMscopeR sleep=%u pg=%u dma=%u cnt=%lu half=%lu done=%lu err=%lu pcmR_q31=%lu pcm_blk=%lu duty=%lu/%lu wr=%lu overlap=%lu skip=%lu phase_err=%lu\n",
        (unsigned)(nora_gpio_rp_read(MPQ6612A_RIGHT_SLEEP_RP) == NORA_GPIO_LEVEL_HIGH),
        (unsigned)PG6CONbits.ON, (unsigned)DMA5CHbits.CHEN,
        (unsigned long)nora_dma_read_count(NORA_DMA_CHANNEL_5),
        (unsigned long)s_scope[1].half, (unsigned long)s_scope[1].done,
        (unsigned long)s_scope[1].dma_errors, (unsigned long)s_scope[1].pcm_peak,
        (unsigned long)s_scope[1].pcm_blocks, (unsigned long)s_scope[1].duty_min,
        (unsigned long)s_scope[1].duty_max, (unsigned long)s_scope[1].writes,
        (unsigned long)s_scope[1].write_overlap, (unsigned long)s_stereo_skips,
        (unsigned long)s_stereo_phase_errors);
#endif
#undef SCOPE_STR
#undef SCOPE_STR2
#endif
}

static bool pwm6_init(void)
{
//
// location assignment
//
// DIM-P33  RP29  CVDAN28/CVDTX12/CMP3D/RP29/RB12
// DIM-P35  RP90  RP90/RF9
// DIM-P37  RP89  RP89/RF8
// DIM-P39  RP92  RP92/RF11
    nora_pwm_generator_cfg_t cfg = { 0 };

    if( !pwm_audio_dma_config( NORA_DMA_CHANNEL_5,
#if MPQ6612A_STEREO
                                NORA_DMA_TRIGGER_PWM_GEN5, // common L/R sample event
#else
                                NORA_DMA_TRIGGER_PWM_GEN6,
#endif
                                &g_pwm_audio_pg6_duty[0], &PG6DC ) )
    {
        return false;
    }

    cfg.period          = PG_PER_COUNT;
    cfg.duty_init        = PG_DC_INIT;
#if MPQ6612A_STEREO
    cfg.dead_time_high = 0;
    cfg.dead_time_low = 0;
    cfg.rp_h = MPQ6612A_RIGHT_IN1_RP;
    cfg.rp_l = MPQ6612A_RIGHT_IN2_RP;
    cfg.start_disabled = true;
#else
    cfg.dead_time_high   = AUDIO_DEAD_TIME;
    cfg.dead_time_low    = AUDIO_DEAD_TIME;
    cfg.rp_h             = (nora_gpio_rp_t)82u;
    cfg.rp_l             = (nora_gpio_rp_t)98u;
#endif
    cfg.pen_h            = true;
    cfg.pen_l            = true;
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = true;
    cfg.trigger_master   = NORA_PWM_GEN_5;   /* PWM5(Left) & 6(Right) pair */
    cfg.is_module_master = true;

    return nora_pwm_generator_init(NORA_PWM_GEN_6, &cfg);
}

static bool pwm7_init(void)
{
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
//
// new location assignment
//
// DIM-P21  RP28  CVDAN27/CVDTX11/CMP7B/RP28/IOMAF10/IOMBF10/SDI2/RB11
// DIM-P23  RP97  RP97/RG0
// DIM-P25  RP82  CVDTX30/RP82/RF1
// DIM-P27  RP98  RP98/APWM4H/IOMBD11/RG1
//
// NOTE (found while porting to hal_pwm, unchanged from the pre-hal_pwm code):
// these RPs are identical to PG5/PG6's (28/97 and, via PG8 below, 82/98).
// Harmless while PWM_AUDIO_ENABLE_SECONDARY_PAIR stays undefined (the only
// state ever shipped/tested), but the secondary pair's real DIM pins need to
// be picked before this is ever turned on.
    nora_pwm_generator_cfg_t cfg = { 0 };

    if( !pwm_audio_dma_config( NORA_DMA_CHANNEL_6, NORA_DMA_TRIGGER_PWM_GEN7,
                                &g_pwm_audio_pg7_duty[0], &PG7DC ) )
    {
        return false;
    }

    cfg.period          = PG_PER_COUNT;
    cfg.duty_init        = PG_DC_INIT;
    cfg.dead_time_high   = AUDIO_DEAD_TIME;
    cfg.dead_time_low    = AUDIO_DEAD_TIME;
    cfg.rp_h             = (nora_gpio_rp_t)28u;
    cfg.rp_l             = (nora_gpio_rp_t)97u;
    cfg.pen_h            = true;
    cfg.pen_l            = true;
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = false;
    cfg.is_module_master = true;

    return nora_pwm_generator_init(NORA_PWM_GEN_7, &cfg);
#else
    return true;
#endif //defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
}

static bool pwm8_init(void)
{
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
//
// new location assignment
//
// DIM-P21  RP28  CVDAN27/CVDTX11/CMP7B/RP28/IOMAF10/IOMBF10/SDI2/RB11
// DIM-P23  RP97  RP97/RG0
// DIM-P25  RP82  CVDTX30/RP82/RF1
// DIM-P27  RP98  RP98/APWM4H/IOMBD11/RG1
    nora_pwm_generator_cfg_t cfg = { 0 };

    if( !pwm_audio_dma_config( NORA_DMA_CHANNEL_7, NORA_DMA_TRIGGER_PWM_GEN8,
                                &g_pwm_audio_pg8_duty[0], &PG8DC ) )
    {
        return false;
    }

    cfg.period          = PG_PER_COUNT;
    cfg.duty_init        = PG_DC_INIT;
    cfg.dead_time_high   = AUDIO_DEAD_TIME;
    cfg.dead_time_low    = AUDIO_DEAD_TIME;
    cfg.rp_h             = (nora_gpio_rp_t)82u;
    cfg.rp_l             = (nora_gpio_rp_t)98u;
    cfg.pen_h            = true;
    cfg.pen_l            = true;
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = true;
    cfg.trigger_master   = NORA_PWM_GEN_7;   /* PWM7(Left) & 8(Right) pair */
    cfg.is_module_master = true;

    return nora_pwm_generator_init(NORA_PWM_GEN_8, &cfg);
#else
    return true;
#endif //defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
}
#endif //APP_TARGET == APP_TARGET_AK512

#else

bool classic_audio_pwm_start(void) { return false; }
void classic_audio_pwm_stop(void) {}
bool classic_audio_pwm_fault_active(void) { return false; }
void classic_audio_pwm_scope_service(void) {}
void classic_audio_pwm_scope_print(void) {}
void classic_audio_pwm_scope_set_silence(bool silence) { (void)silence; }
void classic_audio_pwm_scope_process_pcm(const int32_t* pcm, unsigned stride)
{ (void)pcm; (void)stride; }

void classic_audio_pwm_init(void)
{
    /* PWM audio output is not available on AK128. */
}

#endif /* APP_TARGET == APP_TARGET_AK512 */

#endif /* APP_USE_PWM_AUDIO && APP_PWM_AUDIO_HAL */
