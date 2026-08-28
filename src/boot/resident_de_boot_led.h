#ifndef RESIDENT_BOOT_LED_H
#define RESIDENT_BOOT_LED_H

#include <stdint.h>

/* Progress display for the resident bootloader's XMODEM receive, driven on the
 * board's 8 user LEDs (the same RC8..RC15 / RC3..RC10 bank the application
 * drives through button_led.c -- vendored here because button_led.c is
 * application-only, see boot_image.psd1). One consumer, one call site pair in
 * resident_de_bootloader.c: no callback indirection, to spend nothing extra of
 * this image's tight ROM cap. Gated by RESIDENT_BOOT_ENA_LED_PROGRESS.
 *
 * CONSTRAINT: these calls must stay cheap and must never touch UART1. They run
 * from inside receive_sink() while resident_xmodem_receive() owns UART1 for the
 * ACK/NAK handshake; an extra byte there corrupts the transfer.
 */

#if !defined(RESIDENT_BOOT_ENA_LED_PROGRESS)
#define RESIDENT_BOOT_ENA_LED_PROGRESS 1
#endif

#if RESIDENT_BOOT_ENA_LED_PROGRESS

/* Configure the 8 LED pins as outputs, all off. Call once at boot bring-up. */
void resident_boot_led_init(void);

/* All 8 LEDs off. Called the moment receive_sink() validates a fresh package
 * header, so a bar left over from a prior failed/idle attempt clears only when
 * a real new transfer actually starts -- not on every handshake retry. */
void resident_boot_led_progress_reset(void);

/* Light done*8/total LEDs, filling from LED7 toward LED0. Called once per
 * XMODEM block, after its bytes are written to Flash. total is always > 0 by
 * the time this can be called (the manifest is validated before any payload
 * byte is accepted), but a total==0 guard is kept anyway, defensively. */
void resident_boot_led_progress(uint32_t done, uint32_t total);

#else

#define resident_boot_led_init()                  ((void)0)
#define resident_boot_led_progress_reset()        ((void)0)
#define resident_boot_led_progress(done, total)   ((void)0)

#endif /* RESIDENT_BOOT_ENA_LED_PROGRESS */

#endif /* RESIDENT_BOOT_LED_H */
