#include "app_specific_config_defs.h"
#include "asrc_clock_control.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_clock_control.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#if APP_B_INDEP_DOMAIN && APP_USE_CCP_FS_DETECT

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_app_asrc.h"
#include "asrc_audio_path.h"
#include "asrc_rate_plan.h"
#include "board/clock/sonora_clock.h"
#include "board/devices/wm8904.h"
#include "nora_ccp_input_capture.h"
/* Backend fast path, for the two hand-written CCP vectors below. This is the one
 * backend header any application file in this tree includes, and it is included for
 * the ISR fast path specifically -- not to reach a register. */
#include "nora_ccp_input_capture_dspic33ak_fast.h"
/* DSPload: these two vectors run at IPL 5, above both legs, and fire on every captured clock edge
 * while audio streams -- the prime suspect for `stolen`. The hooks must not reintroduce the
 * W0-W5 pushes that the `context` attribute exists to remove (see the long note below): verify
 * the push count is still 0 in the disassembly after touching anything in these handlers. */
#include "hal_timer/nora_cpu_load_prof_fast.h"
#include "nora_gpio.h"
#include "nora_pps.h"
#include "timer_app.h"

#define I2C_INST_A  (2u)
#if APP_AK128_J3_TDM_B
#define I2C_INST_B  (1u)
#else
#define I2C_INST_B  (3u)
#endif

#define CCPDET_STRIDE     (16u)
#define CCPDET_EMA_ALPHA  (0.02f)

/*
 * Capture time base: FCY (PLL1 <- FRC) by default, or CLKGEN13 <- PLL2 <- codec-A
 * XTALout (fixed 12.288 MHz on REFI1<-RP16, fs-independent) when the build opts in.
 * See ccpdet_arm() for what the choice does and does not change, and
 * [internal] clock_design_contract.md section 9 for the policy.
 */
#define CCPDET_TIMEBASE_FROM_PLL2   (RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2)
#if CCPDET_TIMEBASE_FROM_PLL2
#define CCPDET_TIMEBASE_HZ          ((uint32_t)SONORA_CLOCK_CCP_PLL2_HZ)
#else
#define CCPDET_TIMEBASE_HZ          ((uint32_t)FCY)
#endif

/*
 * CCP1/2 each interrupt at fs/16 (3 kHz at 48 kHz, 6 kHz at 96 kHz).
 * Keep those high-priority ISRs integer-only and defer the unchanged per-sample
 * floating-point EMA to the foreground, without changing capture cadence.
 *
 * DEPTH IS A RAM DECISION, NOT A SERVO REQUIREMENT.  ccpdet_process_periods()
 * drains on every foreground pass, so the servo needs only a handful of slots;
 * the queue exists to keep the EMA fed across a foreground stall.  512 slots
 * (2,048 B PER DETECTOR, 4,096 B of the 64 KiB data region for the pair) were
 * chosen to span a slow telemetry print -- about 85 ms at 96 kHz.
 *
 * 128 slots (~42 ms at 48 kHz) were tried on 2026-08-26 to reclaim RAM and
 * MEASURED INSUFFICIENT on real hardware within minutes of continuous
 * operation: period queue overrun A/B climbed from 11/11 to 27/32 over about
 * three minutes of the periodic telemetry print, not a one-time startup
 * burst.  256 slots restore the ~85 ms budget the original 512 was sized
 * for (fs/16 capture rate halves the per-slot time at 48 kHz vs. 96 kHz, so
 * 256 at 48 kHz spans what 512 spanned at 96 kHz) while still reclaiming
 * 1,536 B over the original 512.  Do not drop this below 256 without an
 * equivalent multi-minute on-target overrun check -- a short run can look
 * clean and still be short of margin, as 128 was.
 */
#define CCPDET_PERIOD_QUEUE_LENGTH  (256u)
#define CCPDET_PERIOD_QUEUE_MASK    (CCPDET_PERIOD_QUEUE_LENGTH - 1u)

#if (CCPDET_PERIOD_QUEUE_LENGTH & CCPDET_PERIOD_QUEUE_MASK) != 0u
#error "CCPDET_PERIOD_QUEUE_LENGTH must be a power of two."
#endif

#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
#define CCPDET_FF_MIN_CAPTURES (256u)
#define CCPDET_FF_STABLE_PPM   (100.0f)
#define CCPDET_FF_STABLE_COUNT (8u)
#define CCPDET_FF_NOMINAL_PPM  (1000.0f)
#endif

typedef struct {
    nora_ccp_inst_t  inst;
    nora_pps_input_t pps_in;
    nora_gpio_rp_t   rp;
    volatile uint32_t     prev;
    volatile bool         have;
    volatile uint32_t     capture_count;
    volatile uint32_t     latest_period;
    volatile bool         period_queue_enabled;
    volatile uint16_t     period_write;
    volatile uint16_t     period_read;
    volatile uint32_t     period_overrun_count;
    volatile uint32_t     period_queue[CCPDET_PERIOD_QUEUE_LENGTH];
    bool                  inited;
    float                 ema_period;
} ccpdet_t;

static ccpdet_t s_det_a = {
    .inst   = NORA_CCP1,
    .pps_in = NORA_PPS_INPUT_ICM1,
    .rp     = BOARD_ASRC_CLOCK_A_RP,
};
static ccpdet_t s_det_b = {
    .inst   = NORA_CCP2,
    .pps_in = NORA_PPS_INPUT_ICM2,
    .rp     = BOARD_ASRC_CLOCK_B_RP,
};

#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
static float   s_ff_candidate;
static uint8_t s_ff_stable_count;
static bool    s_ff_acquired;
#endif

static inline __attribute__((always_inline)) void
ccpdet_capture_isr( ccpdet_t* detector, uint32_t timestamp )
{
    detector->capture_count++;
    if( detector->have )
    {
        const uint32_t period = timestamp - detector->prev;
        detector->latest_period = period;

        if( detector->period_queue_enabled )
        {
            const uint16_t write = detector->period_write;
            const uint16_t read  = detector->period_read;
            if( (uint16_t)( write - read ) < CCPDET_PERIOD_QUEUE_LENGTH )
            {
                detector->period_queue[write & CCPDET_PERIOD_QUEUE_MASK] =
                    period;
                detector->period_write = (uint16_t)( write + 1u );
            }
            else
            {
                detector->period_overrun_count++;
            }
        }
    }
    detector->prev = timestamp;
    detector->have = true;
}

static void ccpdet_process_periods( ccpdet_t* detector )
{
    if( !detector->period_queue_enabled )
    {
        /*
         * Capture is armed before the audio startup sequence, while foreground
         * service is not yet running.  Preserve liveness and the latest period,
         * but intentionally discard that unbounded boot backlog.
         */
        detector->period_read = detector->period_write;
        const uint32_t latest = detector->latest_period;
        detector->period_queue_enabled = true;
        if( latest != 0u )
        {
            detector->ema_period = (float)latest;
            detector->inited = true;
        }
        return;
    }

    uint16_t read = detector->period_read;
    const uint16_t write = detector->period_write;

    while( read != write )
    {
        const float period =
            (float)detector->period_queue[read & CCPDET_PERIOD_QUEUE_MASK];
        read = (uint16_t)( read + 1u );
        detector->period_read = read;

        if( !detector->inited )
        {
            detector->ema_period = period;
            detector->inited = true;
        }
        else
        {
            detector->ema_period +=
                ( period - detector->ema_period ) * CCPDET_EMA_ALPHA;
        }
    }
}

/*
 * ASRC owns these vectors directly instead of entering the callback-oriented
 * generic HAL dispatcher.  Both handlers are leaf ISRs after inlining: no
 * callback lookup, no instance table walk, and no floating-point context save.
 *
 * They now say that through the HAL's _hot fast path instead of poking CCPxSTAT /
 * CCPxBUF / IFS1. The instance is a literal, so the accessors constant-fold to the
 * same direct SFR accesses this loop used to write out by hand -- measured
 * identical at 240 bytes / 73 instructions / 0 calls before and after. The object
 * code is the point of a fast path; naming the chip here never was.
 *
 * WHY `context` AND NOT `no_auto_psv`
 * ----------------------------------
 * `context` says "do not save W0-W7 on entry -- the hardware already gave this ISR its
 * own copies". On dsPIC33A that is a statement of fact, not an optimisation gamble.
 * DS70005591D (Alternate Working Register Arrays): the CPU implements up to seven
 * alternate W0-W7 arrays plus AccA/AccB/RCOUNT and the DSP CORCON bits, and "each
 * Alternate W array is INHERENTLY assigned to a respective IPL (e.g., IPL4 is assigned
 * to Context 4)". Inherent -- there is no register to program and no way to forget. The
 * switch happens on exception entry and unwinds on RETFIE. It was also observed directly:
 * a debugger-frozen trap capture on this board showed SR.CTX == SR.IPL == 5, i.e. entry
 * into these very vectors had already switched banks.
 *
 * So the six `mov.l wN,[w15++]` pushes the compiler emitted for W0-W5 here were saving
 * registers that no longer needed saving. Both handlers are leaf after inlining and touch
 * nothing outside W0-W7, so `context` removes every one of them and the ISR gets shorter
 * as well as narrower. Precedent in this repo: _PWM5..8Interrupt in
 * src/app/apps/classic/classic_audio_pwm.c.
 *
 * SAFE DESPITE SHARING ONE BANK. CCP1 and CCP2 are both armed at IRQ priority 5 (see
 * ccpdet_arm below), so both run in CTX5. Equal priority means neither can preempt the
 * other, so they cannot be live in that bank at the same time. If either priority is ever
 * changed, changing only ONE of them is still fine -- what would NOT be fine is declaring
 * `context` on a handler that can preempt another handler at its own IPL, which the
 * architecture does not permit anyway.
 *
 * IT IS ALSO THE ADOPTED WORKAROUND FOR AN A1 SILICON DEFECT. DO NOT REVERT THESE TWO
 * ATTRIBUTES. The long-unexplained STACK ERROR trap on dsPIC33AK512MPS512 rev A1
 * (DEVREV=0x01) is caused by these pushes, and removing them is what stops it. Measured
 * on this board 2026-08-26 with a controlled A/B, both images identical except for the two
 * attributes here (report section 21.11 / 21.12):
 *
 *   `context`      (no pushes, 48 instructions):  70 stress cycles / 29 min, zero traps
 *   `no_auto_psv`  (6 pushes,  70 instructions):  trap at cycle 16 / 6 min, and again after
 *
 * The recurrence reproduced the original fingerprint exactly -- STK-SR=0x000500A1
 * (CTX=5 IPL=5 RA=0), INTTREG=0x00002C05 VEC=5 ILR=11, ~1.8 kB of SPLIM headroom, and a
 * stacked PC of <CCP ISR>+0, which in that build is the FIRST `mov.l wN,[w15++]`. Note
 * what the numbers rule out: with 1.8 kB to spare it is not stack exhaustion, and RA=0
 * retired the REPEAT hypothesis. It is not in the errata (DS80001162E rev E has no
 * stack/STKERR/SPLIM/W15 entry at all).
 *
 * So this is a silicon workaround wearing the clothes of an optimisation. If a future
 * reader "cleans up" the attribute back to `no_auto_psv` because the ISRs look like they
 * do not need `context`, the STACK ERROR comes back -- it took weeks to localise.
 * app_silicon.{c,h} prints the die revision in the boot banner so any log can be checked
 * against this note; A2 has not been tested yet (owner: a spare AK512 at a later date).
 * NOT declared `context`: the trap handlers in app_traps.c would be wrong to rely on it,
 * because traps "execute in whatever register context the CPU was in prior to the trap
 * event" (same section) -- they get away with it only because they reset the device.
 */
void __attribute__((interrupt, context)) _CCP1Interrupt(void)
{
    uint32_t timestamp;

    nora_cpu_load_prof_enter( (uint8_t)NORA_CPU_LOAD_OWNER_OTHER );

    while( nora_ccp_icap_read_hot( NORA_CCP1, &timestamp ) )
    {
        ccpdet_capture_isr( &s_det_a, timestamp );
    }
    nora_ccp_icap_irq_clear_hot( NORA_CCP1 );

    nora_cpu_load_prof_exit();
}

/* `context` for the same reason as _CCP1Interrupt -- see the note above it. */
void __attribute__((interrupt, context)) _CCP2Interrupt(void)
{
    uint32_t timestamp;

    nora_cpu_load_prof_enter( (uint8_t)NORA_CPU_LOAD_OWNER_OTHER );

    while( nora_ccp_icap_read_hot( NORA_CCP2, &timestamp ) )
    {
        ccpdet_capture_isr( &s_det_b, timestamp );
    }
    nora_ccp_icap_irq_clear_hot( NORA_CCP2 );

    nora_cpu_load_prof_exit();
}

static void ccpdet_reset( ccpdet_t* detector )
{
    detector->prev = 0u;
    detector->have = false;
    detector->latest_period = 0u;
    detector->period_queue_enabled = false;
    detector->period_read = detector->period_write;
    detector->period_overrun_count = 0u;
    detector->inited = false;
    detector->ema_period = 0.0f;
    detector->capture_count = 0u;
}

static bool ccpdet_arm( ccpdet_t* detector )
{
    if( !nora_gpio_rp_set_analog( detector->rp, false ) ) { return false; }
    if( !nora_pps_route_input( detector->pps_in, detector->rp ) ) { return false; }

    /*
     * Time base. The default is the FCY peripheral clock, which is PLL1 <- FRC and
     * therefore carries the FRC's absolute frequency error into every reported fs --
     * a per-part number, NOT a constant (+0.66 % on one board, +0.41 % on another).
     * With CCPDET_TIMEBASE_FROM_PLL2 the capture time base comes from
     * CLKGEN13 <- PLL2 <- codec-A XTALout instead, so the clock doing the measuring
     * and the rates being measured share one crystal: 99.84 / 12.288 = 8.125 exactly.
     *
     * Both choices land within 0.16 % of each other by construction, so counts per
     * interval and 32-bit rollover behaviour are the same either way.
     *
     * This only changes ABSOLUTE accuracy. The servo tracks the FIFO fill error --
     * a physical sample count -- so it is immune to time-base error in both cases.
     */
    const nora_ccp_icap_config_t config = {
        .source          = NORA_CCP_SRC_PIN,
        .edge            = NORA_CCP_EDGE_EVERY_16TH_RISING,
#if CCPDET_TIMEBASE_FROM_PLL2
        .clock           = NORA_CCP_CLK_CLKGEN13,
#else
        .clock           = NORA_CCP_CLK_PERIPHERAL,
#endif
        .prescaler       = NORA_CCP_PS_1,
        .use_32bit       = true,
        .irq_ops         = NORA_CCP_IRQ_EVERY_EVENT,
        .irq_enable      = true,
        .irq_priority    = 5,
        .timebase_src_hz = CCPDET_TIMEBASE_HZ,
    };
    if( nora_ccp_icap_configure( detector->inst, &config ) != NORA_CCP_OK )
    {
        return false;
    }
    return nora_ccp_icap_start( detector->inst ) == NORA_CCP_OK;
}

void asrc_clock_control_init_reset( void )
{
    static bool initialized = false;
    if( !initialized )
    {
        const bool ok_a = ccpdet_arm( &s_det_a );
        const bool ok_b = ccpdet_arm( &s_det_b );
        if( !ok_a || !ok_b )
        {
            printf(" CCP detect init failed (a=%d b=%d)\n", (int)ok_a, (int)ok_b);
        }
        initialized = ok_a && ok_b;
    }

    ccpdet_reset( &s_det_a );
    ccpdet_reset( &s_det_b );
#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
    s_ff_candidate = 0.0f;
    s_ff_stable_count = 0u;
    s_ff_acquired = false;
#endif
}

static uint32_t ccpdet_fs_x100( const ccpdet_t* detector )
{
    const float period = detector->ema_period;
    if( !detector->inited || ( period <= 0.0f ) ) { return 0u; }
    const uint32_t timebase_hz = nora_ccp_icap_timebase_hz( detector->inst );
    return (uint32_t)( ( (float)CCPDET_STRIDE * (float)timebase_hz * 100.0f ) / period );
}

void asrc_clock_control_tick( void )
{
    /* num comes from the path too, not from the build config: the runtime gate can select a
     * RATIONAL front end (48 -> 32 kHz is 2/3), and a plan of 1/3 there would feed the servo a
     * step half of what the front end actually delivers. */
    const asrc_rate_plan_t ab_rate_plan = {
        asrc_audio_path_ab_fixed_rate_num(),
        asrc_audio_path_ab_fixed_rate_den()
    };
#if APP_B_ROUTE_USES_BA
    /* Mirror plan for the other down-sampling case (leg B at 48 kHz, leg A low): the B->A
     * engine then sees an input rate of fs_B/den, so its feed-forward ratio must be divided
     * by the same den.  At most one of the two dens is != 1. */
    const asrc_rate_plan_t ba_rate_plan = {
        asrc_audio_path_ba_fixed_rate_num(),
        asrc_audio_path_ba_fixed_rate_den()
    };
#endif
#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
    const bool ff_guard_active = APP_ASRC_FF_ACQUIRE_GUARD ||
        ( ab_rate_plan.fixed_input_den != 1u )
#if APP_B_ROUTE_USES_BA
        || ( ba_rate_plan.fixed_input_den != 1u )
#endif
        ;
#endif
    static uint32_t last = UINT32_MAX;

    /* Drain on every foreground pass; only the ratio publication stays 50 Hz. */
    ccpdet_process_periods( &s_det_a );
    ccpdet_process_periods( &s_det_b );

    const uint32_t current = GetTicks();
    if( (uint32_t)( current - last ) < APP_ASRC_FF_PERIOD_MS ) { return; }
    last = current;

    if( !s_det_a.inited || !s_det_b.inited ) { return; }
#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
    if( ff_guard_active &&
        ( ( s_det_a.capture_count < CCPDET_FF_MIN_CAPTURES ) ||
          ( s_det_b.capture_count < CCPDET_FF_MIN_CAPTURES ) ) )
    {
        return;
    }
#endif

    const float period_a = s_det_a.ema_period;
    const float period_b = s_det_b.ema_period;
    if( ( period_a <= 0.0f ) || ( period_b <= 0.0f ) ) { return; }

    const float measured_ab = period_b / period_a;
    const float planned_ab = asrc_rate_plan_step( measured_ab, &ab_rate_plan );
#if APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8
    if( ff_guard_active && !s_ff_acquired )
    {
#if APP_B_CODEC_MASTER
        const uint32_t nominal_a_hz = wm8904_get_rate_hz( I2C_INST_A );
        const uint32_t nominal_b_hz = wm8904_get_rate_hz( I2C_INST_B );
        if( ( nominal_a_hz == 0u ) || ( nominal_b_hz == 0u ) )
        {
            s_ff_candidate = 0.0f;
            s_ff_stable_count = 0u;
            return;
        }
        const float nominal_ab = asrc_rate_plan_step(
            (float)nominal_a_hz / (float)nominal_b_hz, &ab_rate_plan );
        float nominal_delta_ppm = ( planned_ab / nominal_ab - 1.0f ) * 1.0e6f;
        if( nominal_delta_ppm < 0.0f ) { nominal_delta_ppm = -nominal_delta_ppm; }
        if( nominal_delta_ppm > CCPDET_FF_NOMINAL_PPM )
        {
            s_ff_candidate = 0.0f;
            s_ff_stable_count = 0u;
            return;
        }
#endif
        if( s_ff_candidate <= 0.0f )
        {
            s_ff_candidate = planned_ab;
            s_ff_stable_count = 1u;
            return;
        }
        float delta_ppm = ( planned_ab / s_ff_candidate - 1.0f ) * 1.0e6f;
        if( delta_ppm < 0.0f ) { delta_ppm = -delta_ppm; }
        if( delta_ppm > CCPDET_FF_STABLE_PPM )
        {
            s_ff_candidate = planned_ab;
            s_ff_stable_count = 1u;
            return;
        }
        if( ++s_ff_stable_count < CCPDET_FF_STABLE_COUNT ) { return; }
        s_ff_acquired = true;
    }
#endif

    audio_app_asrc_set_ratio_ab( planned_ab );
#if APP_B_ROUTE_USES_BA
    audio_app_asrc_set_ratio_ba( asrc_rate_plan_step( period_a / period_b, &ba_rate_plan ) );
#endif
}

uint32_t asrc_clock_control_capture_count_a( void )
{
    return s_det_a.capture_count;
}

uint32_t asrc_clock_control_capture_count_b( void )
{
    return s_det_b.capture_count;
}

#define CCP_DISPLAY_WARMUP_WINDOWS  (2u)
static float    s_display_scale = 1.0f;
static uint8_t  s_display_latched;
static uint32_t s_display_accumulator;
static uint8_t  s_display_count;

static void display_scale_latch( uint32_t fs_a_x100 )
{
    if( s_display_latched || ( fs_a_x100 == 0u ) ) { return; }
    s_display_accumulator += fs_a_x100;
    if( ++s_display_count < CCP_DISPLAY_WARMUP_WINDOWS ) { return; }

    const uint32_t nominal_a_hz = wm8904_get_rate_hz( I2C_INST_A );
    const float measured_a_hz =
        ( (float)s_display_accumulator / (float)s_display_count ) / 100.0f;
    if( ( nominal_a_hz > 0u ) && ( measured_a_hz > 0.0f ) )
    {
        const float scale = (float)nominal_a_hz / measured_a_hz;
        if( ( scale > 0.98f ) && ( scale < 1.02f ) )
        {
            const uint32_t scale_x10000 = (uint32_t)( scale * 10000.0f + 0.5f );
            s_display_scale = scale;
            printf("CCP  display self-cal: scale=%lu.%04lu (fsA_nom=%lu meas=%lu Hz)\n",
                   (unsigned long)( scale_x10000 / 10000u ),
                   (unsigned long)( scale_x10000 % 10000u ),
                   (unsigned long)nominal_a_hz,
                   (unsigned long)( (uint32_t)( measured_a_hz + 0.5f ) ) );
        }
    }
    s_display_latched = 1u;
}

static uint32_t display_scaled( uint32_t raw )
{
    return (uint32_t)( (float)raw * s_display_scale + 0.5f );
}

/*
 * Read straight out of the detector rather than latching a copy at telemetry time: the per-leg
 * TDM line is printed BEFORE this module's own CCP line in the same report, so a cached value
 * would be one report (~2 s) stale exactly across a rate change -- the moment it is read.
 * Two divides per call, once per printed line, off the hot path.
 * Scaled the same way the CCP line is, so the two never disagree on screen.
 */
uint32_t asrc_clock_control_measured_fs_hz( uint8_t leg )
{
    const uint32_t x100 = ( leg == 0u ) ? ccpdet_fs_x100( &s_det_a )
                                        : ccpdet_fs_x100( &s_det_b );
    if( x100 == 0u ) { return 0u; }                 /* nothing measured yet -> caller omits it */
    return display_scaled( x100 ) / 100u;
}

void asrc_clock_control_debug_print( uint32_t fs_a_hz,
                                     uint32_t fs_b_hz,
                                     uint32_t recover_count )
{
    const uint32_t fs_a_x100 = ccpdet_fs_x100( &s_det_a );
    const uint32_t fs_b_x100 = ccpdet_fs_x100( &s_det_b );
    display_scale_latch( fs_a_x100 );
    audio_app_asrc_dbg_print( display_scaled( fs_a_hz ), display_scaled( fs_b_hz ) );
    asrc_audio_path_dbg_print();

    const unsigned long ratio_x1e6 = ( fs_b_x100 > 0u )
        ? (unsigned long)( ( (uint64_t)fs_a_x100 * 1000000ULL ) / fs_b_x100 ) : 0ul;
    const uint32_t display_a_x100 = display_scaled( fs_a_x100 );
    const uint32_t display_b_x100 = display_scaled( fs_b_x100 );
    printf("CCP  fsA=%lu.%02lu fsB=%lu.%02lu Hz  ratioAB=%lu.%06lu recover=%lu\n",
           (unsigned long)( display_a_x100 / 100u ),
           (unsigned long)( display_a_x100 % 100u ),
           (unsigned long)( display_b_x100 / 100u ),
           (unsigned long)( display_b_x100 % 100u ),
           ratio_x1e6 / 1000000ul, ratio_x1e6 % 1000000ul,
           (unsigned long)recover_count );
    if( ( s_det_a.period_overrun_count != 0u ) ||
        ( s_det_b.period_overrun_count != 0u ) )
    {
        printf("CCP  period queue overrun A/B=%lu/%lu\n",
               (unsigned long)s_det_a.period_overrun_count,
               (unsigned long)s_det_b.period_overrun_count );
    }
    const bool capture_overrun_a =
        nora_ccp_icap_overflow( NORA_CCP1, false );
    const bool capture_overrun_b =
        nora_ccp_icap_overflow( NORA_CCP2, false );
    if( capture_overrun_a || capture_overrun_b )
    {
        printf("CCP  capture FIFO overrun A/B=%u/%u\n",
               (unsigned)capture_overrun_a,
               (unsigned)capture_overrun_b );
    }
}

#else

void asrc_clock_control_init_reset( void ) { }
void asrc_clock_control_tick( void ) { }
uint32_t asrc_clock_control_capture_count_a( void ) { return 0u; }
uint32_t asrc_clock_control_capture_count_b( void ) { return 0u; }
void asrc_clock_control_debug_print( uint32_t fs_a_hz,
                                     uint32_t fs_b_hz,
                                     uint32_t recover_count )
{
    (void)fs_a_hz;
    (void)fs_b_hz;
    (void)recover_count;
}

#endif
