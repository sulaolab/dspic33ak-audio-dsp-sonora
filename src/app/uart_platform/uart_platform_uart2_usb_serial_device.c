#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nora_gpio.h"
#include "nora_pps.h"
#include "nora_uart.h"
#include "uart_platform_board.h"
#include "uart_platform_uart2_usb_serial_device.h"

/*
 * UART2 -> PKOB4 -> Windows "USB Serial Device" -> Console endpoint.
 *
 * Interactive command input + mirrored console output (Biquad CSV bulk transfer
 * is rejected on this port -- UART1 only; see app_debug.c CSV BEGIN policy).
 *
 * The PKOB4 back-channel UART nets on the DIM edge (P100 = device RX,
 * P102 = device TX) map to different device RP pins per board:
 *
 *   AK512 (EV74H48A / dsPIC33AK512MPS512 DIM):
 *     TX: U2TX  -> RH0/RP113 -> DIM P102_UART_PKoB_TX -> PKOB4
 *     RX: PKOB4 -> DIM P100_UART_PKoB_RX -> RD10/RP59 -> U2RX
 *   AK128 (dsPIC33AK128MC106 General-Purpose DIM, DS70005556):
 *     TX: U2TX  -> RD9/RP58  -> DIM P102_UART_PKoB_TX -> PKOB4
 *     RX: PKOB4 -> DIM P100_UART_PKoB_RX -> RD10/RP59 -> U2RX
 *
 *   RH0/RP113 does not exist on AK128MC106; RD10/RP59 (RX) is common to both.
 *   On the AK512 board RP58/RD9 is the red RGB LED (see pwm.c) -- that pin is
 *   only repurposed as U2TX on the AK128 DIM, so the two boards do not clash.
 */
#define UART2_USB_SERIAL_DEVICE_INST          (NORA_UART_INST_2)

#if   APP_TARGET == APP_TARGET_AK512
#define UART2_USB_SERIAL_DEVICE_TX_RP         ((nora_gpio_rp_t)113u)  /* RH0 */
#elif APP_TARGET == APP_TARGET_AK128
#define UART2_USB_SERIAL_DEVICE_TX_RP         ((nora_gpio_rp_t)58u)   /* RD9 */
#else
#error "Unhandled APP_TARGET in UART2 USB Serial Device TX pin map -- add an arm for the new device"
#endif

#define UART2_USB_SERIAL_DEVICE_RX_RP         ((nora_gpio_rp_t)59u)   /* RD10 (both boards) */
#if APP_ASRC_MEAS_UART2_STREAM
/* Stream build: UART2 is a dedicated TX-only binary DATA port at the stream baud. */
#define UART2_USB_SERIAL_DEVICE_BAUDRATE      ((uint32_t)APP_ASRC_STREAM_BAUD)
#else
#define UART2_USB_SERIAL_DEVICE_BAUDRATE      (UART_BRG)
#endif
#define UART2_USB_SERIAL_DEVICE_CLK_HZ        (UART_PLATFORM_CLKGEN8_CLK_HZ)

static bool s_ready = false;

static size_t write_direct(const uint8_t *data, size_t length);

/*
 * Core port API -- names deliberately parallel to UART1_* (see
 * uart_platform_uart1_usb_serial_port.h). UART1 is the master naming; UART2
 * mirrors it so the two Windows ports read the same at the call sites.
 */

nora_uart_status_t UART2_Initialize(void)
{
    nora_uart_config_t config;
    nora_uart_status_t st;

    s_ready = false;

    if (!nora_uart_is_present(UART2_USB_SERIAL_DEVICE_INST)) {
        return NORA_UART_ERR_NOT_PRESENT;
    }

    if (!nora_pinmux_route_output(NORA_PPS_OUTPUT_U2TX,
                                       UART2_USB_SERIAL_DEVICE_TX_RP, true)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }
    if (!nora_pinmux_route_input(NORA_PPS_INPUT_U2RX,
                                      UART2_USB_SERIAL_DEVICE_RX_RP)) {
        return NORA_UART_ERR_UNSUPPORTED;
    }

    config.uart_clk_hz         = (uint32_t)UART2_USB_SERIAL_DEVICE_CLK_HZ;
    config.baudrate            = (uint32_t)UART2_USB_SERIAL_DEVICE_BAUDRATE;
    config.timeout_ms          = 0u;
    config.get_ms              = 0;
    config.data_bits           = 8u;
    config.stop_bits           = 1u;
    config.parity              = NORA_UART_PARITY_NONE;
    config.enable_tx           = true;
#if APP_ASRC_MEAS_UART2_STREAM
    config.enable_rx           = false;  /* dedicated binary DATA port: TX-only, no command input */
#else
    config.enable_rx           = true;
#endif
    config.rx_mode             = NORA_UART_RX_MODE_POLLING;
    config.rx_ring_buffer      = 0;
    config.rx_ring_buffer_size = 0u;
    config.rx_irq_priority     = 0u;
#if APP_ASRC_MEAS_UART2_STREAM
    /* Interrupt-driven binary DATA-port TX: priority 5 sits ABOVE the SPI2 block/RX-DMA ISR
     * (PRIO_TDM_DMA = 4), so the U2TX interrupt preempts the ~99 us L128 ASRC pull to keep the TX
     * FIFO fed and the 2.0 Mbaud wire saturated (a polled/blocking write got starved during the
     * pull -> effective ~120 kB/s < 138 kB/s production -> ring overflow). 5 ties CCP/UART1 (no
     * mutual preempt); the tiny U2TX ISR never starves the CCP capture. */
    config.tx_irq_priority     = 5u;
#else
    config.tx_irq_priority     = 0u;
#endif

    st = nora_uart_init(UART2_USB_SERIAL_DEVICE_INST, &config);
    s_ready = (st == NORA_UART_OK);
    return st;
}

void UART2_Deinitialize(void)
{
    (void)nora_uart_deinit(UART2_USB_SERIAL_DEVICE_INST);
    s_ready = false;
}

bool UART2_IsRxReady(void)
{
    if (!s_ready) {
        return false;
    }
    return nora_uart_rx_ready(UART2_USB_SERIAL_DEVICE_INST);
}

uint8_t UART2_Read(void)
{
    /* Symmetric with UART1_Read(): returns 0 when no byte is available. */
    uint8_t data = 0u;

    if (!s_ready) {
        return 0u;
    }
    if (nora_uart_read_byte(UART2_USB_SERIAL_DEVICE_INST, &data) != NORA_UART_OK) {
        return 0u;
    }
    return data;
}

void UART2_RxFlush(void)
{
    if (!s_ready) {
        return;
    }
    nora_uart_rx_flush(UART2_USB_SERIAL_DEVICE_INST);
}

/*
 * UART2-only extras (no UART1 twin; named in the same UART2_* style).
 */

/* Mirror TX: echo host-visible console output onto the "USB Serial Device". */
size_t UART2_WriteMirror(const uint8_t *data, size_t length)
{
    return write_direct(data, length);
}

bool UART2_MirrorTxDone(void)
{
    if (!s_ready) {
        return true;   /* nothing in flight if the port never came up */
    }
    return nora_uart_tx_done(UART2_USB_SERIAL_DEVICE_INST);
}

static size_t write_direct(const uint8_t *data, size_t length)
{
    if (!s_ready || (data == 0) || (length == 0u)) {
        return 0u;
    }

    return nora_uart_write(UART2_USB_SERIAL_DEVICE_INST, data, length);
}

#if APP_ASRC_MEAS_UART2_STREAM
/*
 * Dedicated binary DATA-port wrappers (stream build). INTERRUPT-DRIVEN async TX: the streamer
 * submits one contiguous ring run and returns immediately; the high-priority (IPL5) U2TX ISR
 * drains it to the wire in the background, preempting the SPI2 block ISR so the FIFO never starves.
 * All through the public UART HAL -- no register/PPS access. The submitted buffer must stay valid
 * until the transfer completes (the ring keeps [tail..tail+run) occupied until TX-not-busy, so the
 * producer never overwrites in-flight bytes).
 */

/* Non-blocking submit of one run. Returns bytes handed to the async engine (== length) or 0 if a
 * previous transfer is still in flight / the submit was rejected. */
size_t UART2_StreamSubmit(const uint8_t *data, size_t length)
{
    if (!s_ready || (data == 0) || (length == 0u)) {
        return 0u;
    }
    if (nora_uart_tx_is_busy(UART2_USB_SERIAL_DEVICE_INST)) {
        return 0u;
    }
    if (nora_uart_tx_start(UART2_USB_SERIAL_DEVICE_INST, data, length) != NORA_UART_OK) {
        return 0u;
    }
    return length;
}

/* True while an async run is still being handed to the TX FIFO (SEND_COMPLETE not yet reached). */
bool UART2_StreamTxBusy(void)
{
    if (!s_ready) {
        return false;
    }
    return nora_uart_tx_is_busy(UART2_USB_SERIAL_DEVICE_INST);
}

bool UART2_StreamTxDone(void)
{
    if (!s_ready) {
        return true;   /* nothing in flight if the port never came up */
    }
    /* physical shift-register empty AND no async run still queued */
    return nora_uart_tx_done(UART2_USB_SERIAL_DEVICE_INST) &&
           !nora_uart_tx_is_busy(UART2_USB_SERIAL_DEVICE_INST);
}

uint32_t UART2_StreamBaud(void)
{
    if (!s_ready) {
        return 0u;
    }
    return nora_uart_get_baudrate(UART2_USB_SERIAL_DEVICE_INST);
}

uint32_t UART2_StreamClockHz(void)
{
    return (uint32_t)UART2_USB_SERIAL_DEVICE_CLK_HZ;
}
#endif /* APP_ASRC_MEAS_UART2_STREAM */
