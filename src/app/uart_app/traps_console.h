#ifndef SONORA_TRAPS_CONSOLE_H
#define SONORA_TRAPS_CONSOLE_H

#include "app_console.h"

// Common "exceptions" console module (module 'x'). The trap latch in surviving RAM
// (diagnostics/app_traps.c) read on demand, plus the means to fire a trap deliberately.
//   ?xl : last trap + traps since power-on (read-only, does NOT consume the record)
//   *xa : force an address error   *xm : force a math error   *xs : force a stack overflow
// The three forced traps exist only when APP_TRAP_TEST_CMDS is 1 (diagnostics/app_traps.h).
//   *xw : paint the free stack   ?xw : how close the deepest context came to SPLIM
// The watermark pair follows APP_STACK_WATERMARK, which defaults to APP_TRAP_TEST_CMDS.
void traps_console_onmsg( app_console_msg_t* msg );

#endif /* SONORA_TRAPS_CONSOLE_H */
