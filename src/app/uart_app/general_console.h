#ifndef SONORA_GENERAL_CONSOLE_H
#define SONORA_GENERAL_CONSOLE_H

#include "app_console.h"

// Common "general / basic info" console module (module 'g'), owned by the shared console.
// Read-only (kind '?') firmware/protocol basic info that is not owned by any specific
// subsystem. Board-hardware individual info (UDID) stays in the system module ('s'); low-level
// register/clock dumps stay in the diagnostics module ('d').
//   ?gv / ?gv00 : version -- protocol tag + selected-app build role + git commit
//   ?gh         : lightweight hello (parser/response liveness)
void general_console_onmsg( app_console_msg_t* msg );

#endif /* SONORA_GENERAL_CONSOLE_H */
