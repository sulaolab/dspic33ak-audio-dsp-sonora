#ifndef NORA_TICK_TIMER_H
#define NORA_TICK_TIMER_H

/*
 * nora_tick_timer.h
 * ----------------------
 * Minimal Timer1-based 1 ms monotonic tick source for non-blocking timing
 * and timeout callbacks.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NORA_TICK_TIMER_OK = 0,
    NORA_TICK_TIMER_ERR_INVALID_ARG,
    NORA_TICK_TIMER_ERR_NOT_PRESENT,
    NORA_TICK_TIMER_ERR_NOT_INITIALIZED,
    NORA_TICK_TIMER_ERR_OUT_OF_RANGE,
    /*
     * `timer_clk_hz` does not divide exactly by 1000 at any available prescaler, so no
     * PR1 value gives a 1.000 ms period. Distinct from ERR_OUT_OF_RANGE, where the
     * period IS exact but the count does not fit in 32 bits.
     *
     * REFUSING IS THE POINT. The alternative -- rounding to the nearest count and
     * returning OK -- was what this did until 2026-08-11, and it is the worse failure:
     * every cadence in the tree is expressed in milliseconds of this tick
     * (timer_app/timer_app.h), so a silently approximate tick makes every one of them
     * wrong by the same unstated factor, and the symptom is a timing drift nobody can
     * attribute. A refused init is reported once at bring-up by the profile that asked
     * for it.
     */
    NORA_TICK_TIMER_ERR_INEXACT_PERIOD,
    /*
     * The request is well-formed and this silicon cannot honour it. Currently returned
     * for exactly one input: clock_source == NORA_TICK_TIMER_CLOCK_FRC.
     *
     * WHY, because "the newer part offers fewer options" is the natural suspicion and it
     * is not what is happening -- dsPIC33AK's Timer1 has NO clock selection of its own
     * to offer. Measured against DS70005591C section 26:
     *
     *   Table 26-1 (Timer Summary)  clock source = Standard Speed Peripheral Clock
     *                              (System Clock/2)
     *   Table 26-2 (TCS)           1 = external clock source, 0 = peripheral clock.
     *                              TWO values, no FRC among them
     *   Figure 26-1 (block dgm)    the mux has two inputs: TxCK (PPS) and System Clock
     *   T1CON bit map              15:8 = ON/SIDL/TMWDIS/TMWIP/PRWIP,
     *                              7:0 = TGATE/TCKPS[1:0]/TSYNC/TCS.
     *                              Bits 9:8 are not documented -- there is no TECS
     *
     * dsPIC33CK's T1CON does have a real TECS, and 0b11 selects FRC; that is the whole
     * of the difference. AK's DFP header declares `uint8_t TECS:2` anyway, and its ATDF
     * value group holds a single entry (0x0 = T1CK) -- a 2-bit field with one documented
     * value, absent from the data sheet's own register map, is a header leftover. DO NOT
     * implement this against that bitfield.
     *
     * A caller who needs an FRC-clocked tick on AK has two routes, and neither is a
     * tick-timer operation:
     *
     *  1. Re-clock the part. nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC,
     *     NORA_CLOCK_FRC_HZ) makes the peripheral clock 4 MHz, and Timer1 then runs off
     *     FRC through System Clock/2 -- pass timer_clk_hz = NORA_CLOCK_FRC_HZ / 2, not
     *     NORA_CLOCK_FRC_HZ. This is a whole-chip decision, which is why
     *     nora_tick_timer_init() must not make it.
     *  2. Emit FRC on a pin from a CLKGEN and return it to TxCK through PPS (TCS = 1).
     *     Costs a pin and a board trace, so it is a board decision. NOT VERIFIED: which
     *     CLKGEN can emit FRC, and which TxCK-capable RP can receive it, are unchecked.
     *
     * There is NO CLKGEN in Timer1's own path -- route 1 works by moving the system
     * clock, not by aiming a generator at the timer. (Claiming otherwise was a wrong
     * answer given once during this investigation.)
     */
    NORA_TICK_TIMER_ERR_NOT_SUPPORTED
} nora_tick_timer_status_t;

/* Recommended CPU interrupt priority for the 1 ms Timer1 tick. Applications may
 * supply a different non-zero priority when their interrupt ordering requires
 * it. */
#define NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY   4u

/* Which clock family feeds the tick. Portable across the NORA families so that a
 * dsPIC33CK consumer's configuration still compiles here; see ERR_NOT_SUPPORTED for what
 * this backend can actually select. */
typedef enum {
    NORA_TICK_TIMER_CLOCK_INTERNAL = 0,
    NORA_TICK_TIMER_CLOCK_FRC
} nora_tick_timer_clock_source_t;

typedef struct {
    /* Actual input clock supplied to Timer1, in Hz. */
    uint32_t timer_clk_hz;

    /* Timer1 clock source. This backend supports NORA_TICK_TIMER_CLOCK_INTERNAL only;
     * NORA_TICK_TIMER_CLOCK_FRC returns NORA_TICK_TIMER_ERR_NOT_SUPPORTED, which is
     * documented at that enumerator. Zero-initialised configs get CLOCK_INTERNAL. */
    nora_tick_timer_clock_source_t clock_source;

    /* CPU interrupt priority for the Timer1 tick. Valid range: 1..7. */
    uint8_t irq_priority;

    /* Keep Timer1 running while the CPU is in Idle mode. */
    bool run_in_idle;
} nora_tick_timer_config_t;

/* Start Timer1 with a periodic interrupt. Call once after the clock is up. */
nora_tick_timer_status_t nora_tick_timer_init(
    const nora_tick_timer_config_t *config);

nora_tick_timer_status_t nora_tick_timer_deinit(void);

bool nora_tick_timer_is_present(void);

/* Milliseconds elapsed since init. Monotonic; wraps after ~49 days at 1 kHz.
 * Returns 0 before successful init and after deinit. */
uint32_t nora_tick_timer_get_ms(void);

/* True after successful initialization. */
bool nora_tick_timer_is_initialized(void);

void nora_tick_timer_irq_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* NORA_TICK_TIMER_H */
