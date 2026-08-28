#ifndef SONORA_AUDIO_TRANSPORT_CLIENT_H
#define SONORA_AUDIO_TRANSPORT_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "transport_static_config.h"

typedef void (*audio_transport_co_clock_process_t)( const int32_t* src,
                                                    int32_t*       dst_a,
                                                    int32_t*       dst_b,
                                                    void*          user );
typedef void (*audio_transport_leg_process_t)( const int32_t* src,
                                               int32_t*       dst,
                                               void*          user );
typedef void (*audio_transport_prepare_t)( uint32_t sample_rate_hz, void* user );
typedef void (*audio_transport_reset_t)( void* user );
typedef uint32_t (*audio_transport_clock_progress_t)( uint8_t leg, void* user );

#define AUDIO_TRANSPORT_LEG_A  TRANSPORT_LEG_A_VALUE
#define AUDIO_TRANSPORT_LEG_B  TRANSPORT_LEG_B_VALUE

#define AUDIO_TRANSPORT_CLIENT_CAP_CLOCK_PROGRESS  (1u << 0)

typedef struct
{
    uint32_t                         capabilities;
    audio_transport_co_clock_process_t co_clock_process;
    audio_transport_leg_process_t      leg_a_process;
    audio_transport_leg_process_t      leg_b_process;
    audio_transport_prepare_t          prepare;
    audio_transport_reset_t            reset_stream_state;
    audio_transport_clock_progress_t   clock_progress;
    void*                              user;
} audio_transport_client_t;

typedef enum
{
    AUDIO_TRANSPORT_CLIENT_BIND_OK = 0,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_NULL,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_ALREADY_BOUND,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_REQUIRED_HOOK,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_TOPOLOGY,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_CAPABILITY,
    AUDIO_TRANSPORT_CLIENT_BIND_ERR_MISSING_PROGRESS,
} audio_transport_client_bind_result_t;

/*
 * Bind exactly one static-lifetime client before transport start.  Rebinding the
 * same descriptor is idempotent; switching to another descriptor is rejected.
 */
audio_transport_client_bind_result_t audio_transport_client_bind(
    const audio_transport_client_t* client );
const char* audio_transport_client_bind_result_name(
    audio_transport_client_bind_result_t result );

/* Transitional access for the next dependency-inversion batch. */
const audio_transport_client_t* audio_transport_client_get( void );

#endif /* SONORA_AUDIO_TRANSPORT_CLIENT_H */
