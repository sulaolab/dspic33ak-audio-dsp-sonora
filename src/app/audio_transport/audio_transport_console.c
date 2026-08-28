#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "app_console.h"
#include "audio_transport_console.h"
#include "audio_transport.h"   // audio_transport_restart(), audio_transport_frmerr_force_trip()

//===========================================================
// audio_transport_console.c
//
// Common transport console module 't'. App-agnostic: does not include any application
// private header and does not branch on application identity.
//===========================================================

void audio_transport_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    switch( msg->name )
    {
    case 's':   // *ts : verified analog mute + terminal stop before flash/reset
                // ?ts : report the result of the last *ts
        if( msg->data_len != 0u )
        {
            printf(" \"*ts/?ts\" takes no value\n");
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        if( msg->kind == '?' )
        {
            audio_transport_stop_for_flash_report();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        msg->data_len = 0u;
        msg->status = audio_transport_stop_for_flash()
            ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'r':   // *tr : mute-bounded same-rate restart (was *nt03)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            printf(" \"*tr\" force audio stop/restart (same rate)\n");
            const bool restarted = audio_transport_restart();
            msg->data_len = 0u;
            msg->status   = restarted
                ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        break;

    case 'd':   // *td<NN> : declick research one-shot restart with strategy bitmask NN (HEX byte)
                // ?td      : print the bitmask legend + captured-servo status
        if( msg->kind == '?' )
        {
            printf(" \"?td\" declick restart strategy bitmask (one-shot, *td<NN>):\n");
            // The legend belongs to the driver's build policy, not to this console: when the
            // strategies are compiled out this prints one line saying so instead of a menu.
            audio_transport_declick_print_help();
            audio_transport_declick_print_status();   // per-codec captured-servo status (WARM_SERVO)
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            break;
        }
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            // Default mask 0 (baseline) when no payload byte was supplied, so "*td" == "*tr".
            const uint8_t mask = ( msg->data_len >= 1u ) ? msg->data[0] : 0x00u;
            if( ( mask != 0x00u ) && !audio_transport_declick_research_available() )
            {
                // Refuse rather than silently run the default: the strategies are compiled out.
                printf(" \"*td\" mask=0x%02x not available -- declick research compiled out;"
                       " use \"*td00\" or \"*tr\"\n", (unsigned)mask );
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
                break;
            }
            printf(" \"*td\" declick one-shot restart mask=0x%02x\n", (unsigned)mask );
            const bool restarted = audio_transport_restart_declick( mask );
            msg->data_len = 0u;
            msg->status   = restarted
                ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        }
        break;

    case 'q':   // Telemetry control. Payload = mode word (2 bytes), optional period word (2 bytes):
                //   *tq0000        -> OFF
                //   *tq0001        -> ON  (resolved default period)
                //   *tq0002 XXXX   -> ON  with period = 0xXXXX ms  (e.g. *tq00020BB8 = 3000ms)
                // Short forms map by value: *tq00 -> OFF, *tq01 -> ON. No payload -> OFF.
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            const uint16_t mode = ( msg->data_len >= 2u )
                                  ? (uint16_t)( ( (uint16_t)msg->data[0] << 8 ) | msg->data[1] )
                                  : ( ( msg->data_len == 1u ) ? msg->data[0] : 0u );
            if( mode == 2u )
            {
                const uint16_t ms = ( msg->data_len >= 4u )
                                    ? (uint16_t)( ( (uint16_t)msg->data[2] << 8 ) | msg->data[3] )
                                    : 0u;
                audio_transport_set_dbg_period_ms( (uint32_t)ms );   // 0 also disables
                printf(" \"*tq\" telemetry %s period=%ums\n", ( ms == 0u ) ? "OFF" : "ON", (unsigned)ms );
            }
            else
            {
                const bool on = ( mode != 0u );
                audio_transport_dbg_enable( on );                    // ON = resolved default period
                printf(" \"*tq\" telemetry %s\n", on ? "ON" : "OFF" );
            }
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;

    case 'f':   // *tf : TDM frame-slip force-trip, arms one recovery episode (was *nt43)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        {
            // audio_transport_frmerr_force_trip() is a safe no-op when auto-recovery is not
            // built in, so calling it needs no gate -- but REPORTING it does. Announcing
            // "armed, fires on the next recover tick" and returning OK on a profile that
            // compiled the recovery path out describes something that will never happen, and
            // the resulting silence reads as a broken co-clocked build. UNSUPPORTED, matching
            // *cy's "known thing, not built into this target" convention, says which it is.
            if( !audio_transport_frmerr_autorecovery_available() )
            {
                printf(" \"*tf\" TDM frame-slip auto-recovery is not built into this"
                       " configuration (it needs the independent dual-clock-domain topology"
                       " with edge capture on both legs); nothing to force\n");
                msg->data_len = 0u;
                msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
                break;
            }
            audio_transport_frmerr_force_trip();
            printf(" \"*tf\" TDM frame-slip force-trip armed (restart-until-healthy fires on next recover tick)\n");
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
        }
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
