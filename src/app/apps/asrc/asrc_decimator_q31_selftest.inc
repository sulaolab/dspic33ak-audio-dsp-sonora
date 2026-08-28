/*
 * asrc_decimator_q31_selftest.inc -- boot bit-exactness check of the Q31 front end.
 *
 * Included from asrc_decimator_q31.inc, which is itself included from
 * asrc_decimator_48_to_8.c.  Kept in its own file because it is a startup check
 * rather than part of the front end, and because everything in here is compiled
 * out when APP_ASRC_FRONTEND_SELFTEST is 0.
 *
 * WHAT IT CHECKS, AND WHY THIS ORACLE
 *
 * The float front end's selftest compared an optimized float path against a
 * reference float path.  Once the shipping arithmetic is Q31 that comparison
 * verifies nothing about the shipping path, so it is replaced rather than kept:
 * the oracle here is an INDEPENDENT Q31 implementation -- linear shift register,
 * int64 accumulate, no hardware modulo, no batching, no assembly.  Comparing the
 * two therefore tests exactly the things that can break in the fast path:
 *
 *   - the modulo ring geometry (YMODSRT/YMODEND vs the hand-folded window start),
 *   - the batch arithmetic (b outputs from one call, window span T + (b-1)*D),
 *   - the per-stage phase bookkeeping across block boundaries,
 *   - the two-stage chain and its unmasked intermediate,
 *   - the channel mask that spreads the codec pair over ASRC_CH,
 *   - and saturation at the top of the range.
 *
 * The accumulator model is the one the kernel bench measured against hardware at
 * 0 LSB error (asrc_fir_kernel_bench.c, firb_ref_q31): a fractional MAC of two
 * Q31 values adds the product shifted left by one, and sacr.l rounds at bit 31.
 * Written here as (sum + 2^30) >> 31 over the UNDOUBLED products, which is the
 * same number with an extra factor of two of int64 headroom -- and that headroom
 * is the point, because the near-full-scale case below deliberately drives the
 * output past +-1.0, where the doubled form would overflow int64 before it could
 * describe what the hardware is supposed to do.
 *
 * ROUNDING: this reference rounds halves away from zero.  fractsetup leaves
 * CORCON with convergent rounding, so the two can disagree by one Q31 LSB when a
 * sum lands exactly on a half -- and only then.  One Q31 LSB is invisible after
 * the s24-left mask unless the tie straddles a mask boundary, in which case the
 * masked values differ by exactly 256.  So a difference of exactly 256 is
 * counted as a rounding tie and reported; ANY other difference fails.  That is
 * the "bit-identical, or different only by explicable rounding" bar, made
 * machine-checkable instead of argued.
 *
 * STACK: about 1.6 kB in one frame (760 B oracle history + 576 B output block +
 * the input block).  Deliberately small: the float selftest it replaces needed
 * ~5 kB, which the 16ch BIDIR image cannot spare -- see the note in
 * audio_app_meas.c about a stack overflow being unable to report itself.
 */

#define DEC_Q31_ST_BLOCK  (16u)                  /* input frames per process() call */
#define DEC_Q31_ST_BLOCKS (16u)                  /* calls per case: 256 frames, so
                                                  * every ring (190, 147, 43) wraps */
#define DEC_Q31_ST_OUTMAX ((DEC_Q31_ST_BLOCK / 2u) + 1u)

/* Cases.  0..3 are the original four and are what every INTEGER chain runs; the
 * 2/3 rational chain runs all eleven (they cover the ten required stimulus conditions), because it is the new arithmetic and because
 * its schedule (two phase rows, a 3-input period that does not divide the
 * 16-frame block) has boundary conditions the integer chains do not have.
 *
 * Adding 4..9 to the integer chains would be an unrelated change to shipping
 * chains that are already verified, so it is not done here. */
#define DEC_Q31_ST_IMPULSE   (0u)   /* full-scale impulse, off the block start   */
#define DEC_Q31_ST_DC        (1u)
#define DEC_Q31_ST_NORMAL    (2u)   /* LCG pseudorandom                          */
#define DEC_Q31_ST_FULLSCALE (3u)   /* +-FS steps: drives sacr.l into saturation */
#define DEC_Q31_ST_CASES     (4u)
#define DEC_Q31_ST_ZERO      (4u)   /* silence in, silence out, exactly          */
#define DEC_Q31_ST_SMALL     (5u)   /* small impulse: no saturation anywhere     */
#define DEC_Q31_ST_ALTFS     (6u)   /* +-FS EVERY frame = Nyquist square wave    */
#define DEC_Q31_ST_SINE      (7u)   /* fs/8 = 6 kHz, in band                     */
#define DEC_Q31_ST_NEARNYQ   (8u)   /* 7fs/16 = 21 kHz, deep in the stopband     */
#define DEC_Q31_ST_BLOCKEDGE (9u)   /* impulses ON the block boundaries          */
#define DEC_Q31_ST_PHASEEDGE (10u)  /* impulses at every schedule phase          */
#define DEC_Q31_ST_R23_CASES (11u)

/* Q31 sine, 16 points, 0.5 FS, masked to s24-left like every other stimulus.
 * A table rather than sinf(): this file must not pull libm into an image whose
 * program region is the binding constraint. */
static const int32_t s_q31_st_sine16[16] = {
    0x00000000,  0x187DE200,  0x2D413C00,  0x3B20D700,
    0x40000000,  0x3B20D700,  0x2D413C00,  0x187DE200,
    0x00000000, -0x187DE200, -0x2D413C00, -0x3B20D700,
   -0x40000000, -0x3B20D700, -0x2D413C00, -0x187DE200
};

/* Where a failure was, in numbers rather than in a formatted string: printf
 * already has a caller in asrc_audio_path.c, and snprintf would not.  `what` is
 * a string literal, so it costs no RAM. */
static asrc_decimator_q31_fail_t s_q31_st_fail;

const asrc_decimator_q31_fail_t *asrc_decimator_q31_selftest_fail(void)
{
    return &s_q31_st_fail;
}

/* The oracle's per-stage state.  One channel, and partitioned over ONE 190-entry
 * buffer exactly the way s_q31_hist is, so a two-stage chain costs the same
 * 760 B as a single-stage one.  h[0] is the newest sample. */
typedef struct
{
    int32_t  h[DEC_Q31_HIST_PER_CH];
    uint8_t  phase[DEC_Q31_MAX_STAGES];
} dec_q31_st_ref_t;

/* sum is Q62 (undoubled products).  Rounds at bit 31 and saturates, which is
 * what sacr.l does with SATA and ACCSAT enabled. */
static int32_t dec_q31_st_sacr(int64_t sum)
{
    const int64_t v = (sum + 0x40000000LL) >> 31;
    if (v > 2147483647LL)  { return (int32_t)0x7FFFFFFF; }
    if (v < -2147483648LL) { return (int32_t)0x80000000; }
    return (int32_t)v;
}

/* One oracle stage: push `x`, and if this sample completes a decimation period
 * emit the dot product of the newest `taps` samples.  Returns true when it
 * emitted.  coeff[0] multiplies the OLDEST sample of the window, matching the
 * kernel (which walks the window forwards from its start). */
static bool dec_q31_st_ref_stage(dec_q31_st_ref_t *r, uint8_t s, int32_t x,
                                 int32_t *out)
{
    const dec_q31_stage_t *const st = &s_q31.stage[s];
    int32_t *const h = &r->h[st->ring_off];
    const uint16_t T = st->taps;

    for (uint16_t k = (uint16_t)(T - 1u); k > 0u; --k) { h[k] = h[k - 1u]; }
    h[0] = x;

    r->phase[s] = (uint8_t)(r->phase[s] + 1u);
    if (r->phase[s] < st->decim) { return false; }
    r->phase[s] = 0u;

    const int32_t *const c = &s_q31_coeff[st->coeff_off];
    int64_t sum = 0;
    for (uint16_t k = 0u; k < T; ++k)
    {
        sum += (int64_t)c[k] * (int64_t)h[T - 1u - k];
    }
    *out = dec_q31_st_sacr(sum);
    return true;
}

/* The whole chain, one channel.  The s24-left mask is applied only by the last
 * stage, the same asymmetry the fast path has. */
static size_t dec_q31_st_ref_chain(dec_q31_st_ref_t *r, const int32_t *in,
                                   size_t frames, int32_t *out)
{
    size_t n = 0u;
    for (size_t i = 0u; i < frames; ++i)
    {
        int32_t v1;
        if (!dec_q31_st_ref_stage(r, 0u, in[i], &v1)) { continue; }
        if (s_q31.stages == 1u)
        {
            out[n++] = dec_q31_to_s24_left(v1);
            continue;
        }
        int32_t v2;
        if (dec_q31_st_ref_stage(r, 1u, v1, &v2))
        {
            out[n++] = dec_q31_to_s24_left(v2);
        }
    }
    return n;
}

/* Test input.  `lcg` carries the pseudorandom state, so a case is reproducible
 * from its case id alone.  Every value is s24-left (low byte zero), because that
 * is what the front end is fed in service. */
static int32_t dec_q31_st_sample(uint8_t kase, uint32_t n, uint32_t *lcg)
{
    switch (kase)
    {
    case DEC_Q31_ST_IMPULSE:
        /* Not at n == 0: an impulse in the middle of the first block moves the
         * ring write pointer before the first output, so a window start that is
         * right only when wr == 0 fails here. */
        return (n == 5u) ? (int32_t)0x7FFFFF00 : 0;

    case DEC_Q31_ST_DC:
        return (int32_t)0x40000000;   /* -6 dBFS DC: exercises the tap sum */

    case DEC_Q31_ST_NORMAL:
        *lcg = (*lcg * 1103515245u) + 12345u;
        return (int32_t)((int32_t)(*lcg & 0x3FFFFF00u) - 0x20000000);  /* +-0.25 FS */

    case DEC_Q31_ST_ZERO:
        return 0;

    case DEC_Q31_ST_SMALL:
        /* A quiet impulse.  The full-scale one above cannot distinguish a wrong
         * coefficient from a clamp; this one never saturates, so every tap of the
         * response has to match exactly. */
        return (n == 7u) ? (int32_t)0x00100000 : 0;

    case DEC_Q31_ST_ALTFS:
        /* +-FS on alternate frames: the Nyquist square wave, i.e. the worst input
         * a decimation lowpass can be handed, and the one where a swapped phase
         * row shows up as a large error rather than a small one. */
        return (n & 1u) ? (int32_t)0x80000100 : (int32_t)0x7FFFFF00;

    case DEC_Q31_ST_SINE:
        return s_q31_st_sine16[(n * 2u) & 15u];      /* fs/8 */

    case DEC_Q31_ST_NEARNYQ:
        return s_q31_st_sine16[(n * 7u) & 15u];      /* 7fs/16, just under fs/2 */

    case DEC_Q31_ST_BLOCKEDGE:
        /* Impulses exactly ON the block seam -- the last frame of one call and the
         * first frame of the next.  A schedule that is carried wrongly across
         * process() calls misplaces these and nothing else. */
        return (((n % DEC_Q31_ST_BLOCK) == 0u) ||
                ((n % DEC_Q31_ST_BLOCK) == (DEC_Q31_ST_BLOCK - 1u)))
                   ? (int32_t)0x40000000 : 0;

    case DEC_Q31_ST_PHASEEDGE:
        /* One impulse every 5 frames.  5 is coprime with both the 3-input
         * schedule period and the 16-frame block, so over 240 frames an impulse
         * lands on every (block offset, phase row, ring position) combination the
         * chain has -- which is the phase-boundary case, enumerated rather than
         * assumed. */
        return ((n % 5u) == 0u) ? (int32_t)0x40000000 : 0;

    default:
        /* Near full scale, with a step every 32 frames.  The step is what pushes
         * the output past +-1.0: these are unity-gain lowpasses, so a full-scale
         * transient overshoots (Gibbs) and sacr.l has to clamp.  Both polarities,
         * so the positive max and the negative min are both reached. */
        return ((n / 32u) & 1u) ? (int32_t)0x80000100 : (int32_t)0x7FFFFF00;
    }
}

static void dec_q31_st_note(const char *what, uint32_t den, uint8_t kase,
                            uint8_t live_ch, uint32_t blk, uint32_t idx,
                            uint8_t ch, int32_t got, int32_t want)
{
    s_q31_st_fail.what    = what;
    s_q31_st_fail.den     = (uint16_t)den;
    s_q31_st_fail.kase    = kase;
    s_q31_st_fail.live_ch = live_ch;
    s_q31_st_fail.block   = (uint16_t)blk;
    s_q31_st_fail.index   = (uint16_t)idx;
    s_q31_st_fail.channel = ch;
    s_q31_st_fail.got     = got;
    s_q31_st_fail.want    = want;
}

/* One chain x one case x one channel polarity.
 *
 * `live_ch` says which of the two source channels carries the signal; the other
 * is fed silence and must come out silent.  That is what turns "16 channels all
 * agree" into a real check of the `c & mask` replication: a mask bug either mixes
 * the silent channel into the live one or copies the live one everywhere. */
static bool dec_q31_st_run(uint32_t den,
                           asrc_decimator_48_to_24_variant_t v24,
                           asrc_decimator_48_to_12_variant_t v12,
                           uint8_t kase, uint8_t live_ch, uint32_t *ties)
{
    if (!asrc_decimator_q31_init(1u, den, (uint8_t)ASRC_DEC_Q31_CH, v24, v12))
    {
        dec_q31_st_note("init", den, kase, live_ch, 0u, 0u, 0u, 0, 0);
        return false;
    }

    dec_q31_st_ref_t ref;
    memset(&ref, 0, sizeof(ref));

    int32_t in[DEC_Q31_ST_BLOCK * 2u];
    int32_t got[DEC_Q31_ST_OUTMAX * ASRC_DEC_Q31_CH];
    int32_t want[DEC_Q31_ST_OUTMAX];
    int32_t mono[DEC_Q31_ST_BLOCK];
    uint32_t lcg = 0x13579BDFu + kase;

    for (uint32_t blk = 0u; blk < DEC_Q31_ST_BLOCKS; ++blk)
    {
        for (uint32_t f = 0u; f < DEC_Q31_ST_BLOCK; ++f)
        {
            const int32_t v = dec_q31_st_sample(kase,
                                                (blk * DEC_Q31_ST_BLOCK) + f,
                                                &lcg);
            mono[f] = v;
            in[(f * 2u) + live_ch]        = v;
            in[(f * 2u) + (1u - live_ch)] = 0;
        }

        size_t produced = 0u;
        if (!asrc_decimator_q31_process_s24_left(in, DEC_Q31_ST_BLOCK, 2u, 2u,
                                                 got, DEC_Q31_ST_OUTMAX,
                                                 ASRC_DEC_Q31_CH, &produced))
        {
            dec_q31_st_note("refused", den, kase, live_ch, blk, 0u, 0u, 0, 0);
            return false;
        }

        const size_t expect = dec_q31_st_ref_chain(&ref, mono,
                                                   DEC_Q31_ST_BLOCK, want);
        if (produced != expect)
        {
            dec_q31_st_note("frames", den, kase, live_ch, blk, 0u, 0u,
                            (int32_t)produced, (int32_t)expect);
            return false;
        }

        for (size_t o = 0u; o < produced; ++o)
        {
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                const int32_t v = got[(o * ASRC_DEC_Q31_CH) + c];
                const int32_t w = ((c & 1u) == live_ch) ? want[o] : 0;
                if (v == w) { continue; }
                /* The one difference the two implementations are allowed: a
                 * rounding tie that happened to straddle a mask boundary. */
                if (((v - w) == 256) || ((v - w) == -256))
                {
                    ++(*ties);
                    continue;
                }
                dec_q31_st_note("value", den, kase, live_ch, blk,
                                (uint32_t)o, c, v, w);
                return false;
            }
        }
    }
    return true;
}

/*
 * ---------------------------------------------------------------------------
 * The 2/3 rational chain (48 -> 32 kHz AUDIO MODE)
 * ---------------------------------------------------------------------------
 *
 * ORACLE.  Deliberately the TEXTBOOK construction and not the shipping one: zero
 * stuff by L = 2, convolve the WHOLE 97-tap prototype at 96 kHz, keep every
 * M = 3rd sample.  The shipping path does none of that -- it computes only the
 * two phases that survive and spends no MAC on a coefficient that would meet a
 * stuffed zero -- so agreement between the two is evidence, not a tautology.  It
 * is exactly what pins down the parts that are easy to get wrong by one:
 *
 *   - WHICH input is newest for output k (floor(3k/2), not ceil),
 *   - which phase row output k belongs to,
 *   - the ring start each row's batch is handed,
 *   - the interleave of the two rows' batches back into output order,
 *   - and the schedule carried across process() calls and across ring wraps.
 *
 * The 97-tap prototype is reconstructed from the SHIPPING X-space rows, by the
 * exact inverse of the mapping init() used to store them -- h[96-2t] = row0[t],
 * h[95-2t] = row1[t] -- so it does NOT assume the rows are symmetric and would
 * catch a reversed row.  What it cannot check is whether the rows are the right
 * FILTER: that is the coefficient CRC (build time) plus the host audio gate
 * (tools/asrc/asrc_48_to_32_audio_gate.py), which measures the shipping include.
 *
 * Failures report den = 203, i.e. num*100 + den, so a 2/3 failure can never be
 * read as a /3 (48 -> 16 kHz) failure.
 *
 * STACK: about 1.3 kB in one frame for the 2-channel run below (388 B oracle
 * window + 704 B output block + the rest), and about 2.2 kB for the 16-channel
 * isolation run, whose input block alone is 1 kB.  The linked stack region is
 * 3,176 B and the audio ISRs are not running yet at boot, so both fit -- but
 * neither has room to grow much, which is why the isolation run verifies ONE
 * channel per call instead of keeping sixteen oracles alive.
 */
#define DEC_Q31_ST_R23_DEN  (203u)   /* num*100 + den, for the failure record */

typedef struct
{
    int32_t u[ASRC_DECIMATOR_48_TO_32_PROTO_TAPS];   /* u[0] newest, zeros and all */
    uint8_t mi;                                      /* index of the NEXT push, mod M */
} dec_q31_st_r23_t;

/* h[j] of the 97-tap prototype, read back out of the shipping X coefficients. */
static int32_t dec_q31_st_r23_proto(uint16_t j)
{
    const uint16_t n = ASRC_DECIMATOR_48_TO_32_PROTO_TAPS;
    return ((j & 1u) == 0u)
               ? s_q31_coeff[(uint16_t)(n - 1u - j) / 2u]
               : s_q31_coeff[ASRC_DECIMATOR_48_TO_32_PHASE0_TAPS +
                             ((uint16_t)(n - 2u - j) / 2u)];
}

static size_t dec_q31_st_r23_ref(dec_q31_st_r23_t *r, const int32_t *in,
                                 size_t frames, int32_t *out)
{
    const uint16_t n = ASRC_DECIMATOR_48_TO_32_PROTO_TAPS;
    size_t emitted = 0u;

    for (size_t i = 0u; i < frames; ++i)
    {
        for (uint8_t half = 0u; half < ASRC_DECIMATOR_48_TO_32_L; ++half)
        {
            for (uint16_t k = (uint16_t)(n - 1u); k > 0u; --k)
            {
                r->u[k] = r->u[k - 1u];
            }
            r->u[0] = (half == 0u) ? in[i] : 0;   /* the zero stuffing, literally */

            const uint8_t mi = r->mi;
            r->mi = (uint8_t)((mi + 1u) % ASRC_DECIMATOR_48_TO_32_M);
            if (mi != 0u) { continue; }           /* not a kept sample */

            int64_t sum = 0;
            for (uint16_t j = 0u; j < n; ++j)
            {
                sum += (int64_t)dec_q31_st_r23_proto(j) * (int64_t)r->u[j];
            }
            out[emitted++] = dec_q31_to_s24_left(dec_q31_st_sacr(sum));
        }
    }
    return emitted;
}

/* One case of the 2/3 chain, two live channels, the same live/silent polarity
 * check the integer chains get. */
static bool dec_q31_st_r23_case(uint8_t kase, uint8_t live_ch, uint32_t *ties)
{
    if (!asrc_decimator_q31_init(ASRC_DECIMATOR_48_TO_32_L,
                                 ASRC_DECIMATOR_48_TO_32_M,
                                 (uint8_t)ASRC_DEC_Q31_CH,
                                 ASRC_DECIMATOR_48_TO_24_FOR_24000,
                                 ASRC_DECIMATOR_48_TO_12_FOR_11025))
    {
        dec_q31_st_note("init", DEC_Q31_ST_R23_DEN, kase, live_ch, 0u, 0u, 0u, 0, 0);
        return false;
    }

    dec_q31_st_r23_t ref;
    memset(&ref, 0, sizeof(ref));

    int32_t  in[DEC_Q31_ST_BLOCK * 2u];
    int32_t  got[DEC_Q31_R23_MAX_OUT * ASRC_DEC_Q31_CH];
    int32_t  want[DEC_Q31_R23_MAX_OUT];
    int32_t  mono[DEC_Q31_ST_BLOCK];
    uint32_t lcg = 0x2468ACE0u + kase;

    for (uint32_t blk = 0u; blk < DEC_Q31_ST_BLOCKS; ++blk)
    {
        for (uint32_t f = 0u; f < DEC_Q31_ST_BLOCK; ++f)
        {
            const int32_t v = dec_q31_st_sample(kase,
                                                (blk * DEC_Q31_ST_BLOCK) + f,
                                                &lcg);
            mono[f] = v;
            in[(f * 2u) + live_ch]        = v;
            in[(f * 2u) + (1u - live_ch)] = 0;
        }

        size_t produced = 0u;
        if (!asrc_decimator_q31_process_s24_left(in, DEC_Q31_ST_BLOCK, 2u, 2u,
                                                 got, DEC_Q31_R23_MAX_OUT,
                                                 ASRC_DEC_Q31_CH, &produced))
        {
            dec_q31_st_note("refused", DEC_Q31_ST_R23_DEN, kase, live_ch,
                            blk, 0u, 0u, 0, 0);
            return false;
        }

        const size_t expect = dec_q31_st_r23_ref(&ref, mono, DEC_Q31_ST_BLOCK, want);
        if (produced != expect)
        {
            dec_q31_st_note("frames", DEC_Q31_ST_R23_DEN, kase, live_ch, blk, 0u,
                            0u, (int32_t)produced, (int32_t)expect);
            return false;
        }

        for (size_t o = 0u; o < produced; ++o)
        {
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                const int32_t v = got[(o * ASRC_DEC_Q31_CH) + c];
                const int32_t w = ((c & 1u) == live_ch) ? want[o] : 0;
                if (v == w) { continue; }
                if (((v - w) == 256) || ((v - w) == -256))
                {
                    ++(*ties);
                    continue;
                }
                dec_q31_st_note("value", DEC_Q31_ST_R23_DEN, kase, live_ch, blk,
                                (uint32_t)o, c, v, w);
                return false;
            }
        }
    }
    return true;
}

/*
 * CHANNEL ISOLATION at the full ASRC_CH width.  All sixteen channels carry
 * DIFFERENT audio -- a per-channel LCG seed, so no two channels share a sample
 * and no channel is silent -- and channel `live_ch`'s output is required to equal
 * the oracle of channel `live_ch`'s input ALONE.  Any leak between channels
 * (ring indexing, batch interleave, stride arithmetic) therefore shows up as a
 * mismatch; with identical or silent neighbours it would not.
 *
 * One channel per call because sixteen oracle windows do not fit the stack (see
 * the note above).  The caller loops over all ASRC_CH of them, so every channel
 * is checked against fifteen different neighbours.
 *
 * `out` (out_frames x ASRC_DEC_Q31_CH interleaved) is the caller's buffer, for
 * the RAM reason spelled out at the declaration in asrc_decimator_48_to_8.h: on
 * the Q31 arm the stack cannot hold the input block, the output block and the
 * oracle at once.  The capacity is checked here rather than assumed, so a caller
 * that shrinks its buffer gets a named failure instead of a silent overrun.
 */
bool asrc_decimator_q31_isolation_selftest(uint8_t live_ch, int32_t *out,
                                           size_t out_frames)
{
    if ((out == NULL) || (out_frames < (size_t)DEC_Q31_R23_MAX_OUT))
    {
        dec_q31_st_note("out_frames", DEC_Q31_ST_R23_DEN, 0u, live_ch, 0u, 0u, 0u,
                        (int32_t)out_frames, (int32_t)DEC_Q31_R23_MAX_OUT);
        return false;
    }
    if (live_ch >= (uint8_t)ASRC_DEC_Q31_CH)
    {
        dec_q31_st_note("live_ch", DEC_Q31_ST_R23_DEN, 0u, live_ch, 0u, 0u, 0u,
                        (int32_t)live_ch, (int32_t)ASRC_DEC_Q31_CH);
        return false;
    }
    if (!asrc_decimator_q31_init(ASRC_DECIMATOR_48_TO_32_L,
                                 ASRC_DECIMATOR_48_TO_32_M,
                                 (uint8_t)ASRC_DEC_Q31_CH,
                                 ASRC_DECIMATOR_48_TO_24_FOR_24000,
                                 ASRC_DECIMATOR_48_TO_12_FOR_11025))
    {
        dec_q31_st_note("init", DEC_Q31_ST_R23_DEN, 0u, live_ch, 0u, 0u, 0u, 0, 0);
        return false;
    }

    dec_q31_st_r23_t ref;
    memset(&ref, 0, sizeof(ref));

    int32_t  in[DEC_Q31_ST_BLOCK * ASRC_DEC_Q31_CH];
    int32_t *got = out;            /* caller's buffer -- see the note above */
    int32_t  want[DEC_Q31_R23_MAX_OUT];
    int32_t  mono[DEC_Q31_ST_BLOCK];
    uint32_t lcg[ASRC_DEC_Q31_CH];

    /* Distinct seeds, and none of them zero: a channel whose stream happened to
     * match another's would weaken exactly the property under test. */
    for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
    {
        lcg[c] = 0x0A5A0001u + ((uint32_t)c * 0x11119u);
    }

    for (uint32_t blk = 0u; blk < DEC_Q31_ST_BLOCKS; ++blk)
    {
        for (uint32_t f = 0u; f < DEC_Q31_ST_BLOCK; ++f)
        {
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                const int32_t v = dec_q31_st_sample(DEC_Q31_ST_NORMAL,
                                                    (blk * DEC_Q31_ST_BLOCK) + f,
                                                    &lcg[c]);
                in[(f * ASRC_DEC_Q31_CH) + c] = v;
                if (c == live_ch) { mono[f] = v; }
            }
        }

        size_t produced = 0u;
        /* input_channels = ASRC_CH, so the `c & mask` replication is the identity
         * and every channel really does carry its own audio. */
        if (!asrc_decimator_q31_process_s24_left(in, DEC_Q31_ST_BLOCK,
                                                 ASRC_DEC_Q31_CH,
                                                 (uint8_t)ASRC_DEC_Q31_CH,
                                                 got, DEC_Q31_R23_MAX_OUT,
                                                 ASRC_DEC_Q31_CH, &produced))
        {
            dec_q31_st_note("refused", DEC_Q31_ST_R23_DEN, 0u, live_ch,
                            blk, 0u, 0u, 0, 0);
            return false;
        }

        const size_t expect = dec_q31_st_r23_ref(&ref, mono, DEC_Q31_ST_BLOCK, want);
        if (produced != expect)
        {
            dec_q31_st_note("frames", DEC_Q31_ST_R23_DEN, 0u, live_ch, blk, 0u,
                            0u, (int32_t)produced, (int32_t)expect);
            return false;
        }

        for (size_t o = 0u; o < produced; ++o)
        {
            const int32_t v = got[(o * ASRC_DEC_Q31_CH) + live_ch];
            const int32_t w = want[o];
            if (v == w) { continue; }
            /* No tie allowance here: this run and the 2-channel run above share
             * the same arithmetic, so a difference means a CHANNEL problem. */
            dec_q31_st_note("isolation", DEC_Q31_ST_R23_DEN, 0u, live_ch, blk,
                            (uint32_t)o, live_ch, v, w);
            return false;
        }
    }
    return true;
}

/*
 * Every chain the runtime routing gate can select, every case, both channel
 * polarities.  Enumerated rather than derived: a divider or variant added to the
 * gate without a row here is a chain that ships unverified, and this list is
 * where that omission is visible.
 */
bool asrc_decimator_q31_selftest(uint32_t *rounding_ties)
{
    static const struct
    {
        uint32_t den;
        asrc_decimator_48_to_24_variant_t v24;
        asrc_decimator_48_to_12_variant_t v12;
    } chains[] = {
        { 2u, ASRC_DECIMATOR_48_TO_24_FOR_24000, ASRC_DECIMATOR_48_TO_12_FOR_11025 },
        { 2u, ASRC_DECIMATOR_48_TO_24_FOR_22050, ASRC_DECIMATOR_48_TO_12_FOR_11025 },
        { 3u, ASRC_DECIMATOR_48_TO_24_FOR_24000, ASRC_DECIMATOR_48_TO_12_FOR_11025 },
        { 4u, ASRC_DECIMATOR_48_TO_24_FOR_24000, ASRC_DECIMATOR_48_TO_12_FOR_11025 },
        { 4u, ASRC_DECIMATOR_48_TO_24_FOR_24000, ASRC_DECIMATOR_48_TO_12_FOR_12000 },
        { 6u, ASRC_DECIMATOR_48_TO_24_FOR_24000, ASRC_DECIMATOR_48_TO_12_FOR_11025 },
    };

    uint32_t ties = 0u;
    memset(&s_q31_st_fail, 0, sizeof(s_q31_st_fail));

    for (uint8_t i = 0u; i < (uint8_t)(sizeof(chains) / sizeof(chains[0])); ++i)
    {
        for (uint8_t kase = 0u; kase < DEC_Q31_ST_CASES; ++kase)
        {
            for (uint8_t ch = 0u; ch < 2u; ++ch)
            {
                if (!dec_q31_st_run(chains[i].den, chains[i].v24,
                                    chains[i].v12, kase, ch, &ties))
                {
                    if (rounding_ties != NULL) { *rounding_ties = ties; }
                    return false;
                }
            }
        }
    }
    /* The 2/3 rational chain, all eleven cases, both polarities. */
    for (uint8_t kase = 0u; kase < DEC_Q31_ST_R23_CASES; ++kase)
    {
        for (uint8_t ch = 0u; ch < 2u; ++ch)
        {
            if (!dec_q31_st_r23_case(kase, ch, &ties))
            {
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
        }
    }
    if (rounding_ties != NULL) { *rounding_ties = ties; }
    return true;
}
