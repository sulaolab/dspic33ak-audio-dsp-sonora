#ifndef RTE_DEVICE_USART_DSPIC33AK_EXAMPLE_H
#define RTE_DEVICE_USART_DSPIC33AK_EXAMPLE_H

/*
 * Example RTE configuration for the dsPIC33AK USART CMSIS-Driver wrapper.
 *
 * This is a USART-only example configuration file.
 * It is not intended to be a shared application-level RTE_Device.h.
 * In an integrated application, copy the required USART definitions into that
 * application's RTE_Device.h or equivalent configuration header.
 * Do not add I2C/SPI/etc. settings to this USART example file.
 *
 * The values below are self-contained example values (UART1 clocked at
 * 200 MHz, console baud 230400). Adjust UART_CLK_HZ / BAUDRATE / IRQ priorities
 * / RX ring size to the target board. A common generic baud rate is 115200u;
 * this example uses 230400u to mirror the validated Sonora console setup
 * without depending on Sonora-only platform headers.
 */

#include <stdbool.h>

/*
 * The application / board integration layer must prepare the UART clock, GPIO,
 * and PPS routing before the CMSIS driver powers UART1.
 */
bool app_uart1_platform_prepare(void);

#define RTE_USART1 1

#define RTE_USART1_PLATFORM_PREPARE() \
    app_uart1_platform_prepare()

#define RTE_USART1_UART_CLK_HZ       200000000u   /* example board UART clock     */
#define RTE_USART1_BAUDRATE          230400u      /* board console baud           */
#define RTE_USART1_TX_ENABLE         1u
#define RTE_USART1_RX_ENABLE         1u
#define RTE_USART1_RX_RING_SIZE      256u         /* async RX uses the ISR ring    */
#define RTE_USART1_RX_IRQ_PRIORITY   5u
#define RTE_USART1_TX_IRQ_PRIORITY   5u

#endif /* RTE_DEVICE_USART_DSPIC33AK_EXAMPLE_H */
