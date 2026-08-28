#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_audio_pwm.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <xc.h>

#include "classic_audio_pwm.h"
#include "nora_pwm.h"   /* hal_pwm: PG generator register HAL */
#include "nora_dma.h"   /* hal_dma: DMA channel register HAL */
#include "apps/shared/float_conversion.h"
#include "pwm_audio_dma_buffer.h"

#if APP_TARGET == APP_TARGET_AK512

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
    /* PWM5-8 share RP97 with the optional second WM8904. */
    bool ok = true;
    ok = pwm5_init() && ok;
    ok = pwm6_init() && ok;
    ok = pwm7_init() && ok;
    ok = pwm8_init() && ok;
    if( !ok )
    {
        printf(" WARNING: classic_audio_pwm_init: one or more PWM/DMA channels failed to init\n");
    }
#endif
}

void classic_audio_pwm_process_left_primary( float* in, int num_proc_ch )
{
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
    convert_float_to_pwm_20bit(
        in, num_proc_ch, 0u, dest_ptr, APP_BLOCK_FRAMES,
        PWM_AUDIO_UPSAMPLE_FACTOR, PWM_AUDIO_PERIOD_COUNT_Q4 );
}

void classic_audio_pwm_process_right_primary( float* in, int num_proc_ch )
{
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

#if APP_TARGET == APP_TARGET_AK512

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
    return nora_dma_channel_enable(ch, true);
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
    cfg.dead_time_high   = AUDIO_DEAD_TIME;
    cfg.dead_time_low    = AUDIO_DEAD_TIME;
    cfg.rp_h             = (nora_gpio_rp_t)28u;
    cfg.rp_l             = (nora_gpio_rp_t)97u;
    cfg.pen_h            = true;
    cfg.pen_l            = true;
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = false;
    cfg.is_module_master = true;

    return nora_pwm_generator_init(NORA_PWM_GEN_5, &cfg);
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

    if( !pwm_audio_dma_config( NORA_DMA_CHANNEL_5, NORA_DMA_TRIGGER_PWM_GEN6,
                                &g_pwm_audio_pg6_duty[0], &PG6DC ) )
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

void classic_audio_pwm_init(void)
{
    /* PWM audio output is not available on AK128. */
}

#endif /* APP_TARGET == APP_TARGET_AK512 */
