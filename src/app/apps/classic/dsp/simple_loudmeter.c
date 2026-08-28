#if 0
/*
 * simple_loudmeter.c
 *
 * Implementation of single-slot loudness meter with intuitive range mapping.
 * Mapping rule:
 *   level_dBFS <= low_dBFS   -> target = maxBoost_dB
 *   level_dBFS >= high_dBFS  -> target = 0 dB
 *   else linear interpolation between.
 *
 * Detector and smoothing:
 *   - Block RMS across monitored channels (average across channels).
 *   - Attack/Release smoothing on linear envelope.
 *   - dBFS conversion with floor.
 *   - Boost smoothing in dB (asymmetric rise/fall).
 */


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
#include "apps/shared/float_conversion.h"
#include "lpf_process.h"


#include "simple_loudmeter.h"





//===========================================================
// Definition
//===========================================================

// gain tracer EQ   m->maxBoost_dB
/////////////////////////////////////////////
//#define LOUD_GAIN_MAX             ( 30.0f)   // 
//#define LOUD_GAIN_MAX             ( 20.0f)   // for sony srs-x3
//#define LOUD_GAIN_MAX             ( 36.0f)   // for bose wave radio spk unit thru PWM
#define LOUD_GAIN_MAX             ( 30.0f)   // for bose wave radio spk unit thru PWM




//===========================================================
// Enum & Struct typedef
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

static inline float   coef_from_ms(float ms, float fs);
static inline float   lin_to_db_floor(float x, float floor_dBFS);


static inline void    simple_loudmeter_set_range(simple_loudmeter_t* m, float low_dBFS, float high_dBFS);
static inline void    simple_loudmeter_set_max_boost(simple_loudmeter_t* m, float maxBoost_dB);
static inline void    simple_loudmeter_set_attack_release(simple_loudmeter_t* m, float att_ms, float rel_ms);
static inline void    simple_loudmeter_set_boost_smoothing(simple_loudmeter_t* m, float rise_ms, float fall_ms);
static inline void    simple_loudmeter_set_floor(simple_loudmeter_t* m, float floor_dBFS);
static inline void    simple_loudmeter_set_bypass_thresh(simple_loudmeter_t* m, float thresh_dB);




//===========================================================
// Variables
//===========================================================




//===========================================================
// Global Function
//===========================================================

simple_loudmeter_t Slm;
float              Boost_dB;



/*
 * app_loudmeter_init
 * ------------------
 * Purpose : Initialize global simple loudness meter instance with defaults.
 * Notes   : Does not allocate memory. All parameters are set to safe defaults
 *           oriented to 48 kHz but generally valid for other rates.
 */
void app_loudmeter_init(void)
{
    Boost_dB = 0;

    /* Example: TDM8, process first 2 channels, 48 kHz */
    simple_loudmeter_init(&Slm, SAMPLE_RATE, STAGE_1_PROC_CH);

    /* Optional tuning */
    // simple_loudmeter_set_attack_release(&LM, 6.0f, 240.0f);
    // simple_loudmeter_set_boost_smoothing(&LM, 10.0f, 80.0f);
    // simple_loudmeter_set_floor(&LM, -96.0f);
}


/*
 * app_loudmeter_process
 * ---------------------
 * Function : Processes one block of audio samples, computes the perceived loudness
 *             as a gain boost value (in dB), and applies it to the LPF stage.
 *
 * Parameters:
 *     in  - Pointer to interleaved input samples
 *
 * Returns:
 *     None
 *
 * Description:
 *     - Calculates the current loudness level using simple_loudmeter_process().
 *     - Updates the global variable Boost_dB with the computed boost value.
 *     - Passes Boost_dB to app_lpf_set_gain(), which adjusts the LPF gain accordingly.
 *
 * Notes:
 *     - This operation is block-based but has effectively zero output latency
 *       since the gain is applied immediately to the next processing stage.
 *     - The enable/disable state is controlled externally (e.g., button in main.c).
 */
void app_loudmeter_process(const float* in)
{
    /* Compute boost dB for this block */
    float boost_dB = simple_loudmeter_process(&Slm, in);

    /* If you have a downstream LPF/EQ, you can apply boost_dB there.
       For testing, you can log boost_dB or send to telemetry. */
    Boost_dB = boost_dB;

    // enable/disable are decided by the button in main.c
    app_lpf_set_gain( Boost_dB );
}























/*
 * simple_loudmeter_init
 * ---------------------
 * Function : Initialize a simple_loudmeter_t structure with safe and
 *             practical default parameters for loudness-based gain control.
 *
 * Parameters:
 *     m         - Pointer to the loudness meter state structure
 *     fs        - Sampling rate in Hz
 *     num_proc_ch - Number of interleaved input channels (e.g., TDM slot count)
 *
 * Returns:
 *     None
 *
 * Description:
 *     - Clears the structure and initializes all internal fields.
 *     - Sets attack/release times and level mapping ranges suitable
 *       for 48 kHz audio applications.
 *     - Configures the dBFS thresholds for full vs. zero boost,
 *       and defines the maximum boost range and smoothing behavior.
 *
 * Default behavior:
 *     Attack time:    ~8 ms (quick response to rising levels)
 *     Release time:   ~10 ms (moderate decay for stable reading)
 *     Level mapping:  -60 dBFS -> full boost,  -16 dBFS -> no boost
 *     Max boost:      LOUD_GAIN_MAX (user-defined constant)
 *     Boost smoothing: fast fall (0.01 ms), gentle rise (100 ms)
 *
 * Notes:
 *     - Designed for integration into block-based processing chains.
 *     - Parameter values can be fine-tuned per application needs.
 *     - Call this once before using simple_loudmeter_process().
 */
void simple_loudmeter_init(simple_loudmeter_t* m, float fs, int num_proc_ch)
{
    memset(m, 0, sizeof(*m));

    /* Buffer layout */
    m->num_proc_ch = num_proc_ch;
    m->sampleRate  = fs;

    /* Detector state */
    m->env_lin      = 0.0f;
    m->currBoost_dB = 0.0f;


    /* Detector defaults (48 kHz oriented) */
//    m->att_ms     = 8.0f;     /* faster attack for snappy response */
//    m->rel_ms     = 220.0f;   /* slower release for stable reading */
//    m->att_ms     = 8.0f;     /* faster attack for snappy response */
//    m->rel_ms     = 80.0f;    /* slower release for stable reading */
    m->att_ms     = 8.0f;     /* faster attack for snappy response */
    m->rel_ms     = 10.0f;    /* slower release for stable reading */
    m->floor_dBFS = -96.0f;   /* clamp floor when converting to dBFS */

    /* Intuitive mapping range:
     * Example: below -40 dBFS -> full boost, above -15 dBFS -> zero boost.
     * Tune these to your system's headroom and feel.
     */
//    m->high_dBFS  = -20.0f;
    m->high_dBFS  = -16.0f;
//    m->low_dBFS   = -55.0f;
    m->low_dBFS   = -60.0f;

    /* Boost range */
//    m->maxBoost_dB = 12.0f;   /* cap the maximum boost (try 8..18 dB) */
//    m->maxBoost_dB = 28.0f;   /* cap the maximum boost */
//    m->maxBoost_dB = 30.0f;   /* cap the maximum boost */
//    m->maxBoost_dB = 32.0f;   /* cap the maximum boost */
//    m->maxBoost_dB = 36.0f;   /* cap the maximum boost */
//    m->maxBoost_dB = 42.0f;   /* cap the maximum boost */
    m->maxBoost_dB = LOUD_GAIN_MAX;   /* cap the maximum boost */


    /* Boost smoothing (asymmetric) */
//    m->gainSmoothFall_ms = 1.0f;    /* boost fall quickly */
    m->gainSmoothFall_ms = 0.01f;    /* boost fall quickly */

//    m->gainSmoothRise_ms = 60.0f;  /* boost rise gently */
    m->gainSmoothRise_ms = 100.0f;  /* boost rise gently */

    /* Optional downstream bypass threshold */
    m->bypassThresh_dB   = 0.25f;
}


/*
 * simple_loudmeter_process
 * ------------------------
 * Function : Analyze one block of interleaved audio samples and compute
 *             a smoothed loudness-based gain boost value (in dB).
 *
 * Parameters:
 *     m   - Pointer to the loudness meter state structure
 *     in  - Pointer to interleaved float samples
 *            (length = APP_BLOCK_FRAMES * m->num_proc_ch)
 *
 * Returns:
 *     Smoothed boost gain in dB (>= 0)
 *
 * Description:
 *     1) Computes block RMS level averaged across all monitored channels.
 *     2) Applies attack/release smoothing on the linear RMS envelope.
 *     3) Converts the smoothed RMS to dBFS with a defined floor.
 *     4) Maps the loudness level to a target boost in dB:
 *          - Quiet -> full boost
 *          - Loud  -> no boost
 *          - Linear interpolation in between
 *     5) Applies asymmetric smoothing on the boost value
 *        (gentle rise, quick fall) for perceptual stability.
 *
 * Notes:
 *     - The computed value represents the instantaneous "desired boost"
 *       for this block; typically used to drive a downstream LPF or EQ gain.
 *     - The result is zero-latency: no lookahead or output delay is introduced.
 *     - The function stores intermediate values (e.g., env_lin, level_dBFS)
 *       in the structure for optional debugging or telemetry use.
 */
float simple_loudmeter_process(simple_loudmeter_t* m, const float* in)
{
    const int inbch      = m->num_proc_ch;
          int sample_num = APP_BLOCK_FRAMES;


    /* 1) Block RMS across monitored channels (average across channels) */
    double acc = 0.0;
    for (int n = 0; n < sample_num; n++)
    {
        double frame_acc = 0.0;
        for (int ch = 0; ch < inbch; ch++)
        {
            float x = in[n * inbch + ch];
            frame_acc += (double)(x * x);
        }
        acc += frame_acc / (double)inbch;
    }
    float rms_lin = (sample_num > 0) ? (float)sqrt(acc / (double)sample_num) : 0.0f;

    /* 2) Attack/Release smoothing on linear RMS */
    float aA  = coef_from_ms(m->att_ms, m->sampleRate);
    float aR  = coef_from_ms(m->rel_ms, m->sampleRate);
    float env = m->env_lin;
    if (rms_lin > env)
    {
        env = aA * env + (1.0f - aA) * rms_lin;  /* attack */
    }
    else
    {
        env = aR * env + (1.0f - aR) * rms_lin;  /* release */
    }
    m->env_lin = env;

    /* 3) Convert to dBFS with floor */
    float level_dBFS = lin_to_db_floor(env, m->floor_dBFS);

    /* 4) Intuitive mapping: clamp by range and linearly map to 0..maxBoost */
    float targetBoost;
    if (level_dBFS <= m->low_dBFS)
    {
        /* Very quiet -> full boost */
        targetBoost = m->maxBoost_dB;
    }
    else if (level_dBFS >= m->high_dBFS)
    {
        /* Loud -> zero boost (bypass region) */
        targetBoost = 0.0f;
    }
    else
    {
        /* Linear interpolation between low..high */
        float t = (m->high_dBFS - level_dBFS) / (m->high_dBFS - m->low_dBFS);
        /* t in (0..1): t=1 at low, t=0 at high */
        t = clampf(t, 0.0f, 1.0f);
        targetBoost = t * m->maxBoost_dB;
    }

    /* 5) Smooth the boost in dB (asymmetric rise/fall) */
    float aRise = coef_from_ms(m->gainSmoothRise_ms, m->sampleRate);
    float aFall = coef_from_ms(m->gainSmoothFall_ms, m->sampleRate);
    float aG    = (targetBoost > m->currBoost_dB) ? aRise : aFall;

    m->currBoost_dB = aG * m->currBoost_dB + (1.0f - aG) * targetBoost;

    // save for debug
    m->DBG_level_dBFS = level_dBFS;

    return m->currBoost_dB;  /* dB */
}















//===========================================================
// Local Function
//===========================================================

/* One-pole coefficient from time constant (ms) and sampling rate (Hz) */
static inline float coef_from_ms(float ms, float fs)
{
    if (ms <= 0.0f) return 0.0f; /* no smoothing */
    /* a = exp( -1 / (tau * fs) ), tau = ms / 1000 */
    float a = expf( -(1.0f / ((ms * 0.001f) * fs)) );
    return clampf(a, 0.0f, 0.999999f);
}

/* Convert linear to dBFS with a floor */
static inline float lin_to_db_floor(float x, float floor_dBFS)
{
    float floor_lin = db_to_lin(floor_dBFS);
    if (x < floor_lin) x = floor_lin;
    return 20.0f * log10f(x);
}








/* Convenience setters (all ASCII-safe) */
static inline void simple_loudmeter_set_range(simple_loudmeter_t* m, float low_dBFS, float high_dBFS)
{
    m->low_dBFS  = low_dBFS;
    m->high_dBFS = high_dBFS;
}
static inline void simple_loudmeter_set_max_boost(simple_loudmeter_t* m, float maxBoost_dB)
{
    m->maxBoost_dB = maxBoost_dB;
}
static inline void simple_loudmeter_set_attack_release(simple_loudmeter_t* m, float att_ms, float rel_ms)
{
    m->att_ms = att_ms; m->rel_ms = rel_ms;
}
static inline void simple_loudmeter_set_boost_smoothing(simple_loudmeter_t* m, float rise_ms, float fall_ms)
{
    m->gainSmoothRise_ms = rise_ms;
    m->gainSmoothFall_ms = fall_ms;
}
static inline void simple_loudmeter_set_floor(simple_loudmeter_t* m, float floor_dBFS)
{
    m->floor_dBFS = floor_dBFS;
}
static inline void simple_loudmeter_set_bypass_thresh(simple_loudmeter_t* m, float thresh_dB)
{
    m->bypassThresh_dB = thresh_dB;
}
#endif //01

