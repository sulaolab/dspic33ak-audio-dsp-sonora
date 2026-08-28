/**
 * @file    nora_canfd_common_dspic33ak.c
 * @brief   dsPIC33AK CAN FD HAL - shared primitives implementation.
 */
#include "nora_canfd_common.h"
#include "nora_canfd_device.h"

/* Only module-level state: the current mode per instance. */
static nora_canfd_mode_t g_mode[NORA_CANFD_INST_COUNT];

/* Bit-timing field limits (actual segment counts, before the -1 encoding). */
typedef struct {
    uint16_t tseg1_min, tseg1_max;
    uint16_t tseg2_min, tseg2_max;
    uint16_t sjw_max;
} bt_limits_t;

static const bt_limits_t k_nominal_limits = { 2u, 256u, 1u, 128u, 128u };
static const bt_limits_t k_data_limits    = { 1u,  32u, 1u,  16u,  16u };

bool nora_canfd_inst_is_valid(nora_canfd_instance_t inst)
{
    return (unsigned)inst < (unsigned)NORA_CANFD_INST_COUNT;
}

nora_canfd_status_t nora_canfd_get_regs(nora_canfd_instance_t inst,
                                                  const nora_canfd_regs_t **regs)
{
    const nora_canfd_device_t *dev;

    if (!nora_canfd_inst_is_valid(inst) || regs == NULL) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    dev = nora_canfd_get_device(inst);
    if (dev == NULL) {
        return NORA_CANFD_ERR_NOT_PRESENT;
    }
    *regs = &dev->regs;
    return NORA_CANFD_OK;
}

/*
 * Solve one phase: pick the smallest BRP that lands the total Tq count inside
 * the segment limits, then split for the requested sample point.
 * Register fields are (actual - 1); returns the assembled BTCFG word.
 */
static bool solve_phase(uint32_t fcan_hz, uint32_t bps, uint8_t sample_pct,
                        const bt_limits_t *lim, uint8_t pos_sjw, uint8_t pos_tseg2,
                        uint8_t pos_tseg1, uint8_t pos_brp,
                        uint32_t *btcfg_out, uint16_t *tseg1_actual_out,
                        uint16_t *brp_actual_out)
{
    uint32_t brp;

    if (bps == 0u) {
        return false;
    }
    for (brp = 1u; brp <= 256u; brp++) {
        uint32_t total = fcan_hz / (bps * brp);          /* total Tq per bit */
        uint16_t tseg1, tseg2, sjw;

        if ((fcan_hz % (bps * brp)) != 0u) {
            continue;                                     /* require exact division */
        }
        if (total < 4u) {
            break;                                        /* too few Tq, give up */
        }
        /* sample point: (1 + tseg1) / total = sample_pct/100 */
        tseg1 = (uint16_t)(((total * sample_pct) + 50u) / 100u) - 1u;
        if (tseg1 < lim->tseg1_min || tseg1 > lim->tseg1_max) {
            continue;
        }
        if (total < (uint32_t)(1u + tseg1 + lim->tseg2_min)) {
            continue;
        }
        tseg2 = (uint16_t)(total - 1u - tseg1);
        if (tseg2 < lim->tseg2_min || tseg2 > lim->tseg2_max) {
            continue;
        }
        sjw = (tseg2 < lim->sjw_max) ? tseg2 : lim->sjw_max;

        *btcfg_out = ((uint32_t)(brp - 1u)   << pos_brp)
                   | ((uint32_t)(tseg1 - 1u) << pos_tseg1)
                   | ((uint32_t)(tseg2 - 1u) << pos_tseg2)
                   | ((uint32_t)(sjw - 1u)   << pos_sjw);
        if (tseg1_actual_out) *tseg1_actual_out = tseg1;
        if (brp_actual_out)   *brp_actual_out = (uint16_t)brp;
        return true;
    }
    return false;
}

nora_canfd_status_t nora_canfd_calc_bit_timing(uint32_t fcan_hz,
                                                         uint32_t nominal_bps,
                                                         uint32_t data_bps,
                                                         uint8_t  sample_pct,
                                                         uint32_t *nbtcfg,
                                                         uint32_t *dbtcfg,
                                                         uint32_t *tdc)
{
    uint16_t dtseg1_actual = 0u;
    uint16_t dbrp_actual = 1u;

    if (nbtcfg == NULL || dbtcfg == NULL || tdc == NULL ||
        sample_pct == 0u || sample_pct >= 100u) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }

    if (!solve_phase(fcan_hz, nominal_bps, sample_pct, &k_nominal_limits,
                     NORA_CANFD_NBTCFG_SJW_POS, NORA_CANFD_NBTCFG_TSEG2_POS,
                     NORA_CANFD_NBTCFG_TSEG1_POS, NORA_CANFD_NBTCFG_BRP_POS,
                     nbtcfg, NULL, NULL)) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }
    if (!solve_phase(fcan_hz, data_bps, sample_pct, &k_data_limits,
                     NORA_CANFD_DBTCFG_SJW_POS, NORA_CANFD_DBTCFG_TSEG2_POS,
                     NORA_CANFD_DBTCFG_TSEG1_POS, NORA_CANFD_DBTCFG_BRP_POS,
                     dbtcfg, &dtseg1_actual, &dbrp_actual)) {
        return NORA_CANFD_ERR_INVALID_ARG;
    }

    /* Auto TDC; SSP offset = DBRP * DTSEG1 (datasheet recommendation for the
     * configured data sample point). TDCV is measured automatically. */
    {
        uint32_t tdco = (uint32_t)dbrp_actual * (uint32_t)dtseg1_actual;
        if (tdco > 0x7Fu) tdco = 0x7Fu;
        *tdc = ((uint32_t)NORA_CANFD_TDC_TDCMOD_AUTO << NORA_CANFD_TDC_TDCMOD_POS)
             | (tdco << NORA_CANFD_TDC_TDCO_POS);
    }
    return NORA_CANFD_OK;
}

void nora_canfd_set_mode(nora_canfd_instance_t inst, nora_canfd_mode_t mode)
{
    if (nora_canfd_inst_is_valid(inst)) {
        g_mode[inst] = mode;
    }
}

nora_canfd_mode_t nora_canfd_get_mode(nora_canfd_instance_t inst)
{
    if (!nora_canfd_inst_is_valid(inst)) {
        return NORA_CANFD_MODE_NONE;
    }
    return g_mode[inst];
}

/* ---- shared queries declared in the public header ---- */

bool nora_canfd_is_present(nora_canfd_instance_t inst)
{
    return nora_canfd_instance_is_present(inst);
}

bool nora_canfd_is_initialized(nora_canfd_instance_t inst)
{
    return nora_canfd_get_mode(inst) != NORA_CANFD_MODE_NONE;
}
