#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "gain_ctrl.h"


#include "pinger_synth.h"


#if defined(ENA_PINGER_SOUND)
//===========================================================
// Definition
//===========================================================

#define F_M_PI                     ((float)M_PI)

#define PINGER_FREQ_0_HZ           (783.67f)
#define PINGER_FREQ_1_HZ           (1045.82f)

#define PINGER_GAIN_0              (0.58546306f)
#define PINGER_GAIN_1              (0.14744467f)

#define PINGER_PHASE_0_RAD         (1.86780830f)
#define PINGER_PHASE_1_RAD         (-2.65065191f)

/* accepted snapshot */
#define PINGER_ATTACK_MS           (28.5f)
#define PINGER_DECAY_MS            (140.0f)

/*
 * Original measured event starts:
 * 0.43177, 1.43973, 2.44542, 3.45369, ...
 * spacing is roughly ~1.0079 to 1.0090 s.
 * For embedded use, use one fixed period.
 */
#define PINGER_PERIOD_S            (1.0079f)
#define PINGER_START_OFFSET_S      (0.4318f)

/*
 * Finite event length used in offline model.
 * This is not a hard fade tail. It just stops the event after it has
 * already decayed enough.
 */
#define PINGER_EVENT_LEN_S         (0.56f)

#define PINGER_GATE_ATTACK_S       (0.005f)
#define PINGER_GATE_RELEASE_S      (0.030f)
#define PINGER_GATE_EPS            (0.000001f)
#define PINGER_INTERNAL_SAMPLE_RATE_HZ     (48000u)

//===========================================================
// Local Function
//===========================================================



static inline float pinger_get_valid_fs(float fs)
{
    if (fs > 0.0f)
    {
        return fs;
    }

    /* Fallback only. Normal operation should pass fs via init. */
    return (float)SAMPLE_RATE;
}


static inline float pinger_wrap_phase(float x)
{
    while (x >= (2.0f * F_M_PI)) x -= (2.0f * F_M_PI);
    while (x < 0.0f)             x += (2.0f * F_M_PI);
    return x;
}

static inline float pinger_alpha_from_tau(float fs, float tau_s)
{
    if (tau_s <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (fs * tau_s));
}

static inline float pinger_dc_block(float x, float *x1, float *y1)
{
    float y = x - *x1 + 0.995f * (*y1);
    *x1 = x;
    *y1 = y;
    return y;
}

static inline void pinger_update_env_coeff(pinger_synth_t *s)
{
    /*
     * Envelope:
     * e[n] = attack_state * decay_state
     *
     * attack_state <- attack_state + a_att * (1 - attack_state)
     * decay_state  <- decay_state  * d_dec
     */
    s->attack_coeff = pinger_alpha_from_tau(s->fs, s->ta_s);
    s->decay_coeff  = expf(-1.0f / (s->fs * s->td_s));
}

static inline void pinger_start_event(pinger_synth_t *s)
{
    s->pulse_active = 1u;
    s->event_elapsed_samples = 0u;
    s->env = 0.0f;
}

static inline float pinger_osc_sum(pinger_synth_t *s)
{
    float y = 0.0f;

    for (uint8_t i = 0; i < PINGER_OSC_NUM; i++)
    {
        y += s->osc[i].gain * sinf(s->osc[i].phase);
        s->osc[i].phase = pinger_wrap_phase(s->osc[i].phase + s->osc[i].step);
    }

    return y;
}

//===========================================================
// Global Function
//===========================================================

void pinger_synth_init(pinger_synth_t *s, float fs)
{
    memset(s, 0, sizeof(*s));
    s->fs = pinger_get_valid_fs(fs);

    fs = s->fs;

    s->osc[0].phase = PINGER_PHASE_0_RAD;
    s->osc[0].step  = (2.0f * F_M_PI * PINGER_FREQ_0_HZ) / fs;
    s->osc[0].gain  = PINGER_GAIN_0;

    s->osc[1].phase = PINGER_PHASE_1_RAD;
    s->osc[1].step  = (2.0f * F_M_PI * PINGER_FREQ_1_HZ) / fs;
    s->osc[1].gain  = PINGER_GAIN_1;

    s->ta_s = PINGER_ATTACK_MS * 0.001f;
    s->td_s = PINGER_DECAY_MS  * 0.001f;
    pinger_update_env_coeff(s);
    s->attack_state = 0.0f;
    s->decay_state  = 0.0f;

    s->event_period_samples       = (uint32_t)(PINGER_PERIOD_S * fs + 0.5f);
    s->event_start_offset_samples = (uint32_t)(PINGER_START_OFFSET_S * fs + 0.5f);
    s->event_len_samples          = (uint32_t)(PINGER_EVENT_LEN_S * fs + 0.5f);

    s->sample_count      = 0u;
    s->next_event_sample = s->event_start_offset_samples;

//    s->master_gain = db_to_lin(-18.0f);
    s->master_gain = db_to_lin(-22.0f);

    s->gate = 0.0f;
    s->gate_target = 0.0f;
    s->gate_attack_alpha  = pinger_alpha_from_tau(fs, PINGER_GATE_ATTACK_S);
    s->gate_release_alpha = pinger_alpha_from_tau(fs, PINGER_GATE_RELEASE_S);
}

void pinger_synth_set_master_gain_db(pinger_synth_t *s, float db)
{
    s->master_gain = db_to_lin(db);
}

void pinger_synth_set_decay_ms(pinger_synth_t *s, float td_ms)
{
    s->td_s = td_ms * 0.001f;
    pinger_update_env_coeff(s);
}

void pinger_synth_set_attack_ms(pinger_synth_t *s, float ta_ms)
{
    s->ta_s = ta_ms * 0.001f;
    pinger_update_env_coeff(s);
}

void pinger_synth_gate_on(pinger_synth_t *s)
{
    pinger_synth_init( s, s->fs );

    s->gate_target = 1.0f;
}

void pinger_synth_gate_off(pinger_synth_t *s)
{
    s->gate_target = 0.0f;
}

float pinger_synth_process_sample(pinger_synth_t *s)
{
    float y = 0.0f;
    float gate_alpha;

    if (s == NULL)
    {
        return 0.0f;
    }

    /*
     * Fully silent state:
     * Do not spend oscillator/envelope cycles.
     * pinger_synth_gate_on() restarts the timing, so sample_count does not
     * need to advance while fully gated off.
     */
    if ((s->gate_target <= 0.0f) && (s->gate <= PINGER_GATE_EPS) && (s->pulse_active == 0u))
    {
        return 0.0f;
    }

    /* global on/off smoothing */
    gate_alpha = (s->gate_target > s->gate) ? s->gate_attack_alpha : s->gate_release_alpha;
    s->gate += gate_alpha * (s->gate_target - s->gate);

    /* periodic trigger */
    if (s->sample_count == s->next_event_sample)
    {
        pinger_start_event(s);
        s->attack_state = 0.0f;
        s->decay_state  = 1.0f;
        s->next_event_sample += s->event_period_samples;
    }

    if (s->pulse_active != 0u)
    {
        float tone;

        s->attack_state += s->attack_coeff * (1.0f - s->attack_state);
        s->decay_state  *= s->decay_coeff;
        s->env = s->attack_state * s->decay_state;

        tone = pinger_osc_sum(s);
        y = tone * s->env;

        s->event_elapsed_samples++;
        if (s->event_elapsed_samples >= s->event_len_samples)
        {
            s->pulse_active = 0u;
            s->env = 0.0f;
        }
    }

    y *= (s->master_gain * s->gate);
    y = pinger_dc_block(y, &s->dc_x1, &s->dc_y1);

    if (y > 1.0f)  y = 1.0f;
    if (y < -1.0f) y = -1.0f;

    s->sample_count++;

    return y;
}


//===========================================================
// API
//===========================================================

static pinger_synth_t g_pinger;

void app_pinger_init_48k(void)
{
    /*
     * app_pinger_* is a 48 kHz mono source for fx_domain_48k.
     * The system sample rate is handled only by fx_domain_48k.
     */
    pinger_synth_init(&g_pinger, (float)PINGER_INTERNAL_SAMPLE_RATE_HZ);
    pinger_synth_gate_off(&g_pinger);
}

float app_pinger_process_sample_48k(void)
{
    return pinger_synth_process_sample(&g_pinger);
}


void app_pinger_set_enable(bool enable)
{
    if (enable)
    {
        pinger_synth_gate_on(&g_pinger);
    }
    else
    {
        pinger_synth_gate_off(&g_pinger);
    }
}

#endif //defined(ENA_PINGER_SOUND)
