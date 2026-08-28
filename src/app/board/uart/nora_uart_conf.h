#ifndef NORA_UART_CONF_H
#define NORA_UART_CONF_H

//===========================================================
// board/uart/nora_uart_conf.h  (project-supplied HAL config)
//
// Compile-time configuration for the NORA UART HAL (src/app/hal_uart). The HAL
// ships no conf.h of its own: it picks this file up if the project puts one on
// the include path, and otherwise falls back to its documented default, so a
// project that vendors hal_uart without this file keeps the previous behaviour.
// Same arrangement as board/i2c/nora_i2c_conf.h and board/audio/nora_spi_i2s_tdm_conf.h.
//===========================================================


//===========================================================
// How many UART instances this product's per-instance state covers.
//
// The public enum (NORA_UART_INST_1..4, NORA_UART_INST_COUNT) is NOT affected:
// the API surface stays identical on every device, and code written against the
// HAL keeps compiling. What narrows is only the SIZE of the driver's
// per-instance arrays -- and, so the narrowing stays honest,
// nora_uart_is_present() / nora_uart_dspic33ak_get_device() report the
// instances above this count as absent, exactly as they already do for an
// instance the silicon lacks. A narrowed instance therefore fails at init with
// NORA_UART_ERR_NOT_PRESENT instead of writing past an array.
//
// This product uses two: UART1 is the console (uart_platform_board.c,
// uart_platform_irq.c, uart_platform_stdio.c) and UART2 is the PKOB4/USB
// serial-device mirror (uart_platform_uart2_usb_serial_device.c). Nothing in
// src/app references NORA_UART_INST_3 or _4 -- grep before changing this.
// The 20 per-instance arrays in nora_uart_dspic33ak.c and the 6 in
// nora_uart_dspic33ak_rx_isr_ring.c are 216 B + 188 B at 4 instances; two
// instances is half of that (2026-08-22 AK512 ASRC RAM work,
// [internal] report_ak512_asrc_ram_gate_2026-08-22.md).
//
// Raise this (here or with -D) before calling any UART API with instance 3 or 4
// -- including through the CMSIS-Driver wrapper (Driver_USART_dsPIC33AK.c maps
// Driver_USART0 to NORA_UART_INST_1 only, so it needs nothing more today).
// NORA_UART_HW_INST_MAX in nora_uart.h is the ceiling and is checked there.
//===========================================================
#ifndef NORA_UART_INST_SUPPORTED_COUNT
#define NORA_UART_INST_SUPPORTED_COUNT   2
#endif

#endif /* NORA_UART_CONF_H */
