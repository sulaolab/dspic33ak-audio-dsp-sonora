#ifndef SONORA_ASRC_APP_BUILD_CONFIG_H
#define SONORA_ASRC_APP_BUILD_CONFIG_H

/* ASRC App variation expansion.  No Classic Demo feature is selected here. */
#ifndef APP_ASRC_CLOCK_OWNER
  #if (APP_BUILD == APP_BUILD_ASRC_DSPIC_BIDIR) || (APP_BUILD == APP_BUILD_ASRC_DSPIC_LIGHT)
    #define APP_ASRC_CLOCK_OWNER  APP_ASRC_CLOCK_OWNER_SPI2
  #else
    #define APP_ASRC_CLOCK_OWNER  APP_ASRC_CLOCK_OWNER_CODEC
  #endif
#endif

/* Whether the dsPIC SPI2 owns leg B's clock as an INDEPENDENT TDM master.  DERIVED (not a user
 * knob) from (APP_PROFILE, APP_ASRC_CLOCK_OWNER).  Defined here in the ASRC build-config -- and
 * as a 0 stub in the Classic build-config -- so the shared config header reads a neutral symbol
 * (both apps define it) rather than reaching into ASRC-private config.  The effective
 * APP_USE_SPI2_INDEPENDENT_MASTER is still resolved later (only 1 when the SPI2 audio path
 * exists), so an AK128 build silently ignores it. */
#ifndef APP_REQ_SPI2_INDEPENDENT_MASTER
  #if (APP_PROFILE == APP_PROFILE_ASRC) && (APP_ASRC_CLOCK_OWNER == APP_ASRC_CLOCK_OWNER_SPI2)
    #define APP_REQ_SPI2_INDEPENDENT_MASTER (1)   // ASRC app, clock owner = dsPIC SPI2 master
  #else
    #define APP_REQ_SPI2_INDEPENDENT_MASTER (0)   // ASRC with codec-master owner
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_CODEC_A_B_ONLY) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_B_A_ONLY) || \
    (APP_BUILD == APP_BUILD_ASRC_DSPIC_LIGHT) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #ifndef APP_ENA_ASRC_BIDIR
    #define APP_ENA_ASRC_BIDIR  (0)
  #endif
#endif

/* 96 kHz A->B bring-up.
 *
 * Physical transport: WM8904 96 kHz mode is I2S / 2 slots / 32 bit.  That is a
 * hardware fact, not a preference: SYSCLK is the 12.288 MHz crystal, the 96 kHz
 * register set uses CLK_SYS_RATE=128fs with BCLK_DIV=/2, so BCLK = 6.144 MHz and
 * a frame is 6.144M / 96k = 64 BCLK = 2 x 32-bit slots.  A TDM8 frame would need
 * 8 x 32 x 96k = 24.576 MHz, which this SYSCLK cannot produce.  ENA_96K_RATE
 * therefore resolves APP_USE_I2S_FORMAT=1 / APP_SLOTS_PER_FS=2 in the shared
 * config, and three #errors there enforce it.
 *
 * Internal ASRC width stays 8 channels: the physical L/R pair is replicated
 * across all ASRC_CH channels by the established mechanism (asrc_push writes
 * p[c & 1u]) and only channels 0/1 are emitted to the 2 physical slots (asrc_pull
 * writes d[s] for s < APP_SLOTS_PER_FS).  This is the same "compute width wider
 * than the physical slot count" arrangement the 16-channel 48 kHz build already
 * ships; it gives a representative 8-channel workload over real 2-channel audio.
 *
 * Clock: A = ADC-only codec master, B = DAC-only codec master on its own XTAL
 * (B-XTAL -> B-MCLK jumper).  The WM8904 cannot run ADC and DAC together at or
 * above its 88.2 kHz boundary, so at 96 kHz this is one-way A->B and BIDIR is
 * rejected.
 * No PLL/clock-tree change is involved: only the codec's own divisors move.
 */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #ifndef ENA_96K_RATE
    #define ENA_96K_RATE
  #endif
  /* Internal compute width.  8 is what the halved 96 kHz block window can carry:
   * the shipping 16-channel bidirectional 48 kHz build measures 76.6 % of a
   * 333.3 us window, and at 96 kHz that window is 166.7 us. */
  #ifndef ASRC_CH
    #define ASRC_CH  (8u)
  #endif
  /* Near-unity 96k->96k starts at the FIFO centre.  The default +28 pre-bias is
   * calibrated for the direct 48k->43.4k path (see asrc_app_config.h), which
   * this is not.  Same reasoning as the 48->8 integration preset. */
  #ifndef APP_ASRC_FAST_ACQUIRE_OFFSET
    #define APP_ASRC_FAST_ACQUIRE_OFFSET (0)
  #endif
  /* Leg B is not pinned to 96 kHz -- `*ar` moves it, and below ~22 kHz the direct step is large
   * enough that the ring cannot hold the look-ahead one pull needs (R+jitter 110 at 16 kHz, 200 at
   * 8 kHz, against ASRC_FILL_TARGET_MAX = 104), so the setpoint clamps and the block's tail outputs
   * emit zeros -- audible break-up.  The runtime front end fixes exactly that: a 21-tap 96 -> 48 kHz
   * pre-stage plus the existing 48 kHz chain puts the resampler back at step ~1.0.  At 96k/96k and
   * every rate from 22.05 kHz up the gate selects den 1, so the unity operating point this preset
   * was calibrated for is untouched.  See
   * [internal] asrc_96k.md part 3. */
  #ifndef APP_ASRC_RUNTIME_48K_TO_8
    #define APP_ASRC_RUNTIME_48K_TO_8 (1)
  #endif
  /* Bring-up needs the load/deadline telemetry that the standard BIDIR preset
   * enables, since confirming CPU load and block-deadline margin is the point. */
  #define APP_ASRC_HEADROOM_INSTRUMENT  (1)
  #define APP_ASRC_LED_FRAME_STRIDE     (16u)
  #define APP_ASRC_HEADROOM_DBG_PERIOD_MS (10000u)
  /* Reject startup-transient CCP estimates before the near-unity servo engages,
   * exactly as the other near-unity (48->8 integration) path does. */
  #ifndef APP_ASRC_FF_ACQUIRE_GUARD
    #define APP_ASRC_FF_ACQUIRE_GUARD (1)
  #endif
#endif

/*
 * AK128 bi-codec bring-up profile.
 *
 * Each ASRC object carries one physical TDM8 frame without the normal
 * stereo-replication workload: slots 0..7 map one-to-one to ASRC channels
 * 0..7.  There are two objects, A->B and B->A, so the bidirectional target is
 * 8ch <-> 8ch (16 processing channels total).
 *
 * Polyphase, at the AK512 M30 production filter (M=30 taps, fc=0.465, Kaiser
 * beta=11) and, since the resident bootloader moved to a 16 KiB region on
 * 2026-08-20, at the AK512 phase count as well, with the coefficients resident
 * in program flash rather than RAM.  What each choice is forced by:
 *
 *   coefficients in flash  A RAM-resident table is s_poly[L+1][M] floats: 7.8 kB
 *                          even at L=64, against ~5.4 kB of data memory left
 *                          once the two ASRC objects and the four TDM DMA
 *                          ping-pong buffers are placed.  Flash is the only
 *                          option here, and it costs (L+1)*M*4 bytes of program
 *                          memory instead.
 *   L = 128 (AK512 parity) Purely a program-memory trade: L only sizes the
 *                          table (15,480 B at L=128 against 7,800 B at L=64).
 *                          It touches neither RAM (that is M and the FIFO) nor
 *                          the per-sample tap count, so the phase resolution
 *                          costs nothing in either.  Q43 measured the THD+N
 *                          floor at -127.7 dB for L128 against -106.5 dB for
 *                          L64, and the 108 KiB application region left by the
 *                          16 KiB resident bootloader has room for the bigger
 *                          table, so this build takes it.  L=64 is still a
 *                          supported geometry: set ASRC_POLY_L to 64 and the
 *                          l64m30 table is selected again (it is excluded from
 *                          this MPLAB configuration now, so re-include it).
 *   FIFO = 64 frames       A 128-frame ring would put the two ASRC objects at
 *                          10,112 B of RAM and overflow the part.  Note this
 *                          rules out the fixed-geometry asm producer kernels
 *                          (mchp_asrc_push8_tdm30_f32 has FIFO=128 baked into
 *                          its .equ constants), so the C push path with its
 *                          mirror overhang carries this build.
 *
 * MEAS/capture stays out of the resident AK128 image.  The low-rate decimator
 * front ends did too until 2026-08-20; see the APP_ASRC_RUNTIME_48K_TO_8 block
 * below for why they are in now and what had to come out to fit them.
 */
#if (APP_BUILD == APP_BUILD_ASRC_AK128_CODEC_BIDIR)
  #ifndef ASRC_CH
    #define ASRC_CH  (8u)
  #endif
  #ifndef APP_ASRC_INTERP
    #define APP_ASRC_INTERP  (ASRC_INTERP_POLY)
  #endif
  /* Same filter and the same phase count as the AK512 M30 production profile
   * below.  ASRC_POLY_L is still spelled out here rather than left to fall
   * through to asrc_app_config.h's L=128 HIFI default, so that this profile's
   * geometry reads in one place and does not move if that default does.
   * The window is spelled as a literal for the same reason the M30 block below
   * does: the ASRC_WINDOW_* names are not in scope yet. */
  #ifndef ASRC_POLY_M
    #define ASRC_POLY_M  (30u)
  #endif
  #ifndef ASRC_POLY_L
    #define ASRC_POLY_L  (128u)
  #endif
  #ifndef ASRC_POLY_FC
    #define ASRC_POLY_FC  (0.465f)
  #endif
  #ifndef ASRC_POLY_WINDOW
    #define ASRC_POLY_WINDOW  (2) /* ASRC_WINDOW_KAISER_11 */
  #endif
  /* Coefficients live in program flash: audio_app_asrc_poly_l128m30_flash.c,
   * generated by tools/gen_asrc_poly_flash_table.py for exactly the four values
   * above.  audio_app_asrc.c cross-checks the table's element count against
   * ASRC_POLY_L/M, but nothing can check fc or the window -- regenerate the
   * table if either changes. */
  #ifndef ASRC_COEFF_STORAGE
    #define ASRC_COEFF_STORAGE  (ASRC_COEFF_STORAGE_FLASH)
  #endif
  #ifndef APP_ASRC_FIFO_FRAMES
    #define APP_ASRC_FIFO_FRAMES  (64u)
  #endif
  /* FILL SETPOINT SLACK -- measured, not assumed.
   *
   * The setpoint is R(step) + ASRC_FILL_JITTER and the ceiling is
   * ASRC_FILL_TARGET_MAX = FIFO-4-BLOCK-JITTER, so the slack appears on BOTH bounds and
   * a value only fits while  2*JITTER <= FIFO - BLOCK - 20 - floor(step*(BLOCK-1)).  For the
   * worst Main-profile ratio (48/44.1, step 1.08844) that is 2*J <= 12, i.e. J <= 6.
   *
   * 4 was too small, and by exactly the amount this raises it.  MEASURED 2026-08-20 on an AK512
   * board over ~4 minutes of 48<->44.1 with a min-hold on the pull-start fill
   * (fmin= in the telemetry), 118 print windows per leg:
   *
   *     leg           R    set   worst fmin   set-fmin   fmin-R
   *     AB 48->44.1   32   36        31          5         -1
   *     BA 44.1->48   29   33        28          5         -1
   *
   * The servo's downward excursion from its setpoint is 5 frames, so a slack of 4 puts the
   * worst pull ONE frame under the window it needs and that pull holds its previous frame
   * (starve, ~2/s per leg).  6 puts it one frame above.  It is the ceiling above, so this
   * ring has no more to give: a rarer excursion of 7 would starve again, and the robust fix
   * stays FIFO=128 (which lifts the ceiling to 38).
   *
   * NOTE the earlier reasoning this replaces: the fill spread seen in ordinary telemetry
   * (p-p 17 against BLOCK 16) suggested the excursion was a full block phase, +/-8, which
   * would have needed J>=8 and could not fit.  That spread is an artefact of sampling one
   * asynchronous fill per print -- the worst PULL is 5 down, not 8.
   */
  #ifndef ASRC_FILL_JITTER
    /* 8 since 2026-08-20: at block 8 the bound above becomes 2J <= 29, so the ceiling is 14
     * and 8 is a middle value that keeps the low rates unclamped (24 kHz needs R+J = 38
     * against a ceiling of 44).  At 44.1 it is not binding -- FIFO/2 = 32 is larger. */
    #define ASRC_FILL_JITTER  (8u)
  #endif
  /* SLACK THE *ar PAIR GATE REQUIRES -- this ring's number, not AK512's.
   *
   * audio_app_asrc.c defaults ASRC_FILL_SLACK_REQUIRED to 8, measured on AK512 (FIFO 128,
   * block 16) over all 100 pairs.  The quantity is the servo's worst downward excursion from
   * its setpoint plus one, so it belongs to the ring geometry and has to be restated here.
   *
   * 7 for this ring: the block-8 measurement in the ADDENDUM below reports
   *     AB 48->44.1  R=23 set=32 fmin>=28   excursion set-fmin = 4
   *     BA 44.1->48  R=22 set=32 fmin>=26   excursion set-fmin = 6
   * so the worst excursion seen is 6 and a pull needs fmin >= R+1, i.e. slack >= 7.
   *
   * NOT 6.  6 is the block-16 number (the table above the ADDENDUM: set-fmin = 5), and block
   * 16 is retired on this profile -- carrying 6 forward would be citing a measurement of a ring
   * this build no longer has.
   *
   * The value is not load-bearing on this ring, which is why one rate pair's worth of evidence
   * is enough to write it down: tools_local/predict_gate_ak128.py sweeps the gate over 6, 7 and
   * 8 and all three accept the same 67 of 81 pairs.  The clamp at ASRC_FILL_TARGET_MAX = 44
   * leaves no pair with a slack between 3 and 8, so the only pairs the constant newly refuses
   * are 44.1 <-> 12 kHz (R=41, set=44, slack=3) at any of the three values.  Whether those two
   * run clean today is the one thing an AK128 hardware sweep still has to answer.
   */
  #ifndef ASRC_FILL_SLACK_REQUIRED
    #define ASRC_FILL_SLACK_REQUIRED  (7u)
  #endif
  /* CONSUMER BLOCK LENGTH -- the low-rate reach knob on this compact ring.
   *
   * One pull drains a whole block with the producer's ISR locked out, so it needs
   *   R(step) = floor( step * (APP_BLOCK_FRAMES - 1) ) + ASRC_POLY_AHEAD + 1
   * frames of look-ahead at its start, and R has to stay under the producer's overflow
   * guard ASRC_FIFO_FRAMES-4 = 60.  With the fleet default of 16 that caps this ring at
   * fs_in < 3*fs_out: 22.05 kHz against a 48 kHz peer, but NOT 16/12/11.025/8 kHz.
   * Halving the block halves the rd advance one pull makes, so it halves R:
   *
   *     out rate    step     R(16)   R(8)   guard   fill target max
   *     16000       3.0000     61      37     60     40 (16) / 48 (8)
   *     12000       4.0000     76      44     60
   *     11025       4.3537     81      46     60
   *      8000       6.0000    106      58     60
   *
   * Deepening the ring instead is not available here: 64 -> 128 frames costs
   * 64 x ASRC_CH x 4 B x 2 engines = 4096 B of data memory and only ~3.5 KB is free.
   * A decimating front end (the AK512 answer, APP_ASRC_RUNTIME_48K_TO_8) is not either:
   * ASRC_DECIMATOR_48_TO_8_MAX_CHANNELS is 2 and this build is 8-channel TDM one-to-one,
   * so both its history and its ISR time would be 4x what that path was priced at.
   *
   * MEASURED on an AK512 board, 2026-08-20, this build at L=128/M=30 (block 8 built
   * with -Define APP_BLOCK_FRAMES=8; see the study report):
   *
   *     block 16                      block 8
   *     program 100,012               program 100,012      (identical -- no code delta)
   *     data    12,862 free 3,522     data    10,814 free 5,570   (-2,048 B: DMA halves)
   *     48/48 kHz  TDMsum 66.4 %      48/48 kHz  TDMsum 76.0 % margin 39.9 us, sat=0
   *     pull 81.1 us                  pull 41.0 us
   *     *ar0101 REFUSED               *ar0101 runs: fill 51..58/64 set=48! step 4.35390,
   *                                     miss=0 over 6.7e5 blocks, TDMsum peak 91.1 %
   *     *ar0100 REFUSED               *ar0100 runs: fill 58..59/64 (guard 60 -- no margin),
   *                                     step 6.00057, miss=0, TDMsum peak 84.5 %
   *
   * So the look-ahead wall is cheap to remove and the ring even GAINS 2 KB.  What block 8
   * does NOT buy is anti-aliasing: both directions still report fe=direct, i.e. the poly
   * cutoff stays at ASRC_POLY_FC of the INPUT rate, so everything between the output
   * Nyquist and 0.465*48 kHz folds back.  That is a listening decision, not a telemetry
   * one, and it has precedent -- the 22.05 kHz pair this build already ships and has
   * passed a listening test is fe=direct too.
   *
   * (SUPERSEDED 2026-08-20 -- the default is now 8; the addendum after this block records
   * the owner decision and the re-measured margin.)  Block 8 costs 72 us of TDMsum margin on the primary 48/48 kHz
   * configuration (111.9 -> 39.9 us) and that configuration is the one under listening
   * approval, so the trade is the owner's to make, not this file's.  Flip by predefining
   * the macro here or with a one-shot -Define APP_BLOCK_FRAMES=8.
   */
  /* ADDENDUM 2026-08-20 -- DEFAULT IS NOW 8 (owner decision, after listening).
   *
   * What decided it was not the low-rate reach above but STARVE on the Main profile.  At
   * block 16 the look-ahead R(48/44.1) is 32 against a setpoint of 36-38, and the pull-start
   * fill floor sits at R-1 -- the starve path itself enforces that floor, because a starved
   * frame holds rd and recovers the one frame that was missing.  So the setpoint could not
   * buy its way out: raising ASRC_FILL_JITTER 4 -> 6 (the ceiling this ring allows at block
   * 16) only halved the rate, 1.81 -> 0.96 /s on AB and 2.39 -> 1.43 /s on BA.
   *
   * Block 8 drops R to 23, which puts the floor ABOVE what a pull needs, and starve stops:
   *
   *     48<->44.1, block 8      AB: R=23 set=32 fmin>=28 (+5)  starve 0.00 /s  drop 0 /s
   *                             BA: R=22 set=32 fmin>=26 (+4)  starve 0.00 /s  drop 0 /s
   *     24 kHz                  clamp LIFTS (set 40! -> 38), starve 4.2 -> 0.02 /s
   *
   * Note the setpoint lands on ASRC_FILL_TARGET (= FIFO/2 = 32) at 44.1, not on R+JITTER=31,
   * so BLOCK is what fixed the Main profile -- the jitter slack below only binds where
   * R+JITTER exceeds FIFO/2, i.e. at the low rates.
   *
   * RE-MEASURED MARGIN (this build, with the fmin instrument): the 39.9 us above was taken
   * before it.  Worst observed is 27.7 us at 48/48 (83.3 %, bound 85.2 %) and 20.8 us at
   * 24 kHz (87.5 %, bound 90.0 %), sat=0 in both.  The owner accepted ~40 us; these are
   * tighter, so treat the low-rate margin as the number to watch if more load is added.
   *
   * Listening: 48/48 passed a full-length audition plus a deliberately awkward sine sweep,
   * and 22.05 kHz down to 8 kHz showed none of the grit the clamped block-16 build had.
   * 8 kHz is NOT qualified by that -- it is AM-radio material and still clamped here
   * (R=58 against a ceiling of 44); its handling is a separate discussion.
   * Evidence: [internal] report_ak128_crackle_and_fifo128_2026-08-20.md sections 14-15.
   */
  #ifndef APP_BLOCK_FRAMES
    #define APP_BLOCK_FRAMES  (8)
  #endif
  #ifndef APP_ASRC_TDM8_ONE_TO_ONE
    #define APP_ASRC_TDM8_ONE_TO_ONE  (1)
  #endif
  #ifndef APP_ASRC_MEAS
    #define APP_ASRC_MEAS  (0)
  #endif
  /* MEASUREMENT (DR / THD+N) on this profile is a -Define, not a separate preset:
   *
   *     buildtools/build.ps1 -Define APP_ASRC_MEAS=1
   *
   * audio_app_meas.c and audio_app_meas_tones.c are wholly inside #if APP_ASRC_MEAS, so
   * this configuration LINKS them unconditionally and they cost nothing at MEAS=0 -- which
   * is exactly how the AK512 ASRC configuration has always carried them (it lists no
   * exclusion for either file).  Nothing about the audio path changes, so the measured
   * resampler is the shipped resampler.
   *
   * The two things that must change on a 16 KB part are set here so the recipe stays a
   * single define:
   *
   *  - One-way A->B.  APP_ENA_ASRC_BIDIR=0 drops ASRC_ENGINE_COUNT to 1, which frees
   *    3,062 B of s_asrc[].  That is where the capture buffer comes from.  It also matches
   *    what the tone harness actually does: the sine is injected into the A-domain input
   *    and the capture is taken at the B-domain output, so the B->A engine is dead weight.
   *  - A shorter capture.  512 samples = 2,048 B, which leaves the stack allowance THICKER
   *    than the shipping bidirectional image (see report_ak128_dr_thdn_readiness_2026-08-19).
   *    Cost is about 6 dB of FFT processing gain against the 2048 AK512 uses; raise it with
   *    -Define APP_MEAS_CAP_LEN=768 (or 1024) if the noise floor needs it and the stack
   *    allowance still looks sane in the .map.
   */
#if APP_ASRC_MEAS
  #ifndef APP_ENA_ASRC_BIDIR
    #define APP_ENA_ASRC_BIDIR  (0)
  #endif
  #ifndef APP_MEAS_CAP_LEN
    #define APP_MEAS_CAP_LEN  (512u)
  #endif
  /* No BCLK dedicated-timer observer: it wants two spare 32-bit external-count timers and
   * the AK512 BCLK RPs.  AK128 has no T2/T3 pair at all.  Observer only -- nothing in a
   * DR / THD+N measurement reads it. */
  #ifndef APP_MEAS_Q11_BCLK_OBSERVER
    #define APP_MEAS_Q11_BCLK_OBSERVER  (0)
  #endif
  /* No control-variable trace (*ag / ?ag).  Its Q34 buffer alone is 16 KB -- the whole part -- and
   * its two printf bodies are ~4.4 KB of program memory.  It is a servo diagnostic: DR and THD+N
   * read s_cap[] through *ac / ?ac and never touch it.  This is what makes a MEAS image fit. */
  #ifndef APP_MEAS_CTRL_TRACE
    #define APP_MEAS_CTRL_TRACE  (0)
  #endif
#endif
  /* TEMPORARY AK128 BASELINE (2026-08-25): retain TDM8, eight independent
   * channels and bidirectional ASRC, but qualify only 32 / 44.1 / 48 kHz.
   * The low-rate Q31 front end costs 6,080 B for its ASRC_CH-wide history on
   * this 16 KB part.  It is deliberately out of this image; the console
   * rejects every rate that would need it before tearing a stream down. */
  #define APP_ASRC_AK128_BASELINE_RATE_ONLY  (1)
  #ifndef APP_ASRC_RUNTIME_48K_TO_8
    #define APP_ASRC_RUNTIME_48K_TO_8  (0)
  #endif
  /* The boot bit-exactness selftest of those chains does NOT fit: the application region is
   * 0x1B000 = 110,592 B and the front end asks for about 10.7 kB against ~10.6 kB free, so the
   * first build with it overflowed the program region.  The oracle plus its six per-chain
   * drivers are ~2.9 kB of that, and they are a startup check rather than an audio-path
   * feature.  The check still runs at every boot of the AK512 ASRC image, which compiles the
   * same sources with the same MAX_CHANNELS and the same coefficient tables. */
  #ifndef APP_ASRC_FRONTEND_SELFTEST
    #define APP_ASRC_FRONTEND_SELFTEST  (0)
  #endif
  #ifndef APP_ASRC_48K_TO_8_DECIMATOR
    #define APP_ASRC_48K_TO_8_DECIMATOR  (0)
  #endif
  #ifndef APP_ASRC_48K_TO_8_INTEGRATION
    #define APP_ASRC_48K_TO_8_INTEGRATION  (0)
  #endif
  /* The global +28 startup pre-bias was calibrated for a 128-frame AK512
   * ring.  On the compact 64-frame AK128 ring it lands on the overflow guard,
   * so start at the centred fill target until this hardware is characterized. */
  #ifndef APP_ASRC_FAST_ACQUIRE_OFFSET
    #define APP_ASRC_FAST_ACQUIRE_OFFSET  (0)
  #endif
  /* AK128 does not expose the full CCP fast-map used by the AK512 rate
   * detector.  The profile instead seeds 1.0 at stream reset and derives the
   * A:B feed-forward ratio from both DMA block counters in the foreground;
   * the FIFO-fill servo remains active and no CCP ISR is linked. */
  #ifndef APP_USE_CCP_FS_DETECT
    #define APP_USE_CCP_FS_DETECT  (0)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_CODEC_B_A_ONLY)
  #ifndef APP_ENA_ASRC_FROM_B
    #define APP_ENA_ASRC_FROM_B  (1)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_DSPIC_LIGHT)
  #ifndef APP_ENA_ASRC_LIGHT
    #define APP_ENA_ASRC_LIGHT  (1)
  #endif
#endif

/* The validated M30 headroom profile is now the standard BIDIR product path.
 * Keep profiling, sparse integer LED metering, and the 10 s foreground report
 * exactly as hardware-qualified.  The named HEADROOM presets remain only for
 * reproducible M32/M30 comparison builds. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30)
  #define APP_ASRC_HEADROOM_INSTRUMENT  (1)
  #define APP_ASRC_LED_FRAME_STRIDE     (16u)
  #define APP_ASRC_HEADROOM_DBG_PERIOD_MS (10000u)
#endif

/* The production codec-BIDIR image normally uses the direct A->B ASRC.  At
 * 8/11.025 kHz, the large direct step makes the 16-frame producer/consumer
 * bursts reach the 128-frame FIFO guard and creates audible discontinuities.
 * Compile fixed 48->8 (/6) and 48->16 (/3) anti-alias front ends so stream
 * reset can select a near-unity path at those rates. Other rates stay direct. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR)
  #ifndef APP_ASRC_RUNTIME_48K_TO_8
    #define APP_ASRC_RUNTIME_48K_TO_8 (1)
  #endif
#endif

/* "*aq" front-stage FIR kernel bench: compiled OUT of the shipping BiDir image.
 *
 * It holds 1,184 B of data memory whether or not anyone runs it (1,024 B of X-space
 * coefficients + 160 B of outputs) and 10,188 B of program, and this profile is the
 * one that needs the room: it carries both resampler instances plus the 16-channel
 * Q31 front end. Nothing else changes -- the source stays in the tree and in every
 * MPLAB configuration, so any other profile built from the same sources still has
 * the bench, and its numbers are already recorded in
 * [internal] report_ak512_fir_kernel_measured_2026-08-21.md.
 * In THIS image "*aq" answers ERR_UNSUPPORTED, the same honest answer it gives on
 * the AK128, because the measurement genuinely is not in the image.
 *
 * Overridable: build with -Define ASRC_FIR_KERNEL_BENCH_AVAILABLE=1 to get it back
 * without editing anything. (2026-08-22 RAM work,
 * [internal] report_ak512_asrc_ram_gate_2026-08-22.md.) */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR)
  #ifndef ASRC_FIR_KERNEL_BENCH_AVAILABLE
    #define ASRC_FIR_KERNEL_BENCH_AVAILABLE (0)
  #endif
#endif

#if APP_ASRC_EXPERIMENTAL_M28 && \
    (APP_BUILD != APP_BUILD_ASRC_CODEC_BIDIR) && \
    (APP_BUILD != APP_BUILD_ASRC_CODEC_MEAS)
  #error "APP_ASRC_EXPERIMENTAL_M28 is supported only with the standard BIDIR or MEAS preset"
#endif

/* Standard BIDIR and MEAS use production M30.  M28 is deliberately hidden
 * behind a compile-time developer switch rather than a routine build preset.
 * Host gate: M30 -0.741 dB @20 kHz / -108.5 dB worst image; experimental M28
 * -0.928 dB / -105.3 dB. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #if APP_ASRC_EXPERIMENTAL_M28
    #define ASRC_POLY_M                   (28u)
    #define ASRC_POLY_FC                  (0.4632f)
    /* Literal window value: the ASRC_WINDOW_* names live in asrc_app_config.h,
     * which app_specific_config_defs.h includes AFTER this header, so they are
     * not in scope here. 2 == ASRC_WINDOW_KAISER_11. */
    #define ASRC_POLY_WINDOW              (2) /* ASRC_WINDOW_KAISER_11 family */
    #define ASRC_POLY_KAISER_BETA         (10.55f)
    #define ASRC_POLY_KAISER_NAME         "poly-k10.55"
  #else
    #define ASRC_POLY_M                   (30u)
    #define ASRC_POLY_FC                  (0.465f)
    #define ASRC_POLY_WINDOW              (2) /* ASRC_WINDOW_KAISER_11 */
  #endif

  /* Coefficient storage for the shipping AK512 bidir profile.
   *
   * The float table is s_poly[ASRC_POLY_L + 1][ASRC_POLY_M] = 129 x 30 x 4 B =
   * 15,480 B of .bss, measured in the serial-update map.  That profile has no
   * spare data RAM (62,396 B of sections plus the stack fill the whole 64 KiB
   * region), so the identical bits are taken from program flash instead:
   * audio_app_asrc_poly_l128m30_flash.c, generated by
   * tools/gen_asrc_poly_flash_table.py for exactly L=128, M=30, fc=0.465,
   * window=2 -- the values resolved above.  The kernel arithmetic is
   * byte-identical; only c0/c1 change where they point.  Cost is about
   * +6.4 pt of the CPU window (flash reads are slower than RAM reads).
   *
   * Deliberately NOT applied to the M28 developer switch (no flash table for
   * that geometry) nor to the MEAS/96K presets, which are not RAM-bound and
   * would rather keep the faster RAM reads.
   *
   * ASRC_COEFF_STORAGE_FLASH itself is defined in audio_app_asrc.c, which is
   * fine: this is a macro body, expanded at the #if in that file, by which
   * point the name is in scope.
   */
  #if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) && !APP_ASRC_EXPERIMENTAL_M28
    #ifndef ASRC_COEFF_STORAGE
      #define ASRC_COEFF_STORAGE  (ASRC_COEFF_STORAGE_FLASH)
    #endif
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_DECIMATOR_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #ifndef APP_ASRC_MEAS
    #define APP_ASRC_MEAS  (1)
  #endif
#endif

/* Direction for the two 96 kHz MEAS presets.  APP_MEAS_DIR is #ifndef-guarded in
 * asrc_app_config.h, so selecting it here needs no edit at the use site.
 *
 * Two presets rather than one runtime switch because B->A (48 k -> 96 k) needs the B->A
 * resampler instance, whose RAM the A->B preset frees for the capture buffer -- measured
 * 89 % vs 82 % of data memory. */
#if (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B)
  #ifndef APP_MEAS_DIR
    #define APP_MEAS_DIR  (MEAS_DIR_AB)
  #endif
#endif
#if (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A)
  #ifndef APP_MEAS_DIR
    #define APP_MEAS_DIR  (MEAS_DIR_BA)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_DECIMATOR_MEAS)
  #ifndef APP_ASRC_48K_TO_8_DECIMATOR
    #define APP_ASRC_48K_TO_8_DECIMATOR (1)
  #endif
  #ifndef APP_ENA_ASRC_BIDIR
    #define APP_ENA_ASRC_BIDIR (0)
  #endif
#endif

#if (APP_BUILD == APP_BUILD_ASRC_48K_TO_8K_INTEGRATION)
  #ifndef APP_ASRC_48K_TO_8_DECIMATOR
    #define APP_ASRC_48K_TO_8_DECIMATOR (1)
  #endif
  #ifndef APP_ASRC_48K_TO_8_INTEGRATION
    #define APP_ASRC_48K_TO_8_INTEGRATION (1)
  #endif
  #ifndef APP_ENA_ASRC_BIDIR
    #define APP_ENA_ASRC_BIDIR (0)
  #endif
  /* Reject startup-transient CCP estimates before enabling the near-unity
   * servo. Keep this integration-specific so legacy ASRC presets retain their
   * established cold-boot latency. */
  #ifndef APP_ASRC_FF_ACQUIRE_GUARD
    #define APP_ASRC_FF_ACQUIRE_GUARD (1)
  #endif
  /* Fixed 3:1 then 2:1 stage: ASRC input rate = source rate * 1/6. */
  #ifndef APP_ASRC_AB_FIXED_RATE_NUM
    #define APP_ASRC_AB_FIXED_RATE_NUM (1u)
  #endif
  #ifndef APP_ASRC_AB_FIXED_RATE_DEN
    #define APP_ASRC_AB_FIXED_RATE_DEN (6u)
  #endif
  /* The legacy +28-frame startup pre-bias was calibrated for the direct
   * approximately-43 kHz path. Near-unity 8 kHz starts at the FIFO centre. */
  #ifndef APP_ASRC_FAST_ACQUIRE_OFFSET
    #define APP_ASRC_FAST_ACQUIRE_OFFSET (0)
  #endif
#endif

#ifndef APP_ASRC_FF_ACQUIRE_GUARD
  #define APP_ASRC_FF_ACQUIRE_GUARD (0)
#endif

/* Direct ASRC paths have no deterministic rate change before the engine. */
#ifndef APP_ASRC_AB_FIXED_RATE_NUM
  #define APP_ASRC_AB_FIXED_RATE_NUM (1u)
#endif
#ifndef APP_ASRC_AB_FIXED_RATE_DEN
  #define APP_ASRC_AB_FIXED_RATE_DEN (1u)
#endif

/* Same pair for the B->A direction. Only the RUNTIME low-rate feature ever raises this
 * above 1 (and then only while leg B is the 48 kHz side); the one-way 48->8 integration
 * preset above deliberately leaves B->A direct. */
#ifndef APP_ASRC_BA_FIXED_RATE_NUM
  #define APP_ASRC_BA_FIXED_RATE_NUM (1u)
#endif
#ifndef APP_ASRC_BA_FIXED_RATE_DEN
  #define APP_ASRC_BA_FIXED_RATE_DEN (1u)
#endif

/*
 * Profiles that keep the SYMMETRIC RX-ISR priorities (rate-monotonic off).
 *
 * These nine had RM off before 2026-08-27, but not by anyone's decision: the
 * APP_ASRC_RATE_MONOTONIC_ISR macro used to be defined inside asrc_audio_path.c's low-rate
 * front-end guard, so every profile that compiles no front end left it UNDEFINED, and `#if`
 * silently read that as 0.  The macro now lives in asrc_app_config.h defaulting to 1, which
 * would have flipped all nine at once.  This block pins them back to what they were measured
 * and recorded under, and turns the accident into a stated choice -- the AK128 bi-codec
 * profile is deliberately NOT listed, because enabling RM there is the change this was all
 * about.
 *
 * The four MEAS / HEADROOM entries are the ones that must not move: RM asymmetry is precisely
 * what makes a per-leg wall-clock reading over-report, so every DR / THD+N / M30-vs-M32 number
 * on file was taken with symmetric priorities.  Flipping RM under them would leave the reports
 * describing a configuration that no longer exists.  The remaining five are pinned for the
 * plainer reason that nobody asked for them to change and no hardware has run them with RM.
 *
 * To qualify one of these WITH rate-monotonic priorities, delete its line here (or build with
 * -Define APP_ASRC_RATE_MONOTONIC_ISR=1) and re-measure -- do not edit a recorded report to
 * match a new number.  Listing them individually rather than reusing one of the shared blocks
 * above is deliberate: APP_BUILD_ASRC_CODEC_BIDIR and the three 96 kHz profiles share those
 * blocks and already ran with RM on, so a shared-block edit would silently switch THEM off.
 */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32) || \
    (APP_BUILD == APP_BUILD_ASRC_DECIMATOR_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_DSPIC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_DSPIC_LIGHT) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_A_B_ONLY) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_B_A_ONLY)
  #ifndef APP_ASRC_RATE_MONOTONIC_ISR
    #define APP_ASRC_RATE_MONOTONIC_ISR  (0)
  #endif
#endif

#if (APP_ASRC_AB_FIXED_RATE_NUM == 0u) || (APP_ASRC_AB_FIXED_RATE_DEN == 0u)
  #error "ASRC fixed-rate plan numerator and denominator must be non-zero."
#endif
#if (APP_ASRC_BA_FIXED_RATE_NUM == 0u) || (APP_ASRC_BA_FIXED_RATE_DEN == 0u)
  #error "ASRC B->A fixed-rate plan numerator and denominator must be non-zero."
#endif

#endif /* SONORA_ASRC_APP_BUILD_CONFIG_H */
