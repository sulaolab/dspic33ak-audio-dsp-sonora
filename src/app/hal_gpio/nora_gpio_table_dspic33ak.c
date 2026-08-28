/*
 * nora_gpio_table_dspic33ak.c -- apply a board's pin list, and the shared descriptions.
 *
 * See the header for why the list is data rather than control flow.
 *
 * There is nothing device-specific in this file: it goes through nora_gpio_config(),
 * which owns every register access. It carries the family suffix because the rest of
 * hal_gpio does, and because the CK copy is byte-for-byte the same below this comment.
 */

#include "nora_gpio_table.h"

#include <stddef.h>

const nora_gpio_config_t nora_gpio_cfg_output_low = {
    .dir          = NORA_GPIO_DIR_OUTPUT,
    .pull         = NORA_GPIO_PULL_NONE,
    .analog       = false,
    .open_drain   = false,
    .initial_high = false,
};

const nora_gpio_config_t nora_gpio_cfg_output_high = {
    .dir          = NORA_GPIO_DIR_OUTPUT,
    .pull         = NORA_GPIO_PULL_NONE,
    .analog       = false,
    .open_drain   = false,
    .initial_high = true,
};

const nora_gpio_config_t nora_gpio_cfg_input_pullup = {
    .dir          = NORA_GPIO_DIR_INPUT,
    .pull         = NORA_GPIO_PULL_UP,
    .analog       = false,
    .open_drain   = false,
    .initial_high = false,
};

const nora_gpio_config_t nora_gpio_cfg_analog_input = {
    .dir          = NORA_GPIO_DIR_INPUT,
    .pull         = NORA_GPIO_PULL_NONE,
    .analog       = true,
    .open_drain   = false,
    .initial_high = false,
};

bool nora_gpio_table_apply(const nora_gpio_table_entry_t *table,
                                uint8_t                             count)
{
    uint8_t i;

    if ((table == NULL) && (count != 0u)) {
        return false;
    }

    for (i = 0u; i < count; i++) {
        if (!nora_gpio_config(table[i].pin, table[i].config)) {
            return false;
        }
    }

    return true;
}
