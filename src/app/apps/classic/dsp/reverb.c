#if 0
/**
 * @file reverb_ctrl.c
 * @brief Simple feedback delay-based reverb effect for interleaved audio.
 *
 * This module implements a lightweight reverb using one comb-filter style
 * delay line per channel. Feedback gain, wet/dry mix, and delay length are
 * configurable at runtime.
 *
 * Design characteristics:
 *   - Each processed channel has its own circular delay buffer and write index.
 *   - Delay buffer memory and index arrays must be supplied externally to
 *     allow flexible integration and memory management.
 *   - Processing operates on interleaved multi-channel (TDM) audio frames.
 *   - Anti-denormal handling is applied to prevent floating-point performance
 *     issues when signals decay toward silence.
 *
 * Typical use:
 *   1. Allocate delay buffer and index arrays sized by the number of channels.
 *   2. Call @c reverb_init() once at startup.
 *   3. Process audio blocks with @c reverb_process().
 *   4. Adjust parameters (wet/dry mix, delay time, feedback gain) as needed.
 *
 * This implementation is intended for simple ambience or coloration effects,
 * not for high-quality convolution reverb.
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

#include "timer_app.h"
#include "apps/shared/float_conversion.h"
#include "gain_ctrl.h"


#include "reverb.h"





//===========================================================
// Definition
//===========================================================

// Define maximum delay size in samples
#if APP_TARGET == APP_TARGET_AK128
// #define MAX_DELAY_MS          20     // msec
 #define MAX_DELAY_MS          10     // msec
 #define DEFAULT_DELAY_MS      10     // msec

#else

#if !defined(ENA_REVERB2)
// #define MAX_DELAY_MS          130    // msec
 #define MAX_DELAY_MS          80     // msec
 #define DEFAULT_DELAY_MS      50     // msec
#else
 #define MAX_DELAY_MS          10     // msec
 #define DEFAULT_DELAY_MS      10     // msec
#endif //!defined(ENA_REVERB2)

#endif //APP_TARGET == APP_TARGET_AK128



//#define GET_DELAY_CNT(msec)    (48000 / (1000/(float)msec))  // unit: count (compiler makes warning)
#define GET_DELAY_CNT(msec)    (48000 * (msec) / 1000)  // unit: count

#define MAX_DELAY_SAMPLES      GET_DELAY_CNT(MAX_DELAY_MS)

// Prevents denormalized floating-point values from causing performance issues
#define ANTI_DENORMAL(x)       (((x) > -1.0e-15f && (x) < 1.0e-15f) ? 0.0f : (x))




//===========================================================
// Enum & Struct typedef
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

void simple_reverb_init(float delay_ms, float wetMix, float fbGain);
void simple_reverb_process(const float* in, float* out, int samples);







//===========================================================
// Variables
//===========================================================


//===========================================================
// Global Function
//===========================================================


static reverb_t   MyReverb;

void app_reverb_init(void)
{
#define REVERB_PROC_CH       STAGE_1_PROC_CH

    static float      ReverbBuf[REVERB_PROC_CH * MAX_DELAY_SAMPLES];
    static int        ReverbIdx[REVERB_PROC_CH];

    // Clear all buffer and write index memory
    memset(ReverbBuf, 0, sizeof(ReverbBuf));
    memset(ReverbIdx, 0, sizeof(ReverbIdx));

    reverb_init(&MyReverb,          // reverb_t* rv
                 ReverbBuf,         // float* buf
                 ReverbIdx,         // int* wridx
                 0.35f,             // float feedbackGain
                 0.5f,              // float wetMix
                 REVERB_PROC_CH );

}

void app_reverb_process( const float* in, float* out )
{
    reverb_process(&MyReverb, in, out, APP_BLOCK_FRAMES);
}

void app_reverb_enabled( int dly_ms )
{
    printf(" %3d(ms) (MAX:%dms) @%ld\n", dly_ms, MAX_DELAY_MS, GetTicks());

    reverb_set_enabled_drywet(&MyReverb, dly_ms);
}







/**
 * @brief Enable or disable reverb by changing wet/dry mix parameters.
 *
 * @param rv Pointer to reverb_t structure
 */
void reverb_set_enabled_drywet(reverb_t* rv, int dly_ms)
{
    if( dly_ms > MAX_DELAY_MS )
    {
        rv->delaySamples = GET_DELAY_CNT( MAX_DELAY_MS );
    }
    else
    {
        rv->delaySamples = GET_DELAY_CNT( dly_ms );
    }

    if( dly_ms != 0 )
    {
        rv->wetMix = rv->wetMix_orig;
        rv->dryMix = 1.0f - rv->wetMix_orig;
    }
    else
    {
        rv->wetMix = 0.0f;
        rv->dryMix = 1.0f;
    }
}


/**
 * @brief Initialize a @c reverb_t processor instance.
 *
 * Sets default parameters and assigns external buffer memory for the delay lines.
 * Each channel uses its own circular buffer of length MAX_DELAY_SAMPLES.
 *
 * Behavior:
 *   - Sets initial delay length to DEFAULT_DELAY_MS.
 *   - Configures feedback gain and wet/dry mix (dryMix = 1 - wetMix).
 *   - Stores pointers to externally allocated delay buffer and write index array.
 *   - Disables the effect by default (wetMix = 0, dryMix = 1).
 *
 * @param rv           Pointer to @c reverb_t instance
 * @param buf          Pointer to external float buffer of size [MAX_DELAY_SAMPLES * num_proc_ch]
 * @param wridx        Pointer to external index array of size [num_proc_ch]
 * @param feedbackGain Feedback gain coefficient (e.g. 0.3f)
 * @param wetMix       Initial wet signal ratio (0.0f to 1.0f)
 * @param num_proc_ch    Number of interleaved channels (TDM slots) to process
 */
void reverb_init(reverb_t* rv, float* buf, int* wridx, float feedbackGain, float wetMix, int num_proc_ch)
{
    rv->num_proc_ch  = num_proc_ch;

    rv->delaySamples = GET_DELAY_CNT( DEFAULT_DELAY_MS );
    rv->feedbackGain = feedbackGain;

    rv->wetMix_orig  = wetMix;
    rv->wetMix       = wetMix;
    rv->dryMix       = 1.0f - rv->wetMix;

    rv->delayBuf     = buf;
    rv->wrIdx        = wridx;

    // disable reverb as default
    reverb_set_enabled_drywet(rv, 0);
}


/**
 * @brief Process audio through the reverb effect.
 *
 * Applies a simple feedback delay-based reverb to an interleaved
 * multi-channel (TDM) audio buffer. Each channel uses its own
 * circular delay line with feedback, and the result is mixed
 * according to the current wet/dry settings.
 *
 * Processing steps per sample:
 *   1. Read the delayed signal from the channel's delay buffer.
 *   2. Mix input and feedback, then write back into the delay buffer.
 *   3. Output a weighted sum of dry input and delayed (wet) signal.
 *
 * @param rv       Pointer to initialized @c reverb_t instance
 * @param in       Input buffer [samples * num_proc_ch], interleaved
 * @param out      Output buffer [samples * num_proc_ch], interleaved
 * @param samples  Number of samples per channel to process
 */
void reverb_process(reverb_t* rv, const float* in, float* out, int samples)
{
    for (int ch = 0; ch < rv->num_proc_ch; ++ch)
    {
        float* buf  = &rv->delayBuf[ch * MAX_DELAY_SAMPLES];
        int* wrIdx  = &rv->wrIdx[ch];

        for (int n = 0; n < samples; ++n)
        {
            // Compute input sample index for this channel
            int   idx         = n * rv->num_proc_ch + ch;

            // Compute read index for delayed signal
            int   rdIdx       = (*wrIdx + MAX_DELAY_SAMPLES - rv->delaySamples) % MAX_DELAY_SAMPLES;
            float delayed     = buf[rdIdx];
            float inputSample = in[idx];

            // Calculate feedback signal and write to delay buffer
            float     y = inputSample + delayed * rv->feedbackGain;
            buf[*wrIdx] = ANTI_DENORMAL(y);
            *wrIdx      = (*wrIdx + 1) % MAX_DELAY_SAMPLES;

            // Output: mix original and delayed signal
            out[idx]    = inputSample * rv->dryMix + delayed * rv->wetMix;
        }
    }
}
#endif //01
