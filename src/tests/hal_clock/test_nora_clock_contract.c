/*
 * Structural tests for the NORA Clock contract's dsPIC33AK backend.
 *
 * These are the cases from the mini-revision proposal r4 §10 stage C: the rules
 * the contract states in prose, made mechanical, on a host where the register
 * file can be preset into states hardware will not reach on demand -- an
 * undecodable COSC, a PLL whose dividers were changed behind the HAL's back, an
 * operating point above the ceiling.  Stage D (hardware) covers what a fake
 * cannot: that the sequences actually switch a real clock.
 *
 * WHAT A PASS HERE DOES AND DOES NOT MEAN
 *   The model in fake_xc/ is this suite's own invention.  A case that only
 *   asserts the model's arithmetic proves nothing, so each case below is written
 *   against a rule the contract states, and the model exists to make the rule
 *   observable.  Where the model had to invent something the datasheet does not
 *   give us (raw word bit positions), no assertion depends on it.
 */

#include <stdio.h>
#include <string.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"
#include "nora_clock_device_dspic33ak.h"

#include "nora_fake_clock.h"

static int s_failures;

static void check(int ok, const char *expr, int line)
{
    if (ok == 0) {
        (void)printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
        s_failures++;
    }
}

#define CHECK(cond) check((cond) ? 1 : 0, #cond, __LINE__)

/* --------------------------------------------------------------------------
 * The status, source, and diagnostic numbering is a CONSUMED contract
 * -------------------------------------------------------------------------- */
/*
 * r4 §8: these numbers are printed on consoles and read out of logs by people
 * comparing an AK board with a CK board, so renumbering them is a breaking
 * change to something no compiler checks.  Pinning them here makes that
 * explicit -- a reordered enum fails this case instead of quietly changing what
 * a log line means.
 */
static void case_contract_numbering(void)
{
    CHECK(NORA_CLOCK_OK == 0);
    CHECK(NORA_CLOCK_ERR_INVALID_ARG == 1);
    CHECK(NORA_CLOCK_ERR_NOT_SUPPORTED == 2);
    CHECK(NORA_CLOCK_ERR_UNREPRESENTABLE == 3);
    CHECK(NORA_CLOCK_ERR_TIMEOUT == 4);
    CHECK(NORA_CLOCK_ERR_NOT_PRESENT == 5);

    CHECK(NORA_CLOCK_PLL_1 == 1);
    CHECK(NORA_CLOCK_PLL_2 == 2);

    CHECK(NORA_CLOCK_SOURCE_FRC == 0x00);
    CHECK(NORA_CLOCK_SOURCE_BFRC == 0x01);
    CHECK(NORA_CLOCK_SOURCE_PRIMARY == 0x02);
    CHECK(NORA_CLOCK_SOURCE_LPRC == 0x03);
    CHECK(NORA_CLOCK_SOURCE_PLL_1 == 0x10);
    CHECK(NORA_CLOCK_SOURCE_PLL_2 == 0x11);
    CHECK(NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV == 0x40);
    CHECK(NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV == 0x41);
    CHECK(NORA_CLOCK_SOURCE_REFI1 == 0x42);
    CHECK(NORA_CLOCK_SOURCE_REFI2 == 0x43);
    CHECK(NORA_CLOCK_SOURCE_FRC_DIVIDED == 0x44);
    CHECK(NORA_CLOCK_SOURCE_UNKNOWN == 0xffff);

    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_NONE == 0);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT == 1);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT == 2);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_CLKRDY_TIMEOUT == 3);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_PLL_SWITCH_TIMEOUT == 4);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_FOUT_SWITCH_TIMEOUT == 5);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_PLL_LOCK_TIMEOUT == 6);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_PLL_NOT_READY == 7);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_PLL_DRIVES_SYSTEM == 8);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_UNKNOWN == 9);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_NO_DIVIDER_SOLUTION == 10);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_FOSC_OVER_LIMIT == 11);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_SYSTEM_DIVIDER_UNKNOWN == 12);
    CHECK(NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_CONFLICT == 13);

    CHECK(NORA_CLOCK_FRC_HZ == 8000000UL);
}

/* --------------------------------------------------------------------------
 * Two capability questions, two predicates (r4 §2)
 * -------------------------------------------------------------------------- */
static void case_capability_sets(void)
{
    nora_clock_pll_config_t cfg;

    nora_fake_clock_reset();

    /* AK: six system-clock sources. */
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_FRC));
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_BFRC));
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_PRIMARY));
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_LPRC));
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_PLL_1));
    CHECK(nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_PLL_2));

    /* The AK-tree-only nodes are reached through the CLKGEN face, not this one,
     * and the two observation-only values are not arguments at all. */
    CHECK(!nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_REFI1));
    CHECK(!nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_REFI2));
    CHECK(!nora_clock_system_source_is_supported(
        NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV));
    CHECK(!nora_clock_system_source_is_supported(
        NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV));
    CHECK(!nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_FRC_DIVIDED));
    CHECK(!nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_UNKNOWN));

    /* AK: five PLL inputs, the same set on both PLLs -- a fact about this part,
     * which is why the predicate still takes the pll argument. */
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_FRC));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_BFRC));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_PRIMARY));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_REFI1));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_REFI2));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_2,
        NORA_CLOCK_SOURCE_FRC));
    CHECK(nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_2,
        NORA_CLOCK_SOURCE_REFI2));

    /* LPRC can drive the system clock and cannot feed a PLL: the asymmetry one
     * predicate could not express. */
    CHECK(!nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_LPRC));
    CHECK(!nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_PLL_1));
    CHECK(!nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_PLL_2));
    CHECK(!nora_clock_pll_input_is_supported(NORA_CLOCK_PLL_1,
        NORA_CLOCK_SOURCE_FRC_DIVIDED));

    /* An instance that does not exist supports nothing -- a different answer
     * from "that source is not a legal input". */
    CHECK(!nora_clock_pll_input_is_supported((nora_clock_pll_t)0,
        NORA_CLOCK_SOURCE_FRC));
    CHECK(!nora_clock_pll_input_is_supported((nora_clock_pll_t)3,
        NORA_CLOCK_SOURCE_FRC));

    /*
     * r4 §8: PLL_2 EXISTS on this part, so ERR_NOT_PRESENT is not an answer the
     * AK backend ever gives.  It is CK's answer for the same call, and that is
     * the whole reason the status value exists -- "this instance is absent" is
     * not "that argument was wrong".
     */
    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;
    cfg.target_hz = 100000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_2, &cfg, NULL) !=
        NORA_CLOCK_ERR_NOT_PRESENT);
}

/* --------------------------------------------------------------------------
 * FRC_DIVIDED is observation-only, and AK absorbs it with no code change
 * -------------------------------------------------------------------------- */
/*
 * The enumerator exists for a family whose oscillator mux offers "FRC" and "FRC
 * through a divider" as two SELECTIONS.  A backend there must not report the
 * divided selection as FRC, because the same-source rule would then make a
 * boot-time switch_source(FRC, 0) a no-op while the part still ran divided.
 *
 * On AK there is no such selection, so the whole of AK's obligation is to treat
 * the value as contract-known and unreachable: not a legal argument, no
 * frequency, and never produced by the device table (see the round-trip case).
 */
static void case_frc_divided_is_observation_only(void)
{
    nora_fake_clock_reset();

    CHECK(!nora_clock_system_source_is_supported(NORA_CLOCK_SOURCE_FRC_DIVIDED));
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC_DIVIDED, 0u) ==
        NORA_CLOCK_ERR_NOT_SUPPORTED);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_FRC_DIVIDED) == 0u);
}

/* --------------------------------------------------------------------------
 * The device table is an exact inverse, and names no PLL as a PLL input
 * -------------------------------------------------------------------------- */
static void case_device_table_round_trip(void)
{
    uint32_t value;
    uint32_t clkgen_named = 0u;
    uint32_t pll_named = 0u;

    for (value = 0u; value < 64u; value++) {
        nora_clock_source_t source;
        uint16_t back;

        if (nora_clock_device_decode_clkgen_source((uint16_t)value, &source)) {
            clkgen_named++;
            /* Never produced on this family. */
            CHECK(source != NORA_CLOCK_SOURCE_FRC_DIVIDED);
            CHECK(source != NORA_CLOCK_SOURCE_UNKNOWN);
            CHECK(nora_clock_device_encode_clkgen_source(source, &back));
            CHECK(back == (uint16_t)value);
        }

        if (nora_clock_device_decode_pll_source((uint16_t)value, &source)) {
            pll_named++;
            /*
             * No encoding in the PLL input table names a PLL output.  This is
             * the property that lets the core reconstruct a PLL's input
             * frequency without a recursion depth guard, so it is asserted
             * rather than left as a comment.
             */
            CHECK(source != NORA_CLOCK_SOURCE_PLL_1);
            CHECK(source != NORA_CLOCK_SOURCE_PLL_2);
            CHECK(source != NORA_CLOCK_SOURCE_FRC_DIVIDED);
            CHECK(nora_clock_device_encode_pll_source(source, &back));
            CHECK(back == (uint16_t)value);
        }
    }

    CHECK(clkgen_named == 10u);
    CHECK(pll_named == 5u);
}

/* --------------------------------------------------------------------------
 * input_hz is API-specific, and a contradiction is an error (r4 §5)
 * -------------------------------------------------------------------------- */
static void case_frc_input_hz_rules(void)
{
    nora_clock_pll_config_t cfg;

    nora_fake_clock_reset();

    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_FRC) == NORA_CLOCK_FRC_HZ);

    /* Reset leaves the part on FRC, so this is the same-source path. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 0u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_clock_last_diag() == 0u);

    /* Restating a frequency the HAL determines reads well at a call site and is
     * accepted. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 8000000u) ==
        NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);

    /*
     * Contradicting it is not.  Note this refusal survives the same-source
     * no-op: the disagreement is about a fact, and resolving it happens before
     * the transition question, so a caller cannot hide a wrong number behind
     * "no switch was needed anyway".
     */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 7000000u) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_CONFLICT);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_FRC) == NORA_CLOCK_FRC_HZ);

    /* The same rule through the other entry point. */
    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 7000000u;
    cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_CONFLICT);
    CHECK(nora_fake_pllcon(1u)->ON == 0u);
}

/* --------------------------------------------------------------------------
 * pll_configure() needs an exact input frequency; switch_source() does not
 * -------------------------------------------------------------------------- */
static void case_pll_configure_needs_a_known_input(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t resolved = 0u;

    nora_fake_clock_reset();

    /* REFI1 is an input PIN: nobody has said what the board drives it at. */
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_REFI1) == 0u);

    cfg.source = NORA_CLOCK_SOURCE_REFI1;
    cfg.input_hz = 0u;
    cfg.target_hz = 200000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, &resolved) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_UNKNOWN);
    /* Refused before the first write: the PLL was never enabled. */
    CHECK(nora_fake_pllcon(1u)->ON == 0u);
    CHECK(nora_fake_pll_source_switches(1u) == 0u);

    cfg.input_hz = 24000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, &resolved) ==
        NORA_CLOCK_OK);
    CHECK(resolved == 200000000u);
    CHECK(nora_clock_last_diag() == 0u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_REFI1) == 24000000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 200000000u);
    CHECK(nora_fake_plldiv(1u)->PLLFBDIV == 25u);
    CHECK(nora_fake_plldiv(1u)->PLLPRE == 1u);
    CHECK(nora_fake_plldiv(1u)->POSTDIV1 == 3u);
    CHECK(nora_fake_plldiv(1u)->POSTDIV2 == 1u);
    CHECK(nora_fake_pllcon(1u)->ON == 1u);
    CHECK(nora_fake_oscctrl()->PLL1RDY == 1u);

    /* A later nonzero declaration REPLACES an earlier one: a REFI pin can be
     * re-driven, and "previously declared" must not harden into "immutable". */
    cfg.input_hz = 12000000u;
    cfg.target_hz = 100000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, &resolved) ==
        NORA_CLOCK_OK);
    CHECK(resolved == 100000000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_REFI1) == 12000000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 100000000u);
    /* Reconfiguring took the PLL out of lock and put it back. */
    CHECK(nora_fake_pll_on_clears(1u) == 1u);
    CHECK(nora_fake_oscctrl()->PLL1RDY == 1u);
}

/* --------------------------------------------------------------------------
 * A declaration belongs to the SOURCE, not the API (r4 §7): switch -> pll
 * -------------------------------------------------------------------------- */
static void case_declaration_from_switch_reaches_pll_configure(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t resolved = 0u;

    nora_fake_clock_reset();

    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PRIMARY) == 0u);

    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PRIMARY, 24000000u) ==
        NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 1u);
    CHECK(nora_fake_clkcon(1u)->COSC == NORA_FAKE_NOSC_PRIMARY);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PRIMARY) == 24000000u);
    CHECK(nora_clock_get_fosc_hz() == 24000000u);

    /*
     * One store, read from the other direction.  The proof is the solution, not
     * the status: PLLFBDIV 25 can only come from a 24 MHz input.  Solved from
     * FRC's 8 MHz the same target gives 75, so a backend that had kept the
     * declaration per-API would fail here rather than silently misprogram.
     */
    cfg.source = NORA_CLOCK_SOURCE_PRIMARY;
    cfg.input_hz = 0u;
    cfg.target_hz = 200000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, &resolved) ==
        NORA_CLOCK_OK);
    CHECK(resolved == 200000000u);
    CHECK(nora_fake_plldiv(1u)->PLLFBDIV == 25u);
}

/* --------------------------------------------------------------------------
 * ...and the other direction: pll_configure -> switch_source
 * -------------------------------------------------------------------------- */
static void case_declaration_from_pll_configure_reaches_switch(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t resolved = 0u;

    nora_fake_clock_reset();

    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_REFI2) == 0u);

    cfg.source = NORA_CLOCK_SOURCE_REFI2;
    cfg.input_hz = 12288000u;
    cfg.target_hz = 196608000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_2, &cfg, &resolved) ==
        NORA_CLOCK_OK);
    CHECK(resolved == 196608000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_REFI2) == 12288000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_2) == 196608000u);

    /* Without that declaration the switch could not compute an operating point
     * for PLL_2 at all; with it, the preflight has a number to check. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_2, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_get_fosc_hz() == 196608000u);
    CHECK(nora_clock_get_fcy_hz() == 98304000u);
}

/* --------------------------------------------------------------------------
 * Unknown -> known promotion, and the same-source declaration update (r4 §6)
 * -------------------------------------------------------------------------- */
/*
 * One sequence covers four rules at once, because they are one rule seen from
 * four sides: an unknown frequency is not a refusal; a request for the source
 * already running is a declaration update and not a clock event; zero never
 * clears a declaration; and a successful no-op leaves last_diag() at 0.
 */
static void case_unknown_to_known_promotion(void)
{
    nora_clock_state_t state;

    nora_fake_clock_reset();

    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_LPRC) == 0u);

    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_LPRC, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 1u);
    CHECK(nora_clock_get_fosc_hz() == 0u);

    memset(&state, 0, sizeof(state));
    CHECK(nora_clock_get_state(&state) == NORA_CLOCK_OK);
    CHECK(state.source == NORA_CLOCK_SOURCE_LPRC);
    CHECK(state.ready);
    CHECK(state.locked); /* not PLL-derived: nothing to lock */
    CHECK(state.fosc_hz == 0u);

    /* Declaration update. No second switch. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_LPRC, 32768u) ==
        NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 1u);
    CHECK(nora_clock_last_diag() == 0u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_LPRC) == 32768u);
    CHECK(nora_clock_get_fosc_hz() == 32768u);

    /* Zero means "use what I told you before", never "forget it". */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_LPRC, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_LPRC) == 32768u);
    CHECK(nora_fake_clkgen_source_switches(1u) == 1u);
}

/* --------------------------------------------------------------------------
 * get_state() is ONE observation pass (r4 §3)
 * -------------------------------------------------------------------------- */
static void case_one_observation_pass(void)
{
    nora_clock_state_t state;

    nora_fake_clock_reset();

    memset(&state, 0, sizeof(state));
    CHECK(nora_clock_get_state(&state) == NORA_CLOCK_OK);
    CHECK(state.source == NORA_CLOCK_SOURCE_FRC);
    CHECK(state.fosc_hz == NORA_CLOCK_FRC_HZ);
    CHECK(state.fosc_hz == nora_clock_get_fosc_hz());
    CHECK(nora_clock_get_fcy_hz() == (NORA_CLOCK_FRC_HZ / 2u));

    CHECK(nora_clock_get_state(NULL) == NORA_CLOCK_ERR_INVALID_ARG);
}

/* --------------------------------------------------------------------------
 * switch_source() is SOURCE ONLY: the divider survives (r4 §1)
 * -------------------------------------------------------------------------- */
static void case_divider_is_preserved_across_a_switch(void)
{
    nora_clock_pll_config_t cfg;
    nora_clock_state_t state;

    nora_fake_clock_reset();

    CHECK(nora_clock_dspic33ak_system_divider_set(4u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkdiv(1u)->INTDIV == 2u);
    CHECK(nora_fake_clkdiv(1u)->FRACDIV == 0u);
    CHECK(nora_fake_clkgen_div_switches(1u) == 1u);
    CHECK(nora_clock_get_fosc_hz() == 2000000u);

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;
    cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_OK);

    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) ==
        NORA_CLOCK_OK);

    /* The old behaviour normalised CLKGEN1 to /1 on every portable switch. */
    CHECK(nora_fake_clkdiv(1u)->INTDIV == 2u);
    CHECK(nora_clock_get_fosc_hz() == 100000000u);
    CHECK(nora_clock_get_fcy_hz() == 50000000u);

    memset(&state, 0, sizeof(state));
    CHECK(nora_clock_get_state(&state) == NORA_CLOCK_OK);
    CHECK(state.source == NORA_CLOCK_SOURCE_PLL_1);
    CHECK(state.locked);
    CHECK(state.fosc_hz == 100000000u);
}

/* --------------------------------------------------------------------------
 * An illegal operating point is refused before the first write (r4 §1)
 * -------------------------------------------------------------------------- */
/*
 * One register preset serves both halves of the rule: PLL1 at 400 MHz is over
 * the ceiling on /1 and legal on /4.  The refusal is therefore about the
 * operating point rather than about the source, which is what preserving the
 * divider made possible in the first place.
 *
 * The witness for "before the first write" is CLK1CON.OE, cleared here by the
 * test: the system switch sequence sets ON and OE unconditionally as its first
 * two writes, so OE surviving at 0 means the sequence was never entered.  (The
 * model cannot count writes -- a pointer accessor sees reads and writes alike --
 * so a preset field a real write would change is how this gets proven.)
 */
static void case_illegal_operating_point_refused_before_any_write(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t cosc_before;

    nora_fake_clock_reset();

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;
    cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 400000000u);

    nora_fake_clkcon(1u)->OE = 0u;
    cosc_before = nora_fake_clkcon(1u)->COSC;

    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_FOSC_OVER_LIMIT);
    CHECK(nora_fake_clkcon(1u)->OE == 0u);
    CHECK(nora_fake_clkcon(1u)->COSC == cosc_before);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_fake_clkgen_on_clears(1u) == 0u);

    /* Same PLL, same source, now inside the limits. */
    CHECK(nora_clock_dspic33ak_system_divider_set(4u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkcon(1u)->OE == 1u); /* a sequence that DID run */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_get_fosc_hz() == 100000000u);
}

/* --------------------------------------------------------------------------
 * An undecodable selection is never "the same source" (r4 §6)
 * -------------------------------------------------------------------------- */
static void case_unknown_cosc_is_never_the_same_source(void)
{
    nora_clock_state_t state;

    nora_fake_clock_reset();

    /* An encoding this device table does not name -- reachable on hardware if a
     * clock failure monitor or a bootloader left the part somewhere unexpected. */
    nora_fake_clkcon(1u)->COSC = 15u;
    nora_fake_clkcon(1u)->NOSC = 15u;

    memset(&state, 0, sizeof(state));
    CHECK(nora_clock_get_state(&state) == NORA_CLOCK_OK);
    CHECK(state.source == NORA_CLOCK_SOURCE_UNKNOWN);
    CHECK(!state.locked); /* unconfirmed reads as not locked */
    CHECK(state.ready);   /* and that is independent of CLKRDY */
    CHECK(state.fosc_hz == 0u);
    CHECK(nora_clock_get_fosc_hz() == 0u);

    /*
     * Not knowing what the part runs on is not knowing that no transition is
     * needed.  If this compared equal to anything, a recovery switch would
     * become a silent no-op.
     */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 0u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_source_switches(1u) == 1u);
    CHECK(nora_fake_clkcon(1u)->COSC == NORA_FAKE_NOSC_FRC);
}

/* --------------------------------------------------------------------------
 * A PLL's frequency comes from its registers, not from the last request (r4 §4)
 * -------------------------------------------------------------------------- */
static void case_pll_hz_follows_registers(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t resolved = 0u;

    nora_fake_clock_reset();

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;
    cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, &resolved) ==
        NORA_CLOCK_OK);
    CHECK(resolved == 400000000u);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 400000000u);
    CHECK(nora_fake_plldiv(1u)->PLLFBDIV == 100u);
    CHECK(nora_fake_plldiv(1u)->POSTDIV1 == 2u);

    /*
     * Change the hardware behind the HAL's back -- which is what a reconfigure
     * that timed out mid-sequence leaves behind.  A backend that remembered its
     * last successful request would still answer 400 MHz here.
     */
    nora_fake_plldiv(1u)->PLLFBDIV = 50u;
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 200000000u);

    /* A field combination that is not a divider is unknown, not rounded. */
    nora_fake_plldiv(1u)->POSTDIV1 = 0u;
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 0u);
    nora_fake_plldiv(1u)->POSTDIV1 = 2u;
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 200000000u);

    /* A non-integer result has no answer in Hz. */
    nora_fake_plldiv(1u)->POSTDIV1 = 3u;
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 0u);
    nora_fake_plldiv(1u)->POSTDIV1 = 2u;

    /* A disabled PLL produces nothing. */
    nora_fake_pllcon(1u)->ON = 0u;
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_1) == 0u);
}

/* --------------------------------------------------------------------------
 * The PLL the CPU runs from is never reconfigured
 * -------------------------------------------------------------------------- */
static void case_pll_driving_the_system_is_not_reconfigured(void)
{
    nora_clock_pll_config_t cfg;
    uint32_t feedback_before;

    nora_fake_clock_reset();

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;
    cfg.target_hz = 200000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_OK);

    /* Exactly at the project ceiling on /1: the comparison is `>`, so the
     * boundary value is accepted rather than refused. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_get_fosc_hz() == 200000000u);

    feedback_before = nora_fake_plldiv(1u)->PLLFBDIV;
    cfg.target_hz = 100000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_PLL_DRIVES_SYSTEM);
    CHECK(nora_fake_plldiv(1u)->PLLFBDIV == feedback_before);
    /* Never taken out of lock: there would have been no clock to return on. */
    CHECK(nora_fake_pll_on_clears(1u) == 0u);
    CHECK(nora_fake_oscctrl()->PLL1RDY == 1u);

    /* The other PLL is a different instance and stays available. */
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_2, &cfg, NULL) ==
        NORA_CLOCK_OK);
    CHECK(nora_clock_source_hz(NORA_CLOCK_SOURCE_PLL_2) == 100000000u);
}

/* --------------------------------------------------------------------------
 * A PLL must be locked before the CPU is pointed at it
 * -------------------------------------------------------------------------- */
static void case_switch_to_an_unlocked_pll_is_refused(void)
{
    nora_fake_clock_reset();

    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_PLL_NOT_READY);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_clock_get_fosc_hz() == NORA_CLOCK_FRC_HZ);
}

/* --------------------------------------------------------------------------
 * Only exact PLL solutions are accepted
 * -------------------------------------------------------------------------- */
static void case_unrepresentable_pll_target(void)
{
    nora_clock_pll_config_t cfg;

    nora_fake_clock_reset();

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.input_hz = 0u;

    /* 7 MHz from 8 MHz: every exact combination lands outside the VCO window. */
    cfg.target_hz = 7000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_ERR_UNREPRESENTABLE);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_NO_DIVIDER_SOLUTION);

    /* Above what the PLL can output at all. */
    cfg.target_hz = 900000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_ERR_UNREPRESENTABLE);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_NO_DIVIDER_SOLUTION);

    /* A zero target is a bad argument, not an unrepresentable one -- and it
     * carries no backend phase, so the latch cleared at entry stays clear. */
    cfg.target_hz = 0u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &cfg, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() == 0u);

    /* None of the three touched the PLL. */
    CHECK(nora_fake_pllcon(1u)->ON == 0u);
    CHECK(nora_fake_pll_source_switches(1u) == 0u);
}

/* --------------------------------------------------------------------------
 * system_divider_set() argument rules
 * -------------------------------------------------------------------------- */
static void case_system_divider_set_arguments(void)
{
    uint32_t divide_by;

    nora_fake_clock_reset();

    CHECK(nora_clock_dspic33ak_system_divider_set(0u) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_fake_clkgen_div_switches(1u) == 0u);
    CHECK(nora_clock_last_diag() == 0u);

    /*
     * The INTDIV/FRACDIV pair expresses EVERY integer divider in 1..65535 (even
     * values as INTDIV = n/2, odd ones as the same INTDIV plus the FRACDIV half
     * step), so ERR_UNREPRESENTABLE is currently unreachable through this entry
     * point.  The round-trip check in the backend is an assertion that the
     * encode/decode pair agree, not a filter on the argument -- worth saying,
     * because a reader looking for the input that triggers it will not find one.
     */
    for (divide_by = 1u; divide_by <= 1024u; divide_by++) {
        CHECK(nora_clock_dspic33ak_system_divider_set((uint16_t)divide_by) !=
            NORA_CLOCK_ERR_UNREPRESENTABLE);
    }
    CHECK(nora_clock_dspic33ak_system_divider_set(65535u) !=
        NORA_CLOCK_ERR_UNREPRESENTABLE);

    /* An odd divider is the half step, and Fosc reports the divided output. */
    nora_fake_clock_reset();
    CHECK(nora_clock_dspic33ak_system_divider_set(3u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkdiv(1u)->INTDIV == 1u);
    CHECK(nora_fake_clkdiv(1u)->FRACDIV == 256u);
    CHECK(nora_clock_get_fosc_hz() == (NORA_CLOCK_FRC_HZ / 3u));

    /* A FRACDIV the encode side never produces is not a divider this HAL can
     * name, so Fosc is unknown rather than rounded. */
    nora_fake_clkdiv(1u)->FRACDIV = 100u;
    CHECK(nora_clock_get_fosc_hz() == 0u);

    /* ...and a switch under such a divider cannot check its operating point, so
     * it is refused rather than waved through as /1. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_BFRC, 16000000u) ==
        NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_last_diag() ==
        (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_SYSTEM_DIVIDER_UNKNOWN);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
}

/* --------------------------------------------------------------------------
 * CLKGEN1's ON is never cleared; a peripheral generator's is
 * -------------------------------------------------------------------------- */
/*
 * The defect this whole split exists for.  The generic CLKGEN sequence disables
 * the generator before re-sourcing it, which is correct for a generator nothing
 * is executing from and stops the CPU dead on CLKGEN1.  Nothing in the final
 * register state records that it happened, so the model counts the 1 -> 0
 * transitions and this case reads them.
 */
static void case_system_generator_is_never_stopped(void)
{
    nora_clock_dspic33ak_clkgen_config_t cfg;

    nora_fake_clock_reset();

    /*
     * CLKGEN6 has to be RUNNING first.  The model counts ON transitions it
     * observes going 1 -> 0, and after reset every generator but CLKGEN1 is off --
     * so the generic sequence's `ON = 0` would be a write of the value already
     * there and the stop would be uncountable.  Starting it from BFRC also makes
     * the re-source below a real change of selection, and is the situation the
     * contrast is about: a peripheral generator that something may be clocked from
     * is stopped by this path, which is exactly why CLKGEN1 may not use it.
     */
    nora_fake_clkcon(6u)->ON = 1u;
    nora_fake_clkcon(6u)->OE = 1u;
    nora_fake_clkcon(6u)->NOSC = NORA_FAKE_NOSC_BFRC;
    nora_fake_clkcon(6u)->COSC = NORA_FAKE_NOSC_BFRC;
    nora_fake_clkcon(6u)->CLKRDY = 1u;

    cfg.source = NORA_CLOCK_SOURCE_FRC;
    cfg.divide_by = 2u;
    CHECK(nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
        &cfg) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkcon(6u)->ON == 1u);
    CHECK(nora_fake_clkcon(6u)->OE == 1u);
    CHECK(nora_fake_clkcon(6u)->COSC == NORA_FAKE_NOSC_FRC);
    CHECK(nora_fake_clkcon(6u)->CLKRDY == 1u);
    CHECK(nora_fake_clkdiv(6u)->INTDIV == 1u);
    CHECK(nora_fake_clkgen_source_switches(6u) == 1u);
    CHECK(nora_fake_clkgen_div_switches(6u) == 1u);
    /* Expected here, and the reason the system path is separate. */
    CHECK(nora_fake_clkgen_on_clears(6u) == 1u);
    CHECK(nora_fake_clkgen_on_clears(1u) == 0u);

    /* CLKGEN1 through the AK CLKGEN face. */
    cfg.divide_by = 1u;
    CHECK(nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_1,
        &cfg) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_on_clears(1u) == 0u);

    /* CLKGEN1 through the portable face. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_BFRC, 0u) ==
        NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_on_clears(1u) == 0u);
    CHECK(nora_fake_clkcon(1u)->COSC == NORA_FAKE_NOSC_BFRC);

    /* ...and through the divider half. */
    CHECK(nora_clock_dspic33ak_system_divider_set(2u) == NORA_CLOCK_OK);
    CHECK(nora_fake_clkgen_on_clears(1u) == 0u);
}

/* --------------------------------------------------------------------------
 * A raw capture reads the block it names, and reports zeros when it cannot
 * -------------------------------------------------------------------------- */
static void case_raw_capture_wiring(void)
{
    nora_clock_dspic33ak_clkgen_config_t clkgen_cfg;
    nora_clock_pll_config_t pll_cfg;
    nora_clock_dspic33ak_clkgen_raw_t raw6;
    nora_clock_dspic33ak_clkgen_raw_t raw8;
    nora_clock_dspic33ak_clkgen_raw_t raw_absent;
    nora_clock_dspic33ak_raw_t raw;

    nora_fake_clock_reset();

    clkgen_cfg.source = NORA_CLOCK_SOURCE_FRC;
    clkgen_cfg.divide_by = 2u;
    CHECK(nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
        &clkgen_cfg) == NORA_CLOCK_OK);

    memset(&raw6, 0, sizeof(raw6));
    memset(&raw8, 0, sizeof(raw8));
    nora_clock_dspic33ak_clkgen_raw_capture(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
        &raw6);
    nora_clock_dspic33ak_clkgen_raw_capture(NORA_CLOCK_DSPIC33AK_CLKGEN_8,
        &raw8);
    /* Generator 6 was configured and 8 was not: a capture that read the wrong
     * block would show it here. */
    CHECK(raw6.con != 0u);
    CHECK(raw6.div != 0u);
    CHECK(raw8.con == 0u);
    CHECK(raw8.div == 0u);

    /* A record that could not be taken must not look taken -- so the fields are
     * written to zero rather than left holding the caller's data. */
    raw_absent.con = 0xdeadbeefu;
    raw_absent.div = 0xdeadbeefu;
    nora_clock_dspic33ak_clkgen_raw_capture(
        (nora_clock_dspic33ak_clkgen_t)7, &raw_absent);
    CHECK(raw_absent.con == 0u);
    CHECK(raw_absent.div == 0u);

    /* A null out pointer is a no-op, not a fault. */
    nora_clock_dspic33ak_clkgen_raw_capture(NORA_CLOCK_DSPIC33AK_CLKGEN_1, NULL);
    nora_clock_dspic33ak_raw_capture(NULL);

    pll_cfg.source = NORA_CLOCK_SOURCE_FRC;
    pll_cfg.input_hz = 0u;
    pll_cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &pll_cfg, NULL) ==
        NORA_CLOCK_OK);

    memset(&raw, 0, sizeof(raw));
    nora_clock_dspic33ak_raw_capture(&raw);
    /* PLL1 was configured and PLL2 was not: the two blocks are not swapped. */
    CHECK(raw.pll1con != 0u);
    CHECK(raw.pll1div != 0u);
    CHECK(raw.pll2con == 0u);
    CHECK(raw.pll2div == 0u);
    /* Only that a locked PLL shows up somewhere in the word -- this suite makes
     * no claim about where. */
    CHECK(raw.oscctrl != 0u);
}

/* --------------------------------------------------------------------------
 * Argument validation on every entry point
 * -------------------------------------------------------------------------- */
static void case_argument_validation(void)
{
    nora_clock_dspic33ak_clkgen_config_t clkgen_cfg;
    nora_clock_pll_config_t pll_cfg;

    nora_fake_clock_reset();

    clkgen_cfg.source = NORA_CLOCK_SOURCE_FRC;
    clkgen_cfg.divide_by = 1u;
    CHECK(nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
        NULL) == NORA_CLOCK_ERR_INVALID_ARG);
    CHECK(nora_clock_dspic33ak_clkgen_configure(
        (nora_clock_dspic33ak_clkgen_t)7, &clkgen_cfg) ==
        NORA_CLOCK_ERR_INVALID_ARG);

    /* Not an encodable CLKGEN source on this family. */
    clkgen_cfg.source = NORA_CLOCK_SOURCE_FRC_DIVIDED;
    CHECK(nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
        &clkgen_cfg) == NORA_CLOCK_ERR_INVALID_ARG);

    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, NULL, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);

    pll_cfg.source = NORA_CLOCK_SOURCE_FRC;
    pll_cfg.input_hz = 0u;
    pll_cfg.target_hz = 400000000u;
    CHECK(nora_clock_pll_configure((nora_clock_pll_t)7, &pll_cfg, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);

    /* LPRC drives the system clock but is not a PLL input. */
    pll_cfg.source = NORA_CLOCK_SOURCE_LPRC;
    pll_cfg.input_hz = 32768u;
    pll_cfg.target_hz = 100000000u;
    CHECK(nora_clock_pll_configure(NORA_CLOCK_PLL_1, &pll_cfg, NULL) ==
        NORA_CLOCK_ERR_INVALID_ARG);

    /* Not portable system-clock selections: NOT_SUPPORTED, distinct from a bad
     * argument, and refused before anything is touched. */
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_REFI1, 0u) ==
        NORA_CLOCK_ERR_NOT_SUPPORTED);
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV, 0u) ==
        NORA_CLOCK_ERR_NOT_SUPPORTED);
    CHECK(nora_clock_switch_source(NORA_CLOCK_SOURCE_UNKNOWN, 0u) ==
        NORA_CLOCK_ERR_NOT_SUPPORTED);
    CHECK(nora_fake_clkgen_source_switches(1u) == 0u);
    CHECK(nora_fake_pll_source_switches(1u) == 0u);
}

int main(void)
{
    case_contract_numbering();
    case_capability_sets();
    case_frc_divided_is_observation_only();
    case_device_table_round_trip();
    case_frc_input_hz_rules();
    /* Ordered so that no case inherits a frequency declaration another made:
     * the declaration store is process-global by design (it describes the
     * board, not a call), and only nora_fake_clock_reset() clears registers. */
    case_pll_configure_needs_a_known_input();          /* declares REFI1 */
    case_declaration_from_switch_reaches_pll_configure(); /* declares PRIMARY */
    case_declaration_from_pll_configure_reaches_switch(); /* declares REFI2 */
    case_unknown_to_known_promotion();                 /* declares LPRC */
    case_one_observation_pass();
    case_divider_is_preserved_across_a_switch();
    case_illegal_operating_point_refused_before_any_write();
    case_unknown_cosc_is_never_the_same_source();
    case_pll_hz_follows_registers();
    case_pll_driving_the_system_is_not_reconfigured();
    case_switch_to_an_unlocked_pll_is_refused();
    case_unrepresentable_pll_target();
    case_system_divider_set_arguments();
    case_system_generator_is_never_stopped();
    case_raw_capture_wiring();
    case_argument_validation();

    if (s_failures != 0) {
        (void)printf("nora_clock contract tests: %d FAILURE(S)\n", s_failures);
        return 1;
    }

    (void)printf("nora_clock contract tests: PASS\n");
    return 0;
}
