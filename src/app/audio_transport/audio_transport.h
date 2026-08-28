#ifndef AUDIO_TRANSPORT_H
#define AUDIO_TRANSPORT_H

//===========================================================
// audio_transport.h
//
// Transitional shared codec/transport integration. This is NOT part of the public
// SPI/I2S/TDM HAL core and is NOT a CMSIS-wrapper dependency: the HAL core neither
// includes nor calls this layer.
//
// audio_transport currently owns codec/transport orchestration, co-clock safety,
// recovery and transitional callback composition. Classic PCM interpretation,
// DSP processing and work buffers are owned by apps/classic/classic_audio_path.c.
// DMA ping-pong buffers remain owned by nora_spi_i2s_tdm_dspic33ak.c.
//===========================================================

#include <stdbool.h>
#include <stdint.h>


//===========================================================
// audio_app is currently the top of the shared audio integration call tree.
// main() chooses one top-level route explicitly:
//   - HAL direct: application -> nora_spi_i2s_tdm HAL
//   - CMSIS-SAI:  application -> CMSIS-Driver SAI wrapper -> HAL
// audio_app owns the TDM stream lifecycle and WM8904 codec ordering. Application
// DSP remains behind the selected app path. CMSIS-SAI details stay private to
// audio_transport_sai_live.c / audio_transport_sai_drytest.c.
//===========================================================

// Bring up the HAL-direct audio stream: WM8904 shutdown -> (master init) -> TDM configure/start
// -> stream-running check -> WM8904 slave init (DC-servo needs BCLK) -> unmute.
// Idempotent (no-op if already started). On a start failure it stays muted.
void audio_transport_hal_start( void );

// Stream-management state machine for the HAL-direct route.
bool audio_transport_hal_manage( void );

// Tear down the HAL-direct route (currently shared transport stop).
void audio_transport_hal_stop( void );

// Bring up the CMSIS-SAI live route. The codec lifecycle is shared with the HAL route;
// only the transport start path is routed through the CMSIS-SAI wrapper harness.
void audio_transport_cmsis_sai_start( void );

// Stream-management state machine for the CMSIS-SAI live route.
bool audio_transport_cmsis_sai_manage( void );

// Tear down the CMSIS-SAI live route (currently shared transport stop).
void audio_transport_cmsis_sai_stop( void );

// Route-specific debug line hook. In a CMSIS-SAI live build this prints the wrapper
// live harness diagnostics; otherwise it is a no-op.
void audio_transport_dbg_print( void );

// Change the audio_transport_dbg_print() period at runtime (ms). Zero disables periodic output.
void audio_transport_set_dbg_period_ms( uint32_t ms );

// Simple telemetry ON/OFF: ON restores the resolved default period; OFF disables the periodic line.
void audio_transport_dbg_enable( bool on );

// True while periodic telemetry is enabled (period != 0). This is the PARENT SWITCH for every
// periodic monitor, not just this file's line: an app's sonora_app_debug_print() must consult it
// so that "*tq0000" silences the console completely.
//
// That is not a convenience. *feaa55 / *fu5A disable telemetry and then drain the UART before
// resetting, so that the update status is the last thing on the wire. Any monitor that keeps
// printing through that drain interleaves with the status -- and, during an XMODEM transfer, with
// the protocol itself. One switch the console can trust is the only version of this that is safe.
bool audio_transport_dbg_enabled( void );

//===========================================================
// DSPload measurement window length, in microseconds.
//
// The window of the engine-wide DSPload telemetry line is a FIXED TIME, deliberately unrelated to
// the block period, so the same image can be read at 1 ms / 10 ms / 100 ms and the readings
// compared. The setter takes effect on the next report (the profiler is re-based, which clears
// the accumulators -- a window change cannot be averaged across).
//
// window_us = 0 restores the build default (APP_DSPLOAD_WINDOW_US). Both return the value in
// force after the call. Console: *dl<hex us> to set, ?dl to read.
//===========================================================
uint32_t audio_transport_dbg_set_dsploadprof_window_us( uint32_t window_us );
uint32_t audio_transport_dbg_get_dsploadprof_window_us( void );

// (audio_transport_asrc_dbg_print lives in apps/asrc/audio_transport_asrc.h.)

// TDM auto-recovery tick: a sustained FRMERR, or an RX block stall while independently-captured
// FS keeps advancing, starts a mute-held full-restart episode. Mute is released only after both
// legs produce a configured run of NEW FRMERR-clean blocks; bounded failures latch safe-mute.
// Self-gated by the configured check/cooldown policy; call every main-loop iteration. No-op
// unless the independent-domain clock detector and automatic recovery policy are enabled.
void audio_transport_frmerr_recover_tick( void );

// Number of full-restart attempts made by bit-slip recovery since boot -- shown on telemetry.
uint32_t audio_transport_frmerr_recover_count( void );

// True once FRMERR recovery has latched a safe-mute (restart attempts kept failing -> silent fault).
// Surface on telemetry so a persistent silence is distinguishable from a normal quiet stream.
bool audio_transport_frmerr_safe_mute( void );

// TEST (*nt43): arm one recovery episode without requiring a physical FS disturbance. It uses
// the same mute-held restart and clean-frame qualification path as a real FRMERR/liveness trip.
// Harmless no-op in builds where automatic recovery is unavailable.
void audio_transport_frmerr_force_trip( void );

// Whether the force hook above, and automatic recovery generally, are built into THIS image.
// Exists so a console verb can say "not in this configuration" instead of reporting a trip it
// armed and that nothing will ever consume -- the no-op is correct, but silently reporting
// success for it is what makes a co-clocked profile look like a broken one.
//
// Deliberately a function, not a macro: the answer depends on the resolved transport policy,
// and callers (console modules) have no business including that header to ask.
bool audio_transport_frmerr_autorecovery_available( void );

// Clear a TRANSIENT frmerr/liveness state so the manage loop does not fire a full-transport
// auto-recovery. Used after a deliberate CODEC-B-only declick pop test (*ap), which briefly stops
// B's clock (expected/benign -- B is cleanly re-inited). No-op where auto-recovery is unavailable.
void audio_transport_frmerr_reset( void );

// Compatibility wrappers for older app code/debug commands. They dispatch to the
// selected route according to the resolved CMSIS-SAI live-test selection.
void audio_transport_start( void );

void audio_transport_stop( void );

/* Terminal pre-flash stop: verify the codec analog mute, then stop TDM/DMA.
 * The result remains available through audio_transport_stop_for_flash_report(). */
bool audio_transport_stop_for_flash( void );
void audio_transport_stop_for_flash_report( void );

/* True once a dual-codec build's WM8904-B has definitively failed to answer and the
 * manage loop has latched into a reported, non-retrying stop (the guidance printed by
 * audio_transport_report_codec_b_missing()). Callers with their OWN periodic telemetry
 * (e.g. the app's bass-enhancer level-meter line) should check this and go quiet: a wall
 * of unrelated "everything is fine"-looking chatter is exactly what scrolls the one-time
 * guidance message off screen before anyone reads it. */
bool audio_transport_codec_b_missing_stop_active( void );

bool audio_transport_manage( void );

/* Mute-bounded same-rate restart; false leaves the stream mute-held and diagnosed. */
bool audio_transport_restart( void );

/*
 * Declick research (one-shot): same as audio_transport_restart(), but the mute-bounded restart runs
 * with the codec declick-strategy bitmask `declick_mask` (bitwise-OR of wm8904_declick_mask_t) armed
 * for its duration, then re-armed to NONE. `declick_mask == 0` is identical to audio_transport_restart().
 * Used by the *td console command for A/B listening comparison against *tr (baseline). Shipping and the
 * FRMERR auto-recovery restart never call this, so their behavior is unchanged.
 */
bool audio_transport_restart_declick( uint8_t declick_mask );

/*
 * Declick research: is the codec-side A/B strategy code present in this build? The driver fixes that
 * as a build policy inside wm8904.c, so ask -- do not assume. False => a non-zero *td mask is not
 * honoured and the console refuses it instead of pretending to apply a strategy.
 */
bool audio_transport_declick_research_available( void );

/* Declick research: print the *td<NN> strategy legend, or the one-line "compiled out" note. */
void audio_transport_declick_print_help( void );

/* Declick research: print per-codec retained-servo capture status (used by the ?td console help). */
void audio_transport_declick_print_status( void );

/*
 * Declick research (automated loopback pop measurement): re-init ONLY CODEC-B with the declick strategy
 * bitmask, leaving the transport and CODEC-A running. Intended for the B-codec-master topology so A can
 * record B's restart pop via an external loop (B HPOUT -> A LINE-IN). Returns false (no-op) on other
 * topologies. Runs synchronously so the manage-loop recovery cannot hijack the measurement window.
 */
bool audio_transport_restart_codec_b_only_declick( uint8_t declick_mask );

/*
 * Phase-split variants of the B-only declick restart, for isolating SHUTDOWN-phase vs STARTUP-phase pop.
 * Call shutdown_only(), measure, then startup_only(), measure. shutdown_only leaves the declick mask
 * armed for the paired startup_only, which clears it. No-op (false) outside the B-codec-master topology.
 */
bool audio_transport_declick_b_shutdown_only( uint8_t declick_mask );
bool audio_transport_declick_b_startup_only( uint8_t declick_mask );

#endif //!AUDIO_TRANSPORT_H
