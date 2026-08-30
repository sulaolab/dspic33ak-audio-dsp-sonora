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
 * Atomicity: these are read-modify-write on a whole 32-bit port register, and this core has
 * no atomic set/clear alias for the GPIO SFRs (LATxSET/LATxCLR are a PIC32 feature, not a
 * dsPIC33A one), while `bset`/`bclr` on an SFR is a single instruction only for a COMPILE-TIME
 * constant bit -- which a table-driven pin cannot supply.  A plain RMW here therefore loses a
 * concurrent write to ANY OTHER PIN on the same port: the preempted context reads the port,
 * the preempting one changes a different bit, and the first one writes its stale copy back.
 *
 * So the RMW runs with interrupts masked.  The window is a handful of instructions and the
 * whole HAL is called from configuration code, device chip-selects and LED updates -- never
 * per audio sample -- so the cost does not land in any hot path.
 *
 * The mask SAVES AND RESTORES the interrupt state rather than unconditionally re-enabling, so
 * these are safe to call from an ISR and from inside another critical section.  Read accessors
 * need nothing: a 32-bit load is not a modify.
 *
 * Motivation: the LED level meter is driven from both TDM RX-block ISRs, which today cannot
 * preempt each other only because they share one interrupt priority.  This layer must not be
 * what makes an interrupt-priority decision elsewhere unsafe.
 * Validated on an AK512 16-channel mixed-rate configuration.
 */

/*
 * The three modify accessors share one shape: save the interrupt state, mask, do the RMW,
 * restore. Written out per accessor rather than as a macro taking an operator, so the
 * read-modify-write stays an ordinary expression in each function.
 */
static inline void nora_gpio_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    const unsigned int isr_state = __builtin_get_isr_state();
    __builtin_disable_interrupts();
    *reg |= mask;
    __builtin_set_isr_state(isr_state);
}

static inline void nora_gpio_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    const unsigned int isr_state = __builtin_get_isr_state();
    __builtin_disable_interrupts();
    *reg &= ~mask;
    __builtin_set_isr_state(isr_state);
}

static inline void nora_gpio_reg_toggle(volatile uint32_t *reg, uint32_t mask)
{
    const unsigned int isr_state = __builtin_get_isr_state();
    __builtin_disable_interrupts();
    *reg ^= mask;
    __builtin_set_isr_state(isr_state);
}

static inline bool nora_gpio_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

#endif /* NORA_GPIO_REG_H */
