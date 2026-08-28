#ifndef SONORA_SELECTED_APP_CONSOLE_H
#define SONORA_SELECTED_APP_CONSOLE_H

#include "app_console.h"

/*
 * Console command contract for the linker-selected application.
 *
 * The shared console parser routes every module NOT owned by common code (transport 't',
 * system 's', diagnostics 'd', general 'g', legacy 'n') to this single contract. The selected
 * application implements it exactly once -- ASRC owns module 'a', Classic owns module 'c' -- and
 * the source manifest guarantees one implementation links. The handler validates its own module
 * letter (returning APP_CONSOLE_ERR_NOT_FOUND otherwise) and sets msg->status (APP_CONSOLE_*) plus
 * msg->data_len; line framing and response serialization stay shared. Text output (help, values)
 * is written to the console via printf, mirroring general_console_onmsg().
 */
void sonora_app_console_onmsg( app_console_msg_t* msg );

/*
 * Raw single-key hotkey input for the linker-selected application.
 *
 * The shared UART layer offers every non-console keystroke (i.e. not part of a '*'/'?' command
 * line) to the selected app here. The app consumes the keys it owns and returns IGNORED for the
 * rest, which the UART layer then feeds to the console parser. Keeping this app-blind lets the
 * UART/console infrastructure stay unaware of any specific app's control set.
 *
 * The result also tells the UART layer whether the action may have blocked (e.g. a muted
 * transition with delay_ms), so it can drop keystrokes that queued during the operation --
 * preserving the historical per-key flush behavior without leaking the UART FIFO drain into the
 * app.
 */
typedef enum {
    SONORA_HOTKEY_IGNORED = 0,   /* not this app's hotkey: caller feeds it to the console parser */
    SONORA_HOTKEY_HANDLED,       /* consumed */
    SONORA_HOTKEY_HANDLED_FLUSH, /* consumed; op may have blocked -- caller drops buffered RX */
} sonora_hotkey_result_t;

sonora_hotkey_result_t sonora_app_handle_hotkey( char c );

#endif /* SONORA_SELECTED_APP_CONSOLE_H */
