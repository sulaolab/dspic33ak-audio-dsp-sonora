#if 0

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


#include "lpf_process.h"





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


//===========================================================
// Global Function
//===========================================================

// Static instances (example)
       lpf_t My_Lpf;

void app_lpf_init(void)
{
    static float StateBufLPF[APP_SLOTS_PER_FS * 4]; // 4 floats per channel

    memset(StateBufLPF, 0, sizeof(StateBufLPF));
    // Bind buffer and set channel layout
    lpf_init(&My_Lpf, &StateBufLPF[0], STAGE_1_PROC_CH);

    // Set cutoff (e.g., 80 Hz @ 48 kHz)
//    lpf_set_coeffs_lr4(&My_Lpf, SAMPLE_RATE, 80.0f);
    lpf_set_coeffs_lr4(&My_Lpf, SAMPLE_RATE, 85.0f);
//    lpf_set_coeffs_lr4(&My_Lpf, SAMPLE_RATE, 120.0f);
}

void app_lpf_process(const float* in, float* out)
{
    // TDM8 / 32-sample block
    lpf_process24(&My_Lpf, in, out, APP_BLOCK_FRAMES);
}

// ------------------------------------------------------------
// Gain setter
// ------------------------------------------------------------
void app_lpf_set_gain( float gain_dB )
{
    My_Lpf.gain_dB  = gain_dB;
    My_Lpf.gain_lin = db_to_lin( My_Lpf.gain_dB );
}

void app_lpf_enable( bool enable )
{
    if( enable )
    {
        My_Lpf.wet    = 1.0f;
    }
    else
    {
        My_Lpf.wet    = 0.0f;  // bypass
    }
}











// ------------------------------------------------------------
// Internal helper: one Butterworth 2nd-order LPF biquad
// Bilinear transform with pre-warp: K = tan(w0*T/2)
// ------------------------------------------------------------
static void lpf_calc_biquad_lowpass_butter(float fs, float fc,
                                           float* b0, float* b1, float* b2,
                                           float* a1, float* a2)
{
    const float w0   = 2.0f * (float)M_PI * fc;
    const float T    = 1.0f / fs;
    const float K    = tanf(0.5f * w0 * T);
    const float K2   = K * K;
    const float rt2  = 1.41421356237f; // sqrt(2)

    const float a0d  = (1.0f + rt2*K + K2);
    const float b0d  = K2;
    const float b1d  = 2.0f * K2;
    const float b2d  = K2;
    const float a1d  = 2.0f * (K2 - 1.0f);
    const float a2d  = (1.0f - rt2*K + K2);

    *b0 = b0d / a0d;
    *b1 = b1d / a0d;
    *b2 = b2d / a0d;
    *a1 = a1d / a0d;   // DF-II-T applies "-a1*y" in the loop
    *a2 = a2d / a0d;
}


// ------------------------------------------------------------
// Public: init
// ------------------------------------------------------------
void lpf_init( lpf_t* plpf, float* stateBuffer, int num_proc_ch )
{
    memset(plpf, 0, sizeof(lpf_t));

    plpf->num_proc_ch  = num_proc_ch;
    plpf->filtStates = stateBuffer;
    plpf->gain_dB    = 0.0f;
    plpf->gain_lin   = db_to_lin( plpf->gain_dB );
    plpf->wet        = 0.0f;   // OFF(bypass) by default
}


// ------------------------------------------------------------
// Public: set LR4 coefficients (compute one Butterworth biquad)
// ------------------------------------------------------------
void lpf_set_coeffs_lr4(lpf_t* plpf, float fs, float fc)
{
    float b0,b1,b2,a1,a2;
    lpf_calc_biquad_lowpass_butter(fs, fc, &b0,&b1,&b2,&a1,&a2);

    plpf->b0 = b0; plpf->b1 = b1; plpf->b2 = b2;
    plpf->a1 = a1; plpf->a2 = a2;

    plpf->DBG_fs = fs;
    plpf->DBG_fc = fc;
}

// ------------------------------------------------------------
// Process LR4 (with gain)
// ------------------------------------------------------------
void lpf_process24(      lpf_t* plpf,
                   const float* in,
                         float*  out,
                         int     samples )
{
    const float b0  = plpf->b0;
    const float b1  = plpf->b1;
    const float b2  = plpf->b2;
    const float a1  = plpf->a1;
    const float a2  = plpf->a2;
    const float g   = plpf->gain_lin;   // linear gain factor
    const float wet = plpf->wet;                  // 0.0f..1.0f
    const float dry = 1.0f - wet;

    const int num_proc_ch = plpf->num_proc_ch;

    for (int ch = 0; ch < num_proc_ch; ch++)
    {
        float* z = &plpf->filtStates[ch * 4];
        float z1a = z[0];
        float z2a = z[1];
        float z1b = z[2];
        float z2b = z[3];

        for (int n = 0; n < samples; n++)
        {
            const int   idx = (n * num_proc_ch) + ch;
            const float x   = in[idx];

            // Stage A
            float y1 = b0 * x + z1a;
            z1a = b1 * x - a1 * y1 + z2a;
            z2a = b2 * x - a2 * y1;

            // Stage B
            float y2 = b0 * y1 + z1b;
            z1b = b1 * y1 - a1 * y2 + z2b;
            z2b = b2 * y1 - a2 * y2;

            // Apply linear gain
//            out[idx] = y2 * g;
            // Dry/Wet mix (gain applies to wet path)
            out[idx] = dry * x + wet * (y2 * g);
        }

        z[0] = z1a; z[1] = z2a;
        z[2] = z1b; z[3] = z2b;
    }
}
#endif //01


