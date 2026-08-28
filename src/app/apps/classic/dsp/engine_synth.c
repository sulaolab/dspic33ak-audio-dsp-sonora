
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
#include "board/devices/pot_drv.h"
#include "apps/shared/float_conversion.h"


#include "engine_synth.h"


#if defined(ENA_ENGINE_SYNTH)
//===========================================================
// Definition
//===========================================================

#define MIN_IDLE_RPM        (200.0f)

#define ENGINE_SYNTH_INTERNAL_SAMPLE_RATE_HZ  (48000u)
#define ENGINE_SYNTH_FEEDER_SAMPLES          (APP_BLOCK_FRAMES)
#define ENGINE_SYNTH_FEEDER_CH               (2u)


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// Variables
//===========================================================

static enginesynth_wrap_t   s_eng;
static engine_blip_state_t  s_blip;
       bool                 Start_Blip = false;

static float                s_engine_fx_in[ENGINE_SYNTH_FEEDER_CH * ENGINE_SYNTH_FEEDER_SAMPLES];
static float                s_engine_fx_out[ENGINE_SYNTH_FEEDER_CH * ENGINE_SYNTH_FEEDER_SAMPLES];
static uint16_t             s_engine_fx_pos = ENGINE_SYNTH_FEEDER_SAMPLES;


//===========================================================
// Global Function
//===========================================================







/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////



// ==== Blipping implementation ===================================

// Simple easing: 0..1 -> 0..1, p=1 linear, p>1 accelerates toward end
static inline float ease_pow(float x, float p)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return powf(x, (p > 0.0f) ? p : 1.0f);
}

void EngineBlip_init(engine_blip_state_t* b)
{
    // Default parameters
//    b->p.peak_rpm_default = 4500.0f; // Max RPM for blip (fixed peak)
    b->p.peak_rpm_default = 5000.0f; // Max RPM for blip (fixed peak)
//    b->p.attack_ms        = 120.0f;  // Attack time
    b->p.attack_ms        = 500.0f;  // Attack time
//    b->p.hold_ms          = 100.0f;  // Hold time
    b->p.hold_ms          = 400.0f;  // Hold time
//    b->p.decay_ms         = 350.0f;  // Decay time
    b->p.decay_ms         = 1300.0f;  // Decay time
//    b->p.cooldown_ms      = 300.0f;  // Cooldown to prevent rapid retrigger
    b->p.cooldown_ms      = 0.0f;  // Cooldown to prevent rapid retrigger
    b->p.attack_curve     = 1.4f;    // 1.0=linear, >1 accelerates toward end
//    b->p.decay_curve      = 1.2f;    // 1.0=linear, >1 accelerates toward end
    b->p.decay_curve      = 0.1f;    // 1.0=linear, >1 accelerates toward end

    b->phase      = BLIP_IDLE;
    b->t_in_phase = 0.0f;
    b->rpm_start  = 0.0f;
    b->rpm_peak   = b->p.peak_rpm_default;
}

void EngineBlip_trigger(engine_blip_state_t* b, float pot_rpm)
{
    // Decide this blip's peak based on current POT (clamped by safety range)
    float peak = b->p.peak_rpm_default;

    // Ensure at least +800 rpm above POT (gives a light feel)
    if (peak < pot_rpm + 800.0f)
    {
        peak = pot_rpm + 800.0f;
    }

    b->rpm_start  = pot_rpm;
    b->rpm_peak   = peak;
    b->t_in_phase = 0.0f;
    b->phase      = BLIP_ATTACK;
}

float EngineBlip_update(engine_blip_state_t* b, float pot_rpm, float dt_sec)
{
    const engine_blip_params_t* p = &b->p;

    float rpm_cmd = pot_rpm; // Default follows POT

    switch (b->phase)
    {
    case BLIP_IDLE:
        // Do nothing (follow POT)
        break;

    case BLIP_ATTACK:
    {
        b->t_in_phase += dt_sec;
        float T = p->attack_ms * 0.001f;
        float x = clampf((T > 0.0f) ? (b->t_in_phase / T) : 1.0f, 0.0f, 1.0f);
        float e = ease_pow(x, p->attack_curve);
        rpm_cmd = (1.0f - e) * b->rpm_start + e * b->rpm_peak;

        if (x >= 1.0f) { b->phase = BLIP_HOLD; b->t_in_phase = 0.0f; }
        break;
    }

    case BLIP_HOLD:
    {
        b->t_in_phase += dt_sec;
        rpm_cmd = b->rpm_peak;
        float T = p->hold_ms * 0.001f;
        if (b->t_in_phase >= T) { b->phase = BLIP_DECAY; b->t_in_phase = 0.0f; }
        break;
    }

    case BLIP_DECAY:
    {
        b->t_in_phase += dt_sec;
        float T = p->decay_ms * 0.001f;
        float x = clampf((T > 0.0f) ? (b->t_in_phase / T) : 1.0f, 0.0f, 1.0f);
        float e = ease_pow(x, p->decay_curve);

        // Return-to target is the current POT (naturally adapts even if POT moves)
        rpm_cmd = (1.0f - e) * b->rpm_peak + e * pot_rpm;

        if (x >= 1.0f) { b->phase = BLIP_COOLDOWN; b->t_in_phase = 0.0f; }
        break;
    }

    case BLIP_COOLDOWN:
    {
        b->t_in_phase += dt_sec;
        rpm_cmd = pot_rpm;
        float T = p->cooldown_ms * 0.001f;
        if (b->t_in_phase >= T) { b->phase = BLIP_IDLE; b->t_in_phase = 0.0f; }
        break;
    }
    default:
        b->phase = BLIP_IDLE;
        break;
    }

    // Clamp final output to a safe range (e.g., 600~8000 rpm)
//    return clampf(rpm_cmd, 600.0f, 8000.0f);
    return clampf(rpm_cmd, MIN_IDLE_RPM, 8000.0f);
}


/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////














































static void default_tone_for(engine_type_t t, engine_tone_t* tone)
{
    memset(tone, 0, sizeof(*tone));

    tone->baseGain     = 0.35f;
//    tone->outSoftLimit = 0.95f;
    tone->outSoftLimit = 0.65f;


    switch (t)
    {
    default:
    case ENG_I4:  tone->tilt_dB_per_oct = -6.0f; tone->oddBoost_dB  = +2.0f; break;
    case ENG_I5:  tone->tilt_dB_per_oct = -5.0f; tone->oddBoost_dB  = +1.0f; break;
    case ENG_I6:  tone->tilt_dB_per_oct = -4.5f; tone->evenBoost_dB = +2.0f; break;
    case ENG_V8:  tone->tilt_dB_per_oct = -3.5f; tone->evenBoost_dB = +3.0f; break;
    case ENG_V12: tone->tilt_dB_per_oct = -3.0f; tone->evenBoost_dB = +4.0f; break;
    }

    /* Harmonic coefficients are prepared for the full range (ENGINE_MAX_HARM).
       The actual number used is controlled by s->numHarm. */
    for (int n = 0; n < ENGINE_MAX_HARM; n++)
    {
        tone->A_linear[n] = 1.0f / (float)(n + 1);
    }
}

/* --- Public API --- */

void EngineSynth_init(enginesynth_wrap_t* w,
                      float fs,
                      engine_type_t type,
                      int slot_L,
                      int slot_R,
                      int num_proc_ch )
{
    memset(w, 0, sizeof(*w));

    w->num_proc_ch = num_proc_ch;
    w->slot_L      = slot_L;
    w->slot_R      = slot_R;
    w->mixGain     = 0.0f;   // init to gain=0

    engine_synth_t* s = &w->core;
    memset(s, 0, sizeof(*s));
    s->fs = fs;
    s->type = type;
    default_tone_for(type, &s->tone);
    s->rpm_cur = 1000.0f;
    s->rpm_tgt = 1000.0f;
    s->rpm_slew_per_s = 3000.0f;


#if defined(EN_ORG)
    /* Legacy default params */
    w->params.eq_fc_hz          = 110.0f;
    w->params.eq_gain_db        = +6.0f;
    w->params.eq_q              = 0.9f;

    w->params.lpf_min_hz        = 650.0f;
    w->params.lpf_max_hz        = 1800.0f;
    w->params.lpf_q             = 0.707f;
    w->params.lpf_mult          = 18.0f;

    w->params.am_rumble_depth   = 0.22f;
    w->params.am_sub_depth      = 0.10f;

    w->params.noise_post_lpf_hz = 1500.0f;
    w->params.noise_mix         = 0.06f;
#else
    /* Revised default params */
    w->params.eq_fc_hz          = 100.0f;
    w->params.eq_gain_db        = +8.0f;
    w->params.eq_q              = 0.9f;

    w->params.lpf_min_hz        = 550.0f;
    w->params.lpf_max_hz        = 1500.0f;
    w->params.lpf_q             = 0.65f;
    w->params.lpf_mult          = 18.0f;

    w->params.am_rumble_depth   = 0.32f;
    w->params.am_sub_depth      = 0.14f;

    w->params.noise_post_lpf_hz = 1050.0f;
    w->params.noise_mix         = 0.055f;
#endif //defined(EN_ORG)


    /* EO (perceptual model) - V8 uses EO=2.0 from simultaneous/semi-simultaneous firing pairs */

    switch (s->type)
    {
    default:
    case ENG_I4:  w->params.eo = 2.0f; s->numHarm = 12; break;
    case ENG_I5:  w->params.eo = 2.5f; s->numHarm = 12; break;
    case ENG_I6:  w->params.eo = 3.0f; s->numHarm = 10; break;
    case ENG_V8:  w->params.eo = 2.0f; s->numHarm = 8;  break; // Cross-plane V8 perceptual EO=2.0
    case ENG_V12: w->params.eo = 6.0f; s->numHarm = 10; break;
    }

    /* From here, init what used to be static */
    w->rpm_ref_smooth = s->rpm_cur;

    w->lfo1_phase = 0.0f;
    w->lfo2_phase = 0.0f;
    w->rum_phase  = 0.0f;
    w->sub_phase  = 0.0f;

    w->rng        = 0x9E3779B9u;
    w->noise_lp   = 0.0f;

    memset(&w->eq, 0, sizeof(w->eq));
    memset(&w->lp, 0, sizeof(w->lp));
}




void EngineSynth_setGain(enginesynth_wrap_t* w, float mixGain)
{
    w->mixGain = mixGain;
}

void EngineSynth_setRPM(engine_synth_t* s, float rpm_now)
{
    s->rpm_cur = s->rpm_tgt = rpm_now;
}

void EngineSynth_setTargetRPM(engine_synth_t* s, float rpm_target, float slew_rpm_per_s)
{
    s->rpm_tgt = (rpm_target < 0) ? 0 : rpm_target;
    if (slew_rpm_per_s > 0) s->rpm_slew_per_s = slew_rpm_per_s;
}


//temp //===========================================================
//temp // For V8 rumble: even up / odd down and calmer at high RPM
//temp //   - Auto-adjust even/odd/tilt/numHarm based on RPM
//temp //   - Call timing: each frame or when RPM changes
//temp //===========================================================
//temp void EngineRumble_applyV8Tone(engine_synth_t* s, float rpm)
//temp {
//temp     // Base: presets per RPM range for V8
//temp     float evenBoost, oddBoost, tilt;
//temp     int   numHarm;
//temp 
//temp     if (rpm < 1500.0f) {
//temp         evenBoost = +6.0f;   // stronger even
//temp         oddBoost  = -3.0f;   // weaker odd
//temp         tilt      = -3.5f;   // slightly brighter
//temp         numHarm   = 12;      // more harmonics at low RPM
//temp     } else if (rpm < 3000.0f) {
//temp         evenBoost = +4.0f;
//temp         oddBoost  = -2.0f;
//temp         tilt      = -5.0f;
//temp         numHarm   = 10;
//temp     } else {
//temp         evenBoost = +2.0f;
//temp         oddBoost  = -4.0f;   // reduce odd more at high RPM
//temp         tilt      = -6.0f;   // stronger high roll-off (less synthy)
//temp         numHarm   = 8;       // reduce harmonic count
//temp     }
//temp 
//temp     // Apply (do not touch other tone elements)
//temp     engine_tone_t t = s->tone;
//temp     t.evenBoost_dB     = evenBoost;
//temp     t.oddBoost_dB      = oddBoost;
//temp     t.tilt_dB_per_oct  = tilt;
//temp     t.numHarm          = (numHarm > ENGINE_MAX_HARM) ? ENGINE_MAX_HARM : numHarm;
//temp 
//temp     s->tone    = t;
//temp     s->numHarm = t.numHarm;
//temp }

//===========================================================
// LFO (low-frequency wobble) settings: modulate f0 by +/- depth ratio
//   - depth_pct: e.g., 4.0f -> +/-4%
//   - freq_hz  : e.g., 2.0f (gentle at 2 Hz)
//   - Keep as file-scope accessible if you want to call from anywhere
//===========================================================
//static float g_lfo_phase = 0.0f;
//static float g_lfo_freq_hz = 2.0f;
//static float g_lfo_depth = 0.04f;   // = 4% (+/-0.04)

void EngineRumble_setLFO(float freq_hz, float depth_pct)
{
    (void)depth_pct;

    if (freq_hz < 0.1f)  freq_hz = 0.1f;
    if (freq_hz > 10.0f) freq_hz = 10.0f;

//    // % -> ratio
//    float d = depth_pct * 0.01f;
//    if (d < 0.0f) d = 0.0f;
//    if (d > 0.10f) d = 0.10f;  // limit to +/-10% (prevent instability)

//    g_lfo_freq_hz = freq_hz;
//    g_lfo_depth   = d;
}




void EngineSynth_process(enginesynth_wrap_t* w, float* p_in, float* p_out, int samples)
{
    // ==== Fixed params for high-RPM behavior (simple version) ====
    const float RPM_TILT_AM_TH    = 4000.0f;  // threshold for harmonics & AM

    const float TILT_DELTA_DB_OCT = -2.0f;   // additional tilt beyond 4000 rpm (-2 dB/oct)
    const int   HIHARM_START_K    = 6;       // additional damping beyond k>6 at >=4000 rpm
    const float HIHARM_SLOPE_DB   = -1.0f;   // -1 dB per harmonic index above threshold

    const float AM_RUMBLE_SCALE_HI = 0.7f;   // reduce AM(f0/2) to 0.7 beyond 4000 rpm

    // ==== Noise enhancement (smooth interpolation) ====
    const float RPM_NOISE_START   = 4600.0f;   // start ramp here
    const float RPM_NOISE_END     = 6400.0f;   // reach max here
    const float NOISE_BOOST_DB    = +8.0f;     // max boost
    const float NOISE_FC_MULT_MAX = 1.25f;     // max LPF fc multiplier (x1.25)
    const float NOISE_PULSE_MIX   = 0.7f;      // 0=sine, 1=hard half-wave rectified

    engine_synth_t* s = &w->core;
    const float fs = s->fs;

    // ==== Low EQ (peaking EQ) ====
    const float EQ_FC_HZ   = w->params.eq_fc_hz;
    const float EQ_GAIN_DB = w->params.eq_gain_db;
    const float EQ_Q       = w->params.eq_q;

    // ==== Variable lowpass ====
    const float LPF_MIN_HZ = w->params.lpf_min_hz;
    const float LPF_MAX_HZ = w->params.lpf_max_hz;
    const float LPF_MULT   = w->params.lpf_mult;
    const float LPF_Q      = w->params.lpf_q;

    // ==== AM ====
    const float AM_RUMBLE_DEPTH = w->params.am_rumble_depth;
    const float AM_SUB_DEPTH    = w->params.am_sub_depth;

    // ==== Noise ====
    const float NOISE_POST_LPF_HZ = w->params.noise_post_lpf_hz;
    const float NOISE_MIX         = w->params.noise_mix;

    // EO
    const float EO  = w->params.eo;

    float rpm_start = s->rpm_cur;
    float rpm_end   = s->rpm_tgt;

    // Slew-rate limiting
    {
        float max_delta = s->rpm_slew_per_s * ((float)samples / fs);
        float desired   = rpm_end - rpm_start;
        if (desired >  max_delta) rpm_end = rpm_start + max_delta;
        if (desired < -max_delta) rpm_end = rpm_start - max_delta;
    }

    const int   slot_L = w->slot_L;
    const int   slot_R = w->slot_R;
    const float mix_g  = w->mixGain;

    // ---------------- Effective params for V8 (continuous interp + hysteresis) ----------------
    float eff_evenBoost_dB = s->tone.evenBoost_dB;
    float eff_oddBoost_dB  = s->tone.oddBoost_dB;
    float eff_tilt_dB_per_oct = s->tone.tilt_dB_per_oct;
    int   eff_numHarm = s->numHarm;

    // RPM smoothing (preserve wrap)
    {
        const float alpha = 0.2f; // around 5-10 Hz
        float rpm_ref = rpm_end;
        w->rpm_ref_smooth = (1.0f - alpha) * w->rpm_ref_smooth + alpha * rpm_ref;
    }

    if (s->type == ENG_V8) {
        const float L_even=+6.0f, L_odd=-3.0f, L_tilt=-3.5f;
        const float M_even=+4.0f, M_odd=-2.0f, M_tilt=-5.0f;
        const float H_even=+2.0f, H_odd=-5.0f, H_tilt=-10.0f;

        float w_LM, w_MH;
        { float x=(w->rpm_ref_smooth-1400.0f)/200.0f; if(x<0)x=0; if(x>1)x=1; w_LM=x*x*(3-2*x); }
        { float x=(w->rpm_ref_smooth-2900.0f)/200.0f; if(x<0)x=0; if(x>1)x=1; w_MH=x*x*(3-2*x); }

        float even_LM = (1.0f - w_LM) * L_even + w_LM * M_even;
        float odd_LM  = (1.0f - w_LM) * L_odd  + w_LM * M_odd;
        float tilt_LM = (1.0f - w_LM) * L_tilt + w_LM * M_tilt;

        eff_evenBoost_dB    = (1.0f - w_MH) * even_LM + w_MH * H_even;
        eff_oddBoost_dB     = (1.0f - w_MH) * odd_LM  + w_MH * H_odd;
        eff_tilt_dB_per_oct = (1.0f - w_MH) * tilt_LM + w_MH * H_tilt;

        // Further emphasize even up / odd down (more "rumbly")
        eff_evenBoost_dB += +1.5f;
        eff_oddBoost_dB  += -2.0f;

        // numHarm step switching (with hysteresis)
        static int numHarm_state;
        if      (w->rpm_ref_smooth < 1450.0f) numHarm_state = 12;
        else if (w->rpm_ref_smooth > 1550.0f && w->rpm_ref_smooth < 2950.0f) numHarm_state = 8;
        else if (w->rpm_ref_smooth > 3050.0f) numHarm_state = 6;
        else                                  numHarm_state = 12;

        eff_numHarm = numHarm_state;
        if (eff_numHarm > s->numHarm) eff_numHarm = s->numHarm;
        if (eff_numHarm < 1)          eff_numHarm = 1;
    }

    // ---------------- Precompute f0 at frame head and harmonic gains ----------------
    float f0_head = (rpm_start / 60.0f) * EO;
    if (f0_head < 1.0f) f0_head = 1.0f;

    struct { int k_max; float gain_k[ENGINE_MAX_HARM]; } pc;

    {
        int kmax_nyq = (int)floorf((0.48f * fs) / f0_head);
        int kmax = (eff_numHarm < kmax_nyq) ? eff_numHarm : kmax_nyq;
        if (kmax < 1) kmax = 1;
        pc.k_max = kmax;

        const float inv_ln2 = 1.4426950408889634f;
        for (int k = 1; k <= kmax; k++) {
            float A      = s->tone.A_linear[k - 1];
            float add_dB = ((k & 1) ? eff_oddBoost_dB : eff_evenBoost_dB);
            float oct    = logf((float)k) * inv_ln2;
            add_dB      += eff_tilt_dB_per_oct * oct;

            // --- "Drying" at high RPM: beyond 4000 rpm, strengthen tilt and damp high-order ---
            if (rpm_end >= RPM_TILT_AM_TH)
            {
                add_dB += TILT_DELTA_DB_OCT * oct;  // additional -2 dB/oct tilt
                if (k > HIHARM_START_K)
                {
                    add_dB += HIHARM_SLOPE_DB * (float)(k - HIHARM_START_K); // k>6: -1 dB/order
                }
            }
            else
            {
                // Legacy anti-whistle measure (keep for low-mid RPM)
                if (k > 4)
                {
                    add_dB += -2.0f * (float)(k - 4);
                }
            }

            pc.gain_k[k - 1] = A * db_to_lin(add_dB);
        }
    }

    // ---------------- Peaking EQ coefficients (per frame) ----------------
    {
        float A   = powf(10.0f, EQ_GAIN_DB * 0.05f);  // 10^(dB/40)
        float w0  = 2.0f * (float)M_PI * EQ_FC_HZ / fs;
        float c   = cosf(w0);
        float s0  = sinf(w0);
        float alpha = s0 / (2.0f * EQ_Q);

        float b0 = 1.0f + alpha * A;
        float b1 = -2.0f * c;
        float b2 = 1.0f - alpha * A;
        float a0 = 1.0f + alpha / A;
        float a1 = -2.0f * c;
        float a2 = 1.0f - alpha / A;

        w->eq.bq.b0 = b0/a0;
        w->eq.bq.b1 = b1/a0;
        w->eq.bq.b2 = b2/a0;
        w->eq.bq.a1 = a1/a0;
        w->eq.bq.a2 = a2/a0;
    }

    // ---------------- Variable LPF coefficients (smooth fc target -> coeffs) ----------------
    {
        float f0_avg = ((rpm_start + rpm_end) * 0.5f) / 60.0f * EO;
        if (f0_avg < 1.0f) f0_avg = 1.0f;
        float fc_target = LPF_MULT * f0_avg;
        if (fc_target < LPF_MIN_HZ) fc_target = LPF_MIN_HZ;
        if (fc_target > LPF_MAX_HZ) fc_target = LPF_MAX_HZ;

        const float tau_sec = 0.030f;                // 30 ms
        float dt = (float)samples / fs;
        float beta = 1.0f - expf(-dt / tau_sec);
        if (w->lp_fc_smooth <= 0.0f) {
            w->lp_fc_smooth = fc_target;
        } else {
            w->lp_fc_smooth += beta * (fc_target - w->lp_fc_smooth);
        }

        float w0  = 2.0f * (float)M_PI * w->lp_fc_smooth / fs;
        float c   = cosf(w0);
        float s0  = sinf(w0);
        float alpha = s0 / (2.0f * LPF_Q);

        float b0 = (1.0f - c) * 0.5f;
        float b1 =  1.0f - c;
        float b2 = (1.0f - c) * 0.5f;
        float a0 =  1.0f + alpha;
        float a1 = -2.0f * c;
        float a2 =  1.0f - alpha;

        w->lp.bq.b0 = b0/a0;
        w->lp.bq.b1 = b1/a0;
        w->lp.bq.b2 = b2/a0;
        w->lp.bq.a1 = a1/a0;
        w->lp.bq.a2 = a2/a0;
    }

    // ---- Simple first-order LPF for noise (basic IIR)----
//    const float noise_alpha_base = expf(-2.0f * (float)M_PI * NOISE_POST_LPF_HZ / fs);

    // RNG helper
    #define XRAND01() ( \
        w->rng ^= (w->rng<<13), w->rng ^= (w->rng>>17), w->rng ^= (w->rng<<5), \
        ((w->rng & 0xFFFFFFFFu) * 2.3283064365386963e-10f) \
    )

    // Mix gain for detuned bank B (relative)
    const float detune_mix_gain = db_to_lin(-6.0f);

    // Small random detune (move to wrap if you need isolated instances)
    static float detune_jitter_c = 0.0f; // +/- 1 cent
    static float jitter_phase = 0.0f;    // 4 Hz

    for (int smp = 0; smp < samples; smp++)
    {
        float t   = (samples <= 1) ? 1.0f : (float)smp / (float)(samples - 1);
        float rpm = rpm_start + (rpm_end - rpm_start) * t;

        // Base f0
        float f0  = (rpm / 60.0f) * EO;
        if (f0 < 1.0f) f0 = 1.0f;

        // --- Detune computation (RPM-linked + small random) ---
        float detune_c; // cent
        if      (rpm <= 2000.0f) detune_c = 3.0f;
        else if (rpm >= 7000.0f) detune_c = 6.5f;
        else if (rpm <= 5000.0f) {
            float uu = (rpm - 2000.0f)/3000.0f;              // 0..1
            detune_c = 3.0f + (5.0f - 3.0f)*uu;              // 3 -> 5
        } else {
            float uu = (rpm - 5000.0f)/2000.0f;              // 0..1
            detune_c = 5.0f + (6.5f - 5.0f)*uu;              // 5 -> 6.5
        }
        // Slow +/-1 cent random (update around 4 Hz)
        jitter_phase += 2.0f*(float)M_PI * 4.0f / fs;       // 4 Hz
        if (jitter_phase >= 2.0f*(float)M_PI) {
            jitter_phase -= 2.0f*(float)M_PI;
            float r = (XRAND01()*2.0f - 1.0f);              // [-1,1]
//            detune_jitter_c = r * 1.0f;                     // +/-1 cent
/// test
            // Jitter width: +/-1.0 cent -> +/-1.5 cent
            detune_jitter_c = r * 1.5f;  // was 1.0f
            // Jitter update ~4 Hz stays OK (3.5~5 Hz also fine)
        }
        detune_c += detune_jitter_c;

        const float detune_ratio = powf(2.0f, detune_c / 1200.0f); // 2^(cent/1200)

        // === FM (light, continuous) ===
        const float lfo1_f = 3.0f,  lfo1_d = 0.010f;
        const float lfo2_f = 12.0f, lfo2_d = 0.0025f;
        float rum_f = 0.5f * f0; if (rum_f < 0.5f) rum_f = 0.5f;

        w->lfo1_phase += 2.0f*(float)M_PI * lfo1_f / fs;
        w->lfo2_phase += 2.0f*(float)M_PI * lfo2_f / fs;
        w->rum_phase  += 2.0f*(float)M_PI * rum_f  / fs;
        w->sub_phase  += 2.0f*(float)M_PI * (0.25f * f0) / fs;

        if (w->lfo1_phase >= 2.0f*(float)M_PI) w->lfo1_phase -= 2.0f*(float)M_PI;
        if (w->lfo2_phase >= 2.0f*(float)M_PI) w->lfo2_phase -= 2.0f*(float)M_PI;
        if (w->rum_phase  >= 2.0f*(float)M_PI) w->rum_phase  -= 2.0f*(float)M_PI;
        if (w->sub_phase  >= 2.0f*(float)M_PI) w->sub_phase  -= 2.0f*(float)M_PI;

        float wobble_raw = lfo1_d * sinf(w->lfo1_phase)
                         + lfo2_d * sinf(w->lfo2_phase)
                         + 0.016f * sinf(w->rum_phase); // keep f0/2 FM modest

        const float W_MAX = 0.05f; // +/-5%
        float wobble = 1.0f + W_MAX * tanhf(wobble_raw / W_MAX);
        f0 *= wobble;

        // === Phase update ===
        float dphi0 = 2.0f * (float)M_PI * f0 / fs;
        s->phase0 += dphi0;
        if (s->phase0 >= 2.0f*(float)M_PI) s->phase0 -= 2.0f*(float)M_PI;

        // === Harmonic synthesis A (reference bank) ===
        float yA = 0.0f;
        for (int k = 1; k <= pc.k_max; k++) {
            yA += pc.gain_k[k - 1] * sinf(s->phase0 * (float)k);
        }

        // === Harmonic synthesis B (detuned bank 2: V8 only) ===
        float y = yA;
        if (s->type == ENG_V8) {
            float fB = f0 * detune_ratio;
            float dphiB = 2.0f * (float)M_PI * fB / fs;
            w->phase_bankB += dphiB;
            if (w->phase_bankB >= 2.0f*(float)M_PI) w->phase_bankB -= 2.0f*(float)M_PI;

            float yB = 0.0f;
            for (int k = 1; k <= pc.k_max; k++) {
                yB += pc.gain_k[k - 1] * sinf(w->phase_bankB * (float)k);
            }
            y += detune_mix_gain * yB;
        }

        // === AM (f0/2 and f0/4) ===
        {
            // Make AM(f0/2) slightly shallower above 4000 rpm
            float AM_RUMBLE_DEPTH_eff = AM_RUMBLE_DEPTH;
            if (rpm >= RPM_TILT_AM_TH) {
                AM_RUMBLE_DEPTH_eff *= AM_RUMBLE_SCALE_HI; // x0.7
            }
            float am_env = 1.0f
                         + AM_RUMBLE_DEPTH_eff * sinf(w->rum_phase)  // f0/2
                         + AM_SUB_DEPTH        * sinf(w->sub_phase); // f0/4

            if (am_env < 0.0f) am_env = 0.0f;
            y *= am_env;
        }

        // === Small noise (smooth ramp-in + pulse gate) ===
        {
            // u as a function of rpm: 0->1 between RPM_NOISE_START and END
            float x = (rpm - RPM_NOISE_START) / (RPM_NOISE_END - RPM_NOISE_START + 1e-9f);
            if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
            float u = x * x * (3.0f - 2.0f * x); // smoothstep

            // Gate (blend of sine and half-wave rectified)
            float gate_sine = 0.5f + 0.5f * sinf(w->rum_phase);  // 0..1
            float gate_half = fmaxf(0.0f, sinf(w->rum_phase));   // 0..1
            float gate = (1.0f - NOISE_PULSE_MIX) * gate_sine + NOISE_PULSE_MIX * gate_half;

            // Multiply white noise by gate
            float uwhite = (XRAND01() * 2.0f - 1.0f) * gate;     // -1..1

            // Smoothly interpolate noise LPF fc via u (base -> brighter)
            float fc_eff_mult = 1.0f + (NOISE_FC_MULT_MAX - 1.0f) * u;          // 1.0 -> NOISE_FC_MULT_MAX
            float noise_alpha_eff = expf(-2.0f * (float)M_PI
                                         * (NOISE_POST_LPF_HZ * fc_eff_mult) / fs);

            // 1st-order LPF
            w->noise_lp = noise_alpha_eff * w->noise_lp + (1.0f - noise_alpha_eff) * uwhite;

            // Interpolate noise amount by u (0 -> +NOISE_BOOST_DB)
            float noise_mix_eff = NOISE_MIX * (1.0f + (db_to_lin(NOISE_BOOST_DB) - 1.0f) * u);

            y += noise_mix_eff * w->noise_lp;
        }

        // === Shaping -> EQ -> LPF ===
        y *= s->tone.baseGain;
        if (s->tone.outSoftLimit > 0.0f) {
            float lim = s->tone.outSoftLimit;
            y = lim * tanhf(y / lim);
        }

        // Peaking EQ (DF2T)
        float y1     = w->eq.bq.b0 * y + w->eq.bqs.z1;
        w->eq.bqs.z1 = w->eq.bq.b1 * y + w->eq.bqs.z2 - w->eq.bq.a1 * y1;
        w->eq.bqs.z2 = w->eq.bq.b2 * y                - w->eq.bq.a2 * y1;

        // Lowpass (DF2T)
        float     y2 = w->lp.bq.b0 * y1 + w->lp.bqs.z1;
        w->lp.bqs.z1 = w->lp.bq.b1 * y1 + w->lp.bqs.z2 - w->lp.bq.a1 * y2;
        w->lp.bqs.z2 = w->lp.bq.b2 * y1                - w->lp.bq.a2 * y2;

        // Output
        float outval = mix_g * y2;
        if (slot_L >= 0)
        {
            int idx_L = slot_L * samples + smp;
            p_out[idx_L] = p_in[idx_L] + outval;
        }
        if (slot_R >= 0)
        {
            int idx_R = slot_R * samples + smp;
            p_out[idx_R] = p_in[idx_R] + outval;
        }
    }

    s->rpm_cur = rpm_end;
}





//===========================================================
// 48 kHz fx_domain source feeder
//===========================================================

static void local_engine_synth_reset_feeder(void)
{
    memset(s_engine_fx_in,  0, sizeof(s_engine_fx_in));
    memset(s_engine_fx_out, 0, sizeof(s_engine_fx_out));

    s_engine_fx_pos = ENGINE_SYNTH_FEEDER_SAMPLES;
}


static void local_engine_synth_render_block_48k(void)
{
    uint16_t pot;
    float    pot_rpm;
    float    dt;
    float    rpm_cmd;

    memset(s_engine_fx_in,  0, sizeof(s_engine_fx_in));
    memset(s_engine_fx_out, 0, sizeof(s_engine_fx_out));

    // 1) POT -> rpm (arbitrary mapping)
    pot     = POT_Read();                       // 0..0x0FFF
    pot_rpm = (float)pot * ENG_SYNTH_POT_SCALE_FACTOR;

    // 2) Block time [s] in the fixed 48 kHz FX domain
    dt = (float)ENGINE_SYNTH_FEEDER_SAMPLES / (float)ENGINE_SYNTH_INTERNAL_SAMPLE_RATE_HZ;

    // 3) Triggered by user input
    if ( Start_Blip )
    {
        EngineBlip_trigger(&s_blip, pot_rpm);
        Start_Blip = false;
    }

    // 4) Generate this block's RPM command
    rpm_cmd = EngineBlip_update(&s_blip, pot_rpm, dt);

    // 5) Apply to core
    EngineSynth_setTargetRPM(&s_eng.core, rpm_cmd, s_eng.core.rpm_slew_per_s);

    // 6) Render one 48 kHz mono-equivalent block into ch-major scratch buffer.
    //    slot0 and slot1 receive the same engine sample in the existing renderer.
    EngineSynth_process(&s_eng,
                            s_engine_fx_in,
                            s_engine_fx_out,
                            (int)ENGINE_SYNTH_FEEDER_SAMPLES);

    s_engine_fx_pos = 0u;
}


void app_engine_synth_init_48k(void)
{
    // 48 kHz mono source for fx_domain_48k.
    // The system sample rate is handled only by fx_domain_48k.
    EngineSynth_init(&s_eng,
                     (float)ENGINE_SYNTH_INTERNAL_SAMPLE_RATE_HZ,
                     ENG_V8,
                     0/*slot L*/,
                     1/*slot R*/,
                     ENGINE_SYNTH_FEEDER_CH);

    EngineSynth_setGain(&s_eng, Gain_EngineSynth);

    EngineSynth_setRPM(&s_eng.core, 0.0f);    // startup RPM = 0

    EngineBlip_init(&s_blip);

    local_engine_synth_reset_feeder();
}


float app_engine_synth_process_sample_48k(void)
{
    float y;

    if( s_eng.mixGain == 0.0f )
    {
        local_engine_synth_reset_feeder();
        return 0.0f;
    }

    if( s_engine_fx_pos >= ENGINE_SYNTH_FEEDER_SAMPLES )
    {
        local_engine_synth_render_block_48k();
    }

    /*
     * Existing EngineSynth_process() writes the same mono engine value
     * into slot0 and slot1. Return slot0 as the 48 kHz mono FX source.
     */
    y = s_engine_fx_out[s_engine_fx_pos];

    s_engine_fx_pos++;

    return y;
}



void app_engine_synth_enable( bool enable )
{
    if( enable )
    {
        EngineSynth_setGain(&s_eng, Gain_EngineSynth);
    }
    else
    {
        EngineSynth_setGain(&s_eng, 0.0f);
    }

    local_engine_synth_reset_feeder();
}
bool app_engine_synth_is_enable( void )
{
    if( s_eng.mixGain != 0.0f )
    {
        return true;
    }
    return false;
}




void app_engine_synth_blip_start( void )
{
    printf("\n blipping start!!!\n");
    Start_Blip = true;
}

#endif //defined(ENA_ENGINE_SYNTH)
