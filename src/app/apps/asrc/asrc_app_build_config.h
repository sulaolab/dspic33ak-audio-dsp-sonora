#ifndef SONORA_ASRC_APP_BUILD_CONFIG_H
#define SONORA_ASRC_APP_BUILD_CONFIG_H

/* UNIVERSAL DEFAULT FOR THE TWO AK512 SHIPPING PRESETS (2026-09-17, owner decision).
 *
 * WHAT THIS IS.  Everything the Universal profile needs -- the Q31 rear, flash coefficient
 * storage, L=256, the 1:1 front-end stage, member B's own pin -- already exists and is
 * already gated on APP_ASRC_UNIVERSAL_PROFILE (and, downstream of it, on
 * APP_ASRC_LPF1TO1_PROTOTYPE).  PR #31 qualified all of it as an opt-in profile.  This block
 * is the ONLY thing that changes: which value those two macros resolve to when nobody passes
 * a -Define, for exactly the two presets that PR #31's own build matrix ran as
 * `bidir_shipping` / `96k_a2b_shipping`.  It is a DEFAULT SELECTION change, not a DSP change --
 * no filter, no coefficient, no pin below this block is touched.
 *
 * WHY THIS IS THE FIRST THING IN THE FILE.  Both macros are tested as VALUES further down in
 * this same file (the BIDIR preset's L=256 pin, member B's whole pin block) -- and this file is
 * read before asrc_app_config.h, which is where the project-wide fallback 0 for both macros
 * lives.  So a preset default has to arrive HERE, ahead of every consumer in this file, or
 * those consumers see an undefined name exactly the way the C0.3 configuration guard (class A
 * ORDER, buildtools/check_config_gate.py) already refused once this PR cycle.  APP_BUILD
 * itself is available this early -- the very next block in this file already switches on it.
 *
 * WHY ONLY THESE TWO PRESETS.  APP_BUILD_ASRC_CODEC_BIDIR is Universal member A;
 * APP_BUILD_ASRC_CODEC_96K_A_TO_B is member B.  No other preset is named here, so no other
 * preset's default moves: AK128, the 96K_12CH_32K stress architecture, MEAS/HEADROOM builds
 * and every Classic build keep reading the project-wide fallback in asrc_app_config.h exactly
 * as before.  Widening the generic fallback instead of pinning these two presets was
 * considered and rejected for that reason -- it would enable Universal on presets it was never
 * qualified against.
 *
 * LPF1TO1_PROTOTYPE FOLLOWS THE RESOLVED PROFILE, NOT A SECOND HARD-CODED 1.  Universal
 * requires it (asrc_app_config.h's own #error says so), so writing a bare `(1)` here would let
 * `-Define APP_ASRC_UNIVERSAL_PROFILE=0` produce the broken state UNIVERSAL_PROFILE=0 with
 * LPF1TO1_PROTOTYPE=1.  Assigning it FROM the (already-resolved) profile macro instead means
 * the escape hatch in requirement 3 keeps working unchanged: an explicit 0 for the profile
 * carries LPF1TO1 down to 0 with it, and only a bare no-define build (or an explicit profile=1)
 * reaches Universal.
 *
 * THE ESCAPE HATCH IS UNCHANGED.  `#ifndef` never overrides an explicit -Define, on either
 * macro, in either direction: -Define APP_ASRC_UNIVERSAL_PROFILE=0 still means Legacy on both
 * presets, and -Define APP_ASRC_UNIVERSAL_PROFILE=1 still means what it always meant. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #ifndef APP_ASRC_UNIVERSAL_PROFILE
    #define APP_ASRC_UNIVERSAL_PROFILE (1)
  #endif
  #ifndef APP_ASRC_LPF1TO1_PROTOTYPE
    #define APP_ASRC_LPF1TO1_PROTOTYPE APP_ASRC_UNIVERSAL_PROFILE
  #endif
#endif

/* THE FOUR ANTI-ALIAS FRONT-END ROWS ARE DEFAULT ON FOR THE BIDIR PRESET (2026-09-20, owner
 * decision after listening).
 *
 * WHAT THIS IS, AND WHAT IT IS NOT.  The four rows -- 22.05 <-> 8, 32 <-> 11.025, 44.1 <-> 12
 * and 44.1 <-> 8 kHz -- already exist, are already implemented, and are already gated
 * (asrc_app_config.h defines each at 0 and states what it adds).  This block is the ONLY thing
 * that changes: which value those four macros resolve to when nobody passes a -Define, for
 * exactly the one preset whose hardware evidence covers them.  It is a DEFAULT SELECTION
 * change.  No filter, no coefficient, no policy, no law and no ring below this block moves --
 * and that is checkable rather than asserted: s_asrc_unsafe_direct_routes[] is unchanged, every
 * pair THAT TABLE LISTS still reads REFUSE on its DIRECT route (a filtered row publishes den != 1
 * and asrc_direct_route_is_allowed() exempts it by ratio), ASRC_POLY_FC is still the selected
 * production value,
 * asrc_fill_law() and ASRC_FILL_TARGET_MAX are untouched, and no telemetry was removed.
 *
 * ★ "EVERY LISTED PAIR" IS NOT "ALL FOUR OF THESE PAIRS", and the difference matters here because
 * this block is where someone will come to check the claim.  Three of the four rows above
 * (22.05 -> 8, 32 -> 11.025, 44.1 -> 12 kHz) ARE in s_asrc_unsafe_direct_routes[].  44.1 -> 8 kHz
 * is NOT, and must not be added: its DIRECT route reads ALLOW from the anti-alias predicate, and
 * what refuses it is the FILL LAW (set_required 116 against TARGET_MAX 104).  The property the four
 * rows share is "the pair does not exist without its filtered row", not "the policy refuses all
 * four".  PR #37 had to correct exactly this claim in antialias_gate_delta.py and PR #38's review
 * caught it again in the status board; test_asrc_rate_plan.py's OPTIN_FRONTEND_ROWS now carries the
 * distinction as data (blocker="alias" vs "fill") and asserts it per row.
 *
 * WHY IT IS THE FIRST THING AFTER THE PROFILE BLOCK.  Same order argument as the block above:
 * the project-wide fallback 0 for all four macros lives in asrc_app_config.h, which the
 * preprocessor reaches AFTER this file, so a preset default has to arrive here, ahead of any
 * consumer.  Each row's own `#if APP_ASRC_ANTIALIAS_* && !APP_ASRC_LPF1TO1_PROTOTYPE` #error
 * sits in asrc_app_config.h and therefore reads both macros AFTER this block has resolved
 * them -- which is what keeps the pairing check honest under this default.
 *
 * WHY ONLY THE BIDIR PRESET.  Its qualification is the qualification these rows have.  The
 * evidence is that preset at 16+16 channels: 81/81 cells accepted / live / every impact counter
 * delta 0, 6 windows x 960 s of long dwell in both orientations per pair, a 48/48 regression
 * either side, worst arena 171 of 209, worst new-row load 72.5 % against the image's own
 * pre-existing worst of 93.3 % (22.05/48, an EXISTING row this work did not touch), and owner
 * listening PASS on 2026-09-20.  No other preset is named here, so no other preset's default
 * moves -- 96K_A_TO_B, the 96K_12CH_32K stress architecture, AK128, every MEAS and HEADROOM
 * build and every Classic build keep reading the fallback 0 in asrc_app_config.h.  Widening
 * the generic fallback instead was considered and rejected for exactly that reason: the arena
 * and CPU evidence above is per-preset, and 96K_12CH_32K in particular ships on ~1.2 pt of CPU
 * window.
 *
 * MEAS IS DELIBERATELY NOT INCLUDED, and this is a trap rather than an omission.  A MEAS build
 * still reads 0, so measuring one of these four pairs on a MEAS image WITHOUT passing the gate
 * measures the REFUSED DIRECT route and reports it as the pair -- which looks healthy in the
 * log.  Pass the gate explicitly and confirm `fe=` in the *ac arm line before trusting a
 * capture.  Naming MEAS here would hide that: a measurement image should measure what it was
 * asked to measure, not inherit a product default.
 *
 * THE ESCAPE HATCH IS UNCHANGED.  All four are #ifndef, so -Define APP_ASRC_ANTIALIAS_...=0
 * returns this preset to the pre-2026-09-20 image, per row, and a build with all four at 0 is
 * the image the tree produced before this block.
 *
 * WHAT IT COSTS.  +908 B of program for the four armed rows (measured: 253,472 -> 254,380 B),
 * and nothing in data memory -- the coefficient sets are const and the front-end history arena
 * is statically sized for the worst row either way.
 *
 * [internal] study_remaining_rate_pairs_20260920.md sections 6 and 8,
 * [internal] report_owner_listening_20260920.md, [internal] status_board_20260920c.md. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR)
  #ifndef APP_ASRC_ANTIALIAS_22K05_TO_8K
    #define APP_ASRC_ANTIALIAS_22K05_TO_8K (1)
  #endif
  #ifndef APP_ASRC_ANTIALIAS_32K_TO_11K025
    #define APP_ASRC_ANTIALIAS_32K_TO_11K025 (1)
  #endif
  #ifndef APP_ASRC_ANTIALIAS_44K1_TO_12K
    #define APP_ASRC_ANTIALIAS_44K1_TO_12K (1)
  #endif
  #ifndef APP_ASRC_ANTIALIAS_44K1_TO_8K
    #define APP_ASRC_ANTIALIAS_44K1_TO_8K (1)
  #endif
#endif

/* The full81 campaign measured six additional lower-rate DIRECT down-conversions aliasing into
 * the protected band.  They are one implementation class: a source-rate 1:1 LPF using arrays
 * already present in both Universal product images.  The 96K_A_TO_B console and pair policy allow
 * these low/low pairs, so that product preset must not silently fall back to the known-bad direct
 * route.  Its v6.30 A/B map adds 2,024 program bytes and no data/Y-history/coefficient storage.
 *
 * MEAS remains explicit: a measurement image measures the gate it was asked to measure.  The
 * #ifndef is the per-preset escape hatch for a controlled pre-fix comparison. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #ifndef APP_ASRC_ANTIALIAS_DIRECT_1TO1
    #define APP_ASRC_ANTIALIAS_DIRECT_1TO1 (1)
  #endif
#endif

/* Nine further direct-route holes (2026-10-03 qualification), same 1:1 class and existing arrays.
 * BIDIR: qualified on MEAS cd951f5 (81 cells, X 14 -> 5) and on the product image
 * cc8d73e (18 directed cells live, RT clean, max_demand <= 87.7 %, 2 x 960 s dwell on 44.1<->24).
 * 96K_A_TO_B (2026-10-05): same rear and coefficient arrays at 8 ch, so the BIDIR alias figures carry;
 * its own image c9f0953 ran all 22 low/low + 96k cells A->B live, RT clean, max_demand <= 34.4 % on
 * these rows, 960 s dwell on 44.1->32; +144 B program with DIRECT5, no data.  #ifndef = escape hatch. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #ifndef APP_ASRC_ANTIALIAS_DIRECT_1TO1_EXT
    #define APP_ASRC_ANTIALIAS_DIRECT_1TO1_EXT (APP_ASRC_ANTIALIAS_DIRECT_1TO1)   /* follows its base gate's opt-out */
  #endif
#endif

/* The last five direct-route holes (2026-10-04): two 1:1 rows + three /2 or /3 + 1:1, existing arrays.
 * BIDIR: qualified on MEAS 4aed8d1 (qual81 RT 81/81, 0 FAIL, alias -112.2..-125.7 dBc on
 * the five) and on the product image a3c41d8 (10 directed cells live, RT clean, max_demand <= 70.2 %,
 * 960 s dwell on 32->12).  96K_A_TO_B qualified with the EXT rows above (2026-10-05, same image).
 * #ifndef = escape hatch. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #ifndef APP_ASRC_ANTIALIAS_DIRECT5
    #define APP_ASRC_ANTIALIAS_DIRECT5 (APP_ASRC_ANTIALIAS_DIRECT_1TO1)   /* follows its base gate's opt-out */
  #endif
#endif

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
 * >>> THE 2-SLOT I2S FRAME IS AN OPERATIONAL ESCAPE FROM TODAY'S CODEC, NOT AN
 * >>> ASRC REQUIREMENT.  READ THIS BEFORE TREATING ANY OF IT AS A SPECIFICATION.
 *
 * Everything narrow about this preset -- 2 slots, one-way A->B, no BIDIR -- is a
 * property of the WM8904 that happens to be on this evaluation board.  None of it
 * is a property of the ASRC, of the DSP, or of the product.  Concretely:
 *
 *   - 2 slots exist because THIS codec's SYSCLK is a 12.288 MHz crystal.  A codec
 *     that can clock a 24.576 MHz BCLK carries TDM8 at 96 kHz with no ASRC change.
 *   - A->B only exists because THIS codec cannot run its own ADC and DAC together
 *     at or above 88.2 kHz.  A codec without that boundary is bidirectional at
 *     96 kHz with no ASRC change.
 *
 * So when a codec with a bidirectional TDM8 96 kHz mode is adopted, BOTH limits are
 * simply REMOVED -- they are not relaxed, negotiated or migrated.  Nothing in the
 * ASRC has to be redesigned for that to happen, and that is exactly the property
 * this comment exists to protect.
 *
 * WHAT THIS FORBIDS, therefore:
 *   - Do not design, size, measure or specify any ASRC stage against "2 channels",
 *     "one direction" or "96 kHz is A->B".  Those are transport facts with a
 *     shelf life, and an ASRC written against them is wrong the day the codec
 *     changes -- silently, because it will still build and still pass.
 *   - Do not let APP_SLOTS_PER_FS or the transport's rate gate reach any stage
 *     between asrc_push and asrc_pull.  See THE CHANNEL-WIDTH AUTHORITY in
 *     asrc_app_config.h; the physical width is confined to the two mapping ends.
 *   - Do not record a number measured at 2 slots as an ASRC capability figure
 *     without saying which of the three constraint families -- codec/HW, ASRC
 *     algorithm, or CPU budget -- produced it.
 *
 * The ASRC's own answer to "how many channels" is ASRC_CH, and it is 8 here for a
 * CPU-budget reason stated below -- deliberately NOT for a codec reason.
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
/* 96 kHz -> 32 kHz, 12 channels, Q31: the whole recipe of the measured configuration.
 * Everything the 2026-09-08 dwell was built with is pinned HERE, so the preset is the
 * only thing anyone has to select.  Before this existed the same image needed ten
 * -Define values and one -AsDefine, and a forgotten one produced a build that ran but
 * was not the thing that had been measured.
 *
 * This block sits ABOVE the shared 96 kHz block below and every knob is #ifndef, so
 * these values win and the preset still inherits the rest of the 96 kHz setup
 * (ENA_96K_RATE, the runtime front end, the headroom instrument) from there.
 *
 * Deliberately NOT pinned: APP_ASRC_STAGE_PROFILE / APP_ASRC_LEG_PROFILE.  Both
 * default to 0 and the measured image had them off; a profiling build passes them on
 * the command line, which is how a probe build should differ from the shipping one.
 *
 * WHY THIS IS NOT A LOAD STUDY.  APP_ASRC_96K_LOAD_STUDY exists to lift the two
 * 96 kHz guards for a bench measurement, and this preset does NOT set it -- it is a
 * selectable configuration, not a measurement, so the guards name it directly in
 * asrc_app_validate.h instead.  The ASRC_CH <= 8 guard states a budget for a 96 kHz
 * leg B (a 166.7 us block); leg B here boots at 32 kHz, so the block is 500 us and
 * the guard's premise does not describe this operating point.
 */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K)
  /* Twelve channels each way.  Measured, not assumed: Hard RT functional PASS with a
   * worst leg B response of 494.1 us against the 500 us block.  The 5 % engineering
   * reserve FAILS at that margin, which is why the tier is internal. */
  #ifndef ASRC_CH
    #define ASRC_CH  (12u)
  #endif
  /* Leg B boots at 32 kHz.  This is not a preference: the 500 us block it produces is
   * what makes 12 channels fit, and every number recorded for this preset assumes it.
   * `*ar` can still move leg B at runtime; above 32 kHz is outside what was measured. */
  #ifndef APP_TRANSPORT_LEG_B_BOOT_RATE_HZ
    #define APP_TRANSPORT_LEG_B_BOOT_RATE_HZ  (32000)
  #endif
  /* Q31 sample path plus the optimised Q31 row kernels and the HB31 half-band
   * pre-stage.  These three are what bought the margin, so they are pinned rather
   * than suggested.  ASRC_SAMPLE_Q31 also forces ASRC_POLY_METHOD to the Q31 kernel,
   * so no ASRC_POLY_METHOD is set here -- setting one would be dead code. */
  #ifndef ASRC_SAMPLE_Q31
    #define ASRC_SAMPLE_Q31  (1)
  #endif
  #ifndef APP_ASRC_Q31_OPT_KERNELS
    #define APP_ASRC_Q31_OPT_KERNELS  (1)
  #endif
  #ifndef APP_ASRC_Q31_PRE_HALFBAND
    #define APP_ASRC_Q31_PRE_HALFBAND  (1)
  #endif
  /* Rear polyphase: M=30, fc 0.464, Kaiser beta 11.3 -- the production geometry.
   *
   * THIS PRESET USED TO PIN M=28 HERE AND IT NEVER TOOK EFFECT.  The pin was
   * `#ifndef APP_ASRC_EXPERIMENTAL_M28 / #define 1`, but apps/app_build_config.h
   * defaults that same switch to 0 at its line 137 and only includes THIS header
   * at its line 275 -- so the macro is always already defined by the time the
   * preset is read, and a preset-level `#ifndef` on it is unreachable by
   * construction.  Every image of this preset has run M30.  Measured, not
   * argued: the board reported `ASRCpath[M=30]`, named the then-current kernel `poly-k11`
   * (the DEFAULT ASRC_POLY_KAISER_NAME -- the M28 arm would say
   * "poly-k10.55"), and a live tap measured -1.323 dB at 13,680 Hz against a
   * host model that gives M30 -1.282 dB and M28 -1.589 dB.
   *
   * So the pin is removed rather than repaired.  M28 bought 2 taps of CPU on
   * the composed front end; the third band bought far more than that, so there
   * is nothing left for it to buy here -- and the whole 2026-09-10 validation
   * (Hard RT, 30-minute soak with 804 M samples at clips=0, listening PASS)
   * was performed on M30.  Changing the filter after that evidence, to reach a
   * geometry nothing needs, would only invalidate the evidence.
   *
   * M28 IS RETIRED, PRESET-WIDE, AS OF 2026-09-12 (see the shared M30-family block
   * further down in this file): `-Define APP_ASRC_EXPERIMENTAL_M28=1` is now a build
   * error on every AK512 M30 preset, this one included, not just unreachable on it.
   * The guard after the M selection below still fails the build if this preset ever
   * resolves to anything but 30, so the signboard cannot drift even if that changes.
   *
   * Host gate for the record: M30 -0.741 dB at 20 kHz / -108.5 dB worst image;
   * M28 -0.928 dB / -105.3 dB. */
  /* Rear phase count: L = 128, PINNED, and this preset is the reason the standard
   * BIDIR preset's L=256 (2026-09-15) had to be a per-preset pin rather than a new
   * generic default.
   *
   * The Q31 coefficient table is (L+1) x M x 4 B and this preset keeps it in RAM
   * (it inherits the generic ASRC_Q31_COEFF_STORAGE default; see asrc_poly_q31.inc),
   * so L=256 would cost +15,360 B of DATA memory: 52,146 B -> 67,506 B against a
   * 65,536 B region.  It does not fit -- not tightly, at all.  Pinning the table into
   * flash instead would fit, and costs +13.2 pt of the CPU window (measured, see
   * asrc_poly_q31.inc), which this preset does not have: its worst leg B response is
   * 494.1 us against the 500 us block, about 1.2 pt.
   *
   * So L=128 here is a resource decision, not a quality one, and it is spelled out
   * rather than inherited so that it does not move if the generic default does.  The
   * guard after the M selection below fails the build if it ever resolves otherwise.
   * [internal] study_ak512_universal_nyquist_prototype_phase1_2026-09-14.md 31.9 */
  #ifndef ASRC_POLY_L
    #define ASRC_POLY_L  (128u)
  #endif
  /* Two engines.  APP_ENA_ASRC_BIDIR defaults to 1 in asrc_app_config.h and this
   * preset is absent from the lists above that force it to 0, so nothing needs
   * defining here -- but the reason it is left alone is not obvious: at 96 kHz the
   * ANALOG path is one-way (the WM8904 has no simultaneous ADC+DAC at or above
   * 88.2 kHz), and running the B->A engine anyway over an idle capture line is
   * deliberate, because that engine costs CPU and the CPU cost is the measurement. */
#endif

#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K)
  #ifndef ENA_96K_RATE
    #define ENA_96K_RATE
  #endif
  /* Internal compute width.  8 is what the halved 96 kHz block window can carry:
   * the shipping 16-channel bidirectional 48 kHz build measures 76.6 % of a
   * 333.3 us window, and at 96 kHz that window is 166.7 us.
   *
   * THIS 8 IS A CPU BUDGET, NOT A CODEC CONSEQUENCE.  It is unrelated to the 2
   * physical slots above: a rate may set workload, never channel count.  Raising it
   * is a block-design and CPU question to be answered on its own evidence -- ON HOLD
   * as of 2026-09-03 by owner decision, behind the 48 kHz Nyquist-region work.
   * EXIT CONDITION: reconsider once the 48 kHz leg's Nyquist-region handling is
   * closed.  Until then do not cite this 8 as an ASRC limit anywhere. */
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
  /* NOTE ON THE LOW-RATE ROWS THIS PRESET ENABLES.  A 96 kHz leg composes the 96 -> 48 kHz
   * pre-stage in front of one of the 48 kHz chains, and since 2026-09-03 that pre-stage is a Q31
   * cascade stage inside the ASRC_CH-wide front end.  So every 96 kHz row this preset publishes is
   * served at the full logical width, and the pair gate accepts them.
   *
   * It was NOT so until then, and the reason is worth keeping: the pre-stage existed only in the
   * legacy float family, which carries ASRC_DECIMATOR_FLOAT_MAX_CHANNELS = 2 -- fewer than this
   * preset's ASRC_CH -- so that family was DISQUALIFIED as a front end and the pairs needing one
   * (leg B at 16 kHz and below, plus the 22.05/24 kHz rows) were refused by the pair gate with a
   * reason.  Refused, not narrowed: filtering two channels while the resampler converts ASRC_CH is
   * a missing anti-alias stage, not a narrower feature.  Porting the pre-stage was the fix, exactly
   * as recorded here; lowering ASRC_CH was never one, and still is not. */
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
 * Polyphase, at the AK512 M30 production filter (M=30 taps, fc=0.464, Kaiser
 * beta=11.3) and, since the resident bootloader moved to a 16 KiB region on
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
    #define ASRC_POLY_FC  (0.464f)
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
  /* FILL SETPOINT SLACK -- measured, not assumed.  (LOWER HALF SUPERSEDED 2026-09-12; see the
   * CORRECTION at the end of this block.  The measurement is kept because it is still the record of
   * what a dense-block-phase pair does, and because it is the measurement the correction reads.)
   *
   * When this was written the setpoint was R(step) + ASRC_FILL_JITTER and the ceiling was
   * ASRC_FILL_TARGET_MAX = FIFO-4-BLOCK-JITTER, so the slack appeared on BOTH bounds and
   * a value only fit while  2*JITTER <= FIFO - BLOCK - 20 - floor(step*(BLOCK-1)).  For the
   * worst Main-profile ratio (48/44.1, step 1.08844) that was 2*J <= 12, i.e. J <= 6.  Since
   * 2026-09-12 the setpoint comes from ASRC_FILL_BLOCK_RESERVE and JITTER holds only the UPPER
   * bound, so that two-sided constraint no longer applies and J is free of the low-rate rows.
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
   *
   * CORRECTION 2026-09-12 -- the retraction just above was right about THIS PAIR and wrong as a
   * general statement, and the difference is the block PHASE, not the sampling.  48 <-> 44.1 kHz is
   * 147/160: 160 phase classes, so the pull-start fill sawtooth is dense and its amplitude really is
   * about BLOCK/2, which is the 5 measured above.  A pair on a LOW-ORDER rational has q classes
   * spaced BLOCK/q, and when the phase slips one producer block the top class loses a whole block:
   * worst = set - BLOCK + (BLOCK/2)(q-1)/q, i.e. a FULL block at q = 1.  So "the worst pull is 5
   * down, not 8" holds for 48 <-> 44.1 and does not generalise -- AK512 32 -> 12 kHz (8/3, q = 3)
   * measured 10 down from a setpoint of 64 and starved.  The lower reserve is therefore derived from
   * BLOCK now (ASRC_FILL_BLOCK_RESERVE), not from this table.  What this table still settles is that
   * 4 frames of UPPER headroom was too small on this ring, which is why J = 8 stays below.
   * [internal] ak512_32to12_starve_analysis_2026-09-11.md.
   */
  #ifndef ASRC_FILL_JITTER
    /* 8 since 2026-08-20.  The original justification was the two-sided bound (at block 8, 2J <= 29
     * -> ceiling 14, and 8 is a middle value that keeps the low rates unclamped).  Since 2026-09-12
     * J is UPPER-bound only, so what 8 now buys is headroom between the setpoint and the producer
     * overflow guard, and its cost is ASRC_FILL_TARGET_MAX = 64-4-8-8 = 44 -- which is exactly what
     * refuses the six low-rate pairs listed below.  Lowering it to 4 would lift the cap to 48, which
     * (checked, not guessed: [internal tool] a1_fill_reserve_gate_delta.py) brings all six back; that is a live option for this ring, but it trades upper headroom the
     * 2026-08-20 measurement said this ring needed, so it is a measurement question, not an edit. */
    #define ASRC_FILL_JITTER  (8u)
  #endif
  /* SLACK THE *ar PAIR GATE REQUIRES -- RETIRED 2026-09-12, and this ring keeps the derived value.
   *
   * This profile used to set ASRC_FILL_SLACK_REQUIRED = 7, reasoning that the required slack is
   * "the servo's worst downward excursion from its setpoint plus one" and measuring that excursion
   * at 6 on 48 <-> 44.1 kHz (the ADDENDUM below).  The measurement was right and the reasoning was
   * incomplete: an excursion measured on a pair with 160 block-phase classes is the DENSE-phase
   * limit BLOCK/2, and a pair sitting on a low-order rational loses a whole producer BLOCK instead.
   * AK512 32 -> 12 kHz = 8/3 (three classes) starved on 8 frames of slack for exactly that reason.
   * The quantity is now derived from the ring geometry -- ASRC_FILL_BLOCK_RESERVE = BLOCK+1 = 9
   * here -- and audio_app_asrc.c #errors if this macro is defined, so a stale override cannot sit
   * here doing nothing.  See asrc_fill_law() and
   * [internal] ak512_32to12_starve_analysis_2026-09-11.md.
   *
   * WHAT THAT COSTS THIS RING, and it is not nothing: FIFO 64 with block 8 has
   * ASRC_FILL_TARGET_MAX = 44, so six pairs whose required setpoint lands at 45-47 are refused
   * where the old gate accepted them -- 22.05 -> 8, 24 -> 8, 32 -> 11.025, 44.1 -> 16, 48 -> 16 and
   * 96 -> 32 kHz.  THREE of those (24 -> 8, 48 -> 16, 96 -> 32) fail this profile's OWN retired
   * constant of 7 once R is corrected to the nominal-integer form, i.e. they were accepted only
   * because the old R under-counted by one frame; the other three fail only the full-block reserve.
   * Every remaining pair keeps its verdict and gains 1-2 frames of setpoint (see
   * [internal tool] a1_fill_reserve_gate_delta.py).  Whether this ring wants those six back -- by
   * measurement on hardware, or by declaring a smaller reserve with a transport argument for why a
   * producer block cannot be deferred here -- is an open AK128 question, not something to answer by
   * restoring a constant whose premise was the dense-phase limit.
   */
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
   * A decimating front end (the AK512 answer, APP_ASRC_RUNTIME_48K_TO_8) was ALSO ruled out here,
   * and that reasoning has since been corrected -- it is kept visible because it is the exact
   * mistake the channel-width authority in asrc_audio_path.c now forbids.  It read: "the float
   * decimator family carries 2 channels and this build is 8-channel TDM one-to-one, so both its
   * history and its ISR time would be 4x what that path was priced at."  A legacy
   * implementation's capacity is not a reason to drop a feature: the Q31 front end carries ASRC_CH
   * by construction at ~1 cycle/MAC, so the 4x was a property of the implementation that was
   * looked at, never of the feature.  Whether the front end fits AK128 is a question to be
   * re-priced against the Q31 family, not one already answered here.
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
   * Nyquist and ASRC_POLY_FC*48 kHz folds back.  That is a listening decision, not a telemetry
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
  /* AK128 ROM-fit product surface: retain TDM8, eight independent channels
   * and bidirectional ASRC, but support the 48 kHz <-> 48 kHz cell only.
   *
   * This is deliberately a compile-time product restriction, rather than a
   * console-only menu filter.  It lets audio_app_asrc.c omit the generic
   * multi-rate admission predicates, while its public pair gate still rejects
   * an out-of-cell request before transport teardown.  The two 48 kHz clocks
   * remain independent, so the ASRC engines and their L128/M30 filter are
   * retained; this does not lower the interpolation geometry or its quality.
   */
  #define APP_ASRC_AK128_FIXED_48K_PAIR  (1)
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

/* PRODUCTION SAMPLE ARM for the shipping AK512 bidir profile: Q31, coefficients in flash.
 *
 * PINNED HERE AND NOT IN THE GENERIC #ifndef (asrc_app_config.h for the arm,
 * asrc_poly_q31.inc for the storage) because those defaults are shared with presets this
 * configuration was never measured against, and a global flip would move two of them
 * silently:
 *
 *   AK128 bi-codec  inherits the generic arm default, so it would go float -> Q31 with no
 *     measurement behind it, on the profile with ~3.5 KB of data RAM to spare.
 *   96K_12CH_32K    already pins ASRC_SAMPLE_Q31 = 1 but inherits the generic RAM storage
 *     default, and it ships on +5.9 us of leg-B wall margin ([internal]
 *     report_ak512_96k32k_12ch_main_landing_2026-09-08.md).  The recorded RAM -> FLASH cost
 *     for the Q31 table is +13.2 pt of the CPU window (see asrc_poly_q31.inc) -- margin
 *     that preset does not have.
 *
 * Measured for THIS preset at 16+16 ch over the full 9x9 rate matrix: 71 of 81 pairs run
 * (the other 10 are the pre-existing pair-gate refusals), every one with miss / starve /
 * drop / bad delta 0, worst max_demand 80.5 % at 48 -> 32 kHz, and Q31 beats float on every
 * cell paired on one commit (+2.3 .. +12.0 pt).  Data RAM +250 B, program -2,596 B.
 * [internal] ak512_q31_flash_full_ratepair_production_feasibility_2026-09-12.md
 *
 * FLOAT REMAINS BUILDABLE: both are #ifndef, so -Define ASRC_SAMPLE_Q31=0 returns this
 * profile to the float arm and its own ASRC_COEFF_STORAGE pin below.  Q31 with RAM
 * coefficients is NOT a candidate -- it traps deterministically at this channel count
 * (section 11 of the same report).
 *
 * ASRC_COEFF_STORAGE_FLASH is defined in audio_app_asrc.c; as a macro body that is fine,
 * the same way the float pin further down relies on it. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR)
  #ifndef ASRC_SAMPLE_Q31
    #define ASRC_SAMPLE_Q31  (1)
  #endif
  #ifndef ASRC_Q31_COEFF_STORAGE
    #define ASRC_Q31_COEFF_STORAGE  (ASRC_COEFF_STORAGE_FLASH)
  #endif
  /* P0 QUIET_TRACK default ON for this preset (2026-09-29, owner decision), at the verified
   * operating point only (asrc_app_config.h APP_P0_POINT_A_HZ).  It follows the sample arm and
   * the FF guard it enters from, so -Define ASRC_SAMPLE_Q31=0 still builds, without the track. */
  #ifndef APP_ASRC_QUIET_TRACK
    #define APP_ASRC_QUIET_TRACK  ( ASRC_SAMPLE_Q31 && ( APP_ASRC_FF_ACQUIRE_GUARD || APP_ASRC_RUNTIME_48K_TO_8 ) )
  #endif
  /* PHASE COUNT L = 256, PINNED FOR THIS PRESET **UNDER THE UNIVERSAL PROFILE ONLY**
   * (2026-09-15 owner decision on the value; 2026-09-16 on where the gate sits).
   *
   * WHY THE GATE.  This preset IS Universal member A, but it is also the shipping/legacy
   * AK512 bidir image, and those are the same MPLAB configuration.  Pinned unconditionally
   * the phase count would move the SHIPPING image -- program +15,360 B and a different rear
   * filter -- which is a default change, and the Universal profile is specified as opt-in
   * until its listening gate is closed.  So the pin is `APP_ASRC_UNIVERSAL_PROFILE &&`
   * exactly as member B's is, and a plain build of this preset is byte-for-byte the image
   * the pre-Universal tree produced.  Member A without the profile is not member A.
   *
   * WHY `defined(...) &&` AND NOT A BARE TEST.  The macro's fallback 0 lives in
   * asrc_app_config.h, which the preprocessor reaches AFTER this file, so a bare
   * `#if APP_ASRC_UNIVERSAL_PROFILE` here is reading an undefined name and relying on C's
   * implicit-zero rule.  That is exactly the dependency the C0.3 configuration guard
   * (buildtools/check_config_gate.py, class A ORDER) refuses, and it refused this -- so the
   * test is spelled out: defined and true, or explicitly not the profile.  Behaviour is
   * unchanged either way; what changes is that the intent is in the source rather than in
   * the language's default.
   *
   * WHAT IT BUYS, when the profile is on.  This is the universal-rate profile: it is the one asked to hold a
   * uniform spectral specification (worst in-band unwanted component <= -100 dBc over
   * 0 .. 0.8125 x output Nyquist, passband +/-0.10 dB) across the whole 45-pair rate
   * matrix.  At L=128 three pairs miss it -- the 160/147 family (12->11.025, 24->22.05,
   * 48->44.1) at -98.27 dBc, and they miss it on the REAR resampler's own
   * phase-interpolation spur, which no front end can filter.  Doubling L moves that spur
   * by 20*log10(4) = 12.04 dB and closes the specification: 45/45 pairs pass, and in
   * every remaining non-PASS row the limiter has moved back to the front-end alias.
   *
   * WHAT IT COSTS.  Program memory, (L+1) x M x 4 B: 15,480 B -> 30,840 B, i.e.
   * +15,360 B, and nothing else.  RAM is unchanged because ASRC_Q31_COEFF_STORAGE is
   * FLASH here (the pin just above).  Runtime CPU is unchanged because blend_row reads
   * two rows per output frame at any L, so the MAC count, the per-frame coefficient
   * traffic, ASRC_POLY_AHEAD and the A1 fill law are all L-independent -- which is also
   * why this does not disturb the A1 qualification fence.
   *
   * WHY IT IS PINNED HERE AND NOT IN THE GENERIC #ifndef.  Exactly the reason the two
   * pins above are: the generic default (asrc_app_config.h, L=128) is shared with presets
   * that cannot pay +15,360 B of program, and a global flip would move them silently.
   *   96K_12CH_32K   its Q31 table is in RAM, so the cost would land in DATA memory:
   *     52,146 B -> 67,506 B against a 65,536 B region.  Moving it to flash to dodge that
   *     costs +13.2 pt of the CPU window and the preset ships on ~1.2 pt.  It stays L=128,
   *     pinned explicitly in its own block above and guarded below.
   *   AK128 bi-codec its float table is already in flash, and the 108 KiB application
   *     region had 10,580 B free at the last measurement -- 4,780 B short.  It pins
   *     L=128 in its own block above.
   *
   * THE FLOAT ARM NEEDS ONE MORE SWITCH NOW.  Both pins are #ifndef, so
   * -Define ASRC_SAMPLE_Q31=0 still returns this profile to the float arm -- but the float
   * flash tables in audio_app_asrc.c are (128,32) / (128,30) / (64,30), so that build must
   * also say -Define ASRC_POLY_L=128.  Without it the build stops at the float table
   * selector with a message naming the missing geometry, which is the intended loud
   * failure: there is no (256,30) float table in the tree and generating one would put
   * 30,840 B in a profile that does not ship.
   *
   * [internal] study_ak512_universal_nyquist_prototype_phase1_2026-09-14.md, sections 31
   * (the 45-pair L128/L256 comparison) and 31.9 (the per-profile resource derivation). */
  #if defined( APP_ASRC_UNIVERSAL_PROFILE ) && APP_ASRC_UNIVERSAL_PROFILE
    #ifndef ASRC_POLY_L
      #define ASRC_POLY_L  (256u)
    #endif
  #endif
#endif

/* M28 IS RETIRED FOR THE ENTIRE AK512 M30 PRODUCT FAMILY (2026-09-12, owner decision).
 *
 * It was never a routine build preset -- always this one compile-time developer switch,
 * gated to the presets below -- and it is not going to be used going forward.  The switch
 * itself (its 0/1 sanity check in app_build_config.h) is left defined so a stray
 * `-Define APP_ASRC_EXPERIMENTAL_M28=1` gets a clear, immediate error naming the reason,
 * rather than being silently absorbed by an #ifndef somewhere and only surfacing as a
 * confusing failure much later (which is exactly what used to happen: see the M28-pin
 * history in the 96K_12CH_32K preset block above, and the A1 qualification fence's own
 * `_Static_assert`, which already refuses any M30-family image that resolves to
 * ASRC_POLY_AHEAD == 14 -- M28's value -- rather than the qualified 15.  This #error just
 * says the same thing before the fence has to). */
#if APP_ASRC_EXPERIMENTAL_M28
  #error "APP_ASRC_EXPERIMENTAL_M28 is retired: M28 is not supported on any AK512 M30 production preset (APP_BUILD_ASRC_CODEC_BIDIR, ASRC_CODEC_MEAS, the BIDIR/MEAS M30 headroom builds, the 96 kHz A<->B/MEAS presets, or ASRC_CODEC_96K_12CH_32K). The geometry and table (asrc_poly_q31_table_m28.h) stay in the tree as a record; they are not a live production option."
#endif

/* Standard BIDIR and MEAS use production M30 / fc 0.464 / Kaiser beta 11.3
 * unconditionally now that the M28 switch above
 * is a hard error rather than a second arm.
 * Host gate for the record: M30 -0.815 dB @20 kHz / -107.2 dB worst image; M28
 * -0.928 dB / -105.3 dB -- M28 was never the better filter, only the cheaper one, and the
 * third-band front end (see the 96K_12CH_32K preset block) bought more CPU than M28 ever
 * did, which is most of why nobody needs the taps M28 saved any more. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_A_TO_B) || \
    (APP_BUILD == APP_BUILD_ASRC_MEAS_96K_B_TO_A) || \
    (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K)
  /* THE AK512 M30 PRODUCT FAMILY, NAMED ONCE.  These eight presets are the ones whose ring the
   * A1 qualification fence's pair list was derived for, so they are the ones the fence must be
   * ACTIVE on -- see the _Static_assert in audio_app_asrc.c.
   *
   * It is defined HERE, inside the block that already enumerates the family, because the
   * membership test and the filter resolution must not drift apart: a preset added to the list
   * above gets the flag automatically, and one removed loses it.  Spelling the eight-way
   * condition out a second time in audio_app_asrc.c is exactly the drift this avoids.
   *
   * Membership is a PRESET fact, not a filter-geometry fact -- which matters now only in
   * principle, since the #error above means every member always resolves M30 in practice. */
  #define APP_ASRC_RING_IS_AK512_M30_FAMILY  (1)

  /* THE LONGER REAR PROTOTYPE, M34 (2026-09-30, owner decision "M32 か M34").
   *
   *   M34 / Kaiser beta 14.0 / fc 0.46336 / L 256, Q31, baked by
   *   tools/asrc/asrc_q31_resampler_model.py table --banner-exact --taps 34 --fc 0.46336 --beta 14.0 --phases 256
   *
   * Host model ([internal] rear-filter host study, 2026-09-30, reproduced bit-exactly against the M30
   * table): BA 8 -> 48 kHz THD+N -123.6 -> -136.6 dB, 8k-f image -124.6 -> -146.1 dBc, stopband
   * worst -111.9 -> -132.6 dB, and no tested input tone's image is worse than M30 (the M32/beta13
   * alternative is 7 dB worse at the 3.25 kHz protected-band edge, which is why it was not taken).
   *
   * SCOPE: exactly the images that run the Universal Q31 rear at L=256 through this preset list --
   * member A (standard BIDIR), member B (96K_A_TO_B; owner approval 2026-10-01, so both images of
   * the two-image bundle run ONE rear again) and the MEAS preset built with the Universal defines
   * (the P0 AB/BA measurement images).  Member B's price at 96 kHz / 8 ch is estimated in the M34
   * report (one engine, 8 ch: +3.9 pt at 96 -> 96, +2.0 pt at 96 -> 48 whose measured M30
   * max_demand is 71.5 %) and must be measured
   * with `*ts` on hardware.  96K_12CH_32K is pinned to M30 by its own #error below and its _opt
   * kernels are assembled for 30 taps.
   *
   * WHAT MOVES WITH M.  ASRC_POLY_MH 14 -> 16 and ASRC_POLY_AHEAD 15 -> 17 (asrc_app_config.h):
   * the A1 fill law's R_safe and the setpoint rise by 2 frames on the 24 down-conversion pairs
   * above the FIFO/2 floor; the fence's unqualified-direct list is IDENTICAL at AHEAD 17
   * (re-derived with [internal tool] a1_fill_reserve_gate_delta.py, which carries the M34 row), which
   * is why the fence in audio_app_asrc.c accepts AHEAD 17 when -- and only when -- this is on.
   * Ring RAM +M*4 B per channel (mirror overhang), s_ceff_q31 +16 B per engine, table +4,112 B
   * of flash.
   *
   * BACK TO M30: -Define APP_ASRC_REAR_POLY_M34=0.  The M30 table, vectors and every M30 number
   * stay valid for that build. */
  #ifndef APP_ASRC_REAR_POLY_M34
    #if ( (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) || \
          (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B) ) && \
        defined( APP_ASRC_UNIVERSAL_PROFILE ) && APP_ASRC_UNIVERSAL_PROFILE
      #define APP_ASRC_REAR_POLY_M34  (1)
    #else
      #define APP_ASRC_REAR_POLY_M34  (0)
    #endif
  #endif
  #if APP_ASRC_REAR_POLY_M34
    #define ASRC_POLY_M                 (34u)
    #define ASRC_POLY_FC                (0.46336f)
    #define ASRC_POLY_WINDOW            (2) /* ASRC_WINDOW_KAISER_11: the Kaiser formula; beta below */
    #define ASRC_POLY_KAISER_BETA       (14.0f)
    /* The telemetry signboard names the window it really runs: `[poly-k14 x16ch]`.  It used to
     * stay at the default "poly-k11.3" so host parsers kept matching -- a log excerpt then
     * claimed beta 11.3 for a beta 14 image (#41 review).  The one parser that matched the
     * name (tools/asrc/q31_rate_regression.py) now takes any `poly-k<beta>`; the geometry is
     * still in the boot banner `ASRCpath[M=34 L=256 ...]`. */
    #define ASRC_POLY_KAISER_NAME       "poly-k14"
    /* APP_ASRC_Q31_OPT_KERNELS stays 0 on these presets, so the generic
     * mchp_asrc_q31_blend_row / _row16 run (any even M >= 2, no --defsym).  M34 is not a
     * multiple of 4, so it could NOT take the float 2x2 SCHED_V2/V3 unrolls -- which the Q31
     * rear never uses. */
  #else
  #define ASRC_POLY_M                   (30u)
  #define ASRC_POLY_FC                  (0.464f)
  #define ASRC_POLY_WINDOW              (2) /* ASRC_WINDOW_KAISER_11 */
  #endif

  /* LIN4 REAR BLEND KERNEL, DEFAULT ON FOR THE AK512 BIDIR AND MEAS Q31 PRESETS (2026-10-11, owner
   * decision).  The SAME linear blend as mchp_asrc_q31_blend_row, read four taps per pass from the
   * table padded to a 16-byte row stride: bit-exact (the boot poly selftest checks every row at six
   * weights against the shipped kernel), 606 -> 380 cycles per output frame.  Evidence: [internal]
   * REPORT_asrc_hifi_48to44k1_p5_2026-10-11.md -- product BIDIR 81/81 cells RT clean, max_demand
   * median -5.5 pt (worst cell A22.05k/B48k 96.5 -> 88.7 %); BA MEAS quick81 0 spectral FAIL.
   *
   * SCOPE: exactly the M34 rear on BIDIR and MEAS, the configuration it was measured on.  Not member
   * B (96K_A_TO_B, never run with it), not the M30 escape hatch (APP_ASRC_REAR_POLY_M34=0 keeps the
   * shipped kernel), not AK128 (float rear; LIN4 does not apply there).
   *
   * Not taken when a build asks for a call site LIN4 does not replace: the quadratic research blend,
   * the _opt kernels or the stage profile (asrc_app_config.h #errors on each pairing).  Those are
   * tested with defined() because their own fallbacks are read after this file.
   *
   * BACK TO THE SHIPPED KERNEL: -Define APP_ASRC_REAR_LIN4=0.  Both kernels stay in the tree. */
  #ifndef APP_ASRC_REAR_LIN4
    #if APP_ASRC_REAR_POLY_M34 && \
        ( (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) || (APP_BUILD == APP_BUILD_ASRC_CODEC_MEAS) ) && \
        defined( ASRC_SAMPLE_Q31 ) && ASRC_SAMPLE_Q31 && \
        !( defined( APP_ASRC_REAR_QUAD_BLEND ) && APP_ASRC_REAR_QUAD_BLEND ) && \
        !( defined( APP_ASRC_Q31_OPT_KERNELS ) && APP_ASRC_Q31_OPT_KERNELS ) && \
        !( defined( APP_ASRC_STAGE_PROFILE ) && APP_ASRC_STAGE_PROFILE )
      #define APP_ASRC_REAR_LIN4  (1)
    #else
      #define APP_ASRC_REAR_LIN4  (0)
    #endif
  #endif

  /* THE SIGNBOARD GUARD.  This preset's entire 2026-09-10 validation was taken on
   * M30, and a preset-level pin on APP_ASRC_EXPERIMENTAL_M28 is unreachable (see the
   * preset block above), so a build of it that silently resolved to another geometry
   * would invalidate every recorded number without saying so.  Fail the build instead.
   * M28 is retired before this block is reached (see the #error above); this guard
   * remains as an independent invariant that the validated 96K_12CH_32K preset must
   * resolve M30. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K) && (ASRC_POLY_M != 30u)
  #error "APP_BUILD_ASRC_CODEC_96K_12CH_32K is validated at ASRC_POLY_M == 30 (see the preset block); this build resolved to something else"
#endif

  /* THE SAME SIGNBOARD, FOR L.  Added 2026-09-15 with the standard BIDIR preset's
   * L=256: the phase count is now a per-preset resource decision, so the two presets
   * that must NOT follow it say so as an invariant and not only as a pin.  Both
   * failures are silent otherwise -- 96K_12CH_32K would overflow data memory at link
   * time with no mention of L, and a BIDIR image that quietly resolved L=128 would
   * still run while missing the -100 dBc specification on three rate pairs. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_12CH_32K) && (ASRC_POLY_L != 128u)
  #error "APP_BUILD_ASRC_CODEC_96K_12CH_32K is validated at ASRC_POLY_L == 128 and its Q31 coefficient table is in RAM: L=256 costs +15,360 B of data memory (52,146 -> 67,506 B against 65,536 B), and moving the table to flash to fit it costs +13.2 pt of a CPU window that has ~1.2 pt spare. See the preset block."
#endif
  /* THE ESCAPE IS DELIBERATE AND IT IS NOT A LOOPHOLE.  Without it this invariant forbids
   * its own control experiment: the only way to build the Q31 arm at L=128 for an A/B against
   * L=256 would be to edit this file, and an A/B whose control needs a source edit does not get
   * run.  (Found immediately, 2026-09-15: an arm-time intermediate-ring ovf count differed
   * between the two images and the L=128 Q31 control could not be built to check it.)  So the
   * escape is a -Define nobody reaches by accident, it names what it acknowledges, and an image
   * built with it is NOT the specified product image -- it is a measurement. */
/* SCOPED TO THE PROFILE, 2026-09-16, for the same reason the pin above is: without the
 * profile this preset is the legacy shipping image, which is specified at L=128 and must not
 * be refused by its own tree.  `defined(...) &&` for the reason spelled out at the pin: the
 * profile macro's fallback is in asrc_app_config.h, read after this file. */
#if (APP_BUILD == APP_BUILD_ASRC_CODEC_BIDIR) &&     defined( APP_ASRC_UNIVERSAL_PROFILE ) && APP_ASRC_UNIVERSAL_PROFILE &&     ASRC_SAMPLE_Q31 && (ASRC_POLY_L != 256u) && !defined( ASRC_POLY_L_ACK_NOT_SPECIFIED )
  #error "APP_BUILD_ASRC_CODEC_BIDIR under APP_ASRC_UNIVERSAL_PROFILE is Universal member A and is specified at ASRC_POLY_L == 256: at L=128 the 160/147 rate family (12->11.025, 24->22.05, 48->44.1) sits at -98.27 dBc and misses the -100 dBc product gate on the rear resampler's own phase-interpolation spur, which no front end can filter. Build the float arm with -Define ASRC_SAMPLE_Q31=0 -Define ASRC_POLY_L=128 if L=128 is what you want."
#endif

  /* Coefficient storage for the shipping AK512 bidir profile.
   *
   * The float table is s_poly[ASRC_POLY_L + 1][ASRC_POLY_M] = 129 x 30 x 4 B =
   * 15,480 B of .bss, measured in the serial-update map.  That profile has no
   * spare data RAM (62,396 B of sections plus the stack fill the whole 64 KiB
   * region), so the identical bits are taken from program flash instead:
   * audio_app_asrc_poly_l128m30_flash.c, generated by
   * tools/gen_asrc_poly_flash_table.py for exactly L=128, M=30, fc=0.464,
   * window=2, Kaiser beta=11.3 -- the values resolved above.  The kernel arithmetic is
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

/* Every other preset is outside the family.  Spelled out rather than left undefined: an #if on a
 * name that was never defined reads 0 and passes silently, which is the failure mode this file
 * warns about elsewhere, and here it would silently disarm the fence's own assert. */
#ifndef APP_ASRC_RING_IS_AK512_M30_FAMILY
  #define APP_ASRC_RING_IS_AK512_M30_FAMILY  (0)
#endif
/* The M34 rear exists only inside the family block above; a -Define APP_ASRC_REAR_POLY_M34=1 on
 * any other preset would otherwise be absorbed silently and build M32/M30. */
#ifndef APP_ASRC_REAR_POLY_M34
  #define APP_ASRC_REAR_POLY_M34  (0)
#endif
#if APP_ASRC_REAR_POLY_M34 && !APP_ASRC_RING_IS_AK512_M30_FAMILY
  #error "APP_ASRC_REAR_POLY_M34 applies only to the AK512 M30-family presets (standard BIDIR / MEAS under APP_ASRC_UNIVERSAL_PROFILE); this preset does not resolve the rear geometry there"
#endif

/* UNIVERSAL PROFILE, MEMBER B: THE Q31 REAR, PINNED (2026-09-16, owner decision).
 *
 * The Universal profile is a two-image bundle -- the split is forced, not preferred: 96 kHz
 * bidirectional is impossible on the board's WM8904 (no simultaneous ADC+DAC at or above
 * 88.2 kHz) and 16 channels do not fit the 166.7 us block a 96 kHz leg leaves.  So member A is
 * the standard BIDIR preset (Q31 rear, flash coefficients, L=256, all pinned above) and member B
 * is this 96 kHz A->B preset.  Until this pin, member B inherited the GENERIC float arm, so the
 * bundle shipped two different rear resamplers under one profile name.
 *
 * WHY THAT IS NOT COSMETIC.  Phase H raised L to 256 for exactly one reason: the Q31 rear's
 * phase-interpolation spur, whose closed form 40*log10(f/(L*rear_in)) matches measurement to
 * 0.02 dB.  A float rear does not have that spur, so a float member B is not the thing Phase H
 * qualified -- L=256 buys it nothing, and its audio evidence would belong to a different filter.
 * Two images, one architecture, is the whole claim the profile makes.
 *
 * WHAT IT COSTS, measured on THIS preset (MPLAB X v6.35 / xc-dsc 3.31.01 / DFP 1.5.269 -- the
 * report this cites said v6.30, which was wrong: every build log says v6.35, and v6.30's
 * generator cannot resolve this project's pinned DFP at all.  MPLAB configuration
 * dsPIC33AK512_ASRC_SERIAL_UPDATE, coefficient bank CRC32 0x02692603): program 214,108 ->
 * 253,924 B (+39,816 B), data 54,528 -> 23,826 B (-30,702 B, 83 % -> 36 % of the 65,536 B
 * region).  Data FALLS: the Q31 table is pinned into flash below, while the float L=256 table was
 * generated into RAM at boot.  There is therefore no resource argument on the other side either.
 *
 * LEGACY 96k A->B IS UNTOUCHED.  The pins sit inside `#if APP_ASRC_UNIVERSAL_PROFILE`, so a
 * non-Universal build of this preset keeps the float arm and the generic phase count.  This is a
 * PROFILE decision; it deliberately does not become the preset's default.
 *
 * IF APP_ASRC_UNIVERSAL_PROFILE IS NOT DEFINED YET WHEN THIS IS READ the pins do not fire -- it
 * arrives as a -Define, and this header is read before the `#ifndef` default in asrc_app_config.h.
 * That is stated as `defined(...) &&` rather than left to C's implicit-zero rule, because the C0.3
 * configuration guard (buildtools/check_config_gate.py, class A ORDER) refuses a guard that reads
 * a macro whose first #define the preprocessor has not reached.  It refused this line.
 * That failure is loud rather than silent, which is why the pin is allowed to depend on it: the
 * three profile-side invariants (L == 256 and the Q31 rear in asrc_app_config.h, flash storage in
 * asrc_poly_q31.inc) are all evaluated AFTER every preset has had its say, and each stops the
 * build naming its own reason.
 *
 * [internal] study_ak512_universal_phase_i_firmware_realization_2026-09-16.md sections 12 and
 * I3-9. */
#if defined( APP_ASRC_UNIVERSAL_PROFILE ) && APP_ASRC_UNIVERSAL_PROFILE &&     (APP_BUILD == APP_BUILD_ASRC_CODEC_96K_A_TO_B)
  #ifndef ASRC_SAMPLE_Q31
    #define ASRC_SAMPLE_Q31  (1)
  #endif
  /* Flash, not RAM.  ASRC_COEFF_STORAGE_FLASH is defined in audio_app_asrc.c; as a macro body
   * that is fine, exactly as the BIDIR pin above relies on it. */
  #ifndef ASRC_Q31_COEFF_STORAGE
    #define ASRC_Q31_COEFF_STORAGE  (ASRC_COEFF_STORAGE_FLASH)
  #endif
  /* L = 256, so that the profile's phase-count requirement is a PIN here and not only an #error
   * in asrc_app_config.h.  Before this, member B reached L=256 by an explicit -Define on the
   * command line, which is a build-recipe fact rather than a source fact and does not survive
   * being rebuilt from the MPLAB configuration alone. */
  #ifndef ASRC_POLY_L
    #define ASRC_POLY_L  (256u)
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
  /* F1(a), 2026-09-04: the *ag / ?ag control-variable trace is OFF BY DEFAULT in every MEAS
   * preset, and has to be asked for.
   *
   * WHY THE DEFAULT MOVED.  asrc_app_config.h defaults it to 1 because every AK512 MEAS preset
   * has always carried it, so every MEAS image paid for its Q34 buffer -- 16,384 B of X space,
   * the single largest allocation in the image -- plus ~4.4 KB of program for the two
   * printf-heavy trace bodies.  Nothing in a DR or THD+N run reads it: those go through
   * s_cap[] via *ac / ?ac.  It is a SERVO diagnostic.  The bill came due on the Q31 MEAS arm,
   * which does not link at all until the buffer is gone -- every Q31 measurement so far had to
   * be driven with an explicit -Define APP_MEAS_CTRL_TRACE=0 on the command line, which is a
   * default in the wrong place, not a build option.
   *
   * NOTHING IS LOST, AND NOTHING IS SILENT.  The exported symbols stay as stubs
   * (audio_app_meas.c's !APP_MEAS_CTRL_TRACE arm), so the console still answers *ag / ?ag, and
   * a servo-trace session is one flag away:
   *
   *     buildtools/build.ps1 -Full -Define APP_MEAS_CTRL_TRACE=1
   *
   * Say so in the report when a measurement needed that flag -- an image built with it is 16 KB
   * of X space away from the one every other measurement used, and on the Q31 arm it does not
   * build at all.  This is a measurement-only diagnostic, so it follows the same rule as the
   * rest of them: opt-in, never resident by default. */
  #ifndef APP_MEAS_CTRL_TRACE
    #define APP_MEAS_CTRL_TRACE  (0)
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
