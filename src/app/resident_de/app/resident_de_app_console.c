/* Compiled only in a serial-update image -- the three standalone MPLAB
 * configurations exclude src/resident_de/ wholesale, and the header supplies a
 * refusing stub in their place. Nothing in this file needs to ask which delivery
 * mode it is in. */
#include "resident_de/app/resident_de_app_console.h"

#include <stdio.h>
#include <xc.h>

#include "hal_nvm/nora_nvm.h"
#include "audio_transport/audio_transport.h"
/* Same-side sibling: the resident-presence oracle lives with the handoff code that owns the
 * boot banner, so the banner and this refusal can never disagree. */
#include "resident_de/app/resident_de_app_handoff.h"
/* App-side console plumbing: which port the command in flight arrived on. This is a
 * same-side dependency (both this file and app_debug.c are application code); the
 * bootloader image compiles neither. */
#include "uart_app/app_debug.h"
#include "uart_platform/uart_platform_stdio.h"
#include "resident_de_arm_timing.h"
#include "resident_de_manifest.h"
#include "resident_de_mailbox.h"
#include "resident_de_pipe.h"

#define RESIDENT_MAILBOX_PROBE_TOKEN UINT32_C(0x41505031) /* "APP1" */

static void print_reset_registers(void)
{
    const uint32_t mbist = (uint32_t)MBISTCON;

    printf(" RCON=%08lX MBIST=%08lX[EN=%lu STAT=%lu DONE=%lu FLTINJ=%lu]"
           " ECC=%08lX/%08lX/%08lX/%08lX DMA=%08lX\n",
           (unsigned long)RCON, (unsigned long)mbist,
           (unsigned long)((mbist >> 0) & 1u),
           (unsigned long)((mbist >> 4) & 1u),
           (unsigned long)((mbist >> 7) & 1u),
           (unsigned long)((mbist >> 8) & 1u),
           (unsigned long)RAMXECCCON, (unsigned long)RAMYECCCON,
           (unsigned long)PWBXECCCON, (unsigned long)PWBYECCCON,
           (unsigned long)DMACON);
}

static void resident_software_reset(void) __attribute__((noreturn));

static void resident_software_reset(void)
{
    /* Preserve the complete status line before RESET -- on the UART2 console mirror too,
     * not just UART1. See uart_platform_stdio_tx_drain(). */
    uart_platform_stdio_tx_drain();
    __builtin_disable_interrupts();
    __asm__ volatile ("reset" ::: "memory");
    for (;;) { }
}

static const char *resident_manifest_state(void)
{
    uint32_t words[NORA_NVM_U32_PER_WORD];
    uint32_t index;

    if (nora_nvm_read_word(RESIDENT_MANIFEST_ADDRESS, words) !=
        NORA_NVM_OK)
    {
        return "read-error";
    }
    if ((words[0] == RESIDENT_MANIFEST_MAGIC_WORD0) &&
        (words[1] == RESIDENT_MANIFEST_MAGIC_WORD1))
    {
        return "committed";
    }
    for (index = 0u; index < NORA_NVM_U32_PER_WORD; index++)
    {
        if (words[index] != UINT32_MAX)
        {
            return "invalid";
        }
    }
    return "erased";
}

void resident_de_app_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) { return; }

    if ((msg->kind == '?') && (msg->name == 'm'))
    {
        uint32_t near_words[4];
        uint32_t far_words[4];
        resident_boot_mailbox_snapshot(near_words);
        resident_boot_far_sentinel_snapshot(far_words);
        printf(" resident mailbox A=%08lX %08lX %08lX %08lX"
               " B=%08lX %08lX %08lX %08lX\n",
               (unsigned long)near_words[0], (unsigned long)near_words[1],
               (unsigned long)near_words[2], (unsigned long)near_words[3],
               (unsigned long)far_words[0], (unsigned long)far_words[1],
               (unsigned long)far_words[2], (unsigned long)far_words[3]);
        print_reset_registers();
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        return;
    }
    if ((msg->kind == '*') && (msg->name == 'm'))
    {
        uint32_t words[4];

        if ((msg->data_len != 1u) ||
            ((msg->data[0] != 0xA4u) && (msg->data[0] != 0xA5u)))
        {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        if (msg->data[0] == 0xA4u)
        {
            resident_boot_probe_set(RESIDENT_MAILBOX_PROBE_TOKEN);
            resident_boot_mailbox_snapshot(words);
            printf(" resident SRAM probe published=%08lX %08lX %08lX %08lX;"
                   " hold for inspection\n",
                   (unsigned long)words[0], (unsigned long)words[1],
                   (unsigned long)words[2], (unsigned long)words[3]);
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_OK;
            return;
        }
        printf(" resident synchronized SRAM probe; software reset\n");
        print_reset_registers();
        /* All visible diagnostics leave before the atomic reset tail. */
        uart_platform_stdio_tx_drain();
        resident_boot_probe_reset_sync(RESIDENT_MAILBOX_PROBE_TOKEN);
    }

    if ((msg->kind == '?') && (msg->name == 'u'))
    {
        printf(" resident manifest=%s\n", resident_manifest_state());
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        return;
    }
    if ((msg->kind == '*') && (msg->name == 'u'))
    {
        /* 0x5A -- NON-DESTRUCTIVE request: ask the resident bootloader for update mode
         * through the cross-reset container, leaving the installed application intact. If
         * no package is offered the bootloader times out and boots this application again,
         * and a reset button press does the same, because the bootloader consumes the
         * request as it reads it.
         *
         * This verb no longer has a destructive form. The Flash-manifest erase moved to
         * *feaa55 below; 0xA5 here is now simply rejected. */
        if ((msg->data_len != 1u) || (msg->data[0] != 0x5Au))
        {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        /* REFUSE when no compatible resident engine is there to receive the request.
         *
         * Placed ahead of everything else on purpose: past this point the handler mutes the
         * analog output, stops the telemetry and resets the CPU. With no engine in Flash the
         * reset lands straight back in this application about 14 ms later -- measured
         * 2026-09-02 -- and the operator is left with a silent, telemetry-dead board and no
         * hint why. Nothing is armed and nothing is muted on this path.
         *
         * resident_de_app_resident_is_present() is the oracle -- a programmed boot region
         * AND a cross-reset container of this generation. NOT the container alone: it lives
         * in no-init RAM, so after the engine is erased and the board merely reset it still
         * reads established and would answer "present" (measured 2026-09-02). The pipe's
         * cause record is not consulted either: it was added without a layout-version bump,
         * so a compatible older engine does not write one and would be judged absent.
         *
         * This is not the "never refuse an arm" case argued below for *ts. That warning
         * exists so a doubtful mute cannot strand an operator who still has a working way
         * in; here the arm is guaranteed to fail, and the refusal names the real way in. */
        if (!resident_de_app_resident_is_present())
        {
            printf(" resident update: REFUSED -- no usable resident bootloader detected"
                   " (boot region empty, or a cross-reset layout this build cannot use)."
                   " Nothing was armed, the transport was NOT muted and the board was"
                   " NOT reset\n");
            printf(" resident update: this is what an image built and programmed from"
                   " MPLAB X looks like -- the IDE flashes the application ONLY and never"
                   " pairs the resident bootloader, so the serial downloader cannot run."
                   " Build and flash with buildtools instead: switch_config.ps1, then"
                   " build.ps1, then program"
                   " dist/<conf>/production/*.factory.production.hex once over"
                   " PKOB4/PICkit. See buildtools/README.md \"Support scope\"\n");
            printf(" resident update: other ways in -- Button 3 held at reset, or *feaa55"
                   " to erase the manifest page\n");
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        audio_transport_dbg_enable(false);
        /* Do *ts here, so the operator cannot forget it. The reset below hands the board to
         * the bootloader with HPOUT still live otherwise -- the same POP hazard *ts exists
         * for before a programmer reset -- and the running transport also keeps interrupting
         * the XMODEM transfer that follows. Measured 2026-08-13: arming without *ts first
         * left the stream dead after the update (run=0, blk frozen) until the next reset.
         * Idempotent: sending *ts by hand beforehand is still fine, it re-proves the mute.
         * Not a refusal path -- if the mute cannot be verified the call says so loudly, but
         * the arm request still goes through, because an arm command that silently declines
         * would strand an operator who has no other way in. */
        if (!audio_transport_stop_for_flash())
        {
            printf(" resident update: WARNING -- *ts could not verify the analog mute;"
                   " arming anyway, expect a POP at the reset\n");
        }
        printf(" resident update requested via SRAM; application left installed\n");
        /* What happens next, and for how long. Both figures are quoted from the
         * constants the bootloader actually runs on (resident_de_arm_timing.h), never
         * retyped here -- an arm message that states a wrong deadline is worse than one
         * that states none, because the operator plans around it. */
        printf(" resident update: after the reset the bootloader sends XMODEM-CRC 'C'"
               " every %u ms and waits about %u s; with no package offered it launches"
               " this application again\n",
               (unsigned)RESIDENT_DE_ARM_HANDSHAKE_MS,
               RESIDENT_DE_ARM_TIMEOUT_S);
        /* The trap this warning exists for: the arm command is accepted on either port
         * because this handler is port-blind, but the bootloader's own console is UART1
         * only -- it is a separate image that never brings up UART2. So on the PKOB4
         * mirror the operator sees this reply, then absolute silence, then the
         * application banner a minute later, with nothing to suggest they were watching
         * the wrong port. Two days were lost to exactly that.
         *
         * Warn, do NOT refuse: arming from UART2 and then sending the package with a
         * terminal on UART1 is a legitimate way to work, and it is the only way when
         * the monitor holds UART1. */
        if (app_debug_input_is_uart2())
        {
            printf(" resident update: WARNING -- this is UART2, the PKOB4"
                   " \"USB Serial Device\" console mirror. The bootloader does NOT talk"
                   " here. Send the package on \"USB Serial Port\" (UART1); nothing will"
                   " appear on this port until the application boots again\n");
        }
        resident_boot_pipe_request_set();
        /* The request -- and the UART2 warning above it -- must be visible before the reset
         * that delivers it. */
        uart_platform_stdio_tx_drain();
        resident_software_reset();
    }

    /* *feaa55 -- DESTRUCTIVE: erase the Flash page holding the manifest. Unlike *fu5A the
     * recovery request then lives in Flash, so it survives a power cycle and does not depend
     * on the SRAM container or the reset handoff being intact -- but it is one-way: until an
     * update succeeds there is no valid application. Both paths are kept: *fu5A is the normal
     * arm command, and this is the fallback for when the App console still answers but that
     * SRAM/reset route cannot be trusted. It is NOT a way out of an App too broken to accept
     * commands -- both verbs arrive through this same handler, so if one is unreachable so is
     * the other. That case needs the debugger or Button 3.
     *
     * It does NOT reset. The application keeps running on the image already in Flash --
     * only its committed-manifest page is gone, and the App never reads that at runtime.
     * Recovery is entered by the next reset, whenever the operator chooses to do it. That
     * split is deliberate: the erase and the reboot are separately observable, so `?fu` can
     * be read back as `erased` before the board is committed to recovery. It also means a
     * mis-sent key does not cost a reboot cycle. Contrast *fu5A, which must reset to hand
     * its SRAM request to the resident image before the container is consumed.
     *
     * This was *fuA5 until it was moved here, one transposed nibble away from the
     * non-destructive *fu5A on the same verb letter. That is far too easy to reach by
     * accident for something that leaves the panel without an application, so it now has
     * its own letter and a two-byte key: no single mistyped character lands on it.
     *
     * For the same reason there is deliberately NO ?fe. A query form would make *fe a
     * habitually typed prefix, which is exactly what the move was meant to prevent --
     * `?fu` already reports manifest state. This verb is for test and debug use only. */
    if ((msg->kind == '*') && (msg->name == 'e'))
    {
        uint32_t erased_word[NORA_NVM_U32_PER_WORD] = {
            UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
        };

        if ((msg->data_len != 2u) || (msg->data[0] != 0xAAu) ||
            (msg->data[1] != 0x55u))
        {
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        /* Equivalent to *tq0000. The console handler runs in main-loop
         * context, so disabling telemetry here keeps the periodic lines from
         * interleaving with the erase result. It stays disabled after this verb
         * returns; re-enable with *tq if the App is left running. */
        audio_transport_dbg_enable(false);
        printf(" resident update requested; invalidating manifest\n");
        /* Make the destructive step visible before Flash erase. */
        uart_platform_stdio_tx_drain();
        if (nora_nvm_page_erase(RESIDENT_MANIFEST_ADDRESS) !=
            NORA_NVM_OK)
        {
            printf(" resident manifest erase failed wrec=0x%02X\n",
                   (unsigned int)nora_nvm_last_wrec());
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        if (nora_nvm_verify(RESIDENT_MANIFEST_ADDRESS, erased_word,
                            sizeof(erased_word)) != NORA_NVM_OK)
        {
            printf(" resident manifest erase verify failed\n");
            msg->data_len = 0u;
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        printf(" resident manifest invalidated; reset to enter recovery\n");
        msg->data_len = 0u;
        msg->status = APP_CONSOLE_OK;
        return;
    }
    msg->data_len = 0u;
    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}
