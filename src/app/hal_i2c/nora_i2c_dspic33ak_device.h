#ifndef NORA_I2C_DSPIC33AK_DEVICE_H
#define NORA_I2C_DSPIC33AK_DEVICE_H

#include <stdbool.h>

#include "nora_i2c.h"
#include "nora_i2c_dspic33ak_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal dsPIC33AK I2C controller inventory. Not part of the public API. */
typedef struct {
    bool present;
    nora_i2c_regs_t regs;
} nora_i2c_device_t;

const nora_i2c_device_t *nora_i2c_get_device(
    nora_i2c_instance_t inst);

bool nora_i2c_instance_is_present(nora_i2c_instance_t inst);

/* Per-instance interrupt flag / enable access.  See the comment block above the
 * definitions in nora_i2c_dspic33ak_device.c for why these exist instead of an
 * { &IFSn, &IECn, mask } descriptor.  false means the instance has no such
 * interrupt on this device. */
bool nora_i2c_device_event_irq_is_mapped(nora_i2c_instance_t inst);
bool nora_i2c_device_event_irq_clear_flag(nora_i2c_instance_t inst);
bool nora_i2c_device_event_irq_enable(nora_i2c_instance_t inst, bool enable);
bool nora_i2c_device_rx_irq_clear_flag(nora_i2c_instance_t inst);
bool nora_i2c_device_rx_irq_enable(nora_i2c_instance_t inst, bool enable);
bool nora_i2c_device_tx_irq_clear_flag(nora_i2c_instance_t inst);
bool nora_i2c_device_tx_irq_enable(nora_i2c_instance_t inst, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_DSPIC33AK_DEVICE_H */
