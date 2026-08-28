/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_DMA_H
#define NORA_DMA_H

#include <stdint.h>
#include <stdbool.h>

/*
 * nora_dma.h
 * NORA DMA HAL public interface.
 *
 * Portability scope:
 *   This interface minimizes application changes between NORA-supported
 *   dsPIC33AK and dsPIC33CK ports. It is not a universal, arbitrary-processor
 *   DMA HAL: channel inventory, trigger-to-hardware mapping, address-window
 *   rules, and register behavior remain properties of the selected NORA port
 *   and backend.
 *
 * Clean low-level DMA abstraction currently used by SPI/I2S/TDM consumers.
 * Channel ownership is a consumer/project-config decision, not a HAL policy;
 * a system enabling multiple DMA users must assign non-overlapping channels.
 *
 * Design boundaries (intentional)
 * -------------------------------
 *  - Knows no SPI register layout or SPI/audio transfer policy; no PWM, DSP,
 *    printf, or application code.
 *  - Contains NO ping-pong / block-streaming policy.  Ping-pong audio policy
 *    belongs later under audio (e.g. tdm_audio_dma_stream), which configures a
 *    channel through this HAL and decides what to do with each half/buffer.
 *  - The caller owns the DMA buffers; this HAL only takes addresses.
 *  - No callback framework: the DMA ISRs stay in the consumer modules.  The
 *    current SPI/I2S/TDM consumer owns its interrupt handlers.
 *
 * Not generalized (no current user needs it): scatter-gather, linked
 * descriptors, match mode, peripheral-chained channels (PCHEN/PPEN), runtime
 * channel allocation, OS integration.
 */

/*
 * Logical NORA DMA channel identity.  A port backend validates and maps this
 * identity to the DMA channel inventory of its processor; code outside a
 * backend must not treat these values as SFR indexes.
 */
typedef enum {
    NORA_DMA_CHANNEL_0,
    NORA_DMA_CHANNEL_1,
    NORA_DMA_CHANNEL_2,
    NORA_DMA_CHANNEL_3,
    NORA_DMA_CHANNEL_4,
    NORA_DMA_CHANNEL_5,
    NORA_DMA_CHANNEL_6,
    NORA_DMA_CHANNEL_7,
} nora_dma_channel_t;

/*
 * DMA triggers currently needed by the SPI/I2S/TDM transport, plus the AK512
 * Classic-app PWM audio-DAC (PG5-8).  These are logical peripheral events,
 * not hardware trigger-select register values. The selected NORA port maps
 * them to its device-specific trigger representation.
 *
 * Enumerator set is per family: AK reaches SPI4 where CK reaches SPI3; the
 * PWM_GEN5-8 triggers are AK512-only (no PG5-8 on AK128, no dsPIC33CK port).
 *
 * NORA_DMA_TRIGGER_NONE is the software-only channel: no peripheral event may
 * fire it, and it is driven solely by nora_dma_software_trigger().  It is not a
 * "don't care" -- the select field always names something, so a channel with no
 * peripheral attached needs a positively quiet choice rather than a leftover
 * one.  Naming the intent here is what lets a software-triggered consumer move
 * between families, since the quiet code itself is per device.
 */
typedef enum {
    NORA_DMA_TRIGGER_NONE,
    NORA_DMA_TRIGGER_SPI1_RX,
    NORA_DMA_TRIGGER_SPI1_TX,
    NORA_DMA_TRIGGER_SPI2_RX,
    NORA_DMA_TRIGGER_SPI2_TX,
    NORA_DMA_TRIGGER_SPI3_RX,
    NORA_DMA_TRIGGER_SPI3_TX,
    NORA_DMA_TRIGGER_SPI4_RX,
    NORA_DMA_TRIGGER_SPI4_TX,
    NORA_DMA_TRIGGER_PWM_GEN5,
    NORA_DMA_TRIGGER_PWM_GEN6,
    NORA_DMA_TRIGGER_PWM_GEN7,
    NORA_DMA_TRIGGER_PWM_GEN8,
} nora_dma_trigger_t;

/* A raw, backend-owned DMA status snapshot.  Use the query functions below
 * rather than interpreting processor status bits in a consumer. */
typedef uint32_t nora_dma_status_t;

/* Transfer element width. */
typedef enum {
    NORA_DMA_SIZE_BYTE,           /* 1 byte    */
    NORA_DMA_SIZE_HALFWORD,       /* 16-bit    */
    NORA_DMA_SIZE_WORD,           /* 32-bit (used by current SPI/TDM consumer) */
} nora_dma_size_t;

/* Address behavior after each element. */
typedef enum {
    NORA_DMA_ADDR_FIXED,          /* unchanged         */
    NORA_DMA_ADDR_INCREMENT,      /* increment by SIZE */
    NORA_DMA_ADDR_DECREMENT,      /* decrement by SIZE */
} nora_dma_addr_mode_t;

/* Transfer/repeat mode. */
typedef enum {
    NORA_DMA_TRMODE_ONESHOT,           /* One-Shot                 */
    NORA_DMA_TRMODE_REPEAT_ONESHOT,    /* Repeated One-Shot (used) */
    NORA_DMA_TRMODE_CONTINUOUS,        /* Continuous               */
    NORA_DMA_TRMODE_REPEAT_CONTINUOUS, /* Repeated Continuous      */
} nora_dma_trmode_t;

/*
 * One channel's configuration.
 *
 * Mirrors exactly the configuration semantics the current code sets per channel; nothing
 * more.  RELOADS/RELOADD/RELOADC are explicit because the current RX/TX channels
 * use them asymmetrically (RX reloads dst, TX reloads src).
 */
typedef struct {
    volatile void            *src;
    volatile void            *dst;
    /* On the current dsPIC33AK backend, count is the number
     * of elements (of `size` width) to transfer per repeat -- it is NOT an
     * "elements - 1" register. Current users pass the element count of one
     * ping-pong half (ARRAY_SIZE() of that half-buffer). */
    uint32_t                  count;

    nora_dma_addr_mode_t src_mode;
    nora_dma_addr_mode_t dst_mode;
    nora_dma_size_t      size;
    nora_dma_trmode_t    tr_mode;

    bool                      reload_count;
    bool                      reload_src;
    bool                      reload_dst;

    bool                      half_int_en;
    bool                      done_int_en;

    nora_dma_trigger_t        trigger;      /* logical peripheral trigger */

    /* CPU interrupt control.
     * irq_priority is written only when irq_priority_set is true, so a caller
     * can intentionally keep its port reset/default priority. */
    bool                      irq_priority_set;
    uint8_t                   irq_priority; /* 0..7, used iff irq_priority_set */
    bool                      irq_enable;
} nora_dma_channel_cfg_t;

/* Pure DMA ping-pong timing mechanism (NOT policy):
 * maps a backend status snapshot to which buffer half just completed.
 * DONE takes precedence over HALF, matching the current RX handler behavior.
 */
typedef enum {
    NORA_DMA_HALF_NONE   = 0,   /* neither HALF nor DONE set             */
    NORA_DMA_HALF_FIRST  = 1,   /* HALF: first half just filled/emptied  */
    NORA_DMA_HALF_SECOND = 2,   /* DONE: second half just filled/emptied */
} nora_dma_half_t;

/*
 * Pure predicates over a status snapshot. All are side-effect-free and take the
 * word, not the channel, so a caller that already snapshotted can classify it
 * without touching hardware again.
 *
 * Each has a `_hot` static-inline twin in the backend's *_fast.h for ISR use; see
 * the fast header for the naming rule.
 *
 * Which question to ask depends on how the channel was armed, and the two
 * "completed" predicates are NOT interchangeable:
 *
 *   - ping-pong (a repeating transfer whose halves are consumed alternately):
 *     ask has_completed_half(), then half_from_status() for which half it was.
 *   - single-shot (one transfer, armed once, waited on until it finishes):
 *     ask has_completed(). has_completed_half() is already true at the MIDPOINT
 *     of such a transfer, so spinning on it releases the caller while the second
 *     half is still being written — and half_from_status() cannot express "the
 *     whole transfer" either, because a single-shot has no second half to name.
 */
bool nora_dma_status_has_half_done_conflict(nora_dma_status_t status);
bool nora_dma_status_has_overrun(nora_dma_status_t status);

/* A ping-pong half boundary was reached (HALF or DONE). Not the single-shot
 * question — see the note above. */
bool nora_dma_status_has_completed_half(nora_dma_status_t status);

/* The transfer as a whole completed (DONE). This is the single-shot question,
 * and it is what a self-test that arms one full-block transfer and spins must
 * wait on. */
bool nora_dma_status_has_completed(nora_dma_status_t status);

/* ---- Global ---- */

/* Configure DMA global state.
 * Turns the DMA controller on and programs the allowed DMA address window.
 * Safe to call more than once; the address window is written each time.
 * No printf / halt / application handling. */
void nora_dma_global_init(void);

/* Returns true if the controller is on and the address window matches the
 * configured values. Side-effect-free: no register writes, no printf, no halt. */
bool nora_dma_global_is_ready(void);

/* ---- Per channel ---- */

/*
 * Invalid-channel handling convention across this API (ch >= device channel
 * count):
 *   - config / enable          return false (and write nothing).
 *   - void IRQ/status helpers  silently ignore the call (no register write).
 *   - read helpers             return 0.
 */

/* Configure a channel (SRC/DST/CNT, CH fields, trigger, IRQ priority/enable).
 * Leaves the channel DISABLED. Call nora_dma_channel_enable(ch, true) to
 * start.
 * Returns false (and writes NO channel register) if cfg is NULL, the channel
 * index is invalid, the DMA controller is not ready (nora_dma_global_init()
 * must have been called first), or cfg holds an out-of-range enum / IRQ
 * priority. Returns true on success. Never calls nora_dma_global_init()
 * itself.
 * Re-config safe: masks the channel's CPU IRQ and clears stale status /
 * pending CPU interrupt flag before and after programming, so a stale interrupt or leftover
 * HALF/DONE status cannot disturb a stop -> re-config -> restart cycle. */
bool nora_dma_channel_config(nora_dma_channel_t ch, const nora_dma_channel_cfg_t *cfg);

/* Start/stop the channel.
 * enable==true: returns false (writes nothing) if the channel index is invalid
 * or the DMA controller is not ready; otherwise sets CHEN and returns true.
 * enable==false: always disables (safe direction) and returns true, except for
 * an invalid channel index which returns false. */
bool nora_dma_channel_enable(nora_dma_channel_t ch, bool enable);

/* Request one transfer from software: exactly the effect the selected peripheral
 * trigger would have if it fired once. The channel must already be configured and
 * enabled. This is the only way to exercise a channel with no peripheral attached,
 * which is what makes "is the DMA controller itself working?" answerable
 * independently of the peripheral's event wiring. No-op for an invalid channel.
 *
 * Portable by contract, not by local need: no application in this tree calls it
 * today, and it exists anyway because a consumer written for another NORA family
 * must be able to move here unchanged. */
void nora_dma_software_trigger(nora_dma_channel_t ch);

/* True while a software- or peripheral-requested transfer is still outstanding;
 * hardware clears the request when it is serviced. False for an invalid channel. */
bool nora_dma_request_pending(nora_dma_channel_t ch);

/* Elements still to move in the current transfer. Falling with no status flag set
 * is direct evidence the channel is being triggered and is moving data. Returns 0
 * for an invalid channel. The contract is uint32_t in every family; a backend whose
 * count register is narrower returns the widened value. */
uint32_t nora_dma_read_count(nora_dma_channel_t ch);

/* General IRQ control: set/clear the channel's CPU interrupt enable,
 * independently of CHEN.
 * Needed by the TDM soft-stop path, which masks the DMA IRQ before stopping the
 * channel so the ISR cannot run during teardown. */
void nora_dma_irq_enable(nora_dma_channel_t ch, bool enable);

/* Re-program a channel's CPU interrupt priority (0..7) after nora_dma_configure().
 * Task level only: IPCx is never written by hardware or by an ISR, so this is an
 * ordinary read-modify-write with a single writer. Programming a priority while
 * that channel's interrupt is enabled is the caller's decision -- an interrupt
 * already in flight keeps the level it was accepted at. */
void nora_dma_irq_set_priority(nora_dma_channel_t ch, uint8_t prio);

/* Read back a channel's CPU interrupt priority (0..7); 0 for an invalid channel.
 * The counterpart of nora_dma_irq_set_priority, so a caller that re-programs a
 * priority at run time can PROVE what the hardware holds instead of trusting that
 * its own write happened -- 0 also being the "interrupt disabled" encoding is
 * exactly why an unverified priority write is worth reporting. */
uint8_t nora_dma_irq_get_priority(nora_dma_channel_t ch);

/* General IRQ control: read the channel's CPU interrupt enable;
 * false for an invalid channel.
 * Lets a caller save/restore the IE state around a brief mask without hardcoding the
 * channel's SFR (used by the TDM core's per-instance RX-IE guard). */
bool nora_dma_irq_is_enabled(nora_dma_channel_t ch);

/* Save/mask and restore the CPU interrupt enable around a brief critical
 * section. The returned value from nora_dma_irq_disable_save() is for the
 * paired nora_dma_irq_restore() call. */
bool nora_dma_irq_disable_save(nora_dma_channel_t ch);
void nora_dma_irq_restore(nora_dma_channel_t ch, bool was_enabled);

/* Clear channel status flags. */
void nora_dma_clear_status(nora_dma_channel_t ch);

/* Clear the channel's CPU interrupt flag. */
void nora_dma_clear_irq_flag(nora_dma_channel_t ch);

/* Read raw channel status. Use nora_dma_half_from_status() to interpret it. */
nora_dma_status_t nora_dma_read_status(nora_dma_channel_t ch);

/* Read the active source address. The TX-side ping-pong consumer compares this
 * against its own half-buffer address; that comparison remains consumer policy. */
uint32_t nora_dma_read_src(nora_dma_channel_t ch);

/* Interpret a raw backend status value as a ping-pong half indicator (pure mechanism). */
nora_dma_half_t nora_dma_half_from_status(nora_dma_status_t status);

/* Ordered ISR snapshot sequence (NOT a single atomic instruction): clear the CPU
 * interrupt flag, snapshot status, then clear status. Returns a raw status
 * snapshot. Operation order is backend-defined and currently preserves:
 * clear IRQ flag, read status, clear status.
 *
 * Order note (verify against the device data sheet for the trigger/repeat modes
 * you use): clearing the CPU interrupt flag before reading+clearing status is intended so that
 * a HALF/DONE event occurring between the status read and clear remains
 * latched (and re-asserts the flag) rather than being silently lost. This
 * ordering has not been independently characterised against every DMA mode;
 * confirm the selected port's latching behaviour if you rely on it. */
nora_dma_status_t nora_dma_isr_snapshot(nora_dma_channel_t ch);

#endif /* NORA_DMA_H */
