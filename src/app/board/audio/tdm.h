#ifndef AUDIO_TRANSPORT_BOARD_TDM_CONFIG_H
#define AUDIO_TRANSPORT_BOARD_TDM_CONFIG_H

//===========================================================
// board/audio/tdm.h
//
// THE single, complete description of this board's TDM transport configuration. Read
// THIS file (and its .c) to see, in ONE place, every leg and every field it runs with --
// no default builder, no clock-profile overlay, no per-leg resolver, no inheritance.
//
// Design (the sync-domain plan, Step 4.5 -- process notes archived outside this repo):
//   - One complete system table (nora_spi_i2s_tdm_leg_setup_t[AUDIO_TDM_LEG_COUNT])
//     is selected at build time and passed ONCE to nora_spi_i2s_tdm_configure_system().
//   - Every field of every leg is stated explicitly in the table. Fields whose value is a
//     build-config fact are written as a resolved transport expression (e.g. the primary
//     clock role/source and controller BRG from resolved_transport_config.h) --
//     NOT copied from another leg, NOT overlaid onto a base, NOT left unset.
//   - No fallback: if the table or a leg cannot be obtained, the caller does not start.
//   Duplication between legs is accepted deliberately: seeing each leg's full configuration
//   in one table beats sharing a base and overriding.
//
// Board-adapter example (not part of the generic SPI/I2S/TDM HAL core).
//===========================================================

#include <stdint.h>
#include <stdbool.h>

#include "nora_spi_i2s_tdm.h"        // nora_spi_i2s_tdm_leg_setup_t / _config_t
#include "resolved_transport_config.h"     // neutral topology/geometry/clock facts

// Semantic leg aliases. The value IS the HAL leg index (0-based, dense), so it indexes
// both the system table and the HAL's per-instance handles. Codec B exists only when a
// second SPI transport is built.
enum {
    AUDIO_TDM_LEG_CODEC_A = 0,
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    AUDIO_TDM_LEG_CODEC_B,
#endif
    AUDIO_TDM_LEG_COUNT
};

// The complete, build-selected TDM system table: one fully-specified leg_setup per leg,
// in leg-index order. Never NULL. Pass directly to nora_spi_i2s_tdm_configure_system()
// together with audio_transport_board_tdm_leg_count(); do not copy-and-edit it.
const nora_spi_i2s_tdm_leg_setup_t* audio_transport_board_tdm_system( void );

// Number of legs in the system table (== AUDIO_TDM_LEG_COUNT).
uint8_t audio_transport_board_tdm_leg_count( void );

// One leg's complete setup (NULL if leg_index is out of range -- fail closed, never a
// fallback to another leg). Read-only board data.
const nora_spi_i2s_tdm_leg_setup_t* audio_transport_board_tdm_leg_setup( uint8_t leg_index );

#endif //!AUDIO_TRANSPORT_BOARD_TDM_CONFIG_H
