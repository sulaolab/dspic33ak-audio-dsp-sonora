/*
 * nora_cpu_load_prof_dspic33ak.c
 * ------------------------------
 * Foreground half of the fixed-window CPU load profiler: the state definition and the
 * configure / reset / snapshot entry points. The ISR half is inline in
 * nora_cpu_load_prof_fast.h.
 *
 * Everything here runs with interrupts masked for the duration of the state access. The window
 * is closed and re-based from the foreground rather than from a designated ISR: the grid is
 * free-running by design, so nothing needs to be phase-locked to an audio block. That is the
 * point of the redesign -- the predecessor tied its window phase to one leg's block boundary,
 * which made the reading depend on which leg was streaming.
 */

#include "nora_cpu_load_prof.h"

#if NORA_CPU_LOAD_PROF

#include "nora_cpu_load_prof_fast.h"
#include "nora_high_res_timer.h"

#include <string.h>


nora_cpu_load_prof_state_t g_nora_cpu_load_prof;


/* Zero every statistic but keep the grid and the current ownership. Caller holds the lock. */
static void prof_clear_stats( void )
{
    uint8_t i;

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
    {
        g_nora_cpu_load_prof.self_ticks[i]      = 0u;
        g_nora_cpu_load_prof.last_self_ticks[i] = 0u;
        g_nora_cpu_load_prof.acc_self_ticks[i]  = 0u;
        g_nora_cpu_load_prof.max_self_ticks[i]  = 0u;
    }
    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_LEG_COUNT; i++ )
    {
        g_nora_cpu_load_prof.stolen_ticks[i]      = 0u;
        g_nora_cpu_load_prof.last_stolen_ticks[i] = 0u;
        g_nora_cpu_load_prof.acc_stolen_ticks[i]  = 0u;
    }

    g_nora_cpu_load_prof.max_sum_ticks    = 0u;
    g_nora_cpu_load_prof.max_demand_ticks = 0u;
    g_nora_cpu_load_prof.windows          = 0u;
    g_nora_cpu_load_prof.unbalanced       = 0u;
    g_nora_cpu_load_prof.neg_delta        = 0u;
    g_nora_cpu_load_prof.rebase           = 0u;
    g_nora_cpu_load_prof.depth_overflow   = 0u;
}


//===========================================================
// The two ISR hooks.
//
// They live here, out of line, on purpose: an inlined hook needs more than the eight
// hardware-banked working registers, which puts register pushes back into the prologue of every
// `context` interrupt handler that calls it -- and on this part those pushes trap (see the header
// note, and the DO-NOT-REVERT note above _CCP1Interrupt in the application-level ASRC clock
// control). Called, the
// register traffic happens in this function's own frame, where it is ordinary.
//
// Cost is a call, a masked timer read, and the arithmetic for one segment: tens of cycles, no
// loop except the bounded window catch-up.
//===========================================================
void nora_cpu_load_prof_enter( uint8_t id )
{
    const unsigned int isr_state = nora_cpu_load_prof_lock();

    nora_cpu_load_prof_charge( nora_high_res_timer_get_count() );

    if( g_nora_cpu_load_prof.depth < (uint8_t)NORA_CPU_LOAD_PROF_MAX_DEPTH )
    {
        /* Suspend the current owner: (owner, leg_active) packed into one byte. */
        g_nora_cpu_load_prof.stack[g_nora_cpu_load_prof.depth] =
            (uint8_t)( g_nora_cpu_load_prof.owner |
                       (uint8_t)( g_nora_cpu_load_prof.leg_active << 4 ) );
        g_nora_cpu_load_prof.depth++;
    }
    else
    {
        /* Nested deeper than tracked. Counted; the innermost owner still gets its time, and the
         * matching _exit will restore NONE, so the accounting degrades visibly instead of
         * writing past the array. */
        g_nora_cpu_load_prof.depth_overflow++;
    }

    g_nora_cpu_load_prof.owner = id;
    if( ( id == (uint8_t)NORA_CPU_LOAD_OWNER_LEG_A ) ||
        ( id == (uint8_t)NORA_CPU_LOAD_OWNER_LEG_B ) )
    {
        g_nora_cpu_load_prof.leg_active = id;
    }

    nora_cpu_load_prof_unlock( isr_state );
}


void nora_cpu_load_prof_exit( void )
{
    const unsigned int isr_state = nora_cpu_load_prof_lock();

    if( g_nora_cpu_load_prof.depth == 0u )
    {
        // _exit with nothing entered: some path out of an instrumented ISR skipped its _enter, or
        // called _exit twice. Counted, and the state is left alone so the profiler keeps producing
        // usable numbers rather than corrupting the grid.
        g_nora_cpu_load_prof.unbalanced++;
        nora_cpu_load_prof_unlock( isr_state );
        return;
    }

    nora_cpu_load_prof_charge( nora_high_res_timer_get_count() );

    g_nora_cpu_load_prof.depth--;
    {
        const uint8_t packed = g_nora_cpu_load_prof.stack[g_nora_cpu_load_prof.depth];

        g_nora_cpu_load_prof.owner      = (uint8_t)( packed & 0x0Fu );
        g_nora_cpu_load_prof.leg_active = (uint8_t)( packed >> 4 );
    }

    nora_cpu_load_prof_unlock( isr_state );
}


void nora_cpu_load_prof_configure_at( uint32_t now, uint32_t window_period_ticks )
{
    const unsigned int isr_state = nora_cpu_load_prof_lock();

    if( window_period_ticks != 0u )
    {
        g_nora_cpu_load_prof.period      = window_period_ticks;
        g_nora_cpu_load_prof.end         = now + window_period_ticks;
        g_nora_cpu_load_prof.seg_start   = now;
        /* Ownership is NOT reset. configure() can be called from the foreground while an ISR is
         * mid-flight only in the sense that the call itself is masked; on the other side of the
         * mask the owner is whatever it was, and pretending otherwise would leave the next
         * _exit unbalanced. */
        prof_clear_stats();
        g_nora_cpu_load_prof.initialized = true;
    }

    nora_cpu_load_prof_unlock( isr_state );
}


void nora_cpu_load_prof_reset_at( uint32_t now )
{
    const unsigned int isr_state = nora_cpu_load_prof_lock();

    if( g_nora_cpu_load_prof.period != 0u )
    {
        g_nora_cpu_load_prof.end       = now + g_nora_cpu_load_prof.period;
        g_nora_cpu_load_prof.seg_start = now;
        prof_clear_stats();
        g_nora_cpu_load_prof.initialized = true;
    }

    nora_cpu_load_prof_unlock( isr_state );
}


void nora_cpu_load_prof_configure( uint32_t window_period_ticks )
{
    nora_cpu_load_prof_configure_at( nora_high_res_timer_get_count(), window_period_ticks );
}


void nora_cpu_load_prof_reset( void )
{
    nora_cpu_load_prof_reset_at( nora_high_res_timer_get_count() );
}


/*
 * INTERRUPT-MASK BUDGET -- read this before adding anything to the critical section below.
 *
 * The masked region holds off EVERY vector, the audio block ISR included, so its length is added
 * straight to that ISR's interrupt-entry latency. It is not paid by the caller; it is paid by the
 * hardest real-time thing in the system.
 *
 * This function used to compute the per-window means -- six 64-bit divisions -- INSIDE the mask.
 * Measured on AK506 Nano at 84-stage x 4ch DRC (block period 666.6 us, ISR response 651.1 us,
 * margin +15.5 us): the periodic report that calls this delayed the block ISR's ENTRY by +53.0 us
 * against a 667.6-670.4 us quiet baseline -- 3.4x the whole margin -- and it was audible as a click
 * synchronised to the report. `resp` (entry->exit) and `miss` (a whole block late) are both
 * structurally blind to an entry delay, so every existing instrument read healthy throughout.
 *
 * So: inside the mask, COPY raw shared state to locals and reset the accumulators. Nothing else.
 * Division, scaling and any other derived value belong after the unlock, where the block ISR can
 * preempt them freely. The 64-bit copies stay inside because a 64-bit load is not atomic against
 * the hooks on this core -- shortening the mask must not be bought with a torn read.
 */
bool nora_cpu_load_prof_get( nora_cpu_load_snapshot_t* out, bool clear_peak )
{
    unsigned int isr_state;
    uint32_t     windows;
    uint8_t      i;
    /* Raw accumulators, carried out of the critical section so the division happens outside it. */
    uint64_t     acc_self[NORA_CPU_LOAD_OWNER_COUNT];
    uint64_t     acc_stolen[NORA_CPU_LOAD_LEG_COUNT];

    if( out == NULL )
    {
        return false;
    }

    memset( out, 0, sizeof( *out ) );

    isr_state = nora_cpu_load_prof_lock();

    if( !g_nora_cpu_load_prof.initialized )
    {
        nora_cpu_load_prof_unlock( isr_state );
        return false;
    }

    out->window_period_ticks = g_nora_cpu_load_prof.period;
    out->windows             = g_nora_cpu_load_prof.windows;
    out->unbalanced          = g_nora_cpu_load_prof.unbalanced;
    out->neg_delta           = g_nora_cpu_load_prof.neg_delta;
    out->rebase              = g_nora_cpu_load_prof.rebase;
    out->depth_overflow      = g_nora_cpu_load_prof.depth_overflow;
    out->max_sum_ticks       = g_nora_cpu_load_prof.max_sum_ticks;
    out->max_demand_ticks    = g_nora_cpu_load_prof.max_demand_ticks;

    windows = g_nora_cpu_load_prof.windows;

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
    {
        out->last_self_ticks[i] = g_nora_cpu_load_prof.last_self_ticks[i];
        out->max_self_ticks[i]  = g_nora_cpu_load_prof.max_self_ticks[i];
        acc_self[i]             = g_nora_cpu_load_prof.acc_self_ticks[i];
    }

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_LEG_COUNT; i++ )
    {
        out->last_stolen_ticks[i] = g_nora_cpu_load_prof.last_stolen_ticks[i];
        acc_stolen[i]             = g_nora_cpu_load_prof.acc_stolen_ticks[i];
    }

    if( clear_peak )
    {
        /* Clears the accumulators, the peaks and the window count, but NOT the open window's
         * partial totals and NOT the grid: the next reading then covers exactly the interval
         * from here to the next call, and no boundary is invented in the middle of an ISR. */
        for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
        {
            g_nora_cpu_load_prof.acc_self_ticks[i] = 0u;
            g_nora_cpu_load_prof.max_self_ticks[i] = 0u;
        }
        g_nora_cpu_load_prof.acc_stolen_ticks[0] = 0u;
        g_nora_cpu_load_prof.acc_stolen_ticks[1] = 0u;
        g_nora_cpu_load_prof.max_sum_ticks       = 0u;
        g_nora_cpu_load_prof.max_demand_ticks    = 0u;
        g_nora_cpu_load_prof.windows             = 0u;
    }

    nora_cpu_load_prof_unlock( isr_state );

    /* ---- Derived values. INTERRUPTS ARE ON from here; keep it that way. ----
     * Six 64-bit divisions on local copies. Identical arithmetic on identical inputs to what used
     * to run inside the mask, so every published figure is bit-for-bit unchanged -- only the
     * interrupt latency it cost is gone. Per-window mean: zero windows means nothing has closed
     * yet, so report 0 rather than dividing, and let the caller see windows == 0 and say so. */
    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_OWNER_COUNT; i++ )
    {
        out->mean_self_ticks[i] = ( windows != 0u )
            ? (uint32_t)( acc_self[i] / (uint64_t)windows )
            : 0u;
    }

    for( i = 0u; i < (uint8_t)NORA_CPU_LOAD_LEG_COUNT; i++ )
    {
        out->mean_stolen_ticks[i] = ( windows != 0u )
            ? (uint32_t)( acc_stolen[i] / (uint64_t)windows )
            : 0u;
    }

    out->initialized = true;

    return true;
}

#endif // NORA_CPU_LOAD_PROF
