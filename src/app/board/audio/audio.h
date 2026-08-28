#ifndef AUDIO_TRANSPORT_BOARD_H
#define AUDIO_TRANSPORT_BOARD_H

//===========================================================
// board/audio/audio.h
//
// This file is a board adapter example for the Sonora hardware.
// It is not part of the generic SPI/I2S/TDM HAL core.
//
// Phase D5a: board adapter split out of nora_spi_i2s_tdm_dspic33ak.c.
//
// Holds the board-specific PPS/GPIO pin mapping and the CLC pass-through for
// FS/BCLK/MCLK. This is NOT a public driver API. The transport driver
// (nora_spi_i2s_tdm_start) calls these entries instead of the individual
// board functions; behavior,
// device gating, and ordering are unchanged. The PPS pin functions and the
// CLC setup remain private (static) inside board/audio/audio.c.
//===========================================================

#include <stdint.h>
#include <stdbool.h>

#include "nora_spi_i2s_tdm.h"   // nora_spi_i2s_tdm_clock_event_t / _role_t

// These match the HAL core's port hook signatures (nora_spi_i2s_tdm_port_t)
// so audio_app binds them directly. The fallible hooks return false instead of
// trapping, so an unsupported config (e.g. USB-audio + MASTER) aborts start() rather
// than halting the CPU.

// Configure SPI/TDM remappable pins (PPS) and GPIO for the role + current device.
// Returns false for a role this board cannot drive (e.g. the USB-audio MASTER pin
// map is not implemented) or for a pin/PPS routing failure.
bool audio_transport_board_config_pins( nora_spi_i2s_tdm_clock_role_t role );

// Configure CLC pass-through for FS/BCLK/MCLK (AK512 only; no-op elsewhere).
// Returns true (no failure path today); takes role for symmetry/future use.
bool audio_transport_board_clc_passthrough( nora_spi_i2s_tdm_clock_role_t role );

// USB audio clock detection (gated by the resolved board input fact internally).
// _init arms the RB15 change-notification detect (no-op + true when disabled);
// _ready reports clock presence (always true when USB audio input is disabled).
bool audio_transport_board_usb_clock_init( nora_spi_i2s_tdm_clock_role_t role );
bool audio_transport_board_usb_clock_ready( nora_spi_i2s_tdm_clock_role_t role );

// Read-and-clear the next external-clock stop/resume edge detected by the RB15 / CN
// handler. STOPPED (RB15 fall) takes priority over RESUMED (rise) so the app always
// mutes+stops before any restart. Returns NONE when USB audio input is not selected
// (no detect). Called via nora_spi_i2s_tdm_consume_clock_event().
nora_spi_i2s_tdm_clock_event_t audio_transport_board_consume_clock_event( void );

// The board's complete TDM configuration is the system table in
// audio_transport_board_tdm_config.{c,h} (audio_transport_board_tdm_system()). There is no board-side
// default-config builder anymore: audio_transport_board_get_default_config() and its SPI2-master
// variant were removed. The CMSIS wrapper's Driver_SAI_dsPIC33AK_GetDefaultConfig() shim
// (in board/audio/audio.c) hands back leg A of that same table.

// Board/clock PORT table for the HAL core: bind it with
// nora_spi_i2s_tdm_set_port( &audio_transport_board_port ) once before the first
// configure()/start()/is_active(). Const data -- no register wrapper.
extern const nora_spi_i2s_tdm_port_t audio_transport_board_port;

// Application-semantic codec-leg accessors. HAL spiN() names literal physical SPIn,
// while SPI34_TEST deliberately maps dense codec legs 0/1 onto physical SPI3/SPI4.
// Application code therefore addresses codec A/B by dense table position, never by
// a physical-peripheral name.
static inline nora_spi_i2s_tdm_inst_t* audio_transport_tdm_leg_a( void )
{
    return nora_spi_i2s_tdm_inst( 0u );
}

static inline nora_spi_i2s_tdm_inst_t* audio_transport_tdm_leg_b( void )
{
    return nora_spi_i2s_tdm_inst( 1u );
}

#endif //!AUDIO_TRANSPORT_BOARD_H
