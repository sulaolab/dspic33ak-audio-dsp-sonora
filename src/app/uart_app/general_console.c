#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "app_console.h"
#include "general_console.h"
#include "apps/sonora_app.h"   // sonora_app_name(): selected-app build role (neutral contract)

//===========================================================
// general_console.c
//
// Common console module 'g' = general / firmware-protocol basic info. Read-only (kind '?').
// This module is app-agnostic: it never includes an application private header or
// app_specific_config_defs.h, and learns the selected-app build role only through the
// neutral sonora_app_name() contract.
//===========================================================

// Git revision injected by buildtools/build.ps1 as a BARE TOKEN (-DSONORA_GIT_COMMIT=<hash>[_dirty])
// and stringified here -- the same pattern as the boot banner in main.c. Fallback "(unknown)" for
// IDE-direct builds that bypass build.ps1.
#define GEN_STRINGIFY2(x) #x
#define GEN_STRINGIFY(x)  GEN_STRINGIFY2(x)
#ifndef SONORA_GIT_COMMIT
#define SONORA_GIT_COMMIT (unknown)
#endif

void general_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    // Module 'g' is a read-only namespace; there is no write ('*') form.
    if( msg->kind != '?' )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch( msg->name )
    {
    case 'v':   // ?gv / ?gv00 : version = protocol tag + selected-app build role + git commit
        printf( " SONORA console-v2 %s %s\n",
                sonora_app_name(),
                GEN_STRINGIFY( SONORA_GIT_COMMIT ) );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    case 'h':   // ?gh : lightweight hello (parser/response liveness)
        printf( " SONORA console hello\n" );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
