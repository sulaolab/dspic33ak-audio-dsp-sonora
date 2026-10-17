#ifndef SONORA_PWM_AUDIO_BOARD_H
#define SONORA_PWM_AUDIO_BOARD_H

/* Main-loop service of the fixed PWM audio board backend: polls the power
 * stage fault input and, on a fault, reaches EMERGENCY_SAFE and latches
 * NORA_PWM_AUDIO_FAULT_STAGE. Cheap when nothing is running. */
void pwm_audio_board_service(void);

/* Scope diagnostics are board-specific; no extension of the portable API. */
#include <stdbool.h>
void pwm_audio_board_scope_print(bool running, bool neutral);

/* Foreground lab control: ignore the external power-stage fault input and
 * allow nSLEEP High without a PCB / bridge supply. The caller stops PWM
 * before changing this mode. No change to the portable HAL contract. */
void pwm_audio_board_set_lab_mode(bool enabled);
void pwm_audio_board_control_print(void);

#endif /* SONORA_PWM_AUDIO_BOARD_H */
