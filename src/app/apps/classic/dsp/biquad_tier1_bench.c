/*
 * biquad_tier1_bench.c - see biquad_tier1_bench.h.
 */
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#if defined(ENA_BIQUAD_TIER1_BENCH) && ENA_BIQUAD_TIER1_BENCH

#if !SONORA_APP_IS_CLASSIC
#  error "biquad_tier1_bench.c is Classic-app-owned; build it only in a Classic manifest."
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arm_math.h"
#include "nora_high_res_timer.h"

#include "biquad_tier1_bench.h"
#include "shared_ref/iir_bench_reference.h"

#if defined(ENA_PERF_MONITOR) && ENA_PERF_MONITOR
#include "diagnostics/perf_monitor_dspic33a.h"
#endif

/* ------------------------------------------------------------------ */
/* Geometry and working set                                            */
/* ------------------------------------------------------------------ */
/*
 * One channel. Tier 1 is channel-count-free by construction: both this kernel
 * and arm_biquad_cascade_df2T_f32() are single-channel block processors, and
 * the channel count is an outer loop in the caller. Measuring one channel and
 * reporting cycles/sample/section is therefore the symmetric thing to do, and
 * it removes the 4-versus-8 question from Tier 1 entirely.
 */
#define T1_FRAMES           IIR_BENCH_FRAMES
#define T1_MAX_SECTIONS     IIR_BENCH_AP_SECTIONS
#define T1_ITERATIONS       (64u)

/* The channel count Tier 2 and the DRC record use. Only used to turn a
 * cycles/sample/section figure into N_max, never to size a buffer here. */
#define T1_NMAX_CHANNELS    (4u)

/* ------------------------------------------------------------------ */
/* Which kernel is under test                                          */
/* ------------------------------------------------------------------ */
/*
 * A compile-time selector, so that both pairings the protocol defines are
 * measured by ONE harness rather than by two harnesses that would differ in
 * ways nobody wrote down.
 *
 *   BEST_AVAILABLE : biquad_cascade_df2T_f32_dspic33ak_opt_v1  (project asm)
 *   VENDOR_LIBRARY : mchp_biquad_cascade_df2T_f32              (Microchip lib)
 *
 * Everything except the call itself is shared, and must stay shared: the same
 * input block, the same coefficient banks and their CRC guard, the same X/Y
 * memory placement, the same 1 ch x 32 frames x N geometry, the same
 * high-resolution timer, the same warm-up, and the same known-answer test with
 * the same fault injections. The measurement scope does not move. If it did,
 * the difference between the two numbers would not be the difference between
 * the two kernels.
 *
 * The two functions have identical signatures (see
 * dspic33-cmsis-dsp/Include/dsp/filtering_functions.h), so the selection is a
 * one-line dispatch rather than a shim. A wrapper here would add its own call
 * overhead to one arm of the comparison and not the other.
 *
 * Selecting an arm, without touching project settings. build.ps1 -Define takes
 * an ARRAY, so several defines go in ONE comma-separated argument; passing
 * -Define twice is a PowerShell parameter-binding error, not a second define:
 *
 *   .\buildtools\build.ps1 -Full -Define ENA_BIQUAD_TIER1_BENCH=1
 *   .\buildtools\build.ps1 -Full -Define ENA_BIQUAD_TIER1_BENCH=1,BIQUAD_TIER1_KERNEL=2
 *
 * Both arms build. Measured 2026-09-17, clean -Full builds of
 * dsPIC33AK512_CLASSIC_SERIAL_UPDATE (Classic DRC, MPLAB X v6.35,
 * xc-dsc v3.31.01), one build each:
 *
 *   Best-available (opt_v1)          193,372 B program / 43,284 B data
 *   Vendor-library (mchp_...df2T)    193,520 B program / 43,284 B data
 *                                    ------------------------------------
 *   vendor cell costs                   +148 B program /      0 B data
 *
 * Read that delta as one pair of builds, not as a stable constant: builds in
 * this tree are not bit-reproducible and section layout shifts between them.
 * Neither arm's known-answer test has ever executed - the Best-available kernel
 * is assembly, so it cannot run off-target, and no board measurement exists.
 */
#define BIQUAD_TIER1_KERNEL_BEST_AVAILABLE  1
#define BIQUAD_TIER1_KERNEL_VENDOR_LIBRARY  2
/*
 * Arm 3 is the two-sample register-ping-pong kernel, opt_v3. MEASURED AND LOST:
 * 13.986 cyc/sample/section against arm 1's 11.484 on the AK506 Nano, ap84, 84
 * sections (2026-09-22) - the same 1.218 ratio at all ten sweep points. Its
 * correctness gates all passed, so it is a correct kernel that is slower.
 *
 * Kept as the instrument that produced that number, not as a live contender.
 * Arm 1 is Best-available. Selecting this arm is how the result is reproduced;
 * nothing in a shipping configuration reaches it.
 *
 * The full result, and why it argues against the three-register rotation that
 * was going to be the next step, is in
 * [internal] iir_df2t_opt_v3_pingpong_status_2026-09-22.md.
 */
#define BIQUAD_TIER1_KERNEL_PINGPONG_V3     3
/*
 * Arm 4 is the SPACING SWEEP, and it is not a candidate kernel at all - it is an
 * experiment about why arm 3 lost.
 *
 * Arm 3 removed 15 % of the inner-loop instructions and measured 21.8 % slower.
 * The explanation on offer was that opt_v1's two mov.s are the interval hiding
 * mac.s result latency across the loop-carried edge, and that arm 3's tighter
 * schedule cannot cover it. That was an argument from instruction distance with
 * no counter behind it.
 *
 * This arm settles it by going the OTHER way: it builds arm 3's loop four times
 * with 0, 1, 2 and 3 independent issue slots inserted at exactly that edge, puts
 * opt_v1 in the same image as the control, and reads the Performance Monitor on
 * every one of them. If the cross-sample dependency is the cost, widening the
 * gap must buy cycles back and PMU event 8 must fall with it. If widening
 * changes nothing, the hypothesis is refuted and the counters say where to look
 * instead.
 *
 * Selecting this arm changes what *cQ and *cW measure to a five-way comparison
 * and adds *cG. Nothing in a shipping configuration reaches it.
 */
#define BIQUAD_TIER1_KERNEL_GAP_SWEEP       4
/*
 * Arm 5 is the SECOND spacing sweep, at a different edge, and it is also not a
 * candidate kernel.
 *
 * Arm 4 widened the CROSS-SAMPLE edge and refuted that hypothesis: event 8 sat
 * at 3.999 on all four of its arms, each added slot cost a full cycle, and the
 * arm with opt_v1's exact instruction count was still 34.8 % slower. The
 * attribution survived - the stall is an FPU register dependency and opt_v1 has
 * none - but the location did not.
 *
 * So this arm asks the same question one level in. Each sample accumulates d1 in
 * two steps through one F-register, with a store between them and nothing that
 * could cover latency. Arm 5 builds that loop four times with 0, 1, 2 and 3
 * independent issue slots between the d1 partial and the d1 final, IN BOTH
 * samples, and reads the same eight counters.
 *
 * A fall in event 8 would locate the stall in that region. It would NOT isolate
 * the d1 edge by itself: the slots also delay the d2 accumulation behind them, so
 * separating the two operands needs a different arm shape and is not attempted
 * here. A flat event 8 kills the distance explanation for this edge too.
 *
 * Arm 4's kernels, tables and measurement functions are untouched by this arm -
 * it selects a different kernel file (biquad_cascade_df2T_f32_dspic33ak_d1gap.s)
 * and nothing else. Selecting it changes what *cQ and *cW measure to a five-way
 * comparison and adds *cG, exactly as arm 4 does. Nothing in a shipping
 * configuration reaches it.
 */
#define BIQUAD_TIER1_KERNEL_D1GAP_SWEEP     5
/*
 * Arm 6 is the first arm in this family that contains CANDIDATES rather than
 * instrumentation. Arms 4 and 5 were experiments whose output was a counter
 * reading; these are four kernels that are allowed to win and ship.
 *
 * WHAT THE TWO EXPERIMENTS ESTABLISHED, WHICH IS WHY THESE FOUR
 *   opt_v1 is LATENCY-bound, not issue-bound. It retires 10.433 instructions
 *   per sample/section in 11.484 cycles, and every one of those excess cycles is
 *   ONE stall: ev9 = 0.999, the CPU unable to read an F-register. ev8 is exactly
 *   zero. Meanwhile opt_v3 showed that removing instructions from a single
 *   channel's chain converts them straight into ev8 (-1.5 instructions, +4
 *   cycles), and Phase C showed those stall cycles are genuinely empty issue
 *   slots: three nopr per pair inserted into them cost nothing measurable.
 *
 *   Empty slots plus a 3-cycle MAC latency (DS70005540C Table 6-2) means the way
 *   forward is to put USEFUL work in the slots, not to remove instructions.
 *   Two ways to get useful independent work, and this arm measures both:
 *
 *   sp_a / sp_b / sp_c  Reschedule ONE channel so the y consumer is further from
 *                       the y producer. Same 10 instructions, same arithmetic,
 *                       same 1..512 blockSize contract as opt_v1 - so a winner
 *                       here is a drop-in replacement. Ceiling is opt_v1's one
 *                       stall cycle: about 11.484 -> 10.5, call it -8 %.
 *
 *   x2                  Interleave TWO INDEPENDENT CHANNELS, whose MACs cannot
 *                       depend on each other, so each covers the other's
 *                       latency. 9.0 instructions per sample against opt_v1's
 *                       10.0 and every producer-consumer distance >= 5 against
 *                       opt_v1's 3. Much larger prize (predicted -17 to -19 %)
 *                       but it needs a new call shape and equal stage counts.
 *
 * Selecting this arm changes what *cQ and *cW measure to a five-way comparison
 * and adds *cG. Nothing in a shipping configuration reaches it.
 */
#define BIQUAD_TIER1_KERNEL_CANDIDATE       6

/*
 * The two sweeps share every piece of scaffolding that is not a kernel name:
 * five arms in one image, one measurement function per arm holding one direct
 * call, opt_v1 first and last as the drift check, the same eight PMU events and
 * the same bit-exactness gate. T1_SWEEP_BUILD means "this image is one of the
 * sweeps", so the shared code does not have to name both arms every time.
 *
 * Defined before the validity gate below rather than after it, because the gate
 * for the PMU requirement uses it.
 */
#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_GAP_SWEEP ) \
 || ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_D1GAP_SWEEP ) \
 || ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_CANDIDATE )
#  define T1_SWEEP_BUILD   1
#else
#  define T1_SWEEP_BUILD   0
#endif

/*
 * Arm 6 alone processes TWO channels in one call (its x2 arm does), so it needs
 * a second channel's coefficients, state and buffers, and a per-arm divisor when
 * normalising to cycles per sample per section. Arms 4 and 5 do not, and must
 * not carry the extra RAM: s_coeff/s_state placement in X/Y memory is part of
 * the measured result, and adding a second pair beside them would move the
 * layout those sweeps were measured with.
 */
#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_CANDIDATE )
#  define T1_TWO_CHANNEL_BUILD   1
#else
#  define T1_TWO_CHANNEL_BUILD   0
#endif

#ifndef BIQUAD_TIER1_KERNEL
#define BIQUAD_TIER1_KERNEL  BIQUAD_TIER1_KERNEL_BEST_AVAILABLE
#endif

#if ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_BEST_AVAILABLE ) \
 && ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_VENDOR_LIBRARY ) \
 && ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_PINGPONG_V3 )    \
 && ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_GAP_SWEEP )      \
 && ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_D1GAP_SWEEP )    \
 && ( BIQUAD_TIER1_KERNEL != BIQUAD_TIER1_KERNEL_CANDIDATE )
#  error "BIQUAD_TIER1_KERNEL must be BIQUAD_TIER1_KERNEL_BEST_AVAILABLE, _VENDOR_LIBRARY, _PINGPONG_V3, _GAP_SWEEP, _D1GAP_SWEEP or _CANDIDATE"
#endif

/*
 * Either sweep reads the Performance Monitor, so neither can be selected in a
 * build that compiled the PMU out. Caught here rather than at link time, where
 * it would surface as an undefined reference to perf_monitor_start.
 */
#if T1_SWEEP_BUILD
#  if !defined(ENA_PERF_MONITOR) || !ENA_PERF_MONITOR
#    error "A sweep build (BIQUAD_TIER1_KERNEL = _GAP_SWEEP, _D1GAP_SWEEP or _CANDIDATE) needs ENA_PERF_MONITOR=1: the stall breakdown is the point."
#  endif
#endif

#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_BEST_AVAILABLE )
#  define T1_KERNEL_NAME    "biquad_cascade_df2T_f32_dspic33ak_opt_v1 (project asm)"
#  define T1_KERNEL_PAIRING "Best-available"
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_PINGPONG_V3 )
#  define T1_KERNEL_NAME    "biquad_cascade_df2T_f32_dspic33ak_opt_v3 (project asm, 2-sample ping-pong)"
#  define T1_KERNEL_PAIRING "Rejected candidate - measured SLOWER than Best-available (13.986 vs 11.484)"
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_GAP_SWEEP )
#  define T1_KERNEL_NAME    "opt_v1 + gap0..gap3 (A->B spacing sweep, 5 arms, one image)"
#  define T1_KERNEL_PAIRING "Experiment - stall attribution, not a candidate"
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_D1GAP_SWEEP )
#  define T1_KERNEL_NAME    "opt_v1 + d1gap0..d1gap3 (in-sample d1 spacing sweep, 5 arms, one image)"
#  define T1_KERNEL_PAIRING "Experiment - stall attribution, not a candidate"
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_CANDIDATE )
#  define T1_KERNEL_NAME    "opt_v1 + sp_a/sp_b/sp_c/x2 (candidates: store position, 2-channel interleave)"
#  define T1_KERNEL_PAIRING "Candidates - these are allowed to replace Best-available"
#else
#  define T1_KERNEL_NAME    "mchp_biquad_cascade_df2T_f32 (Microchip vendor library)"
#  define T1_KERNEL_PAIRING "Vendor-library"
#endif

/* Representative section points for the sweep. 84 individual measurements
 * would say nothing these ten do not. */
static const uint16_t s_sweep_points[] =
    { 1u, 2u, 4u, 6u, 8u, 16u, 32u, 48u, 64u, 84u };

/*
 * Coefficients are copied out of the vendored const header into RAM, because
 * fault injection has to be able to perturb one without corrupting the table
 * the comparison is made against.
 *
 * X/Y memory placement matches what the shipping Classic/DRC path does
 * (coefficients in .xbss, state in .ybss). That placement is part of the
 * measured result on this architecture, so the bench has to reproduce it or it
 * would be timing a different memory layout than the record was set with.
 */
static float32_t s_coeff[ T1_MAX_SECTIONS * 5u ]
    __attribute__((section(".xbss"), aligned(4)));
static float32_t s_state[ T1_MAX_SECTIONS * 2u ]
    __attribute__((section(".ybss"), aligned(4)));

static float32_t s_in [ T1_FRAMES ] __attribute__((section(".xbss"), aligned(4)));
static float32_t s_out[ T1_FRAMES ] __attribute__((section(".ybss"), aligned(4)));

#if T1_TWO_CHANNEL_BUILD
/*
 * Channel B's working set, for the x2 arm only.
 *
 * Same X/Y split as channel A's, for the same reason: the measured result
 * includes the memory placement, and a two-channel kernel whose second channel
 * lived in a different kind of memory would not be measuring interleaving.
 *
 * Present only in the candidate build. Arms 4 and 5 were measured without these
 * arrays in the image; adding them there would move s_coeff/s_state placement
 * and invalidate the comparison against their recorded numbers.
 */
static float32_t s_coeff_b[ T1_MAX_SECTIONS * 5u ]
    __attribute__((section(".xbss"), aligned(4)));
static float32_t s_state_b[ T1_MAX_SECTIONS * 2u ]
    __attribute__((section(".ybss"), aligned(4)));

static float32_t s_in_b [ T1_FRAMES ] __attribute__((section(".xbss"), aligned(4)));
static float32_t s_out_b[ T1_FRAMES ] __attribute__((section(".ybss"), aligned(4)));

static mchp_biquad_cascade_df2T_instance_f32 s_inst_b;

/* opt_v1's output for channel B, so the x2 arm's second channel can be checked
 * the same way its first is. */
static float32_t s_ref_out_b[ T1_FRAMES ];

/*
 * Channel C's working set, for the x3 arm only.
 *
 * Same X/Y split as A's and B's, for the same reason: the measured result
 * includes the memory placement, so a third channel living in a different kind
 * of memory would not be measuring interleaving.
 *
 * This DOES move s_coeff/s_state placement relative to the x2-only image, so the
 * x3 arm's numbers are compared against x2 measured IN THE SAME IMAGE, never
 * against x2's recorded 9.922 from the earlier build. The sweep prints opt_v1
 * first and last for exactly this reason.
 */
static float32_t s_coeff_c[ T1_MAX_SECTIONS * 5u ]
    __attribute__((section(".xbss"), aligned(4)));
static float32_t s_state_c[ T1_MAX_SECTIONS * 2u ]
    __attribute__((section(".ybss"), aligned(4)));

static float32_t s_in_c [ T1_FRAMES ] __attribute__((section(".xbss"), aligned(4)));
static float32_t s_out_c[ T1_FRAMES ] __attribute__((section(".ybss"), aligned(4)));

static mchp_biquad_cascade_df2T_instance_f32 s_inst_c;

/* opt_v1's output for channel C, so x3's third channel is checked the same way
 * the other two are. */
static float32_t s_ref_out_c[ T1_FRAMES ];

/*
 * Channel D's working set, for the x4t arm only.
 *
 * Same X/Y split as A/B/C's, for the same reason the others have it: the measured
 * result includes the memory placement, so a fourth channel living in a
 * different kind of memory would not be measuring a four-channel interleave.
 *
 * This DOES move s_coeff/s_state placement again relative to the x3-only image,
 * which is why the sweep prints opt_v1 first and last and why x4t is compared
 * against x2/x3td measured IN THIS SAME IMAGE, never against their recorded
 * numbers from the earlier builds.
 */
static float32_t s_coeff_d[ T1_MAX_SECTIONS * 5u ]
    __attribute__((section(".xbss"), aligned(4)));
static float32_t s_state_d[ T1_MAX_SECTIONS * 2u ]
    __attribute__((section(".ybss"), aligned(4)));

static float32_t s_in_d [ T1_FRAMES ] __attribute__((section(".xbss"), aligned(4)));
static float32_t s_out_d[ T1_FRAMES ] __attribute__((section(".ybss"), aligned(4)));

static mchp_biquad_cascade_df2T_instance_f32 s_inst_d;

/* opt_v1's output for channel D, so x4t's fourth channel is checked the same way
 * the other three are. */
static float32_t s_ref_out_d[ T1_FRAMES ];

/*
 * x3's argument arrays. The kernel takes three ARRAYS rather than nine scalars
 * because xc-dsc passes only the first seven 32-bit arguments in registers and
 * 3 instances + 3 sources + 3 destinations + blockSize is ten. Rebuilt by
 * t1_configure_c(), because a fresh init() writes new pointers into an instance.
 * Nothing in a timed region writes them.
 *
 * FOUR ENTRIES, not three: x4t reads a fourth element from each array and the
 * three-channel kernels read only the first three, so one set of arrays serves
 * both. Sized here rather than declaring a separate x4 set so that a
 * three-channel kernel and a four-channel one can never disagree about which
 * instance channel A is - they are literally the same slot.
 */
static const mchp_biquad_cascade_df2T_instance_f32 *s_x3_inst[4];
static const float32_t *s_x3_src[4];
static       float32_t *s_x3_dst[4];
#endif

/*
 * Channel 0's state as it stands after its whole run - warm-up AND the
 * observation block - kept so the shared-state negative test can force another
 * channel's observation block to run on it.
 *
 * Taken after the block rather than after warm-up because a bank may have no
 * warm-up at all: lp6's warm-up is 0 blocks, so a post-warm-up snapshot is just
 * the zeroed state every channel already starts from, and imposing it changes
 * nothing. See t1_kat_once() for the measurement that caught that.
 *
 * Deliberately NOT in .ybss. s_state is, because the measured path's X/Y
 * placement is part of the measured result; adding 672 B beside it would shift
 * that layout and move the timing for a buffer the benchmark never touches.
 * KAT and the candidate equivalence gate share this un-timed reference snapshot.
 */
static float32_t s_state_final_ch0[ T1_MAX_SECTIONS * 2u ];

static mchp_biquad_cascade_df2T_instance_f32 s_inst;

/* Somewhere the optimiser cannot argue the output away. Written outside the
 * timed region, so it costs the measurement nothing. */
static volatile uint32_t s_sink;

static bool s_ready;

#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_BEST_AVAILABLE )
/* The project-owned kernel. Declared here rather than pulled from a header
 * because it is deliberately not part of the library's public surface. The
 * vendor kernel needs no declaration here: filtering_functions.h already
 * declares it, with this identical signature. */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_PINGPONG_V3 )
/*
 * Two symbols, because opt_v3 is split into an even-only fast path and a
 * dispatch wrapper that keeps opt_v1's 1..512 contract.
 *
 * The TIMED path calls the bare kernel, not the wrapper. Tier 1 always measures
 * T1_FRAMES = 32, which is even, so the wrapper would add a branch to the
 * measured region and route to the same place regardless - the measurement would
 * then include a dispatch cost that the thing under evaluation does not have.
 *
 * The wrapper is exercised by the equivalence test below, where the odd lengths
 * that make it necessary actually occur.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v3(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void biquad_cascade_df2T_f32_dspic33ak_opt_v3_block(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );
#elif T1_SWEEP_BUILD
/*
 * Five arms: opt_v1 as the control, plus the four spacing arms of whichever
 * sweep this build selected. Arm 0 of a sweep is always opt_v3's schedule
 * re-emitted in this image, so it doubles as the check that this build
 * reproduces the original 13.986 rather than measuring something new.
 *
 * WHICH KERNEL FILE, AND WHY THE ARMS ARE NAMED THROUGH MACROS
 *   BIQUAD_TIER1_KERNEL=4 measures biquad_cascade_df2T_f32_dspic33ak_gap.s
 *   (spacing at the CROSS-SAMPLE edge) and =5 measures
 *   biquad_cascade_df2T_f32_dspic33ak_d1gap.s (spacing INSIDE each sample, at
 *   the d1 partial -> d1 final edge). Those are the only two things that differ:
 *   the four kernel symbols and the labels printed beside them.
 *
 *   Everything else - one measurement function per arm holding one direct call,
 *   opt_v1 first and last, the eight PMU events, the bit-exactness gate - is
 *   shared, deliberately. Two copies of that scaffolding could drift, and the
 *   whole argument this bench exists to make rests on the arms differing ONLY in
 *   the kernel they call. So the arm-specific part is these four symbol names,
 *   reached through T1_SWEEP_K0..K3, and a =4 build compiles the same code it
 *   compiled before =5 existed.
 *
 * All five kernels have opt_v1's signature. The sweep kernels are even-only;
 * T1_FRAMES is 32 and the equivalence test never asks them for an odd length, so
 * no dispatch wrapper is involved anywhere in either sweep.
 */
#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_CANDIDATE )
   /*
    * The candidate arm's four kernels. Unlike the sweeps, these are NOT four
    * variants of one loop: sp_a/sp_b/sp_c are three schedules of opt_v1's ten
    * instructions, and x2 is a different kernel entirely with its own signature.
    *
    * So x2 cannot go through T1_SWEEP_K3 - that macro is used where a kernel has
    * opt_v1's signature. It is declared and called separately below, and
    * T1_SWEEP_K3 is pointed at sp_c so the shared paths stay well-formed.
    */
#  define T1_SWEEP_K0   biquad_cascade_df2T_f32_dspic33ak_sp_a
#  define T1_SWEEP_K1   biquad_cascade_df2T_f32_dspic33ak_sp_b
#  define T1_SWEEP_K2   biquad_cascade_df2T_f32_dspic33ak_sp_c
#  define T1_SWEEP_K3   biquad_cascade_df2T_f32_dspic33ak_sp_c   /* see T1_ARM_X2 */
   /* Instructions per TWO SAMPLES, to keep the column comparable with the
    * sweeps. sp_* are opt_v1's ten per sample; x2 is 18 per PAIR OF CHANNELS,
    * which is 9 per sample - the one arm where this column's "pair" means two
    * channels rather than two samples. */
#  define T1_SWEEP_PAIR_INSTR_0   20u
#  define T1_SWEEP_PAIR_INSTR_1   20u
#  define T1_SWEEP_PAIR_INSTR_2   20u
#  define T1_SWEEP_PAIR_INSTR_3   18u
#  define T1_SWEEP_NAME_0   "sp_a store-last"
#  define T1_SWEEP_NAME_1   "sp_b store-mid"
#  define T1_SWEEP_NAME_2   "sp_c copy-moved"
#  define T1_SWEEP_NAME_3   "x2 2ch-interleave"
#  define T1_SWEEP_LABEL    "candidate comparison"
#  define T1_SWEEP_EDGE     "y producer -> y consumer distance; x2 = two independent chains"
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_GAP_SWEEP )
#  define T1_SWEEP_K0   biquad_cascade_df2T_f32_dspic33ak_gap0
#  define T1_SWEEP_K1   biquad_cascade_df2T_f32_dspic33ak_gap1
#  define T1_SWEEP_K2   biquad_cascade_df2T_f32_dspic33ak_gap2
#  define T1_SWEEP_K3   biquad_cascade_df2T_f32_dspic33ak_gap3
   /* Instructions per TWO SAMPLES, from the disassembly of gap.s: the slots go
    * in once per pair, at the A->B edge, so the step is one per arm. */
#  define T1_SWEEP_PAIR_INSTR_0   17u
#  define T1_SWEEP_PAIR_INSTR_1   18u
#  define T1_SWEEP_PAIR_INSTR_2   19u
#  define T1_SWEEP_PAIR_INSTR_3   20u
#  define T1_SWEEP_NAME_0   "gap0 (=opt_v3)"
#  define T1_SWEEP_NAME_1   "gap1"
#  define T1_SWEEP_NAME_2   "gap2"
#  define T1_SWEEP_NAME_3   "gap3"
#  define T1_SWEEP_LABEL    "A->B spacing sweep"
#  define T1_SWEEP_EDGE     "sample A's producers -> sample B's consumers"
#else
#  define T1_SWEEP_K0   biquad_cascade_df2T_f32_dspic33ak_d1gap0
#  define T1_SWEEP_K1   biquad_cascade_df2T_f32_dspic33ak_d1gap1
#  define T1_SWEEP_K2   biquad_cascade_df2T_f32_dspic33ak_d1gap2
#  define T1_SWEEP_K3   biquad_cascade_df2T_f32_dspic33ak_d1gap3
   /* Instructions per TWO SAMPLES, from the disassembly of d1gap.s. The slots go
    * in ONCE PER SAMPLE here, so the step is TWO per arm, not one - a different
    * expectation from gap.s, and the gate below is what catches getting it
    * wrong. */
#  define T1_SWEEP_PAIR_INSTR_0   17u
#  define T1_SWEEP_PAIR_INSTR_1   19u
#  define T1_SWEEP_PAIR_INSTR_2   21u
#  define T1_SWEEP_PAIR_INSTR_3   23u
#  define T1_SWEEP_NAME_0   "d1gap0 (=opt_v3)"
#  define T1_SWEEP_NAME_1   "d1gap1"
#  define T1_SWEEP_NAME_2   "d1gap2"
#  define T1_SWEEP_NAME_3   "d1gap3"
#  define T1_SWEEP_LABEL    "in-sample d1 spacing sweep"
#  define T1_SWEEP_EDGE     "d1 partial -> d1 final, both samples"
#endif

extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void T1_SWEEP_K0(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void T1_SWEEP_K1(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void T1_SWEEP_K2(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void T1_SWEEP_K3(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

#if T1_TWO_CHANNEL_BUILD
/*
 * x2's own signature: two instances, two input pointers, two output pointers.
 *
 * BOTH INSTANCES MUST HAVE THE SAME numStages - the kernel drives both cascades
 * from channel A's count. t1_configure_b() below builds channel B with the same
 * section count as channel A, and the equivalence gate would catch a mismatch as
 * a bit difference on channel B, so the constraint is enforced rather than
 * trusted.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x2(
    const mchp_biquad_cascade_df2T_instance_f32 * SA,
    const float32_t * pSrcA,
          float32_t * pDstA,
    const mchp_biquad_cascade_df2T_instance_f32 * SB,
    const float32_t * pSrcB,
          float32_t * pDstB,
          uint32_t    blockSize );

/*
 * x2r: the same call shape as x2, plus opt_v3's rotation inside. EVEN blockSize
 * only, because it unrolls two samples per channel - T1_FRAMES is 32, and the
 * kernel returns having written nothing if that ever changes to an odd value,
 * which the equivalence gate would catch as an all-samples mismatch rather than
 * as silence.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x2r(
    const mchp_biquad_cascade_df2T_instance_f32 * SA,
    const float32_t * pSrcA,
          float32_t * pDstA,
    const mchp_biquad_cascade_df2T_instance_f32 * SB,
    const float32_t * pSrcB,
          float32_t * pDstB,
          uint32_t    blockSize );

/*
 * x3: THREE channels per call, and an EXPERIMENT WITH A NEGATIVE PREDICTION
 * rather than a candidate. x2 is issue-bound (CPI 1.004, ev8 = ev9 = 0), so a
 * third chain has no stall left to cover and removes no instruction per sample;
 * the prediction recorded before measuring is a small LOSS from the larger
 * per-stage prologue. It is measured to settle whether the core really issues
 * one instruction per cycle here - if x3 comes out materially FASTER, that
 * premise is wrong and the finding is about the core, not about channel count.
 *
 * Array arguments, not nine scalars: only W0..W6 carry arguments.
 * ALL THREE INSTANCES MUST HAVE THE SAME numStages (S[0]'s drives all three).
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x3(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/*
 * x3r: x3's three channels PLUS x2r's register rotation - the combination of the
 * two independent levers. x2r removed one mov.s per sample but handed a cycle
 * back to ev8 with only two chains to cover it; x3 removed loop-control overhead
 * and kept ev8 at zero with a third chain available. This applies both, and it
 * is a CANDIDATE rather than an experiment.
 *
 * EVEN blockSize only - it unrolls two samples per channel, the same narrowing
 * x2r has. T1_FRAMES is 32, and the kernel returns having written nothing if that
 * ever becomes odd, which the equivalence gate would catch as an all-samples
 * mismatch rather than as silence.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x3r(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/*
 * x3rl: x3r with channel A's a2 coefficient LATE-LOADED from memory over the
 * register that just held x, instead of kept resident. A MICRO-EXPERIMENT, not a
 * candidate - it is x3r plus two CPU-side loads per iteration and is not expected
 * to be faster.
 *
 * It exists to answer whether a CPU-side memory->F-register move can hide inside
 * an FPU stall, because that decides whether x4 is possible at all: x4 needs 8
 * F-registers per channel (32 total) rather than 9 (36, and the part has 32), and
 * late-loading a2 is what gets it to 8. If the move is free, x4 becomes realistic;
 * if it costs a full slot, x4 is closed by measurement rather than by argument.
 *
 * Channels B and C are untouched, so the whole delta against x3r is the reload.
 * a2's value is unchanged, so channel A must stay bit-identical.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x3rl(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/*
 * x3t: three channels, THREE samples per iteration, and only THREE work
 * registers per channel - a different rotation topology, not x3r with an extra
 * sample. A CANDIDATE.
 *
 * It removes x3r's mov.s outright instead of trading for it: old d2 is itself the
 * accumulator b1*x wants, and x's register is recycled in place by
 * `mul.s b2,C,C`, so the body is 7 instructions per channel-sample against x3r's
 * 8. That also drops the per-channel F-register budget from 9 to 8, which is what
 * would make a four-channel kernel representable (4 x 8 = 32 = F0..F31) - not
 * attempted yet.
 *
 * Unlike x2r/x3r this kernel does NOT narrow the blockSize contract: the
 * remainder after the triples is handled by a tail that runs the same seven
 * instructions one sample at a time, so it takes any 1..512 including odd.
 *
 * Before it was written: the recurrence was expanded symbolically and matches
 * DF2T exactly, and `mul.s` writing its own source was confirmed to assemble with
 * a distinguishable encoding (06 71 0e 84 against 06 71 0c 84).
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x3t(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/*
 * x3td: x3t with the LOOP CONTROL REPLACED BY DTB, and nothing else changed. A
 * CANDIDATE, and the loop shape the four-channel kernel will be built on.
 *
 * x3t tests and decrements its sample counter every iteration - five instructions
 * (cp, bra, sub, the assembler's neop, bra) over nine channel-samples = 0.556,
 * which is 64 % of its entire overhead above the 7.000 arithmetic floor. Here the
 * triple count and remainder are computed ONCE PER CALL and the sample loop is
 * driven by `dtb`: one instruction, 0.111 per channel-sample.
 *
 * Counted from its own disassembly, not from the source: body 64 per iteration
 * against x3t's 68, prologue 24 against 22 (the per-stage reload of the trip count
 * plus a zero guard), tail and write-backs identical. Predicted 7.474 against
 * x3t's measured 7.874. THE CYCLE COUNT IS NOT PREDICTED.
 *
 * Same 1..512 contract as x3t, including odd and including 1 and 2 - blockSize
 * below three yields zero triples, which `dtb` cannot express (it decrements
 * before testing, so zero would wrap), so that case branches straight to the tail.
 *
 * Bit-identical to x3t on all three channels by construction: the arithmetic
 * instructions and their order are untouched.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x3td(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/*
 * x4t: x3td's topology with a FOURTH channel, and the reason the three-channel
 * work was done. A CANDIDATE.
 *
 * Same call shape as x3td - it just reads a fourth element from each array. Same
 * seven instructions per channel-sample, same three-sample rotation, same `dtb`
 * loop control. The only difference is the channel count, deliberately: x3td
 * measured the dtb change on its own, so if a stall appears here it is the fourth
 * channel and nothing else.
 *
 * ITS VALUE IS NOT ITS PER-KERNEL NUMBER. The audio path has four channels, so a
 * three-channel kernel must put the fourth through opt_v1 at 11.485:
 *     x3td x3 + opt_v1 x1 = 8.567     x4t, one kernel = ~7.55 predicted
 * The point of spending a fourth channel's registers is to stop paying opt_v1.
 *
 * BOTH REGISTER BANKS ARE NOW FULL. F: 4 x (5 coeff + 3 work) = 32 = F0..F31
 * exactly. W: four channels want 18 and the part has 15 usable (W16 does not
 * exist - the assembler was asked), so three pointers are spilled to the frame,
 * and they are COEFFICIENT pointers because a spilled coeff costs 2 instructions
 * per stage against a spilled state pointer's 3.
 *
 * Same 1..512 contract as x3td, including odd.
 *
 * Instruction count from its own linked disassembly, RAW - no calibration factor
 * carried over from another kernel, which is the one thing x3td's prediction got
 * wrong: 968 per stage over 128 channel-samples = 7.563 against x3td's measured
 * 7.579. THE CYCLE COUNT IS NOT PREDICTED.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_x4t(
    const mchp_biquad_cascade_df2T_instance_f32 * const * S,
    const float32_t * const * pSrc,
          float32_t * const * pDst,
          uint32_t    blockSize );

/* x1s2p: one channel, two adjacent SOS stages fused.  This is the portability
 * experiment: unlike x4t, it keeps the public single-channel CMSIS call shape
 * and must stand on its own.  The current target-safe baseline has no I/O
 * pipeline; only the inter-stage RAM round-trip is removed. */
extern void biquad_cascade_df2T_f32_dspic33ak_x1s2p(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

/* x1s3p: one channel, three adjacent SOS stages fused.  Its two input F
 * registers preload src[n+1] before writing dst[n]; no output is deferred. */
extern void biquad_cascade_df2T_f32_dspic33ak_x1s3p(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

extern void biquad_cascade_df2T_f32_dspic33ak_x1s3p_readyN(const mchp_biquad_cascade_df2T_instance_f32 *S, const float32_t *pSrc, float32_t *pDst, uint32_t blockSize);
extern void biquad_cascade_df2T_f32_dspic33ak_x1s3p_ready32(const mchp_biquad_cascade_df2T_instance_f32 *S, const float32_t *pSrc, float32_t *pDst, uint32_t blockSize);

/* x2c: x2's schedule with the CPU-side stores clustered at the end. Keeps
 * opt_v1's 1..512 blockSize contract - it is not unrolled. */
extern void biquad_cascade_df2T_f32_dspic33ak_x2c(
    const mchp_biquad_cascade_df2T_instance_f32 * SA,
    const float32_t * pSrcA,
          float32_t * pDstA,
    const mchp_biquad_cascade_df2T_instance_f32 * SB,
    const float32_t * pSrcB,
          float32_t * pDstB,
          uint32_t    blockSize );
#endif
#endif

/* ------------------------------------------------------------------ */
/* Bit-pattern and CRC helpers                                         */
/* ------------------------------------------------------------------ */
/*
 * memcpy, not a pointer cast: the cast would be a strict-aliasing violation
 * and the optimiser is entitled to act on that.
 */
static float32_t bits_to_f32( uint32_t bits )
{
    float32_t v;

    memcpy( &v, &bits, sizeof(v) );
    return v;
}

/*
 * CRC-32 (IEEE, reflected, init 0xFFFFFFFF, final xor) over the little-endian
 * float32 bit patterns of a bank - the same bytes zlib.crc32 saw in the
 * generator. Bytewise and table-free: this runs once at init, and a 1 KiB table
 * to save microseconds in a function called twice would be a poor trade in a
 * project that watches its RAM.
 */
static uint32_t crc32_words( const uint32_t *words, uint32_t count )
{
    uint32_t crc = 0xFFFFFFFFu;

    for( uint32_t i = 0u; i < count; i++ )
    {
        const uint32_t w = words[i];

        for( uint32_t b = 0u; b < 4u; b++ )
        {
            crc ^= ( w >> ( 8u * b ) ) & 0xFFu;   /* little-endian byte order */

            for( uint32_t k = 0u; k < 8u; k++ )
            {
                crc = ( crc & 1u ) ? ( ( crc >> 1 ) ^ 0xEDB88320u )
                                   : ( crc >> 1 );
            }
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* Bank description                                                    */
/* ------------------------------------------------------------------ */
typedef struct
{
    const char     *name;
    const uint32_t *coeff_bits;
    uint32_t        sections;
    const uint32_t (*expected)[ IIR_BENCH_FRAMES ];
    float32_t       tolerance;
    uint32_t        warmup_blocks;
    uint32_t        crc_expected;
} t1_bank_t;

static const t1_bank_t s_bank_lp =
{
    "lp6", iir_bench_lp_coeff_bits, IIR_BENCH_LP_SECTIONS,
    iir_bench_lp_expected_bits, IIR_BENCH_LP_TOLERANCE,
    IIR_BENCH_LP_WARMUP_BLOCKS, IIR_BENCH_LP_CRC32,
};

static const t1_bank_t s_bank_ap =
{
    "ap84", iir_bench_ap_coeff_bits, IIR_BENCH_AP_SECTIONS,
    iir_bench_ap_expected_bits, IIR_BENCH_AP_TOLERANCE,
    IIR_BENCH_AP_WARMUP_BLOCKS, IIR_BENCH_AP_CRC32,
};

static const t1_bank_t *bank_of( biquad_tier1_bank_t which )
{
    return ( which == BIQUAD_TIER1_BANK_AP ) ? &s_bank_ap : &s_bank_lp;
}

/* ------------------------------------------------------------------ */
/* Configure / run one block                                           */
/* ------------------------------------------------------------------ */
#define T1_FAULT_COEFF_DELTA    (1.0e-3f)
#define T1_FAULT_COEFF_INDEX    (3u)     /* -a1 of section 0: a feedback term,
                                          * so the error propagates through the
                                          * whole cascade rather than scaling
                                          * one tap */

static bool t1_configure( const t1_bank_t *bank,
                          uint32_t sections,
                          biquad_tier1_fault_t fault )
{
    if( ( sections == 0u ) || ( sections > bank->sections ) )
    {
        /* Longer than the bank: refused, because the alternative is a read
         * past the end of a const array that produces plausible numbers. */
        return false;
    }
    if( ( fault == BIQUAD_TIER1_FAULT_SECTION_COUNT ) && ( sections < 2u ) )
    {
        /* Nothing to take away. Refused rather than silently ignored: a fault
         * injection that quietly does nothing turns the negative test into a
         * test that always passes. */
        return false;
    }

    for( uint32_t i = 0u; i < ( sections * 5u ); i++ )
    {
        s_coeff[i] = bits_to_f32( bank->coeff_bits[i] );
    }

    if( fault == BIQUAD_TIER1_FAULT_COEFF )
    {
        s_coeff[ T1_FAULT_COEFF_INDEX ] += T1_FAULT_COEFF_DELTA;
    }

    const uint32_t n = ( fault == BIQUAD_TIER1_FAULT_SECTION_COUNT )
                           ? ( sections - 1u )
                           : sections;

    memset( s_state, 0x00, sizeof(s_state) );

    /*
     * Through the library's own initializer, never by assigning the struct
     * fields directly. The ASRC H2 bench learned this the hard way: a Phase-2
     * attempt that filled the instance in C faulted with a BUS ERROR, and the
     * fix was to call this function. The instance layout is the library's to
     * define, not the caller's to assume.
     */
    mchp_biquad_cascade_df2T_init_f32( &s_inst, (uint8_t)n, s_coeff, s_state );

    return true;
}

#if T1_TWO_CHANNEL_BUILD
/*
 * Channel B, for the x2 arm.
 *
 * SAME COEFFICIENTS AND SAME SECTION COUNT as channel A, deliberately. The
 * kernel drives both cascades from channel A's stage count, and giving B a
 * different filter would test two things at once. What makes B a genuine second
 * channel for scheduling purposes is that it has its OWN coefficient array, its
 * OWN state and its OWN registers - the FPU cannot know the numbers happen to
 * match, so the dependency structure the kernel exploits is real either way.
 *
 * Fault injection is not offered here: the faults exist to prove the KAT can
 * fail, and the KAT runs on channel A.
 */
static bool t1_configure_b( const t1_bank_t *bank, uint32_t sections )
{
    if( ( sections == 0u ) || ( sections > bank->sections ) )
    {
        return false;
    }

    for( uint32_t i = 0u; i < ( sections * 5u ); i++ )
    {
        s_coeff_b[i] = bits_to_f32( bank->coeff_bits[i] );
    }

    memset( s_state_b, 0x00, sizeof(s_state_b) );

    mchp_biquad_cascade_df2T_init_f32( &s_inst_b, (uint8_t)sections,
                                       s_coeff_b, s_state_b );
    return true;
}

/*
 * Channel C, for the x3 arm. Same coefficients and same section count as A and
 * B, for the reason given above channel B: what makes it a genuine third channel
 * for scheduling purposes is its OWN coefficient array, its OWN state and its
 * OWN registers, not different numbers.
 *
 * Also rebuilds x3's argument arrays, because a fresh init() writes new pointers
 * into the instances and the arrays must point at the current ones.
 */
static bool t1_configure_c( const t1_bank_t *bank, uint32_t sections )
{
    if( ( sections == 0u ) || ( sections > bank->sections ) )
    {
        return false;
    }

    for( uint32_t i = 0u; i < ( sections * 5u ); i++ )
    {
        s_coeff_c[i] = bits_to_f32( bank->coeff_bits[i] );
    }

    memset( s_state_c, 0x00, sizeof(s_state_c) );

    mchp_biquad_cascade_df2T_init_f32( &s_inst_c, (uint8_t)sections,
                                       s_coeff_c, s_state_c );

    s_x3_inst[0] = &s_inst;   s_x3_src[0] = s_in;    s_x3_dst[0] = s_out;
    s_x3_inst[1] = &s_inst_b; s_x3_src[1] = s_in_b;  s_x3_dst[1] = s_out_b;
    s_x3_inst[2] = &s_inst_c; s_x3_src[2] = s_in_c;  s_x3_dst[2] = s_out_c;
    return true;
}

/*
 * Channel D, for the x4t arm. Same coefficients and same section count as A, B
 * and C, for the reason given above channel B: what makes it a genuine fourth
 * channel for scheduling purposes is its OWN coefficient array, its OWN state and
 * its OWN registers, not different numbers.
 *
 * Fills slot 3 of the shared argument arrays. It must run AFTER t1_configure_c(),
 * which writes slots 0..2 - so both are called together, in order, everywhere.
 */
static bool t1_configure_d( const t1_bank_t *bank, uint32_t sections )
{
    if( ( sections == 0u ) || ( sections > bank->sections ) )
    {
        return false;
    }

    for( uint32_t i = 0u; i < ( sections * 5u ); i++ )
    {
        s_coeff_d[i] = bits_to_f32( bank->coeff_bits[i] );
    }

    memset( s_state_d, 0x00, sizeof(s_state_d) );

    mchp_biquad_cascade_df2T_init_f32( &s_inst_d, (uint8_t)sections,
                                       s_coeff_d, s_state_d );

    s_x3_inst[3] = &s_inst_d; s_x3_src[3] = s_in_d;  s_x3_dst[3] = s_out_d;
    return true;
}
#endif

static void t1_load_input( uint32_t channel )
{
    for( uint32_t i = 0u; i < T1_FRAMES; i++ )
    {
        s_in[i] = bits_to_f32( iir_bench_input_bits[channel][i] );
    }
}

#if T1_TWO_CHANNEL_BUILD
/*
 * Channel B gets a DIFFERENT input block from the shared reference, so that a
 * kernel which accidentally filtered channel A's samples twice - a plausible
 * way to get an interleaved kernel wrong - would show up as a bit difference on
 * channel B rather than passing.
 */
static void t1_load_input_b( uint32_t channel )
{
    for( uint32_t i = 0u; i < T1_FRAMES; i++ )
    {
        s_in_b[i] = bits_to_f32( iir_bench_input_bits[channel][i] );
    }
}

/*
 * Channel C gets a THIRD distinct input block, so a kernel that filtered one
 * channel's samples three times - the plausible way to get a three-channel
 * kernel wrong - shows up as a bit difference rather than passing.
 */
static void t1_load_input_c( uint32_t channel )
{
    for( uint32_t i = 0u; i < T1_FRAMES; i++ )
    {
        s_in_c[i] = bits_to_f32( iir_bench_input_bits[channel][i] );
    }
}

/*
 * Channel D gets a FOURTH distinct input block, so a kernel that filtered one
 * channel's samples four times - the plausible way to get a four-channel kernel
 * wrong - shows up as a bit difference rather than passing. The shared reference
 * header carries eight, so a fourth distinct block exists to use.
 */
static void t1_load_input_d( uint32_t channel )
{
    for( uint32_t i = 0u; i < T1_FRAMES; i++ )
    {
        s_in_d[i] = bits_to_f32( iir_bench_input_bits[channel][i] );
    }
}
#endif

#if T1_SWEEP_BUILD
/*
 * THE SPACING SWEEP'S ARMS, AND WHY THIS IS NOT A FUNCTION POINTER.
 *
 * The obvious way to put five kernels in one image is a function-pointer table
 * that t1_process() dereferences. That would put an INDIRECT CALL inside the
 * timed region, on the measured path, for all five arms - and this experiment
 * is trying to resolve a difference of about 2.5 cycles per sample per section.
 * An instrument may not add an unknown of that kind to the thing it measures.
 *
 * So each arm gets its own timed-region function, and each one contains exactly
 * ONE DIRECT CALL. The selection happens in t1_measure_arm() below, OUTSIDE the
 * timer, once per measurement rather than once per block. The timed path is then
 * structurally identical to the single-kernel arms' - a direct call and nothing
 * else - and the five arms are identical to each other by construction.
 *
 * t1_arm_t is an index into the descriptor table, not a callable: nothing in
 * this file ever calls a kernel through a variable.
 */
typedef enum
{
    T1_ARM_OPT_V1 = 0,
    T1_ARM_S0,          /* 0 slots = opt_v3's schedule, whichever edge  */
    T1_ARM_S1,          /* +1 slot                                      */
    T1_ARM_S2,          /* +2                                           */
    T1_ARM_S3,          /* +3                                           */
#if T1_TWO_CHANNEL_BUILD
    /*
     * A sixth arm, in the candidate build only: x2r, the two-channel interleave
     * with opt_v3's register rotation folded in. The sweeps keep exactly five
     * arms so their recorded numbers stay comparable.
     */
    T1_ARM_S4,
    /*
     * A seventh arm: x2c, x2 with both stores moved to the end of the loop. Same
     * 18 instructions, same arithmetic - only the CPU-side placement differs, so
     * it isolates "does a longer run of consecutive FPU instructions cost
     * anything on this part" at fixed instruction count.
     */
    T1_ARM_S5,
    /*
     * An eighth arm: x3, three channels per call. Experiment, not candidate -
     * see the extern above. Present in the candidate build only.
     */
    T1_ARM_S6,
    /*
     * A ninth arm: x3r, three channels with rotation. Candidate - it is allowed
     * to replace Best-available if it measures faster and stays bit-exact.
     */
    T1_ARM_S7,
    /*
     * A tenth arm: x3rl, the a2 late-load micro-experiment. Experiment, not
     * candidate - see the extern above.
     */
    T1_ARM_S8,
    /*
     * An eleventh arm: x3t, the 3-register / 3-sample rotation. Candidate - it is
     * allowed to replace Best-available if it measures faster and stays bit-exact.
     */
    T1_ARM_S9,
    /*
     * A twelfth arm: x3td, x3t with dtb loop control. Candidate - it is allowed to
     * replace Best-available if it measures faster and stays bit-exact.
     */
    T1_ARM_S10,
    /*
     * A thirteenth arm: x4t, x3td's topology with a FOURTH channel. Candidate,
     * and the one that can close the audio path's four channels in a single
     * kernel rather than leaving the fourth to opt_v1.
     */
    T1_ARM_S11,
    /* Fourteenth arm: 1ch x 2 SOS fusion, target-safe no-I/O-pipeline base. */
    T1_ARM_S12,
    /* Fifteenth arm: 1ch x 3 SOS fusion with next-input ping-pong. */
    T1_ARM_S13,
    T1_ARM_S14, /* RAW4/div */
    T1_ARM_S15, /* RAW4/div/N32 */
#endif
    T1_ARM_COUNT,
} t1_arm_t;

/*
 * The measurement order the owner asked for:
 *
 *   v1 -> gap0 -> gap1 -> gap2 -> gap3 -> v1
 *
 * opt_v1 appears FIRST AND LAST on purpose. The closing repeat is a drift check
 * covering everything a single-pass sweep cannot separate from the variable:
 * die temperature over the run, any clock or PLL movement, and the measurement
 * system itself. If the two opt_v1 readings agree, every gap figure between them
 * was taken under the same conditions; if they do not, the sweep is void and the
 * drift is the finding, not the spacing.
 */
static const t1_arm_t s_arm_order[] =
{
    T1_ARM_OPT_V1,
    T1_ARM_S0,
    T1_ARM_S1,
    T1_ARM_S2,
    T1_ARM_S3,
#if T1_TWO_CHANNEL_BUILD
    T1_ARM_S4,
    T1_ARM_S5,
    T1_ARM_S6,
    T1_ARM_S7,
    T1_ARM_S8,
    T1_ARM_S9,
    T1_ARM_S10,
    T1_ARM_S11,
    T1_ARM_S12,
    T1_ARM_S13,
    T1_ARM_S14, /* RAW4/div */
    T1_ARM_S15, /* RAW4/div/N32 */
#endif
    T1_ARM_OPT_V1,
};

/*
 * Expected instructions per two samples, from the disassembly of the selected
 * sweep's kernel file. Printed next to the PMU's measured instruction count so a
 * spacer that did not retire - or an alignment pad that appeared - is visible in
 * the result rather than silently folded into the cycle figure.
 *
 * The step differs between the two sweeps and that is the point of keeping these
 * numbers next to the kernel selection above rather than hard-coding them here:
 * gap.s inserts once per PAIR (+1 per arm), d1gap.s once per SAMPLE (+2 per arm).
 * A build that took the wrong table would print a mismatch on every arm, which
 * is the intended failure mode.
 *
 * opt_v1's 20 is its 10/sample x 2, for a like-for-like column; it is not a
 * pair-unrolled kernel.
 */
static const uint8_t s_arm_instr_per_pair[ T1_ARM_COUNT ] =
{
    20u,                        /* opt_v1: 10 per sample            */
    T1_SWEEP_PAIR_INSTR_0,      /* arm 0 : opt_v3's schedule        */
    T1_SWEEP_PAIR_INSTR_1,      /* arm 1 : +1 slot                  */
    T1_SWEEP_PAIR_INSTR_2,      /* arm 2 : +2                       */
    T1_SWEEP_PAIR_INSTR_3,      /* arm 3 : +3                       */
#if T1_TWO_CHANNEL_BUILD
    /* x2r: 33 instructions per iteration, and one iteration is FOUR
     * channel-samples (two samples on each of two channels), so the
     * "per pair of samples" column is 33/2 = 16.5 -> printed as 16. The
     * measured PMU figure is what matters; this column is a sanity anchor. */
    16u,
    /* x2c: 18 per pair of channel-samples, same as x2. */
    18u,
    /* x3: 27 instructions per iteration, and one iteration is THREE
     * channel-samples, so 9 per sample - the same per-sample count as x2, which
     * is the whole point of the experiment. The per-pair column is 9 x 2 = 18,
     * for a like-for-like reading against x2's 18. */
    18u,
    /* x3r: 48 instructions per iteration over SIX channel-samples = 8 per
     * sample, so 16 in the per-pair column - the same as x2r's, which is the
     * comparison that matters. */
    16u,
    /* x3rl: 50 per iteration over six channel-samples = 8.333 per sample. The
     * per-pair column rounds to 16, the same as x3r's - the PMU's measured
     * instruction count is what separates them, and it should come out about
     * 0.333 higher. */
    16u,
    /* x3t: 63 arithmetic + 2 loop control per iteration over NINE channel-samples
     * = 7.222 per sample, so 14 in the per-pair column against x3r's 16. This is
     * the only arm in the table whose iteration is not a pair of samples, so the
     * column is a scaled anchor rather than a literal count - the PMU figure is
     * what matters. */
    14u,
    /* x3td: 63 arithmetic + 1 dtb per iteration over NINE channel-samples =
     * 7.111 per sample, so 14 in the per-pair column - the same rounded value as
     * x3t's, because the difference between them (0.444) is smaller than this
     * column's resolution. The PMU's measured instruction count is what separates
     * them, and it should come out about 0.4 LOWER than x3t's. */
    14u,
    /* x4t: 84 arithmetic + 1 dtb per iteration over TWELVE channel-samples =
     * 7.083 per sample, so 14 in the per-pair column again - the fourth channel
     * changes the count per ITERATION, not per channel-sample, which is the
     * whole claim this arm tests. The PMU figure is what separates it from x3td,
     * and the RAW model over a whole stage (968/128 = 7.563) says it should land
     * within 0.02 of x3td's 7.579 - the extra prologue, not the extra channel,
     * being the only difference in the arithmetic. */
    14u,
    /* x1s2p: 12 data instructions/sample, hence 24 per displayed pair.
     * The PMU and linked disassembly remain authoritative for all fixed costs. */
    24u,
    /* x1s3p: 17 instructions in a preloading sample body, hence 34 per
     * displayed pair.  The final body is one instruction shorter. */
    34u,
    34u, /* ready */
    34u, /* ready32 */
#endif
};

static const char *const s_arm_name[ T1_ARM_COUNT ] =
{
    "opt_v1",
    T1_SWEEP_NAME_0,
    T1_SWEEP_NAME_1,
    T1_SWEEP_NAME_2,
    T1_SWEEP_NAME_3,
#if T1_TWO_CHANNEL_BUILD
    "x2r 2ch+rotation",
    "x2c 2ch-FPU-clustered",
    "x3 3ch interleave",
    "x3r 3ch+rotation",
    "x3rl a2 late-load",
    "x3t 3reg/3sample rot",
    "x3td x3t+dtb loopctl",
    "x4t 4ch/3reg/dtb",
    "x1s2p 1ch/2SOS fused base",
    "x1s3p 1ch/3SOS input-pingpong",
    "x1s3p_readyN RAW4/div",
    "x1s3p_ready32 RAW4/div/N32",
#endif
};

/* One timed-region function per arm. Each body is one direct call. */
static void t1_process_opt_v1( void )
{
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst, s_in, s_out, T1_FRAMES );
}

static void t1_process_s0( void )
{
    T1_SWEEP_K0( &s_inst, s_in, s_out, T1_FRAMES );
}

static void t1_process_s1( void )
{
    T1_SWEEP_K1( &s_inst, s_in, s_out, T1_FRAMES );
}

static void t1_process_s2( void )
{
    T1_SWEEP_K2( &s_inst, s_in, s_out, T1_FRAMES );
}

#if T1_TWO_CHANNEL_BUILD
static void t1_process_s5( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x2c( &s_inst,   s_in,   s_out,
                                           &s_inst_b, s_in_b, s_out_b,
                                           T1_FRAMES );
}

static void t1_process_s4( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x2r( &s_inst,   s_in,   s_out,
                                           &s_inst_b, s_in_b, s_out_b,
                                           T1_FRAMES );
}

static void t1_process_s6( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x3( s_x3_inst, s_x3_src, s_x3_dst,
                                          T1_FRAMES );
}

static void t1_process_s7( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x3r( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES );
}

static void t1_process_s8( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x3rl( s_x3_inst, s_x3_src, s_x3_dst,
                                            T1_FRAMES );
}

static void t1_process_s9( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x3t( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES );
}

static void t1_process_s10( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x3td( s_x3_inst, s_x3_src, s_x3_dst,
                                            T1_FRAMES );
}

static void t1_process_s11( void )
{
    /* Same arrays as the three-channel arms: x4t reads the fourth element that
     * t1_configure_d() filled, and the others read only the first three. */
    biquad_cascade_df2T_f32_dspic33ak_x4t( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES );
}

static void t1_process_s12( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x1s2p( &s_inst, s_in, s_out, T1_FRAMES );
}

static void t1_process_s13( void )
{
    biquad_cascade_df2T_f32_dspic33ak_x1s3p( &s_inst, s_in, s_out, T1_FRAMES );
}
static void t1_process_s14(void)
{
    biquad_cascade_df2T_f32_dspic33ak_x1s3p_readyN(&s_inst, s_in, s_out, T1_FRAMES);
}

static void t1_process_s15(void)
{
    biquad_cascade_df2T_f32_dspic33ak_x1s3p_ready32(&s_inst, s_in, s_out, T1_FRAMES);
}

#endif

static void t1_process_s3( void )
{
#if T1_TWO_CHANNEL_BUILD
    /*
     * The candidate build's arm 3 is x2, which filters TWO channels per call.
     * Both channels run here so the correctness paths exercise the real kernel;
     * the KAT still compares channel A, and t1_x2_equivalence() below checks
     * channel B.
     */
    biquad_cascade_df2T_f32_dspic33ak_x2( &s_inst,   s_in,   s_out,
                                          &s_inst_b, s_in_b, s_out_b,
                                          T1_FRAMES );
#else
    T1_SWEEP_K3( &s_inst, s_in, s_out, T1_FRAMES );
#endif
}

/*
 * Which arm the shared paths (KAT, warm-up, sweep) run.
 *
 * Set OUTSIDE every timed region, by t1_measure_arm() and by the console's arm
 * selector. t1_process() below still dispatches on it, which is why t1_process()
 * is NOT what the sweep times - the per-arm functions above are. t1_process()
 * serves the correctness paths, where a branch costs nothing that matters.
 */
static t1_arm_t s_arm = T1_ARM_OPT_V1;
#endif /* T1_SWEEP_BUILD */

static inline void t1_process( void )
{
#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_BEST_AVAILABLE )
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst, s_in, s_out, T1_FRAMES );
#elif ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_PINGPONG_V3 )
    biquad_cascade_df2T_f32_dspic33ak_opt_v3( &s_inst, s_in, s_out, T1_FRAMES );
#elif T1_SWEEP_BUILD
    /*
     * NOT A TIMED PATH. Used by the known-answer test, the equivalence test and
     * the untimed warm-up, where a switch is free. Every timed region in this
     * arm calls one of the t1_process_<arm>() functions above directly.
     */
    switch( s_arm )
    {
    case T1_ARM_S0: t1_process_s0(); break;
    case T1_ARM_S1: t1_process_s1(); break;
    case T1_ARM_S2: t1_process_s2(); break;
    case T1_ARM_S3: t1_process_s3(); break;
#if T1_TWO_CHANNEL_BUILD
    case T1_ARM_S4: t1_process_s4(); break;
    case T1_ARM_S5: t1_process_s5(); break;
    case T1_ARM_S6: t1_process_s6(); break;
    case T1_ARM_S7: t1_process_s7(); break;
    case T1_ARM_S8: t1_process_s8(); break;
    case T1_ARM_S9: t1_process_s9(); break;
    case T1_ARM_S10: t1_process_s10(); break;
    case T1_ARM_S11: t1_process_s11(); break;
    case T1_ARM_S12: t1_process_s12(); break;
    case T1_ARM_S13: t1_process_s13(); break;
    case T1_ARM_S14: t1_process_s14(); break;
    case T1_ARM_S15: t1_process_s15(); break;
#endif
    case T1_ARM_OPT_V1:
    default:        t1_process_opt_v1(); break;
    }
#else
    mchp_biquad_cascade_df2T_f32( &s_inst, s_in, s_out, T1_FRAMES );
#endif
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
bool biquad_tier1_bench_init( void )
{
    s_ready = false;

    const uint32_t lp = crc32_words( iir_bench_lp_coeff_bits,
                                     IIR_BENCH_LP_SECTIONS * 5u );
    const uint32_t ap = crc32_words( iir_bench_ap_coeff_bits,
                                     IIR_BENCH_AP_SECTIONS * 5u );

    printf( "Tier1 bench: CRC gate - recomputed shared reference"
            " lp=%08lX ap=%08lX\r\n",
            (unsigned long)lp, (unsigned long)ap );

    if( ( lp != IIR_BENCH_LP_CRC32 ) || ( ap != IIR_BENCH_AP_CRC32 ) )
    {
        printf( "Tier1 bench: CRC MISMATCH (expected lp=%08lX ap=%08lX).\r\n",
                (unsigned long)IIR_BENCH_LP_CRC32,
                (unsigned long)IIR_BENCH_AP_CRC32 );
        printf( "  The vendored shared reference is not the one the other"
                " device is running.\r\n" );
        printf( "  Refusing to measure: a cross-device comparison of two"
                " different filters is worse\r\n" );
        printf( "  than no comparison. Re-vendor"
                " shared_ref/iir_bench_reference.h.\r\n" );
        return false;
    }

    if( nora_high_res_timer_get_count() == nora_high_res_timer_get_count() )
    {
        /* Two reads in a row returning the same value is not proof of a stalled
         * counter at 200 MHz - the reads are only a few cycles apart - so this
         * is a hint, not a verdict. The run itself refuses a zero interval. */
        printf( "Tier1 bench: high-res timer read twice with no change;"
                " if every measurement comes back zero the timer is not"
                " running.\r\n" );
    }

    s_ready = true;
    return true;
}

/*
 * Run init once, on demand.
 *
 * Measured on hardware 2026-09-17: biquad_tier1_bench_init() had NO CALLER
 * anywhere in the tree, so s_ready was never set and every console entry point
 * refused with "not initialised (CRC check did not pass)". The refusal was
 * correct in effect and misleading in wording - the CRC check had not failed,
 * it had never run - and it made the whole harness unmeasurable while looking
 * like a coefficient problem.
 *
 * Fixed here rather than by adding a call to the application's start-up path,
 * for two reasons. The bench is measurement-only code that must not depend on
 * where it happens to be wired into an application it does not own; and doing
 * it on first use means the CRC verdict is printed immediately above the
 * measurement it gates, in the same console capture, instead of thousands of
 * telemetry lines earlier at boot.
 *
 * init() is idempotent and cheap (two table CRCs, run once), so a failed
 * attempt is retried on the next command rather than latched.
 */
static bool t1_ensure_ready( const char *what )
{
    if( s_ready )
    {
        return true;
    }

    if( biquad_tier1_bench_init() )
    {
        return true;
    }

    printf( "Tier1 bench: %s refused - initialisation failed above.\r\n", what );
    return false;
}

void biquad_tier1_bench_print_build_info( void )
{
    printf( "--- Tier 1: foreground isolated DF2T bench ---\r\n" );
    printf( " kernel     : %s\r\n", T1_KERNEL_NAME );
    printf( " pairing    : %s (protocol section 1)\r\n", T1_KERNEL_PAIRING );
    printf( " geometry   : 1 ch x %u frames x N sections, %u iterations\r\n",
            (unsigned)T1_FRAMES, (unsigned)T1_ITERATIONS );
    printf( " placement  : coeff .xbss, state .ybss (as the DRC path)\r\n" );
    printf( " banks      : lp6 (CRC %08lX), ap84 (CRC %08lX)\r\n",
            (unsigned long)IIR_BENCH_LP_CRC32,
            (unsigned long)IIR_BENCH_AP_CRC32 );
    printf( " scope      : KERNEL ONLY - no gather, scatter, conversion or\r\n" );
    printf( "              expansion. This is a FLOOR, not a budget.\r\n" );
    printf( " NOT the historical in-ISR CMSIS-IIR number: that one is a single\r\n" );
    printf( "              instantaneous sample of ONE channel taken inside the\r\n" );
    printf( "              DMA0 ISR. Both are kept, under different names.\r\n" );
}

/* ------------------------------------------------------------------ */
/* Known-answer test                                                   */
/* ------------------------------------------------------------------ */
/* Residuals are printed in units of 1e-9 so no float formatter is needed.
 * Saturated rather than wrapped: a wrapped magnitude could read as a small
 * number, and "small" is the answer that matters here. */
static uint32_t err_to_nano( float32_t e )
{
    const float32_t scaled = e * 1.0e9f;

    if( scaled >= 4.0e9f )
    {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)scaled;
}

static float32_t f32_abs( float32_t v )
{
    return ( v < 0.0f ) ? -v : v;
}

/*
 * Runs every channel of the reference block through the bank and returns the
 * worst absolute residual. Each channel is an independent run from zero state,
 * which is how the generator computed the expected rows.
 *
 * BIQUAD_TIER1_FAULT_STATE_CARRY makes the observation block run on channel 0's
 * final state instead of the channel's own. This injects the state-carry class
 * of defect in the single-channel harness. Everything else about the run -
 * reset, warm-up count, coefficients - stays identical to
 * the clean path, so the only difference under test is the state the compared
 * block starts from.
 */
static bool t1_kat_once( const t1_bank_t *bank,
                         biquad_tier1_fault_t fault,
                         float32_t *worst,
                         uint32_t *worst_ch,
                         uint32_t *worst_n )
{
    float32_t w = 0.0f;
    uint32_t  wc = 0u;
    uint32_t  wn = 0u;

    for( uint32_t ch = 0u; ch < IIR_BENCH_CHANNELS; ch++ )
    {
        if( !t1_configure( bank, bank->sections, fault ) )
        {
            return false;
        }

        t1_load_input( ch );

        /* The long bank's group delay exceeds one block, so the compared block
         * is the one after warm-up - exactly as the generator computed it. */
        for( uint32_t b = 0u; b < bank->warmup_blocks; b++ )
        {
            t1_process();
        }

        /*
         * Shared-state fault injection, AFTER warm-up and immediately before the
         * observation block.
         *
         * Redesigned 2026-09-17 after the first hardware run. The original
         * version injected by skipping t1_configure() for channels 1..N, so each
         * channel inherited the previous channel's final state - and then ran the
         * bank's mandatory warm-up before the compared block. On ap84 that
         * warm-up is 8 blocks = 256 samples, which flushed the inherited state
         * and left a residual of 1,110 e-9 against a 10,000 e-9 tolerance: NOT
         * CAUGHT. That was a NEGATIVE-TEST DESIGN DEFECT, not a clean-path kernel
         * failure - and strictly speaking it was warm-up working exactly as
         * intended, since forgetting the initial state is what warm-up is for.
         * (On lp6, warm-up 0 blocks, the identical guard fired at 448,774,272
         * e-9, which is why the defect hid for as long as it did.)
         *
         * So the fault now means "the observation block ran on the wrong
         * channel's state" rather than "state was never reset". Channel 0 is
         * unaffected by construction - it is the state being borrowed - exactly
         * as it was unaffected before, so detection still comes from channels
         * 1..N.
         *
         * The state borrowed is channel 0's state at the END of its run, not at
         * the end of its warm-up. Corrected 2026-09-17, before the redesign ever
         * reached hardware, by running this known-answer test off-target
         * (the host-side known-answer test). Snapshotting after warm-up fixed ap84 -
         * 1,087,289,600 e-9, caught - and simultaneously BLINDED lp6, whose
         * warm-up is 0 blocks: with nothing processed yet, channel 0's
         * post-warm-up state is the zeroed state that t1_configure() gives every
         * channel anyway, so imposing it perturbs nothing and the residual came
         * back at exactly the clean-run value, 89 e-9. The first design was blind
         * on the long bank and the second would have been blind on the short one,
         * for opposite reasons. Taking the snapshot one block later is blind on
         * neither: after channel 0's observation block the state is non-zero for
         * any bank, whatever its warm-up count.
         *
         * Nothing else moves: warm-up counts, coefficient banks, the clean KAT
         * path and the benchmark measurement path are untouched. Only the
         * injection point of this one fault changed.
         */
        if( ( fault == BIQUAD_TIER1_FAULT_STATE_CARRY ) && ( ch != 0u ) )
        {
            memcpy( s_state, s_state_final_ch0,
                    bank->sections * 2u * sizeof(float32_t) );
        }

        t1_process();

        if( ( fault == BIQUAD_TIER1_FAULT_STATE_CARRY ) && ( ch == 0u ) )
        {
            memcpy( s_state_final_ch0, s_state,
                    bank->sections * 2u * sizeof(float32_t) );
        }

        for( uint32_t n = 0u; n < T1_FRAMES; n++ )
        {
            const float32_t ref = bits_to_f32( bank->expected[ch][n] );
            const float32_t e   = f32_abs( s_out[n] - ref );

            if( e > w )
            {
                w  = e;
                wc = ch;
                wn = n;
            }
        }
    }

    *worst    = w;
    *worst_ch = wc;
    *worst_n  = wn;
    return true;
}

static const char *t1_fault_name( biquad_tier1_fault_t f )
{
    switch( f )
    {
    case BIQUAD_TIER1_FAULT_NONE:          return "none";
    case BIQUAD_TIER1_FAULT_COEFF:         return "coefficient perturbed";
    case BIQUAD_TIER1_FAULT_STATE_CARRY:   return "observation block on channel 0 final state";
    case BIQUAD_TIER1_FAULT_SECTION_COUNT: return "one section short";
    default:                               return "?";
    }
}

static bool t1_self_test_bank( const t1_bank_t *bank )
{
    float32_t worst = 0.0f;
    uint32_t  wc = 0u, wn = 0u;
    bool      ok;

    printf( "KAT [%s]: %u ch x %u frames x %lu sections, %lu warm-up blocks,"
            " tolerance %lu e-9\r\n",
            bank->name, (unsigned)IIR_BENCH_CHANNELS, (unsigned)T1_FRAMES,
            (unsigned long)bank->sections,
            (unsigned long)bank->warmup_blocks,
            (unsigned long)err_to_nano( bank->tolerance ) );

    if( !t1_kat_once( bank, BIQUAD_TIER1_FAULT_NONE, &worst, &wc, &wn ) )
    {
        printf( "  could not configure the reference geometry - FAIL\r\n" );
        return false;
    }

    ok = ( worst <= bank->tolerance );
    printf( "  clean run      : max |err| = %lu e-9 at ch %lu sample %lu -> %s\r\n",
            (unsigned long)err_to_nano( worst ),
            (unsigned long)wc, (unsigned long)wn, ok ? "PASS" : "FAIL" );

    for( uint32_t f = (uint32_t)BIQUAD_TIER1_FAULT_NONE + 1u;
         f < (uint32_t)BIQUAD_TIER1_FAULT_COUNT;
         f++ )
    {
        const biquad_tier1_fault_t fault = (biquad_tier1_fault_t)f;

        if( !t1_kat_once( bank, fault, &worst, &wc, &wn ) )
        {
            printf( "  fault run      : could not inject '%s' - FAIL\r\n",
                    t1_fault_name( fault ) );
            ok = false;
            continue;
        }

        const bool caught = ( worst > bank->tolerance );

        printf( "  fault run      : max |err| = %lu e-9 -> %s  [%s]\r\n",
                (unsigned long)err_to_nano( worst ),
                caught ? "caught, as required" : "NOT CAUGHT - FAIL",
                t1_fault_name( fault ) );
        if( !caught )
        {
            ok = false;
        }
    }

    printf( "  verdict [%s]  : %s\r\n", bank->name, ok ? "PASS" : "FAIL" );
    return ok;
}

#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_PINGPONG_V3 )
/*
 * The opt_v3 dispatch wrapper versus opt_v1, bit for bit, at several block
 * lengths.
 *
 * WHY THIS IS SEPARATE FROM THE KAT ABOVE
 * The KAT compares against the shared reference at T1_FRAMES, which is 32 for
 * every caller in the tree. That only ever reaches the even fast path, so the
 * KAT passing says nothing about the odd-length route. A kernel whose odd path
 * is unreachable in test and reachable in production is exactly the shape of
 * defect this project has been bitten by, so the odd lengths here are the point
 * of the function, not padding.
 *
 * WHAT IS UNDER TEST IS THE WRAPPER, deliberately. The wrapper is what callers
 * see and what holds opt_v1's 1..512 contract; testing the bare kernel at odd
 * lengths would only confirm that it refuses them. Routing correctness - even
 * lengths reaching opt_v3, odd lengths reaching opt_v1, neither silently
 * dropping a sample - is the property that matters, and it is visible here as
 * bit equality at every length.
 *
 * Bit equality is the right bar rather than a tolerance. On even lengths opt_v3
 * performs the same five operations on the same values in the same order as
 * opt_v1 and only renames registers; on odd lengths the wrapper calls opt_v1
 * itself. So ANY difference is a defect in the register rotation or in the
 * routing, not rounding - and a tolerance would hide precisely the
 * one-sample-stale-state failure the swap could plausibly introduce. Nothing
 * here touches the KAT's own tolerances.
 *
 * A zero-difference result at an odd length is a weaker statement than at an
 * even one - the wrapper is comparing opt_v1 against opt_v1 - so it proves the
 * route was taken and no sample was dropped, not that any new arithmetic is
 * right. Both are worth having: dropping the tail sample is the failure this
 * catches.
 *
 * Each run re-inits the shared instance, so the comparison is per-call rather
 * than across a carried state chain.
 */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32 * S,
    const float32_t * pSrc,
          float32_t * pDst,
          uint32_t    blockSize );

/* 1 is the degenerate odd case. 7 and 31 are odd with a substantial body. 2 is
 * the smallest even case - exactly one pair. 6 and 32 are the even controls,
 * 32 being the geometry every real caller uses. */
static const uint16_t s_v3_equiv_lengths[] = { 1u, 2u, 6u, 7u, 31u, 32u };

static float32_t s_v3_ref_out[ T1_FRAMES ];

static bool t1_v3_equivalence( const t1_bank_t *bank )
{
    bool all_ok = true;

    printf( "opt_v3 dispatch vs opt_v1 [%s]: bit-exact, %lu sections\r\n",
            bank->name, (unsigned long)bank->sections );

    for( uint32_t li = 0u;
         li < ( sizeof(s_v3_equiv_lengths) / sizeof(s_v3_equiv_lengths[0]) );
         li++ )
    {
        const uint32_t len = (uint32_t)s_v3_equiv_lengths[ li ];
        uint32_t mismatches = 0u;
        uint32_t first_bad  = 0u;

        t1_load_input( 0u );

        /* Reference: opt_v1 from zero state. */
        if( !t1_configure( bank, bank->sections, BIQUAD_TIER1_FAULT_NONE ) )
        {
            printf( "  n=%2lu : could not configure - FAIL\r\n",
                    (unsigned long)len );
            all_ok = false;
            continue;
        }
        memset( s_v3_ref_out, 0x00, sizeof(s_v3_ref_out) );
        biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst, s_in, s_v3_ref_out, len );

        /* Candidate: opt_v3 from zero state, same input, same length. */
        if( !t1_configure( bank, bank->sections, BIQUAD_TIER1_FAULT_NONE ) )
        {
            printf( "  n=%2lu : could not configure - FAIL\r\n",
                    (unsigned long)len );
            all_ok = false;
            continue;
        }
        memset( s_out, 0x00, sizeof(s_out) );
        biquad_cascade_df2T_f32_dspic33ak_opt_v3_block( &s_inst, s_in, s_out, len );

        for( uint32_t n = 0u; n < len; n++ )
        {
            uint32_t a, b;

            memcpy( &a, &s_v3_ref_out[n], sizeof(a) );
            memcpy( &b, &s_out[n],        sizeof(b) );

            if( a != b )
            {
                if( mismatches == 0u )
                {
                    first_bad = n;
                }
                mismatches++;
            }
        }

        printf( "  n=%2lu %s: %lu/%lu samples differ%s -> %s\r\n",
                (unsigned long)len,
                ( ( len & 1u ) != 0u ) ? "(odd  -> opt_v1)" : "(even -> opt_v3)",
                (unsigned long)mismatches, (unsigned long)len,
                ( mismatches != 0u ) ? "" : ", bit-identical",
                ( mismatches == 0u ) ? "PASS" : "FAIL" );

        if( mismatches != 0u )
        {
            printf( "        first difference at sample %lu\r\n",
                    (unsigned long)first_bad );
            all_ok = false;
        }
    }

    printf( "  verdict        : %s\r\n", all_ok ? "PASS" : "FAIL" );
    return all_ok;
}
#endif /* BIQUAD_TIER1_KERNEL_PINGPONG_V3 */

bool biquad_tier1_bench_self_test( void )
{
    if( !t1_ensure_ready( "KAT" ) )
    {
        return false;
    }

    const bool lp_ok = t1_self_test_bank( &s_bank_lp );
    const bool ap_ok = t1_self_test_bank( &s_bank_ap );
    bool       ok    = lp_ok && ap_ok;

#if ( BIQUAD_TIER1_KERNEL == BIQUAD_TIER1_KERNEL_PINGPONG_V3 )
    /* Runs after the KAT so that a coefficient-bank or CRC problem is reported
     * as such before anything is attributed to the candidate kernel. */
    const bool eq_lp = t1_v3_equivalence( &s_bank_lp );
    const bool eq_ap = t1_v3_equivalence( &s_bank_ap );

    if( !eq_lp || !eq_ap )
    {
        ok = false;
    }
#endif

    printf( "KAT verdict: %s\r\n", ok ? "PASS" : "FAIL" );
    return ok;
}

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */
typedef struct
{
    bool     valid;
    uint32_t sections;
    uint32_t cyc_min;
    uint32_t cyc_mean;
    uint32_t cyc_max;
} t1_result_t;

/* (a * scale) / b with a 64-bit intermediate. */
static uint32_t scaled_div( uint32_t a, uint32_t scale, uint32_t b )
{
    if( b == 0u )
    {
        return 0u;
    }
    return (uint32_t)( ( (uint64_t)a * (uint64_t)scale ) / (uint64_t)b );
}

/*
 * ONE TIMER TICK IS NOT ONE INSTRUCTION CYCLE ON THIS PART.
 *
 * dsPIC33A issues up to one instruction per clock, so the instruction rate is
 * PLL1_CLK_HZ (200 MHz). The high-resolution timer is clocked at FCY
 * (100 MHz, 1:1 prescale) - the repo's "instruction-cycle frequency" comment on
 * FCY is the classic two-clocks-per-instruction dsPIC model and does not apply
 * here. So one tick is PLL1_CLK_HZ / FCY = 2 instruction cycles, and a bench
 * that prints raw ticks as "cycles" reads half the real figure.
 *
 * Confirmed two independent ways on AK512 (2026-08-21): a 199-instruction FIR
 * completing in 113 ticks is impossible at one instruction per tick, and the
 * existing telemetry's tick-to-us conversion already matches the real 333.3 us
 * TDM window.
 *
 * The ratio is derived from the clock tree rather than written as a literal 2,
 * so it cannot go quietly stale if the tree changes. The us values the timer
 * produces are correct as they stand - it is only "cycles" that needs this.
 */
#define T1_CYCLES_PER_TICK  ( (uint32_t)( PLL1_CLK_HZ / FCY ) )

_Static_assert( ( PLL1_CLK_HZ % FCY ) == 0u,
                "the instruction rate must be an integer multiple of the "
                "high-res timer clock, or ticks cannot be converted to cycles" );
_Static_assert( T1_CYCLES_PER_TICK >= 1u,
                "PLL1_CLK_HZ / FCY must be at least 1" );

static uint32_t ticks_to_cycles( uint32_t ticks )
{
    return ticks * T1_CYCLES_PER_TICK;
}

static bool t1_measure( const t1_bank_t *bank,
                        uint32_t sections,
                        t1_result_t *r )
{
    memset( r, 0, sizeof(*r) );
    r->sections = sections;

    if( !t1_configure( bank, sections, BIQUAD_TIER1_FAULT_NONE ) )
    {
        return false;
    }

    t1_load_input( 0u );

    /*
     * Warm-up, untimed: let the state variables reach realistic magnitudes and
     * the loop settle. Never fewer than 4 blocks, and never fewer than the
     * bank's own warm-up - the allpass bank's group delay exceeds one block, so
     * 4 blocks would time a cascade the signal has not reached yet.
     */
    const uint32_t warmup = ( bank->warmup_blocks > 4u )
                                ? bank->warmup_blocks : 4u;

    for( uint32_t i = 0u; i < warmup; i++ )
    {
        t1_process();
    }

    uint32_t min = 0xFFFFFFFFu;
    uint32_t max = 0u;
    uint64_t sum = 0u;

    for( uint32_t i = 0u; i < T1_ITERATIONS; i++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        t1_process();
        const uint32_t d = nora_high_res_timer_elapsed_count( t0 );

        if( d == 0u )
        {
            /* A zero interval is a stopped counter, not a free kernel.
             * Refused rather than reported. */
            return false;
        }

        if( d < min ) { min = d; }
        if( d > max ) { max = d; }
        sum += d;
    }

    s_sink ^= (uint32_t)( s_out[0] != 0.0f );

    r->cyc_min  = min;
    r->cyc_max  = max;
    r->cyc_mean = (uint32_t)( sum / (uint64_t)T1_ITERATIONS );
    r->valid    = true;
    return true;
}

#if T1_SWEEP_BUILD
/* ------------------------------------------------------------------ */
/* Spacing sweep: per-arm measurement with the PMU                     */
/* ------------------------------------------------------------------ */
/*
 * The eight events, in counter order.
 *
 * Counters 0 and 1 are the cycle and instruction references, which is what makes
 * the printed CPI meaningful (DS70005591D section 3.5.3.2). The other six are
 * the stall classification, and the choice is deliberately not just event 8:
 *
 *   8  FPU instr stall  - the hypothesis under test: FPU pipeline stalled on a
 *                         register data dependency.
 *   9  FPU read stall   - the CPU side of the same hazard. The DF2T loop stores
 *                         y one instruction after computing it, so this is where
 *                         that shows up. Reading 8 without 9 would attribute a
 *                         store-side stall to the cross-sample edge.
 *   3  FPU write stall  - the opposite direction, for completeness of the FPU
 *                         picture.
 *   7  CPU addr hazard  - a non-FPU data dependency would mean the mechanism is
 *                         not the FPU at all.
 *  13  fetch read stall - RULES OUT the instrument. The sweep arms are physically
 *                         LONGER loops, so if adding instructions cost cycles
 *                         through program fetch rather than through scheduling,
 *                         this is the counter that says so. Without it, a fetch
 *                         penalty would be indistinguishable from "spacing did
 *                         not help".
 *   6  branch mispredict- the loop is a dtb per pair; a mispredict difference
 *                         between arms would be an alternative explanation.
 *
 * Eight events, eight counters: nothing had to be dropped, so there is no second
 * pass and no risk of two passes disagreeing.
 */
/*
 * Channel B setup, as a macro so that a build without a second channel expands
 * it to nothing rather than paying an empty function call inside the
 * measurement path.
 */
#if T1_TWO_CHANNEL_BUILD
#  define T1_SETUP_CHANNEL_B( bank, sections )                                 \
        do {                                                                   \
            if( !t1_configure_b( (bank), (sections) ) ) { return false; }       \
            t1_load_input_b( 1u );                                             \
            if( !t1_configure_c( (bank), (sections) ) ) { return false; }       \
            t1_load_input_c( 2u );                                             \
            if( !t1_configure_d( (bank), (sections) ) ) { return false; }       \
            t1_load_input_d( 3u );                                             \
        } while( 0 )
#else
#  define T1_SETUP_CHANNEL_B( bank, sections )   do { } while( 0 )
#endif

/*
 * How many samples one call of this arm filters.
 *
 * All arms filter T1_FRAMES samples except the candidate build's x2, which
 * filters T1_FRAMES on each of two channels. Everything that normalises a cycle
 * count to "per sample per section" goes through this, so the x2 column is the
 * same quantity as opt_v1's rather than twice as good.
 */
static uint32_t t1_samples_per_call( t1_arm_t arm )
{
#if T1_TWO_CHANNEL_BUILD
    if( arm == T1_ARM_S11 )
    {
        /* x4t filters FOUR channels per call. Forgetting this divisor would read
         * as a 4x win rather than as a four-channel kernel. */
        return (uint32_t)T1_FRAMES * 4u;
    }
    if( ( arm == T1_ARM_S6 ) || ( arm == T1_ARM_S7 )
     || ( arm == T1_ARM_S8 ) || ( arm == T1_ARM_S9 )
     || ( arm == T1_ARM_S10 ) )
    {
        return (uint32_t)T1_FRAMES * 3u;
    }
    if( ( arm == T1_ARM_S3 ) || ( arm == T1_ARM_S4 ) || ( arm == T1_ARM_S5 ) )
    {
        return (uint32_t)T1_FRAMES * 2u;
    }
#else
    (void)arm;
#endif
    return (uint32_t)T1_FRAMES;
}

static const perf_event_t s_pmu_events[ PERF_MONITOR_COUNTERS ] =
{
    PERF_EVENT_CPU_CYCLES,          /* c0 - reference                  */
    PERF_EVENT_INSTR_COMPLETED,     /* c1 - CPI partner, spacer check  */
    PERF_EVENT_FPU_INSTR_STALL,     /* c2 - event 8, the hypothesis     */
    PERF_EVENT_FPU_READ_STALL,      /* c3 - event 9                    */
    PERF_EVENT_FPU_WRITE_STALL,     /* c4 - event 3                    */
    PERF_EVENT_ADDR_HAZARD,         /* c5 - event 7                    */
    PERF_EVENT_FETCH_READ_STALL,    /* c6 - event 13, instrument check  */
    PERF_EVENT_BRANCH_MISPREDICT,   /* c7 - event 6                    */
};

typedef struct
{
    bool     valid;
    uint32_t sections;
    uint32_t cyc_min;
    uint32_t cyc_mean;
    uint32_t cyc_max;
    bool     pmu_valid;
    uint64_t ev[ PERF_MONITOR_COUNTERS ];
} t1_arm_result_t;

/*
 * T1_DEFINE_ARM_RUN - one measurement function per arm.
 *
 * Generated from a macro for the same reason the kernels themselves are: the
 * five timed loops must be identical apart from the call inside them, and a
 * macro makes that true rather than reviewed. `call` expands to a DIRECT call,
 * so the timed region is a call and a pair of timer reads - no switch, no
 * function pointer, no table lookup.
 *
 * PMU BRACKETING IS OUTSIDE THE TIMED LOOP, NOT INSIDE EACH ITERATION. The
 * counters accumulate over all T1_ITERATIONS blocks, which is what the
 * per-sample normalisation below divides by. Bracketing every iteration would
 * put two SFR writes inside the timed window and measure the instrument.
 *
 * `pmu` is consulted before and after the loop only, never within it. So a
 * pmu=false pass and a pmu=true pass have byte-identical timed regions, which is
 * exactly what makes "does the PMU move the cycle figure?" answerable.
 */
#define T1_DEFINE_ARM_RUN( suffix, call )                                      \
static bool t1_run_##suffix( const t1_bank_t *bank,                            \
                             uint32_t sections,                                \
                             bool pmu,                                         \
                             t1_arm_result_t *r )                              \
{                                                                              \
    memset( r, 0, sizeof(*r) );                                                \
    r->sections = sections;                                                    \
                                                                               \
    if( !t1_configure( bank, sections, BIQUAD_TIER1_FAULT_NONE ) )             \
    {                                                                          \
        return false;                                                          \
    }                                                                          \
                                                                               \
    t1_load_input( 0u );                                                       \
                                                                               \
    /* Channel B, for the two-channel arm. Set up for EVERY arm in a candidate   \
     * build, not only for x2: the arrays then hold the same values whichever    \
     * arm runs, so a single-channel arm cannot be measured with a colder cache  \
     * or a different memory state than x2 was. Costs nothing inside the timed   \
     * region - this is above the warm-up, let alone the timer. */              \
    T1_SETUP_CHANNEL_B( bank, sections );                                      \
                                                                               \
    /* Same warm-up rule as t1_measure(): at least 4 blocks, and never fewer    \
     * than the bank's own, because ap84's group delay exceeds one block. Runs   \
     * through the same direct call, so the loop is warm in the same way the     \
     * measured pass will exercise it. */                                      \
    const uint32_t warmup = ( bank->warmup_blocks > 4u )                       \
                                ? bank->warmup_blocks : 4u;                    \
                                                                               \
    for( uint32_t i = 0u; i < warmup; i++ )                                    \
    {                                                                          \
        call;                                                                  \
    }                                                                          \
                                                                               \
    uint32_t min = 0xFFFFFFFFu;                                                \
    uint32_t max = 0u;                                                         \
    uint64_t sum = 0u;                                                         \
                                                                               \
    if( pmu )                                                                  \
    {                                                                          \
        perf_monitor_start();                                                   \
    }                                                                          \
                                                                               \
    for( uint32_t i = 0u; i < T1_ITERATIONS; i++ )                             \
    {                                                                          \
        const uint32_t t0 = nora_high_res_timer_get_count();                   \
        call;                                                                  \
        const uint32_t d = nora_high_res_timer_elapsed_count( t0 );            \
                                                                               \
        if( d < min ) { min = d; }                                             \
        if( d > max ) { max = d; }                                             \
        sum += d;                                                              \
    }                                                                          \
                                                                               \
    if( pmu )                                                                  \
    {                                                                          \
        perf_monitor_stop();                                                    \
        perf_monitor_read_all( r->ev, PERF_MONITOR_COUNTERS );                 \
        r->pmu_valid = true;                                                   \
    }                                                                          \
                                                                               \
    /* A zero interval is a stopped counter, not a free kernel - checked after   \
     * the loop rather than inside it, so the timed region stays a call and two   \
     * timer reads. */                                                         \
    if( min == 0u )                                                            \
    {                                                                          \
        return false;                                                          \
    }                                                                          \
                                                                               \
    s_sink ^= (uint32_t)( s_out[0] != 0.0f );                                  \
                                                                               \
    r->cyc_min  = min;                                                         \
    r->cyc_max  = max;                                                         \
    r->cyc_mean = (uint32_t)( sum / (uint64_t)T1_ITERATIONS );                 \
    r->valid    = true;                                                        \
    return true;                                                               \
}

T1_DEFINE_ARM_RUN( opt_v1,
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst, s_in, s_out, T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s0, T1_SWEEP_K0( &s_inst, s_in, s_out, T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s1, T1_SWEEP_K1( &s_inst, s_in, s_out, T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s2, T1_SWEEP_K2( &s_inst, s_in, s_out, T1_FRAMES ) )
#if T1_TWO_CHANNEL_BUILD
/*
 * x2's timed region: ONE direct call that filters two channels, exactly as the
 * other arms' timed regions hold one direct call that filters one. The per-arm
 * divisor in t1_gap_print_row() is what turns the result into cycles per sample
 * per section, so the printed figure stays comparable with opt_v1's.
 */
T1_DEFINE_ARM_RUN( s3,
    biquad_cascade_df2T_f32_dspic33ak_x2( &s_inst,   s_in,   s_out,
                                          &s_inst_b, s_in_b, s_out_b,
                                          T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s4,
    biquad_cascade_df2T_f32_dspic33ak_x2r( &s_inst,   s_in,   s_out,
                                           &s_inst_b, s_in_b, s_out_b,
                                           T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s5,
    biquad_cascade_df2T_f32_dspic33ak_x2c( &s_inst,   s_in,   s_out,
                                           &s_inst_b, s_in_b, s_out_b,
                                           T1_FRAMES ) )
/*
 * x3's timed region: ONE direct call filtering three channels. The divisor in
 * t1_samples_per_call() is 3x, so the printed figure is the same quantity as
 * opt_v1's - forgetting that would read as a 3x win.
 */
T1_DEFINE_ARM_RUN( s6,
    biquad_cascade_df2T_f32_dspic33ak_x3( s_x3_inst, s_x3_src, s_x3_dst,
                                          T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s7,
    biquad_cascade_df2T_f32_dspic33ak_x3r( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s8,
    biquad_cascade_df2T_f32_dspic33ak_x3rl( s_x3_inst, s_x3_src, s_x3_dst,
                                            T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s9,
    biquad_cascade_df2T_f32_dspic33ak_x3t( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s10,
    biquad_cascade_df2T_f32_dspic33ak_x3td( s_x3_inst, s_x3_src, s_x3_dst,
                                            T1_FRAMES ) )
/*
 * x4t's timed region: ONE direct call filtering four channels.
 * t1_samples_per_call() returns 4 x T1_FRAMES for this arm, so the printed figure
 * is the same quantity as opt_v1's.
 */
T1_DEFINE_ARM_RUN( s11,
    biquad_cascade_df2T_f32_dspic33ak_x4t( s_x3_inst, s_x3_src, s_x3_dst,
                                           T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s12,
    biquad_cascade_df2T_f32_dspic33ak_x1s2p( &s_inst, s_in, s_out,
                                             T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s13,
    biquad_cascade_df2T_f32_dspic33ak_x1s3p( &s_inst, s_in, s_out,
                                             T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s14,
    biquad_cascade_df2T_f32_dspic33ak_x1s3p_readyN( &s_inst, s_in, s_out, T1_FRAMES ) )
T1_DEFINE_ARM_RUN( s15,
    biquad_cascade_df2T_f32_dspic33ak_x1s3p_ready32( &s_inst, s_in, s_out, T1_FRAMES ) )
#else
T1_DEFINE_ARM_RUN( s3, T1_SWEEP_K3( &s_inst, s_in, s_out, T1_FRAMES ) )
#endif

/*
 * Arm selection, OUTSIDE any timed region.
 *
 * This switch runs once per measurement, before the timer is touched. It also
 * sets s_arm so that the correctness paths (which go through t1_process()) run
 * the same kernel this measurement did.
 */
static bool t1_run_arm( t1_arm_t arm,
                        const t1_bank_t *bank,
                        uint32_t sections,
                        bool pmu,
                        t1_arm_result_t *r )
{
    s_arm = arm;

    switch( arm )
    {
    case T1_ARM_OPT_V1: return t1_run_opt_v1( bank, sections, pmu, r );
    case T1_ARM_S0:     return t1_run_s0( bank, sections, pmu, r );
    case T1_ARM_S1:     return t1_run_s1( bank, sections, pmu, r );
    case T1_ARM_S2:     return t1_run_s2( bank, sections, pmu, r );
    case T1_ARM_S3:     return t1_run_s3( bank, sections, pmu, r );
#if T1_TWO_CHANNEL_BUILD
    case T1_ARM_S4:     return t1_run_s4( bank, sections, pmu, r );
    case T1_ARM_S5:     return t1_run_s5( bank, sections, pmu, r );
    case T1_ARM_S6:     return t1_run_s6( bank, sections, pmu, r );
    case T1_ARM_S7:     return t1_run_s7( bank, sections, pmu, r );
    case T1_ARM_S8:     return t1_run_s8( bank, sections, pmu, r );
    case T1_ARM_S9:     return t1_run_s9( bank, sections, pmu, r );
    case T1_ARM_S10:    return t1_run_s10( bank, sections, pmu, r );
    case T1_ARM_S11:    return t1_run_s11( bank, sections, pmu, r );
    case T1_ARM_S12:    return t1_run_s12( bank, sections, pmu, r );
    case T1_ARM_S13:    return t1_run_s13( bank, sections, pmu, r );
    case T1_ARM_S14:    return t1_run_s14( bank, sections, pmu, r );
    case T1_ARM_S15:    return t1_run_s15( bank, sections, pmu, r );
#endif
    default:            return false;
    }
}
#endif /* T1_SWEEP_BUILD */

/*
 * N_max: sections per channel that fit one block period, kernel only, at the
 * protocol's Tier 2 channel count.
 *
 *   N_max = block_counts * sections / (T1_NMAX_CHANNELS * cyc_min)
 *
 * derived by substituting C = cyc_min / (frames * sections) into
 * block_counts / (channels * frames * C) and cancelling, so there is no
 * intermediate rounding. Counts, not cycles: the ratio is dimensionless as long
 * as both sides come from the same timer.
 */
static uint32_t t1_n_max( const t1_result_t *r, uint32_t block_counts )
{
    if( !r->valid || ( r->cyc_min == 0u ) )
    {
        return 0u;
    }
    return (uint32_t)( ( (uint64_t)block_counts * r->sections )
                       / ( (uint64_t)T1_NMAX_CHANNELS * r->cyc_min ) );
}

/* One 32-frame block at 48 kHz, in high-res-timer counts. Derived from the
 * timer's own us conversion so it cannot disagree with the measurements. */
static uint32_t t1_block_counts( void )
{
    /* 666.67 us = 6667 tenths of a microsecond. Search-free inversion: the
     * timer's counts-to-us_x10 is linear, so scale a large reference count. */
    const uint32_t ref_counts = 100000u;
    const uint32_t ref_us10   = nora_high_res_timer_count_to_us_x10( ref_counts );

    if( ref_us10 == 0u )
    {
        return 0u;
    }
    return scaled_div( ref_counts, 6667u, ref_us10 );
}

static void t1_print( const t1_result_t *r, const t1_bank_t *bank )
{
    printf( "[%s] 1 ch x %u frames x %lu sections, %u iterations, bank %s\r\n",
            T1_KERNEL_PAIRING,
            (unsigned)T1_FRAMES, (unsigned long)r->sections,
            (unsigned)T1_ITERATIONS, bank->name );

    if( !r->valid )
    {
        printf( "  refused - geometry does not fit the bank, or the timer"
                " returned a zero interval\r\n" );
        return;
    }

    const uint32_t samples = T1_FRAMES;
    const uint32_t ss      = samples * r->sections;
    const uint32_t block   = t1_block_counts();

    printf( "  ticks/block    : min %lu, mean %lu, max %lu"
            " (1 tick = %lu instruction cycles)\r\n",
            (unsigned long)r->cyc_min, (unsigned long)r->cyc_mean,
            (unsigned long)r->cyc_max,
            (unsigned long)T1_CYCLES_PER_TICK );

    printf( "  cycles/block   : min %lu\r\n",
            (unsigned long)ticks_to_cycles( r->cyc_min ) );

    const uint32_t per_ss =
        scaled_div( ticks_to_cycles( r->cyc_min ), 1000u, ss );

    printf( "  cyc/sample/sec : min %lu.%03lu  <- the figure the protocol"
            " compares\r\n",
            (unsigned long)( per_ss / 1000u ),
            (unsigned long)( per_ss % 1000u ) );

    const uint32_t us10 = nora_high_res_timer_count_to_us_x10( r->cyc_min );

    printf( "  time/block     : min %lu.%lu us\r\n",
            (unsigned long)( us10 / 10u ), (unsigned long)( us10 % 10u ) );

    if( block != 0u )
    {
        const uint32_t share = scaled_div( r->cyc_min, 100000u, block );

        printf( "  share of block : min %lu.%03lu %% (1 ch)\r\n",
                (unsigned long)( share / 1000u ),
                (unsigned long)( share % 1000u ) );
        printf( "  N_max (%u ch)   : %lu sections per channel fit one block"
                " (kernel only)\r\n",
                (unsigned)T1_NMAX_CHANNELS,
                (unsigned long)t1_n_max( r, block ) );
    }
}

void biquad_tier1_bench_run( uint32_t sections, biquad_tier1_bank_t which )
{
    const t1_bank_t *const bank = bank_of( which );
    t1_result_t r;

    if( !t1_ensure_ready( "run" ) )
    {
        return;
    }

    (void)t1_measure( bank, sections, &r );
    t1_print( &r, bank );
}

void biquad_tier1_bench_sweep( void )
{
    if( !t1_ensure_ready( "sweep" ) )
    {
        return;
    }

    /*
     * The sweep runs on the allpass bank throughout, including at the short
     * points. The 6-section Butterworth bank cannot reach the top of the sweep
     * at all, and mixing two filter families inside one sweep would put a
     * coefficient change inside a length comparison.
     */
    const uint32_t block = t1_block_counts();

    printf( "[%s] section sweep, 1 ch x %u frames, bank %s (%lu warm-up blocks)\r\n",
            T1_KERNEL_PAIRING,
            (unsigned)T1_FRAMES, s_bank_ap.name,
            (unsigned long)s_bank_ap.warmup_blocks );
    printf( "  N_max = sections per channel that fit one block at %u ch,"
            " kernel only\r\n", (unsigned)T1_NMAX_CHANNELS );

    for( uint32_t i = 0u;
         i < ( sizeof(s_sweep_points) / sizeof(s_sweep_points[0]) );
         i++ )
    {
        t1_result_t r;

        if( !t1_measure( &s_bank_ap, s_sweep_points[i], &r ) || !r.valid )
        {
            printf( "  %3u sec : (refused)\r\n", (unsigned)s_sweep_points[i] );
            continue;
        }

        const uint32_t per_ss =
            scaled_div( ticks_to_cycles( r.cyc_min ), 1000u,
                        T1_FRAMES * r.sections );

        printf( "  %3lu sec : min %7lu ticks  %lu.%03lu cyc/sample/sec"
                "  N_max %3lu\r\n",
                (unsigned long)r.sections, (unsigned long)r.cyc_min,
                (unsigned long)( per_ss / 1000u ),
                (unsigned long)( per_ss % 1000u ),
                (unsigned long)t1_n_max( &r, block ) );
    }
}

#if T1_SWEEP_BUILD
/* ------------------------------------------------------------------ */
/* Spacing sweep: the experiment                                       */
/* ------------------------------------------------------------------ */
/*
 * Every sweep arm must be bit-identical to opt_v1.
 *
 * The arms differ from opt_v3 only by spacer instructions that touch no
 * register, and opt_v3 is bit-identical to opt_v1 by construction (same five
 * operations, same order, renamed registers). So bit equality - not a tolerance
 * - is the right bar, and a single differing bit means the macro emitted
 * something that perturbs state, which would invalidate every cycle figure
 * taken from that arm.
 *
 * Checked at n = 32, the geometry the sweep measures, on the ap84 bank. The
 * arms are even-only and Tier 1 never asks for an odd length, so the odd-route
 * question opt_v3's wrapper raised does not arise here.
 */
static float32_t s_gap_ref_out[ T1_FRAMES ];

static bool t1_gap_equivalence( const t1_bank_t *bank )
{
    bool all_ok = true;

    printf( "%s arms vs opt_v1 [%s]: bit-exact, %lu sections, n=%u\r\n",
            T1_SWEEP_LABEL,
            bank->name, (unsigned long)bank->sections, (unsigned)T1_FRAMES );

    /* Reference: opt_v1 from zero state. */
    if( !t1_configure( bank, bank->sections, BIQUAD_TIER1_FAULT_NONE ) )
    {
        printf( "  could not configure the reference - FAIL\r\n" );
        return false;
    }
    t1_load_input( 0u );
    memset( s_gap_ref_out, 0x00, sizeof(s_gap_ref_out) );
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst, s_in, s_gap_ref_out,
                                              T1_FRAMES );
    memcpy( s_state_final_ch0, s_state,
            bank->sections * 2u * sizeof(float32_t) );

#if T1_TWO_CHANNEL_BUILD
    /*
     * Channel B's reference, from opt_v1 on channel B's OWN input. A two-channel
     * kernel has a failure mode a single-channel gate cannot see: it can filter
     * channel A correctly and write channel A's result, or garbage, to channel
     * B's output. So B needs its own reference and its own comparison.
     */
    if( !t1_configure_b( bank, bank->sections ) )
    {
        printf( "  could not configure channel B's reference - FAIL\r\n" );
        return false;
    }
    t1_load_input_b( 1u );
    memset( s_ref_out_b, 0x00, sizeof(s_ref_out_b) );
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst_b, s_in_b, s_ref_out_b,
                                              T1_FRAMES );

    /* Channel C's reference, for x3. Same argument as channel B's: a
     * three-channel kernel can filter A correctly and still write the wrong
     * thing to C. */
    if( !t1_configure_c( bank, bank->sections ) )
    {
        printf( "  could not configure channel C's reference - FAIL\r\n" );
        return false;
    }
    t1_load_input_c( 2u );
    memset( s_ref_out_c, 0x00, sizeof(s_ref_out_c) );
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst_c, s_in_c, s_ref_out_c,
                                              T1_FRAMES );

    /* Channel D's reference, for x4t. Same argument as B's and C's: a
     * four-channel kernel can filter A, B and C correctly and still write the
     * wrong thing to D. */
    if( !t1_configure_d( bank, bank->sections ) )
    {
        printf( "  could not configure channel D's reference - FAIL\r\n" );
        return false;
    }
    t1_load_input_d( 3u );
    memset( s_ref_out_d, 0x00, sizeof(s_ref_out_d) );
    biquad_cascade_df2T_f32_dspic33ak_opt_v1( &s_inst_d, s_in_d, s_ref_out_d,
                                              T1_FRAMES );
#endif

    for( uint32_t a = (uint32_t)T1_ARM_S0; a < (uint32_t)T1_ARM_COUNT; a++ )
    {
        const t1_arm_t arm = (t1_arm_t)a;
        uint32_t mismatches = 0u;
        uint32_t first_bad  = 0u;
        uint32_t state_mismatches = 0u;
        uint32_t first_bad_state  = 0u;
        /* Per channel, so a multi-channel arm's failure names the channel rather
         * than only the total. "96/128 differ" left three candidate defects
         * indistinguishable; this is the cheapest instrument that separates them. */
        uint32_t bad_ch[4] = { 0u, 0u, 0u, 0u };

        if( !t1_configure( bank, bank->sections, BIQUAD_TIER1_FAULT_NONE ) )
        {
            printf( "  %-14s : could not configure - FAIL\r\n",
                    s_arm_name[a] );
            all_ok = false;
            continue;
        }

        t1_load_input( 0u );
        memset( s_out, 0x00, sizeof(s_out) );

#if T1_TWO_CHANNEL_BUILD
        /* Channel B back to zero state with its own input, so that an arm which
         * writes B's buffer is compared against B's reference from the same
         * starting point the reference was taken from. */
        if( !t1_configure_b( bank, bank->sections ) )
        {
            printf( "  %-14s : could not configure channel B - FAIL\r\n",
                    s_arm_name[a] );
            all_ok = false;
            continue;
        }
        t1_load_input_b( 1u );
        memset( s_out_b, 0x00, sizeof(s_out_b) );

        if( !t1_configure_c( bank, bank->sections ) )
        {
            printf( "  %-14s : could not configure channel C - FAIL\r\n",
                    s_arm_name[a] );
            all_ok = false;
            continue;
        }
        t1_load_input_c( 2u );
        memset( s_out_c, 0x00, sizeof(s_out_c) );

        if( !t1_configure_d( bank, bank->sections ) )
        {
            printf( "  %-14s : could not configure channel D - FAIL\r\n",
                    s_arm_name[a] );
            all_ok = false;
            continue;
        }
        t1_load_input_d( 3u );
        memset( s_out_d, 0x00, sizeof(s_out_d) );
#endif

        /* Through t1_process()'s switch, not a timed path - correctness only. */
        s_arm = arm;
        t1_process();

        for( uint32_t n = 0u; n < T1_FRAMES; n++ )
        {
            uint32_t x, y;

            memcpy( &x, &s_gap_ref_out[n], sizeof(x) );
            memcpy( &y, &s_out[n],         sizeof(y) );

            if( x != y )
            {
                if( mismatches == 0u )
                {
                    first_bad = n;
                }
                mismatches++;
                bad_ch[0]++;
            }
        }

        /* A candidate that writes the right block but leaves a rotated or stale
         * DF2T delay pair must not reach the PMU table.  Compare every state word
         * bit-for-bit against opt_v1; the snapshot is outside the timed memory
         * placement, so this gate cannot perturb the measurement it protects. */
        for( uint32_t q = 0u; q < ( bank->sections * 2u ); q++ )
        {
            uint32_t x, y;

            memcpy( &x, &s_state_final_ch0[q], sizeof(x) );
            memcpy( &y, &s_state[q],           sizeof(y) );
            if( x != y )
            {
                if( state_mismatches == 0u )
                {
                    first_bad_state = q;
                }
                state_mismatches++;
            }
        }

#if T1_TWO_CHANNEL_BUILD
        /*
         * Channel B, for the arm that actually writes it. Counted into the SAME
         * mismatch total, so the verdict line cannot pass while B is wrong.
         */
        if( ( arm == T1_ARM_S3 ) || ( arm == T1_ARM_S4 )
         || ( arm == T1_ARM_S5 ) || ( arm == T1_ARM_S6 )
         || ( arm == T1_ARM_S7 ) || ( arm == T1_ARM_S8 )
         || ( arm == T1_ARM_S9 ) || ( arm == T1_ARM_S10 )
         || ( arm == T1_ARM_S11 ) )
        {
            for( uint32_t n = 0u; n < T1_FRAMES; n++ )
            {
                uint32_t x, y;

                memcpy( &x, &s_ref_out_b[n], sizeof(x) );
                memcpy( &y, &s_out_b[n],     sizeof(y) );

                if( x != y )
                {
                    if( mismatches == 0u )
                    {
                        first_bad = n;
                    }
                    mismatches++;
                    bad_ch[1]++;
                }
            }
        }

        /* Channel C, for the three- and four-channel arms. Counted into the SAME
         * total, so the verdict line cannot pass while C is wrong. */
        if( ( arm == T1_ARM_S6 ) || ( arm == T1_ARM_S7 )
         || ( arm == T1_ARM_S8 ) || ( arm == T1_ARM_S9 )
         || ( arm == T1_ARM_S10 ) || ( arm == T1_ARM_S11 ) )
        {
            for( uint32_t n = 0u; n < T1_FRAMES; n++ )
            {
                uint32_t x, y;

                memcpy( &x, &s_ref_out_c[n], sizeof(x) );
                memcpy( &y, &s_out_c[n],     sizeof(y) );

                if( x != y )
                {
                    if( mismatches == 0u )
                    {
                        first_bad = n;
                    }
                    mismatches++;
                    bad_ch[2]++;
                }
            }
        }

        /* Channel D, for the four-channel arm only. Counted into the SAME total:
         * x4t's whole claim is that the fourth channel is as correct as the other
         * three, and a gate that stopped at C could not see it fail. */
        if( arm == T1_ARM_S11 )
        {
            for( uint32_t n = 0u; n < T1_FRAMES; n++ )
            {
                uint32_t x, y;

                memcpy( &x, &s_ref_out_d[n], sizeof(x) );
                memcpy( &y, &s_out_d[n],     sizeof(y) );

                if( x != y )
                {
                    if( mismatches == 0u )
                    {
                        first_bad = n;
                    }
                    mismatches++;
                    bad_ch[3]++;
                }
            }
        }
#endif

        /* The denominator is what was actually compared: T1_FRAMES for a
         * single-channel arm, twice that for a two-channel one. Printing
         * T1_FRAMES for x2 would understate the gate by half. */
        const unsigned long compared = (unsigned long)t1_samples_per_call( arm );

        printf( "  %-14s : %lu/%lu samples, %lu/%lu state words differ%s -> %s\r\n",
                s_arm_name[a],
                (unsigned long)mismatches, compared,
                (unsigned long)state_mismatches,
                (unsigned long)( bank->sections * 2u ),
                ( ( mismatches != 0u ) || ( state_mismatches != 0u ) )
                    ? "" : ", bit-identical",
                ( ( mismatches == 0u ) && ( state_mismatches == 0u ) )
                    ? "PASS" : "FAIL" );

        if( mismatches != 0u )
        {
            printf( "                   first difference at sample %lu\r\n",
                    (unsigned long)first_bad );
            printf( "                   per channel A/B/C/D: %lu/%lu/%lu/%lu\r\n",
                    (unsigned long)bad_ch[0], (unsigned long)bad_ch[1],
                    (unsigned long)bad_ch[2], (unsigned long)bad_ch[3] );
#if T1_TWO_CHANNEL_BUILD
            /*
             * Sample 0 of every channel, reference beside produced, as raw bit
             * patterns. A ZERO on the produced side means the kernel never wrote
             * that buffer; a WRONG NON-ZERO means it wrote the wrong value. Those
             * are different defects and a mismatch count cannot separate them.
             */
            {
                uint32_t rb[4], gb[4];

                memcpy( &rb[0], &s_gap_ref_out[0], sizeof(rb[0]) );
                memcpy( &gb[0], &s_out[0],         sizeof(gb[0]) );
                memcpy( &rb[1], &s_ref_out_b[0],   sizeof(rb[1]) );
                memcpy( &gb[1], &s_out_b[0],       sizeof(gb[1]) );
                memcpy( &rb[2], &s_ref_out_c[0],   sizeof(rb[2]) );
                memcpy( &gb[2], &s_out_c[0],       sizeof(gb[2]) );
                memcpy( &rb[3], &s_ref_out_d[0],   sizeof(rb[3]) );
                memcpy( &gb[3], &s_out_d[0],       sizeof(gb[3]) );

                for( uint32_t k = 0u; k < 4u; k++ )
                {
                    printf( "                   ch%c s0 ref=%08lX got=%08lX\r\n",
                            (char)( 'A' + (int)k ),
                            (unsigned long)rb[k], (unsigned long)gb[k] );
                }
            }
#endif
        }

        if( state_mismatches != 0u )
        {
            printf( "                   first state-word difference at %lu\r\n",
                    (unsigned long)first_bad_state );
        }

        if( ( mismatches != 0u ) || ( state_mismatches != 0u ) )
        {
            all_ok = false;
        }
    }

    printf( "  verdict        : %s\r\n", all_ok ? "PASS" : "FAIL" );
    s_arm = T1_ARM_OPT_V1;
    return all_ok;
}

/*
 * Per-arm result line.
 *
 * `instr_expected` is the disassembly's instructions-per-pair; the measured
 * value comes from PMU event 2 divided by the same geometry. Printing both is
 * how a spacer that did not retire, or a stall that is really a fetch penalty,
 * becomes visible instead of being absorbed into the cycle number.
 */
static void t1_gap_print_row( t1_arm_t arm, const t1_arm_result_t *r )
{
    if( !r->valid )
    {
        printf( "  %-14s : (refused)\r\n", s_arm_name[arm] );
        return;
    }

    /*
     * SAMPLES PER CALL, WHICH IS NOT THE SAME FOR EVERY ARM.
     *
     * Every arm but one filters T1_FRAMES samples per call. x2 filters
     * T1_FRAMES on EACH of two channels, so its call does twice the work and its
     * cycles must be divided by twice as much to be the same quantity as
     * opt_v1's cycles/sample/section. Getting this wrong would make x2 look
     * exactly 2x better than it is, which is the single most likely way to
     * report a false win here - so the divisor is derived from the arm, not
     * assumed.
     */
    const uint32_t ss     = t1_samples_per_call( arm ) * r->sections;
    const uint32_t per_ss = scaled_div( ticks_to_cycles( r->cyc_min ), 1000u, ss );

    printf( "  %-14s : min %7lu ticks  %lu.%03lu cyc/sample/sec",
            s_arm_name[arm],
            (unsigned long)r->cyc_min,
            (unsigned long)( per_ss / 1000u ),
            (unsigned long)( per_ss % 1000u ) );

    if( !r->pmu_valid )
    {
        printf( "  (no PMU)\r\n" );
        return;
    }

    /*
     * Per sample-section, over the whole measured run: the counters accumulated
     * across T1_ITERATIONS blocks, so that is the divisor.
     *
     * x1000 fixed point, computed in 64-bit. The cycle count here is the PMU's
     * own (event 1), NOT the timer's - printing both is a cross-check between
     * two independent instruments, and if they disagree the sweep is not
     * trustworthy whatever the stall columns say.
     */
    /* Same divisor as the cycle column above, for the same reason. */
    const uint64_t total_ss = (uint64_t)ss * (uint64_t)T1_ITERATIONS;

    const uint64_t cyc   = ( r->ev[0] * 1000ull ) / total_ss;
    const uint64_t instr = ( r->ev[1] * 1000ull ) / total_ss;
    const uint64_t e8    = ( r->ev[2] * 1000ull ) / total_ss;
    const uint64_t e9    = ( r->ev[3] * 1000ull ) / total_ss;
    const uint64_t e3    = ( r->ev[4] * 1000ull ) / total_ss;
    const uint64_t e7    = ( r->ev[5] * 1000ull ) / total_ss;
    const uint64_t e13   = ( r->ev[6] * 1000ull ) / total_ss;
    const uint64_t e6    = ( r->ev[7] * 1000ull ) / total_ss;

    const uint64_t cpi = ( r->ev[1] != 0u )
                            ? ( r->ev[0] * 1000ull ) / r->ev[1]
                            : 0u;

    printf( "\r\n" );
    printf( "                   PMU/sample/sec: cyc %lu.%03lu  instr %lu.%03lu"
            "  CPI %lu.%03lu  (pair instr: expected %u)\r\n",
            (unsigned long)( cyc / 1000u ),   (unsigned long)( cyc % 1000u ),
            (unsigned long)( instr / 1000u ), (unsigned long)( instr % 1000u ),
            (unsigned long)( cpi / 1000u ),   (unsigned long)( cpi % 1000u ),
            (unsigned)s_arm_instr_per_pair[arm] );
    printf( "                   stalls/sample/sec: ev8 FPUdep %lu.%03lu"
            "  ev9 FPUrd %lu.%03lu  ev3 FPUwr %lu.%03lu\r\n",
            (unsigned long)( e8 / 1000u ), (unsigned long)( e8 % 1000u ),
            (unsigned long)( e9 / 1000u ), (unsigned long)( e9 % 1000u ),
            (unsigned long)( e3 / 1000u ), (unsigned long)( e3 % 1000u ) );
    printf( "                                     ev7 hazard %lu.%03lu"
            "  ev13 fetch %lu.%03lu  ev6 mispred %lu.%03lu\r\n",
            (unsigned long)( e7 / 1000u ),  (unsigned long)( e7 % 1000u ),
            (unsigned long)( e13 / 1000u ), (unsigned long)( e13 % 1000u ),
            (unsigned long)( e6 / 1000u ),  (unsigned long)( e6 % 1000u ) );
}

void biquad_tier1_bench_gap_sweep( uint32_t sections, bool pmu )
{
    if( !t1_ensure_ready( "gap sweep" ) )
    {
        return;
    }

    if( ( sections == 0u ) || ( sections > T1_MAX_SECTIONS ) )
    {
        printf( "gap sweep: refused - sections must be 1..%u\r\n",
                (unsigned)T1_MAX_SECTIONS );
        return;
    }

    if( pmu && !perf_monitor_configure( s_pmu_events, PERF_MONITOR_COUNTERS ) )
    {
        /*
         * Refused rather than falling back to a timer-only run. A silent
         * downgrade would produce a table that looks like the experiment and is
         * missing the half of it that matters.
         */
        printf( "gap sweep: refused - PMU would not configure (SFR readback"
                " mismatch)\r\n" );
        return;
    }

    printf( "%s, ap84 bank, 1 ch x %u frames x %lu sections,"
            " %u iterations, PMU %s\r\n",
            T1_SWEEP_LABEL,
            (unsigned)T1_FRAMES, (unsigned long)sections,
            (unsigned)T1_ITERATIONS, pmu ? "ON" : "OFF" );
    printf( "  edge under test : %s\r\n", T1_SWEEP_EDGE );
    printf( "  order v1 -> %s..%s -> v1; the closing v1 is the drift"
            " check, not a repeat\r\n",
            T1_SWEEP_NAME_0, T1_SWEEP_NAME_3 );

    uint32_t v1_first = 0u;
    uint32_t v1_last  = 0u;

    for( uint32_t i = 0u;
         i < ( sizeof(s_arm_order) / sizeof(s_arm_order[0]) );
         i++ )
    {
        const t1_arm_t  arm = s_arm_order[i];
        t1_arm_result_t r;

        if( !t1_run_arm( arm, &s_bank_ap, sections, pmu, &r ) )
        {
            printf( "  %-14s : (refused)\r\n", s_arm_name[arm] );
            continue;
        }

        t1_gap_print_row( arm, &r );

        if( arm == T1_ARM_OPT_V1 )
        {
            if( v1_first == 0u )
            {
                v1_first = r.cyc_min;
            }
            else
            {
                v1_last = r.cyc_min;
            }
        }
    }

    /*
     * The drift verdict.
     *
     * Stated as a verdict rather than left for the reader to eyeball, because a
     * sweep whose two opt_v1 readings disagree does not have a spacing result in
     * it at all - the gap numbers in between were taken under conditions that
     * moved. 1 % of the min tick count is the threshold: this bench's own
     * min/max spread on a quiet board is about 1.4 % (15435..15647 measured
     * 2026-09-22), so anything inside 1 % of the MINIMUM is within the noise the
     * instrument already shows.
     */
    if( ( v1_first != 0u ) && ( v1_last != 0u ) )
    {
        const uint32_t hi   = ( v1_last > v1_first ) ? v1_last : v1_first;
        const uint32_t lo   = ( v1_last > v1_first ) ? v1_first : v1_last;
        const uint32_t d    = hi - lo;
        const uint32_t pct  = scaled_div( d, 1000u, lo );
        const bool     ok   = ( ( d * 100u ) <= lo );

        printf( "  drift check    : opt_v1 first %lu ticks, last %lu ticks,"
                " delta %lu (%lu.%03lu %%) -> %s\r\n",
                (unsigned long)v1_first, (unsigned long)v1_last,
                (unsigned long)d,
                (unsigned long)( pct / 1000u ), (unsigned long)( pct % 1000u ),
                ok ? "STABLE" : "DRIFTED - sweep not comparable" );
    }
    else
    {
        printf( "  drift check    : n/a - opt_v1 did not measure twice\r\n" );
    }

    s_arm = T1_ARM_OPT_V1;
}

bool biquad_tier1_bench_gap_verify( void )
{
    if( !t1_ensure_ready( "gap verify" ) )
    {
        return false;
    }

    /* ap84 is the bank the sweep measures; lp6 adds nothing here because the
     * arms differ from opt_v1 in scheduling, not in section count handling. */
    return t1_gap_equivalence( &s_bank_ap );
}
#endif /* T1_SWEEP_BUILD */

#endif /* ENA_BIQUAD_TIER1_BENCH */
