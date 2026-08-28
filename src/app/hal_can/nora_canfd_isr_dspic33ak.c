/**
 * @file    nora_canfd_isr_dspic33ak.c
 * @brief   dsPIC33AK CAN FD HAL - OPTIONAL interrupt / event layer.
 *
 * Additive only: this compilation unit touches extra interrupt-enable bits in
 * registers the device map already exposes (CxINT, CxFIFOCON1, CxTXQCON) and
 * reads CxTREC. It does NOT modify the blocking node layer or the message-RAM
 * layout. A project that never references this file gets the exact same
 * blocking behaviour as before.
 *
 * The HAL owns only the module interrupt sources and the top-level priority /
 * enable / flag bits (_CxIF/_CxIE/_CxIP). It does NOT define the _CxInterrupt
 * vector - the application owns that and forwards to nora_canfd_irq_handler().
 */
#include "nora_canfd_isr.h"
#include "nora_canfd_common.h"
#include "nora_canfd_dspic33ak_reg.h"

#include <xc.h>
#include <stddef.h>

/* ISR-layer state only; the node layer's g_node[] is never touched here. */
static nora_canfd_event_callback_t g_cb[NORA_CANFD_INST_COUNT];
static void                            *g_ud[NORA_CANFD_INST_COUNT];
static volatile bool                    g_tx_busy[NORA_CANFD_INST_COUNT];

static uint8_t                          g_irq_priority[NORA_CANFD_INST_COUNT];
/* True between isr_enable() and isr_disable(). Async TX is an interrupt-driven service, so
 * tx_start() requires it: without this flag an isr_disable() could be undone by a later
 * tx_start(), which re-arms the TX CPU line (at the priority isr_enable() last recorded)
 * behind the back of the caller that deliberately disabled interrupts. */
static bool                             g_isr_enabled[NORA_CANFD_INST_COUNT];

/* ---------------------------------------------------------------------- */
/* Top-level CPU interrupt line helpers (per instance, guarded by symbol).  */
/*                                                                          */
/* IMPORTANT: on dsPIC33AK the CAN FD module has SEPARATE CPU interrupt      */
/* vectors/IFS-IEC bits: CxRX (receive FIFO), CxTX (transmit FIFO) and Cx    */
/* (general/error). The RX-FIFO-not-empty interrupt is delivered on CxRX,    */
/* NOT on the general Cx line - so both of those must be enabled and cleared */
/* and forwarded to nora_canfd_irq_handler(). The TX line is armed only by   */
/* nora_canfd_tx_start(), so _CxTXInterrupt need only be defined by a        */
/* consumer that actually uses the (unvalidated) async TX path.              */
/* ---------------------------------------------------------------------- */
static void irq_line_enable(nora_canfd_instance_t inst, uint8_t priority)
{
    g_irq_priority[inst] = priority;
    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1RXIF)
        _C1RXIF = 0; _C1RXIP = priority; _C1RXIE = 1;
#endif
#if defined(_C1IF)
        _C1IF = 0; _C1IP = priority; _C1IE = 1;
#endif
        break;
    case NORA_CANFD_INST_2:
#if defined(_C2RXIF)
        _C2RXIF = 0; _C2RXIP = priority; _C2RXIE = 1;
#endif
#if defined(_C2IF)
        _C2IF = 0; _C2IP = priority; _C2IE = 1;
#endif
        break;
    default:
        break;
    }
}

static void irq_line_disable(nora_canfd_instance_t inst)
{
    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1RXIF)
        _C1RXIE = 0; _C1RXIF = 0;
#endif
#if defined(_C1TXIF)
        _C1TXIE = 0; _C1TXIF = 0;
#endif
#if defined(_C1IF)
        _C1IE = 0; _C1IF = 0;
#endif
        break;
    case NORA_CANFD_INST_2:
#if defined(_C2RXIF)
        _C2RXIE = 0; _C2RXIF = 0;
#endif
#if defined(_C2TXIF)
        _C2TXIE = 0; _C2TXIF = 0;
#endif
#if defined(_C2IF)
        _C2IE = 0; _C2IF = 0;
#endif
        break;
    default:
        break;
    }
}

static void irq_line_clear(nora_canfd_instance_t inst)
{
    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1RXIF)
        _C1RXIF = 0;
#endif
#if defined(_C1TXIF)
        _C1TXIF = 0;
#endif
#if defined(_C1IF)
        _C1IF = 0;
#endif
        break;
    case NORA_CANFD_INST_2:
#if defined(_C2RXIF)
        _C2RXIF = 0;
#endif
#if defined(_C2TXIF)
        _C2TXIF = 0;
#endif
#if defined(_C2IF)
        _C2IF = 0;
#endif
        break;
    default:
        break;
    }
}

/* The TRANSMIT CPU line is enabled only while a tx_start() is outstanding, so a
 * receive-only consumer never needs to define the _CxTXInterrupt vector (an
 * enabled line with no vector traps to _DefaultInterrupt). isr_enable() leaves
 * it alone; tx_start() arms it, tx_abort()/the TX_COMPLETE path disarm it.      */
static void irq_tx_line_enable(nora_canfd_instance_t inst)
{
    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1TXIF)
        _C1TXIF = 0; _C1TXIP = g_irq_priority[inst]; _C1TXIE = 1;
#endif
        break;
    case NORA_CANFD_INST_2:
#if defined(_C2TXIF)
        _C2TXIF = 0; _C2TXIP = g_irq_priority[inst]; _C2TXIE = 1;
#endif
        break;
    default:
        break;
    }
}

static void irq_tx_line_disable(nora_canfd_instance_t inst)
{
    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1TXIF)
        _C1TXIE = 0; _C1TXIF = 0;
#endif
        break;
    case NORA_CANFD_INST_2:
#if defined(_C2TXIF)
        _C2TXIE = 0; _C2TXIF = 0;
#endif
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Setup                                                                  */
/* ---------------------------------------------------------------------- */
nora_canfd_status_t nora_canfd_isr_set_callback(
    nora_canfd_instance_t inst,
    nora_canfd_event_callback_t callback,
    void *user_data)
{
    if (!nora_canfd_inst_is_valid(inst)) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    g_cb[inst] = callback;
    g_ud[inst] = user_data;
    return NORA_CANFD_OK;
}

nora_canfd_status_t nora_canfd_isr_enable(nora_canfd_instance_t inst,
                                                    uint8_t priority)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;

    if (priority == 0u) {
        priority = NORA_CANFD_ISR_DEFAULT_PRIORITY;
    }
    if (priority > 7u) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }
    if (!nora_canfd_is_initialized(inst)) {
        return NORA_CANFD_ERR_NOT_INITIALIZED;
    }

    /* RX FIFO 1: interrupt when not-empty and on overflow. */
    nora_canfd_reg_set(regs->FIFOCON1,
                            NORA_CANFD_FIFOCON_TFNRFNIE | NORA_CANFD_FIFOCON_RXOVIE);
    /* Module roll-up enables: RX + RX-overflow only. NOTE: on dsPIC33AK the
     * bus-error (CERR) / invalid-message (IVM) module interrupts are delivered
     * on SEPARATE CPU vectors (CxWARN/CxMON/...), not the RX/TX/general lines
     * this layer forwards - enabling them here would trap to _DefaultInterrupt
     * unless those vectors are also wired. Bus health is instead surfaced
     * synchronously via nora_canfd_get_status() (CxTREC), and BUS_OFF is
     * derived from TREC inside the handler. */
    nora_canfd_reg_set(regs->INT,
                            NORA_CANFD_INT_RXIE | NORA_CANFD_INT_RXOVIE);

    g_tx_busy[inst] = false;
    irq_line_enable(inst, priority);
    g_isr_enabled[inst] = true;
    return NORA_CANFD_OK;
}

nora_canfd_status_t nora_canfd_isr_disable(nora_canfd_instance_t inst)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;

    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }

    g_isr_enabled[inst] = false;
    irq_line_disable(inst);
    /* Drop all module interrupt enables (TX, RX, RXOV, CERR, IVM). */
    nora_canfd_reg_clear(regs->INT,
                              NORA_CANFD_INT_TXIE | NORA_CANFD_INT_RXIE |
                              NORA_CANFD_INT_RXOVIE | NORA_CANFD_INT_CERRIE |
                              NORA_CANFD_INT_IVMIE);
    nora_canfd_reg_clear(regs->FIFOCON1,
                              NORA_CANFD_FIFOCON_TFNRFNIE | NORA_CANFD_FIFOCON_RXOVIE);
    nora_canfd_reg_clear(regs->TXQCON, NORA_CANFD_TXQCON_TXQEIE);
    g_tx_busy[inst] = false;
    return NORA_CANFD_OK;
}

/* ---------------------------------------------------------------------- */
/* Async transmit                                                         */
/* ---------------------------------------------------------------------- */
nora_canfd_status_t nora_canfd_tx_start(nora_canfd_instance_t inst,
                                                  const nora_canfd_frame_t *frame)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;

    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }
    /* Async TX is an interrupt service: completion is reported only through the event
     * callback, and arming it re-enables the TX CPU line. Refuse while the event layer is
     * disabled, so an isr_disable() cannot be silently undone here (and so a caller that
     * wants no interrupts uses the blocking nora_canfd_transmit() instead). */
    if (!g_isr_enabled[inst]) {
        return NORA_CANFD_ERR_SEQUENCE;
    }

    /* Queue the frame using the existing (non-waiting after TXREQ) node path. */
    st = nora_canfd_transmit(inst, frame);
    if (st != NORA_CANFD_OK) {
        return st;
    }

    g_tx_busy[inst] = true;
    /* Arm "TXQ empty" so TX_COMPLETE fires once everything queued is on the bus. */
    nora_canfd_reg_set(regs->TXQCON, NORA_CANFD_TXQCON_TXQEIE);
    nora_canfd_reg_set(regs->INT, NORA_CANFD_INT_TXIE);
    irq_tx_line_enable(inst);
    return NORA_CANFD_OK;
}

bool nora_canfd_tx_is_busy(nora_canfd_instance_t inst)
{
    if (!nora_canfd_inst_is_valid(inst)) {
        return false;
    }
    return g_tx_busy[inst];
}

nora_canfd_status_t nora_canfd_tx_abort(nora_canfd_instance_t inst)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;

    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }
    nora_canfd_reg_clear(regs->TXQCON, NORA_CANFD_TXQCON_TXQEIE);
    nora_canfd_reg_clear(regs->INT, NORA_CANFD_INT_TXIE);
    irq_tx_line_disable(inst);
    g_tx_busy[inst] = false;
    return NORA_CANFD_OK;
}

/* ---------------------------------------------------------------------- */
/* ISR entry + status                                                     */
/* ---------------------------------------------------------------------- */
void nora_canfd_irq_handler(nora_canfd_instance_t inst)
{
    const nora_canfd_regs_t *regs;
    uint32_t intf;
    uint32_t events = 0u;

    if (nora_canfd_get_regs(inst, &regs) != NORA_CANFD_OK) {
        irq_line_clear(inst);
        return;
    }

    intf = *regs->INT;

    if ((intf & NORA_CANFD_INT_RXIF) != 0u) {
        /* Handler does not drain; the callback consumes via receive(). */
        events |= NORA_CANFD_EVENT_RX_AVAILABLE;
    }
    if ((intf & NORA_CANFD_INT_RXOVIF) != 0u) {
        events |= NORA_CANFD_EVENT_RX_OVERFLOW;
        nora_canfd_reg_clear(regs->FIFOSTA1, NORA_CANFD_FIFOSTA_RXOVIF);
    }
    if (((intf & NORA_CANFD_INT_TXIF) != 0u) &&
        ((*regs->TXQSTA & NORA_CANFD_TXQSTA_TXQEIF) != 0u)) {
        events |= NORA_CANFD_EVENT_TX_COMPLETE;
        /* One-shot: disarm so the level-sensitive TXQ-empty flag stops re-firing. */
        nora_canfd_reg_clear(regs->TXQCON, NORA_CANFD_TXQCON_TXQEIE);
        nora_canfd_reg_clear(regs->INT, NORA_CANFD_INT_TXIE);
        irq_tx_line_disable(inst);
        g_tx_busy[inst] = false;
    }
    if ((intf & NORA_CANFD_INT_CERRIF) != 0u) {
        events |= NORA_CANFD_EVENT_BUS_ERROR;
    }
    if ((intf & NORA_CANFD_INT_IVMIF) != 0u) {
        events |= NORA_CANFD_EVENT_INVALID_MSG;
    }

    /* Clear the writable module flags (TXIF/RXIF/RXOVIF are read-only roll-ups). */
    nora_canfd_reg_clear(regs->INT, NORA_CANFD_INT_CLR_MASK);

    if ((*regs->TREC & NORA_CANFD_TREC_TXBO) != 0u) {
        events |= NORA_CANFD_EVENT_BUS_OFF;
    }

    /* Callback first (drains RX so RXIF de-asserts), then clear the CPU flag. */
    if (events != 0u && g_cb[inst] != NULL) {
        g_cb[inst](inst, events, g_ud[inst]);
    }
    irq_line_clear(inst);
}

nora_canfd_status_t nora_canfd_get_status(nora_canfd_instance_t inst,
                                                    nora_canfd_bus_status_t *status)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;
    uint32_t t;

    if (status == NULL) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }

    t = *regs->TREC;
    status->rx_err_count  = (uint8_t)((t & NORA_CANFD_TREC_RERRCNT_MASK) >> NORA_CANFD_TREC_RERRCNT_POS);
    status->tx_err_count  = (uint8_t)((t & NORA_CANFD_TREC_TERRCNT_MASK) >> NORA_CANFD_TREC_TERRCNT_POS);
    status->error_warning = (t & NORA_CANFD_TREC_EWARN) != 0u;
    status->error_passive = (t & (NORA_CANFD_TREC_TXBP | NORA_CANFD_TREC_RXBP)) != 0u;
    status->bus_off       = (t & NORA_CANFD_TREC_TXBO) != 0u;
    status->rx_overflow   = (*regs->FIFOSTA1 & NORA_CANFD_FIFOSTA_RXOVIF) != 0u;
    return NORA_CANFD_OK;
}

nora_canfd_status_t nora_canfd_clear_rx_overflow(nora_canfd_instance_t inst)
{
    const nora_canfd_regs_t *regs;
    nora_canfd_status_t st;

    st = nora_canfd_get_regs(inst, &regs);
    if (st != NORA_CANFD_OK) {
        return st;
    }
    nora_canfd_reg_clear(regs->FIFOSTA1, NORA_CANFD_FIFOSTA_RXOVIF);
    return NORA_CANFD_OK;
}
