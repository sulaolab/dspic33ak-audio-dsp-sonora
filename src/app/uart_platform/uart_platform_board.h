#ifndef UART_PLATFORM_BOARD_H
#define UART_PLATFORM_BOARD_H

#include <stdbool.h>
#include "app_specific_config_defs.h"
#include "../hal_uart/nora_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sonora board UART platform layer.
 *
 * This is NOT the reusable UART HAL core.  It is the Sonora project-specific
 * layer that holds this board's UART routing (PPS / GPIO idle) and the
 * recommended UART config (baud / clock).  It lives under
 * src/uart_platform/ so that project-specific details stay out of the public
 * driver (src/hal_uart/).
 *
 * Dependency direction (one-way):
 *     uart_platform_board  ->  nora_uart (public HAL API) + xc.h (PPS/GPIO)
 * It must NOT know about printf, app_console, app_uart_process, or the legacy
 * UART1_* internals.
 *
 * The values here are COPIED (not moved) from the legacy src/uart/uart1.c so
 * the legacy driver stays byte-for-byte unchanged.
 */

/*
 * UART1 clock policy/facts for the Sonora board.
 *
 * CLKGEN8 is configured from PLL1 with divide-by-1 by
 * uart_platform_board_apply_pins_and_clock(). Consumers pass the resulting
 * frequency to the UART HAL; they do not maintain another 200 MHz UART fact.
 */
#define UART_PLATFORM_CLKGEN8_DIVIDE_BY     (1u)
#define UART_PLATFORM_CLKGEN8_CLK_HZ        (PLL1_CLK_HZ / UART_PLATFORM_CLKGEN8_DIVIDE_BY)
#define UART_PLATFORM_UART1_CLK_HZ          UART_PLATFORM_CLKGEN8_CLK_HZ

/*
 * Boot-fault console: UART1 running off the reset-default FRC instead of PLL1,
 * for the one case where the clock bring-up itself failed and there is no PLL1
 * to run the normal console from. The CPU is still alive on the FRC there, and
 * CLKGEN source switching still works, so this is reachable.
 *
 * The baud has to drop: from an 8 MHz UART clock, 230400 lands 3.6% off and is
 * unusable, while 19200 lands 0.16% off. So when investigating a boot failure,
 * start the monitor with --baud 19200.
 */
#define UART_PLATFORM_FRC_CLK_HZ            (8000000UL)
#define UART_PLATFORM_BOOT_FAULT_BAUD       (19200u)

typedef struct {
    nora_uart_instance_t inst;
    nora_uart_config_t config;
} uart_platform_board_config_t;

/*
 * Return the default UART config for the Sonora board/project.
 *
 * This is a board/project helper, not a generic UART HAL API.
 * Returns false for instances that the board does not define.
 */
bool uart_platform_board_get_default_config(
    nora_uart_instance_t inst,
    uart_platform_board_config_t *out);

/*
 * Configure CLKGEN8, PPS, GPIO direction, idle state, and UART clock source for
 * the selected UART instance on the Sonora board.
 *
 * This is intentionally separate from nora_uart_init() so that the HAL
 * core remains reusable and board-specific pin routing stays isolated.
 * Returns false for unsupported instances / unknown device.
 */
bool uart_platform_board_apply_pins_and_clock(
    nora_uart_instance_t inst);

/*
 * Same, but source CLKGEN8 from the FRC instead of PLL1 -- for the boot-fault
 * console only, when PLL1 is not available. See UART_PLATFORM_BOOT_FAULT_BAUD.
 */
bool uart_platform_board_apply_pins_and_frc_clock(
    nora_uart_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* UART_PLATFORM_BOARD_H */
