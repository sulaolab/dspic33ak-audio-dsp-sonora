#ifndef SONORA_APP_BUILD_CONFIG_H
#define SONORA_APP_BUILD_CONFIG_H

/*
 * Top-level application selection.
 *
 * APP_BUILD selects one variation.  The variation structurally selects exactly
 * one application; application-specific expansion is delegated to that app's
 * build-config header below.  Keep the numeric APP_BUILD values stable because
 * existing MPLAB and command-line builds may pass them with -DAPP_BUILD=... .
 */

/*
 * APP_BUILD profile metadata.
 *
 * Every '#define APP_BUILD_<name> (n)' below carries a trailing comment holding
 * ';'-separated fields, and this header is the single source of truth for all of
 * them: the buildtools scripts parse the comment instead of keeping their own
 * lists, so there is no second place to update.
 *
 *   #define APP_BUILD_STD_DEMO_1 (1)  / * tier: normal; artifact: classic1; display: Classic 1 * /
 *
 * tier: normal|advanced|internal
 *   How widely the preset is offered (see below). A preset without a marker is
 *   reported as 'unclassified' and hidden from every list - it is never silently
 *   treated as normal.
 *
 * artifact: <lower-case token>
 *   The stable filename token for this profile's serial update package
 *   (artifacts/serial_update_packages/sonora_<artifact>_<timestamp>.sfb). Lower
 *   case letters, digits and underscore only, unique across profiles. Declared
 *   here rather than abbreviated by a script, so that renaming a display name
 *   never renames files. Building a serial update package for a profile with no
 *   artifact tag is refused rather than given an ambiguous name.
 *
 * display: <free text>
 *   What the interactive menus call this profile. The APP_BUILD_* name is an
 *   internal identifier and is not what a user should have to choose from.
 *
 * normal:
 *   Standard user-facing presets.
 *   Shown in the default interactive preset list.
 *   Included in the regular smoke-test scope.
 *
 * advanced:
 *   User-facing presets for specialized hardware or use cases.
 *   Hidden from the default interactive preset list (switch_config.ps1
 *   -Advanced shows them).
 *   Included in the supported build scope.
 *
 * internal:
 *   Developer, measurement, reproduction, load-check and fault-isolation
 *   presets.
 *   Hidden from the default interactive preset list (switch_config.ps1 -All
 *   shows them).
 *   Outside the regular smoke-test contract, so one may temporarily fail to
 *   build during development. Naming it explicitly with -Preset always works.
 *
 * Tier says nothing about which application a preset belongs to, and nothing
 * about per-device availability (see src/app_specific_config_defs.h).
 */
#define SONORA_APP_CLASSIC_AUDIO_DEMO  (1)
#define SONORA_APP_ASRC                (2)

/* Classic Audio Demo variations. */
#define APP_BUILD_STD_DEMO_1           (1)  /* tier: normal; artifact: classic1; display: Classic 1 */
#define APP_BUILD_STD_DEMO_2           (2)  /* tier: normal; artifact: classic2; display: Classic 2 */
#define APP_BUILD_DRC_DEMO             (3)  /* tier: normal; artifact: classic_drc; display: Classic DRC */
#define APP_BUILD_USB_48               (4)  /* tier: advanced; artifact: classic_usb48; display: Classic USB 48k */
#define APP_BUILD_USB_96               (5)  /* tier: advanced; artifact: classic_usb96; display: Classic USB 96k */
#define APP_BUILD_DEMO_96K             (6)  /* tier: normal; artifact: classic_96k; display: Classic 96k */

/* ASRC App variations. */
#define APP_BUILD_ASRC_CODEC_BIDIR     (7)  /* tier: normal; artifact: asrc_bi; display: ASRC Codec BI */
#define APP_BUILD_ASRC_CODEC_A_B_ONLY  (8)  /* tier: internal; artifact: asrc_a_to_b; display: ASRC Codec A-to-B */
#define APP_BUILD_ASRC_CODEC_B_A_ONLY  (9)  /* tier: internal; artifact: asrc_b_to_a; display: ASRC Codec B-to-A */
#define APP_BUILD_ASRC_CODEC_MEAS      (10) /* tier: advanced; artifact: asrc_meas; display: ASRC Codec measurement */
#define APP_BUILD_ASRC_DSPIC_BIDIR     (11) /* tier: normal; artifact: asrc_dspic_bi; display: ASRC dsPIC BI */
#define APP_BUILD_ASRC_DSPIC_LIGHT     (12) /* tier: internal; artifact: asrc_dspic_light; display: ASRC dsPIC light */
#define APP_BUILD_ASRC_CODEC_BIDIR_SPI34_TEST (13)   /* tier: internal; artifact: asrc_bi_spi34; display: ASRC Codec BI SPI3/4 test */
#define APP_BUILD_ASRC_DECIMATOR_MEAS  (14) /* tier: internal; artifact: asrc_decimator_meas; display: ASRC decimator measurement */
#define APP_BUILD_ASRC_48K_TO_8K_INTEGRATION (15)    /* tier: internal; artifact: asrc_48k_to_8k; display: ASRC 48k-to-8k integration */
#define APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32 (16) /* tier: internal; artifact: asrc_bi_m32; display: ASRC Codec BI headroom M32 */
#define APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30 (17) /* tier: internal; artifact: asrc_bi_m30; display: ASRC Codec BI headroom M30 */
#define APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30  (18) /* tier: internal; artifact: asrc_meas_m30; display: ASRC Codec measurement headroom M30 */
/* 96 kHz bring-up: physical I2S 2 ch (L/R) with an 8-channel internal ASRC
 * compute width, one-way A->B, WM8904-B codec-master on its own XTAL.  The
 * WM8904 cannot run ADC and DAC simultaneously at or above its 88.2 kHz
 * boundary, which at 96 kHz is exactly this build, so A is
 * ADC-only and B is DAC-only and bidirectional ASRC is structurally
 * impossible at this rate (guarded in asrc_app_validate.h). */
#define APP_BUILD_ASRC_CODEC_96K_A_TO_B (21) /* tier: internal; artifact: asrc_96k_a_to_b; display: ASRC Codec 96k A-to-B */
/* 96 kHz DIGITAL QUALITY CAPTURE (THD+N / DR), one preset per direction.  Same MEAS harness as
 * preset 10 -- an on-chip sine is injected into the resampler input in software and its output is
 * captured in software, so the codec never carries the test signal and the WM8904's
 * no-simultaneous-ADC+DAC limit at 96 kHz does not apply (that limit is about the ANALOG path).
 *
 * 96K_MEAS_A_TO_B  : leg A at 96 kHz.  With leg B at 48 kHz this measures the DIRECT resampler at
 *                    step 2.0; with leg B at 8/11.025/12/16 kHz it measures the composed
 *                    /2 pre-stage + 48 kHz chain (the rows the runtime gate serves).
 * 96K_MEAS_B_TO_A  : the UPsampling direction, 48 k -> 96 k.  No front end is involved, so this is
 *                    the clean reference point for the resampler alone.
 *
 * Tone note: the tables are sample-domain exact, not absolute-frequency (audio_app_meas_tones.h),
 * so on a 96 kHz leg the LOW tone lands at 2 kHz -- a valid THD+N/DR fundamental -- while the HIGH
 * tone lands at 36 kHz, above a 48 kHz leg's Nyquist.  Use the LOW tone on a 96 kHz source. */
#define APP_BUILD_ASRC_MEAS_96K_A_TO_B  (22) /* tier: internal; artifact: asrc_meas_96k_a_to_b; display: ASRC measurement 96k A-to-B */
#define APP_BUILD_ASRC_MEAS_96K_B_TO_A  (23) /* tier: internal; artifact: asrc_meas_96k_b_to_a; display: ASRC measurement 96k B-to-A */
#define APP_BUILD_ASRC_AK128_CODEC_BIDIR (24) /* tier: internal; artifact: asrc_ak128_bi; display: ASRC AK128 Bi-Codec */
/* Values 19/20 were used only on the pre-main M28 research branch and are
 * intentionally not public presets.  Flip this internal compile-time switch
 * only while building the standard BIDIR or MEAS preset to reproduce it. */
#ifndef APP_ASRC_EXPERIMENTAL_M28
  #define APP_ASRC_EXPERIMENTAL_M28  (0)
#endif
#if (APP_ASRC_EXPERIMENTAL_M28 != 0) && (APP_ASRC_EXPERIMENTAL_M28 != 1)
  #error "APP_ASRC_EXPERIMENTAL_M28 must be 0 or 1"
#endif

#ifndef APP_BUILD
  #if defined(SONORA_MPLAB_APP_ASRC)
    #define APP_BUILD  (APP_BUILD_ASRC_CODEC_BIDIR)
  #else
    #define APP_BUILD  (APP_BUILD_STD_DEMO_1)
  #endif
#endif

#if (APP_BUILD >= APP_BUILD_STD_DEMO_1) && (APP_BUILD <= APP_BUILD_DEMO_96K)
  #define SONORA_APP  SONORA_APP_CLASSIC_AUDIO_DEMO
#elif (APP_BUILD >= APP_BUILD_ASRC_CODEC_BIDIR) && (APP_BUILD <= APP_BUILD_ASRC_AK128_CODEC_BIDIR)
  #define SONORA_APP  SONORA_APP_ASRC
#else
  #error "APP_BUILD is not a known Classic Audio Demo or ASRC App variation."
#endif

#define SONORA_APP_IS_CLASSIC  (SONORA_APP == SONORA_APP_CLASSIC_AUDIO_DEMO)
#define SONORA_APP_IS_ASRC     (SONORA_APP == SONORA_APP_ASRC)

/* Exact preset identity for self-identifying boot logs. */
#if (APP_BUILD == APP_BUILD_STD_DEMO_1)
  #define APP_BUILD_NAME    "APP_BUILD_STD_DEMO_1"
  #define APP_BUILD_DETAIL  "co-clocked dual codec; WM8904-A drives BCLK/FS, B slave"
#elif (APP_BUILD == APP_BUILD_STD_DEMO_2)
  #define APP_BUILD_NAME    "APP_BUILD_STD_DEMO_2"
  #define APP_BUILD_DETAIL  "co-clocked dual codec; dsPIC drives BCLK/FS"
#elif (APP_BUILD == APP_BUILD_DRC_DEMO)
  #define APP_BUILD_NAME    "APP_BUILD_DRC_DEMO"
  #define APP_BUILD_DETAIL  "co-clocked dual codec; DF2T DRC cascade"
#elif (APP_BUILD == APP_BUILD_USB_48)
  #define APP_BUILD_NAME    "APP_BUILD_USB_48"
  #define APP_BUILD_DETAIL  "USB audio input; 48 kHz"
#elif (APP_BUILD == APP_BUILD_USB_96)
  #define APP_BUILD_NAME    "APP_BUILD_USB_96"
  #define APP_BUILD_DETAIL  "USB audio input; 96 kHz"
#elif (APP_BUILD == APP_BUILD_DEMO_96K)
  #define APP_BUILD_NAME    "APP_BUILD_DEMO_96K"
  #define APP_BUILD_DETAIL  "non-USB 96 kHz; co-clocked dual codec"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_BIDIR"
  #if APP_ASRC_EXPERIMENTAL_M28
    #define APP_BUILD_DETAIL  "ASRC EXPERIMENTAL M28 Kaiser-10.55 fc0.4632; bidirectional A<->B"
  #else
    #define APP_BUILD_DETAIL  "ASRC production M30 Kaiser-11 fc0.465; bidirectional A<->B; auto low-rate front-end; sparse LED meter; 10 s telemetry"
  #endif
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_A_B_ONLY)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_A_B_ONLY"
  #define APP_BUILD_DETAIL  "ASRC; codec master; one-way A->B"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_B_A_ONLY)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_B_A_ONLY"
  #define APP_BUILD_DETAIL  "ASRC; codec master; one-way B->A"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_MEAS"
  #if APP_ASRC_EXPERIMENTAL_M28
    #define APP_BUILD_DETAIL  "ASRC EXPERIMENTAL M28 Kaiser-10.55 fc0.4632; one-way digital quality capture"
  #else
    #define APP_BUILD_DETAIL  "ASRC production M30 Kaiser-11 fc0.465; one-way digital quality capture"
  #endif
#elif (APP_BUILD == APP_BUILD_ASRC_DSPIC_BIDIR)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_DSPIC_BIDIR"
  #define APP_BUILD_DETAIL  "ASRC; dsPIC master; bidirectional A<->B"
#elif (APP_BUILD == APP_BUILD_ASRC_DSPIC_LIGHT)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_DSPIC_LIGHT"
  #define APP_BUILD_DETAIL  "ASRC; dsPIC master; LIGHT load test"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_SPI34_TEST)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_BIDIR_SPI34_TEST"
  #define APP_BUILD_DETAIL  "ASRC; WM8904-B codec master; bidirectional A<->B; physical SPI3/SPI4 test bank"
#elif (APP_BUILD == APP_BUILD_ASRC_DECIMATOR_MEAS)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_DECIMATOR_MEAS"
  #define APP_BUILD_DETAIL  "fixed 48-to-8 kHz decimator; standalone digital measurement"
#elif (APP_BUILD == APP_BUILD_ASRC_48K_TO_8K_INTEGRATION)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_48K_TO_8K_INTEGRATION"
  #define APP_BUILD_DETAIL  "48-to-8 kHz decimator feeding near-unity A-to-B ASRC; requires external 8 kHz B transport"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32"
  #define APP_BUILD_DETAIL  "ASRC headroom A/B candidate; M32; raw-tick path profile; sparse LED meter; 10 s telemetry"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30"
  #define APP_BUILD_DETAIL  "ASRC headroom candidate; M30 Kaiser-11 fc0.465; raw-tick path profile; sparse LED meter; 10 s telemetry"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30"
  #define APP_BUILD_DETAIL  "ASRC M30 Kaiser-11 fc0.465; one-way digital quality capture"
#elif (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_CODEC_96K_A_TO_B"
  #define APP_BUILD_DETAIL  "ASRC 96 kHz; physical I2S 2 ch (L/R) + internal 8 ch compute; one-way A->B; WM8904-A ADC-only master, WM8904-B DAC-only codec-master (B-XTAL jumper)"
#elif (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_MEAS_96K_A_TO_B"
  #define APP_BUILD_DETAIL  "ASRC 96 kHz MEAS; A->B digital quality capture (THD+N / DR); leg B rate selects direct resampler vs composed pre-stage chain"
#elif (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_MEAS_96K_B_TO_A"
  #define APP_BUILD_DETAIL  "ASRC 96 kHz MEAS; B->A digital quality capture (THD+N / DR); 48 k -> 96 k upsample, no front end"
#elif (APP_BUILD == APP_BUILD_ASRC_AK128_CODEC_BIDIR)
  #define APP_BUILD_NAME    "APP_BUILD_ASRC_AK128_CODEC_BIDIR"
  #define APP_BUILD_DETAIL  "ASRC AK128; TDM8 8ch A<->B; Codec-A/Codec-B master; Curiosity J3 DIM-Pxx U-jumpers"
#else
  /* The retired M28 research values 19/20 now fall inside the ASRC APP_BUILD
   * range, so an unnamed preset must fail loudly instead of building without
   * an identity string. */
  #error "APP_BUILD has no APP_BUILD_NAME/DETAIL entry (retired or unknown preset value)."
#endif

/*
 * Compatibility names used by the existing resolved platform configuration.
 * APP_PROFILE is now derived from APP_BUILD and may not select a different app.
 */
#define APP_PROFILE_DEMO  (0)
#define APP_PROFILE_ASRC  (1)

#ifndef APP_PROFILE
  #if SONORA_APP_IS_ASRC
    #define APP_PROFILE  APP_PROFILE_ASRC
  #else
    #define APP_PROFILE  APP_PROFILE_DEMO
  #endif
#endif

#if SONORA_APP_IS_ASRC && (APP_PROFILE != APP_PROFILE_ASRC)
  #error "ASRC APP_BUILD variation conflicts with APP_PROFILE. Select the app through APP_BUILD."
#endif
#if SONORA_APP_IS_CLASSIC && (APP_PROFILE != APP_PROFILE_DEMO)
  #error "Classic APP_BUILD variation conflicts with APP_PROFILE. Select the app through APP_BUILD."
#endif

/* ASRC clock-owner tokens are public only so ASRC variations can be overridden. */
#define APP_ASRC_CLOCK_OWNER_SPI2   (0)
#define APP_ASRC_CLOCK_OWNER_CODEC  (1)

#if SONORA_APP_IS_ASRC
  #include "asrc/asrc_app_build_config.h"
#else
  #include "classic/classic_demo_build_config.h"
#endif

#endif /* SONORA_APP_BUILD_CONFIG_H */
