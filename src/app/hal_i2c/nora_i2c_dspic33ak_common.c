#include "nora_i2c.h"
#include "nora_i2c_dspic33ak_device.h"
#include "nora_i2c_dspic33ak_reg.h"
#include "nora_i2c_dspic33ak_internal.h"

/* --------------------------------------------------------------------------
 * Shared helpers used by both the master and slave engines.
 *
 * The resolution helpers (inst_is_valid / get_regs / calc_brg / is_present) are
 * pure. The only module state here is the per-instance role (set by the master
 * and slave engines on init/deinit) behind nora_i2c_is_initialized(); the
 * engines' own per-instance state (timeout config, pending tracking, slave
 * callbacks) still lives in their respective translation units.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Status -> short name
 * -------------------------------------------------------------------------- */
const char *nora_i2c_status_str(nora_i2c_status_t status)
{
    switch (status) {
    case NORA_I2C_OK:                  return "OK";
    case NORA_I2C_ERR_INVALID_ARG:     return "ERR_INVALID_ARG";
    case NORA_I2C_ERR_NOT_PRESENT:     return "ERR_NOT_PRESENT";
    case NORA_I2C_ERR_NOT_INITIALIZED: return "ERR_NOT_INITIALIZED";
    case NORA_I2C_ERR_BUSY:            return "ERR_BUSY";
    case NORA_I2C_ERR_TIMEOUT:         return "ERR_TIMEOUT";
    case NORA_I2C_ERR_NACK:            return "ERR_NACK";
    case NORA_I2C_ERR_BUS:             return "ERR_BUS";
    case NORA_I2C_ERR_COLLISION:       return "ERR_COLLISION";
    case NORA_I2C_ERR_UNSUPPORTED:     return "ERR_UNSUPPORTED";
    case NORA_I2C_ERR_SEQUENCE:        return "ERR_SEQUENCE";
    default:                                return "?";
    }
}

/* --------------------------------------------------------------------------
 * Validate instance number
 * -------------------------------------------------------------------------- */
bool nora_i2c_inst_is_valid(nora_i2c_instance_t inst)
{
    /* NORA_I2C_INST_SUPPORTED_COUNT: every per-instance array is that wide and
     * every access passes through here -- see nora_i2c.h. */
    return ((unsigned)inst < (unsigned)NORA_I2C_INST_SUPPORTED_COUNT);
}

/* --------------------------------------------------------------------------
 * Resolve instance to register table
 * -------------------------------------------------------------------------- */
nora_i2c_status_t nora_i2c_get_regs(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs)
{
    const nora_i2c_device_t *dev;

    if (regs == 0) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ERR_INVALID_ARG;
    }

    dev = nora_i2c_get_device(inst);
    if (dev == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    *regs = &dev->regs;
    return NORA_I2C_OK;
}

/* --------------------------------------------------------------------------
 * Calculate BRG value
 * -------------------------------------------------------------------------- */
uint32_t nora_i2c_calc_brg(uint32_t fcy_hz, uint32_t bus_hz)
{
    uint64_t div;

    if (bus_hz == 0u) {
        return 0u;
    }

    /*
     * Round to the nearest divider while using 64-bit arithmetic to avoid
     * uint32_t overflow in fcy_hz + bus_hz on future faster devices.
     */
    div = ((uint64_t)fcy_hz + (uint64_t)bus_hz) /
          (2ull * (uint64_t)bus_hz);

    if (div == 0ull) {
        return 0u;
    }

    return (uint32_t)(div - 1ull);
}

/* --------------------------------------------------------------------------
 * Check whether I2C instance exists on the selected device
 * -------------------------------------------------------------------------- */
bool nora_i2c_is_present(nora_i2c_instance_t inst)
{
    return nora_i2c_instance_is_present(inst);
}

/* --------------------------------------------------------------------------
 * Shared role / lifecycle state
 *
 * The master and slave engines record their role here on init/deinit so the
 * public nora_i2c_is_initialized() reflects either role. This is the only
 * module state in the common layer.
 * -------------------------------------------------------------------------- */
static nora_i2c_role_t g_role[NORA_I2C_INST_SUPPORTED_COUNT];

void nora_i2c_set_role(nora_i2c_instance_t inst,
                            nora_i2c_role_t role)
{
    if (nora_i2c_inst_is_valid(inst)) {
        g_role[inst] = role;
    }
}

nora_i2c_role_t nora_i2c_get_role(nora_i2c_instance_t inst)
{
    if (!nora_i2c_inst_is_valid(inst)) {
        return NORA_I2C_ROLE_NONE;
    }
    return g_role[inst];
}

/* --------------------------------------------------------------------------
 * Initialized query (true once init'd as either master or slave)
 * -------------------------------------------------------------------------- */
bool nora_i2c_is_initialized(nora_i2c_instance_t inst)
{
    return (nora_i2c_get_role(inst) != NORA_I2C_ROLE_NONE);
}
