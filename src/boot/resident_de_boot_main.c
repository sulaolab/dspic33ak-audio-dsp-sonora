#include <xc.h>
#include <stddef.h>

#include "nora_clock_dspic33ak.h"   /* AK-only: the entry oscillator-word capture */
#include "resident_de_abi.h"
#include "resident_de_arm_timing.h"
#include "resident_de_pipe.h"
#include "resident_de_mailbox.h"
#include "resident_de_boot_led.h"
#include "resident_de_boot_platform.h"
#include "resident_de_bootloader.h"

/* Consecutive idle XMODEM handshake rounds before the update wait gives up and launches
 * the installed application instead. The round shape and the resulting total live in
 * resident_de_arm_timing.h, because the application quotes that total to the operator
 * when it accepts *fu5A and the two must not drift. */
#define RESIDENT_BOOT_IDLE_ROUNDS_BEFORE_LAUNCH RESIDENT_DE_ARM_IDLE_ROUNDS

/* Which commit this image was built from, stamped by buildtools/build_resident_bootloader.ps1
 * as a bare token so it can be stringified here. One tree means one HAL, so the boot image
 * moves with the application it was built beside; the commit on the banner is what identifies
 * the pair. Building an older bootloader means checking out that commit or release tag. */
#define RESIDENT_BOOT_STR2(x) #x
#define RESIDENT_BOOT_STR(x)  RESIDENT_BOOT_STR2(x)
#if !defined(SONORA_BOOT_GIT_COMMIT)
#define SONORA_BOOT_GIT_COMMIT unknown  /* IDE builds do not go through the script. */
#endif

/* mikroBUS I2C pad selection -- and why fuses live HERE, in an image that never
 * touches I2C.
 *
 * A #pragma config is not code and is not "run" by whoever declares it: it tells
 * the linker to emit a word at a fixed flash address (FDEVOPT = 0x7F3020), which
 * the device reads AT RESET and latches for the whole run.  So these settings
 * govern the APPLICATION's pin mapping, for as long as it runs, even though the
 * bootloader itself only ever speaks UART.
 *
 * Ownership follows the address, not the user.  The application is linked into
 * its own region above this one (AK128 0x804000.., AK512 0x808000..) and delivered
 * by serial update, and a serial update writes ONLY that region -- it cannot
 * rewrite a fuse at all.  The resident bootloader is what the programmer writes,
 * so it is the only image that can guarantee a fuse.  Consequence worth stating
 * plainly: any fuse the APPLICATION depends on must be declared here.
 * src/app/main.c declares the same settings, but on a serial-update image those
 * declarations cannot take effect; treat this file as the authority and keep the
 * two in step.
 *
 * That asymmetry is exactly what the first AK128 bi-codec hardware run found
 * (2026-08-17): ALTI2C2 was here, so codec A answered on I2C2; ALTI2C1 was only
 * in main.c, so I2C1 stayed on its standard pads and codec B's device-ID read
 * failed.
 *
 * ALTI2C2 is unconditional -- both parts put codec A's mikroBUS-A SDA/SCL on the
 * I2C2 alternate pads. */
#pragma config FDEVOPT_ALTI2C2 = ON

/* ALTI2C1 is AK128-only, deliberately not unconditional: the two parts do not
 * agree on how mikroBUS-B is wired.
 *
 *   AK128  mikroBUS-B SDA/SCL = DIM-P4/P6 = ASDA1/ASCL1  -> needs ALTI2C1 = ON
 *   AK512  mikroBUS-B is on I2C3, standard pads          -> ALTI2C3 stays OFF
 *                                                           (see src/app/main.c)
 *
 * So AK512 never enables I2C1 and gains nothing here, while its fuse word is
 * part of a working, shipped image -- leave it alone.
 *
 * On AK128 this is safe for every image, not only the bi-codec one: I2C1's
 * STANDARD pads are RB3 (DIM-P13) and RB4 (DIM-P43 = S2 push button), so
 * selecting the alternates moves I2C1 off the button pad rather than onto it. */
#if defined(__dsPIC33AK128MC106__)
#pragma config FDEVOPT_ALTI2C1 = ON
#endif

/* Dual-partition boot mode, AK512-ONLY. The dsPIC33AK128MC106 has no Flash Dual
 * Partition feature and its device pack does not define these settings at all, so the
 * compiler rejects them outright ("unknown configuration setting: 'BTMODE'") rather
 * than ignoring them. Same arming, and the same reason, as src/app/main.c:126-130.
 *
 * Do not reintroduce BTMODE = DUAL on either part: the second-panel updater this once
 * served was retired with the resident bootloader, and nothing here rebuilds it. */
#if defined(__dsPIC33AK512MPS512__)
#pragma config BTMODE = SINGLE
#pragma config NOBTSWP = OFF
#elif !defined(__dsPIC33AK128MC106__)
#error "Unknown target: state whether this part has Flash Dual Partition (BTMODE/NOBTSWP)."
#endif

#if RESIDENT_BOOT_ENA_BOOT_TRACE || RESIDENT_BOOT_ENA_LAUNCH_GUARD
static void write_hex_digit(uint8_t nibble)
{
    static const char hex[] = "0123456789ABCDEF";
    resident_boot_platform_write_byte((uint8_t)hex[nibble & 0x0fu]);
}

static void write_hex32(uint32_t value)
{
    write_hex_digit((uint8_t)(value >> 28));
    write_hex_digit((uint8_t)(value >> 24));
    write_hex_digit((uint8_t)(value >> 20));
    write_hex_digit((uint8_t)(value >> 16));
    write_hex_digit((uint8_t)(value >> 12));
    write_hex_digit((uint8_t)(value >> 8));
    write_hex_digit((uint8_t)(value >> 4));
    write_hex_digit((uint8_t)value);
}
#endif

#if RESIDENT_BOOT_ENA_BOOT_TRACE
static void write_hex_words(const char *label, const uint32_t *words,
                            size_t count)
{
    size_t index;

    resident_boot_platform_write(label);
    for (index = 0u; index < count; index++) {
        if (index != 0u) {
            resident_boot_platform_write(" ");
        }
        write_hex32(words[index]);
    }
    resident_boot_platform_write("\r\n");
}
#endif

int main(void)
{
    const uint32_t reset_cause = (uint32_t)RCON;
    nora_clock_dspic33ak_raw_t entry_clock;
    const bool reset_badop = (INTCON1bits.BADOPERR != 0u);
    const bool reset_addr = (INTCON1bits.ADDRERR != 0u);
    const bool reset_stack = (INTCON1bits.STKERR != 0u);
    const bool reset_wdto = (RCONbits.WDTO != 0u);
    const bool reset_swr = (RCONbits.SWR != 0u);
    const bool reset_extr = (RCONbits.EXTR != 0u);
    const bool reset_por = (RCONbits.POR != 0u);
    const bool reset_bor = (RCONbits.BOR != 0u);
    const bool entry_dmt_on = (DMTCONbits.ON != 0u);
    const bool entry_wdt_on = (WDTCONbits.ON != 0u);
    const bool entry_dmte = (INTCON5bits.DMTE != 0u);
    resident_boot_manifest_t manifest;
    uint32_t launch_ivt;
    uint32_t launch_entry;
    uint32_t probe_token = 0u;
    uint32_t default_vector = 0u;
    uint32_t default_detail = 0u;
    uint32_t mailbox_words[4];
    uint32_t far_words[4];
    resident_boot_reset_source_trace_t source_trace;
    resident_boot_precrt_trace_t precrt_trace;
    uint16_t launch_count;
    bool update_requested;
    bool loop_suspected;
    bool installed_valid;
    bool probe_valid;
    bool source_trace_valid;
    bool default_trace_valid;
    bool force_recovery;

    /* The oscillator control words as this image was entered, before
     * resident_boot_platform_init() normalizes the clock. Taken through the
     * backend's typed capture rather than by naming the SFRs here: this record
     * depends on dsPIC33AK register layout, and the type it names says so.
     * Nothing above this line touches a clock register, so it is still the
     * entry state. */
    nora_clock_dspic33ak_raw_capture(&entry_clock);

    /* The erased FWDT configuration starts the runtime watchdog. The resident
     * image owns long blocking UART/NVM operations and does not use WDT
     * supervision, so stop it before any diagnostic or validation work. */
    WDTCONbits.ON = 0u;

    /* Reset-cause bits are sticky. Preserve the entry snapshot above, then
     * clear only the cause flags so any subsequent reset is unambiguous. */
    RCONbits.POR = 0u;
    RCONbits.BOR = 0u;
    RCONbits.EXTR = 0u;
    RCONbits.SWR = 0u;
    RCONbits.WDTO = 0u;
    RCONbits.CM = 0u;

    resident_boot_mailbox_snapshot(mailbox_words);
    resident_boot_far_sentinel_snapshot(far_words);
    resident_boot_reset_source_snapshot(&source_trace);
    resident_boot_precrt_snapshot(&precrt_trace);
    source_trace_valid = resident_boot_reset_source_trace_valid(&source_trace);
    default_trace_valid = resident_boot_default_interrupt_take(&default_vector,
                                                                &default_detail);
    default_trace_valid = reset_swr && default_trace_valid;

    /* A launch/probe record is meaningful only immediately after the software
     * reset used by its publisher.  Preserve every raw value above, then
     * invalidate cold/debugger/other-reset residue in C rather than letting
     * the pre-CRT observer destroy it before capture. */
    if (!reset_swr) {
        resident_boot_mailbox_invalidate();
    }
    probe_valid = reset_swr && resident_boot_probe_take(&probe_token);
    force_recovery = (reset_extr || reset_por || reset_bor) &&
                     resident_boot_platform_recovery_button_pressed();

    /* The cross-reset container, read before anything else can disturb it.
     *
     * init() establishes the container only when it cannot be trusted -- a power-on
     * class reset, or bytes belonging to a different layout version -- and leaves valid
     * records alone otherwise. That is what makes it safe to call ahead of the reads
     * below; wiping first would make an incoming message vanish with no trace.
     *
     * The cold flag is derived from the RCON snapshot taken at the top of main, not from
     * a fresh read: the cause bits are sticky and were cleared above, so a second read
     * would report nothing. */
    resident_boot_pipe_init(reset_por || reset_bor);

    /* Hand the application the reset cause it can no longer see for itself. RCON's cause
     * bits were cleared above, and the launch hand-over is another reset on top of that,
     * so without this the application reads a clean RCON every boot and reports
     * OTHER/unknown -- including straight after its own *sr. AFTER pipe_init() on purpose:
     * init() zeroes the container when it re-establishes, which would drop a record
     * published before it.
     *
     * Publish only when this pass is NOT the launch hand-over. The bootloader runs twice
     * per launch: pass 1 sees the real reset and calls launch_reset(), so pass 2 always
     * sees SWR. Publishing on pass 2 as well overwrote the real cause with the hand-over's
     * own SWR, and a genuine power cycle reached the application as "warm" -- which
     * silently switched off both power-on-only behaviours downstream (the boot-banner hold
     * and the cold WM8904 start). SWR + a record still unconsumed can only be that second
     * pass: after any boot the application actually reached, take() has already consumed
     * it, so a real *sr publishes normally. */
    if (!(reset_swr && resident_boot_pipe_cause_pending())) {
        resident_boot_pipe_cause_publish(reset_cause);
    }

    /* An update request is only meaningful immediately after the software reset its
     * publisher performed, and taking it CONSUMES it. Consuming on read is what makes
     * the wait escapable: after a timeout, or a reset button press, no request remains
     * and the application boots normally. */
    update_requested = reset_swr && resident_boot_pipe_request_take();

#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
    launch_count = resident_boot_pipe_launch_count();
    loop_suspected = (launch_count > RESIDENT_BOOT_PIPE_LAUNCH_LIMIT);
#else
    launch_count = 0u;
    loop_suspected = false;
#endif

    if (!force_recovery && reset_swr &&
        resident_boot_launch_take(&launch_ivt, &launch_entry)) {
        if ((launch_ivt == RESIDENT_APP_BASE_ADDRESS) &&
            (launch_entry >= RESIDENT_APP_BASE_ADDRESS) &&
            (launch_entry < RESIDENT_MANIFEST_ADDRESS) &&
            ((launch_entry & UINT32_C(1)) == 0u)) {
            resident_boot_platform_jump_early(launch_ivt, launch_entry);
        }
        /* A corrupt/stale launch record is consumed and normal recovery below
         * revalidates Flash before another launch attempt. */
    }

    if (!resident_boot_platform_init()) {
        for (;;) {
            /* Clock/UART/timer startup failed; remain in the resident image. */
        }
    }

    /* All-off before anything can print or transfer, so a stale LAT state from
     * a prior image never shows as a lit LED. */
    resident_boot_led_init();

    /* Which boot<->app agreement this engine was built against. The application
     * prints the same number on its Delivery banner line, so a log shows whether the
     * pair matches instead of leaving it to be inferred from build dates. */
    resident_boot_platform_write("\r\nBL " RESIDENT_BOOT_STR(SONORA_BOOT_GIT_COMMIT)
                                 " DE ABI=" RESIDENT_DE_ABI_VERSION_STR "\r\n");

#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("V=");
    if (default_trace_valid) {
        write_hex32(default_vector);
        resident_boot_platform_write("\r\nF=");
        write_hex32(default_detail);
    } else {
        resident_boot_platform_write("NONE\r\nF=NONE");
    }
    resident_boot_platform_write("\r\nT D");
    resident_boot_platform_write(entry_dmt_on ? "1" : "0");
    resident_boot_platform_write(" W");
    resident_boot_platform_write(entry_wdt_on ? "1" : "0");
    resident_boot_platform_write(" E");
    resident_boot_platform_write(entry_dmte ? "1\r\n" : "0\r\n");
#endif

    if (reset_badop) {
        resident_boot_platform_write("\r\nBL BADOP\r\n");
    } else if (reset_addr) {
        resident_boot_platform_write("\r\nBL ADDR\r\n");
    } else if (reset_stack) {
        resident_boot_platform_write("\r\nBL STACK\r\n");
    } else if (reset_wdto) {
        resident_boot_platform_write("\r\nBL WDTO\r\n");
    } else if (reset_swr) {
        resident_boot_platform_write("\r\nBL SWR\r\n");
    } else if (reset_extr) {
        resident_boot_platform_write("\r\nBL EXTR\r\n");
    } else if (reset_por) {
        resident_boot_platform_write("\r\nBL POR\r\n");
    } else if (reset_bor) {
        resident_boot_platform_write("\r\nBL BOR\r\n");
    } else {
        resident_boot_platform_write("\r\nBL OTHER\r\n");
    }
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("RCON raw: ");
    write_hex32(reset_cause); resident_boot_platform_write("\r\n");
    {
        /* Printed in the order the label names, which is not the capture
         * struct's field order. */
        const uint32_t entry_clock_words[6] = {
            entry_clock.oscctrl,
            entry_clock.pll1con,
            entry_clock.pll1div,
            entry_clock.pll2con,
            entry_clock.pll2div,
            entry_clock.clkfail
        };

        write_hex_words("Entry OSC/PLL1C/PLL1D/PLL2C/PLL2D/FAIL: ",
                        entry_clock_words, 6u);
    }
    write_hex_words("Mailbox raw: ", mailbox_words, 4u);
    write_hex_words("Far sentinel: ", far_words, 4u);
    resident_boot_platform_write("Pre-CRT: M=");
    write_hex32(precrt_trace.magic);
    resident_boot_platform_write(" RCON="); write_hex32(precrt_trace.rcon);
    resident_boot_platform_write(" MBIST="); write_hex32(precrt_trace.mbistcon);
    resident_boot_platform_write(" W15="); write_hex32(precrt_trace.w15);
    resident_boot_platform_write(" SPLIM="); write_hex32(precrt_trace.splim);
    resident_boot_platform_write(" SR="); write_hex32(precrt_trace.sr);
    resident_boot_platform_write("\r\n");
    write_hex_words("Pre-CRT A: ", precrt_trace.mailbox_a, 4u);
    write_hex_words("Pre-CRT B: ", precrt_trace.mailbox_b, 4u);
    resident_boot_platform_write("Reset source: M=");
    write_hex32(source_trace.magic);
    resident_boot_platform_write(source_trace_valid ? " VALID" : " INVALID");
    resident_boot_platform_write(" SRC="); write_hex32(source_trace.source);
    resident_boot_platform_write(" RCON="); write_hex32(source_trace.rcon);
    resident_boot_platform_write(" MBIST="); write_hex32(source_trace.mbistcon);
    resident_boot_platform_write(" ECC="); write_hex32(source_trace.ramxecccon);
    resident_boot_platform_write("/"); write_hex32(source_trace.ramyecccon);
    resident_boot_platform_write("/"); write_hex32(source_trace.pwbxecccon);
    resident_boot_platform_write("/"); write_hex32(source_trace.pwbyecccon);
    resident_boot_platform_write(" DMA="); write_hex32(source_trace.dmacon);
    resident_boot_platform_write("\r\n");
    resident_boot_platform_write("Reset MBIST bits: EN=");
    write_hex32((source_trace.mbistcon >> 0) & 1u);
    resident_boot_platform_write(" STAT=");
    write_hex32((source_trace.mbistcon >> 4) & 1u);
    resident_boot_platform_write(" DONE=");
    write_hex32((source_trace.mbistcon >> 7) & 1u);
    resident_boot_platform_write(" FLTINJ=");
    write_hex32((source_trace.mbistcon >> 8) & 1u);
    resident_boot_platform_write("\r\n");
    resident_boot_platform_write("Reset CPU: SR="); write_hex32(source_trace.sr);
    resident_boot_platform_write(" SPLIM="); write_hex32(source_trace.splim);
    resident_boot_platform_write(" W15="); write_hex32(source_trace.w15);
    resident_boot_platform_write(" INTCON="); write_hex32(source_trace.intcon1);
    resident_boot_platform_write("/"); write_hex32(source_trace.intcon3);
    resident_boot_platform_write("/"); write_hex32(source_trace.intcon4);
    resident_boot_platform_write("\r\n");
    write_hex_words("Reset IEC0..11: ", source_trace.iec, 12u);
    write_hex_words("Reset IFS0..11: ", source_trace.ifs, 12u);
    write_hex_words("Reset CLK1/6/8 CON/DIV: ", source_trace.clkgen, 6u);
    write_hex_words("Reset mailbox A: ", source_trace.mailbox_a, 4u);
    write_hex_words("Reset mailbox B: ", source_trace.mailbox_b, 4u);
    /* The probe verdict itself is NOT printed here -- it moved below the trace guard so
     * the terse build reports it too.  See the MBP= block. */
#else
    /* Captured and consumed unconditionally -- taking a record is what keeps it from
     * being read again after the next reset -- but only the trace build prints them.
     * reset_cause is no longer merely discarded here: it is forwarded to the application
     * through the pipe above, and this cast only says the terse build does not PRINT it. */
    (void)reset_cause;
    (void)entry_clock;
    (void)entry_dmt_on;
    (void)entry_wdt_on;
    (void)entry_dmte;
    (void)mailbox_words;
    (void)far_words;
    (void)source_trace;
    (void)source_trace_valid;
    (void)precrt_trace;
    (void)default_vector;
    (void)default_detail;
    (void)default_trace_valid;
#endif
    /* Terse on purpose: this image is within a few hundred bytes of full, and every
     * string is spent from that. The crash breadcrumb is deliberately NOT reported here
     * -- printf and the room for it live on the application side. */
#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
    resident_boot_platform_write("LC=");
    write_hex32((uint32_t)launch_count);
    resident_boot_platform_write(loop_suspected ? " OVER\r\n" : "\r\n");
#endif

    /* Mailbox-probe verdict, printed in EVERY build (it used to live inside the
     * BOOT_TRACE block).  Without this, a *fmA5 round is unobservable in the shipped
     * terse image: "no PASS line" cannot be told apart from "probe failed", and a
     * BOOT_TRACE build cannot substitute -- it is 98.8% of the 0x8000 panel and hangs on
     * this very software-reset path (measured 2026-08-08).  Only a software reset can
     * carry a record, so cold/EXTR boots stay silent and cost nothing.
     * Same availability condition as write_hex32 above. */
#if RESIDENT_BOOT_ENA_BOOT_TRACE || RESIDENT_BOOT_ENA_LAUNCH_GUARD
    if (reset_swr) {
        if (probe_valid) {
            resident_boot_platform_write("MBP=");
            write_hex32(probe_token);
            resident_boot_platform_write("\r\n");
        } else {
            resident_boot_platform_write("MBP=none\r\n");
        }
    }
#else
    (void)probe_token;
    (void)probe_valid;
#endif

    installed_valid = resident_boot_installed_manifest(&manifest);

    /* Four reasons not to hand over. Only the first two leave the application
     * installed and bootable, which is what decides whether the wait below may give
     * up and launch it anyway. */
    if (installed_valid && !force_recovery && !update_requested && !loop_suspected) {
        /* Count this attempt before leaving. The application clears the counter once it
         * has run long enough to call the boot a success; if it never gets that far,
         * this climbs until loop_suspected stops the cycle. */
#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
        resident_boot_pipe_launch_set((uint16_t)(launch_count + 1u));
#endif
        resident_boot_platform_write("Manifest OK; resetting into application.\r\n");
        resident_boot_platform_launch_reset(manifest.ivt_address,
                                            manifest.entry_address);
    }

    /* App is untouched; it boots again on timeout or reset. */
    if (update_requested) {
        resident_boot_platform_write("BL REQ\r\n");
    }

    if (force_recovery) {
        resident_boot_platform_write(
            "Button 3 held at reset; forced recovery mode.\r\n"
            "Installed application remains valid until a package header is accepted.\r\n");
    }

#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
    /* Does NOT time out; power-cycle clears the counter and retries. */
    if (loop_suspected) {
        resident_boot_platform_write("BL LOOP\r\n");
    }
#endif

    if (!installed_valid) {
        resident_boot_platform_write("No valid application; recovery mode.\r\n");
    }

    /* Give up and launch the installed application only when there IS one to launch and
     * it is not the thing that is failing. With no valid application there is nothing to
     * fall back to, and after a launch-loop the application is the suspect -- in both
     * cases waiting forever is the safe state. */
    {
        const bool may_give_up = installed_valid && !loop_suspected;
        unsigned  idle_rounds = 0u;

        for (;;) {
            resident_boot_install_status_t status;
            /* Say what the wait is doing and how long it lasts. "Nothing is happening"
             * and "the handshake is running and you have N seconds" look identical on a
             * terminal otherwise, and the give-up figure is not guessable from here.
             * Split on may_give_up because promising a boot-away that cannot happen is
             * worse than saying nothing: with no valid application to fall back to, or
             * with the application itself under suspicion, this wait is endless by
             * design. */
            resident_boot_platform_write(
                "Sending XMODEM-CRC 'C' every " RESIDENT_DE_ARM_HANDSHAKE_S_STR
                " s -- send the .sfb package now.\r\n");
            if (may_give_up) {
                resident_boot_platform_write(
                    "If nothing is offered within about " RESIDENT_DE_ARM_TIMEOUT_S_STR
                    " s the installed application is launched again.\r\n");
            } else {
                resident_boot_platform_write(
                    "No application to fall back to: this wait does not time out.\r\n");
            }
            status = resident_boot_receive_and_install();
            if (status == RESIDENT_BOOT_INSTALL_OK) {
                resident_boot_platform_write("Update committed; resetting.\r\n");
                resident_boot_platform_reset();
            }
            /* A TRANSFER status with nobody sending is the handshake giving up, which
             * takes about 30 s. Anything else means a transfer was attempted and went
             * wrong, so the operator is present -- keep waiting for their retry rather
             * than booting away underneath them. */
            if (status == RESIDENT_BOOT_INSTALL_TRANSFER) {
                idle_rounds++;
            } else {
                idle_rounds = 0u;
                resident_boot_platform_write("Update failed; retrying recovery.\r\n");
                continue;
            }
            if (may_give_up && (idle_rounds >= RESIDENT_BOOT_IDLE_ROUNDS_BEFORE_LAUNCH)) {
        #if RESIDENT_BOOT_ENA_LAUNCH_GUARD
        resident_boot_pipe_launch_set((uint16_t)(launch_count + 1u));
#endif
                resident_boot_platform_write("BL IDLE->APP\r\n");
                resident_boot_platform_launch_reset(manifest.ivt_address,
                                                    manifest.entry_address);
            }
        }
    }
}
