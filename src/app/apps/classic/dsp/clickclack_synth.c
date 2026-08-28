
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "apps/shared/float_conversion.h"


#include "clickclack_tables.h"
#include "clickclack_synth.h"


#if defined(ENA_CLICK_CLACK)
//===========================================================
// Definition
//===========================================================


//===========================================================
// Local helpers
//===========================================================

static inline float softclip_tanh(float x)
{
    // Very mild soft clip. Replace with a cheaper approx later if needed.
    return tanhf(x);
}


static inline void local_reset_tick_playback( clickclack_t* p )
{
    p->tick_pos = CLICKCLACK_TICK_LEN; // idle
}


static inline void local_start_tick_playback( clickclack_t* p )
{
    p->tick_pos = 0u;
}


static inline float local_get_tick_sample_48k( clickclack_t* p )
{
    const int16_t* table;
    float          tick;

    if( p->tick_pos >= CLICKCLACK_TICK_LEN )
    {
        p->tick_pos = CLICKCLACK_TICK_LEN;
        return 0.0f;
    }

    table = (p->ab == 0u) ? g_clickclack_tickA : g_clickclack_tickB;
    // The tables are int16 scaled by 1/32768 (see
    // tools/classic/gen_dsp_tick_tables.py).
    tick  = (float)table[p->tick_pos] * CLICKCLACK_TICK_SCALE;

    p->tick_pos++;

    if( p->tick_pos >= CLICKCLACK_TICK_LEN )
    {
        p->tick_pos = CLICKCLACK_TICK_LEN;
    }

    return tick;
}


//===========================================================
// Global instance for app_ wrapper
//===========================================================


//===========================================================
// Core API
//===========================================================

void clickclack_init_48k(clickclack_t* p)
{
    if (p == NULL) return;
    memset(p, 0, sizeof(*p));

    p->enable         = false;

    p->period_samples = 1u;     // initialize only
    p->phase_samples  = 0u;
    p->ab             = 0u;     // start with A
    p->tick_gain      = 0.35f;  // safe default (mix headroom)

    local_reset_tick_playback( p );

    // Default "tone knobs" (kept for future real-time synth)
    p->metal  = 1.0f;
    p->ring   = 1.0f;
    p->mech   = 1.0f;
    p->attack = 1.0f;
}

void clickclack_set_enable(clickclack_t* p, bool en)
{
    if (p == NULL) return;

    p->ab = 0u; // start with A
    local_reset_tick_playback( p );

    p->phase_samples = 0u;
    p->enable        = en;
}

void clickclack_set_period_ms(clickclack_t* p, float period_ms)
{
    if (p == NULL) return;
    if (period_ms < 50.0f)  period_ms = 50.0f;
    if (period_ms > 2000.0f) period_ms = 2000.0f;

    float s = (period_ms * 0.001f) * (float)CLICKCLACK_INTERNAL_SAMPLE_RATE_HZ;
    uint32_t ps = (uint32_t)(s + 0.5f);
    if (ps < 1u) ps = 1u;
    p->period_samples = ps;
}

void clickclack_set_gain(clickclack_t* p, float gain)
{
    if (p == NULL) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    p->tick_gain = gain;
}

void clickclack_set_params(clickclack_t* p, float metal, float ring, float mech, float attack)
{
    if (p == NULL) return;
    p->metal  = metal;
    p->ring   = ring;
    p->mech   = mech;
    p->attack = attack;
}

float clickclack_process_sample_48k(clickclack_t* p)
{
    float tick;

    if (p == NULL)
    {
        return 0.0f;
    }

    if (!p->enable)
    {
        return 0.0f;
    }

    // --- Tick scheduler ---
    // At phase == 0, start a new tick and toggle A/B
    if (p->phase_samples == 0u)
    {
        local_start_tick_playback( p );
        p->ab ^= 1u; // toggle A/B each period
    }

    // generate tick sample in the 48 kHz FX domain
    tick = local_get_tick_sample_48k( p );

    // apply gain (+ mild softclip for safety)
    tick *= p->tick_gain;
    tick = softclip_tanh(tick * 1.10f);

    // advance phase
    p->phase_samples++;
    if (p->phase_samples >= p->period_samples)
    {
        p->phase_samples = 0u;
    }

    return tick;
}




//===========================================================
// API
//===========================================================

static clickclack_t g_clickclack;


void app_clickclack_init_48k(void)
{
    clickclack_init_48k(&g_clickclack);

    // Match the user's chosen cadence
    clickclack_set_period_ms(&g_clickclack, 400.0f);

    // These initial values reproduce the current "best sounding" stream
    // (original_recon_residual_noise_period_0p40s_30s.wav) closely.
    clickclack_set_gain(&g_clickclack, Gain_ClickClack);
    clickclack_set_params(&g_clickclack, 1.0f, 1.0f, 1.0f, 1.0f);
}

float app_clickclack_process_sample_48k(void)
{
    return clickclack_process_sample_48k(&g_clickclack);
}

void app_clickclack_set_enable(bool en)
{
    clickclack_set_enable(&g_clickclack, en);
}

void app_clickclack_set_period_ms(float period_ms)
{
    clickclack_set_period_ms(&g_clickclack, period_ms);
}

void app_clickclack_set_gain(float gain)
{
    clickclack_set_gain(&g_clickclack, gain);
}

void app_clickclack_set_params(float metal, float ring, float mech, float attack)
{
    clickclack_set_params(&g_clickclack, metal, ring, mech, attack);
}
#endif //defined(ENA_CLICK_CLACK)
