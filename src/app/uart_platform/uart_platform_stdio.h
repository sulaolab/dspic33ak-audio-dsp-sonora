#ifndef UART_PLATFORM_STDIO_H
#define UART_PLATFORM_STDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UART stdio retarget provider (UART platform layer, NOT public driver).
 *
 * Provides the libc retarget hooks write()/read() (the printf/scanf backend)
 * on top of the new UART HAL. This is the sole provider now that the legacy
 * src/uart/uart1.c (which used to own write()/read()) has been removed.
 *
 * The section attributes make the linker use these as the libc .libc.write /
 * .libc.read hooks.
 */

int write(int handle, void *buffer, unsigned int len)
    __attribute__((__section__(".libc.write")));

int read(int handle, void *buffer, unsigned int len)
    __attribute__((__section__(".libc.read")));

/*
 * Console TX mute (write()-only gate).
 *
 * When muted, write() discards its payload and reports it as fully written, so
 * printf-routed console text (telemetry, driver diagnostics, codec-init retry
 * chatter, ...) never reaches the UART. It exists so a firmware-update transfer
 * can take EXCLUSIVE ownership of the console UART for its duration: any
 * competing subsystem that emits text mid-transfer would otherwise interleave
 * bytes into the board->host stream and corrupt the XMODEM ACK/NAK framing the
 * host protocol depends on. The XMODEM engine itself writes control bytes via
 * the UART HAL directly (not through write()), so it is unaffected by the mute.
 *
 * read() is never gated. Non-reentrant flag; set/clear from thread context only
 * (the update orchestrator brackets the transfer with it).
 */
void uart_platform_stdio_set_tx_mute(bool mute);
bool uart_platform_stdio_tx_muted(void);

/*
 * Block until every byte write() emitted has physically left the wire, on ALL the ports
 * write() fans out to (UART1 plus, in a normal build, the UART2 "USB Serial Device"
 * console mirror).
 *
 * Call this before a RESET or any other action that stops the CPU mid-stream. Waiting on
 * UART1 alone truncates the mirror, which means it truncates precisely the operator who
 * is working on the PKOB4 port. Because write() owns the fan-out, this function is the
 * only place that knows the full list -- so a caller must not hand-roll the wait.
 */
void uart_platform_stdio_tx_drain(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_STDIO_H */
