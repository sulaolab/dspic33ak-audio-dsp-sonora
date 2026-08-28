#ifndef NORA_CLOCK_DSPIC33AK_H
#define NORA_CLOCK_DSPIC33AK_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"

/*
 * dsPIC33AK-only part of the Clock HAL: the CLKGEN blocks, the register capture,
 * and the backend's diagnostic codes.
 *
 * WHY THIS HEADER EXISTS
 *   CLKGEN is not a clock concept, it is a dsPIC33AK BLOCK. The AK clock tree is a
 *   fan-out: two PLLs feed a set of numbered CLKGEN generators, each with its own
 *   source select and divider, and each feeding a different consumer (CLKGEN1 is
 *   the CPU's, CLKGEN13 is the CCP/SCCP time base, ...). dsPIC33C/CK has no such
 *   block at all -- it has one system PLL switched by OSCCON.OSWEN, and peripheral
 *   clocks are per-peripheral selects owned by those peripherals' own HALs
 *   (e.g. UART BCLKSEL).
 *
 *   So there is no CK implementation of these calls to write, and there never will
 *   be. Leaving them on nora_clock.h made that header look portable while its only
 *   entry point was AK-shaped. Naming them nora_clock_dspic33ak_* puts the fact at
 *   every call site instead: board code that programs a CLKGEN is AK-specific and
 *   should read that way, and board code that only needs "what is Fcy" or "switch
 *   the system clock" uses the portable face in nora_clock.h and ports as-is.
 *
 * WHO MAY INCLUDE THIS
 *   Board / platform bring-up that genuinely owns the AK clock tree
 *   (board/clock/, the resident engine's boot platform, UART board wiring).
 *   Application and DSP code must not: if application code needs a frequency, it
 *   wants nora_clock_get_fcy_hz().
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NORA_CLOCK_DSPIC33AK_CLKGEN_1  = 1,
    NORA_CLOCK_DSPIC33AK_CLKGEN_5  = 5,
    NORA_CLOCK_DSPIC33AK_CLKGEN_6  = 6,
    NORA_CLOCK_DSPIC33AK_CLKGEN_8  = 8,
    NORA_CLOCK_DSPIC33AK_CLKGEN_9  = 9,
    NORA_CLOCK_DSPIC33AK_CLKGEN_10 = 10,
    NORA_CLOCK_DSPIC33AK_CLKGEN_12 = 12,
    /* CLKGEN13 is the CCP/SCCP time-base generator: it is what CCPxCON1.CLKSEL=1
     * selects, so it is the one generator that moves a capture time base without
     * touching any other peripheral. */
    NORA_CLOCK_DSPIC33AK_CLKGEN_13 = 13
} nora_clock_dspic33ak_clkgen_t;

typedef struct {
    nora_clock_source_t source;
    uint16_t divide_by;
} nora_clock_dspic33ak_clkgen_config_t;

/*
 * Configure a supported CLKGEN instance from a logical source and divider.
 *
 * Configuring CLKGEN1 also changes what nora_clock_get_fosc_hz() reports, because
 * on this family CLKGEN1's output IS the system clock: Fosc = source / divide_by.
 * The frequency is derived from what the HAL knows about that source (the contract's
 * FRC frequency, a PLL read back from its own registers, or a frequency a caller
 * declared); for a source it was never told, Fosc reads back as 0 (unknown) rather
 * than guessed. Declare it with nora_clock_switch_source() if a caller ever needs
 * that case.
 *
 * CLKGEN1 is routed through the same system-clock switch sequence as
 * nora_clock_switch_source() -- it does not clear the generator's enable to change
 * its source, because CLKGEN1 is clocking the CPU that is executing this call.
 * The other generators use the general sequence, which does.
 *
 * NOTE that this call re-sources CLKGEN1 as well as re-dividing it, and does not
 * preflight the resulting Fosc. It is the raw AK face: a caller that wants only the
 * divider, checked, uses nora_clock_dspic33ak_system_divider_set() below.
 */
nora_clock_status_t
nora_clock_dspic33ak_clkgen_configure(
    nora_clock_dspic33ak_clkgen_t clkgen,
    const nora_clock_dspic33ak_clkgen_config_t *config);

/*
 * Change the system clock's divider, leaving its source alone.
 *
 * This exists because nora_clock_switch_source() no longer touches it. The portable
 * call used to force this divider to /1 as a side effect, which made a portable
 * operation carry an AK-only policy -- CK has no writable equivalent for the sources
 * that API exposes. The two halves are now separate, and this is the AK-only half:
 *
 *     nora_clock_switch_source()                -> system source, portable
 *     nora_clock_dspic33ak_system_divider_set() -> system divider, AK only
 *
 * A caller that needs both states which order its transition requires. Moving to a
 * slower source before lowering the divider is safe; the reverse can overshoot, which
 * is why this call and the switch each refuse a resulting Fosc above what this
 * backend will select (NORA_CLOCK_ERR_INVALID_ARG, with a diagnostic code).
 *
 * That check needs the current source's frequency, and when this HAL has not been told
 * it, the check is skipped rather than turned into a refusal -- matching the portable
 * contract's rule that an unknown frequency is never itself a reason to refuse. An
 * unnameable frequency means this call cannot promise the resulting operating point is
 * legal; it does not mean the caller is denied the divider it asked for.
 *
 * divide_by is the true divisor, 1..: 1 and even values are exact, an odd value above
 * 1 uses the generator's half step. A value the divider fields cannot express returns
 * NORA_CLOCK_ERR_UNREPRESENTABLE rather than the nearest one that can.
 */
nora_clock_status_t
nora_clock_dspic33ak_system_divider_set(uint16_t divide_by);

/* -------------------------------------------------------------------------- */
/* Backend diagnostic codes                                                   */
/* -------------------------------------------------------------------------- */

/*
 * What nora_clock_last_diag() returns on this backend. Implementation-defined:
 * these name phases of the dsPIC33AK oscillator sequence and preconditions of
 * this backend, so a portable consumer prints the number and does not branch on
 * it. Values are assigned because they get printed and logged.
 */
typedef enum {
    NORA_CLOCK_DSPIC33AK_DIAG_NONE                  = 0,

    /* A phase of a silicon switch sequence did not complete in the polling
     * budget. These are the values the collapsed NORA_CLOCK_ERR_TIMEOUT keeps. */
    NORA_CLOCK_DSPIC33AK_DIAG_DIV_SWITCH_TIMEOUT    = 1,
    NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_SWITCH_TIMEOUT = 2,
    NORA_CLOCK_DSPIC33AK_DIAG_CLKRDY_TIMEOUT        = 3,
    NORA_CLOCK_DSPIC33AK_DIAG_PLL_SWITCH_TIMEOUT    = 4,
    NORA_CLOCK_DSPIC33AK_DIAG_FOUT_SWITCH_TIMEOUT   = 5,
    NORA_CLOCK_DSPIC33AK_DIAG_PLL_LOCK_TIMEOUT      = 6,

    /* Preconditions this backend refuses rather than attempts. */
    NORA_CLOCK_DSPIC33AK_DIAG_PLL_NOT_READY         = 7,
    NORA_CLOCK_DSPIC33AK_DIAG_PLL_DRIVES_SYSTEM     = 8,
    NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_UNKNOWN     = 9,
    NORA_CLOCK_DSPIC33AK_DIAG_NO_DIVIDER_SOLUTION   = 10,

    /* The resulting system clock would be above what this backend selects. Raised
     * by both system-clock mutators, before either writes anything. */
    NORA_CLOCK_DSPIC33AK_DIAG_FOSC_OVER_LIMIT       = 11,

    /* CLKGEN1's divider fields hold a combination this HAL cannot read as a divisor,
     * so the resulting frequency cannot be checked. Refused rather than assumed to
     * be /1, which would wave through exactly the case the check is for. */
    NORA_CLOCK_DSPIC33AK_DIAG_SYSTEM_DIVIDER_UNKNOWN = 12,

    /* A caller declared a frequency that contradicts one this HAL determines (the
     * contract's FRC frequency, or a PLL output read from its registers). Restating
     * the known value is accepted; disagreeing with it is not. */
    NORA_CLOCK_DSPIC33AK_DIAG_SOURCE_HZ_CONFLICT    = 13
} nora_clock_dspic33ak_diag_t;

/* -------------------------------------------------------------------------- */
/* Register capture                                                           */
/* -------------------------------------------------------------------------- */

/*
 * A snapshot of the oscillator control words, for a post-mortem record or a
 * bring-up dump.
 *
 * This is deliberately typed, named per-backend, and split by scope. A caller that
 * wants these words is depending on dsPIC33AK register layout -- the type it names
 * says so, and the field list is exactly the register set it depends on, so
 * porting it fails to compile instead of silently decoding a different silicon's
 * bits. Anything that is a *question* about the clock rather than a record of it
 * belongs in nora_clock_get_state(), which needs no decode at the call site.
 *
 * The fields are raw register values, not decoded. Their bit layout is the device
 * datasheet's, and it is not this HAL's contract.
 */
typedef struct {
    uint32_t oscctrl;
    uint32_t clkfail;
    uint32_t pll1con;
    uint32_t pll1div;
    uint32_t pll2con;
    uint32_t pll2div;
} nora_clock_dspic33ak_raw_t;

void nora_clock_dspic33ak_raw_capture(nora_clock_dspic33ak_raw_t *out);

/*
 * The same, per generator: one CLKGEN's control and divider words. Separate from
 * the above because the generators are an array -- a record that wants CLKGEN1,
 * CLKGEN6 and CLKGEN8 calls this three times rather than forcing every caller to
 * carry a struct sized for the generators it does not use.
 */
typedef struct {
    uint32_t con;
    uint32_t div;
} nora_clock_dspic33ak_clkgen_raw_t;

void nora_clock_dspic33ak_clkgen_raw_capture(
    nora_clock_dspic33ak_clkgen_t clkgen,
    nora_clock_dspic33ak_clkgen_raw_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_DSPIC33AK_H */
