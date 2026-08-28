#ifndef SONORA_DIAG_CONSOLE_H
#define SONORA_DIAG_CONSOLE_H

#include "app_console.h"

// Common "diagnostics" console module (module 'd'). Low-level register/clock/perf dumps that do
// not belong to system, transport, or any application -- read-only.
//   ?dr : codec (WM8904) register dump (was ?ntCD); data[0] = codec instance
void diag_console_onmsg( app_console_msg_t* msg );

#endif /* SONORA_DIAG_CONSOLE_H */
