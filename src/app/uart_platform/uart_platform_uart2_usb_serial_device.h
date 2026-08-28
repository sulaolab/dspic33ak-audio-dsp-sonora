#ifndef UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H
#define UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_specific_config_defs.h"
#include "nora_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UART2 -> PKOB4 -> Windows "USB Serial Device" -> Console endpoint.
 *
 * UART2_* API, deliberately symmetric with the UART1_* API in
 * uart_platform_uart1_usb_serial_port.h (UART1 is the master naming). The app
 * drains RX via UART2_IsRxReady()/UART2_Read() and feeds it into the same
 * command parser as UART1 (see app_debug.c), so UART2 is a second interactive
 * command-input endpoint. Console output is mirrored here via UART2_WriteMirror.
 *
 * Capability note: UART2 supports the interactive console only. Biquad CSV bulk
 * transfer is rejected on this port (UART2 RX is polling, not suited to
 * continuous bulk RX) -- see the CSV BEGIN source policy in app_debug.c. UART1
 * is the CSV-capable port.
 */

/* Core port API -- one-to-one with UART1_*. */
nora_uart_status_t UART2_Initialize(void);
void    UART2_Deinitialize(void);
bool    UART2_IsRxReady(void);
uint8_t UART2_Read(void);
void    UART2_RxFlush(void);

/* UART2-only extra (no UART1 twin): mirror console output onto this port. */
size_t  UART2_WriteMirror(const uint8_t *data, size_t length);

/* True once every mirrored byte has left the shift register. UART2_WriteMirror blocks
 * only as far as the TX FIFO, so a RESET issued straight after a printf truncates the
 * tail on this port. Callers should not wait on this directly -- use
 * uart_platform_stdio_tx_drain(), which waits for every port write() fans out to. */
bool    UART2_MirrorTxDone(void);

#if APP_ASRC_MEAS_UART2_STREAM
/*
 * Dedicated binary DATA-port API for the long-coherent ASRC stream build. In this build
 * UART2_Initialize() brings UART2 up TX-only at APP_ASRC_STREAM_BAUD and the console mirror
 * is compiled out (uart_platform_stdio.c), so UART2 carries binary frames ONLY. These are the
 * thin platform wrappers the streamer uses -- all go through the public UART HAL, never touch
 * UART/PPS registers directly.
 */
size_t   UART2_StreamSubmit(const uint8_t *data, size_t length); /* non-blocking async TX submit; 0 if busy */
bool     UART2_StreamTxBusy(void);                               /* true while an async run is still in flight */
bool     UART2_StreamTxDone(void);                               /* true when physical shifter empty AND not busy */
uint32_t UART2_StreamBaud(void);                                 /* actual generated baud (read back from HAL) */
uint32_t UART2_StreamClockHz(void);                              /* UART2 source clock (for host baud-error math) */
#endif /* APP_ASRC_MEAS_UART2_STREAM */

#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_UART2_USB_SERIAL_DEVICE_H */
