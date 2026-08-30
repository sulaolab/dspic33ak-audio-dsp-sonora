#ifndef NORA_FAKE_CLOCK_H
#define NORA_FAKE_CLOCK_H

#include <stdint.h>

/*
 * Host-side model of the dsPIC33AK clock register file, for the Clock HAL's
 * structural tests.  Only src/app/hal_clock/nora_clock_dspic33ak_reg.c includes
 * <xc.h>, so this is the whole of what has to be faked: the core and the device
 * table compile on the host against nothing at all.
 *
 * WHY THE MODEL MOVES BY ITSELF
 *   The register layer waits on hardware.  Every switch commit is followed by a
 *   poll -- WAIT_CLEAR(OSWEN), WAIT_CLEAR(DIVSWEN), and the combined
 *   "CLKRDY && COSC == source" loop -- with a budget of 1,000,000 iterations
 *   before it gives up with a timeout diagnostic.  A fake made of plain
 *   variables never clears those bits, so every one of those loops would spin to
 *   its budget and the suite would test the timeout path and nothing else.
 *
 *   So the model has a sequencer: nora_fake_clock_tick() is called on EVERY
 *   register access and advances any commit in flight, completing it after
 *   NORA_FAKE_SEQUENCER_TICKS observations.  A commit therefore takes a few poll
 *   iterations to finish, which is what the loops under test are for.
 *
 * WHY THE ACCESSORS ARE FUNCTIONS
 *   xc.h maps each SFR name to a call, e.g. CLK1CONbits -> (*nora_fake_clkcon(1u)),
 *   because that is the only hook a plain `CLK1CONbits.ON = 1;` in the code under
 *   test will run through.  The consequence is worth stating: a pointer accessor
 *   CANNOT TELL A READ FROM A WRITE, so this model has no write counter.  Tests
 *   that need to prove "no register was written" preset a field to a value the
 *   sequence under test would necessarily change (see the OE witness in
 *   test_nora_clock_contract.c) and assert it survived.
 *
 * WHY THE FIELDS ARE uint32_t AND NOT BITFIELDS
 *   These structs model FIELDS, NOT BIT POSITIONS.  Real bit offsets live in the
 *   DFP and are not knowledge this suite has or needs -- the code under test only
 *   ever names fields.  Bitfields would also make every `x.ON = 1` a truncating
 *   store under /W4 /WX.  The word accessors below compose a value from the
 *   fields at positions this file invented, so NO TEST MAY ASSERT A RAW WORD
 *   LAYOUT; they exist so that a raw capture is a coherent function of the model
 *   state, which is what makes "generator 6 reads CLK6 and not CLK8" testable.
 */

#define NORA_FAKE_SEQUENCER_TICKS (4u)

/* NOSC/COSC encoding this device table uses; see nora_clock_device_dspic33ak.c. */
#define NORA_FAKE_NOSC_FRC     (1u)
#define NORA_FAKE_NOSC_BFRC    (2u)
#define NORA_FAKE_NOSC_PRIMARY (3u)
#define NORA_FAKE_NOSC_LPRC    (4u)
#define NORA_FAKE_NOSC_PLL1    (5u)
#define NORA_FAKE_NOSC_PLL2    (6u)

typedef struct {
    uint32_t ON;
    uint32_t OE;
    uint32_t NOSC;
    uint32_t COSC;
    uint32_t OSWEN;
    uint32_t DIVSWEN;
    uint32_t CLKRDY;
} nora_fake_clkcon_t;

typedef struct {
    uint32_t INTDIV;
    uint32_t FRACDIV;
} nora_fake_clkdiv_t;

typedef struct {
    uint32_t ON;
    /* No OE: PLLxCON has no OE bit -- only CLKxCON does. */
    uint32_t NOSC;
    uint32_t OSWEN;
    uint32_t DIVSWEN;
    uint32_t PLLSWEN;
    uint32_t FOUTSWEN;
} nora_fake_pllcon_t;

typedef struct {
    uint32_t PLLFBDIV;
    uint32_t PLLPRE;
    uint32_t POSTDIV1;
    uint32_t POSTDIV2;
} nora_fake_plldiv_t;

typedef struct {
    uint32_t PLL1RDY;
    uint32_t PLL2RDY;
} nora_fake_oscctrl_t;

/* --------------------------------------------------------------------------
 * SFR accessors (what xc.h's macros expand to)
 * -------------------------------------------------------------------------- */

nora_fake_clkcon_t *nora_fake_clkcon(uint32_t generator);
nora_fake_clkdiv_t *nora_fake_clkdiv(uint32_t generator);
nora_fake_pllcon_t *nora_fake_pllcon(uint32_t pll);
nora_fake_plldiv_t *nora_fake_plldiv(uint32_t pll);
nora_fake_oscctrl_t *nora_fake_oscctrl(void);

uint32_t nora_fake_clkcon_word(uint32_t generator);
uint32_t nora_fake_clkdiv_word(uint32_t generator);
uint32_t nora_fake_pllcon_word(uint32_t pll);
uint32_t nora_fake_plldiv_word(uint32_t pll);
uint32_t nora_fake_oscctrl_word(void);
uint32_t nora_fake_clkfail_word(void);

/* --------------------------------------------------------------------------
 * Test-side control and inspection
 * -------------------------------------------------------------------------- */

/*
 * Reset to a plausible power-on state: CLKGEN1 enabled, running FRC undivided
 * and ready; every other generator off; both PLLs off and not ready; every
 * counter zero.  Call this at the top of each case -- the model is global state
 * and a case that inherits the previous one's registers is not testing what it
 * says it is.
 *
 * CLKRDY is deliberately NOT tied to ON here.  The general CLKGEN sequence drops
 * ON before re-sourcing, and if the model cleared CLKRDY with it, the sequence's
 * closing WAIT_SET(CLKRDY) would be satisfied only by the OSWEN completion --
 * true on this silicon, but a coupling this file has no evidence for.  A PLL's
 * RDY *is* cleared with its ON, which is the one lock rule the reg layer's own
 * comments rely on (a PLL taken out of lock is not ready).
 */
void nora_fake_clock_reset(void);

/* Completed OSWEN commits on one generator: how many source switches actually
 * reached the hardware.  A refused call must leave this unchanged. */
uint32_t nora_fake_clkgen_source_switches(uint32_t generator);

/* Completed DIVSWEN commits on one generator. */
uint32_t nora_fake_clkgen_div_switches(uint32_t generator);

/*
 * How many times this generator's ON was observed going 1 -> 0.
 *
 * This is the counter the whole system-clock split exists for.  CLKGEN1's output
 * IS the CPU clock, so clearing its ON stops the core executing the switch; the
 * generic CLKGEN sequence does exactly that and is correct only for a generator
 * nothing is running from.  "CLKGEN1's ON was never cleared" is otherwise
 * invisible in the final register state, and a refactor that quietly routed the
 * system path back through the generic macro would pass every other assertion
 * in this suite.
 */
uint32_t nora_fake_clkgen_on_clears(uint32_t generator);

uint32_t nora_fake_pll_source_switches(uint32_t pll);
uint32_t nora_fake_pll_on_clears(uint32_t pll);

#endif /* NORA_FAKE_CLOCK_H */
