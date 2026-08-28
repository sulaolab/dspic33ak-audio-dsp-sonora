// Sonora board clock policy and routing.
#ifndef SONORA_CLOCK_H
#define SONORA_CLOCK_H

#include <stdbool.h>

#include "resolved_board_config.h"
#include "nora_clock.h"
#include "nora_gpio.h"

/*
 * Physical PPS-capable pins that carry the two ASRC clock-domain signals.
 * Keep board wiring here rather than in ASRC feature code: an AK/CK port can
 * change these board facts without changing the feature's capture logic.
 */
#define BOARD_ASRC_CLOCK_A_RP  ((nora_gpio_rp_t)70u)
#define BOARD_ASRC_CLOCK_B_RP  ((nora_gpio_rp_t)29u)

/*
 * The clock structure is deliberately fixed and unconditional:
 *
 *   PLL1 <- FRC, 200 MHz      ALWAYS, every build. CPU / SysCLK / CLKGEN1,6,9.
 *   PLL2 <- REFI1, 798.72 MHz only where an application needs a codec-coherent
 *                             clock, and always that same configuration.
 *
 * This is not a style choice. A PLL only re-locks across a non-power reset to
 * the configuration it was already locked to; asking for a different one hangs
 * and latches, so the board then needs a power cycle. Any per-application
 * variation -- including a fallback -- makes two configurations reachable on one
 * board and reintroduces that failure. Do NOT add a fallback or a second PLL1
 * source here. See [internal] clock_design_contract.md.
 */

/* Boot PLL1 feeds the application CLKGENs prepared by sonora_clock_boot_init(). */
#define SONORA_CLOCK_BOOT_CLKGEN_DIVIDE_BY     (1u)
#define SONORA_CLOCK_BOOT_CLKGEN_HZ            (PLL1_CLK_HZ / SONORA_CLOCK_BOOT_CLKGEN_DIVIDE_BY)

/* SPI/I2S/TDM consumes CLKGEN9 when dsPIC is the transport clock master. */
#define SONORA_CLOCK_SPI_TDM_CLKGEN9_HZ        (SONORA_CLOCK_BOOT_CLKGEN_HZ)

/* PLL2(798.72 MHz)/4 = 199.68 MHz for CLKGEN9 when SPI2's transport clock is
 * taken from PLL2 instead of PLL1. */
#define SONORA_CLOCK_SPI2_PLL2_DIVIDE_BY       (4u)

/* PWM upstream clock policy: external 12.288 MHz REFI1 -> PLL2 -> CLKGEN5. */
#define SONORA_CLOCK_PWM_REFI1_HZ          (12288000UL)
#define SONORA_CLOCK_PWM_PLL2_MULTIPLIER   (65UL)
#define SONORA_CLOCK_PWM_PLL2_HZ           (SONORA_CLOCK_PWM_REFI1_HZ * SONORA_CLOCK_PWM_PLL2_MULTIPLIER)
#define SONORA_CLOCK_PWM_CLKGEN5_DIVIDE_BY (1u)
#define SONORA_CLOCK_PWM_MCLK_HZ           (SONORA_CLOCK_PWM_PLL2_HZ / SONORA_CLOCK_PWM_CLKGEN5_DIVIDE_BY)

/* CCP capture time base from PLL2: 798.72 MHz / 8 = 99.84 MHz.
 *
 * The 8 is chosen so the new time base lands within 0.16 % of the FCY 100 MHz one
 * it replaces -- capture counts per interval, 32-bit rollover, and the ISR budget
 * all stay effectively unchanged, which makes this a safe substitution rather than
 * a re-tune. It is also exactly rational with the codec: 99.84 / 12.288 = 8.125,
 * so the time base and the sample rates it measures share one crystal and the
 * measurement's frequency bias is structurally zero rather than merely small. */
#define SONORA_CLOCK_CCP_PLL2_DIVIDE_BY    (8u)
#define SONORA_CLOCK_CCP_PLL2_HZ           (SONORA_CLOCK_PWM_PLL2_HZ / SONORA_CLOCK_CCP_PLL2_DIVIDE_BY)

typedef enum {
    SONORA_CLOCK_PWM_OK = 0,
    SONORA_CLOCK_PWM_ERR_REFI1_ROUTE,
    SONORA_CLOCK_PWM_ERR_PLL2,
    SONORA_CLOCK_PWM_ERR_CLKGEN5,
    SONORA_CLOCK_PWM_ERR_CLKGEN9,
    SONORA_CLOCK_PWM_ERR_CLKGEN13
} sonora_clock_pwm_status_t;

/*
 * Which step of sonora_clock_boot_init() failed. main.c hands this straight to
 * LED_fault_indicate_forever(), because when the clock bring-up is what broke
 * there is no UART yet and the LED bank is the only channel that exists.
 *
 * Values start at 8 to stay clear of the BOOT_FAULT_* codes in main.c, and show
 * in binary on LED1.. (LED0 is the heartbeat):
 *
 *   9  = 1001  PLL1 <- FRC did not lock
 *   10 = 1010  the system clock is not running from PLL1 at the expected rate --
 *              read back from the hardware after the switch, not compared against
 *              the request
 *   11 = 1011  the system-clock switch onto PLL1 (CLKGEN1, the CPU's own
 *              generator) -- reaching the LED at all means the CPU survived
 *   12 = 1100  CLKGEN6
 *   13 = 1101  CLKGEN9
 */
#define SONORA_CLOCK_FAIL_NONE            (0u)
#define SONORA_CLOCK_FAIL_PLL1_FRC        (9u)
#define SONORA_CLOCK_FAIL_PLL1_FRC_RATE   (10u)
#define SONORA_CLOCK_FAIL_CLKGEN1         (11u)
#define SONORA_CLOCK_FAIL_CLKGEN6         (12u)
#define SONORA_CLOCK_FAIL_CLKGEN9         (13u)

bool sonora_clock_boot_init(void);

/* Valid after sonora_clock_boot_init() returned false. */
unsigned sonora_clock_boot_fail_stage(void);

sonora_clock_pwm_status_t sonora_clock_pwm_prepare(nora_clock_status_t *detail);

/*
 * The single place that requests a PLL2 configuration: REFI1 12.288 MHz -> 798.72 MHz.
 *
 * Every PLL2 consumer (PWM MCLK, SPI2 transport clock, the Q27B coherent-offset
 * test, the CCP capture time base) calls this and then configures only its own
 * CLKGEN. That is not tidiness: one board must never have two reachable PLL2
 * configurations, or the first reflash across them latches the PLL handshake and
 * needs a power cycle. Keeping the config literal in one function is what makes
 * that property checkable by reading rather than by hoping.
 *
 * REFI1 pin routing stays with the caller, because which pin carries a usable
 * 12.288 MHz -- and WHICH SIGNAL it is (codec-A XTALout on RP16, the legacy BCLK
 * route on RP75, the USB source's MCLK on RP26, AK128's RP33) -- is a
 * per-board/per-consumer fact rather than part of the PLL request. Callers that
 * need the signal to be fs-INDEPENDENT must say so themselves; this function only
 * promises the 798.72 MHz configuration from a 12.288 MHz reference.
 * Call only after the clock on REFI1 is actually running.
 */
sonora_clock_pwm_status_t sonora_clock_pll2_from_refi1(nora_clock_status_t *detail);

#if RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2
/* Re-source SPI2's transport clock (CLKGEN9) from PLL2 <- REFI1, so it is coherent
 * with the codec. SysCLK stays on PLL1 <- FRC.
 *
 * REFI1 is routed from RP16 -- FIXED, not board-aware. This consumer only needs
 * coherence with codec-A, not fs-independence, so the legacy RP75 route would in
 * principle serve it too; but it has never been wired up here, and widening to RP75
 * means first checking that the fixed input_hz = 12.288 MHz still holds on that
 * route. Until then the supported configuration is RP16, i.e. the Rev.4 PCB.
 * Call application-side, AFTER WM8904-A is up so REFI1 has a reference, and
 * BEFORE the SPI2 transport starts. Returns SONORA_CLOCK_PWM_OK on success; a
 * failure must be reported and the boot stopped, not worked around. */
sonora_clock_pwm_status_t sonora_clock_spi2_from_pll2(nora_clock_status_t *detail);
#endif

#if RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2
/*
 * Re-source the CCP/SCCP capture time base (CLKGEN13) from PLL2 <- REFI1, so the
 * clock the ASRC measures sample rates WITH comes from the same crystal as the
 * sample rates it measures. CLKGEN13 is what CCPxCON1.CLKSEL=1 selects, so this
 * moves the capture time base and nothing else -- no other peripheral changes
 * clock, and SysCLK stays on PLL1 <- FRC.
 *
 * What this buys: absolute fs accuracy. It does NOT change servo behaviour, which
 * is driven by the FIFO fill error and is immune to time-base error either way.
 *
 * The reference is specifically codec-A's XTALout on RP16 (Rev.4 PCB): a FIXED
 * 12.288 MHz that does NOT follow fs. That is what makes a fixed PLL2 input_hz
 * sound, and it is measured -- a 48k -> 16k codec-A rate change moved the codec's
 * BCLK_DIV while the reported rate stayed exact. This path therefore routes RP16
 * explicitly instead of using the per-board REFI1 router, and builds whose REFI1
 * carries something else (legacy RP75, USB MCLK on RP26, AK128's RP33) are refused
 * in resolved_board_config.h.
 *
 * Call application-side, AFTER WM8904-A is up (REFI1 needs its clock present). A
 * failure must be reported and the start stopped, not worked around.
 *
 * Position relative to the CCP, precisely: BEFORE THE FIRST ARM. On a restart or
 * rate change the CCP is already running (arming is one-shot), so this call moves
 * CLKGEN13 UNDER A LIVE CAPTURE. That is safe only because the transport is muted
 * across the window and the subsequent reset_stream_state() -> ccpdet_reset()
 * discards the captures that straddled the switch -- if that stops being true, this
 * call needs a stop/re-arm around it instead. See the call site in
 * audio_transport.c.
 *
 * Known and deliberately unexercised: PLL2's reference is produced by the same
 * codec whose rates are being measured, so PLL2 loses its input if codec-A's
 * clock output stops entirely (a full power-down -- NOT a rate change, which is
 * measured safe). See [internal] clock_design_contract.md section 9.
 */
sonora_clock_pwm_status_t sonora_clock_ccp_timebase_from_pll2(nora_clock_status_t *detail);
#endif /* RESOLVED_BOARD_CCP_TIMEBASE_FROM_PLL2 */

#if RESOLVED_BOARD_COHERENT_OFFSET_CLOCK
/* Q27B coherent fixed-offset test: PLL2 (798.72 MHz) from REFI1<-WM8904-A BCLK (12.288 MHz),
 * then CLKGEN9 (SPI2 transport clock) divided from PLL2 to ~=200 MHz. Re-sources SPI2's clock from
 * the A-locked PLL2 instead of the FRC-derived PLL1; CPU/system stays on PLL1. Call AFTER WM8904-A
 * (codec master) is up (BCLK present on REFI1) and BEFORE the SPI2 transport starts. Reuses the PWM
 * REFI1/PLL2 values; PWM must be OFF. */
#define SONORA_CLOCK_Q27B_CLKGEN9_DIVIDE_BY (4u)   /* 798.72 MHz / 4 = 199.68 MHz (~ the PLL1 200 MHz) */
#define SONORA_CLOCK_Q27B_CLKGEN9_HZ        (SONORA_CLOCK_PWM_PLL2_HZ / SONORA_CLOCK_Q27B_CLKGEN9_DIVIDE_BY)
sonora_clock_pwm_status_t sonora_clock_q27b_coherent_clkgen9(nora_clock_status_t *detail);
#endif /* RESOLVED_BOARD_COHERENT_OFFSET_CLOCK */

#endif /* SONORA_CLOCK_H */
