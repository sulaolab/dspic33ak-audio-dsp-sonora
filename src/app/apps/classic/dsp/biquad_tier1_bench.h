/*
 * biquad_tier1_bench.h - foreground isolated DF2T bench (protocol Tier 1).
 *
 * WHY THIS EXISTS
 * ---------------
 * The dsPIC33AK DF2T number this project has quoted for a long time -
 * ~154.4 us for 84 sections, ~11.49 cycles/sample/section at 200 MHz - comes
 * from the console's `CMSIS-IIR` telemetry in the Classic/DRC build. That
 * number is real and it is the one the 84-section record was set with, but as a
 * benchmark it has three properties nobody chose:
 *
 *   1. it is ONE CHANNEL, not four. The timer sits inside the per-channel loop
 *      in biquad_cascade_4ch.c and `dt` is a plain assignment, so it retains
 *      the last channel only;
 *   2. it is a SINGLE INSTANTANEOUS SAMPLE - not a min, not a mean, not a
 *      peak-hold - so any interrupt landing inside that one window inflates it;
 *   3. it is measured INSIDE THE DMA0 ISR, in a build that is also streaming
 *      audio.
 *
 * A useful isolated result needs a foreground measurement with warm-up,
 * repeated iterations and min/mean/max. Comparing that to the historical
 * in-ISR sample would conflate the measurement method with kernel cost.
 *
 * This opt-in harness therefore measures a 1 channel x 32 frames x N sections
 * geometry over fixed coefficients and a fixed input block. The historical
 * in-ISR figure remains a cross-check under its own name rather than being
 * silently replaced.
 *
 * WHAT MAKES RESULTS REPEATABLE
 * -----------------------------
 * shared_ref/iir_bench_reference.h carries the coefficient banks, input block,
 * expected output and a CRC-32 per bank. This harness recomputes those CRCs at
 * run time and refuses to report a measurement if either disagrees: a result
 * based on different filters would not be meaningful.
 *
 * WHAT THIS IS NOT
 * ----------------
 * Not in the audio path. Not reachable from an ISR. Compiled out entirely
 * unless ENA_BIQUAD_TIER1_BENCH is 1, which no shipping configuration sets.
 * It touches no streaming state and no transport.
 *
 * It also measures the KERNEL ONLY: no gather, no scatter, no int32/float
 * conversion, no channel expansion. That is Tier 1 by definition, and it is a
 * FLOOR, not a budget - on this project the work outside the kernel has
 * repeatedly cost more than the kernel did. Pricing a stage means differencing
 * two images, not adding this number to a load figure.
 *
 * Kernel under test: biquad_cascade_df2T_f32_dspic33ak_opt_v1, the
 * project-owned assembly - the protocol's "Best-available implementation" cell
 * for this device, and the kernel the 84-section record was actually set with.
 * The Microchip vendor kernel is deliberately NOT called here; that is a
 * separate measurement and a separate protocol cell.
 */
#ifndef BIQUAD_TIER1_BENCH_H
#define BIQUAD_TIER1_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "app_specific_config_defs.h"

#if defined(ENA_BIQUAD_TIER1_BENCH) && ENA_BIQUAD_TIER1_BENCH

/*
 * Coefficient banks, both from the shared reference.
 *
 * LP is the 6-section Butterworth: the arithmetic check at the short operating
 * point. AP is the 84-section allpass bank, the only one long enough to reach
 * the record's cascade length - a 84-section low-pass decays the signal into
 * denormals and the timing then measures denormal handling instead of the
 * kernel. See the header comment in shared_ref/iir_bench_reference.h.
 */
typedef enum
{
    BIQUAD_TIER1_BANK_LP = 0,
    BIQUAD_TIER1_BANK_AP,
} biquad_tier1_bank_t;

/*
 * Deliberate fault injections prove that the known-answer test can fail when
 * its arithmetic or state assumptions are violated.
 */
typedef enum
{
    BIQUAD_TIER1_FAULT_NONE = 0,
    BIQUAD_TIER1_FAULT_COEFF,           /* perturb one feedback coefficient  */
    /* The observation block runs on channel 0's post-warm-up state instead of
     * its own. Redefined 2026-09-17: it used to skip the state reset before
     * warm-up, and the bank's mandatory warm-up then flushed the injected
     * difference below observability on ap84. See biquad_tier1_bench.c. */
    BIQUAD_TIER1_FAULT_STATE_CARRY,
    BIQUAD_TIER1_FAULT_SECTION_COUNT,   /* run one section short             */
    BIQUAD_TIER1_FAULT_COUNT,
} biquad_tier1_fault_t;

/*
 * Verifies both coefficient banks against their CRCs and reports what it
 * found. Everything else refuses to run until this has returned true, so a
 * mismatched vendored header cannot produce a number.
 */
bool biquad_tier1_bench_init( void );

/* Build facts that change the numbers, printed rather than assumed. */
void biquad_tier1_bench_print_build_info( void );

/*
 * Known-answer test over both banks, plus the fault injections that prove it
 * can fail. A tolerance that cannot fail is not a test, so the negative half is
 * part of the verdict this returns.
 */
bool biquad_tier1_bench_self_test( void );

/* One measurement: 1 channel x IIR_BENCH_FRAMES x `sections`. */
void biquad_tier1_bench_run( uint32_t sections, biquad_tier1_bank_t bank );

/*
 * Section sweep over representative points, on the allpass bank throughout.
 * A single operating point cannot separate the
 * per-sample cost from the fixed per-call cost; the difference between adjacent
 * points can.
 */
void biquad_tier1_bench_sweep( void );

/*
 * SPACING SWEEPS - only in a BIQUAD_TIER1_KERNEL=4 or =5 build.
 *
 * Why these are gated on the kernel arm rather than always declared: they are
 * not a measurement of the DF2T kernel, they are an experiment about the core.
 * A build that selected a single kernel has no arms to sweep, and a caller that
 * could ask for one anyway would be asking a question the image cannot answer.
 *
 * WHAT THE SWEEPS ARE FOR
 * opt_v3 cut 15 % of the inner-loop instructions and ran 21.8 % slower
 * (2026-09-22). The stated suspect was an FPU register dependency too tight to
 * hide mac.s result latency - argued from instruction distance, with no counter
 * read. A sweep turns that into a measurement: the same loop is built with 0, 1,
 * 2 and 3 independent issue slots at ONE named edge, opt_v1 runs in the same
 * image as the control, and the Performance Monitor classifies the stalls.
 *
 * TWO EDGES, TWO BUILDS, ONE ENTRY POINT
 *   =4  the CROSS-SAMPLE edge: sample A's state producers to sample B's
 *       consumers. MEASURED AND REFUTED (2026-09-22): event 8 stayed at 3.999 on
 *       all four arms, each slot cost a full cycle, and the arm with opt_v1's
 *       exact instruction count was still 34.8 % slower.
 *   =5  the IN-SAMPLE d1 edge: each sample's d1 partial to its d1 final, which
 *       sit one instruction apart with only a store between them. Open question.
 *
 * Both builds expose the same two functions below, because the experiment's
 * shape is identical and only the kernel file changes. Which edge an image
 * actually swept is printed by the sweep itself and by *cB, so a log can never
 * be mistaken for the other one.
 *
 * If event 8 falls with the gap, that edge is in the stall. If it does not, the
 * distance explanation for that edge is dead and the other counters say where to
 * look next. Either outcome is the deliverable.
 */
#if defined(BIQUAD_TIER1_KERNEL) \
 && ( ( BIQUAD_TIER1_KERNEL == 4 ) || ( BIQUAD_TIER1_KERNEL == 5 ) || ( BIQUAD_TIER1_KERNEL == 6 ) )
/*
 * Runs v1 -> arm0 -> arm1 -> arm2 -> arm3 -> v1 at `sections`, printing cycles
 * and, with `pmu` true, the eight-counter stall breakdown per arm.
 *
 * opt_v1 runs first AND last: the closing reading is a drift check over
 * temperature, clock and the measurement system itself, and the sweep declares
 * itself not comparable if the two disagree by more than 1 %.
 *
 * `pmu` false is not a convenience - it is the control that proves the counters
 * do not move the cycle figure. The timed region is byte-identical either way.
 */
void biquad_tier1_bench_gap_sweep( uint32_t sections, bool pmu );

/*
 * Every sweep arm against opt_v1, bit for bit, at the sweep's own geometry.
 *
 * The arms add only register-free spacers to a schedule that is already
 * bit-identical to opt_v1, so bit equality is the bar and any difference is a
 * defect in the kernel macro - not a rounding effect, and not something a
 * tolerance may be widened to accept. Run this before believing any cycle
 * number the sweep prints.
 */
bool biquad_tier1_bench_gap_verify( void );
#endif

#endif /* ENA_BIQUAD_TIER1_BENCH */

#endif /* BIQUAD_TIER1_BENCH_H */
