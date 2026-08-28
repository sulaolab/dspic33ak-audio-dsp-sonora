// Sonora board-wide debug pin assignments.
#ifndef BOARD_DBG_PINS_H
#define BOARD_DBG_PINS_H

/*
 * board_dbg_pins.h
 * ----------------
 * Shared debug / scope marker pins, expressed as GPIO HAL pin handles so every
 * user (main init, ADC ISRs, TDM ISR) refers to the same board pin definition.
 *
 * These are dsPIC33AK512MPS512 (Curiosity) debug outputs:
 *   DIM56  : RE4
 *   DIM102 : RH0
 *
 * Note: toggling a debug marker through the GPIO HAL is a cross-TU function call
 * (not a single-instruction LAT bit-flip), so it adds a little latency in hot
 * ISR paths. That is acceptable for a coarse scope marker; switch back to a
 * direct register write if exact timing fidelity is ever required.
 */

#include "nora_gpio.h"

#define BOARD_DBG_PIN_E4   NORA_GPIO_PIN(NORA_GPIO_PORT_E, 4)   // DIM56
#define BOARD_DBG_PIN_H0   NORA_GPIO_PIN(NORA_GPIO_PORT_H, 0)   // DIM102

#endif /* BOARD_DBG_PINS_H */
