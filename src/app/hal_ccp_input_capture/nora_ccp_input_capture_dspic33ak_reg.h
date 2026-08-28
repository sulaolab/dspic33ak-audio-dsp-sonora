#ifndef NORA_CCP_INPUT_CAPTURE_DSPIC33AK_REG_H
#define NORA_CCP_INPUT_CAPTURE_DSPIC33AK_REG_H

// dsPIC33AK CCP Input Capture HAL register definitions.
//
// Contains only the SCCP/MCCP fields used by the Input Capture HAL. PPS and
// CLKGEN configuration are intentionally outside this module.
//
// Reference: dsPIC33AK512MPS512 Family Data Sheet DS70005591C,
// Section 27.3, CCP Register Summary and register descriptions.
//
// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#include <stdbool.h>
#include <stdint.h>

/* CCPxCON1 */
#define DSPIC33AK_CCP_CON1_ON          (1UL << 15)
#define DSPIC33AK_CCP_CON1_CCSEL       (1UL << 4)
#define DSPIC33AK_CCP_CON1_T32         (1UL << 5)

#define DSPIC33AK_CCP_CON1_MOD_MASK    (0xFUL << 0)
#define DSPIC33AK_CCP_CON1_MOD_POS     (0u)
#define DSPIC33AK_CCP_CON1_TMRPS_MASK  (0x3UL << 6)
#define DSPIC33AK_CCP_CON1_TMRPS_POS   (6u)
#define DSPIC33AK_CCP_CON1_CLKSEL_MASK (0x7UL << 8)
#define DSPIC33AK_CCP_CON1_CLKSEL_POS  (8u)
#define DSPIC33AK_CCP_CON1_SYNC_MASK   (0x1FUL << 16)
#define DSPIC33AK_CCP_CON1_SYNC_POS    (16u)
#define DSPIC33AK_CCP_CON1_TRIGEN      (1UL << 23)
#define DSPIC33AK_CCP_CON1_OPS_MASK    (0xFUL << 24)
#define DSPIC33AK_CCP_CON1_OPS_POS     (24u)

/* Input Capture free-running time base; CCPxCON1.TRIGEN remains clear. */
#define DSPIC33AK_CCP_SYNC_FREE_RUNNING (0x1FU)

/* CCPxCON2 */
#define DSPIC33AK_CCP_CON2_ICS_MASK    (0x7UL << 16)
#define DSPIC33AK_CCP_CON2_ICS_POS     (16u)

/* CCPxSTAT. DS70005591C Section 27.3.4 marks both fields R/W. */
#define DSPIC33AK_CCP_STAT_ICBNE       (1UL << 0)
#define DSPIC33AK_CCP_STAT_ICOV        (1UL << 1)

static inline void dspic33ak_ccp_reg_set(volatile uint32_t *reg,
                                         uint32_t mask)
{
    *reg |= mask;
}

static inline void dspic33ak_ccp_reg_clear(volatile uint32_t *reg,
                                           uint32_t mask)
{
    *reg &= ~mask;
}

static inline void dspic33ak_ccp_reg_set_or_clear(volatile uint32_t *reg,
                                                  uint32_t mask,
                                                  bool set)
{
    if (set)
    {
        *reg |= mask;
    }
    else
    {
        *reg &= ~mask;
    }
}

static inline void dspic33ak_ccp_reg_write_field(volatile uint32_t *reg,
                                                 uint32_t mask,
                                                 uint32_t position,
                                                 uint32_t value)
{
    *reg = (*reg & ~mask) | ((value << position) & mask);
}

#endif /* NORA_CCP_INPUT_CAPTURE_DSPIC33AK_REG_H */
