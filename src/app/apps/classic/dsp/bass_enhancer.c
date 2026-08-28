
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>   // for fmaxf
//#include "SPI_TDM_drv.h"
#include "simple_loudmeter.h"


#include "bass_enhancer.h"




#if defined(ENA_BASS_ENHANCER)
//===========================================================
// Definition
//===========================================================

// Room resonance and loudness stabilizer values are now preset parameters:
//   sh->p.tone.room_f0_hz / room_q
//   sh->p.guard.loud_slew_* / loud_deadband_db

#define BASSH_XOVER_X2        (1)        // 0: 2nd order / 1: 4th order
#define EPS                   (1.0e-7f)

// Low-band scratch. One audio block, written by the low-band pass and consumed
// by the main pass. Static rather than automatic to keep the audio ISR's stack
// where it was.
#define BASSENH_LOW_SCRATCH   (APP_BLOCK_FRAMES)

// Bypass telemetry costs one level pass and nothing else.
//
// TDM1:max is a PEAK over the print window, not an average, so decimating this
// work (running it one block in N) cannot reduce it -- the one block that does
// the work still sets the peak. Measured: 1-in-8 read identically to every
// block. And since the block deadline is a worst-case constraint, the peak is
// the right metric, so decimation would be complexity for nothing.
//
// What does help is not doing the expensive part in the audio ISR at all. The
// only thing the ISR has to maintain is the wideband envelope; the dB
// conversion, the quiet curve and the bloom-time derivation are display
// arithmetic, needed at the 2.5 Hz the telemetry line prints, so they live in
// app_bassenh_dbg_prt() instead -- six transcendentals moved out of the ISR.


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// Variables
//===========================================================

static biquad_mono_t  g_room_bq;     // mono room-mode resonance
static bassenh_t      g_bassenh;

static float          g_low_scratch[BASSENH_LOW_SCRATCH];


// Block-constant control values, computed once per audio block by
// bassenh_block_prepare() and consumed by whichever sample loop runs. Both the
// legacy and the optimised loop see identical values, which is what makes the
// 'E' A/B a comparison of loop structure alone.
typedef struct {
    float wet;
    float dry;
    float thr;
    float env_floor_lpf;
    float duck_lpf_coef;
    float exc_target;
    float exc_aA;
    float exc_aR;
    float gain_lpf;
    float block_quiet;
} bassenh_blk_t;


//===========================================================
// Local Function
//===========================================================

static inline uint32_t local_get_valid_sample_rate(uint32_t sample_rate_Hz)
{
    if( sample_rate_Hz != 0u )
    {
        return sample_rate_Hz;
    }

    return (uint32_t)SAMPLE_RATE;
}


// ---------- biquad (RBJ cookbook) ----------
static void biquad_reset(biquad_mono_t* b)
{
    b->bqs.z1 = 0.0f;
    b->bqs.z2 = 0.0f;
}


static inline float biquad_process(biquad_mono_t* b, float x)
{
    float y   = b->bq.b0*x + b->bqs.z1;
    b->bqs.z1 = b->bq.b1*x - b->bq.a1*y + b->bqs.z2;
    b->bqs.z2 = b->bq.b2*x - b->bq.a2*y;
    return y;
}


static void biquad_make_lpf(biquad_mono_t* b, float fs, float fc, float Q)
{
    float w0 = 2.0f*(float)M_PI*fc/fs;
    float c  = cosf(w0);
    float s  = sinf(w0);
    float alpha = s/(2.0f*Q);
    float b0 = (1.0f - c)*0.5f;
    float b1 =  1.0f - c;
    float b2 = (1.0f - c)*0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f*c;
    float a2 =  1.0f - alpha;

    b->bq.b0 = b0/a0;
    b->bq.b1 = b1/a0;
    b->bq.b2 = b2/a0;
    b->bq.a1 = a1/a0;
    b->bq.a2 = a2/a0;
    biquad_reset(b);
}


static void biquad_make_hpf(biquad_mono_t* b, float fs, float fc, float Q)
{
    float w0 = 2.0f*(float)M_PI*fc/fs;
    float c  = cosf(w0);
    float s  = sinf(w0);
    float alpha = s/(2.0f*Q);
    float b0 = (1.0f + c)*0.5f;
    float b1 = -(1.0f + c);
    float b2 = (1.0f + c)*0.5f;
    float a0 =  1.0f + alpha;
    float a1 = -2.0f*c;
    float a2 =  1.0f - alpha;

    b->bq.b0 = b0/a0;
    b->bq.b1 = b1/a0;
    b->bq.b2 = b2/a0;
    b->bq.a1 = a1/a0;
    b->bq.a2 = a2/a0;
    biquad_reset(b);
}


static inline float local_calc_bloom_feedback(float fs, int bloom_D, float bloom_time_ms)
{
    float time_ms  = clampf(bloom_time_ms, 40.0f, 900.0f);
    float Tsec     = time_ms * 0.001f;
    float per_loop = (float)bloom_D / fs;
    float r        = expf(-per_loop / Tsec);

    return fminf(0.985f, r);
}


// ---------- (re)build filters & cached values ----------
static void bassenh_rebuild(bassenh_t* sh)
{
    const float fs = sh->fs;

    // loudness-aware coefficients & init
    sh->loud_aA = expf(-1.0f / (0.010f * fs));  // 10 ms attack
    sh->loud_aR = expf(-1.0f / (0.200f * fs));  // 200 ms release

    // Same time constants for the bypass level meter, at its own update rate.
    {
        const float mtr_sec = (float)APP_BLOCK_FRAMES / fs;
        sh->meter_aA = expf(-mtr_sec / 0.010f);
        sh->meter_aR = expf(-mtr_sec / 0.200f);
    }
    sh->loud_prime = false;

    // smoothed gain (linear). Start neutral.
    sh->loud_gain_lin = 1.0f;
    sh->loud_env_wide = 0.0f;
    sh->loud_env_low  = 0.0f;

    float dc_hz  = clampf(sh->p.guard.dc_hpf_hz, 5.0f, 40.0f);

    const float kLowFcMin = 40.0f;
    const float kLowFcMax = 300.0f;
    const float nyqSafe   = fs * 0.45f;
    sh->p.tone.low_xover_hz = clampf(sh->p.tone.low_xover_hz, kLowFcMin, fminf(kLowFcMax, nyqSafe));
    float low_fc = sh->p.tone.low_xover_hz;

    for(int ch=0; ch<sh->num_proc_ch; ++ch)
    {
        biquad_make_hpf(&sh->hpf_dc[ch],   fs, dc_hz,  0.707f);
        biquad_make_lpf(&sh->lpf_low1[ch], fs, low_fc, 0.5412f);
        biquad_make_lpf(&sh->lpf_low2[ch], fs, low_fc, 1.3065f);
    }

    // Envelope
    const float atk_ms = 2.0f;
    const float rel_ms = 80.0f;
    sh->env_aA = expf(-1.0f / (atk_ms * 0.001f * fs));
    sh->env_aR = expf(-1.0f / (rel_ms * 0.001f * fs));
    sh->env    = 0.0f;

    // Bloom buffer
    {
        float Dms = clampf(sh->p.tone.bloom_delay_ms, 8.0f, 28.0f);
        sh->bloom_D = (int)(Dms * 0.001f * fs);
        if (sh->bloom_D < 1) sh->bloom_D = 1;
        if (sh->bloom_D >= BLOOM_BUF_MAX) sh->bloom_D = BLOOM_BUF_MAX-1;

        sh->bloom_g = local_calc_bloom_feedback(fs, sh->bloom_D, sh->p.tone.bloom_time_ms);
        sh->bloom_w = 0;
    }

    // Bloom line contents and the excursion follower are part of the state a
    // rebuild resets: since the bypass exit stops the line from decaying, a
    // stale tail must not be able to survive into the next enable.
    memset(sh->bloom_buf, 0, sizeof(sh->bloom_buf));
    sh->exc_env = 0.0f;
    sh->loud_bonus_dB_slow = 0.0f;

    // Room-mode-like BPF
    {
        float f0 = clampf(sh->p.tone.room_f0_hz, 25.0f, 120.0f);
        float Q  = clampf(sh->p.tone.room_q,      0.7f,   5.0f);

        // RBJ bandpass (constant skirt gain, peak gain = Q)
        float w0 = 2.0f*(float)M_PI * f0 / fs;
        float c  = cosf(w0);
        float s  = sinf(w0);
        float alpha = s/(2.0f*Q);

        float b0 =   alpha;
        float b1 =   0.0f;
        float b2 =  -alpha;
        float a0 =   1.0f + alpha;
        float a1 =  -2.0f*c;
        float a2 =   1.0f - alpha;

        g_room_bq.bq.b0  = b0/a0;
        g_room_bq.bq.b1  = b1/a0;
        g_room_bq.bq.b2  = b2/a0;
        g_room_bq.bq.a1  = a1/a0;
        g_room_bq.bq.a2  = a2/a0;
        g_room_bq.bqs.z1 = 0.0f;
        g_room_bq.bqs.z2 = 0.0f;
    }
}


// ---------------------------------------------------------------------------
// Per-block control path.
//
// Runs once per audio block, ahead of whichever sample loop is selected. It
// snapshots the parameters, advances the block-rate control state (bloom
// feedback, the equal-loudness gain) and publishes the debug snapshot.
// ---------------------------------------------------------------------------
static void bassenh_block_prepare(bassenh_t* sh, int frames, bassenh_blk_t* c)
{
    // Snapshot values. Keep them constant during this audio block.
    const float cf_wet = clampf(sh->p.tone.wet_mix,         0.0f, 1.0f);
    const float cf_dry = clampf(sh->p.tone.dry_mix,         0.0f, 1.0f);
    const float cf_thr = clampf(sh->p.guard.limiter_thresh, 0.5f, 0.99f);

    const float cf_fs_boost = sh->fs;

    const float cf_env_floor_lpf  = sh->p.guard.env_floor_lpf;
    const float cf_duck_lpf_coef  = sh->p.guard.duck_lpf_coef;
    const float cf_lpf_base_db    = sh->p.tone.lpf_base_db;
    const float cf_exc_target     = sh->p.guard.exc_target;

    const float cf_exc_attack_ms  = sh->p.guard.exc_attack_ms;
    const float cf_exc_release_ms = sh->p.guard.exc_release_ms;
    const float cf_aA             = expf(-1.0f / (cf_fs_boost * (cf_exc_attack_ms  * 1e-3f)));
    const float cf_aR             = expf(-1.0f / (cf_fs_boost * (cf_exc_release_ms * 1e-3f)));

    // Block analysis phase
    const float L_wide_db_start = 20.0f * log10f(fmaxf(sh->loud_env_wide, EPS));

    float t = (L_wide_db_start - sh->p.loud.L_hi_dbfs) / (sh->p.loud.L_lo_dbfs - sh->p.loud.L_hi_dbfs);
    t = clampf(t, 0.0f, 1.0f);
    const float block_quiet = t * t * (3.0f - 2.0f * t); // smoothstep

    // Quiet-linked bloom duration.
    // At medium/high level, use tone.bloom_time_ms.
    // At quiet playback, smoothly approach tone.bloom_time_quiet_ms.
    const float bloom_time_normal_ms = clampf(sh->p.tone.bloom_time_ms,       40.0f, 900.0f);
    float       bloom_time_quiet_ms  = clampf(sh->p.tone.bloom_time_quiet_ms, 40.0f, 900.0f);
    const float bloom_quiet_curve    = clampf(sh->p.tone.bloom_quiet_curve,    0.25f,   4.0f);

    if (bloom_time_quiet_ms < bloom_time_normal_ms)
    {
        bloom_time_quiet_ms = bloom_time_normal_ms;
    }

    const float q_bloom = powf(block_quiet, bloom_quiet_curve);
    const float bloom_time_dyn_ms = bloom_time_normal_ms
                                  + (bloom_time_quiet_ms - bloom_time_normal_ms) * q_bloom;
    const float bloom_g_target = local_calc_bloom_feedback(sh->fs, sh->bloom_D, bloom_time_dyn_ms);

    const float bloom_smooth_ms = clampf(sh->p.guard.bloom_time_smooth_ms, 10.0f, 1000.0f);
    const float block_sec       = (float)frames / sh->fs;
    const float a_bloom         = expf(-block_sec / (bloom_smooth_ms * 0.001f));
    sh->bloom_g = a_bloom * sh->bloom_g + (1.0f - a_bloom) * bloom_g_target;

    float lpf_bonus_db = 0.0f;
    float desire_dB    = 0.0f;

    if (sh->p.loud.enabled)
    {
        desire_dB    = sh->p.loud.Bmax_dB * powf(block_quiet, sh->p.loud.beta);
        lpf_bonus_db = 20.0f * log10f(fmaxf(sh->loud_gain_lin, EPS));
    }

    float total_unclamped_db = cf_lpf_base_db + lpf_bonus_db;
    float lpf_gain_db        = clampf(total_unclamped_db, 0.0f, sh->p.guard.total_limit_db);
    float gain_lpf           = db_to_lin(lpf_gain_db);
    float snap_lpf_base_db   = cf_lpf_base_db;

    if (!sh->p.enabled)
    {
        lpf_gain_db      = 0.0f;
        lpf_bonus_db     = 0.0f;
        snap_lpf_base_db = 0.0f;
    }

    sh->dbg_L_wide_db    = L_wide_db_start;
    sh->dbg_lpf_base_db  = snap_lpf_base_db;
    sh->dbg_lpf_bonus_db = lpf_bonus_db;
    sh->dbg_lpf_gain_db  = lpf_gain_db;
    sh->dbg_low_fc        = sh->p.tone.low_xover_hz;
    sh->dbg_quiet         = block_quiet;
    sh->dbg_bloom_time_ms = bloom_time_dyn_ms;
    sh->dbg_bloom_g       = sh->bloom_g;

    // Equal-loudness dynamic gain update
    //
    // Previous implementation updated loud_bonus_dB_slow, converted it to linear
    // with powf(), and smoothed loud_gain_lin for every sample.  The control
    // target itself is block based and very slow (tens to hundreds of ms), so
    // update it once per audio block to remove the per-sample powf() cost.
    if (sh->p.loud.enabled && sh->loud_prime)
    {
        // First block after switch-on: land on the target rather than slew to
        // it. The bypass meter kept loud_env_wide live while the effect was
        // off, so block_quiet -- and therefore desire_dB -- is already the
        // right number for the material that is playing; there is nothing to
        // be gained by taking 1.5 s to reach it, and the missing boost was
        // audible as the low-end tail arriving weak after switch-on.
        sh->loud_bonus_dB_slow = desire_dB;
        sh->loud_gain_lin      = db_to_lin(desire_dB);
        sh->loud_prime         = false;
    }
    else if (sh->p.loud.enabled)
    {
        float diff_dB = desire_dB - sh->loud_bonus_dB_slow;

        if (diff_dB >  sh->p.guard.loud_deadband_db)      diff_dB -= sh->p.guard.loud_deadband_db;
        else if (diff_dB < -sh->p.guard.loud_deadband_db) diff_dB += sh->p.guard.loud_deadband_db;
        else diff_dB = 0.0f;

        const float max_up_dB_blk = sh->p.guard.loud_slew_up_dbps * block_sec;
        const float max_dn_dB_blk = sh->p.guard.loud_slew_dn_dbps * block_sec;

        if (diff_dB >  max_up_dB_blk) diff_dB =  max_up_dB_blk;
        if (diff_dB < -max_dn_dB_blk) diff_dB = -max_dn_dB_blk;

        sh->loud_bonus_dB_slow += diff_dB;

        const float desire_lin_slow = db_to_lin(sh->loud_bonus_dB_slow);
        const float tau_ms          = (desire_lin_slow > sh->loud_gain_lin) ? sh->p.loud.atk_ms
                                                                            : sh->p.loud.rel_ms;
        const float tau_s           = fmaxf(tau_ms, 1.0f) * 0.001f;
        const float aB_blk          = expf(-block_sec / tau_s);

        sh->loud_gain_lin = aB_blk * sh->loud_gain_lin + (1.0f - aB_blk) * desire_lin_slow;
    }

    c->wet            = cf_wet;
    c->dry            = cf_dry;
    c->thr            = cf_thr;
    c->env_floor_lpf  = cf_env_floor_lpf;
    c->duck_lpf_coef  = cf_duck_lpf_coef;
    c->exc_target     = cf_exc_target;
    c->exc_aA         = cf_aA;
    c->exc_aR         = cf_aR;
    c->gain_lpf       = gain_lpf;
    c->block_quiet    = block_quiet;
}




// ---------------------------------------------------------------------------
// Bypass level meter.
//
// The wideband level readout (`Lv=` in app_bassenh_dbg_prt) is worth keeping
// while the effect is bypassed, and it does not need the hot path to produce
// it: the display updates at a few hertz, the follower's time constants are 10
// and 200 ms, and the block rate is 1.5 kHz. So instead of the per-sample
// follower the sample loop runs, average |L|+|R| across the block and advance a
// one-pole once per block, with block-rate weights (meter_aA/aR).
//
// One pass of abs-and-accumulate over 32 frames, versus the ~40 operations per
// sample the full loop was spending to produce the same reading -- and the
// block control path (bassenh_block_prepare) then publishes the rest of the
// telemetry line exactly as it did before, so the bypassed display is
// unchanged. It also keeps loud_env_wide live, which is what lets switch-on
// prime the equal-loudness gain to the correct target instead of ramping.
// ---------------------------------------------------------------------------
static void bassenh_bypass_meter(bassenh_t* sh,
                                 const float* p_in_L,
                                 const float* p_in_R,
                                 int frames)
{
    float acc = 0.0f;

    for (int i = 0; i < frames; ++i)
    {
        acc += fabsf(p_in_L[i]) + fabsf(p_in_R[i]);
    }

    // Mean of 0.5*(|L|+|R|), the same quantity the sample loop follows.
    const float wide_avg = acc * (0.5f / (float)frames);
    const float a        = (wide_avg > sh->loud_env_wide) ? sh->meter_aA : sh->meter_aR;

    sh->loud_env_wide = a * sh->loud_env_wide + (1.0f - a) * wide_avg;
}


// ---------------------------------------------------------------------------
// Switch-on reset.
//
// Bypass does not run the effect, so there is no warm state to resume from --
// what is in the delay line is whatever the last enabled block left, however
// long ago, and reading that back out would be a burst of stale material.
// Clear it instead.
//
// loud_env_wide is deliberately NOT cleared: the bypass meter keeps it tracking
// the real input, so the primed gain below starts at the right value. Clearing
// it would report silence, which reads as "quiet playback" and would prime the
// maximum boost regardless of how loud the material actually is.
// ---------------------------------------------------------------------------
static void bassenh_reset_for_enable(bassenh_t* sh)
{
    for (int ch = 0; ch < sh->num_proc_ch; ++ch)
    {
        biquad_reset(&sh->hpf_dc[ch]);
        biquad_reset(&sh->lpf_low1[ch]);
        biquad_reset(&sh->lpf_low2[ch]);
    }
    biquad_reset(&g_room_bq);

    memset(sh->bloom_buf, 0, sizeof(sh->bloom_buf));
    sh->bloom_w = 0;

    sh->env                = 0.0f;
    sh->exc_env            = 0.0f;
    sh->loud_bonus_dB_slow = 0.0f;
    sh->loud_gain_lin      = 1.0f;
    sh->loud_prime         = true;
}


// ---------------------------------------------------------------------------
// Low-band extraction pass.
//
// Run as its own pass over a block-sized scratch buffer rather than inline in
// the main loop: the chain's coefficients and state then stay in registers for
// the whole run instead of being reloaded from *sh every sample (the compiler
// cannot prove `sh` does not alias the output buffer, so inline it must
// reload).
//
// Note that the extracted band is summed to mono on the last line. Filtering
// the mono sum instead would be the same algebra with half the filters, and was
// measured at -8.3 us -- but it is not bit-exact, and it was not taken. The
// analysis is in [internal] bass_enhancer_load_reduction.md sections
// 3.6 and 5.1, and tools/bassenh_bitexact/ still models both orderings.
// ---------------------------------------------------------------------------
static void bassenh_lowband_pass(bassenh_t* sh,
                                 const float* p_in_L,
                                 const float* p_in_R,
                                 float* p_low,
                                 int n)
{
    biquad_mono_t b1L = sh->lpf_low1[0];
    biquad_mono_t b1R = sh->lpf_low1[1];
    biquad_mono_t b2L = sh->lpf_low2[0];
    biquad_mono_t b2R = sh->lpf_low2[1];

    for (int i = 0; i < n; ++i)
    {
        float lowL = biquad_process(&b1L, p_in_L[i]);
        float lowR = biquad_process(&b1R, p_in_R[i]);
#if BASSH_XOVER_X2
              lowL = biquad_process(&b2L, lowL);
              lowR = biquad_process(&b2R, lowR);
#endif //BASSH_XOVER_X2
        p_low[i] = 0.5f * (lowL + lowR);
    }

    sh->lpf_low1[0].bqs = b1L.bqs;
    sh->lpf_low1[1].bqs = b1R.bqs;
    sh->lpf_low2[0].bqs = b2L.bqs;
    sh->lpf_low2[1].bqs = b2R.bqs;
}


// ---------------------------------------------------------------------------
// Sample loop.
//
// Arithmetically identical to the pre-optimisation body, operation for
// operation and in the same order -- verified bit-exact, output and full final
// state, 59/59 cases in both accumulation models (tools/bassenh_bitexact/).
// What changed is everything around the arithmetic:
//
//   - every block-invariant is hoisted out (floor_dyn, the soft-clip knee, the
//     clamped bloom mix, the 1-a envelope weights);
//   - the filter state and the loop-carried scalars live in locals, so they
//     are not written back to *sh every sample;
//   - the bloom delay line is walked in runs that cannot wrap, replacing the
//     per-sample index dance with two post-incremented pointers.
// ---------------------------------------------------------------------------
static void bassenh_loop_opt(bassenh_t* sh,
                             const bassenh_blk_t* c,
                             const float* p_in_L,
                             const float* p_in_R,
                             float* p_out_L,
                             float* p_out_R,
                             int frames)
{
    // ---- block invariants, hoisted out of the sample loop ----
    const float floor_dyn  = c->env_floor_lpf * (0.2f + 0.8f * c->block_quiet);
    const float duck_coef  = c->duck_lpf_coef;
    const float exc_target = c->exc_target;
    const float exc_1mA    = 1.0f - c->exc_aA;
    const float exc_1mR    = 1.0f - c->exc_aR;
    const float cf_wet     = c->wet;
    const float cf_dry     = c->dry;
    const float sub_gain   = c->gain_lpf;

    const float sc_tt      = clampf(c->thr, 0.5f, 0.99f);
    const float sc_c       = 0.4f + 0.4f * (1.0f - sc_tt);   // 0.4..0.8

    const float e_aA = sh->env_aA,  e_1mA = 1.0f - e_aA;
    const float e_aR = sh->env_aR,  e_1mR = 1.0f - e_aR;
    const float l_aA = sh->loud_aA, l_1mA = 1.0f - l_aA;
    const float l_aR = sh->loud_aR, l_1mR = 1.0f - l_aR;

    const float loud_g     = sh->loud_gain_lin;   // block-rate, updated in prepare
    const float bloom_g    = sh->bloom_g;         // block-rate, updated in prepare
    const float bloom_duck = sh->p.guard.bloom_duck;
    const float bloom_mix  = clampf(sh->p.tone.bloom_mix, 0.0f, 1.0f);

    const int   N = BLOOM_BUF_MAX;
    const int   D = sh->bloom_D;

    // ---- loop-carried state in locals ----
    biquad_mono_t room = g_room_bq;
    biquad_mono_t dcL  = sh->hpf_dc[0];
    biquad_mono_t dcR  = sh->hpf_dc[1];

    float env     = sh->env;
    float exc_env = sh->exc_env;
    float lw_env  = sh->loud_env_wide;
    int   w       = sh->bloom_w;

    int done = 0;

    while (done < frames)
    {
        int seg = frames - done;
        if (seg > BASSENH_LOW_SCRATCH) seg = BASSENH_LOW_SCRATCH;

        bassenh_lowband_pass(sh, p_in_L + done, p_in_R + done, g_low_scratch, seg);

        int off = 0;

        while (off < seg)
        {
            int rd = w - D; if (rd < 0) rd += N;

            // Longest run over which neither bloom index wraps.
            int run = seg - off;
            if (run > N - w)  run = N - w;
            if (run > N - rd) run = N - rd;

            const float* pin_L = p_in_L   + done + off;
            const float* pin_R = p_in_R   + done + off;
            float*       pout_L = p_out_L + done + off;
            float*       pout_R = p_out_R + done + off;
            const float* plow  = &g_low_scratch[off];
            const float* prd   = &sh->bloom_buf[rd];
            float*       pwr   = &sh->bloom_buf[w];

            for (int k = 0; k < run; ++k)
            {
                const float xL   = pin_L[k];
                const float xR   = pin_R[k];
                const float lowM = plow[k];

                // Wideband loudness envelope
                const float wide_abs = 0.5f * (fabsf(xL) + fabsf(xR));
                lw_env = (wide_abs > lw_env) ? (l_aA * lw_env + l_1mA * wide_abs)
                                             : (l_aR * lw_env + l_1mR * wide_abs);

                // Low-band envelope and ducking
                const float low_abs = fabsf(lowM);
                env = (low_abs > env) ? (e_1mA * low_abs + e_aA * env)
                                      : (e_1mR * low_abs + e_aR * env);

                const float env_eff  = (env < floor_dyn) ? floor_dyn : env;
                const float duck_lpf = clampf(1.0f - duck_coef * env, 0.35f, 1.0f);

                // Sub generation
                float subM = sub_gain * env_eff * duck_lpf * lowM * loud_g;

                // Bloom -- read before write, so D == 0 (aliased indices) still
                // sees the previous block's sample exactly as the legacy loop did.
                const float y_raw = prd[k];
                const float inj   = 0.75f * lowM + 0.25f * subM;
                pwr[k] = inj + bloom_g * y_raw;

                const float y      = biquad_process(&room, y_raw);
                const float duck_b = 1.0f - fminf(bloom_duck * env, bloom_duck);
                const float m      = bloom_mix * duck_b;

                subM += m * y;

                // Excursion guard
                const float x_abs = fabsf(subM);
                const float a_exc = (x_abs > exc_env) ? exc_1mA : exc_1mR;

                exc_env += a_exc * (x_abs - exc_env);

                if (exc_env > exc_target)
                {
                    subM *= (exc_target / (exc_env + 1e-12f));
                }

                // Output mix
                float yL = cf_dry * xL + cf_wet * subM;
                float yR = cf_dry * xR + cf_wet * subM;

                // DC block & soft clip
                yL = biquad_process(&dcL, yL);
                yR = biquad_process(&dcR, yR);

                const float yL2 = yL * yL;
                const float yR2 = yR * yR;

                yL = yL * (1.0f - sc_c * yL2);
                yR = yR * (1.0f - sc_c * yR2);

                pout_L[k] = yL * sc_tt * 1.2f;
                pout_R[k] = yR * sc_tt * 1.2f;
            }

            w += run; if (w >= N) w = 0;
            off += run;
        }

        done += seg;
    }

    // ---- publish state ----
    g_room_bq.bqs      = room.bqs;
    sh->hpf_dc[0].bqs  = dcL.bqs;
    sh->hpf_dc[1].bqs  = dcR.bqs;
    sh->env            = env;
    sh->exc_env        = exc_env;
    sh->loud_env_wide  = lw_env;
    sh->bloom_w        = w;
}


//===========================================================
// Global Function
//===========================================================

void bassenh_init(bassenh_t* sh, int num_proc_ch, uint32_t sample_rate_Hz)
{
    memset(sh, 0, sizeof(*sh));

    sh->fs          = (float)local_get_valid_sample_rate(sample_rate_Hz);
    sh->num_proc_ch = num_proc_ch;       // channel(slot) per Fs of buffer

    bassenh_rebuild(sh);
}




void bassenh_process(bassenh_t* sh, const float* in, float* out, int frames)
{
    const int ch = sh->num_proc_ch;

    if (ch != 2)
    {
        return;
    }

    bassenh_blk_t c;


    // Bypass costs a block-rate meter and nothing else.
    //
    // The disabled state used to run the whole per-sample loop and then throw
    // the result away one sample at a time, which cost within 4.1 us of having
    // the effect genuinely ON. Take the exit instead, keeping only the level
    // readout the telemetry line needs. The app calls this in place, so the copy
    // below is skipped as well.
    if (!sh->p.enabled)
    {
        bassenh_bypass_meter(sh, &in[0 * frames], &in[1 * frames], frames);

        if (out != in)
        {
            memcpy(out, in, (size_t)(2 * frames) * sizeof(float));
        }
        return;
    }

    bassenh_block_prepare(sh, frames, &c);

    bassenh_loop_opt(sh,
                     &c,
                     &in [0 * frames],
                     &in [1 * frames],
                     &out[0 * frames],
                     &out[1 * frames],
                     frames);
}


//===========================================================
// API
//===========================================================

void app_bassenh_init(uint32_t sample_rate_Hz)
{
    bassenh_init(&g_bassenh,
                 STAGE_1_PROC_CH,
                 local_get_valid_sample_rate(sample_rate_Hz));

    app_bassenh_preset_speaker_A(&g_bassenh);
}


void app_bassenh_enable(bool en)
{
    // Enabling starts from a clean state.
    //
    // While bypassed the module does not run at all (see bassenh_process), so
    // there is no warm envelope or decaying bloom tail to resume from -- what
    // is in there is whatever the last enabled block left, however long ago.
    // Reset rather than resume: deterministic, and inaudible because
    // UsrOperate_Bmode() holds a 600 ms mute across the transition.
    //
    // Safe to do here even though the audio ISR is running: p.enabled is still
    // false at this point (set at the end of this function), so the ISR is
    // taking the bypass exit and is not touching any of this state.
    if (en)
    {
        bassenh_reset_for_enable(&g_bassenh);
    }

    // init debug params
    g_bassenh.dbg_L_wide_db    = 0.0f;
    g_bassenh.dbg_quiet        = 0.0f;
    g_bassenh.dbg_lpf_base_db  = 0.0f;
    g_bassenh.dbg_lpf_bonus_db = 0.0f;
    g_bassenh.dbg_lpf_gain_db  = 0.0f;
    g_bassenh.dbg_low_fc        = 0.0f;
    g_bassenh.dbg_bloom_time_ms = 0.0f;
    g_bassenh.dbg_bloom_g       = 0.0f;

    // set enabled/disabled
    g_bassenh.p.enabled = en;
}


bool app_bassenh_is_enabled(void)
{
    return g_bassenh.p.enabled;
}


// ===== Presets =====

// Sub-Halo Bloom
void app_bassenh_preset_speaker_A(bassenh_t* sh)
{
    bassenh_params_t* p = &sh->p;

    p->enabled = false;

    // ======================================================
    // TONE: sound character / demo tuning
    // ======================================================
    p->tone.low_xover_hz     = 110.0f;
    p->tone.lpf_base_db      = 6.0f;

    p->tone.wet_mix          = 1.00f;
    p->tone.dry_mix          = 1.00f;

    p->tone.bloom_delay_ms      = 22.0f;
//    p->tone.bloom_time_ms       = 300.0f;  // normal / medium-volume tail
    p->tone.bloom_time_ms       = 100.0f;   // normal / medium-volume tail
//    p->tone.bloom_time_quiet_ms = 760.0f;  // quiet-playback tail
    p->tone.bloom_time_quiet_ms = 900.0f;  // quiet-playback tail
    p->tone.bloom_quiet_curve   = 1.6f;    // >1.0: extends mainly at quiet level
    p->tone.bloom_mix           = 0.78f;

    // Room-mode-like resonance after bloom
//    p->tone.room_f0_hz       = 50.0f;
    p->tone.room_f0_hz       = 55.0f;
    p->tone.room_q           = 2.4f;

    // ======================================================
    // LOUD: equal-loudness dynamic bass boost
    // ======================================================
    p->loud.enabled          = true;
    p->loud.alpha            = 1.0f;
    p->loud.beta             = 1.0f;
//    p->loud.Bmax_dB          = 10.5f;
    p->loud.Bmax_dB          = 12.5f;
    p->loud.L_hi_dbfs        = -16.0f;
    p->loud.L_lo_dbfs        = -70.0f;
    p->loud.atk_ms           = 50.0f;
    p->loud.rel_ms           = 300.0f;

    // ======================================================
    // GUARD: protection, stability, and anti-runaway controls
    // ======================================================
    p->guard.limiter_thresh  = 0.92f;
    p->guard.dc_hpf_hz       = 20.0f;
    p->guard.total_limit_db  = 33.0f;

    p->guard.env_floor_lpf       = 0.28f;
    p->guard.duck_lpf_coef       = 0.40f;
    p->guard.bloom_duck          = 0.18f;
    p->guard.bloom_time_smooth_ms = 120.0f;

    p->guard.exc_target      = 0.23f;
    p->guard.exc_attack_ms   = 2.2f;
    p->guard.exc_release_ms  = 240.0f;

    // Usually fixed: loudness bonus stabilizer
    p->guard.loud_slew_up_dbps = 8.0f;
    p->guard.loud_slew_dn_dbps = 12.0f;
    p->guard.loud_deadband_db  = 0.25f;

    bassenh_rebuild(sh);
}


// ===== Processing =====


void app_bassenh_process(const float* in, float* out)
{
    bassenh_process(&g_bassenh, in, out, APP_BLOCK_FRAMES);
}


void app_bassenh_dbg_prt( void )
{

    // Bypassed: derive the readout HERE rather than in the audio ISR.
    //
    // The level itself has to be followed in the ISR (bassenh_bypass_meter, one
    // pass of abs-and-accumulate), but the dB conversion, the quiet curve and
    // the bloom-time derivation are display arithmetic. Doing them at this
    // print's 2.5 Hz instead of the block rate's 1.5 kHz keeps six
    // transcendentals out of the ISR, and out of the peak-block time that the
    // deadline margin is measured against.
    //
    // The gain columns read +0.0 because nothing is being applied, which is how
    // the bypassed state has always presented.
    if( !g_bassenh.p.enabled )
    {
        const bassenh_params_t* p = &g_bassenh.p;

        const float L_wide_db = 20.0f * log10f(fmaxf(g_bassenh.loud_env_wide, EPS));

        float t = (L_wide_db - p->loud.L_hi_dbfs) / (p->loud.L_lo_dbfs - p->loud.L_hi_dbfs);
        t = clampf(t, 0.0f, 1.0f);
        const float quiet = t * t * (3.0f - 2.0f * t);   // smoothstep, as in the block path

        const float t_norm  = clampf(p->tone.bloom_time_ms,       40.0f, 900.0f);
        float       t_quiet = clampf(p->tone.bloom_time_quiet_ms, 40.0f, 900.0f);
        if( t_quiet < t_norm )
        {
            t_quiet = t_norm;
        }
        const float t_dyn = t_norm
                          + (t_quiet - t_norm) * powf(quiet, clampf(p->tone.bloom_quiet_curve,
                                                                    0.25f, 4.0f));

        printf("Lv=%2.2fdB Qt=%1.2f | LPF:base: +0.0 + bonus: +0.0 =  +0.0dB"
               " | fc=%.0fHz | Bloom:T=%.0fms g=%.3f\n",
                L_wide_db,
                quiet,
                p->tone.low_xover_hz,
                t_dyn,
                local_calc_bloom_feedback(g_bassenh.fs, g_bassenh.bloom_D, t_dyn));
        return;
    }

    printf("Lv=%2.2fdB Qt=%1.2f | LPF:base:%+5.1f + bonus:%+5.1f = %+5.1fdB | fc=%.0fHz | Bloom:T=%.0fms g=%.3f\n",
            g_bassenh.dbg_L_wide_db,
            g_bassenh.dbg_quiet,
            g_bassenh.dbg_lpf_base_db,
            g_bassenh.dbg_lpf_bonus_db,
            g_bassenh.dbg_lpf_gain_db,
            g_bassenh.dbg_low_fc,
            g_bassenh.dbg_bloom_time_ms,
            g_bassenh.dbg_bloom_g);
}




void app_bassenh_dbg_minus_key_hdr( void )
{
    g_bassenh.p.loud.Bmax_dB -= 1.0f;
    printf("loud.Bmax_dB=%2.2f \n", g_bassenh.p.loud.Bmax_dB);
}


void app_bassenh_dbg_plus_key_hdr( void )
{
    g_bassenh.p.loud.Bmax_dB += 1.0f;
    printf("loud.Bmax_dB=%2.2f \n", g_bassenh.p.loud.Bmax_dB);
}


void app_bassenh_dbg_set_lpf_cap_db( float lpf_cap_db )
{
    g_bassenh.p.guard.total_limit_db = lpf_cap_db;
    printf("total_limit_db=%2.2f \n", g_bassenh.p.guard.total_limit_db);
}


void app_bassenh_dbg_prt_lpf_cap_db( void )
{
    printf("total_limit_db=%2.2f \n", g_bassenh.p.guard.total_limit_db);
}


#endif //defined(ENA_BASS_ENHANCER)
