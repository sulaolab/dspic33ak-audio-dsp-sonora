#if 0
/**
 * @file reverb_ctrl.c
 * @brief Simple comb-filter-based reverb processor with feedback and wet/dry mixing.
 * 
 * This module defines a configurable reverb effect using a single delay line per channel.
 * Parameters such as feedback gain, wet mix, and delay time are adjustable.
 * Buffer and index memory must be supplied externally to allow flexible integration.
 *
 * Suitable for multi-channel TDM interleaved audio frames.
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

#include "timer_app.h"
#include "apps/shared/float_conversion.h"
#include "gain_ctrl.h"


#include "reverb_smooth.h"





//===========================================================
// Definition
//===========================================================

#if defined(ENA_REVERB2) && APP_TARGET == APP_TARGET_AK512
  #define MAX_DELAY_MS            (120)
#else
  #define MAX_DELAY_MS            (2)
#endif //defined(ENA_REVERB2) && APP_TARGET == APP_TARGET_AK512

#define GET_DELAY_CNT(ms)         (48000 * (ms) / 1000)
#define MAX_DELAY_SAMPLES         GET_DELAY_CNT(MAX_DELAY_MS)




//===========================================================
// Enum & Struct typedef
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

static inline float  anti_denormal(float x);
static        void   layout_comb_segments(reverb_smooth_t* rv);
static        void   compute_feedback_gains(reverb_smooth_t* rv);
static        float  lpf1_alpha(float fs, float fc);







//===========================================================
// Variables
//===========================================================

static reverb_smooth_t RV2;
static float           Reverb2Buf[STAGE_1_PROC_CH * MAX_DELAY_SAMPLES];
static int             Reverb2Idx[STAGE_1_PROC_CH * RV_NCOMB];
static float           Reverb2LPF[STAGE_1_PROC_CH * RV_NCOMB];


//===========================================================
// Global Function
//===========================================================

void app_reverb2_init(void)
{
    reverb_smooth_init(&RV2,
                       Reverb2Buf,
                       Reverb2Idx,
                       Reverb2LPF,
                       SAMPLE_RATE,        // 48000
                       APP_SLOTS_PER_FS,
                       STAGE_1_PROC_CH,
                       MAX_DELAY_SAMPLES);

// //    reverb_smooth_set_decay_T60_ms(&RV2, 1200.0f);  // 1.2 s tail
//     reverb_smooth_set_decay_T60_ms(&RV2, 0.0f);     // startup with OFF effect
//     reverb_smooth_set_feedback_lpf(&RV2, 6000.0f);  // 6 kHz damping
//     reverb_smooth_set_wet(&RV2, 0.35f);             // 35% wet (taste)

    reverb_smooth_set_feedback_lpf(&RV2, 2500.0f);  // 2.5 kHz damping
    reverb_smooth_set_wet(&RV2, 0.70f);             // 70% wet (taste)
}

void app_reverb2_process(const float* in, float* out)
{
    reverb_smooth_process(&RV2, in, out, APP_BLOCK_FRAMES); // TDM8 32-sample block
}

void app_reverb2_enabled( int dly_ms )
{
    printf(" %3d(ms) @%ld\n", dly_ms, GetTicks());

    reverb_smooth_set_decay_T60_ms(&RV2, (float)dly_ms);
}






















void reverb_smooth_init( reverb_smooth_t* rv,
                         float* delayBuf,
                         int*   wrIdxComb,
                         float* fbLpfState,
                         int    fs,
                         int    chPerFs,
                         int    chProcs,
                         int    maxDelaySamps )
{
    memset(rv, 0, sizeof(*rv));
    rv->delayBuf      = delayBuf;
    rv->wrIdxComb     = wrIdxComb;
    rv->fbLpfState    = fbLpfState;
    rv->fs            = fs;
    rv->chPerFs       = chPerFs;
    rv->chProcs       = chProcs;
    rv->maxDelaySamps = maxDelaySamps;

    // Default perceptual params
    rv->T60_ms     = 600.0f;       // -60 dB in ~0.6 s (perceptually long even with short lines)
    rv->fb_lpf_fc  = 6000.0f;      // gentle high-frequency damping
    rv->fb_lpf_alpha = lpf1_alpha((float)fs, rv->fb_lpf_fc);

    // Clear memories
    memset(delayBuf,   0, sizeof(float) * chProcs * maxDelaySamps);
    memset(wrIdxComb,  0, sizeof(int)   * chProcs * RV_NCOMB);
    memset(fbLpfState, 0, sizeof(float) * chProcs * RV_NCOMB);

    // Partition per-channel buffer into RV_NCOMB circular segments
    layout_comb_segments(rv);

    // Compute per-comb feedback gains to match target T60
    compute_feedback_gains(rv);

    // Default wet/dry
    rv->wetMix = 0.5f;
    rv->dryMix = 0.5f;
}


// -----------------------------------------------------------------------------
// Recommended T60 (reverb tail length to -60 dB) by instrument / application
// Values are in milliseconds (ms). Adjust to taste.
//
// Vocals      : 800-1500 ms   // medium tails add warmth and space
// Acoustic Gt : 600-1200 ms   // shorter tails keep clarity, longer adds ambience
// Snare Drum  : 1200-1800 ms  // longer tails emphasize spaciousness
// Kick Drum   : 400-800 ms    // short tails to avoid muddy low end
// Piano       : 1000-2000 ms  // longer tails give concert-hall feeling
// Strings     : 1500-2500 ms  // lush sustain for orchestral feel
// Synth Pads  : 2000-4000 ms  // very long tails for ambient / atmospheric sound
// Small Room  : 400-800 ms    // tight, natural room reflections
// Hall        : 1500-2500 ms  // large hall / concert ambience
// Plate-style : 1000-2000 ms  // bright, dense reverb typical for vocals/snare
//
// Example:
// reverb_smooth_set_decay_T60_ms(&RV2, 1200.0f);  // set ~1.2 s tail
// -----------------------------------------------------------------------------
void reverb_smooth_set_decay_T60_ms(reverb_smooth_t* rv, float T60_ms)
{
//    if (T60_ms < 100.0f)  T60_ms = 100.0f;   // keep reasonable
    if (T60_ms <= 0.0f)
    {
        // OFF state: set wet=0, skip feedback
        rv->T60_ms = 0.0f;
        for (int i=0;i<RV_NCOMB;i++)
        {
            rv->fb_g[i] = 0.0f;
        }
        return;
    }
    if (T60_ms > 4000.0f)
    {
        T60_ms = 4000.0f;
    }
    rv->T60_ms = T60_ms;
    compute_feedback_gains(rv);
}


void reverb_smooth_set_feedback_lpf(reverb_smooth_t* rv, float fc_hz)
{
    if (fc_hz < 500.0f)   fc_hz = 500.0f;
    if (fc_hz > 12000.0f) fc_hz = 12000.0f;
    rv->fb_lpf_fc    = fc_hz;
    rv->fb_lpf_alpha = lpf1_alpha((float)rv->fs, fc_hz);
}


void reverb_smooth_set_wet(reverb_smooth_t* rv, float wetMix)
{
    if (wetMix < 0.0f) wetMix = 0.0f;
    if (wetMix > 1.0f) wetMix = 1.0f;
    rv->wetMix = wetMix;
    rv->dryMix = 1.0f - wetMix;
}


void reverb_smooth_process(const reverb_smooth_t* rv,
                           const float* in, float* out, int frameSize)
{
    const int chPerFs  = rv->chPerFs;
    const int chProcs  = rv->chProcs;
    const int maxSamps = rv->maxDelaySamps;
    const float wet    = rv->wetMix;
    const float dry    = rv->dryMix;
    const float a      = rv->fb_lpf_alpha;

    for (int ch = 0; ch < chProcs; ++ch)
    {
        float  const* chBufBase = &rv->delayBuf[ch * maxSamps];
        int*          wrBase    = &rv->wrIdxComb[ch * RV_NCOMB];
        float*        lpfState  = &rv->fbLpfState[ch * RV_NCOMB];

        for (int n = 0; n < frameSize; ++n)
        {
            const int   idx = n * chPerFs + ch;
            const float x   = in[idx];

            // Sum of parallel combs (dense reflections)
            float combSum = 0.0f;

            for (int k = 0; k < RV_NCOMB; ++k)
            {
                const int base = rv->combBase[k];
                const int L    = rv->combLen[k];

                // Local pointers into the shared per-channel buffer
                float* buf  = (float*)(chBufBase + base);
                int*   widx = &wrBase[k];

                // Read oldest sample (delay output)
                const int rd = (*widx - 1 + L) % L;  // one-sample lookback is fine
                float d = buf[rd];

                // Feedback path LPF (one-pole): s += a*(d - s); use s for feedback
                float s = lpfState[k] + a * (d - lpfState[k]);
                lpfState[k] = s;

                // Write new sample into this comb's circular segment
                float y = x + rv->fb_g[k] * s;
                buf[*widx] = anti_denormal(y);

                // Advance write index
                *widx = (*widx + 1) % L;

                // Accumulate delayed output (pre-LPF signal gives a bit more brightness)
                combSum += d;
            }

            // Mix dry/wet
            out[idx] = dry * x + wet * (combSum / (float)RV_NCOMB);
        }
    }
}


































//===========================================================
// Local Function
//===========================================================

// Utility: avoid denormals
static inline float anti_denormal(float x)
{
    return (x > -1.0e-15f && x < 1.0e-15f) ? 0.0f : x;
}

// Distribute total buffer into RV_NCOMB circular segments with mutually
// different prime-ish lengths to reduce metallic ringing.
// The sum of combLen[] <= maxDelaySamps.
static void layout_comb_segments(reverb_smooth_t* rv)
{
    // Ratios sum <= 1.0 (use ~0.80 to leave margin)
    // Tuned for smoother density under 130 ms total.
    const float ratios[RV_NCOMB] = {
        0.22f, 0.26f, 0.16f, 0.20f   // sum = 0.84
    };

    int base = 0;
    for (int i = 0; i < RV_NCOMB; ++i)
    {
        int L = (int)(ratios[i] * (float)rv->maxDelaySamps);
        if (L < 16) L = 16; // keep minimum length

        // Nudge to a prime-ish number to avoid common periods
        // (simple odd rounding)
        if ((L % 2) == 0) L += 1;

        // Clamp against remaining space
        if (base + L > rv->maxDelaySamps)
            L = rv->maxDelaySamps - base;

        rv->combBase[i] = base;
        rv->combLen[i]  = (L > 0) ? L : 16;
        base += rv->combLen[i];
    }
}

// Compute per-comb feedback gains from target T60 (to -60 dB).
// For a comb of length Li, the per-sample feedback g satisfies:
//     10^(-60/20) = g^(Li)  over Li samples,
// ->  g_i = 10^(-3 * Li/fs / (T60_sec))
static void compute_feedback_gains(reverb_smooth_t* rv)
{
    const float fs    = (float)rv->fs;
    const float T60_s = rv->T60_ms * 0.001f;
    const float k     = -3.0f / T60_s; // -60 dB -> 10^(-3)

    for (int i = 0; i < RV_NCOMB; ++i)
    {
        const float Li_sec = (float)rv->combLen[i] / fs;
        rv->fb_g[i] = powf(10.0f, k * Li_sec);
        // Safety clamp
        if (rv->fb_g[i] > 0.99f) rv->fb_g[i] = 0.99f;
        if (rv->fb_g[i] < 0.10f) rv->fb_g[i] = 0.10f;
    }
}

// 1-pole LPF alpha from cutoff (per-sample coefficient)
// y += alpha * (x - y);  alpha = 1 - exp(-2*pi*fc/fs)
static float lpf1_alpha(float fs, float fc)
{
    if (fc <= 0.0f) return 1.0f; // bypass -> immediate follow
    float a = 1.0f - expf(-2.0f * (float)M_PI * fc / fs);
    if (a < 0.0001f) a = 0.0001f;
    if (a > 1.0f)    a = 1.0f;
    return a;
}
#endif //01

