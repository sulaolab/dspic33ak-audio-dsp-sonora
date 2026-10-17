/*
 * asrc_rear_isolation_selftest.inc -- PER-CHANNEL BIT-EXACT ORACLE FOR THE REAR Q31 POLYPHASE.
 *
 * TEST-ONLY.  Compiled out entirely unless APP_ASRC_REAR_ISOLATION_SELFTEST is 1, which it is not
 * by default: see the gate's comment in asrc_app_config.h.  Included from audio_app_asrc.c after
 * asrc_poly_q31.inc, so it sees the coefficient table, the two assembly kernels and the engine.
 *
 * ---------------------------------------------------------------------------
 * WHICH HOLE THIS CLOSES
 * ---------------------------------------------------------------------------
 * The FRONT END has had a per-channel bit-exact oracle since 2026-09-06
 * (asrc_decimator_q31_isolation_selftest, 16 distinct LCG streams, one channel verified per call,
 * no rounding-tie allowance).  The REAR polyphase had three weaker pieces instead:
 *
 *   - source: the loops are bounded by ASRC_CH and every channel has its own ring;
 *   - timing: an 8-channel rear would have to run at 5.92 cycles/MAC, which the kernel cannot;
 *   - asrc_q31_check_16ch(): the kernels vs a scalar reference, at 16 channels.
 *
 * The third is the closest, and it is still not this test, for two reasons that are exactly the
 * ones a channel bug hides behind:
 *
 *   1. IT DOES NOT USE THE PRODUCTION RING.  Its history is a local ASRC_CH x 66 array with its
 *      own small stride, no wrap and no mirror.  The shipping reader indexes
 *      `&a->ch[0][(rd - MH) & ASRC_FIFO_MASK]` at a 2,168-byte stride and depends on the
 *      producer's MIRROR of the first ASRC_POLY_M frames to keep the tap window contiguous across
 *      the wrap.  A stride, mask or mirror fault is invisible to a test that has none of them.
 *   2. IT WALKS ONE PHASE SEQUENCE.  frac starts at 0 and steps by 3/2, so the blend weight takes
 *      one family of values.  The phase cases that are easy to get wrong by one -- exactly on a
 *      row boundary, one sub-step past it, one sub-step short of the next -- are not among them.
 *
 * This test uses the production ring at the production stride, walks the phase through the awkward
 * cases on purpose, and compares EVERY channel against an oracle that shares no indexing, no
 * accumulator and no phase arithmetic with the shipping path.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELIBERATELY *NOT* CALLED, AND WHY THAT IS NOT A CHEAT
 * ---------------------------------------------------------------------------
 * asrc_pull() itself is not called.  Not for convenience: with ASRC_CH = 16 and
 * APP_SLOTS_PER_FS = 8 the scatter at the end of the pull writes only slots 0..7
 * (`d[s] = (s < ASRC_CH) ? out[s] : 0` over `s < APP_SLOTS_PER_FS`), so CHANNELS 8..15 NEVER REACH
 * `dst` -- their outputs are unobservable from outside the function.  A 16-channel per-channel
 * oracle driven through asrc_pull() could therefore only ever check half the width, and closing a
 * 16-channel hole with an 8-channel test is worse than leaving it open and saying so.  Changing the
 * pull to expose them was refused: this phase does not touch the production hot path.
 *
 * So the GOT side reproduces the pull's rear block VERBATIM -- same phase call, same wbase
 * expression, same two kernels, same stride argument, same s24-left mask, same frac/rd advance --
 * and the risk that comes with a verbatim copy is stated rather than hidden: IF THE PULL'S REAR
 * BLOCK CHANGES, THIS COPY MUST CHANGE WITH IT.  The `ASRC_REAR_ISO_MIRRORS` comment below lists
 * the five expressions that are copied, so a reviewer can diff them against asrc_pull() by name.
 * What the copy does NOT weaken is the property under test: the oracle is independent, so a
 * channel/indexing/arithmetic fault in the kernels, the ring layout, the mirror or the coefficient
 * blend still shows up here.
 *
 * ---------------------------------------------------------------------------
 * THE ORACLE'S INDEPENDENCE, ITEM BY ITEM
 * ---------------------------------------------------------------------------
 *   production                                   oracle
 *   ------------------------------------------   ----------------------------------------------
 *   ring index `(rd - MH) & ASRC_FIFO_MASK`      absolute frame number, `% ASRC_FIFO_FRAMES` per
 *   plus the producer's mirror overhang          tap.  Reads the ring for NOTHING -- it
 *                                                regenerates the sample from its generator.  So a
 *                                                broken mirror is a mismatch, not a shared bug.
 *   `asrc_q31_phase_of()` on a float frac        integer phase: p = num*L/DEN, wb = num*L%DEN
 *   `asrc_q31_wb()` (float multiply, truncate)   (wb_num << 31) / DEN in int64
 *   `mchp_asrc_q31_blend_row` (assembly)         scalar C blend, one tap at a time
 *   `mchp_asrc_q31_row16` (assembly, 16 ch,      scalar int64 dot per channel, taps accumulated in
 *   one AGU walk at the ring stride)             REVERSE order (equal by associativity, and it
 *                                                cannot inherit a walk-direction fault)
 *   `asrc_q31_to_slot()`                         its own mask
 *
 * WHICH KERNEL PAIR IS UNDER TEST.  The base pair (mchp_asrc_q31_blend_row / _row16), always --
 * never the APP_ASRC_Q31_OPT_KERNELS variants, even in an image that carries them.  That is
 * deliberate and it is not a gap: the opt pair's own selftest (asrc_q31_opt_selftest) already
 * compares it against the base pair BIT FOR BIT over the awkward inputs, so "opt equals base" and
 * "base is right per channel" are two separate claims proven in two separate places.  Testing the
 * opt pair here would make this test's result depend on a runtime kernel-selection bit, which is
 * exactly the coupling that selftest's own comment refuses.
 *
 * WHY THE BLEND IS STILL WRITTEN AS TWO PRODUCTS.  `(int64_t)c1[k] - (int64_t)c0[k]` is
 * MIS-COMPILED by XC-DSC v3.31.01 at -O3 (it sign-extends c1's low halfword; measured 2026-08-22,
 * 53 % of taps wrong).  The form below is a compiler constraint, not a shared implementation -- see
 * the long note in asrc_q31_ref_blend().
 *
 * ---------------------------------------------------------------------------
 * WHY A HASH AND NOT AN LCG CHAIN FOR THE TEST SIGNAL
 * ---------------------------------------------------------------------------
 * The oracle needs the sample at an ARBITRARY absolute frame index (that is how it avoids reading
 * the ring).  A per-channel LCG chain only gives sample n after n iterations, so an oracle built on
 * one would have to keep a per-channel history -- sixteen of them, which is the stack overflow that
 * forced the front-end test to verify one channel per call.  A random-access mixing function costs
 * O(1) per tap, so THIS test checks all sixteen channels in every output frame and still fits in
 * one small frame.  The generator is per-channel distinct and non-zero by construction, which is
 * the property the isolation claim actually needs.
 *
 * ---------------------------------------------------------------------------
 * WHY EVERY PHASE VALUE IS DYADIC
 * ---------------------------------------------------------------------------
 * frac and step are integers over ASRC_REAR_ISO_FRAC_DEN, a power of two, and the values stay below
 * 2.0 -- so `frac += step` in float32 on the got side is EXACT, and the oracle's integer phase is
 * provably the same number rather than approximately the same.  That is what lets this test demand
 * bit-exactness with NO rounding-tie allowance: a tie policy here would hide the channel faults it
 * exists to catch.  Non-dyadic phases are covered by the 110 generated vectors in
 * asrc_poly_q31.inc (Proof B/C), which is the right place for them.
 *
 * ---------------------------------------------------------------------------
 * STACK
 * ---------------------------------------------------------------------------
 * One frame, no recursion, no ISR overlap (boot-only, gated on
 * asrc_audio_path_isr_has_started()).  The arrays are, on the L=256/M=30 geometry:
 *
 *     ceff[30]      120 B    the blended row the kernels get (X space, so the MACs run at 2
 *                            cycles instead of 1.012 -- a timing property, not a numeric one, and
 *                            a boot-time test does not care)
 *     ceff_ref[30]  120 B    the oracle's own blended row
 *     xw[30]        120 B    the oracle's tap window, regenerated per channel per output
 *     out[16]        64 B    the kernel's 16 outputs
 *     dig[16]       128 B    per-channel FNV digest of this pass's outputs
 *     dig_ref[16]   128 B    the reference pass's digests, for the perturbation test
 *     ------------------
 *                   680 B  + scalars
 *
 * against 2,980 B of free stack on the Q31 arm (the figure in asrc_decimator_q31_selftest.inc).
 * The static assertion below fails the BUILD if the geometry ever makes those arrays big enough to
 * matter, because "it fitted last time" is not a stack analysis.  Nothing here is static, so the
 * default-off build pays no data either.
 */

/* The engine whose ring this test borrows.  BORROWED, not owned: audio_app_asrc_reset_all() calls
 * asrc_reset() on it a few lines after this test returns, and asrc_reset() zero-clears every
 * channel of the ring -- so the test leaves nothing behind.  It is legal only because the caller
 * has already established that no audio ISR has ever run (see the gate at the call site); a rear
 * ring borrowed after that point is being read by the pull ISR, which is forbidden. */
#define ASRC_REAR_ISO_ENGINE      (&s_asrc[ASRC_SELFTEST_ENGINE])

/* frac/step are integers over this denominator.  Power of two, at least ASRC_POLY_L (so one phase
 * row is a whole number of units) and small enough that num*L stays inside 32 bits. */
#define ASRC_REAR_ISO_FRAC_DEN    (4096u)
#define ASRC_REAR_ISO_PH          (ASRC_REAR_ISO_FRAC_DEN / ASRC_POLY_L)   /* units per phase row */

#define ASRC_REAR_ISO_OUTS        (24u)    /* output frames per case */

_Static_assert( ( ASRC_REAR_ISO_FRAC_DEN & ( ASRC_REAR_ISO_FRAC_DEN - 1u ) ) == 0u,
                "ASRC_REAR_ISO_FRAC_DEN must be a power of two, or the dyadic-exactness argument "
                "in asrc_rear_isolation_selftest.inc does not hold and the oracle is not exact" );
_Static_assert( ASRC_REAR_ISO_PH >= 2u,
                "ASRC_REAR_ISO_FRAC_DEN is too small for ASRC_POLY_L: the phase cases 'one unit "
                "past the boundary' and 'one unit short of the next' collapse into each other" );
_Static_assert( ( ASRC_REAR_ISO_PH % 2u ) == 0u,
                "ASRC_REAR_ISO_PH must be even, or the mid-phase case cannot be expressed exactly" );
_Static_assert( ( 6u * ASRC_POLY_M * sizeof( int32_t ) ) + ( 4u * ASRC_CH * sizeof( uint64_t ) )
                    < 1536u,
                "the rear isolation selftest's frame has grown past its stack analysis -- re-derive "
                "the budget in the file header before raising this bound" );

/* ASRC_REAR_ISO_MIRRORS: the five expressions copied verbatim out of asrc_pull()'s rear block.
 * Diff these by name if the pull changes.
 *   1. phase        asrc_q31_phase_of( a->frac, &wb )
 *   2. window       ( a->rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK
 *   3. blend        mchp_asrc_q31_blend_row( ROW(p), ROW(p+1), ceff, ASRC_POLY_M, asrc_q31_wb(wb) )
 *   4. dot          mchp_asrc_q31_row16( &a->ch[0][wbase],
 *                                        ASRC_FIFO_PHYS * sizeof(asrc_samp_t),
 *                                        ceff, ASRC_POLY_M, out, ASRC_CH )
 *   5. advance      frac += step; while( frac >= 1.0f ) { frac -= 1.0f; rd++; }   + out[] masking
 */

/* Which case, so a failure names the phase geometry rather than an index. */
typedef enum
{
    ASRC_REAR_ISO_PHASE_EXACT = 0,   /* wb == 0 on every output: the row boundary itself      */
    ASRC_REAR_ISO_PHASE_SMALL,       /* wb == 1/PH: one sub-step past the boundary            */
    ASRC_REAR_ISO_PHASE_MID,         /* wb == 1/2                                             */
    ASRC_REAR_ISO_PHASE_NEAR,        /* wb == (PH-1)/PH: one sub-step short of the next row   */
    ASRC_REAR_ISO_PHASE_WALK,        /* step not a multiple of a phase row: wb visits all of  */
                                     /* them, and rd advances one frame per output            */
    ASRC_REAR_ISO_RING_WRAP,         /* the tap window starts near the ring end, so it reads  */
                                     /* the producer's mirror overhang                        */
    ASRC_REAR_ISO_STEP_LT1,          /* step < 1: rd stands still on some outputs             */
    ASRC_REAR_ISO_RAILS,             /* history at +-full scale, so sacr.l saturates          */
    ASRC_REAR_ISO_CASES
} asrc_rear_iso_case_t;

static const char* asrc_rear_iso_case_name( uint8_t c )
{
    switch( (asrc_rear_iso_case_t)c )
    {
        case ASRC_REAR_ISO_PHASE_EXACT: return "phase-exact";
        case ASRC_REAR_ISO_PHASE_SMALL: return "phase-small";
        case ASRC_REAR_ISO_PHASE_MID:   return "phase-mid";
        case ASRC_REAR_ISO_PHASE_NEAR:  return "phase-near-next";
        case ASRC_REAR_ISO_PHASE_WALK:  return "phase-walk";
        case ASRC_REAR_ISO_RING_WRAP:   return "ring-wrap-mirror";
        case ASRC_REAR_ISO_STEP_LT1:    return "step-below-one";
        case ASRC_REAR_ISO_RAILS:       return "rails-saturate";
        default:                        return "?";
    }
}

/* One case: where frac starts, how far it steps, where rd starts, and which signal generator the
 * ring is filled from.  All three phase numbers are in ASRC_REAR_ISO_FRAC_DEN units. */
typedef struct
{
    uint32_t frac0_num;
    uint32_t step_num;
    uint32_t rd0;
    uint8_t  rails;
} asrc_rear_iso_case_def_t;

/* rd0 for the wrap case: the FIRST tap of the window sits ASRC_POLY_M-8 frames before the ring's
 * end, so 8 taps come from the ring proper and the remaining M-8 from the mirror overhang.  Written
 * as MH + (FRAMES - 8) because the reader subtracts MH again. */
#define ASRC_REAR_ISO_RD0_WRAP    ( ASRC_POLY_MH + ( ASRC_FIFO_FRAMES - 8u ) )
#define ASRC_REAR_ISO_RD0_MID     ( ASRC_POLY_MH + ( ASRC_FIFO_FRAMES / 4u ) )

static const asrc_rear_iso_case_def_t asrc_rear_iso_cases[ASRC_REAR_ISO_CASES] =
{
    /* frac0                          step                                   rd0                      rails */
    { 0u,                             ASRC_REAR_ISO_FRAC_DEN + 3u * ASRC_REAR_ISO_PH, ASRC_REAR_ISO_RD0_MID,  0u },
    { 1u,                             ASRC_REAR_ISO_FRAC_DEN + 3u * ASRC_REAR_ISO_PH, ASRC_REAR_ISO_RD0_MID,  0u },
    { ASRC_REAR_ISO_PH / 2u,          ASRC_REAR_ISO_FRAC_DEN + 5u * ASRC_REAR_ISO_PH, ASRC_REAR_ISO_RD0_MID,  0u },
    { ASRC_REAR_ISO_PH - 1u,          ASRC_REAR_ISO_FRAC_DEN + 3u * ASRC_REAR_ISO_PH, ASRC_REAR_ISO_RD0_MID,  0u },
    { 0u,                             ASRC_REAR_ISO_FRAC_DEN + 37u,                   ASRC_REAR_ISO_RD0_MID,  0u },
    { 3u,                             ASRC_REAR_ISO_FRAC_DEN + 37u,                   ASRC_REAR_ISO_RD0_WRAP, 0u },
    { ASRC_REAR_ISO_PH / 2u,          ASRC_REAR_ISO_FRAC_DEN - 37u,                   ASRC_REAR_ISO_RD0_MID,  0u },
    { ASRC_REAR_ISO_PH / 2u,          ASRC_REAR_ISO_FRAC_DEN + 5u * ASRC_REAR_ISO_PH, ASRC_REAR_ISO_RD0_MID,  1u },
};

/* Failure record.  Numbers, not a formatted string: printf already has a single caller and adding
 * snprintf for a diagnostic would cost ROM in every build that keeps the caller (the printf
 * single-caller note in the ROM diet report). */
typedef struct
{
    const char* what;      /* NULL while everything passes                                  */
    uint8_t     kase;
    uint8_t     channel;   /* the channel that disagreed, or the perturbed one               */
    uint8_t     other;     /* for isolation failures: the channel whose output moved         */
    uint16_t    frame;     /* output frame within the case                                   */
    uint32_t    phase;     /* the phase row this output used                                 */
    uint32_t    wbq;       /* and its blend weight, so the case can be reproduced on a host  */
    int32_t     got;
    int32_t     want;
    uint32_t    xor_bits;  /* got ^ want: says whether it is one LSB or a different channel  */
} asrc_rear_iso_fail_t;

static asrc_rear_iso_fail_t s_rear_iso_fail;
static uint32_t             s_rear_iso_fails;
static uint32_t             s_rear_iso_checks;    /* channel-output comparisons actually made */
static uint32_t             s_rear_iso_ch_seen;   /* bitmap: which channels were compared     */

uint32_t asrc_rear_isolation_selftest_fails( void )  { return s_rear_iso_fails; }
uint32_t asrc_rear_isolation_selftest_checks( void ) { return s_rear_iso_checks; }
uint32_t asrc_rear_isolation_selftest_channels( void )
{
    uint32_t n = 0u;
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        if( ( s_rear_iso_ch_seen & ( 1u << c ) ) != 0u ) { n++; }
    }
    return n;
}
const asrc_rear_iso_fail_t* asrc_rear_isolation_selftest_fail( void )
{
    return &s_rear_iso_fail;
}

static void asrc_rear_iso_note( const char* what, uint8_t kase, uint8_t channel, uint8_t other,
                                uint16_t frame, uint32_t phase, uint32_t wbq,
                                int32_t got, int32_t want )
{
    s_rear_iso_fails++;
    if( s_rear_iso_fail.what != (const char*)0 ) { return; }   /* keep the first */
    s_rear_iso_fail.what     = what;
    s_rear_iso_fail.kase     = kase;
    s_rear_iso_fail.channel  = channel;
    s_rear_iso_fail.other    = other;
    s_rear_iso_fail.frame    = frame;
    s_rear_iso_fail.phase    = phase;
    s_rear_iso_fail.wbq      = wbq;
    s_rear_iso_fail.got      = got;
    s_rear_iso_fail.want     = want;
    s_rear_iso_fail.xor_bits = (uint32_t)got ^ (uint32_t)want;
}

/* ------------------------------------------------------- the test signal --- */
/*
 * S(ch, frame, salt): the sample channel `ch` carries at ABSOLUTE frame `frame`.  Random access, so
 * the oracle never needs a history (see the file header).  Two properties are required and both are
 * checked by the sanity pass below rather than argued for: every channel's stream is non-zero, and
 * no two channels share a sample at the same frame.
 *
 * `salt` is what the perturbation test changes for ONE channel, so the same generator produces the
 * "different audio" pass without a second code path.
 *
 * The result is masked to s24-left because that is what the producer stores (asrc_ring_conv), so
 * the test drives the rear with words the codec could actually have delivered.
 */
static int32_t asrc_rear_iso_sample( uint32_t ch, uint32_t frame, uint32_t salt )
{
    uint32_t h = ( frame * 2654435761u ) + ( ch * 0x9E3779B9u ) + 0x0A5A0001u + salt;
    h ^= ( h >> 15 );
    h *= 2246822519u;
    h ^= ( h >> 13 );
    h *= 3266489917u;
    h ^= ( h >> 16 );
    /* Never zero: OR in a bit above the s24-left mask boundary, so a channel cannot go silent and
     * make the isolation claim vacuous for that frame. */
    h |= 0x00010000u;
    return (int32_t)( h & 0xFFFFFF00u );
}

/* The rails variant: alternate between the two Q31 extremes, with the pattern rotated per channel
 * so the channels still differ.  sum|c| over a coefficient row is 2.009, so this drives the
 * accumulator past full scale and makes sacr.l saturate -- the boundary the s24-left store sits on.
 */
static int32_t asrc_rear_iso_sample_rail( uint32_t ch, uint32_t frame, uint32_t salt )
{
    const uint32_t bit = ( frame + ch + salt ) & 1u;
    return ( bit != 0u ) ? (int32_t)0x7FFFFF00 : (int32_t)0x80000000;
}

static int32_t asrc_rear_iso_gen( uint8_t rails, uint32_t ch, uint32_t frame, uint32_t salt )
{
    return ( rails != 0u ) ? asrc_rear_iso_sample_rail( ch, frame, salt )
                           : asrc_rear_iso_sample( ch, frame, salt );
}

/* ------------------------------------------------------------- the oracle --- */
/*
 * Q31 round-and-saturate at bit 31, with CONVERGENT (round-half-to-even) rounding.
 *
 * ★ THIS IS THE ONE PLACE THIS ORACLE DOES *NOT* COPY THE OTHER Q31 ORACLES, AND IT IS WHY IT CAN
 * DEMAND BIT-EXACTNESS.  asrc_poly_q31.inc's asrc_q31_ref_sacr() and the front end's
 * dec_q31_st_sacr() both compute `(sum + 2^30) >> 31`, which rounds halves toward +infinity.  The
 * hardware does not: `fractsetup` leaves CORCON in convergent rounding (stated in the ROUNDING note
 * at the head of asrc_decimator_q31_selftest.inc), so on an exact half sacr.l rounds to the EVEN
 * Q31 value.  Those two conventions disagree by one LSB31 exactly at a tie, and the s24-left mask
 * turns that into a difference of exactly 256 whenever the tie straddles a mask boundary.  That is
 * the whole reason the other Q31 tests carry a "+-256 is a tie, not a failure" allowance.
 *
 * An allowance is the wrong instrument for THIS test: it would forgive the one-LSB end of exactly
 * the channel faults the test exists to catch.  So the oracle implements the hardware's rounding
 * instead, the tie class disappears, and bit-exactness becomes the honest bar rather than an
 * aspiration with an exception attached.
 *
 * MEASURED, not assumed.  The first hardware run of this test (2026-09-16, member A) FAILED with
 * exactly one difference in 1,152 comparisons: `phase-mid`, ch 1, frame 1, got 0x4A886B00 want
 * 0x4A886C00, i.e. the hardware's Q31 result was ONE LSB BELOW this oracle's round-half-up value
 * and the pre-mask words were 0x4A886BFF vs 0x4A886C00.  Rounding the tie to even reproduces the
 * hardware; rounding it up does not.  If the silicon were NOT convergent, this form would disagree
 * on many samples rather than one, so the change is self-checking in the safe direction.
 *
 * Written out with an explicit floor and remainder rather than as an adjusted shift: the tie case
 * has to be nameable to be right, and `sum >> 31` is an arithmetic shift (floor) for both signs, so
 * `sum & 0x7FFFFFFF` is the non-negative remainder above that floor for negative sums too.
 */
static int32_t asrc_rear_iso_sacr( int64_t sum )
{
    int64_t       v   = sum >> 31;                  /* floor(sum / 2^31), both signs */
    const int64_t rem = sum & 0x7FFFFFFFLL;         /* 0 <= rem < 2^31               */

    if( rem > 0x40000000LL )
    {
        v += 1;                                     /* nearer the next value up */
    }
    else if( rem == 0x40000000LL )
    {
        if( ( v & 1LL ) != 0LL ) { v += 1; }        /* exact half -> to even    */
    }

    if( v >  2147483647LL ) { return (int32_t)0x7FFFFFFF; }
    if( v < -2147483648LL ) { return (int32_t)0x80000000; }
    return (int32_t)v;
}

/*
 * ceff[k] = c0[k] + round( wbq * (c1[k] - c0[k]) ), the rounding at bit 31.
 *
 * TWO PRODUCTS, NOT A WIDENED DIFFERENCE.  `(int64_t)c1[k] - (int64_t)c0[k]` is mis-compiled by
 * XC-DSC v3.31.01 at -O3: it forms the high word of the widened c1 with `se.w`, a 16->32 sign
 * extension of c1's LOW HALFWORD, where a 32->64 extension is required.  Measured on target
 * 2026-08-22 with c0=-1155, c1=-978, wbq=2119991296: -2114520020 instead of -980, 53 % of taps
 * wrong, while the assembly kernel and the host model agreed with each other.  So this form is a
 * COMPILER CONSTRAINT and not a shared implementation with the kernel -- the full note lives in
 * asrc_q31_ref_blend() in asrc_poly_q31.inc.
 *
 * ★ THE ACCUMULATOR IS MODELLED, NOT SHORT-CUT.  The kernel is `lac.l c0 / mac.l wbq,delta /
 * sacr.l`, so c0 is inside the accumulator when the single rounding happens.  Under convergent
 * rounding that matters: the tie is broken on the parity of the RESULT, which is
 * c0 + floor(wbq*delta / 2^31) -- not on the parity of the product's quotient alone.  Rounding the
 * product first and adding c0 afterwards (what asrc_q31_ref_blend does) therefore picks the wrong
 * side of a tie whenever c0 is odd, which is why that reference needs a +-1 LSB allowance on the
 * blend and this one does not.  `c0 << 31` cannot overflow int64 here: |c| <= 0.93 so the true
 * accumulator value stays below 2^62, and the blended result is convex in (c0,c1).
 */
static void asrc_rear_iso_blend( const int32_t* c0, const int32_t* c1, int32_t wbq, int32_t* ceff )
{
    for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
    {
        const int64_t p = (int64_t)wbq * (int64_t)c1[k]
                        - (int64_t)wbq * (int64_t)c0[k];
        ceff[k] = asrc_rear_iso_sacr( ( (int64_t)c0[k] << 31 ) + p );
    }
}

/* The dot, accumulated from the LAST tap backwards.  Integer addition is associative so the value
 * is identical, and the reversed walk cannot inherit a direction fault from the kernel's AGU. */
static int32_t asrc_rear_iso_dot( const int32_t* x, const int32_t* ceff )
{
    int64_t sum = 0;
    uint32_t k = ASRC_POLY_M;
    while( k != 0u )
    {
        k--;
        sum += (int64_t)ceff[k] * (int64_t)x[k];
    }
    return asrc_rear_iso_sacr( sum );
}

/* The oracle's phase: integers only.  p = floor(num*L/DEN), wb = (num*L mod DEN)/DEN, and the Q31
 * weight is (wb_num << 31)/DEN -- exact, because DEN is a power of two.  The shipping path computes
 * the same three numbers in float32 (asrc_q31_phase_of / asrc_q31_wb); every value this test uses
 * is dyadic, so "the same number" is provable rather than approximate (file header). */
static uint32_t asrc_rear_iso_phase( uint32_t frac_num, int32_t* wbq )
{
    const uint32_t pf = frac_num * ASRC_POLY_L;
    uint32_t       p  = pf / ASRC_REAR_ISO_FRAC_DEN;
    const uint32_t wn = pf % ASRC_REAR_ISO_FRAC_DEN;
    if( p >= ASRC_POLY_L ) { p = ASRC_POLY_L - 1u; }
    *wbq = (int32_t)( ( (int64_t)wn << 31 ) / (int64_t)ASRC_REAR_ISO_FRAC_DEN );
    return p;
}

#define ASRC_REAR_ISO_FNV_OFF  0xCBF29CE484222325ULL
#define ASRC_REAR_ISO_FNV_PRM  0x100000001B3ULL

static uint64_t asrc_rear_iso_fnv( uint64_t h, int32_t v )
{
    const uint32_t u = (uint32_t)v;
    for( uint32_t s = 0u; s < 32u; s += 8u )
    {
        h ^= (uint64_t)( ( u >> s ) & 0xFFu );
        h *= ASRC_REAR_ISO_FNV_PRM;
    }
    return h;
}

/* ------------------------------------------------------------ ring filling --- */
/*
 * Fill the whole ring, plus the producer's mirror overhang, from the generator.  This is the ONE
 * place the test writes production state, and it writes it exactly the way the producer does:
 * `ch[c][i]` for i < ASRC_FIFO_FRAMES, mirrored at `ch[c][i + ASRC_FIFO_FRAMES]` for
 * i < ASRC_POLY_M.  The mirror is filled from the SAME generator call, not copied from the ring, so
 * a mirror the reader depends on is present without this function having to agree with the
 * producer's own copy loop.
 *
 * `only_ch` is ASRC_CH for "all channels" or a channel index for the perturbation pass, so the
 * perturbation rewrites one channel and leaves the other fifteen byte-identical.
 */
static void asrc_rear_iso_fill_ring( asrc_t* a, uint8_t rails, uint32_t salt, uint32_t only_ch )
{
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        if( ( only_ch < ASRC_CH ) && ( c != only_ch ) ) { continue; }
        const uint32_t s = ( only_ch < ASRC_CH ) ? salt : 0u;
        for( uint32_t i = 0u; i < ASRC_FIFO_FRAMES; i++ )
        {
            const int32_t v = asrc_rear_iso_gen( rails, c, i, s );
            a->ch[c][i] = (asrc_samp_t)v;
            if( i < ASRC_POLY_M ) { a->ch[c][i + ASRC_FIFO_FRAMES] = (asrc_samp_t)v; }
        }
    }
}

/* The two properties the isolation claim needs from the generator, checked instead of asserted:
 * no channel is silent at any frame the taps can reach, and no two channels share a sample there. */
static uint32_t asrc_rear_iso_check_signal( uint8_t rails, uint32_t salt )
{
    uint32_t fails = 0u;
    for( uint32_t i = 0u; i < 64u; i++ )
    {
        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            const int32_t v = asrc_rear_iso_gen( rails, c, i, salt );
            if( v == 0 )
            {
                asrc_rear_iso_note( "signal-zero", 0xFFu, (uint8_t)c, 0xFFu, (uint16_t)i,
                                    0u, 0u, v, 1 );
                fails++;
            }
            for( uint32_t d = c + 1u; d < ASRC_CH; d++ )
            {
                if( asrc_rear_iso_gen( rails, d, i, salt ) == v )
                {
                    /* The rails generator repeats by construction (two values), so a collision
                     * there is expected and only the non-rails generator must be injective. */
                    if( rails != 0u ) { continue; }
                    asrc_rear_iso_note( "signal-collision", 0xFFu, (uint8_t)c, (uint8_t)d,
                                        (uint16_t)i, 0u, 0u, v, v );
                    fails++;
                }
            }
        }
    }
    return fails;
}

/* --------------------------------------------------------------- one case --- */
/*
 * Run one case and compare EVERY channel of EVERY output frame against the oracle.
 *
 * `dig` (optional) receives a per-channel FNV digest of that channel's outputs, which is what the
 * perturbation pass compares: a digest over all ASRC_REAR_ISO_OUTS outputs is a stronger statement
 * than "the last frame matched" and costs 8 bytes per channel instead of 24 frames of them.
 */
static uint32_t asrc_rear_iso_run_case( asrc_t* a, uint8_t kase, uint32_t salt, uint32_t only_ch,
                                        uint64_t* dig, uint8_t compare )
{
    const asrc_rear_iso_case_def_t* const cd = &asrc_rear_iso_cases[kase];
    uint32_t fails = 0u;

    asrc_rear_iso_fill_ring( a, cd->rails, salt, only_ch );

    int32_t ceff[ASRC_POLY_M];
    int32_t ceff_ref[ASRC_POLY_M];
    int32_t xw[ASRC_POLY_M];
    int32_t out[ASRC_CH];

    if( dig != (uint64_t*)0 )
    {
        for( uint32_t c = 0u; c < ASRC_CH; c++ ) { dig[c] = ASRC_REAR_ISO_FNV_OFF; }
    }

    /* The got side's state, in the production representation. */
    uint32_t rd   = cd->rd0;
    float    frac = (float)cd->frac0_num / (float)ASRC_REAR_ISO_FRAC_DEN;
    const float step = (float)cd->step_num / (float)ASRC_REAR_ISO_FRAC_DEN;
    /* The oracle's state, in its own integer representation.  Advanced independently below, and the
     * two are required to agree implicitly: if they diverge the outputs stop matching. */
    uint32_t rd_ref   = cd->rd0;
    uint32_t frac_num = cd->frac0_num;

    for( uint16_t n = 0u; n < (uint16_t)ASRC_REAR_ISO_OUTS; n++ )
    {
        /* ---- got: verbatim from asrc_pull()'s rear block (ASRC_REAR_ISO_MIRRORS 1-4) ---- */
        float          wb;
        const uint32_t pq    = asrc_q31_phase_of( frac, &wb );
        const uint32_t wbase = ( rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
        mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( pq ), ASRC_POLY_Q31_ROW( pq + 1u ),
                                 ceff, ASRC_POLY_M, asrc_q31_wb( wb ) );
        mchp_asrc_q31_row16( &a->ch[0][wbase],
                             (uint32_t)( ASRC_FIFO_PHYS * sizeof( asrc_samp_t ) ),
                             ceff, ASRC_POLY_M, out, ASRC_CH );
        for( uint32_t c = 0u; c < ASRC_CH; c++ ) { out[c] = asrc_q31_to_slot( out[c] ); }

        /* ---- oracle: its own phase, its own blend, its own indexing, its own accumulator ---- */
        int32_t        wbq_ref;
        const uint32_t p_ref = asrc_rear_iso_phase( frac_num, &wbq_ref );
        asrc_rear_iso_blend( ASRC_POLY_Q31_ROW( p_ref ), ASRC_POLY_Q31_ROW( p_ref + 1u ),
                             wbq_ref, ceff_ref );

        /* The absolute frame the first tap belongs to.  Note what is NOT here: no ring mask, no
         * mirror, no ASRC_FIFO_PHYS.  The oracle reduces modulo the ring LENGTH only because the
         * test filled the ring periodically, and it regenerates the sample rather than reading it. */
        const uint32_t w_ref = ( rd_ref - ASRC_POLY_MH );

        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            const uint32_t s = ( ( only_ch < ASRC_CH ) && ( c == only_ch ) ) ? salt : 0u;
            for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
            {
                xw[k] = asrc_rear_iso_gen( cd->rails, c,
                                           ( w_ref + k ) % ASRC_FIFO_FRAMES, s );
            }
            const int32_t want = asrc_q31_to_slot( asrc_rear_iso_dot( xw, ceff_ref ) );
            const int32_t got  = out[c];

            if( dig != (uint64_t*)0 ) { dig[c] = asrc_rear_iso_fnv( dig[c], got ); }

            if( compare != 0u )
            {
                s_rear_iso_checks++;
                s_rear_iso_ch_seen |= ( 1u << c );
                /* NO TIE ALLOWANCE: a difference of any size is a failure here.
                 *
                 * The other Q31 tests allow exactly +-256 because their reference rounds halves the
                 * other way from the silicon (see asrc_rear_iso_sacr).  This oracle implements the
                 * hardware's convergent rounding in both the blend and the dot, so there is no tie
                 * class left to forgive -- which is the point: an allowance would forgive the
                 * one-LSB end of exactly the channel faults this test exists to catch.
                 *
                 * A +-256 difference is still reported under its OWN name.  It is a failure like
                 * any other, but the log says "value-tie" rather than "value", so a rounding
                 * disagreement is diagnosable from one line instead of being read as crosstalk.
                 * That is not hypothetical: it is how the FIRST hardware run of this test was
                 * diagnosed in one reading (1 of 1,152, phase-mid, ch 1) instead of sending someone
                 * to look for a stride bug. */
                if( got != want )
                {
                    const int32_t d = got - want;
                    asrc_rear_iso_note( ( ( d == 256 ) || ( d == -256 ) ) ? "value-tie" : "value",
                                        kase, (uint8_t)c, 0xFFu, n,
                                        p_ref, (uint32_t)wbq_ref, got, want );
                    fails++;
                    if( fails >= 8u ) { return fails; }   /* the first few say everything */
                }
            }
        }

        /* ---- advance: got side in float (ASRC_REAR_ISO_MIRRORS 5), oracle side in integers ---- */
        frac += step;
        while( frac >= 1.0f ) { frac -= 1.0f; rd++; }
        frac_num += cd->step_num;
        while( frac_num >= ASRC_REAR_ISO_FRAC_DEN ) { frac_num -= ASRC_REAR_ISO_FRAC_DEN; rd_ref++; }
    }
    return fails;
}

/* ------------------------------------------------------- the perturbation --- */
/*
 * CHANNEL ISOLATION, the other direction.  The per-output comparison above proves that channel c's
 * output equals the oracle of channel c's input; this proves the converse -- that changing ONE
 * channel's input moves ONLY that channel's output.  The two fail differently: a kernel that
 * summed two channels' taps would fail the first, while one that wrote its outputs one slot over
 * could pass the first at every channel it happened to check and still fail this.
 *
 * The perturbed channel is required to CHANGE.  Without that check a perturbation that never
 * reached the arithmetic (a fill that wrote the wrong ring, say) would make the whole test
 * vacuously pass.
 */
static uint32_t asrc_rear_iso_perturb( asrc_t* a, uint8_t kase )
{
    uint32_t fails = 0u;
    uint64_t dig_ref[ASRC_CH];
    uint64_t dig[ASRC_CH];

    /* Reference pass: all channels from salt 0.  No comparison -- the oracle comparison already
     * ran for this case; here only the digests matter. */
    (void)asrc_rear_iso_run_case( a, kase, 0u, ASRC_CH, dig_ref, 0u );

    /* Ends, both sides of the 8-channel slot boundary, and one interior channel: a scatter or
     * stride fault shows at a boundary, and 15 -> 0 wrap-around shows at the ends. */
    static const uint8_t victims[] = { 0u, 1u, 7u, 8u, 15u };
    for( uint32_t vi = 0u; vi < ( sizeof( victims ) / sizeof( victims[0] ) ); vi++ )
    {
        const uint8_t v = victims[vi];
        if( v >= ASRC_CH ) { continue; }

        (void)asrc_rear_iso_run_case( a, kase, 0x5A17u + (uint32_t)v, v, dig, 0u );

        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            if( c == (uint32_t)v )
            {
                if( dig[c] == dig_ref[c] )
                {
                    asrc_rear_iso_note( "perturb-inert", kase, v, (uint8_t)c, 0u, 0u, 0u,
                                        (int32_t)(uint32_t)dig[c],
                                        (int32_t)(uint32_t)dig_ref[c] );
                    fails++;
                }
            }
            else if( dig[c] != dig_ref[c] )
            {
                asrc_rear_iso_note( "crosstalk", kase, v, (uint8_t)c, 0u, 0u, 0u,
                                    (int32_t)(uint32_t)dig[c], (int32_t)(uint32_t)dig_ref[c] );
                fails++;
            }
        }
        /* Put the victim's ring back, so the next victim starts from the reference signal. */
        asrc_rear_iso_fill_ring( a, asrc_rear_iso_cases[kase].rails, 0u, v );
    }
    return fails;
}

/* ------------------------------------------------------------------ entry --- */
/*
 * Returns the failure count; 0 is a pass.  The caller prints, because printf has one caller in this
 * application and a second one here would cost ROM in every image that keeps it.
 *
 * The ring belongs to the live engine, so the CALLER must have established that no audio ISR has
 * ever run.  audio_app_asrc_reset_all() zero-clears the ring a few lines later, which is this
 * test's whole cleanup.
 */
uint32_t asrc_rear_isolation_selftest( void )
{
    asrc_t* const a = ASRC_REAR_ISO_ENGINE;

    s_rear_iso_fail.what = (const char*)0;
    s_rear_iso_fails     = 0u;
    s_rear_iso_checks    = 0u;
    s_rear_iso_ch_seen   = 0u;

    /* The generator first: if the test signal were degenerate every later result would be
     * meaningless in the direction that looks like a pass. */
    if( asrc_rear_iso_check_signal( 0u, 0u ) != 0u ) { return s_rear_iso_fails; }

    for( uint8_t kase = 0u; kase < (uint8_t)ASRC_REAR_ISO_CASES; kase++ )
    {
        if( asrc_rear_iso_run_case( a, kase, 0u, ASRC_CH, (uint64_t*)0, 1u ) != 0u )
        {
            return s_rear_iso_fails;   /* STOP at the first mismatching case */
        }
    }

    /* The perturbation pass is expensive (6 runs per case), so it runs on the two cases that cover
     * the two distinct read geometries: the ordinary interior window and the mirrored one. */
    if( asrc_rear_iso_perturb( a, (uint8_t)ASRC_REAR_ISO_PHASE_WALK ) != 0u )
    {
        return s_rear_iso_fails;
    }
    if( asrc_rear_iso_perturb( a, (uint8_t)ASRC_REAR_ISO_RING_WRAP ) != 0u )
    {
        return s_rear_iso_fails;
    }

    return s_rear_iso_fails;
}
