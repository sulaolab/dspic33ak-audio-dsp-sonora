#ifndef SONORA_AUDIO_TRANSPORT_CONSOLE_H
#define SONORA_AUDIO_TRANSPORT_CONSOLE_H

#include "app_console.h"

// Common "transport" console module (module 't'), owned by the shared audio transport. Holds
// app-agnostic transport actions: restart, fault injection, snapshot. Not app-specific -- does
// not depend on ASRC/Classic identity.
//   *ts / ?ts : verified analog mute + terminal stop before programmer reset / report state
//   *tr : mute-bounded same-rate restart (was *nt03)
//   *tf : TDM frame-slip force-trip, arms one recovery episode (was *nt43)
void audio_transport_console_onmsg( app_console_msg_t* msg );

#endif /* SONORA_AUDIO_TRANSPORT_CONSOLE_H */
