/* Earliest resident reset capture.
 *
 * The linker points the physical reset vector here only for the resident
 * image.  Hardware W15 is still at its reset value.  This code does not use
 * the stack, CALL/RCALL, C runtime, or peripherals.  W0-W2 are scratch; the
 * normal XC-DSC reset entry initializes them before relying on their values.
 */

        .section .resident_precrt_entry,code
        .global _resident_precrt_entry
        .extern __resetPRI

/* PER DEVICE. This is the one file in the resident image the C-side per-device
 * machinery cannot reach: .equ cannot include shared/resident_de_mailbox.h, so the
 * same three RAM addresses are restated here and the two SFR addresses are taken
 * from this device's linker script. Getting it wrong is not a link error -- it is a
 * store outside data RAM in the FIRST instructions after reset, which raises an
 * Address Error while IVTBASE is still 0, so the part vectors to address 0 with no
 * console, no trace record and RCON showing no software reset (measured on the
 * AK128MC106, 2026-08-15: this file still carried the AK512 addresses).
 *
 *   AK512MPS512  data 0x4000..0x13FFF  mailbox_b 0x13E00  pre-CRT 0x13EF0
 *   AK128MC106   data 0x4000..0x7FFF   mailbox_b  0x7E00  pre-CRT  0x7EF0
 *
 * MAILBOX_B / PRECRT must equal RESIDENT_BOOT_FAR_SENTINEL_ADDRESS and
 * RESIDENT_BOOT_PRECRT_TRACE_ADDRESS in shared/resident_de_mailbox.h.
 * RCON_ADDR / MBIST_ADDR / SPLIM_ADDR must equal _RCON / _MBISTCON / _SPLIM in
 * src/linker/p33<device>_serial_update_app.gld -- they differ between the parts. */
        .equ MAILBOX_A,   0x04050
        .equ SPLIM_ADDR,  0x00004
#if defined(__dsPIC33AK512MPS512__)
        .equ MAILBOX_B,   0x13E00
        .equ PRECRT,      0x13EF0
        .equ RCON_ADDR,   0x031A0
        .equ MBIST_ADDR,  0x03B00
#elif defined(__dsPIC33AK128MC106__)
        .equ MAILBOX_B,   0x07E00
        .equ PRECRT,      0x07EF0
        .equ RCON_ADDR,   0x003198
        .equ MBIST_ADDR,  0x003AA0
#else
#error "resident_de_boot_precrt.S: unsupported device -- give this part its top-of-RAM diagnostic addresses and its _RCON/_MBISTCON, and never fall back to another part's."
#endif
        .equ PRECRT_MAGIC, 0x50435231

_resident_precrt_entry:
        mov.sl  #PRECRT, w2
        movs.l  #0, w1
        mov.l   w1, [w2]          /* magic invalid while record is written */

        /* Capture the raw reset cause, but never use it to modify either
         * sentinel.  RCON cause bits are sticky: a later software reset can
         * still report an earlier POR/BOR/EXTR unless software cleared it.
         * Clearing A/B here would therefore destroy the evidence before it
         * was observed.  Cold-reset validity is interpreted later in C. */
        mov.sl  #RCON_ADDR, w0
        mov.l   [w0], w1
        mov.l   w1, [w2+36]

        mov.sl  #MAILBOX_A, w0
        mov.l   [w0++], w1
        mov.l   w1, [w2+4]
        mov.l   [w0++], w1
        mov.l   w1, [w2+8]
        mov.l   [w0++], w1
        mov.l   w1, [w2+12]
        mov.l   [w0], w1
        mov.l   w1, [w2+16]

        mov.sl  #MAILBOX_B, w0
        mov.l   [w0++], w1
        mov.l   w1, [w2+20]
        mov.l   [w0++], w1
        mov.l   w1, [w2+24]
        mov.l   [w0++], w1
        mov.l   w1, [w2+28]
        mov.l   [w0], w1
        mov.l   w1, [w2+32]

        mov.sl  #MBIST_ADDR, w0
        mov.l   [w0], w1
        mov.l   w1, [w2+40]
        mov.l   w15, [w2+44]
        mov.sl  #SPLIM_ADDR, w0
        mov.l   [w0], w1
        mov.l   w1, [w2+48]
        movs.l  #0, w1
        mov     SR, w1
        mov.l   w1, [w2+52]

        mov.l   #PRECRT_MAGIC, w1 /* publish last */
        mov.l   w1, [w2]
        bra     __resetPRI

        .size _resident_precrt_entry, .-_resident_precrt_entry
