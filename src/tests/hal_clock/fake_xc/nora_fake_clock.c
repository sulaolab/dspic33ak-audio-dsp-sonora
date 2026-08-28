#include "nora_fake_clock.h"

#include <string.h>

/*
 * Which generators exist on this part, from nora_clock_dspic33ak.h's clkgen enum:
 * 1, 5, 6, 8, 9, 10, 12, 13.  The arrays are indexed by generator number, so a
 * gap is simply a slot nothing ever reaches -- and a request for a generator this
 * part does not have must not index out of bounds, which is what
 * nora_fake_slot_valid() is for.
 */
#define NORA_FAKE_CLKGEN_SLOTS (14u)
#define NORA_FAKE_PLL_SLOTS    (3u)

typedef struct {
    nora_fake_clkcon_t con;
    nora_fake_clkdiv_t div;
    uint32_t prev_on;
    uint32_t oswen_ticks;
    uint32_t divswen_ticks;
    uint32_t source_switches;
    uint32_t div_switches;
    uint32_t on_clears;
} nora_fake_clkgen_slot_t;

typedef struct {
    nora_fake_pllcon_t con;
    nora_fake_plldiv_t div;
    uint32_t prev_on;
    uint32_t oswen_ticks;
    uint32_t divswen_ticks;
    uint32_t pllswen_ticks;
    uint32_t foutswen_ticks;
    uint32_t source_switches;
    uint32_t on_clears;
} nora_fake_pll_slot_t;

static nora_fake_clkgen_slot_t s_clkgen[NORA_FAKE_CLKGEN_SLOTS];
static nora_fake_pll_slot_t s_pll[NORA_FAKE_PLL_SLOTS];
static nora_fake_oscctrl_t s_oscctrl;

/* A slot to absorb an out-of-range access rather than corrupt memory.  Reaching
 * it is itself a finding, but a crash would say less than a failed assertion. */
static nora_fake_clkgen_slot_t s_clkgen_void;
static nora_fake_pll_slot_t s_pll_void;

static void nora_fake_clock_tick(void);

/* --------------------------------------------------------------------------
 * Sequencer
 * -------------------------------------------------------------------------- */

/*
 * One commit bit, advanced by one observation.  Returns nonzero on the tick that
 * completes it, so the caller can apply that bit's side effects.
 */
static int nora_fake_advance(uint32_t *bit, uint32_t *ticks)
{
    if (*bit == 0u) {
        *ticks = 0u;
        return 0;
    }

    (*ticks)++;
    if (*ticks < NORA_FAKE_SEQUENCER_TICKS) {
        return 0;
    }

    *bit = 0u;
    *ticks = 0u;
    return 1;
}

static void nora_fake_tick_clkgen(nora_fake_clkgen_slot_t *slot)
{
    if ((slot->prev_on != 0u) && (slot->con.ON == 0u)) {
        slot->on_clears++;
    }
    slot->prev_on = slot->con.ON;

    if (nora_fake_advance(&slot->con.DIVSWEN, &slot->divswen_ticks) != 0) {
        slot->div_switches++;
    }

    if (nora_fake_advance(&slot->con.OSWEN, &slot->oswen_ticks) != 0) {
        /* The switch took: the selection becomes the current one and the
         * generator reports ready.  Both are what the reg layer's combined
         * "CLKRDY && COSC == source" poll is written against. */
        slot->con.COSC = slot->con.NOSC;
        slot->con.CLKRDY = 1u;
        slot->source_switches++;
    }
}

static void nora_fake_tick_pll(nora_fake_pll_slot_t *slot, uint32_t *rdy)
{
    if ((slot->prev_on == 0u) && (slot->con.ON != 0u)) {
        /*
         * "ON starts an internal DIVSWEN asynchronously on this silicon" --
         * configure_pll1() in the reg layer observes for it and tolerates its
         * absence.  Modelling the presence exercises the branch that waits.
         */
        slot->con.DIVSWEN = 1u;
        slot->divswen_ticks = 0u;
    }
    if ((slot->prev_on != 0u) && (slot->con.ON == 0u)) {
        slot->on_clears++;
        /* Out of lock: a disabled PLL is not ready.  Without this the closing
         * WAIT_SET(PLLxRDY) would pass on a stale 1 and prove nothing. */
        *rdy = 0u;
    }
    slot->prev_on = slot->con.ON;

    (void)nora_fake_advance(&slot->con.DIVSWEN, &slot->divswen_ticks);
    (void)nora_fake_advance(&slot->con.PLLSWEN, &slot->pllswen_ticks);
    (void)nora_fake_advance(&slot->con.FOUTSWEN, &slot->foutswen_ticks);

    if (nora_fake_advance(&slot->con.OSWEN, &slot->oswen_ticks) != 0) {
        slot->source_switches++;
        if (slot->con.ON != 0u) {
            *rdy = 1u;
        }
    }
}

/*
 * Every block advances on every access.  A poll of one register therefore also
 * lets another block's commit finish, which is both simpler and closer to
 * hardware than tying progress to the register being polled.
 */
static void nora_fake_clock_tick(void)
{
    uint32_t i;

    for (i = 0u; i < NORA_FAKE_CLKGEN_SLOTS; i++) {
        nora_fake_tick_clkgen(&s_clkgen[i]);
    }

    nora_fake_tick_pll(&s_pll[1], &s_oscctrl.PLL1RDY);
    nora_fake_tick_pll(&s_pll[2], &s_oscctrl.PLL2RDY);
}

/* --------------------------------------------------------------------------
 * Accessors
 * -------------------------------------------------------------------------- */

static nora_fake_clkgen_slot_t *clkgen_slot(uint32_t generator)
{
    if (generator >= NORA_FAKE_CLKGEN_SLOTS) {
        return &s_clkgen_void;
    }

    return &s_clkgen[generator];
}

static nora_fake_pll_slot_t *pll_slot(uint32_t pll)
{
    if ((pll == 0u) || (pll >= NORA_FAKE_PLL_SLOTS)) {
        return &s_pll_void;
    }

    return &s_pll[pll];
}

nora_fake_clkcon_t *nora_fake_clkcon(uint32_t generator)
{
    nora_fake_clock_tick();
    return &clkgen_slot(generator)->con;
}

nora_fake_clkdiv_t *nora_fake_clkdiv(uint32_t generator)
{
    nora_fake_clock_tick();
    return &clkgen_slot(generator)->div;
}

nora_fake_pllcon_t *nora_fake_pllcon(uint32_t pll)
{
    nora_fake_clock_tick();
    return &pll_slot(pll)->con;
}

nora_fake_plldiv_t *nora_fake_plldiv(uint32_t pll)
{
    nora_fake_clock_tick();
    return &pll_slot(pll)->div;
}

nora_fake_oscctrl_t *nora_fake_oscctrl(void)
{
    nora_fake_clock_tick();
    return &s_oscctrl;
}

/*
 * Word forms.  Bit positions below are this file's invention (see the header):
 * they make each word a coherent, distinct function of the model state so that a
 * raw capture can be checked for reading the right register block, and they are
 * not the silicon's layout.
 */
uint32_t nora_fake_clkcon_word(uint32_t generator)
{
    const nora_fake_clkcon_t *con = nora_fake_clkcon(generator);

    return (con->ON & 1u) | ((con->OE & 1u) << 1) | ((con->OSWEN & 1u) << 2) |
           ((con->DIVSWEN & 1u) << 3) | ((con->CLKRDY & 1u) << 4) |
           ((con->NOSC & 0xffu) << 8) | ((con->COSC & 0xffu) << 16);
}

uint32_t nora_fake_clkdiv_word(uint32_t generator)
{
    const nora_fake_clkdiv_t *div = nora_fake_clkdiv(generator);

    return (div->INTDIV & 0xffffu) | ((div->FRACDIV & 0xffffu) << 16);
}

uint32_t nora_fake_pllcon_word(uint32_t pll)
{
    const nora_fake_pllcon_t *con = nora_fake_pllcon(pll);

    return (con->ON & 1u) | ((con->OE & 1u) << 1) | ((con->OSWEN & 1u) << 2) |
           ((con->DIVSWEN & 1u) << 3) | ((con->PLLSWEN & 1u) << 4) |
           ((con->FOUTSWEN & 1u) << 5) | ((con->NOSC & 0xffu) << 8);
}

uint32_t nora_fake_plldiv_word(uint32_t pll)
{
    const nora_fake_plldiv_t *div = nora_fake_plldiv(pll);

    return (div->PLLFBDIV & 0x1ffu) | ((div->PLLPRE & 0xfu) << 12) |
           ((div->POSTDIV1 & 0x7u) << 16) | ((div->POSTDIV2 & 0x7u) << 20);
}

uint32_t nora_fake_oscctrl_word(void)
{
    const nora_fake_oscctrl_t *ctrl = nora_fake_oscctrl();

    return (ctrl->PLL1RDY & 1u) | ((ctrl->PLL2RDY & 1u) << 1);
}

uint32_t nora_fake_clkfail_word(void)
{
    nora_fake_clock_tick();
    return 0u;
}

/* --------------------------------------------------------------------------
 * Test-side control and inspection
 * -------------------------------------------------------------------------- */

void nora_fake_clock_reset(void)
{
    memset(s_clkgen, 0, sizeof(s_clkgen));
    memset(s_pll, 0, sizeof(s_pll));
    memset(&s_clkgen_void, 0, sizeof(s_clkgen_void));
    memset(&s_pll_void, 0, sizeof(s_pll_void));
    memset(&s_oscctrl, 0, sizeof(s_oscctrl));

    /* Reset state: the part runs from FRC through CLKGEN1, undivided.  INTDIV 0
     * with FRACDIV 0 is the field pair's spelling of divide-by-one. */
    s_clkgen[1].con.ON = 1u;
    s_clkgen[1].con.OE = 1u;
    s_clkgen[1].con.NOSC = NORA_FAKE_NOSC_FRC;
    s_clkgen[1].con.COSC = NORA_FAKE_NOSC_FRC;
    s_clkgen[1].con.CLKRDY = 1u;
    s_clkgen[1].prev_on = 1u;
}

uint32_t nora_fake_clkgen_source_switches(uint32_t generator)
{
    return clkgen_slot(generator)->source_switches;
}

uint32_t nora_fake_clkgen_div_switches(uint32_t generator)
{
    return clkgen_slot(generator)->div_switches;
}

uint32_t nora_fake_clkgen_on_clears(uint32_t generator)
{
    return clkgen_slot(generator)->on_clears;
}

uint32_t nora_fake_pll_source_switches(uint32_t pll)
{
    return pll_slot(pll)->source_switches;
}

uint32_t nora_fake_pll_on_clears(uint32_t pll)
{
    return pll_slot(pll)->on_clears;
}
