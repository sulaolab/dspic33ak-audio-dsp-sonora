#ifndef NORA_CLOCK_H
#define NORA_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Clock HAL public interface -- the AK/CK common face.
 *
 * This HAL exposes logical PLL programming requests, a system-clock source
 * switch, the authoritative current frequencies, and one observation of what the
 * hardware is actually running on.  It hides device NOSC encodings, XC-DSC
 * bitfields, and SFR names below the public API.  The generic core validates PLL
 * input, VCO, divider, and output constraints from its backend's device facts (on
 * AK, the dsPIC33AK512MPS512 DFP 1.3.185), then drives the internal register layer
 * that owns switch, ready, and timeout sequencing.
 *
 * Board policy stays above this HAL: FRC boot policy, PPS routing, REFI pin
 * selection, UART / PWM / audio clock requirements, and application frequency
 * choices belong to the board or peripheral integration layers.  No public type
 * in this header is an XC-DSC / DFP register type.
 *
 * WHAT IS **NOT** HERE, AND WHY
 *   The CLKGEN blocks moved to nora_clock_dspic33ak.h.  CLKGEN is a dsPIC33AK
 *   block, not a clock concept: CK has no such thing (one system PLL switched by
 *   OSCCON.OSWEN, plus per-peripheral selects), so there is no CK implementation
 *   of those calls to write.  Keeping them here made this header read as portable
 *   while its only entry point was AK-shaped.  Code that programs a CLKGEN is
 *   board bring-up and now says so at the call site; code that just needs a
 *   frequency uses nora_clock_get_fcy_hz() and ports unchanged.
 *
 *   A raw register dump is not here either.  Post-mortem and bring-up code that
 *   genuinely wants the oscillator control words asks its backend for them by
 *   name (nora_clock_<family>_raw_capture()), so the register set it depends on
 *   is visible in the type it names, and no portable-looking call returns a word
 *   whose bit layout is silicon.  Everything that is a *question* about the clock
 *   -- which source, running, locked, at what frequency -- is nora_clock_get_state()
 *   and needs no register decode at the call site.
 *
 * PORTABILITY OF nora_clock_source_t
 *   FRC / BFRC / PRIMARY / LPRC are oscillators every family has; together with
 *   PLL_1 / PLL_2 they are the values nora_clock_switch_source() accepts.  The
 *   remaining values name nodes that exist because of how one family fans its
 *   clock tree out; they live in a separate numeric range.  Some are
 *   nora_clock_pll_configure() inputs (AK's fractional-divider VCO taps and REFI
 *   pins, also used by the AK CLKGEN calls); FRC_DIVIDED is observation-only, like
 *   NORA_CLOCK_SOURCE_UNKNOWN.  A backend that
 *   lacks a value returns NORA_CLOCK_ERR_NOT_SUPPORTED rather than silently
 *   accepting it.
 *
 *   Naming a source is not the same as being allowed to use it for a given
 *   operation, and this header does not pretend otherwise.  "Can the system clock
 *   run from it" and "can it feed this PLL" are different hardware questions with
 *   different answers on the SAME part -- on AK, LPRC can drive the system clock but
 *   cannot feed a PLL, while REFI1/REFI2 can feed a PLL and are not system-clock
 *   selections -- so there are two predicates, nora_clock_system_source_is_supported()
 *   and nora_clock_pll_input_is_supported(), and neither answers the other.  Both are
 *   compile-target properties and touch no hardware.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Status types                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Every value is explicitly assigned: consoles and post-mortem records print the
 * integer, so the numbering is a consumed contract, not an implementation detail
 * that a future insertion may renumber.  NORA_CLOCK_ERR_TIMEOUT keeps the value 4
 * for that reason.
 *
 * There is deliberately no per-phase timeout value ("divider switch", "PLL
 * switch", "source switch", "ready").  Which phase of a silicon-specific
 * sequence stalled is not portable, and the previous 16-value enum declared ten
 * such values of which seven were unreachable.  The phase is still reported --
 * as a backend-defined detail code from nora_clock_last_diag().
 */
typedef enum {
    NORA_CLOCK_OK                   = 0,

    /* The caller's arguments are wrong for this call: a null pointer, a zero
     * divider, an unknown enum value, a source the selected block cannot
     * consume, or a precondition the caller must establish first (switching the
     * system clock to a PLL that has not been configured, or reconfiguring the
     * PLL that is currently driving the system clock). */
    NORA_CLOCK_ERR_INVALID_ARG      = 1,

    /* This backend cannot reach that source at all -- the value is meaningful in
     * the portable enum but has no route on this silicon. */
    NORA_CLOCK_ERR_NOT_SUPPORTED    = 2,

    /* The exact target cannot be represented within the published device limits
     * and integer divider ranges.  This HAL does not approximate. */
    NORA_CLOCK_ERR_UNREPRESENTABLE  = 3,

    /* A hardware switch or a PLL lock did not complete within the register
     * layer's polling budget, or completed without taking effect.  Which phase:
     * nora_clock_last_diag(). */
    NORA_CLOCK_ERR_TIMEOUT          = 4,

    /* The requested PLL or generator INSTANCE is not present in this backend's NORA
     * clock model.  A silicon block outside that model does not make the portable
     * instance present -- the dsPIC33CK256MP508's Auxiliary PLL exists, but its
     * outputs are peripheral clock options and it is not a system-clock source in the
     * NOSC mux, so mapping portable PLL_2 onto it would extend this HAL's scope rather
     * than implement this contract; CK answers PLL_2 with this status.
     *
     * The line between this and NORA_CLOCK_ERR_NOT_SUPPORTED is the KIND of argument:
     *   NOT_PRESENT    the requested portable instance is absent from the model
     *   NOT_SUPPORTED  the instance or source exists in the model, but this routing or
     *                  operation is unsupported on this silicon
     * They are deliberately not merged.  These integers are printed by consoles and
     * post-mortem records, so one value carrying both meanings would leave a printed
     * "2" ambiguous between "that source has no route here" and "that PLL instance is
     * not implemented", which no console legend can separate afterwards. */
    NORA_CLOCK_ERR_NOT_PRESENT      = 5
} nora_clock_status_t;

/* -------------------------------------------------------------------------- */
/* PLL / source types                                                         */
/* -------------------------------------------------------------------------- */

typedef enum {
    NORA_CLOCK_PLL_1 = 1,
    NORA_CLOCK_PLL_2 = 2
} nora_clock_pll_t;

/*
 * 0x00..0x3f  portable: an oscillator or a PLL output every family can name.
 * 0x40..      extension: a node that exists because of how one silicon family
 *             fans its clock tree out.  A portable consumer never names one.
 */
typedef enum {
    NORA_CLOCK_SOURCE_FRC              = 0x00,
    NORA_CLOCK_SOURCE_BFRC             = 0x01,
    /* Logical primary oscillator source; device-specific NOSC encoding is not
     * exposed through this API. */
    NORA_CLOCK_SOURCE_PRIMARY          = 0x02,
    NORA_CLOCK_SOURCE_LPRC             = 0x03,

    NORA_CLOCK_SOURCE_PLL_1            = 0x10,
    NORA_CLOCK_SOURCE_PLL_2            = 0x11,

    NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV = 0x40,
    NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV = 0x41,
    NORA_CLOCK_SOURCE_REFI1            = 0x42,
    NORA_CLOCK_SOURCE_REFI2            = 0x43,

    /*
     * The internal RC oscillator through a divider that is part of the SOURCE
     * SELECTION rather than a separate divider field -- a family whose oscillator mux
     * offers "FRC" and "FRC divided" as two selections reports this one for the
     * second.  It is in the extension range for the usual reason: it exists because
     * of how one silicon family shapes its clock tree, and a portable consumer never
     * names it.
     *
     * A BACKEND MUST NOT REPORT THIS AS NORA_CLOCK_SOURCE_FRC.  That is a behavioural
     * requirement, not a labelling preference, and the same-source rule on
     * nora_clock_switch_source() is why: folding the divided selection into FRC would
     * make a boot-time nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 0) -- the call
     * whose whole job is to leave a known clock behind -- compare equal to what the
     * part was reset onto, perform no clock event, and return NORA_CLOCK_OK with the
     * part still running divided.  Reporting the two selections as two sources makes
     * that switch happen.
     *
     * Observation only, like NORA_CLOCK_SOURCE_UNKNOWN: it is not a legal argument, and
     * nora_clock_system_source_is_supported() answers false for it, because the divisor
     * is not part of this contract and a caller cannot say which one it means.  A
     * backend that can read that divisor may report the divided frequency for it
     * through nora_clock_source_hz(); one that cannot reports 0.
     */
    NORA_CLOCK_SOURCE_FRC_DIVIDED      = 0x44,

    /* The hardware reports a source encoding this backend does not map to a
     * logical name.  Only nora_clock_get_state() produces it; it is never a
     * legal argument. */
    NORA_CLOCK_SOURCE_UNKNOWN          = 0xffff
} nora_clock_source_t;

/* -------------------------------------------------------------------------- */
/* Device facts                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Nominal FRC frequency used by this contract, in Hz -- 8 MHz on both families
 * implementing this contract (dsPIC33AK and dsPIC33CK) -- valid while the device's
 * FRC tuning control is at its
 * supported center / default setting.  FRC does not follow the system clock
 * selection, so anything clocked directly from FRC references this constant rather
 * than the system Fcy.  Single definition point for the project.
 *
 * This is a SCOPE STATEMENT, not a claim that the silicon cannot tune FRC.  It can:
 * AK has FRCTUN.TUN[5:0] and BFRCTUN.TUN[5:0], and CK has OSCTUN.TUN[5:0]
 * (DS70005399D section 9.4).  Runtime RC tuning is outside the current NORA Clock
 * contract -- no call in this HAL writes a tuning field, and no source in either repo
 * that vendors this contract writes one either, which is what makes the constant true
 * here by construction rather
 * than by hope.
 *
 * WIDENING THE SCOPE IS NOT A DOCUMENTATION CHANGE
 *   A backend that ever supports tuning replaces this constant with a query that reads
 *   the tuning register, and adds that register to its raw capture so a post-mortem can
 *   still say which FRC the recorded numbers were derived from.  Until both exist, code
 *   that tunes FRC and code that trusts this constant cannot coexist, and the constant
 *   must not be quietly redefined to mean "whatever FRC happens to be".
 */
#define NORA_CLOCK_FRC_HZ (8000000UL)

/* -------------------------------------------------------------------------- */
/* Configuration / observation types                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    /* An oscillator, or an extension-range input pin.  Never a PLL output: a PLL
     * is not a legal input to a PLL. */
    nora_clock_source_t source;

    /* That source's frequency in Hz, or 0 for "use what the contract already knows".
     *
     * THE RULES BELOW ARE THIS API'S.  How a zero is treated is a property of the
     * OPERATION, not of the field: nora_clock_pll_configure() must divide this number
     * to reach target_hz, so an unknown input makes the request unanswerable and it is
     * refused.  nora_clock_switch_source() only has to select a source, so an unknown
     * frequency there costs the caller a reported Fosc and nothing else, and it is
     * NOT refused -- see that function.  The classification that follows is shared by
     * both; only the consequence of "unknown" differs.
     *
     * CONTRACT-KNOWN vs CALLER-DECLARED
     *   Contract-known means this HAL determines the frequency itself: FRC
     *   (NORA_CLOCK_FRC_HZ, within the tuning scope stated above) and a PLL output,
     *   which a backend reconstructs from that PLL's own registers.
     *
     *   Everything else is caller-declared -- an external oscillator, a board input
     *   pin, and also BFRC and LPRC.  Those two are on-chip, and that is precisely
     *   why they are listed here: "device-known" is not the same as "on-chip".  Their
     *   data sheet numbers are nominal, and a nominal number must not be silently
     *   promoted to exact HAL knowledge by virtue of being printed in a data sheet.
     *
     * 0
     *   Contract-known source: the contract's value is used.
     *   Caller-declared source: the frequency a previous call declared is reused; if
     *   none was ever declared, NORA_CLOCK_ERR_INVALID_ARG.  The HAL cannot measure
     *   an oscillator, and guessing would corrupt every frequency derived from it.
     *
     * nonzero
     *   Contract-known source: must equal what the contract determines.  Restating it
     *   is accepted; contradicting it is NORA_CLOCK_ERR_INVALID_ARG, because one of
     *   the two numbers is wrong and the HAL must not silently choose which.
     *   Caller-declared source: a declaration, which deliberately REPLACES any
     *   earlier one for that source.  Re-declaring is how a board says the hardware
     *   changed.
     *
     * A DECLARATION BELONGS TO THE SOURCE, NOT TO THE CALL THAT MADE IT
     *   There is one frequency declaration per logical source, shared by every API in
     *   this contract that names that source.  A frequency declared here is what
     *   nora_clock_source_hz() then reports for that source, what nora_clock_switch_source()
     *   uses to compute Fosc after selecting it, and what a later 0 in either API reuses
     *   -- and equally, a frequency declared through nora_clock_switch_source() satisfies
     *   a later 0 here.  Both directions, one store.
     *
     *   Per-API declarations would let one clock have two contradictory frequencies at
     *   once, differing only in which function was asked; the caller's board has one
     *   crystal on that pin.  A backend must not keep a second copy for its own use. */
    uint32_t input_hz;

    uint32_t target_hz;
} nora_clock_pll_config_t;

/*
 * What the hardware says right now, read from the hardware on every call.
 *
 * This is an observation, not a record of what was requested: a clock failure
 * monitor can move the system clock without this HAL being involved, so a cached
 * "what I last programmed" would be confidently wrong exactly when it matters.
 */
typedef struct {
    /* The logical source the hardware reports as driving the system clock, or
     * NORA_CLOCK_SOURCE_UNKNOWN. */
    nora_clock_source_t source;

    /* The hardware reports that source as running. */
    bool ready;

    /* For a PLL-derived source (including a VCO tap), that PLL reports lock.
     * true when the source is not PLL-derived -- there is nothing to lock.
     * false when .source is NORA_CLOCK_SOURCE_UNKNOWN: lock cannot be
     * established for a source the backend could not name. */
    bool locked;

    /* Current Fosc in Hz, computed from .source and the system divider of the SAME
     * register observation pass that filled the fields above, or 0 when the HAL has
     * not been told that source's frequency.  Identical to nora_clock_get_fosc_hz():
     * one computation, not two truths. */
    uint32_t fosc_hz;
} nora_clock_state_t;

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * Configure PLL1 or PLL2 for an exact target frequency.
 *
 * This programs a PLL.  It does not switch the system clock onto it -- that is
 * nora_clock_switch_source().
 *
 * Which sources may feed a PLL is a per-backend question, answered by
 * nora_clock_pll_input_is_supported().  It is NOT the set nora_clock_switch_source()
 * accepts, and on AK it is neither a subset nor a superset of it: AK PLL inputs are
 * FRC / BFRC / PRIMARY / REFI1 / REFI2, so LPRC can drive the system clock but cannot
 * feed a PLL, while REFI1 / REFI2 can feed a PLL and are not system-clock selections.
 * (CK PLL inputs are FRC / PRIMARY.)  A PLL output is never a legal PLL input on any
 * backend and returns NORA_CLOCK_ERR_INVALID_ARG.
 *
 * The PLL currently driving the system clock may not be reconfigured: it returns
 * NORA_CLOCK_ERR_INVALID_ARG, and the caller switches the system clock away
 * first.  The HAL does not take a hidden detour through another oscillator to
 * make the call appear to work; a clock re-source is the caller's decision
 * because only the caller knows what else is timed off that clock.
 *
 * resolved_hz is optional and is written only after the hardware programming
 * sequence completes successfully.  It is the frequency the divider solution
 * actually produces; this HAL only accepts exact solutions, so a successful call
 * always resolves to config->target_hz.  It is an output for the caller's record,
 * not a value to test for inequality.
 *
 * ON FAILURE
 *   A failed reconfigure must not leave that PLL's PREVIOUS frequency standing as the
 *   current truth.  After an error, nora_clock_source_hz() for that PLL output reports
 *   what the PLL hardware now says -- which may be 0, meaning unknown -- and never the
 *   frequency the last successful call produced.  A half-programmed PLL is not running
 *   at either frequency, and reporting the old one would be a confident lie exactly
 *   where a caller is deciding how to recover.  resolved_hz is untouched on failure.
 *
 *   The rule is "derive the current frequency from the current hardware wherever that
 *   hardware state exists" -- not "everything must be hardware-derived".  Where a
 *   backend genuinely cannot read a value back (CK cannot observe its intended PLL
 *   input before the system switch), the honest answer is unknown; software intent
 *   must not be presented as a register readback.
 */
nora_clock_status_t
nora_clock_pll_configure(
    nora_clock_pll_t pll,
    const nora_clock_pll_config_t *config,
    uint32_t *resolved_hz);

/*
 * Switch the system clock to an oscillator (FRC, BFRC, PRIMARY, LPRC) or to a PLL
 * output (PLL_1, PLL_2) that nora_clock_pll_configure() already brought up.  What a
 * given backend actually offers is nora_clock_system_source_is_supported(); any other
 * value returns NORA_CLOCK_ERR_NOT_SUPPORTED.
 *
 * SOURCE ONLY
 *   This call changes the system-clock SOURCE.  It does not implicitly normalize or
 *   otherwise modify a backend-specific system divider.  That divider is not a
 *   portable concept: on AK it is CLKGEN1's, owned by
 *   nora_clock_dspic33ak_system_divider_set(), and CK has no writable equivalent for
 *   the sources this call exposes -- so a portable call that quietly forced it to /1
 *   would be carrying one family's policy under a portable name.  A caller that needs
 *   both states both, in the order its transition requires.
 *
 * DIRECT TRANSITION
 *   The request is for a DIRECT transition.  The HAL does not silently detour through
 *   an intermediate oscillator or clock mode to make the call appear to work: a detour
 *   re-clocks everything else timed off this clock, and only the caller knows what
 *   that is.
 *
 *   Before touching hardware, a backend must reject the request when it can determine
 *   either that
 *     - the requested direct transition itself is prohibited by that silicon
 *       (CK: PLL mode -> PLL mode; the caller passes through a non-PLL source), or
 *     - the resulting operating point would violate that silicon's clock limits
 *       (AK: selecting a ~798.72 MHz PLL2 output while the system divider is /1).
 *
 *   These are two instances of one portable rule -- do not begin a transition this
 *   silicon cannot legally complete -- which is why both live here rather than one
 *   being a frequency check and the other a backend quirk.  Both are refused as
 *   NORA_CLOCK_ERR_INVALID_ARG before the first register write, so a refusal leaves
 *   the clock exactly as it was.  nora_clock_last_diag() may identify the
 *   backend-specific precondition that made the direct transition illegal.
 *
 * SAME SOURCE = A DECLARATION UPDATE, NOT A CLOCK SWITCH
 *   If the requested source is already the one driving the system clock, there is no
 *   transition to perform, and this call does not issue a hardware clock-switch
 *   sequence.  It validates the request, applies any frequency declaration in input_hz,
 *   and returns NORA_CLOCK_OK.  Re-running a switch sequence onto the running source
 *   would be a real clock event -- a re-lock, a switch-completion poll, a window in
 *   which the part is neither on the old nor the new selection -- performed to reach
 *   the state the part is already in, which is a cost with no purpose and a risk with
 *   no benefit.  It is not an error either: asking for the state you already have is
 *   satisfied, not rejected.
 *
 *   "Already the source" is judged from the source the hardware is OBSERVED to report,
 *   never from what this HAL last programmed -- otherwise a clock failure monitor that
 *   moved the part elsewhere would turn a genuine recovery switch into a silent no-op,
 *   which is precisely when the caller most needs the switch to happen.
 *   NORA_CLOCK_SOURCE_UNKNOWN is never "the same source": a backend that cannot name
 *   what the part is running on has not established that no transition is needed, so
 *   the request is carried out.
 *
 *   Where a family folds "the PLL" into its source selection, identity of the SOURCE is
 *   not identity of that PLL's configuration -- the part can report PLL_1 while this
 *   HAL's caller believes that PLL is fed from a different input.  It is still the same
 *   source, and still a no-op.  Such a state cannot be produced through this contract:
 *   nora_clock_pll_configure() refuses to reconfigure the PLL the system clock is
 *   running from, so if it exists, something outside this HAL put it there and the
 *   OBSERVED selection is the authority on what the part is doing.  Re-issuing the
 *   switch to make the record agree would be the prohibited PLL-mode-to-PLL-mode
 *   transition above, attempted on a path the caller believes is a no-op, and taking a
 *   detour through another oscillator to make it legal is what DIRECT TRANSITION
 *   forbids.  A caller that wants that PLL configured differently switches the system
 *   clock away from it first -- the rule it already lives under.
 *
 * input_hz is that source's frequency in Hz.  Its classification and its declaration
 * semantics are nora_clock_pll_config_t.input_hz's, above -- 0 uses what the contract
 * already knows, a value equal to a contract-known frequency is accepted as a
 * restatement, a value contradicting one is NORA_CLOCK_ERR_INVALID_ARG, and a nonzero
 * value for a caller-declared source replaces any earlier declaration for that source
 * (including on the same-source path: an updated declaration is applied even though no
 * clock event occurs).  A zero never CLEARS a declaration -- "I am not restating it" is
 * not "forget it".
 *
 * AN UNKNOWN FREQUENCY IS NOT A REASON TO REFUSE
 *   Unlike nora_clock_pll_configure(), this call does not need the number: selecting a
 *   source is legal whether or not anyone can name its frequency.  If the frequency is
 *   unknown -- a caller-declared source never declared, passed as 0 -- the switch is
 *   performed, and nora_clock_get_fosc_hz() / nora_clock_state_t.fosc_hz then report 0,
 *   which is the honest answer to "what is Fosc" and the reason that 0 is a documented
 *   return.  A HAL that instead refused would be forcing callers to invent a plausible
 *   number to get a legal operation performed, and every frequency derived from that
 *   invention would be wrong while looking authoritative.
 *
 *   What an unknown frequency removes is only the FREQUENCY arm of the preflight above:
 *   an operating point that cannot be computed cannot be checked.  The silicon
 *   transition-legality arm is unaffected and still refuses.
 *
 * ON FAILURE
 *   The HAL does not deliberately disable the clock that is currently running the
 *   part: the system-clock path never clears the generator's enable to change its
 *   source, so a request that cannot complete fails as a returned status rather
 *   than as a stopped CPU.  That is the guarantee.  It is *not* a promise that the
 *   hardware is still selecting the old source on error -- a switch can be left
 *   part-way through by the sequencer, and a clock failure monitor may act on its
 *   own.  After any error, nora_clock_get_state() is the authority on what the
 *   part is actually running on; do not assume the previous configuration.
 */
nora_clock_status_t
nora_clock_switch_source(
    nora_clock_source_t source,
    uint32_t input_hz);

/*
 * Authoritative current system oscillator / instruction clock, in Hz.
 *
 * "Authoritative" means read from the hardware: the source the part is actually
 * running on, scaled by the system divider, resolved against the frequencies this
 * HAL knows or was told.  Not a compile-time constant that a runtime clock change
 * could silently invalidate, and not a cached request that a clock failure
 * monitor could have overridden.
 *
 * Returns 0 when the system clock is running from a source whose frequency the
 * HAL has not been told -- 0 is the honest answer and callers must treat it as
 * unknown rather than dividing by it.  Identical to nora_clock_get_state().fosc_hz.
 */
uint32_t nora_clock_get_fosc_hz(void);
uint32_t nora_clock_get_fcy_hz(void);

/*
 * That source's frequency in Hz, or 0 if unknown to this HAL.
 *
 * Known means one of: a frequency this contract defines (FRC, within the tuning scope
 * on NORA_CLOCK_FRC_HZ); a PLL output, derived from that PLL's current registers; or
 * a frequency a caller declared for that source through a previous call.
 *
 * "A previous call" means through ANY of this contract's calls that name the source --
 * nora_clock_pll_configure() naming it as a PLL input, or nora_clock_switch_source()
 * selecting it -- because a declaration belongs to the source and not to the API that
 * carried it.  This function is the read side of that one store.
 *
 * Deriving a PLL output READS clock registers.  It changes nothing and does not
 * require that source to be driving anything, but it is a read of present hardware
 * state rather than a lookup of a remembered request -- which is exactly why a failed
 * reconfigure cannot leave a stale frequency behind.
 *
 * WHICH MEANS 0 IS REACHABLE FOR A PLL A SUCCESSFUL CALL JUST CONFIGURED
 *   Reading a PLL back needs its input select to be readable, and on some silicon that
 *   is only true once the system clock has been switched onto the PLL: dsPIC33CK cannot
 *   observe the intended PLL input before that switch.  So between a SUCCESSFUL
 *   nora_clock_pll_configure(PLL_1, ...) and the nora_clock_switch_source(PLL_1, ...)
 *   that follows it, such a backend answers 0 for PLL_1 while a backend that can read
 *   the select answers the frequency.  This is a window in the success path, not a
 *   failure: 0 is "this HAL cannot presently derive it", never "the call did not work".
 *
 *   A consumer that needs the number in that window takes it from
 *   nora_clock_pll_configure()'s resolved_hz output, which is what the request resolved
 *   to and is available on every backend.  A backend must NOT close the window by
 *   remembering the request and reporting it here: that would make this function
 *   sometimes a readback and sometimes a record of intent, with no way for a caller to
 *   tell which one it got -- and the intent is the value that stays confidently wrong
 *   when the hardware moves.
 */
uint32_t nora_clock_source_hz(nora_clock_source_t source);

/*
 * Can the system clock be switched to that source on this backend?
 *
 * A compile-target property, not a runtime state: it answers "does this silicon offer
 * it as a system-clock selection", not "is it on".  It does not answer whether the
 * source may feed a PLL -- that is the next predicate, and the two sets differ on the
 * same part -- and it does not promise the switch will succeed: a supported source can
 * still be refused because the direct transition is prohibited or the resulting
 * operating point would be out of range.
 *
 *   AK: FRC, BFRC, PRIMARY, LPRC, PLL_1, PLL_2
 *   CK: FRC, BFRC, PRIMARY, LPRC, PLL_1
 */
bool nora_clock_system_source_is_supported(nora_clock_source_t source);

/*
 * Can that source feed that PLL on this backend?
 *
 * Also a compile-target property.  The pll parameter is part of the question, not
 * decoration: nothing says a family's PLL instances must accept the same inputs, and a
 * predicate without it could not express a part where they differ.  That both AK PLLs
 * currently accept the same set is a fact about the part, not about the contract.  A
 * pll instance this part does not have answers false.
 *
 *   AK: FRC, BFRC, PRIMARY, REFI1, REFI2
 *   CK: FRC, PRIMARY
 */
bool nora_clock_pll_input_is_supported(
    nora_clock_pll_t pll,
    nora_clock_source_t source);

/*
 * Read what the hardware is running on.  Returns NORA_CLOCK_ERR_INVALID_ARG for a
 * null pointer, otherwise NORA_CLOCK_OK -- observation cannot fail, an unknown
 * field is reported as unknown inside the struct.
 *
 * SINGLE-PASS OBSERVATION
 *   Every field, .fosc_hz included, comes from ONE pass over the clock registers.  The
 *   frequency therefore describes the same source and the same divider that the other
 *   fields describe, instead of being recomputed from a second read that could have
 *   moved in between -- a clock failure monitor can re-source the system clock without
 *   this HAL being involved.
 *
 *   "Single pass" is not "atomic".  The hardware may change underneath a reader at any
 *   point, and this HAL does not stop the clock to look at it.  What the pass buys is
 *   an internally consistent struct, not a guarantee that the part still matches it by
 *   the time the caller reads the fields.
 */
nora_clock_status_t nora_clock_get_state(nora_clock_state_t *out);

/*
 * Backend-defined detail code for the most recent clock-changing call: which
 * phase of the silicon sequence stalled, or which precondition failed.  0 means
 * "no detail".
 *
 * This is HISTORY, not state.  It is deliberately not a field of
 * nora_clock_state_t: a state struct describes the hardware now, and mixing a
 * record of the last failure into it invites code to read a stale diagnostic as a
 * present condition.
 *
 * LIFETIME
 *   Cleared to 0 when a clock-changing call starts.  0 after that call succeeds.
 *   Retained after it fails, until the next clock-changing call.  The read-only
 *   calls in this header never change it, so a failure can be reported after
 *   inspecting the state it produced.
 *
 *   "A clock-changing call" is by API, not by whether hardware moved: a
 *   nora_clock_switch_source() that finds the part already on the requested source
 *   performs no clock event, but it is still one of these calls and still clears this
 *   to 0 -- it succeeded.  A stale nonzero code surviving a successful call would make
 *   the no-op path look like the one failure mode that leaves a diagnostic behind.
 *
 * The values are the backend's own (nora_clock_dspic33ak_diag_t /
 * nora_clock_dspic33ck_diag_t); a portable
 * consumer may print it and must not branch on it.
 */
uint16_t nora_clock_last_diag(void);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_H */
