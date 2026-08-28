#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_audio_path.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "apps/shared/LED_level_meter.h"
#include "nora_high_res_timer.h"
#include "asrc_audio_path.h"
#include "audio_app_asrc.h"
#include "audio_app_meas.h"

/*
 * The codec rate query and the TDM leg handles below are needed by TWO things: the low-rate
 * front end's chain selection, and asrc_audio_path_apply_isr_priorities() -- which is not part
 * of the front end and runs in every ASRC build. They therefore sit AHEAD of the front-end
 * guard.
 *
 * They were inside it until 2026-08-27, and that silently disabled rate-monotonic priorities
 * on AK128: APP_ASRC_RATE_MONOTONIC_ISR was defined in the same guarded block, so in a build
 * with no front end (APP_ASRC_RUNTIME_48K_TO_8 == 0) the `#if APP_ASRC_RATE_MONOTONIC_ISR`
 * further down saw an UNDEFINED identifier, which the preprocessor evaluates as 0 -- no
 * warning, no error, and apply_isr_priorities() linked as a 4-byte empty function. The macro
 * now lives in asrc_app_config.h, next to an #error that fails the build if it goes missing
 * again.
 */
#include "board/devices/wm8904.h"
#include "board/audio/audio.h"

#define I2C_INST_A (2u)   // I2C2 -- WM8904-A on MikroBUS-A
#if APP_AK128_J3_TDM_B
#define I2C_INST_B (1u)   // I2C1 -- WM8904-B on MikroBUS-B, DIM-P4/P6
#else
#define I2C_INST_B (3u)   // I2C3 -- WM8904-B on MikroBUS-B
#endif

/* The re-defense promised above: this file DECIDES on APP_ASRC_RATE_MONOTONIC_ISR with an #if,
 * and an #if cannot tell "defined as 0" from "never defined". Assert visibility instead of
 * letting the second case pass as the first. */
#ifndef APP_ASRC_RATE_MONOTONIC_ISR
#  error "APP_ASRC_RATE_MONOTONIC_ISR is not visible here -- it belongs to asrc_app_config.h. Do not move it into a conditional block: #if treats an undefined macro as 0, which is how RM was silently lost on AK128 before 2026-08-27."
#endif

/* printf newline, kept out of the format strings so the source carries no escape that a
 * text tool can mangle. */
#define ASRC_PRIO_EOL   "\n"

#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
#include "asrc_decimator_48_to_8.h"

// Which front-end implementation this build uses.
//
// Q31 16ch is the shipping front end: same chains, same coefficients, same band edges as the
// float one, with the arithmetic in Q31 and the width at ASRC_CH.  It is selected for every build
// EXCEPT a 96 kHz one, because a 96 kHz leg composes a 21-tap 96 -> 48 kHz pre-stage in front of
// these chains and that pre-stage is still stereo float.  96 kHz and BIDIR are mutually exclusive
// (asrc_app_validate.h #errors on the pair), so the shipping BIDIR image never compiles the float
// arm at all.
//
// Follow-up, deliberately NOT done here: the 96 kHz down-conversion front end remains stereo and
// the current 21-tap 96 -> 48 pre-stage is designed only as the first stage of deeper decimation;
// Nyquist-safe 96 -> 48/44.1/etc. requires a separate 16ch audit.
#if APP_USE_96K_RATE
#define PATH_FRONTEND_Q31 (0)
#else
#define PATH_FRONTEND_Q31 (1)
#endif

// Scratch capacity for one block of decimated frames.  A /den front end emits at most
// ceil(APP_BLOCK_FRAMES / den) frames per block, so the SMALLEST divider the routing gate can
// select sets the size.  Widened from 6 to 8 frames on 2026-07-29 (den 3 -> den 2) ahead of the
// 22.05/24 kHz work, which are den == 2 and produce 8 frames per 16-frame block.
//
// Getting this wrong fails SILENTLY in the worst way: asrc_decimator_*_process_* is
// all-or-nothing on output capacity, so a one-frame shortfall makes every call return false,
// no block is ever pushed, and the path goes MUTE rather than degrading.  Hence the guards
// below rather than a bare literal.
#define DECIMATED_MIN_DEN (2u)
// ...and, since 2026-08-23, the LARGEST NUMERATOR.  A front end whose ratio is num/den emits at
// most ceil(in * num / den) frames, so what sizes the buffer is the largest RATIO the gate can
// select -- which until now was the same thing as the smallest divider only because every front end
// was 1/den.  The 48 -> 32 kHz AUDIO MODE front end is 2/3: a larger ratio than 1/2, and 11 frames
// out of a 16-frame block rather than 8.  So the invariant is restated as the ratio it always was,
// with DECIMATED_MIN_DEN kept as the integer chains' half of it.
#define DECIMATED_MAX_NUM (ASRC_DECIMATOR_48_TO_32_L)
// A 96 kHz input leg composes TWO stages: a fixed 96 -> 48 kHz /2 pre-stage, then one of the
// 48 kHz chains above.  The second stage's input is therefore no longer APP_BLOCK_FRAMES -- it is
// what the pre-stage emitted, at most ceil(APP_BLOCK_FRAMES / 2).  So the capacity that matters is
// derived from THAT, not from the block, and the intermediate 48 kHz frames need scratch of their
// own (both stages are live in the same block, which is also why the pre-stage state cannot join
// the union below).
#define PRESTAGE_DEN (2u)
#define PRESTAGE_BLOCK_CAPACITY \
    (((uint32_t)APP_BLOCK_FRAMES + (PRESTAGE_DEN - 1u)) / PRESTAGE_DEN)
// The stage FEEDING the second stage: a 48 kHz leg feeds it a whole block, a 96 kHz leg feeds it
// only what the pre-stage produced.  The larger of the two sizes the output scratch, so one buffer
// serves both.  (Today the 48 kHz leg's block is the larger, so this is APP_BLOCK_FRAMES and the
// capacity is unchanged from before the pre-stage existed -- stated as a max rather than assumed,
// so a future block-size split cannot silently undersize it.)
#define SECOND_STAGE_MAX_INPUT_FRAMES                             \
    ( ( (uint32_t)APP_BLOCK_FRAMES > PRESTAGE_BLOCK_CAPACITY )     \
          ? (uint32_t)APP_BLOCK_FRAMES : PRESTAGE_BLOCK_CAPACITY )
#define DECIMATED_BLOCK_CAPACITY_INTEGER \
    ((SECOND_STAGE_MAX_INPUT_FRAMES + (DECIMATED_MIN_DEN - 1u)) / DECIMATED_MIN_DEN)
// The rational chain's own exact count, taken from the front end's header rather than recomputed
// here, so the buffer and the front end can never disagree about how many frames a block yields.
#define DECIMATED_BLOCK_CAPACITY_R23 \
    ASRC_DECIMATOR_48_TO_32_OUT_FRAMES(SECOND_STAGE_MAX_INPUT_FRAMES)
#if PATH_FRONTEND_Q31
// Only the Q31 front end implements a rational ratio, so only that build pays the extra frames.
// A float build's scratch stays the size it has always been, byte for byte.
#define DECIMATED_BLOCK_CAPACITY                                   \
    ((DECIMATED_BLOCK_CAPACITY_INTEGER > DECIMATED_BLOCK_CAPACITY_R23) \
         ? DECIMATED_BLOCK_CAPACITY_INTEGER : DECIMATED_BLOCK_CAPACITY_R23)
#else
#define DECIMATED_BLOCK_CAPACITY (DECIMATED_BLOCK_CAPACITY_INTEGER)
#endif
// Guard 1: the capacity really does cover a full block at the smallest divider.  Keeps the
// formula and DECIMATED_MIN_DEN from drifting apart if either is edited.
_Static_assert( DECIMATED_BLOCK_CAPACITY * DECIMATED_MIN_DEN >= SECOND_STAGE_MAX_INPUT_FRAMES,
                "DECIMATED_BLOCK_CAPACITY too small for DECIMATED_MIN_DEN" );
// Guard 1b: the same statement in its general form -- capacity * den >= in * num -- for the
// largest ratio the gate can select.  This is the one that would have caught the 2/3 front end
// being routed through an 8-frame buffer, which is a MUTE path and not a build error.
#if PATH_FRONTEND_Q31
_Static_assert( DECIMATED_BLOCK_CAPACITY * ASRC_DECIMATOR_48_TO_32_M >=
                    SECOND_STAGE_MAX_INPUT_FRAMES * DECIMATED_MAX_NUM,
                "DECIMATED_BLOCK_CAPACITY too small for the 2/3 front end" );
_Static_assert( DECIMATED_BLOCK_CAPACITY >= DECIMATED_BLOCK_CAPACITY_R23,
                "DECIMATED_BLOCK_CAPACITY below the 2/3 front end's exact frame count" );
#endif
// Guard 2: every divider path_decimator_init() implements must fit.  A new divider added below
// without a line here, or added with a value under DECIMATED_MIN_DEN, is a compile error rather
// than silence on the bench.
_Static_assert( DECIMATED_BLOCK_CAPACITY * 6u >= (uint32_t)APP_BLOCK_FRAMES, "/6 block overflow" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 4u >= (uint32_t)APP_BLOCK_FRAMES, "/4 block overflow" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 3u >= (uint32_t)APP_BLOCK_FRAMES, "/3 block overflow" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 2u >= (uint32_t)APP_BLOCK_FRAMES, "/2 block overflow" );
#if PATH_FRONTEND_Q31
_Static_assert( DECIMATED_BLOCK_CAPACITY >=
                    ASRC_DECIMATOR_48_TO_32_OUT_FRAMES((uint32_t)APP_BLOCK_FRAMES),
                "2/3 block overflow" );
#endif
// Guard 2b: the same, for the COMPOSED dividers a 96 kHz leg selects.  Each is checked against the
// pre-stage's output rather than the block, because that is what its second stage is fed -- e.g.
// composed /12 is a /2 to 8 frames and then a /6 to 2 frames, which needs capacity 2, not 12.
_Static_assert( PRESTAGE_BLOCK_CAPACITY * PRESTAGE_DEN >= (uint32_t)APP_BLOCK_FRAMES,
                "pre-stage scratch too small for one block" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 6u >= PRESTAGE_BLOCK_CAPACITY, "/2+/6 block overflow" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 4u >= PRESTAGE_BLOCK_CAPACITY, "/2+/4 block overflow" );
_Static_assert( DECIMATED_BLOCK_CAPACITY * 3u >= PRESTAGE_BLOCK_CAPACITY, "/2+/3 block overflow" );
// The float front ends, one per divider, overlaid because only one is ever live.  The Q31 front
// end owns its own storage (Y history, X coefficients) and needs none of this.
#if !PATH_FRONTEND_Q31
static union
{
    asrc_decimator_48_to_8_t  to_8;
    asrc_decimator_48_to_16_t to_16;
    asrc_decimator_48_to_24_t to_24;
    asrc_decimator_48_to_12_t to_12;
} s_path_decimator;
#endif
#if APP_USE_96K_RATE
// Deliberately OUTSIDE the union: on a composed 96 kHz chain the pre-stage and one union member
// are both live within the same block, so they cannot overlay each other.  +336 B of history.
// Gated on APP_USE_96K_RATE so a 48 kHz build carries neither the history nor the scratch.
static asrc_decimator_96_to_48_t s_path_prestage;
static int32_t s_path_prestage_out[PRESTAGE_BLOCK_CAPACITY * 2u];
#endif
// Channels in the decimated block.  The Q31 front end computes ASRC_CH of them (the extra ones
// are the 16ch workload, not audio -- see asrc_decimator_q31.inc), so the scratch has to be wide
// enough to RECEIVE them even though the push below only ever reads channels 0 and 1.
#if PATH_FRONTEND_Q31
#define PATH_DECIMATED_STRIDE (ASRC_CH)
#else
#define PATH_DECIMATED_STRIDE (2u)
#endif
static int32_t s_path_decimated[DECIMATED_BLOCK_CAPACITY * PATH_DECIMATED_STRIDE];
#if PATH_FRONTEND_Q31
// The 16ch isolation selftest below borrows this buffer instead of putting a second 16ch block on
// the stack (the Q31 arm has no room for one).  It writes at stride ASRC_CH, so the loan is only
// sound while this scratch is that wide -- stated here rather than trusted, because the #if above
// is far enough away to be edited independently.
_Static_assert( PATH_DECIMATED_STRIDE == ASRC_CH,
                "the 16ch isolation selftest writes s_path_decimated at stride ASRC_CH" );
#endif
// All three are written by the main-loop rate-change path and read by a block ISR (the leg whose
// input is the 48 kHz side -- leg A for A->B, leg B for B->A).
// volatile keeps the publication order below (ready=0 -> den -> init -> ready=1) from being
// re-ordered by the optimiser: an ISR that saw the new den while the union still held the
// other front end's state would index history[] past its end.
//
// ONE decimator instance serves either direction, and AT MOST ONE of the two denominators below is
// ever != 1.  Originally that followed from a front end needing its input leg at exactly 48 kHz
// (A=48k and B=48k being mutually exclusive).  The 96 kHz pre-stage widens the input rates a front
// end can take, so the invariant is now maintained deliberately rather than falling out of the
// rates: only the DOWN-sampling direction gets a front end.  The up-sampling direction needs none
// -- its step is below 1, so R(step) shrinks and never approaches the clamp that motivates all of
// this.  That keeps the union sound and keeps the "not ready -> asrc_audio_path_reset()" self-heal
// below single-writer: only the owning leg's ISR can reach it.
static volatile uint8_t s_path_decimator_ready;
/*
 * SINGLE-CALLER INVARIANT FOR THE FRONT END, ENFORCED RATHER THAN ASSUMED.
 *
 * There is ONE decimator: one filter state and one output scratch (s_path_decimated), shared by
 * whichever leg owns the front end.  Both leg callbacks contain a call to
 * path_decimator_process(), each guarded by its own denominator being != 1, and the arming code
 * below picks the coefficients for exactly one direction (`active_den`).  So the design requires
 * that AT MOST ONE direction is ever decimated.  Today that holds because the front end exists
 * to bring a low leg up against a 48 kHz peer and both legs cannot be the 48 kHz one.
 *
 * It was only a comment, and it is load-bearing twice over: with both denominators != 1 the two
 * legs would run one filter state and one scratch buffer against different rates, and they would
 * do it from two different ISRs -- which is also the one place in this file that would become a
 * data race the moment those ISRs stop sharing a priority.  A compile-time assert cannot express
 * it (the rate pair is runtime), so it is refused at arming time instead: the front end is not
 * armed, this sticky flag is set, and the refusal is printed once.  Fail closed and visibly, rather
 * than serve audio through a decimator that is initialised for the other direction.
 * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 14.1 item 4.
 */
static volatile uint8_t s_path_frontend_dual_den_unsupported;
static volatile uint8_t s_path_frontend_den_ab;
static volatile uint8_t s_path_frontend_den_ba;
// The matching NUMERATORS.  1 for every integer chain, 2 for 48 -> 32 kHz, and never 0 once the
// gate has run.  Deliberately NOT read by the block ISR: the Q31 front end resolved its whole
// chain in init(), so path_decimator_process() already ignores the divider (see the (void)den
// there) and the numerator changes nothing in the hot path.  These exist for the RATE PLAN --
// step_ff = measured_ratio * num / den -- and for telemetry, both main-loop context.
static volatile uint8_t s_path_frontend_num_ab = 1u;
static volatile uint8_t s_path_frontend_num_ba = 1u;
#if APP_USE_96K_RATE
// Divider of the 48 kHz-input stage BEHIND the 96 kHz pre-stage, or 0 when no pre-stage is active.
//
// This is the resolved chain SHAPE, not the input rate, and that is deliberate: the rate is known
// once at rate-change time, so resolving it there keeps the block ISR to a single byte compared
// against zero rather than a volatile load plus a multiply.  In a build with no 96 kHz leg even
// that is gone -- see path_decimator_process(), which drops the branch entirely.  Both steps were
// measured with tools/asrc/hotpath_invariance.py, which holds the leg callbacks byte-identical.
static volatile uint8_t s_path_frontend_second_den;
#else
#define s_path_frontend_second_den (0u)
#endif
// Suffix for the telemetry `fe=` field, naming WHICH coefficient set is loaded when the
// divider alone does not say.  /4 serves two output rates with two different sets of band
// edges, so `fe=/4` on its own cannot distinguish "12 kHz got its own 4700 Hz set" from
// "12 kHz silently got the 11.025 kHz set" -- the two are audibly different and would look
// identical in the log.  Written in main-loop context here, read in main-loop context by the
// telemetry printer; a pointer store is atomic on this core.
static const char* s_path_frontend_tag = "";

// One stage init for an already-resolved divider and its coefficient variants.  The Q31 front end
// takes them all at once (its chain table is internal, and the variant arguments it does not need
// are ignored); each float front end takes its own struct instead.  So the divider dispatch lives
// here, and path_second_stage_init() below stays purely about RESOLVING the chain -- which rate
// gets which coefficient set, and which telemetry tag names it.  That resolution is spec, and it
// is identical for both arithmetics: this swap is Q31-for-float, not a filter redesign.
static bool path_stage_init( uint32_t num, uint32_t den,
                            asrc_decimator_48_to_24_variant_t v24,
                            asrc_decimator_48_to_12_variant_t v12 )
{
#if PATH_FRONTEND_Q31
    return asrc_decimator_q31_init( num, den, (uint8_t)ASRC_CH, v24, v12 );
#else
    // No float front end is rational, so a num != 1 request has no implementation here and must
    // fail CLOSED rather than quietly running the 1/den chain at the wrong output rate.
    if( num != 1u ) { return false; }
    if( den == 6u ) { return asrc_decimator_48_to_8_init( &s_path_decimator.to_8, 2u ); }
    if( den == 4u ) { return asrc_decimator_48_to_12_init( &s_path_decimator.to_12, 2u, v12 ); }
    if( den == 3u ) { return asrc_decimator_48_to_16_init( &s_path_decimator.to_16, 2u ); }
    if( den == 2u ) { return asrc_decimator_48_to_24_init( &s_path_decimator.to_24, 2u, v24 ); }
    return false;
#endif
}

// Initialise the 48 kHz-input stage for a divider, assuming its input is `stage_in_frames` frames
// per block.  Split out of path_decimator_init() so a 48 kHz leg (fed a whole block) and a 96 kHz
// leg (fed only what the pre-stage emitted) share one implementation and one set of coefficient
// choices -- the stage does not care which produced its input, only how many frames it gets.
static bool path_second_stage_init( uint32_t num, uint32_t den, uint32_t low_rate_hz,
                                   uint32_t stage_in_frames )
{
    // Guard 3: fail CLOSED on a divider the scratch buffer cannot hold.  Returning false leaves
    // s_path_decimator_ready clear, so the owning block ISR drops its block instead of calling
    // process_* against a buffer that is one frame short -- which would mute the path with no
    // counter moving.  This catches a gate row added with a divider below DECIMATED_MIN_DEN,
    // which no static assert can see (the gate is a runtime rate table).
    //
    // Stated as capacity * den >= in * num, which is the general form of the same inequality: for
    // every integer chain num is 1 and this is byte-for-byte the test it has always been, and for
    // the 2/3 front end it is 11 * 3 >= 16 * 2, i.e. 33 >= 32.  Writing it with the numerator
    // rather than dropping the check is the point -- a rational ratio makes the OLD form read as
    // satisfied when it is not.
    if( ( num == 0u ) || ( num > DECIMATED_MAX_NUM ) || ( den < DECIMATED_MIN_DEN ) ||
        ( ( DECIMATED_BLOCK_CAPACITY * den ) < ( stage_in_frames * num ) ) )
    {
        s_path_frontend_tag = "";
        return false;
    }
    if( num == ASRC_DECIMATOR_48_TO_32_L )
    {
        // 48 -> 32 kHz AUDIO MODE: the only rational chain, L=2/M=3, N=97.
        //
        // It must be selected on the PAIR and not on den alone.  2/3 and 1/3 share den == 3 and
        // are different filters at different output rates: taking the 1/3 arm below would run the
        // 48 -> 16 kHz band edges (stopband 8000 Hz) on a 32 kHz output, which is not a build
        // error, does not change the rate, and is audible only as dullness plus alias.
        //
        // PARTIAL PROTECTION, not strict: this design protects 0-13 kHz and leaves residual alias
        // in 13-16 kHz by design.  Measured on the host by
        // tools/asrc/asrc_48_to_32_audio_gate.py -- 0-13 kHz worst -107.02 dB, 13-16 kHz worst
        // -25.50 dB.  The tag says `audio` for exactly that reason: a log line reading `fe=2/3`
        // alone would not distinguish this from a full-band 48 -> 32 front end, which this is not.
        if( ( den != ASRC_DECIMATOR_48_TO_32_M ) || ( low_rate_hz != 32000u ) )
        {
            s_path_frontend_tag = "";
            return false;
        }
        s_path_frontend_tag = ":audio";
        return path_stage_init( ASRC_DECIMATOR_48_TO_32_L, ASRC_DECIMATOR_48_TO_32_M,
                               ASRC_DECIMATOR_48_TO_24_FOR_24000,
                               ASRC_DECIMATOR_48_TO_12_FOR_11025 );
    }
    if( num != 1u )
    {
        // Every arm below is a 1/den chain.  Fail closed rather than run one at the wrong ratio.
        s_path_frontend_tag = "";
        return false;
    }
    if( den == 6u )
    {
        s_path_frontend_tag = "";
        // /6 has one coefficient set, so neither variant argument applies to it.
        return path_stage_init( 1u, 6u, ASRC_DECIMATOR_48_TO_24_FOR_24000,
                               ASRC_DECIMATOR_48_TO_12_FOR_11025 );
    }
    if( den == 4u )
    {
        // Two coefficient sets over one structure: same 27+129 taps, stopband pinned on the
        // OUTPUT Nyquist of the rate actually being served.  Name the rate rather than
        // defaulting, so a rate added to the gate below without its own set fails here.
        asrc_decimator_48_to_12_variant_t variant;
        if( low_rate_hz == 11025u )
        {
            variant = ASRC_DECIMATOR_48_TO_12_FOR_11025;
            s_path_frontend_tag = ":11k";
        }
        else if( low_rate_hz == 12000u )
        {
            variant = ASRC_DECIMATOR_48_TO_12_FOR_12000;
            s_path_frontend_tag = ":12k";
        }
        else
        {
            s_path_frontend_tag = "";
            return false;
        }
        return path_stage_init( 1u, 4u, ASRC_DECIMATOR_48_TO_24_FOR_24000, variant );
    }
    if( den == 3u )
    {
        // 16 kHz output: one 161-tap 3:1 stage, stopband on its own 8000 Hz Nyquist.  Only one
        // coefficient set exists for this divider, so no tag is needed to disambiguate it.
        s_path_frontend_tag = "";
        return path_stage_init( 1u, 3u, ASRC_DECIMATOR_48_TO_24_FOR_24000,
                               ASRC_DECIMATOR_48_TO_12_FOR_11025 );
    }
    if( den == 2u )
    {
        // Two coefficient sets over one structure, same as /4 above: 107 taps either way, with
        // the stopband pinned on the OUTPUT Nyquist of the rate actually being served.  24 kHz
        // gets 12000 Hz; 22.05 kHz is not 48000/2, so the resampler pulls 24 k -> 22.05 k after
        // this stage and the stopband has to come down to 11025 Hz -- the 24 kHz set leaves
        // 11.025-12 kHz input only -18.9 dB down, which then folds into the 22.05 kHz band.
        asrc_decimator_48_to_24_variant_t variant;
        if( low_rate_hz == 24000u )
        {
            variant = ASRC_DECIMATOR_48_TO_24_FOR_24000;
            s_path_frontend_tag = ":24k";
        }
        else if( low_rate_hz == 22050u )
        {
            variant = ASRC_DECIMATOR_48_TO_24_FOR_22050;
            s_path_frontend_tag = ":22k";
        }
        else
        {
            s_path_frontend_tag = "";
            return false;
        }
        return path_stage_init( 1u, 2u, variant, ASRC_DECIMATOR_48_TO_12_FOR_11025 );
    }
    s_path_frontend_tag = "";
    return false;
}

// `den` is the COMPOSED ratio the gate selected, and `in_rate_hz` says which leg rate produced it.
// Both are needed: /2 means "48 -> 24 kHz" from a 48 kHz leg but "96 -> 48 kHz" from a 96 kHz one,
// so the divider alone cannot name the front end.  A 96 kHz leg always runs the pre-stage, then
// den/2 of the 48 kHz chain behind it.
static bool path_decimator_init( uint32_t num, uint32_t den, uint32_t low_rate_hz,
                                uint32_t in_rate_hz )
{
#if APP_USE_96K_RATE
    if( in_rate_hz == 96000u )
    {
        // No rational chain is reachable from a 96 kHz leg: the gate publishes num == 1 for every
        // 96 kHz row, and composing 2/3 behind the /2 pre-stage is a chain that has never been
        // designed, let alone measured.  Refuse rather than compose something untested.
        if( num != 1u )
        {
            s_path_frontend_tag = "";
            return false;
        }
        if( ( den < PRESTAGE_DEN ) || ( ( den % PRESTAGE_DEN ) != 0u ) )
        {
            s_path_frontend_tag = "";
            return false;
        }
        if( !asrc_decimator_96_to_48_init( &s_path_prestage, 2u ) )
        {
            s_path_frontend_tag = "";
            return false;
        }
        const uint32_t second_den = den / PRESTAGE_DEN;
        if( second_den == 1u )
        {
            // 96 -> 48 kHz with nothing behind it: the resampler then runs at step 1.00000.
            s_path_frontend_tag = "";
            return true;
        }
        // The second stage sees the pre-stage's output, not the block, so its capacity check has
        // to be made against that -- passing APP_BLOCK_FRAMES here would reject composed /12.
        return path_second_stage_init( 1u, second_den, low_rate_hz, PRESTAGE_BLOCK_CAPACITY );
    }
#else
    (void)in_rate_hz;   // no 96 kHz leg in this build; the gate can only publish 48 kHz rows
#endif
    return path_second_stage_init( num, den, low_rate_hz, (uint32_t)APP_BLOCK_FRAMES );
}

static bool path_second_stage_process( uint32_t den, const int32_t* src, size_t src_frames,
                                       size_t src_stride, size_t* produced )
{
#if PATH_FRONTEND_Q31
    // The Q31 front end holds the resolved chain from init(), so it needs no divider dispatch --
    // one call whichever chain is live.  `2u` is the source's REAL channel count (the codec pair);
    // the front end repeats it up to ASRC_CH internally.
    (void)den;
    return asrc_decimator_q31_process_s24_left(
        src, src_frames, src_stride, 2u,
        s_path_decimated, DECIMATED_BLOCK_CAPACITY, PATH_DECIMATED_STRIDE, produced );
#else
    if( den == 6u )
    {
        return asrc_decimator_48_to_8_process_s24_left(
            &s_path_decimator.to_8, src, src_frames, src_stride,
            s_path_decimated, DECIMATED_BLOCK_CAPACITY, 2u, produced );
    }
    if( den == 4u )
    {
        return asrc_decimator_48_to_12_process_s24_left(
            &s_path_decimator.to_12, src, src_frames, src_stride,
            s_path_decimated, DECIMATED_BLOCK_CAPACITY, 2u, produced );
    }
    if( den == 3u )
    {
        return asrc_decimator_48_to_16_process_s24_left(
            &s_path_decimator.to_16, src, src_frames, src_stride,
            s_path_decimated, DECIMATED_BLOCK_CAPACITY, 2u, produced );
    }
    if( den == 2u )
    {
        return asrc_decimator_48_to_24_process_s24_left(
            &s_path_decimator.to_24, src, src_frames, src_stride,
            s_path_decimated, DECIMATED_BLOCK_CAPACITY, 2u, produced );
    }
    return false;
#endif
}

// Returns the decimated block in s_path_decimated (or s_path_prestage_out when the pre-stage is the
// only stage), with `produced` frames at stride 2.  `out` names which buffer to push from, so the
// caller does not have to know the chain shape.
//
// `second_den` is s_path_frontend_second_den: 0 means "no pre-stage, `den` is a 48 kHz-input stage",
// which is the only case a 48 kHz build ever takes -- one byte tested against zero, so that build's
// callbacks are unchanged.
static bool path_decimator_process( uint32_t den, uint32_t second_den, const int32_t* src,
                                    const int32_t** out, size_t* produced )
{
#if !APP_USE_96K_RATE
    // No 96 kHz leg is reachable in this build, so the routing gate can never publish a pre-stage
    // and second_den is always 0.  Compiled out rather than left as a runtime test, because this
    // function inlines into both per-block leg callbacks: at 48 kHz the test would cost a volatile
    // load and a branch per block for a chain that cannot occur.
    //
    // Verified, not assumed.  tools/asrc/hotpath_invariance.py watches both leg callbacks (anything
    // reachable from a block ISR at audio rate is watched) and reports them as 151 -> 150 and
    // 91 -> 90 instructions against the pre-96k-frontend BIDIR build.  Both deltas are one fewer
    // `neop`, i.e. assembler alignment padding: the REAL instruction histograms are identical
    // (134 -> 134 and 81 -> 81), and data_used is byte-identical at 49420.  So do not "fix" that
    // report by reverting this gate -- without it the callbacks genuinely grow (measured 151 -> 193
    // and 91 -> 138), which is the regression this arm exists to prevent.
    (void)second_den;
    *out = s_path_decimated;
    return path_second_stage_process( den, src, APP_BLOCK_FRAMES, APP_SLOTS_PER_FS, produced );
#else
    if( second_den == 0u )
    {
        *out = s_path_decimated;
        return path_second_stage_process( den, src, APP_BLOCK_FRAMES, APP_SLOTS_PER_FS, produced );
    }

    size_t pre_frames = 0u;
    if( !asrc_decimator_96_to_48_process_s24_left(
            &s_path_prestage, src, APP_BLOCK_FRAMES, APP_SLOTS_PER_FS,
            s_path_prestage_out, PRESTAGE_BLOCK_CAPACITY, 2u, &pre_frames ) )
    {
        return false;
    }
    if( second_den == 1u )
    {
        *out = s_path_prestage_out;
        *produced = pre_frames;
        return true;
    }
    // The intermediate is already channel-packed at stride 2, unlike the raw TDM/I2S source.
    *out = s_path_decimated;
    return path_second_stage_process( second_den, s_path_prestage_out, pre_frames, 2u, produced );
#endif // APP_USE_96K_RATE
}
#endif

typedef struct
{
    volatile uint32_t callback_a_ticks;
    volatile uint32_t callback_b_ticks;
    volatile uint32_t push_ab_ticks;
    volatile uint32_t push_ba_ticks;
    volatile uint32_t meter_a_ticks;
    volatile uint32_t meter_b_ticks;
} asrc_path_profile_t;

#if APP_ASRC_HEADROOM_INSTRUMENT
static asrc_path_profile_t s_path_profile;
#endif
#if (APP_ASRC_LED_FRAME_STRIDE > 1u)
static uint16_t s_path_meter_phase[2];
#endif

#if APP_ASRC_HEADROOM_INSTRUMENT
static inline void profile_peak_ticks( volatile uint32_t* peak, uint32_t started )
{
    // Keep ISR profiling to one timer read, one 32-bit subtract, and a peak
    // update. Unit conversion uses 64-bit division and belongs in foreground.
    const uint32_t elapsed = nora_high_res_timer_get_count() - started;
    if( elapsed > *peak ) { *peak = elapsed; }
}
#endif

static inline void path_push_ab( const int32_t* src )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t started = nora_high_res_timer_get_count();
#endif
    audio_app_asrc_push_ab( src );
#if APP_ASRC_HEADROOM_INSTRUMENT
    profile_peak_ticks( &s_path_profile.push_ab_ticks, started );
#endif
}

#if APP_B_ROUTE_USES_BA
static inline void path_push_ba( const int32_t* src )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t started = nora_high_res_timer_get_count();
#endif
    audio_app_asrc_push_ba( src );
#if APP_ASRC_HEADROOM_INSTRUMENT
    profile_peak_ticks( &s_path_profile.push_ba_ticks, started );
#endif
}
#endif

static inline void path_meter_submit( const int32_t* buf, uint8_t leg )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t started = nora_high_res_timer_get_count();
#endif
#if (APP_ASRC_LED_FRAME_STRIDE > 1u)
    const uint8_t meter_leg = ( leg != 0u ) ? 1u : 0u;
    level_meter_process_i32_sparse( buf, APP_SLOTS_PER_FS, APP_BLOCK_FRAMES,
                                    APP_ASRC_LED_FRAME_STRIDE,
                                    &s_path_meter_phase[meter_leg] );
#else
    level_meter_process_i32( buf, APP_SLOTS_PER_FS, APP_BLOCK_FRAMES );
#endif
#if APP_ASRC_HEADROOM_INSTRUMENT
    profile_peak_ticks( ( leg == 0u ) ? &s_path_profile.meter_a_ticks
                                      : &s_path_profile.meter_b_ticks,
                        started );
#else
    (void)leg;
#endif
}

/*
 * Pure front-end selection: which decimating / resampling front end (num/den per engine)
 * this build would put in front of the resampler for a given rate PAIR.
 *
 * Pure and side-effect free on purpose.  Two callers need the same answer at different
 * times: asrc_audio_path_reset() applies it after a rate change, and the *ar pair gate
 * has to know it BEFORE one, so it can refuse a pair whose effective step the ring
 * cannot serve.  One table, two callers -- there is no second place to edit.
 */
void asrc_audio_path_frontend_plan( uint32_t a_rate_hz, uint32_t b_rate_hz,
                                    asrc_frontend_plan_t* plan )
{
    if( plan == NULL ) { return; }
#if APP_ASRC_48K_TO_8_INTEGRATION
    /* One-way 48 -> 8 kHz preset: the chain is fixed at build time, so the pair does not
     * select anything.  Answer with the preset rather than falling through the runtime
     * table, which does not know about it. */
    (void)a_rate_hz; (void)b_rate_hz;
    plan->num_ab = 1u; plan->num_ba = 1u;
    plan->den_ab = 6u; plan->den_ba = 1u;
    plan->low_rate_hz = 8000u; plan->in_rate_hz = 48000u;
    return;
#else
        // The decimator ratios are hard-wired: the 48 kHz leg is decimated by an INTEGER den, one
        // fixed coefficient set per served rate.  Covered today: 8 / 11.025 / 12 / 16 / 22.05 /
        // 24 kHz (den 6 / 4 / 4 / 3 / 2 / 2).  Because the ratio is fixed and integer, a
        // front end can only sit on the leg that RUNS at 48 kHz, decimating towards the other
        // leg's low rate.  Both down-sampling directions get one; every other pair falls back to
        // the direct variable-ratio path -- which handles any A:B pair, just at full cost and with
        // no band-limiting of its own.  A=48k and B=48k are mutually exclusive, hence else-if.
        // 11.025 kHz uses /4 (48 -> 24 -> 12 kHz), respec'd from /3 on 2026-07-29: cheaper front
        // end, a 200 Hz wider passband, and a resampler step of 1.08843 instead of 1.45125.
        // See section 11 of [internal] report_asrc_d2_alias_route_2026-07-28.md.
        // 12 kHz joined on 2026-07-29 with its OWN /4 coefficient set: 48/12 is exactly 4, so the
        // decimator's output IS the final rate and the resampler runs at step 1.00000.  Its
        // stopband sits at 6000 Hz rather than 5512.5 Hz, and that slack buys a 4700 Hz passband
        // out of the same 129 taps.
        // 16 kHz joined on 2026-07-29 and took over den == 3, which the /4 respec had left
        // unreachable: a single 161-tap 3:1 stage, stopband on 8000 Hz, step 1.00000.  It cannot be
        // a cheap cascade -- a 3:1 decimation to 16 kHz creates the fold itself, so no later stage
        // can undo it (report section 12).
        // 24 kHz joined on 2026-07-29 as den == 2: a single 107-tap 2:1 stage, stopband on 12000 Hz,
        // step 1.00000.  Same forced structure as /3 for the same reason, and NOT a half-band --
        // that was priced and costs over 401 taps here rather than saving half (see the /2 block in
        // asrc_decimator_48_to_8.h).  It produces 8 frames per block, which the scratch buffer was
        // widened to hold earlier the same day (see DECIMATED_MIN_DEN above).
        // 22.05 kHz joined on 2026-07-29, and is the FIRST rate here that is not 48000/den: den == 2
        // is the only integer choice whose intermediate rate (24 kHz) reaches it, so the stage
        // decimates 48 -> 24 kHz and the resampler then pulls 24 k -> 22.05 k at step 1.08843.  The
        // output Nyquist is therefore 11025 Hz, 975 Hz BELOW the intermediate's, so it cannot borrow
        // the 24 kHz coefficients (measured: they leave the alias only -19.7 dB down).  It gets its
        // own set over the same 107-tap structure -- see the /2 variant block in
        // asrc_decimator_48_to_8.h.
        // Rates NOT in this table still resample directly, where the only band limit is
        // ASRC_POLY_FC of the INPUT rate (22.32 kHz from 48 kHz).  32 and 44.1 kHz are in that state
        // today -- neither has an integer den (48/32 = 1.5, 48/44.1 = 1.088), so both would need
        // den == 1 with a non-unity resampler, i.e. a decimate-by-1 stage rather than a rate change,
        // which is a different structure from every row below.  Their SEVERITY is not the same,
        // though, and the difference is why only one of them is a to-do:
        //   32 kHz   -- worst alias 0.00 dB, folding 16-22.3 kHz down into 9.7-16 kHz.  Real and
        //               worth fixing; deferred purely on cost (every option priced out of budget).
        //   44.1 kHz -- worst alias -4.99 dB over a 270 Hz band at 21.78-22.05 kHz, i.e. inaudible.
        //               Not a to-do at all: nothing to fix, not merely unaffordable.
        // Priced per rate in [internal] study_asrc_lowpass_per_rate_2026-07-29.md.
        // 96 kHz leg A joined on 2026-08-02, and is the first row set whose front end is a CASCADE: a
        // fixed 21-tap 96 -> 48 kHz pre-stage, then the existing 48 kHz chain for the target rate, so
        // the published denominator is the COMPOSED ratio (12 / 8 / 6) rather than one stage's.  From
        // 96 kHz the direct step is up to 12, which needs R+jitter of 200 against a cap of 104 -- the
        // clamp that produces the audible break-up.  The pre-stage puts the resampler back at step
        // ~1.0.  See [internal] asrc_96k.md part 3.
        //
        // 22.05 and 24 kHz are deliberately ABSENT here: their R+jitter from 96 kHz is 85 and 80, both
        // already inside the cap, so they resample directly and pay nothing for a stage they do not
        // need.  32 and 48 kHz likewise fit (65 and 50).  Only 16 kHz and below are clamped.
        // 32 kHz joined on 2026-08-23 and is the FIRST row whose front end is not 1/den: L=2/M=3,
        // num 2 / den 3, N=97, a polyphase resampling front end rather than a decimator.  It exists
        // because 48/32 = 1.5 has no integer divider, so before this row 32 kHz resampled directly
        // with no band limit but ASRC_POLY_FC of 48 kHz -- worst alias 0.00 dB (see the note below,
        // which is now half answered).
        //
        // It is AUDIO MODE / PARTIAL PROTECTION and must not be described as anything stronger: it
        // protects 0-13 kHz (host-measured worst alias -107.02 dB) and deliberately leaves residual
        // alias in 13-16 kHz (worst -25.50 dB, from a 16225 Hz input landing at 15775 Hz).  A strict
        // 0-16 kHz front end needs N = 161 or more and was priced out.
        //
        // BOTH LEGS, since 2026-08-24.  It was AB-only until then, on the reasoning that "32 -> 48 kHz
        // UP-samples, so it creates no fold to protect against".  That sentence is about the wrong
        // engine: the row a pair (A=32k, B=48k) needs is in the BA table, and BA there is
        // 48000 -> 32000, i.e. the SAME down-conversion the AB row exists for.  So the pair
        // A=32k/B=48k ran its 48 -> 32 kHz leg with no band limit at all -- exactly the worst-alias
        // 0.00 dB case this row was added to fix, just in the direction nobody measured.  Found by
        // the CPU-margin study: that pair read 80.4 % where its mirror read 99.6 %, and the missing
        // 19 points WERE the missing filter.  See [internal]
        // [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 4.
        //
        // The "at most one ratio != 1/1" invariant still holds, and that is why this is safe to add:
        // A=48k and B=48k are mutually exclusive (the else-if below), so at most one of the two
        // tables ever fires, and within a pair only the leg whose ENGINE INPUT is 48 kHz gets a row.
        uint32_t num_ab = 1u;
        uint32_t num_ba = 1u;
        uint32_t den_ab = 1u;
        uint32_t den_ba = 1u;
        uint32_t low_rate_hz = 0u;
        uint32_t in_rate_hz = 0u;
#if APP_USE_96K_RATE
        if( a_rate_hz == 96000u )
        {
            den_ab = ( b_rate_hz == 8000u )  ? 12u :
                     ( b_rate_hz == 11025u ) ? 8u :
                     ( b_rate_hz == 12000u ) ? 8u :
                     ( b_rate_hz == 16000u ) ? 6u : 1u;
            low_rate_hz = b_rate_hz;
            if( den_ab != 1u ) { in_rate_hz = a_rate_hz; }
        }
        else
#endif
        if( a_rate_hz == 48000u )
        {
            den_ab = ( b_rate_hz == 8000u )  ? 6u :
                     ( b_rate_hz == 11025u ) ? 4u :
                     ( b_rate_hz == 12000u ) ? 4u :
                     ( b_rate_hz == 16000u ) ? 3u :
                     ( b_rate_hz == 22050u ) ? 2u :
                     ( b_rate_hz == 24000u ) ? 2u :
#if APP_ASRC_RUNTIME_48K_TO_8
                     ( b_rate_hz == 32000u ) ? ASRC_DECIMATOR_48_TO_32_M : 1u;
            // The numerator rides with the denominator: 32 kHz is the only row that sets it, and
            // path_second_stage_init() cross-checks the pair against the output rate, so a row that
            // sets one without the other fails closed instead of running the wrong filter.
            num_ab = ( b_rate_hz == 32000u ) ? ASRC_DECIMATOR_48_TO_32_L : 1u;
#else
                     1u;
            // The 32 kHz AUDIO MODE front end (ASRC_DECIMATOR_48_TO_32_*) lives in the runtime
            // low-rate front end, which this build does not compile in (APP_ASRC_RUNTIME_48K_TO_8
            // == 0). 32 kHz therefore falls back to the pre-2026-08-23 direct path here, same as
            // any other rate this build has no front end for.
            num_ab = 1u;
#endif
            low_rate_hz = b_rate_hz;
            if( den_ab != 1u ) { in_rate_hz = a_rate_hz; }
        }
#if APP_B_ROUTE_USES_BA
        else if( b_rate_hz == 48000u )
        {
            den_ba = ( a_rate_hz == 8000u )  ? 6u :
                     ( a_rate_hz == 11025u ) ? 4u :
                     ( a_rate_hz == 12000u ) ? 4u :
                     ( a_rate_hz == 16000u ) ? 3u :
                     ( a_rate_hz == 22050u ) ? 2u :
                     ( a_rate_hz == 24000u ) ? 2u :
#if APP_ASRC_RUNTIME_48K_TO_8
                     ( a_rate_hz == 32000u ) ? ASRC_DECIMATOR_48_TO_32_M : 1u;
            // Rides with the denominator exactly as in the AB table above: 32 kHz is the only row
            // that sets it, and path_second_stage_init() cross-checks the pair against the output
            // rate, so a row setting one without the other fails closed rather than running the
            // wrong filter.
            num_ba = ( a_rate_hz == 32000u ) ? ASRC_DECIMATOR_48_TO_32_L : 1u;
#else
                     1u;
            // See the AB table's comment above: no runtime front end in this build, so 32 kHz
            // falls back to the direct path here too.
            num_ba = 1u;
#endif
            low_rate_hz = a_rate_hz;
            if( den_ba != 1u ) { in_rate_hz = b_rate_hz; }
        }
#endif
        // B->A gets no front end when leg A is the 96 kHz side: that direction UP-samples (step below
        // 1), so its look-ahead shrinks rather than hitting the clamp, and leaving it direct is what
        // keeps "at most one denominator != 1" true -- the invariant the single union depends on.
    plan->num_ab      = num_ab;
    plan->num_ba      = num_ba;
    plan->den_ab      = den_ab;
    plan->den_ba      = den_ba;
    plan->low_rate_hz = low_rate_hz;
    plan->in_rate_hz  = in_rate_hz;
#endif
}

void asrc_audio_path_reset( void )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    s_path_profile = (asrc_path_profile_t){ 0 };
#endif
#if (APP_ASRC_LED_FRAME_STRIDE > 1u)
    s_path_meter_phase[0] = 0u;
    s_path_meter_phase[1] = 0u;
#endif
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    // One-shot bit-exactness check of the front end THIS BUILD SHIPS, against an independent
    // reference -- bit-exact or bust.  Once, not per restart: it costs a few ms and the answer
    // cannot change at runtime.  It runs before the chain is selected below, and that selection
    // re-inits the front end, so the check cannot leak state into the audio path.
#if APP_ASRC_FRONTEND_SELFTEST
    static uint8_t s_path_dec_selftested = 0u;
    if( !s_path_dec_selftested )
    {
        s_path_dec_selftested = 1u;
#if PATH_FRONTEND_Q31
        // The Q31 fast path (hardware Y modulo, batched, assembly kernel) against a linear
        // shift register, int64, no-modulo, no-batching Q31 oracle.  `ties` counts the only
        // difference the sacr.l rounding mode is allowed to produce (see the header of
        // asrc_decimator_q31_selftest.inc); it is printed rather than hidden, because a count
        // that starts moving is worth seeing even though it is not a failure.
        uint32_t dec_ties = 0u;
        const bool dec_ok = asrc_decimator_q31_selftest( &dec_ties );
        printf( " ASRC Q31 front-end selftest: %s (rounding ties %lu)\n",
                dec_ok ? "pass" : "FAIL", (unsigned long)dec_ties );
        if( !dec_ok )
        {
            // Numbers, not a formatted message: printf has one caller in this file and the ROM
            // diet keeps it that way, so the failure site is reported as fields.
            const asrc_decimator_q31_fail_t* f = asrc_decimator_q31_selftest_fail();
            printf( "  %s: /%lu case %lu live %lu blk %lu out %lu ch %lu got %ld want %ld\n",
                    f->what, (unsigned long)f->den, (unsigned long)f->kase,
                    (unsigned long)f->live_ch, (unsigned long)f->block,
                    (unsigned long)f->index, (unsigned long)f->channel,
                    (long)f->got, (long)f->want );
        }
#else
        // The float front end, against the modulo-ring algorithm it was derived from.
        const bool dec_ok = asrc_decimator_selftest();
        printf(" ASRC front-end decimator selftest: %s\n", dec_ok ? "pass" : "FAIL");
#endif
        if( !dec_ok ) { while( 1 ) { } }
#if PATH_FRONTEND_Q31
        // CHANNEL ISOLATION of the 2/3 front end at the full ASRC_CH width.  Separate from the
        // check above because it asks a different question: that one asks whether the arithmetic
        // is right, this one asks whether channel N's output is built from channel N's input and
        // nothing else.  All sixteen channels carry DIFFERENT audio here (per-channel LCG seed),
        // which is what makes a leak visible -- with silent or identical neighbours it would not
        // be.  One channel per call because sixteen oracles do not fit the stack.
        bool iso_ok = true;
        uint8_t iso_ch = 0u;
        for( iso_ch = 0u; iso_ch < (uint8_t)ASRC_CH; ++iso_ch )
        {
            // s_path_decimated is the block ISR's decimated-output scratch, and it is idle
            // here BY CONSTRUCTION: s_path_decimator_ready is still 0 at this point (this init
            // has not published a denominator yet), so nothing else can be writing it.  Lending
            // it costs no RAM, which matters because the Q31 arm leaves only 2,980 bytes of
            // stack -- too few for this check's input block, output block and oracle together.
            if( !asrc_decimator_q31_isolation_selftest( iso_ch, s_path_decimated,
                                                        DECIMATED_BLOCK_CAPACITY ) )
            {
                iso_ok = false;
                break;
            }
        }
        printf( " ASRC Q31 16ch isolation selftest: %s (%lu of %lu channels)\n",
                iso_ok ? "pass" : "FAIL",
                (unsigned long)( iso_ok ? (uint32_t)ASRC_CH : (uint32_t)iso_ch ),
                (unsigned long)ASRC_CH );
        if( !iso_ok )
        {
            const asrc_decimator_q31_fail_t* f = asrc_decimator_q31_selftest_fail();
            printf( "  %s: /%lu case %lu live %lu blk %lu out %lu ch %lu got %ld want %ld\n",
                    f->what, (unsigned long)f->den, (unsigned long)f->kase,
                    (unsigned long)f->live_ch, (unsigned long)f->block,
                    (unsigned long)f->index, (unsigned long)f->channel,
                    (long)f->got, (long)f->want );
            while( 1 ) { }
        }
#endif
    }
#endif /* APP_ASRC_FRONTEND_SELFTEST -- silent when absent, on purpose: printing
        * nothing is the honest report, and the check runs in the AK512 image. */
    // Retire the old front end BEFORE publishing a new denominator: the owning block ISR
    // gates on `ready`, so between these two writes it drops its block instead of running
    // path_decimator_process() against a half-initialised union (see the volatile note above).
    s_path_decimator_ready = 0u;
#if APP_ASRC_48K_TO_8_INTEGRATION
    const uint32_t num_ab = 1u;   // /6 is an integer chain
    const uint32_t num_ba = 1u;
    const uint32_t den_ab = 6u;   // one-way A->B integration build
    const uint32_t den_ba = 1u;
    const uint32_t low_rate_hz = 8000u;   // fixed by the preset; /6 has a single coefficient set
    const uint32_t in_rate_hz = 48000u;   // this preset's leg A is 48 kHz, so no pre-stage
#else
    // The codec rate has already been committed before transport invokes this
    // reset hook.  A failed rate change restores the old codec rate and invokes
    // the hook again, so the path selection follows rollback automatically.
    const uint32_t a_rate_hz = wm8904_get_rate_hz( I2C_INST_A );
    const uint32_t b_rate_hz = wm8904_get_rate_hz( I2C_INST_B );
    // Table moved to asrc_audio_path_frontend_plan() so the *ar pair gate can ask the
    // same question before a rate change.  Read it there, not here.
    asrc_frontend_plan_t plan;
    asrc_audio_path_frontend_plan( a_rate_hz, b_rate_hz, &plan );
    const uint32_t num_ab      = plan.num_ab;
    const uint32_t num_ba      = plan.num_ba;
    const uint32_t den_ab      = plan.den_ab;
    const uint32_t den_ba      = plan.den_ba;
    const uint32_t low_rate_hz = plan.low_rate_hz;
    const uint32_t in_rate_hz  = plan.in_rate_hz;
#endif
    // Numerators BEFORE denominators: the rate plan reads num/den as a pair in main-loop
    // context, and publishing the numerator first means a plan sampled between the two stores
    // sees the OLD ratio (num=1 with the old den) rather than a mixed one.  The block ISR is
    // unaffected either way -- it reads neither (see the note on s_path_frontend_num_ab).
    s_path_frontend_num_ab = (uint8_t)num_ab;
    s_path_frontend_num_ba = (uint8_t)num_ba;
    s_path_frontend_den_ab = (uint8_t)den_ab;
    s_path_frontend_den_ba = (uint8_t)den_ba;
#if APP_USE_96K_RATE
    // Resolve the chain shape here, once, so the block ISR does not have to: 0 = no pre-stage.
    s_path_frontend_second_den =
        ( in_rate_hz == 96000u ) ? (uint8_t)( den_ab / PRESTAGE_DEN ) : 0u;
#endif
    /* Refuse a plan that would need TWO front ends -- see s_path_frontend_dual_den_unsupported.
     * Checked before active_den/active_num, because those two lines are exactly where a second
     * decimated direction would be silently dropped. */
    if( ( den_ab != 1u ) && ( den_ba != 1u ) )
    {
        s_path_frontend_dual_den_unsupported = 1u;
        s_path_decimator_ready               = 0u;
        printf( " ASRC front end: BOTH directions ask to be decimated (/%lu and /%lu);"
                " only one decimator exists -- front end NOT armed\n",
                (unsigned long)den_ab, (unsigned long)den_ba );
        return;
    }
    s_path_frontend_dual_den_unsupported = 0u;

    const uint32_t active_den = ( den_ab != 1u ) ? den_ab : den_ba;
    const uint32_t active_num = ( den_ab != 1u ) ? num_ab : num_ba;
    s_path_decimator_ready =
        ( ( active_den != 1u ) &&
          path_decimator_init( active_num, active_den, low_rate_hz, in_rate_hz ) ) ? 1u : 0u;
#endif
}

uint32_t asrc_audio_path_ab_fixed_rate_num( void )
{
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    return ( s_path_frontend_num_ab != 0u ) ? s_path_frontend_num_ab : 1u;
#else
    return APP_ASRC_AB_FIXED_RATE_NUM;
#endif
}

uint32_t asrc_audio_path_ba_fixed_rate_num( void )
{
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    return ( s_path_frontend_num_ba != 0u ) ? s_path_frontend_num_ba : 1u;
#else
    return APP_ASRC_BA_FIXED_RATE_NUM;
#endif
}

uint32_t asrc_audio_path_ab_fixed_rate_den( void )
{
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    return ( s_path_frontend_den_ab != 0u ) ? s_path_frontend_den_ab : 1u;
#else
    return APP_ASRC_AB_FIXED_RATE_DEN;
#endif
}

uint32_t asrc_audio_path_ba_fixed_rate_den( void )
{
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    return ( s_path_frontend_den_ba != 0u ) ? s_path_frontend_den_ba : 1u;
#else
    return APP_ASRC_BA_FIXED_RATE_DEN;
#endif
}

const char* asrc_audio_path_frontend_tag( void )
{
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    return s_path_frontend_tag;
#else
    return "";
#endif
}

/*
 * RATE-MONOTONIC LEG PRIORITIES (step 5 of the audit follow-up).
 *
 * Both leg ISRs sat at one priority, so neither could preempt the other and each leg's worst
 * response time carried the other leg's whole block ISR as blocking. Measured over the
 * pre-change 100-pair sweep, that blocking is what put 11 of the 13 front-end pairs at a
 * NEGATIVE deadline margin even though every leg fits its own deadline on its own; the same
 * pairs come out at +70..+124 us under a rate-monotonic model.
 * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md sections 12.2 and 13.1.
 *
 * The rule is the whole policy: the leg with the higher sample rate has the shorter block
 * period, hence the shorter deadline, hence the higher priority. Equal rates keep both legs on
 * the base priority, which is byte-for-byte the previous behaviour.
 *
 * Intended to be NOT dynamic: applied once per rate commit, from task level. A servo that moved
 * priorities while streaming would be a second control loop to reason about, and the question
 * this is here to answer -- does the rate-monotonic model reproduce on hardware -- does not
 * need one.
 *
 * THAT INTENT IS NOT WHAT HAPPENS TODAY, and this paragraph used to claim it was. Measured on
 * hardware 2026-08-25: a leg-B rate change takes audio_transport's FAST path, which keeps the
 * transport and codec A running and re-inits codec B alone -- and then calls the transport
 * client's reset_stream_state(), which is asrc_transport_reset(), which is this function. So
 * one callback is reached down two paths that make OPPOSITE claims about whether anything is
 * streaming.
 *
 * What that costs, stated precisely. It is NOT self-preemption: this runs at task level, so on a
 * single core it cannot execute while a leg ISR is in flight, and a source at the same IPL as the
 * running level does not preempt it either. What it does do is (1) update the two legs' IPC in
 * SEQUENCE with both legs' ISRs live, so between the two writes the priority map is neither the
 * old one nor the new one and an ISR accepted in that window sees a map no design step describes,
 * and (2) reset the ASRC stream state -- FIFO, ring pointers, servo -- while the producers and
 * consumers of that state keep firing. Note also that the setter below writes IPC WITHOUT masking
 * the RX interrupt, whereas the DMA HAL's ordinary reconfigure path masks the IRQ first for
 * exactly this reason.
 *
 * Build with APP_TRANSPORT_LEG_B_FAST_RATE_CHANGE=0 to force the leg-B change through the same
 * whole-transport restart leg A takes, which restores this precondition without touching the
 * priority logic; that is the A/B currently open on the stack-error question.
 * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 19.3.
 *
 * APP_ASRC_RATE_MONOTONIC_ISR=0 restores the symmetric priorities for an A/B on the bench
 * without touching this logic.
 */
void asrc_audio_path_apply_isr_priorities( void )
{
#if APP_ASRC_RATE_MONOTONIC_ISR
    const uint32_t a_rate_hz = wm8904_get_rate_hz( I2C_INST_A );
    const uint32_t b_rate_hz = wm8904_get_rate_hz( I2C_INST_B );

    nora_spi_i2s_tdm_inst_t* const leg_a = audio_transport_tdm_leg_a();
    nora_spi_i2s_tdm_inst_t* const leg_b = audio_transport_tdm_leg_b();

    if( ( leg_a == NULL ) || ( leg_b == NULL ) ||
        ( a_rate_hz == 0u ) || ( b_rate_hz == 0u ) )
    {
        return;   /* not known yet -- leave the base priorities alone */
    }

    if( a_rate_hz == b_rate_hz )
    {
        /* Same deadline: no rate-monotonic order exists. Put both back on the base. */
        (void)nora_spi_i2s_tdm_set_rate_monotonic_priorities( leg_a, NULL );
        printf( " ASRC ISR prio: %luHz/%luHz equal -> both base (IP A=%u B=%u)%s",
                (unsigned long)a_rate_hz, (unsigned long)b_rate_hz,
                (unsigned)nora_spi_i2s_tdm_inst_irq_priority( leg_a ),
                (unsigned)nora_spi_i2s_tdm_inst_irq_priority( leg_b ),
                ASRC_PRIO_EOL );
        return;
    }

    nora_spi_i2s_tdm_inst_t* const shorter = ( a_rate_hz > b_rate_hz ) ? leg_a : leg_b;
    nora_spi_i2s_tdm_inst_t* const longer  = ( a_rate_hz > b_rate_hz ) ? leg_b : leg_a;

    if( !nora_spi_i2s_tdm_set_rate_monotonic_priorities( shorter, longer ) )
    {
        printf( " ASRC ISR prio: REFUSED (HAL)%s", ASRC_PRIO_EOL );
        return;
    }
    printf( " ASRC ISR prio: leg %c high (%luHz), leg %c low (%luHz) (IP A=%u B=%u)%s",
            ( a_rate_hz > b_rate_hz ) ? 'A' : 'B',
            (unsigned long)( ( a_rate_hz > b_rate_hz ) ? a_rate_hz : b_rate_hz ),
            ( a_rate_hz > b_rate_hz ) ? 'B' : 'A',
            (unsigned long)( ( a_rate_hz > b_rate_hz ) ? b_rate_hz : a_rate_hz ),
            (unsigned)nora_spi_i2s_tdm_inst_irq_priority( leg_a ),
            (unsigned)nora_spi_i2s_tdm_inst_irq_priority( leg_b ),
            ASRC_PRIO_EOL );
#endif
}

void asrc_audio_path_dbg_print( void )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t cb_a_ticks = s_path_profile.callback_a_ticks;
    const uint32_t cb_b_ticks = s_path_profile.callback_b_ticks;
    const uint32_t ps_a_ticks = s_path_profile.push_ab_ticks;
    const uint32_t ps_b_ticks = s_path_profile.push_ba_ticks;
    const uint32_t lm_a_ticks = s_path_profile.meter_a_ticks;
    const uint32_t lm_b_ticks = s_path_profile.meter_b_ticks;
    s_path_profile = (asrc_path_profile_t){ 0 };
    const uint32_t cb_a = nora_high_res_timer_count_to_us_x10( cb_a_ticks );
    const uint32_t cb_b = nora_high_res_timer_count_to_us_x10( cb_b_ticks );
    const uint32_t ps_a = nora_high_res_timer_count_to_us_x10( ps_a_ticks );
    const uint32_t ps_b = nora_high_res_timer_count_to_us_x10( ps_b_ticks );
    const uint32_t lm_a = nora_high_res_timer_count_to_us_x10( lm_a_ticks );
    const uint32_t lm_b = nora_high_res_timer_count_to_us_x10( lm_b_ticks );
    printf("ASRCpath[M=%u L=%u W=%u]: cbA=%lu.%luus pushAB=%lu.%luus ledA=%lu.%luus  "
           "cbB=%lu.%luus pushBA=%lu.%luus ledB=%lu.%luus led_stride=%u\n",
           (unsigned)ASRC_POLY_M, (unsigned)ASRC_POLY_L, (unsigned)ASRC_POLY_WINDOW,
           (unsigned long)(cb_a / 10u), (unsigned long)(cb_a % 10u),
           (unsigned long)(ps_a / 10u), (unsigned long)(ps_a % 10u),
           (unsigned long)(lm_a / 10u), (unsigned long)(lm_a % 10u),
           (unsigned long)(cb_b / 10u), (unsigned long)(cb_b % 10u),
           (unsigned long)(ps_b / 10u), (unsigned long)(ps_b % 10u),
           (unsigned long)(lm_b / 10u), (unsigned long)(lm_b % 10u),
           (unsigned)APP_ASRC_LED_FRAME_STRIDE );
#endif
    // The front-end state used to be printed here as a pair of "ASRCpath <dir> front-end: ..."
    // lines. Since 2026-07-29 it is the trailing `fe=` field of the AB/BA lines in
    // audio_app_asrc_dbg_print() instead -- same information (divider identity plus the
    // intermediate ring's ovf/udf), next to the engine it describes, two fewer lines per report.
    // The den accessors above are what that field reads.
}

// --- Declick pop measurement (see [internal] manual_wm8904_mute_restart_declick.md) ---
// Producer (block ISR, pop_meas_observe) / consumer (main-loop console, *_read). Volatile handshake;
// no big buffer -- just running peak-abs + sum-of-squares of A's ADC over channels 0/1 while armed.
static volatile uint8_t  s_pop_active = 0u;
static volatile int32_t  s_pop_peak   = 0;
static volatile uint64_t s_pop_sumsq  = 0u;
static volatile uint32_t s_pop_frames = 0u;

void asrc_audio_path_pop_meas_reset( void )
{
    s_pop_peak = 0; s_pop_sumsq = 0u; s_pop_frames = 0u;
}

void asrc_audio_path_pop_meas_set_active( bool on )
{
    s_pop_active = on ? 1u : 0u;
}

void asrc_audio_path_pop_meas_read( int32_t* out_peak, uint64_t* out_sumsq, uint32_t* out_frames )
{
    if( out_peak )   { *out_peak   = s_pop_peak; }
    if( out_sumsq )  { *out_sumsq  = s_pop_sumsq; }
    if( out_frames ) { *out_frames = s_pop_frames; }
}

// Observe A's raw ADC block (channels 0/1 = LINE-IN L/R, where the B->A loop lands). Cheap; NULL-guarded.
static void pop_meas_observe( const int32_t* adc_block )
{
    if( !s_pop_active || ( adc_block == NULL ) ) { return; }
    for( uint32_t n = 0u; n < (uint32_t)APP_BLOCK_FRAMES; n++ )
    {
        const int32_t* f = &adc_block[ n * (uint32_t)APP_SLOTS_PER_FS ];
        for( uint8_t ch = 0u; ch < 2u; ch++ )
        {
            const int32_t s = f[ch] >> 8;               // 24-bit signed sample
            const int32_t a = ( s < 0 ) ? -s : s;
            if( a > s_pop_peak ) { s_pop_peak = a; }
            s_pop_sumsq += (uint64_t)( (int64_t)s * (int64_t)s );
        }
        s_pop_frames++;
    }
}

void asrc_audio_path_leg_a_callback( const int32_t* src, int32_t* dst, void* user )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t callback_started = nora_high_res_timer_get_count();
#endif
    (void)user;
    pop_meas_observe( src );   // A's ADC (B HPOUT looped into A LINE-IN) -- declick pop metric
    // ASRC ROUTE select (A side). All routes here are pure ASRC -- no Classic/DRC kernel.
#if APP_ASRC_48K_TO_8_INTEGRATION
    size_t produced = 0u;
    const int32_t* pushed = NULL;
    if( !s_path_decimator_ready && !s_path_frontend_dual_den_unsupported ) { asrc_audio_path_reset(); }
    if( s_path_decimator_ready &&
        path_decimator_process( s_path_frontend_den_ab, s_path_frontend_second_den,
                                src, &pushed, &produced ) )
    {
        audio_app_asrc_push_ab_frames( pushed, produced, PATH_DECIMATED_STRIDE );
    }
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#elif APP_ASRC_RUNTIME_48K_TO_8
#if APP_ASRC_MEAS && (APP_MEAS_DIR == MEAS_DIR_AB)
    /* MEASUREMENT A->B, with the runtime front end compiled in.
     *
     * This branch sits EARLIER in the #elif chain than the generic A->B MEAS injection further
     * down (the B_ROUTE_ASRC_FROM_A arm), so without this the synthetic tone would never be
     * generated: leg A would push its live ADC, which is silent, and the capture reads the
     * noise floor.  Measured -88.6 dBFS peak before this was added.
     *
     * The tone is injected HERE, ahead of the routing gate, rather than bypassing the front
     * end -- that is the whole point.  It then flows through whichever chain the gate
     * selected, so leg B at 8/11.025/12/16 kHz measures the composed /2 pre-stage + 48 kHz
     * chain, while leg B at 48 kHz (den 1) measures the direct resampler at step 2.0. */
    static int32_t s_meas_in_ab[ APP_SLOTS_PER_FS * APP_BLOCK_FRAMES ];
    audio_app_meas_gen_input( s_meas_in_ab );
    const int32_t* const a_src = s_meas_in_ab;
#else
    const int32_t* const a_src = src;
#endif
    if( s_path_frontend_den_ab != 1u )
    {
        size_t produced = 0u;
        const int32_t* pushed = NULL;
        if( !s_path_decimator_ready && !s_path_frontend_dual_den_unsupported ) { asrc_audio_path_reset(); }
        if( s_path_decimator_ready &&
            path_decimator_process( s_path_frontend_den_ab, s_path_frontend_second_den,
                                    a_src, &pushed, &produced ) )
        {
            audio_app_asrc_push_ab_frames( pushed, produced, PATH_DECIMATED_STRIDE );
        }
    }
    else
    {
        path_push_ab( a_src );
    }
#if APP_B_ROUTE_USES_BA
    audio_app_asrc_pull_ba( dst );
#if APP_ASRC_MEAS && (APP_MEAS_DIR == MEAS_DIR_BA)
    /* MEASUREMENT B->A: capture the UPsampled output here.  Same shadowing problem as the
     * injection above -- this branch precedes the dedicated MEAS_DIR_BA arm further down the
     * chain, so without this call s_ready never sets, ?ac dumps nothing, and the capture
     * script times out waiting for *MEAS_END. */
    audio_app_meas_capture( dst );
#endif
    path_meter_submit( dst, 0u );
#else
    // One-way A->B (the 96 kHz preset): there is no B->A engine to pull from, so leg A's output is
    // silence -- same as the fixed integration preset above.  Before the 96 kHz rows existed, every
    // build reaching here was bidirectional, so this arm was unguarded and would not link.
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
    path_meter_submit( dst, 0u );
#endif
#elif APP_ASRC_48K_TO_8_DECIMATOR
    (void)src;
    audio_app_meas_decimator_process_block();
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#elif APP_ASRC_MEAS && (APP_MEAS_DIR == MEAS_DIR_BA)
    // MEASUREMENT B->A: A OUT = resampled B-input sine (injected in the SPI2 path); capture
    // it here. No A-side DSP/gain/flip4 -- only the B->A resampler is under test.
    audio_app_asrc_pull_ba( dst );
    audio_app_meas_capture( dst );
    path_meter_submit( dst, 0u );
#elif APP_B_INDEP_DOMAIN && (APP_B_ROUTE == B_ROUTE_ASRC_LIGHT)
    // LOAD TEST: no heavy A-DSP. A OUT = resampled B input (B->A FIFO).
    path_push_ab( src );
    audio_app_asrc_pull_ba( dst );
    path_meter_submit( dst, 0u );
#elif APP_B_INDEP_DOMAIN && (APP_B_ROUTE == B_ROUTE_ASRC_BIDIR)
    // Bidirectional cross: A OUT = resampled B input. The meter holds the max of A/B submits.
    path_push_ab( src );
    audio_app_asrc_pull_ba( dst );
    path_meter_submit( dst, 0u );
#elif APP_B_INDEP_DOMAIN && (APP_B_ROUTE == B_ROUTE_ASRC_FROM_A)
    // A->B one-way: tap A input into the A->B FIFO and keep A output silent.
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#if APP_ASRC_MEAS
    static int32_t s_meas_in[ APP_SLOTS_PER_FS * APP_BLOCK_FRAMES ];
    audio_app_meas_gen_input( s_meas_in );
    path_push_ab( s_meas_in );
#else
    path_push_ab( src );
#endif
#elif APP_B_INDEP_DOMAIN && (APP_B_ROUTE == B_ROUTE_ASRC_FROM_B)
    // B->A one-way: A OUT = resampled B input.
    audio_app_asrc_pull_ba( dst );
    path_meter_submit( dst, 0u );
#else
    #error "asrc_audio_path_leg_a_callback: unsupported ASRC route (APP_B_ROUTE)."
#endif
#if APP_ASRC_HEADROOM_INSTRUMENT
    profile_peak_ticks( &s_path_profile.callback_a_ticks, callback_started );
#endif
}

void asrc_audio_path_leg_b_callback( const int32_t* src, int32_t* dst, void* user )
{
    (void)user;
    if( ( src == NULL ) || ( dst == NULL ) )
    {
        return;
    }
#if APP_ASRC_HEADROOM_INSTRUMENT
    const uint32_t callback_started = nora_high_res_timer_get_count();
#endif
    // ASRC ROUTE select (B side).
#if APP_ASRC_48K_TO_8_INTEGRATION
    (void)src;
    audio_app_asrc_pull_ab( dst );
    path_meter_submit( dst, 1u );
#elif APP_ASRC_48K_TO_8_DECIMATOR
    (void)src;
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#elif APP_ASRC_MEAS && (APP_MEAS_DIR == MEAS_DIR_BA)
    // MEASUREMENT B->A: inject an on-chip sine into B->A and keep B output silent.
    (void)src;
    static int32_t s_meas_in_b[ APP_SLOTS_PER_FS * APP_BLOCK_FRAMES ];
    audio_app_meas_gen_input( s_meas_in_b );
#if APP_ASRC_RUNTIME_48K_TO_8
    // This arm PRECEDES the B_ROUTE_ASRC_BIDIR arm below, so it must carry the front end itself.
    // It did not until 2026-08-21, and the result was not a wrong number but an unmeasurable one:
    // with leg A at a low rate the 48 kHz injection went in undecimated, the B->A ring filled to
    // its overflow guard (fill pinned at ASRC_FIFO_FRAMES-4) and the guard discarded ~2/3 of every
    // second's frames (measured: fill=124/128, drop growing ~32 k/s at 48 k -> 16 k, i.e. exactly
    // the 48 kHz surplus over a 16 kHz reader).  The capture that came out of that read -14.8 dBFS
    // for a -1 dBFS tone with a -13 dBc second harmonic.  The shipping BIDIR image was never
    // affected -- it takes the arm below, which decimates -- which is why the same rate pair and
    // direction ran with drop=0 in the section 7 hardware cases.
    if( s_path_frontend_den_ba != 1u )
    {
        size_t produced = 0u;
        const int32_t* pushed = NULL;
        if( !s_path_decimator_ready && !s_path_frontend_dual_den_unsupported ) { asrc_audio_path_reset(); }
        if( s_path_decimator_ready &&
            path_decimator_process( s_path_frontend_den_ba, s_path_frontend_second_den,
                                    s_meas_in_b, &pushed, &produced ) )
        {
            audio_app_asrc_push_ba_frames( pushed, produced, PATH_DECIMATED_STRIDE );
        }
    }
    else
#endif
    {
        path_push_ba( s_meas_in_b );
    }
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#elif APP_B_ROUTE == B_ROUTE_ASRC_FROM_A
    audio_app_asrc_pull_ab( dst );
    path_meter_submit( dst, 1u );
#if APP_ASRC_MEAS
    audio_app_meas_capture( dst );
#endif
#elif APP_B_ROUTE == B_ROUTE_ASRC_FROM_B
    // B->A one-way: push B input into B->A and keep B output silent.
    path_push_ba( src );
    for( uint32_t i = 0u; i < (uint32_t)APP_SLOTS_PER_FS * (uint32_t)APP_BLOCK_FRAMES; i++ ) { dst[i] = 0; }
#elif APP_B_ROUTE == B_ROUTE_ASRC_BIDIR
#if APP_ASRC_RUNTIME_48K_TO_8
    // Mirror of the leg-A branch above: when leg B is the 48 kHz side and leg A runs low, the
    // B->A direction is the down-sampling one and needs the same anti-alias front end.  Only
    // one of the two denominators can be != 1, so only one leg ever takes this path.
    if( s_path_frontend_den_ba != 1u )
    {
        size_t produced = 0u;
        const int32_t* pushed = NULL;
        if( !s_path_decimator_ready && !s_path_frontend_dual_den_unsupported ) { asrc_audio_path_reset(); }
        if( s_path_decimator_ready &&
            path_decimator_process( s_path_frontend_den_ba, s_path_frontend_second_den,
                                    src, &pushed, &produced ) )
        {
            audio_app_asrc_push_ba_frames( pushed, produced, PATH_DECIMATED_STRIDE );
        }
    }
    else
#endif
    {
        path_push_ba( src );
    }
    audio_app_asrc_pull_ab( dst );
    path_meter_submit( dst, 1u );
#elif APP_B_ROUTE == B_ROUTE_ASRC_LIGHT
    path_push_ba( src );
    audio_app_asrc_pull_ab( dst );
    path_meter_submit( dst, 1u );
#else
    #error "asrc_audio_path_leg_b_callback: unsupported ASRC route (APP_B_ROUTE)."
#endif
#if APP_ASRC_HEADROOM_INSTRUMENT
    profile_peak_ticks( &s_path_profile.callback_b_ticks, callback_started );
#endif
}
