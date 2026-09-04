#ifndef SONORA_AUDIO_TRANSPORT_CONTROL_H
#define SONORA_AUDIO_TRANSPORT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "transport_static_config.h"

/*
 * Apply a new commanded rate to one resolved transport leg, then perform the
 * existing mute-bounded restart with a rate-change transition reason.
 *
 * The caller owns choosing the target operating point.  This interface knows
 * only the neutral leg and requested rate; board/codec identity remains an
 * implementation detail until the board adapter phase is completed.
 */
bool audio_transport_reconfigure_leg_rate_hz( transport_leg_t leg,
                                              uint32_t        sample_rate_hz );

/*
 * Can this build put `leg` at `sample_rate_hz` at all?
 *
 * Answers WITHOUT touching the codec or restarting the stream, so a caller can
 * reject an impossible request up front instead of tearing down a working stream
 * and reporting the failure afterwards. Returns false and, when `reason_out` is
 * non-NULL, points it at a short static explanation.
 *
 * The rules live here rather than in the console because they are properties of
 * the resolved transport and the leg's converter role, not of the command syntax.
 * Both apply at the WM8904's own >= 88.2 kHz high-rate boundary; 96 kHz is the only
 * rate the driver offers there, so in practice both are about 96 kHz:
 *   - it needs a 2-slot (I2S) frame; a TDM8 build cannot reach it
 *     (8 x 32 x 96k = 24.576 MHz BCLK is beyond this SYSCLK).
 *   - it cannot run ADC and DAC together, so a leg whose role is ADC+DAC cannot be
 *     moved there.
 *
 * BOTH ARE PROPERTIES OF THIS CODEC, NOT OF THE ASRC.  They are refusals by the
 * TRANSPORT, and they must never be read back as an ASRC specification, nor be
 * allowed to size or restrict any ASRC stage: a codec with a bidirectional TDM8
 * 96 kHz mode removes both without a single ASRC change.  The channel-width rule
 * that keeps that true is THE CHANNEL-WIDTH AUTHORITY in
 * src/app/apps/asrc/asrc_app_config.h.
 */
bool audio_transport_leg_rate_is_supported( transport_leg_t leg,
                                            uint32_t        sample_rate_hz,
                                            const char**    reason_out );

#endif /* SONORA_AUDIO_TRANSPORT_CONTROL_H */
