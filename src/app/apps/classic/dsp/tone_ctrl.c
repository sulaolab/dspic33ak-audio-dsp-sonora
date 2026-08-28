
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
#include "gain_ctrl.h"


#include "tone_ctrl.h"





//===========================================================
// Definition
//===========================================================

// ---------------------------------------------
// Q Factor and Its Effect on Frequency Response
// ---------------------------------------------
// Q     | Description
// ------|------------------------------------------------------
// 0.707 | Gentle slope (Butterworth), affects wide frequency band
// 1.0   | Moderately sharp response, better localization
// 1.5   | Sharper transition, affects narrower frequency band
// 2.0+  | Very sharp response, primarily affects target frequency
// 
// - Lower Q results in a smooth and wide transition region.
// - Higher Q narrows the effect, making the filter more selective.
// - Choose Q based on how tightly you want to focus the tonal boost/cut.

#define Q_FACTOR_0707    (0.707f)
#define Q_FACTOR_1000    (1.0f)
#define Q_FACTOR_1200    (1.2f)
#define Q_FACTOR_2500    (2.5f)



#define TREBLE_HZ        (5000.0f)
#define MIDDLE_HZ        (1000.0f)
#define BASS_HZ          ( 100.0f)




//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

static inline uint32_t local_get_valid_sample_rate( uint32_t sample_rate_Hz );


//===========================================================
// Variables
//===========================================================



//===========================================================
// Global Function
//===========================================================

/**
 * @brief Initialize a tone filter instance.
 *
 * Sets up a @c tone_t structure with default values and assigns
 * external state buffer memory for biquad filter processing.
 *
 * Behavior:
 *   - Clears all structure fields and zeroes the filter state buffer.
 *   - Configures number of input channels based on TONE_SLOTS_PER_FS.
 *   - Stores Q factor and filter mode selector (@p filter_type).
 *   - Stores sample rate for coefficient calculation and smoothing.
 *   - The filter state buffer must be provided externally with a size of
 *     [channels * 2] floats (two delay elements per channel).
 *
 * @param ptone          Pointer to @c tone_t instance to initialize
 * @param rampTime_ms    Coefficient smoothing time in milliseconds
 * @param initialGain    Reserved for future initial gain/blend (not used yet)
 * @param Q_or_Slope     Quality factor of the filter (defines bandwidth)
 * @param filter_type    Filter type selector (0 = peaking EQ, 1 = high-shelf)
 * @param num_proc_ch    channel number of processing
 * @param sample_rate_Hz Audio sample rate in Hz
 */
void tone_init( tone_t*  ptone,
                float    rampTime_ms,
                float    initialGain,
                float    Q_or_Slope,
                int      filter_type,
                int      num_proc_ch,
                uint32_t sample_rate_Hz )
{
    memset( ptone, 0x00, sizeof(tone_t) );

    ptone->num_proc_ch    = num_proc_ch;
    ptone->sample_rate_Hz = local_get_valid_sample_rate( sample_rate_Hz );
    ptone->filter_type    = filter_type;
    ptone->Q_or_Slope     = Q_or_Slope;


    float ramp_s = fmaxf(rampTime_ms, 1.0f) * 0.001f;
    float N      = ramp_s * (float)ptone->sample_rate_Hz;        // How many samples to follow
    float frame  = (float)APP_BLOCK_FRAMES;
    ptone->coeff_smooth_g = 1.0f - expf(-frame / fmaxf(N, 1.0f));

    (void)initialGain;
}




/**
 * @brief Sets peaking EQ coefficients based on user gain and frequency settings.
 *
 *        This version uses Audio EQ Cookbook formula for a peaking EQ.
 *
 * @param ptone       Pointer to tone_t instance
 * @param gain_dB     Desired gain in decibels (positive or negative)
 * @param center_Hz   Center frequency of peaking EQ (Hz)
 */
void tone_set_coeffs(tone_t* ptone, float gain_dB, float center_Hz)
{
    float b0,b1,b2,a0,a1,a2;

    float sample_rate_Hz = (float)local_get_valid_sample_rate( ptone->sample_rate_Hz );

    /* 10^(dB/40) written as e^(dB * ln10/40): identical value, and it is the only
     * powf() call left in a Classic image. powf() is the general x^y -- it costs
     * 1,876 bytes of libm on its own, while expf() is already linked (every
     * one-pole smoother in the DSP calls it). On the AK128, where the program
     * Flash has to make room for the resident bootloader, that trade is free. */
    float A      = expf(gain_dB * (float)(M_LN10 / 40.0));
    float w0     = 2.0f * M_PI * center_Hz / sample_rate_Hz;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float Q      = 0.0f;
    float S      = 0.0f;
    float alpha  = 0.0f;
    float beta   = 0.0f;

    int type = ptone->filter_type;

    if (type == TONE_EQ_PEAKING)   // 0
    {
        Q     = fmaxf(ptone->Q_or_Slope, 0.001f);
        alpha = sin_w0 / (2.0f * Q);

        b0 =  1.0f + alpha * A;
        b1 = -2.0f * cos_w0;
        b2 =  1.0f - alpha * A;
        a0 =  1.0f + alpha / A;
        a1 = -2.0f * cos_w0;
        a2 =  1.0f - alpha / A;
    }
    else
    {
        // RBJ shelf uses slope S
        S     = fmaxf(ptone->Q_or_Slope, 0.001f);
        alpha = (sin_w0 / 2.0f) * sqrtf( (A + 1.0f/A) * (1.0f/S - 1.0f) + 2.0f );
        beta  = 2.0f * sqrtf(A) * alpha;

        if (type == TONE_EQ_LOW_SHELF) // 2
        {
            b0 =    A * ((A + 1.0f) - (A - 1.0f)*cos_w0 + beta);
            b1 =  2*A * ((A - 1.0f) - (A + 1.0f)*cos_w0);
            b2 =    A * ((A + 1.0f) - (A - 1.0f)*cos_w0 - beta);
            a0 =         (A + 1.0f) + (A - 1.0f)*cos_w0 + beta;
            a1 = -2.0f* ((A - 1.0f) + (A + 1.0f)*cos_w0);
            a2 =         (A + 1.0f) + (A - 1.0f)*cos_w0 - beta;
        }
        else // TONE_EQ_HIGH_SHELF (1)
        {
            b0 =    A * ((A + 1.0f) + (A - 1.0f)*cos_w0 + beta);
            b1 = -2*A * ((A - 1.0f) + (A + 1.0f)*cos_w0);
            b2 =    A * ((A + 1.0f) + (A - 1.0f)*cos_w0 - beta);
            a0 =         (A + 1.0f) - (A - 1.0f)*cos_w0 + beta;
            a1 = 2.0f * ((A - 1.0f) - (A + 1.0f)*cos_w0);
            a2 =         (A + 1.0f) - (A - 1.0f)*cos_w0 - beta;
        }
    }

    // Normalize
    ptone->bq_target.b0 = b0 / a0;
    ptone->bq_target.b1 = b1 / a0;
    ptone->bq_target.b2 = b2 / a0;
    ptone->bq_target.a1 = a1 / a0;
    ptone->bq_target.a2 = a2 / a0;

    // debug: the two the console prints (classic_controls.c). The intermediate
    // coefficients used to be latched here too, but nothing ever read them --
    // both the inputs (these two) and the result (bq_target) stay in the struct,
    // so a debugger sees everything without paying 6 floats per tone instance.
    ptone->DBG_gain_dB = gain_dB;
    ptone->DBG_tar_Hz  = center_Hz;
}


/**
 * @brief Applies peaking EQ tone filter with smooth gain ramping (blend).
 *
 *        Uses Direct Form II Transposed biquad and exponential smoothing
 *        to blend EQ effect in/out over multiple frames (blocks).
 *
 * @param ptone         Pointer to tone_t instance
 * @param in            Interleaved input buffer [frameSize * num_ch]
 * @param out           Interleaved output buffer [frameSize * num_ch]
 * @param frameSize     Number of samples per channel
 * @param num_ch        Number of audio channels
 */



/**
 * @brief Applies peaking EQ tone filter with smooth gain ramping (blend).
 *
 *        Uses Direct Form II Transposed biquad and exponential smoothing
 *        to blend EQ effect in/out over multiple frames (blocks).
 *
 * @param ptone         Pointer to tone_t instance
 * @param in            Channel-major input buffer [num_ch][frameSize]
 * @param out           Channel-major output buffer [num_ch][frameSize]
 * @param frameSize     Number of samples per channel
 * @param num_ch        Number of audio channels
 */
void tone_process(       tone_t* ptone,
                       const float*  in,
                             float*  out,
                             int     samples )
{
    float g = ptone->coeff_smooth_g;
    ptone->bq.b0 = lerp(ptone->bq.b0, ptone->bq_target.b0, g);
    ptone->bq.b1 = lerp(ptone->bq.b1, ptone->bq_target.b1, g);
    ptone->bq.b2 = lerp(ptone->bq.b2, ptone->bq_target.b2, g);
    ptone->bq.a1 = lerp(ptone->bq.a1, ptone->bq_target.a1, g);
    ptone->bq.a2 = lerp(ptone->bq.a2, ptone->bq_target.a2, g);

    float b0 = ptone->bq.b0;
    float b1 = ptone->bq.b1;
    float b2 = ptone->bq.b2;
    float a1 = ptone->bq.a1;
    float a2 = ptone->bq.a2;

    int num_proc_ch = ptone->num_proc_ch;

    // --- Apply filter per channel ---
    // process the actual channel only
    for (int ch = 0; ch < num_proc_ch; ch++)
    {
        float z1 = ptone->bqs[ch].z1;
        float z2 = ptone->bqs[ch].z2;

        const float* p_in  = &in [ch * samples];
              float* p_out = &out[ch * samples];

        for (int sample_idx = 0; sample_idx < samples; sample_idx++)
        {
            float x = p_in[sample_idx];

            // Direct Form II Transposed Biquad
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;

            // Apply blend
            float mix = 1.0f;     // float mix=0..1  mix is only future expansion
            p_out[sample_idx] = x + mix * (y - x);
        }

        // Save state
        ptone->bqs[ch].z1 = z1;
        ptone->bqs[ch].z2 = z2;
    }
}









//===========================================================
// Local Function
//===========================================================

static inline uint32_t local_get_valid_sample_rate( uint32_t sample_rate_Hz )
{
    if( sample_rate_Hz != 0u )
    {
        return sample_rate_Hz;
    }

    /* Fallback only. Normal operation should pass sample_rate_Hz via init. */
    return (uint32_t)SAMPLE_RATE;
}













//===========================================================
// API
//===========================================================

/*static*/ tone_t My_ToneTre;
/*static*/ tone_t My_ToneMid;
/*static*/ tone_t My_ToneBas;

void app_tone_init( uint32_t sample_rate_Hz )
{
    tone_init(&My_ToneTre, 300, 0.0f, Q_FACTOR_1000, TONE_EQ_HIGH_SHELF, TONE_SLOTS_PER_FS, sample_rate_Hz);
    tone_init(&My_ToneMid, 300, 0.0f, Q_FACTOR_1200, TONE_EQ_PEAKING,    TONE_SLOTS_PER_FS, sample_rate_Hz);
    tone_init(&My_ToneBas, 300, 0.0f, Q_FACTOR_1000, TONE_EQ_LOW_SHELF,  TONE_SLOTS_PER_FS, sample_rate_Hz);

    // set gain and frequency
    tone_set_coeffs(&My_ToneTre, 0.0f, TREBLE_HZ);
    tone_set_coeffs(&My_ToneMid, 0.0f, MIDDLE_HZ);
    tone_set_coeffs(&My_ToneBas, 0.0f, BASS_HZ  );

    // copy target params to current for startup
    My_ToneTre.bq = My_ToneTre.bq_target;
    My_ToneMid.bq = My_ToneMid.bq_target;
    My_ToneBas.bq = My_ToneBas.bq_target;
}


void app_tone_set_coeffs_tre( float gain_dB )
{
    tone_set_coeffs(&My_ToneTre, gain_dB, TREBLE_HZ);
}
void app_tone_set_coeffs_mid( float gain_dB )
{
    tone_set_coeffs(&My_ToneMid, gain_dB, MIDDLE_HZ);
}
void app_tone_set_coeffs_bas( float gain_dB )
{
    tone_set_coeffs(&My_ToneBas, gain_dB, BASS_HZ);
}





void app_tone_process_tre( const float* in, float* out )
{
    tone_process(&My_ToneTre, in, out, APP_BLOCK_FRAMES);
}
void app_tone_process_mid( const float* in, float* out )
{
    tone_process(&My_ToneMid, in, out, APP_BLOCK_FRAMES);
}
void app_tone_process_bas( const float* in, float* out )
{
    tone_process(&My_ToneBas, in, out, APP_BLOCK_FRAMES);
}

