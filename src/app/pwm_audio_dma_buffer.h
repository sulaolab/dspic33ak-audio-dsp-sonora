#ifndef SONORA_PWM_AUDIO_DMA_BUFFER_H
#define SONORA_PWM_AUDIO_DMA_BUFFER_H

#include <stdint.h>

#include "app_specific_config_defs.h"
#include "board/clock/sonora_clock.h"

/*
 * Transitional timing and DMA-buffer contract shared by the common PWM clock
 * setup and the Classic-owned PWM audio implementation. PG5-8 and DMA4-7 are
 * configured entirely by classic_audio_pwm.c.
 */
// #define PWM_AUDIO_ENABLE_SECONDARY_PAIR

/*
 * Carrier = SAMPLE_RATE * PWM_AUDIO_UPSAMPLE_FACTOR. Target is still 10
 * (48000 * 10 = 480kHz, matching the switching frequency range of commercial
 * Class-D amplifier ICs e.g. FDA801B, rather than one PWM pulse per audio
 * sample -- 10 reuses a value the ancestor board's own commented-out history
 * already considered: 8/10/20/30 -> 384k/480k/960k/1.44M).
 *
 * Settled on 2 (96kHz) after real-HW characterization with two MIC4607
 * half-bridge drivers (L/R), 2026-08-17. MIC4607's datasheet (DS20005610E
 * section 6.1) explains why higher factors misbehave: an input pulse
 * narrower than its minimum pulse width (t_PW, typ 50ns) "can result in no
 * output pulse at all", and separately its bootstrap cap must fully recharge
 * within the off-time -- both constraints tighten as the carrier period
 * shrinks. MIC4607 is a three-phase motor-driver part (datasheet performance
 * curves stop at ~100kHz), not an audio Class-D IC.
 *
 * What was actually tried on HW, same two driver boards throughout:
 *   10 (480kHz): one of the two boards produced no output at all.
 *    5 (240kHz): output present but visibly degraded/marginal on that board.
 *    2  (96kHz): clean output on both boards, normal audio playback confirmed.
 * 1 (48kHz) also works cleanly but sits uncomfortably close to the audio
 * band itself, hence preferring 2 over 1 here. Revisiting a higher factor
 * (10 was the original FDA801B-class Class-D target) needs a driver IC
 * actually rated for that switching frequency, not MIC4607 -- no amount of
 * dead-time or firmware timing tuning fixes an input-pulse-width or
 * bootstrap-recharge ceiling.
 *
 * Raising this shrinks PWM_AUDIO_PERIOD_COUNT_Q4 by the same factor, which
 * shrinks PWM duty resolution 1:1 (roughly 18-bit at factor 1 down to ~15-bit
 * at factor 10, on this device's 798.72MHz PWM master clock). convert_float_
 * to_pwm_20bit() (apps/shared/float_conversion.c) is zero-order-hold only, so
 * this factor buys switching-frequency headroom but not any oversampling SNR
 * back -- a real Class-D IC's modulator recovers that via 1st-order
 * error-feedback (noise-shaping). Deliberately deferred (2026-08-16); see
 * float_conversion.c's function comment for where it would go.
 */
#define PWM_AUDIO_UPSAMPLE_FACTOR  (2u)
#if APP_USE_PWM_AUDIO
#define PWM_AUDIO_GENERATOR_CLOCK_HZ  (SONORA_CLOCK_PWM_MCLK_HZ)
#else
#define PWM_AUDIO_GENERATOR_CLOCK_HZ  (FCY)
#endif
#define PWM_AUDIO_PERIOD_COUNT_Q4 \
    ((PWM_AUDIO_GENERATOR_CLOCK_HZ / \
      (SAMPLE_RATE * PWM_AUDIO_UPSAMPLE_FACTOR)) << 4)
#define PWM_AUDIO_DUTY_WORD_COUNT \
    (2u * APP_BLOCK_FRAMES * PWM_AUDIO_UPSAMPLE_FACTOR)

#if APP_TARGET == APP_TARGET_AK512
extern volatile uint32_t g_pwm_audio_pg5_duty[PWM_AUDIO_DUTY_WORD_COUNT];
extern volatile uint32_t g_pwm_audio_pg6_duty[PWM_AUDIO_DUTY_WORD_COUNT];
#if defined(PWM_AUDIO_ENABLE_SECONDARY_PAIR)
extern volatile uint32_t g_pwm_audio_pg7_duty[PWM_AUDIO_DUTY_WORD_COUNT];
extern volatile uint32_t g_pwm_audio_pg8_duty[PWM_AUDIO_DUTY_WORD_COUNT];
#endif
#endif

#endif /* SONORA_PWM_AUDIO_DMA_BUFFER_H */
