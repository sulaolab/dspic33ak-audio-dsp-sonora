/*
 * perf_monitor_dspic33a.h - dsPIC33A Performance Monitor Unit (PMU), the
 * cycles-per-instruction instrument.
 *
 * WHY THIS EXISTS
 * ---------------
 * A cycle count says a loop is slow. It does not say WHY, and on this core the
 * difference between "issue-bound" and "stalled on an FPU register dependency"
 * changes what the next kernel should look like - in opposite directions. The
 * opt_v3 experiment (2026-09-22) removed 15 % of the instructions from the DF2T
 * inner loop and got 21.8 % MORE cycles; the leading explanation was a
 * cross-sample FPU RAW dependency, but no counter had been read, so it stayed a
 * hypothesis. This module is what turns that class of question into a
 * measurement.
 *
 * The PMU is eight independent 64-bit counters, each selecting one event from
 * DS70005591D Table 3-10. Counter 0 is conventionally the CPU-cycle reference
 * (event 1), counter 1 the completed-instruction count (event 2), and
 * cycles / instructions is then the CPI the datasheet's section 3.5 describes.
 * The remaining six classify where the extra cycles went.
 *
 * WHAT IT IS NOT
 * --------------
 * Not an audio-path facility, not ISR-safe by design, and compiled out entirely
 * unless ENA_PERF_MONITOR is 1. The counters are global CPU state with no
 * ownership protocol: two callers bracketing overlapping regions would each
 * read the other's events. One caller at a time, in the foreground, is the
 * contract.
 *
 * COUNTING IS NOT FREE OF ITS OWN INSTRUMENT
 * ------------------------------------------
 * Starting and stopping the module is an SFR write on each side of the measured
 * region, so a region that is itself a handful of cycles measures mostly the
 * bracket. The Tier 1 bench regions are ~15,000 ticks, where that is noise --
 * but "PMU on does not move the cycle figure" is verified on hardware rather
 * than assumed, because the whole point is to trust these numbers.
 *
 * WHY THE COUNTERS ARE READ AS 32-BIT HALVES
 * ------------------------------------------
 * Each counter is a pair of read-only SFRs (HPCCNTLx low, HPCCNTHx high). The
 * datasheet requires reads to be taken with ON = 0, which this API enforces by
 * only exposing a read after a stop -- so there is no torn-read window to worry
 * about and no need for a high-low-high re-read dance.
 *
 * NO SATURATION HANDLING IN HARDWARE. DS70005591D section 3.5.3.3: the counters
 * have no saturation or rollover event and it is software's job to stop the
 * module before one occurs. At 200 MHz a 64-bit cycle counter takes about 2,900
 * years to wrap, so the practical risk is zero; it is noted because the
 * datasheet notes it, not because a guard is needed.
 */
#ifndef PERF_MONITOR_DSPIC33A_H
#define PERF_MONITOR_DSPIC33A_H

#include <stdbool.h>
#include <stdint.h>

#include "app_specific_config_defs.h"

#if defined(ENA_PERF_MONITOR) && ENA_PERF_MONITOR

/*
 * Event sources, DS70005591D Table 3-10, SELECTn[4:0]. Only the events this
 * project has a use for are named; the numbering is the hardware's, so an
 * unnamed event can be passed as a plain integer without changing this header.
 *
 * The comments are the datasheet's own wording compressed, because the
 * distinction between events 8, 9 and 3 is exactly what a reader of a stall
 * table needs and getting it backwards inverts the conclusion:
 *
 *   8  FPU INSTRUCTION stall - the FPU pipeline itself is stalled on a register
 *      data dependency. This is the "mac.s result is not ready for the next
 *      mac.s" event, i.e. the one the opt_v3 hypothesis is about.
 *   9  FPU READ stall - the CPU cannot READ an F-register because the FPU is
 *      still updating it. This is the CPU side of the same coin: it fires when
 *      a CPU instruction (e.g. mov.l F5,[W2++], storing y) needs a value the
 *      FPU has not finished writing.
 *   3  FPU WRITE stall - the CPU cannot WRITE an F-register because the FPU is
 *      busy with the existing contents. The direction is opposite to 9.
 *
 * So a DF2T loop that stores y right after computing it stresses 9, and one
 * whose next sample's mac.s consumes the previous sample's accumulator stresses
 * 8. Reading only one of them would attribute the stall to whichever was
 * measured.
 */
typedef enum
{
    PERF_EVENT_NONE                 = 0u,
    PERF_EVENT_CPU_CYCLES           = 1u,   /* reference                       */
    PERF_EVENT_INSTR_COMPLETED      = 2u,   /* CPI numerator partner           */
    PERF_EVENT_FPU_WRITE_STALL      = 3u,   /* CPU cannot write an F-reg       */
    PERF_EVENT_WRITE_STAGE_STALL    = 4u,   /* RAM/SFR write latency           */
    PERF_EVENT_COND_BRANCH          = 5u,   /* conditional branches executed   */
    PERF_EVENT_BRANCH_MISPREDICT    = 6u,   /* mispredicted flow changes       */
    PERF_EVENT_ADDR_HAZARD          = 7u,   /* CPU data dependency, no forward */
    PERF_EVENT_FPU_INSTR_STALL      = 8u,   /* FPU stalled on reg dependency   */
    PERF_EVENT_FPU_READ_STALL       = 9u,   /* CPU cannot read an F-reg        */
    PERF_EVENT_ADDR_READ_STALL      = 10u,  /* RAM/SFR read latency            */
    PERF_EVENT_ADDR_STAGE_STALL     = 11u,  /* address-stage stall, any reason  */
    PERF_EVENT_IRQ_LATENCY          = 12u,  /* cycles of interrupt latency     */
    PERF_EVENT_FETCH_READ_STALL     = 13u,  /* program fetch extra cycle       */
    PERF_EVENT_PROGRAM_FLOW_CHANGE  = 14u,  /* CALL/RETURN/branch/interrupt    */
    PERF_EVENT_VECTOR_FETCH         = 15u,  /* interrupt vector fetches        */
    PERF_EVENT_CACHE_BUSY           = 16u,  /* ISB -> cache transfer cycle     */
    PERF_EVENT_FETCH_PBU_HIT        = 17u,  /* fetch served by cache or ISB    */
    PERF_EVENT_FETCH_PBU_MISS       = 18u,  /* fetch needed program memory     */
} perf_event_t;

#define PERF_MONITOR_COUNTERS   (8u)

/*
 * Programs all eight counters and leaves the module STOPPED and CLEARED.
 *
 * `events` is read in counter order: events[0] goes to counter 0. Fewer than
 * eight entries is allowed; the remainder are set to PERF_EVENT_NONE so they
 * cannot accumulate something nobody selected.
 *
 * Returns false if `count` exceeds the hardware's eight counters, rather than
 * silently programming the first eight of a longer list.
 */
bool perf_monitor_configure( const perf_event_t *events, uint32_t count );

/* Clears every counter and starts counting. Cheap: two SFR writes. */
void perf_monitor_start( void );

/* Stops counting. The datasheet requires ON = 0 before a counter is read, so
 * every read path in this API goes through here first. */
void perf_monitor_stop( void );

/*
 * One counter's 64-bit value. Valid only while the module is stopped; returns 0
 * for an out-of-range index rather than reading a neighbouring SFR.
 */
uint64_t perf_monitor_read( uint32_t counter );

/*
 * Reads every configured counter into `out` in one pass, which is what a caller
 * comparing events wants: eight separate calls would be eight more chances for
 * the caller to forget the stop.
 */
void perf_monitor_read_all( uint64_t *out, uint32_t count );

/*
 * Prints what each counter is selecting and what it counted, plus the derived
 * CPI when counters 0 and 1 hold the cycle and instruction references.
 *
 * `label` names the measured region, `norm_divisor` scales each count to a
 * per-unit figure (e.g. samples x sections) - 0 suppresses the normalised
 * column rather than dividing by zero.
 */
void perf_monitor_print( const char *label, uint32_t norm_divisor );

/* The event a counter is currently selecting, for a caller building its own
 * table. PERF_EVENT_NONE for an out-of-range index. */
perf_event_t perf_monitor_event_of( uint32_t counter );

/* Short human name for an event, e.g. "FPU instr stall". Never NULL. */
const char *perf_monitor_event_name( perf_event_t ev );

#endif /* ENA_PERF_MONITOR */

#endif /* PERF_MONITOR_DSPIC33A_H */
