#ifndef NORA_I2C_H
#define NORA_I2C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NORA I2C public types and lifecycle API.
 *
 * This header carries only what the master and slave roles share:
 *   - the instance and status enumerations,
 *   - the millisecond-tick callback type,
 *   - presence / initialized queries and deinit.
 *
 * For the bus-master API include nora_i2c_master.h; for the slave API
 * include nora_i2c_slave.h. A program may include either or both. Both
 * pull in this header, so the shared types are always available.
 *
 * Application and board code choose the instance passed to this API. The
 * enum identifies a logical controller slot; it does not promise that an AK
 * and a CK board use the same physical peripheral for a given slot.
 *
 * This header intentionally does not expose XC-DSC/DFP bitfield types.
 */

/* NORA_I2C_HW_INST_MAX is the enum below as a preprocessor literal -- an
 * enumerator is invisible to #if, which would evaluate it as 0, so the range
 * check further down needs a macro. The _Static_assert keeps them in step. */
#define NORA_I2C_HW_INST_MAX   4

typedef enum {
    NORA_I2C_INST_1 = 0,
    NORA_I2C_INST_2,
    NORA_I2C_INST_3,
    NORA_I2C_INST_4,
    NORA_I2C_INST_COUNT
} nora_i2c_instance_t;

/*
 * Per-instance state width (project-configurable, optional).
 *
 * The enum above, the API and every prototype are unchanged; what narrows is the
 * SIZE of the driver's per-instance arrays. An instance at or above the count
 * reports itself absent (nora_i2c_get_device() returns 0, so
 * nora_i2c_inst_is_valid() is false and init fails with NOT_PRESENT) rather than
 * being accepted into an array that no longer covers it. The HAL ships no
 * conf.h, so vendoring hal_i2c without one keeps the previous behaviour.
 * See board/i2c/nora_i2c_conf.h for this product's value.
 */
#if !defined( NORA_I2C_INST_SUPPORTED_COUNT )
#  if defined( __has_include )
#    if __has_include( "nora_i2c_conf.h" )
#      include "nora_i2c_conf.h"
#    endif
#  endif
#endif

#ifndef NORA_I2C_INST_SUPPORTED_COUNT
#define NORA_I2C_INST_SUPPORTED_COUNT   NORA_I2C_HW_INST_MAX
#endif

#if (NORA_I2C_INST_SUPPORTED_COUNT < 1) || \
    (NORA_I2C_INST_SUPPORTED_COUNT > NORA_I2C_HW_INST_MAX)
#error "NORA_I2C_INST_SUPPORTED_COUNT must be 1..NORA_I2C_HW_INST_MAX."
#endif

_Static_assert( (int)NORA_I2C_INST_COUNT == NORA_I2C_HW_INST_MAX,
                "NORA_I2C_HW_INST_MAX must match nora_i2c_instance_t" );

typedef enum {
    NORA_I2C_OK = 0,
    NORA_I2C_ERR_INVALID_ARG,
    NORA_I2C_ERR_NOT_PRESENT,
    NORA_I2C_ERR_NOT_INITIALIZED,
    NORA_I2C_ERR_BUSY,
    NORA_I2C_ERR_TIMEOUT,
    NORA_I2C_ERR_NACK,
    NORA_I2C_ERR_BUS,
    NORA_I2C_ERR_COLLISION,
    NORA_I2C_ERR_UNSUPPORTED,
    NORA_I2C_ERR_SEQUENCE
} nora_i2c_status_t;

/*
 * Return the status as a short name, e.g. "ERR_NACK".
 * Never NULL: an unrecognised value returns "?".
 */
const char *nora_i2c_status_str(nora_i2c_status_t status);

typedef uint32_t (*nora_i2c_get_ms_fn)(void);

/* Shared lifecycle / query API -------------------------------------------- */

/*
 * Deinitialize the selected I2C instance -- the MASTER role.
 *
 * It sits in this shared header because the peripheral is one thing and the
 * name is older than the two-role split, but it is the counterpart of
 * nora_i2c_init() only. An instance brought up with nora_i2c_slave_init() is
 * released by nora_i2c_slave_deinit(); calling this on one answers
 * NORA_I2C_ERR_NOT_INITIALIZED even though nora_i2c_is_initialized() below
 * reports true for it, because that query covers either role and this call
 * does not.
 *
 * If deinit recovers a stale pending transaction, it may return the recovery
 * status while still forcing the peripheral off and clearing HAL state.
 */
nora_i2c_status_t nora_i2c_deinit(
    nora_i2c_instance_t inst);

bool nora_i2c_is_present(
    nora_i2c_instance_t inst);

/*
 * Set the CPU interrupt priority for the selected I2C instance's event line,
 * and any dedicated RX/TX lines supported by the selected backend. Platform
 * startup still owns the interrupt-vector bindings.
 */
nora_i2c_status_t nora_i2c_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority);

/*
 * True once the instance has been initialized in EITHER role. Use
 * nora_i2c_slave_is_active() to tell the roles apart -- and note that
 * nora_i2c_deinit() above releases only the master one.
 */
bool nora_i2c_is_initialized(
    nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_H */
