#ifndef NORA_CPU_LOAD_PROF_FAST_H
#define NORA_CPU_LOAD_PROF_FAST_H

//===========================================================
// ISR fast path for the fixed-window CPU load profiler. See nora_cpu_load_prof.h for what the
// instrument measures and why; this header is the mutable state plus the two hooks an ISR calls.
//
// Naming follows the backend fast-path convention already used by nora_dma_dspic33ak_fast.h and
// nora_spi_i2s_tdm_dspic33ak_diag_fast.h: portable consumers include the plain header and see
// snapshots only, ISRs include the _fast header and see the state.
//
// Unlike those two, this header is NOT device-private and carries no device name. Its hooks are
// called from application ISRs in several unrelated modules (TDM RX blocks, the CCP rate-capture
// vectors, the tick timer, the UART vectors), and none of those should have to include a private
// TDM or DMA backend header to time itself. The state lives in the timer HAL because the
// instrument IS a timer client and nothing else.
//
// HOW TO INSTRUMENT AN ISR
// -----------------------
//     void __attribute__((interrupt, context)) _SomeInterrupt(void)
//     {
//         nora_cpu_load_prof_enter( NORA_CPU_LOAD_OWNER_OTHER );
//         ... body ...
//         nora_cpu_load_prof_exit();          // on EVERY path out of the ISR
//     }
//
// _exit MUST be reached on every path out, once per _enter. An early return that skips it leaves
// the profiler thinking the ISR still owns the CPU; that is what `unbalanced` counts.
//
// WHY THE HOOKS ARE FUNCTION CALLS, AND WHY _exit TAKES NO TOKEN
// -------------------------------------------------------------
// Both properties exist to keep the hooks out of the CALLER'S PROLOGUE, and that is a silicon
// requirement on this part, not a style preference.
//
// dsPIC33AK512MPS512 rev A1 traps STACK ERROR on the `mov.l wN,[w15++]` register pushes a
// compiler emits at the top of an interrupt handler (localised 2026-08-26 by controlled A/B;
// see the long note above _CCP1Interrupt in asrc_clock_control.c -- DO NOT REVERT the
// `__attribute__((interrupt, context))` there). Those vectors are push-free only because they
// are LEAF handlers living entirely in the hardware-banked W0-W7.
//
// An inlined hook destroys that twice over: its body needs more than eight registers, and a
// returned token stays live across the ISR body, which the ABI can only hold in callee-saved
// W8-W13 -- so the handler's prologue pushes them. Measured: inline hooks with a token put
// exactly six `mov.l wN,[w15++]` back at the top of _CCP1Interrupt, _CCP2Interrupt, _T1Interrupt
// and _U1RXInterrupt.
//
// So the hooks are real calls (the register saving happens inside the callee, where it is
// ordinary and harmless), and the previous owner is remembered in the profiler's own bounded
// stack instead of in the caller's frame -- leaving the instrumented handler with nothing live
// across the call and its prologue empty. The internal stack is safe where the predecessor's
// shared state was not, because every hook body runs interrupt-masked: nesting cannot interleave.
//
// WHAT NOT TO INSTRUMENT
// ---------------------
// Only vectors that (a) can actually run while the measured legs are streaming AND (b) sit at or
// above a leg's priority need a hook. A vector below every leg cannot preempt one, so it can only
// take foreground time, which the NONE bucket already accounts for; hooking it costs cycles and
// tells you nothing new. Do not blanket-instrument every ISR in the project.
//===========================================================

#include "nora_cpu_load_prof.h"
#include "nora_high_res_timer.h"

#ifdef __cplusplus
extern "C" {
#endif


#if NORA_CPU_LOAD_PROF

//===========================================================
// Bound on the window-advance loop inside one charge().
//
// Steady state advances 0 or 1 windows per hook. A longer gap means nobody called a hook for
// several windows (streaming stopped, a debugger halt, the foreground sat in a blocking print),
// and walking every empty window would put an unbounded loop inside an interrupt-disabled
// critical section. Past the bound the grid is re-based on `now` and `rebase` is incremented, so
// the discontinuity is reported rather than smeared into the statistics.
//===========================================================
#define NORA_CPU_LOAD_PROF_MAX_CATCHUP   (4u)


//===========================================================
// Ownership nesting depth.
//
// Nesting is bounded by the hardware: one foreground level plus at most one instrumented handler
// per interrupt priority that can actually preempt another. Eight is that with room to spare, and
// an overflow is counted rather than allowed to write past the array -- so a mis-instrumented
// build degrades into a visibly wrong `bad=` field instead of corrupting memory.
//===========================================================
#define NORA_CPU_LOAD_PROF_MAX_DEPTH     (8u)


typedef struct
{
    volatile uint32_t period;          // window length [ticks]; 0 = not configured
    volatile uint32_t end;             // end tick of the window currently open
    volatile uint32_t seg_start;       // tick at which the current owner took the CPU

    // Current (open) window.
    volatile uint32_t self_ticks[NORA_CPU_LOAD_OWNER_COUNT];
    volatile uint32_t stolen_ticks[NORA_CPU_LOAD_LEG_COUNT];

    // Most recently closed window, published atomically-enough for a masked foreground read.
    volatile uint32_t last_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];
    volatile uint32_t last_stolen_ticks[NORA_CPU_LOAD_LEG_COUNT];

    // Sums over the windows closed since the last clear. 64-bit because a 32-bit sum of 10 ms
    // windows wraps after ~43 s of accumulation, which is well inside a plausible reporting
    // interval; the wide add costs nothing where it happens (window close, <= 1 kHz).
    volatile uint64_t acc_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];
    volatile uint64_t acc_stolen_ticks[NORA_CPU_LOAD_LEG_COUNT];

    // Peaks over the windows closed since the last clear. NOT clamped to `period` -- see the
    // note in nora_cpu_load_prof.h: a figure above the budget is the finding.
    volatile uint32_t max_self_ticks[NORA_CPU_LOAD_OWNER_COUNT];
    volatile uint32_t max_sum_ticks;
    volatile uint32_t max_demand_ticks;

    volatile uint32_t windows;         // windows closed since the last clear

    // Permanent-zero health checks; see the snapshot struct.
    volatile uint16_t unbalanced;
    volatile uint16_t neg_delta;
    volatile uint16_t rebase;
    volatile uint16_t depth_overflow;

    // Who owns the CPU right now, and which leg (if any) is somewhere below on the stack.
    // leg_active is what makes `stolen` attributable: when OTHER runs, the time is stolen from
    // whichever leg it interrupted, and that leg is not necessarily the immediately previous
    // owner (OTHER can nest over OTHER over a leg).
    volatile uint8_t  owner;
    volatile uint8_t  leg_active;      // NONE / LEG_A / LEG_B

    // Suspended (owner, leg_active) pairs, one per nesting level, packed one byte each:
    // owner in the low nibble, leg_active in the high nibble. Written and read only inside the
    // masked hook bodies, so no interleaving is possible.
    volatile uint8_t  stack[NORA_CPU_LOAD_PROF_MAX_DEPTH];
    volatile uint8_t  depth;

    volatile bool     initialized;
} nora_cpu_load_prof_state_t;

// Defined by nora_cpu_load_prof_dspic33ak.c. External linkage because the inline hooks below are
// compiled into every instrumented ISR's translation unit while the foreground API is not.
extern nora_cpu_load_prof_state_t g_nora_cpu_load_prof;


//===========================================================
// Critical section. Same idiom as nora_gpio_reg_set() -- save SR's interrupt state, mask, do the
// read-modify-write, restore.
//
// This is not optional decoration. The hook body reads the timer, reads seg_start, and stores a
// new seg_start; a preemption between the read and the store makes the next charge measure a
// NEGATIVE interval, which folded into a uint32 accumulator wraps to ~2^32 and destroys the peak
// (that is exactly what the predecessor profiler did on hardware at 48k/44.1k: peak = 2^32-225
// ticks, printed as load = 12,886,189.8 %). The predecessor answered this by refusing negative
// intervals and counting the refusals; masking removes the race instead, which is why
// `neg_delta` here is a permanent-zero check rather than an expected loss.
//
// The masked region is a few tens of cycles and contains no loop other than the bounded
// catch-up, so worst-case added interrupt latency is bounded and small.
//===========================================================
static inline unsigned int nora_cpu_load_prof_lock( void )
{
    const unsigned int isr_state = __builtin_get_isr_state();

    __builtin_disable_interrupts();
    return isr_state;
}

static inline void nora_cpu_load_prof_unlock( unsigned int isr_state )
{
    __builtin_set_isr_state( isr_state );
}


// Charge [seg_start, upto) to `own`, and to stolen[leg_active] as well when a non-leg owner is
// running on top of a leg. `stolen` is a SUBSET of self_[OTHER], never an addition to a leg's
// own time, so demand = legA_self + legB_self + stolen double-counts nothing.
static inline void nora_cpu_load_prof_charge_seg( uint8_t own, uint32_t upto )
{
    const int32_t d = (int32_t)( upto - g_nora_cpu_load_prof.seg_start );

    if( d <= 0 )
    {
        if( d < 0 )
        {
            g_nora_cpu_load_prof.neg_delta++;
        }
        return;
    }

    g_nora_cpu_load_prof.self_ticks[own] += (uint32_t)d;

    if( ( own == (uint8_t)NORA_CPU_LOAD_OWNER_OTHER ) &&
        ( g_nora_cpu_load_prof.leg_active != (uint8_t)NORA_CPU_LOAD_OWNER_NONE ) )
    {
        g_nora_cpu_load_prof.stolen_ticks[
            NORA_CPU_LOAD_LEG_INDEX( g_nora_cpu_load_prof.leg_active )] += (uint32_t)d;
    }
}


// Publish the window that just ended, fold it into the accumulators and peaks, and clear the
// per-window totals for the next one.
static inline void nora_cpu_load_prof_close_window( void )
{
    const uint32_t a      = g_nora_cpu_load_prof.self_ticks[NORA_CPU_LOAD_OWNER_LEG_A];
    const uint32_t b      = g_nora_cpu_load_prof.self_ticks[NORA_CPU_LOAD_OWNER_LEG_B];
    const uint32_t st     = g_nora_cpu_load_prof.stolen_ticks[0] +
                            g_nora_cpu_load_prof.stolen_ticks[1];
    const uint32_t sum    = a + b;
    const uint32_t demand = sum + st;
    uint8_t i;

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
    {
        const uint32_t v = g_nora_cpu_load_prof.self_ticks[i];

        g_nora_cpu_load_prof.last_self_ticks[i] = v;
        g_nora_cpu_load_prof.acc_self_ticks[i] += (uint64_t)v;
        if( v > g_nora_cpu_load_prof.max_self_ticks[i] )
        {
            g_nora_cpu_load_prof.max_self_ticks[i] = v;
        }
        g_nora_cpu_load_prof.self_ticks[i] = 0u;
    }

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_LEG_COUNT; i++ )
    {
        const uint32_t v = g_nora_cpu_load_prof.stolen_ticks[i];

        g_nora_cpu_load_prof.last_stolen_ticks[i] = v;
        g_nora_cpu_load_prof.acc_stolen_ticks[i] += (uint64_t)v;
        g_nora_cpu_load_prof.stolen_ticks[i] = 0u;
    }

    if( sum > g_nora_cpu_load_prof.max_sum_ticks )
    {
        g_nora_cpu_load_prof.max_sum_ticks = sum;
    }
    if( demand > g_nora_cpu_load_prof.max_demand_ticks )
    {
        g_nora_cpu_load_prof.max_demand_ticks = demand;
    }

    g_nora_cpu_load_prof.windows++;
}


/*
 * Charge everything owed up to `now`, closing whatever windows `now` has passed.
 *
 * The window boundary SPLIT lives here: an ownership interval that spans a boundary is cut at
 * the boundary, the part before it charged to the window being closed and the remainder carried
 * into the next one by moving seg_start to the boundary. Charging the whole interval to the
 * window it ended in would let a long ISR read as an overrun in a window it barely entered, and
 * leave the window it actually occupied looking idle.
 *
 * Must be called with the profiler lock held.
 */
static inline void nora_cpu_load_prof_charge( uint32_t now )
{
    const uint8_t own = g_nora_cpu_load_prof.owner;
    uint8_t guard = 0u;

    if( !g_nora_cpu_load_prof.initialized || ( g_nora_cpu_load_prof.period == 0u ) )
    {
        return;
    }

    while( (int32_t)( now - g_nora_cpu_load_prof.end ) >= 0 )
    {
        nora_cpu_load_prof_charge_seg( own, g_nora_cpu_load_prof.end );
        g_nora_cpu_load_prof.seg_start = g_nora_cpu_load_prof.end;

        nora_cpu_load_prof_close_window();

        g_nora_cpu_load_prof.end += g_nora_cpu_load_prof.period;

        if( ++guard >= NORA_CPU_LOAD_PROF_MAX_CATCHUP )
        {
            // Grid too far behind to walk. Re-base on `now` and drop the gap rather than
            // manufacturing empty windows -- counted, so the discontinuity is never silent.
            uint8_t i;

            for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
            {
                g_nora_cpu_load_prof.self_ticks[i] = 0u;
            }
            g_nora_cpu_load_prof.stolen_ticks[0] = 0u;
            g_nora_cpu_load_prof.stolen_ticks[1] = 0u;

            g_nora_cpu_load_prof.end       = now + g_nora_cpu_load_prof.period;
            g_nora_cpu_load_prof.seg_start = now;
            g_nora_cpu_load_prof.rebase++;
            return;
        }
    }

    nora_cpu_load_prof_charge_seg( own, now );
    g_nora_cpu_load_prof.seg_start = now;
}


//===========================================================
// ISR hooks. Deliberately NOT inline -- see "WHY THE HOOKS ARE FUNCTION CALLS" at the top of this
// header. _enter makes `id` the current owner and suspends the previous one; _exit resumes it.
//
// Both are safe to call before the profiler has been configured: they still track ownership, and
// charge() simply does not accumulate until a window exists.
//===========================================================
extern void nora_cpu_load_prof_enter( uint8_t id );
extern void nora_cpu_load_prof_exit( void );


// Backend entry points used by the foreground API (nora_cpu_load_prof_dspic33ak.c).
void nora_cpu_load_prof_configure_at( uint32_t now, uint32_t window_period_ticks );
void nora_cpu_load_prof_reset_at( uint32_t now );

#else // !NORA_CPU_LOAD_PROF

// OFF: the hooks stay callable so instrumented ISRs need no #if of their own, but here they ARE
// inline and empty, so the optimiser removes the calls entirely and an OFF build is byte-identical
// to an uninstrumented one -- including the push-free prologues the ON build takes care to keep.
static inline void nora_cpu_load_prof_enter( uint8_t id )
{
    (void)id;
}

static inline void nora_cpu_load_prof_exit( void )
{
}

#endif // NORA_CPU_LOAD_PROF

#ifdef __cplusplus
}
#endif

#endif // NORA_CPU_LOAD_PROF_FAST_H
