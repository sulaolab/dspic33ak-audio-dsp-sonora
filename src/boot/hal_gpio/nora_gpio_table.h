#ifndef NORA_GPIO_TABLE_H
#define NORA_GPIO_TABLE_H

/*
 * A board's fixed pins as a table, applied in one call.
 *
 * WHAT THIS IS FOR
 * ----------------
 * A board's user-I/O stage is a list: this pin an output starting low, that one an
 * input with a pull-up, this one analog. Spelled as straight-line code it becomes
 * one `if (!...) return false;` per pin, and the LIST, which is the actual board
 * fact, is buried in the control flow that walks it.
 *
 * As a table the fact is the data and the walk is shared. That is the same move the
 * app-layer seams already make one level up, and the point of it here is that two
 * boards' pin stages stop being two functions that merely resemble each other and
 * become two tables with one applier.
 *
 * SCOPE NOTE FOR THIS FAMILY. This file exists on dsPIC33AK for API parity with the
 * dsPIC33CK NORA HAL, where two boards (DM330030's eight pins, EV88G73A's two) drove
 * it. This codebase currently configures two debug pins from one shared config, so
 * the table saves it no lines today -- it is here so a board written against either
 * family compiles against both, and so the next board added here has the seam
 * already. Nothing in it is device-specific: the backend .c is identical on both
 * families apart from its file name.
 *
 * A DELIBERATELY THIN INTERFACE
 * -----------------------------
 * No failure index is reported, only pass/fail. It was tempting -- a table makes
 * "which entry refused" nearly free to compute -- but the straight-line code it
 * replaces did not report it either and no caller has asked. Add it when something
 * wants it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nora_gpio.h"

typedef struct {
    nora_gpio_pin_t           pin;
    const nora_gpio_config_t *config;
} nora_gpio_table_entry_t;

/*
 * Apply every entry in order, stopping at the first refusal.
 *
 * ENTRIES ARE INDEPENDENT, and that is a property worth keeping. The walk is top to
 * bottom because it is a loop, but no board's table may rely on it: each entry states
 * one pin completely (direction, pull, analog, open-drain, initial level), so applying
 * them in any order gives the same result. If a new board needs pin B configured after
 * pin A, that is a sign the pin descriptions are incomplete; fix the description
 * rather than relying on the sequence.
 *
 * (The CK copy of this header carries the history behind that rule: a board there once
 * cleared ANSEL across all ports at boot and had to re-assert its analog pin
 * afterwards, which only worked if that entry came last. The bulk sweep was deleted;
 * this codebase never had one, each pin owning its own ANSEL.)
 */
bool nora_gpio_table_apply(const nora_gpio_table_entry_t *table,
                                uint8_t                             count);

/*
 * The four descriptions a board would otherwise write out for itself.
 *
 * Sharing them is not just less text -- a pull-up that is stated once cannot be
 * stated inconsistently, and on some boards the pull-up is load-bearing (a button
 * that ties its pin to GND through the switch and nothing else leaves the pin
 * floating when released).
 *
 * A board that needs something these do not describe still writes its own struct
 * and points an entry at it; nothing here is exhaustive.
 */
extern const nora_gpio_config_t nora_gpio_cfg_output_low;
extern const nora_gpio_config_t nora_gpio_cfg_output_high;
extern const nora_gpio_config_t nora_gpio_cfg_input_pullup;
/* Analog input with NO pull: a potentiometer is a divider driven from both rails,
 * and an internal pull would sit across one leg of it and skew the reading. */
extern const nora_gpio_config_t nora_gpio_cfg_analog_input;

#endif /* NORA_GPIO_TABLE_H */
