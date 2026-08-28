#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"
#include "nora_clock_device_dspic33ak.h"
#include "nora_clock_dspic33ak_reg.h"

#include <stddef.h>

#define DSPIC33AK_CLOCK_PLLI_MIN_HZ      (5000000UL)
#define DSPIC33AK_CLOCK_PLLI_MAX_HZ      (64000000UL)
#define DSPIC33AK_CLOCK_VCO_MIN_HZ       (500000000UL)
#define DSPIC33AK_CLOCK_VCO_MAX_HZ       (1600000000UL)
#define DSPIC33AK_CLOCK_OUTPUT_MAX_HZ    (800000000UL)
#define DSPIC33AK_CLOCK_PLLFBDIV_MIN     (16UL)
#define DSPIC33AK_CLOCK_PLLFBDIV_MAX     (400UL)
#define DSPIC33AK_CLOCK_PLLPRE_MIN       (1U)
#define DSPIC33AK_CLOCK_PLLPRE_MAX       (15U)
#define DSPIC33AK_CLOCK_PLLPOST_MIN      (1U)
#define DSPIC33AK_CLOCK_PLLPOST_MAX      (7U)
#define DSPIC33AK_CLOCK_CLKGEN_FRAC_HALF (256U)

/* Fosc -> Fcy on this family. PLL1 at 200 MHz gives 100 MIPS, which is what the
 * project's FCY == PLL1_CLK_HZ / 2 has always asserted; this is that same fact,
 * now owned by the HAL that programmed the clock instead of by a build header. */
#define DSPIC33AK_CLOCK_FOSC_TO_FCY_DIV  (2U)

/*
 * The highest system clock this backend will SELECT, in Hz.
 *
 * It exists because nora_clock_switch_source() no longer normalises the system
 * divider (that was a hidden side effect, see the function): preserving whatever
 * divider is set means a caller can now point the CPU at a source the divider was
 * never chosen for. This project has a real 798.72 MHz PLL2 configuration, and
 * "PLL2 is locked" must not be sufficient reason to run the CPU from it.
 *
 * PROVENANCE, because it decides whether a request is refused: this is the rate
 * the project runs and has verified this part at (200 MHz Fosc / 100 MIPS), not a
 * value read from an electrical-characteristics table -- no such table is available
 * in this repository, and the DFP does not carry one (the device .atdf reports
 * speedmax="0"). It is therefore a deliberately CONSERVATIVE refusal threshold
 * rather than a claim about silicon: a part rated higher raises this constant with
 * datasheet evidence, and until then the HAL refuses instead of guessing upward.
 * The limits above it (PLL input / VCO / output) are the datasheet's.
 */
#define DSPIC33AK_CLOCK_SYSTEM_MAX_HZ    (200000000UL)

typedef struct {
    uint32_t feedback_div;
    uint16_t post_div1;
    uint16_t post_div2;
    uint16_t pre_div;
    uint32_t output_hz;
} dspic33ak_clock_pll_solution_t;

/*
 * Sources whose frequency only the board knows -- everything except FRC and the two
 * PLL outputs.
 *
 * FRC is the contract's one known-by-assumption frequency (NORA_CLOCK_FRC_HZ under
 * default tuning), and the PLL outputs are reconstructed from their own registers,
 * so neither needs a slot.  Everything else is here, INCLUDING BFRC and LPRC: they
 * are on-chip, but "on-chip" is not "exactly known".  Their nominal data-sheet
 * numbers are not accurate enough to hand to baud-rate or bit-clock arithmetic as
 * fact, so this HAL reports them only if a caller declared them.
 */
typedef enum {
    DSPIC33AK_CLOCK_DECLARED_BFRC = 0,
    DSPIC33AK_CLOCK_DECLARED_PRIMARY,
    DSPIC33AK_CLOCK_DECLARED_LPRC,
    DSPIC33AK_CLOCK_DECLARED_REFI1,
    DSPIC33AK_CLOCK_DECLARED_REFI2,
    DSPIC33AK_CLOCK_DECLARED_COUNT
} dspic33ak_clock_declared_t;

/*
 * What this HAL knows about frequencies -- deliberately NOT a cache of "what Fosc
 * is", and no longer a cache of what the PLLs run at either.
 *
 * The current operating point is read from the hardware on every query (see
 * current_fosc_hz), because a clock failure monitor can move the system clock
 * without this HAL being called, and a remembered answer would be confidently
 * wrong exactly then.  A PLL's output is read back the same way (see pll_output_hz):
 * this file used to remember each solved output, which is a correct record of the
 * last successful REQUEST and not necessarily the current hardware -- a programming
 * sequence that mutated dividers and then timed out left the previous frequency
 * standing as the answer.  Reconstructing from PLLxCON/PLLxDIV makes that failure
 * report whatever the hardware actually holds, and 0 when that cannot be named.
 *
 * What genuinely cannot be read back is how many Hz an EXTERNAL source carries, so
 * s_declared_hz is the one thing still remembered: what a caller told this HAL about
 * a source the silicon cannot measure.  Frequency knowledge is per-source; the
 * system clock's frequency is then derived from whichever source the hardware
 * selects.
 */
static uint32_t s_declared_hz[DSPIC33AK_CLOCK_DECLARED_COUNT] = { 0u, 0u, 0u, 0u, 0u };

/* Detail of the most recent clock-changing call; see nora_clock_last_diag(). */
static uint16_t s_diag = (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_NONE;

/* --------------------------------------------------------------------------
 * Local helper prototypes
 *
 * Public Clock HAL entry points stay at the top of this file.  Static helpers
 * below perform validation, clock-math solving, and internal register-layer
 * request construction; raw SFR access lives in nora_clock_dspic33ak_reg.c.
 * -------------------------------------------------------------------------- */

static uint16_t clkgen_integer_divider_intdiv(uint16_t divide_by);
static uint16_t clkgen_integer_divider_fracdiv(uint16_t divide_by);
static uint16_t clkgen_divider_from_fields(uint16_t intdiv, uint16_t fracdiv);
static bool source_parent_pll(nora_clock_source_t source, nora_clock_pll_t *pll);
static bool system_source_encode(nora_clock_source_t source, uint16_t *encoded);
static bool declared_slot(nora_clock_source_t source,
    dspic33ak_clock_declared_t *slot);
static void record_declared_hz(nora_clock_source_t source, uint32_t input_hz);
static nora_clock_status_t resolve_source_hz(
    nora_clock_source_t source,
    uint32_t input_hz,
    uint32_t *out_hz);
static uint32_t pll_output_hz(nora_clock_pll_t pll);
static bool read_system_source(dspic33ak_clock_reg_system_t *reg,
    nora_clock_source_t *source);
static uint32_t fosc_from_system_snapshot(
    const dspic33ak_clock_reg_system_t *reg,
    nora_clock_source_t source);
static uint32_t current_fosc_hz(void);
static nora_clock_status_t check_operating_point(uint32_t candidate_fosc_hz);
static void diag_begin(void);
static nora_clock_status_t diag_fail(
    nora_clock_status_t status,
    nora_clock_dspic33ak_diag_t diag);
static nora_clock_status_t solve_pll(
    uint32_t input_hz,
    uint32_t target_hz,
    dspic33ak_clock_pll_solution_t *solution);
static nora_clock_status_t configure_pll(
    nora_clock_pll_t pll,
    const nora_clock_pll_config_t *config,
    uint32_t *resolved_hz);
static nora_clock_status_t configure_clkgen(
    nora_clock_dspic33ak_clkgen_t clkgen,
    nora_clock_source_t source,
    uint16_t divide_by);

/* ========================================================================== */
/* 1. Public API                                                              */
/* ========================================================================== */

/* -------------------------------------------------------------------------- */
/* Configure PLL from a logical clock request                                 */
/* -------------------------------------------------------------------------- */
nora_clock_status_t
nora_clock_pll_configure(
    nora_clock_pll_t pll,
    const nora_clock_pll_config_t *config,
    uint32_t *resolved_hz)
{
    diag_begin();

    if (config == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    switch (pll) {
    case NORA_CLOCK_PLL_1:
    case NORA_CLOCK_PLL_2:
        return configure_pll(pll, config, resolved_hz);
    default:
        return NORA_CLOCK_ERR_INVALID_ARG;
    }
}

/* -------------------------------------------------------------------------- */
/* Switch the system clock (portable face)                                    */
/* -------------------------------------------------------------------------- */
/*
 * On this family the system clock is CLKGEN1's output, so "switch the system clock"
 * is "point CLKGEN1 at that source". CK reaches the same end through OSCCON NOSC +
 * OSWEN; the caller sees one call either way.
 *
 * SOURCE ONLY. This call used to route through configure_clkgen(CLKGEN1, source, 1),
 * so every portable source switch also forced CLKGEN1 to /1. That divider is not a
 * portable concept -- CK has no writable equivalent for the sources this API exposes
 * -- so normalising it here made the portable call mean "source, plus one backend's
 * divider policy". The divider is now changed only by a caller that asked, through
 * nora_clock_dspic33ak_system_divider_set() or the CLKGEN face.
 *
 * Which is exactly why the preflight below exists. Preserving the divider means the
 * caller can now point the CPU at a source the current divider was never chosen for,
 * and this project really does keep PLL2 near 798.72 MHz for peripheral use: "PLL2
 * is locked" must not be sufficient reason to run the CPU from it. Dropping the
 * hidden /1 without adding this check would trade a hidden side effect for a
 * silently over-clocked part.
 *
 * The request is also a DIRECT transition: this HAL never detours through an
 * intermediate oscillator on the caller's behalf, because only the caller knows what
 * else is timed off the clock it is standing on. No direct transition between these
 * sources is known to be prohibited on this part -- the refusals below are all about
 * the resulting operating point -- and none is invented here; a backend whose silicon
 * does prohibit a particular direct transition refuses it in the same place, before
 * the first hardware write. (CK does have such a rule: PLL mode to PLL mode.)
 *
 * A request for the source already running is a DECLARATION UPDATE, not a transition:
 * no switch sequence is issued. That is not an optimisation of a rare case -- boot
 * code normalising the clock it was reset onto hits it every time -- and re-running
 * the sequence would spend a real clock event, with a real in-transit window, to reach
 * the state the part is already in.
 *
 * Accepted: the four oscillators every family has, plus a PLL output that
 * nora_clock_pll_configure() already brought up. The AK-tree-only sources (the VCO
 * fractional taps, the REFI input pins) are reached through the AK CLKGEN face --
 * they are not system-clock sources on a face this HAL wants to keep portable.
 */
nora_clock_status_t
nora_clock_switch_source(
    nora_clock_source_t source,
    uint32_t input_hz)
{
    dspic33ak_clock_reg_system_t reg;
    nora_clock_status_t status;
    nora_clock_source_t current;
    nora_clock_pll_t parent;
    uint32_t source_hz;
    uint16_t encoded;

    diag_begin();

    if (!system_source_encode(source, &encoded)) {
        return NORA_CLOCK_ERR_NOT_SUPPORTED;
    }

    /* Resolve before touching anything: a declaration belongs to the source, not to
     * the switch, so it stays true whether or not the sequence completes -- and a
     * caller's number that contradicts a frequency this HAL knows is an error rather
     * than a silent overwrite (see resolve_source_hz). */
    status = resolve_source_hz(source, input_hz, &source_hz);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    /* source_hz may legitimately be 0 here, and that is NOT refused. Unlike
     * nora_clock_pll_configure(), which must divide the number to reach a target, this
     * call only has to select a source -- so an unknown frequency costs the caller a
     * reported Fosc (get_fosc_hz() answers 0, honestly) and nothing else. Refusing
     * would force a caller to invent a plausible number to get a legal switch done,
     * and every frequency derived from that invention would be wrong while looking
     * authoritative. It removes only the frequency arm of the preflight below. */

    /* One read serves the same-source test and both preconditions below. Two reads
     * would let the state that justified the lock check differ from the state the
     * divider came from. */
    if (!read_system_source(&reg, &current)) {
        current = NORA_CLOCK_SOURCE_UNKNOWN;
    }

    /* Already there: a declaration update, not a clock switch. Running the switch
     * sequence to reach the state the part is already in is a real clock event -- a
     * switch-completion poll and a window in which the selection is in transit --
     * bought for nothing. The declaration above has been applied, so the call has done
     * everything it was asked for.
     *
     * Judged from the OBSERVED source: if a clock failure monitor moved the part, the
     * caller's switch back is a genuine recovery and must not become a no-op. An
     * undecodable selection is therefore never "the same source" -- not knowing what
     * the part runs on is not knowing that no transition is needed. */
    if ((current != NORA_CLOCK_SOURCE_UNKNOWN) && (current == source)) {
        return NORA_CLOCK_OK;
    }

    /* A PLL must already be locked. Pointing the CPU's generator at a PLL that was
     * never configured does not fail cleanly -- the clock the CPU is executing from
     * stops, so there is no return value to inspect. Checking first turns a dead
     * board into a returned error. */
    if (source_parent_pll(source, &parent)) {
        const bool ready = (parent == NORA_CLOCK_PLL_1) ? reg.pll1_ready
                                                        : reg.pll2_ready;
        if (!ready) {
            return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
                NORA_CLOCK_DSPIC33AK_DIAG_PLL_NOT_READY);
        }
    }

    /* The frequency arm of the preflight: the resulting operating point, from the
     * divider that is actually set. Only reachable when the source frequency is
     * knowable -- an operating point that cannot be computed cannot be checked, and per
     * the contract an unknown frequency is not itself a reason to refuse. What stays
     * unconditional is silicon transition legality, which on this part prohibits none
     * of the transitions this API offers. */
    if (source_hz != 0u) {
        /* An unrecognisable divider is refused rather than assumed to be /1: assuming
         * would make the one case this check exists for -- a divider the caller did not
         * choose -- the case it silently waves through. */
        const uint16_t divider = clkgen_divider_from_fields(reg.intdiv, reg.fracdiv);

        if (divider == 0u) {
            return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
                NORA_CLOCK_DSPIC33AK_DIAG_SYSTEM_DIVIDER_UNKNOWN);
        }

        status = check_operating_point(source_hz / divider);
        if (status != NORA_CLOCK_OK) {
            return status;
        }
    }

    return dspic33ak_clock_reg_system_switch_source(encoded);
}

/* -------------------------------------------------------------------------- */
/* Re-divide the system clock generator (dsPIC33AK-only face)                   */
/* -------------------------------------------------------------------------- */
/*
 * The other half of what nora_clock_switch_source() used to do implicitly, now an
 * explicit AK-only call because a system divider is an AK-only concept.
 *
 * Same preflight rule as the switch, from the other direction: there the source
 * changed under a fixed divider, here the divider changes under a fixed source, and
 * both can land the CPU outside its limits. The candidate is computed from the
 * source the hardware currently reports rather than from anything remembered.
 *
 * Not routed through nora_clock_dspic33ak_clkgen_configure(): that call would also
 * re-write CLKGEN1's source, and this one deliberately does not.
 */
nora_clock_status_t
nora_clock_dspic33ak_system_divider_set(uint16_t divide_by)
{
    dspic33ak_clock_reg_system_t reg;
    nora_clock_source_t source;
    nora_clock_status_t status;
    uint32_t source_hz;

    diag_begin();

    if (divide_by == 0u) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /* Reject a divider the field pair cannot express before using it, rather than
     * silently programming the nearest encodable one. */
    if (clkgen_divider_from_fields(clkgen_integer_divider_intdiv(divide_by),
            clkgen_integer_divider_fracdiv(divide_by)) != divide_by) {
        return NORA_CLOCK_ERR_UNREPRESENTABLE;
    }

    if (!read_system_source(&reg, &source)) {
        source = NORA_CLOCK_SOURCE_UNKNOWN;
    }

    /* Same rule as the portable switch, for the same reason: an unknown current source
     * frequency (0, including the undecodable selection above) removes the frequency
     * check, it does not refuse the call. This is the one place refusing could be
     * argued for -- a lower divider raises Fosc -- but the argument does not survive
     * that this call also LOWERS the frequency, which is exactly what a caller does to
     * get back inside the limits, and refusing would block the recovery along with the
     * risk. The frequency this backend cannot name it does not pretend to check. */
    source_hz = nora_clock_source_hz(source);
    if (source_hz != 0u) {
        status = check_operating_point(source_hz / divide_by);
        if (status != NORA_CLOCK_OK) {
            return status;
        }
    }

    return dspic33ak_clock_reg_system_set_divider(
        clkgen_integer_divider_intdiv(divide_by),
        clkgen_integer_divider_fracdiv(divide_by));
}

/* -------------------------------------------------------------------------- */
/* Authoritative current frequencies (portable face)                          */
/* -------------------------------------------------------------------------- */
uint32_t nora_clock_get_fosc_hz(void)
{
    return current_fosc_hz();
}

uint32_t nora_clock_get_fcy_hz(void)
{
    return current_fosc_hz() / DSPIC33AK_CLOCK_FOSC_TO_FCY_DIV;
}

/* -------------------------------------------------------------------------- */
/* Per-source frequency and routing capability (portable face)                */
/* -------------------------------------------------------------------------- */
/*
 * FRC is the contract's known frequency, under the default-tuning assumption stated
 * at NORA_CLOCK_FRC_HZ -- this part can tune it (FRCTUN.TUN), so the number is a
 * documented contract scope, not a claim that the silicon cannot move.
 *
 * The two PLL outputs are reconstructed from their own registers, so a PLL nobody
 * configured, or one left mid-sequence by a failed reconfigure, reports what the
 * hardware holds instead of what a previous request asked for.
 *
 * Everything else -- BFRC, LPRC, PRIMARY, and above all REFI1/REFI2, which are input
 * PINS fed by whatever the board wired -- is known only if a caller declared it, and
 * 0 says "not known". BFRC and LPRC are on-chip and still belong here: a nominal
 * data-sheet frequency is not accurate enough to be used as fact. Do not "improve"
 * this by substituting a plausible number: a wrong Fcy silently mis-programs every
 * baud rate and bit clock derived from it.
 *
 * The VCO fractional taps have no answer even in principle here: their frequency
 * depends on a fractional divider this HAL does not program, so they stay 0.
 */
uint32_t nora_clock_source_hz(nora_clock_source_t source)
{
    dspic33ak_clock_declared_t slot;

    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
        return NORA_CLOCK_FRC_HZ;
    case NORA_CLOCK_SOURCE_PLL_1:
        return pll_output_hz(NORA_CLOCK_PLL_1);
    case NORA_CLOCK_SOURCE_PLL_2:
        return pll_output_hz(NORA_CLOCK_PLL_2);
    default:
        break;
    }

    if (declared_slot(source, &slot)) {
        return s_declared_hz[slot];
    }

    return 0u;
}

/*
 * Two questions, two predicates. One predicate answering "is this source supported"
 * could not answer either honestly on this part: LPRC can drive the system clock but
 * cannot feed a PLL, and REFI1/REFI2 can feed a PLL but are not portable system-clock
 * selections. The old single predicate answered "can the CLKGEN mux name it", which
 * is neither question -- it said yes to REFI1, which nora_clock_switch_source()
 * refuses, and it had no way to say no to LPRC as a PLL input.
 */
bool nora_clock_system_source_is_supported(nora_clock_source_t source)
{
    uint16_t encoded;

    /* The same helper nora_clock_switch_source() gates on, so the answer and the
     * behaviour cannot drift apart. */
    return system_source_encode(source, &encoded);
}

/*
 * The pll argument is used: an instance that does not exist supports nothing, which
 * is a different answer from "this source is not a legal PLL input". Both AK PLLs
 * accept the same input set today, and that is a fact about this part rather than a
 * reason to drop the distinction from the API.
 */
bool nora_clock_pll_input_is_supported(
    nora_clock_pll_t pll,
    nora_clock_source_t source)
{
    uint16_t encoded;

    switch (pll) {
    case NORA_CLOCK_PLL_1:
    case NORA_CLOCK_PLL_2:
        break;
    default:
        return false;
    }

    return nora_clock_device_encode_pll_source(source, &encoded);
}

/* -------------------------------------------------------------------------- */
/* What the hardware is running on (portable face)                            */
/* -------------------------------------------------------------------------- */
/*
 * ONE observation pass. Every field below is derived from the single
 * dspic33ak_clock_reg_read_system() inside read_system_source(): .fosc_hz used to
 * come from current_fosc_hz(), which reads the same registers a second time, so a
 * clock change between the two reads -- the very thing this function exists to
 * observe -- could return .source from one state and .fosc_hz from another.
 *
 * "Single-pass", not "atomic": the reg layer still reads several SFRs in sequence and
 * this backend does not claim hardware can hold them still. What it does claim is
 * that the struct describes one pass rather than two.
 */
nora_clock_status_t nora_clock_get_state(nora_clock_state_t *out)
{
    dspic33ak_clock_reg_system_t reg;
    nora_clock_pll_t parent;

    if (out == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    if (!read_system_source(&reg, &out->source)) {
        out->source = NORA_CLOCK_SOURCE_UNKNOWN;
    }
    out->ready = reg.ready;

    /* A VCO tap is as PLL-derived as the PLL output itself, so lock is asked of
     * the parent PLL rather than of "is the selected source named PLLn". Getting
     * that wrong under-reports lock on exactly the sources a fan-out tree adds. */
    if (source_parent_pll(out->source, &parent)) {
        out->locked = (parent == NORA_CLOCK_PLL_1) ? reg.pll1_ready
                                                   : reg.pll2_ready;
    } else {
        /* Not PLL-derived: nothing to lock, so locked is true. An unmapped source
         * encoding is the one case where lock cannot be established either way,
         * and it reports false -- unconfirmed reads as not locked, and .source
         * already says UNKNOWN for a caller that wants to tell the two apart. */
        out->locked = (out->source != NORA_CLOCK_SOURCE_UNKNOWN);
    }

    /* From the read above, not from a fresh one. */
    out->fosc_hz = fosc_from_system_snapshot(&reg, out->source);

    return NORA_CLOCK_OK;
}

uint16_t nora_clock_last_diag(void)
{
    return s_diag;
}

/* -------------------------------------------------------------------------- */
/* Configure CLKGEN from a logical clock request (dsPIC33AK-only face)         */
/* -------------------------------------------------------------------------- */
nora_clock_status_t
nora_clock_dspic33ak_clkgen_configure(
    nora_clock_dspic33ak_clkgen_t clkgen,
    const nora_clock_dspic33ak_clkgen_config_t *config)
{
    diag_begin();

    if (config == NULL || config->divide_by == 0u) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /* No Fosc bookkeeping here even though CLKGEN1 IS the system clock on this
     * family: what this call changes is the hardware, and the hardware is what
     * nora_clock_get_fosc_hz() reads. */
    return configure_clkgen(clkgen, config->source, config->divide_by);
}

/* -------------------------------------------------------------------------- */
/* Backend diagnostic latch (written by the register layer too)                */
/* -------------------------------------------------------------------------- */
void dspic33ak_clock_diag_set(nora_clock_dspic33ak_diag_t diag)
{
    s_diag = (uint16_t)diag;
}

/* ========================================================================== */
/* 2. Local helpers                                                           */
/* ========================================================================== */

/* -------------------------------------------------------------------------- */
/* Convert integer CLKGEN divider to INTDIV field                             */
/* -------------------------------------------------------------------------- */
static uint16_t clkgen_integer_divider_intdiv(uint16_t divide_by)
{
    if (divide_by == 1u) {
        return 0u;
    }

    return (uint16_t)(divide_by / 2u);
}

/* -------------------------------------------------------------------------- */
/* Convert integer CLKGEN divider to FRACDIV field                            */
/* -------------------------------------------------------------------------- */
static uint16_t clkgen_integer_divider_fracdiv(uint16_t divide_by)
{
    if ((divide_by <= 1u) || ((divide_by & 1u) == 0u)) {
        return 0u;
    }

    return DSPIC33AK_CLOCK_CLKGEN_FRAC_HALF;
}

/* -------------------------------------------------------------------------- */
/* Recover the integer divider from the INTDIV/FRACDIV fields                  */
/* -------------------------------------------------------------------------- */
/*
 * The inverse of the two functions above, and the reason it needs saying: the
 * field pair is not a plain integer divisor. INTDIV counts in steps of two with
 * zero meaning divide-by-one, and an odd divider is expressed as the half step
 * FRACDIV = 256. Reading INTDIV back as "the divisor" would report half the real
 * frequency, which is precisely the class of undocumented divide-by-two that once
 * cost a CK bring-up.
 *
 * 0 means "not an integer divider this HAL can express" -- any FRACDIV the encode
 * side never produces. Fosc is then reported as unknown rather than as a rounded
 * number, because a genuinely fractional divider makes Fosc non-integer and this
 * API is in Hz.
 */
static uint16_t clkgen_divider_from_fields(uint16_t intdiv, uint16_t fracdiv)
{
    if (intdiv == 0u) {
        return (fracdiv == 0u) ? 1u : 0u;
    }
    if (fracdiv == 0u) {
        return (uint16_t)(intdiv * 2u);
    }
    if (fracdiv == DSPIC33AK_CLOCK_CLKGEN_FRAC_HALF) {
        return (uint16_t)((intdiv * 2u) + 1u);
    }

    return 0u;
}

/* -------------------------------------------------------------------------- */
/* Which PLL, if any, a source is derived from                                 */
/* -------------------------------------------------------------------------- */
/*
 * One function, two callers that must not disagree: whether a source needs a lock
 * check (nora_clock_get_state) and whether reconfiguring a PLL would pull the
 * clock out from under the CPU (configure_pll). Written twice, they drift -- the
 * VCO taps were already missing from one such test.
 */
static bool source_parent_pll(nora_clock_source_t source, nora_clock_pll_t *pll)
{
    if (pll == NULL) {
        return false;
    }

    switch (source) {
    case NORA_CLOCK_SOURCE_PLL_1:
    case NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV:
        *pll = NORA_CLOCK_PLL_1;
        return true;
    case NORA_CLOCK_SOURCE_PLL_2:
    case NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV:
        *pll = NORA_CLOCK_PLL_2;
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Is this a portable system-clock source, and what does CLKGEN1 call it?       */
/* -------------------------------------------------------------------------- */
/*
 * The single place that answers "can the system clock be pointed at this", shared by
 * nora_clock_switch_source() and nora_clock_system_source_is_supported() so the
 * predicate cannot promise what the switch then refuses.
 *
 * Deliberately NOT the device encode table alone. That table answers a wider
 * question -- what the CLKGEN mux can select -- and includes the REFI input pins and
 * the VCO fractional taps, which belong to the AK CLKGEN face rather than to a
 * portable system-clock switch. The set here is the portable one; the encode call
 * then supplies the field value, and its failure is impossible for these six but is
 * still propagated rather than assumed.
 */
static bool system_source_encode(nora_clock_source_t source, uint16_t *encoded)
{
    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
    case NORA_CLOCK_SOURCE_BFRC:
    case NORA_CLOCK_SOURCE_PRIMARY:
    case NORA_CLOCK_SOURCE_LPRC:
    case NORA_CLOCK_SOURCE_PLL_1:
    case NORA_CLOCK_SOURCE_PLL_2:
        break;
    default:
        return false;
    }

    return nora_clock_device_encode_clkgen_source(source, encoded);
}

/* -------------------------------------------------------------------------- */
/* Board-declared frequency slots                                              */
/* -------------------------------------------------------------------------- */
static bool declared_slot(nora_clock_source_t source,
    dspic33ak_clock_declared_t *slot)
{
    if (slot == NULL) {
        return false;
    }

    switch (source) {
    case NORA_CLOCK_SOURCE_BFRC:
        *slot = DSPIC33AK_CLOCK_DECLARED_BFRC;
        return true;
    case NORA_CLOCK_SOURCE_PRIMARY:
        *slot = DSPIC33AK_CLOCK_DECLARED_PRIMARY;
        return true;
    case NORA_CLOCK_SOURCE_LPRC:
        *slot = DSPIC33AK_CLOCK_DECLARED_LPRC;
        return true;
    case NORA_CLOCK_SOURCE_REFI1:
        *slot = DSPIC33AK_CLOCK_DECLARED_REFI1;
        return true;
    case NORA_CLOCK_SOURCE_REFI2:
        *slot = DSPIC33AK_CLOCK_DECLARED_REFI2;
        return true;
    default:
        return false;
    }
}

/*
 * Remember a caller's declaration of what an external source carries.
 *
 * A zero is not a declaration, it is "you already know this one", so it never
 * overwrites a good value. A nonzero one REPLACES an earlier declaration on purpose:
 * a REFI pin or the primary oscillator can legitimately be re-driven at another
 * frequency, and "previously declared" must not harden into "immutable".
 *
 * Which source may be declared at all is resolve_source_hz()'s decision; this
 * function only stores.
 */
static void record_declared_hz(nora_clock_source_t source, uint32_t input_hz)
{
    dspic33ak_clock_declared_t slot;

    if (input_hz == 0u) {
        return;
    }

    if (declared_slot(source, &slot)) {
        s_declared_hz[slot] = input_hz;
    }
}

/* -------------------------------------------------------------------------- */
/* How many Hz to work with, given what the caller said and what the HAL knows  */
/* -------------------------------------------------------------------------- */
/*
 * Two kinds of source, two rules.
 *
 * A source whose frequency this HAL determines -- FRC by the contract's tuning
 * assumption, a PLL output by reading its registers -- cannot be redefined by an
 * argument. A caller may RESTATE the known value, which reads well at a call site
 * (input_hz = SONORA_BOOT_PLL1_INPUT_HZ next to a target_hz), but a different value
 * is a disagreement about a fact and returns ERR_INVALID_ARG. Accepting it silently
 * would feed the solver a number the hardware does not have, and every frequency
 * derived from the result would be wrong in a way nothing reports.
 *
 * A source only the board knows is declared by the caller, and a later nonzero
 * declaration replaces an earlier one; zero means "use what I told you before", which
 * is 0 == unknown if nobody ever did.
 */
static nora_clock_status_t resolve_source_hz(
    nora_clock_source_t source,
    uint32_t input_hz,
    uint32_t *out_hz)
{
    dspic33ak_clock_declared_t slot;
    const uint32_t known = nora_clock_source_hz(source);

    if (out_hz == NULL) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    *out_hz = 0u;

    if (!declared_slot(source, &slot)) {
        if ((input_hz != 0u) && (input_hz != known)) {
            return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
                NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_CONFLICT);
        }
        *out_hz = known;
        return NORA_CLOCK_OK;
    }

    if (input_hz != 0u) {
        record_declared_hz(source, input_hz);
        *out_hz = input_hz;
        return NORA_CLOCK_OK;
    }

    *out_hz = known;
    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* What one PLL is currently running at, reconstructed from its registers       */
/* -------------------------------------------------------------------------- */
/*
 * Fout = Fin * PLLFBDIV / (PLLPRE * POSTDIV1 * POSTDIV2) -- the same relation
 * solve_pll() inverts, applied to the fields the hardware actually holds rather than
 * to the last solution this HAL asked for. That difference is the point: a PLL
 * reconfigure writes dividers and then waits for lock, so a sequence that timed out
 * leaves hardware in one state and the previous request in another. Reading back
 * cannot disagree with the hardware.
 *
 * 0 for anything that cannot be named as an exact integer frequency: PLL off, a field
 * combination that is not a divider, an input whose Hz nobody declared, or a
 * non-integer result. Unknown is the honest answer, and every caller already treats 0
 * that way.
 *
 * No recursion risk in the nora_clock_source_hz() call: no encoding in the PLL input
 * table names a PLL output, so the lookup lands on an oscillator or a REFI pin.
 */
static uint32_t pll_output_hz(nora_clock_pll_t pll)
{
    dspic33ak_clock_reg_pll_state_t reg;
    nora_clock_source_t input;
    uint64_t numerator;
    uint64_t divisor;
    uint32_t input_hz;

    dspic33ak_clock_reg_read_pll(pll, &reg);

    if (!reg.enabled) {
        return 0u;
    }
    if ((reg.feedback_div == 0u) || (reg.pre_div == 0u) ||
        (reg.post_div1 == 0u) || (reg.post_div2 == 0u)) {
        return 0u;
    }
    if (!nora_clock_device_decode_pll_source(reg.source, &input)) {
        return 0u;
    }

    input_hz = nora_clock_source_hz(input);
    if (input_hz == 0u) {
        return 0u;
    }

    numerator = (uint64_t)input_hz * reg.feedback_div;
    divisor = (uint64_t)reg.pre_div * reg.post_div1 * reg.post_div2;
    if ((numerator % divisor) != 0u) {
        return 0u;
    }

    numerator /= divisor;
    /* Above what this PLL can produce is not a frequency, it is a misread: report
     * unknown rather than a number no consumer should act on. */
    if (numerator > (uint64_t)DSPIC33AK_CLOCK_OUTPUT_MAX_HZ) {
        return 0u;
    }

    return (uint32_t)numerator;
}

/* -------------------------------------------------------------------------- */
/* Read the system clock registers and decode the selected source              */
/* -------------------------------------------------------------------------- */
static bool read_system_source(dspic33ak_clock_reg_system_t *reg,
    nora_clock_source_t *source)
{
    dspic33ak_clock_reg_read_system(reg);

    return nora_clock_device_decode_clkgen_source(reg->source, source);
}

/* -------------------------------------------------------------------------- */
/* Fosc implied by one system-register observation                              */
/* -------------------------------------------------------------------------- */
/*
 * The single computation behind both nora_clock_get_fosc_hz() and
 * nora_clock_get_state().fosc_hz. Two implementations of "what is Fosc" is two
 * truths, and the one that is a cached request loses to a clock failure monitor
 * without saying so.
 *
 * Pure with respect to the hardware: it takes an observation instead of making one,
 * so a caller that already read the system registers derives Fosc from THAT read
 * rather than from a second one taken a few instructions later. NORA_CLOCK_SOURCE_
 * UNKNOWN resolves to 0 Hz through the normal lookup, so an undecodable selection
 * needs no special case here.
 *
 * Fosc is CLKGEN1's OUTPUT, so the divider is part of the answer: the generator
 * may legitimately divide, and reporting the source frequency as Fosc would
 * over-report by exactly that factor.
 */
static uint32_t fosc_from_system_snapshot(
    const dspic33ak_clock_reg_system_t *reg,
    nora_clock_source_t source)
{
    uint32_t source_hz;
    uint16_t divider;

    if (reg == NULL) {
        return 0u;
    }

    source_hz = nora_clock_source_hz(source);
    if (source_hz == 0u) {
        return 0u;
    }

    divider = clkgen_divider_from_fields(reg->intdiv, reg->fracdiv);
    if (divider == 0u) {
        return 0u;
    }

    return source_hz / divider;
}

/* One observation, then the pure computation above. */
static uint32_t current_fosc_hz(void)
{
    dspic33ak_clock_reg_system_t reg;
    nora_clock_source_t source;

    if (!read_system_source(&reg, &source)) {
        return 0u;
    }

    return fosc_from_system_snapshot(&reg, source);
}

/* -------------------------------------------------------------------------- */
/* Is this operating point one this backend will select?                       */
/* -------------------------------------------------------------------------- */
/*
 * The preflight both system-clock mutators share -- switching the source under the
 * current divider, and changing the divider under the current source. Refused before
 * the first hardware write, because the hardware in question is the clock the caller
 * is executing from: there is no useful error return from an over-clocked CPU.
 *
 * See DSPIC33AK_CLOCK_SYSTEM_MAX_HZ for what this limit is and is not.
 */
static nora_clock_status_t check_operating_point(uint32_t candidate_fosc_hz)
{
    if (candidate_fosc_hz > DSPIC33AK_CLOCK_SYSTEM_MAX_HZ) {
        return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
            NORA_CLOCK_DSPIC33AK_DIAG_FOSC_OVER_LIMIT);
    }

    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* Diagnostic latch lifetime                                                   */
/* -------------------------------------------------------------------------- */
/*
 * Cleared when a clock-changing call starts; left at NONE if that call succeeds;
 * set on the way out of a failure and retained until the next such call. The
 * read-only APIs never touch it, so a caller can inspect the state a failure
 * produced and still report why it failed.
 */
static void diag_begin(void)
{
    s_diag = (uint16_t)NORA_CLOCK_DSPIC33AK_DIAG_NONE;
}

static nora_clock_status_t diag_fail(
    nora_clock_status_t status,
    nora_clock_dspic33ak_diag_t diag)
{
    s_diag = (uint16_t)diag;
    return status;
}

/* -------------------------------------------------------------------------- */
/* Solve PLL divider fields for an exact target frequency                     */
/* -------------------------------------------------------------------------- */
static nora_clock_status_t solve_pll(
    uint32_t input_hz_arg,
    uint32_t target_hz,
    dspic33ak_clock_pll_solution_t *solution)
{
    uint16_t pre_div;
    uint16_t post_div2;
    uint16_t post_div1;

    if (solution == NULL || input_hz_arg == 0u || target_hz == 0u) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    for (pre_div = DSPIC33AK_CLOCK_PLLPRE_MIN;
         pre_div <= DSPIC33AK_CLOCK_PLLPRE_MAX;
         pre_div++) {
        const uint64_t input_hz = input_hz_arg;

        if (input_hz < ((uint64_t)DSPIC33AK_CLOCK_PLLI_MIN_HZ * pre_div) ||
            input_hz > ((uint64_t)DSPIC33AK_CLOCK_PLLI_MAX_HZ * pre_div)) {
            continue;
        }

        for (post_div2 = DSPIC33AK_CLOCK_PLLPOST_MIN;
             post_div2 <= DSPIC33AK_CLOCK_PLLPOST_MAX;
             post_div2++) {
            for (post_div1 = DSPIC33AK_CLOCK_PLLPOST_MIN;
                 post_div1 <= DSPIC33AK_CLOCK_PLLPOST_MAX;
                 post_div1++) {
                const uint64_t post_product = (uint64_t)post_div1 * post_div2;
                const uint64_t numerator =
                    (uint64_t)target_hz * pre_div * post_product;
                uint64_t feedback_div;
                uint64_t vco_numerator;

                if (post_div1 < post_div2) {
                    continue;
                }
                if ((uint64_t)target_hz > DSPIC33AK_CLOCK_OUTPUT_MAX_HZ) {
                    continue;
                }
                if ((numerator % input_hz) != 0u) {
                    continue;
                }

                feedback_div = numerator / input_hz;
                if (feedback_div < DSPIC33AK_CLOCK_PLLFBDIV_MIN ||
                    feedback_div > DSPIC33AK_CLOCK_PLLFBDIV_MAX) {
                    continue;
                }

                vco_numerator = input_hz * feedback_div;
                if (vco_numerator < ((uint64_t)DSPIC33AK_CLOCK_VCO_MIN_HZ * pre_div) ||
                    vco_numerator > ((uint64_t)DSPIC33AK_CLOCK_VCO_MAX_HZ * pre_div)) {
                    continue;
                }

                solution->feedback_div = (uint32_t)feedback_div;
                solution->post_div1 = post_div1;
                solution->post_div2 = post_div2;
                solution->pre_div = pre_div;
                /* Only exact solutions are accepted, so the resolved output IS the
                 * request. Recorded rather than recomputed so the value handed back
                 * to the caller comes from the accepted solution. */
                solution->output_hz = target_hz;
                return NORA_CLOCK_OK;
            }
        }
    }

    return NORA_CLOCK_ERR_UNREPRESENTABLE;
}

/* -------------------------------------------------------------------------- */
/* Configure one PLL through the internal register layer                       */
/* -------------------------------------------------------------------------- */
static nora_clock_status_t configure_pll(
    nora_clock_pll_t pll,
    const nora_clock_pll_config_t *config,
    uint32_t *resolved_hz)
{
    dspic33ak_clock_pll_solution_t solution;
    dspic33ak_clock_reg_pll_config_t reg_config;
    dspic33ak_clock_reg_system_t reg;
    nora_clock_status_t status;
    nora_clock_source_t current_source;
    nora_clock_pll_t current_parent;
    uint32_t input_hz;
    uint16_t source;

    if (!nora_clock_device_encode_pll_source(config->source, &source)) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    /*
     * Never reconfigure the PLL the CPU is currently running from. Programming a
     * PLL takes it out of lock, so doing that to the system clock's PLL is the
     * one operation in this HAL that cannot be reported as an error -- there is
     * no clock left to return on. The caller switches away first; this HAL does
     * not take a hidden detour through FRC on its behalf, because only the caller
     * knows what else is timed off that clock.
     *
     * A source encoding this device table cannot name is not treated as "might be
     * this PLL": the encodings are enumerated, and refusing on an unknown one
     * would be a boot-time landmine built out of a case that cannot occur.
     */
    if (read_system_source(&reg, &current_source) &&
        source_parent_pll(current_source, &current_parent) &&
        (current_parent == pll)) {
        return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
            NORA_CLOCK_DSPIC33AK_DIAG_PLL_DRIVES_SYSTEM);
    }

    /* input_hz == 0 means "the HAL knows this one" -- FRC under the contract's tuning
     * assumption, or a value a previous call declared for that source. A nonzero one
     * is a declaration for a board-known source, or a restatement of what the HAL
     * already knows; a nonzero one that CONTRADICTS what it knows is refused rather
     * than handed to the solver (see resolve_source_hz). */
    status = resolve_source_hz(config->source, config->input_hz, &input_hz);
    if (status != NORA_CLOCK_OK) {
        return status;
    }
    if (input_hz == 0u) {
        return diag_fail(NORA_CLOCK_ERR_INVALID_ARG,
            NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_UNKNOWN);
    }

    status = solve_pll(input_hz, config->target_hz, &solution);
    if (status != NORA_CLOCK_OK) {
        if (status == NORA_CLOCK_ERR_UNREPRESENTABLE) {
            (void)diag_fail(status,
                NORA_CLOCK_DSPIC33AK_DIAG_NO_DIVIDER_SOLUTION);
        }
        return status;
    }

    reg_config.source = source;
    reg_config.feedback_div = solution.feedback_div;
    reg_config.pre_div = solution.pre_div;
    reg_config.post_div1 = solution.post_div1;
    reg_config.post_div2 = solution.post_div2;

    status = dspic33ak_clock_reg_pll_configure(pll, &reg_config);
    if (status != NORA_CLOCK_OK) {
        return status;
    }

    /*
     * Nothing is remembered here. What this PLL now runs at is in its registers, and
     * nora_clock_source_hz() reads them -- so the answer after a failure above is
     * whatever the hardware holds, not the frequency this call was aiming for.
     *
     * resolved_hz below is a different thing and is still the solver's: "what the
     * successful request resolved to", handed back to the caller that asked for it.
     */
    if (resolved_hz != NULL) {
        *resolved_hz = solution.output_hz;
    }

    return NORA_CLOCK_OK;
}

/* -------------------------------------------------------------------------- */
/* Configure one CLKGEN through the internal register layer                    */
/* -------------------------------------------------------------------------- */
static nora_clock_status_t configure_clkgen(
    nora_clock_dspic33ak_clkgen_t clkgen,
    nora_clock_source_t source,
    uint16_t divide_by)
{
    dspic33ak_clock_reg_clkgen_config_t reg_config;
    uint16_t encoded_source;

    if (!nora_clock_device_encode_clkgen_source(source, &encoded_source)) {
        return NORA_CLOCK_ERR_INVALID_ARG;
    }

    reg_config.source = encoded_source;
    reg_config.intdiv = clkgen_integer_divider_intdiv(divide_by);
    reg_config.fracdiv = clkgen_integer_divider_fracdiv(divide_by);

    return dspic33ak_clock_reg_clkgen_configure(clkgen, &reg_config);
}
