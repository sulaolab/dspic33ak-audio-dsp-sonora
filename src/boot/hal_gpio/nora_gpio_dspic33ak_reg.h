#ifndef NORA_GPIO_REG_H
#define NORA_GPIO_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer for the GPIO HAL.
 *
 * This intentionally uses plain register pointers and bit masks instead of
 * XC-DSC bitfield structures (LATxbits / TRISxbits / ...). The goal is to let
 * one driver body drive any port through a table of register pointers.
 *
 * Width: the dsPIC33AK (dsPIC33A core) GPIO SFRs are declared as 32-bit in the
 * DFP device headers (e.g. "extern volatile uint32_t LATA"), so 32-bit pointers
 * match the header.
 *
 * These accessors are plain read-modify-write. They do NOT disable interrupts.
 * If the same LAT port is updated from both main-line code and an ISR, the
 * caller is responsible for the mutual exclusion (see nora_gpio.h).
 */

static inline void nora_gpio_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void nora_gpio_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline void nora_gpio_reg_toggle(volatile uint32_t *reg, uint32_t mask)
{
    *reg ^= mask;
}

static inline bool nora_gpio_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

#endif /* NORA_GPIO_REG_H */
