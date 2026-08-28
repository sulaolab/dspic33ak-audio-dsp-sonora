#ifndef NORA_CCP_INPUT_CAPTURE_H
#define NORA_CCP_INPUT_CAPTURE_H

// NORA CCP Input Capture HAL
//
// Provides a thin hardware abstraction for SCCP/MCCP Input Capture mode.
// The HAL configures a CCP instance, captures raw edge timestamps, and exposes
// them through polling or an ISR callback.
//
// The HAL does not calculate period, frequency, duty ratio, or sample-rate
// ratio. Those calculations belong to the application. PPS routing and
// peripheral clock routing are also owned by the integrator. Do not use
// polling and callback delivery concurrently on the same instance.
//
// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#include <stdbool.h>
#include <stdint.h>

/** CCP instances in the public API namespace. */
typedef enum
{
    NORA_CCP1 = 0,
    NORA_CCP2,
    NORA_CCP3,
    NORA_CCP4,
    NORA_CCP5,
    NORA_CCP6,
    NORA_CCP7,
    NORA_CCP8,
    NORA_CCP9,
    NORA_CCP_INST_COUNT
} nora_ccp_inst_t;

/** API return status. */
typedef enum
{
    NORA_CCP_OK = 0,
    NORA_CCP_ERR_INSTANCE,
    NORA_CCP_ERR_PARAM
} nora_ccp_status_t;

/** CCPxCON1.MOD encodings supported in Input Capture mode. */
typedef enum
{
    NORA_CCP_EDGE_EVERY_RISING       = 0x1,
    NORA_CCP_EDGE_EVERY_FALLING      = 0x2,
    NORA_CCP_EDGE_EVERY_EDGE         = 0x3,
    NORA_CCP_EDGE_EVERY_4TH_RISING   = 0x4,
    NORA_CCP_EDGE_EVERY_16TH_RISING  = 0x5
} nora_ccp_edge_t;

/** CCPxCON2.ICS input-source values supported by the active backend. */
typedef enum
{
    NORA_CCP_SRC_PIN  = 0x0,
    NORA_CCP_SRC_CMP1 = 0x1,
    NORA_CCP_SRC_CMP2 = 0x2,
    NORA_CCP_SRC_CMP3 = 0x3,
    NORA_CCP_SRC_CLC1 = 0x4,
    NORA_CCP_SRC_CLC2 = 0x5,
    NORA_CCP_SRC_CLC3 = 0x6,
    NORA_CCP_SRC_CLC4 = 0x7
} nora_ccp_src_t;

/**
 * Supported CCPxCON1.CLKSEL time-base clock encodings.
 *
 * The active backend maps CLKSEL = 0x1 to Clock Generator 13. The Input
 * Capture prose's reference to CLKGEN12 is a data-sheet inconsistency.
 */
typedef enum
{
    NORA_CCP_CLK_PERIPHERAL = 0x0,
    NORA_CCP_CLK_CLKGEN13   = 0x1,
    NORA_CCP_CLK_EXT_TCKI   = 0x7
} nora_ccp_clk_t;

/** CCPxCON1.TMRPS time-base prescaler encodings. */
typedef enum
{
    NORA_CCP_PS_1  = 0x0,
    NORA_CCP_PS_4  = 0x1,
    NORA_CCP_PS_16 = 0x2,
    NORA_CCP_PS_64 = 0x3
} nora_ccp_prescaler_t;

/**
 * Publicly supported CCPxCON1.OPS interrupt/capture-event postscalers.
 *
 * The encoded value is N-1: zero interrupts after every capture event, one
 * after every second event, and so on.
 */
typedef enum
{
    NORA_CCP_IRQ_EVERY_EVENT = 0x0,
    NORA_CCP_IRQ_EVERY_2ND   = 0x1,
    NORA_CCP_IRQ_EVERY_4TH   = 0x3,
    NORA_CCP_IRQ_EVERY_8TH   = 0x7,
    NORA_CCP_IRQ_EVERY_16TH  = 0xF
} nora_ccp_irq_ops_t;

/** Input Capture configuration supplied to nora_ccp_icap_configure(). */
typedef struct
{
    nora_ccp_src_t       source;
    nora_ccp_edge_t      edge;
    nora_ccp_clk_t       clock;
    nora_ccp_prescaler_t prescaler;
    bool                      use_32bit;
    nora_ccp_irq_ops_t   irq_ops;
    bool                      irq_enable;
    uint8_t                   irq_priority;
    uint32_t                  timebase_src_hz;
} nora_ccp_icap_config_t;

/**
 * Capture callback type.
 *
 * Called in interrupt context once for each timestamp drained from the FIFO.
 * Keep callbacks short and non-blocking. The callback and polling paths must
 * not consume the same instance concurrently.
 */
typedef void (*nora_ccp_capture_cb_t)(nora_ccp_inst_t inst,
                                           uint32_t timestamp,
                                           void *user);

/**
 * Configure an instance for Input Capture.
 *
 * @param inst CCP instance.
 * @param cfg Non-NULL configuration. timebase_src_hz is the caller-supplied
 *        source frequency before prescaling; it is not measured by the HAL.
 * @return NORA_CCP_OK, NORA_CCP_ERR_INSTANCE, or
 *         NORA_CCP_ERR_PARAM. Invalid parameters cause no register writes.
 *
 * Call before nora_ccp_icap_start(). The function disables the instance's
 * CPU interrupt, stops the module, drains stale captures, clears overflow and
 * interrupt state, then programs the module. It is not ISR-safe and must not
 * race another API call for the same instance.
 */
nora_ccp_status_t nora_ccp_icap_configure(
    nora_ccp_inst_t inst,
    const nora_ccp_icap_config_t *cfg);

/**
 * Register or clear an instance callback.
 *
 * @param inst CCP instance.
 * @param cb Callback, or NULL to discard captures handled by the ISR.
 * @param user Opaque value passed to cb.
 * @return NORA_CCP_OK or NORA_CCP_ERR_INSTANCE.
 *
 * Stop the instance, or protect this call with an application-owned critical
 * section, before changing a callback that an ISR may use. This API does not
 * provide atomic callback/user-pair replacement.
 */
nora_ccp_status_t nora_ccp_icap_set_callback(
    nora_ccp_inst_t inst,
    nora_ccp_capture_cb_t cb,
    void *user);

/**
 * Drain stale captures, clear status, and start an already configured module.
 *
 * @return NORA_CCP_OK or NORA_CCP_ERR_INSTANCE.
 * Not ISR-safe; serialise with other calls for the same instance.
 */
nora_ccp_status_t nora_ccp_icap_start(nora_ccp_inst_t inst);

/**
 * Stop capture without changing the saved configuration or callback.
 *
 * @return NORA_CCP_OK or NORA_CCP_ERR_INSTANCE.
 * Not ISR-safe; the CPU interrupt enable state is left unchanged.
 */
nora_ccp_status_t nora_ccp_icap_stop(nora_ccp_inst_t inst);

/**
 * Poll and remove one raw timestamp from the capture FIFO.
 *
 * @param inst CCP instance.
 * @param timestamp Non-NULL destination.
 * @return true when one value was read; false for no data or invalid input.
 *
 * Do not call for an instance whose ISR/callback path is active. A successful
 * read pops one FIFO entry. This function is not safe against a concurrent ISR
 * draining the same FIFO.
 */
bool nora_ccp_icap_read(nora_ccp_inst_t inst,
                             uint32_t *timestamp);

/**
 * Query the input-capture overflow flag.
 *
 * @param inst CCP instance.
 * @param clear Clear the flag after observing it.
 * @return true if captured data was lost because the FIFO overflowed; false if
 *         no overflow occurred or the instance is invalid.
 *
 * Clearing has a hardware side effect and must be serialised with other code
 * that manages the overflow flag.
 */
bool nora_ccp_icap_overflow(nora_ccp_inst_t inst, bool clear);

/**
 * Return the configured effective time-base tick frequency.
 *
 * The HAL does not read or verify the actual CLKGEN frequency. The result is
 * timebase_src_hz supplied at configure time divided by the selected
 * prescaler. Returns zero before configuration or for an invalid instance.
 */
uint32_t nora_ccp_icap_timebase_hz(nora_ccp_inst_t inst);

/**
 * Service one CCP Input Capture interrupt.
 *
 * Call only from the matching application-owned ISR, unless HAL vector
 * generation is enabled. It drains all FIFO entries, invokes the callback in
 * ISR context for each entry, and clears the CPU interrupt flag. With a NULL
 * callback, captured timestamps are deliberately discarded.
 */
void nora_ccp_icap_isr(nora_ccp_inst_t inst);

/**
 * Clear this instance's CPU interrupt flag.
 *
 * nora_ccp_icap_isr() already does this at the end of its drain, so a caller that
 * uses the HAL's ISR never needs it. It exists for the other legitimate shape: an
 * application that owns the vector because it wants its own per-sample handling
 * (no callback indirection, no bounds check per entry) and therefore drains the
 * FIFO itself with nora_ccp_icap_read(). Without this call such an ISR had to
 * reach for the IFSx SFR directly, which is the whole reason application files
 * ended up including <xc.h> and the backend register header.
 *
 * No-op for an invalid or absent instance.
 */
void nora_ccp_icap_irq_clear(nora_ccp_inst_t inst);

#endif /* NORA_CCP_INPUT_CAPTURE_H */
