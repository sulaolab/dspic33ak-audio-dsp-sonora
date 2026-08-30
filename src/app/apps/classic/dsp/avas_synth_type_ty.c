/* =========================================================================
 * TYPE_TY AVAS synth : L1 line model, run as cluster carriers
 *
 * Replaces the former v3 cluster model (harmonic-grid style: low anchor +
 * 723 Hz cluster + 1.45/1.55 kHz cluster + shared slow FM/AM + glue noise).
 * That engine is gone, not switchable: see WHY IT IS NOT A HARMONIC GRID below.
 *
 * WHAT THIS IS
 * ------------
 *     y(t) = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])          j = 1..185
 *
 * Nothing else.  No FM, no AM, no noise, no envelope beyond the on/off gate.
 * The coefficients are a fixed line-model set -- one frequency, amplitude and
 * phase per line -- generated offline and shipped in
 * avas_synth_type_ty_tables.h.  The engine has no pitch of its own to follow.
 *
 * WHY IT IS NOT A HARMONIC GRID
 * -----------------------------
 * The v3 model synthesised a grid of f0 multiples with per-partial gains.  It
 * sounded thin, and the reason is structural rather than a tuning problem:
 *   - most lines in this coefficient set do not sit on any multiple of the
 *     lowest component, so a grid cannot place them at all.
 *   - what is perceived as one harmonic is a group of 6-13 separate lines
 *     within +-10 Hz of each other, all within 10 dB.  Their mutual beating
 *     IS the perceived richness, and a single partial per grid slot cannot
 *     produce it -- not with AM, not with FM.
 *   - some grid slots hold no line whatsoever.
 * So the grid was abandoned and the line list is used directly.  A grid is not
 * a cheaper approximation of this model; it is a different sound.
 *
 * HOW IT IS COMPUTED  (this is the part that changed)
 * --------------------------------------------------
 * Running 185 oscillators at 48 kHz overflowed the DSP load: the compiled inner
 * loop is ~25 instructions with 3-4 conditional branches per line, i.e. about
 * 5700 cycles against a 4166-cycle per-sample budget -- roughly 137 % for AVAS
 * alone.  The fix is a restructuring that keeps all 185 lines.
 *
 * Group the lines into contiguous clusters no wider than 200 Hz (11 of them for
 * this table).  Within one cluster, exactly:
 *
 *     sum_j A_j cos(2 pi f_j t + p_j) = Re{ e^{i 2 pi fc t} * Z(t) },
 *     Z(t) = sum_j A_j e^{i (2 pi (f_j - fc) t + p_j)}
 *
 * Z is band-limited to the cluster half-span (<= 100 Hz), so it does NOT need
 * to be evaluated at 48 kHz.  Each cluster's Z is rebuilt every AVAS_TYPE_TY_DEC
 * samples and linearly interpolated in between; only the 11 carriers run at full
 * rate.  The rebuilds are STAGGERED: cluster k rebuilds at its own phase within
 * the shared decimation period, so the per-sample cost is nearly flat instead of
 * arriving as one burst every D-th sample (see WHY THE REBUILDS ARE STAGGERED).
 * Cost drops from 185 full-rate oscillators to 11 carriers plus 185/32
 * baseband oscillators per sample -- about 1/7, with the same output.
 *
 * Two details are load-bearing, both established by measurement:
 *   - The rebuild evaluates the envelope ONE BLOCK AHEAD and interpolates
 *     towards it.  Interpolating towards a value already reached delays the
 *     envelope by a whole block, and that delay alone drops accuracy from
 *     48.0 dB to 15.3 dB below signal.  See avas_type_ty_rebuild_cluster().
 *   - The carrier frequency is the cluster's AMPLITUDE-WEIGHTED centroid, not
 *     its geometric middle, so the strongest lines get the smallest baseband
 *     offset and therefore the smallest interpolation error.  Set by the
 *     generator; see the cluster table in avas_synth_type_ty_tables.h.
 *
 * The old direct 185-oscillator engine is not kept behind a switch.  Trimming
 * the line count is NOT the way to buy load either: keeping the strongest 32
 * lines silences 7 of the 13 occupied bands, and even an optimal per-band quota
 * leaves 5.2 dB mean band error, whereas this restructuring costs 0.04 dB.
 * AVAS_TYPE_TY_DEC is the knob.
 *
 * COST
 * ----
 * Measured on hardware: 304.8 us of the 666.6 us block window (45.8 %), margin
 * 180.5 us, no missed block.  The overflow is gone, but this is more than the
 * ~18 % that was aimed for: the carriers dominate (~31 % vs ~14 % for the
 * envelope), so AVAS_TYPE_TY_DEC is no longer the lever -- see avas_synth_type_ty.h.
 *
 * RAM is 2 floats per line (baseband phase + step) plus 6 floats per cluster.
 * Amplitudes stay in flash; the uniform gains are applied once to the summed
 * output.
 *
 * WHY THE REBUILDS ARE STAGGERED  (2026-08-29, AK128 Classic)
 * ----------------------------------------------------------
 * Rebuilding all 11 clusters on the same sample puts all 185 baseband
 * oscillators inside ONE block ISR.  That is only affordable where the block is
 * as long as the decimation period.  It is on AK512 (APP_BLOCK_FRAMES = 32 = D,
 * 666.6 us window); it is NOT on AK128 Classic (APP_BLOCK_FRAMES = 4, 83.3 us),
 * where the measured miss rate was EXACTLY 1 block in 8 = D/APP_BLOCK_FRAMES
 * (13,275 misses in 106,194 blocks = 0.125000), each of those blocks taking
 * 180.3 us against an 83.3 us deadline.  It was heard as a 6 kHz train of
 * dropouts.  The load line read 70.8 %, i.e. inside budget, and that is exactly
 * why it was not believed: a missed block is a HALF+DONE conflict, so one service
 * pass in eight never ran and the average was measured over a system doing
 * seven-eighths of the work.  With the stagger in place and nothing else changed,
 * the same image measures 84.7 % and miss = 0.
 *
 * The clusters are mutually independent, so each one keeps a rebuild period of
 * exactly D samples while starting at a different phase; accuracy is therefore
 * unchanged (the interpolation interval is what sets it, and that is still D).
 * The stagger is spaced by CUMULATIVE LINE COUNT, not by cluster index -- the
 * clusters hold 25,27,27,19,13,19,28,10,9,6,2 lines, so spacing by index would
 * put clusters 0 and 1 (52 lines between them) in the same 4-frame block, where
 * balancing by line count caps the worst block at 32.  See
 * avas_type_ty_cluster_slot(), and
 * [internal] avas_type_ty_ak128_block_burst_2026-08-29.md.
 *
 * AK128 Classic then moved to APP_BLOCK_FRAMES = 32 as well, so the stagger is no
 * longer what stands between this engine and a missed block there.  It stays
 * because it is what makes the engine independent of the block length at all --
 * and because it is worth 8.8 us of peak on a 4-frame block, which is more than
 * that configuration had left.
 *
 * AVAS_TYPE_TY_DEC is NOT a lever against this: the burst is 185 lines whatever
 * D is, so lowering D only makes it more frequent (D = APP_BLOCK_FRAMES means
 * every block) and raising it leaves the same burst in 1 block out of
 * D/APP_BLOCK_FRAMES.
 *
 * KNOWN APPROXIMATIONS vs THE EXACT MODEL
 * ---------------------------------------
 *   - audio_fast_sinf_* is a parabolic approximation, not libm.  Per
 *     oscillator its distortion is low, but 185 of them intermodulate, so the
 *     output is not sample-exact against the model even though the
 *     coefficients are.  Measured floor in the line-free bands: -71.5 dBFS,
 *     against -72.4 dBFS for the direct engine this replaced.
 *   - Phase is a wrapped float accumulator, so each line's phase creeps
 *     relative to the ideal cos(2*pi*f*t) over long runs.  Absolute phases
 *     only matter mutually, and the drift is common-mode to first order.
 *   - The envelope is piecewise linear between rebuilds; see AVAS_TYPE_TY_DEC in
 *     avas_synth_type_ty.h for the measured accuracy per decimation factor.
 *   - The 4 s gate attack (unchanged from the previous engine) is long enough to
 *     dominate a short comparison, so shorten AVAS_TYPE_TY_GATE_ATTACK_S before
 *     comparing renders.
 * ========================================================================= */

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "gain_ctrl.h"
#include "apps/shared/float_conversion.h"
#include "audio_fast_math.h"


/* This is the one translation unit that gets the coefficient arrays themselves.
 * Must precede avas_synth_type_ty.h, which is what pulls the table header in. */
#define AVAS_TYPE_TY_L1_TABLE_DEFINE_DATA
#include "avas_synth_type_ty.h"



#if defined(ENA_AVAS_TYPE_TY_SYNTH)
//===========================================================
// Definition
//===========================================================

#define F_M_PI                    ((float)M_PI)
/* Where the release fade is declared FINISHED, i.e. where the per-sample cost
 * drops to zero and the other AVAS source stops being refused.
 *
 * The release is a one-pole with tau = AVAS_TYPE_TY_GATE_RELEASE_S, so its tail is
 * exponential and never actually reaches zero.  At the former 1e-6 the wait was
 * 13.8 tau ~ 6.9 s (measured on hardware: a request 6.2 s after switching off
 * was still refused, and the load stayed at 73 % that whole time).  -50 dB cuts
 * it to 5.8 tau ~ 2.9 s.  The ramp itself is UNCHANGED -- only the point where
 * we stop rendering it moved, so what is truncated is a -50 dB tail. */
#define AVAS_TYPE_TY_GATE_EPS          (0.0031623f)   /* -50 dB */
#define AVAS_TYPE_TY_INTERNAL_SAMPLE_RATE_HZ  (48000u)

/* Unchanged from the v3 engine so that enabling this source does not change
 * how the system fades the AVAS in and out. */
#define AVAS_TYPE_TY_GATE_ATTACK_S     (4.000f)
#define AVAS_TYPE_TY_GATE_RELEASE_S    (0.500f)

/* 0 dB is the coefficient set's own normalised level (see AVAS_TYPE_TY_L1_NORM). */
#define AVAS_TYPE_TY_TONE_GAIN_DB      (0.0f)

/* ratio = expf(cent * ln2/1200) */
#define AVAS_TYPE_TY_CENT_TO_LN        (0.00057762265f)

/* Every cluster still owes a step-table rewrite.  One bit per cluster because the
 * rebuilds are staggered: see pitch_apply_mask in the header. */
#define AVAS_TYPE_TY_PITCH_APPLY_ALL \
    ((uint16_t)((1u << AVAS_TYPE_TY_L1_CLUSTERS) - 1u))


//===========================================================
// Local Function
//===========================================================

static inline float avas_type_ty_get_valid_fs(float fs)
{
    if (fs > 0.0f)
    {
        return fs;
    }

    /* Fallback only. Normal operation should pass fs via init. */
    return (float)SAMPLE_RATE;
}


static inline float avas_type_ty_alpha_from_tau(float fs, float tau_s)
{
    if (tau_s <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (fs * tau_s));
}

static inline float avas_type_ty_wrap_phase(float x)
{
    return audio_fast_wrap_0_to_2pi(x);
}

static inline bool avas_type_ty_is_fully_gated_off(const avas_type_ty_synth_t *s)
{
    return ((s->gate_target <= 0.0f) && (s->gate <= AVAS_TYPE_TY_GATE_EPS));
}


/* Write the whole step set for one pitch ratio.
 *
 * r multiplies every line frequency, so both the carrier and the baseband
 * offset of each line scale by r -- that, and only that, is what makes this an
 * exact pitch shift of the 185-line model rather than a detune of some of it.
 * The phases are deliberately left alone: they are only mutually meaningful, and
 * a running sound must not jump when the pitch is nudged.
 *
 * Recomputed from the const table every time (not scaled in place), so calling
 * it twice with the same r is a no-op and no base copy of the tables is kept in
 * RAM.  Cost is AVAS_TYPE_TY_L1_CLUSTERS + AVAS_TYPE_TY_L1_TABLE_LINES multiply-divides,
 * once per accepted key press. */
static void avas_type_ty_set_cluster_steps(avas_type_ty_synth_t *s, uint16_t k,
                                           float ratio)
{
    const float w = (2.0f * F_M_PI * ratio) / s->fs;
    const float carrier = s_type_ty_l1_cluster[k].carrier_hz;
    const uint16_t first = s_type_ty_l1_cluster[k].first;
    const uint16_t last  = (uint16_t)(first + s_type_ty_l1_cluster[k].count);

    s->car_step[k] = w * carrier;

    /* Baseband offsets are derived here rather than stored in flash. */
    for (uint16_t i = first; i < last; i++)
    {
        s->bb_step[i] = w * (s_type_ty_l1_line[i].freq_hz - carrier)
                          * (float)AVAS_TYPE_TY_DEC;
    }
}


static void avas_type_ty_set_steps(avas_type_ty_synth_t *s, float ratio)
{
    for (uint16_t k = 0; k < AVAS_TYPE_TY_L1_CLUSTERS; k++)
    {
        avas_type_ty_set_cluster_steps(s, k, ratio);
    }

    s->pitch_ratio = ratio;
}


/* Phase within the shared AVAS_TYPE_TY_DEC period at which cluster k rebuilds.
 *
 * Proportional to the CUMULATIVE line count, not to k: the clusters are of very
 * different sizes, and spacing them by index lands the two largest in the same
 * block on a short-block target.  s_type_ty_l1_cluster[k].first IS the cumulative
 * count, so this is one table read and one divide -- paid once per cluster
 * rebuild, never per sample.
 *
 * Monotone non-decreasing in k, and slot(0) is always 0 because first[0] is 0.
 * Duplicates are possible only for a D smaller than the cluster count, which the
 * caller's loop handles by rebuilding every cluster that shares the slot. */
static inline uint16_t avas_type_ty_cluster_slot(uint16_t k)
{
    return (uint16_t)(((uint32_t)s_type_ty_l1_cluster[k].first
                       * (uint32_t)AVAS_TYPE_TY_DEC)
                      / (uint32_t)AVAS_TYPE_TY_L1_TABLE_LINES);
}


static inline float avas_type_ty_clamp_pitch_ratio(float ratio)
{
    /* The clamp is expressed in cent because that is the unit the trim is
     * specified and printed in.  expf (not exp2f) for the conversion: expf is
     * already linked by the gate alpha, so this pulls in nothing new. */
    const float hi = expf(AVAS_TYPE_TY_PITCH_LIMIT_CENT * AVAS_TYPE_TY_CENT_TO_LN);
    const float lo = 1.0f / hi;

    if (!(ratio > 0.0f)) return 1.0f;   /* also catches NaN */
    if (ratio > hi)      return hi;
    if (ratio < lo)      return lo;
    return ratio;
}


/* Evaluate one cluster's complex envelope Z_k at the current baseband phase and
 * advance that cluster's baseband oscillators by one decimated step.
 *
 * Z_k = sum_{j in cluster} A_j * e^{i * bb_phase[j]}
 *
 * The cluster is a contiguous run of table entries (the generator sorts by
 * frequency for exactly this reason), so no per-line cluster index is needed.
 * bb_step is signed: lines below the carrier rotate backwards. */
static inline void avas_type_ty_eval_cluster(avas_type_ty_synth_t *s, uint16_t k,
                                        float *out_i, float *out_q)
{
    const uint16_t first = s_type_ty_l1_cluster[k].first;
    const uint16_t last  = (uint16_t)(first + s_type_ty_l1_cluster[k].count);
    float zi = 0.0f;
    float zq = 0.0f;

    for (uint16_t i = first; i < last; i++)
    {
        float phase = s->bb_phase[i];
        float amp   = s_type_ty_l1_line[i].amp;

        zi += amp * audio_fast_cosf_0_to_2pi(phase);
        zq += amp * audio_fast_sinf_0_to_2pi(phase);

        s->bb_phase[i] = avas_type_ty_wrap_phase(phase + s->bb_step[i]);
    }

    *out_i = zi;
    *out_q = zq;
}


/* Rebuild ONE cluster's interpolation slope, and hand its pitch trim over.
 *
 * Called once every AVAS_TYPE_TY_DEC samples per cluster, at that cluster's own
 * slot.  env_i/env_q hold the envelope for the sample that is about to be
 * emitted; avas_type_ty_eval_cluster() returns the value AVAS_TYPE_TY_DEC samples
 * LATER (the baseband phase was left one step ahead by the previous call), and
 * the slope walks env_i/env_q onto it over exactly AVAS_TYPE_TY_DEC per-sample
 * increments -- so no explicit hand-over is needed.
 *
 * Computing the target one block ahead is not a refinement, it is the whole
 * accuracy budget: interpolating from the previous target to the current one
 * delays the envelope by a block and measured only 15.3 dB below signal, versus
 * 48.0 dB this way.
 *
 * The pitch hand-over sits here because a rebuild boundary is the only place a
 * cluster's step tables may be rewritten: between two of its rebuilds its
 * baseband phases are mid-flight against a slope computed from the OLD steps, so
 * swapping the tables under them would put some of its lines at the new pitch
 * and the rest at the old one.  Here the slope is about to be recomputed anyway.
 * Clear the bit first, then read the request: a press landing between the two is
 * seen at the next boundary instead of being dropped. */
static void avas_type_ty_rebuild_cluster(avas_type_ty_synth_t *s, uint16_t k)
{
    const float inv_dec = 1.0f / (float)AVAS_TYPE_TY_DEC;
    float next_i;
    float next_q;

    if( s->pitch_apply_mask & (uint16_t)(1u << k) )
    {
        s->pitch_apply_mask &= (uint16_t)~((uint16_t)(1u << k));
        avas_type_ty_set_cluster_steps(s, k, s->pitch_ratio_apply);

        /* Only once every cluster has taken the new ratio do the tables as a
         * whole hold it, which is what pitch_ratio reports. */
        if( s->pitch_apply_mask == 0u )
        {
            s->pitch_ratio = s->pitch_ratio_apply;
        }
    }

    avas_type_ty_eval_cluster(s, k, &next_i, &next_q);

    s->env_di[k] = (next_i - s->env_i[k]) * inv_dec;
    s->env_dq[k] = (next_q - s->env_q[k]) * inv_dec;
}


/* Sum of the running carriers, before any gain.
 *
 *     y = sum_k [ I_k * cos(theta_k) - Q_k * sin(theta_k) ]
 *
 * which is Re{ e^{i theta_k} * Z_k } -- the real part of the cluster's
 * analytic signal.  Only these AVAS_TYPE_TY_L1_CLUSTERS oscillators run at fs. */
static float avas_type_ty_process_carriers(avas_type_ty_synth_t *s)
{
    float y = 0.0f;

    for (uint16_t k = 0; k < AVAS_TYPE_TY_L1_CLUSTERS; k++)
    {
        float phase = s->car_phase[k];

        y += (s->env_i[k] * audio_fast_cosf_0_to_2pi(phase))
           - (s->env_q[k] * audio_fast_sinf_0_to_2pi(phase));

        s->car_phase[k] = avas_type_ty_wrap_phase(phase + s->car_step[k]);

        s->env_i[k] += s->env_di[k];
        s->env_q[k] += s->env_dq[k];
    }

    return y;
}


//===========================================================
// Global Function
//===========================================================

void avas_type_ty_synth_reset_phase(avas_type_ty_synth_t *s)
{
    /* Baseband phases are the measured cos phases as-is: the envelope loop
     * evaluates both cos and sin of them, so the +pi/2 shift the old sine-only
     * bank needed is gone.  Measured phases are -pi..+pi, one wrap brings them
     * into 0..2pi. */
    for (uint16_t i = 0; i < AVAS_TYPE_TY_L1_TABLE_LINES; i++)
    {
        s->bb_phase[i] = avas_type_ty_wrap_phase(s_type_ty_l1_line[i].phase_rad);
    }

    /* Envelope at cluster k's OWN slot, which also leaves its baseband phases one
     * decimated step past that slot -- exactly what avas_type_ty_rebuild_cluster()
     * expects to find at that slot.
     *
     * The pre-advance by slot/D of a step is what keeps the stagger free of a
     * permanent envelope delay.  Priming every cluster at t = 0 instead would make
     * cluster k's rebuild at sample slot(k) deliver Z(D) -- the value belonging to
     * sample D -- so its whole envelope stream would run slot(k) samples late for
     * as long as the engine sounds, tilting its lines' mutual phases by up to
     * 2*pi*100Hz*0.65ms.  Sampling Z on a grid OFFSET by slot(k) is exact; sampling
     * it late is not.  What is left is a hold of at most slot(k) samples (0.65 ms)
     * at start-up, under a 4 s gate attack. */
    for (uint16_t k = 0; k < AVAS_TYPE_TY_L1_CLUSTERS; k++)
    {
        const uint16_t first = s_type_ty_l1_cluster[k].first;
        const uint16_t last  = (uint16_t)(first + s_type_ty_l1_cluster[k].count);
        const float    pre   = (float)avas_type_ty_cluster_slot(k)
                                   / (float)AVAS_TYPE_TY_DEC;

        for (uint16_t i = first; i < last; i++)
        {
            s->bb_phase[i] = avas_type_ty_wrap_phase(s->bb_phase[i]
                                                     + (s->bb_step[i] * pre));
        }

        s->car_phase[k] = 0.0f;
        avas_type_ty_eval_cluster(s, k, &s->env_i[k], &s->env_q[k]);
        s->env_di[k] = 0.0f;
        s->env_dq[k] = 0.0f;
    }

    /* Start the staggered schedule at the top of its period.  Cluster 0's slot is
     * always 0, so it rebuilds on the very first sample and the rest follow at
     * their own slots inside the first AVAS_TYPE_TY_DEC samples.  Until a cluster's
     * slot arrives its slope is 0, i.e. its envelope is HELD at the value primed
     * above (which is Z at that slot, not at t = 0 -- see the loop).
     *
     * The priming loop above is the one place all 11 clusters are evaluated on the
     * same call, and it is deliberately left that way: reset_phase() runs in the
     * caller's context (the main loop, via app_avas_type_ty_set_enable()), not in
     * the block ISR, so its cost cannot miss a deadline. */
    s->dec_phase = 0u;
    s->next_k    = 0u;
    s->next_slot = avas_type_ty_cluster_slot(0u);
}


void avas_type_ty_synth_init(avas_type_ty_synth_t *s, float fs)
{
    memset(s, 0, sizeof(*s));
    fs    = avas_type_ty_get_valid_fs(fs);
    s->fs = fs;

    /* memset above already cleared pitch_req_pending; the request has to start
     * at unity, not at the memset 0.0, because it is what the getter reports. */
    avas_type_ty_set_steps(s, 1.0f);
    s->pitch_ratio_req = 1.0f;
    avas_type_ty_synth_reset_phase(s);

    s->tone_gain   = AVAS_TYPE_TY_L1_NORM * db_to_lin(AVAS_TYPE_TY_TONE_GAIN_DB);
    s->master_gain = Gain_AvasSynth;

    s->gate = 0.0f;
    s->gate_target = 0.0f;
    s->gate_attack_alpha  = avas_type_ty_alpha_from_tau(fs, AVAS_TYPE_TY_GATE_ATTACK_S);
    s->gate_release_alpha = avas_type_ty_alpha_from_tau(fs, AVAS_TYPE_TY_GATE_RELEASE_S);
}


float avas_type_ty_synth_process_sample(avas_type_ty_synth_t *s)
{
    float y;
    float alpha;

    if( avas_type_ty_is_fully_gated_off(s) )
    {
        /* Snap the truncated tail to zero so the stored gate matches what is
         * actually emitted (and so a re-enable does not start from 1e-3). */
        s->gate = 0.0f;

        /* Silence is where a deferred pitch change is free: nothing is being
         * rendered, so rewriting the step tables cannot be heard, and the next
         * enable starts from the new pitch.  Both flags are honoured here --
         * the stop's reset, and any trim keyed while the engine was already
         * silent (which would otherwise wait for the next enable). */
        if( s->pitch_req_pending || s->pitch_apply_mask || s->pitch_req_on_silence )
        {
            s->pitch_req_pending    = 0u;
            s->pitch_apply_mask     = 0u;
            s->pitch_req_on_silence = 0u;
            avas_type_ty_set_steps(s, s->pitch_ratio_req);
        }
        return 0.0f;
    }

    alpha = (s->gate_target > s->gate) ? s->gate_attack_alpha : s->gate_release_alpha;
    s->gate += alpha * (s->gate_target - s->gate);

    /* Take a pending pitch request into the render context's own copies.  One
     * flag test per sample, and it is what makes the per-cluster hand-over
     * race-free: the caller only ever writes pitch_req_pending / pitch_ratio_req,
     * and the mask that is read-modify-written below is private to this context.
     * See pitch_apply_mask in the header. */
    if( s->pitch_req_pending )
    {
        s->pitch_req_pending = 0u;
        s->pitch_ratio_apply = s->pitch_ratio_req;
        s->pitch_apply_mask  = AVAS_TYPE_TY_PITCH_APPLY_ALL;
    }

    /* Envelope rebuild, STAGGERED.  Each cluster is rebuilt once every
     * AVAS_TYPE_TY_DEC samples, but at its own phase, so a handful of baseband
     * oscillators land on this sample instead of all 185 landing on every D-th
     * one.  See WHY THE REBUILDS ARE STAGGERED at the top of this file.
     *
     * A loop, not an if: for a D smaller than the cluster count several clusters
     * share a slot.  The wrap breaks out because cluster 0's slot belongs to the
     * NEXT period, which is also what keeps that degenerate D from spinning. */
    while( s->dec_phase == s->next_slot )
    {
        const uint16_t k = s->next_k;

        avas_type_ty_rebuild_cluster(s, k);

        s->next_k    = (uint8_t)(((k + 1u) < AVAS_TYPE_TY_L1_CLUSTERS)
                                     ? (k + 1u) : 0u);
        s->next_slot = avas_type_ty_cluster_slot(s->next_k);

        if( s->next_k == 0u )
        {
            break;
        }
    }

    if( ++s->dec_phase >= (uint16_t)AVAS_TYPE_TY_DEC )
    {
        s->dec_phase = 0u;
    }

    y = avas_type_ty_process_carriers(s);

    y *= (s->tone_gain * s->master_gain * s->gate);

    /* Final Clamp */
    if (y > 1.0f)  y = 1.0f;
    if (y < -1.0f) y = -1.0f;

    return y;
}


void avas_type_ty_synth_set_tone_gain_db(avas_type_ty_synth_t *s, float db)   { s->tone_gain = AVAS_TYPE_TY_L1_NORM * db_to_lin(db); }
void avas_type_ty_synth_set_master_gain_db(avas_type_ty_synth_t *s, float db) { s->master_gain = db_to_lin(db); }
void avas_type_ty_synth_gate_on(avas_type_ty_synth_t *s)                      { s->gate_target = 1.0f; }
void avas_type_ty_synth_gate_off(avas_type_ty_synth_t *s)                     { s->gate_target = 0.0f; }

void avas_type_ty_synth_request_pitch_ratio(avas_type_ty_synth_t *s, float ratio)
{
    s->pitch_ratio_req     = avas_type_ty_clamp_pitch_ratio(ratio);
    s->pitch_req_pending   = 1u;   /* flag last: the value must be visible first */
}

//===========================================================
// API
//===========================================================

static avas_type_ty_synth_t g_avas_type_ty;

void app_avas_type_ty_init_48k(void)
{
    /*
     * app_avas_type_ty_* is a 48 kHz mono source for fx_domain_48k.
     * The system sample rate is handled only by fx_domain_48k.
     */
    avas_type_ty_synth_init(&g_avas_type_ty, (float)AVAS_TYPE_TY_INTERNAL_SAMPLE_RATE_HZ);
    avas_type_ty_synth_gate_off(&g_avas_type_ty);
}

float app_avas_type_ty_process_sample_48k(void)
{
    return avas_type_ty_synth_process_sample(&g_avas_type_ty);
}


/* "Is this engine still costing anything?"  True until the release fade has
 * finished, which is what the run-time exclusion against the LAMB
 * source needs: the two must never render in the same block. */
bool app_avas_type_ty_is_active(void)
{
    return !avas_type_ty_is_fully_gated_off(&g_avas_type_ty);
}


/* Pitch trim, in cent relative to the table's own pitch.
 *
 * The cent value is kept here rather than derived back from the ratio so that
 * the UI reads exactly what it asked for (and so no logf is linked).  It is the
 * clamped value: the clamp lives with the ratio, so ask the engine for its
 * effective ratio... which we do not need -- clamping the cent here against the
 * same limit gives the identical result and keeps this a plain float. */
static float s_avas_type_ty_pitch_cent = 0.0f;

void app_avas_type_ty_set_pitch_cent(float cent)
{
    if (cent >  AVAS_TYPE_TY_PITCH_LIMIT_CENT) cent =  AVAS_TYPE_TY_PITCH_LIMIT_CENT;
    if (cent < -AVAS_TYPE_TY_PITCH_LIMIT_CENT) cent = -AVAS_TYPE_TY_PITCH_LIMIT_CENT;

    s_avas_type_ty_pitch_cent = cent;
    avas_type_ty_synth_request_pitch_ratio(&g_avas_type_ty,
                                      expf(cent * AVAS_TYPE_TY_CENT_TO_LN));
}

float app_avas_type_ty_get_pitch_cent(void)
{
    return s_avas_type_ty_pitch_cent;
}

/* The ratio the step tables are being asked to hold.  Exposed so the console
 * can print it without linking logf/exp2f of its own. */
float app_avas_type_ty_get_pitch_ratio(void)
{
    return g_avas_type_ty.pitch_ratio_req;
}


/* Every stop path (hotkey 'a', "*cy00", Mute long press) funnels through
 * app_avas_type_ty_set_enable(false), so the reset belongs here rather than in the
 * three callers.  It is a REQUEST, deferred to silence: see
 * pitch_req_on_silence.  The reported cent goes to 0 immediately, because that
 * is what the next sound will be -- what is still fading keeps its pitch. */
static void avas_type_ty_reset_pitch_after_stop(void)
{
    s_avas_type_ty_pitch_cent            = 0.0f;
    g_avas_type_ty.pitch_ratio_req       = 1.0f;
    g_avas_type_ty.pitch_req_pending     = 0u;   /* not at the next rebuild ... */
    g_avas_type_ty.pitch_apply_mask      = 0u;   /* ... nor mid hand-over ... */
    g_avas_type_ty.pitch_req_on_silence  = 1u;   /* ... but once the fade is done */
}


void app_avas_type_ty_set_enable(bool enable)
{
    if(enable)
    {
        /* Restart from the table's phase set so every enable produces the same
         * waveform.  Skipped if the previous release has not finished, where a
         * phase jump would be a click. */
        if( g_avas_type_ty.gate <= AVAS_TYPE_TY_GATE_EPS )
        {
            avas_type_ty_synth_reset_phase(&g_avas_type_ty);
        }
        /* Re-enabled before the release finished, so the deferred reset never
         * reached its silence.  Promote it to an ordinary request: the reported
         * trim is already 0 cent, and leaving the tables detuned would make the
         * console lie until some later silence.  There is no fade to protect
         * here -- the tone is about to be driven back up. */
        if( g_avas_type_ty.pitch_req_on_silence )
        {
            g_avas_type_ty.pitch_req_on_silence = 0u;
            g_avas_type_ty.pitch_req_pending    = 1u;
        }

        avas_type_ty_synth_gate_on(&g_avas_type_ty);
    }
    else
    {
        avas_type_ty_synth_gate_off(&g_avas_type_ty);
        avas_type_ty_reset_pitch_after_stop();
    }
}

#endif //defined(ENA_AVAS_TYPE_TY_SYNTH)
