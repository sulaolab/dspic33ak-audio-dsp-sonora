#ifndef NORA_PWM_AUDIO_CONF_H
#define NORA_PWM_AUDIO_CONF_H

/*
 * board/audio/nora_pwm_audio_conf.h -- project-supplied hal_pwm_audio config.
 *
 * Derived from the app build config, the same way nora_spi_i2s_tdm_conf.h
 * derives NORA_TDM_*. The HAL folder ships only the self-contained
 * nora_pwm_audio_conf.h_example; app-specific headers must not include this.
 *
 * Sonora / Nano dsPIC33AK backend: board/audio/pwm_audio_board_ak.c.
 *   APP_PWM_OUTPUT == 1  MPQ6612A, B or XPRO1 mono / B+XPRO1 stereo, 48/96/192 kHz carrier
 *   APP_PWM_OUTPUT == 0  MIC4607 pair, stereo, PG5+PG6 / DMA4+DMA5, 96 kHz
 */

#include <stdint.h>

#include "app_specific_config_defs.h"

#define NORA_PWM_AUDIO_ENABLE (APP_USE_PWM_AUDIO && APP_PWM_AUDIO_HAL)

#define NORA_PWM_AUDIO_SAMPLE_RATE_HZ   48000u
#define NORA_PWM_AUDIO_FRAMES_PER_BLOCK 32u

#if APP_PWM_OUTPUT == 1
#define NORA_PWM_AUDIO_MAX_CHANNELS (1u + MPQ6612A_STEREO)
#else
#define NORA_PWM_AUDIO_MAX_CHANNELS 2u
#endif

/* The TDM producer and the PWM refill both run from the codec crystal, so
 * there is phase but no drift between them: two queued blocks absorb the
 * producer's ISR jitter. */
#define NORA_PWM_AUDIO_FIFO_BLOCKS  4u
#define NORA_PWM_AUDIO_PRIME_BLOCKS 2u

/* Q4 duty counts: the 48 kHz MPQ period is 266240. */
#define NORA_PWM_AUDIO_DUTY_T uint32_t

#if NORA_PWM_AUDIO_ENABLE
#if (SAMPLE_RATE != 48000) || (APP_BLOCK_FRAMES != 32)
#error "hal_pwm_audio v1 needs 48 kHz audio in 32-frame blocks"
#endif
#endif

#endif /* NORA_PWM_AUDIO_CONF_H */
