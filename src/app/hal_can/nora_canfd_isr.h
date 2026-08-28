/**
 * @file    nora_canfd_isr.h
 * @brief   dsPIC33AK CAN FD HAL - OPTIONAL interrupt / event layer.
 *
 * This file is entirely additive and OPTIONAL. The basic CAN HAL
 * (nora_canfd_node.h: blocking init / transmit / receive) works on its own
 * and is unchanged by this layer - a user who only needs simple blocking CAN
 * never has to include this header or compile nora_canfd_isr_dspic33ak.c.
 *
 * This layer adds what an event-driven driver needs (and what a CMSIS-Driver CAN
 * wrapper maps onto): a user event callback, interrupt enable/disable, a queued
 * transmit helper with an experimental TX-complete event, an ISR entry point the
 * application forwards its CAN vectors to, and a synchronous bus-health query.
 * No ARM_CAN_* types appear here; those stay in the (future) CMSIS wrapper.
 *
 * Opt-in sequence:
 *   1. nora_canfd_init(...)              // basic init (node layer)
 *   2. nora_canfd_isr_set_callback(...)  // your event callback
 *   3. nora_canfd_isr_enable(inst, prio) // arm RX + RX-overflow interrupts
 *   4. forward the CAN vectors to nora_canfd_irq_handler(inst)
 *
 * NOTE (dsPIC33AK): the CAN FD module raises SEPARATE CPU interrupts for
 * receive (_CxRXInterrupt), transmit (_CxTXInterrupt) and general/error
 * (_CxInterrupt), all forwarded to the single nora_canfd_irq_handler() - it
 * reads CxINT and dispatches everything. Forwarding only _CxInterrupt misses
 * RX-FIFO interrupts (RX then only gets serviced on overflow). A receive-only
 * consumer needs _CxRXInterrupt + _CxInterrupt and nothing else: isr_enable()
 * leaves the TX CPU line disabled, and only nora_canfd_tx_start() arms it.
 */
#ifndef NORA_CANFD_ISR_H
#define NORA_CANFD_ISR_H

#include "nora_canfd.h"
#include "nora_canfd_node.h"   /* nora_canfd_frame_t for tx_start */

#ifdef __cplusplus
extern "C" {
#endif

/** Default CPU interrupt priority used when a caller passes 0 to isr_enable. */
#define NORA_CANFD_ISR_DEFAULT_PRIORITY  4u

/* ---------------------------------------------------------------------- */
/* Event bit-flags. The callback receives an OR of these per ISR.         */
/* (Style mirrors hal_uart EVENT_*; maps cleanly to ARM_CAN_EVENT_* later.)*/
/* ---------------------------------------------------------------------- */
#define NORA_CANFD_EVENT_TX_COMPLETE   (1u << 0)  /* all queued TX frames sent */
#define NORA_CANFD_EVENT_RX_AVAILABLE  (1u << 1)  /* >=1 frame waiting in RX FIFO */
#define NORA_CANFD_EVENT_BUS_ERROR     (1u << 2)  /* error state changed (CERRIF) */
#define NORA_CANFD_EVENT_BUS_OFF       (1u << 3)  /* controller went bus-off */
#define NORA_CANFD_EVENT_RX_OVERFLOW   (1u << 4)  /* RX FIFO overflow */
#define NORA_CANFD_EVENT_INVALID_MSG   (1u << 5)  /* invalid message (IVMIF) */

/**
 * User event callback. Invoked from nora_canfd_irq_handler() context with
 * an OR of the NORA_CANFD_EVENT_* bits that occurred.
 *
 * On NORA_CANFD_EVENT_RX_AVAILABLE the handler does NOT drain the FIFO;
 * the callback (or app) must call nora_canfd_receive() to consume the
 * frame(s). Draining inside the callback is fine - data is already present so
 * receive() returns without blocking, and it keeps the RX interrupt from
 * re-asserting immediately (the CMSIS "read in the RECEIVE event" pattern).
 */
typedef void (*nora_canfd_event_callback_t)(nora_canfd_instance_t inst,
                                                 uint32_t events,
                                                 void *user_data);

/** Decoded bus error state plus RX-FIFO overflow (synchronous snapshot). */
typedef struct {
    uint8_t tx_err_count;   /* TERRCNT */
    uint8_t rx_err_count;   /* RERRCNT */
    bool    error_warning;  /* EWARN  - TEC or REC >= 96 */
    bool    error_passive;  /* TXBP or RXBP */
    bool    bus_off;        /* TXBO   */
    bool    rx_overflow;    /* RX FIFO 1 overflowed (sticky) - the "status" path
                             * to detect overflow without the interrupt and
                             * callback. Cleared by
                             * nora_canfd_clear_rx_overflow(). */
} nora_canfd_bus_status_t;

/* ---------------------------------------------------------------------- */
/* Setup                                                                  */
/* ---------------------------------------------------------------------- */

/** Register (or clear, with NULL) the event callback for an instance. */
nora_canfd_status_t nora_canfd_isr_set_callback(
    nora_canfd_instance_t inst,
    nora_canfd_event_callback_t callback,
    void *user_data);

/**
 * Enable interrupts for an instance: the RX-not-empty and RX-overflow sources,
 * plus the RX and general CPU interrupt lines at @p priority (1..7; pass 0 for
 * NORA_CANFD_ISR_DEFAULT_PRIORITY). The TRANSMIT CPU line is deliberately left
 * disabled here - nora_canfd_tx_start() enables it, so a consumer that only
 * receives never has to define the _CxTXInterrupt vector. Call after
 * nora_canfd_init(). TX-complete is armed per-transmit by
 * nora_canfd_tx_start(), not here.
 *
 * Bus-error (CERR) / invalid-message (IVM) sources are deliberately NOT armed:
 * on dsPIC33AK they are delivered on separate CPU vectors this layer does not
 * forward, so enabling them would trap. Bus health is queried synchronously via
 * nora_canfd_get_status(), and BUS_OFF is derived from CxTREC in the handler.
 *
 * The HAL does NOT define the _CxInterrupt vector; the application owns it and
 * forwards to nora_canfd_irq_handler(inst).
 */
nora_canfd_status_t nora_canfd_isr_enable(nora_canfd_instance_t inst,
                                                    uint8_t priority);

/**
 * Disable the top-level CPU interrupt (all three lines) and the module interrupt sources.
 * Also revokes async TX: nora_canfd_tx_start() returns NORA_CANFD_ERR_SEQUENCE until the
 * next nora_canfd_isr_enable(), so nothing can re-arm the TX CPU line behind this call.
 * The blocking nora_canfd_transmit() is unaffected.
 */
nora_canfd_status_t nora_canfd_isr_disable(nora_canfd_instance_t inst);

/* ---------------------------------------------------------------------- */
/* Async transmit                                                         */
/* ---------------------------------------------------------------------- */

/**
 * Queue @p frame into the TX queue (same path as nora_canfd_transmit) and
 * arm the TXQ-empty interrupt so NORA_CANFD_EVENT_TX_COMPLETE fires once
 * all queued frames have actually been transmitted on the bus.
 *
 * Requires the event layer to be enabled: returns NORA_CANFD_ERR_SEQUENCE if called
 * before nora_canfd_isr_enable() or after nora_canfd_isr_disable(). Completion is
 * reported only through the event callback, and arming it re-enables the TX CPU line -
 * so an isr_disable() must not be undone from here. Use the blocking
 * nora_canfd_transmit() to send with interrupts off.
 *
 * This is NOT fully asynchronous: it goes through nora_canfd_transmit(), so
 * if the TX queue is full it first blocks for queue space (honoring the
 * configured timeout). Once the frame is queued it returns without waiting for
 * the frame to leave the bus.
 *
 * NOTE: the RX event path (isr_enable + RX_AVAILABLE) is HW-validated. The
 * TX-complete interrupt arming here is NOT yet validated on dsPIC33AK512MPS512 -
 * enabling it currently triggers an unhandled-interrupt trap, under
 * investigation. Prefer the blocking nora_canfd_transmit() for TX and use
 * the RX events; calling this arms the _CxTXInterrupt line, so that vector must
 * be defined before the first tx_start(); consumers that need a TX-complete signal (e.g. a CMSIS-Driver
 * SEND_COMPLETE) should treat this as unavailable until it is validated.
 */
nora_canfd_status_t nora_canfd_tx_start(nora_canfd_instance_t inst,
                                                  const nora_canfd_frame_t *frame);

/** True between tx_start() and the TX_COMPLETE event. */
bool nora_canfd_tx_is_busy(nora_canfd_instance_t inst);

/** Disarm the TX-complete interrupt and clear the busy flag (does not abort an
 *  in-flight frame on the wire). */
nora_canfd_status_t nora_canfd_tx_abort(nora_canfd_instance_t inst);

/* ---------------------------------------------------------------------- */
/* ISR entry + status                                                     */
/* ---------------------------------------------------------------------- */

/**
 * Service the module interrupt: read/clear the module flags, build the event
 * word and invoke the callback. Ordinary function, NOT an ISR vector - forward
 * ALL of the instance's CAN vectors to it:
 *
 *   void __attribute__((interrupt, no_auto_psv)) _C1RXInterrupt(void)
 *   { nora_canfd_irq_handler(NORA_CANFD_INST_1); }
 *   void __attribute__((interrupt, no_auto_psv)) _C1TXInterrupt(void)  // only
 *   { nora_canfd_irq_handler(NORA_CANFD_INST_1); }             // if tx_start()
 *   void __attribute__((interrupt, no_auto_psv)) _C1Interrupt(void)
 *   { nora_canfd_irq_handler(NORA_CANFD_INST_1); }
 */
void nora_canfd_irq_handler(nora_canfd_instance_t inst);

/**
 * Read and decode the bus error state and the RX-FIFO overflow flag. Works with
 * or without interrupts enabled - this is the synchronous "status" path for
 * overflow detection that complements the RX_OVERFLOW callback event.
 * Use nora_canfd_clear_rx_overflow() to clear the sticky overflow flag.
 */
nora_canfd_status_t nora_canfd_get_status(nora_canfd_instance_t inst,
                                                    nora_canfd_bus_status_t *status);

/**
 * Clear the sticky RX-FIFO-overflow flag.
 *
 * Needed by callers that poll nora_canfd_get_status() instead of taking the
 * RX_OVERFLOW callback: the flag latches, so without an explicit clear the
 * first overflow makes every later snapshot report overflow forever. The
 * interrupt path clears it itself in nora_canfd_irq_handler().
 */
nora_canfd_status_t nora_canfd_clear_rx_overflow(nora_canfd_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CANFD_ISR_H */
