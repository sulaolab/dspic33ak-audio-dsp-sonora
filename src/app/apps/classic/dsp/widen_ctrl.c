// ======================================
// widen_ctrl.c
// ======================================

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>   // for fmaxf
//#include "SPI_TDM_drv.h"


#include "widen_ctrl.h"




#if defined(ENA_WIDEN_CTRL)
//===========================================================
// Definition
//===========================================================

/*
 * Fuse the two widen stages into one loop instead of running them in series
 * (see FUSED TWO-STAGE PATH below).  Comment out to get the sequential path
 * from identical sources -- that build is the A/B reference for the listening
 * check, since the fused output is not bit-identical.  Keeping both costs no
 * extra program memory: the sequential path has to stay regardless, as the
 * fallback for the generic and HPF-enabled feature combinations.
 *
 * Defined here rather than next to ENA_WIDEN_2_STAGE because the fused function
 * is compiled out when it is absent, and that guard has to precede the code.
 */
#define ENA_WIDEN_FUSE_2_STAGE


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// Variables
//===========================================================


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


static inline float local_get_sample_rate_f32(const widen_t *w)
{
    if( (w != NULL) && (w->sample_rate_Hz != 0u) )
    {
        return (float)w->sample_rate_Hz;
    }

    return (float)SAMPLE_RATE;
}


// Operates on the doubled Side signal (see SIDE SCALING in widen_ctrl.h); hpf_z
// therefore also holds a doubled value.  The filter is linear, so the doubling
// passes through it exactly and needs no compensation here.
static inline float side_hpf_proc(widen_t *w, float side)
{
    if (w->side_hpf_hz <= 0.0f) return side; // bypass

    // 1st-order HPF (one-pole) using equivalent LPF on state
    // Design: alpha = exp(-2*pi*fc/fs)  -> simple, stable, cheap
    // Implement HPF via: y = side - z; z = z + alpha*(y);
    float y   = side - w->hpf_z;
    w->hpf_z += w->hpf_a * y;
    return y;
}


// One-pole all-pass: y[n] = -a*x[n] + x[n-1] + a*y[n-1]
static inline float allpass_proc(widen_t *w, float x)
{
    float y  = (-w->ap_a * x) + w->ap_x1 + (w->ap_a * w->ap_y1);
    w->ap_x1 = x;
    w->ap_y1 = y;
    return y;
}


static inline bool local_can_use_fast_nohpf_delay_ap(const widen_t* w)
{
    return ( (w != NULL)                  &&
             (w->enabled)                 &&
             (w->num_proc_ch == 2)         &&
             (w->side_hpf_hz <= 0.0f)      &&
             (w->use_delay)               &&
             (w->use_allpass)             &&
             (w->delay_buf != NULL)        &&
             (w->delay_len > 0)            &&
             (w->delay_samp >= 0)          &&
             (w->delay_samp < w->delay_len) );
}


/*
 * Fast path for the current demo setting:
 *   side_hpf_hz = 0, delay enabled, all-pass enabled.
 *
 * Same audio path as widen_process() and the same output bit pattern; what it
 * removes is the overhead the audio math was buried in.  Measured on the linked
 * ELF (conf dsPIC33AK512, -O3 -ffast-math, APP_BLOCK_FRAMES = 32), the previous
 * version of this loop was 40 instructions per sample of which only 12 were
 * audio: 13 went to integer address arithmetic, 5 to compare/branch/loop, 5 to
 * neop FPU-hazard padding.  Three changes attack exactly those:
 *
 *  1) POINTERS ADVANCE.  The old loop indexed p_in_L[n] .. p_out_R[n], and the
 *     compiler re-derived all four addresses from base + slot*samples + n every
 *     sample.  It cannot strength-reduce them by itself because in and out
 *     alias -- app_widen_process() calls widen_process(w, buf, buf, n).
 *
 *  2) THE DELAY LINE IS WALKED IN NON-WRAPPING RUNS.  delay_len is one
 *     WIDEN_DELAY_MS worth of samples (480 at 48 kHz / 10 ms) and a block is
 *     APP_BLOCK_FRAMES (32), so each of the two pointers wraps at most once per
 *     block.  Splitting the block at the wrap replaces the per-sample
 *     "subtract, test, add len, scale, add base" index dance -- about 13
 *     instructions for the read/write pair -- with two post-incremented
 *     accesses.  Correctness does not depend on 32 < 480: the run length is
 *     computed, so any block/delay_len combination is handled, just with more
 *     segments.
 *
 *  3) THE CORE IS 2x UNROLLED.  That retires the all-pass state shuffle (x1/y1
 *     alternate between registers instead of being copied every sample) and
 *     gives the scheduler independent work for the hazard slots the 1x loop
 *     pads with neop.  Unrolling requires delay_samp >= 2, so that the pair's
 *     two delayed reads cannot alias its two writes; below that the 1x tail
 *     loop takes the whole run.
 *
 * Aliasing contract, unchanged in spirit but now relied on for two samples at a
 * time: out must either be in itself or not overlap in at all.  A partially
 * shifted overlap was already unsupported.
 */
#define WIDEN_UNROLL_MIN_DELAY_SAMP     (2)

static void widen_process_fast_nohpf_delay_ap( widen_t*      w,
                                                   const float*  in,
                                                   float*        out,
                                                   int           samples )
{
    const int   Ls       = w->l_slot;
    const int   Rs       = w->r_slot;
    const float side_g   = w->side_gain;
    const float out_g    = w->out_gain_half;   // see SIDE SCALING in widen_ctrl.h
    const float ap_a     = w->ap_a;

    const float* p_in_L  = &in [Ls * samples];
    const float* p_in_R  = &in [Rs * samples];
          float* p_out_L = &out[Ls * samples];
          float* p_out_R = &out[Rs * samples];

    float* const delay_buf = w->delay_buf;
    const int    delay_len = w->delay_len;

    // Read and write indices both advance by one per sample, so the fixed
    // delay_samp distance between them is established once, not per sample.
    int wr = w->delay_w;
    int rd = wr - w->delay_samp;
    if (rd < 0)
    {
        rd += delay_len;
    }

    float ap_x1 = w->ap_x1;
    float ap_y1 = w->ap_y1;

    const bool can_unroll = (w->delay_samp >= WIDEN_UNROLL_MIN_DELAY_SAMP);

    int left = samples;

    while (left > 0)
    {
        // Longest run in which neither index reaches the end of the buffer.
        int run = left;
        if ((delay_len - wr) < run) { run = delay_len - wr; }
        if ((delay_len - rd) < run) { run = delay_len - rd; }

        const float* p_rd = &delay_buf[rd];
              float* p_wr = &delay_buf[wr];

        int n = run;

        if (can_unroll)
        {
            while (n >= 2)
            {
                // All four inputs are read before any output is written: with
                // out == in, storing sample 0 would otherwise overwrite an
                // input of sample 1.
                const float L0 = p_in_L[0];
                const float R0 = p_in_R[0];
                const float L1 = p_in_L[1];
                const float R1 = p_in_R[1];

                const float M0 = L0 + R0;
                const float M1 = L1 + R1;
                const float S0 = (L0 - R0) * side_g;
                const float S1 = (L1 - R1) * side_g;

                // delay_samp >= 2 -> p_rd[0..1] cannot alias p_wr[0..1], so
                // both delayed samples may be read before either is written.
                const float d0 = p_rd[0];
                const float d1 = p_rd[1];

                p_wr[0] = S0;
                p_wr[1] = S1;

                // One-pole all-pass, two samples deep: sample 1 sees sample 0's
                // x and y as its state, so no register copy is needed.
                const float ya = (-ap_a * d0) + ap_x1 + (ap_a * ap_y1);
                const float yb = (-ap_a * d1) + d0    + (ap_a * ya);

                ap_x1 = d1;
                ap_y1 = yb;

                p_out_L[0] = (M0 + ya) * out_g;
                p_out_R[0] = (M0 - ya) * out_g;
                p_out_L[1] = (M1 + yb) * out_g;
                p_out_R[1] = (M1 - yb) * out_g;

                p_in_L  += 2;
                p_in_R  += 2;
                p_out_L += 2;
                p_out_R += 2;
                p_rd    += 2;
                p_wr    += 2;
                n       -= 2;
            }
        }

        while (n > 0)
        {
            const float L = *p_in_L++;
            const float R = *p_in_R++;

            // M/S split, both doubled (see SIDE SCALING in widen_ctrl.h).
            const float Mid  = L + R;
            const float Side = (L - R) * side_g;

            // Haas delay: read the delayed Side, then overwrite with the new one.
            const float Side_d = *p_rd++;
            *p_wr++ = Side;

            // One-pole all-pass on the delayed Side.
            const float y_ap = (-ap_a * Side_d) + ap_x1 + (ap_a * ap_y1);
            ap_x1 = Side_d;
            ap_y1 = y_ap;

            // Re-M/S; out_g folds the 0.5 that the M/S split did not spend.
            *p_out_L++ = (Mid + y_ap) * out_g;
            *p_out_R++ = (Mid - y_ap) * out_g;

            --n;
        }

        wr += run;
        if (wr >= delay_len) { wr -= delay_len; }
        rd += run;
        if (rd >= delay_len) { rd -= delay_len; }

        left -= run;
    }

    w->delay_w = wr;
    w->ap_x1   = ap_x1;
    w->ap_y1   = ap_y1;
}


#if defined(ENA_WIDEN_FUSE_2_STAGE)
/*
 * ---------------------------------------------------------------------------
 * FUSED TWO-STAGE PATH
 * ---------------------------------------------------------------------------
 *
 * app_widen_process() runs the widener twice in series, in place.  Stage 1
 * re-mixes Mid/Side back to L/R and stage 2 immediately re-splits them; those
 * two operations cancel, and every gain involved folds into a constant:
 *
 *   stage 1 out   L1 = (M + y1) * gh1        M   = L + R        gh1 = 0.5*g1
 *                 R1 = (M - y1) * gh1        y1  = ap1(delay1(sg1*(L - R)))
 *
 *   stage 2 in    M2 = L1 + R1 = 2*gh1*M  = g1*M
 *                 S2 = (L1 - R1) * sg2    = y1 * (g1*sg2)
 *
 *   stage 2 out   L' = (M2 + y2) * gh2 = M*(g1*gh2) + y2*gh2
 *                 R' = (M2 - y2) * gh2 = M*(g1*gh2) - y2*gh2
 *
 * So stage 2 never needs L/R: its Mid and Side are stage 1's M and y1 scaled by
 * constants.  Three coefficients carry all four gains:
 *
 *   k_mid  = g1 * gh2         <- Mid's total path gain
 *   k_out  = gh2              <- the outstanding 0.5 of the doubled M/S, once
 *   k_side = g1 * sg2         <- stage-1 remix and stage-2 split, collapsed
 *
 * Note there is no factor of 2 anywhere: stage 2 computes its own Mid as
 * L1 + R1, which already IS the doubled convention the header describes, and
 * 2*gh1 is just g1.  Getting this wrong would be a plain gain error, audible.
 *
 * What that removes per sample, relative to running the fast path twice: two FP
 * adds (the stage-2 M/S split), two stores and two loads (stage 1's L/R out and
 * stage 2's L/R in), and one entire loop's address arithmetic and loop control.
 *
 * THIS IS NOT BIT-EXACT, and that is the reason it is a separate path rather
 * than a rewrite of widen_process().  Two roundings genuinely disappear: the
 * sequential form rounds L1 and R1 to float32 and then rounds their sum and
 * difference again, while the fused form carries M and y1 straight through.  The
 * algebra is identical, the arithmetic is not.  Acceptance is therefore an error
 * norm plus a listening pass -- see tools/widen_bitexact/widen_bitexact.py and
 * [internal] widen_ctrl_load_reduction.md.
 *
 * It does NOT introduce a new state convention.  delay_buf2 ends up holding
 * y1 * g1 * sg2, which is the value the sequential path writes there as
 * (L1 - R1) * sg2.  So the two paths stay state-compatible and may be switched
 * between mid-stream, like the doubled-Side convention in widen_ctrl.h.
 *
 * Structure mirrors widen_process_fast_nohpf_delay_ap(): non-wrapping runs, a 2x
 * unrolled core and a 1x tail, and the same aliasing contract (out is in itself
 * or does not overlap it).  The run length is now the shortest of FOUR indices
 * instead of two, since both delay lines are walked at once.
 */
static void widen_process_fused_2stage( widen_t*      w1,
                                        widen_t*      w2,
                                        const float*  in,
                                        float*        out,
                                        int           samples )
{
    const int   Ls     = w1->l_slot;
    const int   Rs     = w1->r_slot;

    const float sg1    = w1->side_gain;
    const float ap_a1  = w1->ap_a;
    const float ap_a2  = w2->ap_a;

    const float k_mid  = w1->out_gain * w2->out_gain_half;
    const float k_out  = w2->out_gain_half;
    const float k_side = w1->out_gain * w2->side_gain;

    const float* p_in_L  = &in [Ls * samples];
    const float* p_in_R  = &in [Rs * samples];
          float* p_out_L = &out[Ls * samples];
          float* p_out_R = &out[Rs * samples];

    float* const buf1 = w1->delay_buf;
    float* const buf2 = w2->delay_buf;
    const int    len1 = w1->delay_len;
    const int    len2 = w2->delay_len;

    int wr1 = w1->delay_w;
    int rd1 = wr1 - w1->delay_samp;
    if (rd1 < 0) { rd1 += len1; }

    int wr2 = w2->delay_w;
    int rd2 = wr2 - w2->delay_samp;
    if (rd2 < 0) { rd2 += len2; }

    float ap1_x1 = w1->ap_x1;
    float ap1_y1 = w1->ap_y1;
    float ap2_x1 = w2->ap_x1;
    float ap2_y1 = w2->ap_y1;

    // Both stages must clear the unroll distance: a pair's two delayed reads
    // must not alias its two writes, on either delay line.
    const bool can_unroll = (w1->delay_samp >= WIDEN_UNROLL_MIN_DELAY_SAMP) &&
                            (w2->delay_samp >= WIDEN_UNROLL_MIN_DELAY_SAMP);

    int left = samples;

    while (left > 0)
    {
        // Longest run in which none of the four indices reaches a buffer end.
        int run = left;
        if ((len1 - wr1) < run) { run = len1 - wr1; }
        if ((len1 - rd1) < run) { run = len1 - rd1; }
        if ((len2 - wr2) < run) { run = len2 - wr2; }
        if ((len2 - rd2) < run) { run = len2 - rd2; }

        const float* p_rd1 = &buf1[rd1];
              float* p_wr1 = &buf1[wr1];
        const float* p_rd2 = &buf2[rd2];
              float* p_wr2 = &buf2[wr2];

        int n = run;

        if (can_unroll)
        {
            while (n >= 2)
            {
                // Both samples' inputs are read before any output is written:
                // with out == in, storing sample 0 would clobber sample 1's in.
                const float L0 = p_in_L[0];
                const float R0 = p_in_R[0];
                const float L1 = p_in_L[1];
                const float R1 = p_in_R[1];

                const float M0 = L0 + R0;
                const float M1 = L1 + R1;

                // ---- stage 1 side chain ----
                const float S0 = (L0 - R0) * sg1;
                const float S1 = (L1 - R1) * sg1;

                const float e0 = p_rd1[0];
                const float e1 = p_rd1[1];

                p_wr1[0] = S0;
                p_wr1[1] = S1;

                const float y1a = (-ap_a1 * e0) + ap1_x1 + (ap_a1 * ap1_y1);
                const float y1b = (-ap_a1 * e1) + e0     + (ap_a1 * y1a);

                ap1_x1 = e1;
                ap1_y1 = y1b;

                // ---- stage 2 side chain, fed Mid/Side directly ----
                const float T0 = y1a * k_side;
                const float T1 = y1b * k_side;

                const float f0 = p_rd2[0];
                const float f1 = p_rd2[1];

                p_wr2[0] = T0;
                p_wr2[1] = T1;

                const float y2a = (-ap_a2 * f0) + ap2_x1 + (ap_a2 * ap2_y1);
                const float y2b = (-ap_a2 * f1) + f0     + (ap_a2 * y2a);

                ap2_x1 = f1;
                ap2_y1 = y2b;

                // ---- single re-mix, both stages' gains folded in ----
                const float m0 = M0 * k_mid;
                const float m1 = M1 * k_mid;
                const float o0 = y2a * k_out;
                const float o1 = y2b * k_out;

                p_out_L[0] = m0 + o0;
                p_out_R[0] = m0 - o0;
                p_out_L[1] = m1 + o1;
                p_out_R[1] = m1 - o1;

                p_in_L  += 2;
                p_in_R  += 2;
                p_out_L += 2;
                p_out_R += 2;
                p_rd1   += 2;
                p_wr1   += 2;
                p_rd2   += 2;
                p_wr2   += 2;
                n       -= 2;
            }
        }

        while (n > 0)
        {
            const float L = *p_in_L++;
            const float R = *p_in_R++;

            const float Mid = L + R;

            // ---- stage 1: doubled Side, Haas delay, all-pass ----
            const float S1_in = (L - R) * sg1;

            const float e = *p_rd1++;
            *p_wr1++ = S1_in;

            const float y1 = (-ap_a1 * e) + ap1_x1 + (ap_a1 * ap1_y1);
            ap1_x1 = e;
            ap1_y1 = y1;

            // ---- stage 2: Side arrives as y1, no L/R round trip ----
            const float S2_in = y1 * k_side;

            const float f = *p_rd2++;
            *p_wr2++ = S2_in;

            const float y2 = (-ap_a2 * f) + ap2_x1 + (ap_a2 * ap2_y1);
            ap2_x1 = f;
            ap2_y1 = y2;

            // ---- one re-mix for both stages ----
            const float m = Mid * k_mid;
            const float o = y2  * k_out;

            *p_out_L++ = m + o;
            *p_out_R++ = m - o;

            --n;
        }

        wr1 += run;  if (wr1 >= len1) { wr1 -= len1; }
        rd1 += run;  if (rd1 >= len1) { rd1 -= len1; }
        wr2 += run;  if (wr2 >= len2) { wr2 -= len2; }
        rd2 += run;  if (rd2 >= len2) { rd2 -= len2; }

        left -= run;
    }

    w1->delay_w = wr1;
    w1->ap_x1   = ap1_x1;
    w1->ap_y1   = ap1_y1;

    w2->delay_w = wr2;
    w2->ap_x1   = ap2_x1;
    w2->ap_y1   = ap2_y1;
}


static inline bool local_can_fuse_2stage(const widen_t* w1, const widen_t* w2)
{
    return ( (w1 != w2)                                &&
             local_can_use_fast_nohpf_delay_ap(w1)      &&
             local_can_use_fast_nohpf_delay_ap(w2)      &&
             (w1->l_slot == w2->l_slot)                 &&
             (w1->r_slot == w2->r_slot)                 &&
             (w1->num_proc_ch == w2->num_proc_ch) );
}
#endif //defined(ENA_WIDEN_FUSE_2_STAGE)


//===========================================================
// Global Function
//===========================================================

void widen_init( widen_t* w,
                 uint32_t sample_rate_Hz,
                 int      num_proc_ch,
                 int      l_slot,
                 int      r_slot,
                 float*   delay_buf,
                 int      delay_buf_samples )
{
    memset(w, 0, sizeof(*w));

    w->sample_rate_Hz = local_get_valid_sample_rate(sample_rate_Hz);

    w->num_proc_ch = num_proc_ch;
    w->l_slot      = l_slot;
    w->r_slot      = r_slot;

    w->enabled       = false;
    w->out_gain      = 1.0f;
    w->out_gain_half = 0.5f;
    w->side_gain     = 1.0f;
    w->side_hpf_hz = 0.0f;

    w->use_delay   = false;
    w->delay_ms    = 0.0f;
    w->delay_buf   = delay_buf;
    w->delay_len   = (delay_buf && delay_buf_samples>0) ? delay_buf_samples : 0;
    w->delay_w     = 0;
    w->delay_samp  = 0;

    w->use_allpass = false;
    w->ap_a        = 0.0f;
    w->ap_x1       = 0.0f;
    w->ap_y1       = 0.0f;

    // derive HPF coeff (start neutral)
    w->hpf_a       = 0.0f;
    w->hpf_z       = 0.0f;

    if (w->delay_buf && w->delay_len>0)
    {
        for (int i=0;i<w->delay_len;i++) w->delay_buf[i]=0.0f;
    }
}


void widen_set_params( widen_t* w,
                       bool     enabled,
                       float    out_gain_db,
                       float    side_gain,
                       float    side_hpf_hz,
                       bool     use_delay,
                       float    delay_ms,
                       bool     use_allpass,
                       float    ap_a )
{
    w->enabled     = enabled;
    w->out_gain    = powf(10.0f, out_gain_db * 0.05f);
    w->side_gain   = side_gain;
    w->side_hpf_hz = side_hpf_hz;

    // The loops multiply by this instead of out_gain; it absorbs the 0.5 that
    // the doubled M/S split leaves outstanding (see SIDE SCALING in the header).
    // 0.5 is exact in binary floating point, so nothing is lost here.
    w->out_gain_half = 0.5f * w->out_gain;

    float sample_rate = local_get_sample_rate_f32(w);

    // derive 1st-order HPF pole from fc
    if (side_hpf_hz > 0.0f)
    {
        float alpha = expf(-2.0f * (float)M_PI * side_hpf_hz / sample_rate);
        w->hpf_a = clampf(alpha, 0.0f, 0.9999f);
    }
    else
    {
        w->hpf_a = 0.0f;
        w->hpf_z = 0.0f;
    }

    w->use_delay  = use_delay && (w->delay_buf && w->delay_len>0);
    w->delay_ms   = (w->use_delay) ? fmaxf(0.0f, delay_ms) : 0.0f;
    w->delay_samp = 0;

    // compute delay samples (Right only)
    if (w->use_delay)
    {
        int d = (int)lrintf((w->delay_ms * 0.001f) * sample_rate);
        if (d >= w->delay_len) d = w->delay_len-1;
        if (d < 0) d = 0;

        // Store both quantized delay_ms and cached sample delay.
        // Processing functions use delay_samp directly to avoid lrintf() per block.
        w->delay_samp = d;
        w->delay_ms = 1000.0f * ((float)d / sample_rate); // quantized value
    }

    w->use_allpass = use_allpass;
    w->ap_a        = clampf(ap_a, 0.0f, 0.98f);
    if (!w->use_allpass) { w->ap_x1 = w->ap_y1 = 0.0f; }
}




void widen_process(   widen_t* w,
                    const float*   in,
                          float*   out,
                          int      samples )
{
    const int ch = w->num_proc_ch;

    // bypass if it's not 2ch (currently there is no capability of 4ch)
    if (ch != 2)
    {
        return;
    }

    if (!w->enabled)
    {
        // passthrough
        if (out != in)
        {
           const int n = samples * ch;
           for(int idx=0; idx<n; idx++) out[idx] = in[idx];
        }
        return;
    }

    if (local_can_use_fast_nohpf_delay_ap(w))
    {
        widen_process_fast_nohpf_delay_ap(w, in, out, samples);
        return;
    }

    const int Ls   = w->l_slot;
    const int Rs   = w->r_slot;
    const float sg = w->side_gain;
    const float og = w->out_gain_half;   // see SIDE SCALING in widen_ctrl.h

    const float* p_in_L  = &in [Ls * samples];
    const float* p_in_R  = &in [Rs * samples];
          float* p_out_L = &out[Ls * samples];
          float* p_out_R = &out[Rs * samples];

    // Cached by widen_set_params().
    const int d_samp = w->delay_samp;

    for (int n=0; n<samples; ++n)
    {
        float L = p_in_L[n];
        float R = p_in_R[n];

        // --- M/S split --- both doubled; og takes the factor back out below.
        float Mid  = L + R;
        float Side = L - R;

        // optional HPF on Side (to widen highs only)
        Side = side_hpf_proc(w, Side);

        // widen by boosting Side
        Side *= sg;

        // --- Optional Delay on Right ---
        if (w->use_delay)
        {
            // read delayed sample for Right
            int rd = w->delay_w - d_samp;
            if (rd < 0) rd += w->delay_len;

            float Sd = w->delay_buf[rd];

            w->delay_buf[w->delay_w] = Side;
            w->delay_w++;
            if (w->delay_w >= w->delay_len) w->delay_w = 0;

            Side = Sd;
        }

        // --- Optional All-pass on Right ---
        if (w->use_allpass)
        {
            Side = allpass_proc(w, Side);
        }

        // 4) Re-M/S
        float Lp = Mid + Side;
        float Rp = Mid - Side;

        p_out_L[n] = Lp * og;
        p_out_R[n] = Rp * og;
    }
}










//===========================================================
// API
//===========================================================

#define ENA_WIDEN_2_STAGE

#define WIDEN_DELAY_MS              (10u)

/*
 * Delay buffer size is based on the build-time sample rate.
 * If runtime sample-rate switching is added later, set this to the maximum
 * supported rate, e.g. 96000u.
 */
#define WIDEN_MAX_SAMPLE_RATE_HZ    (SAMPLE_RATE)

static widen_t g_widen1;
static float   g_delay_buf1[WIDEN_MAX_SAMPLE_RATE_HZ * WIDEN_DELAY_MS / 1000u];

#if defined(ENA_WIDEN_2_STAGE)
static widen_t g_widen2;
static float   g_delay_buf2[WIDEN_MAX_SAMPLE_RATE_HZ * WIDEN_DELAY_MS / 1000u];
#endif //defined(ENA_WIDEN_2_STAGE)




void app_widen_init(uint32_t sample_rate_Hz)
{
    uint32_t valid_sample_rate_Hz = local_get_valid_sample_rate(sample_rate_Hz);

    widen_init( &g_widen1,
                valid_sample_rate_Hz,
                STAGE_1_PROC_CH,
                0,
                1,
                g_delay_buf1,
                (int)ARRAY_SIZE(g_delay_buf1) );

    widen_set_params(&g_widen1, false, 0.0f, 1.0f, 0.0f, false, 0.0f, false, 0.0f);

#if defined(ENA_WIDEN_2_STAGE)
    widen_init( &g_widen2,
                valid_sample_rate_Hz,
                STAGE_1_PROC_CH,
                0,
                1,
                g_delay_buf2,
                (int)ARRAY_SIZE(g_delay_buf2) );

    // out_gain_db 0.0f, not the "01.0f" typo this line used to carry: harmless
    // while enabled is false, but it would have been +1 dB on stage 2 if anyone
    // ever enabled the stage without calling widen_set_params() again.
    widen_set_params(&g_widen2, false, 0.0f, 1.0f, 0.0f, false, 0.0f, false, 0.0f);
#endif //defined(ENA_WIDEN_2_STAGE)
}


/*
 * Turning widening off means enabled = false, not "enabled with every feature
 * neutralised".
 *
 * The old form passed enabled = true and switched the features off one by one.
 * That still ran the full per-sample loop -- measured at ~70 instructions per
 * sample across the two stages, ~22 us of the 666.7 us block, i.e. nearly the
 * cost of having widening ON -- to compute a unity M/S round trip.  With
 * enabled = false, widen_process() takes its passthrough exit, and because the
 * app calls it in place (in == out) the passthrough copies nothing: the cost is
 * zero.
 *
 * It is also more accurate, not less.  The round trip
 * 0.5*(L+R) + 0.5*(L-R) does not return L exactly -- (L+R) and (L-R) each round
 * -- so the "neutral" path was quietly lossy.  Skipping it is exact.
 *
 * Nothing else changes: the disabled state already had use_delay = false, so
 * the delay buffer was not being written either way, and re-enabling starts
 * from the same state it did before.
 */
void app_widen_disable(void)
{
    widen_set_params(&g_widen1,
                     false,            // <- disable: passthrough, zero cost
                     0.0f,             // out_gain_db
                     1.0f,             // side_gain (ignored)
                     0.0f,             // side_hpf_hz
                     false, 0.0f,      // use_delay   delay_ms
                     false, 0.0f);     // use_allpass ap_a
#if defined(ENA_WIDEN_2_STAGE)
    widen_set_params(&g_widen2,
                     false,            // <- disable: passthrough, zero cost
                     0.0f,             // out_gain_db
                     1.0f,             // side_gain (ignored)
                     0.0f,             // side_hpf_hz
                     false, 0.0f,      // use_delay    delay_ms
                     false, 0.0f);     // use_allpass  ap_a
#endif //defined(ENA_WIDEN_2_STAGE)
}


#if defined(ENA_WIDEN_2_STAGE)
// Extreme demo: apply widening twice (two-pass in series)
void app_widen_enable(void)
{
    widen_set_params(&g_widen1, true,  // enabled
                    -1.8f,             // out_gain_db
                     1.8f,             // side_gain
                     0.0f,             // side_hpf_hz = 0 -> full-band target
                     true, 2.3f,       // delay on
                     true, 0.70f);     // all-pass on

    widen_set_params(&g_widen2, true,  // enabled
                    -3.0f,             // out_gain_db
                     3.5f,             // side_gain
                     0.0f,             // side_hpf_hz = 0 -> full-band target
                     true, 7.7f,       // delay on
                     true, 0.85f);     // all-pass on
}

#else

void app_widen_enable(void)
{
    widen_set_params(&g_widen1, true,  // enabled
                     5.0f,             // side_gain
                     0.0f,             // side_hpf_hz = 0 -> full-band target
                     true, 8.0f,       // delay on
                     true, 0.85f);     // all-pass on (regularly 0.6~0.8)
}

#endif //defined(ENA_WIDEN_2_STAGE)






void app_widen_process(const float* in, float* out)
{
#if defined(ENA_WIDEN_2_STAGE)

#if defined(ENA_WIDEN_FUSE_2_STAGE)
    // Both stages on the fast path with matching layout: one loop does both.
    if (local_can_fuse_2stage(&g_widen1, &g_widen2))
    {
        widen_process_fused_2stage(&g_widen1, &g_widen2, in, out, APP_BLOCK_FRAMES);
        return;
    }
#endif //defined(ENA_WIDEN_FUSE_2_STAGE)

    widen_process(&g_widen1, in,  out, APP_BLOCK_FRAMES);
    widen_process(&g_widen2, out, out, APP_BLOCK_FRAMES);
#else
    widen_process(&g_widen1, in,  out, APP_BLOCK_FRAMES);
#endif //defined(ENA_WIDEN_2_STAGE)
}



#endif //defined(ENA_WIDEN_CTRL)
