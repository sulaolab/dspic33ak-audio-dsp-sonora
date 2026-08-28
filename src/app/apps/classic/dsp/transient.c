#if 0
/**
 * @file transient.c
 * @brief Implementation of block-based transient shaping with dynamic division.
 *
 * This module enhances the attack of an audio signal by applying gain boosts
 * to blocks of samples, based on the difference between short-term and long-term
 * envelope followers. The short-term envelope reacts quickly to transients, while
 * the long-term envelope provides a smoothed baseline.
 *
 * The algorithm operates in blocks, calculating an average gain for each block
 * and applying it to all samples in that block. Gain smoothing is also applied
 * to avoid abrupt changes between blocks.
 *
 * Adjustable parameters include:
 *  - alpha_short: responsiveness of the short-term envelope
 *  - alpha_long: responsiveness of the long-term envelope
 *  - shape_gain: scaling factor for the transient boost
 *  - threshold: minimum detectable envelope difference
 *
 * This implementation is designed for multi-channel, TDM-interleaved audio buffers.
 */

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>   // for fmaxf
#include "apps/shared/float_conversion.h"
#include "gain_ctrl.h"


#include "transient.h"





//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// Variables
//===========================================================

static transient_t My_Transient;




//===========================================================
// Global Function
//===========================================================

void app_transient_init( void )
{
    transient_init(&My_Transient);
}


void app_transient_toggle( void )
{
    My_Transient.ena_transient ^= 1;

    printf("Enable Transient = %d\n", My_Transient.ena_transient);
}


void app_transient_process(float* out, const float* in)
{
    if( !My_Transient.ena_transient )
    {
         return; // Bypass if disabled (debug)
    }
    transient_process_dynamic(&My_Transient, out, in );
}


void transient_init(transient_t* t)
{
    t->ena_transient = 0;   // Default off

    t->num_proc_ch     = MAX_TRANSIENT_CHANNELS;

    t->divisions     = 8;   // Number of blocks per 1 Fs
    if( t->divisions > APP_BLOCK_FRAMES )
    {
        t->divisions = APP_BLOCK_FRAMES;   // Limit to max block count
    }
    t->samples_per_blk = APP_BLOCK_FRAMES / t->divisions;

    memset(t->env_short, 0, sizeof(t->env_short));
    memset(t->env_long,  0, sizeof(t->env_long));
    memset(t->lpf_env,   0, sizeof(t->lpf_env));

    for (uint8_t ch = 0; ch < t->num_proc_ch; ++ch)
    {
        t->gain[ch] = 1.0f;
    }

    // Tunable parameters (adjust as needed)
//     t->alpha_short = 0.95f;   // Very fast response (~1 sample)
//     t->alpha_long  = 0.005f;  // Slow baseline (~100 samples)
//     t->shape_gain  = 20.0f;   // Strong transient emphasis
//     t->threshold   = 0.0005f; // Detect very small deltas

//      t->alpha_short = 0.6f;    // Very fast response (~1 sample)
// //    t->alpha_short = 0.3f;    // Moderate fast attack detection (~3-5 samples)
// //    t->alpha_short = 0.2f;    // Moderate fast attack detection (~3-5 samples)
//     t->alpha_long  = 0.01f;    // Smooth baseline (~100 samples)
// //    t->shape_gain  = 8.0f;    // Medium transient boost (less distortion)
// //    t->shape_gain  = 12.0f;   // Medium transient boost (less distortion)
//     t->shape_gain  = 18.0f;    // Medium transient boost (less distortion)
// //    t->threshold   = 0.0015f; // Ignore small deltas for noise suppression
//     t->threshold   = 0.001f;   // Ignore small deltas for noise suppression

    t->alpha_short = 0.8f;     // Short-term envelope (fast response)
    t->alpha_long  = 0.005f;   // Long-term envelope (smooth baseline)
    t->shape_gain  = 6.0f;     // Boost amount for transients
//    t->shape_gain  = 18.0f;   // Stronger transient boost
//    t->threshold   = 0.001f;  // Sensitivity lower bound
//    t->threshold   = 0.005f;  // Sensitivity lower bound
    t->threshold   = 0.01f;    // Sensitivity lower bound (ignore small differences)
}


void transient_process_dynamic(transient_t* t, float* out, const float* in)
{
    if (!t || !in || !out) return;

    const float lpf_alpha = 0.0131f;  // = 2*pi*100/48000

    for (uint16_t block_idx = 0; block_idx < t->divisions; ++block_idx)
    {
        uint16_t block_start = block_idx   * t->samples_per_blk;
        uint16_t block_end   = block_start + t->samples_per_blk;

        for (uint8_t ch = 0; ch < t->num_proc_ch; ++ch)
        {
            float gain = 1.0f;

            // --- Envelope tracking for this block
            for (uint16_t s = block_start; s < block_end; ++s)
            {
                uint16_t idx = s * t->num_proc_ch + ch;
                float x = in[idx];

                float abs_x = fabsf(x);
                t->lpf_env[ch] = lpf_alpha * abs_x + (1.0f - lpf_alpha) * t->lpf_env[ch];
                float filtered_abs = t->lpf_env[ch];

                t->env_short[ch] += t->alpha_short * (filtered_abs - t->env_short[ch]);
                t->env_long[ch]  += t->alpha_long  * (filtered_abs - t->env_long[ch]);

                float delta = t->env_short[ch] - t->env_long[ch];
                if (delta < t->threshold)
                {
                    delta = 0.0f;
                }

//                gain = 1.0f + delta * t->shape_gain;
//                float shaped_delta = delta / (1.0f + delta);        // Soft saturation
//                float shaped_delta = delta / (1.0f + delta*delta);  // Soft saturation
//                float shaped_delta = delta / (1.0f + 0.3f * delta); // Soft saturation
//                float shaped_delta = delta / (1.0f + 0.5f * delta); // Soft saturation
                float shaped_delta = delta / (1.0f + 0.8f * delta);   // Soft saturation
                gain = 1.0f + shaped_delta * t->shape_gain;
            }

            // Smooth gain transition between blocks
            float gain_smooth_coeff = 0.3f;
            gain = (gain_smooth_coeff * gain) + ((1.0f - gain_smooth_coeff) * t->gain[ch]);

            // --- Apply gain to this block
            for (uint16_t s = block_start; s < block_end; ++s)
            {
                uint16_t idx = s * t->num_proc_ch + ch;
                out[idx] = in[idx] * gain;
            }

            // Store last gain value
            t->gain[ch] = gain;
        }
    }
}
#endif //01
