/**
 * @file    nora_canfd.h
 * @brief   dsPIC33AK CAN FD HAL - public shared API (instance/status/mode).
 *
 * This header carries only what the role layer(s) share. The transmit/receive
 * role API lives in nora_canfd_node.h, which includes this file.
 *
 * Design notes
 * ------------
 *  - NO MCU/DFP bitfield types and NO ARM CMSIS (ARM_CAN_*) types are ever
 *    exposed here. The HAL is kept wrapper-friendly so a CMSIS-Driver CAN
 *    wrapper (Driver_CAN.h) can sit on top later, like the I2C/UART HALs.
 *  - Instances are addressed by an opaque enum; the device map decides which
 *    are physically present on the part.
 */
#ifndef NORA_CANFD_H
#define NORA_CANFD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/** CAN FD peripheral instances. INST_COUNT sizes all per-instance arrays. */
typedef enum {
    NORA_CANFD_INST_1 = 0,   /**< C1 */
    NORA_CANFD_INST_2,       /**< C2 */
    NORA_CANFD_INST_COUNT
} nora_canfd_instance_t;

/** Status / error codes returned across the HAL. */
typedef enum {
    NORA_CANFD_OK = 0,
    NORA_CANFD_ERR_INVALID_ARG,
    NORA_CANFD_ERR_NOT_PRESENT,
    NORA_CANFD_ERR_NOT_INITIALIZED,
    NORA_CANFD_ERR_BUSY,
    NORA_CANFD_ERR_TIMEOUT,
    NORA_CANFD_ERR_BUS,
    NORA_CANFD_ERR_NO_MESSAGE,
    NORA_CANFD_ERR_UNSUPPORTED,
    NORA_CANFD_ERR_SEQUENCE
} nora_canfd_status_t;

/**
 * Operating mode (maps to C1CON.REQOP/OPMOD codes).
 * Internal Loopback needs no transceiver and no bus partner - used for the
 * phase-1 self-test.
 */
typedef enum {
    NORA_CANFD_MODE_NONE = 0,    /**< not initialized */
    NORA_CANFD_MODE_NORMAL_FD,   /**< REQOP 0b000 - CAN FD */
    NORA_CANFD_MODE_NORMAL_CLASSIC, /**< REQOP 0b110 - CAN 2.0 */
    NORA_CANFD_MODE_INTERNAL_LOOPBACK, /**< REQOP 0b010 - TX looped to RX internally, pins idle */
    NORA_CANFD_MODE_LISTEN_ONLY,  /**< REQOP 0b011 */
    NORA_CANFD_MODE_EXTERNAL_LOOPBACK /**< REQOP 0b101 - TX driven on the pin/transceiver, RX from the bus */
} nora_canfd_mode_t;

/** Free-running millisecond tick provider (NULL = blocking ops never time out). */
typedef uint32_t (*nora_canfd_get_ms_fn)(void);

/* ---- shared lifecycle / queries (init lives in the role header) ---- */

/** Tear down the instance and return it to NONE mode. */
nora_canfd_status_t nora_canfd_deinit(nora_canfd_instance_t inst);

/** True if the instance exists on this device. */
bool nora_canfd_is_present(nora_canfd_instance_t inst);

/**
 * Enable or disable the selected CAN FD module at the PMD gate.
 *
 * This only controls the peripheral module-disable bit. Board code still owns
 * clock routing, PPS, and transceiver pins.
 */
nora_canfd_status_t nora_canfd_module_enable(
    nora_canfd_instance_t inst,
    bool enable);

/** True if the instance has been initialized (mode != NONE). */
bool nora_canfd_is_initialized(nora_canfd_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CANFD_H */
