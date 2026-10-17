#ifndef ASRC_DECIMATOR_48_TO_8_H
#define ASRC_DECIMATOR_48_TO_8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_specific_config_defs.h"   /* APP_USE_96K_RATE, for the pre-stage gate below */

/* Whether the 96 -> 48 kHz pre-stage is compiled at all.  DERIVED, not a user knob: the stage only
 * has a caller where leg A can run at 96 kHz, and asrc_audio_path.c already gates its state and its
 * dispatch on the same condition.
 *
 * It is gated rather than left to --gc-sections because the boot selftest chain calls it
 * unconditionally, which makes it reachable and therefore un-collectable: measured on
 * APP_BUILD_ASRC_CODEC_BIDIR it kept 4 functions, the 21-tap coefficient table and a selftest in a
 * 48 kHz image that can never select the stage.  Keep every pre-stage declaration inside this gate
 * so a build that cannot use it also cannot link against it by accident. */
#define ASRC_DECIMATOR_HAS_96_TO_48  APP_USE_96K_RATE

/* IMPLEMENTATION CAPACITY of this legacy float decimator family -- how many channels THIS code
 * can process.  It is NOT an ASRC channel-width limit and must never be read as one.
 *
 * Three different quantities get called "channels" in this tree and only one of them may set the
 * width of ASRC processing:
 *
 *   ASRC_CH                 the ASRC LOGICAL width.  The only authority over how many channels
 *                           the front end, the history and the resampler process.
 *   APP_SLOTS_PER_FS        the PHYSICAL I/O width (WM8904 = 2 ch, I2S = 2 slots, TDM8 = 8).
 *                           Confined to the mapping at the two ends: replicate into ASRC_CH on
 *                           the way in, narrow to slots on the way out.
 *   this macro              THIS implementation's CAPABILITY, and nothing more.  It is not an
 *                           ASRC specification.  It may only take part in selecting or VALIDATING
 *                           an implementation; it may not change ASRC_CH.
 *
 * So the correct inference is NOT "float is 2 ch, therefore this path is 2 ch" but "ASRC_CH is the
 * requirement; this implementation cannot meet it; it is therefore DISQUALIFIED as a front end".
 * A disqualified implementation is not used at a reduced width -- the rate pairs that need a front
 * end are refused instead, with a reason (audio_app_asrc_rate_pair_is_supported()), and the pairs
 * that need none are unaffected.  There is no acknowledgement macro and no override, because every
 * such escape is a way of writing "reduce the width until it fits": a width reduced to fit measures
 * nothing, and for an anti-alias stage it is worse -- filtering fewer channels than the resampler
 * converts is a MISSING stage, and down-conversion without one is not correct at any channel count.
 *
 * This was not hypothetical: the AK128 TDM8 one-to-one notes in asrc_app_build_config.h used to
 * cite this macro as the reason the low-rate front end "cannot be used" in an 8-channel build --
 * i.e. a legacy implementation's capacity had quietly become a product-level capability decision.
 *
 * 2 is honest for THIS family: ASRC_DECIMATOR_HIST_LEN below sizes every history array by it, and
 * the frozen 48 kHz paths are byte-compared against builds that use it. Widening it is not the fix. */
#define ASRC_DECIMATOR_FLOAT_MAX_CHANNELS (2u)
#define ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS  (43u)
#define ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS  (147u)
#define ASRC_DECIMATOR_48_TO_8_COEFF_CRC32  (0x00F3DAE7UL)
/* den == 3: the 48 -> 16 kHz decimator, REPURPOSED 2026-07-29 to serve a 16 kHz OUTPUT.
 *
 * It was the two-stage front end for the 11.025 kHz path (69 taps to 16 kHz, then a 75-tap
 * decimate-by-1 stage at 16 kHz to reach the 5512.5 Hz output Nyquist).  The /4 respec moved
 * 11.025 kHz to den == 4, leaving den == 3 selected by no rate at all -- 6286 bytes of flash
 * and a boot selftest chain for code the audio path could not reach.  It now does what its
 * name always implied.
 *
 * ONE stage, and that is forced rather than a simplification: a 3:1 decimation to 16 kHz folds
 * at multiples of 16 kHz and the 16 kHz output's Nyquist IS the intermediate's (8000 Hz), so
 * every input above 8000 Hz lands inside the final band during the decimation itself.  Nothing
 * placed after it can separate frequencies already summed into one bin -- measured, a 75-tap
 * "repair" stage at 16 kHz moves the worst alias only -14.8 -> -21.2 dB (report section 12).
 * So this filter carries the whole job, with its stopband pinned on 8000 Hz, and it has to be
 * long: 161 taps, against 27 for the /4 chain's first stage, because den == 4 and 6 give their
 * first stage a huge transition band for free while den == 2 and 3 pay the full transition.
 *
 * Passband 5900 Hz (0.74 of Nyquist).  Stage alone -107.5 dB, cascade -116.1 dB, passband
 * edge -0.01 dB.  161 rather than the 157 the transition width alone suggests: at 157 the
 * stage alone is only -92.3 dB and the cascade cleared the gate solely because the
 * resampler's fc = 7440 Hz covers this stage's weak spot just above 8000 Hz.  That is real
 * but it is exactly the kind of downstream help a single-stage front end must not depend on,
 * so 4 more taps (1.1 us) buy the depth outright.  Priced against 137 taps / 5600 Hz and
 * 173 taps / 6100 Hz in [internal] study_asrc_lowpass_per_rate_2026-07-29.md. */
#define ASRC_DECIMATOR_48_TO_16_TAPS         (161u)
#define ASRC_DECIMATOR_48_TO_16_COEFF_CRC32 (0x225371F0UL)
/* den == 2: the 48 -> 24 kHz decimator, for a 24 kHz OUTPUT (2026-07-29).
 *
 * Structurally the same case as den == 3 above, and single-stage for the same forced reason: a
 * 2:1 decimation to 24 kHz folds at multiples of 24 kHz, and the 24 kHz output's Nyquist IS the
 * intermediate's (12000 Hz), so every input above 12000 Hz lands inside the final band during
 * this very decimation.  Nothing after it can separate what has already been summed into one
 * bin, so the stopband is pinned ON 12000 Hz and this filter carries the whole anti-alias job.
 *
 * Half-band specialisation -- the textbook saving for a 2:1 stage, ~half the multiplies -- was
 * priced and REJECTED: it needs the stopband to start above the output Nyquist and the resampler
 * to cover the gap, and the resampler's 30-tap prototype has far too gentle a transition for
 * that.  Every candidate from +-300 to +-1800 Hz around 12000 Hz needed over 401 taps, i.e. the
 * "saving" is a large loss.  See the HALFRATE_ block in
 * tools/asrc/asrc_decimator_48_to_8_design.py.
 *
 * 107 taps is the smallest odd count clearing -100 dB with the stage evaluated ALONE (the bar
 * the 16 kHz stage was held to, and for the same reason): stage alone -102.37 dB, cascade
 * -115.91 dB, passband edge -0.010 dB.  105 taps would pass the cascade gate at -108.32 dB
 * while the stage alone is only -92.41 dB -- exactly the downstream dependence this structure
 * must not take on.  Passband 8850 Hz = 0.7375 of Nyquist, the same relative width as the
 * 16 kHz stage's 5900/8000; RAM cost zero (the /6 member sizes the union at 190 taps' worth). */
#define ASRC_DECIMATOR_48_TO_24_TAPS         (107u)
#define ASRC_DECIMATOR_48_TO_24_COEFF_CRC32 (0xEA7E7473UL)
/* Second coefficient set over the SAME den == 2 structure, for a 22.05 kHz output (2026-07-29).
 * Same arrangement as the /4 chain's 11.025-vs-12 kHz pair above, and for the same reason.
 *
 * 22.05 kHz differs from every rate handled so far in that it is NOT 48000/den: den == 2 is the
 * only integer choice whose intermediate rate (24 kHz) is at or above it, so the stage decimates
 * 48 -> 24 kHz as usual and the RESAMPLER then pulls 24000 -> 22050 at step 1.0884.  The
 * intermediate Nyquist is therefore 12000 Hz but the OUTPUT Nyquist is 11025 Hz, 975 Hz lower --
 * unlike the 24 kHz case where the two coincide.
 *
 * Reusing the 24 kHz set was priced first and FAILS: it is only -18.9 dB at 11025 Hz, so input
 * between 11025 and 12000 Hz walks through the stage and then folds into the 22.05 kHz band in
 * the resampler.  Measured cascade -24.70 dB, stage alone -19.74 dB against a -100 dB limit.
 * The resampler contributes only ~5 dB of that, so this set is held to the STAGE-ALONE bar like
 * the 16 kHz and 24 kHz sets, with the stopband pinned on the OUTPUT Nyquist, 11025 Hz.
 *
 * The tap count above is shared deliberately.  7875 Hz = 11025 - 3150 keeps the SAME 3150 Hz
 * transition width the 24 kHz set carries, and at a fixed input rate taps follow transition
 * width -- so 107 comes out identical, and the variant costs no extra RAM, no extra history, no
 * second ISR code path.  Selecting one is a coefficient pointer, loaded outside the tap loop.
 * Alternatives priced in [internal] study_asrc_lowpass_per_rate_2026-07-29.md: 7500 Hz /
 * 97 taps (-5.0 units) is the fallback if the DSP margin does not hold, and a wider passband
 * costs taps outright (8000 Hz / 113, 8200 Hz / 119). */
#define ASRC_DECIMATOR_48_TO_24_OUT22K_COEFF_CRC32 (0x474CF2F6UL)

/* Which of the two den == 2 coefficient sets a state instance runs.  As with the /4 enum below
 * there is no default: the caller names the output rate it is decimating towards, so a new rate
 * cannot silently inherit band edges designed for a different Nyquist -- which is exactly the
 * bug that left 12 kHz aliasing at 0 dB. */
typedef enum
{
    ASRC_DECIMATOR_48_TO_24_FOR_24000 = 0,   /* passband 8850 Hz, stopband 12000 Hz */
    ASRC_DECIMATOR_48_TO_24_FOR_22050 = 1    /* passband 7875 Hz, stopband 11025 Hz */
} asrc_decimator_48_to_24_variant_t;
/* /4 respec of the same 11.025 kHz path (2026-07-29): 48 -> 24 -> 12 kHz, two
 * real 2:1 stages, replacing the /3 chain's 48 -> 16 kHz plus decimate-by-1.
 * Cheaper front end (49.1 us vs a measured 51.5 us), lower resampler step
 * (1.08843 vs 1.45125, so most pairs hit the dstep==1 fixed kernel), and a
 * 200 Hz wider passband (4200 vs 4000 Hz) all at once.  Section 11 of
 * [internal] report_asrc_d2_alias_route_2026-07-28.md prices the
 * alternatives; option B there was signed off.
 *
 * The 12 kHz intermediate does NOT let the resampler take over the anti-alias
 * job: with no stage 2 the cascade measures -0.2 dB (the "before" column of the
 * /4 row in tools/asrc/asrc_headroom_filter_check.py), i.e. the resampler
 * contributes essentially nothing.  The reason is WHERE aliases arrive -- the
 * worst ones land at multiples of the intermediate rate, so 12.1 kHz folds to
 * 100 Hz, and at those fold centres the resampler sees a low frequency and
 * passes it at full level.  Only a filter acting BEFORE the fold can stop them.
 * Stage 2 is mandatory and, because its input rate is 24 kHz rather than
 * 16 kHz, needs MORE taps than the /3 chain's stage 2, not fewer. */
#define ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS         (27u)
#define ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS         (129u)
#define ASRC_DECIMATOR_48_TO_12_COEFF_CRC32         (0x06D61853UL)
/* Second coefficient set over the SAME /4 structure, for a 12 kHz output (2026-07-29).
 * 12 kHz was aliasing at 0 dB because the runtime gate had rows for 8 k and 11.025 k only;
 * see [internal] study_asrc_lowpass_per_rate_2026-07-29.md.
 *
 * The tap counts above are shared by both variants deliberately.  A 12 kHz output needs its
 * stopband at 6000 Hz rather than 5512.5 Hz, and that 487.5 Hz of slack buys a 4700 Hz
 * passband (vs 4200 Hz) out of the SAME 129 taps -- so the variant costs no extra RAM, no
 * extra history, no second ISR code path, and no measurable DSP time.  Selecting one is a
 * coefficient pointer.  Cascade alias -108.36 dB out of band (limit -100 dB; the figure was
 * reported as -115.70 dB until the gate's alias mask was corrected on 2026-07-29). */
#define ASRC_DECIMATOR_48_TO_12_OUT12K_COEFF_CRC32  (0xD271CE1DUL)
/* The 96 -> 48 kHz PRE-STAGE (2026-08-02).  Unlike every stage above it, this one is not selected
 * by a den of its own -- it sits in FRONT of them, so a 96 kHz leg can reuse the proven 48 kHz
 * chains unchanged and the composed ratio becomes /2 x /N (12, 8, 6 for 8, 11.025/12, 16 kHz).
 *
 * WHY.  The look-ahead one ASRC pull needs, R(step) = floor(step*(BLOCK-1)) + AHEAD + 1, is
 * proportional to the rate ratio, while the ring offers at most ASRC_FILL_TARGET_MAX = 104 frames
 * (FIFO 128, BLOCK 16).  From 96 kHz, 16 kHz needs 110 and 8 kHz needs 200, so asrc_set_fill_target()
 * clamps: the tail outputs of each block fail the window test, emit zeros and hold rd, which is the
 * audible break-up.  A /2 in front halves the step the ASRC sees and takes R+jitter to 35..36.
 * (Halving APP_BLOCK_FRAMES instead was measured and rejected -- duty 62.3 -> 82.2 %, margin 14.8 us,
 * and 11.025 kHz still broken.  [internal] asrc_96k.md part 3 section 3.)
 *
 * 21 TAPS, where a stand-alone /2 at this input rate costs 107.  This stage is the FIRST OF TWO,
 * exactly like the /4 chain's 27-tap first stage, so it inherits that structure's freedom: it only
 * has to suppress what would fold into the FINAL band after the remaining decimation, i.e. from
 * 48000 - final_Nyquist upward.  Pricing it with the stand-alone shape gave 107 taps at DOUBLE the
 * per-tap cost (its output is 48 kHz, not 24 kHz) and nearly produced a false no-go; see study
 * sections 6.1-6.2.  17.2 us instead of 87.7 us of a 166.6 us window.
 *
 * ONE COEFFICIENT SET FOR EVERY COMPOSED CHAIN, NO VARIANT ENUM -- the one place this is simpler
 * than the /4 and /2 stages it feeds.  Designed against the tightest rate in the menu (32 kHz:
 * passband 15000 Hz, which is the widest second-stage passband, and stopband from 48000 - 16000 =
 * 32000 Hz, the lowest fold edge), it satisfies every rate with margin to spare -- stage alone
 * -108.38 dB at 32 kHz's fold edge rising to -124.22 dB at 8 kHz's, passband edge -0.0001 dB.
 * Composed with each existing chain over that chain's own designed passband: ripple 0.0001 dB,
 * added droop -0.0001 dB, i.e. transparent in band.
 *
 * 21 -> 27 TAPS on 2026-09-02, to admit 96 k -> 22.05 kHz.  The widening was driven by a GATE, not
 * by a filter defect: the fill gate then in force (ASRC_FILL_SLACK_REQUIRED, retired 2026-09-12)
 * refused that pair on the direct path because the setpoint was pinned at R + ASRC_FILL_JITTER = 4
 * frames of slack, and a composed /4 fixed it by putting the resampler back at step 1.0884.  Under
 * the reserve law that replaced it (asrc_fill_law) the direct path is no longer refused for STARVE
 * -- it is held out of the qualified surface by the A1 fence instead -- so from 2026-09-12 this
 * row's technical justification is BAND LIMITING alone, which is what these taps were measured for,
 * and the row is what the pair runs on.  The old four-rate set could not be reused for that row -- at the
 * 36975 Hz fold edge it reads only -58.09 dB -- so the row and the coefficients go together.  The
 * set also covers 24 kHz, whose routing row is not published yet, because doing so cost 2 taps.
 *
 * 27 -> 41 TAPS on 2026-09-03, to admit 96 k -> 32 kHz -- and here the old set does not merely
 * lose margin, it FAILS: at 32 kHz's 32000 Hz fold edge the 27-tap set reads -47.95 dB, so
 * composing it would have shipped an audible alias.  32 kHz becomes the binding case on both
 * edges at once: the lowest fold edge in the menu (32000 Hz, under 24 kHz's 36000) and the widest
 * second-stage passband (15000 Hz, the L=2/M=3 `:audio` stage's own).  41 taps is the minimum that
 * clears the same -100 dB stage-alone bar, at -108.38 dB.  It stays ONE shared set because the new
 * design dominates the old one pointwise -- every other row's fold edge is above 32000 Hz and its
 * passband below 15000 Hz, so all six improve from a uniform -107.46 dB to -110.23..-124.22 dB.
 *
 * RAM +656 B (41 taps of mirrored, channel-interleaved history; +224 B over the 27-tap set), and it
 * does NOT join the union that holds the stages above: a composed chain runs the pre-stage AND a
 * second stage in the same block, so they need to exist at once. */
/* THREE COEFFICIENT SETS, WITH DIFFERENT TAP COUNTS (2026-09-03).
 *
 * The 41-tap set above is the SHARED one, and it serves every composed chain -- 96 kHz down to
 * 32 kHz and below, where a second stage runs behind the pre-stage.  It cannot serve the two rows
 * whose FINAL rate is 44.1 or 48 kHz: the pre-stage has to remove everything that would fold into
 * the final band, so its stopband edge is 48000 - final_Nyquist (25950 / 24000 Hz) and its
 * passband has to reach 20 kHz.  That transition is up to 4.3x narrower than the shared set's
 * 15000 -> 32000 Hz, and no tap count serves both cheaply: the shared set reads only -7.48 dB at
 * the 48 kHz fold edge and -15.54 dB at the 44.1 kHz one, while 169 taps would cost every
 * composed chain 4x the MACs for attenuation none of them uses.  So the FINAL rate selects the set, exactly the way it selects between the
 * two /2 and the two /4 sets -- with one difference that matters to a caller: THESE VARIANTS DO
 * NOT SHARE A TAP COUNT, so history and coefficient storage must be sized for the widest one.
 *
 * Each wide variant is a MINIMUM against the same -100 dB stage-alone bar as every other set
 * here: 111 taps reads -91.92 dB at the 44.1 kHz fold edge and 167 taps -98.72 dB at the 48 kHz
 * one.  Passband ripple 0.0001 dB, |H| at 20 kHz -0.0000 dB.
 *
 * A WIDE VARIANT IS NEVER COMPOSED.  Both serve a chain whose second stage is empty -- 96 -> 48
 * kHz, after which the resampler pulls 48 -> 44.1 kHz (step 1.08844) or runs at step 1.00000 --
 * so a wide ring and a second stage's rings are never live at the same time.  The Q31 front end
 * relies on that to give a wide variant the whole history arena instead of the pre-stage reserve,
 * and asserts it rather than assuming it. */
#if ASRC_DECIMATOR_HAS_96_TO_48
#define ASRC_DECIMATOR_96_TO_48_TAPS         (41u)
#define ASRC_DECIMATOR_96_TO_48_COEFF_CRC32 (0xE22651A1UL)
#define ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS         (113u)
#define ASRC_DECIMATOR_96_TO_48_OUT44K1_COEFF_CRC32 (0x4188A451UL)
#define ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS          (169u)
#define ASRC_DECIMATOR_96_TO_48_OUT48K_COEFF_CRC32  (0x009BDEDAUL)
/* The widest set, which is what storage is sized by.  Written as a max over the three rather than
 * as the literal 169, so adding a variant cannot leave a ring sized for the old widest. */
#define ASRC_DECIMATOR_96_TO_48_TAPS_MAX \
    ((ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS > ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS) \
         ? ((ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS > ASRC_DECIMATOR_96_TO_48_TAPS) \
                ? ASRC_DECIMATOR_96_TO_48_OUT48K_TAPS : ASRC_DECIMATOR_96_TO_48_TAPS) \
         : ((ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS > ASRC_DECIMATOR_96_TO_48_TAPS) \
                ? ASRC_DECIMATOR_96_TO_48_OUT44K1_TAPS : ASRC_DECIMATOR_96_TO_48_TAPS))

/* Which set the pre-stage loads.  Selected by the FINAL rate of the leg, not by the pre-stage's
 * own ratio -- the pre-stage is always 96 -> 48 kHz, and what differs is how much of 24-48 kHz
 * the chain behind it will fold. */
typedef enum
{
    ASRC_DECIMATOR_96_TO_48_SHARED    = 0,   /* 41 taps: pass 15000, stop 32000; composed chains */
    ASRC_DECIMATOR_96_TO_48_FOR_44100 = 1,   /* 113 taps: pass 20000, stop 25950; pre-stage only */
    ASRC_DECIMATOR_96_TO_48_FOR_48000 = 2    /* 169 taps: pass 20000, stop 24000; pre-stage only */
} asrc_decimator_96_to_48_variant_t;
#endif

/* num == 2, den == 3: the 48 -> 32 kHz AUDIO MODE front end (2026-08-23).
 *
 * The first RATIONAL chain here.  Every chain above decimates 48 kHz by an integer; 48/32 = 3/2
 * does not, so this one is L=2/M=3 polyphase -- conceptually up 2, lowpass at the 96 kHz
 * interpolated rate, down 3 -- with only the two phases that are actually needed ever computed.
 * No input is zero-stuffed and no tap is ever multiplied by a known-zero sample.
 *
 * WHAT IT IS NOT.  This is AUDIO MODE / PARTIAL PROTECTION, not a strict 48->32 front end.  The
 * 97-tap prototype's transition band is 15 kHz -> the 16 kHz output Nyquist, which protects
 * 0-13 kHz at -107.0 dB but leaves 13-16 kHz with residual alias, worst -25.5 dB.  Do not
 * describe this chain as full-band, strict, alias-free, or "0-16 kHz protected", and do not quote
 * the -107 dB figure without the -25.5 dB one beside it.  Strict alternatives were priced and do
 * not fit the CPU budget (N=161 needs 122.7 us against 99.0 us) -- see
 * [internal] study_ak512_48_to_32_bandlimit_16ch_2026-08-22.md and the implementation record
 * [internal] study_ak512_48_to_32_audio_n97_implementation_2026-08-23.md.
 *
 * N = 97 is FIXED by that study.  The two phase rows are its even and odd taps, so 49 + 48, and
 * each row is symmetric in its own right (every second tap of a symmetric odd-length set) --
 * which is why neither row has to be stored reversed for a kernel that walks its window from the
 * oldest sample forward.  Both rows read consecutive input samples and hop M=3 input samples
 * between successive outputs of the same row, so the existing Q31 block kernel serves them
 * unchanged: no new assembler.
 *
 * AB LEG ONLY.  BiDir is asymmetric here by design: 32 -> 48 kHz is up-sampling and needs no
 * anti-alias front end at all, so the BA leg gets none.
 *
 * OFFICIALLY SUPPORTED as of 2026-09-03 (owner decision).  The shipping specification of this
 * chain is exactly two clauses, and both must be quoted together:
 *
 *     0-13 kHz  protected
 *     13-16 kHz relaxed
 *
 * Accepted against a 5 % CPU reserve.  Hardware evidence, AK512, HEAD bb4a95a, shipping BIDIR
 * float image, both legs live at 48 <-> 32 kHz:
 *
 *     DSPload max = 91.5 %       -> 8.5 % reserve, so the 5 % reserve criterion passes
 *                                   (the 48/48 baseline on the same image is 71.7 %)
 *     600 s soak                 -> miss=0 drop=0 starve=0 udf=0 bad=0/0/0/0
 *     0-13 kHz alias  -107.03 dBc measured (19,000 -> 13,000 Hz), host gate -107.02
 *     13-16 kHz alias  -22.52 dBc measured worst (16,250 -> 15,750 Hz), level-independent
 *
 * NOT MET: a 10 % CPU reserve (91.5 % leaves 8.5 %).  Also note the 32 kHz leg's per-leg
 * response exceeds its own 500 us block period by up to 88 us -- that is one preemption by the
 * 48 kHz leg under rate-monotonic asymmetry, not a missed deadline, and the harm counters above
 * are what the acceptance rests on.  Do not re-derive either figure from the pre-2026-08-26
 * `TDMsum` numbers: that instrument was replaced by `DSPload` and is gone from src/.
 * Record: [internal] report_ak512_48_to_32_n97_stage3_revalidation_2026-09-03.md. */
#define ASRC_DECIMATOR_48_TO_32_L             (2u)
#define ASRC_DECIMATOR_48_TO_32_M             (3u)
#define ASRC_DECIMATOR_48_TO_32_PROTO_TAPS   (97u)
#define ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS  (49u)
#define ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS  (48u)
#define ASRC_DECIMATOR_48_TO_32_COEFF_CRC32 (0xC426300CUL)

/* Output frames this front end emits from `n` input frames, EXACTLY -- not an upper bound.
 *
 * Output k needs input floor(3k/2) to have arrived, so with inputs x[0..n-1] the even outputs
 * k = 2q are limited by 3q <= n-1 and the odd ones k = 2q+1 by 3q+1 <= n-1.  Counting each and
 * adding gives floor((n-1)/M) + floor((n-2)/M) + 2, which for M = 3 and a 16-frame block is 11 --
 * and 11, 11, 10 over three consecutive blocks, i.e. 32 outputs per 48 inputs, as L/M requires.
 *
 * A caller that sizes a scratch buffer with ceil(n * L / M) gets the same 11 here, but that is a
 * coincidence of these numbers, not a derivation; use this. */
#define ASRC_DECIMATOR_48_TO_32_OUT_FRAMES( n )                     \
    ( ( ( ( (uint32_t)(n) - 1u ) / ASRC_DECIMATOR_48_TO_32_M ) +     \
        ( ( (uint32_t)(n) - 2u ) / ASRC_DECIMATOR_48_TO_32_M ) ) + 2u )

/* Which of the two /4 coefficient sets a state instance runs.  There is no default: the
 * caller names the output rate it is decimating towards, so a new rate cannot silently
 * inherit band edges designed for a different Nyquist. */
typedef enum
{
    ASRC_DECIMATOR_48_TO_12_FOR_11025 = 0,   /* passband 4200 Hz, stopband 5512.5 Hz */
    ASRC_DECIMATOR_48_TO_12_FOR_12000 = 1    /* passband 4700 Hz, stopband 6000.0 Hz */
} asrc_decimator_48_to_12_variant_t;

/*
 * History layout: MIRRORED ring of CHANNEL-INTERLEAVED frames.
 *
 * Mirrored -- every incoming frame is stored twice, at the write index and at
 * write+TAPS -- so the newest TAPS frames are ALWAYS one contiguous, in-time-order
 * run starting at the write index.  The FIR then walks plain pointers with no
 * per-tap index compare and no wrap branch; that bookkeeping, not the multiplies,
 * was the dominant cost of the front end inside leg A's RX ISR.
 *
 * Interleaved -- both channels of one frame sit adjacent, so ONE pointer pair
 * (up from the oldest frame, down from the newest) serves both channels via a
 * constant +0/+1 element offset.  With channel-major history the compiler needs
 * four live pointers and pays four address adds per tap instead.
 *
 * Costs one extra store per input sample and doubles the history RAM; both are
 * negligible here.  Frame stride is always MAX_CHANNELS, also for mono.
 */
#define ASRC_DECIMATOR_HIST_LEN(taps) \
    (2u * (taps) * ASRC_DECIMATOR_FLOAT_MAX_CHANNELS)

typedef struct
{
    float stage1_history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS)];
    float stage2_history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS)];
    uint16_t stage1_write;
    uint16_t stage2_write;
    uint8_t stage1_phase;
    uint8_t stage2_phase;
    uint8_t channels;
    uint64_t input_frames;
    uint64_t output_frames;
} asrc_decimator_48_to_8_t;

typedef struct
{
    float history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_16_TAPS)];
    uint16_t write;
    uint8_t phase;
    uint8_t channels;
    uint64_t input_frames;
    uint64_t output_frames;
} asrc_decimator_48_to_16_t;

/* Same shape as asrc_decimator_48_to_16_t: one rate-changing stage, so one write index and one
 * phase counter.  Kept as its own type rather than folding the two together with a runtime
 * divisor, because that would put a variable where the tap loop currently has a compile-time
 * literal -- and the 16 kHz path is frozen, so its hot path must stay byte-identical. */
typedef struct
{
    float history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_24_TAPS)];
    /* Which variant's coefficients this instance runs.  A pointer rather than a compile-time
     * array reference, for the same reason as the /4 type below: it is loaded once per stage
     * invocation, OUTSIDE the tap loop, so the two variants share one tap loop with its
     * literal count intact. */
    const float* coeff;
    uint16_t write;
    uint8_t phase;
    uint8_t channels;
    uint64_t input_frames;
    uint64_t output_frames;
} asrc_decimator_48_to_24_t;

/* Both stages change rate (2:1 each), so this needs a phase counter per stage --
 * the same shape as asrc_decimator_48_to_8_t above, not the 48->16 shape.
 *
 * The two coefficient pointers are what make one structure serve both output rates.  They are
 * loaded once per stage invocation, OUTSIDE the tap loop, so the loop itself is unchanged; the
 * tap counts stay compile-time literals, which is what the mirrored-history pointer walk and
 * the union sizing depend on.  Cost: two pointers inside a union member that is already
 * 544 bytes smaller than the /6 member which sizes the union -- so zero bytes in practice. */
typedef struct
{
    float stage1_history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS)];
    float stage2_history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS)];
    const float* stage1_coeff;
    const float* stage2_coeff;
    uint16_t stage1_write;
    uint16_t stage2_write;
    uint8_t stage1_phase;
    uint8_t stage2_phase;
    uint8_t channels;
    uint64_t input_frames;
    uint64_t output_frames;
} asrc_decimator_48_to_12_t;

/* The 96 -> 48 kHz pre-stage, FLOAT implementation.  Same shape as asrc_decimator_48_to_16_t --
 * one rate-changing stage, so one write index and one phase counter -- and with no `coeff`
 * pointer, because this implementation serves the SHARED 27-tap set only.  Kept as its own type
 * rather than reusing the 48->24 type for the same reason that one is separate from the 48->16
 * type: the tap count is a compile-time literal in the pointer walk and the frozen paths must
 * stay byte-identical.
 *
 * THE TWO WIDE VARIANTS ARE Q31-ONLY (2026-09-03), and that asymmetry is deliberate.  The
 * variants exist to admit the 44.1 and 48 kHz rows, whose 113/169 taps do not fit the float front
 * end's CPU budget at all (92.5 and 138.4 us of a 140 us budget, against 36.2 and 54.1 us in
 * Q31) -- so no build would ever select them here.  Rather than carry a coefficient pointer and a
 * runtime tap count into a frozen pointer walk for a chain that cannot run, the float arm of
 * asrc_audio_path.c REFUSES those two rows.  Fail-closed, not silently narrower: PATH_FRONTEND_Q31
 * is 1 in every shipping configuration, and this arm survives only as the selftest's oracle. */
#if ASRC_DECIMATOR_HAS_96_TO_48
typedef struct
{
    float history[ASRC_DECIMATOR_HIST_LEN(ASRC_DECIMATOR_96_TO_48_TAPS)];
    uint16_t write;
    uint8_t phase;
    uint8_t channels;
    uint64_t input_frames;
    uint64_t output_frames;
} asrc_decimator_96_to_48_t;
#endif

/* Initialize persistent streaming state.  Supported channel count is 1 or 2. */
bool asrc_decimator_48_to_8_init(asrc_decimator_48_to_8_t* state,
                                 uint8_t channels);

/* Exact number of output frames the next call will produce. */
size_t asrc_decimator_48_to_8_output_frames(
    const asrc_decimator_48_to_8_t* state,
    size_t input_frames);

/*
 * Process interleaved float frames.  Strides are in float elements and must be
 * at least the initialized channel count.  The call is all-or-nothing when the
 * output capacity is too small.
 */
bool asrc_decimator_48_to_8_process_f32(
    asrc_decimator_48_to_8_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);

/*
 * Process signed 24-bit audio in 32-bit left-justified slots.  Conversion is
 * consistent with the existing ASRC: input shifts right by 8; output clamps,
 * truncates to signed 24-bit, then shifts left by 8.
 */
bool asrc_decimator_48_to_8_process_s24_left(
    asrc_decimator_48_to_8_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);

/*
 * Front end for the 48 -> 16 kHz runtime path (den == 3): a single 161-tap 3:1 decimator whose
 * stopband edge is pinned on the 16 kHz output's Nyquist, 8000 Hz.  Passband 5900 Hz.  The
 * resampler that follows runs at step 1.00000, since 48/16 is exactly 3.
 *
 * Single-stage by necessity, not by choice -- see the tap-count block above.  Contrast with the
 * /4 and /6 chains, whose FIRST stage is short because a later rate change is still pending, so
 * it only has to suppress what would fold into the final band after that change.
 *
 * The 5900 Hz passband edge (rather than the 6800 Hz this rate could carry) is a DSP-budget
 * decision -- the trade table is in tools/asrc/asrc_decimator_48_to_8_design.py and
 * [internal] study_asrc_lowpass_per_rate_2026-07-29.md.
 */
bool asrc_decimator_48_to_16_init(asrc_decimator_48_to_16_t* state,
                                  uint8_t channels);
size_t asrc_decimator_48_to_16_output_frames(
    const asrc_decimator_48_to_16_t* state,
    size_t input_frames);
bool asrc_decimator_48_to_16_process_f32(
    asrc_decimator_48_to_16_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);
bool asrc_decimator_48_to_16_process_s24_left(
    asrc_decimator_48_to_16_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);

/*
 * Front end for the 48 -> 24 kHz runtime path (den == 2): a single 107-tap 2:1 decimator whose
 * stopband edge is pinned on the 24 kHz output's Nyquist, 12000 Hz.  Passband 8850 Hz.  The
 * resampler that follows runs at step 1.00000, since 48/24 is exactly 2.
 *
 * Single-stage by necessity and NOT half-band, both forced -- see the tap-count block above for
 * the measurements behind each.  Contrast with the /4 and /6 chains, whose first stage is short
 * because a later rate change is still pending, so it only has to suppress what would fold into
 * the final band after that change.
 *
 * `variant` picks which output rate the stopband is pinned on -- the same arrangement the /4
 * front end uses, and mandatory for the same reason:
 *
 *   FOR_24000 -- stopband 12000 Hz, passband 8850 Hz.  The decimator's own output IS the final
 *                rate, so the resampler runs at step 1.00000.
 *   FOR_22050 -- stopband 11025 Hz, passband 7875 Hz.  The resampler then pulls 24 k to
 *                22.05 k at step 1.08843, so the OUTPUT Nyquist is 975 Hz below the
 *                intermediate's and the FOR_24000 coefficients would leave -18.9 dB of
 *                11.025-12 kHz input to fold in (measured; see the CRC block above).
 */
bool asrc_decimator_48_to_24_init(asrc_decimator_48_to_24_t* state,
                                  uint8_t channels,
                                  asrc_decimator_48_to_24_variant_t variant);
size_t asrc_decimator_48_to_24_output_frames(
    const asrc_decimator_48_to_24_t* state,
    size_t input_frames);
bool asrc_decimator_48_to_24_process_f32(
    asrc_decimator_48_to_24_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);
bool asrc_decimator_48_to_24_process_s24_left(
    asrc_decimator_48_to_24_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);

/*
 * Front end for the /4 runtime paths: a 27-tap 2:1 decimator to 24 kHz, then a 129-tap 2:1
 * decimator to 12 kHz whose stopband edge is pinned at the OUTPUT Nyquist of the rate being
 * served.  `variant` picks which output rate that is:
 *
 *   FOR_11025 -- stopband 5512.5 Hz, passband 4200 Hz.  The resampler then pulls 12 k to
 *                11.025 k at step 1.08843.
 *   FOR_12000 -- stopband 6000.0 Hz, passband 4700 Hz.  The decimator's own output IS the
 *                final rate, so the resampler runs at step 1.00000.
 *
 * Stage 1 is short because it only has to suppress what would fold into the FINAL band after
 * both halvings, so its stopband edge is (24000 - output Nyquist) and its transition is
 * enormous.  Stage 2 carries the whole anti-alias job and is mandatory in both variants: the
 * resampler's fixed fc = 0.464 of its input contributes about -0.2 dB at BOTH rates -- nowhere
 * near the -100 dB gate, because the worst aliases arrive at multiples of the intermediate rate
 * where the resampler sees a low frequency and passes them at full level.
 */
bool asrc_decimator_48_to_12_init(asrc_decimator_48_to_12_t* state,
                                  uint8_t channels,
                                  asrc_decimator_48_to_12_variant_t variant);
size_t asrc_decimator_48_to_12_output_frames(
    const asrc_decimator_48_to_12_t* state,
    size_t input_frames);
bool asrc_decimator_48_to_12_process_f32(
    asrc_decimator_48_to_12_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);
bool asrc_decimator_48_to_12_process_s24_left(
    asrc_decimator_48_to_12_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);

/*
 * PRE-STAGE for a 96 kHz input leg: a single 27-tap 2:1 decimator to 48 kHz, placed in FRONT of
 * any of the chains above so they can serve a 96 kHz leg unchanged.  One shared coefficient set,
 * so no `variant` argument -- see the tap-count block above for why so few taps suffice and why the
 * stand-alone price of 107 is the wrong shape for this position.
 *
 * Composed ratios, which is what the routing gate publishes and the feed-forward plan divides by:
 *
 *   /2       -> 48 kHz     the pre-stage alone; the resampler then runs at step 1.00000
 *   /2 + /2  -> 22.05 kHz  composed den 4, step 1.08843  (the `:22k` variant behind it)
 *   /2 + /3  -> 16 kHz     composed den 6
 *   /2 + /4  -> 12 kHz     composed den 8, step 1.00000; or 11.025 kHz at step 1.08843
 *   /2 + /6  ->  8 kHz     composed den 12
 *
 * 22.05 kHz JOINED ON 2026-09-02, and the reason is worth keeping because the old reasoning was
 * correct when it was written.  It used to read "22.05 and 24 kHz deliberately do NOT get this
 * stage: their R+jitter is already 85 and 80 against the cap of 104, so they resample directly and
 * pay nothing."  R+jitter against the cap is the HARD bound, and it still fits -- but
 * ASRC_FILL_SLACK_REQUIRED (2026-08-24, retired 2026-09-12) added a SOFT bound the direct path
 * failed: at step 4.3537 the setpoint was pinned at R + ASRC_FILL_JITTER, leaving 4 frames of slack
 * where the servo needed 8, so audio_app_asrc_rate_pair_is_supported() refused the pair outright.
 * A composed /4 takes the step to 1.0884 (R 32, set 64, slack 32) and it is accepted.
 *
 * THE STARVE REFUSAL IS GONE as of 2026-09-12: the reserve law (asrc_fill_law) lets the setpoint
 * rise with R, so 96 k -> 22.05 kHz direct is starve-safe at set 99 with a full one-block reserve.
 * The pair is nevertheless still refused on the direct path, by the A1 QUALIFICATION FENCE -- A1
 * fixes starve on the supported surface and does not widen it.  What the /4 row buys is the BAND
 * LIMIT, so the row is unchanged and its reason is now stated once, in the taps paragraph above,
 * instead of resting on a starve verdict.
 *
 * 24 kHz still has no row here.  It used to escape the same refusal only via the exact-step
 * exemption (step exactly 4, floor == ceil) -- an exemption since deleted, because an exact step is
 * the WORST block-phase case and not the safest; it is now admitted on a real reserve like every
 * other row.  Either way it RUNS with no band limit but ASRC_POLY_FC of 96 kHz (44.64 kHz), i.e.
 * 12-44.64 kHz folds into its band.  The pre-stage coefficients above
 * already cover it; publishing the row is a routing decision, not a filter one.
 * (Both rates' 48 kHz-input /2 rows are unaffected and still use asrc_decimator_48_to_24_*.)
 */
#if ASRC_DECIMATOR_HAS_96_TO_48
bool asrc_decimator_96_to_48_init(asrc_decimator_96_to_48_t* state,
                                  uint8_t channels);
size_t asrc_decimator_96_to_48_output_frames(
    const asrc_decimator_96_to_48_t* state,
    size_t input_frames);
bool asrc_decimator_96_to_48_process_f32(
    asrc_decimator_96_to_48_t* state,
    const float* input,
    size_t input_frames,
    size_t input_stride,
    float* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);
bool asrc_decimator_96_to_48_process_s24_left(
    asrc_decimator_96_to_48_t* state,
    const int32_t* input,
    size_t input_frames,
    size_t input_stride,
    int32_t* output,
    size_t output_capacity_frames,
    size_t output_stride,
    size_t* output_frames);
#endif /* ASRC_DECIMATOR_HAS_96_TO_48 */

/*
 * Boot equivalence check.  Streams a deterministic pseudo-random block sequence
 * through every front end chain COMPILED INTO THIS BUILD -- /6, /3, both coefficient variants of
 * each of /2 and /4, so six, plus the 96 -> 48 kHz pre-stage where ASRC_DECIMATOR_HAS_96_TO_48
 * (seven) -- and through a REFERENCE implementation that keeps
 * the original per-channel modulo-ring loop, and requires the s24 outputs to be
 * BIT-EXACT.  The run is long enough to wrap both ring histories (stage 2 needs
 * >147 stage-1 outputs, i.e. >441 input frames).  Returns true on pass.
 */
#if APP_ASRC_FRONTEND_SELFTEST
bool asrc_decimator_selftest(void);
#endif /* APP_ASRC_FRONTEND_SELFTEST -- absent, not stubbed: a build without the
        * check must not be able to print "pass" for a check it never ran. */

/*
 * ---------------------------------------------------------------------------
 * Q31 front end (asrc_decimator_q31.inc)
 * ---------------------------------------------------------------------------
 * The shipping front end.  Same chains, same coefficients, same band edges as
 * the float implementation above -- the arithmetic is Q31 and the width is
 * ASRC_CH channels instead of 2.  See the file header of
 * asrc_decimator_q31.inc for the placement rules (coefficients in X, history in
 * Y, Y modulo only) and why they are safety properties rather than tuning.
 *
 * There is ONE instance, not a caller-supplied state struct: the routing
 * invariant is that at most one direction ever has a front end (only the
 * down-sampling one -- see asrc_audio_path.h), so a second instance could only
 * ever be dead weight, and the 12,160 B Y history is the single largest object
 * this front end owns.  A state pointer would invite exactly the
 * "12,160 x 2" expansion the design forbids.
 *
 * `num` / `den` name the chain as the RATIO it applies, output over input:
 *
 *     1/2  1/3  1/4  1/6   the integer Nyquist chains (one or two FIR stages)
 *     2/3                  48 -> 32 kHz AUDIO MODE, L=2/M=3 rational polyphase
 *
 * A pair, not a single divider, because 2/3 and 1/3 share den == 3 and are
 * different filters at different output rates: 48 -> 32 kHz against
 * 48 -> 16 kHz.  Selecting on `den` alone would silently run the 48 -> 16 kHz
 * band edges (stopband 8 kHz) on a 32 kHz output, which is not a build error and
 * not audible as a wrong rate -- only as a dull, aliased 32 kHz.  Every integer
 * chain therefore passes num == 1, and `num` is the only thing that reaches the
 * rational path.
 *
 * The variant arguments pick the coefficient set where a stage has more than
 * one; each is ignored by the stages that do not.  `vpre` is the pre-stage's own
 * set and is read only when `with_prestage` is true.
 *
 * `with_prestage` composes the fixed 96 -> 48 kHz /2 stage in FRONT of the chain
 * `num`/`den` names, which is how a 96 kHz leg is served: the composed ratio is
 * den*2, and `den == 1` means the pre-stage alone (96 -> 48 kHz, resampler at step
 * 1.00000).  It is a separate argument rather than a wider `den` on purpose --
 * everything behind it stays a 48 kHz-input chain selected by the same divider and
 * the same coefficient variants, so the pre-stage is one more cascade stage rather
 * than a second set of rows.  A RATIONAL chain IS reachable behind it since
 * 2026-09-03: 2/3 after a /2 is the 96 -> 32 kHz row (composed num 2 / den 6), host-
 * and hardware-measured, and it inherits the 2/3 chain's partial specification
 * unchanged (`0-13 kHz protected / 13-16 kHz relaxed`).  Because a composed chain
 * then has THREE stages, dispatch is on `num` and never on a stage count.  `true` in
 * a build with no 96 kHz leg is refused, not silently dropped -- dropping it would run the second stage
 * at twice its intended input rate, which is a wrong filter and not a wrong rate.
 *
 * `vpre` names the pre-stage's coefficient set.  A WIDE set (FOR_44100 / FOR_48000)
 * is accepted only with `den == 1`, i.e. only when the pre-stage IS the chain: it
 * is sized to live in the second stage's history arena, so composing one behind
 * anything is refused rather than overlapped.  SHARED with `den == 1` is refused
 * too -- 96 -> 48 kHz with a 36 kHz stopband protects nothing above 24 kHz, which
 * is the whole reason those rows waited for a variant.
 */
bool asrc_decimator_q31_init(uint32_t num, uint32_t den, uint8_t channels,
                             asrc_decimator_48_to_24_variant_t v24,
                             asrc_decimator_48_to_12_variant_t v12,
#if ASRC_DECIMATOR_HAS_96_TO_48
                             asrc_decimator_96_to_48_variant_t vpre,
#endif
                             bool with_prestage);

#if APP_ASRC_LPF1TO1_PROTOTYPE
/*
 * EXPERIMENTAL: arm a 1:1 anti-alias LPF -- a stage whose rate RATIO is 1/1 and which
 * nevertheless filters.  Separate from asrc_decimator_q31_init() on purpose: that function
 * dispatches on a divider and refuses den == 1, and both of those are correct for the chains it
 * serves.  Everything downstream is shared -- the same stage descriptor, the same ring, the same
 * assembler kernel with decim_bytes = 4, and the same single-stage arm of
 * asrc_decimator_q31_process_s24_left().
 *
 * Selection is by BOTH rates: the stopband edge is Fs_out - 0.8125 * (Fs_out / 2), so the source
 * rate alone cannot pick the set.  A pair with no coefficient set is REFUSED, never served by a
 * neighbouring set's band edges.
 *
 * Emits one output frame per input frame, so the caller's scratch must hold a whole block --
 * see DECIMATED_MIN_DEN in asrc_audio_path.c.
 */
bool asrc_decimator_q31_init_lpf1to1(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                     uint8_t channels);

/* Host/target selftest for the 1:1 stage: impulse response against the coefficient table,
 * N-in/N-out framing, block-boundary continuity over a multi-block random vector, and channel
 * isolation at the full ASRC_CH width.  Returns false and fills
 * asrc_decimator_q31_selftest_fail() on the first mismatch. */
bool asrc_decimator_q31_lpf1to1_selftest(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                         uint32_t *rounding_ties);

/*
 * EXPERIMENTAL: arm a COMPOSED front end -- an integer decimating stage, then the 1:1 stage at
 * the intermediate rate.  44.1 kHz -> /2 -> 22.05 kHz -> 1:1 -> rear ASRC at step 1.378125.
 *
 * This is the first front end in this tree whose two stages were designed AGAINST EACH OTHER:
 * the /2 set's stopband only has to reach Fs_mid - Fstop(1:1), because the 1:1 stage behind it
 * removes the rest, which is what makes it 51 taps instead of the shipped 107.  So the pair of
 * coefficient sets is one design and is regenerated together (asrc_lpf1to1_design.py).
 *
 * `den` is the PUBLISHED ratio and is checked, not trusted: it is what the rear step and the
 * caller's output sizing were computed from, so a row whose ratio disagrees with the chain is a
 * planner bug and is refused instead of being served by a chain of a different ratio.
 *
 * The block path is unchanged -- the generic integer cascade already runs two stages; the only
 * novelty is decim == 1 on the last one.
 */
bool asrc_decimator_q31_init_dec_then_lpf1to1(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                               uint32_t den, uint8_t channels);

#if APP_ASRC_ANTIALIAS_DIRECT_1TO1
/* THE SIX MEASURED DIRECT-ROUTE HOLES, WRITTEN ONCE.
 *
 * Each row keeps the rear ratio at 1/1 and inserts a source-rate band-limit.  The coefficient
 * identifiers intentionally repeat existing arrays: re-derivation at each row's rates matched the
 * stored coefficients to <= 7.4e-11 relative because the normalised band edges are identical.
 * Planner, arm, telemetry tag and sizing asserts all expand this same list.
 *
 * X(fs_in, fs_out, TAPS, coefficient, tag) */
#define ASRC_ANTIALIAS_DIRECT_1TO1_ROWS(X)                                                   \
    X( 11025u,  8000u, ASRC_LP1_22K05_TO_16K_TAPS,  s_lp1_22k05_to_16k_coeff,  ":11k025>8k" ) \
    X( 12000u,  8000u, ASRC_LP1_24K_TO_16K_TAPS,    s_lp1_24k_to_16k_coeff,    ":12k>8k" )     \
    X( 16000u,  8000u, ASRC_LP1_16K_TO_8K_TAPS,     s_lp1_16k_to_8k_coeff,     ":16k>8k" )     \
    X( 16000u, 11025u, ASRC_LP1_16K_TO_11K025_TAPS, s_lp1_16k_to_11k025_coeff, ":16k>11k025" ) \
    X( 16000u, 12000u, ASRC_LP1_16K_TO_12K_TAPS,    s_lp1_16k_to_12k_coeff,    ":16k>12k" )    \
    X( 22050u, 11025u, ASRC_LP1_16K_TO_8K_TAPS,     s_lp1_16k_to_8k_coeff,     ":22k05>11k025" ) \
    ASRC_ANTIALIAS_DIRECT_1TO1_EXT_ROWS(X) \
    ASRC_ANTIALIAS_DIRECT5_1TO1_ROWS(X)

/* NINE MORE DIRECT-ROUTE HOLES, SAME CLASS (APP_ASRC_ANTIALIAS_DIRECT_1TO1_EXT, default 0).
 *
 * The 2026-10-03 qualification (509badd) still FAILs 14 direct down-conversions on protected-band
 * alias.  For these nine, the normalised passband/stopband (0.8125 * fs_out/2 and
 * fs_out - that, divided by fs_in) equals an array already in asrc_lpf1to1_coeffs.inc, so each row
 * is a selector entry only -- no new coefficient array.  The remaining five (24->8, 24->11.025,
 * 32->8, 32->12, 44.1->11.025) need a new design or are too heavy as a single 1:1 stage, and are
 * not here. */
#if APP_ASRC_ANTIALIAS_DIRECT_1TO1_EXT
#define ASRC_ANTIALIAS_DIRECT_1TO1_EXT_ROWS(X)                                                   \
    X( 22050u, 12000u, ASRC_LP1_22K05_TO_12K_TAPS,  s_lp1_22k05_to_12k_coeff,  ":22k05>12k" )   \
    X( 22050u, 16000u, ASRC_LP1_22K05_TO_16K_TAPS,  s_lp1_22k05_to_16k_coeff,  ":22k05>16k" )   \
    X( 24000u, 12000u, ASRC_LP1_16K_TO_8K_TAPS,     s_lp1_16k_to_8k_coeff,     ":24k>12k" )     \
    X( 24000u, 16000u, ASRC_LP1_24K_TO_16K_TAPS,    s_lp1_24k_to_16k_coeff,    ":24k>16k" )     \
    X( 32000u, 22050u, ASRC_LP1_16K_TO_11K025_TAPS, s_lp1_16k_to_11k025_coeff, ":32k>22k05" )   \
    X( 32000u, 24000u, ASRC_LP1_16K_TO_12K_TAPS,    s_lp1_16k_to_12k_coeff,    ":32k>24k" )     \
    X( 44100u, 22050u, ASRC_LP1_16K_TO_8K_TAPS,     s_lp1_16k_to_8k_coeff,     ":44k1>22k05" )  \
    X( 44100u, 24000u, ASRC_LP1_22K05_TO_12K_TAPS,  s_lp1_22k05_to_12k_coeff,  ":44k1>24k" )    \
    X( 44100u, 32000u, ASRC_LP1_22K05_TO_16K_TAPS,  s_lp1_22k05_to_16k_coeff,  ":44k1>32k" )
#else
#define ASRC_ANTIALIAS_DIRECT_1TO1_EXT_ROWS(X)
#endif

/* THE LAST FIVE DIRECT-ROUTE HOLES (APP_ASRC_ANTIALIAS_DIRECT5, default 0).
 *
 * The five the EXT note above left out (cd951f5 qualification: still alias X).  None needs a new
 * coefficient array: every stage below has the same normalised band edges as an array the image
 * already carries, re-derived and compared coefficient by coefficient
 * ([internal tool] antialias_direct5_reuse_check.py, <= 1.6e-10 relative; chain alias -108.9 .. -115.0
 * dBc Q31).  Two are single 1:1 rows and extend the list above; three are composed and get their
 * own list, in the Universal composed shape, because no direct row has decimated before.
 *
 *   24 -> 11.025    1:1 = 48 -> 22.05 at half the clock
 *   32 -> 12        1:1 = the 96 -> 12 row's 32 kHz partner (owner 2026-10-04: 1:1 rather than a
 *                   /2 + 1:1 that would need a new /2 set; ~49 % front end at 16 ch, host model)
 *   24 -> 8         /2 + 1:1 = 48 -> 16 at half the clock
 *   32 -> 8         /3 + 1:1 = 48 -> 12 at 2/3 the clock
 *   44.1 -> 11.025  /3 + 1:1 = 48 -> 12 at 0.91875 x the clock
 *
 * The composed tags name both rates like the Universal ones, so `fe=/3+lp1:32k>8k` cannot be read
 * as the 48 -> 12 row whose arrays it shares. */
#if APP_ASRC_ANTIALIAS_DIRECT5
#define ASRC_ANTIALIAS_DIRECT5_1TO1_ROWS(X)                                                      \
    X( 24000u, 11025u, ASRC_LP1_48K_TO_22K05_TAPS,  s_lp1_48k_to_22k05_coeff,  ":24k>11k025" )  \
    X( 32000u, 12000u, ASRC_LP1_32K_TO_12K_TAPS,    s_lp1_32k_to_12k_coeff,    ":32k>12k" )
#define ASRC_ANTIALIAS_DIRECT5_COMPOSED_ROWS(X)                                                  \
    X( 24000u,  8000u, 2u, ASRC_DEC2_48K_FOR_16K_DECIM,                                          \
       ASRC_DEC2_48K_FOR_16K_TAPS,      s_dec2_48k_for_16k_coeff,                                \
       ASRC_LP1_24K_TO_16K_TAPS,        s_lp1_24k_to_16k_coeff,       ":24k>8k"      )           \
    X( 32000u,  8000u, 3u, ASRC_DEC3_48K_FOR_12K_DECIM,                                          \
       ASRC_DEC3_48K_FOR_12K_TAPS,      s_dec3_48k_for_12k_coeff,                                \
       ASRC_LP1_16K_TO_12K_TAPS,        s_lp1_16k_to_12k_coeff,       ":32k>8k"      )           \
    X( 44100u, 11025u, 3u, ASRC_DEC3_48K_FOR_12K_DECIM,                                          \
       ASRC_DEC3_48K_FOR_12K_TAPS,      s_dec3_48k_for_12k_coeff,                                \
       ASRC_LP1_16K_TO_12K_TAPS,        s_lp1_16k_to_12k_coeff,       ":44k1>11k025" )
#else
#define ASRC_ANTIALIAS_DIRECT5_1TO1_ROWS(X)
#define ASRC_ANTIALIAS_DIRECT5_COMPOSED_ROWS(X)
#endif

/* The exact composed-row tag, or NULL when (rates, ratio) is not a DIRECT5 composed row. */
const char* asrc_antialias_direct_composed_tag(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                               uint32_t den);

/* The exact pair tag, or NULL when the pair is not one of the direct 1:1 rows. */
const char* asrc_antialias_direct_1to1_tag(uint32_t out_rate_hz, uint32_t in_rate_hz);
#endif

#if APP_ASRC_UNIVERSAL_PROFILE
/*
 * THE UNIVERSAL PROFILE'S 15 ROWS, WRITTEN ONCE.
 *
 * WHY A LIST AND NOT THREE TABLES.  Four places need the same rows and need them to AGREE: the
 * planner (which pair gets a Universal chain, and what ratio it publishes), the two arm functions
 * (which coefficient sets), the compile-time sizing (every row's two rings against the arena) and
 * the telemetry tag.  Phase H's own finding was that the same rate pair having its spec written in
 * two places is what let "the generator passes" be read as "the uniform spec passes"; a second
 * copy of the ROW SET is the same defect one level down.  So the rows are data here and every
 * consumer expands them.  The host's single source is asrc_universal_coeff_provenance.py's
 * UNIVERSAL_ROUTES; these two lists are its firmware counterpart, in the same order.
 *
 * THE ARGUMENTS ARE DELIBERATELY MORE THAN ANY ONE CONSUMER USES.  The coefficient identifiers and
 * tap-count macros are private to asrc_lpf1to1_coeffs.inc, so an expansion outside that
 * translation unit -- the planner's -- simply does not name them and they never resolve.  That is
 * what lets one list serve both without the planner having to see the coefficients.
 *
 *   ASRC_UNIVERSAL_1TO1_ROWS(X)      X(fs_in, fs_out, TAPS, coeff, tag)
 *   ASRC_UNIVERSAL_COMPOSED_ROWS(X)  X(fs_in, fs_out, den, DECIM, TAPS0, c0, TAPS1, c1, tag)
 *
 * `den` is the PUBLISHED composed ratio and DECIM is the stage-0 decimation the coefficient set
 * was generated for.  They are equal on every row here -- stage 1 is 1:1, so it does not divide --
 * and both are carried so that the arm can ASSERT the equality instead of assuming it.  That is
 * the check that catches a row whose published ratio (which is what the rear step and the caller's
 * scratch sizing were computed from) disagrees with the chain actually armed.
 *
 * THE TAG NAMES BOTH RATES, which the pre-Universal tags do not.  Those name only the protected
 * rate because no two prototype rows shared one; here 48 -> 8 kHz and 96 -> 8 kHz are different
 * chains with the same protected rate, and `fe=/3+lp1:8k` would not tell them apart.
 */
/* SPLIT BY LEG RATE, and then joined below.  A build with no 96 kHz leg cannot ever be asked for
 * a 96 kHz row -- the rate is unreachable -- so including those rows would put their coefficient
 * arrays in the image to serve a pair that cannot be selected.  Every array here is `static const`
 * in one translation unit, so what keeps them out is not naming them, which is what this split
 * does.  (Whether --gc-sections would have dropped them anyway is a MAP question and is section 7's
 * to answer; not naming them does not depend on the answer.) */
#define ASRC_UNIVERSAL_1TO1_ROWS_48K(X)                                                        \
    X( 48000u, 22050u, ASRC_LP1_48K_TO_22K05_TAPS, s_lp1_48k_to_22k05_coeff, ":48k>22k05" )     \
    X( 48000u, 24000u, ASRC_LP1_48K_TO_24K_TAPS,   s_lp1_48k_to_24k_coeff,   ":48k>24k"   )     \
    X( 48000u, 32000u, ASRC_LP1_48K_TO_32K_TAPS,   s_lp1_48k_to_32k_coeff,   ":48k>32k"   )

#define ASRC_UNIVERSAL_1TO1_ROWS_96K(X)                                                        \
    X( 96000u, 48000u, ASRC_LP1_96K_TO_48K_TAPS,   s_lp1_96k_to_48k_coeff,   ":96k>48k"   )

#define ASRC_UNIVERSAL_COMPOSED_ROWS_48K(X)                                                    \
    X( 48000u,  8000u, 3u, ASRC_DEC3_48K_FOR_8K_DECIM,                                         \
       ASRC_DEC3_48K_FOR_8K_TAPS,       s_dec3_48k_for_8k_coeff,                               \
       ASRC_LP1_16K_TO_8K_TAPS,         s_lp1_16k_to_8k_coeff,        ":48k>8k"      )          \
    X( 48000u, 11025u, 3u, ASRC_DEC3_48K_FOR_11K025_DECIM,                                     \
       ASRC_DEC3_48K_FOR_11K025_TAPS,   s_dec3_48k_for_11k025_coeff,                            \
       ASRC_LP1_16K_TO_11K025_TAPS,     s_lp1_16k_to_11k025_coeff,    ":48k>11k025"  )          \
    X( 48000u, 12000u, 3u, ASRC_DEC3_48K_FOR_12K_DECIM,                                        \
       ASRC_DEC3_48K_FOR_12K_TAPS,      s_dec3_48k_for_12k_coeff,                               \
       ASRC_LP1_16K_TO_12K_TAPS,        s_lp1_16k_to_12k_coeff,       ":48k>12k"     )          \
    X( 48000u, 16000u, 2u, ASRC_DEC2_48K_FOR_16K_DECIM,                                        \
       ASRC_DEC2_48K_FOR_16K_TAPS,      s_dec2_48k_for_16k_coeff,                               \
       ASRC_LP1_24K_TO_16K_TAPS,        s_lp1_24k_to_16k_coeff,       ":48k>16k"     )

#define ASRC_UNIVERSAL_COMPOSED_ROWS_96K(X)                                                    \
    X( 96000u,  8000u, 3u, ASRC_DEC3_96K_FOR_8K_DECIM,                                         \
       ASRC_DEC3_96K_FOR_8K_TAPS,       s_dec3_96k_for_8k_coeff,                                \
       ASRC_LP1_32K_TO_8K_TAPS,         s_lp1_32k_to_8k_coeff,        ":96k>8k"      )          \
    X( 96000u, 11025u, 3u, ASRC_DEC3_96K_FOR_11K025_DECIM,                                     \
       ASRC_DEC3_96K_FOR_11K025_TAPS,   s_dec3_96k_for_11k025_coeff,                            \
       ASRC_LP1_32K_TO_11K025_TAPS,     s_lp1_32k_to_11k025_coeff,    ":96k>11k025"  )          \
    X( 96000u, 12000u, 3u, ASRC_DEC3_96K_FOR_12K_DECIM,                                        \
       ASRC_DEC3_96K_FOR_12K_TAPS,      s_dec3_96k_for_12k_coeff,                               \
       ASRC_LP1_32K_TO_12K_TAPS,        s_lp1_32k_to_12k_coeff,       ":96k>12k"     )          \
    X( 96000u, 16000u, 3u, ASRC_DEC3_96K_FOR_16K_DECIM,                                        \
       ASRC_DEC3_96K_FOR_16K_TAPS,      s_dec3_96k_for_16k_coeff,                               \
       ASRC_LP1_32K_TO_16K_TAPS,        s_lp1_32k_to_16k_coeff,       ":96k>16k"     )          \
    X( 96000u, 22050u, 3u, ASRC_DEC3_96K_FOR_22K05_DECIM,                                      \
       ASRC_DEC3_96K_FOR_22K05_TAPS,    s_dec3_96k_for_22k05_coeff,                             \
       ASRC_LP1_32K_TO_22K05_TAPS,      s_lp1_32k_to_22k05_coeff,     ":96k>22k05"   )          \
    X( 96000u, 24000u, 3u, ASRC_DEC3_96K_FOR_24K_DECIM,                                        \
       ASRC_DEC3_96K_FOR_24K_TAPS,      s_dec3_96k_for_24k_coeff,                               \
       ASRC_LP1_32K_TO_24K_TAPS,        s_lp1_32k_to_24k_coeff,       ":96k>24k"     )          \
    X( 96000u, 32000u, 2u, ASRC_DEC2_96K_FOR_32K_DECIM,                                        \
       ASRC_DEC2_96K_FOR_32K_TAPS,      s_dec2_96k_for_32k_coeff,                               \
       ASRC_LP1_48K_TO_32K_TAPS,        s_lp1_48k_to_32k_coeff,       ":96k>32k"     )

#if ASRC_DECIMATOR_HAS_96_TO_48
#define ASRC_UNIVERSAL_1TO1_ROWS(X)                                                            \
    ASRC_UNIVERSAL_1TO1_ROWS_48K(X)                                                            \
    ASRC_UNIVERSAL_1TO1_ROWS_96K(X)
#define ASRC_UNIVERSAL_COMPOSED_ROWS(X)                                                        \
    ASRC_UNIVERSAL_COMPOSED_ROWS_48K(X)                                                        \
    ASRC_UNIVERSAL_COMPOSED_ROWS_96K(X)
#else
#define ASRC_UNIVERSAL_1TO1_ROWS(X)      ASRC_UNIVERSAL_1TO1_ROWS_48K(X)
#define ASRC_UNIVERSAL_COMPOSED_ROWS(X)  ASRC_UNIVERSAL_COMPOSED_ROWS_48K(X)
#endif

/* THE TAG for a Universal row, or NULL when the (rates, ratio) triple is not one.  NULL rather
 * than "" so the caller can tell "not a Universal row" from "a Universal row with no tag", which
 * is the distinction the pre-Universal tag code could not make (it returned "" for both and the
 * log then printed a chain name with nothing after it). */
const char* asrc_universal_1to1_tag(uint32_t out_rate_hz, uint32_t in_rate_hz);
const char* asrc_universal_composed_tag(uint32_t out_rate_hz, uint32_t in_rate_hz, uint32_t den);
#endif /* APP_ASRC_UNIVERSAL_PROFILE */

/*
 * FRAMES STAGE `stage` HAS PRODUCED since the last arm.  0 for a stage the live chain does not
 * have, and 0 for one that never ran.
 *
 * The per-stage counterpart of asrc_audio_path_frontend_runs_ab(): that counts blocks the front
 * end ran, which for a single-stage chain is the same as "the filter ran", and for a COMPOSITION
 * is not -- a chain can run its first stage and skip its second, which is precisely the failure
 * mode the 2026-09-14 ISR gate produced one level up (`fe=lp1` printed for twelve minutes while
 * the FIR was never called).  A /2 followed by a 1:1 must report the SAME count on both stages.
 */
uint32_t asrc_decimator_q31_stage_output_frames(uint8_t stage);

#if APP_ASRC_FRONTEND_SELFTEST
/* Boot check for the COMPOSED chain, against the divider selftest's oracle (which walks
 * s_q31.stages, so it follows whatever was armed -- it is reused, not copied).  Proves both
 * stages ran, the intermediate stayed full-precision Q31, the two rings do not overlap,
 * continuity across process() calls and ring wraps, and channel replication.  It also reads the
 * per-stage counters where their answer is known, so the runtime instrument is proven at boot
 * rather than trusted on the bench.  One-shot: re-entering it while a chain is live would race
 * the block ISR. */
bool asrc_decimator_q31_dec_then_lpf1to1_selftest(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                                  uint32_t den, uint32_t *rounding_ties);
#endif
#endif

/* NON-ZERO HISTORY WORDS IN THE Q31 ARENA, right now.
 *
 * Called by the audio path where it publishes a freshly armed chain as ready, so that "arming
 * leaves no history behind" is a number taken on the RUNTIME path (a console `*ar` rate change
 * arms through asrc_transport_reset(), not through the boot selftests' direct init calls).
 * Expected 0 at that moment.  Arm-time only -- it walks the whole arena. */
uint16_t asrc_decimator_q31_history_nonzero_words(void);

#if APP_ASRC_FRONTEND_SELFTEST
/* Proves this check is worth reading, then proves the clear, in four steps: the arena really was
 * dirty (the earlier checks' leftover history, or a poke if a reordering left it empty), an arm
 * clears it, the scan COUNTS rather than always returning 0, and the arm clears that too.
 * `out_seen` (optional) returns how dirty the arena was when the check ran.
 * Boot only: it writes the live arena. */
bool asrc_decimator_q31_arm_clear_selftest(uint16_t* out_seen);
#endif

/* Frames this front end would emit for `input_frames`, at its current phase. */
size_t asrc_decimator_q31_output_frames(size_t input_frames);

/* s24-left in, s24-left out.  All-or-nothing on output capacity, like the float
 * entry points: a one-frame shortfall returns false rather than truncating.
 *
 * `input_channels` is the real channel count of the source block (2 for I2S/TDM
 * stereo); it must be a power of two.  The front end repeats those channels up
 * to ASRC_CH, the same way the resampler behind it does, so the output is
 * `channels` wide but only the first `input_channels` of them carry distinct
 * audio.  The rest are the 16ch workload. */
bool asrc_decimator_q31_process_s24_left(const int32_t *input,
                                         size_t input_frames,
                                         size_t input_stride,
                                         uint8_t input_channels,
                                         int32_t *output,
                                         size_t output_capacity_frames,
                                         size_t output_stride,
                                         size_t *output_frames);

#if APP_ASRC_LEG_PROFILE
/* Peak-held tick totals of the composed rational chain's two arithmetic halves, and clear.
 * `pre_ticks` is every cascade stage ahead of the phase rows (the 96 -> 48 kHz /2 pre-stage
 * for the 96 -> 32 kHz pair); `r23_ticks` is the phase rows (the 97-tap 48 -> 32 kHz
 * resampler).  Foreground-only reader -- it clears the peaks for the next window.  Both are
 * 0 in a build whose live chain is not the rational one, which means "this chain did not
 * run", never "it was free". */
void asrc_decimator_q31_profile_read( uint32_t* pre_ticks, uint32_t* r23_ticks );
/* Same two halves, but the LAST call's values rather than the peaks, read-and-clear.  This is
 * what a caller adds to its own rows so that every row describes one and the same block. */
void asrc_decimator_q31_profile_take_last( uint32_t* pre_ticks, uint32_t* r23_ticks );
#endif


/* ---- 96 -> 32 kHz THIRD-BAND front end (opt-in, replaces the composed chain) ---------
 *
 * A 3-path polyphase allpass decimator: H(z) = (1/3)[A0(z^3) + z^-1 A1(z^3) + z^-2 A2(z^3)],
 * 16 first-order sections against the composed chain's 138 taps, and -110.11 dBc of worst
 * alias into the protected 0-15 kHz band against the composed chain's -22.51 dBc.
 *
 * It is NOT a strict full-band front end and is not sold as one: 0-15 kHz is protected,
 * 15-16 kHz is RELAXED at -3.53 dBc, and the phase is not linear (group delay varies 388 us
 * over 0-15 kHz).  Both are accepted specification for this front end.
 *
 * Entry points other than the mode switch are for the rate-change hook and the console.
 * The block path reaches it through asrc_decimator_q31_process_s24_left(), which dispatches
 * on `armed` -- there is deliberately no second process function for a caller to pick. */
#if APP_ASRC_THIRDBAND_96_TO_32
typedef enum
{
    ASRC_THIRDBAND_MODE_OFF = 0,   /* the composed 41-tap /2 + 97-tap 2/3 chain */
    ASRC_THIRDBAND_MODE_ON  = 1,   /* the third band */
} asrc_thirdband_mode_t;

/* Arm or retire, from the rate-change hook ONLY -- transport stopped, between mute and
 * unmute.  Arming loads this stage's coefficients over the composed chain's and zeroes 16
 * sections of recursive state; that is the one point where clearing it is correct, and a
 * per-block reset would turn the filter into a transient generator. */
void asrc_thirdband_96_to_32_arm(bool on);
bool asrc_thirdband_96_to_32_armed(void);

/* Record which front end is wanted.  Switches NOTHING by itself: the two share one arena,
 * so the caller must then take leg A through its ordinary mute-bounded restart (the console
 * re-applies leg A's rate, exactly as `*aj` does) and the arm hook builds what this names.
 * The switch is audible -- the third band needs 30.2 ms to settle to -120 dB -- which is the
 * cost of costing no extra RAM. */
void asrc_thirdband_96_to_32_set_mode(asrc_thirdband_mode_t mode);
asrc_thirdband_mode_t asrc_thirdband_96_to_32_mode(void);

/* Frames this front end would emit for `input_frames`, at its current commutator phase.
 * 3:1 with a 0..2 frame remainder carried between blocks. */
size_t asrc_thirdband_96_to_32_output_frames(size_t input_frames);

/* `clips` counts OUTPUT words that came out at s24-left full scale, which a unity-gain
 * filter whose L1 norm exceeds 1 does on transients -- the composed chain does it too
 * (L1 1.81 against this one's 3.45).  It is a fact to be measured, not a fault: read it
 * against real programme material rather than against a square wave.  `refused` counts
 * blocks longer than the gather buffer, which is a build mismatch and should be 0. */
/* Two coefficient-independent predictions of this structure, as a structural check: H(DC)
 * is exactly 1 because every allpass has A_k(1) = 1, and H(32 kHz) is exactly 0 because
 * 1 + W + W^2 = 0 there whatever the coefficients are.  Between them they catch the sign of
 * the kernel's headroom shift, the commutator's branch order, the 1/3 constant and a
 * coefficient row loaded backwards -- none of which the compiler can see.
 *
 * It runs the LIVE filter state, so stop audio first (`*ts`); it resets the state and the
 * counters on the way out.  Reached from the console as `*av03`. */
/* THE LIVE TAP.  Two 1024-sample 32 kHz windows, channel 0, filled from one arm inside the
 * live block path: buffer 0 = the stage's output, buffer 1 = the block leg B hands to the
 * codec.  So one capture spans push + AB FIFO + servo + the rear polyphase, which is what is
 * left to localise now that both ends of the stage itself measure clean.
 *
 * Costs no RAM: it sits in the idle part of the arena this stage overlays -- which is also why
 * arming REQUIRES the stage to be armed.  Those buffers belong to the composed chain's history
 * whenever the third band is retired, so the arm/retire hook cancels any capture in flight and
 * both fill paths refuse when the stage is not armed.
 *
 * The two sides finish INDEPENDENTLY (different interrupt domains, independent clocks) and
 * `tap_ready()` is true only once both have.  Arm with `*av05`, read with `*av06`.  The 96 kHz
 * input capture that used to be buffer 1 is gone -- it measured -96.90 dBc in band, which
 * closed that side. */
bool asrc_thirdband_96_to_32_tap_arm(void);   /* false = refused, the stage is not armed */
bool asrc_thirdband_96_to_32_tap_ready(uint16_t* out_n, uint16_t* b_n);
/* Per-side progress, so a console can name what it is still waiting for.  False = nothing
 * armed. */
bool asrc_thirdband_96_to_32_tap_side_done(bool* out_done, bool* b_done);
/* Capacity of each capture, reported next to the counts. */
void asrc_thirdband_96_to_32_tap_capacity(uint16_t* out_cap, uint16_t* b_cap);
int32_t asrc_thirdband_96_to_32_tap_word(uint8_t which, uint16_t i);
/* Called by leg B, right after the A->B pull.  No-op unless the stage and the tap are both
 * armed and this side has not already filled. */
void asrc_thirdband_96_to_32_tap_legb(const int32_t* block, uint32_t frames, uint32_t stride);

/* THE DYNAMIC CHECK.  A real waveform through the whole stage -- gather, headroom shift,
 * the 0..2 frame carry across blocks, kernel and the 1/3 -- against host-computed values.
 *
 * It exists because nothing else covered a MOVING signal: the kernel bench re-read one input
 * buffer every frame, and both of *av03's cases hold each branch's input constant, so the
 * allpass state never advanced in any earlier check.  It also drives every channel with the
 * same sample and requires the outputs to match, which catches shared state between channels.
 *
 * `first_bad` is the output frame index that first disagreed (-1 if none), `bad_channel` the
 * first channel that disagreed with channel 0 (-1 if none), `frames` how many outputs the
 * stage produced (must equal the vector's count).  Run with audio stopped (`*ts`); resets the
 * state and counters afterwards.  Reached from the console as `*av04`. */
typedef struct
{
    uint16_t frames;      /* outputs the stage produced           */
    uint16_t expect;      /* outputs the vector says it should    */
    int16_t  first_bad;
    int32_t  got;
    int32_t  want;
    int8_t   bad_channel;
    bool     refused;
    bool     pass;
} asrc_thirdband_vector_t;
bool asrc_thirdband_96_to_32_vector_check(asrc_thirdband_vector_t* r);

typedef struct
{
    int32_t dc_out;      /* settled output for a constant input   */
    int32_t dc_want;     /* the constant itself                   */
    int32_t dc_tol;
    int32_t null_out;    /* settled output for a 32 kHz input     */
    int32_t null_tol;
    bool    pass;
} asrc_thirdband_selftest_t;
bool asrc_thirdband_96_to_32_selftest(asrc_thirdband_selftest_t* r);

void asrc_thirdband_96_to_32_stats(uint32_t* blocks, uint32_t* out_frames,
                                   uint32_t* clips, uint32_t* refused);
void asrc_thirdband_96_to_32_stats_clear(void);

/* What is actually compiled in, for the console line: coefficient digest, section count,
 * the headroom shift in bits, and the host-designed worst alias in milli-dBc. */
void asrc_thirdband_96_to_32_identity(uint32_t* coeff_crc, uint16_t* sections,
                                      int16_t* headroom_shift, int32_t* worst_alias_milli);

#if APP_ASRC_LEG_PROFILE
/* The same two halves the composed chain reports, so one telemetry line describes either
 * front end: `gather` is everything ahead of the arithmetic (the channel repeat and the
 * headroom shift), `kernel` is the whole 3:1 arithmetic.  Read-and-clear. */
void asrc_thirdband_96_to_32_profile_take_last(uint32_t* gather, uint32_t* kernel);
#endif
#endif /* APP_ASRC_THIRDBAND_96_TO_32 */


#if APP_ASRC_FRONTEND_SELFTEST
/* Where the Q31 boot check stopped, as numbers: the caller already has a printf,
 * and formatting in here would need a second one (see the printf single-caller
 * note in the ROM diet report).  `what` is a string literal, so it costs no RAM. */
typedef struct
{
    const char* what;      /* "init" / "refused" / "frames" / "value" */
    uint16_t    den;
    uint16_t    block;
    uint16_t    index;     /* output frame within the block */
    uint8_t     kase;      /* 0 impulse, 1 DC, 2 normal, 3 near full scale */
    uint8_t     live_ch;   /* which source channel carried the signal */
    uint8_t     channel;   /* which output channel disagreed */
    int32_t     got;
    int32_t     want;
} asrc_decimator_q31_fail_t;

/* Bit-exactness of the Q31 front end against an independent Q31 oracle, over
 * every chain the routing gate can select.  `rounding_ties` (optional) counts
 * the differences attributable to the sacr.l rounding mode -- see the file
 * header of asrc_decimator_q31_selftest.inc.  A tie is not a failure; anything
 * else is. */
bool asrc_decimator_q31_selftest(uint32_t* rounding_ties);
const asrc_decimator_q31_fail_t* asrc_decimator_q31_selftest_fail(void);

/* CHANNEL ISOLATION of the 2/3 rational front end, at the full ASRC_CH width.
 *
 * Separate from the check above because it proves a different property and needs
 * a different shape.  The bit-exactness check runs two live channels against the
 * oracle; this one drives ALL ASRC_CH channels with per-channel distinct data
 * (a per-channel LCG seed) at stride ASRC_CH, and requires that channel N's
 * output equal the oracle of channel N's INPUT ALONE -- so any cross-channel
 * leak in the ring indexing, the batch interleave or the stride arithmetic shows
 * up as a mismatch instead of as fifteen channels of plausible audio.
 *
 * One channel is verified per call, `live_ch` selecting it, because the oracle
 * state for sixteen channels at once does not fit: see the stack note in
 * asrc_decimator_q31_selftest.inc.  The caller loops over all of them.
 * Failures land in the same asrc_decimator_q31_selftest_fail() record.
 *
 * The 16ch OUTPUT block comes from the caller (`out`, `out_frames` frames of
 * ASRC_DEC_Q31_CH interleaved samples) rather than from this function's stack.
 * Not a style choice: with the Q31 generic resampler compiled in
 * (ASRC_SAMPLE_Q31 == 1) the free RAM between the statics and the Y-space
 * history is 2,980 bytes of stack, and a 16-frame x 16-channel input block, a
 * 16ch output block and the 97-tap oracle together overflow it -- the AK512
 * image trapped with STACK ERROR before this parameter existed.  Passing a
 * buffer the caller ALREADY owns costs no RAM at all, where making it static
 * here would only move the same bytes out of the stack region and shrink the
 * stack by as much as it saved. */
bool asrc_decimator_q31_isolation_selftest(uint8_t live_ch, int32_t* out,
                                           size_t out_frames);
#endif /* APP_ASRC_FRONTEND_SELFTEST -- absent rather than stubbed, so a build
        * without the check cannot print "pass" for a check it never ran. */

#endif /* ASRC_DECIMATOR_48_TO_8_H */
