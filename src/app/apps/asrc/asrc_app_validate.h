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

// The per-stage tick accumulators are placed around the Q31 kernel calls only, so on any other
// polyphase arm they would report 0 for every stage -- and a 0 that means "not instrumented"
// reads exactly like a 0 that means "free". Fail instead of publishing that ambiguity.
#if APP_ASRC_STAGE_PROFILE && (ASRC_POLY_METHOD != ASRC_POLY_Q31)
  #error "APP_ASRC_STAGE_PROFILE instruments the Q31 polyphase arm only (build with ASRC_SAMPLE_Q31=1); on any other arm every stage would report 0, which is indistinguishable from 'free'."
#endif

// --- 96 kHz ASRC constraints ---
// These encode hardware facts, not preferences, so an unsupported combination fails
// at compile time instead of producing a silently wrong image.
// The 96 kHz 12-channel preset is an AK512 configuration.  switch_config offers every
// ASRC profile on any device that has an ASRC configuration, so on AK128 this preset is
// selectable and would fail late, in the linker, on data memory it was never sized for.
// Say so here instead: an internal preset may fail to build, but it should fail with the
// reason.
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K) && (APP_TARGET != APP_TARGET_AK512)
  #error "APP_BUILD_ASRC_CODEC_96K_12CH_32K is an AK512 preset: 12 channels of Q31 history plus two engines do not fit the AK128 data memory. Use APP_BUILD_ASRC_AK128_CODEC_BIDIR on AK128."
#endif

#if defined(ENA_96K_RATE)

  // The WM8904 cannot run its ADC and DAC simultaneously at fs >= 88.2 kHz
  // (datasheet boundary; 96 kHz is the only such rate the driver offers, and it is
  // enforced in one place, in wm8904_init_role()). A bidirectional cross-connect
  // needs both codecs capturing AND playing, so it is structurally impossible at
  // this rate: A is ADC-only and B is DAC-only, hence one-way A->B.
  //
  // APP_ASRC_96K_LOAD_STUDY (bench-only, default 0) lifts this ONE guard, and it does not
  // touch the ADC+DAC fact above.  It cannot: the codec roles come from the nominal RATE in
  // audio_transport.c, so under the study switch leg A is still ADC-only and leg B is still
  // DAC-only.  BIDIR=1 then means "run two ASRC engines", which is a DSP workload question --
  // and the B->A engine resamples an idle capture line, on purpose, at full width.  See the
  // switch's own comment in asrc_app_config.h before reading any number out of such a build.
  // APP_BUILD_ASRC_CODEC_96K_12CH_32K is named here, not lifted by the study switch: it is a
  // selectable preset whose two engines are a DSP-width decision, and the paragraph above
  // already says what that does and does not mean for the analog path.
  #if APP_ENA_ASRC_BIDIR && !APP_ASRC_96K_LOAD_STUDY && \
      (APP_BUILD != APP_BUILD_ASRC_CODEC_96K_12CH_32K)
    #error "96 kHz ASRC cannot be bidirectional: the WM8904 does not support simultaneous ADC+DAC at or above 88.2 kHz. Use a one-way A->B preset. (Bench CPU-load studies of the two-engine workload set APP_ASRC_96K_LOAD_STUDY=1; that does NOT make the audio path bidirectional.)"
  #endif
  #if APP_ENA_ASRC_FROM_B
    #error "96 kHz ASRC is A->B only: leg B is the DAC-only (output) codec, so it cannot be the ASRC source."
  #endif

  // A 96 kHz block is half the duration of a 48 kHz block (16 frames / 96 kHz =
  // 166.7 us vs 333.3 us), so the per-block compute budget halves while per-frame
  // work does not. The shipping 16-channel width does not fit; 8 does.
  //
  // This is the CPU-BUDGET family, so it is the one guard a CPU load study exists to
  // re-open: it states a measured answer, and refusing to compile the measurement that
  // would revise it makes the answer unfalsifiable.  APP_ASRC_96K_LOAD_STUDY=1 therefore
  // lifts it for bench builds only.  Nothing about the ASRC narrows when it is lifted --
  // every channel is still pushed, resampled and stored at the full logical width; only
  // the deadline verdict moves, and the study's job is to measure that verdict.
  //
  // The measurement happened.  APP_BUILD_ASRC_CODEC_96K_12CH_32K carries 12 channels and
  // is exempt by NAME rather than by the study switch, because the sentence below is
  // about a 96 kHz leg B and that preset boots leg B at 32 kHz: the block is 500 us, not
  // 166.7 us, so the budget being asserted here is not the budget it runs against.  On
  // 2026-09-08 it measured Hard RT functional PASS (worst leg B response 494.1 us of
  // 500 us) and FAILED the 5 % engineering reserve, which is why it is an internal
  // preset.  The exemption is deliberately not conditional on the boot rate define:
  // leg B is runtime-variable (`*ar`), so no compile-time test can promise 32 kHz, and
  // a preset that names itself is honest about which configuration was measured.
  #if (ASRC_CH > 8u) && !APP_ASRC_96K_LOAD_STUDY && \
      (APP_BUILD != APP_BUILD_ASRC_CODEC_96K_12CH_32K)
    #error "96 kHz ASRC requires ASRC_CH <= 8: the 166.7 us block window cannot carry the 16-channel width that fits the 48 kHz 333.3 us window. (Bench CPU-load studies that measure a wider width set APP_ASRC_96K_LOAD_STUDY=1.)"
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
