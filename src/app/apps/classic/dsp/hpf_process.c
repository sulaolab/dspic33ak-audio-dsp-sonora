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
#include <math.h>
#include "gain_ctrl.h"


#include "hpf_process.h"

//===========================================================
// Static instances (example)
//===========================================================
static hpf_t My_Hpf;


//===========================================================
// Application wrappers (same style).
//===========================================================

void app_hpf_init(void)
{
    static float StateBufHPF[APP_SLOTS_PER_FS * 4]; // 4 floats per channel

    memset(StateBufHPF, 0, sizeof(StateBufHPF));
    // Bind buffer and set channel layout
    hpf_init(&My_Hpf, &StateBufHPF[0], STAGE_1_PROC_CH);

    // Set cutoff (for example, 35 Hz at 48 kHz as a DC blocker and subsonic filter).
//    hpf_set_coeffs_lr4(&My_Hpf, SAMPLE_RATE, 35.0f);
//    hpf_set_coeffs_lr4(&My_Hpf, SAMPLE_RATE, 120.0f);
    hpf_set_coeffs_lr4(&My_Hpf, SAMPLE_RATE, 150.0f);
//    hpf_set_coeffs_lr4(&My_Hpf, SAMPLE_RATE, 800.0f);
}

void app_hpf_process(const float* in, float* out)
{
    // TDM8 / 32-sample block
    hpf_process24(&My_Hpf, in, out, APP_BLOCK_FRAMES);
}

void app_hpf_set_gain(float gain_dB)
{
    My_Hpf.gain_dB  = gain_dB;
    My_Hpf.gain_lin = db_to_lin( My_Hpf.gain_dB );
}

void app_hpf_enable( bool enable )
{
    if( enable )
    {
        My_Hpf.wet    = 1.0f;
    }
    else
    {
        My_Hpf.wet    = 0.0f;
    }
}



//===========================================================
// Internal helper: one Butterworth 2nd-order HPF biquad
// Bilinear transform with pre-warp: K = tan(w0*T/2) = tan(pi*fc/fs)
//===========================================================
static void hpf_calc_biquad_highpass_butter(float fs, float fc,
                                            float* b0, float* b1, float* b2,
                                            float* a1, float* a2)
{
    // Safety clamp to avoid extreme warping / instability
    const float fs_min = 1.0f;
    if (fs < fs_min) fs = fs_min;

    // Clamp fc to a sensible range, [1 Hz, fs/3]; adjust as needed.
    const float fc_min = 1.0f;
    const float fc_max = fmaxf(10.0f, fs * (1.0f/3.0f));
    if (fc < fc_min) fc = fc_min;
    if (fc > fc_max) fc = fc_max;

    const float w0  = 2.0f * (float)M_PI * fc;
    const float T   = 1.0f / fs;
    const float K   = tanf(0.5f * w0 * T);     // = tan(pi*fc/fs)
    const float K2  = K * K;
    const float rt2 = 1.41421356237f;          // sqrt(2)

    // Denominator common term
    const float a0d = (1.0f + rt2*K + K2);

    // High-pass (Butterworth 2nd) cookbook (normalized a0 = 1)
    const float b0d = 1.0f;
    const float b1d = -2.0f;
    const float b2d = 1.0f;
    const float a1d = 2.0f * (K2 - 1.0f);
    const float a2d = (1.0f - rt2*K + K2);

    *b0 = b0d / a0d;
    *b1 = b1d / a0d;
    *b2 = b2d / a0d;
    *a1 = a1d / a0d;  // DF-II-T applies "-a1*y" in the loop
    *a2 = a2d / a0d;
}

//===========================================================
// Public: init
//===========================================================
void hpf_init( hpf_t* phpf, float* stateBuffer, int num_proc_ch )
{
    memset(phpf, 0, sizeof(hpf_t));
    phpf->num_proc_ch  = num_proc_ch;

    phpf->gain_dB    = 0.0f;
    phpf->gain_lin   = db_to_lin( phpf->gain_dB );
    phpf->wet        = 0.0f;   // OFF(bypass) by default

    // Clear states: need chPerFs * 4 floats
    phpf->filtStates = stateBuffer;
}

//===========================================================
// Public: set LR4 coefficients (compute one Butterworth biquad)
//===========================================================
void hpf_set_coeffs_lr4(hpf_t* phpf, float fs, float fc)
{
    float b0,b1,b2,a1,a2;
    hpf_calc_biquad_highpass_butter(fs, fc, &b0,&b1,&b2,&a1,&a2);

    phpf->b0 = b0; phpf->b1 = b1; phpf->b2 = b2;
    phpf->a1 = a1; phpf->a2 = a2;

    phpf->DBG_fs = fs;
    phpf->DBG_fc = fc;
}

//===========================================================
// Process LR4 (with gain) using two transposed direct-form II stages.
//===========================================================
void hpf_process24(      hpf_t* phpf,
                   const float* in,
                         float*  out,
                         int     samples )
{
    const float b0  = phpf->b0;
    const float b1  = phpf->b1;
    const float b2  = phpf->b2;
    const float a1  = phpf->a1;
    const float a2  = phpf->a2;
    const float g   = phpf->gain_lin;      // linear gain factor
    const float wet = phpf->wet;           // 0.0f..1.0f
    const float dry = 1.0f - wet;

    const int num_proc_ch = phpf->num_proc_ch;

    for (int ch = 0; ch < num_proc_ch; ch++)
    {
        float* z = &phpf->filtStates[ch * 4];
        float z1a = z[0];
        float z2a = z[1];
        float z1b = z[2];
        float z2b = z[3];

        for (int n = 0; n < samples; n++)
        {
            const int   idx = (n * num_proc_ch) + ch;
            const float x   = in[idx];

            // Stage A (HPF biquad)
            float y1 = b0 * x + z1a;
            z1a = b1 * x - a1 * y1 + z2a;
            z2a = b2 * x - a2 * y1;

            // Stage B (copy same biquad)
            float y2 = b0 * y1 + z1b;
            z1b = b1 * y1 - a1 * y2 + z2b;
            z2b = b2 * y1 - a2 * y2;

            // Dry/Wet mix (gain applies to wet path)
            out[idx] = dry * x + wet * (y2 * g);
        }

        z[0] = z1a; z[1] = z2a;
        z[2] = z1b; z[3] = z2b;
    }
}
#endif //01
