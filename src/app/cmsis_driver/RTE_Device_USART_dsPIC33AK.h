#ifndef RTE_DEVICE_USART_DSPIC33AK_H
#define RTE_DEVICE_USART_DSPIC33AK_H

/*
 * Sonora RTE configuration for the dsPIC33AK USART CMSIS-Driver wrapper.
 *
 * This is the application configuration consumed by Driver_USART_dsPIC33AK.c.
 * Keep RTE_Device_USART_dsPIC33AK_example.h as the template/reference copy.
 */

#include "uart_platform_board.h"

#define RTE_USART1 1

#define RTE_USART1_PLATFORM_PREPARE() \
    uart_platform_board_apply_pins_and_clock(NORA_UART_INST_1)

#define RTE_USART1_UART_CLK_HZ       UART_PLATFORM_UART1_CLK_HZ
#define RTE_USART1_BAUDRATE          230400u      /* board console baud           */
#define RTE_USART1_TX_ENABLE         1u
#define RTE_USART1_RX_ENABLE         1u
#define RTE_USART1_RX_RING_SIZE      256u         /* async RX uses the ISR ring    */
#define RTE_USART1_RX_IRQ_PRIORITY   5u
#define RTE_USART1_TX_IRQ_PRIORITY   5u

#endif /* RTE_DEVICE_USART_DSPIC33AK_H */
