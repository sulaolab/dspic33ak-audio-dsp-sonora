/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_DMA_DSPIC33AK_FAST_H
#define NORA_DMA_DSPIC33AK_FAST_H

/*
 * dsPIC33AK DMA hot-path helpers.
 *
 * This header intentionally exposes XC-DSC SFRs and is therefore never part
 * of the public NORA DMA contract.  Only the dsPIC33AK backend and backend-aware,
 * measured hot-path consumers include it, where a compile-time-constant channel
 * must fold to direct SFR accesses in an ISR or other measured hot path.
 *
 * NAMING RULE FOR EVERY NORA ISR FAST PATH
 *   A fast path is a `static inline` function in <module>_<backend>_fast.h, named
 *   <the portable function it shadows>_hot.  So nora_dma_read_src() is the
 *   out-of-line portable call and nora_dma_read_src_hot() is the inline one; the
 *   two do the same thing, and the out-of-line version in the backend .c is
 *   literally a call to the inline.
 *
 *   The `_hot` suffix is on the PORTABLE stem on purpose, not a <backend> tag in
 *   the middle.  An ISR body written against `_hot` names ports from AK to CK
 *   unchanged -- only the *_fast.h that supplies the inline differs, which is the
 *   same seam every other part of this HAL uses.  Naming it
 *   nora_dma_dspic33ak_read_src() instead (as this header used to) forced the ISR
 *   body itself to name the chip, so every ISR became a port site.
 *
 *   Backend-private helpers with NO portable twin keep <module>_<backend>_<name>:
 *   they are not a variant of anything, they are the only form, and the chip
 *   belongs in their name.
 */

#include <xc.h>

#include "nora_dma.h"
#include "nora_dma_dspic33ak_reg.h"

static inline bool nora_dma_irq_disable_save_hot(nora_dma_channel_t ch)
{
    bool was_enabled;

    switch (ch) {
    case NORA_DMA_CHANNEL_0: was_enabled = (_DMA0IE != 0u); _DMA0IE = 0u; break;
    case NORA_DMA_CHANNEL_1: was_enabled = (_DMA1IE != 0u); _DMA1IE = 0u; break;
    case NORA_DMA_CHANNEL_2: was_enabled = (_DMA2IE != 0u); _DMA2IE = 0u; break;
    case NORA_DMA_CHANNEL_3: was_enabled = (_DMA3IE != 0u); _DMA3IE = 0u; break;
    case NORA_DMA_CHANNEL_4: was_enabled = (_DMA4IE != 0u); _DMA4IE = 0u; break;
    case NORA_DMA_CHANNEL_5: was_enabled = (_DMA5IE != 0u); _DMA5IE = 0u; break;
#if defined(_DMA6IF)
    case NORA_DMA_CHANNEL_6: was_enabled = (_DMA6IE != 0u); _DMA6IE = 0u; break;
#endif
#if defined(_DMA7IF)
    case NORA_DMA_CHANNEL_7: was_enabled = (_DMA7IE != 0u); _DMA7IE = 0u; break;
#endif
    default: was_enabled = false; break;
    }
    return was_enabled;
}

/*
 * Restore one channel's interrupt enable to what disable_save_hot() reported.
 *
 * The disable half above writes the literal 0 and so is already a single
 * `bclr.b`.  This half writes a value the caller supplies, so it must not be
 * written as `_DMAxIE = value`: see NORA_DMA_IRQ_IE_WRITE in the register header
 * for why that becomes a read-modify-write of a byte shared with other
 * peripherals' enables.  The macro branches first so both stores stay literal.
 */
static inline void nora_dma_irq_restore_hot(nora_dma_channel_t ch,
                                                  bool was_enabled)
{
    switch (ch) {
    case NORA_DMA_CHANNEL_0: NORA_DMA_IRQ_IE_WRITE(_DMA0IE, was_enabled); break;
    case NORA_DMA_CHANNEL_1: NORA_DMA_IRQ_IE_WRITE(_DMA1IE, was_enabled); break;
    case NORA_DMA_CHANNEL_2: NORA_DMA_IRQ_IE_WRITE(_DMA2IE, was_enabled); break;
    case NORA_DMA_CHANNEL_3: NORA_DMA_IRQ_IE_WRITE(_DMA3IE, was_enabled); break;
    case NORA_DMA_CHANNEL_4: NORA_DMA_IRQ_IE_WRITE(_DMA4IE, was_enabled); break;
    case NORA_DMA_CHANNEL_5: NORA_DMA_IRQ_IE_WRITE(_DMA5IE, was_enabled); break;
#if defined(_DMA6IF)
    case NORA_DMA_CHANNEL_6: NORA_DMA_IRQ_IE_WRITE(_DMA6IE, was_enabled); break;
#endif
#if defined(_DMA7IF)
    case NORA_DMA_CHANNEL_7: NORA_DMA_IRQ_IE_WRITE(_DMA7IE, was_enabled); break;
#endif
    default: break;
    }
}

static inline uint32_t nora_dma_read_src_hot(nora_dma_channel_t ch)
{
    switch (ch) {
    case NORA_DMA_CHANNEL_0: return DMA0SRC;
    case NORA_DMA_CHANNEL_1: return DMA1SRC;
    case NORA_DMA_CHANNEL_2: return DMA2SRC;
    case NORA_DMA_CHANNEL_3: return DMA3SRC;
    case NORA_DMA_CHANNEL_4: return DMA4SRC;
    case NORA_DMA_CHANNEL_5: return DMA5SRC;
#if defined(_DMA6IF)
    case NORA_DMA_CHANNEL_6: return DMA6SRC;
#endif
#if defined(_DMA7IF)
    case NORA_DMA_CHANNEL_7: return DMA7SRC;
#endif
    default: return 0u;
    }
}

static inline nora_dma_status_t
nora_dma_isr_snapshot_hot(nora_dma_channel_t ch)
{
    nora_dma_status_t status;

    switch (ch) {
    case NORA_DMA_CHANNEL_0: _DMA0IF = 0u; status = DMA0STAT; DMA0STAT = 0u; break;
    case NORA_DMA_CHANNEL_1: _DMA1IF = 0u; status = DMA1STAT; DMA1STAT = 0u; break;
    case NORA_DMA_CHANNEL_2: _DMA2IF = 0u; status = DMA2STAT; DMA2STAT = 0u; break;
    case NORA_DMA_CHANNEL_3: _DMA3IF = 0u; status = DMA3STAT; DMA3STAT = 0u; break;
    case NORA_DMA_CHANNEL_4: _DMA4IF = 0u; status = DMA4STAT; DMA4STAT = 0u; break;
    case NORA_DMA_CHANNEL_5: _DMA5IF = 0u; status = DMA5STAT; DMA5STAT = 0u; break;
#if defined(_DMA6IF)
    case NORA_DMA_CHANNEL_6: _DMA6IF = 0u; status = DMA6STAT; DMA6STAT = 0u; break;
#endif
#if defined(_DMA7IF)
    case NORA_DMA_CHANNEL_7: _DMA7IF = 0u; status = DMA7STAT; DMA7STAT = 0u; break;
#endif
    default: status = 0u; break;
    }
    return status;
}

static inline bool
nora_dma_status_has_half_done_conflict_hot(nora_dma_status_t status)
{
    const uint32_t mask = NORA_DMA_DSPIC33AK_STAT_HALF |
                          NORA_DMA_DSPIC33AK_STAT_DONE;

    return ((status & mask) == mask);
}

static inline bool
nora_dma_status_has_overrun_hot(nora_dma_status_t status)
{
    return ((status & NORA_DMA_DSPIC33AK_STAT_OVERRUN) != 0u);
}

static inline bool
nora_dma_status_has_completed_half_hot(nora_dma_status_t status)
{
    const uint32_t mask = NORA_DMA_DSPIC33AK_STAT_HALF |
                          NORA_DMA_DSPIC33AK_STAT_DONE;

    return ((status & mask) != 0u);
}

/* DONE only: the transfer as a whole. Deliberately not the same question as
 * _has_completed_half_hot(), which is already true at the midpoint. */
static inline bool
nora_dma_status_has_completed_hot(nora_dma_status_t status)
{
    return ((status & NORA_DMA_DSPIC33AK_STAT_DONE) != 0u);
}

#endif /* NORA_DMA_DSPIC33AK_FAST_H */
