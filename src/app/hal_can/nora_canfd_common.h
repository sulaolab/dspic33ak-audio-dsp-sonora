/**
 * @file    nora_canfd_common.h
 * @brief   dsPIC33AK CAN FD HAL - shared primitives used by the role layer:
 *          validation, register lookup, bit-timing computation and per-instance
 *          mode state.
 */
#ifndef NORA_CANFD_COMMON_H
#define NORA_CANFD_COMMON_H

#include "nora_canfd.h"
#include "nora_canfd_dspic33ak_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True if inst is a valid enum value (does not check presence). */
bool nora_canfd_inst_is_valid(nora_canfd_instance_t inst);

/** Resolve an instance to its register table; ERR_NOT_PRESENT if absent. */
nora_canfd_status_t nora_canfd_get_regs(nora_canfd_instance_t inst,
                                                  const nora_canfd_regs_t **regs);

/**
 * Compute nominal + data bit-timing register words and the TDC word for a
 * given CAN clock, target rates and sample point.
 *
 * @param fcan_hz       CAN module clock (FCAN), e.g. 20 MHz.
 * @param nominal_bps   arbitration phase bit rate, e.g. 500000.
 * @param data_bps      data phase bit rate, e.g. 2000000.
 * @param sample_pct    sample point in percent, e.g. 80.
 * @param nbtcfg/dbtcfg/tdc  filled with the CxNBTCFG / CxDBTCFG / CxTDC words.
 * @return OK or ERR_INVALID_ARG if no valid configuration was found.
 */
nora_canfd_status_t nora_canfd_calc_bit_timing(uint32_t fcan_hz,
                                                         uint32_t nominal_bps,
                                                         uint32_t data_bps,
                                                         uint8_t  sample_pct,
                                                         uint32_t *nbtcfg,
                                                         uint32_t *dbtcfg,
                                                         uint32_t *tdc);

/* Per-instance mode bookkeeping (drives is_initialized). */
void                   nora_canfd_set_mode(nora_canfd_instance_t inst,
                                                nora_canfd_mode_t mode);
nora_canfd_mode_t nora_canfd_get_mode(nora_canfd_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CANFD_COMMON_H */
