#ifndef SONORA_SYSTEM_CONSOLE_H
#define SONORA_SYSTEM_CONSOLE_H

#include "app_console.h"

// Common "system/board" console module (module 's').
//   ?si : board identity / UDID (was *nt04) -- read-only
//   *sr : software reset (dsPIC RESET instruction -> RCON.SWR); reboots the board
//   ?sr : software-reset help + the reset cause latched at this boot
void system_console_onmsg( app_console_msg_t* msg );

#endif /* SONORA_SYSTEM_CONSOLE_H */
