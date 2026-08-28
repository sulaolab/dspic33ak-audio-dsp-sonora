#include "board/clock/sonora_clock.h"

#include <stdint.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"   /* AK-only: this board owns the CLKGEN tree */
#include "nora_pps.h"

/* PLL1's input is the internal FRC, so this is the contract's FRC frequency rather
 * than a second copy of 8000000: restating the contract-known value is accepted by
 * nora_clock_pll_configure(), contradicting it is refused, and a local literal is
 * exactly how the two would drift apart. */
#define SONORA_BOOT_PLL1_INPUT_HZ      (NORA_CLOCK_FRC_HZ)

static bool configure_boot_clkgen(nora_clock_dspic33ak_clkgen_t clkgen);
static bool route_pwm_refi1(void);
#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
static bool route_ccp_refi1_xtalout(void);
#endif

/* Set on the way out of sonora_clock_boot_init() so the LED fault indicator can
 * name the step that failed -- see SONORA_CLOCK_FAIL_* in sonora_clock.h. */
static unsigned s_boot_fail_stage = SONORA_CLOCK_FAIL_NONE;

unsigned sonora_clock_boot_fail_stage(void) { return s_boot_fail_stage; }

/* -------------------------------------------------------------------------- */
/* Boot clock: FRC -> PLL1 -> CLKGEN1/6/9. One path, no alternatives.          */
/* -------------------------------------------------------------------------- */
/*
 * There is deliberately no second source and no fallback here. A PLL only
 * re-locks across a non-power reset to the configuration it was already locked
 * to; a fallback would make two PLL1 configurations reachable on one board, and
 * the first reflash across them hangs the handshake unrecoverably (only a power
 * cycle clears it). If this fails, the caller reports and stops -- see
 * [internal] clock_design_contract.md.
 */
bool sonora_clock_boot_init(void)
{
    nora_clock_state_t state;
    const nora_clock_pll_config_t pll1_config = {
        .source    = NORA_CLOCK_SOURCE_FRC,
        .input_hz  = SONORA_BOOT_PLL1_INPUT_HZ,
        .target_hz = PLL1_CLK_HZ,
    };

    s_boot_fail_stage = SONORA_CLOCK_FAIL_NONE;

    if (nora_clock_pll_configure(
            NORA_CLOCK_PLL_1,
            &pll1_config,
            0) != NORA_CLOCK_OK) {
        s_boot_fail_stage = SONORA_CLOCK_FAIL_PLL1_FRC;
        return false;
    }

    /*
     * The CPU's own generator goes through the PORTABLE face. CLKGEN1's output is
     * the system clock, so this is "switch the system clock", not "program one of
     * the AK generators" -- and it is the path that takes the safe live re-source
     * sequence rather than the general one that drops the generator's enable first
     * (see the Q27C note further down for what that did to a running CPU).
     *
     * PLL1's frequency is not restated: the HAL reads it back from PLL1's own
     * registers.
     *
     * The divider is named explicitly, and BEFORE the switch. The portable call
     * changes the source only -- it no longer forces CLKGEN1 to /1 as a side effect,
     * because that divider is an AK concept -- so this board states the operating
     * point it wants instead of inheriting whatever the configuration words left
     * here. Divider first is the safe order for THIS transition: /1 is applied while
     * the system clock is still FRC, and the frequency only rises once, at the switch
     * onto PLL1. Both steps report the same LED stage: they are one decision, "run
     * the CPU from PLL1 undivided", and splitting the indicator would not tell the
     * board bring-up anything it could act on differently.
     */
    if ((nora_clock_dspic33ak_system_divider_set(
             SONORA_CLOCK_BOOT_CLKGEN_DIVIDE_BY) != NORA_CLOCK_OK) ||
        (nora_clock_switch_source(NORA_CLOCK_SOURCE_PLL_1, 0u) != NORA_CLOCK_OK)) {
        s_boot_fail_stage = SONORA_CLOCK_FAIL_CLKGEN1;
        return false;
    }

    /*
     * Verify against the hardware, not against the request. The check that used to
     * sit above compared the rate the HAL returned with the rate just asked for,
     * which cannot fail -- code 10 was unreachable. This asks what the part is
     * actually running on, whether that source is running and locked, and at what
     * Fosc, which is the question the code was always meant to answer.
     */
    if ((nora_clock_get_state(&state) != NORA_CLOCK_OK) ||
        (state.source != NORA_CLOCK_SOURCE_PLL_1) ||
        !state.ready ||
        !state.locked ||
        (state.fosc_hz != PLL1_CLK_HZ)) {
        s_boot_fail_stage = SONORA_CLOCK_FAIL_PLL1_FRC_RATE;
        return false;
    }

    /* Split per CLKGEN so the LED names which one stalled. These two feed
     * peripherals, so they take the general AK sequence; the CPU's generator was
     * switched above. */
    if (!configure_boot_clkgen(NORA_CLOCK_DSPIC33AK_CLKGEN_6)) {
        s_boot_fail_stage = SONORA_CLOCK_FAIL_CLKGEN6;
        return false;
    }
    if (!configure_boot_clkgen(NORA_CLOCK_DSPIC33AK_CLKGEN_9)) {
        s_boot_fail_stage = SONORA_CLOCK_FAIL_CLKGEN9;
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* The one PLL2 configuration request in the project                           */
/* -------------------------------------------------------------------------- */
sonora_clock_pwm_status_t sonora_clock_pll2_from_refi1(nora_clock_status_t *detail)
{
    /*
     * REFI1 12.288 MHz -> 798.72 MHz. Consumers differ only in which CLKGEN they
     * then divide this into (CLKGEN5 for PWM MCLK, CLKGEN9 for the SPI2 transport
     * clock, CLKGEN13 for the CCP capture time base), never in the PLL2 request
     * itself. See the header for why that has to stay true.
     */
    const nora_clock_pll_config_t pll2_config = {
        .source    = NORA_CLOCK_SOURCE_REFI1,
        .input_hz  = SONORA_CLOCK_PWM_REFI1_HZ,
        .target_hz = SONORA_CLOCK_PWM_PLL2_HZ,
    };
    const nora_clock_status_t status =
        nora_clock_pll_configure(NORA_CLOCK_PLL_2, &pll2_config, 0);

    if (detail != 0) {
        *detail = status;
    }
    return (status == NORA_CLOCK_OK) ? SONORA_CLOCK_PWM_OK
                                          : SONORA_CLOCK_PWM_ERR_PLL2;
}

/* -------------------------------------------------------------------------- */
/* SPI2 transport clock from PLL2 (application-side, opt-in)                   */
/* -------------------------------------------------------------------------- */
#if RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2
sonora_clock_pwm_status_t sonora_clock_spi2_from_pll2(nora_clock_status_t *detail)
{
    sonora_clock_pwm_status_t pll2_result;
    nora_clock_status_t  status;
    /*
     * PLL2 <- REFI1 <- RP16 (codec-A's 12.288 MHz clock output -- XTALout on the
     * Rev.4 PCB), then CLKGEN9 /4 = 199.68 MHz for SPI2's transport clock, coherent
     * with the codec. SysCLK stays on PLL1 <- FRC.
     *
     * RP16 is hard-coded here, deliberately not route_pwm_refi1(): this path has
     * only ever been used and verified on the Rev.4 route. Coherence is all this
     * consumer needs, so RP75 would in principle do -- but widening to it is a
     * separate change that must re-check the fixed input_hz, not a comment edit.
     */
    const nora_clock_dspic33ak_clkgen_config_t clkgen9_config = {
        .source    = NORA_CLOCK_SOURCE_PLL_2,
        .divide_by = SONORA_CLOCK_SPI2_PLL2_DIVIDE_BY,
    };

    if (detail != 0) {
        *detail = NORA_CLOCK_OK;
    }

    /* REFI1 reads an external clock, so this owns the pin's input setup
     * (ANSEL = 0 + TRIS = input) via the pinmux helper. */
    if (!nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 16u)) {
        return SONORA_CLOCK_PWM_ERR_REFI1_ROUTE;
    }
    pll2_result = sonora_clock_pll2_from_refi1(detail);
    if (pll2_result != SONORA_CLOCK_PWM_OK) {
        return pll2_result;
    }
    status = nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_9, &clkgen9_config);
    if (status != NORA_CLOCK_OK) {
        if (detail != 0) { *detail = status; }
        return SONORA_CLOCK_PWM_ERR_CLKGEN9;
    }
    return SONORA_CLOCK_PWM_OK;
}
#endif /* RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2 */

/* -------------------------------------------------------------------------- */
/* CCP capture time base from PLL2 (application-side, opt-in)                  */
/* -------------------------------------------------------------------------- */
#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
sonora_clock_pwm_status_t sonora_clock_ccp_timebase_from_pll2(nora_clock_status_t *detail)
{
    sonora_clock_pwm_status_t pll2_result;
    nora_clock_status_t  status;
    /* PLL2 798.72 MHz / 8 = 99.84 MHz into CLKGEN13, the CCP/SCCP time-base
     * generator. Nothing but the capture time base moves. */
    const nora_clock_dspic33ak_clkgen_config_t clkgen13_config = {
        .source    = NORA_CLOCK_SOURCE_PLL_2,
        .divide_by = SONORA_CLOCK_CCP_PLL2_DIVIDE_BY,
    };

    if (detail != 0) {
        *detail = NORA_CLOCK_OK;
    }

    /* Deliberately NOT the board-aware route_pwm_refi1(): this path must have
     * codec-A's XTALout, a fixed 12.288 MHz that does not follow fs, and only the
     * Rev.4 RP16 route is that signal. Following "whichever pin the board uses"
     * would silently accept the legacy RP75 route (documented as BCLK, so it moves
     * with BCLK_DIV), the USB source's MCLK on RP26, or AK128's RP33 -- and PLL2's
     * input_hz is a fixed 12.288 MHz constant, so a wrong pin does not fail loudly,
     * it just reports every rate wrong. resolved_board_config.h refuses those
     * builds at compile time; this hard-coded RP16 is the other half of that
     * contract. */
    if (!route_ccp_refi1_xtalout()) {
        return SONORA_CLOCK_PWM_ERR_REFI1_ROUTE;
    }
    pll2_result = sonora_clock_pll2_from_refi1(detail);
    if (pll2_result != SONORA_CLOCK_PWM_OK) {
        return pll2_result;
    }
    status = nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_13, &clkgen13_config);
    if (status != NORA_CLOCK_OK) {
        if (detail != 0) { *detail = status; }
        return SONORA_CLOCK_PWM_ERR_CLKGEN13;
    }
    return SONORA_CLOCK_PWM_OK;
}
#endif /* RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2 */

/* -------------------------------------------------------------------------- */
/* PWM audio MCLK: REFI1 -> PLL2 -> CLKGEN5                                    */
/* -------------------------------------------------------------------------- */
sonora_clock_pwm_status_t sonora_clock_pwm_prepare(nora_clock_status_t *detail)
{
    sonora_clock_pwm_status_t pll2_result;
    nora_clock_status_t  status;
    const nora_clock_dspic33ak_clkgen_config_t clkgen5_config = {
        .source    = NORA_CLOCK_SOURCE_PLL_2,
        .divide_by = SONORA_CLOCK_PWM_CLKGEN5_DIVIDE_BY,
    };

    if (detail != 0) {
        *detail = NORA_CLOCK_OK;
    }

    if (!route_pwm_refi1()) {
        return SONORA_CLOCK_PWM_ERR_REFI1_ROUTE;
    }

    pll2_result = sonora_clock_pll2_from_refi1(detail);
    if (pll2_result != SONORA_CLOCK_PWM_OK) {
        return pll2_result;
    }

    status = nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_5, &clkgen5_config);
    if (status != NORA_CLOCK_OK) {
        if (detail != 0) {
            *detail = status;
        }
        return SONORA_CLOCK_PWM_ERR_CLKGEN5;
    }

    return SONORA_CLOCK_PWM_OK;
}

#if RESOLVED_BOARD_COHERENT_OFFSET_CLOCK
sonora_clock_pwm_status_t sonora_clock_q27b_coherent_clkgen9(nora_clock_status_t *detail)
{
    sonora_clock_pwm_status_t pll2_result;
    nora_clock_status_t  status;
    /* Same REFI1 12.288 MHz -> PLL2 798.72 MHz path the PWM policy already proves works, but the
     * PLL2 output is divided into CLKGEN9 (SPI2's transport clock) instead of CLKGEN5, so PLL2 --
     * and therefore SPI2's generated BCLK -- is coherent with A. CPU stays on PLL1 (CLKGEN1); DSP
     * SYS unchanged.
     *
     * REFI1 is routed from RP16 below, hard-coded -- NOT route_pwm_refi1(); an earlier version of
     * this comment claimed the RP75 board default and disagreed with the code right beneath it. */
    const nora_clock_dspic33ak_clkgen_config_t clkgen9_config = {
        .source    = NORA_CLOCK_SOURCE_PLL_2,
        .divide_by = SONORA_CLOCK_Q27B_CLKGEN9_DIVIDE_BY,   /* -> 199.68 MHz */
    };

    if (detail != 0) {
        *detail = NORA_CLOCK_OK;
    }
    /* Route REFI1 from RP16 (= RA15), which on the unmodified, standard Rev.4 codec board carries
     * WM8904-A's XTAL_OUT: a buffered, fixed 12.288 MHz that does NOT follow fs (measured, see
     * [internal] clock_design_contract.md). The net is DIM connector pin 79, which the DIM Info Sheet
     * (DS70005563, Table 1/2) maps to Device Pin 20 = CVDAN15/RP16/RA15 -- i.e. "DIM-P79" is the
     * connector pin, NOT RP79. This is not BCLK and not a raw crystal node either: the crystal only
     * ever connects to WM8904, never to the dsPIC (confirmed against the Rev.4 schematic, 2026-08-18). */
    /* REFI1 reads an external clock: own the pin's input setup (ANSEL=0 + TRIS=input) via pinmux. */
    if (!nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 16u)) {
        return SONORA_CLOCK_PWM_ERR_REFI1_ROUTE;
    }
    pll2_result = sonora_clock_pll2_from_refi1(detail);
    if (pll2_result != SONORA_CLOCK_PWM_OK) {
        return pll2_result;
    }
    status = nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_9, &clkgen9_config);
    if (status != NORA_CLOCK_OK) {
        if (detail != 0) { *detail = status; }
        return SONORA_CLOCK_PWM_ERR_CLKGEN9;
    }
    /* NOTE: an earlier variant of this function also re-sourced CLKGEN1 (CPU) from the now-locked
     * PLL2 as a LIVE switch of the running CPU clock ("Q27C"). That live switch was verified to hang
     * the CPU (the CLKGEN macro drops ON before switching the source) and was removed. */
    return SONORA_CLOCK_PWM_OK;
}
#endif /* RESOLVED_BOARD_COHERENT_OFFSET_CLOCK */

static bool configure_boot_clkgen(nora_clock_dspic33ak_clkgen_t clkgen)
{
    const nora_clock_dspic33ak_clkgen_config_t config = {
        .source    = NORA_CLOCK_SOURCE_PLL_1,
        .divide_by = SONORA_CLOCK_BOOT_CLKGEN_DIVIDE_BY,
    };

    return nora_clock_dspic33ak_clkgen_configure(clkgen, &config) ==
           NORA_CLOCK_OK;
}

static bool route_pwm_refi1(void)
{
    /*
     * This function owns the full setup its job needs: make the pin a usable clock
     * INPUT for REFI1. That means digital (ANSEL=0) AND direction=input -- REFI1's
     * source is always an EXTERNAL clock (codec BCLK / PICO2 MCLK) that some other
     * device drives, so the dsPIC only receives on this pin. Use the combined
     * pinmux helper so the pin no longer leans on the boot-time blind ANSELx=0 clear.
     */
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE
    /* SPI: SCK/P83 DIM-P83 OSCO/CLKO/RP33/IOMF5/RC0 */
    return nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 33u);

#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
 #if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    /*
     * USB-audio: the PICO2 source's 12.288 MHz MCLK enters on RP26/RB9
     * (CLCINC, fanned to codec-B MCLK via CLC3 -> RP97). The default
     * RP16/RP75 codec-PCB pins carry no clock in this mode.
     */
    return nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 26u);

 #elif RESOLVED_BOARD_CODEC_REFI1_ON_RP16
    /* Only Rev.4 PCB (white) routes codec-A's XTAL_OUT to RP16. */
    return nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 16u);

 #else
    /* Legacy fallback: BCLK 12.288 MHz enters REFI1 through RP75. */
    return nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 75u);
 #endif

#else
    #error "Unhandled resolved board target in sonora_clock.c PWM REFI1 route"
#endif
}

#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
/*
 * REFI1 route for the CCP capture time base -- RP16 ONLY, no board dispatch.
 *
 * The distinction that matters is WHICH SIGNAL reaches REFI1, not which pin:
 *
 *   RP16 (Rev.4 PCB)  WM8904-A XTALout, fixed 12.288 MHz, fs-INDEPENDENT.
 *                     Measured: a 48k -> 16k codec-A rate change moved BCLK_DIV
 *                     0x0 -> 0x3 and the reported rate stayed exact.
 *   RP75 (legacy)     documented as BCLK -- if so it scales with BCLK_DIV, i.e.
 *                     fs-DEPENDENT. Not verified for this purpose.
 *   RP26 (USB audio)  the USB source's MCLK, a different device entirely.
 *   RP33 (AK128)      OSCO/CLKO, not the codec at all.
 *
 * Only the first is a valid reference for a FIXED PLL2 input_hz, and a wrong one
 * would not fail loudly -- PLL2 would lock happily and every reported rate would
 * be wrong by the ratio. resolved_board_config.h rejects the other three at
 * compile time; keeping this route hard-coded means the two cannot drift apart.
 */
static bool route_ccp_refi1_xtalout(void)
{
    return nora_pinmux_route_input(NORA_PPS_INPUT_REFI1, 16u);
}
#endif /* RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2 */
