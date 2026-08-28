#ifndef SONORA_ASRC_APP_CONFIG_H
#define SONORA_ASRC_APP_CONFIG_H

// --- SPI2 INDEPENDENT CLOCK DOMAIN (Phase 1 experiment) ---
//   1 = decouple SPI2/WM8904-B from SPI1's clock and run it as an INDEPENDENT dsPIC
//       TDM master (FRC->PLL1->CLKGEN9 derived), with WM8904-B as its clock slave.
//       A-domain (SPI1) stays codec-A master / dsPIC slave (APP_USE_SPI_TDM_CLK_MASTER 0).
//       Result: A and B are two truly-async, fully independent in/out streams
//       (audio into A does NOT come out of B -- the co-clock A=B mirror is removed).
//   0 = legacy co-clock topology (SPI2 follows SPI1's BCLK/FS via CLC fan-out).
//   NOTE: transport groundwork for the A<->B ASRC (Phase 2+).
//
// APP_REQ_SPI2_INDEPENDENT_MASTER (the DERIVED request, from APP_PROFILE + APP_ASRC_CLOCK_OWNER)
// is now defined in the ASRC build-config (asrc_app_build_config.h), with a neutral 0 stub in the
// Classic build-config, so the shared config header does not reach into this ASRC-private header
// for it. The effective APP_USE_SPI2_INDEPENDENT_MASTER is still resolved in the shared Resolved
// section and is only 1 when the SPI2 audio path exists (AK512 dual-codec).

// SPI2 master BCLK divider (SPIxBRG). BCLK = PLL1_CLK_HZ / (2*(BRG+1)) = 100MHz/(BRG+1);
// fs = BCLK / (SLOTS_PER_FS*32). BRG=8 -> BCLK=11.111MHz -> TDM8/32bit fs=43.403kHz
// (the 44.1kHz-leaning experiment value; exact 44.1k is unreachable from 200MHz -- see doc).
// Chosen EXPLICITLY (not derived from a target rate) to avoid the get_default_config()
// float-rounding that would land on BRG=7. FS uses a short pulse (FS_PULSE) initially to
// keep CLC10 (the 50%-duty master-FS resource) free during bring-up.
#define APP_SPI2_MASTER_BRG             (8)

// B-domain processing ROUTE (Phase 1.5 scaffold). Selects app_block_cb_leg_b()'s path,
// mirroring the A-side local_*_path dispatch in audio_app_process_audio_block(). Existing
// A-side routes (DRC / filter-cascade / raw-bypass) are untouched. Tokens are OPAQUE
// distinct values (compare with == only). Only meaningful when the SPI2 independent
// domain is active (otherwise SPI2 has no block callback).
#define B_ROUTE_LOOPBACK      (1)   // WM8904-B self-loopback (B-in -> B-out), unity. Phase 1.
#define B_ROUTE_ASRC_FROM_A   (2)   // A-in -> resample -> B-out. A out = silence; LED = ASRC out (B). Phase 2.
#define B_ROUTE_ASRC_BIDIR    (3)   // A<->B cross: A-in->B-out AND B-in->A-out (two ASRCs). Phase 2+.
#define B_ROUTE_ASRC_LIGHT    (4)   // A->B ASRC LOAD TEST: A's heavy DSP (filter-cascade/DRC) is
                                    // stopped; A keeps only gain + LED level-meter + Flip4 keepalive
                                    // so the ASRC's own load stands out. B out = pure asrc_pull.
#define B_ROUTE_ASRC_FROM_B   (5)   // B-in -> resample -> A-out (mirror of FROM_A). B out = silence,
                                    // A out = resampled B (uses the B->A instance). LED = ASRC out.
// --- ASRC quality MEASUREMENT harness (Theme 1: quantify cubic vs poly) ---
//   0 = normal audio build. 1 = bench measurement build: the A->B input is replaced by an
//   on-chip pure sine, and the (digital) A->B output is captured to RAM for offline FFT
//   (dumped over the console). Capturing the DIGITAL resampler output isolates the kernel's
//   image/alias/SNR from the codec DAC/ADC floor (~-90 dB) that would otherwise mask it.
//   The measurement build forces one-way A->B (B_ROUTE_ASRC_FROM_A) so the B->A instance is
//   not allocated -- that frees the ~4 KB the 8 KB capture buffer needs on the 64 KB SRAM.
//   Drive it from the console: *nt20 arm, *nt21 dump, *nt22 tone=low, *nt23 tone=high.
#ifndef APP_ASRC_MEAS
#define APP_ASRC_MEAS         (0)      // base default; set by APP_BUILD (ASRC_CODEC_MEAS)
#endif

// Fixed 48 kHz -> 8 kHz anti-alias decimator.  Keep disabled in every existing
// preset so the shipping ASRC code/data path is unchanged.  A dedicated
// measurement/integration preset may override this before including this file.
#ifndef APP_ASRC_48K_TO_8_DECIMATOR
#define APP_ASRC_48K_TO_8_DECIMATOR (0)
#endif

// Connect decimator output frames to the existing A->B ASRC FIFO.  This setting
// assumes that the B transport is independently configured for approximately
// 8 kHz; it never changes transport clocks itself.
#ifndef APP_ASRC_48K_TO_8_INTEGRATION
#define APP_ASRC_48K_TO_8_INTEGRATION (0)
#endif

// Boot bit-exactness selftest of the front ends compiled into this build (the
// optimized mirrored-window path against the reference modulo-ring oracle kept
// in the same file).  It is a startup check, not an audio-path feature, and its
// oracle plus six per-chain drivers cost about 2.9 kB of program memory -- so a
// ROM-bound profile may drop it and rely on the check running in a roomier build
// of the SAME sources (the AK512 ASRC image runs it at every boot).  Only ever
// set 0 where the program region is the binding constraint, and say so there.
#ifndef APP_ASRC_FRONTEND_SELFTEST
#define APP_ASRC_FRONTEND_SELFTEST (1)
#endif

// Runtime-selectable low-rate fixed front ends.  The standard codec BIDIR
// preset enables 48->8 (/6) at B=8 kHz and 48->16 (/3) at B=11.025 kHz;
// other presets keep the historical compile-time behavior.
#ifndef APP_ASRC_RUNTIME_48K_TO_8
#define APP_ASRC_RUNTIME_48K_TO_8 (0)
#endif

// Q50 Fast-Acquisition servo (MEAS-only): 1 = the ACQUIRE->HANDOVER->TRACK machine is ENABLED from the
// very first boot ratio-lock (the "candidate" cold-boot build). 0 = disabled at boot (the "baseline"
// cold-boot build: the proven steady servo runs from t=0). Runtime *nt39 can still flip it for a warm
// re-arm via *nt3B (servo restart). No effect unless APP_ASRC_MEAS; shipping (MEAS=0) is untouched.
#define APP_Q50_DEFAULT_ON    (0)

// Q51 Feed-Forward Rate Seed (MEAS-only, layered on Q50 candidate): during the muted ACQUIRE window,
// estimate the true rate ratio from the producer/consumer frame counts (delta_wr / n_out, ~11 ppm at
// ~1.9 s vs the CCP feed-forward's ~130 ppm residual bias), and at HANDOVER seed the servo state
// (step_state, step_smooth_st, corr_lpf) at that estimate so TRACK starts near the true step instead of
// integrating it over ~10 s. Seeding happens under mute, so the one-pull step jump is inaudible. 1 =
// seed at boot; runtime *nt3C toggles it. No effect unless APP_ASRC_MEAS && Q50 actuates; TRACK KP/
// corr-LPF/clamp/slew and the shipping path are all unchanged.
#define APP_Q51_DEFAULT_ON    (0)

// Q52 Differential fill-drift rate seed (MEAS-only, replaces Q50/Q51 handover when on). During a FIXED
// muted ACQUIRE window the servo is HELD (ff_freeze + corr_hold -> step == frozen FF ratio, constant),
// so fill drifts freely under the FF-ratio error. The step error is read directly from the drift:
//   Delta_fill = n_out * (step_true - ratio)  ->  step_true = ratio + Delta_fill / n_out.
// This is a DIFFERENTIAL measure: the correction term Delta_fill/n_out is tiny (~130 ppm), so a ~351 ppm
// error in n_out affects it by only ~0.05 ppm -> IMMUNE to the fs_B / frame-count bias that capped Q51
// at ~180 ppm. At handover (under mute) it seeds step_state/step_smooth_st/corr_lpf at step_true AND
// re-centres the FIFO (rd = wr - TARGET), so TRACK starts fill-centred at the true step -> locks almost
// immediately. Quality-lock ~= the window length. 1 = seed at boot; runtime *nt3D. TRACK KP/corr-LPF/
// clamp/slew and shipping are unchanged.
#define APP_Q52_DEFAULT_ON    (0)

// Q57 experiment: ratio-lock fill pre-bias (fill_start = TARGET - off) to cancel the startup block-phase
// kick that the servo otherwise rings out over ~10 s. MEAS-only experiment; 0 = shipping behaviour.
#define APP_Q57_LOCK_OFF      (28)

// Q58: block-phase-aware ratio-lock offset (off = center - intra_block_phase) to remove Q57's per-boot
// residual dips. MEAS-only experiment. APP_Q58_DEFAULT_ON=1 enables it from the first cold-boot lock.
// center = mean phase of the producer sawtooth = APP_BLOCK_FRAMES/2 (structural, not a magic 16); tracks
// the block length automatically. BLOCK/4 is a faster low-latency candidate (locks ~3 s, deeper dips).
#define APP_Q58_DEFAULT_ON    (0)
#define APP_Q58_CENTER        (APP_BLOCK_FRAMES / 2)

// SHIPPING fast-acquire: at ratio-lock, pre-bias the FIFO by a FIXED offset to cancel the producer
// block-phase startup kick, cutting the audible warm-up from ~14 s to ~4 s with steady THD+N/DR
// unchanged. 1 = enabled in the shipping path (APP_ASRC_MEAS=0 too); 0 = legacy ~14 s warm-up. Under
// APP_ASRC_MEAS the *nt41/*nt42 runtime knobs override this default.
#define APP_ASRC_FAST_ACQUIRE (1)
// The fixed pre-bias (frames). It equals the startup fill-sawtooth mean overshoot, which is a
// DETERMINISTIC property of this build's per-block timing (measured via the *nt40 early-boot logger:
// natural startup fill mean ~= TARGET + this). 28 is calibrated for the ASRC profile (A-side demo DSP
// absent) on the 48k->43.4k path. RE-MEASURE with *nt40 if the per-block processing load changes
// materially (the demo-present build needed ~12). See docs asrc section 14.
#ifndef APP_ASRC_FAST_ACQUIRE_OFFSET
#define APP_ASRC_FAST_ACQUIRE_OFFSET (28)
#endif

// --- Long-coherent binary stream of the real A->B ASRC output over UART2 -> PKOB4 (Q19 base) ---
//   NARROW measurement-only role gate. 0 = current UART2 behavior (2nd command console + mirror).
//   1 = UART2 becomes a DEDICATED, TX-only binary DATA port at APP_ASRC_STREAM_BAUD: the console
//   mirror is suppressed and UART2 RX command input is dropped, so UART2 carries binary stream
//   frames ONLY. All control/text (STREAM_ARM/BEGIN/END, ACK) stays on UART1. This lets a long
//   coherent capture of the *real* resampler output be streamed off-chip (host verifies zero-loss)
//   while the ASRC + FF tick keep running un-instrumented. Requires an APP_ASRC_MEAS one-way A->B
//   build (guard in 2.x). Default 0 keeps the normal build byte-for-byte unchanged.
#define APP_ASRC_MEAS_UART2_STREAM  (0)
//   UART2 data-port baud in the stream build. 2.0 Mbaud = exact 200 MHz / 100 divisor (0.000 %
//   error) and sits inside the proven PKOB4 zero-loss envelope (~195 kB/s) with margin over the
//   135.63 kB/s the real stream needs. Do NOT raise it to chase the transport ceiling.
#define APP_ASRC_STREAM_BAUD        (2000000UL)

// --- Q19 freeze-state causal-map evaluation profile (narrow, opt-in) ---
//   0 = canonical dca5eab kernel defaults (FIFO512 / poly L64 / Blackman / CAP2048).
//   1 = recreate the historical Q16 HIGH-QUALITY evaluation profile for the freeze-age causal map:
//       FIFO256, polyphase L=128, Blackman-Harris prototype window, capture length 2016. This is the
//       exact science point Q16 used (buildtools/q16_run.ps1: "L128/BH/FIFO256/MEAS/BRG8/CAP2016").
//       Requires the UART2 stream build (APP_ASRC_MEAS_UART2_STREAM). Default 0 keeps BOTH the normal
//       build and the plain stream build byte-for-byte unchanged -- the Q16 profile is opt-in only.
#define APP_ASRC_Q19_EVAL           (0)

#define ASRC_WINDOW_BLACKMAN        (0)
#define ASRC_WINDOW_BLACKMAN_HARRIS (1)
#define ASRC_WINDOW_KAISER_11       (2)

#if APP_ASRC_Q19_EVAL
// Kernel-geometry overrides consumed in audio_app_asrc.c (both are #ifndef-guarded there). The
// ASRC_WINDOW_* names are defined just above, so the window is selected by name here.
#ifndef ASRC_POLY_L
#define ASRC_POLY_L                 (128u)
#endif
#ifndef ASRC_POLY_WINDOW
#define ASRC_POLY_WINDOW            (ASRC_WINDOW_BLACKMAN_HARRIS)
#endif
#endif

// Q43/Q45 (SHIPPING DEFAULT): high-fidelity kernel -- L128 phases + Blackman-Harris prototype. Q43
// showed the THD+N floor is the interpolation kernel (poly L), not the servo: L128 drops the frozen
// kernel from -106.5 dB (L64) to -127.7 dB (+21 dB). Kept in the STANDARD build (FIFO512 / CAP2048 /
// the *nt20-21 audio-capture path); NOT the full Q19 eval profile. Coeffs generated in RAM at boot
// (s_poly[129][32] ~16.5 KB); flip ASRC_COEFF_STORAGE to FLASH in asrc.c to reclaim the RAM. Set this
// to 0 to fall back to the lean L64 kernel.
#ifndef APP_ASRC_HIFI_KERNEL
#define APP_ASRC_HIFI_KERNEL        (1)
#endif
#if APP_ASRC_HIFI_KERNEL && !APP_ASRC_Q19_EVAL
#ifndef ASRC_POLY_L
#define ASRC_POLY_L                 (128u)
#endif
#ifndef ASRC_POLY_WINDOW
#define ASRC_POLY_WINDOW            (ASRC_WINDOW_BLACKMAN_HARRIS)
#endif
#endif

/* FIR geometry is override-friendly so named build presets can select a
 * candidate without source editing.  M must remain even for the centred read
 * window and the pair kernels. */
#ifndef ASRC_POLY_M
#define ASRC_POLY_M                 (32u)
#endif
#ifndef ASRC_POLY_KAISER_BETA
#define ASRC_POLY_KAISER_BETA       (11.0f)
#endif
#ifndef ASRC_POLY_KAISER_NAME
#define ASRC_POLY_KAISER_NAME       "poly-k11"
#endif
#ifndef ASRC_POLY_FC
#define ASRC_POLY_FC                (0.45f)
#endif
#ifndef ASRC_POLY_L
#define ASRC_POLY_L                 (64u)
#endif
#ifndef ASRC_POLY_WINDOW
#define ASRC_POLY_WINDOW            (ASRC_WINDOW_BLACKMAN)
#endif
#define ASRC_POLY_MH                ((ASRC_POLY_M / 2u) - 1u)
#define ASRC_POLY_AHEAD             (ASRC_POLY_M - 1u - ASRC_POLY_MH)

#ifndef APP_ASRC_HEADROOM_INSTRUMENT
#define APP_ASRC_HEADROOM_INSTRUMENT (0)
#endif
#ifndef APP_ASRC_LED_FRAME_STRIDE
#define APP_ASRC_LED_FRAME_STRIDE    (1u)
#endif
#ifndef APP_ASRC_HEADROOM_DBG_PERIOD_MS
#define APP_ASRC_HEADROOM_DBG_PERIOD_MS (APP_DBG_PERIOD_MS)
#endif
/*
 * Rate-monotonic RX-ISR leg priorities: 1 = the shorter-deadline (higher-rate) leg preempts
 * the other, 0 = both legs stay on the base priority (the pre-2026-08-25 symmetric behaviour).
 *
 * It lives HERE, in the app config every ASRC translation unit includes, and not next to the
 * code that reads it. It was defined inside asrc_audio_path.c's low-rate front-end guard until
 * 2026-08-27, so any profile without a front end -- the AK128 bi-codec image -- left it
 * UNDEFINED. `#if <undefined>` is a silent 0 in C, so RM was compiled out of that image with no
 * diagnostic at all: asrc_audio_path_apply_isr_priorities() linked as a 4-byte empty function
 * and mixed-rate pairs ran with symmetric priorities while every comment claimed otherwise.
 * The #error below is the re-defense: a build that cannot see this header fails loudly instead
 * of quietly reverting to 0.
 */
#ifndef APP_ASRC_RATE_MONOTONIC_ISR
#define APP_ASRC_RATE_MONOTONIC_ISR     (1)
#endif
#if (ASRC_POLY_M & 1u) || (ASRC_POLY_M < 4u)
#error "ASRC_POLY_M must be an even tap count of at least 4"
#endif
#if (APP_ASRC_LED_FRAME_STRIDE < 1u)
#error "APP_ASRC_LED_FRAME_STRIDE must be at least 1"
#endif
#if (APP_ASRC_HEADROOM_DBG_PERIOD_MS < 1u)
#error "APP_ASRC_HEADROOM_DBG_PERIOD_MS must be at least 1 ms"
#endif

// Measurement direction (APP_ASRC_MEAS only): which resampler to characterise.
//   AB : A-in sine -> A->B resample (downsample) -> capture B output.  (one-way, 4 KB + cap)
//   BA : B-in sine -> B->A resample (UPsample)   -> capture A output.  (needs the B->A instance)
#define MEAS_DIR_AB           (0)
#define MEAS_DIR_BA           (1)
// Override-friendly (like ASRC_CH above) so a named build preset can select the upsampling
// direction without a source edit.  BA costs the B->A instance's RAM, which is why AB is the
// default: it frees that for the capture buffer.
#ifndef APP_MEAS_DIR
#define APP_MEAS_DIR          (MEAS_DIR_AB)
#endif

#if APP_ASRC_MEAS
#if APP_MEAS_DIR == MEAS_DIR_BA
#define APP_B_ROUTE           (B_ROUTE_ASRC_BIDIR)    // need the B->A instance (A->B runs but ignored)
#else
#define APP_B_ROUTE           (B_ROUTE_ASRC_FROM_A)   // one-way A->B; frees B->A RAM for capture
#endif
#else
// (Part 1) Product ASRC direction. APP_ENA_ASRC_BIDIR=1 => bidirectional A<->B cross-connect
// (A out = B-in resampled to fsA, B out = A-in resampled to fsB, two ASRC instances); =0 => one-way
// A->B. BIDIR adds one asrc_t (~4.3 KB) and only takes effect when the ASRC engine is on
// (APP_B_INDEP_DOMAIN, i.e. the codec-master / independent build) -- inert in the classic co-clock demo.
#ifndef APP_ENA_ASRC_BIDIR
#define APP_ENA_ASRC_BIDIR    (1)
#endif
// One-way direction selector, used ONLY when APP_ENA_ASRC_BIDIR=0:
//   0 = A->B (FROM_A: A input resampled to B, A out silent),
//   1 = B->A (FROM_B: B input resampled to A, B out silent).
#ifndef APP_ENA_ASRC_FROM_B
#define APP_ENA_ASRC_FROM_B   (0)
#endif
// LIGHT load-test route selector (set by APP_BUILD=ASRC_DSPIC_LIGHT). When 1, the route is
// B_ROUTE_ASRC_LIGHT regardless of FROM_B; BIDIR must be 0 (the preset clears it).
#ifndef APP_ENA_ASRC_LIGHT
#define APP_ENA_ASRC_LIGHT    (0)
#endif
#if APP_ENA_ASRC_BIDIR
#define APP_B_ROUTE           (B_ROUTE_ASRC_BIDIR)    // A<->B both directions
#elif APP_ENA_ASRC_LIGHT
#define APP_B_ROUTE           (B_ROUTE_ASRC_LIGHT)    // A->B ASRC load test (heavy A-DSP stopped)
#elif APP_ENA_ASRC_FROM_B
#define APP_B_ROUTE           (B_ROUTE_ASRC_FROM_B)   // one-way B->A (B silent, A = resampled B)
#else
#define APP_B_ROUTE           (B_ROUTE_ASRC_FROM_A)   // one-way A->B (A silent, B = resampled A)
#endif
#endif                                                // (B_ROUTE_ASRC_LIGHT = a load-test route)
// True when the selected route uses the ASRC engine (any direction / variant).
#define APP_B_ROUTE_IS_ASRC   ( (APP_B_ROUTE == B_ROUTE_ASRC_FROM_A) || \
                                (APP_B_ROUTE == B_ROUTE_ASRC_FROM_B) || \
                                (APP_B_ROUTE == B_ROUTE_ASRC_BIDIR)  || \
                                (APP_B_ROUTE == B_ROUTE_ASRC_LIGHT) )
// True when the route needs the B->A instance (the cross direction): FROM_B + BIDIR + LIGHT.
// (FROM_B is one-way B->A so it uses ONLY ba; the always-allocated ab instance is unused there.)
#define APP_B_ROUTE_USES_BA   ( (APP_B_ROUTE == B_ROUTE_ASRC_FROM_B) || \
                                (APP_B_ROUTE == B_ROUTE_ASRC_BIDIR)  || \
                                (APP_B_ROUTE == B_ROUTE_ASRC_LIGHT) )


// --- CCP FS-DETECT (Theme 2 step: precise clock detection via CCP Input Capture) ---
//   1 = measure codec-A's FS (LRCLK, RP70) with CCP1 Input Capture (32-bit time base) and
//       print a quantization-free fs_A next to the block-count telemetry. Groundwork for the
//       feed-forward rate estimator. Only meaningful in the SPI2 independent-master build.
#ifndef APP_USE_CCP_FS_DETECT
#define APP_USE_CCP_FS_DETECT   (1)
#endif

// Feed-forward update period (ms): how often the measured A:B ratio (from CCP) is pushed
// into the ASRC step. 20 ms is a modest cadence; raise responsiveness by lowering it if a fast
// output-clock sweep needs tighter tracking.
#define APP_ASRC_FF_PERIOD_MS   (20)

// --- ASRC interpolation kernel (Theme 1: quality) ---
//   CUBIC : 4-point Catmull-Rom (light, ~60-70 dB image/alias). The baseline.
//   POLY  : windowed-sinc polyphase FIR (32 taps x 64 phases + inter-phase linear interp),
//           anti-aliased for the downsample direction. Higher quality, more CPU. Keep both
//           so cubic vs poly can be A/B-compared on the same setup.
#define ASRC_INTERP_CUBIC   (1)
#define ASRC_INTERP_POLY    (2)
#ifndef APP_ASRC_INTERP
#define APP_ASRC_INTERP     (ASRC_INTERP_POLY)
#endif

// --- ASRC channel count (per direction) ---
//   Channels the resampler processes per direction. 2 = codec L/R. >2 replicates L/R into the
//   extra channels (the codec is stereo) to give a REPRESENTATIVE multi-channel workload, so
//   dot/coefficient optimizations are evaluated against the real TDM16 target instead of an
//   over-fit 2ch pattern. RAM scales linearly (~ ASRC_CH * (FIFO+M) * 4 B * 2 directions).
// Override-friendly (like the FIR geometry below) so a named build preset can select a
// narrower width without source editing. The 96 kHz preset uses 8: its block window is
// half the 48 kHz one, so the shipping 16-channel width does not fit.
#ifndef ASRC_CH
#define ASRC_CH             (16u)
#endif

// ASRC ring depth (frames, power of 2). Default 512. Shrink (e.g. 256) for high channel counts
// to fit RAM -- halves latency + fill target; the control loop is proportional so it still holds.
#ifndef APP_ASRC_FIFO_FRAMES
#if APP_ASRC_Q19_EVAL
#define APP_ASRC_FIFO_FRAMES (256u)   // Q16 eval profile (FIFO256)
#else
#define APP_ASRC_FIFO_FRAMES (128u)   // RAM-savings research: 128-frame ring (power of 2); was 512.
                                       // Fill target defaults to FIFO/2=64 (center); ~20-frame servo
                                       // margin each side. Define APP_ASRC_FILL_TARGET for an
                                       // asymmetric/lower setpoint (Step 3 experiments).
#endif
#endif

/* 0 keeps the historic multi-channel workload: physical codec L/R is
 * replicated across the ASRC width.  The explicit AK128 TDM8 profile sets 1
 * so one physical slot maps to one ASRC channel. */
#ifndef APP_ASRC_TDM8_ONE_TO_ONE
#define APP_ASRC_TDM8_ONE_TO_ONE (0)
#endif

// --- ASRC poly dot method (load-reduction experiment) ---
//   DUAL : fused dual-dot per channel (2 sub-filter dots = 64 MAC/ch) + per-frame phase share.
//   CEFF : precompute the phase-blended coefficient c_eff once/frame (shared), then ONE dot per
//          channel (32 MAC/ch). Trades a 32-lerp prep/frame for half the per-channel MAC --
//          expected to win as channel count grows. Compare on HW at 4ch.
#define ASRC_POLY_DUAL      (1)
#define ASRC_POLY_CEFF      (2)
#define ASRC_POLY_DUAL2X    (3)   // V3: coeff-shared 2-channel fused dual-dot (load-reduction; ASRC_CH even)
#define ASRC_POLY_DUAL4X    (4)   // B0/wide4: coeff-shared 4-channel fused dual-dot (ASRC_CH mult of 4)
#define ASRC_POLY_STREAM8   (5)   // STREAM-CEFF: coeff-blended 8-channel single-acc dot (half MACs; ASRC_CH mult of 8; Class B -> SFDR)
#define ASRC_POLY_DUAL8X    (6)   // bit-exact 8-channel dual-dot; experimental wide8 (ASRC_CH mult of 8)
#define ASRC_POLY_STREAM8_SINGLE (7) // nearest stored phase; no per-tap coeff blend (ASRC_CH mult of 8)
#define ASRC_POLY_STREAM8_PAIR   (8) // two-output fused STREAM8; shares overlapping M+1 input frames
#define ASRC_POLY_Q31            (9) // Q31 fixed-point: one hoisted blended row/frame, then one
                                     // 30-tap Q31 dot per channel. Requires ASRC_SAMPLE_Q31 == 1.
/*
 * OVERRIDABLE so the blend path can be bisected from a build.  Every rate that sounds clean on
 * AK128 (48->48, 48->32, 48->24, 44.1->44.1) has an inter-phase blend weight of exactly zero --
 * step 1.0/1.5/2.0 land on stored phase rows -- so the per-tap coefficient blend never runs there
 * and a defect in it cannot be heard.  48->44.1 (160/147) is the only tested ratio with wb != 0,
 * and it is the only one that crackles.  Building with ASRC_POLY_STREAM8_SINGLE removes the blend
 * at every ratio (nearest stored phase), which decides whether the blend is at fault:
 *   -Define ASRC_POLY_METHOD=ASRC_POLY_STREAM8_SINGLE
 * That variant is a DIAGNOSTIC, not a candidate image -- nearest-phase selection is the stored-phase
 * performance ceiling experiment, and it costs image rejection.  See
 * [internal] report_ak128_crackle_and_fifo128_2026-08-20.md.
 */
#ifndef ASRC_POLY_METHOD
#define ASRC_POLY_METHOD    (ASRC_POLY_STREAM8_PAIR)
#endif

/*
 * ASRC sample representation.  0 = the shipping float32 path.  1 = Q31 fixed point:
 * the history ring, the coefficient table and the accumulator all become integer,
 * and the TDM slot word (s24 left-justified, i.e. already Q31) is carried through
 * unconverted.  Q31 selects its own dot method, so ASRC_POLY_METHOD is forced to
 * ASRC_POLY_Q31 below rather than being a second independent axis: the seven float
 * interpolation variants above are experiments on the float kernel and have no Q31
 * counterpart.  See asrc_poly_q31.inc for the Q-format design and its bounds.
 */
#ifndef ASRC_SAMPLE_Q31
#define ASRC_SAMPLE_Q31     (0)
#endif

#if ASRC_SAMPLE_Q31
  #undef  ASRC_POLY_METHOD
  #define ASRC_POLY_METHOD  (ASRC_POLY_Q31)
#elif (ASRC_POLY_METHOD == ASRC_POLY_Q31)
  #error "ASRC_POLY_METHOD == ASRC_POLY_Q31 requires ASRC_SAMPLE_Q31 == 1"
#endif

/* The Q31 correctness proof (coefficient digest, the 110 generated vectors, the
 * assembly-vs-C comparison and the 16-channel independence run).  Costs ~700 B of
 * stack while it runs and nothing afterwards; it runs once out of
 * audio_app_asrc_reset_all(), before asrc_reset() clears the scratch it borrows. */
#ifndef APP_ASRC_Q31_SELFTEST
#define APP_ASRC_Q31_SELFTEST (ASRC_SAMPLE_Q31)
#endif

// 1 = clamp then truncate float->24-bit sample (matches convert_codec_float_to_int fast path).
// 0 = lrintf round-to-nearest reference. Difference is less than one 24-bit LSB.
#define ASRC_FAST_SLOT_CONVERT (1)

// 2x2 kernel variant (only when ASRC_POLY_METHOD==DUAL2X): V3 = original; SCHED_V1 = A0
// load-scheduling experiment (loads-first-then-MACs, bit-equivalent, same instruction count) --
// tests for a load->MAC hidden interlock stall on this FPU.
#define ASRC_2X2_V3         (1)
#define ASRC_2X2_SCHED_V1   (2)
#define ASRC_2X2_SCHED_V2   (3)   // A0 schedule + 4-tap unroll (DTB/4; ASRC_POLY_M mult of 4)
#define ASRC_2X2_SCHED_V3   (4)   // A0 schedule + 8-tap unroll (DTB/8; ASRC_POLY_M mult of 8)
#define ASRC_2X2_KERNEL     (ASRC_2X2_SCHED_V2)

// STREAM8 kernel variant (only when ASRC_POLY_METHOD==STREAM8): BASE = ce built then 8 MACs
// (ce serial-chain in front of the MACs); P1 = software-pipelined (tap k+1 ce built during
// tap k MACs to hide the chain). Same math/result (Class B) -- P1 is a pure schedule change.
#define ASRC_STREAM8_BASE   (1)
#define ASRC_STREAM8_P1     (2)
#define ASRC_STREAM8_KERNEL (ASRC_STREAM8_BASE)

// ASRC history layout experiment. CH_MAJOR is the established [channel][frame] ring.
// TILE8 stores each 8-channel group as [tile][frame][lane] so STREAM8 consumes one
// contiguous 8-float vector per tap. TILE8 currently supports STREAM8 BASE only.
#define ASRC_HISTORY_CH_MAJOR (0)
#define ASRC_HISTORY_TILE8    (1)
#define ASRC_HISTORY_LAYOUT   (ASRC_HISTORY_CH_MAJOR)

/* Placed HERE, not next to ASRC_SAMPLE_Q31: an #if above this line would compare
 * two undefined names as 0 != 0 and pass silently. */
#if ASRC_SAMPLE_Q31 && (ASRC_HISTORY_LAYOUT != ASRC_HISTORY_CH_MAJOR)
  #error "ASRC_SAMPLE_Q31 requires the CH_MAJOR history ring (TILE8 is float-only)"
#endif

// Dual-dot kernel version (only when ASRC_POLY_METHOD==DUAL): V1 = plain (1 tap/iter),
// V2A = 2-tap unrolled (halved DTB/loop overhead, same 2 accumulators, bit-equivalent math).
#define ASRC_DUAL_V1        (1)
#define ASRC_DUAL_V2A       (2)
#define ASRC_DUAL_V2B       (3)   // V2a + even/odd 4-accumulator split (diagnostic; changes add order)
#define ASRC_DUAL_KERNEL    (ASRC_DUAL_V2A)

// --- ASRC load-scaling test (bench): CPU headroom / multi-channel projection ---
//   1 = compile in a per-block channel-load MULTIPLIER: the selected poly output path is run N
//       times per output frame over the existing ASRC_CH FIFOs (extra results discarded) to
//       emulate the compute load of N*ASRC_CH channels without allocating more FIFO RAM. Both
//       directions pick it up; with ASRC_CH=8, mult=1 is real 8ch and mult=2 models 16ch. Set
//       the multiplier at runtime with *nt26 (1..ASRC_LOAD_MULT_MAX) and watch TDM load / miss;
//       find where miss first appears. Default multiplier 1 = no extra work. Zero cost at 0.
#define APP_ASRC_LOAD_TEST      (0)
#define ASRC_LOAD_MULT_MAX      (8u)

// XC-DSC standard ramfunc smoke test. When enabled, main calls a tiny C function once after
// console initialization and prints its result. This is a placement/startup-copy probe only;
// keep it off for all ASRC performance measurements.
#ifndef APP_RAMFUNC_C_PROBE
#define APP_RAMFUNC_C_PROBE     (0)
#endif

// Execute the bit-identical SCHED_V2 assembly kernel from XC-DSC's standard
// .ramfunc section. Keep off by default so the normal build uses the original Flash kernel.
#ifndef APP_ASRC_KERNEL_RAMTEST
#define APP_ASRC_KERNEL_RAMTEST (0)
#endif

// Measurement-harness parameters (only used when APP_ASRC_MEAS). The A input tone is
// generated at the A-domain rate (~48 kHz); the output is captured at the B-domain rate.
#define APP_MEAS_TONE_LOW_HZ    (1000.0f)    // *nt22: mid tone for THD / SNR
#define APP_MEAS_TONE_HIGH_HZ   (18000.0f)   // *nt23: near band edge -> exposes image / alias
#define APP_MEAS_TONE_DBFS      (-1.0f)      // tone level below 24-bit full scale
#define APP_MEAS_FS_A_HZ        (48000.0f)   // nominal A rate for the sine phase step (exact rate is
                                             // irrelevant to the FFT; the tone shows where it lands)
// OVERRIDABLE: an 8 KB capture buffer is an AK512 figure.  A 16 KB part sizes this from
// what is left after the resampler, so the guard is what lets a build config -- or a
// one-shot -Define -- pick a length that fits (see the AK128 MEAS block in
// asrc_app_build_config.h).
#ifndef APP_MEAS_CAP_LEN
#if APP_ASRC_Q19_EVAL
#define APP_MEAS_CAP_LEN        (2016u)      // Q16 eval profile (CAP2016)
#else
#define APP_MEAS_CAP_LEN        (2048u)      // captured output samples (int32) -> 8 KB buffer.
#endif
#endif

// OVERRIDABLE: the *ag / ?ag control-variable (servo) trace.  1 = present (default -- every AK512
// MEAS preset has always had it).  0 = omitted, which drops the Q34 buffer (up to 16 KB of .bss)
// and the two printf-heavy trace bodies (~4.4 KB of program memory) while keeping the exported
// symbols as stubs.  Independent of the *ac / ?ac audio capture that DR and THD+N use.
#ifndef APP_MEAS_CTRL_TRACE
#define APP_MEAS_CTRL_TRACE     (1)
#endif
                                             // 64 KB RAM is mostly the base app (~47 KB) + ASRC
                                             // FIFOs, so ~2-3 K samples is the max static buffer;
                                             // a longer record needs incremental UART streaming
                                             // (future). Kaiser b=24 window => no coherent capture.
#define APP_MEAS_DISCARD_FRAMES (4096u)      // output frames skipped after arm before capture starts
                                             // (spec 8.3: drop the capture start-transient / FIFO
                                             //  settling that showed up as a ~-86 dBc bin-1 artifact)

#endif /* SONORA_ASRC_APP_CONFIG_H */
