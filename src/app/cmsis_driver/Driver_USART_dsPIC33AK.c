/* ========================================================================== */
/* CMSIS-Driver USART wrapper for the dsPIC33AK UART HAL (UART1, initial scope) */
/* ========================================================================== */

/*
 * Maps the ARM CMSIS-Driver USART API onto the dsPIC33AK UART HAL asynchronous
 * transfer model. All ARM_USART_* / ARM_DRIVER_* types live here, never in the
 * HAL.
 *
 * Supported (initial): ARM_USART_MODE_ASYNCHRONOUS, 8 data bits, no parity,
 * 1 stop bit, no flow control. Unsupported modes return
 * ARM_DRIVER_ERROR_UNSUPPORTED; unsupported format/control options return
 * CMSIS USART-specific ARM_USART_ERROR_* values where available.
 *
 * Send()/Receive() map onto the HAL async transfer API:
 *   - Send(data, num): the caller-provided buffer must remain valid until the
 *     ARM_USART_EVENT_SEND_COMPLETE callback fires (the HAL transmits straight
 *     from it; this wrapper does not copy).
 *   - Receive(data, num): starts a clean RX transfer, discarding pre-existing
 *     ring/FIFO bytes atomically with arming the async receive; the caller
 *     buffer must remain valid until ARM_USART_EVENT_RECEIVE_COMPLETE.
 *
 * The signal-event callback may run in interrupt (TX/RX ISR) context: keep it
 * short, do not block, and do not printf or call blocking driver APIs from it.
 *
 * NOTE on SEND_COMPLETE: the HAL NORA_UART_EVENT_SEND_COMPLETE means "all
 * data submitted to the TX FIFO", which maps to ARM_USART_EVENT_SEND_COMPLETE.
 * It is NOT physical shift-register-empty, so ARM_USART_EVENT_TX_COMPLETE is
 * never signalled and capabilities.event_tx_complete is 0.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Driver_USART_dsPIC33AK.h"
#include "RTE_Device_USART_dsPIC33AK.h"
#include "nora_uart.h"

/* This wrapper's own implementation version (0.x = initial / pre-release). The
 * CMSIS USART API version it conforms to is ARM_USART_API_VERSION (2.4). */
#define DRIVER_USART_DSPIC33AK_VERSION  ARM_DRIVER_VERSION_MAJOR_MINOR(0, 1)

/* ========================================================================== */
/* Driver version and capabilities                                            */
/* ========================================================================== */

static const ARM_DRIVER_VERSION usart_driver_version = {
    ARM_USART_API_VERSION,             /* api: CMSIS USART API version (2.4)   */
    DRIVER_USART_DSPIC33AK_VERSION     /* drv: this wrapper's version (0.1)    */
};

/* Asynchronous UART only; no synchronous / single-wire / IrDA / smart-card /
 * flow-control / modem lines, and no optional TX-complete or RX-timeout events. */
static const ARM_USART_CAPABILITIES usart_capabilities = {
    1u, /* asynchronous       */
    0u, /* synchronous_master */
    0u, /* synchronous_slave  */
    0u, /* single_wire        */
    0u, /* irda               */
    0u, /* smart_card         */
    0u, /* smart_card_clock   */
    0u, /* flow_control_rts   */
    0u, /* flow_control_cts   */
    0u, /* event_tx_complete  */
    0u, /* event_rx_timeout   */
    0u, /* rts                */
    0u, /* cts                */
    0u, /* dtr                */
    0u, /* dsr                */
    0u, /* dcd                */
    0u, /* ri                 */
    0u, /* event_cts          */
    0u, /* event_dsr          */
    0u, /* event_dcd          */
    0u, /* event_ri           */
    0u  /* reserved           */
};

/* ========================================================================== */
/* Per-instance context (UART1 only for the initial wrapper)                  */
/* ========================================================================== */

#if (RTE_USART1 != 0)

#ifndef RTE_USART1_PLATFORM_PREPARE
#error "RTE_USART1_PLATFORM_PREPARE must be defined by RTE_Device_USART_dsPIC33AK.h"
#endif

typedef struct {
    ARM_USART_SignalEvent_t cb_event;
    uint32_t uart_clk_hz;
    uint32_t baudrate;
    bool     initialized;
    bool     powered;

    /* RX error sticky flags, set from the HAL event callback and cleared at the
     * start of each Receive(); surfaced through GetStatus(). */
    volatile uint8_t rx_overflow;
    volatile uint8_t rx_framing_error;
    volatile uint8_t rx_parity_error;
} usart_ctx_t;

static usart_ctx_t usart1_ctx;

/* Async RX requires the HAL ISR ring backend; the ring storage is owned here. */
static uint8_t usart1_rx_ring[RTE_USART1_RX_RING_SIZE];

#define USART1_HAL_INST  NORA_UART_INST_1

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

/* Map a HAL status code to a CMSIS execution status. */
static int32_t usart_hal_to_arm(nora_uart_status_t st)
{
    switch (st) {
    case NORA_UART_OK:                  return ARM_DRIVER_OK;
    case NORA_UART_ERR_INVALID_ARG:     return ARM_DRIVER_ERROR_PARAMETER;
    case NORA_UART_ERR_BUSY:            return ARM_DRIVER_ERROR_BUSY;
    case NORA_UART_ERR_UNSUPPORTED:     return ARM_DRIVER_ERROR_UNSUPPORTED;
    case NORA_UART_ERR_NOT_INITIALIZED: return ARM_DRIVER_ERROR;
    default:                                 return ARM_DRIVER_ERROR;
    }
}

/* HAL event callback (TX/RX ISR context). Translates HAL events to CMSIS events
 * and records RX error flags. SEND_COMPLETE maps only to
 * ARM_USART_EVENT_SEND_COMPLETE (never ARM_USART_EVENT_TX_COMPLETE).
 * NORA_UART_EVENT_RX_READY is unsolicited byte-stream data and is ignored
 * here (it is not a CMSIS receive-complete). */
static void usart1_hal_event(nora_uart_instance_t inst, uint32_t events, void *user_data)
{
    usart_ctx_t *ctx = &usart1_ctx;
    uint32_t arm_events = 0u;

    (void)inst;
    (void)user_data;

    if (events & NORA_UART_EVENT_SEND_COMPLETE) {
        arm_events |= ARM_USART_EVENT_SEND_COMPLETE;
    }
    if (events & NORA_UART_EVENT_RX_COMPLETE) {
        arm_events |= ARM_USART_EVENT_RECEIVE_COMPLETE;
    }
    if (events & (NORA_UART_EVENT_RX_OVERFLOW |
                  NORA_UART_EVENT_RX_OVERRUN_ERROR)) {
        ctx->rx_overflow = 1u;
        arm_events |= ARM_USART_EVENT_RX_OVERFLOW;
    }
    if (events & NORA_UART_EVENT_RX_FRAMING_ERROR) {
        ctx->rx_framing_error = 1u;
        arm_events |= ARM_USART_EVENT_RX_FRAMING_ERROR;
    }
    if (events & NORA_UART_EVENT_RX_PARITY_ERROR) {
        ctx->rx_parity_error = 1u;
        arm_events |= ARM_USART_EVENT_RX_PARITY_ERROR;
    }

    if ((arm_events != 0u) && (ctx->cb_event != NULL)) {
        ctx->cb_event(arm_events);
    }
}

/* ========================================================================== */
/* CMSIS USART driver functions (UART1)                                       */
/* ========================================================================== */

static ARM_DRIVER_VERSION USART1_GetVersion(void)
{
    return usart_driver_version;
}

static ARM_USART_CAPABILITIES USART1_GetCapabilities(void)
{
    return usart_capabilities;
}

static int32_t USART1_Initialize(ARM_USART_SignalEvent_t cb_event)
{
    usart_ctx_t *ctx = &usart1_ctx;

    if (ctx->powered) {
        ctx->cb_event = cb_event;
        return ARM_DRIVER_OK;
    }

    ctx->cb_event         = cb_event;
    ctx->baudrate         = 0u;
    ctx->uart_clk_hz      = 0u;
    ctx->rx_overflow      = 0u;
    ctx->rx_framing_error = 0u;
    ctx->rx_parity_error  = 0u;
    ctx->powered          = false;
    ctx->initialized      = true;

    /* The HAL callback is registered in PowerControl(ARM_POWER_FULL), after the
     * HAL is initialized (nora_uart_init clears any registered callback). */
    return ARM_DRIVER_OK;
}

static int32_t USART1_PowerControl(ARM_POWER_STATE state);  /* fwd for Uninitialize */

static int32_t USART1_Uninitialize(void)
{
    usart_ctx_t *ctx = &usart1_ctx;

    if (ctx->powered) {
        (void)USART1_PowerControl(ARM_POWER_OFF);
    }

    ctx->cb_event    = NULL;
    ctx->initialized = false;
    return ARM_DRIVER_OK;
}

static int32_t USART1_PowerControl(ARM_POWER_STATE state)
{
    usart_ctx_t *ctx = &usart1_ctx;
    nora_uart_config_t cfg;
    nora_uart_status_t st;

    if (!ctx->initialized) {
        return ARM_DRIVER_ERROR;
    }

    switch (state) {
    case ARM_POWER_FULL:
        if (ctx->powered) {
            return ARM_DRIVER_OK;
        }

        if (!RTE_USART1_PLATFORM_PREPARE()) {
            return ARM_DRIVER_ERROR;
        }

        /* 8N1 asynchronous, ISR ring RX (required for async Receive). */
        cfg.uart_clk_hz        = RTE_USART1_UART_CLK_HZ;
        cfg.baudrate           = RTE_USART1_BAUDRATE;
        cfg.timeout_ms         = 0u;
        cfg.get_ms             = NULL;
        cfg.data_bits          = 8u;
        cfg.stop_bits          = 1u;
        cfg.parity             = NORA_UART_PARITY_NONE;
        cfg.enable_tx          = (RTE_USART1_TX_ENABLE != 0u);
        cfg.enable_rx          = (RTE_USART1_RX_ENABLE != 0u);
        cfg.rx_mode            = NORA_UART_RX_MODE_ISR_RING;
        cfg.rx_ring_buffer     = usart1_rx_ring;
        cfg.rx_ring_buffer_size = (uint16_t)sizeof(usart1_rx_ring);
        cfg.rx_irq_priority    = RTE_USART1_RX_IRQ_PRIORITY;
        cfg.tx_irq_priority    = RTE_USART1_TX_IRQ_PRIORITY;

        st = nora_uart_init(USART1_HAL_INST, &cfg);
        if (st != NORA_UART_OK) {
            return usart_hal_to_arm(st);
        }

        (void)nora_uart_set_callback(USART1_HAL_INST, usart1_hal_event, NULL);

        ctx->uart_clk_hz      = RTE_USART1_UART_CLK_HZ;
        ctx->baudrate         = RTE_USART1_BAUDRATE;
        ctx->rx_overflow      = 0u;
        ctx->rx_framing_error = 0u;
        ctx->rx_parity_error  = 0u;
        ctx->powered          = true;
        return ARM_DRIVER_OK;

    case ARM_POWER_OFF:
        if (ctx->powered) {
            (void)nora_uart_tx_abort(USART1_HAL_INST);
            (void)nora_uart_rx_abort(USART1_HAL_INST);
            (void)nora_uart_set_callback(USART1_HAL_INST, NULL, NULL);
            (void)nora_uart_deinit(USART1_HAL_INST);
        }
        ctx->powered = false;
        return ARM_DRIVER_OK;

    case ARM_POWER_LOW:
        return ARM_DRIVER_ERROR_UNSUPPORTED;

    default:
        return ARM_DRIVER_ERROR_PARAMETER;
    }
}

static int32_t USART1_Send(const void *data, uint32_t num)
{
    usart_ctx_t *ctx = &usart1_ctx;
    nora_uart_status_t st;

    if (!ctx->initialized || !ctx->powered) {
        return ARM_DRIVER_ERROR;
    }
    if ((data == NULL) || (num == 0u)) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    if (nora_uart_tx_is_busy(USART1_HAL_INST)) {
        return ARM_DRIVER_ERROR_BUSY;
    }

    /* Non-blocking: the HAL transmits straight from the caller buffer, which
     * must stay valid until ARM_USART_EVENT_SEND_COMPLETE. */
    st = nora_uart_tx_start(USART1_HAL_INST, (const uint8_t *)data, (size_t)num);
    return usart_hal_to_arm(st);
}

static int32_t USART1_Receive(void *data, uint32_t num)
{
    usart_ctx_t *ctx = &usart1_ctx;
    nora_uart_status_t st;

    if (!ctx->initialized || !ctx->powered) {
        return ARM_DRIVER_ERROR;
    }
    if ((data == NULL) || (num == 0u)) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    if (nora_uart_rx_is_busy(USART1_HAL_INST)) {
        return ARM_DRIVER_ERROR_BUSY;
    }

    /* New receive operation: clear sticky RX error flags. */
    ctx->rx_overflow      = 0u;
    ctx->rx_framing_error = 0u;
    ctx->rx_parity_error  = 0u;

    /* Discard bytes buffered before this call without opening a flush/start
     * race window; bytes arriving after the clean arm are captured by Receive(). */
    st = nora_uart_rx_start_clean(USART1_HAL_INST, (uint8_t *)data, (size_t)num);
    return usart_hal_to_arm(st);
}

static int32_t USART1_Transfer(const void *data_out, void *data_in, uint32_t num)
{
    /* Synchronous full-duplex transfer is not supported by this async UART. */
    (void)data_out;
    (void)data_in;
    (void)num;
    return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static uint32_t USART1_GetTxCount(void)
{
    return (uint32_t)nora_uart_tx_count_get(USART1_HAL_INST);
}

static uint32_t USART1_GetRxCount(void)
{
    return (uint32_t)nora_uart_rx_count_get(USART1_HAL_INST);
}

/* Apply an ARM_USART_MODE_ASYNCHRONOUS control word. Only 8N1 / no-flow is
 * accepted; arg is the baud rate. */
static int32_t usart1_set_async_mode(usart_ctx_t *ctx, uint32_t control, uint32_t arg)
{
    nora_uart_status_t st;

    if (!ctx->powered) {
        return ARM_DRIVER_ERROR;
    }
    if ((control & ARM_USART_DATA_BITS_Msk) != ARM_USART_DATA_BITS_8) {
        return ARM_USART_ERROR_DATA_BITS;
    }
    if ((control & ARM_USART_PARITY_Msk) != ARM_USART_PARITY_NONE) {
        return ARM_USART_ERROR_PARITY;
    }
    if ((control & ARM_USART_STOP_BITS_Msk) != ARM_USART_STOP_BITS_1) {
        return ARM_USART_ERROR_STOP_BITS;
    }
    if ((control & ARM_USART_FLOW_CONTROL_Msk) != ARM_USART_FLOW_CONTROL_NONE) {
        return ARM_USART_ERROR_FLOW_CONTROL;
    }
    if (arg == 0u) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    st = nora_uart_set_baudrate(USART1_HAL_INST, ctx->uart_clk_hz, arg);
    if (st != NORA_UART_OK) {
        return usart_hal_to_arm(st);
    }
    ctx->baudrate = arg;
    return ARM_DRIVER_OK;
}

static int32_t USART1_Control(uint32_t control, uint32_t arg)
{
    usart_ctx_t *ctx = &usart1_ctx;

    if (!ctx->initialized) {
        return ARM_DRIVER_ERROR;
    }

    switch (control & ARM_USART_CONTROL_Msk) {
    case ARM_USART_MODE_ASYNCHRONOUS:
        return usart1_set_async_mode(ctx, control, arg);

    /* Unsupported operating modes. */
    case ARM_USART_MODE_SYNCHRONOUS_MASTER:
    case ARM_USART_MODE_SYNCHRONOUS_SLAVE:
    case ARM_USART_MODE_SINGLE_WIRE:
    case ARM_USART_MODE_IRDA:
    case ARM_USART_MODE_SMART_CARD:
        return ARM_DRIVER_ERROR_UNSUPPORTED;

    case ARM_USART_CONTROL_TX:
        if (!ctx->powered) {
            return ARM_DRIVER_ERROR;
        }
        return usart_hal_to_arm(
            nora_uart_tx_enable(USART1_HAL_INST, (arg != 0u)));

    case ARM_USART_CONTROL_RX:
        if (!ctx->powered) {
            return ARM_DRIVER_ERROR;
        }
        return usart_hal_to_arm(
            nora_uart_rx_enable(USART1_HAL_INST, (arg != 0u)));

    case ARM_USART_ABORT_SEND:
        return usart_hal_to_arm(nora_uart_tx_abort(USART1_HAL_INST));

    case ARM_USART_ABORT_RECEIVE:
        return usart_hal_to_arm(nora_uart_rx_abort(USART1_HAL_INST));

    case ARM_USART_ABORT_TRANSFER:
        (void)nora_uart_tx_abort(USART1_HAL_INST);
        (void)nora_uart_rx_abort(USART1_HAL_INST);
        return ARM_DRIVER_OK;

    /* Continuous break, modem/smart-card/IrDA tuning: not supported. */
    case ARM_USART_CONTROL_BREAK:
    case ARM_USART_SET_DEFAULT_TX_VALUE:
    case ARM_USART_SET_IRDA_PULSE:
    case ARM_USART_SET_SMART_CARD_GUARD_TIME:
    case ARM_USART_SET_SMART_CARD_CLOCK:
    case ARM_USART_CONTROL_SMART_CARD_NACK:
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static ARM_USART_STATUS USART1_GetStatus(void)
{
    usart_ctx_t *ctx = &usart1_ctx;
    ARM_USART_STATUS status = {0};

    status.tx_busy          = nora_uart_tx_is_busy(USART1_HAL_INST) ? 1u : 0u;
    status.rx_busy          = nora_uart_rx_is_busy(USART1_HAL_INST) ? 1u : 0u;
    status.rx_overflow      = ctx->rx_overflow;
    status.rx_framing_error = ctx->rx_framing_error;
    status.rx_parity_error  = ctx->rx_parity_error;
    /* tx_underflow / rx_break stay 0 (not applicable to this async wrapper). */
    return status;
}

static int32_t USART1_SetModemControl(ARM_USART_MODEM_CONTROL control)
{
    (void)control;
    return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static ARM_USART_MODEM_STATUS USART1_GetModemStatus(void)
{
    ARM_USART_MODEM_STATUS status = {0};   /* no modem lines: all inactive */
    return status;
}

/* ========================================================================== */
/* Driver access structure                                                    */
/* ========================================================================== */

ARM_DRIVER_USART Driver_USART1 = {
    USART1_GetVersion,
    USART1_GetCapabilities,
    USART1_Initialize,
    USART1_Uninitialize,
    USART1_PowerControl,
    USART1_Send,
    USART1_Receive,
    USART1_Transfer,
    USART1_GetTxCount,
    USART1_GetRxCount,
    USART1_Control,
    USART1_GetStatus,
    USART1_SetModemControl,
    USART1_GetModemStatus
};

#endif /* RTE_USART1 != 0 */
