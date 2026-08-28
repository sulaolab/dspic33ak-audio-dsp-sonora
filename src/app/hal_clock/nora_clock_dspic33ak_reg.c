#include "nora_clock_dspic33ak_reg.h"

#include <xc.h>

#define DSPIC33AK_CLOCK_POLL_LIMIT (1000000UL)

/*
 * A stalled phase is reported as the portable NORA_CLOCK_ERR_TIMEOUT plus a
 * backend diagnostic code naming the phase.  The phase itself is not a portable
 * status: "the fractional divider switch did not complete" has no meaning on a
 * part without that switch, and declaring one status value per phase produced ten
 * values of which seven were never returned.
 */
#define DSPIC33AK_CLOCK_WAIT_CLEAR(EXPR, DIAG)            \
    do {                                                  \
        uint32_t poll_count = DSPIC33AK_CLOCK_POLL_LIMIT; \
        while ((EXPR) != 0u) {                            \
            if (--poll_count == 0u) {                     \
                dspic33ak_clock_diag_set(DIAG);           \
                return NORA_CLOCK_ERR_TIMEOUT;            \
            }                                             \
        }                                                 \
    } while (0)

#define DSPIC33AK_CLOCK_WAIT_SET(EXPR, DIAG)              \
    do {                                                  \
        uint32_t poll_count = DSPIC33AK_CLOCK_POLL_LIMIT; \
        while ((EXPR) == 0u) {                            \
            if (--poll_count == 0u) {                     \
                dspic33ak_clock_diag_set(DIAG);           \
                return NORA_CLOCK_ERR_TIMEOUT;            \
            }                                             \
        }                                                 \
    } while (0)

/*
 * The general CLKGEN sequence: disable the generator, point it at the new source,
 * re-enable, then commit divider and source.
 *
 * Clearing ON first is correct for a generator that feeds a peripheral, and wrong
 * for CLKGEN1, which feeds the CPU.  CLKGEN1 goes through
 * dspic33ak_clock_reg_system_switch() instead.
 */
#define DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CONBITS, DIVBITS, CONFIG) \
    do {                                                           \
        (CONBITS).ON = 0;                                          \
        (CONBITS).NOSC = (CONFIG)->source;                         \
        (CONBITS).ON = 1;                                          \
        (CONBITS).OE = 1;                                          \
        (DIVBITS).INTDIV = (CONFIG)->intdiv;                       \
        (DIVBITS).FRACDIV = (CONFIG)->fracdiv;                     \
        (CONBITS).DIVSWEN = 1;                                     \
        DSPIC33AK_CLOCK_WAIT_CLEAR((CONBITS).DIVSWEN,              \
            NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT);         \
        (CONBITS).OSWEN = 1;                                       \
        DSPIC33AK_CLOCK_WAIT_CLEAR((CONBITS).OSWEN,                \
            NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT);      \
        DSPIC33AK_CLOCK_WAIT_SET((CONBITS).CLKRDY,                 \
            NORA_CLOCK_DSPIC33AK_DIAG_CLKRDY_TIMEOUT);             \
    } while (0)

/* --------------------------------------------------------------------------
 * Local helper prototypes
 * -------------------------------------------------------------------------- */

static nora_clock_status_t configure_pll1(
    const dspic33ak_clock_reg_pll_config_t *config);
static nora_clock_status_t configure_pll2(
    const dspic33ak_clock_reg_pll_config_t *config);

/* -------------------------------------------------------------------------- */
/* Configure PLL register block                                               */
/* -------------------------------------------------------------------------- */
nora_clock_status_t dspic33ak_clock_reg_pll_configure(
    nora_clock_pll_t pll,
    const dspic33ak_clock_reg_pll_config_t *config)
{
    if (config == 0) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    switch (pll) {
    case NORA_CLOCK_PLL_1:
        return configure_pll1(config);
    case NORA_CLOCK_PLL_2:
        return configure_pll2(config);
    default:
        return NORA_CLOCK_ERR_INVALID_ARG;
    }
}

/* -------------------------------------------------------------------------- */
/* Configure CLKGEN register block                                            */
/* -------------------------------------------------------------------------- */
nora_clock_status_t dspic33ak_clock_reg_clkgen_configure(
    nora_clock_dspic33ak_clkgen_t clkgen,
    const dspic33ak_clock_reg_clkgen_config_t *config)
{
    if (config == 0) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    switch (clkgen) {
    case NORA_CLOCK_DSPIC33AK_CLKGEN_1:
        /* Not the general sequence: this generator is clocking the caller. */
        return dspic33ak_clock_reg_system_switch(config);
    case NORA_CLOCK_DSPIC33AK_CLKGEN_5:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK5CONbits, CLK5DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_6:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK6CONbits, CLK6DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_8:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK8CONbits, CLK8DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_9:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK9CONbits, CLK9DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_10:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK10CONbits, CLK10DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_12:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK12CONbits, CLK12DIVbits, config);
        return NORA_CLOCK_OK;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_13:
        DSPIC33AK_CLOCK_CONFIGURE_CLKGEN(CLK13CONbits, CLK13DIVbits, config);
        return NORA_CLOCK_OK;
    default:
        return NORA_CLOCK_ERR_INVALID_ARG;
    }
}

/* -------------------------------------------------------------------------- */
/* Re-source the system clock generator (CLKGEN1)                             */
/* -------------------------------------------------------------------------- */
nora_clock_status_t dspic33ak_clock_reg_system_switch(
    const dspic33ak_clock_reg_clkgen_config_t *config)
{
    nora_clock_status_t status;

    if (config == 0) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /*
     * Divider first, then source -- the order this function has always used and
     * the one that is hardware-verified.  It is also why the two halves are
     * separate entry points now: this order applies the new divider to the OLD
     * source, which is safe when the new source is no faster (every case in this
     * project) and wrong for a caller that raises the source frequency while
     * lowering the divider.  Such a caller steps through the two primitives in
     * the order its transition needs rather than have this sequence guess.
     */
    status = dspic33ak_clock_reg_system_set_divider(config->intdiv,
        config->fracdiv);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    return dspic33ak_clock_reg_system_switch_source(config->source);
}

/* -------------------------------------------------------------------------- */
/* Re-source the system clock generator, leaving its divider alone            */
/* -------------------------------------------------------------------------- */
/*
 * ON is never cleared here.  CLKGEN1's output is the system clock, so it is
 * clocking the CPU that is executing this function: DS70005591C 13.4.2 re-sources a
 * live generator by committing the source (OSWEN) with the generator left enabled
 * throughout.
 *
 * The general CLKGEN sequence drops ON first, which is right for a generator
 * nothing is executing from and fatal for this one.  That is measured, not
 * inferred: a live CLKGEN1 re-source through the general macro was verified to hang
 * the CPU (the Q27C note in src/board/clock/sonora_clock.c), and the resident
 * engine's boot platform hand-rolled exactly this order rather than call this HAL,
 * for the same reason.
 *
 * CLK1DIV is not written at all.  A source switch that also re-divided the CPU
 * clock was the hidden half of nora_clock_switch_source(); the divider is now only
 * ever changed by a caller that asked for it.
 */
nora_clock_status_t dspic33ak_clock_reg_system_switch_source(uint16_t source)
{
    /* Not named poll_count: DSPIC33AK_CLOCK_WAIT_CLEAR declares one of its own in
     * its block, and a same-named local here would shadow it (MSVC C4456; XC-DSC
     * is silent about it).  The two counters are independent waits either way. */
    uint32_t ready_polls;

    CLK1CONbits.ON = 1;
    CLK1CONbits.OE = 1;

    CLK1CONbits.NOSC = source;
    CLK1CONbits.OSWEN = 1;
    DSPIC33AK_CLOCK_WAIT_CLEAR(CLK1CONbits.OSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT);

    /*
     * Completed *and* took effect.  OSWEN clearing says the sequencer finished; it
     * does not by itself say the requested source is the one now selected, and
     * returning OK on a switch that did not take would leave every frequency
     * derived from this clock silently wrong.  Both conditions are polled together
     * so the failure is one status rather than a race between two waits.
     */
    ready_polls = DSPIC33AK_CLOCK_POLL_LIMIT;
    while ((CLK1CONbits.CLKRDY == 0u) ||
           ((uint16_t)CLK1CONbits.COSC != source)) {
        if (--ready_polls == 0u) {
            dspic33ak_clock_diag_set(NORA_CLOCK_DSPIC33AK_DIAG_CLKRDY_TIMEOUT);
            return NORA_CLOCK_ERR_TIMEOUT;
        }
    }

    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* Re-divide the system clock generator, leaving its source alone             */
/* -------------------------------------------------------------------------- */
/*
 * The other half.  Same reason ON stays set, and the same DIVSWEN commit the
 * combined sequence always performed -- extracted rather than reimplemented, so
 * there is one copy of the live-generator rules.
 */
nora_clock_status_t dspic33ak_clock_reg_system_set_divider(
    uint16_t intdiv,
    uint16_t fracdiv)
{
    CLK1CONbits.ON = 1;
    CLK1CONbits.OE = 1;

    CLK1DIVbits.INTDIV = intdiv;
    CLK1DIVbits.FRACDIV = fracdiv;
    CLK1CONbits.DIVSWEN = 1;
    DSPIC33AK_CLOCK_WAIT_CLEAR(CLK1CONbits.DIVSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT);

    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* Read the system clock state                                                */
/* -------------------------------------------------------------------------- */
void dspic33ak_clock_reg_read_system(dspic33ak_clock_reg_system_t *out)
{
    if (out == 0) {
        return;
    }

    out->source = (uint16_t)CLK1CONbits.COSC;
    out->intdiv = (uint16_t)CLK1DIVbits.INTDIV;
    out->fracdiv = (uint16_t)CLK1DIVbits.FRACDIV;
    out->ready = (CLK1CONbits.CLKRDY != 0u);
    out->pll1_ready = (OSCCTRLbits.PLL1RDY != 0u);
    out->pll2_ready = (OSCCTRLbits.PLL2RDY != 0u);
}

/* -------------------------------------------------------------------------- */
/* Read one PLL's configuration                                               */
/* -------------------------------------------------------------------------- */
void dspic33ak_clock_reg_read_pll(
    nora_clock_pll_t pll,
    dspic33ak_clock_reg_pll_state_t *out)
{
    if (out == 0) {
        return;
    }

    out->source = 0u;
    out->feedback_div = 0u;
    out->pre_div = 0u;
    out->post_div1 = 0u;
    out->post_div2 = 0u;
    out->enabled = false;
    out->ready = false;

    switch (pll) {
    case NORA_CLOCK_PLL_1:
        out->source = (uint16_t)PLL1CONbits.NOSC;
        out->feedback_div = (uint32_t)PLL1DIVbits.PLLFBDIV;
        out->pre_div = (uint16_t)PLL1DIVbits.PLLPRE;
        out->post_div1 = (uint16_t)PLL1DIVbits.POSTDIV1;
        out->post_div2 = (uint16_t)PLL1DIVbits.POSTDIV2;
        out->enabled = (PLL1CONbits.ON != 0u);
        out->ready = (OSCCTRLbits.PLL1RDY != 0u);
        break;
    case NORA_CLOCK_PLL_2:
        out->source = (uint16_t)PLL2CONbits.NOSC;
        out->feedback_div = (uint32_t)PLL2DIVbits.PLLFBDIV;
        out->pre_div = (uint16_t)PLL2DIVbits.PLLPRE;
        out->post_div1 = (uint16_t)PLL2DIVbits.POSTDIV1;
        out->post_div2 = (uint16_t)PLL2DIVbits.POSTDIV2;
        out->enabled = (PLL2CONbits.ON != 0u);
        out->ready = (OSCCTRLbits.PLL2RDY != 0u);
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Register capture (public AK API; defined here because it is all SFR reads)  */
/* -------------------------------------------------------------------------- */
void nora_clock_dspic33ak_raw_capture(nora_clock_dspic33ak_raw_t *out)
{
    if (out == 0) {
        return;
    }

    out->oscctrl = (uint32_t)OSCCTRL;
    out->clkfail = (uint32_t)CLKFAIL;
    out->pll1con = (uint32_t)PLL1CON;
    out->pll1div = (uint32_t)PLL1DIV;
    out->pll2con = (uint32_t)PLL2CON;
    out->pll2div = (uint32_t)PLL2DIV;
}

void nora_clock_dspic33ak_clkgen_raw_capture(
    nora_clock_dspic33ak_clkgen_t clkgen,
    nora_clock_dspic33ak_clkgen_raw_t *out)
{
    if (out == 0) {
        return;
    }

    /* An unknown generator reports zeros rather than a stale or invented value:
     * this is a record, and a record that could not be taken must not look
     * taken. */
    out->con = 0u;
    out->div = 0u;

    switch (clkgen) {
    case NORA_CLOCK_DSPIC33AK_CLKGEN_1:
        out->con = (uint32_t)CLK1CON;
        out->div = (uint32_t)CLK1DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_5:
        out->con = (uint32_t)CLK5CON;
        out->div = (uint32_t)CLK5DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_6:
        out->con = (uint32_t)CLK6CON;
        out->div = (uint32_t)CLK6DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_8:
        out->con = (uint32_t)CLK8CON;
        out->div = (uint32_t)CLK8DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_9:
        out->con = (uint32_t)CLK9CON;
        out->div = (uint32_t)CLK9DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_10:
        out->con = (uint32_t)CLK10CON;
        out->div = (uint32_t)CLK10DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_12:
        out->con = (uint32_t)CLK12CON;
        out->div = (uint32_t)CLK12DIV;
        break;
    case NORA_CLOCK_DSPIC33AK_CLKGEN_13:
        out->con = (uint32_t)CLK13CON;
        out->div = (uint32_t)CLK13DIV;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Configure PLL1                                                             */
/* -------------------------------------------------------------------------- */
static nora_clock_status_t configure_pll1(
    const dspic33ak_clock_reg_pll_config_t *config)
{
    uint32_t start_poll;

    PLL1CONbits.ON = 0;

    /* POSTDIV1/2 must not be changed while the PLL is operating. Stage and
     * commit all normal PLL dividers while ON is clear, then enable the analog
     * generator only after the inherited ASRC configuration is replaced. */
    PLL1DIVbits.PLLFBDIV = config->feedback_div;
    PLL1DIVbits.PLLPRE = config->pre_div;
    PLL1DIVbits.POSTDIV1 = config->post_div1;
    PLL1DIVbits.POSTDIV2 = config->post_div2;

    /* ON starts an internal DIVSWEN asynchronously on this silicon. Do not
     * issue PLLSWEN behind that transfer: first allow the ON-started divider
     * cycle to become visible and finish. A cold path may not assert DIVSWEN,
     * so absence during the short observation window is also valid. */
    PLL1CONbits.ON = 1u;
    PLL1CONbits.OE = 1u;
    start_poll = 10000u;
    while ((PLL1CONbits.DIVSWEN == 0u) && (--start_poll != 0u)) {
    }
    if (PLL1CONbits.DIVSWEN != 0u) {
        DSPIC33AK_CLOCK_WAIT_CLEAR(PLL1CONbits.DIVSWEN,
            NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT);
    }

    PLL1CONbits.PLLSWEN = 1u;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL1CONbits.PLLSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_PLL_SWITCH_TIMEOUT);

    PLL1CONbits.FOUTSWEN = 1u;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL1CONbits.FOUTSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_FOUT_SWITCH_TIMEOUT);

    PLL1CONbits.NOSC = config->source;
    PLL1CONbits.OSWEN = 1u;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL1CONbits.OSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT);
    DSPIC33AK_CLOCK_WAIT_SET(OSCCTRLbits.PLL1RDY,
        NORA_CLOCK_DSPIC33AK_DIAG_PLL_LOCK_TIMEOUT);

    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* Configure PLL2                                                             */
/* -------------------------------------------------------------------------- */
static nora_clock_status_t configure_pll2(
    const dspic33ak_clock_reg_pll_config_t *config)
{
    PLL2CONbits.ON = 1;
    PLL2CONbits.OE = 1;

    PLL2DIVbits.PLLFBDIV = config->feedback_div;
    PLL2DIVbits.PLLPRE = config->pre_div;
    PLL2DIVbits.POSTDIV1 = config->post_div1;
    PLL2DIVbits.POSTDIV2 = config->post_div2;

    PLL2CONbits.PLLSWEN = 1;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL2CONbits.PLLSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_PLL_SWITCH_TIMEOUT);

    PLL2CONbits.FOUTSWEN = 1;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL2CONbits.FOUTSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_FOUT_SWITCH_TIMEOUT);

    PLL2CONbits.NOSC = config->source;
    PLL2CONbits.OSWEN = 1;
    DSPIC33AK_CLOCK_WAIT_CLEAR(PLL2CONbits.OSWEN,
        NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT);
    DSPIC33AK_CLOCK_WAIT_SET(OSCCTRLbits.PLL2RDY,
        NORA_CLOCK_DSPIC33AK_DIAG_PLL_LOCK_TIMEOUT);

    return NORA_CLOCK_OK;
}
