/*
 * nora_tick_timer_dspic33ak.c
 * ----------------------
 * 1 ms time base on Timer1. See nora_tick_timer.h.
 *
 * Timer1 uses a caller-supplied input clock. The HAL selects the smallest
 * available prescaler that can generate an EXACT 1 ms period within the 32-bit
 * PR1 range, then the interrupt handler increments a 32-bit counter. A clock
 * that cannot produce 1.000 ms is refused, not rounded -- see
 * NORA_TICK_TIMER_ERR_INEXACT_PERIOD in the header.
 */

#include "nora_tick_timer.h"

#include <xc.h>

#define DSPIC33AK_TICK_TIMER_HZ             1000u
#define DSPIC33AK_TICK_TIMER_MAX_PRIORITY   7u

#if defined(T1CON) && defined(TMR1) && defined(PR1) && \
    defined(_T1IF) && defined(_T1IE) && defined(_T1IP)
#define DSPIC33AK_TICK_TIMER_PRESENT         1
#else
#define DSPIC33AK_TICK_TIMER_PRESENT         0
#endif

typedef struct {
    uint16_t divisor;
    uint8_t tckps;
} prescaler_option_t;

static const prescaler_option_t prescaler_options[] = {
    { 1u,   0b00u },
    { 8u,   0b01u },
    { 64u,  0b10u },
    { 256u, 0b11u },
};

static volatile uint32_t tick_ms = 0u;
static volatile bool tick_initialized = false;

static nora_tick_timer_status_t calc_period_reg(
    const nora_tick_timer_config_t *config,
    uint32_t *period_reg,
    uint8_t *tckps);
static nora_tick_timer_status_t apply_clock_source(
    nora_tick_timer_clock_source_t clock_source);

nora_tick_timer_status_t nora_tick_timer_init(
    const nora_tick_timer_config_t *config)
{
    uint32_t period_reg;
    uint8_t tckps;
    nora_tick_timer_status_t status;

    status = calc_period_reg(config, &period_reg, &tckps);
    if (status != NORA_TICK_TIMER_OK) {
        return status;
    }

    /* Before touching Timer1: an unsupported clock source must leave the timer as it
     * was, so this refusal happens while the peripheral is still untouched. */
    status = apply_clock_source(config->clock_source);
    if (status != NORA_TICK_TIMER_OK) {
        return status;
    }

#if DSPIC33AK_TICK_TIMER_PRESENT
    _T1IE = 0;
    T1CONbits.ON = 0;
    _T1IF = 0;
    tick_initialized = false;

    T1CON = 0u;
    TMR1 = 0;
    PR1 = period_reg;
    T1CONbits.TCS = 0;
    T1CONbits.TCKPS = tckps;
    T1CONbits.SIDL = config->run_in_idle ? 0u : 1u;
    _T1IP = config->irq_priority;
    tick_ms = 0u;
    tick_initialized = true;
    _T1IF = 0;
    _T1IE = 1;
    T1CONbits.ON = 1;

    return NORA_TICK_TIMER_OK;
#else
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}

nora_tick_timer_status_t nora_tick_timer_deinit(void)
{
#if DSPIC33AK_TICK_TIMER_PRESENT
    if (!tick_initialized) {
        return NORA_TICK_TIMER_ERR_NOT_INITIALIZED;
    }

    _T1IE = 0;
    T1CONbits.ON = 0;
    _T1IF = 0;
    TMR1 = 0;
    PR1 = 0;
    T1CON = 0u;
    tick_ms = 0u;
    tick_initialized = false;

    return NORA_TICK_TIMER_OK;
#else
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}

bool nora_tick_timer_is_present(void)
{
#if DSPIC33AK_TICK_TIMER_PRESENT
    return true;
#else
    return false;
#endif
}

uint32_t nora_tick_timer_get_ms(void)
{
    if (!tick_initialized) {
        return 0u;
    }

    return tick_ms;
}

bool nora_tick_timer_is_initialized(void)
{
    return tick_initialized;
}

void nora_tick_timer_irq_handler(void)
{
#if DSPIC33AK_TICK_TIMER_PRESENT
    _T1IF = 0;

    if (tick_initialized) {
        tick_ms++;
    }
#endif
}

static nora_tick_timer_status_t calc_period_reg(
    const nora_tick_timer_config_t *config,
    uint32_t *period_reg,
    uint8_t *tckps)
{
    uint8_t i;

    if (!nora_tick_timer_is_present()) {
        return NORA_TICK_TIMER_ERR_NOT_PRESENT;
    }

    if ((config == 0) || (period_reg == 0) || (tckps == 0) ||
        (config->timer_clk_hz == 0u) ||
        (config->irq_priority == 0u) ||
        (config->irq_priority > DSPIC33AK_TICK_TIMER_MAX_PRIORITY) ||
        ((config->clock_source != NORA_TICK_TIMER_CLOCK_INTERNAL) &&
         (config->clock_source != NORA_TICK_TIMER_CLOCK_FRC))) {
        return NORA_TICK_TIMER_ERR_INVALID_ARG;
    }

    /*
     * EXACT DIVISORS ONLY. Two independent reasons to skip a prescaler:
     *
     *   remainder      -- this divisor cannot produce 1.000 ms at all
     *   count overflow -- it can, but PR1 is 32 bits; a LARGER prescaler gives a smaller
     *                     count, so the ones left to try are exactly the ones that help
     *
     * The overflow case is why the loop exists. Neither case may return early, and the flag
     * has to record the EXACT case, not the inexact one -- the two walls are not symmetric:
     *
     *   a remainder at divisor d implies a remainder at every LATER one (1|8|64|256), so
     *   seeing one says nothing about the divisors already tried;
     *   an overflow at d says nothing about later ones, which is what the loop continues for.
     *
     * So "some divisor divided exactly, none of them fitted" is out-of-range, and only "no
     * divisor divided exactly at all" is inexact -- which is what the header says each name
     * means.
     *
     * THIS USED TO ROUND -- `(clk + denominator/2) / denominator`, no remainder check,
     * OK returned. Harmless on both current callers (100 MHz / 1 / 1000 = 100,000 and
     * 4 MHz / 1 / 1000 = 4,000, both exact) but it made the header's contract false, and
     * that contract is load-bearing: every cadence in the tree is milliseconds of this
     * tick. See ERR_INEXACT_PERIOD in the header. dsPIC33CK fixed the same defect on
     * 2026-08-03; the logic below is that one, ported to a 32-bit PR1.
     */
    {
        bool saw_exact = false;

        for (i = 0u;
             i < (uint8_t)(sizeof(prescaler_options) / sizeof(prescaler_options[0]));
             i++) {
            const uint64_t denominator =
                (uint64_t)prescaler_options[i].divisor * DSPIC33AK_TICK_TIMER_HZ;
            uint64_t counts;

            if (((uint64_t)config->timer_clk_hz % denominator) != 0u) {
                continue;
            }

            counts = (uint64_t)config->timer_clk_hz / denominator;

            if (counts == 0u) {
                continue;
            }

            saw_exact = true;

            if ((counts - 1u) <= UINT32_MAX) {
                *period_reg = (uint32_t)(counts - 1u);
                *tckps = prescaler_options[i].tckps;
                return NORA_TICK_TIMER_OK;
            }
        }

        /* Nothing fitted. Say WHICH wall was hit: out-of-range means the period IS exact
         * but this timer's divisors cannot bring the count inside 32 bits, while inexact
         * is a design error the caller must fix at the clock. */
        return saw_exact ? NORA_TICK_TIMER_ERR_OUT_OF_RANGE
                         : NORA_TICK_TIMER_ERR_INEXACT_PERIOD;
    }
}

/*
 * Named to match the dsPIC33CK backend's apply_clock_source() so the two files stay
 * diffable, but on this part there is nothing to program: Timer1's mux offers the
 * peripheral clock (TCS = 0, written unconditionally by nora_tick_timer_init()) and an
 * external TxCK pin, and no FRC input. So this function only DECIDES, and the caller
 * runs it before touching the peripheral -- a refused init leaves Timer1 as it was.
 *
 * The full data-sheet evidence for the refusal, and the two board/system-level routes to
 * an FRC-clocked tick, are at NORA_TICK_TIMER_ERR_NOT_SUPPORTED in nora_tick_timer.h.
 * They belong in the header because the reader who needs them is the caller who just got
 * the status back.
 */
static nora_tick_timer_status_t apply_clock_source(
    nora_tick_timer_clock_source_t clock_source)
{
#if DSPIC33AK_TICK_TIMER_PRESENT
    switch (clock_source) {
    case NORA_TICK_TIMER_CLOCK_INTERNAL:
        break;

    case NORA_TICK_TIMER_CLOCK_FRC:
        return NORA_TICK_TIMER_ERR_NOT_SUPPORTED;

    default:
        return NORA_TICK_TIMER_ERR_INVALID_ARG;
    }

    return NORA_TICK_TIMER_OK;
#else
    (void)clock_source;
    return NORA_TICK_TIMER_ERR_NOT_PRESENT;
#endif
}
