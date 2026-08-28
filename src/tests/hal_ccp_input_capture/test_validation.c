// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>

#include "nora_ccp_input_capture.h"
#include "nora_ccp_input_capture_dspic33ak_reg.h"

#define DEFINE_CCP(n)                    \
    volatile uint32_t CCP##n##CON1;      \
    volatile uint32_t CCP##n##CON2;      \
    volatile uint32_t CCP##n##STAT;      \
    volatile uint32_t CCP##n##PR;        \
    volatile uint32_t CCP##n##BUF

DEFINE_CCP(1);
DEFINE_CCP(2);
DEFINE_CCP(3);
DEFINE_CCP(4);
DEFINE_CCP(5);
DEFINE_CCP(6);
DEFINE_CCP(7);
DEFINE_CCP(8);
DEFINE_CCP(9);

/*
 * The IFSx / IECx storage the fake DFP declares. It is a union of the whole word
 * and a single-bit overlay, because the code under test reaches these registers
 * both ways -- see fake_xc/xc.h. `IFS1` and friends are macros for the `.word`
 * view, so the plain-word asserts below read exactly as they did before.
 */
fake_xc_irq_reg_t fake_xc_ifs1;
fake_xc_irq_reg_t fake_xc_ifs3;
fake_xc_irq_reg_t fake_xc_ifs4;
fake_xc_irq_reg_t fake_xc_iec1;
fake_xc_irq_reg_t fake_xc_iec3;
fake_xc_irq_reg_t fake_xc_iec4;
volatile uint32_t IPC6;
volatile uint32_t IPC7;
volatile uint32_t IPC15;
volatile uint32_t IPC16;

/*
 * The bit aliases the backend writes (`_CCP1IE = 1;`) are single-bit lvalues
 * overlaid on IECx / IFSx by fake_xc/xc.h. That overlay relies on
 * least-significant-bit-first bitfield allocation, which the C standard does not
 * promise -- and if it were reversed, every alias would land on the wrong bit
 * while the tests below still passed, because they assert through the same alias.
 * So check each alias against the mask the DFP documents for it, once, first.
 */
#define ASSERT_ALIAS(ie_reg, ie_bit, ie_mask, if_reg, if_bit, if_mask)     do                                                                     {                                                                          (ie_reg) = 0u;                                                         (ie_bit) = 1;                                                          assert((ie_reg) == (uint32_t)(ie_mask));                               (ie_bit) = 0;                                                          assert((ie_reg) == 0u);                                                (if_reg) = UINT32_MAX;                                                 (if_bit) = 0;                                                          assert((if_reg) == (uint32_t)~(uint32_t)(if_mask));                    (if_reg) = 0u;                                                     } while (0)

static void assert_bit_aliases_match_masks(void)
{
    ASSERT_ALIAS(IEC1, _CCP1IE, _IEC1_CCP1IE_MASK,
                 IFS1, _CCP1IF, _IFS1_CCP1IF_MASK);
    ASSERT_ALIAS(IEC1, _CCP2IE, _IEC1_CCP2IE_MASK,
                 IFS1, _CCP2IF, _IFS1_CCP2IF_MASK);
    ASSERT_ALIAS(IEC1, _CCP3IE, _IEC1_CCP3IE_MASK,
                 IFS1, _CCP3IF, _IFS1_CCP3IF_MASK);
    ASSERT_ALIAS(IEC1, _CCP4IE, _IEC1_CCP4IE_MASK,
                 IFS1, _CCP4IF, _IFS1_CCP4IF_MASK);
    ASSERT_ALIAS(IEC3, _CCP5IE, _IEC3_CCP5IE_MASK,
                 IFS3, _CCP5IF, _IFS3_CCP5IF_MASK);
    ASSERT_ALIAS(IEC4, _CCP6IE, _IEC4_CCP6IE_MASK,
                 IFS4, _CCP6IF, _IFS4_CCP6IF_MASK);
    ASSERT_ALIAS(IEC4, _CCP7IE, _IEC4_CCP7IE_MASK,
                 IFS4, _CCP7IF, _IFS4_CCP7IF_MASK);
    ASSERT_ALIAS(IEC4, _CCP8IE, _IEC4_CCP8IE_MASK,
                 IFS4, _CCP8IF, _IFS4_CCP8IF_MASK);
    ASSERT_ALIAS(IEC4, _CCP9IE, _IEC4_CCP9IE_MASK,
                 IFS4, _CCP9IF, _IFS4_CCP9IF_MASK);
}

static nora_ccp_icap_config_t valid_config(void)
{
    const nora_ccp_icap_config_t config =
    {
        NORA_CCP_SRC_PIN,
        NORA_CCP_EDGE_EVERY_RISING,
        NORA_CCP_CLK_PERIPHERAL,
        NORA_CCP_PS_4,
        true,
        NORA_CCP_IRQ_EVERY_EVENT,
        true,
        4u,
        100000000u
    };
    return config;
}

static void capture_callback(nora_ccp_inst_t inst,
                             uint32_t timestamp,
                             void *user)
{
    (void)inst;
    (void)timestamp;
    (void)user;
}

static void assert_invalid_without_write(nora_ccp_icap_config_t *config)
{
    CCP1CON1 = 0xA5A5A5A5u;
    CCP1CON2 = 0x5A5A5A5Au;
    CCP1PR = 0x12345678u;
    IEC1 = 0xABCDEF01u;
    assert(nora_ccp_icap_configure(NORA_CCP1, config) ==
           NORA_CCP_ERR_PARAM);
    assert(CCP1CON1 == 0xA5A5A5A5u);
    assert(CCP1CON2 == 0x5A5A5A5Au);
    assert(CCP1PR == 0x12345678u);
    assert(IEC1 == 0xABCDEF01u);
}

int main(void)
{
    nora_ccp_icap_config_t config = valid_config();

    assert_bit_aliases_match_masks();

    CCP1CON1 = 0u;
    CCP1CON2 = 0u;
    CCP1STAT = 0u;
    CCP1PR = 0u;
    IFS1 = UINT32_MAX;
    IEC1 = 0u;
    IPC6 = 0u;

    assert(nora_ccp_icap_configure(NORA_CCP1, &config) ==
           NORA_CCP_OK);
    assert((CCP1CON1 & DSPIC33AK_CCP_CON1_ON) == 0u);
    assert((CCP1CON1 & DSPIC33AK_CCP_CON1_CCSEL) != 0u);
    assert(((CCP1CON1 & DSPIC33AK_CCP_CON1_SYNC_MASK) >>
            DSPIC33AK_CCP_CON1_SYNC_POS) ==
           DSPIC33AK_CCP_SYNC_FREE_RUNNING);
    assert((CCP1CON1 & DSPIC33AK_CCP_CON1_TRIGEN) == 0u);
    assert(CCP1PR == UINT32_MAX);
    assert(nora_ccp_icap_timebase_hz(NORA_CCP1) == 25000000u);
    assert((IEC1 & _IEC1_CCP1IE_MASK) != 0u);

    assert(nora_ccp_icap_set_callback(NORA_CCP1,
                                            capture_callback,
                                            &config) == NORA_CCP_OK);
    assert(nora_ccp_icap_set_callback(NORA_CCP1,
                                            NULL,
                                            NULL) == NORA_CCP_OK);

    CCP1STAT |= DSPIC33AK_CCP_STAT_ICOV;
    assert(nora_ccp_icap_overflow(NORA_CCP1, false));
    assert((CCP1STAT & DSPIC33AK_CCP_STAT_ICOV) != 0u);
    assert(nora_ccp_icap_overflow(NORA_CCP1, true));
    assert((CCP1STAT & DSPIC33AK_CCP_STAT_ICOV) == 0u);

    assert(nora_ccp_icap_configure(NORA_CCP1, NULL) ==
           NORA_CCP_ERR_PARAM);

    config = valid_config();
    config.irq_priority = 0u;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.irq_priority = 8u;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.edge = (nora_ccp_edge_t)0x6;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.source = (nora_ccp_src_t)0x8;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.clock = (nora_ccp_clk_t)0x2;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.prescaler = (nora_ccp_prescaler_t)0x4;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.irq_ops = (nora_ccp_irq_ops_t)0x2;
    assert_invalid_without_write(&config);
    config = valid_config();
    config.timebase_src_hz = 0u;
    assert_invalid_without_write(&config);

    config = valid_config();
    assert(nora_ccp_icap_configure(
               (nora_ccp_inst_t)NORA_CCP_INST_COUNT,
               &config) == NORA_CCP_ERR_INSTANCE);
    assert(nora_ccp_icap_start(
               (nora_ccp_inst_t)NORA_CCP_INST_COUNT) ==
           NORA_CCP_ERR_INSTANCE);

    return 0;
}
