#ifndef SONORA_CLASSIC_AUDIO_PWM_H
#define SONORA_CLASSIC_AUDIO_PWM_H

#include <stdbool.h>
#include <stdint.h>

/* Classic-owned optional PWM/DMA audio output. */
void classic_audio_pwm_init( void );
/* MPQ6612A: call from main context while the application guarantees silence.
 * No fades/mute policy. Idempotent start/stop; init starts the output once. */
bool classic_audio_pwm_start(void);
void classic_audio_pwm_stop(void);
bool classic_audio_pwm_fault_active(void);
/* Explicit lab start/stop, without external PCB/power-stage validation. */
bool classic_audio_pwm_lab_control(bool enabled);
void classic_audio_pwm_control_print(void);
/* Scope-only diagnostics: main context service/printing; PCM observation runs
 * on completed interleaved codec output, channel L (and R in stereo), before DMA delivery. */
void classic_audio_pwm_scope_service(void);
void classic_audio_pwm_scope_print(void);
/* Scope producer maps these exact TX values to PWM once per audio block. */
void classic_audio_pwm_scope_process_pcm(const int32_t* pcm, unsigned stride);
void classic_audio_pwm_scope_set_silence(bool silence);
void classic_audio_pwm_process_left_primary( float* samples, int channel_count );
void classic_audio_pwm_process_right_primary( float* samples, int channel_count );
void classic_audio_pwm_process_left_secondary( float* samples, int channel_count );
void classic_audio_pwm_process_right_secondary( float* samples, int channel_count );
/* APP_PWM_AUDIO_HAL == 1: hand the final stereo float block to hal_pwm_audio
 * (L only on a mono board). Replaces the four process_* calls above. */
void classic_audio_pwm_hal_process( const float* samples, int channel_count );

#endif /* SONORA_CLASSIC_AUDIO_PWM_H */
