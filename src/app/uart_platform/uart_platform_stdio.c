
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <stddef.h>
#include <stdint.h>
#include "nora_uart.h"


#include "uart_platform_uart2_usb_serial_device.h"
#include "uart_platform_stdio.h"




/*
 * UART stdio retarget (libc write()/read()) on top of the new UART HAL.
 *
 * Sole provider of write()/read() now that the legacy src/uart/uart1.c (which
 * used to own them) has been removed.
 *
 * Dependency direction (one-way):
 *     uart_platform_stdio -> nora_uart (public HAL API)
 * It does NOT include main.c, app_debug, or app_console, does NOT call printf, and
 * does NOT touch UART registers directly.
 */

/* The fixed UART instance the Sonora board routes. */
#define UART_PLATFORM_STDIO_INST  (NORA_UART_INST_1)

/*
 * Console TX mute. When set, write() drops its payload (see header). Used by the
 * firmware-update transfer to own the console UART exclusively so competing
 * printf-routed text cannot corrupt the XMODEM stream. `volatile` because it may
 * be observed from an ISR-context printf while the mainline holds it asserted.
 */
static volatile bool s_tx_muted = false;

void uart_platform_stdio_set_tx_mute(bool mute)
{
    s_tx_muted = mute;
}

bool uart_platform_stdio_tx_muted(void)
{
    return s_tx_muted;
}

/*
 * libc TX hook (printf backend).
 *
 * Mirrors the legacy uart1.c write(): handle ignored, no CR/LF conversion,
 * blocking (the HAL byte writes block on TXBF when no timeout source is set),
 * returns the number of bytes written. NULL/zero-length are guarded (safer than
 * legacy, which had no guard). UART1 remains primary; the PKOB4 monitor mirror
 * is a side effect and never changes this function's return value.
 */
int write(int handle, void *buffer, unsigned int len)
{
    size_t written;

    (void)handle;

    if (len == 0u) {
        return 0;
    }

    if (buffer == 0) {
        return 0;
    }

    /* Console owned by a firmware-update transfer: drop text (report it as fully
     * written so printf does not spin/retry), and emit nothing on the wire -- not
     * even the UART2 mirror -- so the XMODEM stream stays clean. */
    if (s_tx_muted) {
        return (int)len;
    }

    written = nora_uart_write(UART_PLATFORM_STDIO_INST, buffer, (size_t)len);
#if !APP_ASRC_MEAS_UART2_STREAM
    /* Normal build: mirror console text to the PKOB4 "USB Serial Device" (UART2). In the
     * ASRC long-stream build UART2 is a dedicated binary DATA port, so the mirror is
     * suppressed -- all console text stays on UART1 (control), UART2 stays binary-only. */
    (void)UART2_WriteMirror((const uint8_t *)buffer, (size_t)len);
#endif

    return (int)written;
}

/*
 * Block until everything write() has emitted is physically on the wire -- on EVERY port
 * it fans out to, not just the primary.
 *
 * Both writes above hand bytes to a TX FIFO and return before the shift register is
 * empty, so code that resets the CPU straight after a printf loses the tail. Waiting on
 * UART1 alone is the trap: console output is mirrored to UART2, so an operator working on
 * the PKOB4 "USB Serial Device" is exactly the one who loses characters. That is how the
 * *fu5A UART2 warning -- a message whose entire purpose is to be read before the reset --
 * arrived truncated at "...until the application boo".
 *
 * This is also the honest replacement for a fixed delay_ms() "let the FIFO drain": it
 * waits for the real condition, so it cannot be too short at a low baud rate or waste
 * time at a high one.
 */
void uart_platform_stdio_tx_drain(void)
{
    while (!nora_uart_tx_done(UART_PLATFORM_STDIO_INST)) {
    }
#if !APP_ASRC_MEAS_UART2_STREAM
    /* Same condition as the mirror in write(): if the mirror is compiled out, UART2 carries
     * binary stream data this function has no business waiting on. */
    while (!UART2_MirrorTxDone()) {
    }
#endif
}

/*
 * libc RX hook (scanf/getchar backend).
 *
 * Mirrors the legacy uart1.c read(): handle ignored, non-blocking (drains only
 * currently-available bytes up to len), converts '\r' -> '\n', returns the
 * number of bytes read this call (0 when none available). Unlike legacy, it
 * uses the HAL public API instead of reading UART registers directly.
 */
int read(int handle, void *buffer, unsigned int len)
{
    uint8_t *dst = (uint8_t *)buffer;
    unsigned int n = 0u;

    (void)handle;

    if (len == 0u) {
        return 0;
    }

    if (buffer == 0) {
        return 0;
    }

    while (n < len) {
        uint8_t b = 0u;
        nora_uart_status_t st;

        st = nora_uart_read_byte(UART_PLATFORM_STDIO_INST, &b);
        if (st == NORA_UART_ERR_RX_EMPTY) {
            break;   /* no more data available right now (non-blocking) */
        }
        if (st != NORA_UART_OK) {
            break;   /* fatal: stop and return what we have */
        }

        if (b == '\r') {
            b = '\n';
        }

        dst[n] = b;
        n++;
    }

    return (int)n;
}
