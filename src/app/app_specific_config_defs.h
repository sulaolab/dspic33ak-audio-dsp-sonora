#ifndef APP_SPECIFIC_CONFIG_DEFS_H
#define	APP_SPECIFIC_CONFIG_DEFS_H


//===========================================================
// EARLY CLOCK CONSTANTS
//===========================================================
// FCY must be visible BEFORE <xc.h>: <xc.h> transitively pulls in <libpic30.h>, whose
// __delay_ms()/__delay_us() macros are built against FCY.
//
// PLL1_CLK_HZ is the Sonora boot PLL1/CLKGEN output frequency. FCY is derived
// from it and must remain visible before <xc.h>.
//
// Integer (UL) literals -- not 200e+6 -- so FCY and any derived clock math stay integer.
// Do NOT add definitions here that depend on <xc.h> device symbols/registers.
#define PLL1_CLK_HZ                     (200000000UL) // boot PLL1/CLKGEN output
#define FCY                             (PLL1_CLK_HZ / 2UL) // instruction-cycle frequency


//===========================================================
// INCLUDES
//===========================================================
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>


/* ============================================================
 * app_specific_config_defs.h
 *
 * Project/application-level configuration -- the SINGLE app-config file.
 *
 * This file intentionally contains three logical layers (formerly the separate
 * app_config_cmsis.h / app_specific_config_resolved.h / app_specific_config_validate.h
 * headers, now merged here):
 *
 *   1. Raw project/app settings    - direct user/project selections: platform
 *                                    constants, build toggles, hand-tuned sound params.
 *   2. Resolved / derived settings - facts COMPUTED from the raw settings (the ENA_*
 *                                    tree -> 0/1 APP_* facts, geometry, feature set).
 *                                    No independent user choices here.
 *   3. Validation                  - reject invalid / inconsistent combinations.
 *                                    Creates no new configuration values.
 *
 * Keep the order: Raw -> Resolved -> Validation. Each layer depends on the one above.
 * Do not split these layers back into separate headers unless the file becomes too
 * large to manage -- the split was removed because the include-order / dependency /
 * partial-state reasoning cost outweighed its benefit at this size. The three logical
 * layers are preserved here as clearly-marked sections instead of separate files.
 * ============================================================ */


/* ============================================================
 * 1. Raw project/app settings
 * ============================================================ */

//-----------------------------------------------------------
// 1a. PLATFORM  (order-sensitive -- do NOT reorder this block)
//-----------------------------------------------------------

// --- Platform clocks and peripheral rates ---
// PLL1_CLK_HZ / FCY live in the EARLY CLOCK CONSTANTS block at the top of this
// file (FCY must precede <xc.h>).

#define UART_BRG                        (230400)
#define UART_PLATFORM_USE_RX_ISR_RING   (1)         // Platform: UART ring-buffer ISR reception

// Default period (ms) of the app telemetry line printed by audio_transport_dbg_print()
// (TDM load / ASRC / CCP). Runtime-changeable via audio_transport_set_dbg_period_ms().
#define APP_DBG_PERIOD_MS               (2000)


//-----------------------------------------------------------
// 1b. USER TOGGLES  (edit here)
//-----------------------------------------------------------
// Style: each knob is one entry -- "--- HEADING ---", a short prose note, then
// a single #define line. An ACTIVE #define is ON; a "// #define" is OFF. The
// #define line is always last in its entry so it never hides inside the prose.
//
// Grouped into bands, broad -> narrow:
//   HARDWARE       - match to the physical board (set once per PCB).
//   BUILD PROFILE  - pick the overall build (DRC test vs regular, 48k/96k).
//   AUDIO STREAM   - how the TDM/I2S stream is shaped (format/input/topology).
//   EXTRA PATHS    - optional alternate I/O paths; normally all off.
//   CLOCK ROLE     - who drives BCLK/FS.
//   CMSIS / SAI    - CMSIS driver-path + SAI wrapper verification switches.


// --- APP + BUILD VARIATION ---
// APP_BUILD is the only top-level selector. It first selects one application, then the
// selected application expands its own variation. Definitions live under src/apps/:
//   apps/classic/classic_demo_build_config.h  - Standard/DRC/USB/96 kHz variations
//   apps/asrc/asrc_app_build_config.h          - direction/clock-owner/measurement variations
// APP_PROFILE remains a derived compatibility name for the resolved settings below.
#include "apps/app_build_config.h"

// =====================================================================================
//  BUILD CONFIGURATION MAP (quick reference)
//  Top axis = APP_PROFILE (which app). Everything a given build cares about is in its
//  sub-table below; a knob that does NOT appear for your profile is N/A (leave it alone).
// =====================================================================================
//
//  Pick a build with APP_BUILD = <preset> (see the BUILD PRESET block above). The columns show
//  the base toggles that preset expands to -- you normally do NOT set them by hand.
//
//  A) APP_PROFILE = DEMO   (leg B co-clocked; no ASRC; == origin/main)
//     All rows: APP_USE_SPI2_INDEPENDENT_MASTER=0, APP_REQ_B_CODEC_MASTER=0 (both N/A here).
//     Audio format is auto-derived (USB or 96k => I2S). Clock tree is the fixed PLL1<-FRC one that
//     every build shares -- see (2.5e).
//
//       APP_BUILD (preset)      Config                USB_AUDIO_IN  96K_RATE  SPI_TDM_CLK_MASTER  DRC_DF2T_CASCADE
//       STD_DEMO_1  (family default) WM8904-A master        0           0             0                 0
//       STD_DEMO_2              dsPIC master               0           0             1                 0
//       DRC_DEMO                WM8904-A master, DRC       0           0             0                 1
//       USB_48                  USB-audio 48k              1           0             0                 0
//       USB_96                  USB-audio 96k              1           1             0                 0
//       DEMO_96K                non-USB 96k co-clock       0           1             0                 0
//     "master" = who drives BCLK/FS: WM8904-A (external codec master, dsPIC slave), the dsPIC itself,
//     or the Pico2 USB-audio bridge. All use both codecs (WM8904-A/B) at 48K unless 96K is noted.
//     NOTE: USB-audio needs the plain PLL1<-FRC tree, because the codecs are clock slaves to the
//     Pico2 bridge. That is now the only tree there is, so no guard is needed for it any more.
//
//  B) APP_PROFILE = ASRC   (leg B independent domain; ASRC engine on; A heavy DSP dropped)
//     AK512 dual-codec, TDM8. USB_AUDIO_IN / 96K_RATE / SPI_TDM_CLK_MASTER / DRC_DF2T_CASCADE are all
//     N/A here (do not appear). Pick two things:
//       Step 1 - clock owner (APP_ASRC_CLOCK_OWNER):
//         APP_ASRC_CLOCK_OWNER_SPI2   : dsPIC SPI2 generates B's clock (dsPIC-B master)
//         APP_ASRC_CLOCK_OWNER_CODEC  : WM8904-B on its own XTAL (codec-B master; *ar CC RR)  <- default
//       Step 2 - ASRC route (direction / variant):
//         Route              selector                                              A out        B out
//         BIDIR (default)    APP_ENA_ASRC_BIDIR=1                                  resampled B  resampled A
//         FROM_A             APP_ENA_ASRC_BIDIR=0, APP_ENA_ASRC_FROM_B=0           silence      resampled A
//         FROM_B             APP_ENA_ASRC_BIDIR=0, APP_ENA_ASRC_FROM_B=1           resampled B  silence
//         MEAS               APP_ASRC_MEAS=1  (+ APP_MEAS_DIR = AB | BA)           (measurement capture)
//         LIGHT (load test)  APP_B_ROUTE = B_ROUTE_ASRC_LIGHT (override)           both = pure asrc_pull
//       (LED shows the ASRC OUTPUT; BIDIR shows max(A out, B out). No Classic/DRC/Gain/PWM.)
//
//     Common ASRC builds (pick with APP_BUILD):
//       APP_BUILD (preset)        CLOCK_OWNER   ROUTE      (BIDIR / FROM_B / MEAS / LIGHT)  PORT
//       ASRC_CODEC_BIDIR (deflt)    CODEC        BIDIR        1       -      0      -       SPI1/2
//       ASRC_CODEC_A_B_ONLY         CODEC        A->B only    0       0      0      -       SPI1/2
//       ASRC_CODEC_B_A_ONLY         CODEC        B->A only    0       1      0      -       SPI1/2
//       ASRC_CODEC_MEAS             CODEC        MEAS         -       -      1      -       SPI1/2
//       ASRC_DSPIC_BIDIR            SPI2         BIDIR        1       -      0      -       SPI1/2
//       ASRC_DSPIC_LIGHT (test)     SPI2         LIGHT        0       -      0      1       SPI1/2
//       ASRC_CODEC_BIDIR_SPI34_TEST CODEC        BIDIR        1       -      0      -       SPI3/4 (explicit test bank)


// --- HARDWARE / PCB REVISION ---
//   Match these to the physical WM8904 codec board in use.
#define WM8904_PCB_REV4      // REV4 support XTALout(RP16) and MIC Bias
//// workaround ////
#define WM8904_SWAP_ADC_LR   // only Rev.4 PCB(White) needs this -- PCB issue
#define WM8904_SWAP_DAC_LR   // only Rev.4 PCB(White) needs this -- PCB issue
//// workaround ////


// --- RED INPUT JACK (MCHP X32 CODEC PCB) ---
// #define ENA_RED_IN_JACK

// --- MIC INPUT ---
// #define ENA_MIC_IN


// --- PROFILE: DRC test mode ---
//   Selects the DRC (DF2T) biquad-cascade build instead of the regular feature set.
//   Drives the buffer-sizing and feature-set blocks in the DERIVED section.
#ifndef ENA_DRC_DF2T_CASCADE
#define ENA_DRC_DF2T_CASCADE     (0)   // 1 = DRC (DF2T) biquad-cascade build  (base default; set by APP_BUILD)
#endif

// --- DIAG: co-clocked SPI1/SPI2 ping-pong phase probe (step-1 experiment, opt-in) ---
//   Measurement ONLY (detect + log, NO auto-correct): the co-clocked single-producer path
//   (SPI1 ISR fills WM8904-B) tears under load if SPI1/SPI2 TX ping-pong phases are not
//   aligned. This samples both legs' live TX half each SPI1 block and counts mismatches;
//   the running stats print on the audio_transport_dbg_print telemetry line. Default off.
#define APP_TDM_PHASE_PROBE       (0)   // 1 = sample+report SPI1/SPI2 phase alignment (diagnostic)

// --- DIAG: co-clocked phase-mismatch GUARD (opt-in safety net; default OFF) ---
//   Belt-and-suspenders on top of the phase-locked start_all_domains: if the co-clocked SPI2
//   ping-pong is seen crossing its boundary DURING the SPI1 block callback (= B's cross-fill
//   would tear) for APP_TDM_SYNC_GUARD_TRIP consecutive blocks, the main-loop manage path
//   mutes and re-syncs the WHOLE transport (restart -> start_all_domains re-locks phase), never
//   a single leg. With the aligned start this NEVER trips (measured tail_tear=0), so default OFF
//   = zero behaviour change; enable only for hardening. NOT a substitute for the aligned start.
#define APP_TDM_SYNC_GUARD        (0)   // 1 = auto mute + whole-transport resync on persistent mismatch
#define APP_TDM_SYNC_GUARD_TRIP   (8)   // consecutive mismatched blocks before a resync is requested

// --- STARTUP PHASE LOCK (default ON): make co-clocked start deterministic ---
//   The back-to-back SPIEN start is NOT a hardware-guaranteed simultaneous start -- on a
//   free-running external FS the two SPIEN writes can straddle an FS edge, so a co-clocked domain
//   can occasionally come up ~2 frames offset -> WM8904-B tears (observed: intermittent boot tear,
//   clean after a reset). This verifies, while still MUTED, that every co-clocked domain is
//   phase-locked (position diff within tol) a few blocks after start; if not, it re-arms the domain
//   and retries, letting the caller unmute only once locked. Guarantees a clean boot regardless of
//   the marginal SPIEN race and of code-layout timing shifts.
//   LOCK is declared only when ALL of these hold for APP_TDM_LOCK_CONSEC_OK consecutive blocks:
//   |wdiff|<=TOL_WORDS, no half-mismatch (h1==h2), no tail_tear (SPI2 half unchanged across the
//   callback), and the mirrored B-write target is NOT SPI2's transmitting half. wdiff==0 ALONE is
//   necessary-but-not-sufficient (HW showed wdiff=0 with tail_tear=samp + audible tear), so
//   tail_tear/unsafe-target act as vetoes: mis-declaring re-arms (safe), never unmutes a torn start.
#define APP_TDM_STARTUP_PHASE_LOCK (1)  // 1 = verify + re-arm until co-clocked domains lock, then unmute
#define APP_TDM_LOCK_RETRIES       (5)  // max re-arm attempts before giving up (stay muted + report)
#define APP_TDM_LOCK_CONSEC_OK     (6)  // consecutive all-conditions-OK blocks required to declare LOCK
#define APP_TDM_LOCK_WAIT_MS       (12) // per attempt: wait window for the ISR to reach CONSEC_OK (~18 blocks)
#define APP_TDM_LOCK_TOL_WORDS     (2)  // max allowed TX-DMA position diff (words) within a domain

// --- RETIRED EXPERIMENT: SPIROV/SPITUR latch (IGNROV/IGNTUR) ---
//   The old APP_TDM_ROVTUR_DETECT knob (clear IGNROV/IGNTUR so RX-overflow/TX-underrun latch) is
//   GONE. Letting SPIROV or SPITUR become a critical-stop condition can suspend the leg and turn a
//   primary DMA service failure into a permanent callback stall. HW investigation identified the
//   causal chain as SRAM arbitration delay -> DMAxSTAT.OVERRUN -> stopped RX request service ->
//   downstream SPIROV; DMAPR=1 is the root-cause mitigation. The HAL hard-forces IGNROV/IGNTUR=1
//   only to contain the secondary stop, not to classify data loss as benign. DMA OVERRUN is retained
//   directly as dov=/dirq=/ds= telemetry; SPIROV/SPITUR/FRMERR remain sampled per block as
//   rov=/tur=/frm=. Frame-slip recovery continues to use independent FRMERR.

// --- Bit-slip AUTO-RECOVERY (connector-glitch frame-slip) ---
//   When FRMERR (SPIxSTAT frame-sync error) stays set for RECOVER_BLOCKS consecutive RX blocks on
//   either leg -- a metal-shell connector glitch that slipped the TDM frame and would otherwise stay
//   broken until a HW reset -- auto-restart the transport (== *nt03) so the dsPIC SPI/DMA re-locks,
//   with NO HW reset. Checked from the main loop (audio_transport_frmerr_recover_tick), self-gated at
//   CHECK_MS, with a post-restart COOLDOWN_MS so a single glitch or the re-lock transient never
//   restart-loops. FRMERR latches regardless of IGNROV/IGNTUR (which the HAL forces to 1).
//   Set APP_TDM_FRMERR_AUTORECOVER 0 to compile the recovery out (detection/telemetry stay).
#define APP_TDM_FRMERR_AUTORECOVER   (1)
#define APP_TDM_FRMERR_RECOVER_BLOCKS (32)   // consecutive FRMERR blocks -> recover (~10 ms @ 48k/blk16)
#define APP_TDM_FRMERR_CHECK_MS      (5)     // main-loop self-gate period for FRMERR/liveness checks
#define APP_TDM_FRMERR_COOLDOWN_MS   (500)   // after a restart, skip checks this long (re-lock + anti-loop)
// Deadlock-free recovery = MUTE + restart-until-healthy. A noise-induced frame slip is PERSISTENT
// on this HW (the dsPIC slave latches a wrong frame boundary and stays there even after the noise
// is gone), so a re-lock action IS required. The action that actually re-locks is the full restart
// (audio_transport_restart == *nt03: mute -> stop -> codec re-init -> phase-locked start); a transport-
// only re-arm (no codec re-init) does NOT re-lock. The original auto-recover FIRED that restart but
// then relied on FRMERR-consecutive to re-trigger -- and once the leg is frozen NO blocks complete,
// so FRMERR stops counting and it never RETRIES (the "doesn't recover" bug). Fix: retry the working
// restart until the transport is confirmed HEALTHY (block_count actually advancing), using
// health/liveness -- not the FRMERR counter -- as the retry signal. Stay MUTED across recovery
// (anti-blast). If still not healthy after MAX_RESTART attempts, latch a SAFE-MUTE (silence + fault
// report). A retry succeeds only after NEW clean blocks; a moving-but-misframed stream is rejected.
#define APP_TDM_FRMERR_RELOCK_BLOCKS   (8u)     // NEW blocks after restart that must all be FRMERR-clean
#define APP_TDM_FRMERR_RELOCK_TIMEOUT_MS (300u) // per-attempt wait for the restart to go healthy
#define APP_TDM_FRMERR_MAX_RESTART     (6u)     // restart attempts before the safe-mute latch
#define APP_TDM_LIVENESS_STALL_MS      (20u)    // clock advances but RX block_count does not -> recover


// --- PROFILE: 96 kHz mode ---
//   96 kHz path. Auto-selects I2S format + SAMPLE_RATE in the DERIVED section.
//   NOTE: 96 kHz requires the dual-codec topology (see TOPOLOGY below).
// #define ENA_96K_RATE


// --- INPUT SOURCE ---
//   USB-audio on SPI1/DIM; dsPIC = clock slave.
//   NOTE: for a 96kHz USB host also define ENA_96K_RATE; for 48kHz leave it off.
//   Placed BEFORE AUDIO FORMAT: USB (Pico2) is I2S-fixed, so the format below derives from it.
// #define ENA_USB_AUDIO_IN


// --- AUDIO FORMAT ---
//   I2S is a HW FACT (not a user choice) in two cases, so it is auto-derived:
//     - USB-audio (ENA_USB_AUDIO_IN): the Pico2 bridge emits I2S ONLY (2 slots, 1-bit delay) at
//       both 48/96 kHz -- a TDM8 selection would silently mis-frame the Pico2 input.
//     - 96 kHz (ENA_96K_RATE): the WM8904 supports I2S only at 96 kHz (see validation #error).
//   Otherwise (non-USB, 48 kHz) it is the user choice in the #else -- set exactly one to 1:
//     I2S  (2 slots) : also requires APP_USE_1_BIT_DELAY 1 (auto-forced below).
//     TDM8 (8 slots) : default for 48kHz operation.
#if defined(ENA_USB_AUDIO_IN) || defined(ENA_96K_RATE)
  #define APP_USE_I2S_FORMAT      (1)   // USB or 96k: I2S (2 slots) is a HW fact
  #define APP_USE_TDM8_FORMAT     (0)
#else
  #define APP_USE_I2S_FORMAT      (0)   // 1 = I2S  (2 slots)   <-- user choice
  #define APP_USE_TDM8_FORMAT     (1)   // 1 = TDM8 (8 slots)   <-- user choice
#endif

// --- BIT DELAY ---
//   1-bit delay between LRCLK assertion and first data bit.
//   Required (must be 1) when APP_USE_I2S_FORMAT=1; optional for TDM. USB and 96k force I2S -> 1.
#if defined(ENA_USB_AUDIO_IN) || defined(ENA_96K_RATE)
  #define APP_USE_1_BIT_DELAY     (1)   // USB/96k => I2S requires 1-bit delay
#else
  #define APP_USE_1_BIT_DELAY     (0)   // 1 = 1-bit delay enabled   <-- user choice
#endif

// --- FRAME-SYNC WAVEFORM SHAPE ---
//   Selects cfg->fs_shape (HAL feature). The HAL hides FRMSYPW/FRMCNT/CLC:
//     1 = FS_50PCT : 50%-duty FS. I2S -> native (FRMSYPW=1); TDM MASTER -> the HAL emits a
//         half-frame marker and toggles CLC10 into a ~50%-duty FS on the FS pin (owns CLC10
//         + virtual pin RPV8). TDM SLAVE ignores it (FS is an input).
//     0 = FS_PULSE : short 1-BCLK frame sync.
//   NOTE: FS_50PCT-via-CLC10 needs the dsPIC to be the clock master (APP_USE_SPI_TDM_CLK_MASTER 1).
#define APP_USE_FS_50PCT          (1)   // 1 = 50%-duty FS ; 0 = short pulse


// --- TOPOLOGY: dual-codec on MikroBUS-B / SPI2 ---
//   AK512, plus the explicitly selected AK128 Curiosity J3 U-jumper ASRC build
//   (feeds APP_USE_SPI2_AUDIO; resolved_transport_config.h translates it).
//   NOTE: remove bridge resistors between MikroA/B for independent I2C buses.
//   NOTE: 96kHz requires both codecs (WM8904-A=ADC, WM8904-B=DAC; HW limit). See validation below.
#ifndef APP_REQ_MIKROB_WM8904
#define APP_REQ_MIKROB_WM8904     (1)   // 1 = second WM8904 on MikroBUS-B / SPI2
#endif
// Physical SPI bank used by the existing logical codec-A/B TDM pair.
// SPI34_TEST reroutes the SAME MikroBUS A/B WM8904 pins from SPI1/2 to SPI3/4,
// allowing the two available codecs to validate the new peripherals and DMA4-7.
// ALL4 is reserved for the eventual simultaneous two-pair build; it needs a
// second external TDM8 source/sink and a separate board pin map.
#define APP_TDM_PORT_MODE_SPI12       (0)
#define APP_TDM_PORT_MODE_SPI34_TEST  (1)
#define APP_TDM_PORT_MODE_ALL4        (2)
#ifndef APP_ASRC_TDM_PORT_MODE
  #if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_SPI34_TEST)
    #define APP_ASRC_TDM_PORT_MODE      APP_TDM_PORT_MODE_SPI34_TEST
  #else
    #define APP_ASRC_TDM_PORT_MODE      APP_TDM_PORT_MODE_SPI12
  #endif
#endif
// Keep classic/DRC/USB smoke tests one-knob: changing only APP_BUILD to a
// non-ASRC preset always restores the established physical SPI1/SPI2 bank.
#if APP_PROFILE == APP_PROFILE_ASRC
#define APP_TDM_PORT_MODE             APP_ASRC_TDM_PORT_MODE
#else
#define APP_TDM_PORT_MODE             APP_TDM_PORT_MODE_SPI12
#endif


// --- PWM DAC OUTPUT ---
#ifndef APP_USE_PWM_AUDIO
#define APP_USE_PWM_AUDIO         (0)   // 1 = PWM DAC output path
#endif

// --- BOOT BANNER VISIBILITY (POR/BOR only) ---
// The USB-CDC cable carries both power and console, so on a power-on-class reset
// the terminal has not finished enumerating when the early boot banner (printMenu)
// prints -- debuggers miss it. Holding the boot button at power-on re-prints the
// banner for a short window so it can be caught; not holding it boots immediately
// (single early banner, behavior unchanged = fast boot). Warm resets keep the
// terminal open and are never delayed. The button is sampled once at boot, before
// the app's runtime button controls are active, so there is no functional conflict
// with the runtime button assignments.
#ifndef APP_BOOT_BANNER_HOLD_ENABLE
#define APP_BOOT_BANNER_HOLD_ENABLE    (1)     // 0 = compile the boot-banner hold out entirely
#endif
#ifndef APP_BOOT_BANNER_HOLD_BUTTON
#define APP_BOOT_BANNER_HOLD_BUTTON    (1)     // board button id (1..3) held at power-on to enter banner hold
#endif
#ifndef APP_BOOT_BANNER_HOLD_SECONDS
#define APP_BOOT_BANNER_HOLD_SECONDS   (5)     // banner re-print window length (seconds)
#endif
#ifndef APP_BOOT_BANNER_HOLD_REPEAT_MS
#define APP_BOOT_BANNER_HOLD_REPEAT_MS (1000)  // re-print interval within the window (ms)
#endif

// --- ANC TEST ---
// #define ENA_ANC_TEST


// --- CLOCK ROLE ---
//   default = codec master (dsPIC slave). Set 1 to make dsPIC the BCLK/FS master.
//   REQUIRES: BCLK->MCLK jumper on A-board (TDM8/32-bit: BCLK=MCLK=256fs). HW-VERIFIED 2026-06-21.
//   NOTE: SPIxBRG auto-derived from the Sonora CLKGEN9 transport clock fact.
//#define APP_USE_SPI_TDM_CLK_MASTER (1) // 1 = dsPIC generates BCLK/FS (clock master)
#ifndef APP_USE_SPI_TDM_CLK_MASTER
#define APP_USE_SPI_TDM_CLK_MASTER (0) // 0 = dsPIC is clock slave  (base default; set by APP_BUILD)
#endif


// ASRC-app-private raw configuration (routes, kernel/quality knobs, measurement tones, ...).
// Pulled only for ASRC builds; the shared layer above reads none of it, and the SPI2 bridge input
// it used to provide (APP_REQ_SPI2_INDEPENDENT_MASTER) now comes from the neutral build-configs.
// Classic builds neither include nor need it -- this mirrors the app_build_config.h split.
#if SONORA_APP_IS_ASRC
#include "apps/asrc/asrc_app_config.h"
#endif


// ENA_DMA_SELFTEST: run the NORA hal_dma conformance gate once at boot, right after
//   nora_dma_global_init(). This is the dsPIC33CK application src/app/dma_selftest.c
//   moved here with no code change at all (src/dma_selftest.c), and
//   what it verifies is the CONTRACT, not the DMA: section 4 of
//   [internal] nora_dma_tdm_contract_draft_2026-08-10.md defines conformance
//   as "an application moving, not a header matching". Prints one line
//   ("DMA selftest (ch3, RAM->RAM, software CHREQ): PASS") and borrows channel 3
//   before any transport owns it. Off by default and costed at zero: with
//   isolate-each-function + remove-unused-sections the linker discards the whole
//   module when this is undefined (contract rule R0.1). Default: undefined.
// #define ENA_DMA_SELFTEST

// --- CMSIS DRIVER PATH / SAI VERIFICATION  (was app_config_cmsis.h) ---
// Normally all of these are OFF (HAL-direct paths are the default). Enable only
// when testing the CMSIS layer.

// ENA_CMSIS_I2C: route WM8904 I2C through the CMSIS-Driver wrapper
//   (src/cmsis_driver/Driver_I2C_dsPIC33AK.*). When not defined, the I2C HAL
//   (nora_i2c_*) is called directly. Default: undefined (HAL direct).
// #define ENA_CMSIS_I2C

// ENA_CMSIS_USART: initialize and power UART1 through the CMSIS-Driver USART
//   wrapper instead of legacy UART1_Initialize(). Existing stdio / console I/O
//   continues to use the shared UART HAL underneath. Default: undefined.
// #define ENA_CMSIS_USART

// ENA_SAI_WRAPPER_DRYTEST: boot self-test (dry, no live stream). Runs Driver_SAI0
//   API checks once at boot, prints a [SAI-DRYTEST] summary, restores the default
//   config. (Verified 2026-06-21: pass=18 fail=0.)
// #define ENA_SAI_WRAPPER_DRYTEST

// ENA_SAI_WRAPPER_LIVE: LIVE loopback (opt-in). Replaces the demo DSP path with a
//   wrapper-driven passthrough (Receive -> Send, replicated to both codecs). TX is
//   DOUBLE-BUFFERED. Verified glitch-free on HW 2026-06-21. Mutually exclusive with
//   the demo path.
// #define ENA_SAI_WRAPPER_LIVE

// ENA_SAI_WRAPPER_LIVE_TONE: sub-mode of LIVE -- Send a phase-continuous loud tone
//   (~-6 dBFS, 800 Hz, slots 0/1) and ignore RX. Needs LIVE.
// #define ENA_SAI_WRAPPER_LIVE_TONE

// ENA_SAI_LIVE_KEEPALIVE: add an inaudible ~24 kHz keep-alive to the wrapper loopback
//   output (slots 0/1). Needs LIVE; ignored in TONE mode. Opt-in.
// #define ENA_SAI_LIVE_KEEPALIVE

// SAI verification isolation matrix -- set at most ONE. All bypass the demo DSP and
// pass input slots 0/1 to output 0/1; the difference is which side uses the CMSIS
// wrapper copy layer vs a direct demo-path copy:
//   ENA_APP_RAW_BYPASS              : input demo + output demo  (no wrapper) -> OK
//   ENA_SAI_HYBRID_IN_ORIG_OUT_WRAP : output via wrapper Send (tone)         -> OK
//   (ENA_SAI_WRAPPER_LIVE)          : full loopback in+out wrapper            -> OK
// #define ENA_APP_RAW_BYPASS
// #define ENA_SAI_HYBRID_IN_ORIG_OUT_WRAP


//-----------------------------------------------------------
// 1c. SOUND PARAMS  (hand-tuned numeric constants)
//-----------------------------------------------------------
// STAGE_*_PROC_CH must be defined before the Resolved section consumes them below.

#define STAGE_1_PROC_CH           (2)      // stereo
#define STAGE_2_PROC_CH           (4)      // 4ch stereo


// pri gain for WM8904
/////////////////////////////////////////////
#define PRE_GAIN_CODEC_DB         (  0.0f)

// post gain for WM8904
//#define POST_GAIN_CODEC_DB        (  0.0f)
//#define POST_GAIN_CODEC_DB        ( 16.0f)  // for UCA222(Analog-IN to USB Audio)
//#define POST_GAIN_CODEC_DB        (  6.0f)  // -6.0dB in WM8904 output side to reduce hissing noise
#define POST_GAIN_CODEC_DB        ( 12.0f)  // adjusted in=out at FlatEQ


// post gain for PWM
/////////////////////////////////////////////
//#define POST_GAIN_PWM_DB          ( 6.0f) // need some boost
//#define POST_GAIN_PWM_DB          ( 10.0f) // need some boost
#define POST_GAIN_PWM_DB          (14.0f) // at PAS music room test


// gain for the sound effect wave data
/////////////////////////////////////////////
// #define PRE_GAIN_WAV_DB           (-38.0f)
#define PRE_GAIN_WAV_DB           (-26.0f)

// gain for the engine synth
/////////////////////////////////////////////
//#define PRE_GAIN_ENG_SYNTH_DB     (-28.0f)
#define PRE_GAIN_ENG_SYNTH_DB     (-36.0f)

// Gain for the currently selected AVAS synth source (LAMB or TYPE_TY).
// This is the final source gain; per-partial gains remain part of the timbre.
#define PRE_GAIN_AVAS_SYNTH_DB    (-26.0f)


/* ============================================================
 * 2. Resolved / derived settings  (computed from section 1; do NOT hand-edit)
 * ============================================================ */
// Ordering here is load-bearing -- each block depends on macros defined above it.
// This section creates NO independent user choices; it only normalizes / derives.


// (2.0) Target device -- single app-side identity, derived from the toolchain's -mcpu
// predefined macro (the authoritative source). Placed FIRST in the Resolved section so
// every block below selects on APP_TARGET; the __dsPIC33AK*__ vendor macro is confined
// to this one derivation. Add a device: define a token + add one #elif arm.
//
// Token values are OPAQUE and arbitrary on purpose. Compare with == only -- never order
// (< >), never do arithmetic, never infer flash/RAM size or part number from them.
// 0 is reserved as "unset" so an undefined APP_TARGET fails closed (0 != tag).
#define APP_TARGET_AK512   (1)   // arbitrary distinct tag (NOT a size/part number)
#define APP_TARGET_AK128   (2)
// #define APP_TARGET_AK256   (3)   // future

#if   defined(__dsPIC33AK512MPS512__)
  #define APP_TARGET   APP_TARGET_AK512
#elif defined(__dsPIC33AK128MC106__)
  #define APP_TARGET   APP_TARGET_AK128
#else
  #error "Unsupported device -- check -mcpu / MPLAB X device selection."
#endif


// (2.1) Touch: used by the Classic UI on the AK512 Curiosity board.
// The ASRC app has no touch controls, so do not acquire sensors or enable the
// periodic ITC interrupt there.
//
// ENA_OPEN_TOUCH_EXCLUSIVE selects the open ITC touch library (src/hal_touch)
// and is now the default and the only path: the vendor QTouch library has been
// removed from the tree, so the old ENA_TOUCH build no longer exists. The name
// keeps the word 'exclusive' because the property it names still matters --
// exactly one owner may program the ITC. When both libraries were linked, every
// measurement was taken against a peripheral two owners were writing to, which
// cost 3,000 counts of baseline offset before anyone noticed
// ([internal] itc_hardware_reference.md 10).
//
// Defined here rather than left to -D so the demo image and a measurement image
// are the same image. Override it on the build only to take touch out entirely.
#if (APP_TARGET == APP_TARGET_AK512) && SONORA_APP_IS_CLASSIC
  #if !defined(ENA_OPEN_TOUCH_EXCLUSIVE)
    #define ENA_OPEN_TOUCH_EXCLUSIVE 1
  #endif
#endif //(APP_TARGET == APP_TARGET_AK512) && SONORA_APP_IS_CLASSIC


// (2.1b) USB-audio -> I2S coupling: derived up-front at the AUDIO FORMAT section (the
//   ENA_USB_AUDIO_IN toggle was moved above AUDIO FORMAT so APP_USE_I2S_FORMAT /
//   APP_USE_TDM8_FORMAT / APP_USE_1_BIT_DELAY are defined ONCE, positively, per the USB fact --
//   no define-then-#undef override needed here anymore).


// (2.2) Format consistency guard (must follow the user APP_USE_* definitions above).
#if (APP_USE_I2S_FORMAT + APP_USE_TDM8_FORMAT) != 1
  #error "Exactly one of APP_USE_I2S_FORMAT / APP_USE_TDM8_FORMAT must be 1."
#endif

// (2.3) Sample rate, derived from ENA_96K_RATE.
//       Must precede the APP_SLOTS_PER_FS / APP_BLOCK_FRAMES blocks below.
#if defined(ENA_96K_RATE)
  #define SAMPLE_RATE               (96000)
  #if !APP_USE_I2S_FORMAT
    #error "ENA_96K_RATE requires APP_USE_I2S_FORMAT 1 (WM8904 supports I2S only at 96kHz)."
  #endif

#else
  #define SAMPLE_RATE               (48000)

#endif //defined(ENA_96K_RATE)


// (2.3) Block size (APP_BLOCK_FRAMES) is defined LATER, in (2.5f) -- it now depends on
//       APP_B_INDEP_DOMAIN (ASRC engine gate), which is not resolved until (2.5e). See (2.5f).


// (2.4) Slots per frame, derived from the resolved format.
#if APP_USE_I2S_FORMAT
 #define APP_SLOTS_PER_FS         (2)      // I2S:  2 slots
#else
 #define APP_SLOTS_PER_FS         (8)      // TDM8: 8 slots
#endif //APP_USE_I2S_FORMAT


// (2.5) The AK128 DIM leaves the MikroBUS-B TDM pins unconnected.  The one
// deliberate exception is the dedicated ASRC build, which joins Curiosity J3
// DIM-P33/P35/P37/P39 to the four AK128 RP pins with U-jumpers.  Never infer
// this capability merely from the target: the ordinary AK128 Classic image
// remains a single-codec image.
#if (APP_TARGET == APP_TARGET_AK128) && \
    (APP_BUILD == APP_BUILD_ASRC_AK128_CODEC_BIDIR)
  #define APP_AK128_J3_TDM_B       (1)
#else
  #define APP_AK128_J3_TDM_B       (0)
#endif

// APP_USE_SPI2_AUDIO is the APP's view of "this build drives a second codec on
// SPI2 (MikroBUS-B)".  This is normally AK512; only the capability above admits
// the AK128 J3 wiring.  resolved_transport_config.h translates it once; project
// HAL config and board consumers read only the neutral result.
#if APP_REQ_MIKROB_WM8904 && \
    ((APP_TARGET == APP_TARGET_AK512) || APP_AK128_J3_TDM_B)
  #define APP_USE_SPI2_AUDIO        (1)
#else
  #define APP_USE_SPI2_AUDIO        (0)
#endif

// Physical transport derivations. SPI34_TEST keeps two dense logical codec legs (A/B)
// but maps their descriptors onto physical SPI3/SPI4. HAL spiN() accessors still mean
// literal physical SPIn; application code uses dense inst(0)/inst(1) via board helpers.
#define APP_TDM_BASE_ON_SPI34 \
    ( (APP_TARGET == APP_TARGET_AK512) && \
      (APP_TDM_PORT_MODE == APP_TDM_PORT_MODE_SPI34_TEST) )
#define APP_TDM_USES_SPI34 \
    ( (APP_TARGET == APP_TARGET_AK512) && \
      (APP_TDM_PORT_MODE != APP_TDM_PORT_MODE_SPI12) )
#if APP_TDM_BASE_ON_SPI34
#define APP_TDM_PHYS_A_NUM          (3u)
#define APP_TDM_PHYS_B_NUM          (4u)
#else
#define APP_TDM_PHYS_A_NUM          (1u)
#define APP_TDM_PHYS_B_NUM          (2u)
#endif

// SST26 owns the AK512's SPI4 peripheral and its dedicated PPS/GPIO pin set. It is
// available only while the audio transport stays on SPI1/SPI2. In SPI3/SPI4 modes
// this single resolved gate removes the driver API/implementation, pin setup, tests,
// and the dependent button sound-effect feature from the build.
#if (APP_TARGET == APP_TARGET_AK512) && !APP_TDM_USES_SPI34
  #define APP_USE_SST26              (1)
#else
  #define APP_USE_SST26              (0)
#endif

// Extra third/fourth HAL legs are present only in the future simultaneous mode.
// SPI34_TEST reuses the two existing dense rows, so it must not enable extra legs.
#if APP_TARGET == APP_TARGET_AK512 && (APP_TDM_PORT_MODE == APP_TDM_PORT_MODE_ALL4)
  #define APP_USE_SPI34_AUDIO       (1)
#else
  #define APP_USE_SPI34_AUDIO       (0)
#endif


// (2.5b) APP_USE_SPI2_INDEPENDENT_MASTER -- effective SPI2 independent-clock-domain fact
// (Phase 1). The raw request APP_REQ_SPI2_INDEPENDENT_MASTER only takes effect where the
// SPI2 audio path actually exists; on a board without it (AK128) it resolves to 0 so the
// whole feature (board master pins, SPI2 callback, config override) compiles out cleanly.
#if APP_REQ_SPI2_INDEPENDENT_MASTER && APP_USE_SPI2_AUDIO
  #define APP_USE_SPI2_INDEPENDENT_MASTER (1)
#else
  #define APP_USE_SPI2_INDEPENDENT_MASTER (0)
#endif

// (2.5d) Q27B -- COHERENT FIXED-OFFSET TEST (measurement-only clock re-source, default 0).
// SPI2 stays an INDEPENDENT MASTER (ASRC engine / transport / servo / callbacks UNCHANGED), but its
// transport clock CLKGEN9 is re-sourced from the FRC-derived PLL1 to PLL2 LOCKED to WM8904-A's
// 12.288 MHz clock output on REFI1<-RP16 (RP16 hard-coded, as the code does -- an earlier version of
// this note said RP75). SPI2's generated BCLK then becomes COHERENT (low wander) with A while
// the fixed A:B offset comes from the UNCHANGED SPI2 BRG (~1.108). This isolates whether a fixed
// rate offset ALONE (no async wander) re-excites the 8-13 Hz limit cycle. The CPU/system stays on
// PLL1 (CLKGEN1) -- DSP SYS clock UNCHANGED. Only the dsPIC PLL2 + BRG knobs are used (no WM8904 FLL,
// no jitter injection). Init order matters: A (codec master) must be up first so its clock output
// feeds REFI1, then CLKGEN9<-PLL2, then SPI2 starts (handled in audio_transport_start_route). PWM must be OFF
// (PLL2 is reused here). Default 0.
#define APP_Q27B_COHERENT_OFFSET (0)
#if APP_Q27B_COHERENT_OFFSET && !APP_USE_SPI2_INDEPENDENT_MASTER
  #error "APP_Q27B_COHERENT_OFFSET needs the independent-master ASRC engine (requires APP_USE_SPI2_INDEPENDENT_MASTER=1)."
#endif
#if APP_Q27B_COHERENT_OFFSET && defined(ENA_PWM_AUDIO)
  #error "APP_Q27B_COHERENT_OFFSET reuses PLL2; disable ENA_PWM_AUDIO (PWM off)."
#endif

// (2.5c) APP_B_CODEC_MASTER -- WM8904-B chip is the TDM master, clocked from B's own on-board
// 12.288 MHz XTAL (board jumper B-XTAL -> B-MCLK). dsPIC SPI2 SLAVES to B's BCLK/FS on an
// INDEPENDENT sync domain. This is the OPPOSITE clock owner from APP_USE_SPI2_INDEPENDENT_MASTER
// (there the dsPIC SPI2 generates the clock); the two are mutually exclusive.
// It rides APP_USE_SPI2_AUDIO; ordinary AK128 builds still resolve it to 0, while
// the dedicated AK128 J3 ASRC build uses codec-master SPI2 slave pins.
// DERIVED (not a user knob): the WM8904-B codec-master request is on ONLY for the ASRC app with
// clock owner = CODEC. Under DEMO it is structurally 0, so a DEMO build can never enter the
// independent B domain (== origin/main classic demo). Opt in via APP_PROFILE=APP_PROFILE_ASRC +
// APP_ASRC_CLOCK_OWNER=APP_ASRC_CLOCK_OWNER_CODEC. Runtime rate command: *ar CC RR; see
// docs_public/wm8904_codec_master_and_rates.md.
#define APP_REQ_B_CODEC_MASTER  ( (APP_PROFILE == APP_PROFILE_ASRC) && \
                                  (APP_ASRC_CLOCK_OWNER == APP_ASRC_CLOCK_OWNER_CODEC) )
#if APP_REQ_B_CODEC_MASTER && APP_USE_SPI2_AUDIO
  #define APP_B_CODEC_MASTER            (1)
#else
  #define APP_B_CODEC_MASTER            (0)
#endif

// (2.5e) APP_B_INDEP_DOMAIN -- axis-1 umbrella: leg B is an INDEPENDENT (async) ASRC domain,
// regardless of WHO drives its clock (dsPIC SPI2 master = APP_USE_SPI2_INDEPENDENT_MASTER, or
// WM8904-B chip master = APP_B_CODEC_MASTER). This gates the ASRC engine / cross-domain data path /
// B-callback route dispatch / "not co-clocked" guards. The clock-GENERATION topology (leg clock
// role, BRG divider, board master/slave pins, MCLK + FS/BCLK passthrough routing) stays keyed on
// the specific master flag, NOT this umbrella. With APP_B_CODEC_MASTER=0 this is identical to
// APP_USE_SPI2_INDEPENDENT_MASTER, so the umbrella rename is a no-op for the existing configs.
#define APP_B_INDEP_DOMAIN  (APP_USE_SPI2_INDEPENDENT_MASTER || APP_B_CODEC_MASTER)

#if APP_B_CODEC_MASTER && APP_USE_SPI2_INDEPENDENT_MASTER
  #error "APP_B_CODEC_MASTER and APP_USE_SPI2_INDEPENDENT_MASTER are mutually exclusive (one clock master per B sync domain)."
#endif
#if APP_B_CODEC_MASTER && !APP_USE_SPI2_AUDIO
  #error "APP_B_CODEC_MASTER requires APP_USE_SPI2_AUDIO with WM8904-B."
#endif
#if APP_B_CODEC_MASTER && APP_Q27B_COHERENT_OFFSET
  #error "APP_B_CODEC_MASTER (B on its own XTAL = truly independent) is incompatible with APP_Q27B_COHERENT_OFFSET (A-locked coherent offset)."
#endif


// (2.5f) Block size (APP_BLOCK_FRAMES) + DRC-test working-buffer sizing.
//       RELOCATED here (from the old (2.3) slot) because the regular-mode AK512 block size is now
//       gated by APP_B_INDEP_DOMAIN (the ASRC engine, resolved just above at (2.5e)): the ASRC
//       build uses a smaller block (RAM savings); the classic co-clock demo keeps 32 (CMSIS
//       batching). APP_BLOCK_FRAMES is set on BOTH sides of the DRC switch, so it is always
//       defined -- no separate fallback guard is needed. The DRC side ALSO sizes the
//       biquad-cascade / sample-delay working buffers (those DSP stages are DRC-only, enabled in
//       (2.6)); the regular side needs only the block size.
#if ENA_DRC_DF2T_CASCADE
//
// DRC test mode -- block size + DSP working buffers
//
 #if 0
  #define APP_BLOCK_FRAMES              (1)           // frames per ping/pong half
  #define BIQUAD_CASCADE_4CH_NUM_STAGE  (22)          // cmsis opt v1 & o3 OK
//  #define BIQUAD_CASCADE_4CH_NUM_STAGE  (6)           // cmsis opt v1 & o3 OK

  #define AUDIO_SAMPLE_DELAY_POOL_BYTES  (41000)      // in case of using far memory

 #else
  #define APP_BLOCK_FRAMES              (32)          // frames per ping/pong half

  // BIQUAD_CASCADE_4CH_NUM_STAGE -- the ONE place this value is chosen, and the one place the
  // stage-count history is kept. Do not re-introduce a commented ladder of past values next to
  // the #define (it used to live here AND in biquad_cascade_4ch.h, which disagreed); the header
  // now only holds a fallback for builds that never reach this file.
  //
  // Stage-count challenge: how many 4-ch DF2T stages fit under 100% load.
  // Pass criterion = hardware measurement, TDMsum peak < 100% with miss=0 (not a clean compile).
  //   48 kHz, block=32, CMSIS opt v1 / -O2, ~1.84 us per stage for 4 ch:
  //     22 long-standing reference   ... 75 OK
  //     80 OK  (TDM1 94.0%, margin 39.9 us)
  //     82 OK  (TDM1 96.5%, margin 23.3 us, TDMsum peak 98.8%)   measured 2026-08-13
  //     83 OK  (TDM1 97.6%, margin 15.9 us, TDMsum peak 99.8%)   measured 2026-08-13
  //     83 OK  (TDM1 97.3%, margin 17.8 us) with the leg-B block-IRQ gate, listened OK
  //     84 <-- current. OK (TDM1 98.7%, margin 8.6 us, miss=0, sat=0), listened OK, on the
  //        gated build; re-measured 2026-08-13. Equals the old record.
  //        81/82 were once noted "NG" in the header; stale.
  //   Reading the numbers: the TDMsum peak saturates near the 666.6 us window (two ISRs can
  //   straddle one window), so past ~99.5% it stops discriminating -- at 84 it sits at the
  //   ceiling in 98% of windows and says nothing. Judge by TDM1's own max and miss=0, and by
  //   listening (see AVAS acceptance: a full listen, not a residual).
  //   Also do NOT read the per-stage cost off TDM1:max -- its absolute value shifts with build
  //   layout (83->84 moved it 9.2 us). The stage cost is in the DSP line: CMSIS-IIR 152.6 ->
  //   154.4 us, i.e. the expected ~1.8 us for one 4-ch stage.
  //   96 kHz, block=32: 22 OK; 39 was tried as the 96 kHz challenge value.
  #if defined(ENA_96K_RATE)
    #define BIQUAD_CASCADE_4CH_NUM_STAGE  (22)
  #else
    #define BIQUAD_CASCADE_4CH_NUM_STAGE  (84)
  #endif //defined(ENA_96K_RATE)

  // Was 25*1024=25600; the AK512 serial-update (resident bootloader) link for this
  // profile has no headroom left in the single 64 KiB data-RAM region for the stack
  // once BSS is placed (elf-ld "Not enough memory for stack", 2112 needed/104
  // available) -- this pool is pure unused headroom (nothing in the tree sets a
  // nonzero default delay; it is only ever configured live via the debug console),
  // so trimming it is free RAM for the linker, not a feature cut.
  #define AUDIO_SAMPLE_DELAY_POOL_BYTES  (22u * 1024u) // 22*1024=22528
//  #define AUDIO_SAMPLE_DELAY_POOL_BYTES  (41000)      // in case of using far memory

 #endif //01

#else
//
// Regular mode -- block size only (DRC working buffers not used here)
//
// The AUDIO ENGINE is the outer gate here, not the part. Block size follows what
// the engine wants to batch, and that does not change between silicon: an
// 8ch<->8ch ASRC wants the same ping/pong half on AK128 as on AK512.
//
// This was keyed on APP_TARGET first, so everything that was not AK512 fell
// through to one AK128 value -- and the AK128 bi-codec ASRC build therefore
// inherited block=4 (83.3 us at 48 kHz), a number chosen for a Classic-1
// low-latency experiment and never decided for ASRC. Found on the first AK128
// bi-codec hardware run, 2026-08-17, from the console's own
// "block=4 ... margin=83.3us" telemetry.
//
// APP_B_INDEP_DOMAIN is the discriminator rather than SONORA_APP_IS_ASRC because
// it is what this file already calls "the ASRC engine gate" (see (2.3) and the
// note above). The two differ only for a build that raises
// APP_REQ_SPI2_INDEPENDENT_MASTER without being the ASRC App -- no such
// configuration exists, and the owner does not expect one (2026-08-17) -- so
// this is the same set in practice, chosen because it also leaves that
// theoretical case on the 16 it had before this restructure rather than moving
// it. Every ASRC build has it set (the boundary invariants below #error out if
// it does not), so no ASRC build can miss this.
 #if APP_B_INDEP_DOMAIN
  // OVERRIDABLE by an ASRC build-config block (included from (2.0) above, so a
  // #define there wins).  The look-ahead one ASRC pull needs is
  // R(step) = floor(step*(APP_BLOCK_FRAMES-1)) + ASRC_POLY_AHEAD + 1, so the CONSUMER's
  // block length -- not the ring depth alone -- is what bounds the rate ratio a given ring
  // can serve (see ASRC_BURST_RATIO_LIMIT_* in audio_app_asrc.c).  A build that has to
  // reach low output rates out of a small ring can trade ISR rate for look-ahead here.
  #ifndef APP_BLOCK_FRAMES
   #define APP_BLOCK_FRAMES        (16)     // async (ASRC) engine, either part
  #endif
 #elif APP_TARGET == APP_TARGET_AK512
  #define APP_BLOCK_FRAMES         (32)     // classic co-clock demo (CMSIS batching)
 #else
  #define APP_BLOCK_FRAMES         (4)      // AK128 Classic: low-latency experiment value
 #endif //APP_B_INDEP_DOMAIN

#endif //ENA_DRC_DF2T_CASCADE


// (2.5e) CLOCK TREE -- FIXED, one configuration per PLL, no per-application variation.
//
//   PLL1 <- FRC, 200 MHz       ALWAYS, every build. CPU / SysCLK / CLKGEN1,6,9.
//   PLL2 <- REFI1, 798.72 MHz  Only where an application needs a clock coherent with the codec,
//                              and always that same configuration.
//
// This is a hardware constraint, not a preference. A PLL only re-locks across a NON-POWER reset to
// the configuration it was already locked to; asking for a different one hangs the PLLSWEN handshake
// and latches it, so the board then needs a power cycle no matter what the firmware does next.
// Measured with mdb on a halted CPU -- see [internal] clock_design_contract.md and
// [internal] clock_design_contract.md appendix A.
//
// Consequences to respect when editing this file:
//   - NO FALLBACK anywhere in a PLL bring-up. A fallback makes two configurations reachable on one
//     board, which is exactly the failure. If the intended reference is absent, report and stop.
//   - A PLL's source is a property of the BOARD, not of the application. Do not reintroduce a
//     per-profile clock "case".
//   - PLL1_CLK_HZ is shared by the whole project. Changing FCY per build would reintroduce the trap.
//   - If CPU jitter ever needs improving, the answer is a system crystal on POSC (a board property),
//     not sourcing the system from the codec's BCLK.
//
// This replaced an APP_CLK_CASE 0..3 axis (per-profile PLL1/PLL2 feeds, with an XTAL-first PLL1
// attempt and an FRC fallback) and the Q48 RA15/RP16 clamp + 3-state-gate machinery. Q48's premise --
// that the 12.288 MHz on RP16 breaks the FRC->PLL1 boot -- was disproved on 2026-07-30: stopping BCLK
// on both codecs (scope-verified) did not fix the boot. The design contract's section 5 records the
// full reasoning and what would justify re-opening it.

// Does this build take SPI2's transport clock from PLL2 (coherent with the codec) instead of PLL1?
// This is a feature flag, not a clock tree variant: PLL2's own configuration is identical either way.
#ifndef APP_SPI2_CLOCK_FROM_PLL2
#define APP_SPI2_CLOCK_FROM_PLL2   (0)
#endif
#define APP_CLK_SPI_ON_PLL2        (APP_SPI2_CLOCK_FROM_PLL2)

// Does this build take the CCP capture time base (CLKGEN13) from PLL2 instead of the FCY peripheral
// clock? Same kind of flag, and the same single PLL2 configuration -- only the consuming CLKGEN
// differs. It buys ABSOLUTE fs accuracy for the ASRC/MEAS rate measurement: with the time base
// derived from the codec crystal instead of the FRC, the measurement's frequency bias is structurally
// zero (99.84 MHz / 12.288 MHz = 8.125 exactly) rather than the FRC's error -- which is measured
// per part and is NOT a constant: +0.66 % on board ...1164, +0.41 % on board ...0057. That is why a
// self-calibration scale is only valid for the board and the boot that measured it. It does NOT
// affect servo stability, which is driven by the FIFO fill error and is immune to time-base error.
//
// Requires codec-A to master BCLK, because that is the configuration in which the codec's crystal
// output reaches REFI1 (RP16). Note what REFI1 actually carries: XTALout, which is fs-INDEPENDENT.
// Measured -- a 48k -> 16k codec-A rate change moved BCLK_DIV 0x0 -> 0x3 while the reading stayed
// exact -- so a rate change cannot invalidate PLL2's fixed input_hz. See the design contract's
// section 9.
#ifndef APP_CCP_TIMEBASE_FROM_PLL2
#define APP_CCP_TIMEBASE_FROM_PLL2 (0)
#endif

#if APP_CCP_TIMEBASE_FROM_PLL2
  #if APP_PROFILE != APP_PROFILE_ASRC
    #error "APP_CCP_TIMEBASE_FROM_PLL2 only means anything for the ASRC profile -- it is the CCP rate measurement's time base."
  #endif
  // The dsPIC-owner presets drive the transport clock themselves, so codec-A is not mastering and
  // its clock output does not reach REFI1: PLL2 has no reference. Note the reference is codec-A's
  // XTALout on REFI1<-RP16, not BCLK -- codec-master mode is the configuration in which that output
  // is present, not the thing being measured. Report at compile time rather than on hardware.
  #if APP_USE_SPI_TDM_CLK_MASTER
    #error "APP_CCP_TIMEBASE_FROM_PLL2 needs codec-A master mode with its XTALout present on REFI1<-RP16 (requires APP_USE_SPI_TDM_CLK_MASTER=0)."
  #endif
  // PWM audio and the Q27B test are the other PLL2 consumers. Sharing PLL2 with them is sound in
  // principle -- it is the same configuration -- but the combination is unexercised, so refuse it
  // here rather than ship an untested clock interaction.
  #if defined(ENA_PWM_AUDIO)
    #error "APP_CCP_TIMEBASE_FROM_PLL2 shares PLL2 with PWM audio; that combination is unexercised. Disable ENA_PWM_AUDIO."
  #endif
  #if APP_Q27B_COHERENT_OFFSET
    #error "APP_CCP_TIMEBASE_FROM_PLL2 combined with APP_Q27B_COHERENT_OFFSET is unexercised; enable only one."
  #endif
  #if APP_SPI2_CLOCK_FROM_PLL2
    #error "APP_CCP_TIMEBASE_FROM_PLL2 combined with APP_SPI2_CLOCK_FROM_PLL2 is unexercised; enable only one."
  #endif
#endif


// (2.6) Feature set selected by the build profile (toggles + device).
//   The DEMO profile gets the full A-side audio-filter demo set below; the ASRC profile defines
//   NONE of it (positive single-definition -- the ASRC engine resamples the raw A input regardless
//   of the A-side DSP, so dropping the demo set frees flash/RAM/CPU and keeps the ASRC app
//   self-contained). Touch is likewise enabled only for the Classic AK512 UI above.
#if APP_PROFILE != APP_PROFILE_ASRC

#if ENA_DRC_DF2T_CASCADE
//
// DRC Test mode
//
#if APP_TARGET != APP_TARGET_AK128
  // AK128MC106 has only 16 KB data RAM; the biquad-cascade and sample-delay
  // working buffers (sample-delay pool alone is ~12.5 KB) do not fit there.
  // These DSP stages are therefore AK512-only. AK512 is unaffected (the guard
  // is true on AK512, so the macro set is identical to before).
  #define ENA_BIQUAD_IIR_CASCADE
  #define ENA_SAMPLE_DELAY
//  #define ENA_FIR_FILTER
#endif //APP_TARGET != APP_TARGET_AK128


#else
//
// Regular mode
//
 // This block compiles only in the Classic Audio Demo profile (APP_PROFILE != APP_PROFILE_ASRC),
 // so the full standard-demo audio-effect set is always enabled here. (The former ASRC-bench RAM
 // reclaim -- guarding ANC_MONITOR/BASS_ENHANCER/WIDEN_CTRL on !APP_ASRC_MEAS -- lived in the ASRC
 // profile, which no longer shares this header; the guards were structurally always-true here.)
 /* ANC monitor: allocate it only when something reads it.
  *
  * ancmon_t is 4,188 B of RAM -- on AK128MC106 that is 44 % of the whole 16 KiB
  * data space -- and its only consumer is the 200 ms readout in
  * apps/classic/classic_demo_app.c, which is gated by ENA_ANC_TEST (off by
  * default, see "--- ANC TEST ---" above).  Defining ENA_ANC_MONITOR
  * unconditionally therefore reserved the struct so that classic_audio_path.c
  * could memset it once at init and nothing could ever look at it: with
  * ENA_ANC_TEST off, --gc-sections drops every ancmon function except
  * ancmon_init(), and the buffers stay.
  *
  * Turning ENA_ANC_TEST on is NOT sufficient to get a reading.  The two feeds,
  * app_ancmon_process_in() and app_ancmon_process_out(), have no caller in the
  * tree at all, so the printed levels, peaks and clip count would all read zero;
  * reviving the monitor means calling them from the block loop in
  * apps/classic/classic_audio_path.c as well.  Budget for it before doing that
  * on AK128: ancmon_est_delay_main() adds a second 4,096 B static snapshot pair,
  * so the working monitor is 8,284 B and does not fit beside the rest. */
#if defined(ENA_ANC_TEST)
 #define ENA_ANC_MONITOR
#endif
 //
 // memory eaters
 //
 /* Additive AVAS sources.  BOTH are built in and selected at run time, so the
  * A/B needs no rebuild:  'a' = TYPE_TY, 'A' = LAMB, mute-button long
  * press = TYPE_TY.  This used to be a build-time choice with an #error, which
  * meant reflashing to compare the two.
  *
  * They are exclusive at RUN TIME, and strictly: while one is sounding -- gate
  * fade included -- the other refuses to start (UsrOperate_avas_synth* in
  * apps/classic/classic_controls.c).  That is a load requirement, not a UI
  * preference: TYPE_TY alone measures 45.8 % of the 666.6 us block window on
  * hardware, so two engines running together could exceed the budget.  With
  * the refusal, peak load is max(TYPE_TY, LAMB), not their sum.  The
  * idle engine costs nothing measurable -- both return 0 immediately when
  * fully gated off.
  *
  * To build only one (flash pressure, or to profile a single engine), comment
  * the other out HERE.  A -Define cannot undefine, so there is no build switch
  * for it.
  *
  * Pass the bare name, with NO value.  Both switches are tested with
  * defined(), which ignores the value, so -Define ENA_AVAS_TYPE_TY_SYNTH=0 reads
  * like "disable" and in fact SELECTS TYPE_TY.  The preprocessor cannot tell an
  * empty expansion from a 0 one, so this cannot be diagnosed here; use the
  * bare name.  Same for every other ENA_* switch tested with defined().
  *
  * AK128 gets TYPE_TY only. Not the "last thing to drop" principle below being
  * reversed -- AVAS itself stays (TYPE_TY) -- but AK128 Classic 1 no longer
  * fits ROM after the hal_pwm HAL migration (2026-08-16),
  * and dropping the second voice's tables (avas_synth_type_lb*.c/.h) recovers
  * far more than the ~350 B still short. */
 #if !defined(ENA_AVAS_TYPE_LB_SYNTH) && (APP_TARGET != APP_TARGET_AK128)
  #define ENA_AVAS_TYPE_LB_SYNTH
 #endif
 #if !defined(ENA_AVAS_TYPE_TY_SYNTH)
  #define ENA_AVAS_TYPE_TY_SYNTH
 #endif

 #if !defined(ENA_AVAS_TYPE_LB_SYNTH) && !defined(ENA_AVAS_TYPE_TY_SYNTH)
  #error "Select at least one additive AVAS source"
 #endif

 /* Sound effects are an AK512 feature set. The AK128 part has 128 KiB of program
  * Flash and has to fit a 32 KiB resident bootloader plus the manifest page
  * inside it, so the demo's decorative sources are the first thing to go there --
  * the same reasoning that already keeps KINKON / CLICK_CLACK / SND_EFFECT_PLAY
  * below AK512-only. AVAS ITSELF is deliberately NOT in here: it is the demo's
  * point, not decoration, and it is the last thing to drop rather than the
  * first. Its second voice (TYPE_LB) is a narrower exception, gated above
  * for ROM, not decided here. */
 #if APP_TARGET != APP_TARGET_AK128
  #define ENA_PINGER_SOUND
 #endif //APP_TARGET != APP_TARGET_AK128

 #define ENA_FLIP4_KEEPALIVE    // JBL Flip4 anti Auto-Mute (functional, not an effect)

 #if APP_TARGET == APP_TARGET_AK512
  #define ENA_BASS_ENHANCER
  #define ENA_WIDEN_CTRL

  #if !defined(ENA_96K_RATE)
  #define ENA_ENGINE_SYNTH      // it needs POT. AK128 doesn't use POT.
  #endif //!defined(ENA_96K_RATE)

  #define ENA_DEESSER
  #define ENA_KINKON
  #define ENA_CLICK_CLACK

  // ASRC-engine builds (APP_B_INDEP_DOMAIN = codec-master or dsPIC-independent-master) don't use
  // the button sound-effect playback; its tone data (Tone_ON/OFF/Notif ~102 KB of const in PROGRAM
  // FLASH, tone_data_int16.c) + the SST26 verify are then dead weight, so define it only when the
  // ASRC engine is OFF -- reclaims that flash independent of profile.
  #if !APP_B_INDEP_DOMAIN && APP_USE_SST26
  #define ENA_SND_EFFECT_PLAY
  #endif //!APP_B_INDEP_DOMAIN && APP_USE_SST26
 #endif //APP_TARGET == APP_TARGET_AK512

#endif //ENA_DRC_DF2T_CASCADE

#endif //APP_PROFILE != APP_PROFILE_ASRC (the ASRC profile defines none of the demo effect set above)


// (2.6b) Bass-enhancer monitor (the "Lv=..." debug line) visibility is a Classic-app concern
// (the monitor is only meaningful when the A-side DSP bass-enhancer runs). Its derivation now
// lives in the Classic app itself (classic_demo_app.c), next to the sole consumer, so the common
// header no longer references the ASRC-only route/measurement terms it used to gate on.


// (2.7) ENA_* tree -> numeric 0/1 APP_* facts. New validation / debug-print code
// should prefer these so it does not need to repeat defined(ENA_*) logic.
// (APP_TARGET is derived earlier, in section (2.0).)


// User-selectable features / profiles
#if defined(ENA_96K_RATE)
  #define APP_USE_96K_RATE              (1)
#else
  #define APP_USE_96K_RATE              (0)
#endif

#if defined(ENA_USB_AUDIO_IN)
  #define APP_USE_USB_AUDIO_IN          (1)
#else
  #define APP_USE_USB_AUDIO_IN          (0)
#endif

// ENA_DRC_DF2T_CASCADE is defined directly in the user-config section above.


// (APP_USE_SPI2_AUDIO is defined above in (2.5) directly from the app's own condition.)


// Effective numeric geometry
// APP_SLOTS_PER_FS and APP_BLOCK_FRAMES are defined directly in sections (2.4) and (2.3).
#define APP_SAMPLE_RATE_HZ              (SAMPLE_RATE)

// Sample-rate POLICY for this product. This is NOT a HAL property -- the SPI/I2S/TDM
// transport is rate-agnostic (runs at whatever BCLK/FS the clock provides). It is the
// supported-rate set of THIS board/application (the external USB-Audio board's clock
// family): currently the 48 kHz family (48k native, 96k via the dual-codec I2S path).
// Used by the Validation section (compile-time check of APP_SAMPLE_RATE_HZ) and by the
// CMSIS-SAI wrapper to validate an ARM_SAI AUDIO_FREQ request.
#define APP_SAMPLE_RATE_IS_SUPPORTED(hz)   (((hz) == 48000u) || ((hz) == 96000u))
#define APP_STAGE1_PROC_CH              (STAGE_1_PROC_CH)
#define APP_STAGE2_PROC_CH              (STAGE_2_PROC_CH)


// CMSIS-SAI verification modes
#if defined(ENA_SAI_WRAPPER_DRYTEST)
  #define APP_USE_SAI_WRAPPER_DRYTEST   (1)
#else
  #define APP_USE_SAI_WRAPPER_DRYTEST   (0)
#endif

#if defined(ENA_SAI_WRAPPER_LIVE)
  #define APP_USE_SAI_WRAPPER_LIVE      (1)
#else
  #define APP_USE_SAI_WRAPPER_LIVE      (0)
#endif

#if defined(ENA_SAI_WRAPPER_LIVE_TONE)
  #define APP_USE_SAI_WRAPPER_LIVE_TONE (1)
#else
  #define APP_USE_SAI_WRAPPER_LIVE_TONE (0)
#endif

#if defined(ENA_SAI_LIVE_KEEPALIVE)
  #define APP_USE_SAI_LIVE_KEEPALIVE    (1)
#else
  #define APP_USE_SAI_LIVE_KEEPALIVE    (0)
#endif

#if defined(ENA_APP_RAW_BYPASS)
  #define APP_USE_APP_RAW_BYPASS        (1)
#else
  #define APP_USE_APP_RAW_BYPASS        (0)
#endif

#if defined(ENA_SAI_HYBRID_IN_ORIG_OUT_WRAP)
  #define APP_USE_SAI_HYBRID_OUT_WRAP   (1)
#else
  #define APP_USE_SAI_HYBRID_OUT_WRAP   (0)
#endif


/* ============================================================
 * 3. Validation  (reject invalid combinations; create no new config values)
 * ============================================================ */
// Combination checks live here, after the Resolved section has converted the
// scattered ENA_* tree into 0/1 APP_* facts.

// Target device: "exactly one" is now structural -- the #elif/#else+#error in the
// APP_TARGET derivation (Resolved section) already rejects unknown/absent devices.

// App boundary invariants. App identity is selected by APP_BUILD; transport topology
// may vary inside that app but cannot silently turn it into the other application.
#if SONORA_APP_IS_ASRC && (APP_TARGET != APP_TARGET_AK512) && !APP_AK128_J3_TDM_B
  #error "The ASRC App requires AK512 or the explicit AK128 J3 bi-codec capability."
#endif
#if SONORA_APP_IS_ASRC && !APP_B_INDEP_DOMAIN
  #error "The ASRC App requires an independent B clock domain."
#endif
#if SONORA_APP_IS_CLASSIC && APP_B_INDEP_DOMAIN
  #error "Classic Audio Demo variations may not enable the ASRC independent B domain."
#endif


// Audio topology / source conflicts
#if APP_USE_96K_RATE && !APP_USE_SPI2_AUDIO
  #error "ENA_96K_RATE requires a configured second WM8904/SPI2 audio path on dsPIC33AK512."
#endif

#if APP_USE_PWM_AUDIO && (APP_TARGET != APP_TARGET_AK512)
  #error "APP_USE_PWM_AUDIO needs the dsPIC33AK512 target."
#endif

#if APP_TDM_USES_SPI34 && APP_USE_PWM_AUDIO
  #error "SPI3/SPI4 TDM uses DMA4-7; disable PWM audio before selecting an SPI3/4 port mode."
#endif

// (The "SPI3/SPI4 TDM is ASRC-only" check references the ASRC-private APP_B_ROUTE_IS_ASRC and
//  now lives in apps/asrc/asrc_app_validate.h, included from the end of this section.)

#if defined(ENA_SND_EFFECT_PLAY) && !APP_USE_SST26
  #error "ENA_SND_EFFECT_PLAY requires APP_USE_SST26 (button effects are stored in external flash)."
#endif

#if (APP_TDM_PORT_MODE < APP_TDM_PORT_MODE_SPI12) || \
    (APP_TDM_PORT_MODE > APP_TDM_PORT_MODE_ALL4)
  #error "APP_TDM_PORT_MODE is invalid."
#endif

#if APP_TDM_PORT_MODE == APP_TDM_PORT_MODE_ALL4
  #error "ALL4 needs the second TDM8 endpoint pin map and four-leg board system table; use SPI12 or SPI34_TEST for now."
#endif

// WM8904-B (dual codec on MikroBUS-B/SPI2) and PWM audio (PWM5-8) cannot coexist:
// they collide on exactly one pin, RP97 -- used both as PWM5L and as the
// CLC3 -> WM8904-B MCLK fan-out (board/audio/audio.c). The SPI2 data/clock lines
// (RP29/90/89/92) do NOT overlap with the PWM pins; RP97 is the sole conflict, so
// the two output paths are exclusive until PWM5L (or codec-B MCLK) is relocated.
#if APP_USE_PWM_AUDIO && APP_REQ_MIKROB_WM8904
  #error "Audio-out conflict: enable EITHER ENA_MIKRO_B_WM8904 (codec-B) OR ENA_PWM_AUDIO (PWM5-8), not both -- they share MikroBUS-B pin RP97 (PWM5L vs codec-B MCLK). Comment out one in app_specific_config_defs.h."
#endif

#if APP_USE_USB_AUDIO_IN && (APP_TARGET != APP_TARGET_AK512)
  #error "ENA_USB_AUDIO_IN: USB audio DIM connector is only wired on the dsPIC33AK512 Curiosity board."
#endif


// Clock / transport invariants
#if APP_USE_USB_AUDIO_IN && APP_USE_SPI_TDM_CLK_MASTER
  #error "ENA_USB_AUDIO_IN requires SPI-TDM clock slave mode; disable ENA_SPI_TDM_CLK_MASTER."
#endif

#if APP_USE_96K_RATE && !APP_USE_I2S_FORMAT
  #error "ENA_96K_RATE must resolve to I2S format."
#endif

#if APP_USE_I2S_FORMAT && (APP_SLOTS_PER_FS != 2)
  #error "I2S format must resolve to APP_SLOTS_PER_FS == 2."
#endif

#if APP_USE_TDM8_FORMAT && (APP_SLOTS_PER_FS != 8)
  #error "TDM8 format must resolve to APP_SLOTS_PER_FS == 8."
#endif

#if APP_USE_I2S_FORMAT && !APP_USE_1_BIT_DELAY
  #error "I2S format requires APP_USE_1_BIT_DELAY 1."
#endif

#if APP_USE_SPI2_AUDIO && (APP_TARGET != APP_TARGET_AK512) && !APP_AK128_J3_TDM_B
  #error "APP_USE_SPI2_AUDIO requires AK512 or the explicit AK128 J3 TDM-B capability."
#endif

#if APP_USE_SPI2_AUDIO && !APP_REQ_MIKROB_WM8904
  #error "APP_USE_SPI2_AUDIO requires APP_REQ_MIKROB_WM8904 1."
#endif


// SPI2 independent-clock-domain (Phase 1) invariants. This mode makes SPI2 a
// standalone dsPIC TDM master while SPI1 stays codec-A's clock slave, so it needs the
// dual-codec SPI2 path, the AK512 board, and the A-domain in codec-master mode. It is
// incompatible with the 96K split-codec topology (that path is inherently co-clocked:
// A=ADC half, B=DAC half of ONE stream) and with USB-audio-in (A is codec-A master here,
// not the USB clock). TDM8 is required (the BRG constant assumes the 256-BCLK frame).
// (APP_USE_SPI2_INDEPENDENT_MASTER is already 1 only when APP_USE_SPI2_AUDIO is 1 -- see
// its derivation in the Resolved section -- so the "needs the SPI2 path" case is structural.)
#if APP_USE_SPI2_INDEPENDENT_MASTER
  #if APP_USE_SPI_TDM_CLK_MASTER
    #error "APP_USE_SPI2_INDEPENDENT_MASTER requires the A-domain in codec-master mode (APP_USE_SPI_TDM_CLK_MASTER 0)."
  #endif
  #if APP_USE_96K_RATE
    #error "ASRC profile (APP_USE_SPI2_INDEPENDENT_MASTER) is incompatible with ENA_96K_RATE -- that dual-codec split is co-clocked, not independent. FIX: build with APP_PROFILE=APP_PROFILE_DEMO (turns ASRC off), or drop ENA_96K_RATE."
  #endif
  #if APP_USE_USB_AUDIO_IN
    #error "ASRC profile (APP_USE_SPI2_INDEPENDENT_MASTER) is incompatible with ENA_USB_AUDIO_IN -- A is codec-A master here; USB-audio needs the co-clocked (non-independent) SPI2. FIX: build with APP_PROFILE=APP_PROFILE_DEMO (turns ASRC off), or drop ENA_USB_AUDIO_IN."
  #endif
  #if !APP_USE_TDM8_FORMAT
    #error "APP_USE_SPI2_INDEPENDENT_MASTER (Phase 1) assumes TDM8 (APP_SPI2_MASTER_BRG targets the 256-BCLK frame)."
  #endif
#endif

// (The UART2 long-stream and Q19-eval invariants reference ASRC-private measurement symbols
//  (APP_ASRC_MEAS_UART2_STREAM, APP_MEAS_DIR, APP_B_ROUTE_IS_ASRC, APP_ASRC_Q19_EVAL) and now
//  live in apps/asrc/asrc_app_validate.h, included from the end of this section.)


// Numeric configuration sanity. The supported-rate policy lives in one place
// (APP_SAMPLE_RATE_IS_SUPPORTED above) -- this is the product/board's rate set,
// NOT a HAL property.
#if !APP_SAMPLE_RATE_IS_SUPPORTED(APP_SAMPLE_RATE_HZ)
  #error "APP_SAMPLE_RATE_HZ is not in this product's supported-rate set (APP_SAMPLE_RATE_IS_SUPPORTED)."
#endif

#if (APP_BLOCK_FRAMES <= 0)
  #error "APP_BLOCK_FRAMES must be positive."
#endif

#if (APP_STAGE1_PROC_CH <= 0)
  #error "APP_STAGE1_PROC_CH must be positive."
#endif

#if (APP_STAGE2_PROC_CH <= 0)
  #error "APP_STAGE2_PROC_CH must be positive."
#endif


// SAI verification harness consistency
#if APP_TDM_BASE_ON_SPI34 && \
    (APP_USE_SAI_WRAPPER_LIVE || APP_USE_SAI_WRAPPER_DRYTEST)
  #error "CMSIS-SAI Driver_SAI0 is bound to physical SPI1; SPI3/4 test mode is unsupported."
#endif

#if ((APP_USE_APP_RAW_BYPASS + APP_USE_SAI_HYBRID_OUT_WRAP) > 1)
  #error "SAI verify: enable at most ONE of ENA_APP_RAW_BYPASS / ENA_SAI_HYBRID_IN_ORIG_OUT_WRAP."
#endif

#if APP_USE_SAI_HYBRID_OUT_WRAP && !APP_USE_SAI_WRAPPER_LIVE
  #error "SAI verify: ENA_SAI_HYBRID_IN_ORIG_OUT_WRAP requires ENA_SAI_WRAPPER_LIVE."
#endif

#if APP_USE_APP_RAW_BYPASS && APP_USE_SAI_WRAPPER_LIVE
  #error "SAI verify: ENA_APP_RAW_BYPASS is the demo path -- disable ENA_SAI_WRAPPER_LIVE."
#endif

#if APP_USE_SAI_WRAPPER_LIVE_TONE && !APP_USE_SAI_WRAPPER_LIVE
  #error "SAI verify: ENA_SAI_WRAPPER_LIVE_TONE requires ENA_SAI_WRAPPER_LIVE."
#endif

#if APP_USE_SAI_LIVE_KEEPALIVE && !APP_USE_SAI_WRAPPER_LIVE
  #error "SAI verify: ENA_SAI_LIVE_KEEPALIVE requires ENA_SAI_WRAPPER_LIVE."
#endif

// The CMSIS-SAI wrapper live route configures SPI1 as a clock SLAVE (external BCLK/FS). Combining
// it with the self-clocked dsPIC master (APP_USE_SPI_TDM_CLK_MASTER) is contradictory: the master
// path generates no external clock to be "ready", so the stream would report is_running() up on a
// clock that is not actually gated. Reject the nonsensical combo at compile time (F3).
#if APP_USE_SAI_WRAPPER_LIVE && APP_USE_SPI_TDM_CLK_MASTER
  #error "SAI verify: ENA_SAI_WRAPPER_LIVE (SPI1 slave) is incompatible with APP_USE_SPI_TDM_CLK_MASTER (self-clocked master)."
#endif


//===========================================================
// Misc app-wide setup (not configuration values)
//===========================================================

// GPIO is configured through the hal_gpio API (nora_gpio.h): prefer the
// RP-first calls (e.g. nora_gpio_rp_config_digital_output(rp, false)) and
// the packed-pin core for non-RP pins. The old literal (port,bit) GPIO_*
// convenience macros have been removed.


// app_utils.h (COMPILEASSERT, ARRAY_SIZE, biquad_t, app_memcpy etc.) and the
// memset/memcpy ISR-safe overrides live in app_runtime_overrides.h.
// Include that from each app-side .c -- NOT from this config header, so the
// overrides do not reach the HAL core through the resolved compile-time adapters.


// ASRC-app-private validation fragment. Kept out of this shared header so the common layer
// never names an ASRC-private symbol; included only for ASRC builds, after all common and
// ASRC facts above are defined. (Classic builds neither include nor need it.)
#if SONORA_APP_IS_ASRC
#include "apps/asrc/asrc_app_validate.h"
#endif


/* ============================================================
 * 4. Completion marker
 * ============================================================ */
#define APP_SPECIFIC_CONFIG_READY 1

#endif //!APP_SPECIFIC_CONFIG_DEFS_H
