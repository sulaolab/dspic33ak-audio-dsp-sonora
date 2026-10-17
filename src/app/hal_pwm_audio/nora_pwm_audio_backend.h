/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_AUDIO_BACKEND_H
#define NORA_PWM_AUDIO_BACKEND_H

/*
 * Internal contract between the portable core (nora_pwm_audio.c) and the one
 * fixed board backend linked into a build. Not for applications.
 *
 * The backend owns clocks, generators, DMA, pins and the power stage, and the
 * three output states:
 *   RUN_NEUTRAL     generators and DMA run, every channel at neutral duty.
 *   STOP_SAFE       the board's no-output state after a normal stop.
 *   EMERGENCY_SAFE  reachable from a fault ISR without waits, logs or I2C.
 *
 * Board -> core calls (nora_pwm_audio_core_*) are the only way duty data and
 * faults reach the core.
 */

#include <stdbool.h>
#include <stdint.h>

#include "nora_pwm_audio.h"
#include "nora_pwm_audio_conf.h"
#include "nora_pwm_audio_modulator.h"

#ifndef NORA_PWM_AUDIO_DUTY_T
#error "nora_pwm_audio_conf.h must define NORA_PWM_AUDIO_DUTY_T"
#endif
typedef NORA_PWM_AUDIO_DUTY_T nora_pwm_audio_duty_t;

/* ---- implemented by the board backend ---------------------------------- */

/* Check channel_count against the board and fill the modulator in duty units
 * that respect the stage's minimum pulse width. The only hardware it may touch
 * is pins, and only to put them in their safe state (stage asleep, unused
 * driver inputs low, fault input configured); no clock, PG, DMA or power
 * stage. Returns OK, UNSUPPORTED (channel count the board cannot drive) or
 * FAULT. A failure returns with every pin it touched in its safe state: the
 * core calls no backend_stop() after a failed backend_init(). */
nora_pwm_audio_result_t nora_pwm_audio_backend_init(
    uint8_t channel_count, nora_pwm_audio_modulator_t *mod);

/* RUN_NEUTRAL across every DMA buffer, outputs held safe, then clock, PG and
 * DMA ready for every channel, then power stage, fault input check, and a
 * synchronous release of all channels. Any failure returns with every channel
 * in STOP_SAFE. */
nora_pwm_audio_result_t nora_pwm_audio_backend_start(void);

/* STOP_SAFE for every channel, including an unused one. Idempotent. */
void nora_pwm_audio_backend_stop(void);

/* ---- implemented by the core, called by the backend --------------------- */

/* DMA refill ISR, once per audio block for all channels together. On true,
 * duty[ch] points at frames_per_block values for each configured channel;
 * call nora_pwm_audio_core_release() after copying them. On false, fill every
 * channel with neutral duty (the core counts it if it was an underrun). */
bool nora_pwm_audio_core_take(const nora_pwm_audio_duty_t *duty[NORA_PWM_AUDIO_MAX_CHANNELS]);
void nora_pwm_audio_core_release(void);

/* True if core_take() would return a block now. A pure query from the same
 * consumer context: it counts nothing and does not change the prime state,
 * so a backend that buffers blocks of its own can drain the core only while
 * this is true and call core_take() just when its own output runs short. */
bool nora_pwm_audio_core_ready(void);

/* Latch a fault category after the backend has already reached
 * EMERGENCY_SAFE. Callable from any ISR. */
void nora_pwm_audio_core_fault(uint32_t category);

#endif /* NORA_PWM_AUDIO_BACKEND_H */
