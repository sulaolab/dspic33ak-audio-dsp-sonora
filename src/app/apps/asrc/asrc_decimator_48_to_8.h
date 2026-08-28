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

#define ASRC_DECIMATOR_48_TO_8_MAX_CHANNELS (2u)
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
 * ONE COEFFICIENT SET, NO VARIANT ENUM -- the one place this is simpler than the /4 and /2 stages
 * it feeds.  Designed against the tightest of the four rates (16 kHz: passband 6400 Hz, stopband
 * from 40000 Hz), it satisfies all of them: stage alone -104.32 dB at 16 kHz and -107.56 dB at
 * 8/11.025/12 kHz, passband edge -0.0000 dB.  Composed with each existing chain over that chain's
 * own designed passband: ripple 0.0001 dB, added droop -0.00007 dB, i.e. transparent in band.
 *
 * RAM +336 B, and it does NOT join the union that holds the stages above: a composed chain runs the
 * pre-stage AND a second stage in the same block, so they need to exist at once. */
#if ASRC_DECIMATOR_HAS_96_TO_48
#define ASRC_DECIMATOR_96_TO_48_TAPS         (21u)
#define ASRC_DECIMATOR_96_TO_48_COEFF_CRC32 (0xE3A49ADCUL)
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
 * anti-alias front end at all, so the BA leg gets none. */
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
    (2u * (taps) * ASRC_DECIMATOR_48_TO_8_MAX_CHANNELS)

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

/* The 96 -> 48 kHz pre-stage.  Same shape as asrc_decimator_48_to_16_t -- one rate-changing stage,
 * so one write index and one phase counter -- and with no `coeff` pointer, because a single shared
 * coefficient set serves all four final rates (see the macro block above).  Kept as its own type
 * rather than reusing the 48->24 type for the same reason that one is separate from the 48->16 type:
 * the tap count is a compile-time literal in the pointer walk and the frozen paths must stay
 * byte-identical. */
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
 * resampler's fixed fc = 0.465 of its input contributes about -0.2 dB at BOTH rates -- nowhere
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
 * PRE-STAGE for a 96 kHz input leg: a single 21-tap 2:1 decimator to 48 kHz, placed in FRONT of
 * any of the chains above so they can serve a 96 kHz leg unchanged.  One shared coefficient set,
 * so no `variant` argument -- see the tap-count block above for why 21 taps suffice and why the
 * stand-alone price of 107 is the wrong shape for this position.
 *
 * Composed ratios, which is what the routing gate publishes and the feed-forward plan divides by:
 *
 *   /2       -> 48 kHz     the pre-stage alone; the resampler then runs at step 1.00000
 *   /2 + /3  -> 16 kHz     composed den 6
 *   /2 + /4  -> 12 kHz     composed den 8, step 1.00000; or 11.025 kHz at step 1.08843
 *   /2 + /6  ->  8 kHz     composed den 12
 *
 * 22.05 and 24 kHz deliberately do NOT get this stage against a 96 kHz leg: their R+jitter is
 * already 85 and 80 against the cap of 104, so they resample directly and pay nothing.  (Their
 * 48 kHz-input /2 rows are unaffected and still use asrc_decimator_48_to_24_*.)
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
 * The two variant arguments pick the coefficient set where a divider has more
 * than one; they are ignored for the dividers that do not, and for 2/3.
 */
bool asrc_decimator_q31_init(uint32_t num, uint32_t den, uint8_t channels,
                             asrc_decimator_48_to_24_variant_t v24,
                             asrc_decimator_48_to_12_variant_t v12);

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
