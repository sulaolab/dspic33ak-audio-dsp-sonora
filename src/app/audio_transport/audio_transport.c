//===========================================================
// audio_transport.c
//
// Transitional shared audio integration for Sonora. This is NOT part of the public
// SPI/I2S/TDM HAL core and is NOT a CMSIS-wrapper dependency: the HAL core neither
// includes nor calls this file.
//
// This file currently owns codec/transport orchestration, callback composition,
// co-clock phase safety and recovery. Classic PCM interpretation, DSP processing,
// optional audio PWM calls and float work buffers are owned by
// apps/classic/classic_audio_path.c. ASRC processing and clock control are owned by
// apps/asrc/.
//
// Ownership: the DMA ping-pong buffers (the per-instance Tx_<name>/Rx_<name> generated
// from the instance list) remain owned by nora_spi_i2s_tdm_dspic33ak.c and are passed in as
// RX/TX half pointers. Application DSP work buffers are owned by each selected audio path.
//===========================================================

#include "resolved_board_config.h"
#include "resolved_sai_test_config.h"
#include "resolved_transport_policy.h"
#include "app_runtime_overrides.h"
#if RESOLVED_BOARD_COHERENT_OFFSET_CLOCK || RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2
#include "board/clock/sonora_clock.h"   // Q27B / Q48 case1-2: re-source CLKGEN9 from PLL2 after codec-A is up
#endif

#include <xc.h>
#include <stdint.h>
#include <string.h>

#include <stdio.h>                   // printf (start-sequence logging)

#include "board/devices/wm8904.h"    // P5: codec lifecycle -- direct, down-call
#include "hal_reset/nora_reset.h"      // core reset cause: cold vs hot codec start at boot
#include "diagnostics/app_traps.h"     // app_traps_count(): tell a CPU fault from missing wiring
#include "timer_app.h"                          // delay_ms
#include "board/audio/audio.h"   // board pin/clock port table
#include "board/audio/tdm.h"     // board TDM topology table + per-leg resolver (Step 3)
#include "audio_transport.h"
#include "audio_transport_sai_live.h"      // P5: opt-in wrapper-live verification harness
#include "apps/audio_application_telemetry.h" // selected-app diagnostic consumer
#include "audio_transport_client.h"  // application-owned processing/lifecycle contract
#include "audio_transport_control.h" // neutral leg reconfiguration request
#include "audio_transport_snapshot.h" // neutral lifecycle/health telemetry
#include "nora_spi_i2s_tdm.h"   // transport API (configure/start/stop/status/rate-cb)
#include "nora_high_res_timer.h" // convert high-res-timer counts <-> us for the telemetry
#include "hal_timer/nora_cpu_load_prof.h" // DSPload: fixed-window, owner-attributed CPU load
#include "resolved_transport_config.h" // application-neutral topology facts


// I2C bus instances for MikroBUS-A and B codecs (Sonora board: R38/R39 removed).
// Used only in this file to call wm8904_* APIs.
#define I2C_INST_A  (2u)  /* I2C2 -- WM8904-A on MikroBUS-A */
#if APP_AK128_J3_TDM_B
#define I2C_INST_B  (1u)  /* I2C1 -- WM8904-B on MikroBUS-B, DIM-P4/P6 */
#else
#define I2C_INST_B  (3u)  /* I2C3 -- WM8904-B on MikroBUS-B */
#endif

/* An explicit *ts must remain terminal until a successful explicit start
 * (normally *tr).  The manage loop otherwise owns several recovery paths that
 * can restart a stopped transport after an unrelated clock/fault event. */
static bool s_preflash_stop_latched;
static bool s_preflash_stop_confirmed;

#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
/* Set the first time a dual-codec build's WM8904-B apply fails (codec absent/unanswering
 * on its I2C bus). Checked at the top of the manage loop so a missing codec-B produces ONE
 * reported failure instead of the manage loop retrying the whole start sequence every tick
 * forever (TDM_activated is reset to 0 by the failed start's teardown, so nothing else stops
 * it re-attempting). Cleared on the next start that actually succeeds. Gated behind
 * NORA_TDM_USE_SPI2 (like the rest of this feature): a single-codec build (e.g. AK128,
 * which has no MikroBUS-B I2C at all) can never hit this condition, and every byte here
 * competes with a build that was already at 99% ROM before this feature existed.
 * Also excluded from the AK128 J3 bring-up build for the same ROM reason -- see
 * audio_transport_report_codec_b_missing() below for why that build drops the
 * latch/guidance and not just this comment's original single-codec case. */
static bool s_codec_b_missing_stop_latched;
/* GetTicks() timestamp of the last codec-B-missing guidance print (first failure or a
 * periodic repeat -- see audio_transport_dbg_print()). Repeating matters because a USB-CDC
 * terminal that reconnects after the boot-time banner already scrolled by would otherwise
 * find a silent, healthy-looking console with no way to discover why. */
static uint32_t s_codec_b_missing_last_prt_ms;
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B

/*
 * Per-leg converter role, stated once.
 *
 * At fs >= 88.2 kHz the WM8904 cannot run its ADC and DAC simultaneously (the
 * datasheet's boundary; 96 kHz is the only rate above it that this project builds),
 * so the high-rate topology splits them: A captures, B plays. Below that both legs
 * are full duplex.
 *
 * These are keyed on each leg's OWN nominal rate. Re-deriving the role at each
 * call site (or hardcoding it) is what previously made "same role, different
 * rate" unrepresentable on leg B.
 *
 * NOTE this is the BUILD-time role, matching the build's nominal rate, and a
 * runtime rate change deliberately does NOT move it:
 *
 *   - Dropping leg B 96k -> 48k leaves it DAC-only. Legal, not full-duplex, and it
 *     keeps the A->B path intact -- which is what this build is for.
 *   - Dropping leg A 96k -> 48k leaves it ADC-only, likewise.
 *
 * Letting the role follow the rate would silently turn a one-way A->B image into a
 * full-duplex one mid-run, which the ASRC route is not set up to agree with. The
 * consequence to remember when reading telemetry: after "*ar 0 x" leg A is a
 * 48 kHz-family ADC-only capture, NOT a full-duplex codec.
 */
#if RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ >= 88200u
#define AUDIO_TRANSPORT_LEG_A_ROLE   WM8904_ROLE_ADC_ONLY
#else
#define AUDIO_TRANSPORT_LEG_A_ROLE   WM8904_ROLE_ADC_DAC
#endif

#if RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ >= 88200u
#define AUDIO_TRANSPORT_LEG_B_ROLE   WM8904_ROLE_DAC_ONLY
#else
#define AUDIO_TRANSPORT_LEG_B_ROLE   WM8904_ROLE_ADC_DAC
#endif

#if RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED
// Step-1 phase-probe accumulators (SPI1 vs SPI2 ping-pong alignment). Sampled each SPI1
// block in app_block_cb_leg_a; printed on the audio_transport_dbg_print telemetry line.
static volatile uint32_t s_phase_samples    = 0u;
static volatile uint32_t s_phase_mismatch   = 0u;  // h1 != h2 at block START (coarse offset)
static volatile uint32_t s_phase_unresolved = 0u;  // either half unresolved this sample
static volatile int      s_phase_h1         = -1;  // last SPI1 active half (block start)
static volatile int      s_phase_h2         = -1;  // last SPI2 active half (block start)
// Sub-block position diff measured AT THE B-WRITE instant (after process, ~t=628us). This is
// the deadline-relevant sample: pos(SPI1)-pos(SPI2) in words, wrapped to [-half,+half). A fixed
// non-zero value = the offset that tears the late cross-fill even when block-start halves match.
static volatile int32_t  s_phase_wdiff_last = 0;
static volatile int32_t  s_phase_wdiff_min  = 32767;
static volatile int32_t  s_phase_wdiff_max  = -32768;
static volatile uint32_t s_phase_tail_tear  = 0u;  // post-write: SPI2 already transmitting b_dst half
#endif // RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED

#if RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
// Phase-mismatch guard state (opt-in). s_sync_bad_run counts consecutive blocks where SPI2
// crossed its ping-pong boundary during the SPI1 callback (= B cross-fill would tear); when it
// reaches the configured trip threshold, s_sync_resync_needed asks the manage loop to mute + re-sync
// the whole transport. Never trips with the aligned start (tail_tear=0).
static volatile uint32_t s_sync_bad_run       = 0u;
static volatile uint8_t  s_sync_resync_needed = 0u;
#endif // RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED

#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
// Startup phase-lock state. While s_stlock_verifying, the SPI1 block callback checks ALL lock
// conditions each block (|wdiff|<=tol, h1==h2, no tail_tear, safe B target) and counts consecutive
// OK blocks; at the configured qualification count it sets s_stlock_locked (and clears verifying). The start
// loop polls s_stlock_locked. Runtime (outside verify): a NULL mirror (unsafe B target) sets
// s_phase_fault_pending so the manage loop mutes + re-syncs the whole transport (ISR never restarts).
static volatile uint8_t  s_stlock_verifying    = 0u;
static volatile uint8_t  s_stlock_locked       = 0u;
static volatile uint32_t s_stlock_consec       = 0u;
static volatile uint8_t  s_phase_fault_pending = 0u;
// Consecutive UNRESOLVED_DMA_POSITION mirror results (runtime, outside the verify window). A single
// out-of-range live-DMA read at the co-clocked reload boundary is a benign transient (B skips one
// block); only a PERSISTENT run (a real phase loss) escalates to a whole-transport resync.
static volatile uint8_t  s_mirror_unresolved_run = 0u;
// last-block telemetry snapshot (for the PHLOCK try line)
static volatile int32_t  s_stlock_wdiff        = 0;
static volatile uint8_t  s_stlock_halfmm       = 0u;   // 1 = h1!=h2 this block
static volatile uint8_t  s_stlock_tailtear     = 0u;   // 1 = SPI2 half changed across callback
static volatile uint8_t  s_stlock_unsafe       = 0u;   // 1 = mirror refused (target==active)
#endif // RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED


/*
 * Prepare the selected application for the current leg-A sample rate.
 *
 * This does not initialize the SPI/I2S/TDM transport, CMSIS-SAI wrapper, or codec
 * hardware. The guard keeps restart paths from re-initializing DSP state.
 */
static void audio_transport_prepare_client( uint32_t sample_rate_hz )
{
    // (Phase D) fs-AWARE guard: the A-side DSP is tuned for the A-domain rate. Re-run only when that
    // rate actually changed, so a genuine A rate switch (*ar codec=A) re-tunes every fs-parameterized
    // module (meter/gain/tone/fx/widen/bassenh/flip4/delay), while same-rate resync restarts
    // (phase-fault / clock-resume) and B-only rate changes stay no-ops. Runs while muted inside
    // audio_transport_start_route, so re-tuning coefficients here is safe.
    static uint32_t s_tuned_fs = 0u;

    if( sample_rate_hz == s_tuned_fs )
    {
        return;
    }
    s_tuned_fs = sample_rate_hz;

    // Sample interpretation and application DSP preparation stay behind the
    // selected client's contract; transport does not select an application.
    const audio_transport_client_t* client = audio_transport_client_get();
    if( client != NULL )
    {
        client->prepare( sample_rate_hz, client->user );
    }

    // (Phase D) s_tuned_fs was set at the top of this function (fs-aware guard); nothing to latch here.
}


// The A<->B ASRC engine (FIFOs, cubic resampler, drift control, telemetry) lives in
// apps/asrc/audio_app_asrc.c/.h. ASRC route wiring and its SPI callbacks live beside that engine in
// apps/asrc/asrc_audio_path.c; this integration file registers the selected callbacks and uses the
// narrow ASRC lifecycle/telemetry interface. The per-route "output DSP" is only the LED gain meter
// (level_meter_process_i32, int32 direct -- no float convert/scratch). Call the engine via
// audio_app_asrc_push_ab/pull_ab/push_ba/pull_ba/reset_all/dbg_print.


// ---- co-clock transport wrapper: leg A's callback ALSO fills leg B's TX half (same-phase,
//      zero skew). The transport resolves the safe writable B half and owns all phase safety;
//      the selected client only interprets and processes the sample words. ----
#if RESOLVED_TRANSPORT_LEG_B_PRESENT && \
    (RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE)
static void audio_transport_process_coclock( const audio_transport_client_t* client,
                                             const int32_t*                  src,
                                             int32_t*                        dst )
{
    // Legacy co-clock topology: leg A's callback ALSO fills leg B's TX half (A/B same-phase,
    // zero skew). Choose SPI2's half by MIRRORING SPI1's just-handed fill half (`dst`), NOT
    // by reading SPI2's live TX DMA: a live snapshot taken here goes stale when SPI2 crosses
    // its own block boundary during a long (high-load) callback, so the write can land in the
    // half SPI2 is transmitting -> tearing (e.g. the 80-stage DRC load test). Mirroring is
    // deterministic and valid for the whole block. Restores the pre-refactor single-DMA0
    // (src,dst_a,dst_b) behaviour on the independent-instance HAL.
    int32_t* b_dst = NULL;
    const nora_spi_i2s_tdm_mirror_result_t b_res =
        nora_spi_i2s_tdm_inst_tx_fill_mirror(
            audio_transport_tdm_leg_b(), audio_transport_tdm_leg_a(), dst, &b_dst );
#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
    // Runtime B-write safety (outside the startup verify window). b_dst is non-NULL only on OK
    // (a NULL b_dst means B is skipped this block -- NULL-safe copy). Distinguish the mirror result
    // so a transient reload-boundary UNRESOLVED does not immediately force a whole-transport resync,
    // while a genuine UNSAFE (target == transmitting half) faults at once. ISR only sets the flag.
    if( !s_stlock_verifying )
    {
        switch( b_res )
        {
            case NORA_TDM_MIRROR_OK:
                s_mirror_unresolved_run = 0u;
                break;
            case NORA_TDM_MIRROR_UNRESOLVED_DMA_POSITION:
                // Tolerate a few consecutive transients; escalate only if it persists.
                if( s_mirror_unresolved_run < 0xFFu ) { s_mirror_unresolved_run++; }
                if( s_mirror_unresolved_run >=
                    RESOLVED_TRANSPORT_MIRROR_UNRESOLVED_TOLERANCE_BLOCKS )
                {
                    s_phase_fault_pending = 1u;
                }
                break;
            case NORA_TDM_MIRROR_UNSAFE_ACTIVE_HALF:
                s_mirror_unresolved_run = 0u;
                s_phase_fault_pending   = 1u;   // real phase problem -> resync now
                break;
            case NORA_TDM_MIRROR_BAD_ARGUMENT:
            default:
                // Should not occur on this fixed spi1/spi2/dst path; fault closed if it ever does.
                s_mirror_unresolved_run = 0u;
                s_phase_fault_pending   = 1u;
                break;
        }
    }
    // Startup verify: sample leg A/B halves at block START (t~0).
    int slk_h1 = -1, slk_h2_pre = -1;
    if( s_stlock_verifying )
    {
        slk_h1     = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_a() );
        slk_h2_pre = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
    }
#endif // RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
#if RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
    // Guard: SPI2's transmitting half at block START. If it differs from the half at the B-write
    // (post-process) instant, SPI2 crossed its boundary during the callback = a torn cross-fill.
    const int gh_pre = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
#endif // RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
#if RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED
    // (a) block-START (t~0): coarse half compare. Co-clocked in-phase => equal.
    const int h1_pre = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_a() );
    const int h2_pre = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
    s_phase_samples++;
    s_phase_h1 = h1_pre;
    s_phase_h2 = h2_pre;
    if( ( h1_pre < 0 ) || ( h2_pre < 0 ) ) { s_phase_unresolved++; }
    else if( h1_pre != h2_pre )            { s_phase_mismatch++;   }
#endif // RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED

    client->co_clock_process( src, dst, b_dst, client->user );

#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
    // Startup verify: at the B-write instant, evaluate ALL lock conditions. wdiff==0 alone is NOT
    // sufficient (HW: wdiff=0 with tail_tear=samp still tore), so tail_tear / half-mismatch / unsafe
    // target are vetoes. All must hold for the configured number of blocks to declare LOCK.
    if( s_stlock_verifying )
    {
        const int     h2_post = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
        const int32_t p1      = nora_spi_i2s_tdm_inst_tx_active_pos( audio_transport_tdm_leg_a() );
        const int32_t p2      = nora_spi_i2s_tdm_inst_tx_active_pos( audio_transport_tdm_leg_b() );
        const int32_t half = (int32_t)( RESOLVED_TRANSPORT_SLOTS_PER_FRAME *
                                        RESOLVED_TRANSPORT_BLOCK_FRAMES );
        int32_t       wd      = 0;
        uint8_t       ok      = 1u;

        if( ( p1 >= 0 ) && ( p2 >= 0 ) )
        {
            wd = p1 - p2;
            if( wd >=  half ) { wd -= 2 * half; }
            if( wd <  -half ) { wd += 2 * half; }
        }
        else { ok = 0u; }                                   // unresolved -> not OK
        const int32_t wd_abs = ( wd < 0 ) ? -wd : wd;
        const uint8_t halfmm  = ( slk_h1 < 0 || slk_h2_pre < 0 || slk_h1 != slk_h2_pre ) ? 1u : 0u;
        const uint8_t tailt   = ( slk_h2_pre < 0 || h2_post < 0 || h2_post != slk_h2_pre ) ? 1u : 0u;
        const uint8_t unsafe  = ( b_res != NORA_TDM_MIRROR_OK ) ? 1u : 0u;

        if( wd_abs > (int32_t)RESOLVED_TRANSPORT_PHASE_LOCK_TOLERANCE_WORDS ) { ok = 0u; }
        if( halfmm || tailt || unsafe )                { ok = 0u; }

        s_stlock_wdiff = wd; s_stlock_halfmm = halfmm; s_stlock_tailtear = tailt; s_stlock_unsafe = unsafe;
        if( ok )
        {
            if( ++s_stlock_consec >=
                (uint32_t)RESOLVED_TRANSPORT_PHASE_LOCK_REQUIRED_BLOCKS )
            {
                s_stlock_locked = 1u;
                s_stlock_verifying = 0u;
            }
        }
        else { s_stlock_consec = 0u; }
    }
#endif // RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
#if RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
    // Guard post-check (at the B-write instant): SPI2's half changed during the callback => tear.
    // Count consecutive bad blocks; on TRIP, ask the manage loop to mute + re-sync the transport.
    {
        const int gh_post = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
        if( ( gh_pre >= 0 ) && ( gh_post >= 0 ) && ( gh_post != gh_pre ) )
        {
            if( ++s_sync_bad_run >=
                (uint32_t)RESOLVED_TRANSPORT_SYNC_GUARD_TRIP_BLOCKS )
            {
                s_sync_resync_needed = 1u;
            }
        }
        else
        {
            s_sync_bad_run = 0u;
        }
    }
#endif // RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
#if RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED
    // (b) B-WRITE instant (~t=628us, after the heavy DSP): the deadline-relevant sample.
    //   wdiff = pos(SPI1)-pos(SPI2) in words, wrapped [-half,+half): a fixed non-zero =
    //           sub-block offset (invisible to the block-start half compare).
    //   tail_tear = SPI2's half CHANGED vs block-start => it crossed its boundary during the
    //           callback and is now transmitting the half we just filled = the actual tear.
    {
        const int32_t p1 = nora_spi_i2s_tdm_inst_tx_active_pos( audio_transport_tdm_leg_a() );
        const int32_t p2 = nora_spi_i2s_tdm_inst_tx_active_pos( audio_transport_tdm_leg_b() );
        if( ( p1 >= 0 ) && ( p2 >= 0 ) )
        {
            const int32_t half =
                (int32_t)( RESOLVED_TRANSPORT_SLOTS_PER_FRAME *
                           RESOLVED_TRANSPORT_BLOCK_FRAMES );
            int32_t d = p1 - p2;
            if( d >=  half ) { d -= 2 * half; }
            if( d <  -half ) { d += 2 * half; }
            s_phase_wdiff_last = d;
            if( d < s_phase_wdiff_min ) { s_phase_wdiff_min = d; }
            if( d > s_phase_wdiff_max ) { s_phase_wdiff_max = d; }
        }
        const int h2_post = nora_spi_i2s_tdm_inst_tx_active_half( audio_transport_tdm_leg_b() );
        if( ( h2_pre >= 0 ) && ( h2_post >= 0 ) && ( h2_post != h2_pre ) ) { s_phase_tail_tear++; }
    }
#endif // RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED
}
#endif // co-clock topology with leg B


/*
 * Co-clock logical leg-A callback. The wrapper owns transport topology and
 * phase safety, then delegates sample interpretation to the selected client.
 */
#if RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE
static void app_block_cb_leg_a( const int32_t* src, int32_t* dst, void* user )
{
    (void)user;
    const audio_transport_client_t* client = audio_transport_client_get();
    if( ( client == NULL ) || ( client->co_clock_process == NULL ) )
    {
        return;
    }
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    audio_transport_process_coclock( client, src, dst );
#else
    client->co_clock_process( src, dst, NULL, client->user );
#endif
}
#endif // co-clock topology


// NOTE: the _DMAxInterrupt vectors are NOT here and are not the app's concern: the HAL
// defines them itself inside nora_spi_i2s_tdm_dspic33ak.c (generated from the instance list),
// built with the transport so even a CMSIS-only configuration gets them. audio_app only
// registers the block callback (above).


//===========================================================
// TDM stream lifecycle / orchestration (moved from main.c).
//
// audio_app is the TOP of the audio transport call tree: main() chooses HAL-direct
// or CMSIS-SAI wrapper route, then pumps the matching manage function. Everything
// below is called DOWNWARD and DIRECTLY (codec = board/devices/wm8904,
// transport = HAL core or CMSIS wrapper, config = audio_app, DSP = this file).
// The one allowed indirection is the HAL core's block callback (app_block_cb, above)
// and the rate-change callback below, both registered with the core.
//===========================================================

typedef enum
{
    AUDIO_TRANSPORT_ROUTE_HAL_DIRECT = 0,
    AUDIO_TRANSPORT_ROUTE_CMSIS_SAI,
} audio_transport_route_t;

// "Stream should be running" state owned by the app (distinct from the core's
// is_running(): this guards the codec+transport bring-up/teardown sequence).
static uint8_t TDM_activated = 0;
static uint32_t s_stream_epoch = 0u;
static audio_transport_transition_reason_t s_last_transition =
    AUDIO_TRANSPORT_TRANSITION_NONE;
static audio_transport_transition_error_t s_last_transition_error =
    AUDIO_TRANSPORT_TRANSITION_ERROR_NONE;
static bool s_stream_qualified = false;
static bool s_transition_failed = false;
static bool s_mute_held_by_failed_transition = false;
static uint32_t s_current_configured_rate_hz[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS] =
{
    RESOLVED_TRANSPORT_LEG_A_INITIAL_RATE_HZ,
    RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ,
};

static bool audio_transport_start_route_impl( audio_transport_route_t route,
                                        bool unmute_on_success,
                                        audio_transport_transition_reason_t reason );
static void audio_transport_start_route( audio_transport_route_t route,
                                   audio_transport_transition_reason_t reason );
static bool audio_transport_manage_route( audio_transport_route_t route );
static bool audio_transport_restart_route( audio_transport_route_t route,
                                     audio_transport_transition_reason_t reason );
static bool audio_transport_restart_route_muted( audio_transport_route_t route );

static void audio_transport_mark_qualified(
    audio_transport_transition_reason_t reason )
{
    s_stream_epoch++;
    s_last_transition = reason;
    s_stream_qualified = true;
    s_last_transition_error = AUDIO_TRANSPORT_TRANSITION_ERROR_NONE;
    s_transition_failed = false;
    s_mute_held_by_failed_transition = false;
}

static void audio_transport_mark_transition_failed(
    audio_transport_transition_error_t error )
{
    s_stream_qualified = false;
    s_last_transition_error = error;
    s_transition_failed = true;
    s_mute_held_by_failed_transition = true;
}

#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
// Text shared by the first-failure print (below) and the periodic repeat in
// audio_transport_dbg_print(). Kept as one function so the two call sites can never drift.
static void audio_transport_print_codec_b_missing_guidance( void )
{
    printf(" audio_transport_start: WM8904-B did not answer -- stopping (NOT retrying).\n" );

    /*
     * A TRAP SINCE POWER-ON MEANS THIS IS PROBABLY NOT WIRING.  The wiring questions
     * below are the right first move for a board that never had a working leg B, and
     * completely the wrong move for a leg B that WAS working and then stopped: a trap
     * resets the part, so the restart that follows finds the codec mid-reconfigure and
     * reports it as absent.  Measured 2026-08-27: every 24 / 22.05 / 11.025 kHz `*ar`
     * on the AK512 BiDir image takes a STACK ERROR at __CCP2Interrupt+0 and warm-boots,
     * and this text sent the reader to check jumpers on a board whose jumpers were fine
     * (GitHub issue #4).
     *
     * app_traps_count() is the right question and not app_traps_previous_get(): the
     * record is consumed by the boot report, the COUNTER deliberately survives it, so
     * this still fires when the fault happened several restarts ago.
     */
    const uint32_t traps = app_traps_count();
    if( traps != 0u )
    {
        printf("   NOTE: %lu trap(s) since power-on -- suspect a CPU FAULT, not wiring.\n"
               "         A trap resets the part; the restart after it can report a\n"
               "         healthy codec as absent.  Read the boot banner's\n"
               "         \"AK TRAP on the previous run\" block FIRST, and only work\n"
               "         through the wiring list below if there is no trap there.\n",
               (unsigned long)traps );
    }

    printf("   Check, then power-cycle or *tr to try again:\n"
           "   1) Is a SECOND WM8904 codec board fitted on MikroBUS-B?\n"
           "   2) Has the Curiosity Platform's A/B I2C bridge (R38/R39) been removed?\n"
           "   3) Running with only ONE codec board (Classic only)? Set\n"
           "      APP_REQ_MIKROB_WM8904 to 0 in src/app/app_specific_config_defs.h and rebuild.\n" );
}

// Called from every WM8904-B apply-failure site in a dual-codec build. Latches the stop
// (see s_codec_b_missing_stop_latched) and prints the guidance once immediately;
// audio_transport_dbg_print() takes over repeating it every 2 s from here on.
static void audio_transport_report_codec_b_missing( void )
{
    s_codec_b_missing_stop_latched = true;
    s_codec_b_missing_last_prt_ms = GetTicks();
    audio_transport_print_codec_b_missing_guidance();
}
#elif NORA_TDM_USE_SPI2
// AK128 J3 bring-up (APP_BUILD_ASRC_AK128_CODEC_BIDIR) is ROM-critical (98% at
// merge time) and its whole point is the P0/P1 hardware acceptance sequence
// that finds a missing/unanswering WM8904-B by hand before any image is
// trusted -- so the multi-line guidance text and its stop-latch are dropped
// here rather than gated per call site. The apply failure itself still prints
// (see the unconditional "WM8904-B apply failed" lines at each call site) and
// the manage loop simply retries every tick, same as before this feature
// existed. Stub keeps every call site below unchanged.
static void audio_transport_report_codec_b_missing( void )
{
}
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B

// Analog mute/unmute both codecs (A, and B if the dual-SPI MikroB topology is used).
// Brackets the stop side of a stop/restart so a rate-switch/forced-restart doesn't pop.
static void audio_transport_mute( bool mute )
{
    wm8904_set_analog_output_mute( I2C_INST_A, mute );
#if NORA_TDM_USE_SPI2
    wm8904_set_analog_output_mute( I2C_INST_B, mute );
#endif // NORA_TDM_USE_SPI2
}

// Route-aware transport teardown. The two transport routes commit the HAL in different config
// modes, so they must tear down through different APIs:
//   HAL_DIRECT : configure_system() -> SYSTEM mode -> stop_all_domains() + close().
//   CMSIS_SAI  : the wrapper's inst_configure(spi1) -> SINGLE mode, where stop_all_domains()
//                returns ERR_CONFIG_MODE (no HW change). Tear down through the wrapper's own
//                disable path (audio_transport_sai_live_stop -> inst_stop(spi1) + close()).
// TDM_activated is cleared ONLY after a successful teardown -- a failed stop must not masquerade
// as stopped (the stream may still be running; the caller/telemetry should see that).
static bool audio_transport_stop_route( audio_transport_route_t route )
{
    bool ok;

    if( !TDM_activated )
    {
        return true;   // already stopped (idempotent)
    }

    // R(review): mute the codecs BEFORE stopping the transport. Otherwise a path that stops
    // without an explicit pre-mute (e.g. the is_active()==false clock-loss fallback in
    // audio_transport_manage) would leave the WM8904 unmuted with no BCLK/FS -> DC-servo / output-stage
    // pop. Stop always means "no audio", so muting first is always correct (idempotent).
    audio_transport_mute( true );

    if( route == AUDIO_TRANSPORT_ROUTE_CMSIS_SAI )
    {
#if RESOLVED_SAI_TEST_LIVE_ENABLED
        ok = audio_transport_sai_live_stop();
#else
        ok = true;   // CMSIS-LIVE route not built in this config -- nothing to tear down
#endif
    }
    else
    {
        // Stop the whole transport (every sync domain / every leg) then release the shared port.
        // Domain-level teardown mirrors start_all_domains(): no per-leg enumeration, so added legs
        // (SPI3/SPI4, extra domains) are covered automatically and none leak.
        ok = nora_spi_i2s_tdm_stop_all_domains();
        if( ok )
        {
            ok = nora_spi_i2s_tdm_close();
        }
    }

    if( !ok )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_STOP_FAILED );
        printf(" audio_transport_stop: transport teardown FAILED route=%d err=%d (still active)\n",
               (int)route, (int)nora_spi_i2s_tdm_get_last_error() );
        return false;   // do NOT fake a stop
    }

    TDM_activated = 0;
    s_stream_qualified = false;
    printf(" audio_transport_stop: TDM_activated=0 route=%d\n", (int)route );
    return true;
}


// Public HAL-direct stop wrapper (unchanged signature). The CMSIS route stops via
// audio_transport_cmsis_sai_stop(); internal fault/restart paths call audio_transport_stop_route(route).
void audio_transport_stop( void )
{
    // Compat wrapper: dispatch to the selected route per the resolved SAI selection, matching
    // audio_transport_start/manage/restart (header contract). A CMSIS-LIVE build commits the HAL in
    // SINGLE mode, where the HAL_DIRECT teardown (stop_all_domains) would no-op with ERR_CONFIG_MODE.
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    (void)audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI );
#else
    (void)audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT );
#endif
}

bool audio_transport_stop_for_flash( void )
{
    bool mute_ok;
    bool stopped;

    /* Mute and verify first.  A programmer reset while HPOUT is live is the
     * POP hazard; stopping DMA first still leaves the codec output live during
     * the I2C transaction.  Repeating *ts re-proves the quiet state. */
    s_preflash_stop_latched = true;
    mute_ok = wm8904_set_analog_output_mute_verified( I2C_INST_A, true );
#if NORA_TDM_USE_SPI2
    mute_ok = wm8904_set_analog_output_mute_verified( I2C_INST_B, true ) && mute_ok;
#endif

#if RESOLVED_SAI_TEST_LIVE_ENABLED
    stopped = audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI );
#else
    stopped = audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT );
#endif

    s_preflash_stop_confirmed = mute_ok && stopped;
    if( s_preflash_stop_confirmed )
    {
        /* This phrase is the flash-tool gate.  It must never be emitted by
         * either failure path below. */
        printf(" audio transport: stopped by *ts (analog mute verified, TDM/DMA halted)\n");
    }
    else
    {
        printf(" audio transport: *ts stopped TDM/DMA=%d, but analog mute NOT verified; "
               "do NOT flash or reset this board yet\n", stopped ? 1 : 0);
    }
    return s_preflash_stop_confirmed;
}


void audio_transport_stop_for_flash_report( void )
{
    if( s_preflash_stop_confirmed )
    {
        printf(" ?ts: stopped by *ts (analog mute verified, TDM/DMA halted)\n");
    }
    else if( s_preflash_stop_latched )
    {
        printf(" ?ts: *ts did NOT establish verified analog mute; do NOT flash or reset\n");
    }
    else
    {
        printf(" ?ts: no pre-flash stop has been confirmed since boot or restart\n");
    }
}


static bool audio_transport_bind_transport_once( void )
{
    // One-time binding, done by the orchestrator (this layer) before the first
    // configure()/start()/is_active(): bind the board/clock PORT to the HAL core.
    // Keeping it here (not in main) leaves main an entry point only. It is a downward,
    // direct call -- no callbacks added.
    static bool s_bound = false;
    if( !s_bound )
    {
        // set_port() is a fail-closed bool API (rejects while opened/running). Latch s_bound only
        // on success so a rejected bind is retried on the next call rather than silently skipped.
        if( !nora_spi_i2s_tdm_set_port( &audio_transport_board_port ) )   // pin/CLC + clock hooks -> core
        {
            return false;   // caller must NOT proceed with codec shutdown/init + transport start
        }
        s_bound = true;
    }
    return true;
}

#if RESOLVED_TRANSPORT_DUAL_CLOCK_PROGRESS_ENABLED
// Main-loop liveness watchdog. FRMERR is sampled from the RX-block ISR, so it cannot detect a
// transport whose RX DMA has stopped completely. The client's clock-progress source is independent
// of that DMA: if framing keeps advancing while block_count does not, the transport is dead rather
// than clockless.
typedef struct {
    uint32_t last_block_count;
    uint32_t last_capture_count;
    uint32_t block_progress_ms;
    uint32_t clock_progress_ms;
    bool     seeded;
} tdm_liveness_watch_t;

static tdm_liveness_watch_t s_live_a;
static tdm_liveness_watch_t s_live_b;

static void tdm_liveness_watch_reset( void )
{
    memset( &s_live_a, 0, sizeof(s_live_a) );
    memset( &s_live_b, 0, sizeof(s_live_b) );
}

static bool tdm_liveness_inst_stalled( tdm_liveness_watch_t* watch,
                                       const nora_spi_i2s_tdm_status_t* status,
                                       uint32_t capture_count,
                                       uint32_t now_ms )
{
    if( !status->running )
    {
        watch->seeded = false;
        return false;
    }
    if( !watch->seeded )
    {
        watch->last_block_count   = status->block_count;
        watch->last_capture_count = capture_count;
        watch->block_progress_ms  = now_ms;
        watch->clock_progress_ms  = now_ms;
        watch->seeded             = true;
        return false;
    }
    if( status->block_count != watch->last_block_count )
    {
        watch->last_block_count  = status->block_count;
        watch->block_progress_ms = now_ms;
    }
    if( capture_count != watch->last_capture_count )
    {
        watch->last_capture_count = capture_count;
        watch->clock_progress_ms  = now_ms;
    }

    const bool clock_alive = (uint32_t)( now_ms - watch->clock_progress_ms )
                             < RESOLVED_TRANSPORT_LIVENESS_STALL_TIMEOUT_MS;
    const bool blocks_dead = (uint32_t)( now_ms - watch->block_progress_ms )
                             >= RESOLVED_TRANSPORT_LIVENESS_STALL_TIMEOUT_MS;
    return clock_alive && blocks_dead;
}

static bool tdm_liveness_stalled( uint32_t now_ms )
{
    if( !TDM_activated )
    {
        tdm_liveness_watch_reset();
        return false;
    }

    const audio_transport_client_t* client = audio_transport_client_get();
    if( ( client == NULL ) ||
        ( ( client->capabilities & AUDIO_TRANSPORT_CLIENT_CAP_CLOCK_PROGRESS ) == 0u ) ||
        ( client->clock_progress == NULL ) )
    {
        tdm_liveness_watch_reset();
        return false;
    }

    nora_spi_i2s_tdm_status_t st_a;
    if( !nora_spi_i2s_tdm_get_status( &st_a, false ) )
    {
        return false;
    }
    bool stalled = tdm_liveness_inst_stalled( &s_live_a, &st_a,
                                              client->clock_progress(
                                                  AUDIO_TRANSPORT_LEG_A, client->user ),
                                              now_ms );
// A masked leg B cannot be judged for liveness: block_count is the very evidence of progress,
// and it is frozen by design. Including it would declare a healthy transport stalled every time.
#if NORA_TDM_USE_SPI2 && !RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
    nora_spi_i2s_tdm_inst_t* leg_b = audio_transport_tdm_leg_b();
    nora_spi_i2s_tdm_status_t st_b;
    if( ( leg_b != NULL ) && nora_spi_i2s_tdm_inst_get_status( leg_b, &st_b, false ) )
    {
        stalled = tdm_liveness_inst_stalled( &s_live_b, &st_b,
                                             client->clock_progress(
                                                 AUDIO_TRANSPORT_LEG_B, client->user ),
                                             now_ms ) || stalled;
    }
#endif
    return stalled;
}


// Bit-slip AUTO-RECOVERY (connector-glitch frame-slip). A sustained FRMERR on either leg starts a
// muted recovery episode. The proven full restart (== *nt03) is retried until both legs advance
// blocks again; a frozen leg cannot re-trigger from its stopped FRMERR counter, so liveness drives
// retries. Exhausting the bounded attempt budget latches safe-mute. CHECK_MS/COOLDOWN_MS prevent a
// transient from causing a tight restart loop. s_frmerr_recover_count reports restart attempts.
static uint32_t s_frmerr_recover_count = 0u;

uint32_t audio_transport_frmerr_recover_count( void )
{
    return s_frmerr_recover_count;
}

// Tier 3 latch. Declared outside the AUTORECOVER guard so the accessor always links (returns
// false when auto-recover is compiled out). Only ever SET inside the guarded recover tick.
static bool s_frmerr_safe_mute = false;

// True once the recovery has latched a safe-mute (could not re-lock). Shown on telemetry so the
// operator sees "silent fault" instead of a mystery. Cleared only by a full restart / power cycle.
bool audio_transport_frmerr_safe_mute( void ) { return s_frmerr_safe_mute; }

#if RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED
static uint8_t s_frmerr_restarts   = 0u;   // restart attempts in the current recovery episode
static bool    s_frmerr_recovering = false;// in a mute+restart-until-healthy episode
static bool    s_frmerr_force_trip = false;// TEST (*nt43): force one recovery episode on next tick

// TEST hook: arm a one-shot forced recovery so the restart-until-healthy path can be exercised
// without a physical FS disturbance (see audio_transport_frmerr_recover_tick). It runs the same restart
// (== *nt03) a real slip would: on this fix the transport goes healthy and unmutes; if it could
// not re-lock it would latch a safe-mute -- never a loud/deadlocked stream.
void audio_transport_frmerr_force_trip( void ) { s_frmerr_force_trip = true; }

bool audio_transport_frmerr_autorecovery_available( void ) { return true; }

// Recovery must prove more than "DMA moved". Count only NEW blocks observed after the muted
// restart, and reset the clean run whenever FRMERR is observed. This prevents a continuously
// misframed-but-moving stream from being declared healthy and unmuted.
typedef struct {
    uint32_t last_block_count;
    uint32_t last_frm_count;
    uint32_t clean_blocks;
    bool     seeded;
} frmerr_clean_watch_t;

static bool frmerr_clean_watch_update( frmerr_clean_watch_t* watch,
                                       const nora_spi_i2s_tdm_status_t* status )
{
    if( !status->running )
    {
        watch->seeded = false;
        watch->clean_blocks = 0u;
        return false;
    }
    if( !watch->seeded )
    {
        watch->last_block_count = status->block_count;
        watch->last_frm_count   = status->err_frm_block_count;
        watch->clean_blocks     = 0u;
        watch->seeded           = true;
        return false;
    }

    const uint32_t new_blocks = status->block_count - watch->last_block_count;
    if( ( status->err_frm_block_count != watch->last_frm_count ) ||
        ( status->frmerr_consecutive_blocks != 0u ) )
    {
        watch->clean_blocks = 0u;
    }
    else if( new_blocks != 0u )
    {
        const uint32_t room = UINT32_MAX - watch->clean_blocks;
        watch->clean_blocks += ( new_blocks > room ) ? room : new_blocks;
    }
    watch->last_block_count = status->block_count;
    watch->last_frm_count   = status->err_frm_block_count;
    return watch->clean_blocks >= RESOLVED_TRANSPORT_RECOVERY_QUALIFY_BLOCKS;
}

static bool frmerr_transport_clean( frmerr_clean_watch_t* watch_a,
                                    frmerr_clean_watch_t* watch_b )
{
    nora_spi_i2s_tdm_status_t st_a;
    bool clean = nora_spi_i2s_tdm_get_status( &st_a, false )
                 && frmerr_clean_watch_update( watch_a, &st_a );
#if NORA_TDM_USE_SPI2 && !RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
    nora_spi_i2s_tdm_inst_t* leg_b = audio_transport_tdm_leg_b();
    nora_spi_i2s_tdm_status_t st_b;
    clean = ( leg_b != NULL )
            && nora_spi_i2s_tdm_inst_get_status( leg_b, &st_b, false )
            && frmerr_clean_watch_update( watch_b, &st_b )
            && clean;
#else
    // Leg B's block ISR is masked (see RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED), so its
    // block_count never advances. This watcher counts CLEAN BLOCKS -- with a frozen counter it
    // would never qualify and would hold every recovery episode open forever. Leg A carries the
    // verdict, which is sound here: one clock feeds both legs, and B's data is A's mirror.
    (void)watch_b;
#endif
    return clean;
}

// Worst consecutive-FRMERR run across both legs (0 when clean; also 0 when a leg is frozen).
static uint32_t frmerr_worst_consecutive( void )
{
    uint32_t consec = 0u;
    nora_spi_i2s_tdm_status_t st;
    if( nora_spi_i2s_tdm_get_status( &st, false ) ) { consec = st.frmerr_consecutive_blocks; }
    // Masked leg B: its frmerr bookkeeping is ISR-maintained and therefore frozen. Reading it
    // would report a stale run as if it were current, so leg A alone answers here.
#if NORA_TDM_USE_SPI2 && !RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
    {
        nora_spi_i2s_tdm_inst_t*  leg_b = audio_transport_tdm_leg_b();
        nora_spi_i2s_tdm_status_t st2;
        if( ( leg_b != NULL ) &&
            nora_spi_i2s_tdm_inst_get_status( leg_b, &st2, false ) &&
            ( st2.frmerr_consecutive_blocks > consec ) )
        {
            consec = st2.frmerr_consecutive_blocks;
        }
    }
#endif
    return consec;
}

// One restart attempt: the proven full re-lock (audio_transport_restart == *nt03: mute -> stop -> codec
// re-init -> phase-locked start -- the ONLY action that re-locks a persistently-slipped codec-
// master leg on this HW; a transport-only re-arm does NOT), then wait (bounded) for the transport
// to report HEALTHY. Blocks the main loop for the attempt -- acceptable for a rare recovery event.
static bool frmerr_restart_attempt( void )
{
    bool started;
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    started = audio_transport_restart_route_muted( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI );
#else
    started = audio_transport_restart_route_muted( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT );
#endif
    if( !started ) { return false; }

    frmerr_clean_watch_t watch_a = {0};
    frmerr_clean_watch_t watch_b = {0};
    const uint32_t t0 = GetTicks();
    while( (uint32_t)( GetTicks() - t0 ) <
           RESOLVED_TRANSPORT_RECOVERY_QUALIFY_TIMEOUT_MS )
    {
        if( frmerr_transport_clean( &watch_a, &watch_b ) ) { return true; }
    }
    return false;
}
#else
void audio_transport_frmerr_force_trip( void ) { }
bool audio_transport_frmerr_autorecovery_available( void ) { return false; }
#endif // RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED

void audio_transport_frmerr_recover_tick( void )
{
#if RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED
    static uint32_t last          = UINT32_MAX;
    static uint32_t cooldown_from = 0u;
    static bool     in_cooldown   = false;
    const uint32_t  cur           = GetTicks();

    // TEST hook (*nt43): force one recovery episode on demand (physical FS noise is hard to
    // reproduce). Bypass the self-gate/cooldown and clear any prior safe-mute latch / in-flight
    // episode so it is repeatable; it then runs the same mute+restart-until-healthy path a real
    // trip does.
    const bool forced = s_frmerr_force_trip;
    if( forced )
    {
        s_frmerr_force_trip = false;
        s_frmerr_safe_mute  = false;
        s_frmerr_recovering = false;
        s_frmerr_restarts   = 0u;
        in_cooldown         = false;
        tdm_liveness_watch_reset();
    }
    else
    {
        if( (uint32_t)( cur - last ) < RESOLVED_TRANSPORT_RECOVERY_CHECK_PERIOD_MS ) { return; }
        last = cur;

        if( s_frmerr_safe_mute ) { return; }   // latched silent fault: no more retries

        if( in_cooldown )
        {
            if( (uint32_t)( cur - cooldown_from ) < RESOLVED_TRANSPORT_RECOVERY_COOLDOWN_MS ) { return; }
            in_cooldown = false;
        }
    }

    // ENTER a recovery episode on a fresh FRMERR trip (>= threshold) or the forced test hook.
    // While recovering we DRIVE ON HEALTH, not the FRMERR counter (which freezes at 0 on a dead
    // leg and could never re-trigger -- the "doesn't recover" bug).
    const bool stalled = !forced && tdm_liveness_stalled( cur );
    if( !s_frmerr_recovering )
    {
        const uint32_t frm_run = frmerr_worst_consecutive();
        if( !forced && !stalled &&
            ( frm_run < RESOLVED_TRANSPORT_FRMERR_TRIGGER_BLOCKS ) )
        {
            return;
        }
        printf(" [tdm-recover] trigger=%s frm_run=%lu\n",
               stalled ? "liveness" : ( forced ? "forced" : "frmerr" ),
               (unsigned long)frm_run );
        s_frmerr_recovering = true;
        s_frmerr_restarts   = 0u;
        tdm_liveness_watch_reset();
    }

    // Anti-blast: keep MUTED across the whole episode. The recovery-only restart path also holds
    // mute through codec init and transport start; only the clean-frame success branch releases it.
    audio_transport_mute( true );

    s_frmerr_recover_count++;
    s_frmerr_restarts++;

    if( frmerr_restart_attempt() )
    {
        // Transport advanced through a clean-frame window while muted -> episode done.
        s_frmerr_recovering = false;
        tdm_liveness_watch_reset();
        audio_transport_mark_qualified(
            AUDIO_TRANSPORT_TRANSITION_FRMERR_RECOVERY );
        audio_transport_mute( false );
        in_cooldown = false;   // clean-window proof replaces the old post-success blind period
    }
    else if( s_frmerr_restarts >= RESOLVED_TRANSPORT_RECOVERY_MAX_RESTARTS )
    {
        // Restart could not re-lock within the attempt budget -> "correct deadlock": stay MUTED
        // and latch a fault (silence, not a blast, not a runaway). A full reboot / power cycle is
        // the operator's call. (Ongoing noise makes attempts no-ops; once it stops a retry wins,
        // so this latch only fires on a genuinely unrecoverable misalignment.)
        s_frmerr_safe_mute  = true;
        s_frmerr_recovering = false;
    }
    // A failed attempt stays muted and retries after the cooldown. Successful recovery resumes
    // monitoring immediately so a recurring fault cannot produce a 500-ms unobserved blast.
    if( s_frmerr_recovering )
    {
        cooldown_from = GetTicks();
        in_cooldown   = true;
    }
#endif // RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED
}

void audio_transport_frmerr_reset( void )
{
#if RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED
    // Clear the HAL's consecutive-frmerr counters (clear=true) on both legs and reseed the liveness
    // watch, then drop any in-flight recovery bookkeeping. After a *ap B-only restart, B's clock has
    // returned and clean blocks are flowing, so this leaves the manage loop with nothing to recover
    // from -- no log-flooding full-transport restart.
    nora_spi_i2s_tdm_status_t st;
    (void)nora_spi_i2s_tdm_get_status( &st, true );
#if NORA_TDM_USE_SPI2
    {
        nora_spi_i2s_tdm_inst_t*  leg_b = audio_transport_tdm_leg_b();
        nora_spi_i2s_tdm_status_t st2;
        if( leg_b != NULL ) { (void)nora_spi_i2s_tdm_inst_get_status( leg_b, &st2, true ); }
    }
#endif
    tdm_liveness_watch_reset();
    s_frmerr_recovering = false;
    s_frmerr_restarts   = 0u;
#endif // RESOLVED_TRANSPORT_FRMERR_AUTORECOVERY_ENABLED
}
#else // !RESOLVED_TRANSPORT_DUAL_CLOCK_PROGRESS_ENABLED
// Keep the public recovery/diagnostic API linkable in builds without the two-CCP clock detector.
// Those builds cannot distinguish a dead transport from a stopped external clock, so automatic
// liveness recovery is intentionally unavailable and the debug force hook is a harmless no-op.
uint32_t audio_transport_frmerr_recover_count( void ) { return 0u; }
bool audio_transport_frmerr_safe_mute( void ) { return false; }
void audio_transport_frmerr_force_trip( void ) { }
void audio_transport_frmerr_recover_tick( void ) { }
void audio_transport_frmerr_reset( void ) { }
bool audio_transport_frmerr_autorecovery_available( void ) { return false; }
#endif // RESOLVED_TRANSPORT_DUAL_CLOCK_PROGRESS_ENABLED


static void audio_transport_snapshot_copy_leg(
    audio_transport_leg_snapshot_t*              out,
    const nora_spi_i2s_tdm_status_t* status,
    uint8_t                                physical_spi_instance,
    uint32_t                               configured_rate_hz,
    uint32_t                               callback_deadline_us10 )
{
    out->present = true;
    out->active = status->active;
    out->running = status->running;
    out->deadline_miss_latched = status->block_deadline_miss_count != 0u;
    out->physical_spi_instance = physical_spi_instance;
    out->configured_rate_hz = configured_rate_hz;
    out->block_count = status->block_count;
    out->callback_last_us10 = status->load.last_us10;
    out->callback_peak_us10 = status->load.max_us10;
    out->callback_deadline_us10 = callback_deadline_us10;
    out->deadline_miss_count = status->block_deadline_miss_count;
    out->rx_dma_overrun_count = status->rx_dma_overrun_count;
    out->rx_dma_other_irq_count = status->rx_dma_other_irq_count;
    out->rx_dma_last_status = status->rx_dma_last_status;
    out->rx_overrun_block_count = status->err_rov_block_count;
    out->tx_underrun_block_count = status->err_tur_block_count;
    out->frame_error_block_count = status->err_frm_block_count;
}

static bool audio_transport_controller_timing(
    const transport_static_cfg_t*     transport_cfg,
    const transport_leg_static_cfg_t* leg_cfg,
    uint32_t*                         rate_hz,
    uint32_t*                         deadline_us10 )
{
    if( ( transport_cfg == NULL ) || ( leg_cfg == NULL ) ||
        ( rate_hz == NULL ) || ( deadline_us10 == NULL ) ||
        ( leg_cfg->clock_source != TRANSPORT_CLOCK_SOURCE_CONTROLLER ) ||
        ( leg_cfg->controller_clock_hz == 0u ) ||
        ( transport_cfg->slots_per_frame == 0u ) ||
        ( transport_cfg->word_bits == 0u ) )
    {
        return false;
    }

    const uint64_t frame_divisor =
        2ULL * ((uint64_t)leg_cfg->controller_brg + 1ULL) *
        (uint64_t)transport_cfg->slots_per_frame *
        (uint64_t)transport_cfg->word_bits;
    *rate_hz = (uint32_t)((uint64_t)leg_cfg->controller_clock_hz /
                          frame_divisor);
    *deadline_us10 =
        (uint32_t)(((uint64_t)transport_cfg->block_frames * frame_divisor *
                    10000000ULL) /
                   (uint64_t)leg_cfg->controller_clock_hz);
    return true;
}

static bool audio_transport_snapshot_read( audio_transport_snapshot_t* out,
                                           bool clear_callback_peaks )
{
    if( out == NULL )
    {
        return false;
    }

    memset( out, 0, sizeof(*out) );
    out->stream_epoch = s_stream_epoch;
    out->last_transition = s_last_transition;
    out->last_transition_error = s_last_transition_error;
    out->qualified_running = s_stream_qualified;
    out->safe_mute_latched = audio_transport_frmerr_safe_mute();
    out->transition_failed = s_transition_failed;
    out->mute_held_by_failed_transition = s_mute_held_by_failed_transition;
    /* Fail closed rather than under-report: this expression, and the explicit
     * A/B blocks below it, still assume a single leg pair.  A build that
     * allocates more legs must extend them instead of silently publishing a
     * snapshot that omits legs C and D. */
    _Static_assert( TRANSPORT_LEG_MAX == 2u,
                    "snapshot publication still assumes an A/B leg pair" );
    out->leg_count = RESOLVED_TRANSPORT_LEG_B_PRESENT ? 2u : 1u;

    uint32_t rate_a_hz =
        s_current_configured_rate_hz[AUDIO_TRANSPORT_LEG_A];
    uint32_t deadline_a_us10 = ( rate_a_hz != 0u )
        ? (uint32_t)(((uint64_t)RESOLVED_TRANSPORT_BLOCK_FRAMES * 10000000ULL) /
                     rate_a_hz)
        : 0u;
    (void)audio_transport_controller_timing(
        &g_resolved_transport_static_cfg,
        &g_resolved_transport_static_cfg.legs[TRANSPORT_LEG_A],
        &rate_a_hz,
        &deadline_a_us10 );
    nora_spi_i2s_tdm_status_t status_a;
    if( !nora_spi_i2s_tdm_get_status( &status_a, clear_callback_peaks ) )
    {
        return false;
    }
    audio_transport_snapshot_copy_leg( &out->legs[AUDIO_TRANSPORT_LEG_A],
                                       &status_a,
                                       (uint8_t)RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE,
                                       rate_a_hz,
                                       deadline_a_us10 );

#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    nora_spi_i2s_tdm_inst_t* leg_b = audio_transport_tdm_leg_b();
    nora_spi_i2s_tdm_status_t status_b;
    if( ( leg_b == NULL ) ||
        !nora_spi_i2s_tdm_inst_get_status(
            leg_b, &status_b, clear_callback_peaks ) )
    {
        return false;
    }

    uint32_t rate_b_hz =
        s_current_configured_rate_hz[AUDIO_TRANSPORT_LEG_B];
    uint32_t deadline_b_us10 = ( rate_b_hz != 0u )
        ? (uint32_t)(((uint64_t)RESOLVED_TRANSPORT_BLOCK_FRAMES * 10000000ULL) /
                     rate_b_hz)
        : deadline_a_us10;
    (void)audio_transport_controller_timing(
        &g_resolved_transport_static_cfg,
        &g_resolved_transport_static_cfg.legs[TRANSPORT_LEG_B],
        &rate_b_hz,
        &deadline_b_us10 );
    audio_transport_snapshot_copy_leg( &out->legs[AUDIO_TRANSPORT_LEG_B],
                                       &status_b,
                                       (uint8_t)RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE,
                                       rate_b_hz,
                                       deadline_b_us10 );
#endif

    return true;
}

bool audio_transport_snapshot_get( audio_transport_snapshot_t* out )
{
    return audio_transport_snapshot_read( out, false );
}

bool audio_transport_snapshot_take_window( audio_transport_snapshot_t* out )
{
    return audio_transport_snapshot_read( out, true );
}


static void audio_transport_start_hal_transport( void )
{
    const audio_transport_client_t* client = audio_transport_client_get();
    if( client == NULL )
    {
        printf(" audio_transport_start: no transport client bound\n");
        return;
    }

    // The selected application owns its stream state; clear it before callback
    // registration and before any transport domain starts.
    client->reset_stream_state( client->user );

    // Block callbacks:
    //  - Co-clock: one transport-owned wrapper resolves the safe B output half,
    //    then calls the client's single producer for A and optional B.
    //  - Independent domains: each ISR invokes its application-owned leg callback
    //    directly with the client's opaque context.
#if RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE
    if( !nora_spi_i2s_tdm_set_block_callback( audio_transport_tdm_leg_a(), app_block_cb_leg_a, NULL ) )
#elif RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE
    if( !nora_spi_i2s_tdm_set_block_callback(
            audio_transport_tdm_leg_a(), client->leg_a_process, client->user ) )
#else
    #error "Unsupported resolved transport topology"
#endif
    {
        printf(" audio_transport_start: failed to register leg-A block callback\n");
        return;
    }
#if RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE
    // Independent domain: logical leg B has its own callback driven by its own
    // RX-block ISR; the board maps it to physical SPI2 or SPI4.
    if( !nora_spi_i2s_tdm_set_block_callback(
            audio_transport_tdm_leg_b(), client->leg_b_process, client->user ) )
    {
        printf(" audio_transport_start: failed to register leg-B block callback\n");
        return;
    }
#endif // independent dual-domain topology
    // The HAL core no longer derives its config from app macros -- configure each
    // instance ONCE from the platform default before the first start. Restarts re-apply
    // the stored per-instance config, so do NOT reconfigure here.
    static bool                         s_tdm_configured = false;
    if( !s_tdm_configured )
    {
        // The complete TDM configuration is the board system table
        // (board/audio/tdm.c): one fully-specified leg_setup per leg (stream +
        // sync domain), selected at build time. Pass it DIRECTLY to configure_system() in one
        // transactional (all-or-nothing) call -- no local copy, no per-leg resolve, no app-side
        // role rewrite. Fail closed: if the table is missing or configure fails, leave
        // s_tdm_configured false so open()/start below do not run.
        const nora_spi_i2s_tdm_leg_setup_t* setups = audio_transport_board_tdm_system();
        const uint8_t                            setup_count = audio_transport_board_tdm_leg_count();
        if( ( setups != NULL ) &&
            ( setup_count == (uint8_t)AUDIO_TDM_LEG_COUNT ) &&
            nora_spi_i2s_tdm_configure_system( setups, setup_count ) )
        {
            s_tdm_configured = true;
        }
    }

    // Open the shared port once, then start every sync domain PHASE-LOCKED: start_all_domains()
    // arms each domain's legs together and releases their SPIEN back-to-back (slaves first,
    // clock-master last) so co-clocked members latch one FS edge (wdiff=0). open() takes no role
    // -- it derives the clock role from the committed primary leg, so the app cannot pass a role
    // that contradicts the configured stream. start_all_domains rolls back every domain it
    // started on failure, so the app only closes the port here. (Ordering + phase-lock live in
    // the HAL, keyed on each leg's sync_domain + committed config.clock_role.)
    bool ok = nora_spi_i2s_tdm_open();
#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED && NORA_TDM_USE_SPI2 && \
    (RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE)
    // Deterministic co-clocked startup (still MUTED here; unmute happens later, gated on is_running).
    // Start, then verify the domain phase-locked over the configured number of consecutive blocks
    // (ALL conditions: wdiff<=tol, no half-mismatch, no tail_tear, safe B target). If not locked,
    // re-arm and retry. Only a locked start leaves the transport running -> the caller unmutes.
    if( ok )
    {
        ok = false;
        for( int attempt = 1;
             attempt <= RESOLVED_TRANSPORT_PHASE_LOCK_MAX_RETRIES;
             attempt++ )
        {
            s_stlock_consec = 0u; s_stlock_locked = 0u; s_stlock_verifying = 1u;
            if( !nora_spi_i2s_tdm_start_all_domains() ) { s_stlock_verifying = 0u; break; }
            delay_ms( RESOLVED_TRANSPORT_PHASE_LOCK_WAIT_MS );
            s_stlock_verifying = 0u;
            printf(" PHLOCK try=%d wdiff=%ld halfmm=%d tailtear=%d unsafe=%d consec=%lu\n",
                   attempt, (long)s_stlock_wdiff, (int)s_stlock_halfmm, (int)s_stlock_tailtear,
                   (int)s_stlock_unsafe, (unsigned long)s_stlock_consec );
            if( s_stlock_locked ) { printf(" PHLOCK locked try=%d\n", attempt); ok = true; break; }
            nora_spi_i2s_tdm_stop_all_domains();  // misaligned -> tear down (still muted) + retry
            printf(" PHLOCK retry\n");
        }
        if( !ok )
        {
            printf(" PHLOCK failed retries=%d mute-held\n",
                   RESOLVED_TRANSPORT_PHASE_LOCK_MAX_RETRIES );
        }
    }
#else
    if( ok )
    {
        ok = nora_spi_i2s_tdm_start_all_domains();
    }
#endif // resolved startup phase-lock policy
    if( !ok )
    {
        nora_spi_i2s_tdm_close();
        return;
    }

#if RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
    // Started and (where enabled) phase-locked: only now mask leg B's RX-block ISR. Not earlier --
    // the startup phase-lock verify must run against an untouched start, and it reads leg A's
    // callback anyway. Every start comes through here, so a restart re-applies the mask (a stop
    // masks both legs' IRQs and a start re-enables them, so this is not a one-shot).
    //
    // The compile-time gate cannot see leg_b_process, which is a runtime field: a client that DOES
    // consume codec B's ADC must keep its ISR even in a DRC build, so check and refuse rather than
    // silently drop its callback.
    {
        const audio_transport_client_t* gate_client = audio_transport_client_get();
        nora_spi_i2s_tdm_inst_t*        gate_leg_b  = audio_transport_tdm_leg_b();
        if( ( gate_client != NULL ) && ( gate_client->leg_b_process != NULL ) )
        {
            printf(" audio_transport_start: leg-B IRQ gate DECLINED --"
                   " the client registered a leg-B callback\n");
        }
        else if( ( gate_leg_b != NULL ) &&
                 nora_spi_i2s_tdm_inst_set_block_irq_enabled( gate_leg_b, false ) )
        {
            printf(" audio_transport_start: leg-B (TDM%u) block IRQ masked --"
                   " RX unread, TX mirrored by TDM%u\n",
                   (unsigned)RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE,
                   (unsigned)RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE);
        }
        else
        {
            printf(" audio_transport_start: leg-B IRQ gate FAILED -- ISR left armed\n");
        }
    }
#endif // RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
}

static void audio_transport_start_cmsis_sai_transport( void )
{
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    // Opt-in verification: drive the stream through the CMSIS-SAI wrapper harness
    // (isolated in audio_transport_sai_live.c) instead of the demo DSP path.
    audio_transport_sai_live_start();
#else
    printf(" audio_transport_start: CMSIS-SAI live route requested but the live test is disabled\n");
#endif // RESOLVED_SAI_TEST_LIVE_ENABLED
}

static void audio_transport_start_transport( audio_transport_route_t route )
{
    if( route == AUDIO_TRANSPORT_ROUTE_CMSIS_SAI )
    {
        audio_transport_start_cmsis_sai_transport();
    }
    else
    {
        audio_transport_start_hal_transport();
    }
}

static bool audio_transport_start_route_impl(
    audio_transport_route_t                    route,
    bool                                 unmute_on_success,
    audio_transport_transition_reason_t  reason )
{
    if( TDM_activated )
    {
        return true;    // already started.
    }
    if( audio_transport_client_get() == NULL )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_START_FAILED );
        printf(" audio_transport_start: no transport client bound -- aborting route start\n");
        return false;
    }

    // (Phase D) Tune the A-side DSP to the A-domain codec rate. In the codec-master build A is
    // runtime rate-settable, so read its current rate (default 48k); other builds keep the
    // compile-time SAMPLE_RATE (e.g. the 96k test path). The fs-aware guard inside re-tunes only
    // when the rate actually changed.
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    audio_transport_prepare_client( wm8904_get_rate_hz( I2C_INST_A ) );
#else
    audio_transport_prepare_client(
        RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ );
#endif
    if( !audio_transport_bind_transport_once() )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_START_FAILED );
        printf(" audio_transport_start: TDM port bind failed -- aborting route start\n");
        return false;   // fail closed BEFORE codec shutdown/init + transport start
    }

    // There is deliberately NO codec pre-shutdown here any more. The discharge that keeps a re-init
    // from popping lives inside wm8904_init_role(), immediately before the codec is configured.
    //
    // One rule, no inputs: each codec is discharged immediately before it is initialised. Every
    // wm8904_init*() call site in this function already sits where that leg's clock is running --
    // that is precisely why a controller-clocked leg is initialised after the transport start -- so
    // "wait until the clock this codec will use is present" is satisfied by construction instead of
    // by a predicate. What that deleted three of, all of which had produced a real defect:
    //
    //   * the PROCESSOR reset cause (RCON) standing in for "might this codec have a live HPOUT".
    //     A codec's charge state is not a property of how the dsPIC was reset, and the substitution
    //     is what made an MCLR reset -- POR and EXTR both set, so read as cold -- silently skip the
    //     very path it was being used to test.
    //   * a per-topology decision about WHERE each codec may be quiesced. The vendor write sequencer
    //     needs the codec's own SYSCLK, and in a co-clocked build leg B's MCLK is leg A's clock,
    //     which nothing drives this early: quiescing B here could not work and left it wedged, so
    //     leg B failed its first apply on every warm boot (T2.8, 2026-08-11).
    //   * the "is this the first bring-up" test. Discharging a codec that already sits at its reset
    //     defaults costs ~294 ms of write sequencer and changes nothing else, so it does not earn a
    //     condition -- every condition here was another chance to be wrong about board state.

    /*
     * Seed each codec's tracked rate from this build's nominal rate, exactly ONCE per
     * boot, before the very first codec init. The driver's per-instance default is
     * 48000, so a 96 kHz build must state its rate explicitly or the first configure
     * would program 48 kHz values.
     *
     * A ONE-SHOT FLAG is used deliberately, not a comparison against current state.
     * This function also runs on every restart -- including the restart that a rate
     * CHANGE performs -- and at that moment the requested rate is already in the codec
     * driver while s_current_configured_rate_hz[] still holds the OLD value (it is only
     * updated after the restart succeeds). Any state-comparing guard therefore reads as
     * "not changed yet" and pushes the nominal rate back over the request, silently.
     *
     * That is exactly the bug this comment used to describe as fixed: leg B lost its
     * rate to a frame-error auto-recovery restart, and then leg A lost its rate to the
     * rate-change restart itself. Same shape both times -- a path asked to do one thing
     * also quietly re-asserting another, and reporting success. Seed once, then never
     * touch the rate here again.
     */
    {
        static bool nominal_rate_seeded = false;
        if( !nominal_rate_seeded )
        {
            nominal_rate_seeded = true;
            (void)wm8904_set_rate_hz( I2C_INST_A,
                                      RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ );
#if NORA_TDM_USE_SPI2
            (void)wm8904_set_rate_hz( I2C_INST_B,
                                      RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ );
#endif // NORA_TDM_USE_SPI2
        }
    }

    // How this image configures the codec. Printed unconditionally and before any
    // codec is touched: it describes the image, not a leg. (This line once lived
    // inside the MikroB block, which made it invisible on exactly the boards where
    // B is an I2C alias of A -- bridged MikroBUS -- so the one build fact the A/B
    // procedure said to confirm from the banner could not be confirmed there.)
    printf("  WM8904 config path: UNIFIED (rate table + role)\n");

    // init CODEC board (master case: codec drives BCLK/FS before the transport starts)
#if RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE != TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
    printf("---------------------------------------------\n");
    printf("WM8904 on MikroA is configured as TDM Master.\n");
    printf("---------------------------------------------\n");
#if RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ == 96000u
    if( !wm8904_init_role( I2C_INST_A, true, AUDIO_TRANSPORT_LEG_A_ROLE ) )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-A apply failed -- staying muted\n");
        return false;
    }
#else
    if( !wm8904_init( I2C_INST_A, true ) )   // config as TDM master
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-A apply failed -- staying muted\n");
        return false;
    }
#endif // 96 kHz nominal rate
    delay_ms(100);
#endif // leg A is not controller-clocked

#if RESOLVED_BOARD_COHERENT_OFFSET_CLOCK
    // Q27B coherent fixed-offset test: WM8904-A (codec master) is now up, so its fixed 12.288 MHz
    // XTAL_OUT (fs-independent, not BCLK) is present on RP16. Re-source CLKGEN9 (SPI2's transport
    // clock) from PLL2 LOCKED to A's XTAL_OUT (via REFI1) BEFORE the SPI2 transport starts below, so
    // SPI2 generates a BCLK coherent (low wander) with A. The fixed A:B offset comes from the
    // unchanged SPI2 BRG. CPU stays on PLL1 (DSP SYS unchanged). PWM is off (PLL2 reused). Only
    // dsPIC PLL2 + BRG are used (no FLL, no jitter).
    {
        nora_clock_status_t q27b_detail = NORA_CLOCK_OK;
        sonora_clock_pwm_status_t q27b = sonora_clock_q27b_coherent_clkgen9( &q27b_detail );
        // status=0 => PLL2 locked to A and CLKGEN9 re-sourced (CPU stays on PLL1 -- see main.c's
        // SysClock banner for the separate, boot-time CPU-coherence path).
        printf(" *Q27B: CLKGEN9<-PLL2(REFI1<-A XTAL_OUT) status=%d detail=%d (coherent w/ A)\n",
               (int)q27b, (int)q27b_detail );
        if( q27b != SONORA_CLOCK_PWM_OK )
        {
            // Same rule as the PLL2 transport-clock path below: the whole point of
            // this build is a coherent clock, so falling through to PLL1 would run
            // the transport on a clock nobody asked for.
            audio_transport_mark_transition_failed(
                AUDIO_TRANSPORT_TRANSITION_ERROR_CLOCK_SETUP_FAILED );
            printf(" audio_transport_start: Q27B coherent-clock setup FAILED"
                   " -- transport NOT started, staying muted\n");
            return false;
        }
        delay_ms(20);   // let PLL2/CLKGEN9 settle before the SPI2 master starts
    }
#endif // RESOLVED_BOARD_COHERENT_OFFSET_CLOCK

#if RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2
    // WM8904-A (codec master) is up, so its fixed 12.288 MHz XTALout is present on REFI1<-RP16. Re-source
    // CLKGEN9 (SPI2's transport clock) from PLL2<-REFI1 BEFORE the SPI2 transport starts, so it is
    // coherent with the codec. SysCLK stays on PLL1<-FRC. This runs here, application-side, precisely
    // because REFI1 has no reference any earlier.
    {
        nora_clock_status_t clk_detail = NORA_CLOCK_OK;
        sonora_clock_pwm_status_t st = sonora_clock_spi2_from_pll2( &clk_detail );
        printf(" *CLKGEN9<-PLL2<-REFI1: status=%d detail=%d\n",
               (int)st, (int)clk_detail );
        if( st != SONORA_CLOCK_PWM_OK )
        {
            // Report and stop. Starting the transport anyway would run SPI2 on
            // PLL1 instead of the codec-coherent clock that was asked for, which
            // is exactly the "do not force progress on an unintended clock" rule
            // in [internal] clock_design_contract.md. Leave the stream
            // muted and let the caller/telemetry surface it.
            audio_transport_mark_transition_failed(
                AUDIO_TRANSPORT_TRANSITION_ERROR_CLOCK_SETUP_FAILED );
            printf(" audio_transport_start: PLL2 transport-clock setup FAILED"
                   " -- transport NOT started, staying muted\n");
            return false;
        }
        delay_ms(20);   // let PLL2/CLKGEN9 settle before the SPI2 master starts
    }
#endif // RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2

#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
    // WM8904-A (codec master) is up, so its XTALout -- a fixed, fs-INDEPENDENT 12.288 MHz -- is
    // present on REFI1<-RP16. Re-source the CCP capture time base (CLKGEN13) from PLL2<-REFI1, so
    // the clock the ASRC measures sample rates WITH comes from the same crystal as the rates it
    // measures. Nothing else changes clock; SysCLK stays on PLL1<-FRC.
    //
    // Ordering, stated exactly, because it differs between the first start and a restart:
    //   first start  the CCP is not yet armed. Arming happens after this, inside
    //                audio_transport_start_transport() -> reset_stream_state() ->
    //                asrc_clock_control_init_reset().
    //   restart /    asrc_clock_control_init_reset() arms ONCE (a static flag), so the CCP is
    //   rate change  already running here and this call moves CLKGEN13 UNDER A LIVE CAPTURE.
    //                That is deliberate and it is safe for a specific reason, not by luck: the
    //                transport is muted across this window, and reset_stream_state() runs
    //                afterwards and calls ccpdet_reset(), which discards the captures and the
    //                queue that straddled the switch. So the only captures the servo ever sees
    //                are post-switch ones.
    // The stricter alternative -- stop the CCP, move CLKGEN13, re-arm -- was NOT taken: it would
    // add a stop/re-arm path to a hardware-verified sequence to remove a transient that is
    // already discarded. If ccpdet_reset() ever stops being called on the restart path, this
    // reasoning collapses and that alternative becomes necessary.
    {
        nora_clock_status_t clk_detail = NORA_CLOCK_OK;
        sonora_clock_pwm_status_t st = sonora_clock_ccp_timebase_from_pll2( &clk_detail );
        printf(" *CLKGEN13<-PLL2<-REFI1 (CCP time base): status=%d detail=%d\n",
               (int)st, (int)clk_detail );
        if( st != SONORA_CLOCK_PWM_OK )
        {
            // Report and stop, same rule as the SPI2 path above: arming the CCP anyway would
            // measure with the FRC time base while the build asked for the codec-coherent one,
            // and every reported rate would silently carry the FRC's error.
            audio_transport_mark_transition_failed(
                AUDIO_TRANSPORT_TRANSITION_ERROR_CLOCK_SETUP_FAILED );
            printf(" audio_transport_start: PLL2 CCP time-base setup FAILED"
                   " -- transport NOT started, staying muted\n");
            return false;
        }
        delay_ms(20);   // let PLL2/CLKGEN13 settle before the CCP time base is used
    }
#endif // RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2

    // init and start the TDM transport
    audio_transport_start_transport( route );

    // Confirm the stream actually started BEFORE touching the codec. The WM8904 slave
    // DC-servo startup needs BCLK/LRCLK present, so configuring it when the stream did
    // not start is pointless. is_running() is the path-agnostic truth (demo start() or
    // the wrapper Control(CONTROL_TX) path). On failure stay muted and bail out.
    if( !nora_spi_i2s_tdm_is_running() )
    {
        TDM_activated = 0;
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_START_FAILED );
        printf(" audio_transport_start: stream did NOT start -- staying muted (TDM_activated=0)\n");
        return false;
    }
    TDM_activated = 1;

    delay_ms(50);

// note:
// In clock slave mode, BCLK/LRCLK must be present before DC servo startup.
// Otherwise the DC servo startup sequence may not complete, causing
// abnormal headphone output level/headroom.
#if RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
    printf("---------------------------------------------\n");
    printf("WM8904 on MikroA is configured as TDM Slave.\n");
    printf("---------------------------------------------\n");
    if( !wm8904_init( I2C_INST_A, false ) )  // config as TDM slave
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-A apply failed -- mute-held teardown\n");
        (void)audio_transport_stop_route( route );
        return false;
    }
#endif // leg A is controller-clocked

#if NORA_TDM_USE_SPI2
    // Two-codec builds require the Curiosity A/B I2C bridge resistors (R38/R39)
    // depopulated. Bridged-I2C operation is unsupported. Firmware does not infer
    // physical board topology from codec register state -- see
    // [internal] codec_ab_startup_clock_gate.md for the removed alias-probe
    // design and why.
    // TDM Slave must be after nora_spi_i2s_tdm_start();
    printf("---------------------------------------------\n");
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    printf("WM8904 on MikroB is configured as TDM MASTER (own 12.288MHz XTAL).\n");
#else
    printf("WM8904 on MikroB is configured as TDM Slave.\n");
#endif
    // State leg B's rate and converter role explicitly: at 96 kHz the WM8904 is
    // DAC-only (no simultaneous ADC+DAC above its 88.2 kHz boundary), which is the
    // fact that makes the 96 kHz ASRC one-way. Cheap, and it makes a mis-flashed
    // image obvious.
    // (The config-path line moved above, out of this leg-B-only block.)
    printf("  MikroB leg: %lu Hz, %s, %u slot(s)/frame\n",
           (unsigned long)RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ,
#if RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ == 96000u
           "DAC-only (96k: ADC+DAC not simultaneously supported)",
#else
           "ADC+DAC",
#endif
           (unsigned)RESOLVED_TRANSPORT_SLOTS_PER_FRAME );
    printf("---------------------------------------------\n");

    // Leg B's discharge happens inside its wm8904_init*() call below.

// Leg-B clock ownership is decided FIRST, then leg B's own rate selects the register
// set. The previous order tested leg A's rate before clock ownership, which had two
// consequences: a 96 kHz build could never reach the codec-master arm at all (so a
// B-XTAL codec-master image silently configured B as a SLAVE), and leg B's
// configuration depended on leg A's rate -- precisely the same-domain assumption the
// ASRC exists to remove. Leg A keeps its own rate test at its own call site above.
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
  #if RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ == 96000u
    // B-codec-master at 96 kHz: DAC-only (the WM8904 cannot run ADC and DAC
    // simultaneously above its 88.2 kHz boundary), clocked from its OWN 12.288 MHz XTAL via the
    // B-XTAL -> B-MCLK jumper, driving BCLK/FS as master (master_cfg=true sets
    // BCLK_DIR/LRCLK_DIR inside the shared 96 kHz interface configuration).
    if( !wm8904_init_role( I2C_INST_B, true, AUDIO_TRANSPORT_LEG_B_ROLE ) )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-B apply failed -- mute-held teardown\n");
        (void)audio_transport_stop_route( route );
        audio_transport_report_codec_b_missing();
        return false;
    }
  #else
    // B-codec-master: WM8904-B drives BCLK/FS from its SYSCLK = its OWN 12.288 MHz XTAL
    // (board jumper B-XTAL -> B-MCLK; FLL off, SYSCLK_SRC=MCLK, 256fs @48k -- the existing
    // wm8904_config 48k path with master_cfg=true). The dsPIC SPI2 leg is a SLAVE on its own
    // independent domain: exactly like codec-A / SPI1, the codec-master init runs AFTER the
    // transport start and the slave SPI simply idles until B's externally-driven clock appears.
    if( !wm8904_init( I2C_INST_B, true ) )   // config as TDM MASTER
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-B apply failed -- mute-held teardown\n");
        (void)audio_transport_stop_route( route );
        audio_transport_report_codec_b_missing();
        return false;
    }
  #endif // leg B nominal rate
#elif RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ == 96000u
    // Classic 96 kHz co-clocked split (A=ADC master, B=DAC slave): unchanged
    // behaviour. Leg B is not endpoint-clocked here, and in this topology legs A
    // and B share one rate, so this is the same call the previous leg-A-keyed
    // test produced.
    if( !wm8904_init_role( I2C_INST_B, false, AUDIO_TRANSPORT_LEG_B_ROLE ) )
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-B apply failed -- mute-held teardown\n");
        (void)audio_transport_stop_route( route );
        audio_transport_report_codec_b_missing();
        return false;
    }
#else
    if( !wm8904_init( I2C_INST_B, false ) )  // config as TDM slave
    {
        audio_transport_mark_transition_failed(
            AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED );
        printf(" audio_transport_start: WM8904-B apply failed -- mute-held teardown\n");
        (void)audio_transport_stop_route( route );
        audio_transport_report_codec_b_missing();
        return false;
    }
#endif // resolved rate and leg-B clock ownership
#else
    printf("---------------------------------------------\n");
    printf("WM8904 on MikroB is disabled.\n");
    printf("---------------------------------------------\n");
#endif // NORA_TDM_USE_SPI2

    delay_ms(100);

#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
    // Clear any phase-fault latched during the (muted) startup/retry window before unmuting.
    // A retry teardown gap -- the verify window is already closed (s_stlock_verifying=0) but
    // the transport is not yet stopped -- can let an EXPECTED NULL-mirror skip set
    // s_phase_fault_pending. That is muted startup noise, not a runtime fault. We only reach
    // here on a confirmed running+locked start (is_running() gate above), so reset the latch:
    // otherwise a stale fault would make the FIRST manage_route mute + resync a perfectly good
    // stream (audible drop). Only faults detected AFTER unmute should trigger a resync.
    s_phase_fault_pending = 0u;
    // Re-arm the UNRESOLVED tolerance for this fresh lock. This point is reached on EVERY
    // running+locked start_route -- cold boot AND a post-resync restart -- so clearing the counter
    // here guarantees a fresh unresolved-position tolerance budget
    // starts from zero after a resync; otherwise a stale >=TOL count would re-fault on the first
    // post-restart transient block (the verify window never touches the counter).
    s_mirror_unresolved_run = 0u;
#endif // RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
    // A recovery start deliberately stays muted until a separate clean-frame/liveness check
    // succeeds. Normal starts preserve the original behavior and unmute here.
    if( unmute_on_success )
    {
        wm8904_set_analog_output_mute( I2C_INST_A, false );  // unmute A
#if NORA_TDM_USE_SPI2
        wm8904_set_analog_output_mute( I2C_INST_B, false );  // unmute B
#endif // NORA_TDM_USE_SPI2
    }

    if( unmute_on_success )
    {
        audio_transport_mark_qualified( reason );
        /* A completed explicit start/restart is the only operation that
         * releases the terminal pre-flash stop latch. */
        s_preflash_stop_latched = false;
        s_preflash_stop_confirmed = false;
#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
        s_codec_b_missing_stop_latched = false;   // this start reached here, so B answered
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
    }
    else
    {
        s_stream_qualified = false;
    }
    printf(" audio_transport_start: TDM_activated=%d mute_held=%d\n",
           TDM_activated, unmute_on_success ? 0 : 1 );
    return true;
}

static void audio_transport_start_route(
    audio_transport_route_t                    route,
    audio_transport_transition_reason_t  reason )
{
    (void)audio_transport_start_route_impl( route, true, reason );
}

static bool audio_transport_manage_route( audio_transport_route_t route )
{
    if( s_preflash_stop_latched )
    {
        return false;
    }
#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
    // A missing/unanswering WM8904-B already reported once (see
    // audio_transport_report_codec_b_missing()) -- stay stopped instead of re-attempting
    // the whole start sequence every tick. An explicit *tr (audio_transport_restart_route)
    // still bypasses this and is free to try again.
    if( s_codec_b_missing_stop_latched )
    {
        return false;
    }
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B

#if RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
    // Runtime phase fault: the block ISR found the B-write target half was SPI2's transmitting half
    // (mirror returned NULL -> B skipped that block) outside the startup verify window. Mute and
    // re-sync the WHOLE transport (start_route re-runs the startup phase-lock before unmuting).
    if( s_phase_fault_pending )
    {
        s_phase_fault_pending = 0u;
        printf(" [phase-fault] unsafe B-write target detected -> mute + whole-transport resync\n");
        audio_transport_mute( true );
        (void)audio_transport_stop_route( route );   // route-aware teardown (fail leaves TDM_activated set)
        audio_transport_start_route( route, AUDIO_TRANSPORT_TRANSITION_PHASE_RECOVERY );
        // re-verify phase-lock, unmute only when locked (no-op if stop failed)
        return true;
    }
#endif // RESOLVED_TRANSPORT_STARTUP_PHASE_LOCK_ENABLED
#if RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
    // Phase-mismatch guard: a persistent co-clocked leg-A/leg-B misalignment was detected in the
    // block ISR. Mute and re-sync the WHOLE transport (stop + start_route -> start_all_domains
    // re-locks phase); never poke a single leg. Never fires with the aligned start (tail_tear=0).
    if( s_sync_resync_needed )
    {
        s_sync_resync_needed = 0u;
        s_sync_bad_run       = 0u;
        printf(" [sync-guard] persistent leg-A/leg-B phase mismatch -> mute + whole-transport resync\n");
        audio_transport_mute( true );
        (void)audio_transport_stop_route( route );   // route-aware teardown (fail leaves TDM_activated set)
        audio_transport_start_route( route, AUDIO_TRANSPORT_TRANSITION_PHASE_RECOVERY );
        // re-open + start_all_domains; unmutes at its end (no-op if stop failed)
        return true;
    }
#endif // RESOLVED_TRANSPORT_SYNC_GUARD_ENABLED
    // External-clock stop/resume edge (slave mode; only fires when the board has clock
    // detection configured -- otherwise always NONE). STOPPED: mute+stop.
    // RESUMED: restart. NONE falls through to the steady-state level gate. Both paths
    // use the TDM_activated-guarded helpers, so they coexist safely.
    nora_spi_i2s_tdm_clock_event_t ev = nora_spi_i2s_tdm_consume_clock_event();
    if( ev == NORA_SPI_I2S_TDM_CLOCK_EVENT_STOPPED )
    {
        audio_transport_mute( true );
        (void)audio_transport_stop_route( route );
        return false;
    }
    else if( ev == NORA_SPI_I2S_TDM_CLOCK_EVENT_RESUMED )
    {
        audio_transport_start_route( route, AUDIO_TRANSPORT_TRANSITION_CLOCK_RESUME );
        return true;
    }

    if( !nora_spi_i2s_tdm_is_active() )
    {
        (void)audio_transport_stop_route( route );
        return false;
    }
    else
    {
        audio_transport_start_route( route, AUDIO_TRANSPORT_TRANSITION_CLOCK_RESUME );
        return true;
    }
}

// Forced mute-bounded SAME-RATE stop/restart of the audio stream. audio_transport_start_route()
// re-inits the codec, starts the transport, and unmutes at its end -- the only gap it
// leaves is muting before the stop, closed here. Exposed for the *nt03 debug command
// and reused as the clock-RESUMED action above.
static bool audio_transport_restart_route(
    audio_transport_route_t                    route,
    audio_transport_transition_reason_t  reason )
{
    printf(" [restart] mute + stop (same-rate restart)\n");
    audio_transport_mute( true );
    if( !audio_transport_stop_route( route ) )   // route-aware teardown; abort restart if it failed
    {
        printf(" [restart] aborted: teardown failed (transport left active)\n");
        return false;
    }
    const bool started = audio_transport_start_route_impl( route, true, reason );
    printf(" [restart] %s.\n", started ? "done" : "failed" );
    return started;
}

// Recovery-only restart. Unlike the normal route, this never unmutes inside start(); the caller
// must first prove a run of clean frames, then explicitly release mute. This closes the previous
// short blast window between audio_transport_restart() returning and a second mute write.
static bool audio_transport_restart_route_muted( audio_transport_route_t route )
{
    printf(" [restart] mute-held stop + restart\n");
    audio_transport_mute( true );
    if( !audio_transport_stop_route( route ) )
    {
        printf(" [restart] mute-held aborted: teardown failed\n");
        return false;
    }
    const bool ok = audio_transport_start_route_impl(
        route, false, AUDIO_TRANSPORT_TRANSITION_NONE );
    printf(" [restart] mute-held %s\n", ok ? "started" : "failed" );
    return ok;
}

void audio_transport_hal_start( void )
{
    audio_transport_start_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT,
                           AUDIO_TRANSPORT_TRANSITION_INITIAL_START );
}

bool audio_transport_hal_manage( void )
{
    return audio_transport_manage_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT );
}

void audio_transport_hal_stop( void )
{
    (void)audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT );
}

void audio_transport_cmsis_sai_start( void )
{
    audio_transport_start_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI,
                           AUDIO_TRANSPORT_TRANSITION_INITIAL_START );
}

bool audio_transport_cmsis_sai_manage( void )
{
    return audio_transport_manage_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI );
}

void audio_transport_cmsis_sai_stop( void )
{
    (void)audio_transport_stop_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI );
}

// App telemetry period (ms). Default from config; runtime-changeable via the setter.
static uint32_t s_dbg_period_ms = RESOLVED_TRANSPORT_DEBUG_PERIOD_MS;

void audio_transport_set_dbg_period_ms( uint32_t ms )
{
    s_dbg_period_ms = ms;
}

void audio_transport_dbg_enable( bool on )
{
    // Simple telemetry ON/OFF: ON restores the resolved default period, OFF (0) disables the line.
    s_dbg_period_ms = on ? (uint32_t)RESOLVED_TRANSPORT_DEBUG_PERIOD_MS : 0u;
}

bool audio_transport_dbg_enabled( void )
{
    // Period 0 is the single definition of "telemetry off" -- both *tq0000 and *tq0002 with a zero
    // period land here, so callers need not know which form was used.
    return ( s_dbg_period_ms != 0u );
}

static const char* audio_transport_transition_name(
    audio_transport_transition_reason_t reason )
{
    switch( reason )
    {
        case AUDIO_TRANSPORT_TRANSITION_INITIAL_START:   return "initial-start";
        case AUDIO_TRANSPORT_TRANSITION_MANUAL_RESTART:  return "manual-restart";
        case AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE:     return "rate-change";
        case AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE_ROLLBACK:
            return "rate-change-rollback";
        case AUDIO_TRANSPORT_TRANSITION_CLOCK_RESUME:    return "clock-resume";
        case AUDIO_TRANSPORT_TRANSITION_PHASE_RECOVERY:  return "phase-recovery";
        case AUDIO_TRANSPORT_TRANSITION_FRMERR_RECOVERY: return "frmerr-recovery";
        case AUDIO_TRANSPORT_TRANSITION_NONE:
        default:                                         return "none";
    }
}

static const char* audio_transport_transition_error_name(
    audio_transport_transition_error_t error )
{
    switch( error )
    {
        case AUDIO_TRANSPORT_TRANSITION_ERROR_STOP_FAILED:        return "stop-failed";
        case AUDIO_TRANSPORT_TRANSITION_ERROR_START_FAILED:       return "start-failed";
        case AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED: return "codec-apply-failed";
        case AUDIO_TRANSPORT_TRANSITION_ERROR_CLOCK_SETUP_FAILED: return "clock-setup-failed";
        case AUDIO_TRANSPORT_TRANSITION_ERROR_NONE:
        default:                                                   return "none";
    }
}

static void audio_transport_dbg_print_leg_begin(
    const audio_transport_leg_snapshot_t* leg,
    uint8_t                               logical_leg )
{
    const uint32_t max_us10 = leg->callback_peak_us10;
    const uint32_t deadline_us10 = leg->callback_deadline_us10;
    const uint32_t load_x10 = ( deadline_us10 > 0u )
        ? (uint32_t)(((uint64_t)max_us10 * 1000ULL) / deadline_us10) : 0u;
    const int32_t margin_us10 = (int32_t)deadline_us10 - (int32_t)max_us10;

    /* resp=, not max=: this is the RX-block ISR's RESPONSE time (entry to exit), which
     * INCLUDES any higher-priority ISR that preempted it. Under an asymmetric priority map the
     * low-priority leg's figure therefore carries the other leg's execution time, so it is a
     * per-leg diagnostic, NOT a load. Engine load is the DSPload line below. */
    /* The percentage that used to follow resp= is GONE (2026-08-27): resp/deadline reads like a
     * load, and under an asymmetric priority map it is not one -- the demoted leg's figure carries
     * the other leg's execution time, so the number invited exactly the misreading the comment
     * above warns about (measured: 30.5% -> 62.6% on the demoted leg while the real DSP load moved
     * 0.2 pt). Load is the DSPload line; this line keeps resp and margin in microseconds.
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md P1. */
    (void)load_x10;
    printf("TDM%u:resp=%lu.%luus margin=",
           (unsigned)leg->physical_spi_instance,
           (unsigned long)(max_us10 / 10u),
           (unsigned long)(max_us10 % 10u));
    if( margin_us10 < 0 )
    {
        const uint32_t magnitude = (uint32_t)(-margin_us10);
        printf("-%lu.%luus", (unsigned long)(magnitude / 10u),
                              (unsigned long)(magnitude % 10u));
    }
    else
    {
        printf("%lu.%luus", (unsigned long)((uint32_t)margin_us10 / 10u),
                            (unsigned long)((uint32_t)margin_us10 % 10u));
    }

    /*
     * The MEASURED rate this leg's resp/margin belong to, right after the margin they scale
     * with: the deadline is BLOCK/fs, so a reader who wants to know which rate a 105.9 us margin
     * was earned at had to divide it out by hand (and a mixed-rate pair makes that two divisions
     * per report). Measured, not configured -- 47792 where the codec was asked for 48000 -- and
     * omitted entirely when the application measures nothing, rather than printed as 0.
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md section 7.
     */
    {
        const uint32_t fs_hz = audio_application_leg_measured_fs_hz( logical_leg );
        if( fs_hz != 0u ) { printf(" fs=%luHz", (unsigned long)fs_hz); }
    }
}

//===========================================================
// Engine-wide DSP load line ("DSPload").
//
// WHAT IT REPORTS
//   self    = leg A exclusive CPU time + leg B exclusive CPU time, over a FIXED measurement
//             window. This is the DSP load: the share of the CPU the audio legs actually
//             consumed. Exclusive means that when a higher-priority vector preempts a leg, the
//             preemptor's execution is charged to itself, not to the leg it interrupted.
//   stolen  = time a leg was held off by an instrumented higher-priority ISR. What the old
//             wall-time reading silently folded into the leg's own total is now its own number.
//   demand  = self + stolen: the CPU time the legs wanted, including what they waited for.
//
// A_self / B_self / self / stolen / demand are per-window MEANS over the windows closed since the
// previous print -- window-length independent, and the honest "what did it cost" figure.
// max_self / max_demand are the single worst window in that same interval; THAT is what the
// window length buys, because shortening the window pushes the peak toward the per-block truth.
//
// WHAT IT IS NOT
// A deadline instrument. The window is a fixed time, deliberately unrelated to the block period
// (10 ms is ~30 blocks at 333 us), so one late block is averaged away. Deadline margin lives in
// the per-leg TDM lines above (response time, preemption included) and in the real-harm counters
// miss / starve / ovf / udf / drop. Do not read margin from this line.
//
// WHY THE PREDECESSOR WAS REPLACED (2026-08-26)
// The TDMsum line printed the peak TIME UNION during which any TDM RX ISR was executing, over a
// window equal to the shortest running leg's block deadline and phase-locked to that leg's block
// boundary. Wall time is response time: under the rate-monotonic priority map
// (APP_ASRC_RATE_MONOTONIC_ISR, on by default) the demoted leg absorbs the promoted leg's whole
// execution, so the "union" over-counted by exactly the overlap it existed to remove -- 48k/44.1k
// read 100.0 % where the legs' own time summed to about 72.6 %. Figures labelled TDMsum elsewhere
// in this tree came from that instrument and are NOT comparable with these.
//
// NOT CLAMPED. A peak above the window is printed as-is with over= beside it; a load past the
// budget is the finding, and rounding it to "100.0%" is what made the old line unusable.
//
// The window length is a build default (APP_DSPLOAD_WINDOW_US, 10 ms) overridable live from the
// console, so 1 ms / 10 ms / 100 ms are comparable within one image. Nothing here hardcodes it.
//===========================================================

// Live window length in microseconds. Set from the console; 0 restores the build default.
static uint32_t s_dsploadprof_window_us = (uint32_t)( APP_DSPLOAD_WINDOW_US );

uint32_t audio_transport_dbg_set_dsploadprof_window_us( uint32_t window_us )
{
    s_dsploadprof_window_us = ( window_us != 0u )
        ? window_us : (uint32_t)( APP_DSPLOAD_WINDOW_US );
    return s_dsploadprof_window_us;
}

uint32_t audio_transport_dbg_get_dsploadprof_window_us( void )
{
    return s_dsploadprof_window_us;
}

/* Ticks -> tenths of a percent of the window. 64-bit because ticks * 1000 overflows 32 bits for
 * any window past ~43 ms. Deliberately not clamped: a value over 1000 is the diagnosis. */
static uint32_t dsploadprof_pct_x10( uint32_t ticks, uint32_t window_ticks )
{
    if( window_ticks == 0u )
    {
        return 0u;
    }
    return (uint32_t)( ( (uint64_t)ticks * 1000ULL ) / (uint64_t)window_ticks );
}

static void audio_transport_dbg_print_dsploadprof(
    const audio_transport_snapshot_t* snapshot )
{
    static uint32_t s_cfg_epoch  = UINT32_MAX;   // last epoch the profiler was configured for
    static uint32_t s_cfg_window = 0u;           // last window length in high-res-timer counts

    /* us -> high-res-timer counts via FCY: main.c initialises the timer with timer_clk_hz = FCY,
     * so one count is one instruction cycle. The tick rate is not exposed by the timer HAL, so
     * the conversion belongs to the application -- same as the per-leg lines above. */
    const uint32_t window_us    = s_dsploadprof_window_us;
    const uint32_t window_ticks =
        (uint32_t)( ( (uint64_t)window_us * (uint64_t)FCY ) / 1000000ULL );

    if( window_ticks == 0u )
    {
        return;
    }

    /* Re-base on a new stream epoch (initial-start / restart / rate-change / clock-resume /
     * phase- & frmerr-recovery) so a stopped or reconfiguring gap never enters the statistics,
     * and on any window-length change. Unlike the predecessor, nothing about this window depends
     * on WHICH leg is running -- that dependence was the bug. */
    if( ( snapshot->stream_epoch != s_cfg_epoch ) || ( window_ticks != s_cfg_window ) )
    {
        s_cfg_epoch  = snapshot->stream_epoch;
        s_cfg_window = window_ticks;
        nora_cpu_load_prof_configure( window_ticks );
    }

    nora_cpu_load_snapshot_t p;
    if( !nora_cpu_load_prof_get( &p, true ) )
    {
        return;   // profiler never configured (high-res timer down)
    }

    if( p.windows == 0u )
    {
        /* No window closed inside this report interval: the report period is shorter than the
         * measurement window. Say so rather than printing a mean over zero windows. */
        printf("DSPload:win=%lu.%03lums n=0 (report interval shorter than the window)\n",
               (unsigned long)( window_us / 1000u ),
               (unsigned long)( window_us % 1000u ));
        return;
    }

    const uint32_t mean_a      = p.mean_self_ticks[NORA_CPU_LOAD_OWNER_LEG_A];
    const uint32_t mean_b      = p.mean_self_ticks[NORA_CPU_LOAD_OWNER_LEG_B];
    const uint32_t mean_stolen = p.mean_stolen_ticks[0] + p.mean_stolen_ticks[1];
    const uint32_t win_ticks   = p.window_period_ticks;

    /*
     * P1 diet (2026-08-27). Three of the eight percentages were EXACTLY derivable from their
     * neighbours -- self = A_self + B_self, demand = self + stolen, and
     * max_demand differs from max_self only by stolen -- and `win=`/`n=` are constants while the
     * profiler is healthy. What is left is the two per-leg loads and the peak; stolen (and with it
     * max_demand) appears only when a higher-priority vector actually took time, n= only when the
     * accounting is suspect. The window size is stated once, at the first report.
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md P1.
     */
    {
        static uint8_t win_announced = 0u;
        if( !win_announced )
        {
            win_announced = 1u;
            printf("DSPloadcfg:win=%lu.%03lums (self = A+B; demand = self+stolen; stolen/n printed only when they matter)\n",
                   (unsigned long)( window_us / 1000u ),
                   (unsigned long)( window_us % 1000u ));
        }
    }

    printf("DSPload:A=%lu.%lu%% B=%lu.%lu%% max=%lu.%lu%% ",
           (unsigned long)( dsploadprof_pct_x10( mean_a, win_ticks ) / 10u ),
           (unsigned long)( dsploadprof_pct_x10( mean_a, win_ticks ) % 10u ),
           (unsigned long)( dsploadprof_pct_x10( mean_b, win_ticks ) / 10u ),
           (unsigned long)( dsploadprof_pct_x10( mean_b, win_ticks ) % 10u ),
           (unsigned long)( dsploadprof_pct_x10( p.max_sum_ticks, win_ticks ) / 10u ),
           (unsigned long)( dsploadprof_pct_x10( p.max_sum_ticks, win_ticks ) % 10u ));

    if( mean_stolen != 0u )
    {
        printf("stolen=%lu.%lu%% max_demand=%lu.%lu%% ",
               (unsigned long)( dsploadprof_pct_x10( mean_stolen, win_ticks ) / 10u ),
               (unsigned long)( dsploadprof_pct_x10( mean_stolen, win_ticks ) % 10u ),
               (unsigned long)( dsploadprof_pct_x10( p.max_demand_ticks, win_ticks ) / 10u ),
               (unsigned long)( dsploadprof_pct_x10( p.max_demand_ticks, win_ticks ) % 10u ));
    }

    /* Overrun stated directly rather than as a negative margin, and only when it happened. */
    if( p.max_demand_ticks > win_ticks )
    {
        const uint32_t over_us10 =
            nora_high_res_timer_count_to_us_x10( p.max_demand_ticks - win_ticks );

        printf("over=%lu.%luus ", (unsigned long)( over_us10 / 10u ),
                                  (unsigned long)( over_us10 % 10u ));
    }

    /* n= windows closed in this report interval; the reader can check it against
     * (report period / window). bad= four PERMANENT-ZERO checks: unbalanced enter/exit, intervals
     * measured negative, grid re-bases (no hook ran for several windows), ownership nested deeper
     * than tracked. The hooks run interrupt-masked, so unlike the predecessor's race= these are
     * not an expected small loss -- any non-zero value means the accounting is wrong and the
     * percentages are suspect. */
    /* bad= stays unconditional: it is a harm counter, and a reader who greps for it must not have
     * to treat "absent" as "zero". n= joins it only when it is non-zero, because that is when the
     * window count is needed to judge how suspect the percentages are. */
    if( ( p.unbalanced | p.neg_delta | p.rebase | p.depth_overflow ) != 0u )
    {
        printf("n=%lu ", (unsigned long)p.windows);
    }
    printf("bad=%u/%u/%u/%u\n",
           (unsigned)p.unbalanced,
           (unsigned)p.neg_delta,
           (unsigned)p.rebase,
           (unsigned)p.depth_overflow);
}

// Per-leg TDM line tail verbosity (local to this function only; #undef'd right after).
// 0 = compact: only (run,act,blk,miss) -- the everyday health fields.
// 1 = verbose: append the deep-debug DMA/SPI counters (dov/dirq/ds/rov/tur/frm) that were
//     added during bring-up. Flip to 1 when chasing a DMA/framing fault.
#ifndef AUDIO_TRANSPORT_DBG_LEG_VERBOSE
#define AUDIO_TRANSPORT_DBG_LEG_VERBOSE  0
#endif

static void audio_transport_dbg_print_leg_end(
    const audio_transport_leg_snapshot_t* leg )
{
#if AUDIO_TRANSPORT_DBG_LEG_VERBOSE
    printf(" (run,act,blk,miss)=(%d,%d,%lu,%lu) dov=%lu dirq=%lu ds=0x%08lX rov=%lu tur=%lu frm=%lu\n",
           (int)leg->running, (int)leg->active,
           (unsigned long)leg->block_count,
           (unsigned long)leg->deadline_miss_count,
           (unsigned long)leg->rx_dma_overrun_count,
           (unsigned long)leg->rx_dma_other_irq_count,
           (unsigned long)leg->rx_dma_last_status,
           (unsigned long)leg->rx_overrun_block_count,
           (unsigned long)leg->tx_underrun_block_count,
           (unsigned long)leg->frame_error_block_count);
#else
    printf(" (run,act,blk,miss)=(%d,%d,%lu,%lu)\n",
           (int)leg->running, (int)leg->active,
           (unsigned long)leg->block_count,
           (unsigned long)leg->deadline_miss_count);
#endif // AUDIO_TRANSPORT_DBG_LEG_VERBOSE
}

#undef AUDIO_TRANSPORT_DBG_LEG_VERBOSE

bool audio_transport_codec_b_missing_stop_active( void )
{
#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
    return s_codec_b_missing_stop_latched;
#else
    // Single-codec build, or the AK128 J3 bring-up build: the latch this reports
    // does not exist here (never set), so callers with their own periodic
    // telemetry (see classic_demo_app.c) should never silence on its account.
    return false;
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
}

// Owns the app-level telemetry line(s): TDM stream health + per-ISR load, the ASRC state,
// and the CCP-measured rates/ratio. SELF-GATED on s_dbg_period_ms and meant to be called
// every iteration of main()'s loop (it prints only when the period has elapsed). Consumes the
// neutral transport snapshot and derives the block-count rates; the ASRC/CCP details come from
// their own modules. (Moved here from main.c so each module owns its own debug output.)
void audio_transport_dbg_print( void )
{
#if NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B
    // Latched codec-B-missing stop: replace the normal telemetry with the guidance line,
    // repeated every 2 s, for as long as the condition holds. A one-shot print is not
    // enough -- a USB-CDC terminal that reconnects after the boot-time banner already went
    // by (the port re-enumerates on reset, same timing gap as a missed startup banner)
    // would otherwise find a silent, healthy-looking console with nothing to explain why
    // there is no audio.
    if( s_codec_b_missing_stop_latched )
    {
        const uint32_t cur = GetTicks();
        if( (uint32_t)( cur - s_codec_b_missing_last_prt_ms ) >= 2000u )
        {
            s_codec_b_missing_last_prt_ms = cur;
            audio_transport_print_codec_b_missing_guidance();
        }
        return;
    }
#endif // NORA_TDM_USE_SPI2 && !APP_AK128_J3_TDM_B

    static uint32_t last_prt = UINT32_MAX;
    const uint32_t  cur = GetTicks();
    // period 0 == telemetry DISABLED (documented contract of audio_transport_set_dbg_period_ms). Without
    // this explicit guard the unsigned compare `(cur-last_prt) < 0` is never true, so period 0 would print
    // on EVERY call (a flood) instead of disabling output. Guard it so 0 truly silences the line.
    if( ( s_dbg_period_ms == 0u ) ||
        ( (uint32_t)( cur - last_prt ) < s_dbg_period_ms ) )
    {
        return;
    }
    last_prt = cur;

    audio_transport_snapshot_t transport_snapshot;
    const bool transport_snapshot_valid =
        audio_transport_snapshot_take_window( &transport_snapshot );
    /*
     * P1 (2026-08-27): this line is STATE, not health -- every field of it is constant while the
     * stream runs, so repeating it every ~2 s buried the restarts and failures it exists to
     * announce. Print it when any field CHANGES (and once at the first report), which is exactly
     * the transition edge. Nothing is lost: the same snapshot is available on demand.
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md P1.
     */
    static uint8_t  stream_memo_valid = 0u;
    static uint32_t stream_memo_epoch = 0u;
    static uint8_t  stream_memo_flags = 0u;
    static uint8_t  stream_memo_transition = 0u;
    static uint8_t  stream_memo_error = 0u;
    const uint8_t   stream_flags = transport_snapshot_valid
        ? (uint8_t)( ( transport_snapshot.qualified_running               ? 0x01u : 0u )
                   | ( transport_snapshot.safe_mute_latched               ? 0x02u : 0u )
                   | ( transport_snapshot.transition_failed               ? 0x04u : 0u )
                   | ( transport_snapshot.mute_held_by_failed_transition  ? 0x08u : 0u ) )
        : 0u;
    const bool stream_changed = transport_snapshot_valid
        && ( ( stream_memo_valid == 0u )
          || ( stream_memo_epoch != transport_snapshot.stream_epoch )
          || ( stream_memo_flags != stream_flags )
          || ( stream_memo_transition != (uint8_t)transport_snapshot.last_transition )
          || ( stream_memo_error != (uint8_t)transport_snapshot.last_transition_error ) );

    if( transport_snapshot_valid && stream_changed )
    {
        stream_memo_valid      = 1u;
        stream_memo_epoch      = transport_snapshot.stream_epoch;
        stream_memo_flags      = stream_flags;
        stream_memo_transition = (uint8_t)transport_snapshot.last_transition;
        stream_memo_error      = (uint8_t)transport_snapshot.last_transition_error;

        printf("STREAM epoch=%lu qualified=%d transition=%s safe_mute=%d failed=%d error=%s mute_held=%d\n",
               (unsigned long)transport_snapshot.stream_epoch,
               transport_snapshot.qualified_running ? 1 : 0,
               audio_transport_transition_name( transport_snapshot.last_transition ),
               transport_snapshot.safe_mute_latched ? 1 : 0,
               transport_snapshot.transition_failed ? 1 : 0,
               audio_transport_transition_error_name(
                   transport_snapshot.last_transition_error ),
               transport_snapshot.mute_held_by_failed_transition ? 1 : 0 );
    }

#if RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED
    // Step-1 phase-probe readout: samples = leg-A blocks observed; mismatch = blocks where
    // leg A/B were on DIFFERENT ping-pong halves (=phase offset -> the tearing cause);
    // unresolved = a live-DMA snapshot fell outside its buffer (reload boundary). In-phase
    // and locked => mismatch stays 0 across minutes and restarts. (h1,h2)=last sampled halves.
    printf("PHASE: samp=%lu mism=%lu unres=%lu (h1,h2)=(%d,%d) | wdiff last=%ld[%ld..%ld] tail_tear=%lu\n",
           (unsigned long)s_phase_samples, (unsigned long)s_phase_mismatch,
           (unsigned long)s_phase_unresolved, s_phase_h1, s_phase_h2,
           (long)s_phase_wdiff_last, (long)s_phase_wdiff_min, (long)s_phase_wdiff_max,
           (unsigned long)s_phase_tail_tear);
#endif // RESOLVED_TRANSPORT_PHASE_PROBE_ENABLED

#if RESOLVED_SAI_TEST_LIVE_ENABLED
    audio_transport_sai_live_dbg_print();
#endif // RESOLVED_SAI_TEST_LIVE_ENABLED

    // Consolidated TDM stream-health + per-ISR load. Consume the neutral snapshot so
    // console formatting does not query HAL status directly. take_window() cleared the
    // sampled callback peaks, preserving the existing per-print-window max behavior.
    // Application-specific diagnostics are printed separately from the neutral TDM lines.
    if( transport_snapshot_valid )
    {
        const audio_transport_leg_snapshot_t* leg_a =
            &transport_snapshot.legs[AUDIO_TRANSPORT_LEG_A];
        audio_transport_dbg_print_leg_begin( leg_a, AUDIO_TRANSPORT_LEG_A );

        audio_transport_dbg_print_leg_end( leg_a );

        // Logical leg-B health (independent block ISR when the domains are asynchronous).
        if( transport_snapshot.leg_count > AUDIO_TRANSPORT_LEG_B )
        {
#if RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED
            // Say it instead of printing frozen numbers. Every figure on a leg-B line comes from
            // its block ISR, and that ISR is deliberately masked here -- a stalled blk count and a
            // stale max are precisely how a dead leg looks, so printing them invites a hunt for a
            // fault that does not exist. Codec B is streaming: leg A's callback fills its TX half.
            printf("TDM%u:irq-gated (RX unread, TX mirrored by TDM%u -- no leg-B ISR)\n",
                   (unsigned)RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE,
                   (unsigned)RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE);
#else
            const audio_transport_leg_snapshot_t* leg_b =
                &transport_snapshot.legs[AUDIO_TRANSPORT_LEG_B];
            audio_transport_dbg_print_leg_begin( leg_b, AUDIO_TRANSPORT_LEG_B );
            audio_transport_dbg_print_leg_end( leg_b );
#endif
        }

        // Engine-wide DSP load: the legs' own (preemption-excluded) CPU time over a fixed
        // measurement window that is independent of the block period -- see its header.
        audio_transport_dbg_print_dsploadprof( &transport_snapshot );

        audio_application_telemetry_print(
            &transport_snapshot, cur, audio_transport_frmerr_recover_count() );

        // Blank line closing the report: one telemetry set is STREAM ... CCP, and the sets
        // arrive every APP_..._DBG_PERIOD_MS, so without a separator a scrolling console
        // gives no visual cue where one snapshot ends and the next begins.
        printf("\n");
    }
}

void audio_transport_start( void )
{
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    audio_transport_cmsis_sai_start();
#else
    audio_transport_hal_start();
#endif // RESOLVED_SAI_TEST_LIVE_ENABLED
}

bool audio_transport_manage( void )
{
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    return audio_transport_cmsis_sai_manage();
#else
    return audio_transport_hal_manage();
#endif // RESOLVED_SAI_TEST_LIVE_ENABLED
}

bool audio_transport_restart( void )
{
#if RESOLVED_SAI_TEST_LIVE_ENABLED
    return audio_transport_restart_route( AUDIO_TRANSPORT_ROUTE_CMSIS_SAI,
                                    AUDIO_TRANSPORT_TRANSITION_MANUAL_RESTART );
#else
    return audio_transport_restart_route( AUDIO_TRANSPORT_ROUTE_HAL_DIRECT,
                                    AUDIO_TRANSPORT_TRANSITION_MANUAL_RESTART );
#endif // RESOLVED_SAI_TEST_LIVE_ENABLED
}

bool audio_transport_restart_declick( uint8_t declick_mask )
{
    // Declick research (one-shot): arm the codec strategy bitmask for the duration of this restart,
    // then re-arm NONE so the auto-recovery / shipping restart path always runs on baseline. A mask of
    // 0 is exactly audio_transport_restart(). See wm8904_set_pending_declick() / research doc.
    printf(" [declick] one-shot restart with mask=0x%02x\n", (unsigned)declick_mask );
    wm8904_set_pending_declick( declick_mask );
    const bool ok = audio_transport_restart();
    wm8904_set_pending_declick( (uint8_t)WM8904_DECLICK_NONE );
    return ok;
}

bool audio_transport_declick_research_available( void )
{
    return wm8904_declick_research_available();
}

void audio_transport_declick_print_help( void )
{
    wm8904_declick_print_strategy_help();
}

void audio_transport_declick_print_status( void )
{
    // Reports whether a cold-boot STARTUP DC-servo run has captured offsets for each codec. WARM_SERVO
    // (mask bit1) uses DCS_TRIG_DAC_WR only when captured; otherwise it falls back to a STARTUP servo.
    printf("   servo-captured: A=%d", (int)wm8904_declick_servo_captured( I2C_INST_A ) );
#if NORA_TDM_USE_SPI2
    printf(" B=%d", (int)wm8904_declick_servo_captured( I2C_INST_B ) );
#endif
    // Only mention WARM_SERVO where it exists: with the research code compiled out the capture
    // still runs (it is part of the ordinary STARTUP servo path) but nothing can consume it.
    if( wm8904_declick_research_available() )
    {
        printf(" (WARM_SERVO falls back to STARTUP until captured)");
    }
    printf("\n");
}

bool audio_transport_restart_codec_b_only_declick( uint8_t declick_mask )
{
    // Declick research (automated loopback measurement): re-init ONLY the CODEC-B chip with the declick
    // strategy `declick_mask`, WITHOUT stopping the dsPIC transport or touching CODEC-A. Requires the
    // independent B-codec-master topology (B has its own XTAL/clock domain), so A keeps streaming and can
    // record B's analog output looped back into A's line-in (B HPOUT -> cable -> A LINE-IN -> A ADC).
    //
    // This runs synchronously in the caller's (main-loop console) context, so the FRMERR/liveness manage
    // tick -- also main-loop -- cannot fire a whole-transport recovery mid-measurement. B's clock does
    // hiccup on a non-WARM restart (R0 reset); any resulting frame-slip is recovered by the manage loop
    // AFTER this returns (A is unaffected during the window).
#if NORA_TDM_USE_SPI2 && (RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)
    printf(" [declick-B] codec-B-only restart mask=0x%02x (A keeps running)\n", (unsigned)declick_mask );
    (void)audio_transport_declick_b_shutdown_only( declick_mask );
    return audio_transport_declick_b_startup_only( declick_mask );
#else
    (void)declick_mask;
    printf(" [declick-B] not supported: requires the independent B-codec-master topology\n");
    return false;
#endif
}

// Phase-split B-only declick (for isolating SHUTDOWN vs STARTUP pop). The measurement harness arms the
// pop meter, runs one phase, reads the meter, then runs the other -- so each phase's pop is attributed
// separately. Same topology guard / synchronous-context reasoning as the combined call above.
bool audio_transport_declick_b_shutdown_only( uint8_t declick_mask )
{
#if NORA_TDM_USE_SPI2 && (RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)
    wm8904_set_pending_declick( declick_mask );
    wm8904_set_analog_output_mute( I2C_INST_B, true );   // mute B (ramp-down if mask bit4/E)
    wm8904_shutdown( I2C_INST_B );                        // strategy-aware discharge (quench/ordered/WSEQ)
    // pending mask intentionally left armed; the paired startup_only consumes and clears it.
    return true;
#else
    (void)declick_mask;
    return false;
#endif
}

bool audio_transport_declick_b_startup_only( uint8_t declick_mask )
{
#if NORA_TDM_USE_SPI2 && (RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)
    wm8904_set_pending_declick( declick_mask );
    /*
     * Re-init B as codec-master (endpoint clock) in ITS OWN converter role. The role
     * is a property of the leg, not of this call: at 96 kHz leg B is DAC-only,
     * because the WM8904 cannot run ADC and DAC together above 48 kHz. Hardcoding
     * ADC+DAC here made a rate change on leg B unrepresentable -- which is why
     * "*ar 1 8" (B -> 48 kHz) reported success while B kept running at 96 kHz.
     */
    const bool ok = wm8904_init_role( I2C_INST_B, true, AUDIO_TRANSPORT_LEG_B_ROLE );
    if( ok )
    {
        // Only unmute a codec that actually re-initialised. Unmuting a failed/unreachable B
        // would drive a broken analog block (audible pop) and yield a meaningless measurement --
        // match the shipping start_route_impl contract (unmute gated on apply success).
        wm8904_set_analog_output_mute( I2C_INST_B, false );   // unmute B (soft-ramp up if mask bit2/D)
    }
    wm8904_set_pending_declick( (uint8_t)WM8904_DECLICK_NONE );
    return ok;
#else
    (void)declick_mask;
    return false;
#endif
}

#if NORA_TDM_USE_SPI2 && (RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)
// Fast LEG-B rate change: re-init ONLY codec-B (at its already-set new rate) while the transport and
// codec-A keep running. Reuses the *ap codec-B-only primitives: mute B, strategy-none shutdown, re-init
// B as codec-master (which picks up the new wm8904 rate), unmute B. B briefly drops its BCLK while
// re-initing, so the dsPIC SPI2 slave sees a transient frame-slip -- cleared here so the manage loop
// does not escalate to a full-transport recovery. Codec-A never stops and the A-side DSP is untouched
// (A's rate is unchanged, so prepare_client would be a no-op anyway). Roughly half the switch time of
// the whole-transport restart.
//
// The client's stream state IS re-locked here (reset_stream_state), exactly as the full-transport
// restart does at audio_transport_start_hal_transport(). Without it the ASRC feed-forward ratio servo
// cannot follow a large rate step: asrc_apply_ratio() only re-centres the FIFO and re-seeds step_state
// on the invalid->valid transition, so a live (ratio>0) update just moves the target while step_state
// crawls at ASRC_STEP_SLEW (~2e-6/block). Over a 48<->32 (33%) jump the 128-frame ring saturates in ms
// and never recovers. Re-locking drives the ratio back to 0 and re-arms the CCP acquire, so the next
// feed-forward value hits the invalid->valid path: re-centre + re-seed step_state to the new rate.
// This is the SAME re-lock the cold-start path relies on (producer already live, first ratio arrives),
// so it is safe to invoke here without stopping the transport.
static bool audio_transport_reconfigure_codec_b_only( void )
{
    printf(" [rate-B] fast codec-B-only re-init at %lu Hz (transport + codec-A keep running)\n",
           (unsigned long)wm8904_get_rate_hz( I2C_INST_B ) );
    (void)audio_transport_declick_b_shutdown_only( (uint8_t)WM8904_DECLICK_NONE );
    const bool ok = audio_transport_declick_b_startup_only( (uint8_t)WM8904_DECLICK_NONE );
    delay_ms( 50 );
    audio_transport_frmerr_reset();   // clear the transient B-clock frame-slip (benign; B re-inited clean)

    // Re-lock the application stream state (ASRC servo + CCP acquire) to B's new rate. Mirrors the
    // full-restart re-lock without stopping the transport; see the block comment above.
    const audio_transport_client_t* client = audio_transport_client_get();
    if( ( client != NULL ) && ( client->reset_stream_state != NULL ) )
    {
        client->reset_stream_state( client->user );
    }
    return ok;
}
#endif // fast codec-B-only rate change available

bool audio_transport_leg_rate_is_supported( transport_leg_t leg,
                                            uint32_t        sample_rate_hz,
                                            const char**    reason_out )
{
    const char* reason = NULL;
    bool ok = false;

#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    wm8904_role_t role;
    switch( leg )
    {
        case TRANSPORT_LEG_A: role = AUDIO_TRANSPORT_LEG_A_ROLE; ok = true; break;
        case TRANSPORT_LEG_B: role = AUDIO_TRANSPORT_LEG_B_ROLE; ok = true; break;
        default: reason = "unknown transport leg"; break;
    }

    /*
     * >= 88.2 kHz is the WM8904's own high-rate boundary (datasheet), which is why
     * the test is written as a threshold rather than against 96 kHz. In the rate
     * table 96 kHz is the ONLY rate on this side of it -- there is no 88.2 kHz row --
     * so the messages below name 96 kHz, which is what a user can actually ask for.
     */
    if( ok && ( sample_rate_hz >= 88200u ) )
    {
        /*
         * Both conditions are build facts, so they can be answered here with no
         * codec access. Checking them BEFORE the mute-bounded restart is the whole
         * point: a request that cannot succeed should not cost a stream teardown.
         */
        if( RESOLVED_TRANSPORT_SLOTS_PER_FRAME != 2u )
        {
            ok = false;
            reason = "96 kHz needs a 2-slot I2S frame; this build is TDM8";
        }
        else if( role == WM8904_ROLE_ADC_DAC )
        {
            ok = false;
            reason = "96 kHz cannot run ADC and DAC together; this leg is full-duplex";
        }
    }
#else
    (void)leg;
    (void)sample_rate_hz;
    reason = "runtime rate change needs an endpoint-clocked leg B (codec-master build)";
#endif

    if( reason_out != NULL ) { *reason_out = reason; }
    return ok;
}

bool audio_transport_reconfigure_leg_rate_hz( transport_leg_t leg,
                                              uint32_t        sample_rate_hz )
{
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    uint8_t codec_instance;
    uint8_t leg_index;
    switch( leg )
    {
        case TRANSPORT_LEG_A:
            codec_instance = I2C_INST_A;
            leg_index = AUDIO_TRANSPORT_LEG_A;
            break;
        case TRANSPORT_LEG_B:
            codec_instance = I2C_INST_B;
            leg_index = AUDIO_TRANSPORT_LEG_B;
            break;
        default: return false;
    }

    /*
     * Reject an impossible target BEFORE the mute-bounded restart, so a bad request
     * costs nothing instead of tearing down a working stream and failing afterwards.
     * Gated here (not only in the console) so every caller gets the same contract.
     */
    {
        const char* reason = NULL;
        if( !audio_transport_leg_rate_is_supported( leg, sample_rate_hz, &reason ) )
        {
            printf(" [rate] leg %c -> %lu Hz rejected: %s\n",
                   ( leg == TRANSPORT_LEG_A ) ? 'A' : 'B',
                   (unsigned long)sample_rate_hz,
                   ( reason != NULL ) ? reason : "unsupported" );
            return false;
        }
    }

    const uint32_t previous_rate_hz = wm8904_get_rate_hz( codec_instance );
    if( !wm8904_set_rate_hz( codec_instance, sample_rate_hz ) )
    {
        return false;
    }

/*
 * A/B SWITCH for the rate-monotonic-ISR stack-error investigation. Set to 0 and a leg-B rate
 * change takes the SAME whole-transport restart path leg A takes, instead of the live fast path
 * below.
 *
 * WHY IT EXISTS, and why it is a switch rather than a fix. The RM priority assignment
 * (asrc_audio_path_apply_isr_priorities(), added by a9f8cc9) states as its contract that it runs
 * "while the transport is stopped ... before any domain starts", and is "deliberately NOT
 * dynamic". The ASRC layer registers it behind reset_stream_state(), and the fast path below
 * calls reset_stream_state() with the transport DELIBERATELY still running -- so the one callback
 * is reached down two paths that make OPPOSITE claims about whether anything is streaming. That
 * is a control-flow contradiction, not a stale comment: measured on hardware, the fast path does
 * re-run the assignment, and it does so with both legs' ISRs live. It is NOT self-preemption --
 * the assignment runs at task level and so cannot execute while an ISR is in flight, and a source
 * at the same IPL as the running level does not preempt it either -- but it does update the two
 * legs' IPC in SEQUENCE, so between the two writes the priority map is neither the old one nor the
 * new one, and it resets ASRC stream state while the producers of that state keep firing. Note the
 * DMA HAL's ordinary reconfigure path masks the IRQ before rewriting a live channel for exactly
 * this class of reason; the RM priority setter does not.
 *
 * Setting this to 0 restores a9f8cc9's own precondition WITHOUT touching one line of the RM logic,
 * which is what makes the resulting sweep a clean discriminator:
 *   STACK ERROR = 0 here  -> the fault needs the live reconfigure, and the fix belongs at that seam
 *   STACK ERROR persists  -> live IPC change is not the mechanism; look elsewhere
 * The cost of 0 is only switch TIME (leg B stops paying for its own clock domain); it is not a
 * capability change, which is why it is safe to ship either value while the question is open.
 * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 19.
 */
#ifndef APP_TRANSPORT_LEG_B_FAST_RATE_CHANGE
#define APP_TRANSPORT_LEG_B_FAST_RATE_CHANGE 1
#endif

#if APP_TRANSPORT_LEG_B_FAST_RATE_CHANGE && NORA_TDM_USE_SPI2 && (RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)
    // Fast path: a LEG-B rate change on a live stream only needs codec-B re-inited (it owns its own
    // clock domain; the dsPIC SPI2 slave follows). Codec-A and the DSP are unaffected -- A's rate is
    // unchanged and the 16ch ASRC decouples the two legs -- so re-tuning nothing and re-starting nothing
    // else roughly halves the switch time versus the whole-transport restart below. LEG-A still takes the
    // full restart path (its fs-parameterized DSP re-tune via prepare_client requires the transport stopped).
    if( leg == TRANSPORT_LEG_B && TDM_activated )
    {
        if( audio_transport_reconfigure_codec_b_only() )
        {
            s_current_configured_rate_hz[leg_index] = sample_rate_hz;
            return true;
        }
        // Fast path failed: restore the previous rate and re-init B back to it (transport still live).
        (void)wm8904_set_rate_hz( codec_instance, previous_rate_hz );
        printf(" [rate-B] fast re-init failed; requested rate restored to %lu Hz\n",
               (unsigned long)previous_rate_hz );
        (void)audio_transport_reconfigure_codec_b_only();
        return false;
    }
#endif

#if RESOLVED_SAI_TEST_LIVE_ENABLED
    const audio_transport_route_t route = AUDIO_TRANSPORT_ROUTE_CMSIS_SAI;
#else
    const audio_transport_route_t route = AUDIO_TRANSPORT_ROUTE_HAL_DIRECT;
#endif
    if( audio_transport_restart_route( route, AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE ) )
    {
        s_current_configured_rate_hz[leg_index] = sample_rate_hz;
        return true;
    }

    (void)wm8904_set_rate_hz( codec_instance, previous_rate_hz );
    printf(" [rate-change] apply failed; requested rate restored to %lu Hz\n",
           (unsigned long)previous_rate_hz );

    if( !TDM_activated )
    {
        const bool rollback_ok = audio_transport_restart_route(
            route, AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE_ROLLBACK );
        printf(" [rate-change] rollback restart %s\n",
               rollback_ok ? "restored previous stream" : "FAILED (muted/stopped)" );
    }
    return false;
#else
    (void)leg;
    (void)sample_rate_hz;
    return false;
#endif // endpoint-owned leg-B clocking
}
