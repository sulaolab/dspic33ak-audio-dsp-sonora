#if defined(ENA_BASS_ENHANCER)

#ifndef _BASS_ENHANCER_H
#define _BASS_ENHANCER_H


//===========================================================
// Definition
//===========================================================

#define BASSE_SLOTS_PER_FS    (STAGE_1_PROC_CH)

#if APP_TARGET == APP_TARGET_AK512
#define BLOOM_BUF_MAX         (2048)  // up to about 21 ms @96 kHz
#else
#define BLOOM_BUF_MAX         (1024)
#endif //APP_TARGET == APP_TARGET_AK512


//===========================================================
// Enum & Struct typedef
//===========================================================

// =================== Public Parameters ===================
//
// Parameter grouping policy:
//   tone  : sound character / demo tuning
//   loud  : equal-loudness dynamic bass boost
//   guard : protection, stability, and anti-runaway controls

typedef struct {
    // Low-band extraction for driving the sub generator
    float low_xover_hz;      // <= this band used as source (e.g., 120 Hz)

    // Sub generation & mix
    float lpf_base_db;       // basic low-band boost amount
    float wet_mix;           // 0..1 (amount of generated sub added)
    float dry_mix;           // 0..1 (original pass-through)

    // Ultra-light LF "bloom" tail
    float bloom_time_ms;       // normal / medium-volume tail time
    float bloom_time_quiet_ms; // quiet-playback tail time
    float bloom_quiet_curve;   // 1.0=linear, >1.0=stronger only when quiet
    float bloom_delay_ms;      // short delay
    float bloom_mix;           // 0..1

    // Room-mode-like resonance after bloom
    float room_f0_hz;        // resonance center frequency
    float room_q;            // resonance sharpness / peak gain

} bassenh_tone_params_t;


typedef struct {
    bool  enabled;
    float Bmax_dB;
    float alpha;             // reserved / optional curve parameter
    float beta;
    float L_lo_dbfs;
    float L_hi_dbfs;
    float atk_ms;
    float rel_ms;

} bassenh_loud_params_t;


typedef struct {
    // Output protection
    float limiter_thresh;    // 0.5..0.99 (soft-clip knee, e.g., 0.9)
    float dc_hpf_hz;         // 10..30 Hz (DC blocker)
    float total_limit_db;    // maximum low-band gain after loudness bonus

    // Low-band stability / anti-boom controls
    float env_floor_lpf;     // minimum effective envelope for audibility
    float duck_lpf_coef;     // reduces generated sub when input LF is large
    float bloom_duck;        // reduces bloom when input LF is large
    float bloom_time_smooth_ms; // smoothing time for quiet-linked bloom feedback

    // Excursion guard
    float exc_target;
    float exc_attack_ms;
    float exc_release_ms;

    // Loudness bonus stabilizer (dB-domain slew limiter)
    float loud_slew_up_dbps;
    float loud_slew_dn_dbps;
    float loud_deadband_db;

} bassenh_guard_params_t;


typedef struct {
    bool enabled;             // master ON/OFF

    bassenh_tone_params_t  tone;
    bassenh_loud_params_t  loud;
    bassenh_guard_params_t guard;

} bassenh_params_t;




// =================== Internal State ===================
typedef struct {

    float fs;
    int   num_proc_ch;       // channel(slot) per Fs of buffer

    float exc_env;

    // Filters
    biquad_mono_t lpf_low1[BASSE_SLOTS_PER_FS];  // low-band extractor (L/R) 1st
    biquad_mono_t lpf_low2[BASSE_SLOTS_PER_FS];  // low-band extractor (L/R) 2nd
    biquad_mono_t hpf_dc[BASSE_SLOTS_PER_FS];    // DC blocker (L/R)

    // Envelope follower (mono)
    float env;
    float env_aA;
    float env_aR;

    // loudness-aware internal states
    float loud_env_wide;
    float loud_env_low;
    float loud_aA;
    float loud_aR;

    float loud_gain_lin;
    float loud_bonus_dB_slow;

    // Block-rate weights for the bypass level meter (same 10/200 ms attack and
    // release as loud_aA/aR, but applied once per audio block instead of once
    // per sample -- see bassenh_bypass_meter).
    float meter_aA;
    float meter_aR;

    // Set when the effect is enabled: the first enabled block jumps the
    // equal-loudness gain straight to its target instead of slewing up to it
    // at loud_slew_up_dbps, which would take about 1.5 s and audibly weaken
    // the first seconds after switch-on.
    bool  loud_prime;

    //////////////////////////////
    // --- debug snapshot  ---
    //////////////////////////////
    float dbg_L_wide_db;
    float dbg_quiet;
    float dbg_duck_lpf;
    float dbg_lpf_base_db;
    float dbg_lpf_gain_db;
    float dbg_lpf_bonus_db;
    float dbg_exc_env;
    float dbg_exc_target;
    float dbg_g_exc;
    float dbg_low_fc;
    float dbg_bloom_time_ms;
    float dbg_bloom_g;
    //////////////////////////////

    // Ultra-light mono bloom buffer (single delay with feedback)
    float bloom_buf[BLOOM_BUF_MAX];
    int   bloom_w;
    int   bloom_D;
    float bloom_g;

    // Cached parameters
    bassenh_params_t p;

} bassenh_t;






//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

extern void  bassenh_init(bassenh_t* sh, int num_proc_ch, uint32_t sample_rate_Hz);
extern void  bassenh_process(bassenh_t* sh, const float* in, float* out, int frames);






//===========================================================
// API
//===========================================================

extern void  app_bassenh_init(uint32_t sample_rate_Hz);
extern void  app_bassenh_enable(bool en);
extern bool  app_bassenh_is_enabled(void);

extern void  app_bassenh_preset_speaker_A(bassenh_t* sh);

extern void  app_bassenh_process(const float* in, float* out);
extern void  app_bassenh_dbg_prt( void );
extern void  app_bassenh_dbg_minus_key_hdr( void );
extern void  app_bassenh_dbg_plus_key_hdr( void );
extern void  app_bassenh_dbg_set_lpf_cap_db( float lpf_cap_db );
extern void  app_bassenh_dbg_prt_lpf_cap_db( void );




#endif //_BASS_ENHANCER_H
#endif //defined(ENA_BASS_ENHANCER)
