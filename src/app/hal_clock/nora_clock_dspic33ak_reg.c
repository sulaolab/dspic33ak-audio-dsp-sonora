#include "nora_clock_dspic33ak_reg.h"

#include <xc.h>

#define DSPIC33AK_CLOCK_POLL_LIMIT (1000000UL)

/*
 * Budget for the two OBSERVATION windows in the PLL sequence, as opposed to the
 * timeout budget above.  Deliberately three orders of magnitude smaller, because
 * the two are not the same kind of wait: a timeout budget is only ever reached
 * when the hardware is broken, so it can be enormous, while an observation window
 * has "it never happened" as a VALID outcome and is therefore paid in full on
 * every single configure.  A million iterations there would be a real stall added
 * to every boot.
 */
#define DSPIC33AK_CLOCK_OBSERVE_POLLS (10000UL)

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

/* -------------------------------------------------------------------------- */
/* One PLL, described                                                         */
/* -------------------------------------------------------------------------- */
/*
 * PLL1 and PLL2 are the same hardware block instantiated twice, so the safe
 * programming sequence is written once, below, and the per-instance parts are
 * data: one table of operations per PLL.
 *
 * Every operation names its OWN DFP type -- pll1_* touch PLL1CONbits/PLL1DIVbits
 * and pll2_* touch PLL2CONbits/PLL2DIVbits -- so nothing here assumes the two
 * layouts agree. An earlier revision reached PLL2 through PLL1's type with a cast
 * guarded by a sizeof check; that guard was wrong, because equal size does not
 * imply equal field placement, and the whole class of mistake is gone once each
 * instance's fields are named directly.
 *
 * The instances differ in exactly this much:
 *   - which PLLxCON / PLLxDIV the operations touch
 *   - which bit of OSCCTRL reports lock (PLL1RDY b14 / PLL2RDY b15)
 * There is no per-PLL fail or status bit; CLKFAIL is device wide and is captured
 * undecoded. Nothing else differs: not the field set, not the input sources, not
 * the frequency limits. A future real difference belongs in this table, and there
 * is none today -- which is why there is no quirk field to fill in wrongly.
 *
 * The operations are functions and not pointers-to-registers for a second reason
 * as well: the host model of the register file has to observe every access to
 * advance the commit sequencer the wait loops below are written against (see
 * src/tests/hal_clock/fake_xc/nora_fake_clock.h), and an SFR name there expands to
 * a call. On the device each of these is a handful of instructions on a sequence
 * that runs a few times at boot.
 */
typedef enum {
    DSPIC33AK_PLL_COMMIT_DIVSWEN = 0,
    DSPIC33AK_PLL_COMMIT_PLLSWEN,
    DSPIC33AK_PLL_COMMIT_FOUTSWEN,
    DSPIC33AK_PLL_COMMIT_OSWEN
} dspic33ak_pll_commit_t;

typedef struct {
    void     (*enable)(bool on);
    bool     (*enabled)(void);
    /* Writes the four divider fields only; leaves `source` alone. */
    void     (*write_dividers)(const dspic33ak_clock_reg_pll_config_t *config);
    void     (*read_dividers)(dspic33ak_clock_reg_pll_config_t *out);
    void     (*select)(uint16_t nosc);
    uint16_t (*current)(void);      /* COSC -- selected now */
    void     (*commit)(dspic33ak_pll_commit_t which);
    bool     (*pending)(dspic33ak_pll_commit_t which);
    bool     (*clkrdy)(void);       /* PLLxCON.CLKRDY */
    bool     (*locked)(void);       /* OSCCTRL.PLLxRDY */
} dspic33ak_pll_ops_t;

static void pll1_enable(bool on)
{
    PLL1CONbits.ON = on ? 1u : 0u;
}

static bool pll1_enabled(void)
{
    return PLL1CONbits.ON != 0u;
}

static void pll1_write_dividers(const dspic33ak_clock_reg_pll_config_t *config)
{
    PLL1DIVbits.PLLFBDIV = config->feedback_div;
    PLL1DIVbits.PLLPRE = config->pre_div;
    PLL1DIVbits.POSTDIV1 = config->post_div1;
    PLL1DIVbits.POSTDIV2 = config->post_div2;
}

static void pll1_read_dividers(dspic33ak_clock_reg_pll_config_t *out)
{
    out->feedback_div = (uint32_t)PLL1DIVbits.PLLFBDIV;
    out->pre_div = (uint16_t)PLL1DIVbits.PLLPRE;
    out->post_div1 = (uint16_t)PLL1DIVbits.POSTDIV1;
    out->post_div2 = (uint16_t)PLL1DIVbits.POSTDIV2;
}

static void pll1_select(uint16_t nosc)
{
    PLL1CONbits.NOSC = (uint8_t)nosc;
}

static uint16_t pll1_current(void)
{
    return (uint16_t)PLL1CONbits.COSC;
}

static void pll1_commit(dspic33ak_pll_commit_t which)
{
    switch (which) {
    case DSPIC33AK_PLL_COMMIT_DIVSWEN:
        PLL1CONbits.DIVSWEN = 1u;
        break;
    case DSPIC33AK_PLL_COMMIT_PLLSWEN:
        PLL1CONbits.PLLSWEN = 1u;
        break;
    case DSPIC33AK_PLL_COMMIT_FOUTSWEN:
        PLL1CONbits.FOUTSWEN = 1u;
        break;
    default:
        PLL1CONbits.OSWEN = 1u;
        break;
    }
}

static bool pll1_pending(dspic33ak_pll_commit_t which)
{
    switch (which) {
    case DSPIC33AK_PLL_COMMIT_DIVSWEN:
        return PLL1CONbits.DIVSWEN != 0u;
    case DSPIC33AK_PLL_COMMIT_PLLSWEN:
        return PLL1CONbits.PLLSWEN != 0u;
    case DSPIC33AK_PLL_COMMIT_FOUTSWEN:
        return PLL1CONbits.FOUTSWEN != 0u;
    default:
        return PLL1CONbits.OSWEN != 0u;
    }
}

static bool pll1_clkrdy(void)
{
    return PLL1CONbits.CLKRDY != 0u;
}

static bool pll1_locked(void)
{
    return OSCCTRLbits.PLL1RDY != 0u;
}

static void pll2_enable(bool on)
{
    PLL2CONbits.ON = on ? 1u : 0u;
}

static bool pll2_enabled(void)
{
    return PLL2CONbits.ON != 0u;
}

static void pll2_write_dividers(const dspic33ak_clock_reg_pll_config_t *config)
{
    PLL2DIVbits.PLLFBDIV = config->feedback_div;
    PLL2DIVbits.PLLPRE = config->pre_div;
    PLL2DIVbits.POSTDIV1 = config->post_div1;
    PLL2DIVbits.POSTDIV2 = config->post_div2;
}

static void pll2_read_dividers(dspic33ak_clock_reg_pll_config_t *out)
{
    out->feedback_div = (uint32_t)PLL2DIVbits.PLLFBDIV;
    out->pre_div = (uint16_t)PLL2DIVbits.PLLPRE;
    out->post_div1 = (uint16_t)PLL2DIVbits.POSTDIV1;
    out->post_div2 = (uint16_t)PLL2DIVbits.POSTDIV2;
}

static void pll2_select(uint16_t nosc)
{
    PLL2CONbits.NOSC = (uint8_t)nosc;
}

static uint16_t pll2_current(void)
{
    return (uint16_t)PLL2CONbits.COSC;
}

static void pll2_commit(dspic33ak_pll_commit_t which)
{
    switch (which) {
    case DSPIC33AK_PLL_COMMIT_DIVSWEN:
        PLL2CONbits.DIVSWEN = 1u;
        break;
    case DSPIC33AK_PLL_COMMIT_PLLSWEN:
        PLL2CONbits.PLLSWEN = 1u;
        break;
    case DSPIC33AK_PLL_COMMIT_FOUTSWEN:
        PLL2CONbits.FOUTSWEN = 1u;
        break;
    default:
        PLL2CONbits.OSWEN = 1u;
        break;
    }
}

static bool pll2_pending(dspic33ak_pll_commit_t which)
{
    switch (which) {
    case DSPIC33AK_PLL_COMMIT_DIVSWEN:
        return PLL2CONbits.DIVSWEN != 0u;
    case DSPIC33AK_PLL_COMMIT_PLLSWEN:
        return PLL2CONbits.PLLSWEN != 0u;
    case DSPIC33AK_PLL_COMMIT_FOUTSWEN:
        return PLL2CONbits.FOUTSWEN != 0u;
    default:
        return PLL2CONbits.OSWEN != 0u;
    }
}

static bool pll2_clkrdy(void)
{
    return PLL2CONbits.CLKRDY != 0u;
}

static bool pll2_locked(void)
{
    return OSCCTRLbits.PLL2RDY != 0u;
}

static const dspic33ak_pll_ops_t s_pll1_ops = {
    pll1_enable, pll1_enabled, pll1_write_dividers, pll1_read_dividers,
    pll1_select, pll1_current, pll1_commit, pll1_pending, pll1_clkrdy,
    pll1_locked
};

static const dspic33ak_pll_ops_t s_pll2_ops = {
    pll2_enable, pll2_enabled, pll2_write_dividers, pll2_read_dividers,
    pll2_select, pll2_current, pll2_commit, pll2_pending, pll2_clkrdy,
    pll2_locked
};

/*
 * The single place a PLL number turns into registers, so a caller that reads a
 * PLL and a caller that programs one cannot disagree about which registers that
 * PLL is.  Null for an instance this part does not have.
 */
static const dspic33ak_pll_ops_t *dspic33ak_pll_ops(nora_clock_pll_t pll)
{
    switch (pll) {
    case NORA_CLOCK_PLL_1:
        return &s_pll1_ops;
    case NORA_CLOCK_PLL_2:
        return &s_pll2_ops;
    default:
        return 0;
    }
}

static nora_clock_status_t dspic33ak_pll_program(
    const dspic33ak_pll_ops_t *ops,
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

    {
        const dspic33ak_pll_ops_t *ops = dspic33ak_pll_ops(pll);

        if (ops == 0) {
            return NORA_CLOCK_ERR_INVALID_ARG;
        }
        return dspic33ak_pll_program(ops, config);
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
 * the CPU (the Q27C note in the board clock bring-up), and the resident
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

    {
        const dspic33ak_pll_ops_t *ops = dspic33ak_pll_ops(pll);
        dspic33ak_clock_reg_pll_config_t dividers;

        /* Same table the programming path uses, so a caller that reads a PLL and
         * a caller that programs one cannot disagree about which registers that
         * PLL is.  An instance this part does not have keeps the zeros set above,
         * so a record that could not be taken does not look taken. */
        if (ops == 0) {
            return;
        }

        ops->read_dividers(&dividers);

        /*
         * COSC, the source now selected -- NOT NOSC, the one last requested.
         * They differ exactly when a switch did not take, and in that case NOSC
         * would have this report the frequency the caller ASKED for while the PLL
         * is still running the old source.  The whole reason this record is read
         * back from registers instead of remembered is so a sequence that failed
         * part way through cannot leave a wish standing as the truth, and NOSC is
         * a wish.
         */
        out->source = ops->current();
        out->feedback_div = dividers.feedback_div;
        out->pre_div = dividers.pre_div;
        out->post_div1 = dividers.post_div1;
        out->post_div2 = dividers.post_div2;
        out->enabled = ops->enabled();
        out->ready = ops->locked();
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
/* Program one PLL -- the whole safe sequence, once                           */
/* -------------------------------------------------------------------------- */
/*
 * Order and every wait in it are the same for both instances.  This replaced two
 * functions that had drifted apart: PLL1's had been corrected after the rev A1
 * measurements and PLL2's still followed DS70005591D Example 12-4, which the data
 * sheet's own section 12.4.6.1 contradicts.  Both orders were measured working on
 * this silicon (3/3 true cold PORs, and 4/4 cold POR on the unified sequence),
 * so this is not a bug fix dressed as a refactor -- it is one sequence instead
 * of two, and the one kept is the one the data sheet's normative text asks for.
 */

/*
 * Is the PLL still running?  Asked of the two lock indications rather than of ON,
 * because ON is a REQUEST: DS70005591D 12.4.1 has the hardware OR it with every
 * consumer's clock request, so the bit can read back exactly as written while the
 * block keeps going.  A PLL that has actually stopped is not locked and does not
 * report CLKRDY, and the host model ties both to ON for the same reason.
 */
static bool pll_is_running(const dspic33ak_pll_ops_t *ops)
{
    return ops->locked() || ops->clkrdy();
}

/* Is it already set up exactly as asked -- all four dividers and the source that
 * is CURRENT, not the one last requested? */
static bool pll_already_configured(const dspic33ak_pll_ops_t *ops,
                                   const dspic33ak_clock_reg_pll_config_t *config)
{
    dspic33ak_clock_reg_pll_config_t now;

    ops->read_dividers(&now);

    return (now.feedback_div == config->feedback_div) &&
           (now.pre_div == config->pre_div) &&
           (now.post_div1 == config->post_div1) &&
           (now.post_div2 == config->post_div2) &&
           (ops->current() == config->source);
}

static nora_clock_status_t dspic33ak_pll_program(
    const dspic33ak_pll_ops_t *ops,
    const dspic33ak_clock_reg_pll_config_t *config)
{
    /* Not named poll_count: DSPIC33AK_CLOCK_WAIT_CLEAR declares one of its own
     * in its block and a same-named local here would shadow it. */
    uint32_t observe;
    uint32_t ready_polls;

    /*
     * Already locked on exactly these settings: nothing to do, and NOTHING IS
     * WRITTEN.  This is the idempotent case and it matters because
     * sonora_clock_pll2_from_refi1() is reached from four separate board entry
     * points that each secure PLL2, so an application using more than one of them
     * asks for the same operating point twice in a boot.  Answering that by
     * taking the PLL out of lock and putting it back would be a glitch on a clock
     * something is already using, for no change.
     */
    if (ops->locked() && ops->clkrdy() && pll_already_configured(ops, config)) {
        /*
         * One thing may still be missing: the PLL can be locked on exactly these
         * settings with ON CLEAR, because a consumer's clock request is the only
         * thing holding it up (DS70005591D 12.4.1 ORs the two).  Take our own hold
         * as well, or the PLL stops the moment that consumer releases it and the
         * caller is left believing it owns a clock that has gone.  Nothing else is
         * touched: no divider, no source, no switch request.
         *
         * With ON already set this writes nothing at all, which is the ordinary
         * repeat-call case.
         */
        if (!ops->enabled()) {
            ops->enable(true);
        }
        return NORA_CLOCK_OK;
    }

    /*
     * Something has to change, so the generator has to stop first: POSTDIV1 and
     * POSTDIV2 must not be changed while the PLL is operating (DS70005591D
     * 12.4.6.1 step 3b) and Note 1 of the same section recommends settling every
     * divider before the first clock switch.  Example 12-4 does neither.
     */
    ops->enable(false);

    observe = DSPIC33AK_CLOCK_OBSERVE_POLLS;
    while (pll_is_running(ops) && (--observe != 0u)) {
    }

    /*
     * It did not stop, so a consumer is still requesting it (12.4.1 again) and it
     * is NOT ours to reprogram.  Refuse with every divider untouched: writing them
     * to a running PLL is the violation this sequence exists to avoid, and doing it
     * anyway would corrupt the clock of whatever is holding it.  Our stop request
     * is taken back first, or it would take effect later when that consumer
     * released the PLL and stop a clock nobody asked to stop.
     *
     * The wanted operating point being the one it is already on is handled above
     * and never reaches here, so a caller re-asking for what it has cannot be
     * refused by this.
     */
    if (pll_is_running(ops)) {
        ops->enable(true);
        dspic33ak_clock_diag_set(NORA_CLOCK_DSPIC33AK_DIAG_PLL_STOP_REFUSED);
        return NORA_CLOCK_ERR_TIMEOUT;
    }

    ops->write_dividers(config);

    ops->enable(true);
    /* No OE write here, and do not add one back: PLLxCON HAS NO OE BIT.
     * Data sheet DS70005591 (rev C and rev D alike) leaves PLLxCON bit 12
     * unnamed, with no bit description; only CLKxCON bit 12 is OE, and it
     * means "clock output is enabled to be an output on a device pin" -- a
     * PLL output never reaches a pin. The DFP declared it anyway up to
     * MC 1.4.172 / MP 1.3.185, copied from the CLKGEN CON template, and
     * MC 1.5.263 / MP 1.4.260 removed it. The write was also measured
     * non-causal for the warm-entry PLL hang (v71-v72), so nothing here
     * depended on it.
     * It was already re-added once by a merge; that is why this note is long. */

    /*
     * Rev A1 quirk, and a block-level one rather than a per-instance one: writing
     * ON starts a DIVSWEN transfer that software never requested.  Let it become
     * visible, then let it finish, so PLLSWEN is not issued behind a transfer
     * already in flight.  A cold path may not assert it at all, so absence within
     * the observation window is equally valid -- which is exactly why the window
     * is DSPIC33AK_CLOCK_OBSERVE_POLLS and not the timeout budget.
     *
     * Measured on both instances: PLL1 during the original A1 investigation, PLL2
     * on 2026-09-13 (`step DIVSWEN ... us=59 OK` in the cold-POR records of the
     * closure doc). The host model asserts it for both slots too.
     */
    observe = DSPIC33AK_CLOCK_OBSERVE_POLLS;
    while (!ops->pending(DSPIC33AK_PLL_COMMIT_DIVSWEN) && (--observe != 0u)) {
    }
    if (ops->pending(DSPIC33AK_PLL_COMMIT_DIVSWEN)) {
        DSPIC33AK_CLOCK_WAIT_CLEAR(ops->pending(DSPIC33AK_PLL_COMMIT_DIVSWEN),
            NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT);
    }

    ops->commit(DSPIC33AK_PLL_COMMIT_PLLSWEN);
    DSPIC33AK_CLOCK_WAIT_CLEAR(ops->pending(DSPIC33AK_PLL_COMMIT_PLLSWEN),
        NORA_CLOCK_DSPIC33AK_DIAG_PLL_SWITCH_TIMEOUT);

    ops->commit(DSPIC33AK_PLL_COMMIT_FOUTSWEN);
    DSPIC33AK_CLOCK_WAIT_CLEAR(ops->pending(DSPIC33AK_PLL_COMMIT_FOUTSWEN),
        NORA_CLOCK_DSPIC33AK_DIAG_FOUT_SWITCH_TIMEOUT);

    ops->select(config->source);
    ops->commit(DSPIC33AK_PLL_COMMIT_OSWEN);
    DSPIC33AK_CLOCK_WAIT_CLEAR(ops->pending(DSPIC33AK_PLL_COMMIT_OSWEN),
        NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT);

    /*
     * Locked, and locked on the source that was asked for.  Three conditions,
     * each ruling out a different lie, polled together so a failure is one status
     * instead of a race between separate waits -- the same shape
     * dspic33ak_clock_reg_system_switch_source() uses for CLKGEN1:
     *
     *   - OSCCTRL's lock bit: the analog loop has settled
     *   - PLLxCON.CLKRDY:     the PLL block itself agrees it is ready
     *   - COSC == source:     the switch selected what was requested, not what
     *                         was already selected
     *
     * All three are per request and self clearing, so none of them can be
     * satisfied by a lock left standing from a previous configure at different
     * dividers.  The old configure_pll2() waited only for OSCCTRL's bit; on a
     * second call that bit was already 1, so it returned OK without proving a
     * single thing about the new dividers.  PLL1 escaped that only as a side
     * effect of clearing ON.
     */
    ready_polls = DSPIC33AK_CLOCK_POLL_LIMIT;
    while ((!ops->locked()) ||
           (!ops->clkrdy()) ||
           (ops->current() != config->source)) {
        if (--ready_polls == 0u) {
            dspic33ak_clock_diag_set(NORA_CLOCK_DSPIC33AK_DIAG_PLL_LOCK_TIMEOUT);
            return NORA_CLOCK_ERR_TIMEOUT;
        }
    }

    return NORA_CLOCK_OK;
}
