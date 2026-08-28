/* =========================================================================
 * LAMB AVAS synth : low signature + mid resonant v5
 *
 * This version is adapted for memory-chain integration with interleaved 
 * stereo buffers (L/R).
 * ========================================================================= */

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
#include "avas_synth.h"


#if defined(ENA_AVAS_SYNTH)
//===========================================================
// Definition
//===========================================================

#define F_M_PI                    ((float)M_PI)


#define AVAS_LOW_MAX_PARTIALS     (8u)
#define AVAS_MID_MAX_PARTIALS     (8u)
#define AVAS_BREATH_MAX_PARTIALS  (8u)
#define AVAS_HIGH_MAX_PARTIALS    (8u)

#define AVAS_MOD_NUM              (4u)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    float freq_hz;
    float gain;
    float phase_rad;
    float mod_depth;
    uint8_t mod_id;
} avas_partial_cfg_t;

typedef struct
{
    float freq_hz;
    float gain;
    float phase_rad;
} avas_resonant_cfg_t;

//===========================================================
// Variables
//===========================================================

static const avas_partial_cfg_t s_low_cfg[] =
{
    {  55.0f, 1.00f, 0.0f, 0.00f, 0u },
    {  82.5f, 0.18f, 1.4f, 0.00f, 0u },
    { 110.0f, 0.35f, 0.7f, 0.00f, 0u },
};

// static const avas_resonant_cfg_t s_resonant_cfg[] =
// {
//     { 320.0f, 0.11f, 0.10f },
//     { 327.9f, 0.26f, 0.50f },
//     { 330.3f, 0.33f, 1.10f },
//     { 331.4f, 0.35f, 1.70f },
//     { 333.8f, 0.24f, 2.30f },
//     { 342.0f, 0.10f, 2.90f },
// 
//     { 450.7f, 0.09f, 0.30f },
//     { 462.7f, 0.18f, 0.90f },
//     { 471.3f, 0.23f, 1.60f },
//     { 485.0f, 0.12f, 2.10f },
// 
//     { 569.7f, 0.07f, 0.40f },
//     { 581.3f, 0.12f, 1.10f },
//     { 590.0f, 0.16f, 1.80f },
//     { 599.0f, 0.11f, 2.40f },
//     { 613.7f, 0.06f, 2.90f },
// };
static const avas_resonant_cfg_t s_resonant_cfg[] =
{
    { 320.0f, 0.11f, 0.10f },
    { 327.9f, 0.26f, 0.50f },
    { 330.3f, 0.33f, 1.10f },
    { 331.4f, 0.35f, 1.70f },
    { 333.8f, 0.24f, 2.30f },
    { 342.0f, 0.10f, 2.90f },

    { 450.7f, 0.09f, 0.30f },
    { 462.7f, 0.18f, 0.90f },
    { 471.3f, 0.23f, 1.60f },
    { 485.0f, 0.12f, 2.10f },

    { 569.7f, 0.07f, 0.40f },
    { 581.3f, 0.12f, 1.10f },
    { 590.0f, 0.16f, 1.80f },
    { 599.0f, 0.11f, 2.40f },
    { 613.7f, 0.06f, 2.90f },
};

static const avas_resonant_cfg_t s_alert_cfg[] =
{
    {  720.0f, 0.18f, 0.20f },
    {  811.0f, 0.24f, 1.10f },
    {  905.0f, 0.20f, 2.00f },
    { 1022.0f, 0.14f, 2.70f },
};


//===========================================================
// Local Function
//===========================================================

/* Standard Oscillator Process using sinf */
static inline float avas_osc_process_sin(float *phase_rad, float step_rad)
{
    float y = sinf(*phase_rad);

    *phase_rad += step_rad;
    if (*phase_rad >= (2.0f * F_M_PI))
    {
        *phase_rad -= (2.0f * F_M_PI);
    }

    return y;
}

static inline float avas_dc_block(float x, float *x1, float *y1)
{
    float y = x - *x1 + 0.995f * (*y1);
    *x1 = x;
    *y1 = y;
    return y;
}

static inline float avas_alpha_from_tau(float fs, float tau_s)
{
    if (tau_s <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (fs * tau_s));
}

static void avas_set_low_partial(
    avas_synth_t *s,
    uint8_t idx,
    float freq_hz,
    float gain,
    float phase_rad,
    float mod_depth,
    uint8_t mod_id)
{
    s->low_partial[idx].phase = phase_rad;
    s->low_partial[idx].step  = (2.0f * F_M_PI * freq_hz) / s->fs;
    s->low_partial[idx].gain  = gain;
    s->low_partial[idx].mod_depth = mod_depth;
    s->low_partial[idx].mod_id    = mod_id;
}

static void avas_set_resonant_partial(
    avas_synth_t *s,
    uint8_t idx,
    float freq_hz,
    float gain,
    float phase_rad)
{
    s->resonant_partial[idx].phase = phase_rad;
    s->resonant_partial[idx].step  = (2.0f * F_M_PI * freq_hz) / s->fs;
    s->resonant_partial[idx].gain  = gain;
}

static void avas_set_alert_partial(
    avas_synth_t *s,
    uint8_t idx,
    float freq_hz,
    float gain,
    float phase_rad)
{
    s->alert_partial[idx].phase = phase_rad;
    s->alert_partial[idx].step  = (2.0f * F_M_PI * freq_hz) / s->fs;
    s->alert_partial[idx].gain  = gain;
}

static float avas_process_low_block(avas_synth_t *s)
{
    float y = 0.0f;
    for (uint8_t i = 0; i < s->low_num; i++)
    {
        y += s->low_partial[i].gain
           * avas_osc_process_sin(&s->low_partial[i].phase, s->low_partial[i].step);
    }

    y *= s->low_gain;
    y = avas_dc_block(y, &s->low_dc_x1, &s->low_dc_y1);
    return y;
}

static float avas_process_resonant_block(avas_synth_t *s)
{
    float y = 0.0f;
    float env = s->resonant_env_base;

    for (uint8_t i = 0; i < AVAS_LFO_NUM; i++)
    {
        env += s->lfo[i].depth * avas_osc_process_sin(&s->lfo[i].phase, s->lfo[i].step);
    }

    if (env < 0.0f) env = 0.0f;
    if (env > 1.5f) env = 1.5f;

    for (uint8_t i = 0; i < s->resonant_num; i++)
    {
        y += s->resonant_partial[i].gain
           * avas_osc_process_sin(&s->resonant_partial[i].phase, s->resonant_partial[i].step);
    }

    y *= (s->resonant_gain * env);
    y = avas_dc_block(y, &s->resonant_dc_x1, &s->resonant_dc_y1);
    return y;
}

static float avas_process_alert_block(avas_synth_t *s)
{
    float y = 0.0f;
    float env = s->alert_env_base;

    for (uint8_t i = 0; i < AVAS_ALERT_LFO_NUM; i++)
    {
        env += s->alert_lfo[i].depth
            * avas_osc_process_sin(&s->alert_lfo[i].phase, s->alert_lfo[i].step);
    }

    /* depth parameter reserved for future expansion */
    env = 1.0f + (env - 1.0f) * s->alert_env_depth;

    if (env < 0.0f) env = 0.0f;
    if (env > 1.6f) env = 1.6f;

    for (uint8_t i = 0; i < s->alert_num; i++)
    {
        y += s->alert_partial[i].gain
           * avas_osc_process_sin(&s->alert_partial[i].phase, s->alert_partial[i].step);
    }

    y *= (s->alert_gain * env);
    y = avas_dc_block(y, &s->alert_dc_x1, &s->alert_dc_y1);
    return y;
}


//===========================================================
// Global Function
//===========================================================

void avas_synth_init(avas_synth_t *s, float fs)
{
    memset(s, 0, sizeof(*s));
    s->fs = fs;

    s->low_num = (uint8_t)ARRAY_SIZE(s_low_cfg);
    for (uint8_t i = 0; i < s->low_num; i++)
    {
        avas_set_low_partial(s, i, s_low_cfg[i].freq_hz, s_low_cfg[i].gain, 
                             s_low_cfg[i].phase_rad, s_low_cfg[i].mod_depth, s_low_cfg[i].mod_id);
    }

    s->resonant_num = (uint8_t)ARRAY_SIZE(s_resonant_cfg);
    for (uint8_t i = 0; i < s->resonant_num; i++)
    {
        avas_set_resonant_partial(s, i, s_resonant_cfg[i].freq_hz, s_resonant_cfg[i].gain, s_resonant_cfg[i].phase_rad);
    }

    s->alert_num = (uint8_t)ARRAY_SIZE(s_alert_cfg);
    for (uint8_t i = 0; i < s->alert_num; i++)
    {
        avas_set_alert_partial(s, i, s_alert_cfg[i].freq_hz, s_alert_cfg[i].gain, s_alert_cfg[i].phase_rad);
    }

    /* v5 LFOs (step in radians per sample) */
    s->lfo[0].phase = 0.20f; s->lfo[0].step = (2.0f * F_M_PI * 0.41f) / fs; s->lfo[0].depth = 0.085f;
    s->lfo[1].phase = 1.40f; s->lfo[1].step = (2.0f * F_M_PI * 0.73f) / fs; s->lfo[1].depth = 0.055f;
    s->lfo[2].phase = 2.10f; s->lfo[2].step = (2.0f * F_M_PI * 1.17f) / fs; s->lfo[2].depth = 0.030f;

    /* alert LFOs : slow and with some depth */
    s->alert_lfo[0].phase = 0.60f; s->alert_lfo[0].step = (2.0f * F_M_PI * 0.27f) / fs; s->alert_lfo[0].depth = 0.11f;
    s->alert_lfo[1].phase = 1.80f; s->alert_lfo[1].step = (2.0f * F_M_PI * 0.49f) / fs; s->alert_lfo[1].depth = 0.08f;

    s->resonant_env_base  = 0.88f;
    s->resonant_env_depth = 1.0f;
    s->alert_env_base     = 0.92f;
    s->alert_env_depth    = 1.0f;

    s->low_gain       = db_to_lin(-14.0f);
    s->resonant_gain  = db_to_lin(-10.5f);
    s->alert_gain      = db_to_lin(-18.0f);

//    s->master_gain    = db_to_lin(-12.0f);
    s->master_gain    = db_to_lin(-24.0f);

    s->gate = 0.0f;
    s->gate_target = 1.0f;
    s->gate_attack_alpha  = avas_alpha_from_tau(fs, 0.120f);
    s->gate_release_alpha = avas_alpha_from_tau(fs, 0.220f);
}

void avas_synth_reset(avas_synth_t *s)
{
    for (uint8_t i = 0; i < s->low_num; i++)         s->low_partial[i].phase = 0.0f;
    for (uint8_t i = 0; i < s->resonant_num; i++)    s->resonant_partial[i].phase = 0.0f;
    for (uint8_t i = 0; i < AVAS_LFO_NUM; i++)       s->lfo[i].phase = 0.0f;
    for (uint8_t i = 0; i < s->alert_num; i++)       s->alert_partial[i].phase = 0.0f;
    for (uint8_t i = 0; i < AVAS_ALERT_LFO_NUM; i++) s->alert_lfo[i].phase = 0.0f;

    s->gate           = 0.0f;
    s->gate_target    = 1.0f;
    s->low_dc_x1      = s->low_dc_y1      = 0.0f;
    s->resonant_dc_x1 = s->resonant_dc_y1 = 0.0f;
    s->alert_dc_x1    = s->alert_dc_y1    = 0.0f;
}


void avas_synth_process(avas_synth_t *s, float *in, float *out, uint16_t samples, uint16_t num_proc_ch)
{
    /* buffer is expected to be channel-major: ch0[0..samples-1], ch1[0..samples-1], ... */
    for (uint16_t i = 0; i < samples; i++)
    {
        float y;
        float alpha = (s->gate_target > s->gate) ? s->gate_attack_alpha : s->gate_release_alpha;
        s->gate += alpha * (s->gate_target - s->gate);

        // Process synthesis blocks
        y = avas_process_low_block(s);
        y += avas_process_resonant_block(s);
//        y  = avas_process_alert_block(s);

        y *= (s->master_gain * s->gate);

        /* Final Clamp */
        if (y > 1.0f)  y = 1.0f;
        if (y < -1.0f) y = -1.0f;

        // Apply to channel-major buffer (all processing channels)
        for (uint16_t ch = 0; ch < num_proc_ch; ch++)
        {
            uint32_t idx = ((uint32_t)ch * (uint32_t)samples) + (uint32_t)i;
            out[idx] = in[idx] + y;
        }
    }
}

void avas_synth_set_low_gain_db(avas_synth_t *s, float db)      { s->low_gain = db_to_lin(db); }
void avas_synth_set_resonant_gain_db(avas_synth_t *s, float db) { s->resonant_gain = db_to_lin(db); }
void avas_synth_set_alert_gain_db(avas_synth_t *s, float db)    { s->alert_gain = db_to_lin(db); }
void avas_synth_set_master_gain_db(avas_synth_t *s, float db)   { s->master_gain = db_to_lin(db); }
void avas_synth_gate_on(avas_synth_t *s)                        { s->gate_target = 1.0f; }
void avas_synth_gate_off(avas_synth_t *s)                       { s->gate_target = 0.0f; }

//===========================================================
// API
//===========================================================

static avas_synth_t g_avas;

void app_avas_init(void)
{
    avas_synth_init(&g_avas, (float)SAMPLE_RATE);
    avas_synth_gate_on(&g_avas);
}


void app_avas_process(float *in, float *out)
{
    avas_synth_process(&g_avas, in, out, (uint16_t)APP_BLOCK_FRAMES, (uint16_t)STAGE_1_PROC_CH);
}

#endif //defined(ENA_AVAS_SYNTH)


