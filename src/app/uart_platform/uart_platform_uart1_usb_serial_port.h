#ifndef UART_PLATFORM_UART1_USB_SERIAL_PORT_H
#define UART_PLATFORM_UART1_USB_SERIAL_PORT_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_uart.h"
/* Board UART1 clock/baud facts, including UART_PLATFORM_BOOT_FAULT_BAUD, which a
 * caller of UART1_InitializeBootFaultConsole() needs in order to say which speed
 * it is talking at. */
#include "uart_platform_board.h"

/* UART1 -> MCP2221A -> Windows "USB Serial Port" -> Console endpoint.
 * Interactive console + Biquad CSV bulk transfer (UART1 is the CSV-capable port). */
#ifndef UART_PLATFORM_UART1_USB_SERIAL_PORT_INST
 #define UART_PLATFORM_UART1_USB_SERIAL_PORT_INST       (NORA_UART_INST_1)
#endif




#ifdef __cplusplus
extern "C" {
#endif

/*
 * UART1 USB Serial Port adapter - legacy UART1_* compatibility API (NOT public driver).
 *
 * Provides the legacy UART1_* API names, implemented on top of the new UART HAL
 * (nora_uart_* + uart_platform_board_*). The new API is the master; these
 * old names are the subordinate compatibility surface.
 *
 * This wrapper is backend-agnostic: the RX backend (polling FIFO vs ISR ring) is
 * selected by the Sonora board config and handled inside the UART HAL, so this
 * header carries no RX-backend build-switch dependency.
 *
 * This is the sole provider of UART1_* now that the legacy src/uart/uart1.c /
 * uart1.h have been removed; the UART1_* prototypes are declared below. The RX
 * diagnostic API (UART1_DiagGet/Clear / UART1_DIAG_t) has been retired — callers
 * read RX status from the HAL (nora_uart_rx_status_get).
 */

nora_uart_status_t UART1_Initialize(void);

/*
 * UART1 for the boot-fault console: CLKGEN8 from the FRC instead of PLL1, at
 * UART_PLATFORM_BOOT_FAULT_BAUD (19200) rather than 230400, because 230400 is
 * not representable from an 8 MHz UART clock. Use this ONLY when the clock
 * bring-up failed and there is no PLL1 to run the normal console from; once it
 * succeeds, printf() works as usual. Start the monitor with --baud 19200 to read
 * it.
 */
nora_uart_status_t UART1_InitializeBootFaultConsole(void);

void    UART1_Deinitialize(void);
bool    UART1_IsRxReady(void);
uint8_t UART1_Read(void);
void    UART1_RxFlush(void);




#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_UART1_USB_SERIAL_PORT_H */
