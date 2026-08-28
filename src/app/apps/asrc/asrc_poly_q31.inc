/*
 * asrc_poly_q31.inc -- Q31 arithmetic for the generic polyphase ASRC resampler.
 *
 * Included from audio_app_asrc.c when ASRC_SAMPLE_Q31 is 1.  Same filter, same
 * L=128 phases, same M=30 taps, same fc, same window, same per-phase unity-DC
 * normalisation as the shipping float path -- only the arithmetic changes.  This
 * is deliberately NOT a filter redesign: nothing here alters the response.
 *
 * ---------------------------------------------------------------------------
 * Q FORMAT (all four boundaries, with the measured bounds that justify them)
 * ---------------------------------------------------------------------------
 *
 * INPUT.  The TDM slot word is s24 left-justified in 32 bits, so the codec word
 * IS a Q31 sample: slot == s24 << 8.  The float path spends a conversion to get
 * away from that -- (float)(slot >> 8), i.e. the raw signed-24 INTEGER carried
 * as a float, not +-1.0 -- and in doing so exhausts float32's 24-bit mantissa.
 * The Q31 path instead masks the eight sub-LSBs that the float path throws away.
 * So the data-path boundary costs one AND, not a conversion: the "Q31 MAC got
 * faster but the conversions ate the win" failure mode cannot arise here,
 * because there are no conversions left to eat it.
 *
 * COEFFICIENT.  Q31, the same windowed-sinc prototype with the same per-row
 * unity-DC normalisation, rounded half away from zero (the convention already
 * proven on this silicon by dec_q31_from_float in asrc_decimator_q31.inc).  The
 * design is done in float64 on the host and baked in rather than rebuilt on
 * target -- see the table below for why that is required, not merely cheaper.
 * Measured on the shipping table by tools/asrc/asrc_q31_resampler_model.py
 * coeff:
 *
 *     max|c|                       0.929999101  -> Q31 legal as-is; no Q30
 *                                                  rescale, 0.07 of headroom
 *     quantisation error           <= 0.50 LSB31 (rms 1.34e-10) vs the float64
 *                                                  design it was rounded from
 *     worst row DC gain error      1.86e-9  (0.0000 dB, 4 LSB31 on the row sum)
 *     per-phase gain wobble p2p    3.26e-9  after quantisation
 *     vs the float arm's table     <= 1239 LSB31 (5.77e-7): NOT quantisation
 *                                                  error, the two arms simply
 *                                                  design in f64 and f32
 *
 * ACCUMULATOR.  NOT 32-bit -- that was checked, not assumed.  sum|c| over a row
 * is 2.009214068, so a full-scale input drives the raw sum to 2.009 * 2^31 in
 * Q31 units: two guard bits are REQUIRED and a 32-bit accumulator WOULD
 * overflow.  ACCA carries 8 magnitude guard bits above Q31 (127x more than the
 * bound needs) and sacr.l saturates rather than wraps if anything ever exceeds
 * it.  sum|c| > 2 also means a full-scale broadband input legitimately clips --
 * that is a property of the SHIPPING filter, present in the float path today,
 * not something Q31 introduced.
 *
 * BLEND / PHASE INTERPOLATION.  ceff[k] = c0[k] + round(wbq * (c1[k] - c0[k])),
 * wbq being the blend weight in Q31 and the rounding done by sacr.l at bit 31.
 * Chosen over the algebraically equal c0*(1-wb) + c1*wb because 1.0 is NOT
 * representable in Q31: the two-multiply form would carry a systematic 1-LSB31
 * error at wb == 0, i.e. at exactly the step == 1.0 / 1.5 / 2.0 rates that
 * matter most.  The difference cannot leave int32 (|c| <= 0.93, so |delta| <
 * 1.86 in Q31 units), the add-back happens inside the accumulator, and the
 * result is convex in (c0,c1) so it cannot leave int32 either.  A 16-bit blend
 * weight was rejected: it would put the blend floor near -108 dB, inside the
 * audio margin.  There is ONE arithmetic path -- step == 1.0 evaluates the same
 * expression with wbq == 0, it is not a separate fast path -- so the numerical
 * result cannot differ between the step == 1 and step != 1 cases.
 *
 * OUTPUT.  sacr.l rounds Q31 at bit 31 and saturates; the slot store masks the
 * eight sub-LSBs (dec_q31_to_s24_left), truncating toward -inf exactly as the
 * shipping decimator front end does.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS STRUCTURE HAS NO step != 1 PENALTY
 * ---------------------------------------------------------------------------
 * The float path buys its speed from two amortisations that BOTH require the two
 * outputs of a pair to be exactly one input frame apart (d == 1): the whole-block
 * kernel (one register frame and one floatsetup for all 16 output frames, writing
 * straight into the DMA buffer) and the "ce" hoisted blend shared between the two
 * outputs of the pair.  At step != 1 both are lost -- a single pair with d != 1
 * sets batch_ok = 0 and drops the ENTIRE block to the per-frame loop, and each
 * d != 1 pair then runs the union-window kernel over (M+d)|1 taps, padding with
 * zeros that do no work.  That, not "fewer output samples", is the whole of the
 * measured +21 us (AB, d in {1,2}) and +8 us (BA, d in {0,1}) at 48<->32; the
 * output count per pull is APP_BLOCK_FRAMES == 16 at every rate.
 *
 * Here the blend is hoisted per OUTPUT FRAME and shared by all 16 channels, and
 * each channel is one plain 30-tap dot.  There is no pair, no d, no union
 * window, so there is nothing to lose: the per-output cost is the same at every
 * step.  That is the structural reason to expect the step != 1 penalty to
 * collapse, and Phase 7 measures whether it does.
 *
 * ---------------------------------------------------------------------------
 * PLACEMENT
 * ---------------------------------------------------------------------------
 * mchp_asrc_q31_row16 reads the history and the blended row as the two operands
 * of one MAC, so they must be in DIFFERENT spaces or both reads serialise and
 * the MAC silently costs an extra cycle (DS70005591C 4.3.17, measured elsewhere
 * in this tree as exactly 1.012 -> 2.000 cycles/MAC with no other symptom).
 * Only the 120-byte blended row needs forcing, which is why it is the one array
 * here carrying space(ymemory): the 15,480-byte coefficient table is read by
 * ordinary MCU-class loads during the blend, never by the MAC AGU, so it may
 * sit anywhere.  s_asrc -- and therefore the history -- stays in X.  Y could not
 * hold s_asrc in any case: 20,496 bytes against a 32,768-byte Y that already
 * carries the decimator's 12,160-byte history.
 *
 * NO MODULO.  asrc_push mirrors the first ASRC_POLY_M frames of every channel
 * ring, so each tap window is a contiguous span; both kernels use plain
 * post-increment addressing and never touch MODCON/XMODSRT/YMODSRT.  The
 * non-banked-modulo hazard documented in fir_ring_q31_ymod_yonly_dspic33ak.s
 * therefore does not apply to this kernel at all.
 */

/* --------------------------------------------------------- coefficients --- */

#include "asrc_poly_q31_table.h"

#if (ASRC_POLY_Q31_TABLE_L != ASRC_POLY_L) || (ASRC_POLY_Q31_TABLE_M != ASRC_POLY_M)
#  error "asrc_poly_q31_table.h was generated for a different L/M -- regenerate it with tools/asrc/asrc_q31_resampler_model.py table"
#endif

/*
 * The table is DESIGNED ON THE HOST IN float64 AND BAKED IN, unlike the float
 * arm which rebuilds its table at reset in float32.  This is not an
 * optimisation, it is what makes the bit-exactness proof possible: sinf() and
 * sqrtf() in XC-DSC and in the host model are each within an ulp of correct but
 * not identical, one ulp in a tap moves that row's DC normaliser, and the
 * normaliser divides EVERY tap of the row.  A target-generated table therefore
 * lands a few hundred Q31 LSB away from the host oracle (measured 2026-08-22:
 * 448 LSB at p=1,k=15, 2.8e-6 relative) and no oracle over it can be
 * bit-exact -- which is exactly how this was found: the coefficient probe
 * failed while the arithmetic was correct.
 *
 * It stays in RAM (initialised data, not const) so that RAM footprint and
 * coefficient-load cost remain directly comparable with the float arm; moving
 * it to flash would free 15,480 B and is recorded as an open item, not taken
 * here.
 */
static int32_t s_poly_q31[ASRC_POLY_L + 1u][ASRC_POLY_M] __attribute__(( aligned(4) )) =
{
    ASRC_POLY_Q31_TABLE_INIT
};

#define ASRC_POLY_Q31_ROW(p)  ((const int32_t*)&s_poly_q31[(p)][0])

/* The one array whose space is forced.  See PLACEMENT above. */
static int32_t s_ceff_q31[ASRC_POLY_M] __attribute__(( space(ymemory), aligned(4) ));

extern void mchp_asrc_q31_blend_row( const int32_t* c0, const int32_t* c1,
                                     int32_t* ceff, uint32_t taps, int32_t wbq );
extern void mchp_asrc_q31_row16( const int32_t* hist0, uint32_t stride_bytes,
                                 const int32_t* ceff, uint32_t taps,
                                 int32_t* out, uint32_t nch );

/*
 * Nothing to build: the table above is baked in (see its comment).  The entry
 * point is kept, and kept called from the selftest and from reset, because it is
 * where the image asserts that the table it carries is the table the host oracle
 * was generated against -- the float32 windowed-sinc design that used to live
 * here is gone, so this is the only remaining place a header/vector mismatch can
 * be caught on target rather than showing up as an unexplained probe failure.
 */
static void asrc_poly_build_q31( void )
{
    /* deliberately empty -- coefficients are initialised data */
}

/* ------------------------------------------------------- hot-path stages --- */

/*
 * Blend weight -> Q31.  Scaling by 2^31-1 rather than 2^31 keeps the result
 * inside int32 for every wb in [0,1) without a clamp on the hot path; the
 * 1-LSB31 shortfall as wb -> 1 is 4.7e-10 of the coefficient, three orders
 * below the quantisation floor.
 */
static inline int32_t asrc_q31_wb( float wb )
{
    const int32_t v = (int32_t)( wb * 2147483647.0f );
    return ( v < 0 ) ? 0 : v;
}

/* dec_q31_to_s24_left(): drop the eight sub-LSBs the TDM slot cannot carry. */
static inline int32_t asrc_q31_to_slot( int32_t v )
{
    return (int32_t)( (uint32_t)v & 0xFFFFFF00u );
}

/* Phase select, bit-identical to asrc_poly_phase() with
 * APP_ASRC_HEADROOM_INSTRUMENT == 1 (the setting these profiles build with). */
static inline uint32_t asrc_q31_phase_of( float frac, float* wb )
{
    const float pf = frac * (float)ASRC_POLY_L;
#if APP_ASRC_HEADROOM_INSTRUMENT
    uint32_t p = (uint32_t)(int32_t)pf;
#else
    uint32_t p = (uint32_t)pf;
#endif
    if( p >= ASRC_POLY_L ) { p = ASRC_POLY_L - 1u; }
    *wb = pf - (float)p;
    return p;
}

/* ------------------------------------------------- portable C reference ---- */
#if APP_ASRC_Q31_SELFTEST

#include "asrc_poly_q31_vectors.h"

/*
 * The oracle for the assembly above: no kernels, no accumulator model, no
 * placement assumptions, plain int64.  It rounds halves AWAY FROM ZERO where
 * sacr.l rounds per CORCON, so the two may legitimately differ by one Q31 LSB
 * on an exact tie -- which after the slot mask is either invisible or exactly
 * 256.  The selftest counts those separately and fails on anything else: the
 * same bar asrc_decimator_q31_selftest.inc already uses.
 *
 * sum is Q62 (undoubled products), the model the FIR kernel bench measured
 * against hardware at 0 LSB error.
 */
static int32_t asrc_q31_ref_sacr( int64_t sum )
{
    const int64_t v = ( sum + 0x40000000LL ) >> 31;
    if( v > 2147483647LL )  { return (int32_t)0x7FFFFFFF; }
    if( v < -2147483648LL ) { return (int32_t)0x80000000; }
    return (int32_t)v;
}

static void asrc_q31_ref_blend( const int32_t* c0, const int32_t* c1,
                                int32_t wbq, int32_t* ceff )
{
    for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
    {
        /*
         * wbq * (c1 - c0) is written out as wbq*c1 - wbq*c0 rather than as a
         * widened difference, and the add-back is done in 32 bits.  This is not
         * style: XC-DSC v3.31.01 at -O3 MIS-COMPILES
         * "(int64_t)c1[k] - (int64_t)c0[k]".  It forms the upper half of the
         * widened c1 with `se.w` -- a 16->32 sign extension of c1's LOW
         * HALFWORD -- where a 32->64 sign extension (asr #31) is required, so
         * the difference carries a bogus high word whenever bit 15 of c1
         * disagrees with its sign, and that high word then feeds the 64x64
         * multiply's cross term.  Measured on target 2026-08-22 with c0=-1155,
         * c1=-978, wbq=2119991296: -2114520020 instead of -980, and 53 % of all
         * taps wrong -- while the assembly kernel and the host model agreed with
         * each other.  Two int32 x int32 -> int64 products compile to mulss.d,
         * need no widening, and are exact (|wbq * c| < 2^62).
         */
        const int64_t p = (int64_t)wbq * (int64_t)c1[k]
                        - (int64_t)wbq * (int64_t)c0[k];
        ceff[k] = c0[k] + (int32_t)( ( p + 0x40000000LL ) >> 31 );
    }
}

static int32_t asrc_q31_ref_dot( const int32_t* x, const int32_t* ceff )
{
    int64_t sum = 0;
    for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
    {
        sum += (int64_t)ceff[k] * (int64_t)x[k];
    }
    return asrc_q31_ref_sacr( sum );
}

/* ------------------------------------------------------------- selftest ---- */
/*
 * Three proofs, kept separate on purpose (the instruction is explicit that they
 * must not be conflated):
 *
 *   A. the Q31 coefficient table on target == the host model's table
 *      (digest + 8 probes, so an ulp of difference in sinf/cosf/sqrtf reports
 *      as a COEFFICIENT mismatch rather than as 110 failing vectors);
 *   B. the portable C reference == the host Q31 integer model, bit-exact over
 *      all 110 (signal x step x phase) vectors;
 *   C. the assembly kernels == the portable C reference, sample-wise, under the
 *      0-or-exactly-256 tie policy -- including a 16-channel run at the real
 *      production stride to prove input chN -> output chN with no crosstalk.
 *
 * Failures are reported as numbers, not formatted strings: printf already has a
 * caller in asrc_audio_path.c and snprintf would not.  `what` is a literal, so
 * it costs no RAM.
 */
typedef struct
{
    const char* what;       /* NULL while everything passes                */
    uint32_t    vec;        /* vector index, or the probe/phase index      */
    uint32_t    idx;        /* sample or tap index                         */
    int32_t     got;
    int32_t     expect;
} asrc_poly_q31_fail_t;

static asrc_poly_q31_fail_t s_poly_q31_fail;
static uint32_t             s_poly_q31_ties;
static uint32_t             s_poly_q31_fails;   /* set by audio_app_asrc_reset_all() */

uint32_t asrc_poly_q31_selftest_fails( void )
{
    return s_poly_q31_fails;
}

const asrc_poly_q31_fail_t* asrc_poly_q31_selftest_fail( void )
{
    return &s_poly_q31_fail;
}

uint32_t asrc_poly_q31_selftest_ties( void )
{
    return s_poly_q31_ties;
}

#define ASRC_Q31_FNV_OFF  0xCBF29CE484222325ULL
#define ASRC_Q31_FNV_PRM  0x100000001B3ULL

static uint64_t asrc_q31_fnv_i32( uint64_t h, int32_t v )
{
    const uint32_t u = (uint32_t)v;
    for( uint32_t s = 0u; s < 32u; s += 8u )
    {
        h ^= (uint64_t)( ( u >> s ) & 0xFFu );
        h *= ASRC_Q31_FNV_PRM;
    }
    return h;
}

static void asrc_q31_note( const char* what, uint32_t vec, uint32_t idx,
                           int32_t got, int32_t expect )
{
    if( s_poly_q31_fail.what != (const char*)0 ) { return; }   /* keep the first */
    s_poly_q31_fail.what   = what;
    s_poly_q31_fail.vec    = vec;
    s_poly_q31_fail.idx    = idx;
    s_poly_q31_fail.got    = got;
    s_poly_q31_fail.expect = expect;
}

/* One channel through the portable C reference.  Mirrors the model's resample()
 * exactly: rd starts at MH, the window starts at rd - MH, and frac advances
 * after the output is emitted. */
static void asrc_q31_ref_stream( const int32_t* x, float step, float frac0,
                                 uint32_t nout, int32_t* y )
{
    uint32_t rd   = ASRC_POLY_MH;
    float    frac = frac0;
    int32_t  ceff[ASRC_POLY_M];

    for( uint32_t n = 0u; n < nout; n++ )
    {
        float          wb;
        const uint32_t p = asrc_q31_phase_of( frac, &wb );
        asrc_q31_ref_blend( ASRC_POLY_Q31_ROW( p ), ASRC_POLY_Q31_ROW( p + 1u ),
                            asrc_q31_wb( wb ), ceff );
        y[n] = asrc_q31_to_slot( asrc_q31_ref_dot( &x[rd - ASRC_POLY_MH], ceff ) );
        frac += step;
        while( frac >= 1.0f ) { frac -= 1.0f; rd++; }
    }
}

/* Proof A. */
static uint32_t asrc_q31_check_coeff( void )
{
    uint32_t fails = 0u;

    for( uint32_t i = 0u; i < ASRC_Q31_VEC_COEFF_PROBES; i++ )
    {
        const int32_t got = s_poly_q31[asrc_q31_vec_coeff_probe[i].p]
                                     [asrc_q31_vec_coeff_probe[i].k];
        if( got != asrc_q31_vec_coeff_probe[i].c )
        {
            asrc_q31_note( "coeff probe", i, asrc_q31_vec_coeff_probe[i].k,
                           got, asrc_q31_vec_coeff_probe[i].c );
            fails++;
        }
    }

    uint64_t h = ASRC_Q31_FNV_OFF;
    for( uint32_t p = 0u; p <= ASRC_POLY_L; p++ )
    {
        for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
        {
            h = asrc_q31_fnv_i32( h, s_poly_q31[p][k] );
        }
    }
    if( h != ASRC_Q31_VEC_COEFF_DIGEST )
    {
        asrc_q31_note( "coeff digest", 0u, 0u,
                       (int32_t)(uint32_t)h,
                       (int32_t)(uint32_t)ASRC_Q31_VEC_COEFF_DIGEST );
        fails++;
    }
    return fails;
}

/* Proofs B and C over the generated vectors. */
static uint32_t asrc_q31_check_vectors( void )
{
    uint32_t fails = 0u;
    int32_t  xbuf[ASRC_Q31_VEC_NIN];        /* stack, i.e. X space, as the kernel needs */
    int32_t  yref[ASRC_Q31_VEC_NOUT];
    int32_t  ceff_ref[ASRC_POLY_M];

    for( uint32_t v = 0u; v < ASRC_Q31_VEC_COUNT; v++ )
    {
        const asrc_q31_vec_t* const t = &asrc_q31_vectors[v];
        const int32_t* const  src     = asrc_q31_vec_input[(uint32_t)t->sig];

        for( uint32_t i = 0u; i < ASRC_Q31_VEC_NIN; i++ ) { xbuf[i] = src[i]; }

        /* B: portable C reference vs the host model's digest. */
        asrc_q31_ref_stream( xbuf, t->step, t->frac0, ASRC_Q31_VEC_NOUT, yref );

        uint64_t h = ASRC_Q31_FNV_OFF;
        for( uint32_t n = 0u; n < ASRC_Q31_VEC_NOUT; n++ )
        {
            h = asrc_q31_fnv_i32( h, yref[n] );
        }
        if( h != t->digest )
        {
            asrc_q31_note( "vec digest", v, 0u,
                           (int32_t)(uint32_t)h, (int32_t)(uint32_t)t->digest );
            fails++;
        }
        for( uint32_t n = 0u; n < 4u; n++ )
        {
            if( yref[n] != t->head[n] )
            {
                asrc_q31_note( "vec head", v, n, yref[n], t->head[n] );
                fails++;
            }
        }

        /* C: the assembly kernels against that same reference, sample by sample. */
        uint32_t rd   = ASRC_POLY_MH;
        float    frac = t->frac0;
        for( uint32_t n = 0u; n < ASRC_Q31_VEC_NOUT; n++ )
        {
            float          wb;
            const uint32_t p   = asrc_q31_phase_of( frac, &wb );
            const int32_t  wbq = asrc_q31_wb( wb );
            const int32_t* c0  = ASRC_POLY_Q31_ROW( p );
            const int32_t* c1  = ASRC_POLY_Q31_ROW( p + 1u );

            mchp_asrc_q31_blend_row( c0, c1, s_ceff_q31, ASRC_POLY_M, wbq );
            asrc_q31_ref_blend( c0, c1, wbq, ceff_ref );
            for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
            {
                const int32_t d = s_ceff_q31[k] - ceff_ref[k];
                if( ( d > 1 ) || ( d < -1 ) )
                {
                    asrc_q31_note( "blend", v, k, s_ceff_q31[k], ceff_ref[k] );
                    fails++;
                }
                else if( d != 0 ) { s_poly_q31_ties++; }
            }

            int32_t got;
            mchp_asrc_q31_row16( &xbuf[rd - ASRC_POLY_MH], 4u, s_ceff_q31,
                                 ASRC_POLY_M, &got, 1u );
            got = asrc_q31_to_slot( got );
            {
                const int32_t d = got - yref[n];
                if( ( d != 0 ) && ( d != 256 ) && ( d != -256 ) )
                {
                    asrc_q31_note( "row16", v, n, got, yref[n] );
                    fails++;
                }
                else if( d != 0 ) { s_poly_q31_ties++; }
            }

            frac += t->step;
            while( frac >= 1.0f ) { frac -= 1.0f; rd++; }
        }
    }
    return fails;
}

/*
 * 16-channel independence, at the REAL production stride.  Every channel gets
 * its own LCG stream, the kernel is driven exactly as the pull path drives it
 * (one blend per output frame, one call for all 16 channels), and each output is
 * compared against the single-channel C reference dot for that channel.  Any
 * crosstalk -- a stride error, a stale accumulator, a shared pointer -- shows up
 * as a mismatch on the channel that got contaminated.
 *
 * Scratch is s_asrc[ASRC_ENGINE_AB].ch, which is in X (what the kernel needs)
 * and is cleared by the asrc_reset() that runs after the selftests.  The
 * reference is evaluated inside the same loop as the kernel, so no 16 x nout
 * expectation array is needed and the frame stays small.
 */
static uint32_t asrc_q31_check_16ch( void )
{
    uint32_t  fails = 0u;
    asrc_t*   a     = &s_asrc[ASRC_ENGINE_AB];
    int32_t   ceff_ref[ASRC_POLY_M];
    int32_t   got[ASRC_CH];
    const uint32_t nin  = ASRC_Q31_VEC_NIN;
    const uint32_t nout = ASRC_Q31_VEC_NOUT;
    const float    step = 1.5f;                 /* step != 1, so the phase walks */

    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        uint32_t st = 0x1234567u + ( c * 0x9E3779B9u );
        for( uint32_t i = 0u; i < nin; i++ )
        {
            st = ( st * 1664525u ) + 1013904223u;
            a->ch[c][i] = (int32_t)( st & 0xFFFFFF00u );
        }
    }

    uint32_t rd   = ASRC_POLY_MH;
    float    frac = 0.0f;
    for( uint32_t n = 0u; n < nout; n++ )
    {
        float          wb;
        const uint32_t p = asrc_q31_phase_of( frac, &wb );
        const int32_t  wbq = asrc_q31_wb( wb );
        const uint32_t w   = rd - ASRC_POLY_MH;

        mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( p ), ASRC_POLY_Q31_ROW( p + 1u ),
                                 s_ceff_q31, ASRC_POLY_M, wbq );
        asrc_q31_ref_blend( ASRC_POLY_Q31_ROW( p ), ASRC_POLY_Q31_ROW( p + 1u ),
                            wbq, ceff_ref );

        mchp_asrc_q31_row16( &a->ch[0][w], (uint32_t)sizeof( a->ch[0] ),
                             s_ceff_q31, ASRC_POLY_M, got, ASRC_CH );

        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            const int32_t exp = asrc_q31_to_slot(
                                    asrc_q31_ref_dot( &a->ch[c][w], ceff_ref ) );
            const int32_t d   = asrc_q31_to_slot( got[c] ) - exp;
            if( ( d != 0 ) && ( d != 256 ) && ( d != -256 ) )
            {
                asrc_q31_note( "16ch", c, n, asrc_q31_to_slot( got[c] ), exp );
                fails++;
            }
            else if( d != 0 ) { s_poly_q31_ties++; }
        }

        frac += step;
        while( frac >= 1.0f ) { frac -= 1.0f; rd++; }
    }
    return fails;
}

uint32_t asrc_poly_q31_selftest( void )
{
    s_poly_q31_fail.what = (const char*)0;
    s_poly_q31_ties      = 0u;

    asrc_poly_build_q31();

    uint32_t fails = asrc_q31_check_coeff();
    if( fails == 0u )                       /* vectors are meaningless if the */
    {                                       /* table differs -- do not report */
        fails += asrc_q31_check_vectors();   /* 110 failures for one ulp       */
        fails += asrc_q31_check_16ch();
    }
    return fails;
}

#endif /* APP_ASRC_Q31_SELFTEST */
