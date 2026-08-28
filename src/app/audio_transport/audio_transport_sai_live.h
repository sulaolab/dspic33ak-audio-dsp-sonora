#ifndef AUDIO_TRANSPORT_SAI_LIVE_H
#define AUDIO_TRANSPORT_SAI_LIVE_H

//===========================================================
// audio_transport_sai_live.h
//
// Opt-in CMSIS-SAI wrapper LIVE-loopback VERIFICATION harness.
// Isolated out of main.c / audio_transport.c so the production audio path stays clean: this
// file owns the wrapper's RX/TX block buffers, the test-tone generator, the per-block
// instrumentation, and the ARM_SAI event callback. When the resolved live-test
// selection is false this header/TU is empty. NOT part of the public HAL.
//===========================================================

#include "resolved_sai_test_config.h"

#if RESOLVED_SAI_TEST_LIVE_ENABLED

// Drive the stream through the CMSIS-SAI wrapper instead of the demo DSP:
// Initialize + PowerControl(FULL) + Control(CONFIGURE_TX) + arm Receive/Send +
// Control(CONTROL_TX enable). Called by audio_transport_start() for the live-test route.
void audio_transport_sai_live_start( void );

// Print the per-block loopback localiser line ("SAI-LIVE: rxblk/txblk/peaks/busy/...")
// and reset the peak accumulators. Called by the app's debug print under the same gate.
void audio_transport_sai_live_dbg_print( void );

// Tear the CMSIS-SAI wrapper route down (the SINGLE-mode counterpart to _start): disable TX,
// abort any armed Send/Receive, then PowerControl(OFF) -- the wrapper's OFF path runs
// inst_stop(spi1) + close() internally, honouring the HAL's SINGLE ownership contract. Returns
// true only if every wrapper step reported OK (the first failure is kept). Called by
// audio_transport_stop_route() for the CMSIS route.
bool audio_transport_sai_live_stop( void );

#endif // RESOLVED_SAI_TEST_LIVE_ENABLED

#endif //!AUDIO_TRANSPORT_SAI_LIVE_H
