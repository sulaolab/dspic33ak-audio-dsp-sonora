/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_AUDIO_MODULATOR_H
#define NORA_PWM_AUDIO_MODULATOR_H

/*
 * Q1.31 sample -> PWM duty, a pure function in the backend's own duty unit.
 * The modulator does not know whether that unit is a raw count or a
 * fractional (e.g. Q4) count.
 *
 * Contract (v1):
 *   period even, 0 <= min_duty <= period/2, max_duty == period - min_duty.
 *   INT32_MIN -> min_duty, 0 -> exactly period/2, INT32_MAX -> max_duty.
 *   Negative samples are normalised by 2^31, positive ones by 2^31-1, and
 *   each is mapped to the offset from centre with round-to-nearest, ties
 *   away from centre. The result never leaves [min_duty, max_duty].
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t period;
    uint32_t min_duty;
    uint32_t max_duty;
} nora_pwm_audio_modulator_t;

/* Fill mod from period and min_duty; false (mod untouched) if they break the
 * v1 contract above or the amplitude needs more than 31 bits. */
bool nora_pwm_audio_modulator_setup(nora_pwm_audio_modulator_t *mod,
                                    uint32_t period, uint32_t min_duty);

uint32_t nora_pwm_audio_modulate_q31(int32_t sample_q31,
                                     const nora_pwm_audio_modulator_t *mod);

static inline uint32_t nora_pwm_audio_modulator_neutral(
    const nora_pwm_audio_modulator_t *mod)
{
    return mod->period / 2u;
}

#endif /* NORA_PWM_AUDIO_MODULATOR_H */
