#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "app_console.h"
#include "system_console.h"
#include "nora_udid.h"
#include "hal_reset/nora_reset.h"        // reset-cause label for the *sr announce / ?sr help
#include "uart_platform_stdio.h"         // drain every console port before the core resets

//===========================================================
// system_console.c
//
// Common system/board console module 's'.
//   ?si : board identity / UDID (read-only)
//   *sr : software reset (dsPIC RESET instruction -> RCON.SWR); reboots the board
//   ?sr : help / current reset cause
//===========================================================

void system_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    // ---- Write actions (kind '*') -------------------------------------------------
    if( msg->kind == '*' )
    {
        switch( msg->name )
        {
        case 'r':   // *sr : software reset via the dsPIC33A RESET instruction.
            // This is the deterministic way to exercise the RCON provenance boot path on
            // the bench: RESET sets RCON.SWR, a WARM cause. Crucially it does NOT
            // power-cycle the core, so -- unlike a debugger/PKOB4 tool reset, which
            // re-asserts POR -- the POR flag must stay clear on the next boot, proving the
            // AND_CLEAR capture in main() cleared it on the previous boot.
            // What it no longer proves: the codec bring-up does NOT branch on the reset
            // cause any more. Each WM8904 is discharged inside its own wm8904_init_role(),
            // so a *sr boot and a cold boot run the identical codec sequence -- that
            // identity is the property to check, not a warm-only branch
            // ([internal] codec_ab_startup_clock_gate.md §2.5).
            // Opt-in only; no default change.
            printf(" \"*sr\" software reset now (was: %s). Rebooting...\n",
                   nora_reset_snapshot_cause_str() );
            // Wait for the real condition instead of guessing at it: this waits for UART1
            // AND the UART2 console mirror, so an operator on the PKOB4 "USB Serial
            // Device" sees the whole line too. The 150 ms delay this replaces covered
            // both ports only by being generously long.
            uart_platform_stdio_tx_drain();
            __asm__ volatile ( "reset" );     // dsPIC33A software reset -> RCON.SWR set
            // not reached
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;

        default:
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
            break;
        }
        return;
    }

    // ---- Read queries (kind '?') --------------------------------------------------
    if( msg->kind != '?' )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch( msg->name )
    {
    case 'i':   // ?si : board identity / UDID (was *nt04)
    {
        nora_udid_t udid;
        if( nora_udid_read( &udid ) )
        {
            printf(" \"?si\" UDID1=%08lX UDID2=%08lX UDID3=%08lX UDID4=%08lX\n",
                   (unsigned long)udid.word[0], (unsigned long)udid.word[1],
                   (unsigned long)udid.word[2], (unsigned long)udid.word[3] );
            printf(" \"?si\" UDID128=%08lX%08lX%08lX%08lX\n",
                   (unsigned long)udid.word[3], (unsigned long)udid.word[2],
                   (unsigned long)udid.word[1], (unsigned long)udid.word[0] );
            msg->status = APP_CONSOLE_OK;
        }
        else
        {
            printf(" \"?si\" UDID read failed or invalid\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
        }
        msg->data_len = 0u;
        break;
    }

    case 'r':   // ?sr : software-reset help + the reset cause latched at this boot
        printf(" \"*sr\" software reset (dsPIC RESET -> RCON.SWR = warm). Reboots the board\n");
        printf("       without a power cycle, so POR must read clear on the next boot.\n");
        printf("       The codec bring-up does not branch on the reset cause: a *sr boot\n");
        printf("       runs the same WM8904 sequence as a cold boot.\n");
        // Both decodes are printed on purpose. The first is the cold/warm
        // CLASSIFICATION the boot path branches on (power-event-first); the second
        // names the most specific bit RCON holds. They legitimately disagree -- a cold
        // start with MCLR held while the supply rises sets POR+BOR+EXTR together --
        // and seeing both is what makes that visible instead of confusing.
        // See hal_reset/nora_reset.h, "THE TWO PRECEDENCES DIFFER".
        printf(" this boot's reset cause: %s (RCON=0x%08lX)\n",
               nora_reset_snapshot_cause_str(),
               (unsigned long)nora_reset_snapshot_raw() );
        printf(" most specific RCON bit: %s\n",
               nora_reset_cause_str( nora_reset_snapshot_raw(),
                                     nora_reset_snapshot_is_captured() ) );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
