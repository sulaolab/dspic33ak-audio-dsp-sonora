#include <xc.h>
#include "nora_i2c_dspic33ak_device.h"
#include "nora_i2c_slave.h"
#include "nora_i2c_dspic33ak_internal.h"

/*
 * Device/instance mapping layer.
 *
 * This is the only place that should know about I2C1CON1/I2C2CON1/I2C3CON1
 * symbol names.  The portable-ish driver logic should use only the register
 * pointer table returned from nora_i2c_get_device().
 */

static const nora_i2c_device_t g_i2c_devices[NORA_I2C_INST_COUNT] = {
#if defined(I2C1CON1)
    [NORA_I2C_INST_1] = {
        .present = true,
        .regs = {
            .CON1 = &I2C1CON1,
            .CON2 = &I2C1CON2,
            .STAT1 = &I2C1STAT1,
            .STAT2 = &I2C1STAT2,
            .BITO = &I2C1BITO,
            .LBRG = &I2C1LBRG,
            .HBRG = &I2C1HBRG,
            .TRN = &I2C1TRN,
            .RCV = &I2C1RCV,
            .ADD = &I2C1ADD,
            .MSK = &I2C1MSK,
            .INTC = &I2C1INTC,
        },
    },
#else
    [NORA_I2C_INST_1] = { .present = false },
#endif

#if defined(I2C2CON1)
    [NORA_I2C_INST_2] = {
        .present = true,
        .regs = {
            .CON1 = &I2C2CON1,
            .CON2 = &I2C2CON2,
            .STAT1 = &I2C2STAT1,
            .STAT2 = &I2C2STAT2,
            .BITO = &I2C2BITO,
            .LBRG = &I2C2LBRG,
            .HBRG = &I2C2HBRG,
            .TRN = &I2C2TRN,
            .RCV = &I2C2RCV,
            .ADD = &I2C2ADD,
            .MSK = &I2C2MSK,
            .INTC = &I2C2INTC,
        },
    },
#else
    [NORA_I2C_INST_2] = { .present = false },
#endif

#if defined(I2C3CON1)
    [NORA_I2C_INST_3] = {
        .present = true,
        .regs = {
            .CON1 = &I2C3CON1,
            .CON2 = &I2C3CON2,
            .STAT1 = &I2C3STAT1,
            .STAT2 = &I2C3STAT2,
            .BITO = &I2C3BITO,
            .LBRG = &I2C3LBRG,
            .HBRG = &I2C3HBRG,
            .TRN = &I2C3TRN,
            .RCV = &I2C3RCV,
            .ADD = &I2C3ADD,
            .MSK = &I2C3MSK,
            .INTC = &I2C3INTC,
        },
    },
#else
    [NORA_I2C_INST_3] = { .present = false },
#endif

#if defined(I2C4CON1)
    [NORA_I2C_INST_4] = {
        .present = true,
        .regs = {
            .CON1 = &I2C4CON1,
            .CON2 = &I2C4CON2,
            .STAT1 = &I2C4STAT1,
            .STAT2 = &I2C4STAT2,
            .BITO = &I2C4BITO,
            .LBRG = &I2C4LBRG,
            .HBRG = &I2C4HBRG,
            .TRN = &I2C4TRN,
            .RCV = &I2C4RCV,
        },
    },
#else
    [NORA_I2C_INST_4] = { .present = false },
#endif
};

const nora_i2c_device_t *nora_i2c_get_device(
    nora_i2c_instance_t inst)
{
    /* NORA_I2C_INST_SUPPORTED_COUNT, not the enum count: an instance the project
     * narrowed away has no per-instance state, so it must report absent here.
     * The register table itself stays full-width -- it is const, in flash. */
    if ((unsigned)inst >= (unsigned)NORA_I2C_INST_SUPPORTED_COUNT) {
        return 0;
    }

    if (!g_i2c_devices[inst].present) {
        return 0;
    }

    return &g_i2c_devices[inst];
}

bool nora_i2c_instance_is_present(nora_i2c_instance_t inst)
{
    return (nora_i2c_get_device(inst) != 0);
}

nora_i2c_status_t nora_i2c_set_interrupt_priority(
    nora_i2c_instance_t inst,
    uint8_t priority)
{
    if ((unsigned)inst >= (unsigned)NORA_I2C_INST_SUPPORTED_COUNT) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (priority > 7u) {
        return NORA_I2C_ERR_INVALID_ARG;
    }
    if (nora_i2c_get_device(inst) == 0) {
        return NORA_I2C_ERR_NOT_PRESENT;
    }

    switch (inst) {
    case NORA_I2C_INST_1:
    {
        bool ok = false;
#if defined(_I2C1IP)
        _I2C1IP = priority;
        ok = true;
#endif
#if defined(_I2C1RXIP)
        _I2C1RXIP = priority;
        ok = true;
#endif
#if defined(_I2C1TXIP)
        _I2C1TXIP = priority;
        ok = true;
#endif
        return ok ? NORA_I2C_OK : NORA_I2C_ERR_UNSUPPORTED;
    }
    case NORA_I2C_INST_2:
    {
        bool ok = false;
#if defined(_I2C2IP)
        _I2C2IP = priority;
        ok = true;
#endif
#if defined(_I2C2RXIP)
        _I2C2RXIP = priority;
        ok = true;
#endif
#if defined(_I2C2TXIP)
        _I2C2TXIP = priority;
        ok = true;
#endif
        return ok ? NORA_I2C_OK : NORA_I2C_ERR_UNSUPPORTED;
    }
    case NORA_I2C_INST_3:
    {
        bool ok = false;
#if defined(_I2C3IP)
        _I2C3IP = priority;
        ok = true;
#endif
#if defined(_I2C3RXIP)
        _I2C3RXIP = priority;
        ok = true;
#endif
#if defined(_I2C3TXIP)
        _I2C3TXIP = priority;
        ok = true;
#endif
        return ok ? NORA_I2C_OK : NORA_I2C_ERR_UNSUPPORTED;
    }
    case NORA_I2C_INST_4:
    {
        bool ok = false;
#if defined(_I2C4IP)
        _I2C4IP = priority;
        ok = true;
#endif
#if defined(_I2C4RXIP)
        _I2C4RXIP = priority;
        ok = true;
#endif
#if defined(_I2C4TXIP)
        _I2C4TXIP = priority;
        ok = true;
#endif
        return ok ? NORA_I2C_OK : NORA_I2C_ERR_UNSUPPORTED;
    }
    default:
        break;
    }

    return NORA_I2C_ERR_UNSUPPORTED;
}

/* --------------------------------------------------------------------------
 * Interrupt flag / enable access (DFP bit aliases)
 *
 * These replace the old { &IFSn, &IECn, mask } descriptors in the register
 * table.  Each store is a literal into a named bit alias, so the compiler emits
 * a single bset.b / bclr.b on a register that is shared with every other
 * peripheral, instead of a read-modify-write that could undo a bit another
 * interrupt changed in between.  The enable pair is an if/else rather than
 * `_I2CxIE = value` for the same reason: assigning a runtime value to a bit
 * alias is a byte-wide read-modify-write.
 *
 * All of them return false for an instance the device does not have, which is
 * also how the slave driver tests whether an instance has an event interrupt at
 * all (nora_i2c_device_event_irq_is_mapped).
 * -------------------------------------------------------------------------- */

bool nora_i2c_device_event_irq_is_mapped(nora_i2c_instance_t inst)
{
    switch (inst) {
#if defined(_I2C1IE)
    case NORA_I2C_INST_1: return true;
#endif
#if defined(_I2C2IE)
    case NORA_I2C_INST_2: return true;
#endif
#if defined(_I2C3IE)
    case NORA_I2C_INST_3: return true;
#endif
#if defined(_I2C4IE)
    case NORA_I2C_INST_4: return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_event_irq_clear_flag(nora_i2c_instance_t inst)
{
    switch (inst) {
#if defined(_I2C1IF)
    case NORA_I2C_INST_1: _I2C1IF = 0; return true;
#endif
#if defined(_I2C2IF)
    case NORA_I2C_INST_2: _I2C2IF = 0; return true;
#endif
#if defined(_I2C3IF)
    case NORA_I2C_INST_3: _I2C3IF = 0; return true;
#endif
#if defined(_I2C4IF)
    case NORA_I2C_INST_4: _I2C4IF = 0; return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_event_irq_enable(nora_i2c_instance_t inst, bool enable)
{
    switch (inst) {
#if defined(_I2C1IE)
    case NORA_I2C_INST_1:
        if (enable) { _I2C1IE = 1; } else { _I2C1IE = 0; }
        return true;
#endif
#if defined(_I2C2IE)
    case NORA_I2C_INST_2:
        if (enable) { _I2C2IE = 1; } else { _I2C2IE = 0; }
        return true;
#endif
#if defined(_I2C3IE)
    case NORA_I2C_INST_3:
        if (enable) { _I2C3IE = 1; } else { _I2C3IE = 0; }
        return true;
#endif
#if defined(_I2C4IE)
    case NORA_I2C_INST_4:
        if (enable) { _I2C4IE = 1; } else { _I2C4IE = 0; }
        return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_rx_irq_clear_flag(nora_i2c_instance_t inst)
{
    switch (inst) {
#if defined(_I2C1RXIF)
    case NORA_I2C_INST_1: _I2C1RXIF = 0; return true;
#endif
#if defined(_I2C2RXIF)
    case NORA_I2C_INST_2: _I2C2RXIF = 0; return true;
#endif
#if defined(_I2C3RXIF)
    case NORA_I2C_INST_3: _I2C3RXIF = 0; return true;
#endif
#if defined(_I2C4RXIF)
    case NORA_I2C_INST_4: _I2C4RXIF = 0; return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_rx_irq_enable(nora_i2c_instance_t inst, bool enable)
{
    switch (inst) {
#if defined(_I2C1RXIE)
    case NORA_I2C_INST_1:
        if (enable) { _I2C1RXIE = 1; } else { _I2C1RXIE = 0; }
        return true;
#endif
#if defined(_I2C2RXIE)
    case NORA_I2C_INST_2:
        if (enable) { _I2C2RXIE = 1; } else { _I2C2RXIE = 0; }
        return true;
#endif
#if defined(_I2C3RXIE)
    case NORA_I2C_INST_3:
        if (enable) { _I2C3RXIE = 1; } else { _I2C3RXIE = 0; }
        return true;
#endif
#if defined(_I2C4RXIE)
    case NORA_I2C_INST_4:
        if (enable) { _I2C4RXIE = 1; } else { _I2C4RXIE = 0; }
        return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_tx_irq_clear_flag(nora_i2c_instance_t inst)
{
    switch (inst) {
#if defined(_I2C1TXIF)
    case NORA_I2C_INST_1: _I2C1TXIF = 0; return true;
#endif
#if defined(_I2C2TXIF)
    case NORA_I2C_INST_2: _I2C2TXIF = 0; return true;
#endif
#if defined(_I2C3TXIF)
    case NORA_I2C_INST_3: _I2C3TXIF = 0; return true;
#endif
#if defined(_I2C4TXIF)
    case NORA_I2C_INST_4: _I2C4TXIF = 0; return true;
#endif
    default: break;
    }

    return false;
}

bool nora_i2c_device_tx_irq_enable(nora_i2c_instance_t inst, bool enable)
{
    switch (inst) {
#if defined(_I2C1TXIE)
    case NORA_I2C_INST_1:
        if (enable) { _I2C1TXIE = 1; } else { _I2C1TXIE = 0; }
        return true;
#endif
#if defined(_I2C2TXIE)
    case NORA_I2C_INST_2:
        if (enable) { _I2C2TXIE = 1; } else { _I2C2TXIE = 0; }
        return true;
#endif
#if defined(_I2C3TXIE)
    case NORA_I2C_INST_3:
        if (enable) { _I2C3TXIE = 1; } else { _I2C3TXIE = 0; }
        return true;
#endif
#if defined(_I2C4TXIE)
    case NORA_I2C_INST_4:
        if (enable) { _I2C4TXIE = 1; } else { _I2C4TXIE = 0; }
        return true;
#endif
    default: break;
    }

    return false;
}

/* --------------------------------------------------------------------------
 * Slave interrupt vectors
 *
 * Each vector delegates to the portable slave engine, which clears the flag and
 * services I2CxSTAT1. Guarded per-instance so the driver only defines vectors
 * for I2C instances the target actually has.
 *
 * Owning the vectors here - rather than leaving them to the application, as this
 * backend used to - is the convention every other module in this HAL already
 * follows (hal_ccp_input_capture, hal_spi_i2s_tdm) and matches the dsPIC33CK
 * backend, so a slave application is source-portable between the two families.
 *
 * All three sources this silicon exposes per instance are bound, but only the
 * event vector can fire: nora_i2c_slave_init() aggregates every client condition
 * onto I2CxIF through INTC and leaves I2CxRXIE / I2CxTXIE at 0. The RX/TX pair is
 * bound anyway so the hedge documented in the slave engine stays a working path
 * (the service routine is idempotent) instead of unreachable code.
 *
 * The I2CxE (bus error) source is deliberately not bound: this driver does not
 * service it, so an integration that wants error interrupts still owns that
 * vector.
 *
 * The per-instance #if defined(_I2CxIF) guards ask "does this silicon have the
 * source", never "does this product answer as a slave", so a master-only build
 * still gets every vector -- and, through them, the whole slave engine, since
 * --gc-sections keeps what a live vector calls. NORA_I2C_DEFINE_SLAVE_VECTORS
 * (see nora_i2c_slave.h, set per project in board/i2c/nora_i2c_conf.h) is the
 * second question. At 0 the vectors are not defined and the engine drops out of
 * the link with them; the engine itself still compiles, so an integration can
 * own the IVT and call the delegates directly.
 * -------------------------------------------------------------------------- */
/*
 * `context` ON EVERY VECTOR HERE, NOT `no_auto_psv`.
 *
 * On dsPIC33A the alternate W0-W7 array is INHERENTLY tied to the IPL (DS70005591D:
 * seven arrays plus AccA/AccB/RCOUNT and the DSP CORCON bits; "IPL4 is assigned to
 * Context 4"), so an ISR does not need to save W0-W7 -- the hardware already handed it
 * its own copies. `context` states that fact; `no_auto_psv` is a 16-bit-era attribute
 * that says nothing on a part with one unified address space.
 *
 * THIS IS ABOUT NESTING. Every nesting level runs at a higher IPL than the one it
 * preempted, so every level gets a DIFFERENT bank automatically, and interrupts at
 * equal IPL cannot preempt each other at all. Depth is bounded at 7 by the IPL range
 * -- which is also why there is no bank-exhaustion case to guard: this project assigns
 * only IPL 3..5 (see the rate-monotonic assignment in the ASRC app), all well inside
 * the seven arrays. An IPL above 7 would have no array, and nothing here can reach one.
 *
 * The vectors below are one-line thunks calling an out-of-line handler, so they touch
 * nothing beyond the argument registers and `context` takes their prologues to ZERO
 * pushes. That matters beyond code size: prologue pushes at an ISR's first instruction
 * are the documented trigger of the A1 silicon STACK ERROR -- see the DO-NOT-REVERT
 * note in the application-level ASRC clock control, and the
 * per-vector noinline bodies in nora_spi_i2s_tdm_dspic33ak.c for the case where the
 * pushes are W8+ and `context` cannot remove them.
 *
 * NOT for trap handlers: a trap runs in whatever register context the CPU was already
 * in, so it cannot rely on a bank of its own.
 */
#if NORA_I2C_DEFINE_SLAVE_VECTORS

#if defined(_I2C1IF)
void __attribute__((interrupt, context)) _I2C1Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_1);
}
#endif
#if defined(_I2C1RXIF)
void __attribute__((interrupt, context)) _I2C1RXInterrupt(void)
{
    nora_i2c_slave_rx_irq(NORA_I2C_INST_1);
}
#endif
#if defined(_I2C1TXIF)
void __attribute__((interrupt, context)) _I2C1TXInterrupt(void)
{
    nora_i2c_slave_tx_irq(NORA_I2C_INST_1);
}
#endif

#if defined(_I2C2IF)
void __attribute__((interrupt, context)) _I2C2Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_2);
}
#endif
#if defined(_I2C2RXIF)
void __attribute__((interrupt, context)) _I2C2RXInterrupt(void)
{
    nora_i2c_slave_rx_irq(NORA_I2C_INST_2);
}
#endif
#if defined(_I2C2TXIF)
void __attribute__((interrupt, context)) _I2C2TXInterrupt(void)
{
    nora_i2c_slave_tx_irq(NORA_I2C_INST_2);
}
#endif

#if defined(_I2C3IF)
void __attribute__((interrupt, context)) _I2C3Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_3);
}
#endif
#if defined(_I2C3RXIF)
void __attribute__((interrupt, context)) _I2C3RXInterrupt(void)
{
    nora_i2c_slave_rx_irq(NORA_I2C_INST_3);
}
#endif
#if defined(_I2C3TXIF)
void __attribute__((interrupt, context)) _I2C3TXInterrupt(void)
{
    nora_i2c_slave_tx_irq(NORA_I2C_INST_3);
}
#endif

#if defined(_I2C4IF)
void __attribute__((interrupt, context)) _I2C4Interrupt(void)
{
    nora_i2c_slave_event_irq(NORA_I2C_INST_4);
}
#endif
#if defined(_I2C4RXIF)
void __attribute__((interrupt, context)) _I2C4RXInterrupt(void)
{
    nora_i2c_slave_rx_irq(NORA_I2C_INST_4);
}
#endif
#if defined(_I2C4TXIF)
void __attribute__((interrupt, context)) _I2C4TXInterrupt(void)
{
    nora_i2c_slave_tx_irq(NORA_I2C_INST_4);
}
#endif

#endif /* NORA_I2C_DEFINE_SLAVE_VECTORS */
