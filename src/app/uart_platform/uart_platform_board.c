
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"   /* AK-only: CLKGEN8 UART clock policy */
#include "nora_gpio.h"   /* RP-first GPIO API */
#include "nora_pps.h"    /* PPS signal routing */

#include "uart_platform_board.h"

static bool configure_uart_clkgen8(void);
static bool route_uart1_pins(void);

/*
 * UART_PLATFORM_USE_RX_ISR_RING is a Sonora board default selection only.
 * It must not leak into UART1_* API users or the HAL; the board-config layer is
 * the one place that translates it into a HAL rx_mode. Required here so a build
 * that forgets the config fails loudly rather than silently picking a mode.
 */
#ifndef UART_PLATFORM_USE_RX_ISR_RING
 #error "UART_PLATFORM_USE_RX_ISR_RING must be defined by app_specific_config_defs.h"
#endif

/*
 * UART1 RX backend (board default).
 *
 * UART_PLATFORM_USE_RX_ISR_RING selects this board's UART1 RX mode here, at the
 * board-config layer (not in the UART1_* API). When ISR ring mode is selected,
 * the RX ring storage lives here and is handed to the HAL by pointer (the HAL
 * keeps no implicit ring RAM). In polling mode no ring buffer is allocated.
 */
#if UART_PLATFORM_USE_RX_ISR_RING

#ifndef UART_PLATFORM_RX_RING_BUF_SIZE
#define UART_PLATFORM_RX_RING_BUF_SIZE  (256u)
#endif

#ifndef UART_PLATFORM_RX_ISR_PRIORITY
#define UART_PLATFORM_RX_ISR_PRIORITY   (5)
#endif

static uint8_t uart1_rx_ring[UART_PLATFORM_RX_RING_BUF_SIZE];

#endif /* UART_PLATFORM_USE_RX_ISR_RING */

/*
 * TX interrupt priority for the optional async TX transfer engine. The legacy
 * byte-stream path uses blocking TX and never enables the TX interrupt, so this
 * only matters if a higher layer calls nora_uart_tx_start(). Default to the
 * RX ring priority so TX and RX share a tier on this board.
 */
#ifndef UART_PLATFORM_TX_ISR_PRIORITY
#define UART_PLATFORM_TX_ISR_PRIORITY   (5)
#endif




/*
 * Sonora board UART platform layer.
 *
 * All values below are COPIED from the legacy src/uart/uart1.c
 * UART1_Initialize() so the legacy driver remains unchanged.  Only UART1 is
 * wired on this board.
 *
 * Known existing configuration (legacy reference):
 *   - UART instance : UART1
 *   - dsPIC33AK512MPS512 : U1RX = RD1, U1TX = RH1 (idle high)
 *   - dsPIC33AK128MC106  : U1RX = RD3, U1TX = RD1 (idle high)
 *   - target           : external MCP USB controller (NOT the PKoB virtual COM)
 *   - baudrate         : 230400
 *   - UART clock       : CLKGEN8 / PLL1, ~200 MHz (UART platform policy)
 */

bool uart_platform_board_get_default_config(
    nora_uart_instance_t inst,
    uart_platform_board_config_t *out)
{
    if (out == 0) {
        return false;
    }

    /* The Sonora board only routes UART1. */
    if (inst != NORA_UART_INST_1) {
        return false;
    }

    /*
     * Both device variants use the same UART_BRG baud at the board UART1 clock.
     * Split with #ifdef here if the boards ever diverge.
     */
    out->inst = inst;
    out->config.uart_clk_hz = (uint32_t)UART_PLATFORM_UART1_CLK_HZ;
    out->config.baudrate    = UART_BRG;
    out->config.timeout_ms  = 0u;           /* legacy UART has no timeout */
    out->config.get_ms      = 0;            /* timeout disabled */
    out->config.data_bits   = 8u;
    out->config.stop_bits   = 1u;
    out->config.parity      = NORA_UART_PARITY_NONE;
    out->config.enable_tx   = true;
    out->config.enable_rx   = true;
    out->config.tx_irq_priority = UART_PLATFORM_TX_ISR_PRIORITY;

    /*
     * RX backend for UART1 (board choice). The HAL sets up the RX ISR ring inside
     * nora_uart_init() from these fields; the UART1_* API stays backend-
     * agnostic. UART_PLATFORM_USE_RX_ISR_RING selects the default here.
     */
#if UART_PLATFORM_USE_RX_ISR_RING
    out->config.rx_mode             = NORA_UART_RX_MODE_ISR_RING;
    out->config.rx_ring_buffer      = uart1_rx_ring;
    out->config.rx_ring_buffer_size = (uint16_t)sizeof(uart1_rx_ring);
    out->config.rx_irq_priority     = UART_PLATFORM_RX_ISR_PRIORITY;
#else
    out->config.rx_mode             = NORA_UART_RX_MODE_POLLING;
    out->config.rx_ring_buffer      = 0;
    out->config.rx_ring_buffer_size = 0u;
    out->config.rx_irq_priority     = 0u;
#endif

    return true;
}

bool uart_platform_board_apply_pins_and_clock(nora_uart_instance_t inst)
{
    /* Only UART1 is wired on the Sonora board. */
    if (inst != NORA_UART_INST_1) {
        return false;
    }
    if (!configure_uart_clkgen8()) {
        return false;
    }
    return route_uart1_pins();
}

bool uart_platform_board_apply_pins_and_frc_clock(nora_uart_instance_t inst)
{
    /* Boot-fault console: PLL1 is not available, so take CLKGEN8 from the FRC.
     * Switching a CLKGEN's source still completes in that state -- it is the PLL
     * handshake, not the clock-switch sequencer, that is stuck. */
    const nora_clock_dspic33ak_clkgen_config_t config = {
        .source    = NORA_CLOCK_SOURCE_FRC,
        .divide_by = 1u,
    };

    if (inst != NORA_UART_INST_1) {
        return false;
    }
    if (nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_8, &config) !=
            NORA_CLOCK_OK) {
        return false;
    }
    return route_uart1_pins();
}

static bool route_uart1_pins(void)
{
    /*
     * The PPS/GPIO values below are COPIED (not moved) from the legacy
     * UART1_Initialize().
     */
    /*
     * UART1 pin map (single source of truth — RP number = packed GPIO pin + 1):
     *   AK512: U1RX = RD1 (RP50), U1TX = RH1 (RP114)
     *   AK128: U1RX = RD3 (RP52), U1TX = RD1 (RP50)
     * TX is idle-high: LAT seeded High before TRIS is driven (glitch-free).
     * route_* self-bracket IOLOCK; GPIO failures are caught before PPS routing.
     */
#if APP_TARGET == APP_TARGET_AK512
    #define UART1_RX_RP  ((nora_gpio_rp_t)50u)    /* U1RX <- RD1 */
    #define UART1_TX_RP  ((nora_gpio_rp_t)114u)   /* U1TX -> RH1 */
#elif APP_TARGET == APP_TARGET_AK128
    #define UART1_RX_RP  ((nora_gpio_rp_t)52u)    /* U1RX <- RD3 */
    #define UART1_TX_RP  ((nora_gpio_rp_t)50u)    /* U1TX -> RD1 */
#else
    return false;   /* unknown device */
#endif
    bool ok = true;
    ok = nora_pinmux_route_output(NORA_PPS_OUTPUT_U1TX, UART1_TX_RP, true) && ok;  /* TX idle high */
    ok = nora_pinmux_route_input (NORA_PPS_INPUT_U1RX,  UART1_RX_RP) && ok;
    return ok;
}

static bool configure_uart_clkgen8(void)
{
    const nora_clock_dspic33ak_clkgen_config_t config = {
        .source    = NORA_CLOCK_SOURCE_PLL_1,
        .divide_by = UART_PLATFORM_CLKGEN8_DIVIDE_BY,
    };

    return nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_8, &config) ==
           NORA_CLOCK_OK;
}
