#ifndef NORA_SPI_I2S_TDM_DSPIC33AK_HW_H
#define NORA_SPI_I2S_TDM_DSPIC33AK_HW_H

//===========================================================
// nora_spi_i2s_tdm_dspic33ak_hw.{c,h} = the SILICON layer of the SPI/I2S/TDM HAL, kept
// separate from the transport core. hw.c owns the data-sheet device-facts table
// (s_spi_dev[]: SPIxBUF/CON1/BRG/IMSK pointers, logical DMA triggers, CPU IRQ bits, with
// the AK512/AK128 #if) and every function that programs one physical SPI peripheral in
// framed (I2S/TDM) mode + arms its DMA channels.
//
// DELIBERATELY DECOUPLED FROM THE INSTANCE STRUCT: these functions take a
// tdm_spi_inst_t (which physical SPI) plus the raw DMA channels/buffers, NOT a
// tdm_spi_leg_t*. The transport core owns the instance/leg struct (callback, diag,
// running, config) and passes the silicon-relevant fields down. So hw.c knows nothing
// about the engine's instance bookkeeping -- a clean "drive the silicon" boundary. The
// register-level result is identical to the previous in-core per-leg code.
//===========================================================

#include <stdint.h>
#include <stdbool.h>
#include "nora_spi_i2s_tdm.h"   // nora_spi_i2s_tdm_config_t / _role_t

// Device identity is a dsPIC33AK backend concern. Keep compiler part macros out of the
// public transport contract; this adapter is the one place that maps them to backend tags.
// Tag values are arbitrary: compare with == only (never order / arithmetic).
/* CPU interrupt priority of every leg's RX-DMA interrupt. Lives here rather than in
 * hw.c because two translation units need it now: hw.c programs it at configure()
 * time, and the core re-programs one leg to base-1 for rate-monotonic legs
 * (nora_spi_i2s_tdm_set_rate_monotonic_priorities). Above it sit UART2 async TX and
 * the CCP capture at 5, which must keep preempting the audio ISRs -- which is why
 * the asymmetry is made by LOWERING the long-deadline leg, not by raising the other. */
#define PRIO_TDM_DMA              (4)

#define NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK512   (1)
#define NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK128   (2)

#if   defined(__dsPIC33AK512MPS512__)
  #define NORA_SPI_I2S_TDM_DSPIC33AK_DEVICE \
      NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK512
#elif defined(__dsPIC33AK128MC106__)
  #define NORA_SPI_I2S_TDM_DSPIC33AK_DEVICE \
      NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK128
#else
  #error "Unsupported device -- the dsPIC33AK SPI/I2S/TDM backend expects __dsPIC33AK512MPS512__ or __dsPIC33AK128MC106__."
#endif


// Physical SPI instances present on the device (data sheet). SPI4 is AK512 only.
// TDM_SPI_INST_COUNT is the number of silicon SPI instances on the built device
// (the size of s_spi_dev[]); the core uses it to bound-check an instance.
typedef enum {
    TDM_SPI1 = 0,
    TDM_SPI2 = 1,
    TDM_SPI3 = 2,
#if NORA_SPI_I2S_TDM_DSPIC33AK_DEVICE == NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK512
    TDM_SPI4 = 3,
#endif // NORA_SPI_I2S_TDM_DSPIC33AK_DEVICE == NORA_SPI_I2S_TDM_DSPIC33AK_DEV_AK512
    TDM_SPI_INST_COUNT
} tdm_spi_inst_t;


// ---- Per-physical-SPI silicon operations (no instance-struct knowledge) ----
// All are no-ops for an out-of-range inst. apply_config writes SPIxCON1/BRG from the
// already-validated config and leaves DMA-trigger events + the module OFF (caller arms
// triggers, then enables the module). nora_dma_config configures + enables that SPI's RX and
// TX DMA channels (RX: SPIxBUF->rx_buf, TX: tx_buf->SPIxBUF; count = words per buffer)
// and returns false if the DMA controller rejects a channel. nora_dma_trigger_enable toggles
// the SPIxIMSK event->DMA enables; module_enable toggles SPIxCON1.ON; irq_clear_flags
// clears the CPU RX/TX interrupt flags; soft_stop disables triggers + masks CPU IRQs +
// clears the ON bit (no PPS/CLC change).
void nora_spi_i2s_tdm_hw_apply_config( tdm_spi_inst_t inst,
                                            const nora_spi_i2s_tdm_config_t* cfg );
bool nora_spi_i2s_tdm_hw_dma_config( tdm_spi_inst_t inst,
                                          uint8_t  rx_dma_ch,
                                          uint8_t  tx_dma_ch,
                                          int32_t* rx_buffer,
                                          int32_t* tx_buffer,
                                          uint32_t buffer_word_count,
                                          uint8_t  irq_priority );
void nora_spi_i2s_tdm_hw_dma_trigger_enable( tdm_spi_inst_t inst, bool enable );
void nora_spi_i2s_tdm_hw_module_enable( tdm_spi_inst_t inst, bool enable );
void nora_spi_i2s_tdm_hw_irq_clear_flags( tdm_spi_inst_t inst );
void nora_spi_i2s_tdm_hw_soft_stop( tdm_spi_inst_t inst );

// Sample this instance's SPI framed-transport health flags (SPIxSTAT SPIROV/SPITUR/FRMERR) and
// ack the software-clearable ones (SPIROV/FRMERR). SPITUR is R/HSC (not software-clearable; it
// is only observed here, reflecting a live/dynamic underrun condition). Returns the full observed
// mask (NORA_SPI_I2S_TDM_STAT_* bits, as read, before any ack); 0 if none set or inst out of
// range. Cheap (one SFR read + masked write-back), meant to be called once per RX-block ISR (the
// driver otherwise runs with IGNROV/IGNTUR set and never inspects these HW flags). The HAL owns
// these sticky bits once this is called -- other code reading raw SPIxSTAT afterward will see
// SPIROV/FRMERR already acked.
uint32_t nora_spi_i2s_tdm_hw_sample_ack_errflags( tdm_spi_inst_t inst );

// Return the PPS output-function code (_RPOUT_SSx) for one SPI instance's frame-sync
// (SSx = FRMSYNC output in framed master mode). Used by the CLC10 50%-FS module to find,
// by reverse PPS lookup, which physical pin the board routed this instance's FS to.
// Returns false (and leaves *code untouched) if inst is out of range or the device header
// defines no SSx output for it. (data-sheet fact, device-#ifdef'd like the rest of hw.c.)
bool nora_spi_i2s_tdm_hw_get_ss_pps_code( tdm_spi_inst_t inst, uint8_t* code );


#endif // NORA_SPI_I2S_TDM_DSPIC33AK_HW_H
