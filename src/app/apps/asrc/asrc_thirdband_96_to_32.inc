/*
 * asrc_thirdband_96_to_32.inc -- the 96 -> 32 kHz third-band 3-path polyphase allpass
 * front end, as an OPT-IN REPLACEMENT for the composed 41-tap /2 + 97-tap 2/3 chain.
 *
 * Included from asrc_decimator_q31.inc, not compiled on its own, for one concrete
 * reason: it carves its state, its gathered input block and its output scratch out of
 * `s_q31_hist` and its coefficients out of `s_q31_coeff`, both of which are statics of
 * that translation unit.  That is the whole RAM story -- see THE ARENA below.
 *
 * WHAT IT IS.  H(z) = (1/3)[A0(z^3) + z^-1 A1(z^3) + z^-2 A2(z^3)], each A_k a cascade
 * of first-order real allpass sections running at the 32 kHz OUTPUT rate on its own
 * commutator branch.  16 sections total against the shipping chain's 138 taps, and
 * -110.11 dBc of worst alias into the protected 0-15 kHz band against the shipping
 * chain's -22.51 dBc.  The design, the third-band identity it rests on and the
 * asynchronous-ratio gate are in the host study; the hardware kernel measurement is in
 * [internal] report_96to32_thirdband_kernel_bench_2026-09-09.md and the arithmetic
 * re-qualification in [internal] report_96to32_thirdband_stage0_host_gate_2026-09-10.md.
 *
 * WHAT IT IS NOT.  It is not a strict full-band 96 -> 32 kHz front end: 0-15 kHz is
 * protected, 15-16 kHz is RELAXED at -3.53 dBc by specification (the transition is
 * 15 -> 17 kHz and the structure pins |H(48k)| at 1/3), and the phase is not linear --
 * group delay varies 388 us over 0-15 kHz.  Both are accepted, in writing, for this
 * front end; neither is a defect to be reported as one.
 *
 * THE ARENA.  Nothing here is a new object.  The chains are mutually exclusive -- one
 * front end is armed at a time -- so this stage takes the front of `s_q31_hist`
 * (space(ymemory)) for its state, gathered input and output scratch, and the front of
 * `s_q31_coeff` (space(xmemory)) for its coefficients.  Two consequences, both
 * load-bearing:
 *
 *   * THE X/Y SPLIT THE MAC REQUIRES IS ALREADY SATISFIED.  `msc.l [w11], [w9], A` needs
 *     its two operands in different data spaces or both reads serialise and the
 *     instruction silently costs an extra cycle (DS70005591C 4.3.17).  Coefficients are
 *     in X because s_q31_coeff is; state is in Y because s_q31_hist is.
 *   * SWITCHING MODES DESTROYS THE OTHER CHAIN'S STATE.  That is why
 *     asrc_thirdband_96_to_32_set_mode() re-initialises the front end rather than just
 *     flipping a flag, and why the switch is audible as a settling transient.  Giving
 *     this stage its own 2.7 KB would avoid that and was rejected: this preset's RAM is
 *     the binding constraint on channel width.
 *
 * THE STATE MUST BE A FLAT [ch][16][2] BLOCK.  The kernel walks a channel's sections
 * with a constant +8 and channels with a constant +128, which is the layout its 5.995
 * cycles/section/channel was measured at.  Indexing `s_q31_hist` by its own 264-word row
 * stride would change that stride, and "an untouched stage moves several microseconds on
 * a placement change" is a trap this tree has already paid for.
 */

/* ---- geometry ----------------------------------------------------------------------- */

#include "asrc_thirdband_96_to_32_coeffs.inc"
#if APP_ASRC_LEG_PROFILE
/* Named here rather than left to the translation unit: this include sits ABOVE the
 * front end's own use of the timer, so without it the first reference is an implicit
 * declaration and the real header then conflicts with it. */
#include "nora_high_res_timer.h"
#endif

#define TB32_SEC        (ASRC_THIRDBAND_96_TO_32_SECTIONS)
#define TB32_DECIM      (3u)

/* Input headroom, applied here as a right shift and taken back out by the kernel's
 * `sacr.l a, #-HSHIFT`.  NOT a matter of taste and NOT the 6 dB that was expected: the
 * host gate measured the SECTION OUTPUTS -- which are stored int32 words and therefore
 * saturate -- at 1.011x full scale for a 997 Hz full-scale sine, 2.848x for full-scale
 * noise and 4.970x at the L1 bound of the deepest cascade.  6 dB leaves squares and noise
 * clipping; 18 dB clears the rigorous bound.
 *
 * It is FREE OF PRECISION LOSS because the input is s24-left: eight sub-LSBs are already
 * zero, so a shift of up to 8 discards nothing at all.  And it costs the same one
 * instruction per gathered sample whatever the amount.  Measured alias with this shift:
 * -110.11 dBc, i.e. unchanged.
 *
 * Must equal ASRC_TB32_HSHIFT in the assembler kernel.  There is no way for the compiler
 * to check that, so it is stated in both files and the boot check catches a mismatch as a
 * factor-of-eight error rather than as something subtle. */
#ifndef APP_ASRC_THIRDBAND_HEADROOM_SHIFT
#define APP_ASRC_THIRDBAND_HEADROOM_SHIFT (3)
#endif
#define TB32_HSHIFT     (APP_ASRC_THIRDBAND_HEADROOM_SHIFT)

/* The 17th coefficient word: the 1/3 of the 3-path average, which the kernel applies with
 * three MACs so that the branch sum lives in the accumulator's guard bits.  FLOOR and not
 * round: round(2^31/3) gives 3*c = 2^31 + 1, so a full-scale DC input would come out one
 * LSB over and saturate.  floor gives 3*c = 2^31 - 2, a gain of 1 - 2^-30. */
#define TB32_THIRD_Q31  ((int32_t)(0x80000000u / 3u))
_Static_assert(TB32_THIRD_Q31 == 715827882, "floor(2^31/3)");
_Static_assert((3 * (int64_t)TB32_THIRD_Q31) < 0x80000000LL,
               "3*c must stay under unity or full-scale DC saturates");

/* One block's worth of gathered 96 kHz input, plus the up-to-two frames left over when
 * the block length is not a multiple of 3.  16 + 2 = 18 rows, 6 output frames. */
#define TB32_MAX_IN     ((size_t)(((uint16_t)APP_BLOCK_FRAMES) + (TB32_DECIM - 1u)))
#define TB32_MAX_OUT    (TB32_MAX_IN / TB32_DECIM)

#define TB32_STATE_WORDS ((size_t)ASRC_DEC_Q31_CH * TB32_SEC * 2u)
#define TB32_IN_WORDS    (TB32_MAX_IN * (size_t)ASRC_DEC_Q31_CH)
#define TB32_OUT_WORDS   (TB32_MAX_OUT * (size_t)ASRC_DEC_Q31_CH)
#define TB32_ARENA_WORDS (TB32_STATE_WORDS + TB32_IN_WORDS + TB32_OUT_WORDS)

_Static_assert(TB32_ARENA_WORDS <= ((size_t)ASRC_DEC_Q31_CH * DEC_Q31_HIST_PER_CH),
               "the third band does not fit the Q31 history arena it overlays");
_Static_assert((TB32_SEC + 1u) <= DEC_Q31_COEFF_MAX,
               "the third band's coefficient row does not fit s_q31_coeff");
_Static_assert(TB32_SEC == 16u,
               "the assembler kernel is assembled for 16 sections (channel stride 128 B)");

#define TB32_STATE      (&s_q31_hist[0][0])
#define TB32_IN         (TB32_STATE + TB32_STATE_WORDS)
#define TB32_OUT        (TB32_IN + TB32_IN_WORDS)
#define TB32_COEFF      (&s_q31_coeff[0])

/* THE LIVE TAP.  Records, from inside the live block path, the gathered INPUT and the
 * OUTPUT of the same window for channel 0, so the host can run its exact model on the
 * captured input and compare it with the captured output.  That is *av04 done on the real
 * signal in the real context, which is the one thing the synthetic vector cannot cover:
 * *av04 proved the stage bit-exact on a buffer it owned, in a foreground call.
 *
 * It costs NO RAM: the arena this stage overlays is 12,672 B and the stage uses 2,688, so
 * the tap sits in the ~10 KB that is idle whenever the third band is armed.  Sized so that
 * the input window is exactly the one that produced the output window: 3 input frames per
 * output frame. */
/* 1024 outputs is 32 ms, i.e. 31 Hz FFT bins -- enough to resolve the 300-600 Hz the ear
 * reported, which 512 samples was not.  The input capture is a SPECTRUM sample of what the
 * kernel saw, not the exact window that produced the output block, because the arena's idle
 * part cannot hold both at 3:1 and the settling of a cold model would swamp a sample-by-
 * sample comparison over a window this short anyway. */
/* CHANNEL 1 IS THE FAR END OF THE DIGITAL CHAIN, NOT THE 96 kHz INPUT.  The input side is
 * closed: over one armed window the gathered 96 kHz input measured -96.90 dBc in band and
 * the stage output -101.72 dBc, at out/in = 0.9996.  What is left to localise is therefore
 * everything AFTER the stage -- push, the AB FIFO, the servo, the rear polyphase (M = 30 on
 * this preset) -- and the arena's idle part holds two 1024-word buffers, not three.  So the second buffer is
 * spent where it can still change a conclusion.
 *
 * Both taps are now 32 kHz, 1024 samples, channel 0, filled from one arm.  That is worth
 * more than the rate pair was: an off-bin tone scallops by the SAME amount in both windows,
 * so the RATIO of the two peaks is a valid gain for the span between them even though
 * neither peak alone is a valid absolute level. */
#define TB32_TAP_OUT_N   (1024u)
#define TB32_TAP_B_N     (1024u)
#define TB32_TAP_WORDS   ((size_t)TB32_TAP_OUT_N + (size_t)TB32_TAP_B_N)
#define TB32_TAP_OUT     (TB32_OUT + TB32_OUT_WORDS)
#define TB32_TAP_B       (TB32_TAP_OUT + TB32_TAP_OUT_N)
_Static_assert((TB32_ARENA_WORDS + TB32_TAP_WORDS)
                   <= ((size_t)ASRC_DEC_Q31_CH * DEC_Q31_HIST_PER_CH),
               "the live tap does not fit the idle part of the Q31 history arena");

/* The kernel is assembled per channel count because its commutator displacements and its
 * pair count are assembly-time constants.  Select, never default: a width with no kernel
 * is a build error here rather than a front end that filters some other number of
 * channels. */
extern void asrc_thirdband_q31_block_nch12(int32_t *state, const int32_t *coeff,
                                           const int32_t *in, int32_t *out,
                                           uint32_t frames);
extern void asrc_thirdband_q31_block_nch14(int32_t *state, const int32_t *coeff,
                                           const int32_t *in, int32_t *out,
                                           uint32_t frames);
extern void asrc_thirdband_q31_block_nch16(int32_t *state, const int32_t *coeff,
                                           const int32_t *in, int32_t *out,
                                           uint32_t frames);
#if (ASRC_DEC_Q31_CH == 12u)
#define TB32_KERNEL asrc_thirdband_q31_block_nch12
#elif (ASRC_DEC_Q31_CH == 14u)
#define TB32_KERNEL asrc_thirdband_q31_block_nch14
#elif (ASRC_DEC_Q31_CH == 16u)
#define TB32_KERNEL asrc_thirdband_q31_block_nch16
#else
#error "APP_ASRC_THIRDBAND_96_TO_32 has no assembler kernel for this ASRC_CH. Add a section to asrc_thirdband_96_to_32_kernel_dspic33ak.s rather than letting a different width run."
#endif
_Static_assert((ASRC_DEC_Q31_CH % 2u) == 0u,
               "the kernel processes channels in pairs");

/* Clip counting is one compare per output sample.  It is a SAFETY counter, not a
 * measurement -- the output of a unity-gain filter with an L1 norm above 1 does clip on
 * transients, and the shipping chain does the same -- but it is still on the audio path,
 * so it is switchable: the front-end cycle figure must be taken with it OFF, and the
 * audio checks with it ON. */
#ifndef APP_ASRC_THIRDBAND_CLIP_COUNT
#define APP_ASRC_THIRDBAND_CLIP_COUNT (1)
#endif

/* ---- state -------------------------------------------------------------------------- */

/* Boots on whatever APP_ASRC_THIRDBAND_DEFAULT_ON names, because the runtime switch needs a
 * leg-A restart and this preset's restart does not bring WM8904-B back -- see the config
 * header.  0 (the composed chain) unless a build says otherwise. */
static volatile uint8_t s_tb32_mode = (APP_ASRC_THIRDBAND_DEFAULT_ON != 0) ? 1u : 0u;
static volatile uint8_t s_tb32_armed;     /* what the block ISR must actually do      */
static uint8_t  s_tb32_carry;             /* 0..2 input frames held over              */
static uint32_t s_tb32_blocks;
static uint32_t s_tb32_out_frames;
static uint32_t s_tb32_clips;
static uint32_t s_tb32_refused;
/* 0 = idle, 1 = arming (waiting for the next block boundary), 2 = filling, 3 = complete.
 *
 * COMPLETE MEANS BOTH SIDES, and that needs two flags rather than one state.  The two
 * captures are filled from DIFFERENT interrupt domains -- the stage end from leg A's block
 * path, the far end from leg B's pull -- and the two legs run at independent rates on
 * independent clocks.  Whichever side fills first must stop and leave the other running, so
 * a single "full" state set by the stage side would have truncated leg B's window to however
 * much of it happened to have arrived, which is exactly the short capture that makes a level
 * ratio meaningless.  Each side sets its own done flag and the last one to finish promotes
 * the state; the promotion is idempotent, so both domains may run it. */
static volatile uint8_t  s_tb32_tap_state;
static volatile uint16_t s_tb32_tap_out_n;
static volatile uint16_t s_tb32_tap_b_n;  /* leg B's ISR writes this one, leg A zeroes it */
static volatile uint8_t  s_tb32_tap_out_done;
static volatile uint8_t  s_tb32_tap_b_done;
#if APP_ASRC_LEG_PROFILE
static volatile uint32_t s_tb32_gather_ticks;
static volatile uint32_t s_tb32_kernel_ticks;
#endif

/* s_tb32_eligible and s_tb32_last_init are declared in asrc_decimator_q31.inc, above
 * asrc_decimator_q31_init() -- that function fills them, and it has to be defined before
 * this include so that set_mode() below can call it to rebuild the chain this stage
 * overwrote. */

static void tb32_load_coeff(void)
{
    for (uint16_t i = 0u; i < TB32_SEC; ++i)
    {
        TB32_COEFF[i] = s_thirdband_96_to_32_coeff[i];
    }
    TB32_COEFF[TB32_SEC] = TB32_THIRD_Q31;
}

/* Zero the filter memory and the commutator phase.  The ONE place that happens; never
 * per block -- a per-block reset would turn a 16-section recursive filter into a
 * transient generator, which is the same rule the Full-IIR trial stage states. */
/* Stop the live tap and forget it.  Called from the arm/retire hook in both directions and
 * whenever an arm is refused; safe at any time because every writer checks the state it
 * clears.  Order matters: state first, so both fill paths stop before the counters they
 * index with are reset. */
static void tb32_tap_cancel(void)
{
    s_tb32_tap_state = 0u;
    s_tb32_tap_out_done = 0u;
    s_tb32_tap_b_done = 0u;
    s_tb32_tap_out_n = 0u;
    s_tb32_tap_b_n = 0u;
}

/* Promote to complete once BOTH sides are done.  Called from leg A's block path and from leg
 * B's pull, so it must be idempotent and must not care which one arrives second: it only ever
 * writes the same value, and each flag it reads is written by exactly one domain. */
static void tb32_tap_promote(void)
{
    if ((s_tb32_tap_out_done != 0u) && (s_tb32_tap_b_done != 0u))
    {
        s_tb32_tap_state = 3u;
    }
}

static void tb32_reset_state(void)
{
    for (size_t i = 0u; i < TB32_STATE_WORDS; ++i) { TB32_STATE[i] = 0; }
    for (size_t i = 0u; i < TB32_IN_WORDS; ++i)    { TB32_IN[i] = 0; }
    s_tb32_carry = 0u;
}

size_t asrc_thirdband_96_to_32_output_frames(size_t input_frames)
{
    return ((size_t)s_tb32_carry + input_frames) / TB32_DECIM;
}

bool asrc_thirdband_96_to_32_armed(void)
{
    return (s_tb32_armed != 0u);
}

asrc_thirdband_mode_t asrc_thirdband_96_to_32_mode(void)
{
    return (s_tb32_mode != 0u) ? ASRC_THIRDBAND_MODE_ON : ASRC_THIRDBAND_MODE_OFF;
}

void asrc_thirdband_96_to_32_stats(uint32_t *blocks, uint32_t *out_frames,
                                   uint32_t *clips, uint32_t *refused)
{
    if (blocks != NULL)     { *blocks = s_tb32_blocks; }
    if (out_frames != NULL) { *out_frames = s_tb32_out_frames; }
    if (clips != NULL)      { *clips = s_tb32_clips; }
    if (refused != NULL)    { *refused = s_tb32_refused; }
}

void asrc_thirdband_96_to_32_stats_clear(void)
{
    s_tb32_blocks = 0u;
    s_tb32_out_frames = 0u;
    s_tb32_clips = 0u;
    s_tb32_refused = 0u;
}

void asrc_thirdband_96_to_32_identity(uint32_t *coeff_crc, uint16_t *sections,
                                      int16_t *headroom_shift, int32_t *worst_alias_milli)
{
    if (coeff_crc != NULL)   { *coeff_crc = ASRC_THIRDBAND_96_TO_32_COEFF_CRC32; }
    if (sections != NULL)    { *sections = (uint16_t)TB32_SEC; }
    if (headroom_shift != NULL) { *headroom_shift = (int16_t)TB32_HSHIFT; }
    if (worst_alias_milli != NULL)
    {
        *worst_alias_milli = ASRC_THIRDBAND_96_TO_32_WORST_ALIAS_MILLI_DBC;
    }
}

#if APP_ASRC_LEG_PROFILE
/* Same two halves the composed chain reports, so one telemetry line describes either
 * front end: `pre` is everything ahead of the arithmetic (here the gather and the channel
 * repeat), `r23` is the arithmetic (here the whole 3:1 kernel).  Read-and-clear. */
void asrc_thirdband_96_to_32_profile_take_last(uint32_t *gather, uint32_t *kernel)
{
    if (gather != NULL) { *gather = s_tb32_gather_ticks; }
    if (kernel != NULL) { *kernel = s_tb32_kernel_ticks; }
    s_tb32_gather_ticks = 0u;
    s_tb32_kernel_ticks = 0u;
}
#endif

/* ---- the block path ----------------------------------------------------------------- */

/* Returns false only for a request this stage cannot serve, and then it has written
 * nothing -- the caller drops the block, which is a stated refusal rather than a front
 * end that quietly emits the wrong number of frames. */
static bool tb32_process(const int32_t *input, size_t input_frames, size_t input_stride,
                         uint8_t in_mask, int32_t *output, size_t output_stride,
                         size_t *output_frames)
{
    const size_t avail = (size_t)s_tb32_carry + input_frames;
    if (avail > TB32_MAX_IN)
    {
        ++s_tb32_refused;
        return false;
    }
    const size_t n = avail / TB32_DECIM;

#if APP_ASRC_LEG_PROFILE
    const uint32_t t0 = nora_high_res_timer_get_count();
#endif
    /* GATHER.  Repeat the source's real channels up to the logical width and take the
     * headroom shift in the same store.  The shipping chain pays the same repeat inside
     * its own window (dec_q31_hist_push), so this is not an extra the other front end
     * avoids -- but it IS an extra the kernel bench did not measure, which is why the
     * chain figure is expected above the bench's 106.19. */
    {
        int32_t *dst = TB32_IN + ((size_t)s_tb32_carry * (size_t)ASRC_DEC_Q31_CH);
        const int32_t *src = input;
        for (size_t f = 0u; f < input_frames; ++f)
        {
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                dst[c] = src[c & in_mask] >> TB32_HSHIFT;
            }
            dst += (size_t)ASRC_DEC_Q31_CH;
            src += input_stride;
        }
    }
#if APP_ASRC_LEG_PROFILE
    const uint32_t t1 = nora_high_res_timer_get_count();
#endif

    if (n != 0u)
    {
        /* The kernel writes a CONTIGUOUS [frames][ASRC_CH] block: dropping the output
         * stride is what freed the register the third branch's result needed.  When the
         * caller's stride is the channel count -- which is what the audio path uses --
         * that is the caller's buffer and there is no copy at all. */
        int32_t *kout = (output_stride == (size_t)ASRC_DEC_Q31_CH) ? output : TB32_OUT;
        TB32_KERNEL(TB32_STATE, TB32_COEFF, TB32_IN, kout, (uint32_t)n);
        if (kout != output)
        {
            for (size_t f = 0u; f < n; ++f)
            {
                for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
                {
                    output[(f * output_stride) + c] = kout[(f * ASRC_DEC_Q31_CH) + c];
                }
            }
        }
#if APP_ASRC_THIRDBAND_CLIP_COUNT
        /* The output of a unity-gain filter whose L1 norm exceeds 1 clips on transients,
         * and so does the shipping chain's (L1 1.81 against this one's 3.45).  Count it
         * rather than argue about it: s24-left saturation is exactly these two values. */
        for (size_t f = 0u; f < n; ++f)
        {
            const int32_t *row = output + (f * output_stride);
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                if ((row[c] == (int32_t)0x7FFFFF00) || (row[c] == (int32_t)0x80000000))
                {
                    ++s_tb32_clips;
                }
            }
        }
#endif
    }
#if APP_ASRC_LEG_PROFILE
    {
        const uint32_t t2 = nora_high_res_timer_get_count();
        s_tb32_gather_ticks = t1 - t0;
        s_tb32_kernel_ticks = t2 - t1;
    }
#endif

    /* THE LIVE TAP, stage-output end.  Whole blocks only, channel 0 only (*av04 already
     * showed the channels agree).  The far end of the chain is captured by
     * asrc_thirdband_96_to_32_tap_legb(), from leg B, into the buffer that used to hold the
     * 96 kHz input.
     *
     * The input capture that lived here is gone, but the reason it had to sit BEFORE the
     * carry copy stays on the record because it nearly inverted a conclusion: the copy
     * overwrites the first rows of TB32_IN with the NEXT block's leftover, so a tap placed
     * after it records a time-scrambled window, which reads as broadband noise -- exactly
     * the wrong answer to "is the input already dirty".  An instrument watching an in-place
     * FIFO must sit ahead of whatever rewrites it. */
    if (s_tb32_tap_state != 0u)
    {
        if (s_tb32_tap_state == 1u)
        {
            s_tb32_tap_out_n = 0u;
            s_tb32_tap_b_n = 0u;
            s_tb32_tap_out_done = 0u;
            s_tb32_tap_b_done = 0u;
            /* Counters and flags first, state last.  Leg B fills only at state 2, so it
             * cannot be mid-write when this zeroes its counter. */
            s_tb32_tap_state = 2u;
        }
        if ((s_tb32_tap_state == 2u) && (s_tb32_tap_out_done == 0u))
        {
            /* THE INDEX IS RECOMPUTED FROM THE BUFFER, NOT TRUSTED.  An earlier form asked
             * "does the whole block fit?" and skipped the write when it did not, which is
             * correct only while the counter is correct: on hardware this counter was observed
             * at 3196 against a 1024-word buffer, so the write would have run 2 KB past the
             * end into the neighbouring capture and beyond -- an instrument on the audio path
             * becoming the fault it is looking for.  Root cause not identified (every store to
             * the counter in the linked image is a bounded increment or a reset).  So the
             * bound is enforced where the write happens instead of being inferred: clamp the
             * index, take only what fits, and finish the side when the buffer is full.  A
             * corrupted counter now costs one short capture, not a memory overwrite. */
            uint16_t at = s_tb32_tap_out_n;
            if (at > (uint16_t)TB32_TAP_OUT_N) { at = (uint16_t)TB32_TAP_OUT_N; }
            const size_t room = (size_t)((uint16_t)TB32_TAP_OUT_N - at);
            const size_t take = (n < room) ? n : room;
            for (size_t f = 0u; f < take; ++f)
            {
                TB32_TAP_OUT[(size_t)at + f] = output[(f * output_stride)];
            }
            at = (uint16_t)(at + take);
            s_tb32_tap_out_n = at;
            /* Done when the next block could not fit -- this side takes whole blocks, so it
             * normally stops short of the end -- or when it is full either way. */
            if ((take < n) || (((size_t)at + TB32_MAX_OUT) > (size_t)TB32_TAP_OUT_N))
            {
                s_tb32_tap_out_done = 1u;
                tb32_tap_promote();
            }
        }
    }

    /* Keep the remainder for the next block.  Two frames at most, so this is a copy of
     * at most 2 * ASRC_CH words and not a ring. */
    {
        const size_t used = n * TB32_DECIM;
        const size_t keep = avail - used;
        for (size_t i = 0u; i < (keep * (size_t)ASRC_DEC_Q31_CH); ++i)
        {
            TB32_IN[i] = TB32_IN[(used * (size_t)ASRC_DEC_Q31_CH) + i];
        }
        s_tb32_carry = (uint8_t)keep;
    }

    ++s_tb32_blocks;
    s_tb32_out_frames += (uint32_t)n;
    *output_frames = n;
    return true;
}

/* ---- structural self-check ----------------------------------------------------------- */

/* TWO PREDICTIONS THAT DO NOT DEPEND ON THE COEFFICIENTS AT ALL, which is what makes this a
 * structural check and not a re-run of the design:
 *
 *   DC     every A_k(1) = 1, so H(DC) = (1/3)(1 + 1 + 1) = 1 exactly.  A constant comes back
 *          as itself.
 *   32 kHz at w = 2*pi/3 the branches are evaluated at A_k(e^{j3w}) = A_k(1) = 1 whatever
 *          their coefficients are, so H = (1/3)(1 + W + W^2) = 0 EXACTLY, W = e^{-j2pi/3}.
 *          The period-3 input V*cos(2*pi*n/3) = { V, -V/2, -V/2 } must come out as zero.
 *
 * Between them they catch every mistake this integration can make that the compiler cannot
 * see: the SIGN of the kernel's headroom shift (a factor of 64 between the two directions),
 * the commutator's branch order, the 1/3 constant, and a coefficient row loaded in the wrong
 * order.  Neither tolerance is a fitted number -- the DC one is the settling of the deepest
 * pole after the run length below, and the null is bounded by coefficient quantisation.
 *
 * It USES THE LIVE FILTER STATE, so it must run with audio stopped (`*ts`), and it resets the
 * state on the way out.  That is the same rule the kernel bench follows. */
#define TB32_ST_V        ((int32_t)0x40000000)   /* 0.5 FS, and s24-left (low 8 bits zero) */
#define TB32_ST_BLOCKS   (800u)                  /* 800 outputs = 25 ms = 11.5 time constants */

static int32_t tb32_selftest_run(const int32_t *pattern3)
{
    int32_t blk[3 * (size_t)ASRC_DEC_Q31_CH];
    size_t  got = 0u;
    int32_t last = 0;

    tb32_reset_state();
    for (uint8_t f = 0u; f < 3u; ++f)
    {
        for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
        {
            blk[(f * (size_t)ASRC_DEC_Q31_CH) + c] = pattern3[f];
        }
    }
    for (uint16_t i = 0u; i < TB32_ST_BLOCKS; ++i)
    {
        if (!tb32_process(blk, 3u, (size_t)ASRC_DEC_Q31_CH, ASRC_DEC_Q31_FULL_MASK,
                          TB32_OUT, (size_t)ASRC_DEC_Q31_CH, &got))
        {
            return INT32_MIN;   /* refused: cannot happen for a 3-frame block */
        }
        if (got != 0u)
        {
            last = TB32_OUT[((got - 1u) * (size_t)ASRC_DEC_Q31_CH)];
        }
    }
    return last;
}

bool asrc_thirdband_96_to_32_selftest(asrc_thirdband_selftest_t *r)
{
    const int32_t dc3[3]   = { TB32_ST_V, TB32_ST_V, TB32_ST_V };
    /* cos(2*pi*n/3) for n = 0,1,2.  The block is a whole number of triples, so the
     * commutator phase stays aligned with it from call to call. */
    const int32_t null3[3] = { TB32_ST_V, -(TB32_ST_V / 2), -(TB32_ST_V / 2) };
    asrc_thirdband_selftest_t out;

    tb32_load_coeff();
    out.dc_want  = TB32_ST_V;
    out.dc_out   = tb32_selftest_run(dc3);
    out.null_out = tb32_selftest_run(null3);
    /* DC: 25 ms is 11.5 time constants of the deepest pole, so the residual is far below
     * one part in 4096; the s24-left mask alone costs 8 bits. */
    out.dc_tol   = TB32_ST_V / 4096;
    /* Null: exact in the reals, so all that is left is coefficient quantisation and the
     * per-section rounding of sixteen stores. */
    out.null_tol = TB32_ST_V / 1024;
    {
        const int32_t de = (out.dc_out > out.dc_want) ? (out.dc_out - out.dc_want)
                                                     : (out.dc_want - out.dc_out);
        const int32_t ne = (out.null_out < 0) ? -out.null_out : out.null_out;
        out.pass = (out.dc_out != INT32_MIN) && (de <= out.dc_tol) && (ne <= out.null_tol);
    }

    tb32_reset_state();
    asrc_thirdband_96_to_32_stats_clear();
    if (r != NULL) { *r = out; }
    return out.pass;
}

/* Arm the live tap.  Takes effect at the next block; `*av06` reads it back.  Reading it while
 * audio runs is safe because each fill path only writes while state == 2 and stops at its own
 * done flag, and the reader only looks at state 3.
 *
 * REFUSED UNLESS THE STAGE IS ARMED.  The capture buffers are carved out of s_q31_hist, the
 * history arena the COMPOSED chain owns whenever the third band is retired, so a tap that
 * could run without the stage would be an instrument corrupting the audio path it is
 * measuring.  Both write paths check as well -- this returns false so the console can say so
 * instead of leaving a tap that silently never fills. */
bool asrc_thirdband_96_to_32_tap_arm(void)
{
    if (s_tb32_armed == 0u)
    {
        tb32_tap_cancel();
        return false;
    }
    s_tb32_tap_state = 1u;
    return true;
}

/* Counts, clamped to the buffers they index.  The raw counter is deliberately not exposed:
 * every consumer of this either indexes a capture with it or prints it, and both are wrong if
 * it exceeds the buffer. */
bool asrc_thirdband_96_to_32_tap_ready(uint16_t *out_n, uint16_t *b_n)
{
    if (out_n != NULL)
    {
        const uint16_t v = s_tb32_tap_out_n;
        *out_n = (v > (uint16_t)TB32_TAP_OUT_N) ? (uint16_t)TB32_TAP_OUT_N : v;
    }
    if (b_n != NULL)
    {
        const uint16_t v = s_tb32_tap_b_n;
        *b_n = (v > (uint16_t)TB32_TAP_B_N) ? (uint16_t)TB32_TAP_B_N : v;
    }
    return (s_tb32_tap_state == 3u);
}

/* How long each capture can be.  Reported alongside the counts so that a count running past
 * its buffer is visible at the console instead of having to be inferred. */
void asrc_thirdband_96_to_32_tap_capacity(uint16_t *out_cap, uint16_t *b_cap)
{
    if (out_cap != NULL) { *out_cap = (uint16_t)TB32_TAP_OUT_N; }
    if (b_cap != NULL)   { *b_cap = (uint16_t)TB32_TAP_B_N; }
}

/* Which sides have finished, for a console that has to say what it is still waiting for.
 * Returns false when no capture is armed at all. */
bool asrc_thirdband_96_to_32_tap_side_done(bool *out_done, bool *b_done)
{
    if (out_done != NULL) { *out_done = (s_tb32_tap_out_done != 0u); }
    if (b_done != NULL)   { *b_done = (s_tb32_tap_b_done != 0u); }
    return (s_tb32_tap_state != 0u);
}

/* THE LEG-B TAP -- the far end of the digital chain, in the same armed window.
 *
 * `block` is what audio_app_asrc_pull_ab() just produced, i.e. the block the TDM hands to
 * WM8904-B.  Between it and the stage output sit the push, the AB FIFO, the servo and the rear
 * polyphase, so ONE arm measures that whole span rather than needing a second image and a
 * second application of the tone.  Same rate (leg B boots at 32 kHz), same
 * length and same channel as the stage tap, deliberately: it makes the two windows
 * comparable bin for bin.
 *
 * No-op unless `*av05` armed it, and it can never write past the buffer -- an instrument on
 * the audio path must not be able to become the fault it is looking for. */
void asrc_thirdband_96_to_32_tap_legb(const int32_t *block, uint32_t frames, uint32_t stride)
{
    /* s_tb32_armed FIRST.  TB32_TAP_B lives inside s_q31_hist, which belongs to the composed
     * chain whenever the third band is retired, and this runs from leg B's interrupt -- a
     * domain that knows nothing about the switch.  Writing here while composed is filtering
     * would corrupt that chain's history, so the retire path cancels the tap and this refuses
     * even if a cancel were ever missed. */
    if ((block == NULL) || (s_tb32_armed == 0u) || (s_tb32_tap_state != 2u)) { return; }
    if (s_tb32_tap_b_done != 0u) { return; }
    uint16_t w = s_tb32_tap_b_n;
    /* Same clamp as the other side, for the same reason: the loop bound below is only a bound
     * while the counter it starts from is in range. */
    if (w > (uint16_t)TB32_TAP_B_N) { w = (uint16_t)TB32_TAP_B_N; }
    for (uint32_t f = 0u; (f < frames) && (w < TB32_TAP_B_N); ++f)
    {
        TB32_TAP_B[w] = block[(size_t)f * (size_t)stride];
        ++w;
    }
    s_tb32_tap_b_n = w;
    /* This side fills to the last word: leg B hands over whatever the TDM asked for, so it
     * has no block granularity to reserve room for. */
    if (w >= TB32_TAP_B_N)
    {
        s_tb32_tap_b_done = 1u;
        tb32_tap_promote();
    }
}

/* One word of the capture, so the console can stream it without owning the geometry.
 * `which` 0 = the stage's OUTPUT at 32 kHz, 1 = leg B's OUTPUT block at 32 kHz. */
int32_t asrc_thirdband_96_to_32_tap_word(uint8_t which, uint16_t i)
{
    /* Bounded by the BUFFER as well as by the count, so a reader can never be walked off the
     * end of a capture by a count that should be impossible. */
    if (which == 0u)
    {
        return ((i < s_tb32_tap_out_n) && (i < (uint16_t)TB32_TAP_OUT_N))
                   ? TB32_TAP_OUT[i] : 0;
    }
    return ((i < s_tb32_tap_b_n) && (i < (uint16_t)TB32_TAP_B_N)) ? TB32_TAP_B[i] : 0;
}

/* ---- the dynamic check ---------------------------------------------------------------- */

#include "asrc_thirdband_96_to_32_vector.inc"

/* Drive the WHOLE STAGE with a real waveform and compare against host-computed values.
 *
 * This is the check that did not exist, and its absence is why a dynamic fault reached the
 * listening test: the kernel bench re-read one input buffer every frame, and *av03's DC and
 * 32 kHz-null cases each hold a branch's input CONSTANT, so `x1 == x` always held and the
 * allpass state never moved in any of them.
 *
 * It goes through tb32_process() rather than calling the kernel, so the gather, the headroom
 * shift, the 0..2 frame carry across blocks and the 1/3 are all in scope.  The input is one
 * channel at stride 1 with mask 0, which makes every one of the ASRC_CH channels see the
 * same sample -- so the run also proves the channels do not share state, which the earlier
 * checks could not see either (they drove every channel identically AND at DC).
 *
 * No stack: the outputs land in this stage's own arena scratch and are compared in place.
 * Run with audio stopped (`*ts`); it resets the state and the counters afterwards. */
bool asrc_thirdband_96_to_32_vector_check(asrc_thirdband_vector_t *r)
{
    asrc_thirdband_vector_t v;
    size_t idx = 0u;

    v.frames = 0u;
    v.expect = (uint16_t)TB32_VEC_OUT_FRAMES;
    v.first_bad = -1;
    v.got = 0;
    v.want = 0;
    v.bad_channel = -1;
    v.refused = false;

    tb32_load_coeff();
    tb32_reset_state();

    for (uint16_t b = 0u; b < (uint16_t)(TB32_VEC_IN_FRAMES / TB32_VEC_BLOCK); ++b)
    {
        size_t n = 0u;
        /* stride 1 / one source channel: in_mask 0 makes src[c & 0] every channel's sample. */
        if (!tb32_process(&s_thirdband_vec_in[(size_t)b * TB32_VEC_BLOCK],
                          (size_t)TB32_VEC_BLOCK, 1u, 0u,
                          TB32_OUT, (size_t)ASRC_DEC_Q31_CH, &n))
        {
            v.refused = true;
            break;
        }
        for (size_t f = 0u; f < n; ++f)
        {
            const int32_t *row = TB32_OUT + (f * (size_t)ASRC_DEC_Q31_CH);
            if ((idx < TB32_VEC_OUT_FRAMES) && (v.first_bad < 0) &&
                (row[0] != s_thirdband_vec_out[idx]))
            {
                v.first_bad = (int16_t)idx;
                v.got = row[0];
                v.want = s_thirdband_vec_out[idx];
            }
            for (uint8_t c = 1u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                if ((row[c] != row[0]) && (v.bad_channel < 0))
                {
                    v.bad_channel = (int8_t)c;
                }
            }
            ++idx;
        }
    }
    v.frames = (uint16_t)idx;
    v.pass = (!v.refused) && (v.first_bad < 0) && (v.bad_channel < 0) &&
             (idx == (size_t)TB32_VEC_OUT_FRAMES);

    tb32_reset_state();
    asrc_thirdband_96_to_32_stats_clear();
    if (r != NULL) { *r = v; }
    return v.pass;
}

/* ---- arming ------------------------------------------------------------------------- */

/* Arm or retire.  Called ONLY from the rate-change hook, with the transport stopped and
 * between mute and unmute -- the same single point the Full-IIR trial stage uses, and for
 * the same reason: sixteen sections of recursive state may be cleared there and nowhere
 * else.  Arming loads the coefficients over the composed chain's, which is why retiring
 * has to rebuild them. */
void asrc_thirdband_96_to_32_arm(bool on)
{
    /* CANCEL THE TAP ON EVERY TRANSITION, in both directions and before anything else.  The
     * capture buffers overlay s_q31_hist, so a tap left filling across a retire would have leg
     * B's interrupt writing into the COMPOSED chain's history, and a tap left over from a
     * previous arming would splice two windows.  Clearing s_tb32_armed alone is not enough:
     * that is what the block path checks, and leg B's tap does not go through the block
     * path. */
    tb32_tap_cancel();
    if (on)
    {
        if ((s_tb32_eligible == 0u) || (s_tb32_mode == 0u))
        {
            s_tb32_armed = 0u;
            return;
        }
        tb32_load_coeff();
        tb32_reset_state();
        asrc_thirdband_96_to_32_stats_clear();
        s_tb32_armed = 1u;
        return;
    }
    s_tb32_armed = 0u;
}

/* Record which front end is wanted.  It does NOT switch anything by itself, and must not:
 * the two front ends share one arena, so the one being switched away from has to be rebuilt
 * from scratch, and the only place either may be built is the mute-bounded rate-change hook.
 * The console therefore re-applies leg A's own rate after calling this -- the same mechanism
 * `*aj` uses for the 48 -> 32 trial stage -- and the arm hook builds whichever this names.
 *
 * Retiring is free of extra code because asrc_decimator_q31_init() goes through the arm
 * prologue (dec_q31_arm_begin(), which clears the history arena) and reloads the composed
 * chain's coefficients over this stage's on its way through. */
void asrc_thirdband_96_to_32_set_mode(asrc_thirdband_mode_t mode)
{
    s_tb32_mode = (mode == ASRC_THIRDBAND_MODE_ON) ? 1u : 0u;
}
