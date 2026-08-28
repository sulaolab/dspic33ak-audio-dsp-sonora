#ifndef RESIDENT_DE_APP_CONSOLE_H
#define RESIDENT_DE_APP_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_console.h"

/* Console module 'f' -- the application-side face of the resident download
 * engine. Present only in a serial-update image:
 *   ?fu       manifest state
 *   *fu5A     ask the resident bootloader for update mode, application intact
 *   *feaa55   stop periodic telemetry and invalidate the manifest. Does NOT reset --
 *             the App keeps running and recovery is entered by the next reset, so the
 *             erase is observable via ?fu ("erased") first. DESTRUCTIVE and one-way;
 *             own verb letter + two-byte key so no single mistyped character reaches
 *             it. There is deliberately no ?fe.
 *   ?fm       raw shared-SRAM mailbox words
 *   *fmA4     publish the SRAM probe without reset for staged diagnostics
 *   *fmA5     synchronized App -> RESET -> resident dual-SRAM/pre-CRT probe
 */
#if defined(SONORA_DELIVERY_SERIAL_UPDATE_APP)

void resident_de_app_console_onmsg(app_console_msg_t *msg);

#else

/* A standalone image has no resident bootloader, no manifest, and no reserved
 * cross-reset container. Every verb above would act on something that does not
 * exist -- *feaa55 would erase the last Flash page of the panel this very image
 * occupies. So the module is not linked at all, and the dispatcher gets the same
 * answer it gives for any unknown module. This stub is the ONLY place the
 * delivery mode is tested on the application side; no caller needs an #if. */
static inline void resident_de_app_console_onmsg(app_console_msg_t *msg)
{
    if (msg == NULL) { return; }
    msg->data_len = 0u;
    msg->status = APP_CONSOLE_ERR_NOT_FOUND;
}

#endif

#endif
