#ifndef NORA_CCP_INPUT_CAPTURE_DSPIC33AK_FAST_H
#define NORA_CCP_INPUT_CAPTURE_DSPIC33AK_FAST_H

/*
 * dsPIC33AK CCP Input Capture hot-path helpers.
 *
 * This header intentionally exposes XC-DSC SFRs and is therefore never part of
 * the public NORA CCP contract. Only the dsPIC33AK backend and backend-aware,
 * measured hot-path consumers include it -- an application that owns the CCP
 * vector because it wants its own per-sample handling instead of the HAL's
 * callback indirection.
 *
 * NAMING RULE: <the portable function it shadows>_hot, `static inline`, in
 * <module>_<backend>_fast.h. See nora_dma_dspic33ak_fast.h for the full statement
 * of the rule and why the suffix goes on the portable stem rather than a
 * <backend> tag in the middle. Backend-private helpers with no portable twin --
 * nora_ccp_dspic33ak_hot_regs() below -- keep the chip in their name.
 *
 * WHY THIS FILE EXISTS AT ALL
 *   apps/asrc/asrc_clock_control.c hand-writes _CCP1Interrupt/_CCP2Interrupt and
 *   drained the FIFO itself for speed. With no fast path to call it reached for
 *   CCP1STAT / CCP1BUF / IFS1 directly, which made it the ONE application file in
 *   the tree that included <xc.h> and a backend register header -- a port site
 *   that had nothing to do with the application's logic.
 *
 * SFR ACCESS FOLDS ONLY FOR A CONSTANT INSTANCE
 *   nora_ccp_dspic33ak_hot_regs() returns a small struct from a switch. When `inst`
 *   is a literal (which is the case in an ISR bound to one vector) the whole thing
 *   constant-folds to direct SFR accesses. Called with a runtime instance it does
 *   not fold and is worse than the table-driven portable call -- so don't.
 *
 * SPDX-FileCopyrightText: 2026 SulaoLab
 * SPDX-License-Identifier: MIT-0
 */

#include <xc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nora_ccp_input_capture.h"
#include "nora_ccp_input_capture_dspic33ak_reg.h"

/*
 * The register map below requires the complete CCP1..CCP9 inventory and the CCP9
 * IRQ mapping. Use the DFP's capability macros rather than a compiler part-name
 * test: a device without that inventory gets the unsupported build, while the
 * public NORA header stays device-neutral. The backend .c consumes this macro from
 * here so the condition is written once.
 */
#if defined(CCP9CON1) && defined(_IFS4_CCP9IF_MASK) && \
    defined(_IEC4_CCP9IE_MASK) && defined(_IPC16_CCP9IP_MASK) && \
    defined(_IPC16_CCP9IP_POSITION)
#define NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP (1)
#else
#define NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP (0)
#endif

#if NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP

/*
 * Only the two registers a drain loop touches. Not the configure-time set, and
 * NOT the CPU interrupt flag: IFSx is shared by every peripheral, so it is
 * written through the DFP bit alias in nora_ccp_icap_irq_clear_hot() below rather
 * than through a pointer plus a runtime mask.
 */
typedef struct
{
    volatile uint32_t *stat;
    volatile uint32_t *buf;
} nora_ccp_dspic33ak_hot_regs_t;

static inline nora_ccp_dspic33ak_hot_regs_t
nora_ccp_dspic33ak_hot_regs(nora_ccp_inst_t inst)
{
    nora_ccp_dspic33ak_hot_regs_t r = { NULL, NULL };

    switch (inst)
    {
    case NORA_CCP1: r.stat = &CCP1STAT; r.buf = &CCP1BUF; break;
    case NORA_CCP2: r.stat = &CCP2STAT; r.buf = &CCP2BUF; break;
    case NORA_CCP3: r.stat = &CCP3STAT; r.buf = &CCP3BUF; break;
    case NORA_CCP4: r.stat = &CCP4STAT; r.buf = &CCP4BUF; break;
    case NORA_CCP5: r.stat = &CCP5STAT; r.buf = &CCP5BUF; break;
    case NORA_CCP6: r.stat = &CCP6STAT; r.buf = &CCP6BUF; break;
    case NORA_CCP7: r.stat = &CCP7STAT; r.buf = &CCP7BUF; break;
    case NORA_CCP8: r.stat = &CCP8STAT; r.buf = &CCP8BUF; break;
    case NORA_CCP9: r.stat = &CCP9STAT; r.buf = &CCP9BUF; break;
    default:
        break;
    }
    return r;
}

/*
 * Pop one raw timestamp if the capture FIFO has one. Same contract as
 * nora_ccp_icap_read() with one deliberate difference: `timestamp` is NOT checked
 * for NULL. An ISR passes the address of its own local, so the check can never
 * fire there and only costs a branch per FIFO entry.
 */
static inline bool nora_ccp_icap_read_hot(nora_ccp_inst_t inst,
                                          uint32_t *timestamp)
{
    const nora_ccp_dspic33ak_hot_regs_t r = nora_ccp_dspic33ak_hot_regs(inst);

    if ((r.stat == NULL) ||
        ((*r.stat & DSPIC33AK_CCP_STAT_ICBNE) == 0u))
    {
        return false;
    }

    *timestamp = *r.buf;
    return true;
}

/*
 * Clear this instance's CPU interrupt flag. Twin of nora_ccp_icap_irq_clear().
 *
 * Writes the DFP bit alias with a literal 0, so register, bit and value are all
 * compile-time constant and the store is a single bclr.b. The previous shape
 * (`*r.ifs &= ~r.if_mask` out of the register struct) folded to the same one
 * instruction for a literal `inst`, which is the only supported use of this
 * header -- but it is a 32-bit read-modify-write of a register shared with every
 * other peripheral the moment the fold does not happen. The alias cannot degrade
 * that way, and it also drops IFS1/IFS3/IFS4 bank knowledge from this file.
 */
static inline void nora_ccp_icap_irq_clear_hot(nora_ccp_inst_t inst)
{
    switch (inst)
    {
    case NORA_CCP1: _CCP1IF = 0; break;
    case NORA_CCP2: _CCP2IF = 0; break;
    case NORA_CCP3: _CCP3IF = 0; break;
    case NORA_CCP4: _CCP4IF = 0; break;
    case NORA_CCP5: _CCP5IF = 0; break;
    case NORA_CCP6: _CCP6IF = 0; break;
    case NORA_CCP7: _CCP7IF = 0; break;
    case NORA_CCP8: _CCP8IF = 0; break;
    case NORA_CCP9: _CCP9IF = 0; break;
    default:
        break;
    }
}

#endif /* NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP */

#endif /* NORA_CCP_INPUT_CAPTURE_DSPIC33AK_FAST_H */
