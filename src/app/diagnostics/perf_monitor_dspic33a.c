/*
 * perf_monitor_dspic33a.c - see perf_monitor_dspic33a.h.
 */
#include "app_specific_config_defs.h"

#if defined(ENA_PERF_MONITOR) && ENA_PERF_MONITOR

#include <stdio.h>
#include <xc.h>

#include "diagnostics/perf_monitor_dspic33a.h"

/*
 * HPCCON bit positions, DS70005591D section 3.5.2.1.
 *
 * Written as explicit masks on the whole 32-bit register rather than through
 * HPCCONbits, because CLR is R/S (write-1-to-clear-the-counters, always reads
 * 0): a read-modify-write through the bitfield struct would be a correct way to
 * express "set ON" and a misleading way to express "pulse CLR". A single
 * assignment of the whole word states exactly what reaches the SFR.
 */
#define PERF_HPCCON_CLR     (1UL << 13)
#define PERF_HPCCON_ON      (1UL << 15)

/*
 * SELECTn[4:0] live in HPSEL0 (counters 0..3) and HPSEL1 (counters 4..7), one
 * per byte lane. Byte lanes, not a packed 5-bit array: counter n's field starts
 * at bit 8*(n mod 4), which is why the shift below is by 8 and the mask is 5
 * bits inside that lane.
 */
#define PERF_SELECT_MASK    (0x1FUL)
#define PERF_SELECT_SHIFT(n)    ( 8u * ( (n) & 3u ) )

/*
 * What each counter was programmed with, remembered on the C side.
 *
 * Kept rather than read back from HPSEL0/1 on demand for one reason: a printed
 * stall table whose row labels come from the same source the caller configured
 * cannot mislabel a row, whereas re-deriving the label from the SFR would print
 * a plausible table if a write had silently not landed. The two are cross-checked
 * once, in perf_monitor_configure(), where a mismatch is worth knowing about.
 */
static perf_event_t s_event[ PERF_MONITOR_COUNTERS ];
static bool         s_configured;

/*
 * Counter register addresses.
 *
 * The device header declares HPCCNTL0..7 / HPCCNTH0..7 as separate volatile
 * uint32_t SFR objects, not as an array, so an index has to come from
 * somewhere. A table of addresses is that somewhere; the alternative -
 * computing HPCCNTL0's address plus 8*n - would encode the register spacing as
 * arithmetic, and a future part that interleaves them differently would then
 * read the wrong SFR while still compiling. DS70005591D section 3.5.2 lists
 * these at 0x1E20 stepping by 8, which is what the table reflects.
 */
static volatile uint32_t *const s_cnt_lo[ PERF_MONITOR_COUNTERS ] =
{
    &HPCCNTL0, &HPCCNTL1, &HPCCNTL2, &HPCCNTL3,
    &HPCCNTL4, &HPCCNTL5, &HPCCNTL6, &HPCCNTL7,
};

static volatile uint32_t *const s_cnt_hi[ PERF_MONITOR_COUNTERS ] =
{
    &HPCCNTH0, &HPCCNTH1, &HPCCNTH2, &HPCCNTH3,
    &HPCCNTH4, &HPCCNTH5, &HPCCNTH6, &HPCCNTH7,
};

const char *perf_monitor_event_name( perf_event_t ev )
{
    switch( ev )
    {
    case PERF_EVENT_NONE:                return "(none)";
    case PERF_EVENT_CPU_CYCLES:          return "CPU cycles";
    case PERF_EVENT_INSTR_COMPLETED:     return "instr completed";
    case PERF_EVENT_FPU_WRITE_STALL:     return "FPU write stall";
    case PERF_EVENT_WRITE_STAGE_STALL:   return "write stage stall";
    case PERF_EVENT_COND_BRANCH:         return "cond branch";
    case PERF_EVENT_BRANCH_MISPREDICT:   return "branch mispredict";
    case PERF_EVENT_ADDR_HAZARD:         return "CPU addr hazard";
    case PERF_EVENT_FPU_INSTR_STALL:     return "FPU instr stall";
    case PERF_EVENT_FPU_READ_STALL:      return "FPU read stall";
    case PERF_EVENT_ADDR_READ_STALL:     return "addr read stall";
    case PERF_EVENT_ADDR_STAGE_STALL:    return "addr stage stall";
    case PERF_EVENT_IRQ_LATENCY:         return "IRQ latency";
    case PERF_EVENT_FETCH_READ_STALL:    return "fetch read stall";
    case PERF_EVENT_PROGRAM_FLOW_CHANGE: return "program flow change";
    case PERF_EVENT_VECTOR_FETCH:        return "vector fetch";
    case PERF_EVENT_CACHE_BUSY:          return "cache busy";
    case PERF_EVENT_FETCH_PBU_HIT:       return "fetch PBU hit";
    case PERF_EVENT_FETCH_PBU_MISS:      return "fetch PBU miss";
    default:                             return "(unknown)";
    }
}

bool perf_monitor_configure( const perf_event_t *events, uint32_t count )
{
    uint32_t sel0 = 0u;
    uint32_t sel1 = 0u;

    if( ( events == NULL ) || ( count > PERF_MONITOR_COUNTERS ) )
    {
        return false;
    }

    /* Stop before reprogramming. Changing an event under a running counter
     * would attribute cycles counted for the old event to the new one. */
    HPCCON = 0u;

    for( uint32_t i = 0u; i < PERF_MONITOR_COUNTERS; i++ )
    {
        const perf_event_t ev = ( i < count ) ? events[i] : PERF_EVENT_NONE;
        const uint32_t     f  = ( (uint32_t)ev & PERF_SELECT_MASK )
                                  << PERF_SELECT_SHIFT( i );

        s_event[i] = ev;

        if( i < 4u )
        {
            sel0 |= f;
        }
        else
        {
            sel1 |= f;
        }
    }

    HPSEL0 = sel0;
    HPSEL1 = sel1;

    /* Read back. These are plain R/W SFRs, so a mismatch means the write did
     * not reach the register - and every number this module later prints would
     * be labelled with an event that is not the one counting. Cheaper to find
     * out here than in a stall table. */
    if( ( HPSEL0 != sel0 ) || ( HPSEL1 != sel1 ) )
    {
        s_configured = false;
        return false;
    }

    /* Leave cleared and stopped: the caller decides when the region starts. */
    HPCCON = PERF_HPCCON_CLR;

    s_configured = true;
    return true;
}

void perf_monitor_start( void )
{
    /* CLR and ON in one write. CLR is R/S and takes effect on the write, so
     * this starts a region from zero without a separate clear-then-start
     * window in which a few cycles could already be counted. */
    HPCCON = PERF_HPCCON_CLR | PERF_HPCCON_ON;
}

void perf_monitor_stop( void )
{
    HPCCON = 0u;
}

uint64_t perf_monitor_read( uint32_t counter )
{
    if( counter >= PERF_MONITOR_COUNTERS )
    {
        return 0u;
    }

    /*
     * Low then high, with no re-read. DS70005591D section 3.5.3.3 says counter
     * values should only be read when ON = 0, and this API only ever exposes a
     * read after perf_monitor_stop(), so the pair cannot advance between the
     * two loads and the usual high-low-high protocol would be answering a
     * question this hardware does not pose.
     */
    const uint32_t lo = *s_cnt_lo[ counter ];
    const uint32_t hi = *s_cnt_hi[ counter ];

    return ( (uint64_t)hi << 32 ) | (uint64_t)lo;
}

void perf_monitor_read_all( uint64_t *out, uint32_t count )
{
    if( out == NULL )
    {
        return;
    }

    for( uint32_t i = 0u; ( i < count ) && ( i < PERF_MONITOR_COUNTERS ); i++ )
    {
        out[i] = perf_monitor_read( i );
    }
}

perf_event_t perf_monitor_event_of( uint32_t counter )
{
    if( counter >= PERF_MONITOR_COUNTERS )
    {
        return PERF_EVENT_NONE;
    }
    return s_event[ counter ];
}

/*
 * Prints the counters as a table.
 *
 * Counts are printed as 32-bit values with a saturation marker rather than as
 * 64-bit: this tree's printf has no 64-bit conversion that can be relied on,
 * and every region this instrument is pointed at is a few tens of thousands of
 * cycles. A count that genuinely exceeded 32 bits would be a measurement
 * mistake (a region left running), and printing "OVER" is the right way to say
 * so - silently truncating would produce a small, plausible, wrong number.
 */
static void perf_print_count( uint64_t v, uint32_t norm_divisor )
{
    if( v > 0xFFFFFFFFull )
    {
        printf( "      OVER32" );
        return;
    }

    printf( " %11lu", (unsigned long)v );

    if( norm_divisor != 0u )
    {
        /* Three decimal places, computed in 64-bit so the scaling cannot
         * overflow before the divide. */
        const uint64_t scaled = ( v * 1000ull ) / (uint64_t)norm_divisor;

        printf( "  %5lu.%03lu",
                (unsigned long)( scaled / 1000ull ),
                (unsigned long)( scaled % 1000ull ) );
    }
}

void perf_monitor_print( const char *label, uint32_t norm_divisor )
{
    if( !s_configured )
    {
        printf( "PMU [%s]: not configured - nothing to report\r\n",
                ( label != NULL ) ? label : "?" );
        return;
    }

    printf( "PMU [%s]%s\r\n", ( label != NULL ) ? label : "?",
            ( norm_divisor != 0u ) ? "  (second column = per unit)" : "" );

    for( uint32_t i = 0u; i < PERF_MONITOR_COUNTERS; i++ )
    {
        const perf_event_t ev = s_event[i];

        if( ev == PERF_EVENT_NONE )
        {
            continue;
        }

        printf( "  c%lu ev%-2lu %-20s:",
                (unsigned long)i, (unsigned long)ev,
                perf_monitor_event_name( ev ) );
        perf_print_count( perf_monitor_read( i ), norm_divisor );
        printf( "\r\n" );
    }

    /*
     * CPI, but only when counters 0 and 1 actually hold the cycle and
     * instruction references. Deriving it from whatever happens to be in those
     * two slots is how a stall table acquires a confident, meaningless number.
     */
    if( ( s_event[0] == PERF_EVENT_CPU_CYCLES )
     && ( s_event[1] == PERF_EVENT_INSTR_COMPLETED ) )
    {
        const uint64_t cyc   = perf_monitor_read( 0u );
        const uint64_t instr = perf_monitor_read( 1u );

        if( instr != 0u )
        {
            const uint64_t cpi = ( cyc * 1000ull ) / instr;

            printf( "  CPI (c0/c1)              : %lu.%03lu\r\n",
                    (unsigned long)( cpi / 1000ull ),
                    (unsigned long)( cpi % 1000ull ) );
        }
        else
        {
            printf( "  CPI (c0/c1)              : n/a (no instruction"
                    " completed - was the region ever started?)\r\n" );
        }
    }
}

#endif /* ENA_PERF_MONITOR */
