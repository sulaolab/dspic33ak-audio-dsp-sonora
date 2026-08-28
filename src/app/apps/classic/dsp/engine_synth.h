#if defined(ENA_ENGINE_SYNTH)

#ifndef _ENGINE_SYNTH_H
#define _ENGINE_SYNTH_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

//=================== Temporary diagnostic flags (enable only what you need) ===================
// LPF fixed test (enable only one of the following)
// #define DIAG_LPF_LOCK_MIN
// #define DIAG_LPF_LOCK_MAX

// Bypass EQ / Noise (useful when you want to hear LPF effect only)
// #define DIAG_BYPASS_EQ
// #define DIAG_BYPASS_NOISE

// Throttle coefficient updates (pseudo-evaluate how it sounds with longer frame length)
// Meaning of value: 1 = update every frame (normal) / 2 = once per 2 frames / 4 = once per 4 frames...
// #define DIAG_COEF_UPDATE_EVERY  1   // try 2,4,8 etc.
//===============================================================================

#if !defined(ENGINE_MAX_HARM)
#define ENGINE_MAX_HARM      16
#endif //!defined(ENGINE_MAX_HARM)

#define ENG_SYNTH_POT_SCALE_FACTOR     (1.5f)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    ENG_I4 = 0,
    ENG_I5,
    ENG_I6,
    ENG_V8,
    ENG_V12,
    ENG_CUSTOM

} engine_type_t;


/* Note: removed numHarm from tone; tone now holds only timbre (harmonic envelope, odd/even balance, tilt) */
typedef struct
{
    float baseGain;
    float tilt_dB_per_oct;
    float oddBoost_dB;
    float evenBoost_dB;
    float A_linear[ENGINE_MAX_HARM];  // Base amplitude for each harmonic (1..ENGINE_MAX_HARM)
    float outSoftLimit;

} engine_tone_t;


typedef struct
{
    float fs;
    float rpm_cur;
    float rpm_tgt;
    float rpm_slew_per_s;

    float phase0;
    int   numHarm;           // Effective number of harmonics (1..ENGINE_MAX_HARM)

    engine_type_t type;
    engine_tone_t tone;

} engine_synth_t;


/* Parameters used inside process (existing OK) */
typedef struct
{
    // EQ (Peaking)
    float eq_fc_hz;
    float eq_gain_db;
    float eq_q;

    // Variable low-pass
    float lpf_min_hz;
    float lpf_max_hz;
    float lpf_mult;
    float lpf_q;

    // AM (rumble feel)
    float am_rumble_depth; // f0/2
    float am_sub_depth;    // f0/4

    // Noise
    float noise_post_lpf_hz;
    float noise_mix;

    // EO (perceptual model)
    float eo;

} engine_params_t;


typedef struct
{
    engine_synth_t core;
    int   num_proc_ch;

    int   slot_L;
    int   slot_R;
    float mixGain;

    /* Existing parameters */
    engine_params_t params;

    /* Added: previously function-static "shared states" moved here */
    float rpm_ref_smooth;   // Smoothed RPM

    // Phases for LFO / sub-harmonics
    float lfo1_phase;
    float lfo2_phase;
    float rum_phase;   // f0/2
    float sub_phase;   // f0/4

    // For noise
    uint32_t rng;
    float    noise_lp; // Internal state of first-order LPF

    // Filter states (coefficients + delay elements)
    biquad_mono_t eq;
    biquad_mono_t lp;

    // Added: smoothing state for LPF cutoff (Hz)
    float    lp_fc_smooth;


    // --- Shared states for bank detune / cycle-to-cycle jitter ---
    float phase_bankB;   // Phase for bank B (mainly used for V8)
    float c2c_scale;     // Frequency scale per cycle (1.0 is nominal)
    float c2c_state;     // IIR state for jitter (0 baseline, c2c_scale = 1 + c2c_state)


} enginesynth_wrap_t;




/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////

// ==== Blipping (rapid throttle burst) ====

// Parameters (make run-time configurable later if needed)
typedef struct {
    float peak_rpm_default; // Max RPM for the blip (fixed peak)
    float attack_ms;        // Rise time
    float hold_ms;          // Hold time
    float decay_ms;         // Fall time
    float cooldown_ms;      // Cool-down to prevent repeated triggers
    float min_accept_rpm;   // Ignore trigger if POT is below this RPM (requirement)
//    float min_peak_rpm;     // Safety lower bound
//    float max_peak_rpm;     // Safety upper bound
    float attack_curve;     // 1.0 = linear, >1 accelerates late (simple easing)
    float decay_curve;      // 1.0 = linear, >1 accelerates late
} engine_blip_params_t;

typedef enum {
    BLIP_IDLE = 0,
    BLIP_ATTACK,
    BLIP_HOLD,
    BLIP_DECAY,
    BLIP_COOLDOWN
} engine_blip_phase_t;

typedef struct {
    engine_blip_phase_t  phase;

    engine_blip_params_t p;

    float t_in_phase;     // Elapsed time in current phase [s]
    float rpm_start;      // Start RPM at ATTACK
    float rpm_peak;       // Peak RPM for this blip (clamped to range)
} engine_blip_state_t;





//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

extern void  EngineSynthWrap_init(enginesynth_wrap_t* w,
                                  float fs,
                                  engine_type_t type,
                                  int   slot_L,
                                  int   slot_R,
                                  float mixGain,
                                  int   num_proc_ch );

extern void  EngineSynth_setRPM(engine_synth_t* s, float rpm_now);
extern void  EngineSynth_setTargetRPM(engine_synth_t* s, float rpm_target, float slew_rpm_per_s);

extern void  EngineSynth_process(enginesynth_wrap_t* w, float* p_in, float* p_out, int samples);



extern void  EngineBlip_init(engine_blip_state_t* s);
extern void  EngineBlip_trigger(engine_blip_state_t* s, float pot_rpm);
extern float EngineBlip_update(engine_blip_state_t* s, float pot_rpm, float dt_sec);





//===========================================================
// API
//===========================================================

extern void  app_engine_synth_init_48k(void);
extern float app_engine_synth_process_sample_48k(void);

extern void  app_engine_synth_enable( bool enable );
extern bool  app_engine_synth_is_enable( void );
extern void  app_engine_synth_blip_start( void );






#endif //!_ENGINE_SYNTH_H
#endif //defined(ENA_ENGINE_SYNTH)

