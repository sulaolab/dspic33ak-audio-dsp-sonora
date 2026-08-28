#ifndef SONORA_AUDIO_APPLICATION_TELEMETRY_H
#define SONORA_AUDIO_APPLICATION_TELEMETRY_H

#include <stdint.h>

#include "audio_transport_snapshot.h"

/*
 * Linker-selected application telemetry.  This contract is intentionally
 * separate from audio_transport_client_t: diagnostics are control-plane
 * consumers of the neutral transport snapshot, not real-time callbacks.
 */
void audio_application_telemetry_print(
    const audio_transport_snapshot_t* transport,
    uint32_t                          now_ms,
    uint32_t                          recovery_count );

/*
 * MEASURED sample rate of one logical leg in Hz (leg 0 = A, 1 = B), or 0 when this application
 * measures none -- which is the answer for every co-clocked profile, and the reason the field is
 * omitted from the telemetry line rather than printed as a zero.
 *
 * It exists so the neutral per-leg TDM line can name the rate its resp/margin belong to without
 * the transport layer reaching into an application module (the dependency runs app -> transport,
 * never the other way).
 *
 * EVERY application provides it, like audio_application_telemetry_print above -- deliberately NOT
 * as a weak default in audio_transport.c: a weak definition in the same translation unit as the
 * call is satisfied locally, so the application's strong definition never wins (measured
 * 2026-08-27: the linker kept the 4-byte `return 0`). A missing implementation is a link error,
 * which is the loud failure this contract wants.
 */
uint32_t audio_application_leg_measured_fs_hz( uint8_t leg );

#endif /* SONORA_AUDIO_APPLICATION_TELEMETRY_H */
