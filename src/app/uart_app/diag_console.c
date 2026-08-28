#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_console.h"
#include "diag_console.h"
#include "board/devices/wm8904.h"
#include "audio_transport.h"        /* *dl : DSPload measurement window length */

//===========================================================
// diag_console.c
//
// Common diagnostics console module 'd'. Low-level register/clock/perf dumps, plus the few
// settings that only exist to serve a measurement.
//===========================================================


// *dl<hex> / ?dl : DSPload measurement window length in microseconds.
//
// It is a SETTING, not a dump, so this module has to accept kind '*' -- hence the per-command
// kind check below rather than the blanket "queries only" rejection this module used to open with.
// The window is what makes the DSPload line readable at more than one time scale, and nothing
// about it may be hardcoded, so it must be reachable without a rebuild.
static void diag_console_dsploadprof_window( app_console_msg_t* msg )
{
    if( msg->kind == '?' )
    {
        printf( " ?dl window=%luus\n",
                (unsigned long)audio_transport_dbg_get_dsploadprof_window_us() );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        return;
    }

    /* Accept a 16-bit or a 32-bit big-endian hex payload: 2 bytes covers 1 us .. 65.535 ms, which
     * is every window worth using at FCY = 100 MHz (a 43 ms window already fills the 32-bit tick
     * counter's half-range), and 4 bytes is there so a deliberately long window is expressible
     * rather than silently truncated. */
    uint32_t us = 0u;

    if( msg->data_len == 2u )
    {
        us = (uint32_t)( ( (uint32_t)msg->data[0] << 8 ) | (uint32_t)msg->data[1] );
    }
    else if( msg->data_len == 4u )
    {
        us = ( (uint32_t)msg->data[0] << 24 ) | ( (uint32_t)msg->data[1] << 16 ) |
             ( (uint32_t)msg->data[2] << 8  ) |   (uint32_t)msg->data[3];
    }
    else
    {
        printf( " *dl wants a 16- or 32-bit value in us, e.g. *dl2710 (10ms), *dl0000 = default\n" );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
        return;
    }

    us = audio_transport_dbg_set_dsploadprof_window_us( us );

    printf( " *dl window=%luus\n", (unsigned long)us );
    msg->data_len = 0u;
    msg->status   = APP_CONSOLE_OK;
}


void diag_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    if( ( msg->kind != '?' ) && ( msg->kind != '*' ) )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch( msg->name )
    {
    case 'l':   // *dl<us> / ?dl : DSPload measurement window length
        diag_console_dsploadprof_window( msg );
        break;

    case 'r':   // ?dr : codec (WM8904) register dump (was ?ntCD); data[0] = codec instance
        if( msg->kind != '?' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        wm8904_dump_reg( msg->data[0] );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
