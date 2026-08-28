#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Driver_I2C_dsPIC33AK.h"
#include "RTE_Device_I2C_dsPIC33AK_example.h"
#include "nora_i2c_master.h"
#include "nora_i2c_slave.h"

#ifndef ARM_I2C_API_VERSION
#define ARM_I2C_API_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(2, 4)
#endif

#ifndef ARM_DRIVER_VERSION_MAJOR_MINOR
#define ARM_DRIVER_VERSION_MAJOR_MINOR(major, minor) (((major) << 8) | (minor))
#endif

#ifndef ARM_I2C_ADDRESS_10BIT
#define ARM_I2C_ADDRESS_10BIT (0x0400UL)   /* CMSIS Driver_I2C.h value */
#endif

#ifndef ARM_I2C_ADDRESS_GC
#define ARM_I2C_ADDRESS_GC (0x8000UL)      /* CMSIS Driver_I2C.h value */
#endif

#define DRIVER_I2C_DSPIC33AK_VERSION ARM_DRIVER_VERSION_MAJOR_MINOR(0, 3)
#define I2C_ADDR7_MASK               0x7Fu

typedef struct {
    nora_i2c_instance_t hal_inst;
    ARM_I2C_SignalEvent_t cb_event;
    ARM_I2C_STATUS status;
    uint32_t data_count;
    uint32_t fcy_hz;
    uint32_t bus_hz;
    uint32_t timeout_ms;
    uint32_t pending_timeout_ms;
    uint8_t enabled;
    uint8_t initialized;
    uint8_t powered;
    uint8_t pending;

    /* Slave role (appended so the positional initializers below zero these). */
    uint8_t        own_addr;   /* 7-bit own address; 0 => master-only instance */
    uint8_t       *rx_buf;     /* armed by SlaveReceive  */
    uint32_t       rx_num;
    uint32_t       rx_idx;
    const uint8_t *tx_buf;     /* armed by SlaveTransmit */
    uint32_t       tx_num;
    uint32_t       tx_idx;
    uint8_t        rx_overflow; /* a byte arrived past the armed receive buffer */
} i2c_cmsis_context_t;

static ARM_DRIVER_VERSION I2C_GetVersion(void);
static ARM_I2C_CAPABILITIES I2C_GetCapabilities(void);
static int32_t I2C_Initialize(uint32_t index, ARM_I2C_SignalEvent_t cb_event);
static int32_t I2C_Uninitialize(uint32_t index);
static int32_t I2C_PowerControl(uint32_t index, ARM_POWER_STATE state);
static int32_t I2C_MasterTransmit(
    uint32_t index,
    uint32_t addr,
    const uint8_t *data,
    uint32_t num,
    bool xfer_pending);
static int32_t I2C_MasterReceive(
    uint32_t index,
    uint32_t addr,
    uint8_t *data,
    uint32_t num,
    bool xfer_pending);
static int32_t I2C_SlaveTransmit(uint32_t index, const uint8_t *data, uint32_t num);
static int32_t I2C_SlaveReceive(uint32_t index, uint8_t *data, uint32_t num);
static int32_t I2C_GetDataCount(uint32_t index);
static int32_t I2C_Control(uint32_t index, uint32_t control, uint32_t arg);
static ARM_I2C_STATUS I2C_GetStatus(uint32_t index);

static int32_t cmsis_return_from_hal(nora_i2c_status_t hal_status);
static uint32_t cmsis_event_from_hal(nora_i2c_status_t hal_status);
static void clear_status(i2c_cmsis_context_t *ctx);
static void begin_master_transfer(i2c_cmsis_context_t *ctx, uint8_t direction);
static int32_t complete_master_transfer(
    i2c_cmsis_context_t *ctx,
    nora_i2c_status_t hal_status,
    uint32_t requested_count);
static bool address_has_unsupported_flags(uint32_t addr);
static bool address_is_addr7(uint32_t addr);

static i2c_cmsis_context_t g_i2c_ctx[4] = {
    {
        NORA_I2C_INST_1,
        NULL,
        {0},
        0u,
        RTE_I2C0_FCY_HZ,
        RTE_I2C0_BUS_HZ,
        RTE_I2C0_TIMEOUT_MS,
        RTE_I2C0_PENDING_TIMEOUT_MS,
        RTE_I2C0,
        0u,
        0u,
        0u,
    },
    {
        NORA_I2C_INST_2,
        NULL,
        {0},
        0u,
        RTE_I2C1_FCY_HZ,
        RTE_I2C1_BUS_HZ,
        RTE_I2C1_TIMEOUT_MS,
        RTE_I2C1_PENDING_TIMEOUT_MS,
        RTE_I2C1,
        0u,
        0u,
        0u,
    },
    {
        NORA_I2C_INST_3,
        NULL,
        {0},
        0u,
        RTE_I2C2_FCY_HZ,
        RTE_I2C2_BUS_HZ,
        RTE_I2C2_TIMEOUT_MS,
        RTE_I2C2_PENDING_TIMEOUT_MS,
        RTE_I2C2,
        0u,
        0u,
        0u,
    },
    {
        NORA_I2C_INST_4,
        NULL,
        {0},
        0u,
        RTE_I2C3_FCY_HZ,
        RTE_I2C3_BUS_HZ,
        RTE_I2C3_TIMEOUT_MS,
        RTE_I2C3_PENDING_TIMEOUT_MS,
        RTE_I2C3,
        0u,
        0u,
        0u,
    },
};

/* The slave HAL callbacks carry no context, so a single slave instance is
 * supported at a time (sufficient for the loopback validation and for typical
 * single-slave applications). g_slave_ctx points at the powered slave. */
static i2c_cmsis_context_t *g_slave_ctx = NULL;

static void slave_cb_addr(bool is_read)
{
    i2c_cmsis_context_t *c = g_slave_ctx;
    if (c == NULL) {
        return;
    }
    c->status.busy = 1u;
    c->status.mode = 0u;                       /* slave */
    c->status.direction = is_read ? 0u : 1u;   /* 0=transmitter, 1=receiver */
    c->data_count = 0u;

    if (is_read) {
        c->tx_idx = 0u;
        if (c->tx_buf == NULL && c->cb_event != NULL) {
            c->cb_event(ARM_I2C_EVENT_SLAVE_TRANSMIT);
        }
    } else {
        c->rx_idx = 0u;
        c->rx_overflow = 0u;
        if (c->rx_buf == NULL && c->cb_event != NULL) {
            c->cb_event(ARM_I2C_EVENT_SLAVE_RECEIVE);
        }
    }
}

static void slave_cb_rx(uint8_t b)
{
    i2c_cmsis_context_t *c = g_slave_ctx;
    if (c == NULL) {
        return;
    }
    if (c->rx_buf != NULL && c->rx_idx < c->rx_num) {
        c->rx_buf[c->rx_idx++] = b;
        c->data_count = c->rx_idx;
    } else {
        /* Byte received past the armed buffer (or with none armed): report it
         * as an incomplete transfer at STOP rather than dropping it silently. */
        c->rx_overflow = 1u;
    }
}

static uint8_t slave_cb_tx(void)
{
    i2c_cmsis_context_t *c = g_slave_ctx;
    uint8_t b = 0xFFu;
    if (c != NULL && c->tx_buf != NULL && c->tx_idx < c->tx_num) {
        b = c->tx_buf[c->tx_idx++];
        c->data_count = c->tx_idx;
    }
    return b;
}

static void slave_cb_stop(void)
{
    i2c_cmsis_context_t *c = g_slave_ctx;
    uint32_t event;
    bool incomplete;

    if (c == NULL) {
        return;
    }
    c->status.busy = 0u;

    /* TRANSFER_DONE only when the armed buffer was exactly satisfied; otherwise
     * the master ended early (or over-ran), which CMSIS reports as INCOMPLETE. */
    if (c->status.direction == 1u) {            /* slave receiver (master wrote) */
        incomplete = (c->rx_buf != NULL && c->rx_idx != c->rx_num) ||
                     (c->rx_overflow != 0u);
    } else {                                    /* slave transmitter (master read) */
        incomplete = (c->tx_buf != NULL && c->tx_idx != c->tx_num);
    }
    event = incomplete ? ARM_I2C_EVENT_TRANSFER_INCOMPLETE
                       : ARM_I2C_EVENT_TRANSFER_DONE;

    /* The armed buffer is one-shot per CMSIS semantics; the app re-arms with
     * SlaveReceive()/SlaveTransmit() for the next transaction. */
    c->rx_buf = NULL;
    c->tx_buf = NULL;
    c->rx_overflow = 0u;
    if (c->cb_event != NULL) {
        c->cb_event(event);
    }
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
uint32_t Driver_I2C_dsPIC33AK_GetMs(void)
{
    return 0u;
}

static ARM_DRIVER_VERSION I2C_GetVersion(void)
{
    ARM_DRIVER_VERSION version;

    version.api = ARM_I2C_API_VERSION;
    version.drv = DRIVER_I2C_DSPIC33AK_VERSION;

    return version;
}

static ARM_I2C_CAPABILITIES I2C_GetCapabilities(void)
{
    ARM_I2C_CAPABILITIES capabilities = {0};

    return capabilities;
}

static int32_t I2C_Initialize(uint32_t index, ARM_I2C_SignalEvent_t cb_event)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    ctx->cb_event = cb_event;
    ctx->initialized = 1u;
    ctx->powered = 0u;
    ctx->pending = 0u;
    ctx->data_count = 0u;
    clear_status(ctx);

    return ARM_DRIVER_OK;
}

static int32_t I2C_Uninitialize(uint32_t index)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];
    nora_i2c_status_t hal_status = NORA_I2C_OK;

    if (ctx->powered != 0u) {
        if (ctx->own_addr != 0u) {
            hal_status = nora_i2c_slave_deinit(ctx->hal_inst);
            if (g_slave_ctx == ctx) {
                g_slave_ctx = NULL;
            }
        } else {
            hal_status = nora_i2c_deinit(ctx->hal_inst);
        }
    }

    ctx->cb_event = NULL;
    ctx->initialized = 0u;
    ctx->powered = 0u;
    ctx->pending = 0u;
    ctx->data_count = 0u;
    clear_status(ctx);

    /* Uninitialize returns the instance to its initial state, so the slave
     * configuration must not survive into the next Initialize(). (PowerControl
     * (ARM_POWER_OFF) keeps it, so a powered-down slave can be re-powered.) */
    ctx->own_addr = 0u;
    ctx->rx_buf = NULL;
    ctx->tx_buf = NULL;
    ctx->rx_num = 0u;
    ctx->tx_num = 0u;
    ctx->rx_idx = 0u;
    ctx->tx_idx = 0u;
    ctx->rx_overflow = 0u;

    return cmsis_return_from_hal(hal_status);
}

static int32_t I2C_PowerControl(uint32_t index, ARM_POWER_STATE state)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];
    nora_i2c_config_t config;
    nora_i2c_status_t hal_status;

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    switch (state) {
    case ARM_POWER_OFF:
        if (ctx->powered != 0u) {
            if (ctx->own_addr != 0u) {
                hal_status = nora_i2c_slave_deinit(ctx->hal_inst);
                if (g_slave_ctx == ctx) {
                    g_slave_ctx = NULL;
                }
            } else {
                hal_status = nora_i2c_deinit(ctx->hal_inst);
            }
        } else {
            hal_status = NORA_I2C_OK;
        }

        ctx->powered = 0u;
        ctx->pending = 0u;
        ctx->data_count = 0u;
        clear_status(ctx);
        return cmsis_return_from_hal(hal_status);

    case ARM_POWER_LOW:
        return ARM_DRIVER_ERROR_UNSUPPORTED;

    case ARM_POWER_FULL:
        if (ctx->initialized == 0u) {
            return ARM_DRIVER_ERROR;
        }

        if (ctx->powered != 0u) {
            return ARM_DRIVER_OK;
        }

        if (ctx->own_addr != 0u) {
            /* Slave role: an own address was set via Control(OWN_ADDRESS).
             * The slave HAL callbacks are context-free, so only one slave can
             * be active at a time; reject a second one rather than silently
             * hijacking the first. */
            nora_i2c_slave_config_t scfg;

            if (g_slave_ctx != NULL && g_slave_ctx != ctx) {
                return ARM_DRIVER_ERROR_BUSY;
            }

            scfg.addr7         = ctx->own_addr;
            scfg.addr_mask     = 0u;
            scfg.clock_stretch = false;
            scfg.on_addr_match = slave_cb_addr;
            scfg.on_rx_byte    = slave_cb_rx;
            scfg.on_tx_byte    = slave_cb_tx;
            scfg.on_stop       = slave_cb_stop;

            g_slave_ctx = ctx;
            ctx->rx_buf = NULL;
            ctx->tx_buf = NULL;
            hal_status = nora_i2c_slave_init(ctx->hal_inst, &scfg);
            if (hal_status != NORA_I2C_OK) {
                g_slave_ctx = NULL;
            }
        } else {
            config.fcy_hz = ctx->fcy_hz;
            config.bus_hz = ctx->bus_hz;
            config.timeout_ms = ctx->timeout_ms;
            config.get_ms = Driver_I2C_dsPIC33AK_GetMs;
            config.pending_timeout_ms = ctx->pending_timeout_ms;

            hal_status = nora_i2c_init(ctx->hal_inst, &config);
        }
        if (hal_status == NORA_I2C_OK) {
            ctx->powered = 1u;
            ctx->pending = 0u;
            ctx->data_count = 0u;
            clear_status(ctx);
        }
        return cmsis_return_from_hal(hal_status);

    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static int32_t I2C_MasterTransmit(
    uint32_t index,
    uint32_t addr,
    const uint8_t *data,
    uint32_t num,
    bool xfer_pending)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];
    nora_i2c_status_t hal_status;

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    if (ctx->initialized == 0u || ctx->powered == 0u) {
        return ARM_DRIVER_ERROR;
    }

    if (address_has_unsupported_flags(addr)) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    if (data == NULL || num == 0u || address_is_addr7(addr) == false) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    begin_master_transfer(ctx, 0u);

    if (xfer_pending) {
        hal_status = nora_i2c_master_write_no_stop(
            ctx->hal_inst,
            (uint8_t)(addr & I2C_ADDR7_MASK),
            data,
            (size_t)num);
        ctx->pending = (hal_status == NORA_I2C_OK) ? 1u : 0u;
    } else {
        hal_status = nora_i2c_write(
            ctx->hal_inst,
            (uint8_t)(addr & I2C_ADDR7_MASK),
            data,
            (size_t)num);
        ctx->pending = 0u;
    }

    return complete_master_transfer(ctx, hal_status, num);
}

static int32_t I2C_MasterReceive(
    uint32_t index,
    uint32_t addr,
    uint8_t *data,
    uint32_t num,
    bool xfer_pending)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];
    nora_i2c_status_t hal_status;

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    if (ctx->initialized == 0u || ctx->powered == 0u) {
        return ARM_DRIVER_ERROR;
    }

    if (address_has_unsupported_flags(addr) || xfer_pending) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    if (data == NULL || num == 0u || address_is_addr7(addr) == false) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    begin_master_transfer(ctx, 1u);

    if (ctx->pending != 0u) {
        hal_status = nora_i2c_master_read_after_restart(
            ctx->hal_inst,
            (uint8_t)(addr & I2C_ADDR7_MASK),
            data,
            (size_t)num);
        ctx->pending = 0u;
    } else {
        hal_status = nora_i2c_read(
            ctx->hal_inst,
            (uint8_t)(addr & I2C_ADDR7_MASK),
            data,
            (size_t)num);
    }

    return complete_master_transfer(ctx, hal_status, num);
}

static int32_t I2C_SlaveTransmit(uint32_t index, const uint8_t *data, uint32_t num)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
    if (ctx->initialized == 0u || ctx->powered == 0u || ctx->own_addr == 0u) {
        return ARM_DRIVER_ERROR;
    }
    if (data == NULL || num == 0u) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    ctx->tx_buf = data;
    ctx->tx_num = num;
    ctx->tx_idx = 0u;
    ctx->data_count = 0u;

    return ARM_DRIVER_OK;
}

static int32_t I2C_SlaveReceive(uint32_t index, uint8_t *data, uint32_t num)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
    if (ctx->initialized == 0u || ctx->powered == 0u || ctx->own_addr == 0u) {
        return ARM_DRIVER_ERROR;
    }
    if (data == NULL || num == 0u) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }

    ctx->rx_buf = data;
    ctx->rx_num = num;
    ctx->rx_idx = 0u;
    ctx->data_count = 0u;

    return ARM_DRIVER_OK;
}

static int32_t I2C_GetDataCount(uint32_t index)
{
    return (int32_t)g_i2c_ctx[index].data_count;
}

static int32_t I2C_Control(uint32_t index, uint32_t control, uint32_t arg)
{
    i2c_cmsis_context_t *ctx = &g_i2c_ctx[index];

    if (ctx->enabled == 0u) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }

    if (ctx->initialized == 0u) {
        return ARM_DRIVER_ERROR;
    }

    switch (control) {
    case ARM_I2C_BUS_SPEED:
    {
        uint32_t new_bus_hz;
        int32_t ret;

        switch (arg) {
        case ARM_I2C_BUS_SPEED_STANDARD:
            new_bus_hz = 100000u;
            break;
        case ARM_I2C_BUS_SPEED_FAST:
            new_bus_hz = 400000u;
            break;
        case ARM_I2C_BUS_SPEED_FAST_PLUS:
            new_bus_hz = 1000000u;
            break;
        case ARM_I2C_BUS_SPEED_HIGH:
        default:
            return ARM_DRIVER_ERROR_UNSUPPORTED;
        }

        if (ctx->status.busy != 0u || ctx->pending != 0u) {
            return ARM_DRIVER_ERROR_BUSY;
        }

        /*
         * When powered, apply the new BRG immediately via the HAL.  When not
         * powered, only the cached value is updated; the BRG is programmed on
         * the next PowerControl(ARM_POWER_FULL).
         */
        if (ctx->powered != 0u) {
            ret = cmsis_return_from_hal(
                nora_i2c_set_bus_speed(ctx->hal_inst,
                                            ctx->fcy_hz,
                                            new_bus_hz));
            if (ret != ARM_DRIVER_OK) {
                return ret;
            }
        }

        ctx->bus_hz = new_bus_hz;
        return ARM_DRIVER_OK;
    }

    case ARM_I2C_OWN_ADDRESS:
    {
        /* Set the 7-bit own address (arg = 0 reverts to master-only). Must be
         * set before PowerControl(FULL), which decides master vs slave init.
         * 10-bit and general-call flags are not supported yet, and no other
         * bits beyond the 7-bit address field are allowed. */
        uint32_t addr_flags = ARM_I2C_ADDRESS_10BIT | ARM_I2C_ADDRESS_GC;

        if (ctx->powered != 0u) {
            return ARM_DRIVER_ERROR_UNSUPPORTED;
        }
        if ((arg & addr_flags) != 0u) {
            return ARM_DRIVER_ERROR_UNSUPPORTED;
        }
        if ((arg & ~(addr_flags | (uint32_t)I2C_ADDR7_MASK)) != 0u) {
            return ARM_DRIVER_ERROR_PARAMETER;
        }
        ctx->own_addr = (uint8_t)(arg & I2C_ADDR7_MASK);
        return ARM_DRIVER_OK;
    }

    case ARM_I2C_BUS_CLEAR:
    case ARM_I2C_ABORT_TRANSFER:
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static ARM_I2C_STATUS I2C_GetStatus(uint32_t index)
{
    return g_i2c_ctx[index].status;
}

static int32_t cmsis_return_from_hal(nora_i2c_status_t hal_status)
{
    switch (hal_status) {
    case NORA_I2C_OK:
        return ARM_DRIVER_OK;
    case NORA_I2C_ERR_INVALID_ARG:
        return ARM_DRIVER_ERROR_PARAMETER;
    case NORA_I2C_ERR_BUSY:
        return ARM_DRIVER_ERROR_BUSY;
    case NORA_I2C_ERR_TIMEOUT:
        return ARM_DRIVER_ERROR_TIMEOUT;
    case NORA_I2C_ERR_UNSUPPORTED:
    case NORA_I2C_ERR_NOT_PRESENT:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    case NORA_I2C_ERR_NACK:
    case NORA_I2C_ERR_BUS:
    case NORA_I2C_ERR_COLLISION:
    case NORA_I2C_ERR_SEQUENCE:
    case NORA_I2C_ERR_NOT_INITIALIZED:
    default:
        return ARM_DRIVER_ERROR;
    }
}

static uint32_t cmsis_event_from_hal(nora_i2c_status_t hal_status)
{
    switch (hal_status) {
    case NORA_I2C_OK:
        return ARM_I2C_EVENT_TRANSFER_DONE;
    case NORA_I2C_ERR_NACK:
        return ARM_I2C_EVENT_ADDRESS_NACK;
    case NORA_I2C_ERR_BUS:
        return ARM_I2C_EVENT_BUS_ERROR;
    case NORA_I2C_ERR_COLLISION:
        return ARM_I2C_EVENT_ARBITRATION_LOST;
    default:
        return 0u;
    }
}

static void clear_status(i2c_cmsis_context_t *ctx)
{
    ARM_I2C_STATUS status = {0};

    ctx->status = status;
}

static void begin_master_transfer(i2c_cmsis_context_t *ctx, uint8_t direction)
{
    clear_status(ctx);
    ctx->status.busy = 1u;
    ctx->status.mode = 1u;
    ctx->status.direction = direction;
    ctx->data_count = 0u;
}

static int32_t complete_master_transfer(
    i2c_cmsis_context_t *ctx,
    nora_i2c_status_t hal_status,
    uint32_t requested_count)
{
    uint32_t event = cmsis_event_from_hal(hal_status);

    ctx->status.busy = 0u;
    ctx->status.mode = 0u;
    ctx->status.direction = 0u;
    ctx->data_count = (hal_status == NORA_I2C_OK) ? requested_count : 0u;

    if (hal_status == NORA_I2C_ERR_BUS) {
        ctx->status.bus_error = 1u;
    } else if (hal_status == NORA_I2C_ERR_COLLISION) {
        ctx->status.arbitration_lost = 1u;
    }

    if (ctx->cb_event != NULL && event != 0u) {
        ctx->cb_event(event);
    }

    return cmsis_return_from_hal(hal_status);
}

static bool address_has_unsupported_flags(uint32_t addr)
{
    return ((addr & (ARM_I2C_ADDRESS_10BIT | ARM_I2C_ADDRESS_GC)) != 0u);
}

static bool address_is_addr7(uint32_t addr)
{
    uint32_t addr_without_flags = addr & ~(ARM_I2C_ADDRESS_10BIT | ARM_I2C_ADDRESS_GC);

    return ((addr_without_flags & ~I2C_ADDR7_MASK) == 0u);
}

#define I2C_DRIVER_WRAPPERS(n)                                                   \
    static int32_t I2C##n##_Initialize(ARM_I2C_SignalEvent_t cb_event)           \
    {                                                                            \
        return I2C_Initialize((n), cb_event);                                     \
    }                                                                            \
    static int32_t I2C##n##_Uninitialize(void)                                   \
    {                                                                            \
        return I2C_Uninitialize((n));                                             \
    }                                                                            \
    static int32_t I2C##n##_PowerControl(ARM_POWER_STATE state)                  \
    {                                                                            \
        return I2C_PowerControl((n), state);                                      \
    }                                                                            \
    static int32_t I2C##n##_MasterTransmit(                                      \
        uint32_t addr,                                                           \
        const uint8_t *data,                                                     \
        uint32_t num,                                                            \
        bool xfer_pending)                                                       \
    {                                                                            \
        return I2C_MasterTransmit((n), addr, data, num, xfer_pending);           \
    }                                                                            \
    static int32_t I2C##n##_MasterReceive(                                       \
        uint32_t addr,                                                           \
        uint8_t *data,                                                           \
        uint32_t num,                                                            \
        bool xfer_pending)                                                       \
    {                                                                            \
        return I2C_MasterReceive((n), addr, data, num, xfer_pending);            \
    }                                                                            \
    static int32_t I2C##n##_SlaveTransmit(const uint8_t *data, uint32_t num)     \
    {                                                                            \
        return I2C_SlaveTransmit((n), data, num);                                \
    }                                                                            \
    static int32_t I2C##n##_SlaveReceive(uint8_t *data, uint32_t num)            \
    {                                                                            \
        return I2C_SlaveReceive((n), data, num);                                 \
    }                                                                            \
    static int32_t I2C##n##_GetDataCount(void)                                   \
    {                                                                            \
        return I2C_GetDataCount((n));                                             \
    }                                                                            \
    static int32_t I2C##n##_Control(uint32_t control, uint32_t arg)              \
    {                                                                            \
        return I2C_Control((n), control, arg);                                    \
    }                                                                            \
    static ARM_I2C_STATUS I2C##n##_GetStatus(void)                               \
    {                                                                            \
        return I2C_GetStatus((n));                                                \
    }

I2C_DRIVER_WRAPPERS(0)
I2C_DRIVER_WRAPPERS(1)
I2C_DRIVER_WRAPPERS(2)
I2C_DRIVER_WRAPPERS(3)

ARM_DRIVER_I2C Driver_I2C0 = {
    I2C_GetVersion,
    I2C_GetCapabilities,
    I2C0_Initialize,
    I2C0_Uninitialize,
    I2C0_PowerControl,
    I2C0_MasterTransmit,
    I2C0_MasterReceive,
    I2C0_SlaveTransmit,
    I2C0_SlaveReceive,
    I2C0_GetDataCount,
    I2C0_Control,
    I2C0_GetStatus,
};

ARM_DRIVER_I2C Driver_I2C1 = {
    I2C_GetVersion,
    I2C_GetCapabilities,
    I2C1_Initialize,
    I2C1_Uninitialize,
    I2C1_PowerControl,
    I2C1_MasterTransmit,
    I2C1_MasterReceive,
    I2C1_SlaveTransmit,
    I2C1_SlaveReceive,
    I2C1_GetDataCount,
    I2C1_Control,
    I2C1_GetStatus,
};

ARM_DRIVER_I2C Driver_I2C2 = {
    I2C_GetVersion,
    I2C_GetCapabilities,
    I2C2_Initialize,
    I2C2_Uninitialize,
    I2C2_PowerControl,
    I2C2_MasterTransmit,
    I2C2_MasterReceive,
    I2C2_SlaveTransmit,
    I2C2_SlaveReceive,
    I2C2_GetDataCount,
    I2C2_Control,
    I2C2_GetStatus,
};

ARM_DRIVER_I2C Driver_I2C3 = {
    I2C_GetVersion,
    I2C_GetCapabilities,
    I2C3_Initialize,
    I2C3_Uninitialize,
    I2C3_PowerControl,
    I2C3_MasterTransmit,
    I2C3_MasterReceive,
    I2C3_SlaveTransmit,
    I2C3_SlaveReceive,
    I2C3_GetDataCount,
    I2C3_Control,
    I2C3_GetStatus,
};
