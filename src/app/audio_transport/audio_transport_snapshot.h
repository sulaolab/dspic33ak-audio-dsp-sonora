#ifndef SONORA_AUDIO_TRANSPORT_SNAPSHOT_H
#define SONORA_AUDIO_TRANSPORT_SNAPSHOT_H

#include <stdbool.h>
#include <stdint.h>

#include "transport_leg_count.h"

/* Telemetry width follows the built topology; it is not an independent limit. */
#define AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS  (TRANSPORT_LEG_MAX)

typedef enum
{
    AUDIO_TRANSPORT_TRANSITION_NONE = 0,
    AUDIO_TRANSPORT_TRANSITION_INITIAL_START,
    AUDIO_TRANSPORT_TRANSITION_MANUAL_RESTART,
    AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE,
    AUDIO_TRANSPORT_TRANSITION_RATE_CHANGE_ROLLBACK,
    AUDIO_TRANSPORT_TRANSITION_CLOCK_RESUME,
    AUDIO_TRANSPORT_TRANSITION_PHASE_RECOVERY,
    AUDIO_TRANSPORT_TRANSITION_FRMERR_RECOVERY,
} audio_transport_transition_reason_t;

typedef enum
{
    AUDIO_TRANSPORT_TRANSITION_ERROR_NONE = 0,
    AUDIO_TRANSPORT_TRANSITION_ERROR_STOP_FAILED,
    AUDIO_TRANSPORT_TRANSITION_ERROR_START_FAILED,
    AUDIO_TRANSPORT_TRANSITION_ERROR_CODEC_APPLY_FAILED,
    /* An audio clock could not be brought up as requested (e.g. PLL2 <- REFI1
     * for SPI2's transport clock). Distinct from START_FAILED because the
     * transport was never started: running it on an unintended clock is exactly
     * what the clock design contract forbids. */
    AUDIO_TRANSPORT_TRANSITION_ERROR_CLOCK_SETUP_FAILED,
} audio_transport_transition_error_t;

typedef struct
{
    bool     present;
    bool     active;
    bool     running;
    bool     deadline_miss_latched;
    uint8_t  physical_spi_instance;
    uint32_t configured_rate_hz;
    uint32_t block_count;
    uint32_t callback_last_us10;
    uint32_t callback_peak_us10;
    uint32_t callback_deadline_us10;
    uint32_t deadline_miss_count;
    uint32_t rx_dma_overrun_count;
    uint32_t rx_dma_other_irq_count;
    uint32_t rx_dma_last_status;
    uint32_t rx_overrun_block_count;
    uint32_t tx_underrun_block_count;
    uint32_t frame_error_block_count;
} audio_transport_leg_snapshot_t;

typedef struct
{
    uint32_t                            stream_epoch;
    audio_transport_transition_reason_t last_transition;
    audio_transport_transition_error_t  last_transition_error;
    bool                                qualified_running;
    bool                                safe_mute_latched;
    bool                                transition_failed;
    bool                                mute_held_by_failed_transition;
    uint8_t                             leg_count;
    audio_transport_leg_snapshot_t      legs[AUDIO_TRANSPORT_SNAPSHOT_MAX_LEGS];
} audio_transport_snapshot_t;

/* Read one coherent-enough diagnostic snapshot; no counters are cleared. */
bool audio_transport_snapshot_get( audio_transport_snapshot_t* out );

/* Read the current diagnostic window, then clear each leg's callback peak. */
bool audio_transport_snapshot_take_window( audio_transport_snapshot_t* out );

#endif /* SONORA_AUDIO_TRANSPORT_SNAPSHOT_H */
