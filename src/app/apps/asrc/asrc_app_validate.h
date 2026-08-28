#ifndef SONORA_ASRC_APP_VALIDATE_H
#define SONORA_ASRC_APP_VALIDATE_H

/*
 * ASRC-app-private compile-time validation fragment.
 *
 * This header holds the section (3) #error checks that reference ASRC-private
 * configuration symbols (APP_B_ROUTE_IS_ASRC, APP_ASRC_MEAS_UART2_STREAM,
 * APP_MEAS_DIR, APP_ASRC_Q19_EVAL, ...).  It is an INTERNAL FRAGMENT of
 * app_specific_config_defs.h: it is #included ONLY from the end of that header's
 * validation section, and ONLY when SONORA_APP_IS_ASRC, so that the shared/common
 * config header never names an ASRC-private symbol.
 *
 * It deliberately does NOT #include "app_specific_config_defs.h" -- that would
 * create an include cycle.  It relies on being included after all common facts
 * (APP_TDM_USES_SPI34, etc.) and all ASRC-private facts (via asrc_app_config.h)
 * are already defined.  The #error checks are order-independent.
 */

#if APP_TDM_USES_SPI34 && !APP_B_ROUTE_IS_ASRC
  #error "SPI3/SPI4 TDM is currently an ASRC-only experimental transport."
#endif

// UART2 long-stream role is a bench measurement build: it streams the REAL one-way A->B
// resampler output, so it requires an APP_ASRC_MEAS build in the A->B direction (the same
// build that runs B_ROUTE_ASRC_FROM_A and calls audio_app_meas_capture() from the SPI2 ISR).
#if APP_ASRC_MEAS_UART2_STREAM
  #if !APP_ASRC_MEAS
    #error "APP_ASRC_MEAS_UART2_STREAM requires APP_ASRC_MEAS (it streams the measurement A->B output)."
  #endif
  #if APP_MEAS_DIR != MEAS_DIR_AB
    #error "APP_ASRC_MEAS_UART2_STREAM requires APP_MEAS_DIR == MEAS_DIR_AB (one-way A->B; the producer taps the A->B output block)."
  #endif
  #if !APP_B_ROUTE_IS_ASRC
    #error "APP_ASRC_MEAS_UART2_STREAM requires the ASRC route (APP_B_ROUTE_IS_ASRC)."
  #endif
#endif

// Q19 freeze-state causal map = the Q16-profile science build; it only makes sense on top of the
// UART2 stream (the synchronized telemetry sideband rides the same binary stream).
#if APP_ASRC_Q19_EVAL && !APP_ASRC_MEAS_UART2_STREAM
  #error "APP_ASRC_Q19_EVAL requires APP_ASRC_MEAS_UART2_STREAM (the Q16-profile long-stream science build)."
#endif

// --- 96 kHz ASRC constraints ---
// These encode hardware facts, not preferences, so an unsupported combination fails
// at compile time instead of producing a silently wrong image.
#if defined(ENA_96K_RATE)

  // The WM8904 cannot run its ADC and DAC simultaneously at fs >= 88.2 kHz
  // (datasheet boundary; 96 kHz is the only such rate the driver offers, and it is
  // enforced in one place, in wm8904_init_role()). A bidirectional cross-connect
  // needs both codecs capturing AND playing, so it is structurally impossible at
  // this rate: A is ADC-only and B is DAC-only, hence one-way A->B.
  #if APP_ENA_ASRC_BIDIR
    #error "96 kHz ASRC cannot be bidirectional: the WM8904 does not support simultaneous ADC+DAC at or above 88.2 kHz. Use a one-way A->B preset."
  #endif
  #if APP_ENA_ASRC_FROM_B
    #error "96 kHz ASRC is A->B only: leg B is the DAC-only (output) codec, so it cannot be the ASRC source."
  #endif

  // A 96 kHz block is half the duration of a 48 kHz block (16 frames / 96 kHz =
  // 166.7 us vs 333.3 us), so the per-block compute budget halves while per-frame
  // work does not. The shipping 16-channel width does not fit; 8 does.
  #if (ASRC_CH > 8u)
    #error "96 kHz ASRC requires ASRC_CH <= 8: the 166.7 us block window cannot carry the 16-channel width that fits the 48 kHz 333.3 us window."
  #endif

  // The RUNTIME front end is valid at 96 kHz as of 2026-08-02, and this guard used to forbid it.
  // The old reasoning -- "96k->96k is a unity ratio, stay on the direct path" -- was only ever true
  // of the A=B=96 kHz operating point.  Leg B is runtime-variable (`*ar`), and below ~22 kHz the
  // direct step is large enough that the ring cannot hold the look-ahead one pull needs, so the
  // fill setpoint clamps and the block's tail outputs emit zeros: audible break-up.  The runtime
  // gate now answers that with a 96 -> 48 kHz pre-stage plus the existing 48 kHz chain, and selects
  // den 1 at 96k/96k and every rate from 22.05 kHz up -- so the unity path is unchanged where it
  // was the right answer.  See [internal] asrc_96k.md part 3.
  //
  // The two FIXED presets remain incompatible, and for a reason the runtime gate does not share:
  // both hardwire a 48 kHz input leg (den 6 towards 8 kHz, single coefficient set, no rate table),
  // so under ENA_96K_RATE they would decimate a 96 kHz stream with 48 kHz-input coefficients.
  #if APP_ASRC_48K_TO_8_DECIMATOR || APP_ASRC_48K_TO_8_INTEGRATION
    #error "96 kHz ASRC cannot use the FIXED 48->8 front-end presets: they hardwire a 48 kHz input leg. The runtime front end (APP_ASRC_RUNTIME_48K_TO_8) handles 96 kHz via its own 96->48 pre-stage."
  #endif

#endif // defined(ENA_96K_RATE)

#endif /* SONORA_ASRC_APP_VALIDATE_H */
