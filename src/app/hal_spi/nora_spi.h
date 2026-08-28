#ifndef NORA_SPI_H
#define NORA_SPI_H

/*
 * nora_spi.h
 * ---------------
 * Instance-capable, blocking, 8-bit SPI master HAL for dsPIC33AK.
 *
 * Scope (intentionally small for the first iteration):
 *   - master mode
 *   - 8-bit transfer
 *   - blocking polling (no DMA, no interrupts)
 *   - SPI mode 0/1/2/3
 *   - baudrate (BRG) configuration
 *   - SPI instance selection: SPI1 / SPI2 / SPI3 / SPI4
 *
 * Out of scope (NOT handled here):
 *   - DMA / interrupt driven transfer
 *   - frame / audio (TDM/I2S) streaming modes
 *   - 16/24/32-bit transfer
 *   - multi-client bus arbitration / RTOS locking
 *   - Chip-select / WP / RESET / device reset lines  -> belong to the device
 *     driver, not to the SPI bus HAL
 *   - PPS routing and SCK/SDO/SDI/CS GPIO pin setup    -> board-specific init
 *
 * Independence: this HAL is self-contained and does not depend on any audio /
 * TDM SPI driver. If a board uses one SPI instance for another transport, the
 * application or board layer is responsible for avoiding conflicts.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPI peripheral instance. Values match the silicon SPI module numbers. */
typedef enum
{
    NORA_SPI_INST_1 = 1,
    NORA_SPI_INST_2 = 2,
    NORA_SPI_INST_3 = 3,
    NORA_SPI_INST_4 = 4
} nora_spi_instance_t;

/*
 * SPI clock mode. Standard CPOL/CPHA semantics.
 *   MODE_0: CPOL=0, CPHA=0   (idle low,  sample on leading/rising edge)
 *   MODE_1: CPOL=0, CPHA=1
 *   MODE_2: CPOL=1, CPHA=0
 *   MODE_3: CPOL=1, CPHA=1
 * (On dsPIC33AK these map to CKP=CPOL and CKE=!CPHA; the HAL does the mapping.)
 * MODE_0 is a common default for many SPI peripherals.
 */
typedef enum
{
    NORA_SPI_MODE_0 = 0,
    NORA_SPI_MODE_1 = 1,
    NORA_SPI_MODE_2 = 2,
    NORA_SPI_MODE_3 = 3
} nora_spi_mode_t;

/*
 * Result code for the extended (_ex) APIs. The simple blocking APIs hide this
 * behind bool / sentinel returns. A CMSIS-Driver wrapper can map these onto
 * ARM_DRIVER_ERROR_* values - but no CMSIS type appears in this HAL.
 */
typedef enum
{
    NORA_SPI_RESULT_OK = 0,
    NORA_SPI_RESULT_ERROR,
    NORA_SPI_RESULT_INVALID_ARG,
    NORA_SPI_RESULT_NOT_INITIALIZED,
    NORA_SPI_RESULT_UNSUPPORTED,
    NORA_SPI_RESULT_BUSY,
    NORA_SPI_RESULT_TIMEOUT,
    NORA_SPI_RESULT_OVERFLOW
} nora_spi_result_t;

typedef struct
{
    nora_spi_instance_t instance;          /* which SPI module to drive          */
    uint32_t           peripheralClockHz; /* SPI peripheral input clock (Hz)    */
    uint32_t           targetSckHz;       /* desired SCK frequency (Hz)         */
    nora_spi_mode_t     mode;              /* SPI mode 0..3                      */
} nora_spi_config_t;

typedef struct
{
    nora_spi_instance_t instance;          /* bound instance                     */
    uint32_t           actualSckHz;       /* SCK actually achieved from BRG     */
    bool               initialized;       /* true after a successful init       */

    volatile bool          busy;          /* a blocking transfer is in progress */
    volatile bool          overflow;      /* an RX overflow was detected+cleared */
    nora_spi_result_t lastResult;    /* result of the most recent operation */
} nora_spi_handle_t;

/*
 * Snapshot of HAL state, convenient for a CMSIS-Driver GetStatus()-style query
 * or for diagnostics. Read-only copy; does not touch hardware.
 */
typedef struct
{
    bool                   initialized;
    bool                   busy;
    bool                   overflow;
    nora_spi_result_t lastResult;
    uint32_t               actualSckHz;
} nora_spi_status_t;

/*
 * nora_spi_init
 * ------------
 * Configure the selected SPI instance as a blocking 8-bit master and enable it.
 * Does NOT touch PPS/GPIO (board-specific) or CS/WP/RST (device-specific).
 *
 * Returns false (and leaves handle->initialized == false) on:
 *   - NULL handle/config
 *   - instance out of range / not present on this device
 *   - mode out of range (not NORA_SPI_MODE_0..3)
 *   - peripheralClockHz == 0 or targetSckHz == 0
 */
bool nora_spi_init(nora_spi_handle_t *handle, const nora_spi_config_t *config);

/*
 * nora_spi_deinit
 * --------------
 * Disable the SPI instance (clears the module ON bit). Leaves PPS/GPIO intact.
 */
void nora_spi_deinit(nora_spi_handle_t *handle);

/*
 * nora_spi_transfer8
 * -----------------
 * Full-duplex transfer of a single byte (blocking). Caller controls CS.
 * Returns the received byte, or 0xFF if the handle is not initialized.
 */
uint8_t nora_spi_transfer8(nora_spi_handle_t *handle, uint8_t tx);

/*
 * nora_spi_transfer
 * ----------------
 * Full-duplex transfer of len bytes (blocking). tx and/or rx may be NULL:
 *   - tx == NULL : sends 0x00 for each byte
 *   - rx == NULL : discards received bytes
 * Returns false on a NULL/uninitialized handle (len == 0 is a no-op success).
 */
bool nora_spi_transfer(nora_spi_handle_t *handle,
                      const uint8_t *tx,
                      uint8_t *rx,
                      size_t len);

/*
 * nora_spi_write
 * -------------
 * Transmit len bytes, discarding the received data (blocking).
 * Returns false if tx is NULL with a non-zero len (len == 0 is a no-op success).
 */
bool nora_spi_write(nora_spi_handle_t *handle, const uint8_t *tx, size_t len);

/*
 * nora_spi_read
 * ------------
 * Receive len bytes while clocking out the given dummy byte (blocking).
 */
bool nora_spi_read(nora_spi_handle_t *handle, uint8_t *rx, size_t len, uint8_t dummy);

/*
 * nora_spi_wait_done
 * -----------------
 * Block until the shift register has fully emptied (SPIBUSY clear) so it is
 * safe to deassert CS. Also drains a pending RX overflow. The caller (device
 * driver) invokes this before raising CS.
 */
void nora_spi_wait_done(nora_spi_handle_t *handle);


/*
 * API groups
 * ----------
 * Simple APIs (above):
 *   nora_spi_transfer8 / transfer / write / read / wait_done
 *   - existing simple blocking API (bool / sentinel returns, wait forever)
 *   - suitable for small device drivers (flash, sensors, displays, ...)
 *
 * Extended APIs (below, "_ex"):
 *   - return nora_spi_result_t for explicit error handling
 *   - take a polling timeoutCount (0 == wait forever, matching the simple API)
 *   - update handle->busy / overflow / lastResult
 *   - intended for robust drivers and a future CMSIS-Driver wrapper
 *
 * The simple APIs are thin wrappers over the _ex APIs (timeoutCount == 0), so
 * both paths share one implementation.
 *
 * Policy: this HAL is CMSIS-agnostic. Do NOT include CMSIS-Driver headers from
 * hal_spi, and do NOT use ARM_DRIVER_* / ARM_SPI_* symbols here. A CMSIS wrapper
 * lives in a separate layer and maps nora_spi_result_t / _status_t onto the
 * CMSIS types.
 */

/*
 * timeoutCount semantics (all _ex APIs): the value is a bounded count of status
 * polling iterations. timeoutCount == 0 means wait forever (no timeout).
 */

/* Full-duplex single byte. rx may be NULL to discard the received byte. */
nora_spi_result_t nora_spi_transfer8_ex(nora_spi_handle_t *handle,
                                                  uint8_t tx,
                                                  uint8_t *rx,
                                                  uint32_t timeoutCount);

/* Full-duplex buffer. tx and/or rx may be NULL (tx NULL -> sends 0x00). */
nora_spi_result_t nora_spi_transfer_ex(nora_spi_handle_t *handle,
                                                 const uint8_t *tx,
                                                 uint8_t *rx,
                                                 size_t len,
                                                 uint32_t timeoutCount);

/* Transmit only. len > 0 with tx == NULL is rejected as INVALID_ARG. */
nora_spi_result_t nora_spi_write_ex(nora_spi_handle_t *handle,
                                              const uint8_t *tx,
                                              size_t len,
                                              uint32_t timeoutCount);

/* Receive only (clock out dummy). len > 0 with rx == NULL is INVALID_ARG. */
nora_spi_result_t nora_spi_read_ex(nora_spi_handle_t *handle,
                                             uint8_t *rx,
                                             size_t len,
                                             uint8_t dummy,
                                             uint32_t timeoutCount);

/* Wait for the shifter to drain (safe-to-deassert-CS); also clears overflow. */
nora_spi_result_t nora_spi_wait_done_ex(nora_spi_handle_t *handle,
                                                  uint32_t timeoutCount);

/* Read-only state snapshot (no hardware access). */
nora_spi_status_t nora_spi_get_status(const nora_spi_handle_t *handle);

/* Most recent result; INVALID_ARG if handle is NULL. */
nora_spi_result_t nora_spi_get_last_result(const nora_spi_handle_t *handle);

/* Clear the sticky overflow flag and reset lastResult to OK. */
void nora_spi_clear_error(nora_spi_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* NORA_SPI_H */
