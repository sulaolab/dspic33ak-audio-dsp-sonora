#ifndef AUDIO_APP_ASRC_H
#define AUDIO_APP_ASRC_H

//===========================================================
// ASRC App engine interface (audio_app_asrc.h)
//
// A<->B asynchronous sample-rate converter (ASRC) ENGINE for the two independent
// clock domains (A = codec-A master ~48.000kHz, B = dsPIC SPI2 master ~43.403kHz).
//
// Two directions, one cross-domain ring FIFO + variable-ratio resampler each:
//   ab : A->B  (producer = A's RX ISR, consumer = B's RX ISR)
//   ba : B->A  (producer = B's RX ISR, consumer = A's RX ISR) -- only for the
//              routes that cross both ways (BIDIR / LIGHT; APP_B_ROUTE_USES_BA).
//
// The instances, FIFOs, resampler, drift control and telemetry live in
// audio_app_asrc.c. audio_transport.c owns the ROUTE wiring (which ISR pushes/pulls which
// direction) and the app-side output DSP -- it just calls the functions below.
//
// Lock-free: both RX ISRs share PRIO_TDM_DMA and never preempt each other, so each
// FIFO is single-producer / single-consumer with atomic 32-bit indices.
//
// Declared/defined only in an ASRC-route build; callers guard on the same macros.
//===========================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_specific_config_defs.h"   // APP_B_INDEP_DOMAIN / APP_B_ROUTE_* + geometry

#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC

// Clear both FIFOs + resampler phase and invalidate ratios. Call on every (re)start.
// The resampler stays silent until measured feed-forward ratios are set (no nominal seed).
void audio_app_asrc_reset_all( void );

// Feed-forward ratio (live, no compile-time nominal). ratio = input_fs/output_fs for the
// direction: ab = fs_A/fs_B, ba = fs_B/fs_A. <=0 keeps that direction silent (startup,
// before the first measured rate). The fill error still trims the step around this value.
void audio_app_asrc_set_ratio_ab( float ratio );

// A->B direction: push A's input block (slots 0/1) into the FIFO / produce B's output.
void audio_app_asrc_push_ab( const int32_t* src );
void audio_app_asrc_pull_ab( int32_t* dst );

#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
// Push already-decimated frames; FIFO units follow the active intermediate rate.
void audio_app_asrc_push_ab_frames( const int32_t* src, size_t frames, size_t stride );
uint32_t audio_app_asrc_intermediate_overflow_count( void );
uint32_t audio_app_asrc_intermediate_underrun_count( void );
#if APP_B_ROUTE_USES_BA && APP_ASRC_RUNTIME_48K_TO_8
// Mirror image for the other down-sampling case: leg B at 48 kHz feeding a low-rate leg A,
// so B->A owns the front end. Only one direction can have one at a time (A=48k and B=48k
// are mutually exclusive), so these and the ..._ab_frames producer are never both live.
void audio_app_asrc_push_ba_frames( const int32_t* src, size_t frames, size_t stride );
uint32_t audio_app_asrc_intermediate_overflow_count_ba( void );
uint32_t audio_app_asrc_intermediate_underrun_count_ba( void );
#endif
#endif

#if APP_ASRC_MEAS
// Bench (measurement): freeze the A->B step to a constant (loop open) / restore normal control.
void audio_app_asrc_freeze( void );
// Bench: freeze exactly at the live feed-forward ratio and re-centre the FIFO.
// This isolates direct large-ratio kernel behavior without waiting for servo convergence.
void audio_app_asrc_freeze_ratio_ab( void );
void audio_app_asrc_unfreeze( void );
// Bench: the frozen A->B resample step (input samples per output sample). 0 if not frozen.
// The MEAS harness uses it to synthesize an OUTPUT-bin-coherent input tone (Mode K).
float audio_app_asrc_get_freeze_step_ab( void );
// R12 Q12: intentional FEED-FORWARD ratio freeze (NOT Mode-K). Latch the live ratio and make the
// controller use it, while corr_lpf/clamp/slew/applied-step stay live (a->ratio keeps updating).
void    audio_app_asrc_ff_freeze( void );
void    audio_app_asrc_ff_unfreeze( void );
float   audio_app_asrc_get_ff_frozen_ratio_ab( void );
uint8_t audio_app_asrc_get_ff_freeze_ab( void );
// R13 Q13: hold corr_lpf constant (fill-servo integration stopped) while step slew/clamp stay live.
void    audio_app_asrc_corr_hold( void );
void    audio_app_asrc_corr_release( void );
uint8_t audio_app_asrc_get_corr_hold_ab( void );
// R14 Q14: select N=64 moving-average fill vs raw fill for the servo observation path.
void    audio_app_asrc_fill_use_ma( uint8_t on );
uint8_t audio_app_asrc_get_fill_use_ma_ab( void );
void    audio_app_asrc_cfill_en( uint8_t on );      // Q40: continuous-fill estimator toggle (MEAS)
uint8_t audio_app_asrc_get_cfill_en( void );
void    audio_app_asrc_pc_auto( uint8_t on );       // Q41: automatic phase-centering toggle (MEAS)
void    audio_app_asrc_pc_status( void );           // Q41/Q42: print cfill/phase-center/period status
void    audio_app_asrc_hf_en( uint8_t on );         // Q42: measured-producer-period phase toggle (MEAS)
float   audio_app_asrc_get_fill_ma_ab( void );
// R15 Q15: corr_lpf 1 Hz band-split motion selector: 0=FULL,1=SLOW-motion,2=FAST-motion,3=HOLD.
void    audio_app_asrc_corr_mode( uint8_t mode );
uint8_t audio_app_asrc_get_corr_mode_ab( void );
float   audio_app_asrc_get_corr_slow_ab( void );
// R16 Q16: freeze-age instrumentation (t=0 = first pull on the frozen FF; age in control pulls).
uint32_t audio_app_asrc_get_q16_pull_ctr( void );
uint32_t audio_app_asrc_get_q16_freeze_epoch( void );
uint32_t audio_app_asrc_get_q16_age_pulls( void );
float    audio_app_asrc_get_ratio_live_ab( void );
// Bench (R8 Q2): dump the exact 32-bit float bit pattern of every polyphase coefficient
// (whatever storage backs it -- RAM-generated or Flash-resident) + a CRC32, for exact-bit
// extraction and for the Flash self-check. Prints *POLY_BITS_BEGIN .. *POLY_BITS_END.
void audio_app_asrc_dump_poly_bits( void );
// Bench (R8 Q3): perturb the frozen A->B step NON-cumulatively from the base latched at freeze.
// mode: 0=base, 1=ULP (req=signed ULP count), 2=ppm (req=signed ppm, |req|<=1000).
void     audio_app_asrc_set_step_delta( int mode, int32_t req );
float    audio_app_asrc_get_freeze_base_step_ab( void );
int      audio_app_asrc_get_step_delta_mode( void );
int32_t  audio_app_asrc_get_step_delta_req( void );
uint32_t audio_app_asrc_get_fill_ab( void );
#if APP_B_ROUTE_USES_BA
// B->A mirrors of the four A->B fields above, so a MEAS_DIR_BA capture reports the state of the
// engine it actually measured rather than the other leg's (see the definitions). No step-delta
// mirror: *ax 00 perturbs the A->B step only.
float    audio_app_asrc_get_freeze_base_step_ba( void );
float    audio_app_asrc_get_freeze_step_ba( void );
uint32_t audio_app_asrc_get_fill_ba( void );
float    audio_app_asrc_get_ratio_live_ba( void );
float    audio_app_asrc_get_ff_frozen_ratio_ba( void );
#endif
// Q19: live corr_lpf (fill-correction LPF state) and applied step_state, for the synchronized
// freeze-state causal telemetry sideband.
float    audio_app_asrc_get_corr_lpf_ab( void );
float    audio_app_asrc_get_step_state_ab( void );
// Q22/Q23: bench A/B knob -- low-pass the applied step to strip its 8-13 Hz servo motion (Q21
// showed it FM-modulates the carrier) while keeping DC fill authority. Runtime toggle + a
// runtime-selectable beta index {0:0.005,1:0.01,2:0.02,3:0.04} for the corner sweep. Default OFF.
void     audio_app_asrc_step_smooth( uint8_t on, uint8_t beta_idx );
uint8_t  audio_app_asrc_get_step_smooth( void );
float    audio_app_asrc_get_step_smooth_beta( void );
// Q26: ADD a small zero-mean diagnostic sine to the servo error (raw_corr, before the corr LPF) to
// probe the 8-13 Hz generation mechanism. freq_hz in Hz; amp_idx -> {5e-6,1e-5,2e-5,5e-5}. The
// controller (KP/ALPHA/SLEW/clamp/deadband) is unchanged. Logged with applied_step+fill via trace sel=9.
void     audio_app_asrc_inject( uint8_t en, uint8_t freq_hz, uint8_t amp_idx );
uint8_t  audio_app_asrc_get_inject( void );
float    audio_app_asrc_get_inject_amp( void );
// Q30: bench sensitivity screen -- runtime multiplier on the servo gain KP (which=0) or the corr-LPF
// coefficient ALPHA (which=1). code {0:x1, 1:x0.5, 2:x2.0}. Scales the MEAS servo path only (no
// shipping/structure change). corr-LPF time constant ~= 1/ALPHA, so tau x0.5 == ALPHA x2 (code 2).
void     audio_app_asrc_set_servo_mult( uint8_t which, uint8_t code );
float    audio_app_asrc_get_kp_mult( void );
float    audio_app_asrc_get_alpha_mult( void );
// Q50 Fast-Acquisition servo: boot-only ACQUIRE (step-smoothing bypassed, muted audio) -> lock-detect ->
// bumpless HANDOVER (seed TRACK step-smoothing state) -> TRACK (proven steady path, unchanged). Enable
// re-arms on the next ratio (re)lock. lock_pulls / pull_hz give lock time; ho_stepdiff is the seam size.
void     audio_app_asrc_q50_enable( uint8_t on );
uint8_t  audio_app_asrc_q50_get_en( void );
uint8_t  audio_app_asrc_q50_state( void );       // 0=ACQUIRE 1=HANDOVER 2=TRACK
uint32_t audio_app_asrc_q50_lock_pulls( void );  // pulls from ratio-lock to TRACK (0 = not yet locked)
float    audio_app_asrc_q50_pull_hz( void );
float    audio_app_asrc_q50_ho_stepdiff( void ); // applied-step seam at handover (want ~0 = bumpless)
float    audio_app_asrc_q50_strend( void );      // live step-trend (slow-vslow EMA); calibrates STEP_TOL
// Q51 Feed-Forward Rate Seed (layered on Q50 candidate): estimate the true rate from ACQUIRE-window
// frame counts and seed step_state/step_smooth/corr_lpf at handover so TRACK starts at the true step.
void     audio_app_asrc_q51_enable( uint8_t on );
uint8_t  audio_app_asrc_q51_get_en( void );
float    audio_app_asrc_q51_est_step( void );    // last rate estimate = delta_wr / n_out
float    audio_app_asrc_q51_corr_seed( void );   // corr_lpf value seeded at handover
uint8_t  audio_app_asrc_q51_applied( void );     // 1 = last handover seeded (passed sanity), 0 = fell back
// Q52 Differential fill-drift rate seed (replaces Q50/Q51 handover when on): hold the servo over a fixed
// muted window, read the step error from the fill drift, seed step_true + re-centre the FIFO at handover.
void     audio_app_asrc_q52_enable( uint8_t on );
uint8_t  audio_app_asrc_q52_get_en( void );
float    audio_app_asrc_q52_est_step( void );    // step_true = ratio0 + delta_fill / n_out
float    audio_app_asrc_q52_dfill( void );       // observed fill drift over the window (frames)
float    audio_app_asrc_q52_ratio0( void );      // frozen FF ratio held over the window (debug)
float    audio_app_asrc_q52_fill0( void );       // fill at window start (debug)
// Q55 probe: raw producer/consumer counters + elapsed us_x10 (host computes fs_A(t) = delta_wr/delta_t).
uint32_t audio_app_asrc_get_wr( void );
uint32_t audio_app_asrc_get_rd( void );
uint32_t audio_app_asrc_wrt_elapsed_us10( void );
void     audio_app_asrc_q55_prime_en( uint8_t on );   // Q55: silent-startup FIFO held at TARGET
uint8_t  audio_app_asrc_q55_get_prime_en( void );
void     audio_app_asrc_q55log_dump( void );          // dump the early-boot fill/step/ratio log
void     audio_app_asrc_q57_lock_off( int16_t off );  // Q57: ratio-lock fill pre-bias (cancel startup kick)
int16_t  audio_app_asrc_q57_get_lock_off( void );
void     audio_app_asrc_q58_cfg( uint8_t on, float center );  // Q58: block-phase-aware lock offset
uint8_t  audio_app_asrc_q58_get_en( void );
float    audio_app_asrc_q58_last_phase( void );
int16_t  audio_app_asrc_q58_last_off( void );
#endif

#if APP_ASRC_LOAD_TEST
// Bench (load scaling): set/read the interp load multiplier (1..ASRC_LOAD_MULT_MAX). Each
// direction then runs mult*ASRC_CH channels of poly interpolation to project CPU load.
void    audio_app_asrc_set_load_mult( uint8_t mult );
uint8_t audio_app_asrc_get_load_mult( void );
#endif

#if APP_B_ROUTE_USES_BA
// B->A direction (cross): push B's input block into the FIFO / produce A's output.
void audio_app_asrc_set_ratio_ba( float ratio );
void audio_app_asrc_push_ba( const int32_t* src );
void audio_app_asrc_pull_ba( int32_t* dst );
#endif // APP_B_ROUTE_USES_BA

// Can the engines serve this rate PAIR at all?  false = the ring cannot hold the burst
// look-ahead the ratio needs, a shortfall that never resolves with time (see the R(step) law and
// ASRC_BURST_RATIO_LIMIT_* in audio_app_asrc.c).  On false, *reason receives a short static
// string.  A rate of 0 means "not known yet" and is not judged.  ASK BEFORE reconfiguring a leg:
// a rejected pair leaves the running stream untouched, whereas discovering it afterwards leaves
// audibly broken audio that no amount of waiting fixes.
bool audio_app_asrc_rate_pair_is_supported( uint32_t rate_a_hz, uint32_t rate_b_hz,
                                            const char** reason );

// Telemetry line(s) printed with the 2 s TDM report: fill / resample step / peak
// asrc_pull time per direction, plus the caller-measured per-domain rates fsA_hz/fsB_hz.
void audio_app_asrc_dbg_print( uint32_t fsA_hz, uint32_t fsB_hz );

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_CH == 16u) && \
    ((ASRC_POLY_M == 28u) || (ASRC_POLY_M == 30u) || (ASRC_POLY_M == 32u))
// Foreground kernel micro-benchmark ("*az VV", VV = trials): minimum time of one pair8 and one
// pair16 kernel call, i.e. the DSP-load metric that is free of ISR nesting noise. Reads the live
// history read-only; safe while streaming.
void audio_app_asrc_kernel_bench( uint32_t trials );
#endif

#endif // APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC

#endif // AUDIO_APP_ASRC_H
