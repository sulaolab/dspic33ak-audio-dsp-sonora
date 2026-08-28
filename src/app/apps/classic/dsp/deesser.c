

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


#include "deesser.h"




#if defined(ENA_DEESSER)
//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

static inline float  time_ms_to_block_g(float time_ms, float frame_samples);
static        void   biquad_make_lpf(biquad_t* bq, float fc_Hz);
static        void   biquad_make_hpf(biquad_t* bq, float fc_Hz);
static inline float  biquad_df2t(biquad_t bq, biquad_stat_t* st, float x);
static inline float  deesser_gain_from_env(float env, float thr_lin, float sat_lin, float gmin_lin);
static inline float  deesser_calc_sat_env(float thr_lin, float ratio, float max_gr_dB);




//===========================================================
// Variables
//===========================================================


//===========================================================
// Global Function
//===========================================================

void deesser_reset_states(deesser_t* ds)
{
    for (int ch = 0; ch < ds->num_proc_ch; ch++)
    {
        ds->lp_s[ch].z1 = 0.0f; ds->lp_s[ch].z2 = 0.0f;
        ds->det_s[ch].z1 = 0.0f; ds->det_s[ch].z2 = 0.0f;
        ds->env[ch] = 0.0f;
        ds->gain[ch] = 1.0f;
    }
}

void deesser_init(deesser_t* ds, int num_proc_ch, float rampTime_ms)
{
    memset(ds, 0, sizeof(*ds));
    ds->num_proc_ch = num_proc_ch;

    // defaults (safe)
    ds->thr_dB      = -24.0f;
    ds->ratio       = 3.0f;
    ds->max_gr_dB   = 4.0f;
    ds->split_fc_Hz = 5000.0f;
    ds->det_fc_Hz   = 6000.0f;

    // coeff smoothing same spirit as tone_ctrl
    // (used when changing split/det cutoff)
    float frame = 32.0f; // if you want, you can pass samples here later; this is only for coefficient transitions
    ds->coeff_smooth_g = time_ms_to_block_g(fmaxf(rampTime_ms, 1.0f), frame);

    // default AR times
    ds->env_att_g  = time_ms_to_block_g(2.0f, 32.0f);
    ds->env_rel_g  = time_ms_to_block_g(120.0f, 32.0f);
    ds->gain_att_g = time_ms_to_block_g(2.0f, 32.0f);
    ds->gain_rel_g = time_ms_to_block_g(80.0f, 32.0f);

    // build targets
    biquad_make_lpf(&ds->lp_target, ds->split_fc_Hz);
    biquad_make_hpf(&ds->det_target, ds->det_fc_Hz);

    // start current = target
    ds->lp  = ds->lp_target;
    ds->det = ds->det_target;

    deesser_reset_states(ds);
}

void deesser_set_params(deesser_t* ds,
                        float thr_dB,
                        float ratio,
                        float max_gr_dB,
                        float attack_ms,
                        float release_ms,
                        float split_fc_Hz,
                        float det_fc_Hz)
{
    ds->thr_dB    = thr_dB;
    ds->ratio     = fmaxf(ratio, 1.0f);
    ds->max_gr_dB = fmaxf(max_gr_dB, 0.0f);

    ds->split_fc_Hz = split_fc_Hz;
    ds->det_fc_Hz   = det_fc_Hz;

    // Update smoothing gains based on current block size behavior:
    // We'll compute g inside process using current 'samples' so it works for APP_BLOCK_FRAMES=1 too.
    // Here store just the time constants in a way? -> simplest: keep ms, but we didn't store them.
    // So we recompute in process based on ds->env_att_g etc. if needed. For now we precompute for 32-sample typical.
    ds->env_att_g  = time_ms_to_block_g(fmaxf(attack_ms, 0.1f), 32.0f);
    ds->env_rel_g  = time_ms_to_block_g(fmaxf(release_ms, 0.1f), 32.0f);

    // gain smoothing: usually a bit slower on release to avoid chatter
    ds->gain_att_g = time_ms_to_block_g(fmaxf(attack_ms, 0.1f), 32.0f);
    ds->gain_rel_g = time_ms_to_block_g(fmaxf(release_ms * 0.7f, 0.1f), 32.0f);

    // update biquad targets (smoothly moved in process)
    biquad_make_lpf(&ds->lp_target, ds->split_fc_Hz);
    biquad_make_hpf(&ds->det_target, ds->det_fc_Hz);
}


void deesser_process(deesser_t* ds, const float* in, float* out, int samples)
{
    // --- coefficient smoothing (optional) ---
    // If you don't change split/det frequency often, you can set coeff_smooth_g=1.
    float cg = clampf(ds->coeff_smooth_g, 0.0f, 1.0f);
    ds->lp.b0  = lerp(ds->lp.b0,  ds->lp_target.b0,  cg);
    ds->lp.b1  = lerp(ds->lp.b1,  ds->lp_target.b1,  cg);
    ds->lp.b2  = lerp(ds->lp.b2,  ds->lp_target.b2,  cg);
    ds->lp.a1  = lerp(ds->lp.a1,  ds->lp_target.a1,  cg);
    ds->lp.a2  = lerp(ds->lp.a2,  ds->lp_target.a2,  cg);

    ds->det.b0 = lerp(ds->det.b0, ds->det_target.b0, cg);
    ds->det.b1 = lerp(ds->det.b1, ds->det_target.b1, cg);
    ds->det.b2 = lerp(ds->det.b2, ds->det_target.b2, cg);
    ds->det.a1 = lerp(ds->det.a1, ds->det_target.a1, cg);
    ds->det.a2 = lerp(ds->det.a2, ds->det_target.a2, cg);

    // --- recompute AR smoothing gains for THIS block size (important if samples=1) ---
//    float frame = fmaxf((float)samples, 1.0f);

    // Use the same time constants as implied by current ds->env_att_g etc (computed at 32),
    // but to be correct, recompute from ms. Since we didn't store ms, we approximate by scaling:
    // Better approach: store ms in struct; for now, do "safe" behavior:
    // - If samples==1, make smoothing much smaller to avoid zippering.
    float env_att_g  = (samples == 1) ? 0.02f  : ds->env_att_g;
    float env_rel_g  = (samples == 1) ? 0.002f : ds->env_rel_g;
    float gain_att_g = (samples == 1) ? 0.02f  : ds->gain_att_g;
    float gain_rel_g = (samples == 1) ? 0.005f : ds->gain_rel_g;

    // Pre-compute de-esser gain curve parameters once per block.
    // Do not call powf()/logf() in the sample loop.
    const float thr_lin  = db_to_lin(ds->thr_dB);
    const float gmin_lin = db_to_lin(-fmaxf(ds->max_gr_dB, 0.0f));
    const float sat_lin  = deesser_calc_sat_env(thr_lin, ds->ratio, ds->max_gr_dB);

    int chs = ds->num_proc_ch;

    for (int ch = 0; ch < chs; ch++)
    {
        float env  = ds->env[ch];
        float gain = ds->gain[ch];
        biquad_stat_t* lpst  = &ds->lp_s[ch];
        biquad_stat_t* detst = &ds->det_s[ch];

        const float* p_in  = &in [ch * samples];
              float* p_out = &out[ch * samples];

        for (int n = 0; n < samples; n++)
        {
            float x = p_in[n];

            // --- split band ---
            float low  = biquad_df2t(ds->lp, lpst, x);
            float high = x - low;

            // --- detector (HPF 6k) ---
            float d = biquad_df2t(ds->det, detst, x);
            float rect = fabsf(d);

            // envelope AR
            float eg = (rect > env) ? env_att_g : env_rel_g;
            env += eg * (rect - env);

            // gain target from env
            float gain_t = deesser_gain_from_env(env, thr_lin, sat_lin, gmin_lin);

            // gain smoothing AR (critical for avoiding clicks)
            float gg = (gain_t < gain) ? gain_att_g : gain_rel_g; // when reducing gain -> attack (faster)
            gain += gg * (gain_t - gain);

            // apply to high band only
            p_out[n] = low + high * gain;
        }

        ds->env[ch]  = env;
        ds->gain[ch] = gain;
    }
}













//===========================================================
// Local Function
//===========================================================

// Convert time constant to block smoothing gain (same style as your tone_ctrl)
static inline float time_ms_to_block_g(float time_ms, float frame_samples)
{
    // g = 1 - exp(-frame / (Fs * tau)), tau = time_ms/1000
    float t_s = fmaxf(time_ms, 0.1f) * 0.001f;
    float N   = t_s * (float)SAMPLE_RATE;         // samples for time constant
    return 1.0f - expf(-(frame_samples) / fmaxf(N, 1.0f));
}

// RBJ cookbook: 2nd order Butterworth LPF / HPF (Q=0.707)
static void biquad_make_lpf(biquad_t* bq, float fc_Hz)
{
    const float Q = 0.70710678f;
    float w0 = 2.0f * (float)M_PI * fc_Hz / (float)SAMPLE_RATE;
    float c  = cosf(w0);
    float s  = sinf(w0);
    float alpha = s / (2.0f * Q);

    float b0 = (1.0f - c) * 0.5f;
    float b1 = (1.0f - c);
    float b2 = (1.0f - c) * 0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f * c;
    float a2 =  1.0f - alpha;

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
}

static void biquad_make_hpf(biquad_t* bq, float fc_Hz)
{
    const float Q = 0.70710678f;
    float w0 = 2.0f * (float)M_PI * fc_Hz / (float)SAMPLE_RATE;
    float c  = cosf(w0);
    float s  = sinf(w0);
    float alpha = s / (2.0f * Q);

    float b0 =  (1.0f + c) * 0.5f;
    float b1 = -(1.0f + c);
    float b2 =  (1.0f + c) * 0.5f;
    float a0 =   1.0f + alpha;
    float a1 =  -2.0f * c;
    float a2 =   1.0f - alpha;

    bq->b0 = b0 / a0;
    bq->b1 = b1 / a0;
    bq->b2 = b2 / a0;
    bq->a1 = a1 / a0;
    bq->a2 = a2 / a0;
}

// DF2T biquad process (mono)
static inline float biquad_df2t(biquad_t bq, biquad_stat_t* st, float x)
{
    float y = bq.b0 * x + st->z1;
    st->z1  = bq.b1 * x - bq.a1 * y + st->z2;
    st->z2  = bq.b2 * x - bq.a2 * y;
    return y;
}

static inline float deesser_calc_sat_env(float thr_lin, float ratio, float max_gr_dB)
{
    float r = fmaxf(ratio, 1.0f);
    float expn = 1.0f - (1.0f / r);

    if( (max_gr_dB <= 0.0f) || (expn <= 0.0001f) )
    {
        return thr_lin;
    }

    /*
     * Lightweight approximation of the point where the gain curve reaches
     * max_gr_dB. 6 dB is treated as roughly x2 in linear amplitude.
     * Example: ratio=3, max_gr=4 dB -> sat around 2x threshold.
     */
    return thr_lin * (1.0f + (max_gr_dB / (6.0f * expn)));
}


static inline float deesser_gain_from_env(float env, float thr_lin, float sat_lin, float gmin_lin)
{
    if( env <= thr_lin )
    {
        return 1.0f;
    }

    if( sat_lin <= thr_lin )
    {
        return gmin_lin;
    }

    if( env >= sat_lin )
    {
        return gmin_lin;
    }

    float t = (env - thr_lin) / (sat_lin - thr_lin);

    if( t < 0.0f )
    {
        t = 0.0f;
    }
    else if( t > 1.0f )
    {
        t = 1.0f;
    }

    // smoothstep knee from 1.0 to max gain-reduction limit.
    t = t * t * (3.0f - (2.0f * t));

    return 1.0f + ((gmin_lin - 1.0f) * t);
}











//===========================================================
// API
//===========================================================

static deesser_t My_Deesser;

void app_deesser_init(void)
{
    deesser_init(&My_Deesser, DEESSER_MAX_CH, 300.0f);

    // úliÀS¤j
    deesser_set_params(&My_Deesser,
                       -24.0f,     // thr_dB
                        3.0f,      // ratio
                        4.0f,      // max GR
                        2.0f,      // attack ms
                        120.0f,    // release ms
                        5000.0f,   // split fc
                        6000.0f);  // detector HPF fc
}



void app_deesser_process(const float* in, float* out)
{
    deesser_process(&My_Deesser, in, out, APP_BLOCK_FRAMES);
}


#endif //defined(ENA_DEESSER)

