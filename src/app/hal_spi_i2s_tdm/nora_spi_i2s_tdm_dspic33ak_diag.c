
//===========================================================
// INCLUDES
//===========================================================
#include "nora_spi_i2s_tdm_dspic33ak_diag_fast.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>                   // NULL
#include "nora_high_res_timer.h"     // NORA high-resolution timer API (runtime-gated)
#include "nora_dma.h"
#include "nora_dma_dspic33ak_fast.h"  // dsPIC33AK ISR diagnostic fast path
#include "nora_spi_i2s_tdm_dspic33ak_reg.h" // NORA_SPI_I2S_TDM_STAT_* masks (note_errflags)


//===========================================================
// Definition
//===========================================================

// ---- Debug / diagnostics master switch ----
// Default OFF: the diagnostics compile with no printf dependency and no debug GPIO
// toggles. Define ENA_TDM_DBG (here or via the build configuration) to restore the
// printf / scope-GPIO debug behavior. The load/time monitor itself is NOT gated:
// its accumulators are always captured so the load monitor works in production
// when the high-resolution timer HAL has been initialized.
//#define ENA_TDM_DBG

#if defined(ENA_TDM_DBG)
  #include <stdio.h>                 // printf (debug build only)
  #include "nora_tick_timer.h"  // timestamp for the debug trap print
  #include "board/board_dbg_pins.h"  // BOARD_DBG_PIN_* scope pins
  #define TDM_DBG_PRINTF(...)   printf(__VA_ARGS__)
#else
  #define TDM_DBG_PRINTF(...)   ((void)0)
#endif //defined(ENA_TDM_DBG)


//===========================================================
// Global Function
//===========================================================

/*
 * Reset every diagnostic counter.
 *
 * isr_min_count is seeded to UINT32_MAX so the first timed ISR sample becomes the
 * minimum. start() calls this so each stream run reports fresh statistics.
 */
void nora_spi_i2s_tdm_diag_reset( nora_spi_i2s_tdm_diag_t* d )
{
    if( d == NULL )
    {
        return;
    }

    d->block_count               = 0u;
    d->block_deadline_miss_count = 0u;
    d->rx_dma_overrun_count       = 0u;
    d->rx_dma_other_irq_count     = 0u;
    d->rx_dma_last_status         = 0u;
    d->err_rov_block_count        = 0u;
    d->err_tur_block_count        = 0u;
    d->err_frm_block_count        = 0u;
    d->frmerr_consecutive_blocks  = 0u;
    d->isr_start_count           = 0u;
    d->isr_last_count            = 0u;
    d->isr_min_count             = 0xFFFFFFFFUL;
    d->isr_max_count             = 0u;
    d->isr_event_count           = 0u;
    d->isr_measure_active        = false;
}


/*
 * Preserve one raw RX-DMA IRQ cause before HALF/DONE resolution.
 *
 * DMAxSTAT.OVERRUN is the primary transport-stall signal: a request arrived while
 * the channel still had a pending request. An overrun-only snapshot cannot map to a
 * completed ping-pong half, so without this counter the core's early return would erase
 * the root-cause evidence. The raw last value also keeps unexpected DMA status bits
 * inspectable through the public status API.
 */
void nora_spi_i2s_tdm_diag_note_dma_status( nora_spi_i2s_tdm_diag_t* d,
                                             nora_dma_status_t status )
{
    if( d == NULL )
    {
        return;
    }

    d->rx_dma_last_status = status;
    if( nora_dma_status_has_overrun_hot( status ) )
    {
        d->rx_dma_overrun_count++;
    }
    if( !nora_dma_status_has_completed_half_hot( status ) )
    {
        d->rx_dma_other_irq_count++;
    }
}


/*
 * Begin block-ISR timing instrumentation.
 *
 * The load monitor is active only when the high-resolution timer HAL is already
 * initialized. Optional debug GPIO toggles are compiled in only for ENA_TDM_DBG.
 */
void nora_spi_i2s_tdm_diag_isr_begin( nora_spi_i2s_tdm_diag_t* d )
{
    if( d == NULL )
    {
        return;
    }

    // Load/time monitor: capture only when the high-resolution timer is live.
    d->isr_measure_active = nora_high_res_timer_is_initialized();
    if( d->isr_measure_active )
    {
        d->isr_start_count = nora_high_res_timer_get_count();
    }

#if defined(ENA_TDM_DBG)
    // debug-only scope GPIO: measuring the process time on a pin.
    (void)nora_gpio_toggle(BOARD_DBG_PIN_E4);
    (void)nora_gpio_set(BOARD_DBG_PIN_H0);
#endif //defined(ENA_TDM_DBG)
}


/*
 * End block-ISR timing instrumentation and update load statistics.
 *
 * Records last/min/max/event_count for *_get_load(). If the timer is unavailable,
 * measurement is simply abandoned for this ISR without affecting the audio path.
 */
void nora_spi_i2s_tdm_diag_isr_end( nora_spi_i2s_tdm_diag_t* d )
{
    // Load/time monitor: always accumulated (feeds *_get_load).
    uint32_t end_count;
    uint32_t diff_count;

    if( d == NULL )
    {
        return;
    }

    if( !d->isr_measure_active || !nora_high_res_timer_is_initialized() )
    {
        d->isr_measure_active = false;
#if defined(ENA_TDM_DBG)
        // debug-only scope GPIO: measuring the process time on a pin.
        (void)nora_gpio_clear(BOARD_DBG_PIN_H0);
#endif //defined(ENA_TDM_DBG)
        return;
    }

    d->isr_measure_active = false;

    end_count  = nora_high_res_timer_get_count();
    diff_count = end_count - d->isr_start_count;

    d->isr_last_count = diff_count;

    if( diff_count < d->isr_min_count )
    {
        d->isr_min_count = diff_count;
    }

    if( diff_count > d->isr_max_count )
    {
        d->isr_max_count = diff_count;
    }

    d->isr_event_count++;

#if defined(ENA_TDM_DBG)
    // debug-only scope GPIO: measuring the process time on a pin.
    (void)nora_gpio_clear(BOARD_DBG_PIN_H0);
#endif //defined(ENA_TDM_DBG)
}


/*
 * Count one completed block (read via *_read_counts() / get_status()).
 */
void nora_spi_i2s_tdm_diag_note_block( nora_spi_i2s_tdm_diag_t* d )
{
    if( d == NULL )
    {
        return;
    }
    d->block_count++;
}


/*
 * Sample framed-transport health: fold one RX-block's SPIxSTAT flag observation into this
 * instance's diagnostics. MUST be called once per completed block (even when flags==0) so
 * frmerr_consecutive_blocks resets on a clean block. `flags` is the
 * nora_spi_i2s_tdm_hw_sample_ack_errflags() mask. Each counter counts RX BLOCKS in which its
 * bit was observed, not raw event occurrences. When FRMERR is absent, frmerr_consecutive_blocks
 * is reset to zero; the other counters are unchanged when flags == 0.
 */
void nora_spi_i2s_tdm_diag_note_errflags( nora_spi_i2s_tdm_diag_t* d, uint32_t flags )
{
    if( d == NULL )
    {
        return;
    }
    if( flags & NORA_SPI_I2S_TDM_STAT_SPIROV ) { d->err_rov_block_count++; }
    if( flags & NORA_SPI_I2S_TDM_STAT_SPITUR ) { d->err_tur_block_count++; }
    if( flags & NORA_SPI_I2S_TDM_STAT_FRMERR )
    {
        d->err_frm_block_count++;
        d->frmerr_consecutive_blocks++;
    }
    else
    {
        d->frmerr_consecutive_blocks = 0u;   // FRMERR absent this block -> break the run
    }
}


/*
 * Update deadline-miss diagnostics from this instance's DMA status snapshot.
 *
 * HALF+DONE together means software missed a ping-pong service deadline for THIS
 * instance: its RX-block ISR fell a full block behind. Each instance keeps its own
 * block_deadline_miss_count, so the miss is counted in the passed-in diag.
 */
void nora_spi_i2s_tdm_diag_check_deadline( nora_spi_i2s_tdm_diag_t* d,
                                            nora_dma_channel_t channel,
                                            nora_dma_status_t  status )
{
    nora_spi_i2s_tdm_diag_check_deadline_hot( d,
                                                   (uint8_t)channel,
                                                   status );
}


void nora_spi_i2s_tdm_diag_check_deadline_hot(
    nora_spi_i2s_tdm_diag_t* d,
    uint8_t                  channel,
    nora_dma_status_t        status )
{
    if( d == NULL )
    {
        return;
    }

    if( !nora_dma_status_has_half_done_conflict_hot( status ) )
    {
        return;
    }

    // A HALF+DONE conflict means this instance's block ISR fell a full block behind.
    d->block_deadline_miss_count++;

    TDM_DBG_PRINTF(" nora_dma_debug_check: dma=%d half/done conflict @%ld\n",
                   channel,
                   nora_tick_timer_get_ms());
    (void)channel;
}


/*
 * Snapshot the block-ISR load monitor.
 *
 * The caller masks the updating ISR around this call. Returns false until at least
 * one timed event exists or if the high-resolution timer was not initialized. When
 * clear_peak is true, min/max/event accumulation starts fresh afterward.
 */
bool nora_spi_i2s_tdm_diag_get_load( nora_spi_i2s_tdm_diag_t* d,
                                          nora_spi_i2s_tdm_load_t*  monitor,
                                          bool                           clear_peak )
{
    bool     valid;
    uint32_t last_count;
    uint32_t min_count;
    uint32_t max_count;
    uint32_t event_count;

    if( ( d == NULL ) || ( monitor == NULL ) )
    {
        return false;
    }

    last_count  = d->isr_last_count;
    min_count   = d->isr_min_count;
    max_count   = d->isr_max_count;
    event_count = d->isr_event_count;

    if( clear_peak )
    {
        d->isr_min_count   = 0xFFFFFFFFUL;
        d->isr_max_count   = 0u;
        d->isr_event_count = 0u;
    }

    valid = (event_count != 0) && nora_high_res_timer_is_initialized();

    if( !valid )
    {
        last_count  = 0;
        min_count   = 0;
        max_count   = 0;
        event_count = 0;
    }

    monitor->last_count  = last_count;
    monitor->min_count   = min_count;
    monitor->max_count   = max_count;
    monitor->event_count = event_count;

    monitor->last_us10 = nora_high_res_timer_count_to_us_x10( last_count );
    monitor->min_us10  = nora_high_res_timer_count_to_us_x10( min_count  );
    monitor->max_us10  = nora_high_res_timer_count_to_us_x10( max_count  );

    return valid;
}


/*
 * Read the two block counters under the caller's ISR mask.
 */
void nora_spi_i2s_tdm_diag_read_counts( const nora_spi_i2s_tdm_diag_t* d,
                                             uint32_t* block_count,
                                             uint32_t* block_deadline_miss_count )
{
    if( d == NULL )
    {
        return;
    }
    if( block_count != NULL )
    {
        *block_count = d->block_count;
    }
    if( block_deadline_miss_count != NULL )
    {
        *block_deadline_miss_count = d->block_deadline_miss_count;
    }
}


// The engine-wide TDMsum union profiler that lived here was removed on 2026-08-26. It measured
// TDM-active WALL time over a window phase-locked to one leg's block boundary, so a preemptor's
// execution was charged to the leg it preempted -- which made the reading depend on the
// rate-monotonic priority map instead of on the work done. Its replacement is the
// owner-attributed fixed-window meter in hal_timer/nora_cpu_load_prof*.{h,c}, which charges
// EXCLUSIVE time per owner and reports the stolen time as its own quantity.
