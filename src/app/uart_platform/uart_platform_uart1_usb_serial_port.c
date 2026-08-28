
/*
 * Legacy UART1_* compatibility wrapper implementation.
 *
 * Sole provider of UART1_* (the legacy src/uart/uart1.c has been removed),
 * implemented on top of the new UART HAL. UART interrupt vectors live in
 * uart_platform_irq.c so the forwarding path is shared by legacy and future
 * CMSIS USART entry points.
 *
 * UART1 -> MCP2221A -> Windows "USB Serial Port" -> Console endpoint.
 * Supports the interactive console + Biquad CSV bulk transfer (the CSV-capable
 * port; UART2 is interactive console only).
 *
 * Dependency direction (one-way):
 *     uart_platform_uart1_usb_serial_port -> uart_platform_board -> nora_uart
 * It does NOT include main.c, app_debug, or app_console, and it does NOT define
 * write()/read() (that is the stdio retarget compat unit's job).
 */

#include <stdint.h>
#include <stdbool.h>
#include "nora_uart.h"
#include "uart_platform_board.h"


#include "uart_platform_uart1_usb_serial_port.h"


nora_uart_status_t UART1_Initialize(void)
{
    uart_platform_board_config_t board_cfg;

    if (!uart_platform_board_get_default_config(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &board_cfg)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    if (!uart_platform_board_apply_pins_and_clock(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    /*
     * The RX backend (polling FIFO vs ISR ring) is chosen by the board config
     * (board_cfg.config.rx_mode) and set up inside nora_uart_init(); this
     * layer no longer configures or enables the RX ISR itself.
     *
     * The HAL init status is returned to the caller. The HAL never prints or
     * halts; the caller decides how to surface a failure (and must NOT printf()
     * to a UART whose own init just failed).
     */
    return nora_uart_init(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &board_cfg.config);
}

nora_uart_status_t UART1_InitializeBootFaultConsole(void)
{
    uart_platform_board_config_t board_cfg;

    if (!uart_platform_board_get_default_config(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &board_cfg)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    /*
     * Same UART1, different clock facts: CLKGEN8 from the FRC rather than PLL1,
     * and a baud that is actually representable from 8 MHz. Everything else --
     * pins, framing, RX backend -- is the board default, so a boot-fault console
     * behaves like the normal one apart from its speed.
     */
    board_cfg.config.uart_clk_hz = (uint32_t)UART_PLATFORM_FRC_CLK_HZ;
    board_cfg.config.baudrate    = UART_PLATFORM_BOOT_FAULT_BAUD;

    if (!uart_platform_board_apply_pins_and_frc_clock(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    return nora_uart_init(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &board_cfg.config);
}

void UART1_Deinitialize(void)
{
    /* nora_uart_deinit() stops the RX ISR internally when the instance was
     * configured for ISR ring mode, so no separate disable call is needed here. */
    (void)nora_uart_deinit(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST);
}

/*
 * ---------------------------------------------------------------------------
 * RX path: backend-agnostic.
 *
 * These call only the plain UART HAL RX API. The HAL routes each call to the
 * polling FIFO or the ISR ring per the instance's configured rx_mode, so this
 * layer no longer knows or branches on the RX backend. RX FIFO ownership stays
 * exclusive inside the HAL (the ISR is the sole FIFO consumer in ring mode).
 * ---------------------------------------------------------------------------
 */

bool UART1_IsRxReady(void)
{
    return nora_uart_rx_ready(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST);
}


uint8_t UART1_Read(void)
{
    /* Legacy UART1_Read() returns 0 when no byte is available; mirror that. */
    uint8_t data = 0u;

    if (nora_uart_read_byte(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &data) != NORA_UART_OK) {
        return 0u;
    }

    return data;
}


void UART1_RxFlush(void)
{
    nora_uart_rx_flush(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST);
}


