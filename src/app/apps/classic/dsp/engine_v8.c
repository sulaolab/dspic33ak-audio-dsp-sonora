
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "board/devices/pot_drv.h"
#include "apps/shared/float_conversion.h"


#include "engine_v8.h"


/* engine_v8 is the DEFAULT model. engine_synth.c holds the one it replaced and
 * compiles for the opposite setting, so exactly one of the two is ever linked --
 * they define the same app_engine_synth_* entry points. See engine_select.h. */
#if defined(ENA_ENGINE_SYNTH) && !defined(ENA_ENGINE_SYNTH_LEGACY)

#include "engine_v8_tables.h"

//===========================================================
// Definition
//===========================================================

/* The measured V8 model, sample by sample.
 *
 * One engine cycle = 2 crank revolutions, so f_cycle = rpm / 120 and every
 * table here is indexed by cycle phase, not by time. That is why a table of P
 * points per cycle is band-limited to crank ORDER P/4 and not to a fixed Hz --
 * see the point-count table printed into engine_v8_tables.h.
 *
 * Per sample:
 *     wave   two neighbouring RPM bins, linearly interpolated in phase and
 *            crossfaded by rpm
 *     noise  two readers into one shared table, one engine cycle apart, summed
 *            through a periodic Hann window (they are exactly half a window
 *            apart, so the pair of weights is w and 1-w from ONE table)
 *     mask   out = wave + n + gate * LPF1k5( (MASK_AMT-1) * n )
 *
 * The mask is what keeps a fast RPM change from sounding like a chord: below
 * 2125 rpm the wavetables are one transposed waveform (there is no recording of
 * 900-2100 rpm), and the extra low-passed noise while |d rpm/dt| is large covers
 * it. The gate is 0 whenever the speed is steady, so the settled idle -- the part
 * that was listened to and accepted -- is bit-for-bit unmasked.
 *
 * Every number below is frozen: a constant edited in isolation is a change of
 * sound, not a tuning knob.  Numbers, and why each one is what it is:
 *   [internal] astm_v8_resynthesis_analysis.md, section 39
 *   validated against the prototype by tools/classic/astm_v8_resynth/a34_impl_check.py
 */

#define ENGINE_V8_FS_HZ                (48000.0f)
#define ENGINE_V8_BLOCK                (APP_BLOCK_FRAMES)
#define ENGINE_V8_DT_S                 ((float)ENGINE_V8_BLOCK / ENGINE_V8_FS_HZ)

/* Cycle-to-cycle jitter, in cycles (section 36: 0.020 adopted). Applied as a
 * per-cycle duration scale 1 / (1 + d[k+1] - d[k]) so the MEAN rate is exact --
 * a per-cycle random rate would make the engine drift off the commanded rpm. */
#define ENGINE_V8_JITTER               (0.020f)

/* Chord masking (section 38): residual noise x6, low-passed at 1.5 kHz, gated by
 * clip(|d rpm/dt| / 1200). 1200 rather than 4000 because this engine's descent
 * is 2171 rpm/s where the rise is 8656 -- at 4000 the descent was only half
 * masked, which is where the chord was still audible (section 36). */
#define ENGINE_V8_MASK_SCALE           (1200.0f)
#define ENGINE_V8_MASK_EXTRA           (5.0f)      /* x6 total, x1 is already in */

/* 2nd-order Butterworth, 1.5 kHz at 48 kHz (scipy.signal.butter(2, 1500/24000)).
 * The prototype used a brick-wall FFT low-pass; a34 measures the difference this
 * substitution makes at 0.19 dB octave rms. */
#define ENGINE_V8_MASK_B0              (0.00844269293f)
#define ENGINE_V8_MASK_B1              (0.0168853859f)
#define ENGINE_V8_MASK_B2              (0.00844269293f)
#define ENGINE_V8_MASK_A1              (-1.72377617f)
#define ENGINE_V8_MASK_A2              (0.757546944f)

/* Gate smoothing, one pole at 8 Hz on the block rate (1500 Hz). a34 swept
 * 4...200 Hz and the result moves by 0.04 dB, so this is only anti-zipper. */
#define ENGINE_V8_GATE_A               (0.96704493f)

/* Idle wander (section 25, "drift40_rough"): +-40 rpm. The prototype's random
 * walk is normalised over the whole array, which is neither causal nor bounded;
 * a 1-pole process with the same slowness (0.15 Hz) is bounded by construction
 * and the clamp says so out loud. Left running at every rpm on purpose -- +-40
 * rpm is inaudible at 6875 and one unconditional path beats a threshold. */
#define ENGINE_V8_DRIFT_A              (0.999371879f)
#define ENGINE_V8_DRIFT_STEP           (0.460693263f)
#define ENGINE_V8_DRIFT_CLAMP          (40.0f)

/* POT follower slew. Only the POT is limited: the blip curve below IS the
 * measured trajectory (its own rise is 8656 rpm/s) and must not be re-limited. */
#define ENGINE_V8_POT_SLEW_PER_S       (3000.0f)

/* Blip = the measured 3000 rpm flare (a31.flare_traj, section 38): rise 0.253 s,
 * fall 2.057 s with a knot at 45 % of the fall at 1.10 x the base rpm. At the
 * idle this is exactly the trajectory of the approved render; from a raised POT
 * the peak keeps at least +800 rpm of headroom, as the old model did. */
#define ENGINE_V8_BLIP_PEAK_RPM        (3000.0f)
#define ENGINE_V8_BLIP_MIN_STEP_RPM    (800.0f)
#define ENGINE_V8_BLIP_UP_S            (0.253f)
#define ENGINE_V8_BLIP_DN_S            (2.057f)
#define ENGINE_V8_BLIP_KNOT            (0.45f)
#define ENGINE_V8_BLIP_KNOT_RPM_MULT   (1.10f)

/* Output level (section 41). The model's own peak, measured over the full-range
 * trajectory with every element enabled, is 0.5439; 1.8 put that at 0.979 and the
 * clamp below DID engage on the flare. The 0.64 in the note this replaces was the
 * flare-only render, which is 1.4 dB quieter than the sweep. 1.70 leaves the peak
 * at 0.925, i.e. the clamp is a guard again. The absolute level then belongs to
 * PRE_GAIN_ENG_SYNTH_DB alone, which section 41 raises -36 -> -20 dB: at -36 dB
 * the idle sat at -79.8 dBFS after the mix trim and was inaudible on the board. */
#define ENGINE_V8_OUT_GAIN             (1.70f)
#define ENGINE_V8_OUT_CLAMP            (0.95f)

#define ENGINE_V8_LCG_SEED             (0x5EED1234uL)

/* POT deadband, in ADC LSB (section 39, ENGINE_V8_STAGE_POTFILT). Wider than the
 * board's measured wander, so a knob at rest gives a commanded rpm that does not
 * move at all -- slew exactly 0, gate 0, the settled idle unmasked. 96 LSB is
 * 140 rpm of throttle resolution, which is nothing against a 900..6875 span.
 *
 * Measured on the AK512 bench 2026-08-18 with the knob at rest: raw 0..56 LSB,
 * i.e. +-28. At 48 the mean gate was already 0/1000 but it still peaked at
 * 260/1000 -- the odd excursion crossed the dead zone and let a fifth of the
 * mask through for a few tens of ms, which is an audible whoosh at idle and
 * exactly the artefact this element exists to remove. 96 leaves no excursion. */
#define ENGINE_V8_POT_DEADBAND         (96u)


//===========================================================
// Variables
//===========================================================

static float    s_mix_gain    = 0.0f;   /* 0 = disabled; also the is_enable() flag */

/* block rate */
static float    s_base_rpm    = ENGINE_V8_IDLE_RPM;   /* POT follower, slew limited */
static float    s_cmd_rpm     = ENGINE_V8_IDLE_RPM;   /* + blip; the gate reads this */
static float    s_prev_cmd    = ENGINE_V8_IDLE_RPM;
static float    s_rpm         = ENGINE_V8_IDLE_RPM;   /* + drift; what is sounded */
static float    s_drift       = 0.0f;
static float    s_gate        = 0.0f;
static uint16_t s_pos         = ENGINE_V8_BLOCK;      /* forces a block update */

/* blip */
static bool     s_blip_start  = false;
static uint8_t  s_blip_phase  = 0u;    /* 0 = idle, 1 = rise, 2 = fall */
static float    s_blip_t      = 0.0f;
static float    s_blip_from   = 0.0f;
static float    s_blip_peak   = 0.0f;

/* cycle oscillator */
static float    s_phase       = 0.0f;   /* 0..1 within one engine cycle */
static float    s_inc         = 0.0f;
static float    s_jit_cur     = 0.0f;
static float    s_jit_scale   = 1.0f;

/* per-cycle bin pair */
static uint16_t s_wave_off_lo = 0u;
static uint16_t s_wave_off_hi = 0u;
static uint16_t s_wave_pts_lo = 0u;
static uint16_t s_wave_pts_hi = 0u;
static float    s_bin_a       = 0.0f;
static float    s_wave_gain   = 0.0f;
static float    s_noise_gain  = 0.0f;

/* noise readers and the mask filter */
static uint16_t s_noise_off_new = 0u;
static uint16_t s_noise_off_old = 0u;
static float    s_mask_z1     = 0.0f;
static float    s_mask_z2     = 0.0f;

static uint32_t s_rng         = ENGINE_V8_LCG_SEED;

/* bring-up: which elements are in (section 39). Not reset by local_reset() -- it
 * is a bench setting, and losing it on every enable would defeat the point. */
static uint8_t  s_stage       = ENGINE_V8_STAGE_DEFAULT;
static float    s_noise_en    = 1.0f;   /* STAGE_NOISE as a per-sample factor */
static uint16_t s_pot_held    = 0u;
static bool     s_pot_seeded  = false;

/* bring-up telemetry, accumulated at block rate, printed and cleared by
 * app_engine_synth_report(). The POT range is the evidence for the deadband
 * width; the gate mean is the evidence for whether the mask is stuck open. */
static uint16_t s_dbg_pot_min = 0xFFFFu;
static uint16_t s_dbg_pot_max = 0u;
static uint32_t s_dbg_blocks  = 0u;
static float    s_dbg_gate_sum = 0.0f;
static float    s_dbg_gate_max = 0.0f;


//===========================================================
// Local Function
//===========================================================

static inline uint32_t local_lcg_u32(void)
{
    s_rng = (1664525uL * s_rng) + 1013904223uL;
    return s_rng;
}

/* Uniform in [-0.5, 0.5). The top bits are the good ones in an LCG. */
static inline float local_lcg_unit(void)
{
    return ((float)(local_lcg_u32() >> 8) * (1.0f / 16777216.0f)) - 0.5f;
}

/* Three uniforms summed: unit variance, and bounded at +-3 sigma. Standing in
 * for a Gaussian, which is what the prototype drew. */
static inline float local_lcg_tri(void)
{
    return (local_lcg_unit() + local_lcg_unit() + local_lcg_unit()) * 2.0f;
}

/* Linear interpolation into a periodic int16 table of `pts` points (a power of
 * two), at cycle phase `ph` in [0,1). `base` is the table's start index. */
static inline float local_read(const int16_t* t, uint16_t base, uint16_t pts,
                               float ph)
{
    float    x  = ph * (float)pts;
    uint16_t i0 = (uint16_t)x;
    float    f  = x - (float)i0;
    uint16_t i1 = (uint16_t)((i0 + 1u) & (uint16_t)(pts - 1u));

    return (float)t[base + i0] * (1.0f - f) + (float)t[base + i1] * f;
}

/* The same read, Catmull-Rom instead of linear. For the WAVE tables only.
 *
 * Section 45.6. A 256-point cycle read at 48 kHz is an upsample by
 * 48000/(256*f_cyc), and linear interpolation is a poor reconstruction filter: it
 * leaves images clustered around the table rate, 256*rpm/120 Hz. That is 7.3 kHz at
 * 3400 rpm, 8.5 kHz at 3980 and 10.9 kHz at 5100 -- so for exactly the rpm band the
 * owner complained about, the images land where the ear is still sharp, and above
 * ~5500 rpm they move past 11.7 kHz and stop mattering. The images are cycle-locked
 * (the wave is periodic at f_cyc) and they carry the firing pulse, so they arrive as
 * a hard once-per-cycle burst: measured at 3980 rpm, the 6-12 kHz band's
 * cycle-locked envelope was 21.7 dB peak-to-trough while the noise's own
 * contribution there was 4.1 dB. They are pure artefact -- the recording holds
 * nothing above crank order 64 (-31..-56 dB, section 45.2), so there is no real
 * content up there to protect.
 *
 * Measured, cycle-locked envelope peak-to-trough, 6-12 kHz / 1-3 kHz:
 *   3980 rpm  21.68 -> 14.25 dB / 12.16 -> 12.76 dB
 *   4400 rpm  20.52 -> 14.19 dB / 11.09 -> 11.51 dB
 *   5500 rpm  12.68 ->  7.75 dB / 13.98 -> 14.06 dB
 * and the energy above the table's own band limit drops 4.7 dB. The 1-3 kHz figure
 * is the point of the second column: that band holds real harmonics and its pulse
 * is the engine, so it must NOT move, and it does not.
 *
 * Not used for the noise (its images measured 4.1 dB, not worth the taps) nor for
 * the noise envelope (128 smooth points, nothing to reconstruct). */
static inline float local_read_cubic(const int16_t* t, uint16_t base, uint16_t pts,
                                     float ph)
{
    uint16_t m  = (uint16_t)(pts - 1u);
    float    x  = ph * (float)pts;
    uint16_t i1 = (uint16_t)((uint16_t)x & m);
    float    f  = x - (float)(uint16_t)x;
    uint16_t i0 = (uint16_t)((i1 - 1u) & m);
    uint16_t i2 = (uint16_t)((i1 + 1u) & m);
    uint16_t i3 = (uint16_t)((i1 + 2u) & m);

    float p0 = (float)t[base + i0];
    float p1 = (float)t[base + i1];
    float p2 = (float)t[base + i2];
    float p3 = (float)t[base + i3];

    float a = (-0.5f * p0) + (1.5f * p1) - (1.5f * p2) + (0.5f * p3);
    float b = p0 - (2.5f * p1) + (2.0f * p2) - (0.5f * p3);
    float c = (-0.5f * p0) + (0.5f * p2);

    return (((a * f) + b) * f + c) * f + p1;
}

/* The overlap-add weights. NOT local_read(): the table is the RISING QUARTER of
 * the weight, so it is the one table here that is not periodic -- its top
 * neighbour is 1.0, which is off the end of the table and does not fit in the
 * int16 scale either. Read periodically it would interpolate from 0.99996 back
 * down to win[0] = 0 over the last 1/256 of every cycle, i.e. a notch in the noise
 * at the cycle boundary at exactly the rate being synthesised.
 *
 * BOTH weights come from this ONE table, and the pair is POWER-complementary
 * (w_new^2 + w_old^2 == 1), not amplitude-complementary. That is the criterion
 * that applies here: the two noise readers take INDEPENDENT random offsets into
 * the noise table, so they are uncorrelated, and uncorrelated signals add in
 * power. The table used to be a rising Hann half with the caller using
 * `(w, 1 - w)` -- which sums to exactly 1 in AMPLITUDE, and therefore ran the
 * noise POWER from 1.0 at the cycle boundary down to 0.5 mid-cycle: a 3.01 dB dip
 * once per engine cycle, by construction. Measured on the output at a held
 * 4400 rpm, noise only, 3-6 kHz: 4.88 dB peak-to-trough over the cycle, i.e. a
 * 36.7 Hz amplitude modulation of a band that is 98 % noise above ~3700 rpm. That
 * is what the owner heard as churu-churu over 3400-5100 rpm. The table is now
 * sin(pi*ph/2) and the falling weight is cos(pi*ph/2) = sin(pi*(1-ph)/2), i.e.
 * this same table read backwards -- so the fix costs no ROM, only a second
 * interpolation per sample (which is why the old form was chosen).
 *
 * The reversed read is done in the INDEX domain on purpose: calling this with
 * (1 - ph) would evaluate the table at exactly 1.0 whenever s_phase is 0.0 --
 * true at reset, and possible on a cycle wrap -- indexing one past the end.
 *
 * Section 45.5. */
static inline void local_read_win_pair(float ph, float* w_new, float* w_old)
{
    float    x  = ph * (float)ENGINE_V8_WIN_PTS;
    uint16_t i0 = (uint16_t)x;
    float    f  = x - (float)i0;

    /* rising: sin(pi*ph/2), with win[WIN_PTS] read as 1.0 */
    float r0 = (float)g_engine_v8_win[i0] * ENGINE_V8_WIN_SCALE;
    float r1 = ((uint16_t)(i0 + 1u) < ENGINE_V8_WIN_PTS)
             ? (float)g_engine_v8_win[i0 + 1u] * ENGINE_V8_WIN_SCALE
             : 1.0f;

    /* falling: the same table at (1 - ph). In index terms that is
     * (WIN_PTS - i0) - f, so it interpolates from win[WIN_PTS - i0] towards
     * win[WIN_PTS - i0 - 1] by f. WIN_PTS again reads as 1.0; i0 <= WIN_PTS - 1
     * because ph < 1, so (WIN_PTS - i0) >= 1 and the -1 index is in range. */
    uint16_t k  = (uint16_t)(ENGINE_V8_WIN_PTS - i0);
    float    q0 = ( k < ENGINE_V8_WIN_PTS )
                ? (float)g_engine_v8_win[k] * ENGINE_V8_WIN_SCALE
                : 1.0f;
    float    q1 = (float)g_engine_v8_win[k - 1u] * ENGINE_V8_WIN_SCALE;

    *w_new = r0 + (r1 - r0) * f;
    *w_old = q0 + (q1 - q0) * f;
}

/* The shared noise table is read at an arbitrary offset, so the wrap is over the
 * whole table and not over one cycle. */
static inline float local_read_noise(uint16_t off, float ph)
{
    float    x  = ph * (float)ENGINE_V8_NOISE_PTS;
    uint16_t i0 = (uint16_t)x;
    float    f  = x - (float)i0;
    uint16_t a  = (uint16_t)((off + i0) & (ENGINE_V8_NOISE_LEN - 1u));
    uint16_t b  = (uint16_t)((a + 1u) & (ENGINE_V8_NOISE_LEN - 1u));

    return (float)g_engine_v8_noise[a] * (1.0f - f)
         + (float)g_engine_v8_noise[b] * f;
}

/* Pick the RPM bin pair and everything else that is constant over one cycle. */
static void local_pick_bin(float rpm)
{
    uint16_t j;
    float    r = rpm;

    if( r < g_engine_v8_rpm[0] )                  { r = g_engine_v8_rpm[0]; }
    if( r > g_engine_v8_rpm[ENGINE_V8_BINS - 1] ) { r = g_engine_v8_rpm[ENGINE_V8_BINS - 1]; }

    for( j = 0u; j < (ENGINE_V8_BINS - 2u); j++ )
    {
        if( r < g_engine_v8_rpm[j + 1u] ) { break; }
    }

    s_bin_a = (r - g_engine_v8_rpm[j])
            / (g_engine_v8_rpm[j + 1u] - g_engine_v8_rpm[j]);

    s_wave_off_lo = g_engine_v8_wave_off[j];
    s_wave_off_hi = g_engine_v8_wave_off[j + 1u];
    s_wave_pts_lo = g_engine_v8_wave_pts[j];
    s_wave_pts_hi = g_engine_v8_wave_pts[j + 1u];

    s_wave_gain  = ((1.0f - s_bin_a) * g_engine_v8_gain[j]
                    + s_bin_a * g_engine_v8_gain[j + 1u]);
    s_noise_gain = s_wave_gain
                 * ((1.0f - s_bin_a) * g_engine_v8_noise_rms[j]
                    + s_bin_a * g_engine_v8_noise_rms[j + 1u])
                 * ENGINE_V8_NOISE_NORM;
}

static void local_cycle_boundary(void)
{
    float d_next = ENGINE_V8_JITTER * local_lcg_tri();

    /* The draw happens either way, so that switching the element off does not
     * shift the random stream: an A/B has to differ by the element alone. */
    if( (s_stage & ENGINE_V8_STAGE_JITTER) == 0u ) { d_next = 0.0f; }

    /* duration scale of the cycle that starts here */
    s_jit_scale = 1.0f / (1.0f + d_next - s_jit_cur);
    s_jit_cur   = d_next;

    /* the reader that has just finished its two cycles is restarted elsewhere in
     * the table; the one that was new becomes the old one */
    s_noise_off_old = s_noise_off_new;
    s_noise_off_new = (uint16_t)(local_lcg_u32() & (ENGINE_V8_NOISE_LEN - 1u));

    local_pick_bin(s_rpm);
}

/* The measured flare, as a piecewise-linear curve. Returns the commanded rpm. */
static float local_blip_update(float base)
{
    float rpm = base;
    float u;

    switch( s_blip_phase )
    {
    case 1u:   /* rise: base -> peak */
        s_blip_t += ENGINE_V8_DT_S;
        u = s_blip_t / ENGINE_V8_BLIP_UP_S;
        if( u >= 1.0f )
        {
            u            = 1.0f;
            s_blip_phase = 2u;
            s_blip_t     = 0.0f;
        }
        rpm = s_blip_from + (s_blip_peak - s_blip_from) * u;
        break;

    case 2u:   /* fall: peak -> base * 1.10 -> base */
    {
        float knot_rpm = base * ENGINE_V8_BLIP_KNOT_RPM_MULT;
        float t_knot   = ENGINE_V8_BLIP_DN_S * ENGINE_V8_BLIP_KNOT;

        s_blip_t += ENGINE_V8_DT_S;
        if( s_blip_t < t_knot )
        {
            u   = s_blip_t / t_knot;
            rpm = s_blip_peak + (knot_rpm - s_blip_peak) * u;
        }
        else if( s_blip_t < ENGINE_V8_BLIP_DN_S )
        {
            u   = (s_blip_t - t_knot) / (ENGINE_V8_BLIP_DN_S - t_knot);
            rpm = knot_rpm + (base - knot_rpm) * u;
        }
        else
        {
            s_blip_phase = 0u;
            s_blip_t     = 0.0f;
        }
        break;
    }

    default:
        break;
    }

    return rpm;
}

static void local_update_block(void)
{
    uint16_t pot = POT_Read();                      /* 0x0000..0x0FFF */
    float    tgt;
    float    dmax = ENGINE_V8_POT_SLEW_PER_S * ENGINE_V8_DT_S;
    float    slew;
    float    g;

    /* Raw, before the deadband: this is what app_engine_synth_report() shows, and
     * the number that says whether the deadband is wide enough. */
    if( pot < s_dbg_pot_min ) { s_dbg_pot_min = pot; }
    if( pot > s_dbg_pot_max ) { s_dbg_pot_max = pot; }
    if( !s_pot_seeded )       { s_pot_held = pot; s_pot_seeded = true; }

    if( (s_stage & ENGINE_V8_STAGE_POTFILT) != 0u )
    {
        /* Hysteresis, not a low-pass. The held value has to be able to stand
         * perfectly still, because the mask gate differentiates it over a single
         * 0.667 ms block: a one-pole leaves a residue, and a residue of one LSB
         * is already 2190 rpm/s against ENGINE_V8_MASK_SCALE = 1200. Tracking
         * with a deadband-wide dead zone (rather than stepping in deadband-sized
         * jumps) keeps the throttle continuous while the knob is being turned. */
        if( pot > (uint16_t)(s_pot_held + ENGINE_V8_POT_DEADBAND) )
        {
            s_pot_held = (uint16_t)(pot - ENGINE_V8_POT_DEADBAND);
        }
        else if( (uint16_t)(pot + ENGINE_V8_POT_DEADBAND) < s_pot_held )
        {
            s_pot_held = (uint16_t)(pot + ENGINE_V8_POT_DEADBAND);
        }
    }
    else
    {
        s_pot_held = pot;
    }

    if( (s_stage & ENGINE_V8_STAGE_POT) != 0u )
    {
        tgt = ENGINE_V8_IDLE_RPM
            + (float)s_pot_held * ((ENGINE_V8_MAX_RPM - ENGINE_V8_IDLE_RPM)
                                   / ENGINE_V8_POT_FULL_SCALE);
    }
    else
    {
        tgt = ENGINE_V8_IDLE_RPM;   /* stage NONE: the knob is out of the loop */
    }

    if     ( tgt > s_base_rpm + dmax ) { s_base_rpm += dmax; }
    else if( tgt < s_base_rpm - dmax ) { s_base_rpm -= dmax; }
    else                               { s_base_rpm  = tgt;  }

    if( s_blip_start )
    {
        s_blip_from  = s_base_rpm;
        s_blip_peak  = ENGINE_V8_BLIP_PEAK_RPM;
        if( s_blip_peak < s_base_rpm + ENGINE_V8_BLIP_MIN_STEP_RPM )
        {
            s_blip_peak = s_base_rpm + ENGINE_V8_BLIP_MIN_STEP_RPM;
        }
        s_blip_phase = 1u;
        s_blip_t     = 0.0f;
        s_blip_start = false;
    }

    s_cmd_rpm = local_blip_update(s_base_rpm);

    /* The mask gate reads the COMMANDED speed, before the drift is added. Read
     * after it, the drift's own block-to-block step (~0.46 rpm in 0.67 ms = 690
     * rpm/s) would hold the gate at 0.58 forever and mask the settled idle --
     * which is the one thing the design says must stay unmasked. */
    slew       = (s_cmd_rpm - s_prev_cmd) * (1.0f / ENGINE_V8_DT_S);
    s_prev_cmd = s_cmd_rpm;

    g = fabsf(slew) * (1.0f / ENGINE_V8_MASK_SCALE);
    if( g > 1.0f ) { g = 1.0f; }
    if( (s_stage & ENGINE_V8_STAGE_MASK) == 0u ) { g = 0.0f; }
    s_gate = ENGINE_V8_GATE_A * s_gate + (1.0f - ENGINE_V8_GATE_A) * g;

    s_noise_en = ( (s_stage & ENGINE_V8_STAGE_NOISE) != 0u ) ? 1.0f : 0.0f;

    s_dbg_blocks++;
    s_dbg_gate_sum += s_gate;
    if( s_gate > s_dbg_gate_max ) { s_dbg_gate_max = s_gate; }

    s_drift = ENGINE_V8_DRIFT_A * s_drift
            + ENGINE_V8_DRIFT_STEP * local_lcg_tri();
    if     ( s_drift >  ENGINE_V8_DRIFT_CLAMP ) { s_drift =  ENGINE_V8_DRIFT_CLAMP; }
    else if( s_drift < -ENGINE_V8_DRIFT_CLAMP ) { s_drift = -ENGINE_V8_DRIFT_CLAMP; }

    /* The walk above runs either way; only its use is switched. */
    s_rpm = s_cmd_rpm;
    if( (s_stage & ENGINE_V8_STAGE_DRIFT) != 0u ) { s_rpm += s_drift; }
    if( s_rpm < g_engine_v8_rpm[0] ) { s_rpm = g_engine_v8_rpm[0]; }
    if( s_rpm > g_engine_v8_rpm[ENGINE_V8_BINS - 1] )
    {
        s_rpm = g_engine_v8_rpm[ENGINE_V8_BINS - 1];
    }

    /* f_cycle = rpm / 120 (a 4-stroke V8 fires 8 times per 2 revolutions) */
    s_inc = (s_rpm * (1.0f / 120.0f)) * (1.0f / ENGINE_V8_FS_HZ) * s_jit_scale;
}

static float local_next_sample(void)
{
    float wave;
    float n;
    float w;
    float w_old;
    float extra;
    float y;

    /* --- wave: the two neighbouring RPM bins, crossfaded ------------------- */
    wave = s_wave_gain
         * ((1.0f - s_bin_a) * local_read_cubic(g_engine_v8_wave, s_wave_off_lo,
                                                s_wave_pts_lo, s_phase)
            + s_bin_a * local_read_cubic(g_engine_v8_wave, s_wave_off_hi,
                                         s_wave_pts_hi, s_phase))
         * ENGINE_V8_WAVE_SCALE;

    /* --- noise: two readers, one cycle apart, power-complementary weights -- */
    local_read_win_pair(s_phase, &w, &w_old);

    n = (local_read_noise(s_noise_off_new, s_phase) * w
         + local_read_noise((uint16_t)(s_noise_off_old + ENGINE_V8_NOISE_PTS),
                            s_phase) * w_old)
        * ENGINE_V8_NOISE_SCALE
        * (local_read(g_engine_v8_noise_env, 0u, ENGINE_V8_ENV_PTS, s_phase)
           * ENGINE_V8_ENV_SCALE)
        * s_noise_gain
        * s_noise_en;

    /* --- the chord mask: extra noise, low-passed, gated by |d rpm/dt| ------ */
    extra     = ENGINE_V8_MASK_EXTRA * n;
    y         = ENGINE_V8_MASK_B0 * extra + s_mask_z1;
    s_mask_z1 = ENGINE_V8_MASK_B1 * extra + s_mask_z2 - ENGINE_V8_MASK_A1 * y;
    s_mask_z2 = ENGINE_V8_MASK_B2 * extra            - ENGINE_V8_MASK_A2 * y;

    /* --- the cycle oscillator --------------------------------------------- */
    s_phase += s_inc;
    if( s_phase >= 1.0f )
    {
        s_phase -= 1.0f;
        local_cycle_boundary();
        s_inc = (s_rpm * (1.0f / 120.0f)) * (1.0f / ENGINE_V8_FS_HZ) * s_jit_scale;
    }

    return wave + n + s_gate * y;
}

static void local_reset(void)
{
    s_base_rpm    = ENGINE_V8_IDLE_RPM;
    s_cmd_rpm     = ENGINE_V8_IDLE_RPM;
    s_prev_cmd    = ENGINE_V8_IDLE_RPM;
    s_rpm         = ENGINE_V8_IDLE_RPM;
    s_drift       = 0.0f;
    s_gate        = 0.0f;
    s_pos         = ENGINE_V8_BLOCK;

    s_blip_start  = false;
    s_blip_phase  = 0u;
    s_blip_t      = 0.0f;

    s_phase       = 0.0f;
    s_jit_cur     = 0.0f;
    s_jit_scale   = 1.0f;

    s_mask_z1     = 0.0f;
    s_mask_z2     = 0.0f;

    s_pot_seeded   = false;
    s_dbg_pot_min  = 0xFFFFu;
    s_dbg_pot_max  = 0u;
    s_dbg_blocks   = 0u;
    s_dbg_gate_sum = 0.0f;
    s_dbg_gate_max = 0.0f;

    s_rng           = ENGINE_V8_LCG_SEED;
    s_noise_off_old = (uint16_t)(local_lcg_u32() & (ENGINE_V8_NOISE_LEN - 1u));
    s_noise_off_new = (uint16_t)(local_lcg_u32() & (ENGINE_V8_NOISE_LEN - 1u));

    local_pick_bin(s_rpm);
    s_inc = (s_rpm * (1.0f / 120.0f)) * (1.0f / ENGINE_V8_FS_HZ);
}


//===========================================================
// API
//===========================================================

void app_engine_synth_init_48k(void)
{
    /* 48 kHz mono source for fx_domain_48k; the system sample rate is that
     * domain's business, not this model's. */
    s_mix_gain = 0.0f;
    local_reset();
}


float app_engine_synth_process_sample_48k(void)
{
    float y;

    /* Defensive only -- fx_domain_48k checks is_enable() before calling. The state
     * is left alone rather than reset here: app_engine_synth_enable(true) does the
     * reset, so doing it again on every silent sample would be work for nothing. */
    if( s_mix_gain == 0.0f )
    {
        return 0.0f;
    }

    if( s_pos >= ENGINE_V8_BLOCK )
    {
        local_update_block();
        s_pos = 0u;
    }
    s_pos++;

    y = local_next_sample() * ENGINE_V8_OUT_GAIN;

    if     ( y >  ENGINE_V8_OUT_CLAMP ) { y =  ENGINE_V8_OUT_CLAMP; }
    else if( y < -ENGINE_V8_OUT_CLAMP ) { y = -ENGINE_V8_OUT_CLAMP; }

    return s_mix_gain * y;
}


void app_engine_synth_enable( bool enable )
{
    if( enable )
    {
        if( s_mix_gain == 0.0f ) { local_reset(); }
        s_mix_gain = Gain_EngineSynth;
    }
    else
    {
        s_mix_gain = 0.0f;
    }
}


bool app_engine_synth_is_enable( void )
{
    if( s_mix_gain != 0.0f )
    {
        return true;
    }
    return false;
}


void app_engine_synth_blip_start( void )
{
    printf("\n blipping start!!!\n");
    s_blip_start = true;
}


void app_engine_synth_set_stage( uint8_t stage )
{
    s_stage      = (uint8_t)(stage & ENGINE_V8_STAGE_ALL);
    s_pot_seeded = false;   /* re-seed the held value from wherever the knob is */
}


uint8_t app_engine_synth_get_stage( void )
{
    return s_stage;
}


/* Everything printed as an integer on purpose: this runs from the console task,
 * and a %f would pull the whole float formatter in for a bench read-out. */
void app_engine_synth_report( void )
{
    const uint8_t  st = s_stage;
    const uint32_t nb = s_dbg_blocks;
    const uint16_t lo = ( s_dbg_pot_min > s_dbg_pot_max ) ? 0u : s_dbg_pot_min;
    const int      av = (int)( ( ( nb != 0u )
                                 ? ( s_dbg_gate_sum / (float)nb ) : 0.0f )
                               * 1000.0f );
    const int      mx = (int)( s_dbg_gate_max * 1000.0f );

    printf("\n Engine stage %02X [%c%c%c%c%c%c] %s\n",
           (unsigned)st,
           ( st & ENGINE_V8_STAGE_POT     ) ? 'P' : '-',
           ( st & ENGINE_V8_STAGE_POTFILT ) ? 'F' : '-',
           ( st & ENGINE_V8_STAGE_JITTER  ) ? 'J' : '-',
           ( st & ENGINE_V8_STAGE_NOISE   ) ? 'N' : '-',
           ( st & ENGINE_V8_STAGE_DRIFT   ) ? 'D' : '-',
           ( st & ENGINE_V8_STAGE_MASK    ) ? 'M' : '-',
           app_engine_synth_is_enable() ? "on" : "off" );
    printf("  rpm=%4d cmd=%4d  pot raw %4u..%4u held %4u\n",
           (int)s_rpm, (int)s_cmd_rpm,
           (unsigned)lo, (unsigned)s_dbg_pot_max, (unsigned)s_pot_held);
    printf("  gate/1000 mean %4d max %4d over %lu blocks\n",
           av, mx, (unsigned long)nb);

    s_dbg_pot_min  = 0xFFFFu;
    s_dbg_pot_max  = 0u;
    s_dbg_blocks   = 0u;
    s_dbg_gate_sum = 0.0f;
    s_dbg_gate_max = 0.0f;
}


/* Every engine-owned "*cy" subcode that is this model's own. The console does not
 * know what a stage mask is, and must not have to. */
bool app_engine_synth_console_subcode( uint8_t subcode )
{
    if( subcode == 0x07u )
    {
        app_engine_synth_report();
        return true;
    }
    if( ( subcode & 0xC0u ) == 0x40u )   /* 40..7F: the bring-up stage mask */
    {
        app_engine_synth_set_stage( (uint8_t)( subcode & 0x3Fu ) );
        app_engine_synth_report();
        return true;
    }
    return false;
}

/* What is sounding, not what the knob asks for: s_rpm is the commanded speed with
 * the idle drift added, i.e. the speed the wavetables are actually being read at. */
float app_engine_synth_rpm( void )
{
    return app_engine_synth_is_enable() ? s_rpm : 0.0f;
}

#endif //defined(ENA_ENGINE_SYNTH)
