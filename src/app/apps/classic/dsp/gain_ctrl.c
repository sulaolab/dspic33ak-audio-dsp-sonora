/* gain_ctrl.c
 *
 * High resolution gain ramp processor for channel-major multi-channel audio.
 * multi-channel audio.
 *
 * v2 changes:
 * - app_gain_init() now receives sample_rate_Hz as uint32_t.
 * - Gain ramp is changed to a linear time-based ramp.
 * - rampTime_ms specifies the actual transition time.
 * - Target changes restart the ramp from the current gain value.
 * - rampTime_ms <= 0 applies the target immediately.
 * - Overshoot is prevented by remaining-sample countdown and target snap.
 */

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>   // for fmaxf


#include "gain_ctrl.h"





/*--------------------------------------------------------------------------
 * Application-level wrapper
 *-------------------------------------------------------------------------*/

#define DEFAULT_GAIN_RAMP_TIME_MS     (300)
#define DEFAULT_MIN_GAIN              (0.01f)    // 40 dB floor
#define DEFAULT_SNAP_THRESH           (0.001f)   // +/-0.1% snap threshold




/* Global gain controller instance */
/*static*/ audiogain_t My_Gain;


/**
 * @brief Initialise the global gain controller.
 *
 * Call exactly once at start-up, before any audio is processed.
 * The sample rate is stored internally and used to convert ramp time [ms]
 * to ramp length [samples].
 */
void app_gain_init( uint32_t sample_rate_Hz )
{
    /* User-tweakable default parameters */
    gain_init( &My_Gain,
               1.0f,                  /* initialGain      : unity gain                 */
               APP_BLOCK_FRAMES,            /* frameSize        : samples per channel/frame  */
               STAGE_1_PROC_CH,
               sample_rate_Hz );
}


void app_gain_process( float* p_in, float* p_out )
{
    gain_process( &My_Gain, p_in, p_out );
}


void app_mute_set( bool mute, int rampTime_ms )
{
    int mute_on = (mute ? 1:0);

    mute_set( &My_Gain, mute_on, rampTime_ms );
}


/**
 * @brief Map a 0..255 UI value to a linear gain target.
 *
 * @param vol_step 8-bit volume step (0 = mute, 255 = unity).
 */
void app_gain_set( uint8_t vol_step )
{
    gain_set( &My_Gain, vol_step, DEFAULT_GAIN_RAMP_TIME_MS );
}





/*--------------------------------------------------------------------------
 * Local helpers
 *-------------------------------------------------------------------------*/

static inline uint32_t local_get_valid_sample_rate( uint32_t sample_rate_Hz )
{
    if( sample_rate_Hz != 0u )
    {
        return sample_rate_Hz;
    }

    /* Fallback only. Normal operation should pass sample_rate_Hz via init. */
    return (uint32_t)SAMPLE_RATE;
}


static inline uint32_t local_ms_to_samples( uint32_t sample_rate_Hz, int rampTime_ms )
{
    uint64_t ramp_samples_u64;

    if( rampTime_ms <= 0 )
    {
        return 0u;
    }

    sample_rate_Hz = local_get_valid_sample_rate( sample_rate_Hz );

    /*
     * Convert ms to samples with rounding:
     *   ramp_samples = round(sample_rate_Hz * rampTime_ms / 1000)
     *
     * Use uint64_t to avoid overflow when sample_rate_Hz or rampTime_ms is large.
     */
    ramp_samples_u64 = ((uint64_t)sample_rate_Hz * (uint32_t)rampTime_ms) + 500ULL;
    ramp_samples_u64 = ramp_samples_u64 / 1000ULL;

    if( ramp_samples_u64 == 0ULL )
    {
        ramp_samples_u64 = 1ULL;
    }

    if( ramp_samples_u64 > 0xFFFFFFFFULL )
    {
        ramp_samples_u64 = 0xFFFFFFFFULL;
    }

    return (uint32_t)ramp_samples_u64;
}


static inline void local_finish_ramp_now( audiogain_t* pgain )
{
    pgain->prevGain          = pgain->targetGain;
    pgain->rampStep          = 0.0f;
    pgain->rampRemainSamples = 0u;
    pgain->rampTotalSamples  = 0u;
    pgain->status            = RAMP_IDLE;
}


static inline void local_start_linear_ramp( audiogain_t* pgain, int rampTime_ms )
{
    float    gain_diff;
    uint32_t ramp_samples;

    if( pgain == NULL )
    {
        return;
    }

    pgain->DBG_ramp_ms = (float)rampTime_ms;

    gain_diff = pgain->targetGain - pgain->prevGain;

    if( fabsf(gain_diff) <= pgain->snapThresh )
    {
        local_finish_ramp_now( pgain );
        return;
    }

    ramp_samples = local_ms_to_samples( pgain->sample_rate_Hz, rampTime_ms );

    if( ramp_samples == 0u )
    {
        local_finish_ramp_now( pgain );
        return;
    }

    pgain->rampTotalSamples  = ramp_samples;
    pgain->rampRemainSamples = ramp_samples;
    pgain->rampStep          = gain_diff / (float)ramp_samples;

    if( gain_diff > 0.0f )
    {
        pgain->status = RAMPING_UP;
    }
    else
    {
        pgain->status = RAMPING_DOWN;
    }

    pgain->DBG_rampSamples = (float)ramp_samples;
}


static inline float local_update_gain_one_sample( audiogain_t* pgain )
{
    float gain;
    float step;

    gain = pgain->prevGain;

    if( pgain->rampRemainSamples == 0u )
    {
        pgain->prevGain = pgain->targetGain;
        pgain->status   = RAMP_IDLE;
        return pgain->targetGain;
    }

    step = pgain->rampStep;
    gain += step;
    pgain->rampRemainSamples--;

    /*
     * Overshoot prevention:
     * 1. The countdown guarantees exact completion at the requested sample count.
     * 2. The direction check protects against rounding error or an updated target.
     */
    if( (pgain->rampRemainSamples == 0u) ||
        ((step > 0.0f) && (gain >= pgain->targetGain)) ||
        ((step < 0.0f) && (gain <= pgain->targetGain)) )
    {
        gain = pgain->targetGain;
        pgain->rampStep          = 0.0f;
        pgain->rampRemainSamples = 0u;
        pgain->status            = RAMP_IDLE;
    }

    pgain->prevGain = gain;

    return gain;
}





/*--------------------------------------------------------------------------
 * Library implementation
 *-------------------------------------------------------------------------*/

void mute_set( audiogain_t* pgain, int mute_on, int rampTime_ms )
{
    if( pgain == NULL )
    {
        return;
    }

    pgain->mute_on = mute_on;

    if( mute_on != 0 )
    {
        pgain->targetGain = 0.0f;
    }
    else
    {
        pgain->targetGain = pgain->storedGain;
    }

    gain_set_ramptime( pgain, rampTime_ms );
}


/**
 * @brief Map a 0..255 UI value to a linear gain target.
 *
 * @param vol_step 8-bit volume step (0 = mute, 255 = unity).
 */
void gain_set( audiogain_t* pgain, uint8_t vol_step, int rampTime_ms )
{
    float gain;

    if( pgain == NULL )
    {
        return;
    }

    /* Convert [0,255] to [0.0,1.0] */
    gain = vol_step / 255.0f;

    pgain->storedGain = gain;

    /*
     * Preserve mute state:
     * - If muted, update storedGain but keep target at 0.0.
     * - If unmuted, ramp to the new requested gain.
     */
    if( pgain->mute_on != 0 )
    {
        pgain->targetGain = 0.0f;
    }
    else
    {
        pgain->targetGain = gain;
    }

    gain_set_ramptime( pgain, rampTime_ms );
}


// /** Returns gain in linear amplitude from dB. */
// void gain_set_db( audiogain_t* pgain, float dB )
// {
//     float gain;
// 
//     if( pgain == NULL )
//     {
//         return;
//     }
// 
//     if( dB <= -100.0f )
//     {
//         gain = 0.0f;
//     }
//     else
//     {
//         gain = powf(10.0f, dB / 20.0f);
//     }
// 
//     pgain->storedGain = gain;
// 
//     if( pgain->mute_on != 0 )
//     {
//         pgain->targetGain = 0.0f;
//     }
//     else
//     {
//         pgain->targetGain = gain;
//     }
// 
//     gain_set_ramptime( pgain, (int)pgain->rampTime_ms );
// }


void gain_set_ramptime( audiogain_t* pgain, int rampTime_ms )
{
    if( pgain == NULL )
    {
        return;
    }

    pgain->rampTime_ms = (float)rampTime_ms;

    /*
     * Linear ramp policy:
     * - rampTime_ms <= 0: immediately set current gain to target.
     * - rampTime_ms >  0: restart ramp from current gain to target.
     */
    local_start_linear_ramp( pgain, rampTime_ms );
}


/**
 * @brief Initialise an @c audiogain_t instance.
 *
 * @param pgain          Pointer to state structure to initialise
 * @param initialGain    Starting gain (linear)
 * @param samples        Number of samples per channel in one processing frame
 * @param num_proc_ch    Number of input buffer channels
 * @param sample_rate_Hz Audio sample rate in Hz
 */
void gain_init( audiogain_t* pgain,
                float        initialGain,
                int          samples,
                int          num_proc_ch,
                uint32_t     sample_rate_Hz )
{
    if( pgain == NULL )
    {
        return;
    }

    pgain->mute_on           = 0;   /* start with unmute */

    /* Initialize base parameters */
    pgain->minGain           = DEFAULT_MIN_GAIN;
    pgain->snapThresh        = DEFAULT_SNAP_THRESH;

    pgain->samples           = samples;
    pgain->num_proc_ch       = num_proc_ch;
    pgain->sample_rate_Hz    = local_get_valid_sample_rate( sample_rate_Hz );

    pgain->prevGain          = fmaxf( initialGain, pgain->minGain );
    pgain->targetGain        = pgain->prevGain;
    pgain->storedGain        = pgain->prevGain;

    pgain->rampTime_ms       = (float)DEFAULT_GAIN_RAMP_TIME_MS;
    pgain->rampStep          = 0.0f;
    pgain->rampRemainSamples = 0u;
    pgain->rampTotalSamples  = 0u;

    pgain->status            = RAMP_IDLE;

    pgain->DBG_ramp_ms       = pgain->rampTime_ms;
    pgain->DBG_rampSamples   = 0.0f;

    /*
     * No ramp is started by init because current == target.
     * The ramp time is stored and will be used by the next gain/mute command.
     */
}


/**
 * @brief Apply a per-sample linear gain ramp to a channel-major buffer.
 *
 * Buffer layout:
 *   p_in[ch * samples + sample_idx]
 */
void gain_process( audiogain_t* pgain, float* p_in, float* p_out )
{
    int sample_idx;
    int ch;
    int samples;
    int num_proc_ch;

    if( (pgain == NULL) || (p_in == NULL) || (p_out == NULL) )
    {
        return;
    }

    samples     = pgain->samples;
    num_proc_ch = pgain->num_proc_ch;

    for( sample_idx = 0; sample_idx < samples; sample_idx++ )
    {
        float gain = local_update_gain_one_sample( pgain );

        for( ch = 0; ch < num_proc_ch; ch++ )
        {
            int idx = (ch * samples) + sample_idx;
            p_out[idx] = p_in[idx] * gain;
        }
    }
}
