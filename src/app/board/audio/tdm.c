//===========================================================
// board/audio/tdm.c
//
// THE single, complete description of this board's TDM transport configuration (see the
// header). One build-selected system table states EVERY field of EVERY leg explicitly --
// no default builder, no clock-profile overlay, no per-leg resolver, no leg-to-leg copy,
// no base+override. Config-derived fields are written as named config expressions so the
// table reproduces exactly what the former resolver produced, for every build profile.
//
// Board-adapter example (not part of the generic SPI/I2S/TDM HAL core).
//===========================================================

#include "board/audio/tdm.h"


//===========================================================
// The complete TDM system table. One fully-specified leg_setup per leg. EVERY field is
// present; config-derived fields use the named config macro / expression (identical to the
// former audio_transport_board_get_default_config()) so behaviour is preserved across all builds.
//
//   Leg A : domain 0 anchor. Its clock role is the build's configured role
//           (self-clocked MASTER when the resolved owner is CONTROLLER, else SLAVE).
//   Leg B : co-clocked with A (domain 0, forced SLAVE -- rides A's BCLK/FS) OR, in the ASRC
//           controller-owned independent build, its own dsPIC master (domain 1, MASTER,
//           explicit resolved BRG, short FS_PULSE). Selected by the resolved facts.
//
// Designated initializers keyed by leg index (position == leg index). No field is omitted:
// leaving one out would zero-init it, which this table forbids (see the header contract).
//===========================================================
static const nora_spi_i2s_tdm_leg_setup_t s_tdm_system[AUDIO_TDM_LEG_COUNT] =
{
    [AUDIO_TDM_LEG_CODEC_A] =
    {
        .stream =
        {
            .format = ( RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u )
                          ? NORA_SPI_I2S_TDM_FORMAT_I2S
                          : NORA_SPI_I2S_TDM_FORMAT_TDM,
#if RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
            .clock_role                    = NORA_SPI_I2S_TDM_CLOCK_MASTER,
#else
            .clock_role                    = NORA_SPI_I2S_TDM_CLOCK_SLAVE,
#endif
            .slots_per_fs                  = RESOLVED_TRANSPORT_SLOTS_PER_FRAME,
            .word_bits                     = RESOLVED_TRANSPORT_WORD_BITS,
#if RESOLVED_TRANSPORT_FRAME_SYNC_SHAPE == TRANSPORT_FRAME_SYNC_50PCT_VALUE
            .fs_shape                      = NORA_SPI_I2S_TDM_FS_50PCT,
#else
            .fs_shape                      = NORA_SPI_I2S_TDM_FS_PULSE,
#endif
            .block_frames                  = RESOLVED_TRANSPORT_BLOCK_FRAMES,
            .brg                           = RESOLVED_TRANSPORT_PRIMARY_BRG,
            .mclk_enable                   = true,
#if RESOLVED_TRANSPORT_DATA_DELAY_BITS == 1u
            .fs_coincides_first_bclk       = false,   // SPIFE=0 : 1-bit delayed
#else
            .fs_coincides_first_bclk       = true,    // SPIFE=1 : no delay
#endif
            .bclk_idle_high                = true,    // CKP=1
            .bclk_change_on_active_to_idle = false,   // CKE=0
            // IGNROV/IGNTUR are not caller policy: the HAL keeps both set so a downstream FIFO
            // error cannot critical-stop the leg. DMA OVERRUN is monitored separately as the
            // primary service-failure signal; this does not classify lost data as benign.
        },
        .sync_domain = 0u,
    },

#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    [AUDIO_TDM_LEG_CODEC_B] =
    {
        .stream =
        {
            .format = ( RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u )
                          ? NORA_SPI_I2S_TDM_FORMAT_I2S
                          : NORA_SPI_I2S_TDM_FORMAT_TDM,
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
            // Independent dsPIC TDM master: own BCLK/FS, explicit divider, short pulse FS.
            .clock_role                    = NORA_SPI_I2S_TDM_CLOCK_MASTER,
#else
            // Endpoint-owned or leg-A-inherited clock: this controller leg is a slave.
            .clock_role                    = NORA_SPI_I2S_TDM_CLOCK_SLAVE,
#endif
            .slots_per_fs                  = RESOLVED_TRANSPORT_SLOTS_PER_FRAME,
            .word_bits                     = RESOLVED_TRANSPORT_WORD_BITS,
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
            .fs_shape                      = NORA_SPI_I2S_TDM_FS_PULSE,
#elif RESOLVED_TRANSPORT_FRAME_SYNC_SHAPE == TRANSPORT_FRAME_SYNC_50PCT_VALUE
            .fs_shape                      = NORA_SPI_I2S_TDM_FS_50PCT,
#else
            .fs_shape                      = NORA_SPI_I2S_TDM_FS_PULSE,
#endif
            .block_frames                  = RESOLVED_TRANSPORT_BLOCK_FRAMES,
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
            .brg                           = RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG,
#else
            .brg                           = RESOLVED_TRANSPORT_PRIMARY_BRG,
#endif
            .mclk_enable                   = true,
#if RESOLVED_TRANSPORT_DATA_DELAY_BITS == 1u
            .fs_coincides_first_bclk       = false,
#else
            .fs_coincides_first_bclk       = true,
#endif
            .bclk_idle_high                = true,
            .bclk_change_on_active_to_idle = false,
            // IGNROV/IGNTUR hard-forced to 1 by the HAL (DMA ping-pong); not set here.
        },
#if RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE
        .sync_domain = 1u,   // independent/async to leg A (ASRC data-path relation, not clock sync):
                             //   dsPIC SPI2 master OR endpoint-owned WM8904-B clock. A slave-only domain is
                             //   legal (configure_system forbids only >1 MASTER per domain).
#else
        .sync_domain = 0u,   // co-clocked with leg A
#endif
    },
#endif // RESOLVED_TRANSPORT_LEG_B_PRESENT
};

_Static_assert( ( sizeof(s_tdm_system) / sizeof(s_tdm_system[0]) ) == (unsigned)AUDIO_TDM_LEG_COUNT,
                "TDM system table row count must equal AUDIO_TDM_LEG_COUNT" );


//===========================================================
// Public accessors.
//===========================================================
const nora_spi_i2s_tdm_leg_setup_t* audio_transport_board_tdm_system( void )
{
    return s_tdm_system;
}

uint8_t audio_transport_board_tdm_leg_count( void )
{
    return (uint8_t)AUDIO_TDM_LEG_COUNT;
}

const nora_spi_i2s_tdm_leg_setup_t* audio_transport_board_tdm_leg_setup( uint8_t leg_index )
{
    if( leg_index >= (uint8_t)AUDIO_TDM_LEG_COUNT )
    {
        return NULL;   // fail closed: never fall back to another leg
    }
    return &s_tdm_system[leg_index];
}
