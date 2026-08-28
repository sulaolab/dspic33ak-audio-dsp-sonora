#ifndef AUDIO_APP_MEAS_H
#define AUDIO_APP_MEAS_H

//===========================================================
// ASRC App measurement interface (audio_app_meas.h)
//
// ASRC quality MEASUREMENT harness (bench only, APP_ASRC_MEAS builds). Replaces the A->B
// ASRC input with an on-chip pure sine and captures the DIGITAL A->B output to RAM so the
// resampler's image / alias / SNR can be measured by offline FFT -- isolated from the codec
// DAC/ADC floor (~-90 dB) that would otherwise mask a good kernel. Console-driven:
//   *nt20 arm capture   *nt21 dump buffer   *nt22 tone=low   *nt23 tone=high
//
// Only compiled/linked in an APP_ASRC_MEAS build (which also forces one-way A->B so the
// B->A instance is not allocated -- freeing the RAM the capture buffer needs).
//===========================================================

#include <stdint.h>
#include "app_specific_config_defs.h"

#if APP_ASRC_MEAS

// Fill one A-input block with the current test sine (stereo slots 0/1, other slots 0), in the
// 32-bit left-justified codec slot format. The A block callback pushes this into the A->B FIFO
// instead of the codec input.
void audio_app_meas_gen_input( int32_t* block );

// While armed, capture one just-resampled A->B output block's L channel into the buffer.
void audio_app_meas_capture( const int32_t* out_block );

// Console control. arm: start a one-shot capture. dump: print the captured buffer for FFT.
void audio_app_meas_arm( void );
void audio_app_meas_dump( void );
void audio_app_meas_set_tone_low( void );
// "High" = band-edge stress at 18 kHz REAL, so the row is resolved from the live source-leg rate
// (a 48 kHz-nominal HF table would be 36 kHz on a 96 kHz leg and alias). See the implementation.
void audio_app_meas_set_tone_high( void );
void audio_app_meas_set_tone_row( uint8_t row );    // explicit MEAS_TONE_ROW_* (bench override)
void audio_app_meas_set_level_idx( uint8_t idx );   // 0=-1,1=-60,2=-20,3=-40,4=-80,5=-6 dBFS

#if APP_ASRC_48K_TO_8_DECIMATOR
// Standalone fixed-decimator mode. Called only by the 48 kHz A-domain block callback.
void audio_app_meas_decimator_process_block( void );
void audio_app_meas_set_decimator_tone_idx( uint8_t idx ); // 0..8, see implementation table
#endif

// R10 Q10: measurement-only control-variable trace (reuses the capture buffer; reads control state
// only, does not alter the loop). arm: sel 0=applied step,1=corr_lpf,2=fill; decim = store every Nth
// pull. tick: called once per A->B pull with current control state. dump: print for offline analysis.
void audio_app_meas_trace_arm( uint8_t sel, uint16_t decim );   // sel 3 = feed-forward ratio
// Q29 Q10: raw_corr/flags added for sel=10 (servo internal-state trace); ignored by other sel values.
// flags bit0=clamp_hit (target_step hit the +-STEP_LO/HI envelope before clamping), bit1=slew_hit
// (the pre-slew delta exceeded +-ASRC_STEP_SLEW before slew-limiting). Read-only instrumentation --
// does not change KP/ALPHA/SLEW/clamp behavior.
// Q34 fractional-wrap causality trace (sel=11) additionally needs the consumer fractional read phase
// `frac` [0,1) sampled at the SAME block-entry instant as fill/corr_lpf/applied_step -- so the host can
// reconstruct the wrap-event sequence (rd advances) and correlate it against the servo chain. Ignored
// by all other sel values. Read-only; does not change the loop.
// `wraps` (Q34): consumer wraps (rd advances) during the previous block. `wr_adv` (Q35): producer
// frames pushed over the same inter-pull interval. Together: fill[n]-fill[n-1] = wr_adv[n]-wraps[n].
// Both used only by sel=11; ignored by other sel values.
void audio_app_meas_trace_tick( float applied_step, float corr_lpf, uint32_t fill, float ratio,
                                 float fill_ma, float raw_corr, uint8_t flags, float frac,
                                 uint16_t wraps, uint16_t wr_adv );
void audio_app_meas_trace_dump( void );
void audio_app_meas_q11_isolate( uint8_t mode );   // R11 Q11 isolation probe (0=lock,1=+route,2=+timers)

#if APP_ASRC_MEAS_UART2_STREAM
// Q19 base: long-coherent binary stream of the REAL A->B resampler output over UART2 -> PKOB4.
//   arm(seconds): begin producing `seconds` worth of 100-byte frames (called from the *nt31
//     control command; `seconds` is BCD-decoded on UART1). The producer runs in the SPI2 ISR
//     via audio_app_meas_capture() -- one completed A->B output block == one published frame.
//   service(): main-loop consumer; drains the SPSC ring to the UART2 DATA port and emits
//     *STREAM_BEGIN/END on UART1. Returns nonzero while a capture owns the loop so the caller
//     skips telemetry prints (true non-intrusion: ASRC + FF tick keep running, only prints pause).
void audio_app_meas_stream_arm( uint8_t seconds );
int  audio_app_meas_stream_service( void );
#endif // APP_ASRC_MEAS_UART2_STREAM

#endif // APP_ASRC_MEAS

#endif // AUDIO_APP_MEAS_H
