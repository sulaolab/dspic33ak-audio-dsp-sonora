#ifndef SONORA_TRANSPORT_STATIC_CONFIG_H
#define SONORA_TRANSPORT_STATIC_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "transport_leg_count.h"

/*
 * Application-neutral transport vocabulary.
 *
 * The *_VALUE constants are usable by the preprocessor and as integer constant
 * expressions.  The enum types give the runtime mirror named, reviewable
 * values without making a const struct the source of DMA geometry.
 */
#define TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE  (1u)
#define TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE     (2u)

typedef enum
{
    TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER =
        TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE,
    TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN =
        TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE,
} transport_topology_t;

/*
 * Leg identity is an index, not a name.  A, B, C, D are the first four values
 * of that index; only the ones this build allocates (TRANSPORT_LEG_MAX) exist.
 * Call sites must keep using these named constants rather than a runtime
 * variable: a leg index that is a compile-time constant lets the compiler fold
 * `legs[TRANSPORT_LEG_B]` into a direct address, which is what keeps a wider
 * capability free for the narrower build.
 */
#define TRANSPORT_LEG_A_VALUE  (0u)
#define TRANSPORT_LEG_B_VALUE  (1u)
#define TRANSPORT_LEG_C_VALUE  (2u)
#define TRANSPORT_LEG_D_VALUE  (3u)

typedef enum
{
    TRANSPORT_LEG_A = TRANSPORT_LEG_A_VALUE,
#if TRANSPORT_LEG_MAX > 1u
    TRANSPORT_LEG_B = TRANSPORT_LEG_B_VALUE,
#endif
#if TRANSPORT_LEG_MAX > 2u
    TRANSPORT_LEG_C = TRANSPORT_LEG_C_VALUE,
#endif
#if TRANSPORT_LEG_MAX > 3u
    TRANSPORT_LEG_D = TRANSPORT_LEG_D_VALUE,
#endif
} transport_leg_t;

#define TRANSPORT_ENDPOINT_NONE_VALUE           (0u)
#define TRANSPORT_ENDPOINT_BOARD_AUDIO_A_VALUE  (1u)
#define TRANSPORT_ENDPOINT_BOARD_AUDIO_B_VALUE  (2u)

typedef enum
{
    TRANSPORT_ENDPOINT_NONE = TRANSPORT_ENDPOINT_NONE_VALUE,
    TRANSPORT_ENDPOINT_BOARD_AUDIO_A = TRANSPORT_ENDPOINT_BOARD_AUDIO_A_VALUE,
    TRANSPORT_ENDPOINT_BOARD_AUDIO_B = TRANSPORT_ENDPOINT_BOARD_AUDIO_B_VALUE,
} transport_endpoint_t;

#define TRANSPORT_CLOCK_SOURCE_NONE_VALUE          (0u)
#define TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE      (1u)
#define TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE    (2u)
#define TRANSPORT_CLOCK_SOURCE_EXTERNAL_VALUE      (3u)
#define TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE (4u)

typedef enum
{
    TRANSPORT_CLOCK_SOURCE_NONE = TRANSPORT_CLOCK_SOURCE_NONE_VALUE,
    TRANSPORT_CLOCK_SOURCE_ENDPOINT = TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE,
    TRANSPORT_CLOCK_SOURCE_CONTROLLER = TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE,
    TRANSPORT_CLOCK_SOURCE_EXTERNAL = TRANSPORT_CLOCK_SOURCE_EXTERNAL_VALUE,
    TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A = TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE,
} transport_clock_source_t;

#define TRANSPORT_CLOCK_PROGRESS_NONE_VALUE          (0u)
#define TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE  (1u)

typedef enum
{
    TRANSPORT_CLOCK_PROGRESS_NONE = TRANSPORT_CLOCK_PROGRESS_NONE_VALUE,
    TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE = TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE,
} transport_clock_progress_source_t;

#define TRANSPORT_FRAME_SYNC_PULSE_VALUE  (1u)
#define TRANSPORT_FRAME_SYNC_50PCT_VALUE  (2u)

typedef enum
{
    TRANSPORT_FRAME_SYNC_PULSE = TRANSPORT_FRAME_SYNC_PULSE_VALUE,
    TRANSPORT_FRAME_SYNC_50PCT = TRANSPORT_FRAME_SYNC_50PCT_VALUE,
} transport_frame_sync_shape_t;

typedef struct
{
    bool                              present;
    uint8_t                           physical_spi_instance;
    transport_endpoint_t              endpoint;
    transport_clock_source_t          clock_source;
    transport_clock_progress_source_t clock_progress_source;
    uint32_t                          controller_clock_hz;
    uint32_t                          controller_brg;
    uint32_t                          initial_nominal_rate_hz;
    uint32_t                          initial_effective_rate_hz;
} transport_leg_static_cfg_t;

typedef struct
{
    transport_topology_t       topology;
    uint16_t                   slots_per_frame;
    uint16_t                   word_bits;
    uint16_t                   block_frames;
    transport_frame_sync_shape_t frame_sync_shape;
    uint8_t                    data_delay_bits;
    transport_leg_static_cfg_t legs[TRANSPORT_LEG_MAX];
} transport_static_cfg_t;

#endif /* SONORA_TRANSPORT_STATIC_CONFIG_H */
