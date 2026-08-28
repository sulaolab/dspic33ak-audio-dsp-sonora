

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "gain_ctrl.h"


#include "anc_monitor.h"




#if defined(ENA_ANC_MONITOR)
//===========================================================
// Definition
//===========================================================

#ifndef EPSF
#define EPSF (1.0e-12f)
#endif

// ---- critical section (placeholder) ----
// TODO: later replace with real DISI or interrupt mask if needed.
#ifndef ANCMON_CRIT_BEGIN
#define ANCMON_CRIT_BEGIN()  do{}while(0)
#endif
#ifndef ANCMON_CRIT_END
#define ANCMON_CRIT_END()    do{}while(0)
#endif





//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

static inline float f_abs(float x);
static        void  ancmon_update_levels(ancmon_t* m,
                                         const float* x,
                                         float* rmsL, float* rmsR,
                                         float* pkL,  float* pkR,
                                         uint32_t* clip_add);
static inline float lin_to_dbfs(float x);









//===========================================================
// Variables
//===========================================================


//===========================================================
// Global Function
//===========================================================

void ancmon_init(ancmon_t* m,
                 float fs,
                 int ch,
                 int frames,
                 int ref_ch,
                 int err_ch,
                 int delay_est_decim_blocks)
{
    memset(m, 0, sizeof(*m));

    m->fs     = fs;
    m->ch     = ch;
    m->frames = frames;

    m->ref_ch = ref_ch;
    m->err_ch = err_ch;

    m->corr_len     = ANCMON_CORR_LEN;
    m->corr_max_lag = ANCMON_MAX_LAG;

    // init ring buffers
    m->_rb_w = 0;
    memset(m->_rb_ref, 0, sizeof(m->_rb_ref));
    memset(m->_rb_err, 0, sizeof(m->_rb_err));

    m->delay_est_decim_blocks = (delay_est_decim_blocks < 1) ? 1 : delay_est_decim_blocks;
    m->_blk_count = 0;

    m->clip_th = 0.98f;

    // snapshot defaults
    m->est_delay_samp = 0;
    m->est_corr = 0.0f;
    m->seq = 0;
}

void ancmon_set_clip_threshold(ancmon_t* m, float th)
{
    m->clip_th = clampf(th, 0.1f, 0.9999f);
}

void ancmon_update_in(ancmon_t* m, const float* in_interleaved)
{
    float rmsL, rmsR, pkL, pkR;
    uint32_t clips;

    ancmon_update_levels(m, in_interleaved, &rmsL, &rmsR, &pkL, &pkR, &clips);

    m->_blk_count++;

    m->micL_rms = rmsL;
    m->micR_rms = rmsR;
    m->micL_pk  = pkL;
    m->micR_pk  = pkR;

    m->clip_count += clips;
    m->seq++;
}

void ancmon_update_out(ancmon_t* m, const float* out_interleaved)
{
    float rmsL, rmsR, pkL, pkR;
    uint32_t clips;

    ancmon_update_levels(m, out_interleaved, &rmsL, &rmsR, &pkL, &pkR, &clips);

    m->outL_rms = rmsL;
    m->outR_rms = rmsR;
    m->outL_pk  = pkL;
    m->outR_pk  = pkR;

    m->clip_count += clips;

    m->seq++;
}

void ancmon_dbg_hdr(void)
{
    printf("ANCmon: MicL/MicR(dBFS rms/pk) | OutL/OutR(dBFS rms/pk) | ClipCnt | Delay[smp]/ms Corr\n");
}

void ancmon_dbg_prt(const ancmon_t* m)
{
    const float micL_r = lin_to_dbfs(m->micL_rms);
    const float micR_r = lin_to_dbfs(m->micR_rms);
    const float micL_p = lin_to_dbfs(m->micL_pk);
    const float micR_p = lin_to_dbfs(m->micR_pk);

    const float outL_r = lin_to_dbfs(m->outL_rms);
    const float outR_r = lin_to_dbfs(m->outR_rms);
    const float outL_p = lin_to_dbfs(m->outL_pk);
    const float outR_p = lin_to_dbfs(m->outR_pk);

    const float d_ms = (m->fs > 1.0f) ? (1000.0f * (float)m->est_delay_samp / m->fs) : 0.0f;

    printf("Mic L:%6.1f/%6.1f R:%6.1f/%6.1f | Out L:%6.1f/%6.1f R:%6.1f/%6.1f | Clip:%lu | D:%+4d (%+.2fms) C:%+.3f\n",
           micL_r, micL_p, micR_r, micR_p,
           outL_r, outL_p, outR_r, outR_p,
           (unsigned long)m->clip_count,
           m->est_delay_samp,
           d_ms,
           m->est_corr);
}

void ancmon_est_delay_main(ancmon_t* m)
{
    if (m->ref_ch < 0 || m->err_ch < 0) return;
    if (m->ref_ch >= m->ch || m->err_ch >= m->ch) return;

    const int N = m->corr_len;       // = ANCMON_CORR_LEN (512)
    const int L = m->corr_max_lag;   // = ANCMON_MAX_LAG (64)

    // ---- snapshot buffers (static, no stack) ----
    static float s_ref[ANCMON_CORR_LEN];
    static float s_err[ANCMON_CORR_LEN];

    // ---- 4KB snapshot copy: keep this region tiny ----
    int w0;
    ANCMON_CRIT_BEGIN();
    w0 = m->_rb_w; // capture write index
    // ring (time order: oldest->newest) to linear snapshot
    for (int i = 0; i < N; ++i)
    {
        int idx = w0 + i; if (idx >= N) idx -= N;
        s_ref[i] = m->_rb_ref[idx];
        s_err[i] = m->_rb_err[idx];
    }
    ANCMON_CRIT_END();

    // ---- power ----
    float ref_pow = 0.0f;
    float err_pow = 0.0f;
    for (int i = 0; i < N; ++i)
    {
        const float r = s_ref[i];
        const float e = s_err[i];
        ref_pow += r * r;
        err_pow += e * e;
    }
    if (ref_pow < 1e-9f || err_pow < 1e-9f) return;

    const float denom = sqrtf(ref_pow * err_pow) + EPSF;

    int   best_lag = 0;
    float best_c   = 0.0f;
    float best_abs = -1.0f;

    // ---- correlation on snapshot ----
    for (int lag = -L; lag <= L; ++lag)
    {
        float c = 0.0f;

        for (int i = 0; i < N; ++i)
        {
            int j = i + lag;
            if (j < 0)       j += N;
            else if (j >= N) j -= N;

            c += s_ref[i] * s_err[j];
        }

        c /= denom;
        const float a = fabsf(c);
        if (a > best_abs)
        {
            best_abs = a;
            best_c   = c;      // Keep the sign; it is useful for debugging.
            best_lag = lag;
        }
    }

    m->est_delay_samp = best_lag;
//    m->est_corr       = clampf(best_c, -1.0f, 1.0f);
    m->est_corr       = best_c;
}




//===========================================================
// Local Function
//===========================================================

static inline float f_abs(float x)
{
    return (x < 0.0f) ? -x : x;
}

static void ancmon_update_levels(ancmon_t* m,
                                const float* x,
                                float* rmsL, float* rmsR,
                                float* pkL,  float* pkR,
                                uint32_t* clip_add)
{
    // Assumes at least 2ch present for L/R meter.
    float sumL = 0.0f, sumR = 0.0f;
    float pL = 0.0f, pR = 0.0f;
    uint32_t clips = 0;

    const int ch = m->ch;
    const int frames = m->frames;
    const float th = m->clip_th;

    for (int i = 0; i < frames; ++i)
    {
        const float aL = x[i*ch + 0];
        const float aR = (ch > 1) ? x[i*ch + 1] : 0.0f;

        const float absL = f_abs(aL);
        const float absR = f_abs(aR);

        sumL += aL * aL;
        sumR += aR * aR;

        if (absL > pL) pL = absL;
        if (absR > pR) pR = absR;

        if (absL > th) ++clips;
        if (absR > th) ++clips;

        // push to ring for delay estimation (ref/err)
        // Use newest samples only; write both channels at same time index.
        if (m->_rb_ref && m->_rb_err && (m->ref_ch >= 0) && (m->err_ch >= 0) &&
            (m->ref_ch < ch) && (m->err_ch < ch))
        {
            // Write per-sample into ring
            // ring index advances each sample; newest at _rb_w (write position)
            m->_rb_ref[m->_rb_w] = x[i*ch + m->ref_ch];
            m->_rb_err[m->_rb_w] = x[i*ch + m->err_ch];

            m->_rb_w++;
            if (m->_rb_w >= m->corr_len) m->_rb_w = 0;
        }
    }

    *rmsL = sqrtf(sumL / (float)frames + EPSF);
    *rmsR = sqrtf(sumR / (float)frames + EPSF);
    *pkL  = pL;
    *pkR  = pR;
    *clip_add = clips;
}

static inline float lin_to_dbfs(float x)
{
    return 20.0f * log10f(fmaxf(x, 1.0e-12f));
}








//===========================================================
// app wrapper (single instance)
//===========================================================

// You can place these in app_*.c if you prefer.
static ancmon_t g_ancmon;

void app_ancmon_init(void)
{
    // Typical settings for Phase0:
    // - corr_len: 256..1024 (tradeoff)
    // - max_lag: +/-64 samples (about +/-1.33ms @48k)
    // - decim blocks: e.g., update every 8 blocks
    //
    // NOTE: ref/err channel index must match your STAGE_1_PROC_CH mapping.
    // Example: ref=0, err=1 (if ch0=L, ch1=R and you treat L as ref).
    ancmon_init(&g_ancmon,
               (float)SAMPLE_RATE,
               (int)STAGE_1_PROC_CH,
               (int)APP_BLOCK_FRAMES,
               0,
               1,
               8);

    ancmon_set_clip_threshold(&g_ancmon, 0.98f);
}

void app_ancmon_process_in(const float* in_interleaved)
{
    ancmon_update_in(&g_ancmon, in_interleaved);
}

void app_ancmon_process_out(const float* out_interleaved)
{
    ancmon_update_out(&g_ancmon, out_interleaved);
}

void app_ancmon_dbg_prt(void)
{
    ancmon_est_delay_main(&g_ancmon);
    ancmon_dbg_prt(&g_ancmon);
}


#endif //defined(ENA_ANC_MONITOR)
