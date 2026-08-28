// dsPIC33AK CCP Input Capture HAL
//
// Configures SCCP/MCCP instances for Input Capture and returns raw edge
// timestamps through polling or an ISR callback. Period, frequency, duty ratio,
// and sample-rate ratio calculations belong to the application. PPS and CLKGEN
// routing also belong to the integrator.
//
// Register and interrupt facts: dsPIC33AK512MPS512 Family Data Sheet
// DS70005591C, Sections 11.3 and 27.3. The corresponding XC-DSC DFP declares
// the accessed SFRs as volatile uint32_t, matching the access width below.
//
// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#include <xc.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nora_ccp_input_capture.h"
#include "nora_ccp_input_capture_dspic33ak_reg.h"
/* Also the single definition point for NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP, so the
 * DFP capability condition is not written twice. */
#include "nora_ccp_input_capture_dspic33ak_fast.h"

#ifndef NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#define NORA_CCP_DSPIC33AK_DEFINE_VECTORS (0)
#endif

#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP1_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP1_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP2_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP2_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP3_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP3_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP4_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP4_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP5_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP5_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP6_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP6_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP7_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP7_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP8_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP8_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif
#ifndef NORA_CCP_DSPIC33AK_DEFINE_CCP9_VECTOR
#define NORA_CCP_DSPIC33AK_DEFINE_CCP9_VECTOR NORA_CCP_DSPIC33AK_DEFINE_VECTORS
#endif

/* NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP comes from the fast header included above:
 * it needs the same DFP capability test, so the condition lives in one place. */

static bool ccp_inst_in_enum_range(nora_ccp_inst_t inst)
{
    return (unsigned int)inst < (unsigned int)NORA_CCP_INST_COUNT;
}

static bool ccp_inst_present_on_device(nora_ccp_inst_t inst)
{
#if NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP
    return ccp_inst_in_enum_range(inst);
#else
    (void)inst;
    return false;
#endif
}

#if NORA_CCP_DSPIC33AK_HAS_FULL_CCP_MAP

typedef struct
{
    volatile uint32_t *con1;
    volatile uint32_t *con2;
    volatile uint32_t *stat;
    volatile uint32_t *pr;
    volatile uint32_t *buf;
    /* No ifs/if_mask/iec/ie_mask here.  IFSx/IECx are shared by every peripheral,
     * so `*iec |= ie_mask` is a read-modify-write that can undo a bit another
     * interrupt changed in between; the flag/enable bits are written through the
     * DFP bit aliases in ccp_irq_clear_flag() / ccp_irq_set_enable() below, which
     * also keeps the IFS1/IFS3/IFS4 bank split out of this table.  IPCx below is
     * a different case: a 4-bit priority field hardware never writes, programmed
     * only at configure time, so there is no concurrent writer to race with. */
    volatile uint32_t *ipc;
    uint32_t ip_mask;
    uint8_t ip_pos;
} ccp_dev_t;

static const ccp_dev_t s_ccp_dev[NORA_CCP_INST_COUNT] =
{
    [NORA_CCP1] = {&CCP1CON1, &CCP1CON2, &CCP1STAT, &CCP1PR, &CCP1BUF,
                         &IPC6, _IPC6_CCP1IP_MASK, _IPC6_CCP1IP_POSITION},
    [NORA_CCP2] = {&CCP2CON1, &CCP2CON2, &CCP2STAT, &CCP2PR, &CCP2BUF,
                         &IPC6, _IPC6_CCP2IP_MASK, _IPC6_CCP2IP_POSITION},
    [NORA_CCP3] = {&CCP3CON1, &CCP3CON2, &CCP3STAT, &CCP3PR, &CCP3BUF,
                         &IPC7, _IPC7_CCP3IP_MASK, _IPC7_CCP3IP_POSITION},
    [NORA_CCP4] = {&CCP4CON1, &CCP4CON2, &CCP4STAT, &CCP4PR, &CCP4BUF,
                         &IPC7, _IPC7_CCP4IP_MASK, _IPC7_CCP4IP_POSITION},
    [NORA_CCP5] = {&CCP5CON1, &CCP5CON2, &CCP5STAT, &CCP5PR, &CCP5BUF,
                         &IPC15, _IPC15_CCP5IP_MASK, _IPC15_CCP5IP_POSITION},
    [NORA_CCP6] = {&CCP6CON1, &CCP6CON2, &CCP6STAT, &CCP6PR, &CCP6BUF,
                         &IPC16, _IPC16_CCP6IP_MASK, _IPC16_CCP6IP_POSITION},
    [NORA_CCP7] = {&CCP7CON1, &CCP7CON2, &CCP7STAT, &CCP7PR, &CCP7BUF,
                         &IPC16, _IPC16_CCP7IP_MASK, _IPC16_CCP7IP_POSITION},
    [NORA_CCP8] = {&CCP8CON1, &CCP8CON2, &CCP8STAT, &CCP8PR, &CCP8BUF,
                         &IPC16, _IPC16_CCP8IP_MASK, _IPC16_CCP8IP_POSITION},
    [NORA_CCP9] = {&CCP9CON1, &CCP9CON2, &CCP9STAT, &CCP9PR, &CCP9BUF,
                         &IPC16, _IPC16_CCP9IP_MASK, _IPC16_CCP9IP_POSITION}
};

static nora_ccp_capture_cb_t s_callback[NORA_CCP_INST_COUNT];
static void *s_callback_user[NORA_CCP_INST_COUNT];
static uint32_t s_configured_timebase_hz[NORA_CCP_INST_COUNT];

static bool ccp_edge_supported(nora_ccp_edge_t edge)
{
    return edge == NORA_CCP_EDGE_EVERY_RISING ||
           edge == NORA_CCP_EDGE_EVERY_FALLING ||
           edge == NORA_CCP_EDGE_EVERY_EDGE ||
           edge == NORA_CCP_EDGE_EVERY_4TH_RISING ||
           edge == NORA_CCP_EDGE_EVERY_16TH_RISING;
}

static bool ccp_source_supported(nora_ccp_src_t source)
{
    return (unsigned int)source <= (unsigned int)NORA_CCP_SRC_CLC4;
}

static bool ccp_clock_supported(nora_ccp_clk_t clock)
{
    return clock == NORA_CCP_CLK_PERIPHERAL ||
           clock == NORA_CCP_CLK_CLKGEN13 ||
           clock == NORA_CCP_CLK_EXT_TCKI;
}

static bool ccp_prescaler_supported(nora_ccp_prescaler_t prescaler)
{
    return prescaler == NORA_CCP_PS_1 ||
           prescaler == NORA_CCP_PS_4 ||
           prescaler == NORA_CCP_PS_16 ||
           prescaler == NORA_CCP_PS_64;
}

static bool ccp_irq_ops_supported(nora_ccp_irq_ops_t irq_ops)
{
    return irq_ops == NORA_CCP_IRQ_EVERY_EVENT ||
           irq_ops == NORA_CCP_IRQ_EVERY_2ND ||
           irq_ops == NORA_CCP_IRQ_EVERY_4TH ||
           irq_ops == NORA_CCP_IRQ_EVERY_8TH ||
           irq_ops == NORA_CCP_IRQ_EVERY_16TH;
}

static bool ccp_config_valid(const nora_ccp_icap_config_t *config)
{
    if (config == NULL ||
        !ccp_edge_supported(config->edge) ||
        !ccp_source_supported(config->source) ||
        !ccp_clock_supported(config->clock) ||
        !ccp_prescaler_supported(config->prescaler) ||
        !ccp_irq_ops_supported(config->irq_ops) ||
        config->irq_priority > 7u ||
        (config->irq_enable && config->irq_priority == 0u) ||
        config->timebase_src_hz == 0u)
    {
        return false;
    }

    return true;
}

static uint32_t ccp_prescaler_divisor(nora_ccp_prescaler_t prescaler)
{
    switch (prescaler)
    {
        case NORA_CCP_PS_4:
            return 4u;
        case NORA_CCP_PS_16:
            return 16u;
        case NORA_CCP_PS_64:
            return 64u;
        case NORA_CCP_PS_1:
        default:
            return 1u;
    }
}

/*
 * Interrupt flag / enable for one instance, through the DFP bit aliases.
 *
 * Each store is a literal into a named bit, so register, bit and value are all
 * compile-time constant per arm and XC-DSC emits one bset.b / bclr.b on a
 * register it shares with every other peripheral.  ccp_irq_set_enable() is an
 * if/else rather than `_CCPnIE = enable` for the same reason: assigning a runtime
 * value to a bit alias is a byte-wide read-modify-write.
 *
 * The flag clear delegates to nora_ccp_icap_irq_clear_hot() in the fast header -
 * it is the same switch, and having one copy keeps the hot and portable paths
 * from drifting.
 */
static void ccp_irq_set_enable(nora_ccp_inst_t inst, bool enable)
{
    switch (inst)
    {
    case NORA_CCP1:
        if (enable) { _CCP1IE = 1; } else { _CCP1IE = 0; }
        break;
    case NORA_CCP2:
        if (enable) { _CCP2IE = 1; } else { _CCP2IE = 0; }
        break;
    case NORA_CCP3:
        if (enable) { _CCP3IE = 1; } else { _CCP3IE = 0; }
        break;
    case NORA_CCP4:
        if (enable) { _CCP4IE = 1; } else { _CCP4IE = 0; }
        break;
    case NORA_CCP5:
        if (enable) { _CCP5IE = 1; } else { _CCP5IE = 0; }
        break;
    case NORA_CCP6:
        if (enable) { _CCP6IE = 1; } else { _CCP6IE = 0; }
        break;
    case NORA_CCP7:
        if (enable) { _CCP7IE = 1; } else { _CCP7IE = 0; }
        break;
    case NORA_CCP8:
        if (enable) { _CCP8IE = 1; } else { _CCP8IE = 0; }
        break;
    case NORA_CCP9:
        if (enable) { _CCP9IE = 1; } else { _CCP9IE = 0; }
        break;
    default:
        break;
    }
}

static void ccp_icap_drain_fifo(const ccp_dev_t *device,
                                nora_ccp_inst_t inst,
                                bool dispatch_callbacks)
{
    while ((*device->stat & DSPIC33AK_CCP_STAT_ICBNE) != 0u)
    {
        const uint32_t timestamp = *device->buf;
        if (dispatch_callbacks && s_callback[inst] != NULL)
        {
            s_callback[inst](inst, timestamp, s_callback_user[inst]);
        }
    }
}

nora_ccp_status_t nora_ccp_icap_configure(
    nora_ccp_inst_t inst,
    const nora_ccp_icap_config_t *config)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return NORA_CCP_ERR_INSTANCE;
    }
    if (!ccp_config_valid(config))
    {
        return NORA_CCP_ERR_PARAM;
    }

    const ccp_dev_t *device = &s_ccp_dev[inst];

    /* Stop interrupt delivery and the module before touching capture state. */
    ccp_irq_set_enable(inst, false);
    dspic33ak_ccp_reg_clear(device->con1, DSPIC33AK_CCP_CON1_ON);
    ccp_icap_drain_fifo(device, inst, false);
    dspic33ak_ccp_reg_clear(device->stat, DSPIC33AK_CCP_STAT_ICOV);
    nora_ccp_icap_irq_clear_hot(inst);

    *device->con1 = 0u;
    *device->con2 = 0u;

    dspic33ak_ccp_reg_set(device->con1, DSPIC33AK_CCP_CON1_CCSEL);
    dspic33ak_ccp_reg_set_or_clear(device->con1,
                                   DSPIC33AK_CCP_CON1_T32,
                                   config->use_32bit);
    dspic33ak_ccp_reg_write_field(device->con1,
                                  DSPIC33AK_CCP_CON1_MOD_MASK,
                                  DSPIC33AK_CCP_CON1_MOD_POS,
                                  (uint32_t)config->edge);
    dspic33ak_ccp_reg_write_field(device->con1,
                                  DSPIC33AK_CCP_CON1_TMRPS_MASK,
                                  DSPIC33AK_CCP_CON1_TMRPS_POS,
                                  (uint32_t)config->prescaler);
    dspic33ak_ccp_reg_write_field(device->con1,
                                  DSPIC33AK_CCP_CON1_CLKSEL_MASK,
                                  DSPIC33AK_CCP_CON1_CLKSEL_POS,
                                  (uint32_t)config->clock);
    dspic33ak_ccp_reg_write_field(device->con1,
                                  DSPIC33AK_CCP_CON1_SYNC_MASK,
                                  DSPIC33AK_CCP_CON1_SYNC_POS,
                                  DSPIC33AK_CCP_SYNC_FREE_RUNNING);
    dspic33ak_ccp_reg_write_field(device->con1,
                                  DSPIC33AK_CCP_CON1_OPS_MASK,
                                  DSPIC33AK_CCP_CON1_OPS_POS,
                                  (uint32_t)config->irq_ops);
    dspic33ak_ccp_reg_write_field(device->con2,
                                  DSPIC33AK_CCP_CON2_ICS_MASK,
                                  DSPIC33AK_CCP_CON2_ICS_POS,
                                  (uint32_t)config->source);
    *device->pr = UINT32_MAX;

    *device->ipc = (*device->ipc & ~device->ip_mask) |
                   (((uint32_t)config->irq_priority << device->ip_pos) &
                    device->ip_mask);

    s_configured_timebase_hz[inst] =
        config->timebase_src_hz / ccp_prescaler_divisor(config->prescaler);

    if (config->irq_enable)
    {
        ccp_irq_set_enable(inst, true);
    }

    return NORA_CCP_OK;
}

nora_ccp_status_t nora_ccp_icap_set_callback(
    nora_ccp_inst_t inst,
    nora_ccp_capture_cb_t callback,
    void *user)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return NORA_CCP_ERR_INSTANCE;
    }

    s_callback[inst] = callback;
    s_callback_user[inst] = user;
    return NORA_CCP_OK;
}

nora_ccp_status_t nora_ccp_icap_start(nora_ccp_inst_t inst)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return NORA_CCP_ERR_INSTANCE;
    }

    const ccp_dev_t *device = &s_ccp_dev[inst];
    ccp_icap_drain_fifo(device, inst, false);
    dspic33ak_ccp_reg_clear(device->stat, DSPIC33AK_CCP_STAT_ICOV);
    nora_ccp_icap_irq_clear_hot(inst);
    dspic33ak_ccp_reg_set(device->con1, DSPIC33AK_CCP_CON1_ON);
    return NORA_CCP_OK;
}

nora_ccp_status_t nora_ccp_icap_stop(nora_ccp_inst_t inst)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return NORA_CCP_ERR_INSTANCE;
    }

    dspic33ak_ccp_reg_clear(s_ccp_dev[inst].con1, DSPIC33AK_CCP_CON1_ON);
    return NORA_CCP_OK;
}

bool nora_ccp_icap_read(nora_ccp_inst_t inst, uint32_t *timestamp)
{
    if (!ccp_inst_in_enum_range(inst) ||
        !ccp_inst_present_on_device(inst) ||
        timestamp == NULL)
    {
        return false;
    }

    const ccp_dev_t *device = &s_ccp_dev[inst];
    if ((*device->stat & DSPIC33AK_CCP_STAT_ICBNE) == 0u)
    {
        return false;
    }

    *timestamp = *device->buf;
    return true;
}

bool nora_ccp_icap_overflow(nora_ccp_inst_t inst, bool clear)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return false;
    }

    const ccp_dev_t *device = &s_ccp_dev[inst];
    const bool overflowed =
        (*device->stat & DSPIC33AK_CCP_STAT_ICOV) != 0u;
    if (clear && overflowed)
    {
        dspic33ak_ccp_reg_clear(device->stat, DSPIC33AK_CCP_STAT_ICOV);
    }
    return overflowed;
}

uint32_t nora_ccp_icap_timebase_hz(nora_ccp_inst_t inst)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return 0u;
    }
    return s_configured_timebase_hz[inst];
}

void nora_ccp_icap_isr(nora_ccp_inst_t inst)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return;
    }

    const ccp_dev_t *device = &s_ccp_dev[inst];
    ccp_icap_drain_fifo(device, inst, true);
    nora_ccp_icap_irq_clear_hot(inst);
}

void nora_ccp_icap_irq_clear(nora_ccp_inst_t inst)
{
    if (!ccp_inst_in_enum_range(inst) || !ccp_inst_present_on_device(inst))
    {
        return;
    }

    nora_ccp_icap_irq_clear_hot(inst);
}

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
 * note above _CCP1Interrupt in src/app/apps/asrc/asrc_clock_control.c, and the
 * per-vector noinline bodies in nora_spi_i2s_tdm_dspic33ak.c for the case where the
 * pushes are W8+ and `context` cannot remove them.
 *
 * NOT for trap handlers: a trap runs in whatever register context the CPU was already
 * in, so it cannot rely on a bank of its own.
 */
#if NORA_CCP_DSPIC33AK_DEFINE_CCP1_VECTOR
void __attribute__((interrupt, context)) _CCP1Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP1);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP2_VECTOR
void __attribute__((interrupt, context)) _CCP2Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP2);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP3_VECTOR
void __attribute__((interrupt, context)) _CCP3Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP3);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP4_VECTOR
void __attribute__((interrupt, context)) _CCP4Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP4);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP5_VECTOR
void __attribute__((interrupt, context)) _CCP5Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP5);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP6_VECTOR
void __attribute__((interrupt, context)) _CCP6Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP6);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP7_VECTOR
void __attribute__((interrupt, context)) _CCP7Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP7);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP8_VECTOR
void __attribute__((interrupt, context)) _CCP8Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP8);
}
#endif
#if NORA_CCP_DSPIC33AK_DEFINE_CCP9_VECTOR
void __attribute__((interrupt, context)) _CCP9Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP9);
}
#endif

#else

nora_ccp_status_t nora_ccp_icap_configure(
    nora_ccp_inst_t inst,
    const nora_ccp_icap_config_t *config)
{
    (void)config;
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return NORA_CCP_ERR_INSTANCE;
}

nora_ccp_status_t nora_ccp_icap_set_callback(
    nora_ccp_inst_t inst,
    nora_ccp_capture_cb_t callback,
    void *user)
{
    (void)callback;
    (void)user;
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return NORA_CCP_ERR_INSTANCE;
}

nora_ccp_status_t nora_ccp_icap_start(nora_ccp_inst_t inst)
{
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return NORA_CCP_ERR_INSTANCE;
}

nora_ccp_status_t nora_ccp_icap_stop(nora_ccp_inst_t inst)
{
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return NORA_CCP_ERR_INSTANCE;
}

bool nora_ccp_icap_read(nora_ccp_inst_t inst, uint32_t *timestamp)
{
    (void)timestamp;
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return false;
}

bool nora_ccp_icap_overflow(nora_ccp_inst_t inst, bool clear)
{
    (void)clear;
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return false;
}

uint32_t nora_ccp_icap_timebase_hz(nora_ccp_inst_t inst)
{
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
    return 0u;
}

void nora_ccp_icap_isr(nora_ccp_inst_t inst)
{
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
}

void nora_ccp_icap_irq_clear(nora_ccp_inst_t inst)
{
    (void)ccp_inst_in_enum_range(inst);
    (void)ccp_inst_present_on_device(inst);
}

#endif
