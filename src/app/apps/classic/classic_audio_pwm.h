#ifndef SONORA_CLASSIC_AUDIO_PWM_H
#define SONORA_CLASSIC_AUDIO_PWM_H

/* Classic-owned optional PWM/DMA audio output. */
void classic_audio_pwm_init( void );
void classic_audio_pwm_process_left_primary( float* samples, int channel_count );
void classic_audio_pwm_process_right_primary( float* samples, int channel_count );
void classic_audio_pwm_process_left_secondary( float* samples, int channel_count );
void classic_audio_pwm_process_right_secondary( float* samples, int channel_count );

#endif /* SONORA_CLASSIC_AUDIO_PWM_H */
