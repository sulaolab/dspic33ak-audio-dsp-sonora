/* SPDX-License-Identifier: MIT-0 */
#ifndef DRIVER_SAI_DSPIC33AK_H
#define DRIVER_SAI_DSPIC33AK_H

/*
 * CMSIS-Driver SAI wrapper for the dsPIC33AK SPI/I2S/TDM HAL.
 *
 * Thin mapping of the ARM CMSIS-Driver SAI API onto the dsPIC33AK
 * nora_spi_i2s_tdm HAL. No ARM_SAI_* / ARM_DRIVER_* types appear in the HAL;
 * they are confined to this wrapper (same boundary as the USART/I2C wrappers).
 *
 * One instance, Driver_SAI0, = the single SPI/I2S/TDM transport. Implemented:
 * GetVersion, GetCapabilities, Initialize, Uninitialize, PowerControl, GetStatus,
 * a Control parse (slave + I2S/2-slot or TDM8/8-slot, 32-bit; AUDIO_FREQ checked
 * against the integrator rate hook; CONTROL_TX/RX = whole-stream start/stop;
 * configure only while stopped), and a Send/Receive copy layer over the HAL's
 * zero-copy block callback. GetCapabilities and the Control parser advertise/accept
 * ONLY the validated envelope; everything else returns ARM_DRIVER_ERROR_UNSUPPORTED
 * (or the matching ARM_SAI_ERROR_*).
 *
 * The wrapper registers its HAL block callback explicitly in PowerControl(FULL)
 * (the HAL core has no app fallback). Board electricals and the supported-rate set
 * are board/app concerns reached through two weak integration hooks below
 * (Driver_SAI_dsPIC33AK_GetDefaultConfig / _IsSampleRateSupported); a project must
 * override at least GetDefaultConfig before a stream can start.
 *
 * Verified live (full-duplex I2S/TDM loopback) on dsPIC33AK512MPS512 in the
 * Sonora integration project. Not implemented (mirrors the HAL's validated
 * envelope): master clock / MCLK output / prescaler, DATA_SIZE 16/24,
 * justified/PCM/AC97, mono/companding, slot offset/mask, FRAME_ERROR, dynamic
 * sample-rate switching, true independent per-direction TX/RX.
 */

#include <stdbool.h>
#include <stdint.h>

#include "Driver_SAI.h"
#include "nora_spi_i2s_tdm.h"   // nora_spi_i2s_tdm_config_t (default-config hook)

#ifdef __cplusplus
extern "C" {
#endif

extern ARM_DRIVER_SAI Driver_SAI0;

/*
 * Integration hooks (override these in the integrating project).
 *
 * The wrapper is board-/app-independent: it does not know this board's electrical
 * config or this product's supported sample-rate set. Both come through these two
 * hooks, each shipped here as a __attribute__((weak)) default so the wrapper links
 * standalone; the integrator provides a strong definition (same pattern as the I2C
 * wrapper's Driver_I2C_dsPIC33AK_GetMs()).
 */

/*
 * Seed the HAL config used by Control(CONFIGURE_TX/RX). The wrapper derives protocol
 * and slot count from the CMSIS control word and forces the validated slave / 32-bit
 * transport envelope; every other field (board electricals: BRG, fs_shape/SPIFE/CKP/CKE, MCLKEN, block
 * geometry, ...) comes from here. Return false if no config is available (the weak default
 * returns false, so an integrator that has not provided one gets a clean Control() failure
 * rather than a stream on garbage geometry).
 *
 * WIRE-FORMAT CONTRACT (integrator's responsibility): the electrical framing fields returned
 * here are NOT re-derived by the wrapper from the CMSIS control word, so they MUST already be
 * consistent with the protocol the caller will request via Control(CONFIGURE_*):
 *   - fs_shape: FS_50PCT for an I2S request (50%-duty LRCLK); FS_PULSE for a TDM (short sync,
 *     FRAME_SYNC_EARLY=0 / default width).
 *   - fs_coincides_first_bclk (SPIFE): match the I2S 1-bit-delay vs TDM framing convention.
 *   - bclk_idle_high / bclk_change_on_active_to_idle (CKP/CKE): match ARM_SAI_CLOCK_POLARITY_0
 *     (the only polarity the wrapper accepts) for this board/codec.
 * SEPARATELY, mclk_enable is an integration CLOCK-TREE setting, NOT a wire-format field derived
 * from the CMSIS control: it drives SPIxCON1.MCLKEN, selecting the SPI peripheral-clock reference
 * (CLKGEN9 vs the standard peripheral clock). The wrapper's ARM_SAI_MCLK_PIN_INPUT/INACTIVE check
 * is only a declared-attribute gate; it does NOT route a physical MCLK pin or set cfg.mclk_enable.
 * Set mclk_enable per the board clock tree.
 * A Control(CONFIGURE_*) that returns ARM_DRIVER_OK means the CMSIS-expressed fields were
 * accepted; the wrapper trusts this hook for the electrical fields above and does NOT verify
 * them, so an inconsistent hook produces a running-but-mismatched wire format. (The wrapper
 * validates only what CMSIS unambiguously determines: protocol/slots/role/word size/rate.)
 */
bool Driver_SAI_dsPIC33AK_GetDefaultConfig(nora_spi_i2s_tdm_config_t *cfg);

/*
 * Product/board sample-rate policy for Control(CONFIGURE_*, ARM_SAI_AUDIO_FREQ).
 * The transport HAL is rate-agnostic; this hook is the wrapper's allow-list. Return
 * true if hz is supported. The weak default accepts only RTE_SAI0_DEFAULT_SAMPLE_RATE_HZ.
 */
bool Driver_SAI_dsPIC33AK_IsSampleRateSupported(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_SAI_DSPIC33AK_H */
