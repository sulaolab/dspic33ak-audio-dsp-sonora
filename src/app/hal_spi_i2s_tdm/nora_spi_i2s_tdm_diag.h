#ifndef NORA_SPI_I2S_TDM_DIAG_H
#define NORA_SPI_I2S_TDM_DIAG_H

//===========================================================
// Public diagnostics contract for the native SPI/I2S/TDM transport. It exposes per-stream
// health counters and ISR load snapshots only. The dsPIC33AK TDMsum state and ISR fast path live
// in nora_spi_i2s_tdm_dspic33ak_diag_fast.h and are not part of this public contract.
//
// CONCURRENCY: counters are updated from the block-completion ISR. The 32-bit reads in
// *_get_load()/*_read_counts() are NOT atomic on this 16-bit core, so the CALLER must mask the
// updating ISR's CPU interrupt around them. The transport core already brackets its public
// readers with the DMA IE mask; these helpers do no masking themselves.
//===========================================================

#include <stdint.h>
#include <stdbool.h>
#include "nora_dma.h"
#include "nora_spi_i2s_tdm.h"   // nora_spi_i2s_tdm_load_t (public load-monitor type)

// One block-completion ISR's diagnostics. block_count / block_deadline_miss_count are
// software/real-time stream health (not SPI HW over/underrun). The isr_* counters are raw
// high-res-timer ticks; *_get_load() converts them to the public load monitor.
typedef struct {
    volatile uint32_t block_count;                // completed blocks since reset()
    volatile uint32_t block_deadline_miss_count;  // HALF+DONE conflicts on this instance since reset()
    volatile uint32_t rx_dma_overrun_count;       // RX ISR snapshots with DMAxSTAT.OVERRUN since reset()
    volatile uint32_t rx_dma_other_irq_count;     // RX IRQ snapshots with neither HALF nor DONE since reset()
    volatile nora_dma_status_t rx_dma_last_status; // raw DMAxSTAT from the most recent RX IRQ
    volatile uint32_t err_rov_block_count;        // RX blocks where SPIROV was observed, since reset()
    volatile uint32_t err_tur_block_count;        // RX blocks where SPITUR was observed set, since reset()
    volatile uint32_t err_frm_block_count;        // RX blocks where FRMERR was observed, since reset()
    volatile uint32_t frmerr_consecutive_blocks;  // consecutive RX blocks with FRMERR observed (0 on a clean block)
    volatile uint32_t isr_start_count;            // timer count at the current ISR entry
    volatile uint32_t isr_last_count;             // ticks of the last completed ISR
    volatile uint32_t isr_min_count;              // min ticks since the last clear_peak
    volatile uint32_t isr_max_count;              // max ticks since the last clear_peak
    volatile uint32_t isr_event_count;            // number of timed ISR samples since the last clear_peak
    volatile bool     isr_measure_active;         // current ISR has a valid start sample
} nora_spi_i2s_tdm_diag_t;

// Reset every counter (isr_min_count is seeded to UINT32_MAX). Called by start().
void nora_spi_i2s_tdm_diag_reset( nora_spi_i2s_tdm_diag_t* d );

// Begin/end ISR load-time instrumentation. begin() snapshots the entry tick when the
// high-resolution timer HAL is initialized; end() records last/min/max/event counts.
void nora_spi_i2s_tdm_diag_isr_begin( nora_spi_i2s_tdm_diag_t* d );
void nora_spi_i2s_tdm_diag_isr_end( nora_spi_i2s_tdm_diag_t* d );

// Count one completed block for this instance's RX-block ISR.
void nora_spi_i2s_tdm_diag_note_block( nora_spi_i2s_tdm_diag_t* d );

// Fold one RX-block ISR's SPI framed-transport health flags into this instance's diagnostics.
// `flags` is the NORA_SPI_I2S_TDM_STAT_* mask returned by the backend hardware helper.
void nora_spi_i2s_tdm_diag_note_errflags( nora_spi_i2s_tdm_diag_t* d,
                                            uint32_t flags );

// Update deadline diagnostics from THIS instance's DMA status snapshot. A HALF+DONE
// conflict means this instance's RX-block ISR fell a full block behind, so it counts
// one deadline miss in its OWN diag. channel is used only for the debug print label.
void nora_spi_i2s_tdm_diag_check_deadline( nora_spi_i2s_tdm_diag_t* d,
                                            nora_dma_channel_t channel,
                                            nora_dma_status_t  status );

// Preserve the raw RX-DMA interrupt cause before the core resolves HALF/DONE.
void nora_spi_i2s_tdm_diag_note_dma_status( nora_spi_i2s_tdm_diag_t* d,
                                              nora_dma_status_t status );

// Snapshot the load monitor and clear min/max/event when clear_peak is true. Returns false and
// zeroes the monitor until a timed sample exists and the high-resolution timer is initialized.
bool nora_spi_i2s_tdm_diag_get_load( nora_spi_i2s_tdm_diag_t* d,
                                      nora_spi_i2s_tdm_load_t* monitor,
                                      bool clear_peak );

// Read the block and deadline-miss counters. Either output pointer may be NULL.
void nora_spi_i2s_tdm_diag_read_counts( const nora_spi_i2s_tdm_diag_t* d,
                                         uint32_t* block_count,
                                         uint32_t* block_deadline_miss_count );

#endif // NORA_SPI_I2S_TDM_DIAG_H
