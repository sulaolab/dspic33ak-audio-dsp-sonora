#include "resolved_transport_config.h"

/*
 * This typedef is intentionally an array bound compile probe.  Geometry remains
 * an integer constant expression; the const mirror below is never the source of
 * DMA sizing.
 */
typedef uint8_t resolved_transport_geometry_compile_probe_t[
    RESOLVED_TRANSPORT_SLOTS_PER_FRAME * RESOLVED_TRANSPORT_BLOCK_FRAMES];

_Static_assert(
    sizeof(resolved_transport_geometry_compile_probe_t) ==
        (APP_SLOTS_PER_FS * APP_BLOCK_FRAMES),
    "resolved transport geometry must remain a compile-time constant" );

const transport_static_cfg_t g_resolved_transport_static_cfg =
{
    .topology = (transport_topology_t)RESOLVED_TRANSPORT_TOPOLOGY,
    .slots_per_frame = (uint16_t)RESOLVED_TRANSPORT_SLOTS_PER_FRAME,
    .word_bits = (uint16_t)RESOLVED_TRANSPORT_WORD_BITS,
    .block_frames = (uint16_t)RESOLVED_TRANSPORT_BLOCK_FRAMES,
    .frame_sync_shape =
        (transport_frame_sync_shape_t)RESOLVED_TRANSPORT_FRAME_SYNC_SHAPE,
    .data_delay_bits = (uint8_t)RESOLVED_TRANSPORT_DATA_DELAY_BITS,
    .legs[TRANSPORT_LEG_A] =
    {
        .present = (RESOLVED_TRANSPORT_LEG_A_PRESENT != 0u),
        .physical_spi_instance = (uint8_t)RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE,
        .endpoint = (transport_endpoint_t)RESOLVED_TRANSPORT_LEG_A_ENDPOINT,
        .clock_source =
            (transport_clock_source_t)RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE,
        .clock_progress_source =
            (transport_clock_progress_source_t)
                RESOLVED_TRANSPORT_LEG_A_CLOCK_PROGRESS,
        .controller_clock_hz = RESOLVED_TRANSPORT_LEG_A_CONTROLLER_CLOCK_HZ,
        .controller_brg = RESOLVED_TRANSPORT_LEG_A_CONTROLLER_BRG,
        .initial_nominal_rate_hz =
            RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ,
        .initial_effective_rate_hz = RESOLVED_TRANSPORT_LEG_A_INITIAL_RATE_HZ,
    },
    .legs[TRANSPORT_LEG_B] =
    {
        .present = (RESOLVED_TRANSPORT_LEG_B_PRESENT != 0u),
        .physical_spi_instance = (uint8_t)RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE,
        .endpoint = (transport_endpoint_t)RESOLVED_TRANSPORT_LEG_B_ENDPOINT,
        .clock_source =
            (transport_clock_source_t)RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE,
        .clock_progress_source =
            (transport_clock_progress_source_t)
                RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS,
        .controller_clock_hz = RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ,
        .controller_brg = RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG,
        .initial_nominal_rate_hz =
            RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ,
        .initial_effective_rate_hz = RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ,
    },
};
