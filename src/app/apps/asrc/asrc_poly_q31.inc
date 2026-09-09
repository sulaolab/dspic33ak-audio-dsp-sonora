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

/* One baked table per geometry, selected on M.  The M30 table is the shipping
 * default and is byte-identical to what it always was; the M28 table exists for
 * the 96 kHz profile (APP_ASRC_EXPERIMENTAL_M28).  Both are emitted by
 *   tools/asrc/asrc_q31_resampler_model.py table --taps <M> --fc <fc> --beta <b>
 * and carry fc/beta in their banner, which is the ONLY place those two are
 * recorded on the target: the #error below can check L and M, but a table
 * designed with the wrong fc or beta is a well-formed table and cannot be
 * detected here.  Read the banner when adding one. */
#if   ( ASRC_POLY_M == 30u )
#  include "asrc_poly_q31_table.h"
#elif ( ASRC_POLY_M == 28u )
#  include "asrc_poly_q31_table_m28.h"
#else
#  error "no baked Q31 coefficient table for this ASRC_POLY_M -- generate one with tools/asrc/asrc_q31_resampler_model.py table --taps <ASRC_POLY_M> --fc <ASRC_POLY_FC> --beta <ASRC_POLY_KAISER_BETA> and select it above"
#endif

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
 * STORAGE IS ITS OWN KNOB -- ASRC_Q31_COEFF_STORAGE -- AND IT DEFAULTS TO RAM.
 *
 *   RAM   (default) : initialised data, writable.  blend_row reads this table once
 *           per output frame, and RAM reads are what make that cheap.
 *   FLASH : the table becomes const, so the toolchain leaves it resident in program
 *           flash and nothing is copied into RAM at startup.  Frees 15,480 B of data
 *           memory; the bits, the geometry and the arithmetic are unchanged (same
 *           3714 rounding ties).  Select with -Define ASRC_Q31_COEFF_STORAGE=1.
 *
 * WHY IT IS NOT TIED TO ASRC_COEFF_STORAGE, AND WHY THE DEFAULT IS RAM.  It followed
 * that knob for one day (2026-09-07), which put this table in flash on every BIDIR
 * preset.  The owner's decision on the measured result is the opposite: keep the RAM
 * speed now and hold the 15,480 B as a future option.  Measured on hardware at
 * 48000 -> 32000 Hz with ASRC_CH = 16, worst 1 ms window over a long dwell
 * (`[internal] report_ak512_48k32k_16ch_reserve_2026-09-07.md`, section 9):
 *
 *   RAM   : max_demand 74.4 %, data memory 92 %
 *   FLASH : max_demand 87.6 %, data memory 68 %
 *
 * i.e. +13.2 pt of the CPU window -- MORE than the float arm's ~+6.4 pt, not less.
 * The prediction that stood here before said the opposite, and the reason it was
 * wrong is worth keeping: blend_row does amortise the slower reads over ASRC_CH
 * (it builds s_ceff_q31 once per output frame, and all channels then MAC out of
 * RAM), but the denominator is not the whole story -- it still reads 2 rows x 30
 * taps = 60 words PER OUTPUT FRAME at these rates, and that absolute traffic
 * dominates after the division.  Do not re-derive either arm's figure from the
 * shape of the code; both came from hardware dwells.
 *
 * Nothing writes this table (the only readers are blend_row, the coefficient probe
 * and the table hash), so const costs no code when FLASH is selected.
 */
#ifndef ASRC_Q31_COEFF_STORAGE
#  define ASRC_Q31_COEFF_STORAGE  (ASRC_COEFF_STORAGE_RAM)
#endif
#if (ASRC_Q31_COEFF_STORAGE == ASRC_COEFF_STORAGE_FLASH)
#  define ASRC_POLY_Q31_STORAGE_QUAL  const
#  define ASRC_POLY_Q31_STORAGE_NAME  "flash"
#else
#  define ASRC_POLY_Q31_STORAGE_QUAL  /* writable RAM */
#  define ASRC_POLY_Q31_STORAGE_NAME  "ram"
#endif

static ASRC_POLY_Q31_STORAGE_QUAL int32_t
    s_poly_q31[ASRC_POLY_L + 1u][ASRC_POLY_M] __attribute__(( aligned(4) )) =
{
    ASRC_POLY_Q31_TABLE_INIT
};

#define ASRC_POLY_Q31_ROW(p)  ((const int32_t*)&s_poly_q31[(p)][0])

/*
 * The one array whose space is forced.  See PLACEMENT above.
 *
 * ONE ROW PER ENGINE, NOT ONE ROW SHARED.  It was a single row until 2026-09-06 and
 * that was a real data race, not a theoretical one: blend_row writes this buffer and
 * row16 reads it, and at MIXED rates the two leg ISRs no longer share a priority
 * (APP_ASRC_RATE_MONOTONIC_ISR), so the high-rate leg preempts the low-rate one
 * between those two calls and the low leg's dot product runs against the high leg's
 * coefficients.  Counted on hardware at 96/32 12ch: 4.51 per cent of A->B output
 * frames (25910 of 573920), B->A exactly zero, which is the priority relation.  The
 * instruction count is identical either way, so the timing measurements taken with the
 * shared row remain valid; the audio taken with it does not.  Fix costs ASRC_POLY_M
 * words per engine and CPU +-0.
 *
 * Every kernel access to it is a `.l` (4-byte) load or store with a +=4 post-increment,
 * so 4-byte alignment is all the AGUs require and the per-engine row stride does not
 * have to be 8-aligned.  Under APP_ASRC_STAGE_PROFILE one extra word per row carries the
 * owner tag that dbg_ceff_steal compares (see audio_app_asrc.c); the shipping image does
 * not allocate that word.  The selftests below do NOT use any row of this array:
 * reset_all() also runs with the ISRs live (second boot call, and `*as 07`), so they own
 * their scratch outright -- see SELFTEST SCRATCH there.
 */
#if APP_ASRC_STAGE_PROFILE
#define ASRC_CEFF_Q31_STRIDE  ( ASRC_POLY_M + 1u )
#define ASRC_CEFF_OWNER_SLOT  ( ASRC_POLY_M )
#else
#define ASRC_CEFF_Q31_STRIDE  ( ASRC_POLY_M )
#endif
static int32_t s_ceff_q31[ASRC_ENGINE_COUNT][ASRC_CEFF_Q31_STRIDE]
    __attribute__(( space(ymemory), aligned(4) ));

extern void mchp_asrc_q31_blend_row( const int32_t* c0, const int32_t* c1,
                                     int32_t* ceff, uint32_t taps, int32_t wbq );
extern void mchp_asrc_q31_row16( const int32_t* hist0, uint32_t stride_bytes,
                                 const int32_t* ceff, uint32_t taps,
                                 int32_t* out, uint32_t nch );

#if APP_ASRC_Q31_OPT_KERNELS
/*
 * Bit-exact fully-unrolled variants of the two kernels above.  Same coefficients,
 * same arithmetic model, same generated samples -- the only difference is that the
 * tap and channel counts are assembly-time constants, so the loop-control
 * instruction disappears, the pointer reloads collapse, and the blend processes its
 * taps in pairs so that one tap's ALU-latency slot carries the next tap's subtract.
 * The arithmetic is NOT restated in a cheaper algebraic form: the first attempt did
 * that (one MAC of the difference split into a difference of two MACs) and measured
 * 1..2 LSB low on hardware -- see the note in mchp_asrc_q31_row16.s.  _row16_opt also folds
 * the s24-left slot mask into its store, which is why its caller must NOT run the
 * asrc_q31_to_slot() loop afterwards (masking twice is harmless, but it would be
 * measured as part of the "optimised" arm and hide the saving being measured).
 *
 * The counts live in the .s file as .equ; these asserts are the only thing tying
 * them to the C configuration, so a profile with different M or ASRC_CH fails to
 * build rather than silently reading past the coefficient row.
 */
/* ASRC_Q31_OPT_M tracks ASRC_POLY_M exactly as ASRC_Q31_OPT_NCH tracks ASRC_CH:
 * the kernels unroll the tap loop, so the count is fixed at assembly time and is
 * overridden with -Wa,--defsym=ASRC_Q31_OPT_M=<n>.  Pass BOTH sides or neither. */
#ifndef ASRC_Q31_OPT_M
#define ASRC_Q31_OPT_M     (30u)
#endif
_Static_assert( ASRC_POLY_M == ASRC_Q31_OPT_M,
    "APP_ASRC_Q31_OPT_KERNELS: the unrolled kernels run ASRC_Q31_OPT_M taps; pass "
    "BOTH -DASRC_Q31_OPT_M=<ASRC_POLY_M> and -Wa,--defsym=ASRC_Q31_OPT_M=<ASRC_POLY_M> "
    "(.equ ASRC_Q31_OPT_M in mchp_asrc_q31_row16.s)." );
/* ASRC_Q31_OPT_NCH must equal the .equ of the same name in mchp_asrc_q31_row16.s,
 * which defaults to 12 and is overridden with -Wa,--defsym=ASRC_Q31_OPT_NCH=<n>.
 * Both sides have to be passed together; the canary below catches it when they are
 * not, because a _Static_assert cannot see an assembler defsym. */
#ifndef ASRC_Q31_OPT_NCH
#define ASRC_Q31_OPT_NCH   (12u)
#endif
_Static_assert( ASRC_CH == ASRC_Q31_OPT_NCH,
    "APP_ASRC_Q31_OPT_KERNELS: _row16_opt writes ASRC_Q31_OPT_NCH channels; pass "
    "BOTH -DASRC_Q31_OPT_NCH=<ASRC_CH> and -Wa,--defsym=ASRC_Q31_OPT_NCH=<ASRC_CH> "
    "(.equ ASRC_Q31_OPT_NCH in mchp_asrc_q31_row16.s)." );
_Static_assert( ( ASRC_POLY_M % 2u ) == 0u,
    "APP_ASRC_Q31_OPT_KERNELS: _blend_row_opt tiles the taps in PAIRS, so an odd M "
    "would drop the last tap.  Fail the build rather than the filter." );

/*
 * The two variants keep the baseline's argument list, including the trip counts
 * they no longer read, so the call site differs by nothing but the symbol name
 * and the register mapping is provably the baseline's rather than something
 * re-derived.  The dead arguments cost one immediate load each per call.
 */
extern void mchp_asrc_q31_blend_row_opt( const int32_t* c0, const int32_t* c1,
                                         int32_t* ceff, uint32_t taps_ignored,
                                         int32_t wbq );
extern void mchp_asrc_q31_row16_opt( const int32_t* hist0, uint32_t stride_bytes,
                                     const int32_t* ceff, uint32_t taps_ignored,
                                     int32_t* out, uint32_t nch_ignored );

/*
 * Which pair the pull uses.  Runtime, not compile time, because the two arms have
 * to be differenced INSIDE ONE IMAGE: a stage that was not touched at all moved
 * several us on a Y-placement change in this tree on 2026-09-06, and the decision
 * bands for the 12-channel question are 21.0 / 22.8 us wide.  Comparing two images
 * would put that placement noise straight into the answer.
 *
 * volatile: written from console (app) context, read from the pull in interrupt
 * context.  A torn read is not possible (one byte) and a switch that lands one
 * frame early or late is harmless -- both arms produce identical samples, which
 * is also why this needs no lock and no engine reset.
 */
/* Bit per candidate, not one flag for both: the blend and the row16/mask kernels are
 * independent optimisations, and on 2026-09-07 one of them (the first blend attempt)
 * turned out not to be bit-exact.  With one flag that single defect blocked measuring
 * the other candidate at all; with a bit each, a candidate that fails its selftest is
 * refused on its own and the rest can still be priced.  It also gives the measurement
 * a per-candidate breakdown for free (`*au 01`, `*au 02`, `*au 03`). */
#define ASRC_Q31_OPT_BIT_BLEND   (0x1u)
#define ASRC_Q31_OPT_BIT_ROW16   (0x2u)
#define ASRC_Q31_OPT_BIT_ALL      ( ASRC_Q31_OPT_BIT_BLEND | ASRC_Q31_OPT_BIT_ROW16 )

static volatile uint8_t s_q31_opt = 0u;

/* Which bits the boot selftest actually proved bit-exact.  Nothing outside those bits
 * may be selected -- a delta between two different filters is not the number this
 * experiment is looking for. */
static uint8_t s_q31_opt_allow = 0u;
#endif

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

/* Proof-A oracle, one per geometry, selected exactly like the table above:
 * the expected outputs depend on M, so an M28 build checked against the M30
 * vectors fails every case (and did, on 2026-09-08: the coeff probe reported
 * got 15748 want -4550 -- the table was right, the oracle was stale). */
#if   ( ASRC_POLY_M == 30u )
#  include "asrc_poly_q31_vectors.h"
#elif ( ASRC_POLY_M == 28u )
#  include "asrc_poly_q31_vectors_m28.h"
#else
#  error "no Proof-A vectors for this ASRC_POLY_M -- generate them with tools/asrc/asrc_q31_resampler_model.py vectors --taps <ASRC_POLY_M> --fc <ASRC_POLY_FC> --beta <ASRC_POLY_KAISER_BETA> and select them above"
#endif

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

/*
 * SELFTEST SCRATCH -- DEDICATED, NEVER THE LIVE ENGINE'S.
 *
 * Until 2026-09-06 the tests below borrowed running state: the blended row was
 * s_ceff_q31[0], i.e. the AB engine's own row, and the multi-channel test used
 * s_asrc[ASRC_ENGINE_AB].ch as its 12/16-channel history.  That is not safe, because
 * audio_app_asrc_reset_all() is not a boot-only function: it runs twice in the boot
 * sequence and again from the console (`*as 07`), and by then the audio ISRs are
 * streaming.  The selftest and the live pull then read and write the same history and the
 * same scratch row concurrently.  That is what made the SECOND `ASRC Q31 poly selftest`
 * FAIL intermittently (fails=23 / 50 / 71 on different boots of 2026-09-06, pass on many
 * others, both before and after the per-engine s_ceff_q31 split): the kernels were never
 * wrong, the test's own input was being moved under it.  It also means the test was
 * writing into production audio state, so it was not a diagnostics-only problem.
 *
 * Both scratches are now locals.  The stack is X space (0xD75C..0x10B88 on AK512), which
 * is where the kernel wants the sample operand, and it costs no permanent RAM: the linker
 * hands the stack whatever gap is left between the X statics and the Y statics, so a
 * static array here would take the same bytes and never give them back.  The blended row
 * lands in X instead of Y, so the row16 MACs run at 2 cycles instead of 1.012
 * (DS70005591C 4.3.17, and there is no room left in Y anyway -- the front-end Y history
 * ends 4 bytes below the diagnostic block).  The operand-space rule is a timing rule, not
 * a numeric one: a boot-time selftest pays it and computes the same values.
 *
 * ASRC_Q31_ST_SPAN is the read window the multi-channel test walks: the read pointer
 * advances floor(NOUT * step) samples and the kernel reads ASRC_POLY_M from wherever it
 * stands.  The step is written as a fraction so the span is a compile-time constant.  One
 * consequence to record: the multi-channel test used to pass the PRODUCTION channel stride
 * (sizeof(a->ch[0]) = ASRC_FIFO_PHYS * 4 B); it now passes its own, much smaller stride.
 * The kernel takes the stride as a runtime argument and computes it with a 32-bit
 * `add.l`, so nothing about the larger value is structurally different -- but the test no
 * longer covers the shipping number, and that is the price of not touching live state.
 */
#define ASRC_Q31_ST_STEP_N   3u      /* multi-channel test step = 3/2 */
#define ASRC_Q31_ST_STEP_D   2u
#define ASRC_Q31_ST_SPAN     ( ( ( ASRC_Q31_VEC_NOUT * ASRC_Q31_ST_STEP_N ) / \
                                 ASRC_Q31_ST_STEP_D ) + ASRC_POLY_M )

/* Proofs B and C over the generated vectors. */
static uint32_t asrc_q31_check_vectors( void )
{
    uint32_t fails = 0u;
    int32_t  xbuf[ASRC_Q31_VEC_NIN];        /* stack, i.e. X space, as the kernel needs */
    int32_t  yref[ASRC_Q31_VEC_NOUT];
    int32_t  ceff_ref[ASRC_POLY_M];
    int32_t  ceff[ASRC_POLY_M];             /* own blended row, not the AB engine's */

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

            mchp_asrc_q31_blend_row( c0, c1, ceff, ASRC_POLY_M, wbq );
            asrc_q31_ref_blend( c0, c1, wbq, ceff_ref );
            for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
            {
                const int32_t d = ceff[k] - ceff_ref[k];
                if( ( d > 1 ) || ( d < -1 ) )
                {
                    asrc_q31_note( "blend", v, k, ceff[k], ceff_ref[k] );
                    fails++;
                }
                else if( d != 0 ) { s_poly_q31_ties++; }
            }

            int32_t got;
            mchp_asrc_q31_row16( &xbuf[rd - ASRC_POLY_MH], 4u, ceff,
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
 * Scratch is this function's OWN history and OWN blended row, both on the stack, both
 * in X -- see SELFTEST SCRATCH above for why it must not be the live engine's.  The
 * reference is evaluated inside the same loop as the kernel, so no 16 x nout
 * expectation array is needed and the frame stays small.
 */
static uint32_t asrc_q31_check_16ch( void )
{
    uint32_t  fails = 0u;
    int32_t   hist[ASRC_CH][ASRC_Q31_ST_SPAN];  /* own history, own stride */
    int32_t   ceff[ASRC_POLY_M];                /* own blended row         */
    int32_t   ceff_ref[ASRC_POLY_M];
    int32_t   got[ASRC_CH];
    const uint32_t nin  = ASRC_Q31_ST_SPAN;
    const uint32_t nout = ASRC_Q31_VEC_NOUT;
    const float    step = (float)ASRC_Q31_ST_STEP_N /
                          (float)ASRC_Q31_ST_STEP_D;   /* != 1, so the phase walks */

    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        uint32_t st = 0x1234567u + ( c * 0x9E3779B9u );
        for( uint32_t i = 0u; i < nin; i++ )
        {
            st = ( st * 1664525u ) + 1013904223u;
            hist[c][i] = (int32_t)( st & 0xFFFFFF00u );
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
                                 ceff, ASRC_POLY_M, wbq );
        asrc_q31_ref_blend( ASRC_POLY_Q31_ROW( p ), ASRC_POLY_Q31_ROW( p + 1u ),
                            wbq, ceff_ref );

        mchp_asrc_q31_row16( &hist[0][w], (uint32_t)sizeof( hist[0] ),
                             ceff, ASRC_POLY_M, got, ASRC_CH );

        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            const int32_t exp = asrc_q31_to_slot(
                                    asrc_q31_ref_dot( &hist[c][w], ceff_ref ) );
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

#if APP_ASRC_Q31_OPT_KERNELS
/* ------------------------------------------------- optimised-kernel proof --- */
/*
 * asrc_q31_opt_selftest(): the two kernel pairs must agree BIT FOR BIT.
 *
 * This is the only claim the A/B measurement rests on.  If the fast pair merely
 * produced something close, the measured delta would be the price of a different
 * filter, not the price of the same filter computed better -- so this compares
 * all ASRC_POLY_M blended coefficients and all ASRC_CH outputs, including the
 * s24-left mask, as exact int32 patterns rather than within a tolerance.
 *
 * WHAT THIS COVERS AND WHAT IT DOES NOT.  It verifies the arithmetic and the
 * masking.  It does NOT exercise the production history layout: its scratch is
 * a local ASRC_CH x ASRC_Q31_OPT_ST_SPAN array (stride 128 B), not the live
 * 2,168 B FIFO stride, and both operands sit in X rather than X/Y.  Those are
 * timing and integration properties, checked by the placement gate and by the
 * running system; borrowing the live engine to check them here is exactly the
 * mistake that made the poly selftest fail intermittently before 2026-09-06.
 *
 * Deliberately covers the awkward inputs, not just typical ones: wbq = 0, wbq at
 * its maximum, coefficients at the Q31 rails (so sacr.l saturates), and history
 * at the rails (so the accumulator carries its largest magnitudes).  This is not
 * decoration: the FIRST blend candidate (lac/mac/msc, the delta moved into the
 * accumulator) was argued to be exact and was not -- 354 of 882 comparisons here
 * came back 1..2 LSB low on 2026-09-07, which is what sent it back for a rewrite
 * that leaves the arithmetic alone.  The two arms are scored SEPARATELY so that
 * one bad candidate cannot mask the other.
 */
#define ASRC_Q31_OPT_ST_SPAN   ( ASRC_POLY_M + 2u )

static uint32_t s_q31_opt_fails;
static uint32_t s_q31_opt_cases;
static uint32_t s_q31_opt_blend_fails;
static uint32_t s_q31_opt_row16_fails;
static uint32_t s_q31_opt_nch_fails;   /* subset of row16: wrong channel count */

/* Own record, not the APP_ASRC_Q31_SELFTEST one: this test must still report a
 * mismatch in an image built without that selftest, and coupling the two would
 * make the kernel switch depend on a diagnostic that can legitimately be off. */
static struct
{
    const char* what;
    uint32_t    caze;
    uint32_t    idx;
    int32_t     got;
    int32_t     expect;
} s_q31_opt_fail;

static void asrc_q31_opt_note( const char* what, uint32_t caze, uint32_t idx,
                               int32_t got, int32_t expect )
{
    if( s_q31_opt_fail.what == (const char*)0 )
    {
        s_q31_opt_fail.what   = what;
        s_q31_opt_fail.caze   = caze;
        s_q31_opt_fail.idx    = idx;
        s_q31_opt_fail.got    = got;
        s_q31_opt_fail.expect = expect;
    }
}

static uint32_t asrc_q31_opt_selftest( void )
{
    s_q31_opt_fail.what = (const char*)0;

    static const int32_t wbq_cases[] = {
        0, 1, 2147483647, 2147483646, 1073741824, 12345678, 2147483000
    };

    int32_t  hist[ASRC_CH][ASRC_Q31_OPT_ST_SPAN];
    int32_t  c0[ASRC_POLY_M];
    int32_t  c1[ASRC_POLY_M];
    int32_t  ceff_base[ASRC_POLY_M];
    /* bracketed like out_opt below, and for the same reason: a wrong or missing
     * -Wa,--defsym=ASRC_Q31_OPT_M makes the asm write MORE taps than the C side
     * sized, which is a stack overrun the ASRC_POLY_M-limited comparison below
     * cannot see -- the taps it does compare would all still match. */
    int32_t        ceff_opt_g[1u + ASRC_POLY_M + 1u];
    int32_t* const ceff_opt = &ceff_opt_g[1];
    int32_t  out_base[ASRC_CH];
    /* out_opt is bracketed: _row16_opt's channel count comes from an assembler
     * defsym the C preprocessor cannot see, so a --defsym that does not match
     * ASRC_CH would otherwise be a silent buffer overrun here and a wrong trip
     * count in the live pull.  Writing FEWER channels than ASRC_CH shows up as an
     * ordinary value mismatch below, because the untouched slot keeps the canary. */
    int32_t  out_opt[1u + ASRC_CH + 1u];
    uint32_t fails_n = 0u;    /* channel-count (canary) failures */
    uint32_t fails  = 0u;
    uint32_t cases  = 0u;
    uint32_t fails_b = 0u;    /* blend arm */
    uint32_t fails_r = 0u;    /* row16 + mask arm */

    for( uint32_t pat = 0u; pat < 3u; pat++ )
    {
        /* pat 0: pseudo-random   pat 1: coefficient rails   pat 2: history rails */
        uint32_t s = 0x1234567u + pat;
        for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
        {
            s = ( s * 1664525u ) + 1013904223u;
            const int32_t r0 = (int32_t)s;
            s = ( s * 1664525u ) + 1013904223u;
            const int32_t r1 = (int32_t)s;
            if( pat == 1u )
            {
                c0[k] = ( ( k & 1u ) != 0u ) ? 2147483647 : ( -2147483647 - 1 );
                c1[k] = ( ( k & 1u ) != 0u ) ? ( -2147483647 - 1 ) : 2147483647;
            }
            else
            {
                c0[k] = r0;
                c1[k] = r1;
            }
        }
        for( uint32_t c = 0u; c < ASRC_CH; c++ )
        {
            for( uint32_t i = 0u; i < ASRC_Q31_OPT_ST_SPAN; i++ )
            {
                s = ( s * 1664525u ) + 1013904223u;
                hist[c][i] = ( pat == 2u )
                             ? ( ( ( i & 1u ) != 0u ) ? 2147483647 : ( -2147483647 - 1 ) )
                             : (int32_t)s;
            }
        }

        for( uint32_t w = 0u; w < ( sizeof( wbq_cases ) / sizeof( wbq_cases[0] ) ); w++ )
        {
            const int32_t wbq = wbq_cases[w];
            cases++;

            mchp_asrc_q31_blend_row(     c0, c1, ceff_base, ASRC_POLY_M, wbq );
            for( uint32_t i = 0u; i < ( 1u + ASRC_POLY_M + 1u ); i++ )
            {
                ceff_opt_g[i] = (int32_t)0x5A5AA5A5;
            }
            mchp_asrc_q31_blend_row_opt( c0, c1, ceff_opt,  ASRC_POLY_M, wbq );
            if( ( ceff_opt_g[0] != (int32_t)0x5A5AA5A5 )
             || ( ceff_opt_g[1u + ASRC_POLY_M] != (int32_t)0x5A5AA5A5 ) )
            {
                /* wrote outside its ASRC_POLY_M taps: --defsym does not match M */
                if( fails == 0u )
                {
                    asrc_q31_opt_note( "opt blend m", w, ASRC_POLY_M,
                                       ceff_opt_g[1u + ASRC_POLY_M], (int32_t)0x5A5AA5A5 );
                }
                fails++;
                fails_n++;
                fails_b++;
            }
            for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
            {
                if( ceff_base[k] != ceff_opt[k] )
                {
                    if( fails == 0u )
                    {
                        asrc_q31_opt_note( "opt blend", w, k, ceff_opt[k], ceff_base[k] );
                    }
                    fails++;
                    fails_b++;
                }
            }

            mchp_asrc_q31_row16( &hist[0][0],
                                 (uint32_t)sizeof( hist[0] ),
                                 ceff_base, ASRC_POLY_M, out_base, ASRC_CH );
            for( uint32_t c = 0u; c < ASRC_CH; c++ )
            {
                out_base[c] = asrc_q31_to_slot( out_base[c] );
            }
            /* ceff_BASE deliberately, not ceff_opt: the two candidates are proven
             * independently, so a blend mismatch must not be able to fail the row16
             * arm as well (it did on 2026-09-07 and hid whether row16 was sound). */
            /* the mask is folded into this kernel's store -- no conversion loop */
            for( uint32_t i = 0u; i < ( 1u + ASRC_CH + 1u ); i++ )
            {
                out_opt[i] = (int32_t)0x5A5AA5A5;
            }
            mchp_asrc_q31_row16_opt( &hist[0][0],
                                     (uint32_t)sizeof( hist[0] ),
                                     ceff_base, ASRC_POLY_M, &out_opt[1], ASRC_CH );
            if( ( out_opt[0] != (int32_t)0x5A5AA5A5 )
             || ( out_opt[1u + ASRC_CH] != (int32_t)0x5A5AA5A5 ) )
            {
                /* wrote outside its ASRC_CH slots: --defsym does not match ASRC_CH */
                if( fails == 0u )
                {
                    asrc_q31_opt_note( "opt row16 nch", w, ASRC_CH,
                                       out_opt[1u + ASRC_CH], (int32_t)0x5A5AA5A5 );
                }
                fails++;
                fails_n++;
                fails_r++;
            }
            for( uint32_t c = 0u; c < ASRC_CH; c++ )
            {
                if( out_base[c] != out_opt[1u + c] )
                {
                    if( fails == 0u )
                    {
                        asrc_q31_opt_note( "opt row16", w, c, out_opt[1u + c], out_base[c] );
                    }
                    fails++;
                    fails_r++;
                }
            }
        }
    }

    s_q31_opt_cases       = cases;
    s_q31_opt_blend_fails = fails_b;
    s_q31_opt_row16_fails = fails_r;
    s_q31_opt_nch_fails   = fails_n;
    s_q31_opt_allow = (uint8_t)( ( ( fails_b == 0u ) ? ASRC_Q31_OPT_BIT_BLEND : 0u )
                              |  ( ( fails_r == 0u ) ? ASRC_Q31_OPT_BIT_ROW16 : 0u ) );
    return fails;
}
#endif /* APP_ASRC_Q31_OPT_KERNELS */
