/*
 * timer_app.c
 * -----------
 * Sonora application timer compatibility layer.
 */

#include "timer_app.h"

#include <xc.h>
#include "libpic30.h"

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include "nora_tick_timer.h"
/* DSPload: this vector runs at IPL 4 (NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY), i.e. AT the TDM
 * block ISR's base priority and ABOVE the leg the rate-monotonic map demotes to 3, so it can
 * steal time from a measured leg and has to declare itself. */
#include "hal_timer/nora_cpu_load_prof_fast.h"

void timer_app_init(void)
{
    const nora_tick_timer_config_t config = {
        .timer_clk_hz = (uint32_t)(FCY),
        .irq_priority = NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY,
        .run_in_idle = false,
    };

    if (nora_tick_timer_init(&config) != NORA_TICK_TIMER_OK) {
        while (1) {
            Nop();
        }
    }
}

void __attribute__((interrupt, context)) _T1Interrupt(void)
{
    nora_cpu_load_prof_enter( (uint8_t)NORA_CPU_LOAD_OWNER_OTHER );

    nora_tick_timer_irq_handler();

    nora_cpu_load_prof_exit();
}

uint32_t GetTicks(void)
{
    return nora_tick_timer_get_ms();
}

void delay_ms(uint16_t time_ms)
{
    uint32_t start = GetTicks();

    while ((uint32_t)(GetTicks() - start) < time_ms) {
        ;
    }
}

void delay_us(uint16_t time_us)
{
    uint16_t units_of_10us = time_us / 10u;

    __delay32((uint32_t)(units_of_10us * ((uint32_t)(FCY) / 100000u)));
}
