#ifndef SONORA_RESOLVED_TRANSPORT_CONFIG_H
#define SONORA_RESOLVED_TRANSPORT_CONFIG_H

/*
 * Transitional compile-time adapter.
 *
 * Existing APP_* macros remain authoritative during Phase 2.  This header is
 * the only place that translates their combinations into the neutral facts
 * consumed by later transport-client and board-adapter phases.
 */
#include "app_specific_config_defs.h"
#include "audio_transport/transport_static_config.h"
#include "board/clock/sonora_clock.h"

#if APP_B_INDEP_DOMAIN
#define RESOLVED_TRANSPORT_TOPOLOGY \
    TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE
#else
#define RESOLVED_TRANSPORT_TOPOLOGY \
    TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE
#endif

#define RESOLVED_TRANSPORT_SLOTS_PER_FRAME  (APP_SLOTS_PER_FS)
#define RESOLVED_TRANSPORT_WORD_BITS         (32u)
#define RESOLVED_TRANSPORT_BLOCK_FRAMES     (APP_BLOCK_FRAMES)
#define RESOLVED_TRANSPORT_BASE_ON_SPI34     (APP_TDM_BASE_ON_SPI34)
#define RESOLVED_TRANSPORT_USE_SPI34_EXPANSION  (APP_USE_SPI34_AUDIO)
#if APP_USE_FS_50PCT
#define RESOLVED_TRANSPORT_FRAME_SYNC_SHAPE \
    TRANSPORT_FRAME_SYNC_50PCT_VALUE
#else
#define RESOLVED_TRANSPORT_FRAME_SYNC_SHAPE \
    TRANSPORT_FRAME_SYNC_PULSE_VALUE
#endif
#if APP_USE_1_BIT_DELAY
#define RESOLVED_TRANSPORT_DATA_DELAY_BITS  (1u)
#else
#define RESOLVED_TRANSPORT_DATA_DELAY_BITS  (0u)
#endif

#define RESOLVED_TRANSPORT_PRIMARY_BCLK_HZ \
    ((uint64_t)RESOLVED_TRANSPORT_SLOTS_PER_FRAME * \
     (uint64_t)RESOLVED_TRANSPORT_WORD_BITS * \
     (uint64_t)RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ)
#define RESOLVED_TRANSPORT_PRIMARY_BRG \
    ((uint32_t)((uint64_t)SONORA_CLOCK_SPI_TDM_CLKGEN9_HZ / \
                (2ULL * RESOLVED_TRANSPORT_PRIMARY_BCLK_HZ)) - 1u)

#define RESOLVED_TRANSPORT_LEG_A_PRESENT               (1u)
#define RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE          (APP_TDM_PHYS_A_NUM)
#define RESOLVED_TRANSPORT_LEG_A_ENDPOINT              \
    TRANSPORT_ENDPOINT_BOARD_AUDIO_A_VALUE
#define RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ  (APP_SAMPLE_RATE_HZ)

#if APP_USE_SPI_TDM_CLK_MASTER
#define RESOLVED_TRANSPORT_LEG_A_CONTROLLER_CLOCK_HZ \
    (SONORA_CLOCK_SPI_TDM_CLKGEN9_HZ)
#define RESOLVED_TRANSPORT_LEG_A_CONTROLLER_BRG \
    (RESOLVED_TRANSPORT_PRIMARY_BRG)
#define RESOLVED_TRANSPORT_LEG_A_INITIAL_RATE_HZ \
    ((uint32_t)((uint64_t)RESOLVED_TRANSPORT_LEG_A_CONTROLLER_CLOCK_HZ / \
                (2ULL * ((uint64_t)RESOLVED_TRANSPORT_LEG_A_CONTROLLER_BRG + 1ULL) * \
                 (uint64_t)RESOLVED_TRANSPORT_SLOTS_PER_FRAME * \
                 (uint64_t)RESOLVED_TRANSPORT_WORD_BITS)))
#else
#define RESOLVED_TRANSPORT_LEG_A_CONTROLLER_CLOCK_HZ  (0u)
#define RESOLVED_TRANSPORT_LEG_A_CONTROLLER_BRG       (0u)
#define RESOLVED_TRANSPORT_LEG_A_INITIAL_RATE_HZ \
    (RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ)
#endif

#if APP_USE_SPI_TDM_CLK_MASTER
#define RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
#elif APP_USE_USB_AUDIO_IN
#define RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_EXTERNAL_VALUE
#else
#define RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
#endif

#if APP_B_INDEP_DOMAIN && APP_USE_CCP_FS_DETECT
#define RESOLVED_TRANSPORT_LEG_A_CLOCK_PROGRESS \
    TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE
#else
#define RESOLVED_TRANSPORT_LEG_A_CLOCK_PROGRESS \
    TRANSPORT_CLOCK_PROGRESS_NONE_VALUE
#endif

#define RESOLVED_TRANSPORT_LEG_B_PRESENT       (APP_USE_SPI2_AUDIO)

#if APP_USE_SPI2_AUDIO
#define RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE  (APP_TDM_PHYS_B_NUM)
#define RESOLVED_TRANSPORT_LEG_B_ENDPOINT      \
    TRANSPORT_ENDPOINT_BOARD_AUDIO_B_VALUE
/*
 * Nominal rate records application intent. Effective rate is derived from the
 * controller clock and BRG when the dsPIC owns the clock; endpoint/external
 * clocks retain their commanded nominal value rather than an assumed measurement.
 */
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ  (APP_SAMPLE_RATE_HZ)

#if APP_USE_SPI2_INDEPENDENT_MASTER
#if APP_Q27B_COHERENT_OFFSET
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ \
    (SONORA_CLOCK_Q27B_CLKGEN9_HZ)
#elif APP_CLK_SPI_ON_PLL2
/* SPI2's transport clock is CLKGEN9 <- PLL2 <- REFI1: 798.72 MHz / 4. */
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ \
    (SONORA_CLOCK_PWM_PLL2_HZ / SONORA_CLOCK_SPI2_PLL2_DIVIDE_BY)
#else
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ  (PLL1_CLK_HZ)
#endif
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG       (APP_SPI2_MASTER_BRG)
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ \
    ((uint32_t)((uint64_t)RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ / \
                (2ULL * ((uint64_t)RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG + 1ULL) * \
                 (uint64_t)RESOLVED_TRANSPORT_SLOTS_PER_FRAME * \
                 (uint64_t)RESOLVED_TRANSPORT_WORD_BITS)))
#elif APP_B_CODEC_MASTER
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ  (0u)
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG       (0u)
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ \
    (RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ)
#else
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ  (0u)
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG       (0u)
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ \
    (RESOLVED_TRANSPORT_LEG_A_INITIAL_RATE_HZ)
#endif

#if APP_USE_SPI2_INDEPENDENT_MASTER
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
#elif APP_B_CODEC_MASTER
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
#else
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE \
    TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE
#endif

#if APP_B_INDEP_DOMAIN && APP_USE_CCP_FS_DETECT
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS \
    TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE
#else
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS \
    TRANSPORT_CLOCK_PROGRESS_NONE_VALUE
#endif

#else
#define RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE     (0u)
#define RESOLVED_TRANSPORT_LEG_B_ENDPOINT         \
    TRANSPORT_ENDPOINT_NONE_VALUE
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE     \
    TRANSPORT_CLOCK_SOURCE_NONE_VALUE
#define RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS   \
    TRANSPORT_CLOCK_PROGRESS_NONE_VALUE
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ  (0u)
#define RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG       (0u)
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ  (0u)
#define RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ  (0u)
#endif

/*
 * Leg-B RX-block INTERRUPT gate -- an ISR nobody needs, costing time in every block period.
 *
 * In the co-clocked single-producer topology leg A's callback fills BOTH legs' TX halves
 * (tx_fill_mirror), and a client that leaves leg_b_process NULL never reads codec B's ADC.
 * Leg B's ISR then runs once per block to advance diagnostics and call nothing. Codec B keeps
 * playing regardless: its audio comes from leg A's mirror, which derives B's write position from
 * leg A's own dst and reads no leg-B ISR state.
 *
 * Gated for the DRC build only, because that build is the one spending its whole block period in
 * the DSP and it is the one whose stage count the freed microseconds buy. Every other config --
 * ASRC above all -- keeps leg B's ISR untouched.
 *
 * The cost is that leg B's ISR-maintained counters freeze (block_count, deadline_miss, FRMERR
 * bookkeeping, load peaks). Everything that reads them is compiled out under this gate rather
 * than left to read zeros: a frozen block_count otherwise reads as a dead leg, which is exactly
 * the symptom a watchdog is built to act on.
 */
#if RESOLVED_TRANSPORT_LEG_B_PRESENT && ENA_DRC_DF2T_CASCADE && \
    ( RESOLVED_TRANSPORT_TOPOLOGY == TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE )
#define RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED  (1)
#else
#define RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED  (0)
#endif

/* Equivalence proofs against the existing authoritative configuration. */
/*
 * The gate is only sound where leg B's ISR has no work: an independent-domain build drives its
 * own leg-B callback from that very ISR, so masking it would silence codec B outright. Pinned
 * here rather than trusted to the #if above, so a future topology change cannot quietly qualify.
 */
_Static_assert(
    ( RESOLVED_TRANSPORT_LEG_B_BLOCK_IRQ_GATED == 0 ) ||
        ( RESOLVED_TRANSPORT_TOPOLOGY ==
          TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE ),
    "leg-B block-IRQ gate requires the co-clocked single-producer topology "
    "(an independent domain drives its own leg-B callback from that ISR)" );
_Static_assert(
    RESOLVED_TRANSPORT_TOPOLOGY ==
        (APP_B_INDEP_DOMAIN
             ? TRANSPORT_TOPOLOGY_INDEPENDENT_DUAL_DOMAIN_VALUE
             : TRANSPORT_TOPOLOGY_CO_CLOCKED_SINGLE_PRODUCER_VALUE),
    "resolved transport topology must match APP_B_INDEP_DOMAIN" );
_Static_assert( RESOLVED_TRANSPORT_SLOTS_PER_FRAME == APP_SLOTS_PER_FS,
                "resolved slots/frame must match APP_SLOTS_PER_FS" );
_Static_assert( RESOLVED_TRANSPORT_BLOCK_FRAMES == APP_BLOCK_FRAMES,
                "resolved block frames must match APP_BLOCK_FRAMES" );
_Static_assert( RESOLVED_TRANSPORT_BASE_ON_SPI34 == APP_TDM_BASE_ON_SPI34,
                "resolved physical SPI remap must match APP_TDM_BASE_ON_SPI34" );
_Static_assert( RESOLVED_TRANSPORT_USE_SPI34_EXPANSION == APP_USE_SPI34_AUDIO,
                "resolved SPI3/4 expansion must match APP_USE_SPI34_AUDIO" );
_Static_assert( RESOLVED_TRANSPORT_WORD_BITS == 32u,
                "resolved transport word width must match current TDM setup" );
_Static_assert( RESOLVED_TRANSPORT_DATA_DELAY_BITS <= 1u,
                "resolved transport supports zero or one data-delay bit" );
_Static_assert(
    RESOLVED_TRANSPORT_PRIMARY_BCLK_HZ <=
        ((uint64_t)SONORA_CLOCK_SPI_TDM_CLKGEN9_HZ / 2ULL),
    "requested primary TDM BCLK exceeds the controller clock range" );
_Static_assert( RESOLVED_TRANSPORT_LEG_A_SPI_INSTANCE == APP_TDM_PHYS_A_NUM,
                "resolved leg A SPI binding must match APP_TDM_PHYS_A_NUM" );
_Static_assert( RESOLVED_TRANSPORT_LEG_B_PRESENT == APP_USE_SPI2_AUDIO,
                "resolved leg B presence must match APP_USE_SPI2_AUDIO" );
_Static_assert( RESOLVED_TRANSPORT_LEG_A_INITIAL_NOMINAL_RATE_HZ == APP_SAMPLE_RATE_HZ,
                "resolved leg A nominal rate must match APP_SAMPLE_RATE_HZ" );
_Static_assert(
    RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE ==
        (APP_USE_SPI_TDM_CLK_MASTER
             ? TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
             : (APP_USE_USB_AUDIO_IN
                    ? TRANSPORT_CLOCK_SOURCE_EXTERNAL_VALUE
                    : TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE)),
    "resolved leg A clock source must match the current clock owner" );
_Static_assert(
    RESOLVED_TRANSPORT_LEG_A_CLOCK_PROGRESS ==
        ((APP_B_INDEP_DOMAIN && APP_USE_CCP_FS_DETECT)
             ? TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE
             : TRANSPORT_CLOCK_PROGRESS_NONE_VALUE),
    "resolved leg A progress binding must match the current detector" );

#if APP_USE_SPI2_AUDIO
_Static_assert( RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE == APP_TDM_PHYS_B_NUM,
                "resolved leg B SPI binding must match APP_TDM_PHYS_B_NUM" );
_Static_assert( RESOLVED_TRANSPORT_LEG_B_INITIAL_NOMINAL_RATE_HZ == APP_SAMPLE_RATE_HZ,
                "resolved leg B nominal rate must match current startup intent" );
#if APP_USE_SPI2_INDEPENDENT_MASTER
_Static_assert( RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ != 0u,
                "controller-owned leg B requires a controller clock fact" );
_Static_assert(
    RESOLVED_TRANSPORT_LEG_B_INITIAL_RATE_HZ ==
        (uint32_t)((uint64_t)RESOLVED_TRANSPORT_LEG_B_CONTROLLER_CLOCK_HZ /
                   (2ULL * ((uint64_t)RESOLVED_TRANSPORT_LEG_B_CONTROLLER_BRG + 1ULL) *
                    (uint64_t)RESOLVED_TRANSPORT_SLOTS_PER_FRAME *
                    (uint64_t)RESOLVED_TRANSPORT_WORD_BITS)),
    "controller-owned leg B effective rate must be derived from clock and BRG facts" );
#endif
_Static_assert(
    RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE ==
        (APP_USE_SPI2_INDEPENDENT_MASTER
             ? TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
             : (APP_B_CODEC_MASTER
                    ? TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
                    : TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE)),
    "resolved leg B clock source must match the current clock owner" );
_Static_assert(
    RESOLVED_TRANSPORT_LEG_B_CLOCK_PROGRESS ==
        ((APP_B_INDEP_DOMAIN && APP_USE_CCP_FS_DETECT)
             ? TRANSPORT_CLOCK_PROGRESS_EDGE_CAPTURE_VALUE
             : TRANSPORT_CLOCK_PROGRESS_NONE_VALUE),
    "resolved leg B progress binding must match the current detector" );
#else
_Static_assert( RESOLVED_TRANSPORT_LEG_B_SPI_INSTANCE == 0u,
                "an absent leg B must not bind a physical SPI instance" );
_Static_assert( RESOLVED_TRANSPORT_LEG_B_ENDPOINT == TRANSPORT_ENDPOINT_NONE_VALUE,
                "an absent leg B must not bind an endpoint" );
_Static_assert( RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE ==
                    TRANSPORT_CLOCK_SOURCE_NONE_VALUE,
                "an absent leg B must not bind a clock source" );
#endif

#if APP_B_INDEP_DOMAIN
_Static_assert( RESOLVED_TRANSPORT_LEG_B_PRESENT == 1u,
                "independent dual-domain topology requires leg B" );
_Static_assert(
    RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE !=
        TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE,
    "independent leg B must own an explicit clock source" );
#elif APP_USE_SPI2_AUDIO
_Static_assert(
    RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE ==
        TRANSPORT_CLOCK_SOURCE_INHERIT_LEG_A_VALUE,
    "co-clocked leg B must inherit leg A clock ownership" );
#endif

#define RESOLVED_TRANSPORT_CONFIG_READY  (1)

extern const transport_static_cfg_t g_resolved_transport_static_cfg;

#endif /* SONORA_RESOLVED_TRANSPORT_CONFIG_H */
