/*
 * asrc_decimator_q31.inc -- 16-channel Q31 Nyquist front end.
 *
 * THIS FILE IS CODE, not a generated table.  It is #included at the end of
 * asrc_decimator_48_to_8.c rather than compiled on its own because the
 * coefficient tables it converts are `static const` in that translation unit:
 * including it here shares the ONE copy of every coefficient set (and the one
 * set of CRC32 _Static_asserts that pins them) instead of duplicating ~4 KB of
 * float tables in ROM for a second translation unit.
 *
 * What this is.  The float front end above computes 2 channels with a mirrored
 * history and a symmetric-window FIR.  This computes ASRC_CH channels in Q31
 * with the hardware dual-fetch MAC kernel, which needs its two operands in
 * DIFFERENT data spaces: coefficients in X, history in Y.  Measured on this
 * silicon ([internal] report_ak512_fir_kernel_measured_2026-08-21.md):
 *
 *     coefficients in X RAM, history in Y, Y modulo only : 1.052 cycles/MAC
 *     coefficients in PROGRAM FLASH, history in Y        : 3.063 cycles/MAC
 *
 * So the coefficient MASTERS stay in program flash (they are the float tables
 * above) and init() converts the selected chain's masters into an X RAM working
 * buffer.  Every MAC executed from the audio ISR reads X RAM.  Nothing on the
 * audio path touches flash, and nothing on the audio path converts.
 *
 * Y-modulo-only is a SAFETY property, not an optimisation.  MODCON, XMODSRT/
 * XMODEND and YMODSRT/YMODEND are not part of the per-IPL register context
 * (DS70005591C Table 4-2), so whatever this kernel programs is live in every
 * context that can preempt it.  An X modulo would fold a preempting handler's
 * ordinary mov.l [w0] into this kernel's ring; the Y AGU is used only by DSP
 * MAC-class instructions (4.3.16), so with X modulo off the residual condition
 * is "no preempting handler executes a MAC-class instruction" -- which is
 * checkable on the linked image rather than promised in a comment.  Dropping X
 * modulo costs one coefficient-pointer reload per output: measured +1.0 %.
 *
 * Ring geometry -- why 190 samples per channel, and why the batch size varies.
 * The kernel produces `outputs` outputs from one call by stepping the window by
 * `decim` each time, so all of them must be live in the ring at once:
 *
 *     taps + (outputs - 1) * decim <= ring_samples
 *
 * The budget is 190 samples/channel (12,160 B for 16 channels), fixed because
 * /6 is two stages of 43 + 147 taps and a ring can never be shorter than its
 * tap count.  /6 therefore spends the whole budget on taps and takes one output
 * per call; every shallower chain has slack and gets ONE call per channel per
 * block, which is what keeps the ~70-cycle per-call setup from being multiplied
 * by the output count.  The table in init() is that allocation and the
 * _Static_asserts below are what stop it from drifting.
 */

/* Compute width.  ASRC_CH is the resampler's internal width (16 in the shipping
 * 48 kHz BIDIR build, 8 where the 96 kHz block window is half as long), and the
 * front end matches it so the two halves of the path carry the same channel
 * count.  Defined here, not in the header: this file is included at the END of
 * the .c, by which point app_specific_config_defs.h has pulled in
 * apps/asrc/asrc_app_config.h.  The header is included first and would see
 * nothing. */
#if defined(ASRC_CH)
#  define ASRC_DEC_Q31_CH  ASRC_CH
#else
#  define ASRC_DEC_Q31_CH  (2u)
#endif

/* The kernel.  Declared here rather than in a header because it is the only
 * caller in the application; the bench declares its own copy for the same
 * reason.  The signature is fixed by the .s file. */
extern int32_t *fir_ring_q31_ymod_yonly_block(
    const int32_t *coeff,       /* w0  taps entries, X space, no modulo */
    const int32_t *hist,        /* w1  Y space, oldest sample of window */
    uint32_t       taps,        /* w2  >= 2                             */
    int32_t       *out,         /* w3  outputs written here             */
    uint32_t       outputs,     /* w4  >= 1                             */
    uint32_t       decim_bytes, /* w5  decimation factor * 4            */
    const int32_t *ring,        /* w6  YMODSRT (ring base)              */
    uint32_t       ring_bytes); /* w7  ring length in bytes             */

#define DEC_Q31_HIST_PER_CH   (209u)   /* samples/channel -- the fixed budget */

/*
 * The /6 cascade's two rings.  They are deliberately wider than their tap
 * counts: the kernel emits `b` outputs from ONE call while
 * taps + (b-1)*decim <= ring, so a ring equal to the taps pins b at 1 and pays
 * one call's setup per output -- 128 calls per 16-frame block at 16 channels.
 * Sized for the whole block instead.  Stage 1 sees 16 inputs and a phase in
 * [0,2], so at most (16+2)/3 = 6 outputs; stage 2 then sees at most 6 inputs and
 * a phase in [0,1], so at most 3.  32 calls per block.
 *
 * Nothing else moves: /2, /3 and 2/3 are single-ring, and /4 (27/129 taps inside
 * 43/147) already batches, so those rows keep their current allocation.
 */
#define DEC_Q31_R6_S1_OUT     (6u)     /* stage-1 outputs per 16-frame block */
#define DEC_Q31_R6_S2_OUT     (3u)     /* stage-2 outputs per 16-frame block */
#define DEC_Q31_R6_S1_RING    (58u)    /* 43 + 5*3  */
#define DEC_Q31_R6_S2_RING    (151u)   /* 147 + 2*2 */
#define DEC_Q31_COEFF_MAX     (211u)   /* entries in the X working buffer     */
#define DEC_Q31_MAX_BATCH     (16u)    /* outputs per kernel call (scratch)   */
#define DEC_Q31_MAX_STAGES    (2u)
#define DEC_Q31_MAX_MID       (8u)     /* stage-1 output frames per block     */

/*
 * The 2/3 rational chain (48 -> 32 kHz AUDIO MODE), and why its numbers are what
 * they are.  Everything below is derived, not chosen -- the _Static_asserts after
 * the state struct re-derive each one so a changed block size cannot quietly
 * leave a constant behind.
 *
 * SCHEDULE.  Output k of an L=2/M=3 resampler is
 *
 *     y[2q]   = sum(h[2i]   * x[3q   - i]), i = 0..48   (49 taps, newest x[3q])
 *     y[2q+1] = sum(h[2i+1] * x[3q+1 - i]), i = 0..47   (48 taps, newest x[3q+1])
 *
 * so output k needs input floor(3k/2) and nothing newer.  Only the two phases
 * that exist are computed: there is no zero-stuffed 96 kHz stream anywhere in
 * here, and no MAC is spent on a coefficient that would meet a stuffed zero.
 * Each row reads CONSECUTIVE inputs and successive outputs of the SAME row hop
 * M=3 inputs, which is exactly the (taps, decim) shape the existing block kernel
 * already implements -- hence no new assembler, and two calls per channel per
 * block instead of one call per output.
 *
 * BLOCK OUTPUT COUNT.  Outputs available after n inputs is the number of k with
 * floor(3k/2) <= n-1, i.e. floor((n-1)/3) + floor((n-2)/3) + 2.  At n = 16 that
 * is 11, and the steady-state block sequence is 11, 11, 10 (period 3, average
 * 32/3).  11 -- not 10 or 11-on-average -- is what every buffer must hold.
 *
 * RING.  One kernel call per row needs every window of the block live at once.
 * The oldest sample any of them reads is (block frames) + (longest row - 1)
 * behind the newest, = 16 + 48 = 64.  That is the whole ring, exactly, and it is
 * also exactly what a 6-output batch of the 49-tap row spans (49 + 5*3 = 64), so
 * the two independent bounds agree at 64 rather than one of them being slack.
 */
#define DEC_Q31_R23_RING      (64u)    /* samples/channel, shared by both rows */
#define DEC_Q31_R23_MAX_IN    (16u)    /* input frames per call (the TDM block) */
#define DEC_Q31_R23_MAX_OUT   (11u)    /* output frames per call, worst case    */
#define DEC_Q31_R23_MAX_ROW_BATCH (6u) /* outputs of ONE row in one call        */

/* Q31 history.  A DECLARED object in Y space -- space(ymemory) -- not the
 * "address the top of the stack region directly" trick the bench used.  That
 * trick is right for a measurement device and forbidden here: shipping storage
 * must be visible to the linker, to the map and to the layout gate.  4-byte
 * alignment is all the Y AGU needs for a non-power-of-two modulo (the bench's
 * M6 case: 11/11 pass at a 760 B ring). */
static int32_t s_q31_hist[ASRC_DEC_Q31_CH][DEC_Q31_HIST_PER_CH]
    __attribute__((space(ymemory), aligned(4)));

/* Active coefficients, in X, written only by init().  space(xmemory) is what
 * forces X: left to the best-fit allocator these land wherever there is room,
 * which on this application means Y -- and then every MAC silently costs 2.0
 * cycles instead of 1.05, with no other symptom. */
static int32_t s_q31_coeff[DEC_Q31_COEFF_MAX]
    __attribute__((space(xmemory), aligned(4)));

/* Stage-1 outputs at full Q31 precision -- no s24 round trip between stages.
 * Ordinary space: read and written by C (MCU-class), never by the MAC. */
static int32_t s_q31_mid[DEC_Q31_MAX_MID * ASRC_DEC_Q31_CH];

typedef struct
{
    uint16_t taps;
    uint16_t ring;        /* samples/channel                              */
    uint16_t ring_off;    /* offset into the per-channel 190-sample arena  */
    uint16_t coeff_off;   /* offset into s_q31_coeff                       */
    uint8_t  decim;
} dec_q31_stage_t;

static struct
{
    /* For the integer chains these are CASCADE stages, 1 or 2 of them.  For the
     * 2/3 rational chain they are the two PHASE ROWS of one filter: same ring
     * (ring_off 0 for both), same decim 3, different taps and coefficients.
     * `num` is what tells the two apart -- see asrc_decimator_q31_init(). */
    dec_q31_stage_t stage[DEC_Q31_MAX_STAGES];
    uint16_t wr[DEC_Q31_MAX_STAGES];
    uint8_t  phase[DEC_Q31_MAX_STAGES];
    uint8_t  stages;
    uint8_t  channels;
    uint8_t  num;
    uint8_t  den;
    /* Rational schedule.  `r23_d` is (newest input the next output needs) minus
     * (inputs pushed so far), so an output is due exactly while it is <= -1; it
     * lives in [-1, +1] between blocks.  `r23_par` is that output's phase row. */
    int8_t   r23_d;
    uint8_t  r23_par;
} s_q31;

_Static_assert(ASRC_DECIMATOR_48_TO_24_TAPS <= DEC_Q31_HIST_PER_CH, "/2 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_16_TAPS <= DEC_Q31_HIST_PER_CH, "/3 ring");
_Static_assert((DEC_Q31_R6_S1_RING + DEC_Q31_R6_S2_RING) == DEC_Q31_HIST_PER_CH,
               "/6 spends the whole budget");
_Static_assert(ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS <= DEC_Q31_R6_S1_RING, "/6 stage 1 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS <= DEC_Q31_R6_S2_RING, "/6 stage 2 ring");
/* Re-derive both rings from the batch bound, so a narrowed ring fails here
 * instead of quietly going back to one kernel call per output. */
_Static_assert(DEC_Q31_R6_S1_RING >=
                   (ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS +
                    ((DEC_Q31_R6_S1_OUT - 1u) * 3u)),
               "/6 stage 1 ring must span a whole block's batch");
_Static_assert(DEC_Q31_R6_S2_RING >=
                   (ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS +
                    ((DEC_Q31_R6_S2_OUT - 1u) * 2u)),
               "/6 stage 2 ring must span a whole block's batch");
_Static_assert(DEC_Q31_R6_S1_OUT <= DEC_Q31_MAX_MID,
               "/6 stage-1 batch must fit the inter-stage buffer");
_Static_assert((DEC_Q31_R6_S1_OUT <= DEC_Q31_MAX_BATCH) &&
                   (DEC_Q31_R6_S2_OUT <= DEC_Q31_MAX_BATCH),
               "/6 batches must fit the per-channel kernel scratch");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS <= 43u, "/4 stage 1 ring");
_Static_assert(ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS <= 147u, "/4 stage 2 ring");
_Static_assert((ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS +
                ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS) <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for the deepest chain");
/* The 2/3 rational chain.  Each of these re-derives a constant above from the
 * block size, so DEC_Q31_R23_* cannot be edited into agreement with a wrong
 * ring: change DEC_Q31_R23_MAX_IN and the arithmetic here is what fails. */
_Static_assert(DEC_Q31_R23_MAX_OUT ==
                   ((((DEC_Q31_R23_MAX_IN - 1u) / ASRC_DECIMATOR_48_TO_32_M) +
                     ((DEC_Q31_R23_MAX_IN - 2u) / ASRC_DECIMATOR_48_TO_32_M)) + 2u),
               "2/3 worst-case output count is floor((n-1)/3)+floor((n-2)/3)+2");
/* Same count as the PUBLIC macro the audio path sizes its scratch buffer with, so the front
 * end and its caller can never disagree about how many frames a 16-frame block yields. */
_Static_assert(DEC_Q31_R23_MAX_OUT ==
                   ASRC_DECIMATOR_48_TO_32_OUT_FRAMES(DEC_Q31_R23_MAX_IN),
               "front end and asrc_decimator_48_to_8.h must agree on the 2/3 frame count");
_Static_assert(DEC_Q31_R23_MAX_ROW_BATCH == ((DEC_Q31_R23_MAX_OUT + 1u) / 2u),
               "the two rows alternate, so one row takes at most ceil(outputs/2)");
_Static_assert(DEC_Q31_R23_MAX_ROW_BATCH <= DEC_Q31_MAX_BATCH,
               "a row batch must fit the per-channel kernel scratch");
_Static_assert(ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS >= ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
               "phase 0 is assumed to be the longer row by the ring bounds below");
_Static_assert(DEC_Q31_R23_RING >=
                   (DEC_Q31_R23_MAX_IN + ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS - 1u),
               "2/3 ring must span a whole block plus the longest row's window");
_Static_assert(DEC_Q31_R23_RING >=
                   (ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                    ((DEC_Q31_R23_MAX_ROW_BATCH - 1u) * ASRC_DECIMATOR_48_TO_32_M)),
               "2/3 ring must span the widest single-row batch");
_Static_assert(DEC_Q31_R23_RING <= DEC_Q31_HIST_PER_CH,
               "2/3 ring must fit the per-channel history arena");
_Static_assert((ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS) <= DEC_Q31_COEFF_MAX,
               "X coefficient buffer too small for both 2/3 phase rows");
_Static_assert(DEC_Q31_MAX_STAGES >= 2u, "the 2/3 chain keeps one descriptor per row");
_Static_assert(ASRC_DEC_Q31_CH >= 2u, "front end computes at least the real pair");
_Static_assert((ASRC_DEC_Q31_CH & (ASRC_DEC_Q31_CH - 1u)) == 0u,
               "channel repeat is a mask, so the width must be a power of two");

/* Rounded float -> Q31.  Runs in init(), never on the audio path.  Every
 * coefficient of a unity-gain decimation lowpass is well inside +/-1, so the
 * clamps guard a future table rather than an expected path. */
static int32_t dec_q31_from_float(float c)
{
    const float v = c * 2147483648.0f;
    if (v >= 2147483647.0f)  { return (int32_t)0x7FFFFFFF; }
    if (v <= -2147483648.0f) { return (int32_t)0x80000000; }
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/*
 * Q31 -> s24-left.  This is a MASK, and that is the whole saturation story:
 * sacr.l has already saturated the 40-bit accumulator into Q31, so the value
 * cannot exceed the s24-left range once its eight sub-LSBs are dropped.
 * Masking them off is arithmetically identical to (v >> 8) << 8 for negatives
 * too (the shift is arithmetic), i.e. it truncates toward -inf -- matching
 * float_to_s24_left() above, which also truncates.
 */
static inline int32_t dec_q31_to_s24_left(int32_t v)
{
    return (int32_t)((uint32_t)v & 0xFFFFFF00u);
}

static void dec_q31_load_coeff(uint16_t off, const float *src, uint16_t taps)
{
    for (uint16_t i = 0u; i < taps; ++i)
    {
        s_q31_coeff[off + i] = dec_q31_from_float(src[i]);
    }
}

static void dec_q31_stage_set(uint8_t s, uint16_t taps, uint8_t decim,
                              uint16_t ring, uint16_t ring_off,
                              uint16_t coeff_off, const float *coeff)
{
    s_q31.stage[s].taps      = taps;
    s_q31.stage[s].decim     = decim;
    s_q31.stage[s].ring      = ring;
    s_q31.stage[s].ring_off  = ring_off;
    s_q31.stage[s].coeff_off = coeff_off;
    s_q31.wr[s]    = 0u;
    s_q31.phase[s] = 0u;
    dec_q31_load_coeff(coeff_off, coeff, taps);
}

bool asrc_decimator_q31_init(uint32_t num, uint32_t den, uint8_t channels,
                             asrc_decimator_48_to_24_variant_t v24,
                             asrc_decimator_48_to_12_variant_t v12)
{
    if ((channels == 0u) || (channels > ASRC_DEC_Q31_CH))
    {
        return false;
    }

    memset(&s_q31, 0, sizeof(s_q31));
    memset(s_q31_hist, 0, sizeof(s_q31_hist));
    s_q31.channels = channels;
    s_q31.num      = (uint8_t)num;
    s_q31.den      = (uint8_t)den;

    /* The rational chain is selected by the RATIO, before den is looked at: 2/3
     * and 1/3 both have den == 3 and are different filters (32 kHz output against
     * 16 kHz output).  Anything with num != 1 that is not exactly 2/3 is refused
     * rather than approximated. */
    if (num == ASRC_DECIMATOR_48_TO_32_L)
    {
        if (den != ASRC_DECIMATOR_48_TO_32_M)
        {
            return false;
        }
        /* Two PHASE ROWS over ONE shared ring, not a cascade: both descriptors
         * carry ring_off 0 and decim M, and only s_q31.wr[0] is used. */
        s_q31.stages = 2u;
        dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS,
                          ASRC_DECIMATOR_48_TO_32_M,
                          DEC_Q31_R23_RING, 0u, 0u, s_48_to_32_phase0_coeff);
        dec_q31_stage_set(1u, ASRC_DECIMATOR_48_TO_32_PHASE1_TAPS,
                          ASRC_DECIMATOR_48_TO_32_M,
                          DEC_Q31_R23_RING, 0u,
                          ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS,
                          s_48_to_32_phase1_coeff);
        /* k = 0 needs input floor(0) = 0, and nothing has been pushed, so the
         * first output is due as soon as one frame arrives. */
        s_q31.r23_d   = 0;
        s_q31.r23_par = 0u;
        return true;
    }
    if (num != 1u)
    {
        return false;
    }

    switch (den)
    {
    case 2u:
        s_q31.stages = 1u;
        dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_24_TAPS, 2u,
                          DEC_Q31_HIST_PER_CH, 0u, 0u,
                          (v24 == ASRC_DECIMATOR_48_TO_24_FOR_22050)
                              ? s_48_to_24_out22k_coeff : s_48_to_24_coeff);
        return true;

    case 3u:
        s_q31.stages = 1u;
        dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_16_TAPS, 3u,
                          DEC_Q31_HIST_PER_CH, 0u, 0u, s_48_to_16_coeff);
        return true;

    case 4u:
        s_q31.stages = 2u;
        if (v12 == ASRC_DECIMATOR_48_TO_12_FOR_12000)
        {
            dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS, 2u,
                              43u, 0u, 0u, s_48_to_12_out12k_stage1_coeff);
            dec_q31_stage_set(1u, ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS, 2u,
                              147u, 43u, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS,
                              s_48_to_12_out12k_stage2_coeff);
        }
        else
        {
            dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS, 2u,
                              43u, 0u, 0u, s_48_to_12_stage1_coeff);
            dec_q31_stage_set(1u, ASRC_DECIMATOR_48_TO_12_STAGE2_TAPS, 2u,
                              147u, 43u, ASRC_DECIMATOR_48_TO_12_STAGE1_TAPS,
                              s_48_to_12_stage2_coeff);
        }
        return true;

    case 6u:
        s_q31.stages = 2u;
        dec_q31_stage_set(0u, ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS, 3u,
                          DEC_Q31_R6_S1_RING, 0u, 0u, s_stage1_coeff);
        dec_q31_stage_set(1u, ASRC_DECIMATOR_48_TO_8_STAGE2_TAPS, 2u,
                          DEC_Q31_R6_S2_RING, DEC_Q31_R6_S1_RING,
                          ASRC_DECIMATOR_48_TO_8_STAGE1_TAPS,
                          s_stage2_coeff);
        return true;

    default:
        return false;
    }
}

/*
 * Outputs the 2/3 chain will emit if `in_frames` more inputs arrive, at the
 * current schedule state.  Pure: this is also what output_frames() answers with,
 * so the all-or-nothing capacity check and the run can never disagree.
 */
static size_t dec_q31_r23_count(size_t in_frames)
{
    if (in_frames > DEC_Q31_R23_MAX_IN)
    {
        /* Outside the ring's domain -- the walk below would be valid arithmetic
         * for a schedule the ring cannot hold.  Report more frames than this
         * chain can ever emit so the capacity check refuses; process() refuses
         * the oversized block outright as well. */
        return (size_t)DEC_Q31_R23_MAX_OUT + 1u;
    }
    int16_t d = (int16_t)((int16_t)s_q31.r23_d - (int16_t)in_frames);
    uint8_t p = s_q31.r23_par;
    size_t  n = 0u;
    while (d <= -1)
    {
        ++n;
        d = (int16_t)(d + ((p == 0u) ? 1 : 2));
        p ^= 1u;
    }
    return n;
}

/*
 * One block of the 2/3 rational chain.
 *
 * Push the whole block into the shared ring first, then issue ONE kernel call per
 * phase row per channel.  That ordering is why the ring is 64 and not 49: every
 * window the block will read has to still be there when the calls happen.  The
 * two rows' outputs interleave -- row `p0` owns the even output slots of this
 * block and the other row the odd ones -- so nothing has to be re-sorted after.
 */
static size_t dec_q31_r23_run(const int32_t *in, size_t in_frames,
                              size_t in_stride, uint8_t in_ch_mask,
                              int32_t *out, size_t out_stride)
{
    const uint8_t  nch = s_q31.channels;
    const uint16_t R   = DEC_Q31_R23_RING;

    for (size_t i = 0u; i < in_frames; ++i)
    {
        const int32_t *const f = in + (i * in_stride);
        const uint16_t w = s_q31.wr[0];
        for (uint8_t c = 0u; c < nch; ++c)
        {
            s_q31_hist[c][w] = f[c & in_ch_mask];
        }
        s_q31.wr[0] = (uint16_t)(((w + 1u) == R) ? 0u : (w + 1u));
    }

    /* Walk the schedule once, recording per row how many outputs it owes and
     * where its FIRST window ends (as a distance behind the write pointer). */
    int16_t       d  = (int16_t)((int16_t)s_q31.r23_d - (int16_t)in_frames);
    const uint8_t p0 = s_q31.r23_par;
    uint8_t       p  = p0;
    uint16_t      rows[2]  = { 0u, 0u };
    int16_t       first[2] = { 0, 0 };
    size_t        produced = 0u;
    while (d <= -1)
    {
        if (rows[p] == 0u) { first[p] = d; }
        rows[p]++;
        ++produced;
        d = (int16_t)(d + ((p == 0u) ? 1 : 2));
        p ^= 1u;
    }
    s_q31.r23_d   = (int8_t)d;
    s_q31.r23_par = p;

    for (uint8_t r = 0u; r < 2u; ++r)
    {
        if (rows[r] == 0u)
        {
            continue;
        }
        const dec_q31_stage_t *const st = &s_q31.stage[r];
        /* first[r] is negative: the row's first output has its newest sample
         * -first[r] frames behind the write pointer, and its window reaches
         * taps-1 further back still. */
        const uint16_t back  = (uint16_t)((uint16_t)(-first[r]) +
                                          (uint16_t)(st->taps - 1u));
        const uint16_t start = (uint16_t)(((uint32_t)s_q31.wr[0] + R - back) % R);
        const int32_t *const coeff = &s_q31_coeff[st->coeff_off];
        /* Row r owns output slots (r ^ p0), (r ^ p0) + 2, ... of this block. */
        const size_t slot0 = (size_t)((r ^ p0) & 1u);

        for (uint8_t c = 0u; c < nch; ++c)
        {
            int32_t tmp[DEC_Q31_R23_MAX_ROW_BATCH];
            int32_t *const ring = &s_q31_hist[c][st->ring_off];

            (void)fir_ring_q31_ymod_yonly_block(coeff, ring + start, st->taps,
                                                tmp, (uint32_t)rows[r],
                                                (uint32_t)st->decim * 4u,
                                                ring, (uint32_t)R * 4u);
            for (size_t j = 0u; j < (size_t)rows[r]; ++j)
            {
                out[(((j * 2u) + slot0) * out_stride) + c] =
                    dec_q31_to_s24_left(tmp[j]);
            }
        }
    }
    return produced;
}

size_t asrc_decimator_q31_output_frames(size_t input_frames)
{
    if (s_q31.stages == 0u)
    {
        return 0u;
    }
    if (s_q31.num == ASRC_DECIMATOR_48_TO_32_L)
    {
        return dec_q31_r23_count(input_frames);
    }
    size_t n = ((size_t)s_q31.phase[0] + input_frames) / s_q31.stage[0].decim;
    if (s_q31.stages > 1u)
    {
        n = ((size_t)s_q31.phase[1] + n) / s_q31.stage[1].decim;
    }
    return n;
}

/*
 * One stage.  `out` receives full-precision Q31 when this is not the last
 * stage; the last stage masks to s24-left on the way out.
 *
 * Batching: `b` outputs come out of ONE kernel call, bounded by the ring
 * (taps + (b-1)*decim <= ring), by the scratch and by the input on hand.
 * Everything else here is the ring bookkeeping the kernel does not do: the AGU
 * wraps the READ window, the writes below are plain C.
 */
static size_t dec_q31_stage_run(uint8_t s,
                                const int32_t *in, size_t in_frames,
                                size_t in_stride, uint8_t in_ch_mask,
                                int32_t *out, size_t out_stride,
                                bool last)
{
    const dec_q31_stage_t *const st = &s_q31.stage[s];
    const uint16_t T   = st->taps;
    const uint16_t R   = st->ring;
    const uint8_t  D   = st->decim;
    const uint8_t  nch = s_q31.channels;
    const int32_t *const coeff = &s_q31_coeff[st->coeff_off];

    uint16_t bmax = (uint16_t)(1u + (uint16_t)((R - T) / D));
    if (bmax > DEC_Q31_MAX_BATCH) { bmax = DEC_Q31_MAX_BATCH; }

    size_t produced = 0u;
    size_t i = 0u;
    while (i < in_frames)
    {
        const size_t need  = (size_t)D - (size_t)s_q31.phase[s];
        const size_t avail = in_frames - i;

        if (avail < need)
        {
            /* Not enough for another output.  Absorb, and leave the phase. */
            for (size_t k = 0u; k < avail; ++k, ++i)
            {
                const int32_t *const f = in + (i * in_stride);
                const uint16_t w = s_q31.wr[s];
                for (uint8_t c = 0u; c < nch; ++c)
                {
                    s_q31_hist[c][st->ring_off + w] = f[c & in_ch_mask];
                }
                s_q31.wr[s] = (uint16_t)(((w + 1u) == R) ? 0u : (w + 1u));
            }
            s_q31.phase[s] = (uint8_t)((size_t)s_q31.phase[s] + avail);
            break;
        }

        size_t b = 1u + ((avail - need) / D);
        if (b > bmax) { b = bmax; }
        const size_t push_n = need + ((b - 1u) * D);

        for (size_t k = 0u; k < push_n; ++k, ++i)
        {
            const int32_t *const f = in + (i * in_stride);
            const uint16_t w = s_q31.wr[s];
            for (uint8_t c = 0u; c < nch; ++c)
            {
                s_q31_hist[c][st->ring_off + w] = f[c & in_ch_mask];
            }
            s_q31.wr[s] = (uint16_t)(((w + 1u) == R) ? 0u : (w + 1u));
        }
        s_q31.phase[s] = 0u;

        /* Oldest sample of the FIRST output's window.  The batch spans
         * T + (b-1)*D samples and ends at wr-1, so the start is that far back
         * -- modulo the ring by hand, because only the AGU wraps. */
        const uint16_t span = (uint16_t)(T + ((b - 1u) * D));
        const uint16_t w0   = (uint16_t)(((uint32_t)s_q31.wr[s] + R - span) % R);

        for (uint8_t c = 0u; c < nch; ++c)
        {
            int32_t tmp[DEC_Q31_MAX_BATCH];
            int32_t *const ring = &s_q31_hist[c][st->ring_off];

            (void)fir_ring_q31_ymod_yonly_block(coeff, ring + w0, T,
                                                tmp, (uint32_t)b,
                                                (uint32_t)D * 4u,
                                                ring, (uint32_t)R * 4u);
            if (last)
            {
                for (size_t o = 0u; o < b; ++o)
                {
                    out[((produced + o) * out_stride) + c] =
                        dec_q31_to_s24_left(tmp[o]);
                }
            }
            else
            {
                for (size_t o = 0u; o < b; ++o)
                {
                    out[((produced + o) * out_stride) + c] = tmp[o];
                }
            }
        }
        produced += b;
    }
    return produced;
}

/*
 * `input_channels` is how many REAL channels the source frame carries -- 2 for a
 * TDM/I2S block, whatever the slot count is.  The front end computes ASRC_CH of
 * them by repeating the source channels, which is the same arrangement the
 * resampler behind it already uses (asrc_push reads p[c & 1u] and emits only
 * channels 0/1).  So the extra channels are the WORKLOAD, not data: the audio
 * that reaches the codec is still channels 0 and 1, computed by exactly the
 * kernel and coefficients the other fourteen exercise.  Must be a power of two,
 * because the repeat is a mask -- a modulo per sample would cost more than the
 * MAC it feeds.
 */
bool asrc_decimator_q31_process_s24_left(const int32_t *input,
                                         size_t input_frames,
                                         size_t input_stride,
                                         uint8_t input_channels,
                                         int32_t *output,
                                         size_t output_capacity_frames,
                                         size_t output_stride,
                                         size_t *output_frames)
{
    if (output_frames != NULL) { *output_frames = 0u; }
    if ((s_q31.stages == 0u) || (output_frames == NULL) ||
        ((input_frames != 0u) && (input == NULL)) ||
        (input_channels == 0u) ||
        ((input_channels & (uint8_t)(input_channels - 1u)) != 0u) ||
        (input_stride < input_channels) ||
        (output_stride < s_q31.channels))
    {
        return false;
    }
    if (asrc_decimator_q31_output_frames(input_frames) > output_capacity_frames)
    {
        return false;
    }

    const uint8_t in_mask = (uint8_t)(input_channels - 1u);

    /* The rational chain is dispatched before the cascade arms: its two stage
     * descriptors are phase rows, so `stages == 2` must not reach the two-stage
     * code below. */
    if (s_q31.num == ASRC_DECIMATOR_48_TO_32_L)
    {
        /* The ring spans exactly one block plus the longest row's window, so a
         * longer block is refused rather than silently reading samples this call
         * has already overwritten. */
        if (input_frames > DEC_Q31_R23_MAX_IN)
        {
            return false;
        }
        *output_frames = dec_q31_r23_run(input, input_frames, input_stride,
                                         in_mask, output, output_stride);
        return true;
    }

    /* s24-left IS Q31 with eight zero sub-LSBs, so the input needs no
     * conversion at all: the same int32 word is both. */
    if (s_q31.stages == 1u)
    {
        *output_frames = dec_q31_stage_run(0u, input, input_frames,
                                           input_stride, in_mask,
                                           output, output_stride, true);
        return true;
    }

    /* The intermediate buffer holds DEC_Q31_MAX_MID frames, and stage 1 produces
     * (phase + input_frames) / decim of them.  Refusing AFTER the run would be
     * no bound at all -- the write has happened by then -- so the same arithmetic
     * the stage is about to do is done here first.  With the 16-frame TDM block
     * and /2 in stage 1 this is exactly 8, i.e. the buffer is sized to the block
     * and not to a guess; a larger block is refused rather than overrunning. */
    if ((((size_t)s_q31.phase[0] + input_frames) / s_q31.stage[0].decim) >
        DEC_Q31_MAX_MID)
    {
        return false;
    }

    const size_t mid = dec_q31_stage_run(0u, input, input_frames, input_stride,
                                         in_mask, s_q31_mid,
                                         ASRC_DEC_Q31_CH, false);
    /* The intermediate is already ASRC_CH wide, so stage 2 repeats nothing. */
    *output_frames = dec_q31_stage_run(1u, s_q31_mid, mid, ASRC_DEC_Q31_CH,
                                       (uint8_t)(ASRC_DEC_Q31_CH - 1u),
                                       output, output_stride, true);
    return true;
}

#if APP_ASRC_FRONTEND_SELFTEST
/* The boot bit-exactness check.  Its own file because it is a startup check, not
 * part of the front end -- and so that a build without it carries none of it. */
#include "asrc_decimator_q31_selftest.inc"
#endif
