#ifndef DSPIC33AK_CLOCK_REG_H
#define DSPIC33AK_CLOCK_REG_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"   /* nora_clock_dspic33ak_clkgen_t, _diag_t */

/*
 * Internal dsPIC33AK register programming layer.
 *
 * This file is intentionally below the public Clock HAL API.  It owns the raw
 * PLLx / CLKx SFR access, XC-DSC bitfield names, DFP-provided register layout,
 * switch sequencing, ready polling, and timeout reporting.  Callers provide
 * already-encoded register fields; board policy and logical source names do not
 * belong in this layer.
 *
 * It is the only translation unit in the HAL that includes <xc.h>.  That is why
 * the two public register-capture functions declared in nora_clock_dspic33ak.h
 * are also defined here: their whole body is SFR reads, and moving them up to the
 * core would only put SFR names in a second file.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t source;
    uint32_t feedback_div;
    uint16_t pre_div;
    uint16_t post_div1;
    uint16_t post_div2;
} dspic33ak_clock_reg_pll_config_t;

typedef struct {
    uint16_t source;
    uint16_t intdiv;
    uint16_t fracdiv;
} dspic33ak_clock_reg_clkgen_config_t;

/*
 * One read of everything nora_clock_get_state() needs, as raw field values.
 * Undecoded on purpose: turning a NOSC encoding into a logical source name is the
 * device layer's table, and turning INTDIV/FRACDIV back into a divisor is the
 * core's -- this layer reports what the registers say.
 */
typedef struct {
    uint16_t source;      /* CLK1CONbits.COSC   */
    uint16_t intdiv;      /* CLK1DIVbits.INTDIV */
    uint16_t fracdiv;     /* CLK1DIVbits.FRACDIV */
    bool     ready;       /* CLK1CONbits.CLKRDY */
    bool     pll1_ready;  /* OSCCTRLbits.PLL1RDY */
    bool     pll2_ready;  /* OSCCTRLbits.PLL2RDY */
} dspic33ak_clock_reg_system_t;

nora_clock_status_t dspic33ak_clock_reg_pll_configure(
    nora_clock_pll_t pll,
    const dspic33ak_clock_reg_pll_config_t *config);

nora_clock_status_t dspic33ak_clock_reg_clkgen_configure(
    nora_clock_dspic33ak_clkgen_t clkgen,
    const dspic33ak_clock_reg_clkgen_config_t *config);

/*
 * The state of one PLL as its own registers report it, as raw field values.
 *
 * Read back rather than remembered: the frequency a PLL carries is reconstructed
 * from these fields, so a programming sequence that failed half way through cannot
 * leave a previous solver result standing as the current truth.  Undecoded for the
 * same reason as the system struct above -- the NOSC encoding is the device layer's
 * table, and the arithmetic is the core's.
 */
typedef struct {
    uint16_t source;        /* PLLxCONbits.NOSC    */
    uint32_t feedback_div;  /* PLLxDIVbits.PLLFBDIV */
    uint16_t pre_div;       /* PLLxDIVbits.PLLPRE   */
    uint16_t post_div1;     /* PLLxDIVbits.POSTDIV1 */
    uint16_t post_div2;     /* PLLxDIVbits.POSTDIV2 */
    bool     enabled;       /* PLLxCONbits.ON       */
    bool     ready;         /* OSCCTRLbits.PLLxRDY  */
} dspic33ak_clock_reg_pll_state_t;

/*
 * Re-source and re-divide CLKGEN1 -- the generator whose output is the system
 * clock, i.e. the one clocking the CPU that is executing the call.  Separate from
 * dspic33ak_clock_reg_clkgen_configure() because the safe sequence is different,
 * not because the register block is; see the comment on the implementation.
 *
 * Composed of the two primitives below, in that order.  The split exists because
 * the portable face changes the source ONLY: the two halves are different hardware
 * decisions with different safe orderings, and a caller that needs both says so.
 */
nora_clock_status_t dspic33ak_clock_reg_system_switch(
    const dspic33ak_clock_reg_clkgen_config_t *config);

nora_clock_status_t dspic33ak_clock_reg_system_switch_source(uint16_t source);

nora_clock_status_t dspic33ak_clock_reg_system_set_divider(
    uint16_t intdiv,
    uint16_t fracdiv);

void dspic33ak_clock_reg_read_system(dspic33ak_clock_reg_system_t *out);

/*
 * Fields of PLL1 or PLL2.  Reports zeros for an instance this part does not have,
 * so a record that could not be taken does not look taken.
 */
void dspic33ak_clock_reg_read_pll(
    nora_clock_pll_t pll,
    dspic33ak_clock_reg_pll_state_t *out);

/*
 * Backend-private diagnostic latch.  Defined by the HAL core, which owns its
 * lifetime and publishes it through nora_clock_last_diag(); declared here because
 * this is the header the two backend translation units share, and the register
 * layer is where the phase of a stalled sequence is known.
 */
void dspic33ak_clock_diag_set(nora_clock_dspic33ak_diag_t diag);

#ifdef __cplusplus
}
#endif

#endif /* DSPIC33AK_CLOCK_REG_H */
