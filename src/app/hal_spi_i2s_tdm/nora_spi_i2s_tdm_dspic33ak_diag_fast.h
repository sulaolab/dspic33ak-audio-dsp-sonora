#ifndef NORA_SPI_I2S_TDM_DSPIC33AK_DIAG_FAST_H
#define NORA_SPI_I2S_TDM_DSPIC33AK_DIAG_FAST_H

// dsPIC33AK-private diagnostics ISR fast path. This header is intentionally separate from
// nora_spi_i2s_tdm_diag.h: portable consumers see diagnostics snapshots only, never the ISR-only
// entry points.
//
// The engine-wide TDMsum union profiler that used to live here was removed on 2026-08-26 and
// replaced by the owner-attributed fixed-window meter in hal_timer/nora_cpu_load_prof*.h. It
// measured TDM-active WALL time, which counts a preemptor's execution inside the leg it
// preempted -- unreadable once the legs stopped sharing one interrupt priority.

#include "nora_spi_i2s_tdm_diag.h"

/*
 * ISR fast path for the deadline check -- see the naming rule in
 * nora_dma_dspic33ak_fast.h: <portable name>_hot, supplied by the backend header.
 *
 * Not a static inline like the rest of this header, and deliberately so. The
 * portable nora_spi_i2s_tdm_diag_check_deadline() takes nora_dma_channel_t, and
 * XC-DSC treats that enum as wider than the uint8_t the ISR already holds;
 * calling the portable form from tdm_rx_block() pushed that function past the
 * compiler's inline-cost threshold. This keeps the ISR ABI narrow while the
 * portable declaration stays type-safe.
 *
 * It used to live in a separate nora_spi_i2s_tdm_diag_internal.h under the name
 * _isr_raw. One convention, one header: an ISR-only entry point belongs with the
 * other ISR-only entry points of the same module.
 */
void nora_spi_i2s_tdm_diag_check_deadline_hot(
    nora_spi_i2s_tdm_diag_t* d,
    uint8_t                  channel,
    nora_dma_status_t        status );


#endif // NORA_SPI_I2S_TDM_DSPIC33AK_DIAG_FAST_H
