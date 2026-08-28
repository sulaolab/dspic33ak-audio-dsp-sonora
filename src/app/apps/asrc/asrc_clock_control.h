#ifndef SONORA_ASRC_CLOCK_CONTROL_H
#define SONORA_ASRC_CLOCK_CONTROL_H

#include <stdint.h>

/* ASRC-owned clock measurement and feed-forward control. */
void asrc_clock_control_init_reset( void );
void asrc_clock_control_tick( void );

/* Capture counters stay independent of the audio DMA and feed its liveness guard. */
uint32_t asrc_clock_control_capture_count_a( void );
uint32_t asrc_clock_control_capture_count_b( void );

/*
 * CCP-MEASURED sample rate of one logical leg, in Hz; 0 while nothing has been measured yet
 * (startup, or a leg whose capture is not armed). leg: 0 = A, 1 = B, matching audio_transport's
 * logical leg indices. This is the real clock, not the configured rate -- 47792 where the codec
 * was asked for 48000 -- and it is what the per-leg TDM telemetry line reports.
 */
uint32_t asrc_clock_control_measured_fs_hz( uint8_t leg );

/* Print ASRC/CCP detail using block-count rates supplied by shared transport telemetry. */
void asrc_clock_control_debug_print( uint32_t fs_a_hz,
                                     uint32_t fs_b_hz,
                                     uint32_t recover_count );

#endif /* SONORA_ASRC_CLOCK_CONTROL_H */
