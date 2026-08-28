/**
 * @file    nora_canfd_device_dspic33ak.c
 * @brief   dsPIC33AK CAN FD HAL - instance -> register map (the only file that
 *          names raw C1.../C2... SFR symbols). Entries compile in only when the
 *          part defines the registers.
 */
#include <xc.h>
#include "nora_canfd_device.h"

/* Cast each SFR lvalue to a 32-bit register pointer. The DFP exposes the
 * plain register name (e.g. C1CON) as a volatile uint32_t lvalue. */
#define CANREG(name) ((volatile uint32_t *)&(name))

static const nora_canfd_device_t g_canfd_devices[NORA_CANFD_INST_COUNT] = {
#if defined(C1CON)
    [NORA_CANFD_INST_1] = {
        .present = true,
        .regs = {
            .CON      = CANREG(C1CON),
            .NBTCFG   = CANREG(C1NBTCFG),
            .DBTCFG   = CANREG(C1DBTCFG),
            .TDC      = CANREG(C1TDC),
            .FIFOBA   = CANREG(C1FIFOBA),
            .TXQCON   = CANREG(C1TXQCON),
            .TXQSTA   = CANREG(C1TXQSTA),
            .TXQUA    = CANREG(C1TXQUA),
            .FIFOCON1 = CANREG(C1FIFOCON1),
            .FIFOSTA1 = CANREG(C1FIFOSTA1),
            .FIFOUA1  = CANREG(C1FIFOUA1),
            .FLTCON0  = CANREG(C1FLTCON0),
            .FLTOBJ0  = CANREG(C1FLTOBJ0),
            .MASK0    = CANREG(C1MASK0),
            .INT      = CANREG(C1INT),
            .TREC     = CANREG(C1TREC),
        },
    },
#else
    [NORA_CANFD_INST_1] = { .present = false },
#endif

#if defined(C2CON)
    [NORA_CANFD_INST_2] = {
        .present = true,
        .regs = {
            .CON      = CANREG(C2CON),
            .NBTCFG   = CANREG(C2NBTCFG),
            .DBTCFG   = CANREG(C2DBTCFG),
            .TDC      = CANREG(C2TDC),
            .FIFOBA   = CANREG(C2FIFOBA),
            .TXQCON   = CANREG(C2TXQCON),
            .TXQSTA   = CANREG(C2TXQSTA),
            .TXQUA    = CANREG(C2TXQUA),
            .FIFOCON1 = CANREG(C2FIFOCON1),
            .FIFOSTA1 = CANREG(C2FIFOSTA1),
            .FIFOUA1  = CANREG(C2FIFOUA1),
            .FLTCON0  = CANREG(C2FLTCON0),
            .FLTOBJ0  = CANREG(C2FLTOBJ0),
            .MASK0    = CANREG(C2MASK0),
            .INT      = CANREG(C2INT),
            .TREC     = CANREG(C2TREC),
        },
    },
#else
    [NORA_CANFD_INST_2] = { .present = false },
#endif
};

const nora_canfd_device_t *nora_canfd_get_device(nora_canfd_instance_t inst)
{
    if ((unsigned)inst >= (unsigned)NORA_CANFD_INST_COUNT) {
        return NULL;
    }
    if (!g_canfd_devices[inst].present) {
        return NULL;
    }
    return &g_canfd_devices[inst];
}

bool nora_canfd_instance_is_present(nora_canfd_instance_t inst)
{
    return nora_canfd_get_device(inst) != NULL;
}

nora_canfd_status_t nora_canfd_module_enable(
    nora_canfd_instance_t inst,
    bool enable)
{
    if ((unsigned)inst >= (unsigned)NORA_CANFD_INST_COUNT) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    if (nora_canfd_get_device(inst) == NULL) {
        return NORA_CANFD_ERR_NOT_PRESENT;
    }

    switch (inst) {
    case NORA_CANFD_INST_1:
#if defined(_C1MD)
        _C1MD = enable ? 0u : 1u;
        return NORA_CANFD_OK;
#else
        break;
#endif
    case NORA_CANFD_INST_2:
#if defined(_C2MD)
        _C2MD = enable ? 0u : 1u;
        return NORA_CANFD_OK;
#else
        break;
#endif
    default:
        break;
    }

    return NORA_CANFD_ERR_UNSUPPORTED;
}
