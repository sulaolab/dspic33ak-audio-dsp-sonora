/*
 * nora_spi_dspic33ak.c
 * ---------------
 * Instance-capable, blocking, 8-bit SPI master HAL for dsPIC33AK.
 * See nora_spi.h for the public contract and scope.
 *
 * Implementation notes:
 *   - One device-neutral driver body drives any available SPIn through a table
 *     of plain 32-bit register pointers (s_spi_regs[]). Only the s_spi_regs[]
 *     table is device-specific; the public API functions are compiled for every
 *     target. On a device with no table entry for the requested instance,
 *     nora_spi_init() simply returns false.
 *   - Register-level configuration is a plain SPI master: 8-bit, IGNROV/IGNTUR,
 *     clock mode via CKP/CKE.
 *   - PPS routing and SCK/SDO/SDI/CS GPIO are NOT done here. Pin routing and the
 *     CS / WP / RESET control lines belong to the board / device layer; this HAL
 *     owns only the SPI module registers.
 */

//===========================================================
// INCLUDES
//===========================================================
#include "nora_spi.h"

#include <xc.h>

#include "nora_spi_dspic33ak_reg.h"


//===========================================================
// Definition
//===========================================================

/* Per-instance register pointers (uniform 32-bit SFRs). */
typedef struct
{
    volatile uint32_t *con1;
    volatile uint32_t *stat;
    volatile uint32_t *brg;
    volatile uint32_t *buf;
} dspic33ak_spi_regs_t;

#define DSPIC33AK_SPI_ARRAY_LEN(a)   (sizeof(a) / sizeof((a)[0]))


//===========================================================
// Function Prototype
//===========================================================

static uint32_t                     dspic33ak_spi_mode_bits(nora_spi_mode_t mode);
static const dspic33ak_spi_regs_t  *dspic33ak_spi_regs_for(nora_spi_instance_t inst);
static nora_spi_result_t       dspic33ak_spi_set_result(nora_spi_handle_t *handle,
                                                             nora_spi_result_t result);
static bool                         dspic33ak_spi_wait_flag_set(volatile uint32_t *reg,
                                                               uint32_t mask,
                                                               uint32_t timeoutCount);
static bool                         dspic33ak_spi_wait_flag_clear(volatile uint32_t *reg,
                                                                 uint32_t mask,
                                                                 uint32_t timeoutCount);
static void                         dspic33ak_spi_drain_rx_if_ready(const dspic33ak_spi_regs_t *r);
static nora_spi_result_t       dspic33ak_spi_xfer_one(nora_spi_handle_t *handle,
                                                           const dspic33ak_spi_regs_t *r,
                                                           uint8_t tx,
                                                           uint8_t *rx,
                                                           uint32_t timeoutCount);
static nora_spi_result_t       dspic33ak_spi_check_and_clear_overflow(
                                                           nora_spi_handle_t *handle,
                                                           const dspic33ak_spi_regs_t *r);


//===========================================================
// Variables
//===========================================================

/*
 * Register table, indexed by nora_spi_instance_t (1..N); index 0 is an unused
 * placeholder. The table adapts to the target automatically: each row is
 * emitted only when the device header (DFP <xc.h>) defines that instance's
 * SFRs. The Microchip dsPIC33A headers self-define every SFR they declare
 * (e.g. "#define SPI4CON1 SPI4CON1"), so "#if defined(SPInCON1)" is a reliable
 * presence test - no device-name #if is needed here.
 *
 * A row whose SFRs are absent expands to a NULL placeholder, keeping the array
 * index equal to the instance number; dspic33ak_spi_regs_for() rejects any instance
 * whose con1 is NULL or beyond the table.
 */
#define DSPIC33AK_SPI_REG_ROW(n)   { &SPI##n##CON1, &SPI##n##STAT, &SPI##n##BRG, &SPI##n##BUF }
#define DSPIC33AK_SPI_REG_NONE     { 0, 0, 0, 0 }

static const dspic33ak_spi_regs_t s_spi_regs[] =
{
    DSPIC33AK_SPI_REG_NONE,        /* [0] unused */
#if defined(SPI1CON1)
    DSPIC33AK_SPI_REG_ROW(1),      /* [1] SPI1 */
#else
    DSPIC33AK_SPI_REG_NONE,
#endif
#if defined(SPI2CON1)
    DSPIC33AK_SPI_REG_ROW(2),      /* [2] SPI2 */
#else
    DSPIC33AK_SPI_REG_NONE,
#endif
#if defined(SPI3CON1)
    DSPIC33AK_SPI_REG_ROW(3),      /* [3] SPI3 */
#else
    DSPIC33AK_SPI_REG_NONE,
#endif
#if defined(SPI4CON1)
    DSPIC33AK_SPI_REG_ROW(4),      /* [4] SPI4 */
#endif
};


//===========================================================
// Global Function
//===========================================================

bool nora_spi_init(nora_spi_handle_t *handle, const nora_spi_config_t *config)
{
    if (handle == 0)
    {
        return false;   /* cannot record a result without a handle */
    }

    handle->initialized = false;
    handle->actualSckHz = 0u;
    handle->busy        = false;
    handle->overflow    = false;
    handle->lastResult  = NORA_SPI_RESULT_OK;

    if (config == 0)
    {
        (void)dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_INVALID_ARG);
        return false;
    }

    const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(config->instance);
    if (r == 0)
    {
        /* instance not present on this device */
        (void)dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_UNSUPPORTED);
        return false;
    }

    if (config->mode > NORA_SPI_MODE_3)
    {
        (void)dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_INVALID_ARG);
        return false;
    }

    if ((config->peripheralClockHz == 0u) || (config->targetSckHz == 0u))
    {
        (void)dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_INVALID_ARG);
        return false;
    }

    /* BRG = Fpb/(2*Fsck) - 1 ; clamp to the 13-bit field. */
    uint32_t divider = config->peripheralClockHz / (2u * config->targetSckHz);
    if (divider < 1u)
    {
        divider = 1u;   /* requested SCK above max; use fastest (BRG=0) */
    }
    uint32_t brg = divider - 1u;
    if (brg > DSPIC33AK_SPI_BRG_MAX)
    {
        brg = DSPIC33AK_SPI_BRG_MAX;
    }

    /* Disable + clear before reconfiguring. */
    *r->con1 = 0u;
    *r->stat = 0u;

    /*
     * Compose CON1: master, 8-bit, plain SPI (no audio/frame), ignore
     * overflow/underrun. MODE16/MODE32 stay 0 for 8-bit. AUDEN/FRMEN/MCLKEN
     * stay 0. DISSCK/DISSDO/DISSDI stay 0 (all lines active).
     */
    uint32_t con1 = 0u;
    con1 |= DSPIC33AK_SPI_CON1_MSTEN;
    con1 |= DSPIC33AK_SPI_CON1_IGNROV;
    con1 |= DSPIC33AK_SPI_CON1_IGNTUR;
    con1 |= dspic33ak_spi_mode_bits(config->mode);

    *r->brg  = brg;
    *r->con1 = con1;

    /* Flush any stale RX byte and clear a possible overflow. */
    (void)(*r->buf);
    dspic33ak_spi_reg_clear(r->stat, DSPIC33AK_SPI_STAT_SPIROV);

    /* Enable the module. */
    dspic33ak_spi_reg_set(r->con1, DSPIC33AK_SPI_CON1_ON);

    handle->instance    = config->instance;
    handle->actualSckHz = config->peripheralClockHz / (2u * (brg + 1u));
    handle->initialized = true;
    (void)dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_OK);
    return true;
}

void nora_spi_deinit(nora_spi_handle_t *handle)
{
    if (handle == 0)
    {
        return;
    }

    /* Disable the module only if it was actually brought up. */
    if (handle->initialized)
    {
        const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
        if (r != 0)
        {
            dspic33ak_spi_reg_clear(r->con1, DSPIC33AK_SPI_CON1_ON);
        }
    }

    /* Return the handle to a clean, safe state (instance is kept). */
    handle->initialized = false;
    handle->busy        = false;
    handle->overflow    = false;
    handle->actualSckHz = 0u;
    handle->lastResult  = NORA_SPI_RESULT_OK;
}

/* ---- Simple blocking API (thin wrappers over the _ex API, wait forever) ---- */

uint8_t nora_spi_transfer8(nora_spi_handle_t *handle, uint8_t tx)
{
    uint8_t rx = 0xFFu;   /* matches the previous "uninitialized -> 0xFF" return */

    (void)nora_spi_transfer8_ex(handle, tx, &rx, 0u);
    return rx;
}

bool nora_spi_transfer(nora_spi_handle_t *handle,
                            const uint8_t *tx,
                            uint8_t *rx,
                            size_t len)
{
    return (nora_spi_transfer_ex(handle, tx, rx, len, 0u) == NORA_SPI_RESULT_OK);
}

bool nora_spi_write(nora_spi_handle_t *handle, const uint8_t *tx, size_t len)
{
    return (nora_spi_write_ex(handle, tx, len, 0u) == NORA_SPI_RESULT_OK);
}

bool nora_spi_read(nora_spi_handle_t *handle, uint8_t *rx, size_t len, uint8_t dummy)
{
    return (nora_spi_read_ex(handle, rx, len, dummy, 0u) == NORA_SPI_RESULT_OK);
}

void nora_spi_wait_done(nora_spi_handle_t *handle)
{
    (void)nora_spi_wait_done_ex(handle, 0u);
}


/* ---- Extended API (explicit result + polling timeout; 0 == wait forever) ---- */

nora_spi_result_t nora_spi_transfer8_ex(nora_spi_handle_t *handle,
                                                  uint8_t tx,
                                                  uint8_t *rx,
                                                  uint32_t timeoutCount)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    if (!handle->initialized)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }
    if (handle->busy)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_BUSY);
    }

    const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
    if (r == 0)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }

    handle->busy = true;
    nora_spi_result_t res = dspic33ak_spi_xfer_one(handle, r, tx, rx, timeoutCount);
    handle->busy = false;
    return dspic33ak_spi_set_result(handle, res);
}

nora_spi_result_t nora_spi_transfer_ex(nora_spi_handle_t *handle,
                                                 const uint8_t *tx,
                                                 uint8_t *rx,
                                                 size_t len,
                                                 uint32_t timeoutCount)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    if (!handle->initialized)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }
    if (handle->busy)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_BUSY);
    }

    const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
    if (r == 0)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }

    handle->busy = true;

    nora_spi_result_t res = NORA_SPI_RESULT_OK;
    for (size_t i = 0u; i < len; i++)
    {
        uint8_t out = (tx != 0) ? tx[i] : 0x00u;
        uint8_t in;

        res = dspic33ak_spi_xfer_one(handle, r, out, &in, timeoutCount);
        if (res != NORA_SPI_RESULT_OK)
        {
            break;
        }
        if (rx != 0)
        {
            rx[i] = in;
        }
    }

    handle->busy = false;
    return dspic33ak_spi_set_result(handle, res);
}

nora_spi_result_t nora_spi_write_ex(nora_spi_handle_t *handle,
                                              const uint8_t *tx,
                                              size_t len,
                                              uint32_t timeoutCount)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    /* "write" requires a source buffer for a non-zero length. */
    if ((len > 0u) && (tx == 0))
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_INVALID_ARG);
    }
    /* Transmit, discarding RX (busy/result are managed by transfer_ex). */
    return nora_spi_transfer_ex(handle, tx, 0, len, timeoutCount);
}

nora_spi_result_t nora_spi_read_ex(nora_spi_handle_t *handle,
                                             uint8_t *rx,
                                             size_t len,
                                             uint8_t dummy,
                                             uint32_t timeoutCount)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    /* "read" requires a destination buffer for a non-zero length. */
    if ((len > 0u) && (rx == 0))
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_INVALID_ARG);
    }
    if (!handle->initialized)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }
    if (handle->busy)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_BUSY);
    }

    const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
    if (r == 0)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }

    handle->busy = true;

    nora_spi_result_t res = NORA_SPI_RESULT_OK;
    for (size_t i = 0u; i < len; i++)
    {
        uint8_t in;

        res = dspic33ak_spi_xfer_one(handle, r, dummy, &in, timeoutCount);
        if (res != NORA_SPI_RESULT_OK)
        {
            break;
        }
        if (rx != 0)
        {
            rx[i] = in;
        }
    }

    handle->busy = false;
    return dspic33ak_spi_set_result(handle, res);
}

nora_spi_result_t nora_spi_wait_done_ex(nora_spi_handle_t *handle,
                                                  uint32_t timeoutCount)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    if (!handle->initialized)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }

    const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
    if (r == 0)
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_NOT_INITIALIZED);
    }

    /* Wait for TX FIFO drained and shifter idle, then clear a stale overflow. */
    if (!dspic33ak_spi_wait_flag_clear(r->stat, DSPIC33AK_SPI_STAT_SPITBF, timeoutCount))
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_TIMEOUT);
    }
    if (!dspic33ak_spi_wait_flag_clear(r->stat, DSPIC33AK_SPI_STAT_SPIBUSY, timeoutCount))
    {
        return dspic33ak_spi_set_result(handle, NORA_SPI_RESULT_TIMEOUT);
    }

    nora_spi_result_t ovf = dspic33ak_spi_check_and_clear_overflow(handle, r);
    return dspic33ak_spi_set_result(handle, ovf);
}


/* ---- State queries ---- */

nora_spi_status_t nora_spi_get_status(const nora_spi_handle_t *handle)
{
    nora_spi_status_t st;

    st.initialized = false;
    st.busy        = false;
    st.overflow    = false;
    st.lastResult  = NORA_SPI_RESULT_NOT_INITIALIZED;
    st.actualSckHz = 0u;

    if (handle != 0)
    {
        st.initialized = handle->initialized;
        st.busy        = handle->busy;
        st.overflow    = handle->overflow;
        st.lastResult  = handle->lastResult;
        st.actualSckHz = handle->actualSckHz;
    }

    return st;
}

nora_spi_result_t nora_spi_get_last_result(const nora_spi_handle_t *handle)
{
    if (handle == 0)
    {
        return NORA_SPI_RESULT_INVALID_ARG;
    }
    return handle->lastResult;
}

void nora_spi_clear_error(nora_spi_handle_t *handle)
{
    if (handle == 0)
    {
        return;
    }

    handle->overflow   = false;
    handle->lastResult = NORA_SPI_RESULT_OK;

    /*
     * Best-effort: also clear a sticky hardware RX overflow, if reachable.
     * Use the same drain-then-clear sequence as the transfer path
     * (dspic33ak_spi_check_and_clear_overflow): a dummy BUF read before clearing
     * SPIROV, so no stale byte is left behind.
     */
    if (handle->initialized)
    {
        const dspic33ak_spi_regs_t *r = dspic33ak_spi_regs_for(handle->instance);
        if (r != 0)
        {
            if (dspic33ak_spi_reg_is_set(r->stat, DSPIC33AK_SPI_STAT_SPIROV))
            {
                volatile uint32_t dummy = *r->buf;
                (void)dummy;
                dspic33ak_spi_reg_clear(r->stat, DSPIC33AK_SPI_STAT_SPIROV);
            }
        }
    }
}


//===========================================================
// Local Function
//===========================================================

/* Map an SPI mode to the CKP/CKE bits (CKP=CPOL, CKE=!CPHA). */
static uint32_t dspic33ak_spi_mode_bits(nora_spi_mode_t mode)
{
    uint32_t bits = 0u;

    /* CPOL=1 for modes 2 and 3 */
    if ((mode == NORA_SPI_MODE_2) || (mode == NORA_SPI_MODE_3))
    {
        bits |= DSPIC33AK_SPI_CON1_CKP;
    }
    /* CKE = !CPHA -> set for CPHA=0, i.e. modes 0 and 2 */
    if ((mode == NORA_SPI_MODE_0) || (mode == NORA_SPI_MODE_2))
    {
        bits |= DSPIC33AK_SPI_CON1_CKE;
    }
    return bits;
}

/*
 * Resolve an instance to its register set. Returns NULL when the instance is
 * outside the table or has no entry (NULL con1) on this device.
 */
static const dspic33ak_spi_regs_t *dspic33ak_spi_regs_for(nora_spi_instance_t inst)
{
    unsigned idx = (unsigned)inst;

    if (idx >= DSPIC33AK_SPI_ARRAY_LEN(s_spi_regs))
    {
        return 0;
    }
    if (s_spi_regs[idx].con1 == 0)
    {
        return 0;
    }
    return &s_spi_regs[idx];
}

/* Store the result in the handle (if any) and return it for convenient chaining. */
static nora_spi_result_t dspic33ak_spi_set_result(nora_spi_handle_t *handle,
                                                       nora_spi_result_t result)
{
    if (handle != 0)
    {
        handle->lastResult = result;
    }
    return result;
}

/*
 * Poll until (reg & mask) is non-zero. timeoutCount == 0 waits forever (matching
 * the simple blocking API); otherwise it bounds the number of poll iterations.
 */
static bool dspic33ak_spi_wait_flag_set(volatile uint32_t *reg,
                                        uint32_t mask,
                                        uint32_t timeoutCount)
{
    if (timeoutCount == 0u)
    {
        while (!dspic33ak_spi_reg_is_set(reg, mask)) { }
        return true;
    }
    for (uint32_t i = 0u; i < timeoutCount; i++)
    {
        if (dspic33ak_spi_reg_is_set(reg, mask))
        {
            return true;
        }
    }
    return dspic33ak_spi_reg_is_set(reg, mask);
}

/* Poll until (reg & mask) is zero. timeoutCount == 0 waits forever. */
static bool dspic33ak_spi_wait_flag_clear(volatile uint32_t *reg,
                                          uint32_t mask,
                                          uint32_t timeoutCount)
{
    if (timeoutCount == 0u)
    {
        while (dspic33ak_spi_reg_is_set(reg, mask)) { }
        return true;
    }
    for (uint32_t i = 0u; i < timeoutCount; i++)
    {
        if (!dspic33ak_spi_reg_is_set(reg, mask))
        {
            return true;
        }
    }
    return !dspic33ak_spi_reg_is_set(reg, mask);
}

/* If an RX byte is already latched, read and discard it (no overflow logic). */
static void dspic33ak_spi_drain_rx_if_ready(const dspic33ak_spi_regs_t *r)
{
    if (r == 0)
    {
        return;
    }
    if (dspic33ak_spi_reg_is_set(r->stat, DSPIC33AK_SPI_STAT_SPIRBF))
    {
        volatile uint32_t dummy = *r->buf;
        (void)dummy;
    }
}

/*
 * One full-duplex byte with timeout. rx may be NULL to discard the received
 * byte. Returns TIMEOUT if the TX-ready / RX-ready wait expires.
 *
 * If the RX-ready wait times out after SPIxBUF was written, try to drain a
 * late RX byte and clear a pending overflow so the next transaction does not
 * read stale data. The return value stays TIMEOUT; an overflow seen during
 * cleanup is only recorded in handle->overflow (TIMEOUT takes precedence and
 * a late byte arriving after the drain cannot be fully prevented here).
 */
static nora_spi_result_t dspic33ak_spi_xfer_one(nora_spi_handle_t *handle,
                                                     const dspic33ak_spi_regs_t *r,
                                                     uint8_t tx,
                                                     uint8_t *rx,
                                                     uint32_t timeoutCount)
{
    /* Wait until TX buffer has space, load (starts transfer), wait RX, read. */
    if (!dspic33ak_spi_wait_flag_clear(r->stat, DSPIC33AK_SPI_STAT_SPITBF, timeoutCount))
    {
        return NORA_SPI_RESULT_TIMEOUT;
    }
    *r->buf = (uint32_t)tx;
    if (!dspic33ak_spi_wait_flag_set(r->stat, DSPIC33AK_SPI_STAT_SPIRBF, timeoutCount))
    {
        /* Byte was launched but RX did not complete in time: clean up. */
        dspic33ak_spi_drain_rx_if_ready(r);
        (void)dspic33ak_spi_check_and_clear_overflow(handle, r);
        return NORA_SPI_RESULT_TIMEOUT;
    }

    uint8_t in = (uint8_t)(*r->buf);
    if (rx != 0)
    {
        *rx = in;
    }
    return NORA_SPI_RESULT_OK;
}

/*
 * If an RX overflow is pending, drain the buffer and clear SPIROV (same action
 * as the original wait_done), recording it in the handle. Returns OVERFLOW when
 * one was found, OK otherwise. The normal (no-overflow) path is unchanged.
 */
static nora_spi_result_t dspic33ak_spi_check_and_clear_overflow(nora_spi_handle_t *handle,
                                                                     const dspic33ak_spi_regs_t *r)
{
    if (dspic33ak_spi_reg_is_set(r->stat, DSPIC33AK_SPI_STAT_SPIROV))
    {
        volatile uint32_t dummy = *r->buf;
        (void)dummy;
        dspic33ak_spi_reg_clear(r->stat, DSPIC33AK_SPI_STAT_SPIROV);

        if (handle != 0)
        {
            handle->overflow = true;
        }
        return NORA_SPI_RESULT_OVERFLOW;
    }
    return NORA_SPI_RESULT_OK;
}
