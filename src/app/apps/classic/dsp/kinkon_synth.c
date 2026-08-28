#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "apps/shared/float_conversion.h"
#include "kinkon_tables.h"


#include "kinkon_synth.h"




#if defined(ENA_KINKON)
//===========================================================
// Definition
//===========================================================


//===========================================================
// Local helpers
//===========================================================

static inline float softclip_tanh(float x)
{
    return tanhf(x);
}


static inline uint32_t ms_to_samples(float ms)
{
    float s = (ms * 0.001f) * (float)KINKON_INTERNAL_SAMPLE_RATE_HZ;
    uint32_t v = (uint32_t)(s + 0.5f);
    return (v < 1u) ? 1u : v;
}


static inline uint32_t current_len(uint8_t ev)
{
    switch ((kinkon_event_t)ev)
    {
        case KINKON_EVT_A: return KINKON_TICKA_LEN;
        case KINKON_EVT_B: return KINKON_TICKB_LEN;
        case KINKON_EVT_C: return KINKON_TICKC_LEN;
        default:           return 0u;
    }
}


static inline const int16_t* current_table(uint8_t ev)
{
    switch ((kinkon_event_t)ev)
    {
        case KINKON_EVT_A: return g_kinkon_tickA;
        case KINKON_EVT_B: return g_kinkon_tickB;
        case KINKON_EVT_C: return g_kinkon_tickC;
        default:           return NULL;
    }
}


static inline bool event_finished(const kinkon_t* p)
{
    uint32_t len;

    if( p == NULL )
    {
        return true;
    }

    len = current_len(p->current_event);

    if( len == 0u )
    {
        return true;
    }

    return (p->tick_pos >= len);
}


static inline void reset_event_playback(kinkon_t* p, uint8_t ev)
{
    uint32_t len;

    p->current_event = ev;
    len              = current_len(ev);
    p->tick_pos      = len;
}


static inline void start_event(kinkon_t* p, uint8_t ev)
{
    p->current_event = ev;
    p->tick_pos      = 0u;
}


static inline float get_event_sample_48k(kinkon_t* p)
{
    const int16_t* table;
    uint32_t       len;
    float          tick;

    if( p == NULL )
    {
        return 0.0f;
    }

    len = current_len(p->current_event);

    if( (len == 0u) || event_finished(p) )
    {
        p->tick_pos = len;
        return 0.0f;
    }

    table = current_table(p->current_event);

    if( table == NULL )
    {
        p->tick_pos = len;
        return 0.0f;
    }

    // The tables are int16 scaled by 1/32768; the scale is a power of two, so
    // this reconstruction is exact (see tools/classic/gen_dsp_tick_tables.py).
    tick = (float)table[p->tick_pos] * KINKON_TICK_SCALE;

    p->tick_pos++;

    if( p->tick_pos >= len )
    {
        p->tick_pos = len;
    }

    return tick;
}


static inline void finish_graceful_stop(kinkon_t* p)
{
    p->enable              = false;
    p->stop_pending        = false;
    p->cycle_phase_samples = 0u;
    p->seq_state           = 4u;
    reset_event_playback(p, KINKON_EVT_A);
}


static inline float local_kinkon_scheduler_process_sample_48k(kinkon_t* p)
{
    float tick;

    if( p == NULL )
    {
        return 0.0f;
    }

    if( !p->enable )
    {
        return 0.0f;
    }

    // --- scheduler ---
    if (p->seq_state == 0u)
    {
        // Normal cycle start: A
        start_event(p, KINKON_EVT_A);
        p->seq_state = 1u; // wait for B
        p->cycle_phase_samples = 0u;
    }
    else if (p->seq_state == 1u)
    {
        // Normal B still happens even after request_stop()
        if (p->cycle_phase_samples == p->gap_samples)
        {
            start_event(p, KINKON_EVT_B);
            p->seq_state = 2u; // wait for next cycle head
        }
    }
    else if (p->seq_state == 2u)
    {
        if (p->cycle_phase_samples >= p->period_samples)
        {
            p->cycle_phase_samples = 0u;

            if (p->stop_pending)
            {
                // After finishing the normal A+B cycle,
                // output final C at the NEXT cycle head.
                start_event(p, KINKON_EVT_C);
                p->seq_state = 3u; // wait for C tail to finish, then stop
            }
            else
            {
                // Continue normal operation with next A
                start_event(p, KINKON_EVT_A);
                p->seq_state = 1u;
            }
        }
    }

    // Generate event sample in the 48 kHz FX domain
    tick = get_event_sample_48k(p);

    tick *= p->tick_gain;
    tick = softclip_tanh(tick * 1.10f);

    if (p->seq_state == 3u && p->current_event == KINKON_EVT_C && event_finished(p))
    {
        // Graceful-stop finished after the final C sample has been output.
        finish_graceful_stop(p);
    }

    if (p->enable)
    {
        p->cycle_phase_samples++;
        if (p->cycle_phase_samples > p->period_samples && p->seq_state != 2u)
        {
            p->cycle_phase_samples = p->period_samples;
        }
    }

    return tick;
}


//===========================================================
// Core API
//===========================================================

void kinkon_init_48k(kinkon_t* p)
{
    if (p == NULL) return;
    memset(p, 0, sizeof(*p));

    p->enable           = false;
    p->stop_pending     = false;

    // defaults from v5 repro pack
    p->period_samples = ms_to_samples(1.233792f * 1000.0f);
    p->gap_samples    = ms_to_samples(0.369062f * 1000.0f);

    p->cycle_phase_samples = 0u;
    p->seq_state           = 4u; // idle/stopped
    reset_event_playback(p, KINKON_EVT_A);
    p->tick_gain           = 0.35f;

    p->bright = 1.0f;
    p->ring   = 1.0f;
    p->tail   = 1.0f;
    p->attack = 1.0f;
}

void kinkon_set_enable(kinkon_t* p, bool en)
{
    if (p == NULL) return;

    if (en)
    {
        // Always restart from KIN (A)
        p->enable              = true;
        p->stop_pending        = false;
        p->cycle_phase_samples = 0u;
        p->seq_state           = 0u; // scheduler will start A on next sample
        reset_event_playback(p, KINKON_EVT_A);
    }
    else
    {
        // Immediate stop. Do NOT output pattern C here.
        finish_graceful_stop(p);
    }
}

void kinkon_request_stop(kinkon_t* p)
{
    if (p == NULL) return;
    if (!p->enable) return;

    // Graceful stop:
    // keep the current cycle normal (A then B),
    // then at the NEXT cycle head output final C and stop.
    p->stop_pending = true;
}

void kinkon_set_period_ms(kinkon_t* p, float period_ms)
{
    if (p == NULL) return;
    if (period_ms < 100.0f)  period_ms = 100.0f;
    if (period_ms > 3000.0f) period_ms = 3000.0f;

    p->period_samples = ms_to_samples(period_ms);
    if (p->gap_samples >= p->period_samples)
    {
        p->gap_samples = (p->period_samples > 2u) ? (p->period_samples / 2u) : 1u;
    }
}

void kinkon_set_gap_ms(kinkon_t* p, float gap_ms)
{
    if (p == NULL) return;
    if (gap_ms < 10.0f)   gap_ms = 10.0f;
    if (gap_ms > 1500.0f) gap_ms = 1500.0f;

    p->gap_samples = ms_to_samples(gap_ms);
    if (p->gap_samples >= p->period_samples)
    {
        p->gap_samples = (p->period_samples > 2u) ? (p->period_samples / 2u) : 1u;
    }
}

void kinkon_set_gain(kinkon_t* p, float gain)
{
    if (p == NULL) return;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    p->tick_gain = gain;
}

void kinkon_set_params(kinkon_t* p, float bright, float ring, float tail, float attack)
{
    if (p == NULL) return;
    p->bright = bright;
    p->ring   = ring;
    p->tail   = tail;
    p->attack = attack;
}


float kinkon_process_sample_48k(kinkon_t* p)
{
    return local_kinkon_scheduler_process_sample_48k(p);
}


//===========================================================
// app_ wrapper
//===========================================================

static kinkon_t g_kinkon;

void app_kinkon_init_48k(void)
{
    kinkon_init_48k(&g_kinkon);

    kinkon_set_period_ms(&g_kinkon, 1233.792000f);
    kinkon_set_gap_ms(&g_kinkon, 369.062000f);

    // Change to project macro later if desired, e.g. Post_Gain_KinKon
//    kinkon_set_gain(&g_kinkon, 0.35f);
//    kinkon_set_gain(&g_kinkon, 0.15f);
    kinkon_set_gain(&g_kinkon, Gain_KinKon);
    kinkon_set_params(&g_kinkon, 1.0f, 1.0f, 1.0f, 1.0f);
}

float app_kinkon_process_sample_48k(void)
{
    return kinkon_process_sample_48k(&g_kinkon);
}

void app_kinkon_set_enable(bool en)
{
    kinkon_set_enable(&g_kinkon, en);
}

void app_kinkon_request_stop(void)
{
    kinkon_request_stop(&g_kinkon);
}

void app_kinkon_set_period_ms(float period_ms)
{
    kinkon_set_period_ms(&g_kinkon, period_ms);
}

void app_kinkon_set_gap_ms(float gap_ms)
{
    kinkon_set_gap_ms(&g_kinkon, gap_ms);
}

void app_kinkon_set_gain(float gain)
{
    kinkon_set_gain(&g_kinkon, gain);
}

void app_kinkon_set_params(float bright, float ring, float tail, float attack)
{
    kinkon_set_params(&g_kinkon, bright, ring, tail, attack);
}

#endif // defined(ENA_KINKON)
