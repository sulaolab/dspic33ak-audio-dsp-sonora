/* =========================================================================
 * Type_LB AVAS synth : L3 = line model + noise bank
 *
 * Replaces the v07 additive model (16 partials, per-partial AM from two LFOs,
 * three `carrier^power` pulse voices, five shared pitch-wander LFOs, xorshift
 * noise through a one-pole).  That engine is gone rather than switchable, and
 * the reason is that it is not a
 * variant of this one: v07 continues the `type_lb_..._v1..v6` lineage,
 * where L3 is the line-based `L1..L8` lineage, so the two share no coefficients
 * and no arithmetic.  A switch would be two engines, not one engine with a knob.
 *
 * WHAT THIS IS
 * ------------
 *     y(t) = tone(t) + noise(t)
 *
 *     tone(t)  = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])        j = 1..264
 *     noise(t) = sum_b NG[b] * 10^(g_b(t)/20) * noise_b(t)         18 bands
 *                g_b = a slow random walk, sd 1.5 dB at 1.2 Hz, per band
 *
 * Both halves come from ONE coefficient set and are expressed in the SAME units:
 * the 264 line amplitudes and the 18 band levels are directly comparable, so
 * `tone + noise` has no free parameter in it -- which is why there is no mix knob
 * here that anyone is meant to turn.
 *
 * L3 AND NOT L8.  The uniform 1.5 dB / 1.2 Hz movement with no per-band onset
 * ramp is exactly what distinguishes the L3 variant of the coefficient set from
 * L8, and L3 is the shipped one.  Keeping the label in the table's own
 * identifiers (AVAS_TYPE_LB_L3_*) is deliberate: L3, L8 and v07 are easy to
 * confuse, and they are three different sounds.
 *
 * HOW THE TONE HALF IS COMPUTED -- AND WHY IT IS NOT NEW CODE
 * ----------------------------------------------------------
 * Running 264 oscillators at 48 kHz does not fit the per-sample budget.  Group
 * the lines into contiguous clusters no wider than 100 Hz (7 of them here).
 * Within one cluster, exactly:
 *
 *     sum_j A_j cos(2 pi f_j t + p_j) = Re{ e^{i 2 pi fc t} * Z(t) },
 *     Z(t) = sum_j A_j e^{i (2 pi (f_j - fc) t + p_j)}
 *
 * Z is band-limited to the cluster half-span, so it is rebuilt every
 * AVAS_TYPE_LB_DEC samples and linearly interpolated in between; only the 7
 * carriers run at full rate.  All 264 lines stay alive.
 *
 * THAT IS THE SAME EQUATION avas_synth_type_ty.c ALREADY COMPUTED, and it is the
 * finding this whole voice rests on: the Type_TY engine is not a Type_TY engine
 * that happens to be table-driven, it is a LINE-MODEL engine.  So the tone half
 * here is that engine's arithmetic with a second coefficient set -- the two
 * files are deliberately line-for-line comparable, and a change to the
 * cluster/envelope scheme belongs in both.
 *
 * Two details are load-bearing, both established by measurement on the Type_TY
 * voice and unchanged here:
 *   - the rebuild evaluates the envelope ONE BLOCK AHEAD and interpolates
 *     towards it.  Interpolating towards a value already reached delays the
 *     envelope by a whole block, and that delay alone cost 48.0 -> 15.3 dB.
 *   - the carrier frequency is the cluster's AMPLITUDE-WEIGHTED centroid, so
 *     the strongest lines get the smallest baseband offset and therefore the
 *     smallest interpolation error.  Set by the generator.
 *
 * Note which way round the two voices sit: 264 lines but only 7 carriers
 * against the Type_TY's 185 and 11, because all of this voice's energy is below
 * 1.2 kHz.  The carriers are the expensive half, so the tone part is CHEAPER
 * than the Type_TY's despite the longer table.  The noise bank is the addition.
 *
 * THE NOISE BANK -- THE ONLY NEW DSP
 * ---------------------------------
 * One white source, tilted once by cascaded one-poles, feeding a bank of
 * Chamberlin state-variable bandpasses summed with fitted gains, each band's
 * gain modulated by its own slow gust.  Three things about it are not obvious
 * and all three were settled by measuring before writing this file (the numbers
 * live in avas_synth_type_lb_noise_tables.h, which the generator emits):
 *
 *   - THE GAINS MUST BE FITTED, and on the COHERENT sum.  The band levels in the
 *     coefficient set are per-band targets with no skirt overlap; a filter bank
 *     cannot be that, so the composite is not the sum of the targets.  One shared source
 *     means overlapping skirts add with their phases, so a power-sum fit is the
 *     wrong fit.
 *   - THE SOURCE IS TILTED, ONCE.  A 2nd-order skirt falls at 6 dB/octave and
 *     this target falls at 12.4 dB/octave above 1 kHz, so NO vector of gains can
 *     fix it: the loudest band puts a floor across the whole top of the
 *     spectrum.  Tilting the source costs a handful of operations for the entire
 *     bank; a second section per band would cost as much as the bank again.
 *   - THE BAND COUNT IS NOT A WORD-LENGTH QUESTION HERE.  The CK (Q15) port of
 *     this voice built 12 bands; this one builds 15, and what excludes the rest
 *     is the SVF's own stability bound (F < 2 - 1/Q, reached near fs/4) plus
 *     bands the fit itself refuses.  The extra bands buy back the 3.6-5.3 kHz
 *     air that the CK design recorded as its own worst omission, at -12.05 dB.
 *
 * WHAT THIS FILE DOES NOT REPRODUCE FROM THE CK PORT
 * -------------------------------------------------
 * `A_SCALE`, `NORM_SHIFT` and the saturating output arm.  All three exist
 * because a cluster's amplitude sum has to fit Q15 and the whole sum's peak can
 * exceed it.  This core has an FPU; the amplitudes are the measured ones and the
 * output has a clamp.  Porting a workaround for a constraint you do not have is
 * how a port gets slower and worse at the same time.
 *
 * KNOWN APPROXIMATIONS vs THE EXACT MODEL
 * ---------------------------------------
 *   - audio_fast_sinf_* is a parabolic approximation, not libm.  Per oscillator
 *     the distortion is low, but 264 of them intermodulate, so the output is not
 *     sample-exact against the model even though the coefficients are.
 *   - phase is a wrapped float accumulator, so each line's phase creeps relative
 *     to the ideal cos(2*pi*f*t) over long runs.  Absolute phases only matter
 *     mutually, and the drift is common-mode to first order.
 *   - the envelope is piecewise linear between rebuilds (AVAS_TYPE_LB_DEC).
 *   - the gusts use a uniform drive and a first-order 10^(x/20) where the model
 *     uses a Gaussian and pow().  The realised sd is MEASURED (1.60 dB against
 *     the model's 1.50) rather than assumed equal.
 *   - the 4 s gate attack is long enough to dominate a short comparison, so
 *     shorten it before comparing renders.
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
 * Must precede avas_synth_type_lb.h, which is what pulls the table headers in. */
#define AVAS_TYPE_LB_L3_TABLE_DEFINE_DATA
#include "avas_synth_type_lb.h"


#if defined(ENA_AVAS_TYPE_LB_SYNTH)

//===========================================================
// Definition
//===========================================================

#define AVAS_TYPE_LB_TWO_PI              (2.0f * (float)M_PI)
/* ratio = expf(cent * ln2/1200).  Same constant as AVAS_TYPE_TY_CENT_TO_LN -- the two
 * engines share the POT, so they must convert its cent identically. */
#define AVAS_TYPE_LB_CENT_TO_LN          (0.00057762265f)
#define AVAS_TYPE_LB_INTERNAL_FS_HZ      (48000u)

/* Where the release fade is declared FINISHED -- the point at which this engine
 * stops costing anything and stops refusing the TYPE_TY source.  The release is a
 * one-pole (tau = AVAS_TYPE_LB_GATE_RELEASE_S), so its tail is exponential and
 * never actually reaches zero.  Kept identical to AVAS_TYPE_TY_GATE_EPS so both
 * engines hand over on the same rule. */
#define AVAS_TYPE_LB_GATE_EPS            (0.0031623f)   /* -50 dB */

/* Unchanged from the v07 engine and identical to the TYPE_TY voice, so replacing
 * the sound does not change how the system fades the AVAS in and out. */
#define AVAS_TYPE_LB_GATE_ATTACK_S       (4.000f)
#define AVAS_TYPE_LB_GATE_RELEASE_S      (0.500f)

/* 0 dB reproduces the coefficient set's own level: tone+noise peak at 0.9 with
 * the noise 17.85 dB below the tone.  Neither is a mix decision -- see the
 * header. */
#define AVAS_TYPE_LB_TONE_GAIN_DB        (0.0f)

/* THE ONE VALUE THAT IS NOT THE COEFFICIENT SET'S: -8.0 dB of wind.  The
 * coefficient set's own noise/tone ratio is 0 dB of trim, and this ships 8 dB
 * below it, so it is a deliberate departure from "L3 has no free parameters".
 *
 * THE RIGHT NUMBER DEPENDS ON THE PLAYBACK LEVEL, so it must be chosen at the
 * FINAL amplifier volume: wind is broadband and its loudness grows with level
 * faster than the tone's narrow lines do, which means a value chosen at a
 * partial listening level comes out far too loud at full output.  Anyone
 * changing this constant has to re-decide it at full output volume.
 *
 * It is the DEFAULT and not a trim because a trim is lost on every reset, and a
 * level that has to be dialled in by hand every time is effectively a 0 dB
 * image.  "*cn00" returns to the coefficient set's own level at any time.
 *
 * PEAK: the clamp is only a concern in the other direction (the conservative
 * arithmetic peak sum reaches it at +4.6 dB, app_avas_type_lb_noise_headroom_db);
 * -8 dB lowers the peak, so the shipped image now has room rather than 0.6 dB of
 * it.  Anything that raises the noise share still moves that ceiling, so re-read
 * the headroom before dialling UP from the console. */
#define AVAS_TYPE_LB_NOISE_GAIN_DB       (-8.0f)

/* The first-order slope of 10^(x/20), i.e. the whole of the gust's dB->linear
 * conversion (see avas_type_lb_noise_update_gusts).  ln(10)/20 * 1.5 dB is
 * AVAS_TYPE_LB_L3_NOISE_GUST_K to every digit the table carries, which is what makes
 * the depth knob's default the generated constant rather than a value near it. */
#define AVAS_TYPE_LB_LN10_OVER_20        (0.11512925f)

/* Where the linearisation stops being an approximation and starts being a
 * different modulation: at 3 dB sd the clamp (4 sd) already reaches a factor that
 * would zero a band, and the error against the true exponential is asymmetric, so
 * the ear hears pumping rather than a deeper gust.  WARN is what the console
 * prints beside the value; MAX is where the setter stops, left well above WARN
 * because auditioning past the warning is legitimate -- shipping past it without
 * replacing the law is not (header). */
#define AVAS_TYPE_LB_GUST_DEPTH_WARN_DB  (3.0f)
#define AVAS_TYPE_LB_GUST_DEPTH_MAX_DB   (6.0f)


//===========================================================
// Local Function
//===========================================================

static inline float avas_type_lb_get_valid_fs(float fs)
{
    if (fs > 0.0f)
    {
        return fs;
    }

    /* Fallback only. Normal operation should pass fs via init. */
    return (float)SAMPLE_RATE;
}


static inline float avas_type_lb_alpha_from_tau(float fs, float tau_s)
{
    if (tau_s <= 0.0f) return 1.0f;
    return 1.0f - expf(-1.0f / (fs * tau_s));
}


static inline float avas_type_lb_wrap_phase(float x)
{
    return audio_fast_wrap_0_to_2pi(x);
}


static inline bool avas_type_lb_is_fully_gated_off(const avas_synth_type_lb_t *s)
{
    return ((s->gate_target <= 0.0f) && (s->gate <= AVAS_TYPE_LB_GATE_EPS));
}


/* Bring a table phase into 0..2pi.  Used only at reset, so the loop is free.
 *
 * A FULL NORMALISER AND NOT ONE CONDITIONAL ADD, because the parameter file this
 * table is generated from does NOT carry wrapped phases -- MEASURED, it spans
 * 6.04 to 49.04 rad, up to eight turns, where the Type_TY L1 parameter set
 * happens to carry -pi..+pi.  The generator reduces them (cos is 2*pi-periodic, so that
 * is exact), which means this loop should never iterate more than once on the
 * shipped table.  It is here because an oscillator started eight turns outside
 * audio_fast_sinf_0_to_2pi()'s valid range is not slightly wrong, it is a
 * different oscillator, and the per-sample wrap can never recover it: it
 * advances by less than one turn.  So this is the guard that keeps a future
 * table with a different phase convention from being a silent wrong sound
 * rather than a bug someone finds. */
static float avas_type_lb_normalise_phase_full(float phase)
{
    while (phase >= AVAS_TYPE_LB_TWO_PI)
    {
        phase -= AVAS_TYPE_LB_TWO_PI;
    }

    while (phase < 0.0f)
    {
        phase += AVAS_TYPE_LB_TWO_PI;
    }

    return phase;
}


/* ------------------------------------------------------------------------- *
 * The tone half.  Same three functions, same shape and same comments as
 * avas_synth_type_ty.c -- see this file's header for why that is the point.
 * ------------------------------------------------------------------------- */

/* Evaluate one cluster's complex envelope Z_k at the current baseband phase and
 * advance that cluster's baseband oscillators by one decimated step.
 *
 * Z_k = sum_{j in cluster} A_j * e^{i * bb_phase[j]}
 *
 * The cluster is a contiguous run of table entries (the generator sorts by
 * frequency for exactly this reason), so no per-line cluster index is needed.
 * bb_step is signed: lines below the carrier rotate backwards. */
static inline void avas_type_lb_eval_cluster(avas_synth_type_lb_t *s, uint16_t k,
                                           float *out_i, float *out_q)
{
    const uint16_t first = s_type_lb_l3_cluster[k].first;
    const uint16_t last  = (uint16_t)(first + s_type_lb_l3_cluster[k].count);
    float zi = 0.0f;
    float zq = 0.0f;

    for (uint16_t i = first; i < last; i++)
    {
        float phase = s->bb_phase[i];
        float amp   = s_type_lb_l3_line[i].amp;

        zi += amp * audio_fast_cosf_0_to_2pi(phase);
        zq += amp * audio_fast_sinf_0_to_2pi(phase);

        s->bb_phase[i] = avas_type_lb_wrap_phase(phase + s->bb_step[i]);
    }

    *out_i = zi;
    *out_q = zq;
}


/* Rebuild the interpolation slopes for all clusters.
 *
 * Called once every AVAS_TYPE_LB_DEC samples.  env_i/env_q hold the envelope for
 * the sample that is about to be emitted; avas_type_lb_eval_cluster() returns the
 * value AVAS_TYPE_LB_DEC samples LATER (the baseband phase was left one step ahead
 * by the previous call), and the slope walks env_i/env_q onto it over exactly
 * AVAS_TYPE_LB_DEC per-sample increments -- so no explicit hand-over is needed.
 *
 * Computing the target one block ahead is not a refinement, it is the whole
 * accuracy budget: interpolating from the previous target to the current one
 * delays the envelope by a block, which measured 15.3 dB below signal against
 * 48.0 dB this way. */
static void avas_type_lb_rebuild_envelope(avas_synth_type_lb_t *s)
{
    const float inv_dec = 1.0f / (float)AVAS_TYPE_LB_DEC;

    for (uint16_t k = 0; k < AVAS_TYPE_LB_L3_CLUSTERS; k++)
    {
        float next_i;
        float next_q;

        avas_type_lb_eval_cluster(s, k, &next_i, &next_q);

        /* The per-cluster trim belongs HERE and not at the carrier: this runs
         * once per block, and a trim change therefore arrives as the slope of a
         * one-block ramp instead of a step on the envelope.  Unconditional (no
         * `if (gain != 1)`) for the reason the gust mix is -- this loop is
         * inlined into process_sample, and a per-iteration branch made the
         * compiler unswitch the loop and emit two copies of it, +2,336 B. */
        next_i *= s->cl_gain[k];
        next_q *= s->cl_gain[k];

        s->env_di[k] = (next_i - s->env_i[k]) * inv_dec;
        s->env_dq[k] = (next_q - s->env_q[k]) * inv_dec;
    }
}


/* Sum of the running carriers, before any gain.
 *
 *     y = sum_k [ I_k * cos(theta_k) - Q_k * sin(theta_k) ]
 *
 * which is Re{ e^{i theta_k} * Z_k } -- the real part of the cluster's analytic
 * signal.  Only these AVAS_TYPE_LB_L3_CLUSTERS oscillators run at fs. */
static float avas_type_lb_process_carriers(avas_synth_type_lb_t *s)
{
    float y = 0.0f;

    for (uint16_t k = 0; k < AVAS_TYPE_LB_L3_CLUSTERS; k++)
    {
        float phase = s->car_phase[k];

        y += (s->env_i[k] * audio_fast_cosf_0_to_2pi(phase))
           - (s->env_q[k] * audio_fast_sinf_0_to_2pi(phase));

        s->car_phase[k] = avas_type_lb_wrap_phase(phase + s->car_step[k]);

        s->env_i[k] += s->env_di[k];
        s->env_q[k] += s->env_dq[k];
    }

    return y;
}


/* ------------------------------------------------------------------------- *
 * The noise half.  The arithmetic below IS the contract: every coefficient in
 * avas_synth_type_lb_noise_tables.h was fitted against these exact difference
 * equations (the generator proves its transfer function reproduces this
 * recursion to -135 dB), so changing an update order here invalidates the
 * table rather than merely perturbing it.
 * ------------------------------------------------------------------------- */

/* xorshift32, and the same [-1,1) mapping the v07 engine used -- kept on purpose
 * so this board's noise and the CK port's are comparable rather than merely
 * similar.  The zero guard cannot fire from the seeded state (xorshift never
 * reaches zero from a non-zero state); it is there so a future re-seed cannot
 * silence the generator. */
static inline uint32_t avas_type_lb_xorshift32(uint32_t *state)
{
    uint32_t x = *state;

    if (x == 0u) x = 0x4C414D42u;
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    *state = x;
    return x;
}


static inline float avas_type_lb_white(uint32_t *state)
{
    return ((float)((int32_t)(avas_type_lb_xorshift32(state) >> 1)) *
            (1.0f / 1073741824.0f)) - 1.0f;
}


/* The gusts, at the control rate.  They cost NOTHING per sample because the
 * modulated gain is folded into the same coefficient the bank already
 * multiplies by.
 *
 * `nb_walk` is a one-pole random walk normalised to UNIT sd, which is why the
 * clamp is a number of sd rather than a magnitude: it bounds the excursion
 * without being tied to the drive's scale.  The dB->linear conversion is the
 * first-order 10^(x/20); a pow() per band per block is not affordable and would
 * not be more correct, because the target is a modulation SD and that is what
 * the generator measures (1.60 dB against the model's 1.50).
 *
 * The optional COMMON walk (see avas_synth_type_lb_set_gust_corr) is one extra walk
 * per block plus two multiplies per band.  It is skipped entirely at corr = 0 --
 * not as an optimisation but so that the independent model keeps its exact random
 * sequence: one extra draw from nb_grng would shift every band's noise. */
static void avas_type_lb_noise_update_gusts(avas_synth_type_lb_t *s)
{
    const float corr = s->nb_gust_corr;
    float wc = 0.0f;

    if (corr > 0.0f)
    {
        wc = s->nb_walk_c;
        wc += s->nb_gust_a
              * ((s->nb_gust_drive * avas_type_lb_white(&s->nb_grng))
                 - wc);
        if (wc >  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) wc =  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;
        if (wc < -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) wc = -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;
        s->nb_walk_c = wc;
    }

    for (uint16_t b = 0; b < AVAS_TYPE_LB_L3_NOISE_BANDS; b++)
    {
        float w = s->nb_walk[b];
        float m;
        float g;

        w += s->nb_gust_a
             * ((s->nb_gust_drive * avas_type_lb_white(&s->nb_grng))
                - w);
        if (w >  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) w =  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;
        if (w < -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) w = -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;
        s->nb_walk[b] = w;

        /* The stored walk stays the band's OWN one, so the correlation can be
         * turned back down mid-audition without the independent part having been
         * overwritten by the mix.  Re-clamped because the mix of two clamped
         * unit-sd walks can reach (sqrt(1-c^2)+c) times the clamp.
         *
         * UNCONDITIONAL, not guarded by `corr > 0`.  At the default the weights are
         * exactly 1 and 0 and the common walk is exactly 0, so `1*w + 0*0` IS w --
         * the independent model is reproduced bit-for-bit without a branch.  The
         * branch cost 2,336 B of program memory when it was there (measured): this
         * loop is inlined into process_sample, and the compiler unswitched it into
         * two copies. */
        m = (s->nb_gust_ind * w) + (corr * wc);
        if (m >  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) m =  AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;
        if (m < -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP) m = -AVAS_TYPE_LB_L3_NOISE_GUST_CLAMP;

        /* A gust deep enough to invert a band is silence, not a phase flip. */
        g = s_type_lb_l3_noise_g[b] * (1.0f + (s->nb_gust_k * m));
        s->nb_gain[b] = (g > 0.0f) ? g : 0.0f;
    }
}


/* One sample of the bank, in the same units as the line amplitudes -- the caller
 * simply adds it to the carrier sum.
 *
 *  low  += F*band          (band = the previous sample's)
 *  high  = in - low - Q1*band
 *  band += F*high
 *
 * The two loops are the 2nd-order bands and the 4th-order ones.  Splitting them
 * rather than testing an order flag per band is what keeps both branch-free,
 * and it is legitimate because the order rule is a frequency cutoff and the
 * bands are frequency-ascending: the 4th-order bands are always the top ones. */
static float avas_type_lb_noise_sample(avas_synth_type_lb_t *s)
{
    float w = avas_type_lb_white(&s->nb_rng);
    float acc = 0.0f;
    uint16_t b;

#if (AVAS_TYPE_LB_L3_NOISE_TILT_POLES > 0u)
    for (b = 0; b < AVAS_TYPE_LB_L3_NOISE_TILT_POLES; b++)
    {
        s->nb_tilt[b] += AVAS_TYPE_LB_L3_NOISE_TILT_A * (w - s->nb_tilt[b]);
        w = s->nb_tilt[b];
    }
#endif

    for (b = 0; b < AVAS_TYPE_LB_L3_NOISE_BANDS4_FIRST; b++)
    {
        const float f = s_type_lb_l3_noise_f[b];
        float bd = s->nb_band[b];
        float lo = s->nb_low[b] + (f * bd);

        s->nb_low[b] = lo;
        bd += f * (w - lo - (AVAS_TYPE_LB_L3_NOISE_Q1 * bd));
        s->nb_band[b] = bd;

        acc += s->nb_gain[b] * bd;
    }

#if (AVAS_TYPE_LB_L3_NOISE_BANDS4 > 0u)
    for (b = AVAS_TYPE_LB_L3_NOISE_BANDS4_FIRST; b < AVAS_TYPE_LB_L3_NOISE_BANDS; b++)
    {
        const uint16_t j = (uint16_t)(b - AVAS_TYPE_LB_L3_NOISE_BANDS4_FIRST);
        const float f = s_type_lb_l3_noise_f[b];
        float bd = s->nb_band[b];
        float lo = s->nb_low[b] + (f * bd);
        float bd2;
        float lo2;

        s->nb_low[b] = lo;
        bd += f * (w - lo - (AVAS_TYPE_LB_L3_NOISE_Q1 * bd));
        s->nb_band[b] = bd;

        /* Second section, same coefficients, fed by the first's output. */
        bd2 = s->nb_band2[j];
        lo2 = s->nb_low2[j] + (f * bd2);
        s->nb_low2[j] = lo2;
        bd2 += f * (bd - lo2 - (AVAS_TYPE_LB_L3_NOISE_Q1 * bd2));
        s->nb_band2[j] = bd2;

        acc += s->nb_gain[b] * bd2;
    }
#endif

    return acc;
}


//===========================================================
// Global Function
//===========================================================

void avas_synth_type_lb_reset_phase(avas_synth_type_lb_t *s)
{
    /* Baseband phases are the measured cos phases as-is: the envelope loop
     * evaluates both cos and sin of them, so no quarter-turn shift is needed. */
    for (uint16_t i = 0; i < AVAS_TYPE_LB_L3_TABLE_LINES; i++)
    {
        s->bb_phase[i] =
            avas_type_lb_normalise_phase_full(s_type_lb_l3_line[i].phase_rad);
    }

    /* Envelope at t = 0, which also leaves the baseband phases one decimated
     * step ahead -- exactly what avas_type_lb_rebuild_envelope() expects. */
    for (uint16_t k = 0; k < AVAS_TYPE_LB_L3_CLUSTERS; k++)
    {
        s->car_phase[k] = 0.0f;
        avas_type_lb_eval_cluster(s, k, &s->env_i[k], &s->env_q[k]);
        /* Same trim as the rebuild applies, so sample 0 starts at the level being
         * auditioned rather than ramping onto it over the first block. */
        s->env_i[k] *= s->cl_gain[k];
        s->env_q[k] *= s->cl_gain[k];
        s->env_di[k] = 0.0f;
        s->env_dq[k] = 0.0f;
    }

    /* The bank from rest and from its seed, so a re-enable renders the same
     * noise realisation the generator measured -- the tone half is restarted
     * from its measured phases for the same reason. */
    for (uint16_t b = 0; b < AVAS_TYPE_LB_L3_NOISE_BANDS; b++)
    {
        s->nb_low[b]  = 0.0f;
        s->nb_band[b] = 0.0f;
        s->nb_walk[b] = 0.0f;
        s->nb_gain[b] = s_type_lb_l3_noise_g[b];
    }
#if (AVAS_TYPE_LB_L3_NOISE_BANDS4 > 0u)
    for (uint16_t j = 0; j < AVAS_TYPE_LB_L3_NOISE_BANDS4; j++)
    {
        s->nb_low2[j]  = 0.0f;
        s->nb_band2[j] = 0.0f;
    }
#endif
#if (AVAS_TYPE_LB_L3_NOISE_TILT_POLES > 0u)
    for (uint16_t p = 0; p < AVAS_TYPE_LB_L3_NOISE_TILT_POLES; p++)
    {
        s->nb_tilt[p] = 0.0f;
    }
#endif
    s->nb_walk_c = 0.0f;
    s->nb_rng    = AVAS_TYPE_LB_L3_NOISE_SEED;
    s->nb_grng   = AVAS_TYPE_LB_L3_NOISE_GUST_SEED;

    /* Rebuild on the very first sample so the slopes are valid from sample 0. */
    s->dec_count = 1u;
}


/* Write the whole step set for one pitch ratio.  The TYPE_TY engine's
 * avas_type_ty_set_steps() with this voice's tables -- same reasoning, and the same
 * reason it is worth repeating here rather than sharing: the two files are
 * deliberately line-for-line comparable (see the header).
 *
 * r multiplies every line frequency, so both the carrier and the baseband offset
 * of each line scale by r.  That, and only that, is what makes this an exact
 * pitch shift of the 264-line model rather than a detune of part of it: scaling
 * the carriers alone would move each cluster while leaving its internal beating
 * where it was.
 *
 * The phases are deliberately left alone -- they are only mutually meaningful,
 * and a running sound must not jump when the pitch is nudged.
 *
 * Recomputed from the const tables every time rather than scaled in place, so
 * calling it twice with the same r is a no-op (which is what makes the
 * request/apply handshake race-free) and no base copy of the tables sits in RAM.
 * Cost is AVAS_TYPE_LB_L3_CLUSTERS + AVAS_TYPE_LB_L3_TABLE_LINES = 271 multiplies
 * ONCE per accepted change, not per sample; the POT's sampler rate-limits itself
 * to 10 Hz, so this is ~2.7 k multiplies/s against the 264-line envelope rebuild
 * running 1500 times a second.
 *
 * The noise bank is untouched on purpose: the wind is not pitched.  Its band
 * frequencies are fixed band edges, not harmonics of the engine. */
static void avas_type_lb_set_steps(avas_synth_type_lb_t *s, float ratio)
{
    const float w = (AVAS_TYPE_LB_TWO_PI * ratio) / s->fs;

    for (uint16_t k = 0; k < AVAS_TYPE_LB_L3_CLUSTERS; k++)
    {
        const float carrier  = s_type_lb_l3_cluster[k].carrier_hz;
        const uint16_t first = s_type_lb_l3_cluster[k].first;
        const uint16_t last  = (uint16_t)(first + s_type_lb_l3_cluster[k].count);

        s->car_step[k] = w * carrier;

        /* Baseband offsets are derived here rather than stored in flash. */
        for (uint16_t i = first; i < last; i++)
        {
            s->bb_step[i] = w * (s_type_lb_l3_line[i].freq_hz - carrier)
                              * (float)AVAS_TYPE_LB_DEC;
        }
    }

    s->pitch_ratio = ratio;
}


static inline float avas_type_lb_clamp_pitch_ratio(float ratio)
{
    /* The clamp is expressed in cent because that is the unit the trim is
     * specified and printed in.  expf (not exp2f) for the conversion: expf is
     * already linked by the gate alpha, so this pulls in nothing new. */
    const float hi = expf(AVAS_TYPE_LB_PITCH_LIMIT_CENT * AVAS_TYPE_LB_CENT_TO_LN);
    const float lo = 1.0f / hi;

    if (!(ratio > 0.0f)) return 1.0f;   /* also catches NaN */
    if (ratio > hi)      return hi;
    if (ratio < lo)      return lo;
    return ratio;
}


void avas_synth_type_lb_init(avas_synth_type_lb_t *s, float fs)
{
    memset(s, 0, sizeof(*s));
    fs    = avas_type_lb_get_valid_fs(fs);
    s->fs = fs;

    for (uint16_t k = 0; k < AVAS_TYPE_LB_L3_CLUSTERS; k++)
    {
        /* Before reset_phase() below, which primes the envelope THROUGH this
         * trim -- memset leaves it at 0.0, i.e. silence. */
        s->cl_gain[k] = 1.0f;
    }

    /* memset above already cleared pitch_req_pending; the request has to start
     * at unity, not at the memset 0.0, because it is what the getter reports. */
    avas_type_lb_set_steps(s, 1.0f);
    s->pitch_ratio_req = 1.0f;

    avas_synth_type_lb_reset_phase(s);

    /* Both halves carry AVAS_TYPE_LB_L3_NORM so each knob is independent: scaling
     * the tone must not scale the noise with it, and the normalisation belongs
     * to the SUM the two make. */
    s->tone_gain   = AVAS_TYPE_LB_L3_NORM * db_to_lin(AVAS_TYPE_LB_TONE_GAIN_DB);
    s->noise_gain  = AVAS_TYPE_LB_L3_NORM * db_to_lin(AVAS_TYPE_LB_NOISE_GAIN_DB);
    s->master_gain = Gain_AvasSynth;

    /* The generated constants VERBATIM, not a recomputation of them from a rate:
     * the generator's proof that the shipped recurrence matches its transfer
     * function is a proof about these numbers.  set_gust_hz() departs from them
     * deliberately, and only when asked.  Set here rather than in the bank's
     * reset so that a re-enable ('A' off then on) keeps the rate being auditioned. */
    s->nb_gust_a     = AVAS_TYPE_LB_L3_NOISE_GUST_A;
    s->nb_gust_drive = AVAS_TYPE_LB_L3_NOISE_GUST_DRIVE;

    /* Same rule for the depth: the table's K verbatim (it is ln(10)/20 * 1.5 dB,
     * so set_gust_depth_db(1.5) reproduces it), and the bands INDEPENDENT, which
     * is the model.  Both here rather than in the bank's reset so a re-enable
     * keeps whatever is being auditioned. */
    s->nb_gust_k    = AVAS_TYPE_LB_L3_NOISE_GUST_K;
    s->nb_gust_corr = 0.0f;
    s->nb_gust_ind  = 1.0f;

    s->gate = 0.0f;
    s->gate_target = 0.0f;
    s->gate_attack_alpha  = avas_type_lb_alpha_from_tau(fs, AVAS_TYPE_LB_GATE_ATTACK_S);
    s->gate_release_alpha = avas_type_lb_alpha_from_tau(fs, AVAS_TYPE_LB_GATE_RELEASE_S);
}


float avas_synth_type_lb_process_sample(avas_synth_type_lb_t *s)
{
    float y;
    float alpha;

    if( avas_type_lb_is_fully_gated_off(s) )
    {
        /* Snap the truncated tail to zero so the stored gate matches what is
         * actually emitted (and so a re-enable does not start from 1e-3). */
        s->gate = 0.0f;

        /* Silence is where a deferred pitch change is free: nothing is being
         * rendered, so rewriting the step tables cannot be heard, and the next
         * enable starts from the new pitch.  Both flags are honoured here -- the
         * stop's reset, and any trim written while the engine was already silent
         * (which would otherwise wait for the next enable).  Same rule and same
         * placement as the TYPE_TY engine. */
        if( s->pitch_req_pending || s->pitch_req_on_silence )
        {
            s->pitch_req_pending    = 0u;
            s->pitch_req_on_silence = 0u;
            avas_type_lb_set_steps(s, s->pitch_ratio_req);
        }
        return 0.0f;
    }

    alpha = (s->gate_target > s->gate) ? s->gate_attack_alpha : s->gate_release_alpha;
    s->gate += alpha * (s->gate_target - s->gate);

    /* Control rate: 264 baseband oscillators and the per-band gusts, once every
     * AVAS_TYPE_LB_DEC samples.  The burst is absorbed inside one
     * APP_BLOCK_FRAMES ISR.  The gusts ride here rather than in their own
     * counter so there is one control rate in this engine, not two. */
    if( --s->dec_count == 0u )
    {
        s->dec_count = (uint16_t)AVAS_TYPE_LB_DEC;

        /* Pitch trim hand-over.  A rebuild boundary is the ONLY place the step
         * tables may be rewritten: between two rebuilds the baseband phases are
         * mid-flight against slopes computed from the OLD steps, and swapping the
         * tables under them would leave some of the 264 lines at the new pitch and
         * the rest at the old one for the remainder of the block.  Here the slopes
         * are about to be recomputed anyway, so the new steps take effect
         * consistently across every line.
         *
         * Clear the flag first, then read the request: a write landing between the
         * two is seen at the next boundary instead of being dropped. */
        if( s->pitch_req_pending )
        {
            s->pitch_req_pending = 0u;
            avas_type_lb_set_steps(s, s->pitch_ratio_req);
        }

        avas_type_lb_rebuild_envelope(s);
        avas_type_lb_noise_update_gusts(s);
    }

    /* The noise is added BEFORE the gate, so the fade covers both halves --
     * otherwise the bank would be left running at full level under a silent
     * tone, which is audible at the tail of every release. */
    y = (s->tone_gain  * avas_type_lb_process_carriers(s))
      + (s->noise_gain * avas_type_lb_noise_sample(s));

    y *= (s->master_gain * s->gate);

    /* Final Clamp.  AVAS_TYPE_LB_L3_PEAK_ABS is measured on tone+noise, so this
     * is a backstop against a run that beats past the 60 s peak, not a level
     * decision. */
    if (y > 1.0f)  y = 1.0f;
    if (y < -1.0f) y = -1.0f;

    return y;
}


void avas_synth_type_lb_set_tone_gain_db(avas_synth_type_lb_t *s, float db)   { s->tone_gain = AVAS_TYPE_LB_L3_NORM * db_to_lin(db); }
void avas_synth_type_lb_set_noise_gain_db(avas_synth_type_lb_t *s, float db)  { s->noise_gain = AVAS_TYPE_LB_L3_NORM * db_to_lin(db); }
void avas_synth_type_lb_set_master_gain_db(avas_synth_type_lb_t *s, float db) { s->master_gain = db_to_lin(db); }


/* Per-cluster level.  No ramp of its own: the rebuild interpolates the envelope
 * onto the new level over one block (666 us at D = 32), which is slow enough that
 * a 12 dB step is not a click and fast enough to answer a listening question. */
void avas_synth_type_lb_set_cluster_gain_db(avas_synth_type_lb_t *s, uint8_t k, float db)
{
    const float g = db_to_lin(db);

    if (k == AVAS_TYPE_LB_CLUSTER_ALL)
    {
        for (uint16_t i = 0; i < AVAS_TYPE_LB_L3_CLUSTERS; i++)
        {
            s->cl_gain[i] = g;
        }
    }
    else if (k < AVAS_TYPE_LB_L3_CLUSTERS)
    {
        s->cl_gain[k] = g;
    }
    else
    {
        /* Out of range: leave the engine alone.  The caller validates. */
    }
}


/* Move the gust RATE without moving its DEPTH.
 *
 * The walk is w += a*(D*u - w) with u uniform on [-1,1), so var(u) = 1/3 and the
 * stationary variance is a^2 D^2 /3 / (1 - (1-a)^2).  Normalising that to unit sd
 * (which is what makes the clamp a number of sd and the depth one constant K)
 * gives the drive that belongs to a pole:
 *
 *     D = sqrt(3 * (2a - a^2)) / a
 *
 * THE DRIVE IS THEREFORE NOT INDEPENDENT OF THE RATE.  Slowing the pole without
 * rescaling D would also deepen the modulation -- the same one-sample noise gets
 * integrated for longer -- and the audition would be moving two axes at once,
 * which reads as the gust rate being wrong when what changed was the bandwidth.
 * The two are kept orthogonal here by construction.
 *
 * Evaluated at the generated rate this reproduces AVAS_TYPE_LB_L3_NOISE_GUST_DRIVE
 * (34.505972 for a = 0.00502655), so the knob at its default is the shipped table. */
void avas_synth_type_lb_set_gust_hz(avas_synth_type_lb_t *s, float hz)
{
    const float f_ctrl = s->fs / (float)AVAS_TYPE_LB_DEC;
    float a;

    /* Below this the walk barely moves within an audition; above f_ctrl/8 the
     * one-pole stops being a slow gust and starts being part of the noise. */
    if (hz < 0.02f)            hz = 0.02f;
    if (hz > (f_ctrl / 8.0f))  hz = f_ctrl / 8.0f;

    a = (2.0f * 3.14159265358979f * hz) / f_ctrl;

    s->nb_gust_a     = a;
    s->nb_gust_drive = sqrtf(3.0f * ((2.0f * a) - (a * a))) / a;
}


float avas_synth_type_lb_get_gust_hz(const avas_synth_type_lb_t *s)
{
    return (s->nb_gust_a * (s->fs / (float)AVAS_TYPE_LB_DEC))
           / (2.0f * 3.14159265358979f);
}


/* dB sd -> the gain law's K.  ln(10)/20 is the first-order slope of 10^(x/20),
 * and at 1.5 dB it returns the generated AVAS_TYPE_LB_L3_NOISE_GUST_K, so the
 * default of this knob IS the shipped table.  The linearisation is why the header
 * warns about depths past ~3 dB. */
void avas_synth_type_lb_set_gust_depth_db(avas_synth_type_lb_t *s, float db)
{
    if (db < 0.0f)  db = 0.0f;
    if (db > AVAS_TYPE_LB_GUST_DEPTH_MAX_DB) db = AVAS_TYPE_LB_GUST_DEPTH_MAX_DB;

    s->nb_gust_k = AVAS_TYPE_LB_LN10_OVER_20 * db;
}


float avas_synth_type_lb_get_gust_depth_db(const avas_synth_type_lb_t *s)
{
    return s->nb_gust_k / AVAS_TYPE_LB_LN10_OVER_20;
}


/* Mix in a shared walk WITHOUT changing the depth: sqrt(1-c^2) and c weight two
 * independent unit-sd walks, so the mix is unit sd at every c (header).  Only the
 * weights are stored; the walks themselves stay separate, so the knob can be
 * turned back down and the independent realisation is still there. */
void avas_synth_type_lb_set_gust_corr(avas_synth_type_lb_t *s, float corr)
{
    if (corr < 0.0f) corr = 0.0f;
    if (corr > 1.0f) corr = 1.0f;

    s->nb_gust_corr = corr;
    s->nb_gust_ind  = sqrtf(1.0f - (corr * corr));

    /* Entering correlated mode from rest rather than from a stale walk: at
     * corr = 0 the common walk is not advanced, so whatever value it held when the
     * knob was last turned down would otherwise be applied as a step. */
    if (corr <= 0.0f) s->nb_walk_c = 0.0f;
}


float avas_synth_type_lb_get_gust_corr(const avas_synth_type_lb_t *s)
{
    return s->nb_gust_corr;
}
void avas_synth_type_lb_gate_on(avas_synth_type_lb_t *s)                      { s->gate_target = 1.0f; }
void avas_synth_type_lb_gate_off(avas_synth_type_lb_t *s)                     { s->gate_target = 0.0f; }

void avas_synth_type_lb_request_pitch_ratio(avas_synth_type_lb_t *s, float ratio)
{
    s->pitch_ratio_req   = avas_type_lb_clamp_pitch_ratio(ratio);
    s->pitch_req_pending = 1u;   /* flag last: the value must be visible first */
}

//===========================================================
// API
//===========================================================

static avas_synth_type_lb_t g_avas_type_lb;

void app_avas_type_lb_init_48k(void)
{
    /*
     * app_avas_type_lb_* is a 48 kHz mono source for fx_domain_48k.
     * The system sample rate is handled only by fx_domain_48k.
     */
    avas_synth_type_lb_init(&g_avas_type_lb, (float)AVAS_TYPE_LB_INTERNAL_FS_HZ);
    avas_synth_type_lb_gate_off(&g_avas_type_lb);
}


float app_avas_type_lb_process_sample_48k(void)
{
    return avas_synth_type_lb_process_sample(&g_avas_type_lb);
}


/* "Is this engine still costing anything?"  True until the release fade has
 * finished, which is what the run-time exclusion against the TYPE_TY source
 * needs: the two must never render in the same block. */
bool app_avas_type_lb_is_active(void)
{
    return !avas_type_lb_is_fully_gated_off(&g_avas_type_lb);
}


/* Wind-level A/B by ear -- the one level this voice does not take from its
 * coefficient set (see the header's note on the noise gain).  Shadowed in dB because the console reads the
 * value back and because the headroom answer below is a dB answer.  Nothing in
 * avas_synth_type_lb_reset_phase() touches the gains, so a trim survives the 'A'
 * key being toggled off and on. */
static float g_avas_type_lb_noise_gain_db = AVAS_TYPE_LB_NOISE_GAIN_DB;

/* A trim, not a mixer.  The measured ratio is the answer (header); this range is
 * wide enough to hear the question and narrow enough that nobody dials a new
 * balance through it by accident. */
#define AVAS_TYPE_LB_NOISE_TRIM_MIN_DB   (-12.0f)
#define AVAS_TYPE_LB_NOISE_TRIM_MAX_DB   (+12.0f)


float app_avas_type_lb_get_noise_gain_db(void)
{
    return g_avas_type_lb_noise_gain_db;
}


/* Where raising the wind starts costing peak instead of loudness.
 *
 * AVAS_TYPE_LB_L3_NORM scales tone+noise so their (conservatively ARITHMETIC, see
 * the header) peak sum lands at 0.9, and the noise half owns
 * NOISE_PEAK_ABS/PEAK_ABS of that.  Raising the noise by X dB spends only the
 * noise share, so the clamp at +-1.0 is reached when
 *
 *     0.9 * (tone_share + noise_share * 10^(X/20)) = 1.0
 *
 * Past that the output clamp starts working, and a clamp does not sound like
 * "louder wind" -- it sounds like the wind stopping getting louder.  Returned
 * rather than enforced: the peak sum is the arithmetic one, so the real headroom
 * is somewhat larger and this is guidance for the ear, not a limit. */
float app_avas_type_lb_noise_headroom_db(void)
{
    const float share = AVAS_TYPE_LB_L3_NOISE_PEAK_ABS / AVAS_TYPE_LB_L3_PEAK_ABS;

    /* (1.0/0.9 - 1.0) of the normalised peak is what is left over for it. */
    return 20.0f * log10f(1.0f + ((1.0f / 0.9f) - 1.0f) / share);
}


void app_avas_type_lb_set_gust_hz(float hz)
{
    avas_synth_type_lb_set_gust_hz(&g_avas_type_lb, hz);
}


float app_avas_type_lb_get_gust_hz(void)
{
    return avas_synth_type_lb_get_gust_hz(&g_avas_type_lb);
}


void app_avas_type_lb_set_gust_depth_db(float db)
{
    avas_synth_type_lb_set_gust_depth_db(&g_avas_type_lb, db);
}


float app_avas_type_lb_get_gust_depth_db(void)
{
    return avas_synth_type_lb_get_gust_depth_db(&g_avas_type_lb);
}


float app_avas_type_lb_gust_depth_warn_db(void)
{
    return AVAS_TYPE_LB_GUST_DEPTH_WARN_DB;
}


void app_avas_type_lb_set_gust_corr(float corr)
{
    avas_synth_type_lb_set_gust_corr(&g_avas_type_lb, corr);
}


float app_avas_type_lb_get_gust_corr(void)
{
    return avas_synth_type_lb_get_gust_corr(&g_avas_type_lb);
}


void app_avas_type_lb_set_noise_gain_db(float db)
{
    if (db < AVAS_TYPE_LB_NOISE_TRIM_MIN_DB) db = AVAS_TYPE_LB_NOISE_TRIM_MIN_DB;
    if (db > AVAS_TYPE_LB_NOISE_TRIM_MAX_DB) db = AVAS_TYPE_LB_NOISE_TRIM_MAX_DB;

    g_avas_type_lb_noise_gain_db = db;
    avas_synth_type_lb_set_noise_gain_db(&g_avas_type_lb, db);
}


/* Per-cluster tone trim, shadowed in dB for the same two reasons as the wind's
 * (console read-back, and the headroom answer is a dB answer).  Asymmetric range:
 * see the header -- the headroom for raising the tone is barely +1 dB, so ducking
 * is the direction that does the work. */
static float g_avas_type_lb_cl_gain_db[AVAS_TYPE_LB_L3_CLUSTERS] = { 0.0f };

#define AVAS_TYPE_LB_CL_TRIM_MIN_DB      (-24.0f)
#define AVAS_TYPE_LB_CL_TRIM_MAX_DB      (+12.0f)


void app_avas_type_lb_set_cluster_gain_db(uint8_t k, float db)
{
    if (db < AVAS_TYPE_LB_CL_TRIM_MIN_DB) db = AVAS_TYPE_LB_CL_TRIM_MIN_DB;
    if (db > AVAS_TYPE_LB_CL_TRIM_MAX_DB) db = AVAS_TYPE_LB_CL_TRIM_MAX_DB;

    if (k == AVAS_TYPE_LB_CLUSTER_ALL)
    {
        for (uint16_t i = 0; i < AVAS_TYPE_LB_L3_CLUSTERS; i++)
        {
            g_avas_type_lb_cl_gain_db[i] = db;
        }
    }
    else if (k < AVAS_TYPE_LB_L3_CLUSTERS)
    {
        g_avas_type_lb_cl_gain_db[k] = db;
    }
    else
    {
        return;    /* Not a cluster: change nothing, shadow included. */
    }

    avas_synth_type_lb_set_cluster_gain_db(&g_avas_type_lb, k, db);
}


float app_avas_type_lb_get_cluster_gain_db(uint8_t k)
{
    if (k >= AVAS_TYPE_LB_L3_CLUSTERS)  return 0.0f;

    return g_avas_type_lb_cl_gain_db[k];
}


/* The carrier the index means, straight out of the generated table -- so the
 * console prints "66.8 Hz" and the listener knows which of the sounds moved. */
float app_avas_type_lb_get_cluster_carrier_hz(uint8_t k)
{
    if (k >= AVAS_TYPE_LB_L3_CLUSTERS)  return 0.0f;

    return s_type_lb_l3_cluster[k].carrier_hz;
}


/* Same arithmetic as app_avas_type_lb_noise_headroom_db(), with the TONE's share of
 * the measured peak.  It comes out near +1.1 dB because the tone owns 84 % of that
 * peak: the tone half is not where the spare headroom is.  Conservative for a
 * single cluster by construction -- raising one of the seven cannot add more peak
 * than raising all seven -- so it is a safe number to dial one cluster against. */
float app_avas_type_lb_cluster_headroom_db(void)
{
    const float share = AVAS_TYPE_LB_L3_TONE_PEAK_ABS / AVAS_TYPE_LB_L3_PEAK_ABS;

    return 20.0f * log10f(1.0f + ((1.0f / 0.9f) - 1.0f) / share);
}


/* Pitch trim, in cent relative to the table's own pitch.
 *
 * The cent value is kept here rather than derived back from the ratio so that the
 * UI reads exactly what it asked for, and so no logf is linked.  Clamped against
 * the same limit the ratio clamp uses, which gives the identical result and keeps
 * this a plain float.  Same shape as app_avas_type_ty_set_pitch_cent() -- the POT calls
 * whichever engine is sounding, so the two have to behave alike. */
static float s_avas_type_lb_pitch_cent = 0.0f;

void app_avas_type_lb_set_pitch_cent(float cent)
{
    if (cent >  AVAS_TYPE_LB_PITCH_LIMIT_CENT) cent =  AVAS_TYPE_LB_PITCH_LIMIT_CENT;
    if (cent < -AVAS_TYPE_LB_PITCH_LIMIT_CENT) cent = -AVAS_TYPE_LB_PITCH_LIMIT_CENT;

    s_avas_type_lb_pitch_cent = cent;
    avas_synth_type_lb_request_pitch_ratio(&g_avas_type_lb,
                                         expf(cent * AVAS_TYPE_LB_CENT_TO_LN));
}

float app_avas_type_lb_get_pitch_cent(void)
{
    return s_avas_type_lb_pitch_cent;
}

/* The ratio the step tables are being asked to hold.  Exposed so the console can
 * print it without linking logf/exp2f of its own. */
float app_avas_type_lb_get_pitch_ratio(void)
{
    return g_avas_type_lb.pitch_ratio_req;
}


/* Every stop path funnels through app_avas_type_lb_set_enable(false), so the reset
 * belongs here rather than in its callers.  It is a REQUEST, deferred to silence
 * (see pitch_req_on_silence): after gate_off the release fade still runs, and
 * re-pitching a fading tail is an audible slide on the way out.  The reported cent
 * goes to 0 immediately, because that is what the next sound will be -- what is
 * still fading keeps the pitch it was sounding at. */
static void avas_type_lb_reset_pitch_after_stop(void)
{
    s_avas_type_lb_pitch_cent           = 0.0f;
    g_avas_type_lb.pitch_ratio_req      = 1.0f;
    g_avas_type_lb.pitch_req_pending    = 0u;   /* not at the next rebuild ... */
    g_avas_type_lb.pitch_req_on_silence = 1u;   /* ... but once the fade is done */
}


void app_avas_type_lb_set_enable(bool enable)
{
    if(enable)
    {
        /* Restart from the measured phase set (and the bank's seed) so every
         * enable produces the same waveform as the model's t = 0.
         * Skipped if the previous release has not finished, where a phase jump
         * would be a click. */
        if( g_avas_type_lb.gate <= AVAS_TYPE_LB_GATE_EPS )
        {
            avas_synth_type_lb_reset_phase(&g_avas_type_lb);
        }
        /* Re-enabled before the release finished, so the deferred reset never
         * reached its silence.  Promote it to an ordinary request: the reported
         * trim is already 0 cent, and leaving the tables detuned would make the
         * console lie until some later silence.  There is no fade to protect here
         * -- the tone is about to be driven back up. */
        if( g_avas_type_lb.pitch_req_on_silence )
        {
            g_avas_type_lb.pitch_req_on_silence = 0u;
            g_avas_type_lb.pitch_req_pending    = 1u;
        }
        avas_synth_type_lb_gate_on(&g_avas_type_lb);
    }
    else
    {
        avas_synth_type_lb_gate_off(&g_avas_type_lb);
        avas_type_lb_reset_pitch_after_stop();
    }
}

#endif //defined(ENA_AVAS_TYPE_LB_SYNTH)
