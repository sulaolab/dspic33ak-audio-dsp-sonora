#ifndef NORA_I2C_SLAVE_H
#define NORA_I2C_SLAVE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_i2c.h"

/*
 * Project-supplied compile-time config, optional. The HAL ships no conf.h, so a
 * project that vendors hal_i2c without one keeps the documented default below
 * (vectors defined -- the behaviour this driver has always had) instead of
 * failing to compile on a missing include.
 */
#if !defined( NORA_I2C_DEFINE_SLAVE_VECTORS )
#  if defined( __has_include )
#    if __has_include( "nora_i2c_conf.h" )
#      include "nora_i2c_conf.h"
#    endif
#  endif
#endif

/*
 * I2C slave interrupt-vector ownership -- see board/i2c/nora_i2c_conf.h for the
 * full description and for how a project turns it off.
 *
 *   1 (default) : the device layer defines _I2CxInterrupt / _I2CxRXInterrupt /
 *                 _I2CxTXInterrupt for every instance the silicon has, each
 *                 routing to the slave engine below. Turnkey.
 *   0           : no vectors are defined. Everything else in this header is
 *                 still compiled and callable, so an integration that owns the
 *                 IVT calls nora_i2c_slave_event_irq() (and the _rx_irq /
 *                 _tx_irq hedge) from its own vectors.
 *
 * Defaulted rather than #error'd on purpose: this switch gates code OUT, and a
 * project whose conf.h predates it must keep working exactly as before.
 */
#ifndef NORA_I2C_DEFINE_SLAVE_VECTORS
#define NORA_I2C_DEFINE_SLAVE_VECTORS   1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NORA I2C slave (device/client) interface.
 *
 * Include this header to answer as an I2C slave. The shared instance / status
 * types live in nora_i2c.h (included above); the bus-master role is in
 * nora_i2c_master.h. A program may include either or both.
 *
 * The slave is callback-driven and interrupt-based. The dsPIC33A "new" I2C
 * module aggregates address / data / STOP into one event interrupt, routed there
 * through the INTC register by nora_i2c_slave_init(); the dedicated RX/TX buffer
 * interrupts this silicon also has belong to the DMA/smart path and stay masked.
 *
 * This driver defines the I2C interrupt vectors for the instances that exist on
 * the target (in the device layer); each calls nora_i2c_slave_event_irq(inst).
 * The delegate is also exported so an integration that owns the vector itself,
 * or a host-side unit test, can drive the same service routine.
 *
 * Scope: 7-bit addressing only. 10-bit and general-call are not handled yet.
 */

typedef struct {
    uint8_t addr7;          /* 7-bit own address (right-justified, e.g. 0x55) */
    uint8_t addr_mask;      /* I2CxMSK low 7 bits; 0 = exact match            */
    bool    clock_stretch;  /* STREN: hold SCL low to give callbacks time     */

    /* Address phase: the master addressed us. is_read is true when the master
     * wants to read from us (it will clock bytes out of on_tx_byte), false
     * when it will write to us (bytes arrive at on_rx_byte). May be NULL. */
    void (*on_addr_match)(bool is_read);

    /* Master-write: one received data byte. May be NULL (byte is dropped). */
    void (*on_rx_byte)(uint8_t b);

    /* Master-read: return the next byte to transmit. If NULL, 0xFF is sent. */
    uint8_t (*on_tx_byte)(void);

    /* STOP (or bus-release) ended the transaction. May be NULL. */
    void (*on_stop)(void);
} nora_i2c_slave_config_t;

/*
 * Configure the instance as a slave at config->addr7 (right-justified 7-bit;
 * above 0x7F is NORA_I2C_ERR_INVALID_ARG) and enable it.
 *
 * An instance that is live as a master must be released with nora_i2c_deinit()
 * first, otherwise this returns NORA_I2C_ERR_BUSY. Re-initializing an instance
 * that is already a slave is allowed.
 */
nora_i2c_status_t nora_i2c_slave_init(
    nora_i2c_instance_t inst,
    const nora_i2c_slave_config_t *config);

/* Disable the slave: turn the peripheral off, mask its interrupts, drop state. */
nora_i2c_status_t nora_i2c_slave_deinit(
    nora_i2c_instance_t inst);

/* True once nora_i2c_slave_init() has configured this instance. */
bool nora_i2c_slave_is_active(nora_i2c_instance_t inst);

/*
 * Slave interrupt delegate.
 *
 *   _I2CxInterrupt -> nora_i2c_slave_event_irq(inst)
 *
 * Clears the hardware event-interrupt flag and services whatever the I2CxSTAT1
 * register reports (address match, received byte, transmit-continue, STOP). The
 * device layer wires the real vectors to this function; it is public so a custom
 * vector or a host-side test can call it directly.
 */
void nora_i2c_slave_event_irq(nora_i2c_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_I2C_SLAVE_H */
