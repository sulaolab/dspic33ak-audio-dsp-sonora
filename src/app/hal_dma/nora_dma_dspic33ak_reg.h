/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_DMA_DSPIC33AK_REG_H
#define NORA_DMA_DSPIC33AK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal DMA register helper layer.
 *
 * Similar in spirit to nora_i2c_dspic33ak_reg.h: this file intentionally uses plain
 * 32-bit bit masks / bit positions and a few minimal generic helpers instead of
 * XC-DSC bitfield structures such as DMAxCHbits.  The goal is to keep
 * compiler/DFP-specific details away from the readable driver.
 *
 * This file is NOT an active register-access driver.  It deliberately does NOT
 * contain:
 *   - channel lookup functions / register pointer maps,
 *   - IRQ clear/enable/priority operations.
 * Those live as private helpers in nora_dma_dspic33ak.c or, for measured
 * direct-SFR paths, nora_dma_dspic33ak_fast.h.
 *
 * No SPI, audio, ping-pong, or DSP knowledge.  No owned state.
 *
 * Bit positions/masks were checked against:
 *   Microchip dsPIC33AK-MP_DFP 1.2.135  xc16/support/dsPIC33A/h/p33AK512MPS512.h
 *   Microchip dsPIC33AK-MC_DFP 1.4.172  xc16/support/dsPIC33A/h/p33AK128MC106.h
 * The DMAxCH / DMAxSEL / DMAxSTAT bit layout is identical across both devices
 * and across all channels.
 *
 * Keep this file small.  Add only the bits actually used by the readable driver.
 */

/* ---------------------------------------------------------------------------
 * NORA_DMA_IRQ_IE_WRITE - write one interrupt-enable bit without a read-modify-write
 *
 * IECx is shared: on AK512 IEC2<8:15> carries DMA0/1/2IE alongside the SPI3/SPI4
 * enables, and IEC3<24:31> carries DMA6/7IE alongside CNA..CND and CCP5; on AK128
 * IEC2<8:15> carries DMA0..DMA3IE alongside CMP1..CMP3.  So the *whole byte* must
 * never be rewritten - another peripheral's enable can change between the read and
 * the write-back, and that write-back would silently undo it.
 *
 * XC-DSC emits a single `bset.b` / `bclr.b` only when the register, the bit AND the
 * written value are all compile-time constants.  `alias = v` with a runtime `v`
 * is not that: it compiles to `mov.bz` + insert + `mov.b`, i.e. a byte-wide RMW.
 * Branching first and assigning a literal in each arm keeps both stores constant,
 * which is why this is an if/else and not a ternary or a variable.
 * ------------------------------------------------------------------------- */
#define NORA_DMA_IRQ_IE_WRITE(ie_alias, on) \
    do {                                    \
        if (on) {                           \
            (ie_alias) = 1;                 \
        } else {                            \
            (ie_alias) = 0;                 \
        }                                   \
    } while (0)

/* ---- DMACON (global control) ---- */
#define NORA_DMA_DSPIC33AK_CON_ON             (1UL << 15)   /* DMACONbits.ON       */
#define NORA_DMA_DSPIC33AK_BMX_INITPR_DMAPR   (1UL << 0)    /* BMXINITPRbits.DMAPR */

/* ---- DMA controller address window ---- */
#define NORA_DMA_DSPIC33AK_ADDR_WINDOW_LOW    (0x00000100UL) /* -> DMALOW  */
#define NORA_DMA_DSPIC33AK_ADDR_WINDOW_HIGH   (0x00FFFFFFUL) /* -> DMAHIGH */

/* ---- DMAxCH single-bit fields ---- */
#define NORA_DMA_DSPIC33AK_CH_CHEN         (1UL << 0)    /* DMAxCHbits.CHEN    */
#define NORA_DMA_DSPIC33AK_CH_HALFEN       (1UL << 1)    /* DMAxCHbits.HALFEN  */
#define NORA_DMA_DSPIC33AK_CH_DONEEN       (1UL << 3)    /* DMAxCHbits.DONEEN  */
#define NORA_DMA_DSPIC33AK_CH_CHREQ        (1UL << 4)    /* DMAxCHbits.CHREQ   */
#define NORA_DMA_DSPIC33AK_CH_RELOADS      (1UL << 24)   /* DMAxCHbits.RELOADS */
#define NORA_DMA_DSPIC33AK_CH_RELOADD      (1UL << 25)   /* DMAxCHbits.RELOADD */
#define NORA_DMA_DSPIC33AK_CH_RELOADC      (1UL << 26)   /* DMAxCHbits.RELOADC */

/* ---- DMAxCH multi-bit fields (position + mask) ---- */
#define NORA_DMA_DSPIC33AK_CH_SIZE_POS     (6)           /* DMAxCHbits.SIZE   */
#define NORA_DMA_DSPIC33AK_CH_SIZE_MASK    (0x3UL  << NORA_DMA_DSPIC33AK_CH_SIZE_POS)
#define NORA_DMA_DSPIC33AK_CH_TRMODE_POS   (10)          /* DMAxCHbits.TRMODE */
#define NORA_DMA_DSPIC33AK_CH_TRMODE_MASK  (0x3UL  << NORA_DMA_DSPIC33AK_CH_TRMODE_POS)
#define NORA_DMA_DSPIC33AK_CH_DAMODE_POS   (12)          /* DMAxCHbits.DAMODE */
#define NORA_DMA_DSPIC33AK_CH_DAMODE_MASK  (0x3UL  << NORA_DMA_DSPIC33AK_CH_DAMODE_POS)
#define NORA_DMA_DSPIC33AK_CH_SAMODE_POS   (14)          /* DMAxCHbits.SAMODE */
#define NORA_DMA_DSPIC33AK_CH_SAMODE_MASK  (0x3UL  << NORA_DMA_DSPIC33AK_CH_SAMODE_POS)

/* ---- DMAxCNT field ---- */
#define NORA_DMA_DSPIC33AK_CNT_MASK        (0x00FFFFFFUL) /* DMAxCNTbits.CNT (24 bits) */

/* ---- DMAxSEL field ---- */
#define NORA_DMA_DSPIC33AK_SEL_CHSEL_POS   (0)           /* DMAxSELbits.CHSEL */
#define NORA_DMA_DSPIC33AK_SEL_CHSEL_MASK  (0xFFUL << NORA_DMA_DSPIC33AK_SEL_CHSEL_POS)

/* ---- DMAxSTAT flags ---- */
#define NORA_DMA_DSPIC33AK_STAT_OVERRUN    (1UL << 3)    /* DMAxSTATbits.OVERRUN */
#define NORA_DMA_DSPIC33AK_STAT_HALF       (1UL << 4)    /* DMAxSTATbits.HALF    */
#define NORA_DMA_DSPIC33AK_STAT_DONE       (1UL << 5)    /* DMAxSTATbits.DONE    */

/* ---- Minimal generic 32-bit SFR access helpers ---- */
static inline void nora_dma_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void nora_dma_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline bool nora_dma_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

static inline void nora_dma_reg_write_field(volatile uint32_t *reg,
                                                 uint32_t mask,
                                                 uint32_t pos,
                                                 uint32_t value)
{
    *reg = (*reg & ~mask) | ((value << pos) & mask);
}

#endif /* NORA_DMA_DSPIC33AK_REG_H */
