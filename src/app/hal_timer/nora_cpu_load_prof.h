#ifndef NORA_CPU_LOAD_PROF_H
#define NORA_CPU_LOAD_PROF_H

//===========================================================
// Fixed-time-window, owner-attributed CPU load profiler ("DSPload").
//
// WHAT THIS MEASURES, AND WHY IT REPLACED THE UNION PROFILER
// ----------------------------------------------------------
// The predecessor (nora_spi_i2s_tdm_tdmsum_*) measured the WALL TIME during which any TDM
// RX-block ISR was executing, over a window whose length was the shortest running leg's block
// deadline and whose phase was locked to that leg's block boundary. Two properties of that
// design made it unreadable once the legs stopped sharing one interrupt priority
// (APP_ASRC_RATE_MONOTONIC_ISR: the long-deadline leg is demoted below the short one):
//
//   1. Wall time is RESPONSE time, not CPU time. When a higher-priority vector preempts a leg,
//      the preemptor's whole execution is inside the leg's measured interval. Under rate-
//      monotonic priorities the low-priority leg absorbs the high-priority leg wholesale, so
//      the "sum" over-counted by exactly the amount the two legs overlapped -- the one thing a
//      union was supposed to prevent.
//   2. The window was tied to the block period, so it could not be varied to separate a
//      short-lived peak from the sustained load.
//
// This profiler instead measures EXCLUSIVE (self) CPU time per owner:
//
//     DSP load = ( leg A self time + leg B self time ) / fixed window length
//
// "Self" excludes every interval during which something else owned the CPU, so a preemption
// moves time out of the preempted owner and into the preemptor -- the total is conserved and
// the figure no longer depends on the priority assignment. `stolen[leg]` records the time a
// leg lost to a higher-priority non-leg ISR, so what the wall-time reading used to hide is now
// its own named quantity.
//
// The window length is a FIXED TIME, independent of the block period, settable at build time
// (APP_DSPLOAD_WINDOW_US) and at run time (nora_cpu_load_prof_configure). Nothing here
// hardcodes a length: the default below is a default, and every telemetry line prints the
// window it was measured over. 1 ms / 10 ms / 100 ms are therefore comparable within one image.
//
// An ownership interval that crosses a window boundary is SPLIT at the boundary and charged to
// both windows (see nora_cpu_load_prof_charge in the _fast header). Charging the whole interval
// to the window it ended in would let one long ISR read as an overrun in a window it barely
// touched.
//
// READINGS ABOVE 100 % ARE NOT CLAMPED. A load over budget is the diagnosis, not an error to be
// rounded away: 105 % says the measured owners demanded more time than the window grants, or
// that a measurement precondition was broken. Report the figure as a LOAD / DEMAND / BUDGET
// percentage with the overrun beside it -- never as an "occupancy", which a value above 100 %
// cannot be.
//
// WHAT THIS IS NOT
// ---------------
// This is not a deadline instrument. A 10 ms window spans ~30 blocks of a 333 us block period,
// so a single late block is smoothed away. Deadline margin comes from the per-leg response time
// (entry->exit, preemption included) and from the real-harm counters (miss / starve / ovf / udf
// / drop). Choosing a 1 ms window narrows the gap but does not close it.
//===========================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


//===========================================================
// Build-time gate.
//   1 : the profiler state, its ISR hooks and these entry points are compiled.
//   0 : nothing is compiled -- the hooks become empty inline bodies that the optimiser
//       removes entirely, so an OFF build pays no hot-path cost, and a call to one of the
//       foreground entry points below fails at LINK time rather than silently returning a
//       never-updated zero snapshot.
//===========================================================
#ifndef NORA_CPU_LOAD_PROF
#define NORA_CPU_LOAD_PROF   1
#endif


//===========================================================
// Default measurement window, in microseconds.
//
// 10 ms is the default because it is long enough to average the per-block jitter of every rate
// this project streams (8 k .. 192 k) yet short enough that a sustained overload shows up inside
// one console print interval. It is NOT a property of the audio path -- override with
// -DAPP_DSPLOAD_WINDOW_US=<us> for a build-wide change, or the console for a live one.
//===========================================================
#ifndef APP_DSPLOAD_WINDOW_US
#define APP_DSPLOAD_WINDOW_US   (10000UL)
#endif


//===========================================================
// Owners. Exactly one owner holds the CPU at any instant; ownership nests LIFO because this is
// a single core with prioritised vectors, and the previous owner travels in the CALLER'S STACK
// FRAME (the token returned by _enter) instead of in a state array -- so no shared ownership
// stack exists to be corrupted by a preemption, and the nesting depth is bounded by the
// hardware's own interrupt nesting.
//
// NONE covers the foreground (main loop / idle): it is accumulated like any other owner, so a
// window's four self totals plus nothing else account for the whole window, which is the
// consistency check the `windows`/`bad` counters lean on.
//
// OTHER is every instrumented non-leg vector. Deliberately one bucket: the point of the meter
// is what the ASRC legs cost and what was taken from them, not a per-vector breakdown.
//===========================================================
typedef enum {
    NORA_CPU_LOAD_OWNER_NONE  = 0,   // foreground / idle
    NORA_CPU_LOAD_OWNER_LEG_A = 1,   // TDM leg A RX-block ISR
    NORA_CPU_LOAD_OWNER_LEG_B = 2,   // TDM leg B RX-block ISR
    NORA_CPU_LOAD_OWNER_OTHER = 3,   // any other instrumented ISR
    NORA_CPU_LOAD_OWNER_COUNT = 4
} nora_cpu_load_owner_t;

// Legs tracked for `stolen` attribution: LEG_A -> index 0, LEG_B -> index 1.
#define NORA_CPU_LOAD_LEG_COUNT   (2u)

// Owner id -> stolen[] index. Valid only for LEG_A / LEG_B.
#define NORA_CPU_LOAD_LEG_INDEX(owner)   ((uint8_t)((owner) - NORA_CPU_LOAD_OWNER_LEG_A))


//===========================================================
// Foreground snapshot. All durations are raw high-res-timer ticks; the caller converts with
// nora_high_res_timer_count_to_us*() and divides by window_period_ticks for a percentage.
//
// Three time bases are reported and they answer different questions:
//   last_*  -- the most recently CLOSED window. An instantaneous reading; noisy by design.
//   mean_*  -- per-window mean over the windows closed since the last clear. This is the
//              honest "what did it cost" figure; being a mean it is window-length independent.
//   max_*   -- the largest single window since the last clear. This is what the window LENGTH
//              buys you: shorten the window and the peak rises toward the per-block truth.
//===========================================================
typedef struct
{
    uint32_t window_period_ticks;                            // 0 = not configured

    uint32_t last_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];     // last closed window, per owner
    uint32_t last_stolen_ticks[NORA_CPU_LOAD_LEG_COUNT];     // last closed window, per leg

    uint32_t mean_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];     // per-window mean, per owner
    uint32_t mean_stolen_ticks[NORA_CPU_LOAD_LEG_COUNT];     // per-window mean, per leg

    uint32_t max_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];      // peak single window, per owner
    uint32_t max_sum_ticks;                                  // peak of (leg A self + leg B self)
    uint32_t max_demand_ticks;                               // peak of (that sum + stolen)

    uint32_t windows;                                        // windows closed since the clear

    // Permanent-zero health checks. The hooks run inside an interrupt-disabled critical
    // section, which makes "now is never before the segment start" an invariant rather than a
    // hope, so unlike the predecessor's race counters these are not expected to move at all.
    // Any non-zero value means the accounting itself is wrong and the percentages are suspect.
    uint16_t unbalanced;        // _exit with nothing entered (a hook was skipped on some path)
    uint16_t neg_delta;         // an interval measured as negative
    uint16_t rebase;            // grid lost more than the catch-up bound and was re-based
    uint16_t depth_overflow;    // ownership nested deeper than the profiler tracks

    bool     initialized;
} nora_cpu_load_snapshot_t;


#if NORA_CPU_LOAD_PROF

//===========================================================
// Set the window length and re-base the grid, clearing accumulators and peaks.
//
// window_period_ticks is in raw high-res-timer counts -- the CALLER converts from microseconds,
// because the tick rate is the application's choice (main.c initialises the timer with
// timer_clk_hz = FCY, so one tick is one instruction cycle) and this module must not assume it.
// 0 is rejected (the profiler stays in its
// current state) because a zero-length window has no meaning and would spin the advance loop.
// Call once the timer is running, and again whenever the length changes.
//===========================================================
extern void nora_cpu_load_prof_configure( uint32_t window_period_ticks );

// Re-base the grid and clear accumulators/peaks, KEEPING the window length. Use on stream
// stop/resume and on a rate change, so a stopped gap does not enter the statistics.
extern void nora_cpu_load_prof_reset( void );

// Snapshot into *out. Returns false (and zeroes *out) if out is NULL or the profiler was never
// configured. clear_peak also clears the mean accumulators and the window count, so the next
// reading covers exactly the interval since this call.
extern bool nora_cpu_load_prof_get( nora_cpu_load_snapshot_t* out, bool clear_peak );

#endif // NORA_CPU_LOAD_PROF

#ifdef __cplusplus
}
#endif

#endif // NORA_CPU_LOAD_PROF_H
