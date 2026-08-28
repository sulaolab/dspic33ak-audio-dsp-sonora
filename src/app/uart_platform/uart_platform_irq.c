/*
 * UART hardware interrupt vector forwarding for the Sonora board.
 *
 * Keep this unit path-neutral: both the legacy UART1_* wrapper and future
 * CMSIS USART paths use the same UART HAL interrupt handlers.
 */

#include <xc.h>

#include "app_specific_config_defs.h"
#include "nora_uart.h"
/* DSPload: the UART vectors run at IPL 5, above the TDM block ISR on both priority maps, so any
 * of them that can fire while audio is streaming steals measurable time from a leg. */
#include "hal_timer/nora_cpu_load_prof_fast.h"

void __attribute__((interrupt, context)) _U1RXInterrupt(void)
{
    nora_cpu_load_prof_enter( (uint8_t)NORA_CPU_LOAD_OWNER_OTHER );

    nora_uart_rx_irq_handler(NORA_UART_INST_1);

    nora_cpu_load_prof_exit();
}

/* NOT instrumented, on purpose. Console TX is what prints the DSPload line itself, so hooking it
 * would fold the cost of REPORTING the measurement into the measurement -- and it fires in dense
 * bursts around each report, exactly where an added hook is most visible. Its time therefore
 * stays in the NONE (foreground) bucket. If a telemetry burst is ever suspected of pushing a leg
 * past its deadline, that is a response-time question for the per-leg lines, not this one. */
void __attribute__((interrupt, context)) _U1TXInterrupt(void)
{
    nora_uart_tx_irq_handler(NORA_UART_INST_1);
}

#if APP_ASRC_MEAS_UART2_STREAM
/* Q19: interrupt-driven UART2 binary DATA-port TX (IPL5, above the SPI2 block ISR). Only the
 * stream build arms UART2 async TX (tx_irq_priority=5); the normal build leaves UART2 TX polled
 * with no U2TX vector, so this is compiled out and the normal build is byte-identical. */
void __attribute__((interrupt, context)) _U2TXInterrupt(void)
{
    nora_cpu_load_prof_enter( (uint8_t)NORA_CPU_LOAD_OWNER_OTHER );

    nora_uart_tx_irq_handler(NORA_UART_INST_2);

    nora_cpu_load_prof_exit();
}
#endif
