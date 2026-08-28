#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_app.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#include "apps/sonora_app.h"
#include "audio_transport.h"
#include "apps/audio_application_telemetry.h"
#include "audio_transport_client.h"
#include "asrc_audio_path.h"
#include "asrc_clock_control.h"
#include "audio_app_asrc.h"
#include "audio_app_meas.h"
#include "audio_transport_sai_drytest.h"
#include "apps/shared/float_conversion.h"
#include "apps/shared/LED_level_meter.h"

#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC && !APP_USE_CCP_FS_DETECT
#include "timer_app.h"
#endif

#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC && !APP_USE_CCP_FS_DETECT
/*
 * AK128 has no usable CCP fast-map for both external FS signals.  Preserve a
 * genuine asynchronous path without adding CCP ISRs by deriving the nominal
 * A:B ratio from the two independently clocked DMA block counters.  The
 * common block size cancels, so delta_blocks_a/delta_blocks_b is fs_a/fs_b.
 *
 * Two-step publication, and the order is the point:
 *
 *   1. As soon as both legs run, publish the ratio computed from the two
 *      CONFIGURED rates.  That is the invalid->valid edge asrc_apply_ratio()
 *      needs: only there does it re-centre the FIFO, re-seed step_state and
 *      recompute the fill setpoint for the new rate pair.
 *   2. Refine it from the measured block counts after 20 ms (about 60 blocks
 *      at 48 kHz), before a 64-frame FIFO can walk far under a 48/44.1-kHz
 *      mismatch.  Keeping the base count for the full stream makes the
 *      estimate increasingly precise without a RAM history buffer.
 *
 * Until 2026-08-19 this code seeded a live 1.0f instead of passing through
 * the invalid state, so a rate change after boot never produced that edge:
 * the measured ratio arrived as a live update and step_state had to crawl to
 * it at ASRC_STEP_SLEW -- measured 40 s for 48 -> 44.1 kHz, and never at all
 * for 48 -> 22.05 kHz, where the old fixed plausibility window rejected
 * 2.177 outright and left the stale 1.0 in place (correct FIFO, wrong pitch).
 * The fast rate-change path in audio_transport.c documents this failure mode
 * and relies on the re-lock driving the ratio to 0; that held for the
 * CCP-detect path, not for this one.
 *
 * The configured rates are nominal only -- each codec crystal carries its own
 * error, which is the whole reason the measurement exists -- but they are
 * accurate enough to start the resampler at the right pitch, and they bound
 * what a plausible measurement looks like far more tightly than any fixed
 * window can.
 */
#define ASRC_BLOCK_RATIO_FIRST_UPDATE_MS  (20u)

/* How far a measured ratio may sit from the nominal one before it is taken
 * for a reset race or a torn snapshot rather than for clock error.  The first
 * update spans only ~60 blocks, so +-1-block quantisation alone is ~1.7 %;
 * crystal error contributes under 0.1 % because it largely cancels in a
 * ratio.  6 % covers the former, rejects everything the old fixed [0.5, 2.0]
 * window rejected, and unlike that window admits every rate pair the rate
 * command can select.  A pair the FIFO geometry cannot carry is still handled
 * where that belongs: asrc_set_fill_target() clamps the setpoint and raises
 * the capped flag, which telemetry prints as a trailing mark on set=. */
#define ASRC_BLOCK_RATIO_TOLERANCE        (0.06f)

/* Fallback bound for a leg that reports no configured rate: wide enough for
 * the whole 8 kHz .. 96 kHz span the rate command offers (12:1). */
#define ASRC_BLOCK_RATIO_ABS_MIN          (1.0f / 13.0f)
#define ASRC_BLOCK_RATIO_ABS_MAX          (13.0f)

typedef struct
{
    uint32_t stream_epoch;
    uint32_t block_a_base;
    uint32_t block_b_base;
    uint32_t base_ms;
    uint32_t last_poll_ms;
    uint32_t rate_a_hz;        /* configured rates this base was taken under */
    uint32_t rate_b_hz;
    float    nominal_ratio_ab; /* 0 = not known; see the tolerance check */
    bool     have_base;
} asrc_block_ratio_state_t;

static asrc_block_ratio_state_t s_block_ratio_state;

/* Both directions from one A:B figure; the B->A engine takes the reciprocal.
 *
 * `ratio_ab` is the TRANSPORT ratio fs_A/fs_B -- what the two DMA block counters can see.  A
 * decimating front end sits between the transport and the resampler, so the engine behind it is
 * NOT fed fs_A: it is fed fs_A/den, and its step has to be divided by the same den or the
 * consumer drains the ring `den` times too fast.  Measured on an AK512 board the first time
 * this profile ran with the front end on: leg B at 24 kHz reported `fe=/2:24k` and
 * `step=1.99970` at the same time, with starve and the intermediate underrun both climbing
 * without bound -- correct front end, uncorrected step.
 *
 * The CCP detector already carries this correction in its own rate plan (asrc_clock_control.c);
 * this block-count path is the AK128 substitute for it and was written while the front end was
 * compiled out of that profile, so it never had it.  At most one of the two dens is != 1
 * (asrc_audio_path.c enforces that), so only one of the two lines below is ever scaled.
 *
 * The tolerance gate in the caller compares its measurement against the nominal ratio, both raw
 * transport figures, so correcting HERE -- the single point where both reach the engines --
 * keeps that comparison in one consistent unit. */
static void asrc_block_ratio_publish( float ratio_ab )
{
#if APP_ASRC_RUNTIME_48K_TO_8
    /* num/den, not 1/den: the front end's ratio is rational from 2026-08-23 (48 -> 32 kHz is
     * L=2/M=3), and scaling by the denominator alone would understate the intermediate rate by
     * the numerator -- the same uncorrected-step failure this whole block exists to fix, just
     * with a factor of 2 instead of a factor of den. */
    const float num_ab = (float)asrc_audio_path_ab_fixed_rate_num();
    const float den_ab = (float)asrc_audio_path_ab_fixed_rate_den();
    audio_app_asrc_set_ratio_ab(
        ( ratio_ab > 0.0f ) ? ( ( ratio_ab * num_ab ) / den_ab ) : 0.0f );
#if APP_B_ROUTE_USES_BA
    const float num_ba = (float)asrc_audio_path_ba_fixed_rate_num();
    const float den_ba = (float)asrc_audio_path_ba_fixed_rate_den();
    audio_app_asrc_set_ratio_ba(
        ( ratio_ab > 0.0f ) ? ( num_ba / ( ratio_ab * den_ba ) ) : 0.0f );
#endif
#else
    audio_app_asrc_set_ratio_ab( ratio_ab );
#if APP_B_ROUTE_USES_BA
    audio_app_asrc_set_ratio_ba( ( ratio_ab > 0.0f ) ? ( 1.0f / ratio_ab ) : 0.0f );
#endif
#endif
}

static void asrc_block_ratio_reset( void )
{
    s_block_ratio_state.have_base        = false;
    s_block_ratio_state.rate_a_hz        = 0u;
    s_block_ratio_state.rate_b_hz        = 0u;
    s_block_ratio_state.nominal_ratio_ab = 0.0f;
    /* Invalid, not 1.0: asrc_apply_ratio() re-centres the FIFO and re-seeds
     * the step only on the invalid->valid edge, so the ratio has to pass
     * through 0 for a rate change to be acted on at all.  The resampler stays
     * silent for the one service poll it takes to read the new rates. */
    asrc_block_ratio_publish( 0.0f );
}

static void asrc_block_ratio_service( void )
{
    const uint32_t now_ms = GetTicks();
    audio_transport_snapshot_t snapshot;

    if( s_block_ratio_state.have_base &&
        (uint32_t)( now_ms - s_block_ratio_state.last_poll_ms ) <
            ASRC_BLOCK_RATIO_FIRST_UPDATE_MS )
    {
        return;
    }
    s_block_ratio_state.last_poll_ms = now_ms;

    if( !audio_transport_snapshot_get( &snapshot ) ||
        ( snapshot.leg_count <= AUDIO_TRANSPORT_LEG_B ) )
    {
        return;
    }

    const audio_transport_leg_snapshot_t* const leg_a =
        &snapshot.legs[AUDIO_TRANSPORT_LEG_A];
    const audio_transport_leg_snapshot_t* const leg_b =
        &snapshot.legs[AUDIO_TRANSPORT_LEG_B];
    const bool both_running = leg_a->running && leg_b->running;

    /* Re-base on a rate CHANGE as well, not only on a new stream epoch: the
     * fast codec-B-only rate change keeps the epoch, and a leg that has not
     * yet republished its rate when the base is taken would otherwise pin the
     * tolerance check below to the outgoing rate pair for the whole stream.
     * Comparing the reported rates instead of deriving them makes that
     * self-healing: the stale window costs one poll, not the stream. */
    if( !both_running ||
        !s_block_ratio_state.have_base ||
        ( snapshot.stream_epoch != s_block_ratio_state.stream_epoch ) ||
        ( leg_a->configured_rate_hz != s_block_ratio_state.rate_a_hz ) ||
        ( leg_b->configured_rate_hz != s_block_ratio_state.rate_b_hz ) )
    {
        s_block_ratio_state.stream_epoch     = snapshot.stream_epoch;
        s_block_ratio_state.rate_a_hz        = leg_a->configured_rate_hz;
        s_block_ratio_state.rate_b_hz        = leg_b->configured_rate_hz;
        s_block_ratio_state.block_a_base     = leg_a->block_count;
        s_block_ratio_state.block_b_base     = leg_b->block_count;
        s_block_ratio_state.base_ms          = now_ms;
        s_block_ratio_state.have_base        = both_running;
        s_block_ratio_state.nominal_ratio_ab = 0.0f;

        /* Step 1: publish the nominal ratio for the new rate pair right here.
         * This is the invalid->valid edge; the measured refinement follows one
         * ASRC_BLOCK_RATIO_FIRST_UPDATE_MS window later. */
        if( both_running &&
            ( leg_a->configured_rate_hz != 0u ) &&
            ( leg_b->configured_rate_hz != 0u ) )
        {
            const float nominal = (float)leg_a->configured_rate_hz /
                                  (float)leg_b->configured_rate_hz;
            s_block_ratio_state.nominal_ratio_ab = nominal;
            asrc_block_ratio_publish( nominal );
        }
        return;
    }

    if( (uint32_t)( now_ms - s_block_ratio_state.base_ms ) <
        ASRC_BLOCK_RATIO_FIRST_UPDATE_MS )
    {
        return;
    }

    const uint32_t delta_a = leg_a->block_count - s_block_ratio_state.block_a_base;
    const uint32_t delta_b = leg_b->block_count - s_block_ratio_state.block_b_base;
    if( ( delta_a == 0u ) || ( delta_b == 0u ) )
    {
        return;
    }

    const float ratio_ab = (float)delta_a / (float)delta_b;
    /* A reset race or a torn snapshot must never inject a runaway step.  Judge
     * against the nominal ratio wherever it is known -- a measurement that far
     * from the configured rates is not clock error -- and against the offered
     * rate span only when a leg has not reported one. */
    const float nominal = s_block_ratio_state.nominal_ratio_ab;
    if( nominal > 0.0f )
    {
        if( ( ratio_ab < ( nominal * ( 1.0f - ASRC_BLOCK_RATIO_TOLERANCE ) ) ) ||
            ( ratio_ab > ( nominal * ( 1.0f + ASRC_BLOCK_RATIO_TOLERANCE ) ) ) )
        {
            return;
        }
    }
    else if( ( ratio_ab < ASRC_BLOCK_RATIO_ABS_MIN ) ||
             ( ratio_ab > ASRC_BLOCK_RATIO_ABS_MAX ) )
    {
        return;
    }
    asrc_block_ratio_publish( ratio_ab );
}
#endif

static void asrc_transport_prepare( uint32_t sample_rate_hz, void* user )
{
    (void)user;
    /* Safety net only: the gains are built in sonora_app_prepare at boot and the call
     * is idempotent, so this cannot recompute or reprint them on a rate change. It stays
     * so that a build that forgets the boot call still gets non-zero gains before
     * level_meter_init reads Pre_Gain_CODEC below. */
    audio_gains_init();
    level_meter_init( sample_rate_hz,
                      LEVEL_METER_DEFAULT_UPDATE_MS,
                      LEVEL_METER_DEFAULT_ATTACK_MS,
                      LEVEL_METER_DEFAULT_RELEASE_MS );
}

static void asrc_transport_reset( void* user )
{
    (void)user;
    audio_app_asrc_reset_all();
    asrc_audio_path_reset();
    /* Rates are committed by now and no domain has started, so this is the one safe
     * moment to re-order the two leg ISR priorities for this rate pair. */
    asrc_audio_path_apply_isr_priorities();
#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC && !APP_USE_CCP_FS_DETECT
    asrc_block_ratio_reset();
#endif
    asrc_clock_control_init_reset();
}

#if APP_USE_CCP_FS_DETECT
static uint32_t asrc_transport_clock_progress( uint8_t leg, void* user )
{
    (void)user;
    return ( leg == AUDIO_TRANSPORT_LEG_A )
               ? asrc_clock_control_capture_count_a()
               : asrc_clock_control_capture_count_b();
}
#endif

static const audio_transport_client_t s_asrc_transport_client =
{
#if APP_USE_CCP_FS_DETECT
    .capabilities = AUDIO_TRANSPORT_CLIENT_CAP_CLOCK_PROGRESS,
#else
    .capabilities = 0u,
#endif
    .co_clock_process = NULL,
    .leg_a_process = asrc_audio_path_leg_a_callback,
    .leg_b_process = asrc_audio_path_leg_b_callback,
    .prepare = asrc_transport_prepare,
    .reset_stream_state = asrc_transport_reset,
#if APP_USE_CCP_FS_DETECT
    .clock_progress = asrc_transport_clock_progress,
#else
    .clock_progress = NULL,
#endif
    .user = NULL,
};

/*
 * Last measured rate per logical leg, for the neutral per-leg TDM line (see the contract in
 * apps/audio_application_telemetry.h).
 *
 * TWO SOURCES, because the profiles do not have the same instrument:
 *   - AK512 links the CCP capture pair (APP_USE_CCP_FS_DETECT), which measures LRCLK directly and
 *     is quantization-free -> read it live, so the value cannot be stale across a rate change.
 *   - AK128 deliberately does NOT (see asrc_app_build_config.h: no CCP ISR is linked; the
 *     feed-forward ratio comes from the two DMA block counters in the foreground). There the only
 *     measurement in existence is the block-count rate computed once per report below, so that is
 *     what this returns -- which makes it ONE REPORT (~2 s) old on the TDM line, since those lines
 *     are printed before this function's own telemetry runs. It is a per-report average by
 *     construction, so a per-report lag costs nothing except right after a rate change.
 * 0 = nothing measured yet -> the transport omits the field.
 */
static uint32_t s_block_rate_hz[2] = { 0u, 0u };

uint32_t audio_application_leg_measured_fs_hz( uint8_t leg )
{
    if( leg > 1u ) { return 0u; }
#if APP_USE_CCP_FS_DETECT
    const uint32_t ccp_hz = asrc_clock_control_measured_fs_hz( leg );
    if( ccp_hz != 0u ) { return ccp_hz; }
#endif
    return s_block_rate_hz[leg];
}

void audio_application_telemetry_print(
    const audio_transport_snapshot_t* transport,
    uint32_t                          now_ms,
    uint32_t                          recovery_count )
{
    static uint32_t s_prev_block_a = 0u;
    static uint32_t s_prev_block_b = 0u;
    static uint32_t s_prev_ms = 0u;
    static uint32_t s_prev_epoch = UINT32_MAX;
    uint32_t fs_a_hz = 0u;
    uint32_t fs_b_hz = 0u;

    if( ( transport == NULL ) ||
        ( transport->leg_count <= AUDIO_TRANSPORT_LEG_B ) )
    {
        return;
    }

    const audio_transport_leg_snapshot_t* leg_a =
        &transport->legs[AUDIO_TRANSPORT_LEG_A];
    const audio_transport_leg_snapshot_t* leg_b =
        &transport->legs[AUDIO_TRANSPORT_LEG_B];
    /*
     * Epoch is the authoritative measurement boundary.  A qualified restart
     * resets block counters, but a fast new run can already exceed the previous
     * count before telemetry samples it; count ordering alone cannot detect that.
     */
    if( transport->stream_epoch != s_prev_epoch )
    {
        s_prev_epoch = transport->stream_epoch;
    }
    else
    {
        const uint32_t elapsed_ms = (uint32_t)(now_ms - s_prev_ms);
        if( ( s_prev_ms != 0u ) && ( elapsed_ms > 0u ) &&
            ( leg_a->block_count >= s_prev_block_a ) &&
            ( leg_b->block_count >= s_prev_block_b ) )
        {
            fs_a_hz = (uint32_t)(((uint64_t)(leg_a->block_count - s_prev_block_a) *
                                  (uint64_t)APP_BLOCK_FRAMES * 1000ULL) /
                                 elapsed_ms);
            fs_b_hz = (uint32_t)(((uint64_t)(leg_b->block_count - s_prev_block_b) *
                                  (uint64_t)APP_BLOCK_FRAMES * 1000ULL) /
                                 elapsed_ms);
        }
    }

    /* Publish for the per-leg TDM line; 0 (no valid delta this window) is kept as 0 rather than
     * held, so a stalled stream stops claiming a rate. */
    s_block_rate_hz[0] = fs_a_hz;
    s_block_rate_hz[1] = fs_b_hz;

    s_prev_block_a = leg_a->block_count;
    s_prev_block_b = leg_b->block_count;
    s_prev_ms = now_ms;

#if APP_USE_CCP_FS_DETECT
    asrc_clock_control_debug_print( fs_a_hz, fs_b_hz, recovery_count );
#else
    (void)recovery_count;
    audio_app_asrc_dbg_print( fs_a_hz, fs_b_hz );
    asrc_audio_path_dbg_print();
#endif
}

const char* sonora_app_name( void )
{
    return "ASRC App";
}

void sonora_app_print_banner( void )
{
    printf(" Mode: %s  ASRC=%s\n",
#if APP_B_CODEC_MASTER
           "B-CODEC-MASTER (WM8904-B on own XTAL, independent domain; runtime rate via *ar)",
#else
           "SPI2-INDEP-MASTER (dsPIC drives B, independent domain)",
#endif
#if APP_B_ROUTE_USES_BA
           "A<->B (bidir)"
#else
           "A->B"
#endif
    );
}

void sonora_app_prepare( void )
{
    /* Rate-independent gain table: built once here, before audio_transport_start_route
     * calls the fs-parameterized prepare hook (main.c calls this before start_audio). */
    audio_gains_init();
    audio_gains_print();

#if defined(ENA_SAI_WRAPPER_DRYTEST)
    audio_transport_sai_drytest_run();
#endif
}

void sonora_app_start_audio( void )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    // printf is polled and mirrored to both UARTs.  With almost no foreground
    // time, a 2 s report can overrun its own period and become continuous.
    // Keep boot safe; the existing *tq command can select a shorter bench period.
    audio_transport_set_dbg_period_ms( APP_ASRC_HEADROOM_DBG_PERIOD_MS );
#endif
    const audio_transport_client_bind_result_t bind_result =
        audio_transport_client_bind( &s_asrc_transport_client );
    if( bind_result != AUDIO_TRANSPORT_CLIENT_BIND_OK )
    {
        printf(" ASRC transport client bind failed: %s\n",
               audio_transport_client_bind_result_name( bind_result ) );
        return;
    }

#if defined(ENA_SAI_WRAPPER_LIVE)
    audio_transport_cmsis_sai_start();
#else
    audio_transport_hal_start();
#endif
}

void sonora_app_start_aux_output( void )
{
    /* ASRC owns no auxiliary PWM/DMA audio output. */
}

bool sonora_app_manage_audio( void )
{
#if defined(ENA_SAI_WRAPPER_LIVE)
    return audio_transport_cmsis_sai_manage();
#else
    return audio_transport_hal_manage();
#endif
}

void sonora_app_process_controls( void )
{
    /* ASRC owns no local button/touch controls. */
}

bool sonora_app_service( void )
{
#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC
#if APP_USE_CCP_FS_DETECT
    asrc_clock_control_tick();
#else
    asrc_block_ratio_service();
#endif
    audio_transport_frmerr_recover_tick();
#endif

#if APP_ASRC_MEAS_UART2_STREAM
    return audio_app_meas_stream_service() != 0;
#else
    return false;
#endif
}

void sonora_app_debug_print( void )
{
    audio_transport_dbg_print();
}
