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

/*
 * CCP period-queue observation: occupancy / drain-interval max-holds and cumulative
 * enqueue / dequeue / discard tallies, printed once per telemetry report -- never per
 * capture event, so the measurement cannot create the drain stall it exists to measure.
 *
 * On by default while the queue budget is being tracked on real hardware; the numbers cost
 * 14 B per detector plus 10 B global, and one telemetry line. Build with
 * -Define APP_ASRC_CCP_QUEUE_OBSERVE=0 to remove all of it.
 */
#ifndef APP_ASRC_CCP_QUEUE_OBSERVE
#define APP_ASRC_CCP_QUEUE_OBSERVE (1)
#endif

/*
 * Drain the CCP period queues from inside a long blocking console write.
 *
 * Registered as the uart_platform stdio yield hook, so it runs per output chunk rather than
 * once per report. No-op until the queues are running, and single-consumer-safe against an
 * ISR-context printf. See the note over the definition for the measurement behind it.
 */
void asrc_clock_control_drain_yield( void );

/*
 * Whether that registration happens. Exists so the overrun can be REPRODUCED with the
 * observation counters present: -Define APP_ASRC_CCP_QUEUE_DRAIN_ON_WRITE=0 leaves the drain
 * where it was (once per main-loop pass) and lets the same image measure the stall that
 * causes the overrun. Not a supported operating mode -- 0 is the broken side of the A/B.
 */
#ifndef APP_ASRC_CCP_QUEUE_DRAIN_ON_WRITE
#define APP_ASRC_CCP_QUEUE_DRAIN_ON_WRITE (1)
#endif

#if APP_ASRC_CCP_QUEUE_OBSERVE
/*
 * Clear the observation max-holds and cumulative tallies, to start a measurement window.
 * Deliberately does NOT clear period_overrun_count: that history is the evidence a run is
 * judged on, and a clear must not be able to make an overrun disappear.
 */
void asrc_clock_control_queue_stats_clear( void );
#endif

/* Print ASRC/CCP detail using block-count rates supplied by shared transport telemetry. */
void asrc_clock_control_debug_print( uint32_t fs_a_hz,
                                     uint32_t fs_b_hz,
                                     uint32_t recover_count );

#endif /* SONORA_ASRC_CLOCK_CONTROL_H */
