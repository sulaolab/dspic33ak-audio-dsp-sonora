#ifndef NORA_I2C_DSPIC33AK_INTERNAL_H
#define NORA_I2C_DSPIC33AK_INTERNAL_H

#include "nora_i2c.h"
#include "nora_i2c_dspic33ak_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Internal dsPIC33AK helpers shared by the master and slave engines.
 *
 * This header is not part of the NORA I2C public API. It resolves a logical
 * instance to the dsPIC33AK register table, computes the backend timing
 * divider, and owns shared role/lifecycle state. Application and board code
 * must include only nora_i2c.h, nora_i2c_master.h, and/or nora_i2c_slave.h.
 */

bool nora_i2c_inst_is_valid(nora_i2c_instance_t inst);

nora_i2c_status_t nora_i2c_get_regs(
    nora_i2c_instance_t inst,
    const nora_i2c_regs_t **regs);

uint32_t nora_i2c_calc_brg(uint32_t fcy_hz, uint32_t bus_hz);

/*
 * Shared per-instance role/lifecycle state. The master and slave engines set
 * this on init/deinit so the public nora_i2c_is_initialized() reports the
 * truth for either role. The engines keep their own state separately and do
 * not reference each other.
 */
typedef enum {
    NORA_I2C_ROLE_NONE = 0,
    NORA_I2C_ROLE_MASTER,
    NORA_I2C_ROLE_SLAVE
} nora_i2c_role_t;

void nora_i2c_set_role(nora_i2c_instance_t inst,
                       nora_i2c_role_t role);
nora_i2c_role_t nora_i2c_get_role(nora_i2c_instance_t inst);

/*
 * Backend-only slave ISR delegates for the dedicated RX / TX buffer interrupts.
 *
 * These are a hedge, not a path in use: nora_i2c_slave_init() aggregates every
 * client condition onto the event interrupt via INTC and enables only that
 * vector, so I2CxRXIE / I2CxTXIE stay 0 and these never fire. They exist so the
 * device layer can bind the RX/TX vectors defined by this silicon, and because
 * both funnel into the same idempotent service routine an extra pass would be
 * harmless if a future smart/FIFO path did enable them.
 *
 * They are deliberately NOT in nora_i2c_slave.h: how many interrupt sources one
 * slave has is silicon count (the CK part has a single SI2Cx), and the device
 * layer owns the vectors on both families, so no caller can name them.
 */
void nora_i2c_slave_rx_irq(nora_i2c_instance_t inst);
void nora_i2c_slave_tx_irq(nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_DSPIC33AK_INTERNAL_H */
