#ifndef SONORA_SELECTED_APP_H
#define SONORA_SELECTED_APP_H

#include <stdbool.h>

/*
 * Composition-root contract used by main.c.
 *
 * main owns platform/board initialization and preserves its established order.
 * The selected app owns its audio route, app-specific periodic work and app
 * diagnostics.  service() returns true only when the app temporarily owns the
 * remainder of the main-loop iteration (ASRC measurement streaming).
 */
const char* sonora_app_name( void );
void sonora_app_print_banner( void );
void sonora_app_prepare( void );
void sonora_app_start_audio( void );
/* Start optional application-owned outputs after common PWM/RGB initialization. */
void sonora_app_start_aux_output( void );
bool sonora_app_manage_audio( void );
/* Per-loop application control input (e.g. button/touch dispatch). Called once
 * per main-loop iteration; a no-op for apps that own no local controls. */
void sonora_app_process_controls( void );
bool sonora_app_service( void );
void sonora_app_debug_print( void );

#endif /* SONORA_SELECTED_APP_H */
