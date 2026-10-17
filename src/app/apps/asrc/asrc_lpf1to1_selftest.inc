/*
 * asrc_lpf1to1_selftest.inc -- the 1:1 anti-alias stage, checked as its own thing.
 *
 * Included from asrc_decimator_q31.inc AFTER asrc_decimator_q31_selftest.inc, because it re-uses
 * that file's oracle: dec_q31_st_ref_stage() is a linear shift register, int64, no-modulo,
 * no-batching Q31 reference that is already generic in st->decim and needs no change to serve
 * decim == 1.  One oracle, two callers -- an oracle written for this case would agree with a
 * wrong kernel as readily as with a right one.
 *
 * WHY THIS EXISTS AT ALL.  A decim == 1 stage is an ordinary value of the arithmetic
 * dec_q31_stage_run() already does (see the note above asrc_decimator_q31_init_lpf1to1), but
 * "ordinary" is a claim about code that has never been executed with that value: no shipped
 * chain has ever set decim to 1, so every branch that reduces to a no-op there is untested code
 * until something runs it.  Source inspection said it should work; this is what makes it a
 * measurement.
 *
 * FOUR PROPERTIES, each of which fails differently:
 *
 *   kase 0  IMPULSE.  A single sample must walk out as the coefficient table itself, in order.
 *           Catches a reversed window, a wrong coeff_off, and an off-by-one in the batch's ring
 *           start w0 -- none of which an amplitude or energy check would notice.  Checked
 *           against the COEFFICIENTS as well as against the oracle, because an oracle reading
 *           the wrong table would agree with a kernel that read the wrong table.
 *   kase 1  FRAME COUNT and ORDER.  N frames in must be N frames out, on EVERY block, with no
 *           residue held back -- checked per block rather than in total, because a stage that
 *           held one frame back would still balance over four blocks.  The vector is a ramp, so
 *           a dropped or duplicated frame also changes the VALUES.
 *   kase 2  CONTINUITY across block boundaries, four blocks of per-channel random audio,
 *           compared sample by sample.  A stage that lost its history at a block boundary
 *           passes kase 0 and kase 1 and fails only here.
 *   kase 3  NEAR FULL SCALE, alternating +/-2^30, which is where sum|h| = 1.70 actually bites.
 *           The oracle saturates exactly as sacr.l does, so a clamp difference appears as a
 *           value mismatch instead of as a wrapped sample nobody notices.
 *
 * EVERY ASRC_CH CHANNEL IS DRIVEN WITH DIFFERENT AUDIO (per-channel LCG seed) and every channel
 * is compared.  That is what makes a cross-channel leak visible -- with silent or identical
 * neighbours it would not be -- so channel isolation is a property of these four cases rather
 * than a fifth pass that could be skipped.
 *
 * TOLERANCE: exact on the s24-left output.  The mask drops eight sub-LSBs, so the sacr.l
 * rounding-mode difference the divider selftest counts as `ties` cannot reach the comparison
 * unless it straddles a 256 boundary.  A straddle is COUNTED as a tie, never hidden -- same
 * treatment, same reasoning, as the divider check.
 */

#define DEC_Q31_LP1_BLOCKS (4u)
#define DEC_Q31_LP1_CASES  (4u)

static int32_t dec_q31_lp1_sample(uint8_t kase, uint8_t ch, uint16_t n)
{
    if (kase == 0u)
    {
        /* Impulse on the first sample of the first block, on every channel. */
        return (n == 0u) ? (int32_t)0x00400000 : 0;
    }
    if (kase == 1u)
    {
        /* A ramp through zero: the frame-count property is then checked by the VALUES too. */
        return (int32_t)(((int32_t)n - 32) << 16);
    }
    if (kase == 3u)
    {
        /* Alternating full scale -- the adversarial input for an L1 norm above 1. */
        return ((n & 1u) != 0u) ? (int32_t)0x40000000 : (int32_t)(-0x40000000);
    }
    /* kase 2: a per-channel LCG, so no two channels carry the same audio.  The channel is mixed
     * into the seed, which is what makes a leak between channels visible. */
    {
        uint32_t s = 0x1234567u + ((uint32_t)ch * 0x9E3779B9u) + ((uint32_t)n * 0x85EBCA6Bu);
        s ^= s >> 15;
        s *= 0x2545F491u;
        s ^= s >> 13;
        return (int32_t)((int32_t)(s & 0x00FFFF00u) - (int32_t)0x00800000);
    }
}

/*
 * ONE ORACLE, ONE CHANNEL PER PASS -- the same shape asrc_decimator_q31_isolation_selftest()
 * uses, and for the same reason: sixteen dec_q31_st_ref_t are 16,896 B of RAM.  The first
 * version of this file kept an array of them and took data memory from 68 % to 93 %, which on
 * this part is where the STACK ERROR lives.  Measured, not predicted -- the ON build reported it.
 *
 * Checking one channel per pass loses nothing: every channel is DRIVEN in every pass (the input
 * block is built full-width with distinct per-channel audio), only the COMPARISON is one channel
 * at a time.  So a leak from channel j into channel i still shows up, on the pass where i is
 * under test.
 */
static dec_q31_st_ref_t s_lp1_ref;
static int32_t s_lp1_in[DEC_Q31_ST_BLOCK * ASRC_DEC_Q31_CH];
static int32_t s_lp1_out[DEC_Q31_ST_BLOCK * ASRC_DEC_Q31_CH];

static bool dec_q31_lp1_case(uint8_t kase, uint8_t under_test, uint32_t *ties)
{
    const uint16_t taps      = s_q31.stage[0].taps;
    const uint16_t coeff_off = s_q31.stage[0].coeff_off;

    for (uint16_t k = 0u; k < DEC_Q31_HIST_PER_CH; ++k) { s_lp1_ref.h[k] = 0; }
    for (uint8_t s = 0u; s < DEC_Q31_MAX_STAGES; ++s)   { s_lp1_ref.phase[s] = 0u; }

    uint16_t n = 0u;
    for (uint16_t blk = 0u; blk < DEC_Q31_LP1_BLOCKS; ++blk)
    {
        for (uint16_t i = 0u; i < DEC_Q31_ST_BLOCK; ++i)
        {
            for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
            {
                s_lp1_in[((size_t)i * ASRC_DEC_Q31_CH) + c] =
                    dec_q31_lp1_sample(kase, c, (uint16_t)(n + i));
            }
        }

        size_t produced = 0u;
        if (!asrc_decimator_q31_process_s24_left(s_lp1_in, DEC_Q31_ST_BLOCK,
                                                 (size_t)ASRC_DEC_Q31_CH,
                                                 (uint8_t)ASRC_DEC_Q31_CH,
                                                 s_lp1_out, DEC_Q31_ST_BLOCK,
                                                 (size_t)ASRC_DEC_Q31_CH, &produced))
        {
            s_q31_st_fail.what  = "refused";
            s_q31_st_fail.den   = 1u;
            s_q31_st_fail.kase  = kase;
            s_q31_st_fail.block = blk;
            return false;
        }
        if (produced != (size_t)DEC_Q31_ST_BLOCK)
        {
            s_q31_st_fail.what  = "frames";
            s_q31_st_fail.den   = 1u;
            s_q31_st_fail.kase  = kase;
            s_q31_st_fail.block = blk;
            s_q31_st_fail.got   = (int32_t)produced;
            s_q31_st_fail.want  = (int32_t)DEC_Q31_ST_BLOCK;
            return false;
        }

        for (uint16_t i = 0u; i < DEC_Q31_ST_BLOCK; ++i)
        {
            {
                const uint8_t c = under_test;
                const int32_t x = s_lp1_in[((size_t)i * ASRC_DEC_Q31_CH) + c];
                int32_t want = 0;
                if (!dec_q31_st_ref_stage(&s_lp1_ref, 0u, x, &want))
                {
                    /* Unreachable at decim == 1 -- and stated as a FAILURE rather than as a
                     * comment, which is the whole point of executing this. */
                    s_q31_st_fail.what    = "oracle-phase";
                    s_q31_st_fail.den     = 1u;
                    s_q31_st_fail.kase    = kase;
                    s_q31_st_fail.block   = blk;
                    s_q31_st_fail.index   = i;
                    s_q31_st_fail.channel = c;
                    return false;
                }
                want = dec_q31_to_s24_left(want);

                const int32_t got = s_lp1_out[((size_t)i * ASRC_DEC_Q31_CH) + c];
                if (got != want)
                {
                    const int32_t d = (got > want) ? (got - want) : (want - got);
                    if (d > 256)
                    {
                        s_q31_st_fail.what    = "value";
                        s_q31_st_fail.den     = 1u;
                        s_q31_st_fail.kase    = kase;
                        s_q31_st_fail.block   = blk;
                        s_q31_st_fail.index   = i;
                        s_q31_st_fail.channel = c;
                        s_q31_st_fail.got     = got;
                        s_q31_st_fail.want    = want;
                        return false;
                    }
                    if (ties != NULL) { ++(*ties); }
                    continue;
                }

                if (kase == 0u)
                {
                    const uint16_t t = (uint16_t)(((uint32_t)blk * DEC_Q31_ST_BLOCK) + i);
                    if (t < taps)
                    {
                        /* coeff[0] multiplies the OLDEST sample of the window, so the impulse
                         * emerges tap-reversed -- the same convention dec_q31_st_ref_stage()
                         * documents.  Getting this index wrong is the bug the case looks for,
                         * which is why it is written out here rather than taken from the oracle. */
                        const int64_t p =
                            (int64_t)s_q31_coeff[coeff_off + (uint16_t)(taps - 1u - t)]
                            * (int64_t)0x00400000;
                        const int32_t tab = dec_q31_to_s24_left(dec_q31_st_sacr(p));
                        const int32_t dd = (got > tab) ? (got - tab) : (tab - got);
                        if (dd > 256)
                        {
                            s_q31_st_fail.what    = "impulse";
                            s_q31_st_fail.den     = 1u;
                            s_q31_st_fail.kase    = kase;
                            s_q31_st_fail.block   = blk;
                            s_q31_st_fail.index   = i;
                            s_q31_st_fail.channel = c;
                            s_q31_st_fail.got     = got;
                            s_q31_st_fail.want    = tab;
                            return false;
                        }
                    }
                }
            }
        }
        n = (uint16_t)(n + DEC_Q31_ST_BLOCK);
    }
    return true;
}

bool asrc_decimator_q31_lpf1to1_selftest(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                         uint32_t *rounding_ties)
{
    uint32_t ties = 0u;
    for (uint8_t kase = 0u; kase < DEC_Q31_LP1_CASES; ++kase)
    {
        for (uint8_t c = 0u; c < (uint8_t)ASRC_DEC_Q31_CH; ++c)
        {
            /* Re-arm before every pass so each starts from a defined ring, exactly as a rate
             * change would -- and so the arm path itself runs 4 * ASRC_CH times rather than once. */
            if (!asrc_decimator_q31_init_lpf1to1(out_rate_hz, in_rate_hz,
                                                 (uint8_t)ASRC_DEC_Q31_CH))
            {
                s_q31_st_fail.what    = "init";
                s_q31_st_fail.den     = 1u;
                s_q31_st_fail.kase    = kase;
                s_q31_st_fail.channel = c;
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
            if (!dec_q31_lp1_case(kase, c, &ties))
            {
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
        }
    }
    if (rounding_ties != NULL) { *rounding_ties = ties; }
    return true;
}


/*
 * ---------------------------------------------------------------------------
 * The COMPOSED chain (44.1 kHz -> /2 -> 22.05 kHz -> 1:1)
 * ---------------------------------------------------------------------------
 *
 * ORACLE: the divider selftest's, unchanged and not copied.  dec_q31_st_ref_chain() already walks
 * s_q31.stages, so once the chain is armed the oracle follows it -- which is the whole reason
 * dec_q31_st_run() was split into an arm and a body (2026-09-14).  What it proves for a
 * composition, specifically:
 *
 *   - BOTH stages ran.  A chain that skipped its second stage would disagree with the oracle on
 *     VALUES (unfiltered samples) and, at a decimating second stage, on the frame COUNT too.
 *     This is the boot-time counterpart of the `fe stages:` line, which answers the same question
 *     at runtime.
 *   - the intermediate is carried at FULL Q31 precision: the oracle masks to s24-left only on the
 *     last stage, so a chain that masked between the two would differ.
 *   - the two rings do not overlap.  Stage 1's ring sits at ring_off = ring0 in the same arena;
 *     an overlap corrupts both windows, and a random multi-block case sees it.
 *   - continuity across process() calls, per stage, including across ring wraps.
 *   - channel replication, from the `live_ch` polarity the shared body drives.
 *
 * AND THE INSTRUMENT PROVES ITSELF, in the way the arm-clear check does: the per-stage counters
 * that runtime evidence will rest on are read here, where the expected relation is known
 * (equal, and non-zero).  A counter wired to the wrong stage index, or never incremented, fails
 * at boot rather than reading as a healthy log on the bench.
 *
 * Label 2441 in the failure record: not a divider, not a 1000+ composed pre-stage code, and it
 * says which pair -- so a composed failure cannot be read as the plain /2 chain's.
 */
#define DEC_Q31_C44K1_NOTE  (2441u)

bool asrc_decimator_q31_dec_then_lpf1to1_selftest(uint32_t out_rate_hz, uint32_t in_rate_hz,
                                                  uint32_t den, uint32_t *rounding_ties)
{
    uint32_t ties = 0u;
    for (uint8_t kase = 0u; kase < DEC_Q31_ST_CASES; ++kase)
    {
        for (uint8_t live_ch = 0u; live_ch < 2u; ++live_ch)
        {
            /* Re-arm per pass, so each starts from a defined ring exactly as a rate change would
             * -- and so the composed arm path itself runs 2 * DEC_Q31_ST_CASES times. */
            if (!asrc_decimator_q31_init_dec_then_lpf1to1(out_rate_hz, in_rate_hz, den,
                                                          (uint8_t)ASRC_DEC_Q31_CH))
            {
                dec_q31_st_note("init", DEC_Q31_C44K1_NOTE, kase, live_ch, 0u, 0u, 0u, 0, 0);
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
            if (!dec_q31_st_run_armed(DEC_Q31_C44K1_NOTE, kase, live_ch, &ties))
            {
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
            /* The runtime instrument, checked where its answer is known. */
            const uint32_t s0 = asrc_decimator_q31_stage_output_frames(0u);
            const uint32_t s1 = asrc_decimator_q31_stage_output_frames(1u);
            if ((s0 == 0u) || (s0 != s1))
            {
                dec_q31_st_note("stage-count", DEC_Q31_C44K1_NOTE, kase, live_ch, 0u, 0u, 0u,
                                (int32_t)s0, (int32_t)s1);
                if (rounding_ties != NULL) { *rounding_ties = ties; }
                return false;
            }
        }
    }
    if (rounding_ties != NULL) { *rounding_ties = ties; }
    return true;
}
