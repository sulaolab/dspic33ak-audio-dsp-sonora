
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "sine_gen.h"





#if defined(ENA_SINE_GEN)
//===========================================================
// API
//===========================================================

static sine_gen_t g_aux_keepalive;

void app_keepalive_init( uint32_t sample_rate_Hz )
{
#if 1
    // JBL Flip4 anti auto mute only.
    // Default: 30 kHz, -40 dBFS equivalent (= 0.01 peak), added to all active channels.
    sine_gen_init( &g_aux_keepalive,
                   30000.0f,
                   0.01f,
                   STAGE_1_PROC_CH,
                   sample_rate_Hz );
#else
    sine_gen_init( &g_aux_keepalive,
                   500.0f,
                   0.1f,
                   STAGE_1_PROC_CH,
                   sample_rate_Hz );
#endif//01
}

void app_keepalive_set(float freq_Hz, float gain_lin)
{
    sine_gen_set_params(&g_aux_keepalive, freq_Hz, gain_lin);
}

void app_keepalive_set_sample_rate(uint32_t sample_rate_Hz)
{
    sine_gen_set_sample_rate(&g_aux_keepalive, sample_rate_Hz);
}



void app_keepalive_process(const float* in, float* out, int num_proc_ch)
{
    sine_gen_process_add_nch(&g_aux_keepalive, in, out, num_proc_ch, APP_BLOCK_FRAMES);
}







//===========================================================
// Local helpers
//===========================================================

static inline float local_clampf(float x, float min_v, float max_v)
{
    if (x < min_v)
    {
        return min_v;
    }
    if (x > max_v)
    {
        return max_v;
    }

    return x;
}

static inline float wrap_0_2pi(float x)
{
    const float two_pi = (float)(2.0 * M_PI);

    while (x >= two_pi)
    {
        x -= two_pi;
    }
    while (x < 0.0f)
    {
        x += two_pi;
    }

    return x;
}

static inline float get_valid_sample_rate(float sample_rate_Hz)
{
    if (sample_rate_Hz > 0.0f)
    {
        return sample_rate_Hz;
    }

    // Fallback only. Normal operation should use the sample rate stored in sine_gen_t.
    return (float)SAMPLE_RATE;
}

static inline void recalc_inc(sine_gen_t* p)
{
    float fs;
    float f_eff;

    if (p == NULL)
    {
        return;
    }

    fs = get_valid_sample_rate(p->sample_rate_Hz);
    p->sample_rate_Hz = fs;

    // Clamp only for phase increment calculation.
    // Do not overwrite p->freq_Hz, because it is the requested frequency.
    // Example:
    //   30 kHz request @ 48 kHz Fs -> internally clamped to 24 kHz.
    //   If Fs later changes to 96 kHz, the original 30 kHz request is still available.
    f_eff = local_clampf(p->freq_Hz, 0.0f, 0.5f * fs);

    p->freq_eff_Hz = f_eff;
    p->phase_inc   = (float)(2.0 * M_PI) * f_eff / fs;
}

static inline int get_valid_channel_count(int num_proc_ch)
{
    if (num_proc_ch > 0)
    {
        return num_proc_ch;
    }

    return 1;
}


//===========================================================
// Public
//===========================================================

void sine_gen_init( sine_gen_t* p,
                    float       initialFreq_Hz,
                    float       initialGain_lin,
                    int         num_proc_ch,
                    uint32_t    sample_rate_Hz )
{
    if (p == NULL)
    {
        return;
    }

    memset(p, 0x00, sizeof(*p));

    p->num_proc_ch    = get_valid_channel_count(num_proc_ch);
    p->sample_rate_Hz = get_valid_sample_rate((float)sample_rate_Hz);
    p->freq_Hz        = initialFreq_Hz;
    p->freq_eff_Hz    = 0.0f;
    p->gain_lin       = initialGain_lin;

    // IMPORTANT:
    // If user sets freq=Fs/2 (e.g., 24 kHz @ 48 kHz), starting phase at 0 makes
    // sin(phase) always 0. Start at pi/2 so it becomes +1/-1 alternating.
    p->phase_rad      = (float)(0.5 * M_PI);

    recalc_inc(p);
}

void sine_gen_set_sample_rate( sine_gen_t* p, uint32_t sample_rate_Hz )
{
    float f_sample_rate_Hz = (float)sample_rate_Hz;

    if (p == NULL)
    {
        return;
    }

    if (f_sample_rate_Hz <= 0.0f)
    {
        return;
    }

    p->sample_rate_Hz = f_sample_rate_Hz;
    recalc_inc(p);
}

void sine_gen_set_freq( sine_gen_t* p, float freq_Hz )
{
    if (p == NULL)
    {
        return;
    }

    p->freq_Hz = freq_Hz;
    recalc_inc(p);
}

void sine_gen_set_params( sine_gen_t* p, float freq_Hz, float gain_lin )
{
    if (p == NULL)
    {
        return;
    }

    p->freq_Hz  = freq_Hz;
    p->gain_lin = gain_lin;

    recalc_inc(p);
}

void sine_gen_set_phase( sine_gen_t* p, float phase_rad )
{
    if (p == NULL)
    {
        return;
    }

    p->phase_rad = wrap_0_2pi(phase_rad);
}



void sine_gen_process( sine_gen_t* p, float* out, int samples )
{
    if ((p == NULL) || (out == NULL) || (samples <= 0))
    {
        return;
    }
    const float gain        = (p != NULL) ? p->gain_lin  : 0.0f;
    const float dph         = (p != NULL) ? p->phase_inc : 0.0f;
    const int   chs         = get_valid_channel_count(p->num_proc_ch);
    if (chs <= 0)
    {
        return;
    }


    float ph = p->phase_rad;   // >>> load ph

    for (int n = 0; n < samples; n++)
    {
        const float s = gain * sinf(ph);

        // Write same tone to all channels.
        // ch-major layout:
        //   out[ch * samples + n]
        for (int ch = 0; ch < chs; ch++)
        {
            out[(ch * samples) + n] = s;
        }

        ph += dph;
        if (ph >= (float)(2.0 * M_PI))
        {
            ph -= (float)(2.0 * M_PI);
        }
    }

    p->phase_rad = ph;         // <<< save ph
}






void sine_gen_process_add( sine_gen_t* p, const float* in, float* out, int samples )
{
    if (p == NULL)
    {
        return;
    }

    sine_gen_process_add_nch(p, in, out, p->num_proc_ch, samples);
}

void sine_gen_process_add_nch( sine_gen_t*  p,
                                   const float* in,
                                   float*       out,
                                   int          num_proc_ch,
                                   int          samples )
{
    const int   chs  = get_valid_channel_count(num_proc_ch);
    const float gain = (p != NULL) ? p->gain_lin  : 0.0f;
          float ph;
    const float dph  = (p != NULL) ? p->phase_inc : 0.0f;

    if ((p == NULL) || (in == NULL) || (out == NULL) || (samples <= 0) || (chs <= 0))
    {
        return;
    }

    ph = p->phase_rad;

    for (int n = 0; n < samples; n++)
    {
        const float s = gain * sinf(ph);

        // ch-major layout:
        //   in [ch * samples + n]
        //   out[ch * samples + n]
        for (int ch = 0; ch < chs; ch++)
        {
            const int idx = (ch * samples) + n;
            out[idx] = in[idx] + s;
        }

        ph += dph;
        if (ph >= (float)(2.0 * M_PI))
        {
            ph -= (float)(2.0 * M_PI);
        }
    }

    p->phase_rad = ph;
}

#endif //defined(ENA_SINE_GEN)
