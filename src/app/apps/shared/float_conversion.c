
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>   // fminf / fmaxf

#include "apps/shared/float_conversion.h"


//===========================================================
// Definition
//===========================================================
#define ENA_DBG_SANITY_CHK


// Q31 fixed-point scaling constants. Q31_SCALE_FLOAT now lives in float_conversion.h (shared
// with the LED level meter int32 path); Q31_SCALE_INT stays local (only used in this file).
#define Q31_SCALE_INT             (2147483648.0f)         // float -> int32 (Q31)

/* The gain set below is selected by SONORA_APP_IS_ASRC. If that macro is not
 * visible here the #if would read it as 0 and quietly define the Classic gains
 * again -- the "silently back to the wider default" failure this project has
 * been bitten by before -- so require it. */
#if !defined( SONORA_APP_IS_ASRC )
#error "float_conversion.c requires app_specific_config_defs.h (SONORA_APP_IS_ASRC)."
#endif


//===========================================================
// Variables -- audio gains (used here + by synth / wav modules)
//===========================================================

/*
 * Pre_Gain_CODEC is the one gain every application reads: the ASRC build takes
 * it in the LED level meter (LED_level_meter.c), the Classic build in
 * convert_codec_int_to_float() below.
 *
 * Five of the rest belong to Classic-only sources: the WAV player and the four
 * synths (snd_effect_play.c, engine_synth.c, avas_synth_*.c, clickclack_synth.c,
 * kinkon_synth.c). An ASRC build has none of them: app_specific_config_defs.h
 * defines the whole Classic feature set only when APP_PROFILE != APP_PROFILE_ASRC,
 * and nbproject/configurations.xml excludes classic_audio_path.c /
 * classic_audio_pwm.c / snd_effect_play.c from the ASRC configurations.
 * Measured on 2026-08-22 (AK512 ASRC BiDir): convert_codec_int_to_float,
 * convert_codec_float_to_int and convert_float_to_pwm_20bit are all in the
 * linker's discarded list there, so these five were written once at start-up and
 * read by nothing, holding 20 B of data memory plus their init code and printf
 * strings ([internal] report_ak512_asrc_ram_gate_2026-08-22.md).
 *
 * Gating the definitions rather than only the writes is deliberate: a Classic
 * symbol that reappears in an ASRC build fails to link, which is the report we
 * want, instead of silently reading a gain nothing maintains.
 */
float Pre_Gain_CODEC;
/* Post_Gain_CODEC / Post_Gain_PWM stay unconditional: convert_codec_float_to_int()
 * and convert_float_to_pwm_20bit() below read them, and both are COMPILED in every
 * profile -- an ASRC image drops them at LINK time, which a #if cannot see. Gating
 * these two would mean gating those functions as well; that is 8 B, not worth
 * widening the change for. */
float Post_Gain_PWM;
float Post_Gain_CODEC;
#if !SONORA_APP_IS_ASRC
float Pre_Gain_WAV;
float Gain_EngineSynth;
float Gain_AvasSynth;
float Gain_ClickClack;
float Gain_KinKon;
#endif /* !SONORA_APP_IS_ASRC */


//===========================================================
// Global Function
//===========================================================

/**
 * @brief Build the application-wide audio gains from the compile-time dB constants.
 *
 * NOTE: this is gain setup, not a sample conversion (the convert_* functions are
 * stateless). The gains live in this module for convenience and are also read by
 * the synth / wav / PWM / LED-meter modules.
 *
 * Rate independent: every value comes from a compile-time dB constant, so the result
 * is identical on every call. The done flag makes the function idempotent for two
 * reasons at once -- a forgotten boot call cannot leave the gains at 0.0 (any later
 * caller still initialises them), and the transport's fs-parameterized prepare hook
 * can keep calling it without recomputing eight powf() or reprinting the table on
 * every A-leg rate change.
 */
void audio_gains_init( void )
{
    static bool s_gains_ready = false;

    if( s_gains_ready )
    {
        return;
    }
    s_gains_ready = true;

    Pre_Gain_CODEC     = db_to_lin(PRE_GAIN_CODEC_DB    );
    Post_Gain_CODEC    = db_to_lin(POST_GAIN_CODEC_DB   );
    Post_Gain_PWM      = db_to_lin(POST_GAIN_PWM_DB     );
#if !SONORA_APP_IS_ASRC
    Pre_Gain_WAV       = db_to_lin(PRE_GAIN_WAV_DB      );
    Gain_EngineSynth   = db_to_lin(PRE_GAIN_ENG_SYNTH_DB);
    Gain_AvasSynth     = db_to_lin(PRE_GAIN_AVAS_SYNTH_DB);
    Gain_ClickClack    = db_to_lin(-30.0f               );
    Gain_KinKon        = db_to_lin(-15.0f               );
#endif /* !SONORA_APP_IS_ASRC */
}


/**
 * @brief Print the gains built by audio_gains_init.
 *
 * Kept out of audio_gains_init so the boot path decides whether the log carries the
 * table; no audio path depends on this. The Classic gains are printed only where
 * they exist -- an ASRC build does not define them at all.
 */
void audio_gains_print( void )
{
    printf(" Pre_Gain_CODEC     = %.5f\n", Pre_Gain_CODEC);
    printf(" Post_Gain_CODEC    = %.5f\n", Post_Gain_CODEC);
    printf(" Post_Gain_PWM      = %.5f\n", Post_Gain_PWM);
#if !SONORA_APP_IS_ASRC
    printf(" Pre_Gain_WAV       = %.5f\n", Pre_Gain_WAV);
    printf(" Gain_EngineSynth   = %.5f\n", Gain_EngineSynth);
    printf(" Gain_AvasSynth     = %.5f\n", Gain_AvasSynth);
    printf(" Gain_ClickClack    = %.5f\n", Gain_ClickClack);
    printf(" Gain_KinKon        = %.5f\n", Gain_KinKon);
#endif /* !SONORA_APP_IS_ASRC */
}


/**
 * @brief Convert interleaved Q31 (int32_t) codec audio to ch-major normalized floats.
 *
 * Layout:
 *  - Input : int_in[n * channels_in  + ch]   (interleaved, codec/DMA buffer)
 *  - Output: float_out[ch * frameSize + n]   (ch-major: float buf[channels_out][frameSize])
 *
 * Behavior:
 *  - Converts the first min(channels_in, channels_out) channels; extra output
 *    channels are zero-filled.
 *  - Each sample is scaled by Q31_SCALE_FLOAT * Pre_Gain_CODEC and hard-clipped to
 *    [-1.0f, 0.99999994f]. The lower 8 bits are masked off (WM8904 is 24-bit).
 */
void convert_codec_int_to_float(const int32_t* __restrict int_in,
                                int            channels_in,
                                float* __restrict float_out,
                                int            channels_out,
                                int            frameSize)
{
#if defined(ENA_DBG_SANITY_CHK)
    if (!int_in || !float_out || channels_in <= 0 || channels_out <= 0 || frameSize <= 0)
        return;
#endif //defined(ENA_DBG_SANITY_CHK)

    const int read_ch = (channels_in < channels_out) ? channels_in : channels_out;

    /*
     * Fast ISR path.
     * The CODEC data is effectively 24-bit and the DSP chain is float32, so
     * staying in float avoids the double conversion overhead in the DMA ISR.
     */
    const float combined_scale = Q31_SCALE_FLOAT * Pre_Gain_CODEC;

    for (int ch = 0; ch < read_ch; ch++) {
        const int32_t* __restrict in_ch  = &int_in[ch];
              float*   __restrict out_ch = &float_out[ch * frameSize];

        for (int n = 0; n < frameSize; n++) {
            // WM8904 valid 24-bit data: clear unused lower 8 bits
            int32_t raw_val = (*in_ch) & 0xFFFFFF00UL;

            float x = (float)raw_val * combined_scale;

            if (x < -1.0f) x = -1.0f;
            else if (x > 0.99999994f) x = 0.99999994f;

            *out_ch++ = x;
            in_ch += channels_in;
        }
    }

    for (int ch = read_ch; ch < channels_out; ch++) {
        float* __restrict out_ch = &float_out[ch * frameSize];

        for (int n = 0; n < frameSize; n++) {
            out_ch[n] = 0.0f;
        }
    }
}


/**
 * @brief Convert ch-major normalized float audio to interleaved Q31 int32 for the codec.
 *
 * Layout:
 *  - Input : float_in[ch * frameSize + n]   (ch-major: float buf[channels_in][frameSize])
 *  - Output: int_out[n * channels_out + ch]     (interleaved, codec/DMA buffer)
 *
 * Behavior:
 *  - Converts the first min(channels_in, channels_out) channels; extra output slots
 *    are zero-filled (output stride is channels_out, so interleaving stays correct).
 *  - Each sample is scaled by Post_Gain_CODEC * Q31_SCALE_INT, clamped to the Q31
 *    range, then masked to the WM8904 24-bit boundary.
 */
void convert_codec_float_to_int(const float* __restrict float_in,
                                int            channels_in,
                                int32_t* __restrict int_out,
                                int            channels_out,
                                int            frameSize)
{
#if defined(ENA_DBG_SANITY_CHK)
    if (!float_in || !int_out || channels_in <= 0 || channels_out <= 0 || frameSize <= 0)
        return;
#endif //defined(ENA_DBG_SANITY_CHK)

    const int read_ch = (channels_in < channels_out) ? channels_in : channels_out;

    /*
     * Fast ISR path. q31_f is already in Q31 scale (gain folded into the scale):
     *   q31_f = sample * (Post_Gain_CODEC * Q31_SCALE_INT)
     */
    const float scale     = Post_Gain_CODEC * Q31_SCALE_INT;
    const float q31_max_f = 2147483520.0f;   // 0.99999994f * 2^31
    const float q31_min_f = -2147483648.0f;  // INT32_MIN equivalent

    for (int n = 0; n < frameSize; n++) {
        int32_t* __restrict out_frame = &int_out[n * channels_out];

        /*
         * Avoid recomputing &float_in[ch * frameSize] each inner iteration;
         * in_ch advances one ch-major channel block per channel.
         */
        const float* __restrict in_ch = float_in;

        for (int ch = 0; ch < read_ch; ch++) {
            float q31_f = in_ch[n] * scale;

            // Fast path: most samples are in range; clip only when actually out.
            if ((q31_f > q31_max_f) || (q31_f < q31_min_f))
            {
                q31_f = (q31_f > 0.0f) ? q31_max_f : q31_min_f;
            }

            // Fast truncating conversion, then align to WM8904 24-bit boundary.
            int32_t q31 = (int32_t)q31_f;
            out_frame[ch] = q31 & 0xFFFFFF00UL;

            in_ch += frameSize;
        }

        for (int ch = read_ch; ch < channels_out; ch++) {
            out_frame[ch] = 0;
        }
    }
}


/**
 * @brief Convert one ch-major float channel (-1.0f..+1.0f) to 20-bit PWM duty values.
 *
 * Layout:
 *  - Input : float_in[slot * num_samples + sample_idx]   (ch-major)
 *  - Output: output[sample_idx * up_sample_factor + k]
 *
 * Applies Post_Gain_PWM, maps [-1,+1] -> [duty_min, duty_max] around centre, and
 * zero-order-hold upsamples by up_sample_factor. The 0x100 guard band at both ends
 * respects the dsPIC33AK PWM register limits.
 *
 * ZOH means raising up_sample_factor to push the PWM carrier above the
 * sample rate (e.g. pwm_audio_dma_buffer.h's PWM_AUDIO_UPSAMPLE_FACTOR)
 * trades duty resolution for switching frequency 1:1 with no SNR recovery:
 * per_count shrinks by the same factor the carrier rises, and every one of
 * the up_sample_factor sub-periods gets the SAME rounded value, so more
 * pulses per sample buys nothing back here. A real Class-D IC's digital
 * modulator recovers that resolution with a 1st-order error-feedback
 * (noise-shaping) quantizer: carry each sample's rounding error into the
 * next sub-period's target instead of dropping it, per channel (this
 * function has no persistent state today). Deliberately deferred
 * (2026-08-16) until real hardware makes a listening comparison possible --
 * not implemented, not forgotten.
 *

 * @param float_in      ch-major input buffer [channels_in][num_samples]
 * @param channels_in       number of channels in the ch-major buffer
 * @param slot              channel index to convert
 * @param output            duty-count output buffer
 * @param num_samples       samples per channel
 * @param up_sample_factor  upsample factor (PWM carrier = 48000 * up_sample_factor)
 * @param per_count         PWM period count (20-bit max: up to 1048575)
 */
void convert_float_to_pwm_20bit(const float* float_in,
                                int          channels_in,
                                uint8_t      slot,
                                int32_t*     output,
                                size_t       num_samples,
                                uint8_t      up_sample_factor,
                                uint32_t     per_count)
{
#if defined(ENA_DBG_SANITY_CHK)
    if (!float_in || !output || channels_in <= 0 || num_samples == 0u || up_sample_factor == 0u)
        return;

    if ((int)slot >= channels_in)
        return;
#endif //defined(ENA_DBG_SANITY_CHK)

    const double duty_min_d = 256.0;                      // 0x100: dsPIC33AK512 min PWM reg val
    const double duty_max_d = (double)per_count - 256.0;  // per_count - 0x100
    const double duty_center_d    = (duty_min_d + duty_max_d) * 0.5;
    const double duty_amp_d       = (duty_max_d - duty_min_d) * 0.5;

    const float duty_min    = (float)duty_min_d;
    const float duty_max    = (float)duty_max_d;
    const float duty_center = (float)duty_center_d;
    const float duty_amp    = (float)duty_amp_d;

    const float* __restrict in_ch = &float_in[(size_t)slot * num_samples];

    for (size_t sample_idx = 0; sample_idx < num_samples; sample_idx++)
    {
        // Apply gain + clamp input
        float data = in_ch[sample_idx] * Post_Gain_PWM;
        data = fminf(1.0f, fmaxf(-1.0f, data));

        // Scale: -1.0 -> duty_min, 0.0 -> centre, +1.0 -> duty_max, then clamp.
        float duty_f = duty_center + data * duty_amp;
        duty_f = fminf(duty_max, fmaxf(duty_min, duty_f));

        // Zero-Order Hold upsampling
        for (uint8_t k = 0; k < up_sample_factor; k++)
        {
            output[sample_idx * (size_t)up_sample_factor + k] =
                (uint32_t)(duty_f + 0.5f);  // round to nearest
        }
    }
}
