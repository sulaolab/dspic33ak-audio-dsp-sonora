#if defined(ENA_AVAS_TYPE_LB_SYNTH)

#ifndef APP_AVAS_SYNTH_TYPE_LB_H
#define APP_AVAS_SYNTH_TYPE_LB_H

#include <stdbool.h>
#include <stdint.h>

//===========================================================
// This module is a 48 kHz mono source for fx_domain_48k.
// It does not directly process the system ch-major DMA buffer.

// Sound engine: L3 = line model + noise bank.
//   tone(t)  = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])      j = 1..264
//            = sum_k Re{ e^{i 2 pi FC[k] t} * Z_k(t) }         k = 1..7 carriers
//   noise(t) = one tilted white source through a bank of state-variable
//              bandpasses, per-band gains fitted, per-band slow gusts
// Coefficients live in avas_synth_type_lb_tables.h and
// avas_synth_type_lb_noise_tables.h (both generated, do not edit).

// THE TONE HALF IS THE SAME ARITHMETIC AS avas_synth_type_ty.c, and that is the
// whole reason this voice was affordable to add: L3's tone part and the Type_TY
// L1 model are one equation with two coefficient sets.  Only the noise bank is
// new DSP.  If you change the cluster/envelope scheme in one of the two files,
// change it in both -- they are deliberately line-for-line comparable.
//
// This REPLACES the former v07 additive model (16 partials, per-partial AM from
// two LFOs, three `carrier^power` pulse voices, five shared pitch-wander LFOs,
// xorshift noise through a one-pole).  That engine is a different lineage --
// the `type_lb_..._v1..v6` series, not `L1..L8` -- and it is gone rather than
// switchable.  Its gain API (low/core/noise/master) went with
// it: a line model has no `low` and `core` partial groups, so the knobs here
// are tone / noise / master.
//===========================================================

//===========================================================
// Definition
//===========================================================

#include "avas_synth_type_lb_tables.h"
#include "avas_synth_type_lb_noise_tables.h"

/* Envelope decimation: each cluster's complex envelope Z_k is rebuilt once
 * every AVAS_TYPE_LB_DEC samples and linearly interpolated in between, while the
 * carriers run at the full 48 kHz.  Same knob, same units and same measured
 * behaviour as AVAS_TYPE_TY_DEC -- see avas_synth_type_ty.h for the decimation study.
 * It was measured on the Type_TY table, but the quantity it trades (envelope
 * bandwidth against load) is set by the cluster half-span, and this voice's
 * spans are NARROWER (max |f - fc| 60.7 Hz against the Type_TY's 158.9), so
 * D = 32 has more headroom here rather than less.
 *
 * Cost is  AVAS_TYPE_LB_L3_CLUSTERS  full-rate carriers  +
 *          AVAS_TYPE_LB_L3_TABLE_LINES / AVAS_TYPE_LB_DEC  baseband oscillators
 *          + the noise bank's sections, which do NOT decimate.
 *
 * Note which way round the two voices sit: this one has MORE lines (264 vs 185)
 * and FEWER carriers (7 vs 11), because all of its energy is below 1.2 kHz.
 * The carriers are the expensive half, so the TONE part of the Type_LB is
 * cheaper than the Type_TY's despite the longer table.  The noise bank is what
 * puts the total above it.
 *
 * Do NOT trim the line count to buy load: L3 is defined as ALL 264 detected
 * lines (the truncated variants are L4/L5/L8, a different model).  The knobs
 * are this decimation and the noise bank's band count. */
#if !defined(AVAS_TYPE_LB_DEC)
#define AVAS_TYPE_LB_DEC                 (32u)
#endif

#if (AVAS_TYPE_LB_DEC < 2u) || (AVAS_TYPE_LB_DEC > 64u)
#error "AVAS_TYPE_LB_DEC must be 2..64 (envelope bandwidth vs load)"
#endif

/* Normalisation baked into the tone gain: brings the measured peak of
 * tone + noise to 0.9.
 *
 * ON THE SUM, not on the tone.  The two halves are added before this gain and
 * the noise reaches 23 % of the tone's peak; the CK port normalised the tone
 * alone and had to add a saturating output arm once tone+noise was measured to
 * clip.  The generator measures both peaks and emits their sum. */
#define AVAS_TYPE_LB_L3_NORM             (0.9f / AVAS_TYPE_LB_L3_PEAK_ABS)

//===========================================================
// Enum & Struct typedef
//===========================================================

/* ---------------------------------------------------------------------------
 * Run-time pitch trim, driven by the POT while this engine is the sounding one.
 *
 * Deliberately the same clamp and knob range as the TYPE_TY engine (AVAS_TYPE_TY_PITCH_*
 * / AVAS_TYPE_TY_POT_TOP_CENT): the two are runtime-exclusive and share the one knob,
 * so one learned feel has to cover both.  They are separate #defines
 * rather than shared ones because the two voices may legitimately diverge later
 * -- what is shared is the operator's hand, not the tuning.
 *
 * The L3 table is a fixed-pitch coefficient set, so there is no drift of its own
 * to reach, and there is no engine wander to trim against any more either -- the
 * v07 model's five wander LFOs (+-18.2 cent peak against a 55 Hz base) went with
 * that engine, and a line model's pitch is whatever these #defines make it.  So
 * the range is anchored to the OTHER engine's: TYPE_TY sets 200 cent, and one POT
 * has to serve both.
 *
 * HOW IT IS APPLIED -- and this is where L3 differs from the v07 engine that used
 * to be here.  v07 multiplied a base step by pitch_ratio every sample, so a trim
 * was one float write.  A line model has no base step: the carriers and the 264
 * baseband offsets ARE the frequencies.  So a ratio is applied by rebuilding all
 * 271 steps from the const tables (avas_type_lb_set_steps), which is exactly what
 * TYPE_TY does, and for the same reason it must happen at an envelope-rebuild
 * boundary rather than under the caller's hand.  Hence the request/apply handshake
 * in the struct below: latency is at most AVAS_TYPE_LB_DEC samples (0.67 ms), and
 * the cost is 271 multiplies per accepted change against a POT that samples at
 * 10 Hz.  See docs_public/avas_pitch_pot_design.md.
 *
 * The NOISE BANK is not pitched: its band frequencies are fixed band edges, not
 * harmonics of the engine, so the wind does not transpose with the tone.  That is
 * deliberate. */
#if !defined(AVAS_TYPE_LB_PITCH_LIMIT_CENT)
#define AVAS_TYPE_LB_PITCH_LIMIT_CENT  (200.0f)   /* clamp, both directions */
#endif
/* Fully counter-clockwise = this engine's own pitch, clockwise raises it to
 * +this.  Same mapping and same value as TYPE_TY -- one knob, one learned feel. */
#if !defined(AVAS_TYPE_LB_POT_TOP_CENT)
#define AVAS_TYPE_LB_POT_TOP_CENT      (200.0f)
#endif

typedef struct avas_synth_type_lb_s avas_synth_type_lb_t;

struct avas_synth_type_lb_s
{
    float fs;    // internal float sample rate used by oscillator/gate math

    /* Baseband oscillators, one per line, running at fs / AVAS_TYPE_LB_DEC.
     * Split into two arrays rather than one struct-of-2: the envelope rebuild
     * touches phase (read/write) and step (read) only, and the amplitude is
     * read straight out of the const table. */
    float bb_phase[AVAS_TYPE_LB_L3_TABLE_LINES];   // 0..2pi
    float bb_step[AVAS_TYPE_LB_L3_TABLE_LINES];    // 2*pi*(f-fc)*AVAS_TYPE_LB_DEC/fs

    /* Full-rate carriers, one per cluster. */
    float car_phase[AVAS_TYPE_LB_L3_CLUSTERS];     // 0..2pi
    float car_step[AVAS_TYPE_LB_L3_CLUSTERS];      // 2*pi*fc/fs

    /* Current complex envelope and its per-sample slope towards the next
     * rebuild.  env_* reaches the newly built value exactly at the next
     * rebuild, which is why the rebuild computes the envelope one block AHEAD:
     * interpolating towards a value already reached delays the envelope by a
     * whole block, and that delay alone cost ~33 dB of accuracy when it was
     * measured on the Type_TY voice. */
    float env_i[AVAS_TYPE_LB_L3_CLUSTERS];
    float env_q[AVAS_TYPE_LB_L3_CLUSTERS];
    float env_di[AVAS_TYPE_LB_L3_CLUSTERS];
    float env_dq[AVAS_TYPE_LB_L3_CLUSTERS];

    /* The noise bank.  `nb_low`/`nb_band` are the first SVF section's state,
     * one per band; `nb_low2`/`nb_band2` are the second section's, which only
     * the 4th-order bands have.  Those are the TOP bands by construction (the
     * order rule is a frequency cutoff and the bands are frequency-ascending),
     * so one index separates them and the per-sample loops stay branch-free. */
    float nb_low[AVAS_TYPE_LB_L3_NOISE_BANDS];
    float nb_band[AVAS_TYPE_LB_L3_NOISE_BANDS];
#if (AVAS_TYPE_LB_L3_NOISE_BANDS4 > 0u)
    float nb_low2[AVAS_TYPE_LB_L3_NOISE_BANDS4];
    float nb_band2[AVAS_TYPE_LB_L3_NOISE_BANDS4];
#endif
    /* The fitted gain with this block's gust folded in, so the gust costs
     * nothing per sample: it is the same coefficient the bank already
     * multiplies by. */
    float nb_gain[AVAS_TYPE_LB_L3_NOISE_BANDS];
    float nb_walk[AVAS_TYPE_LB_L3_NOISE_BANDS];    // unit-sd random walk, clamped

    /* The COMMON walk, shared by every band, mixed in per
     * avas_synth_type_lb_set_gust_corr().  Only advanced while the correlation is
     * non-zero, which is what keeps corr = 0 bit-identical to the independent
     * model: an extra draw would shift the per-band random sequence. */
    float nb_walk_c;
#if (AVAS_TYPE_LB_L3_NOISE_TILT_POLES > 0u)
    float nb_tilt[AVAS_TYPE_LB_L3_NOISE_TILT_POLES];
#endif
    uint32_t nb_rng;     // white source
    uint32_t nb_grng;    // gust source, separate so the two never interleave

    /* The gust pole and its drive.  Runtime rather than the table's constants
     * because the FLUCTUATION RATE is an A/B question by ear (see
     * avas_synth_type_lb_set_gust_hz); they init to the generated values exactly. */
    float nb_gust_a;
    float nb_gust_drive;

    /* The gust DEPTH (as the gain law's K = ln(10)/20 * depth_dB) and the
     * band-to-band CORRELATION, with sqrt(1-c^2) precomputed so the per-band mix
     * is two multiplies.  Runtime for the same reason as the rate: what the ear
     * calls "how slowly the wind breathes" turned out to be these two axes and
     * not the rate.  They init to the table's own depth and to INDEPENDENT. */
    float nb_gust_k;
    float nb_gust_corr;
    float nb_gust_ind;   // sqrt(1 - corr^2)

    uint16_t dec_count;  // samples left before the next control-rate update

    /* Pitch trim.  pitch_ratio is what the step tables currently hold;
     * pitch_ratio_req is written by the caller's context (main loop, POT sampler)
     * and picked up by the render context at the next envelope rebuild, so the
     * tables are never rewritten while a sample is being computed from them.
     * Latency is at most AVAS_TYPE_LB_DEC samples (0.67 ms at 48 kHz / D = 32).
     *
     * The flag is cleared BEFORE the request is read, and applying a ratio is
     * idempotent (the steps are rebuilt from the const tables, not scaled in
     * place), so a request landing inside the handshake is either seen now or seen
     * at the next rebuild -- it cannot be lost, and re-applying the same value
     * changes nothing.  Identical mechanism to the TYPE_TY engine's. */
    float pitch_ratio;
    float pitch_ratio_req;
    volatile uint8_t pitch_req_pending;

    /* Same request, but "apply only once nothing is being rendered".  Stopping the
     * engine resets the trim, and that reset must NOT be heard: after gate_off the
     * release fade is still running, and re-pitching it mid-fade is an audible
     * slide on the way out.  So the stop marks the reset here and the
     * fully-gated-off early-out applies it, where by definition nothing can be
     * heard. */
    volatile uint8_t pitch_req_on_silence;

    float tone_gain;     // AVAS_TYPE_LB_L3_NORM * user tone gain
    float noise_gain;    // 1.0 = the measured noise/tone ratio of -17.85 dB
    float master_gain;

    /* PER-CLUSTER trim, 1.0 = the coefficient set's level.  Applied to the rebuilt
     * envelope at the control rate, so it costs 2 multiplies per cluster per
     * AVAS_TYPE_LB_DEC samples and NOTHING per sample, and a change reaches the
     * carriers as a one-block interpolation ramp rather than a step -- it cannot
     * click.  Not a mix knob either (see the set_cluster_gain_db note); it exists
     * because the 7 clusters are 7 audibly distinct sounds and "which of them is
     * the one being masked" is a question only the ear can answer. */
    float cl_gain[AVAS_TYPE_LB_L3_CLUSTERS];

    float gate;
    float gate_target;
    float gate_attack_alpha;
    float gate_release_alpha;
};

//===========================================================
// Function Prototype
//===========================================================

extern void  avas_synth_type_lb_init(avas_synth_type_lb_t *s, float fs);
extern float avas_synth_type_lb_process_sample(avas_synth_type_lb_t *s);

/* 0 dB = the coefficient set's own level, i.e. tone+noise peak normalised to
 * 0.9 with the noise 17.85 dB below the tone.  THE NOISE GAIN IS NOT A MIX
 * KNOB: NG[b] and the tone's AMP[] are in the same units, so that ratio comes
 * out of the coefficient set rather than being chosen.  The knob exists so the
 * noise half can be A/B'd by ear.
 *
 * THE SHIPPED DEFAULT IS NOT 0 dB: the image defaults to -8.0 dB of wind
 * (AVAS_TYPE_LB_NOISE_GAIN_DB).  0 dB stays reachable from the console at any
 * time.
 *
 * AND THE RIGHT VALUE DEPENDS ON THE PLAYBACK LEVEL -- a value chosen at a
 * partial amplifier volume comes out far too loud at the final one.  See
 * AVAS_TYPE_LB_NOISE_GAIN_DB for why the loud end decides.  Any wind A/B whose
 * result is going to be written into the image has to be run at full volume. */
extern void avas_synth_type_lb_set_tone_gain_db(avas_synth_type_lb_t *s, float db);
extern void avas_synth_type_lb_set_noise_gain_db(avas_synth_type_lb_t *s, float db);
extern void avas_synth_type_lb_set_master_gain_db(avas_synth_type_lb_t *s, float db);

/* ONE CLUSTER's level, 0 dB = the coefficient set's level.  `k` selects the cluster,
 * AVAS_TYPE_LB_CLUSTER_ALL selects all of them (which is then the same axis as the
 * tone gain above, reachable from the same console key).
 *
 * WHY A PER-CLUSTER KNOB EXISTS.  The 7 carriers are not a spectral slicing of
 * one sound, they are the sound's separate components: cluster 0 alone is
 * 85.31 % of the line energy, 50 lines packed into +-48 Hz around 66.82 Hz, and
 * what 50 nearly-coincident lines make is a slow beat -- a long breath, audibly
 * distinct from the noise bank's rumble.  Those two are
 * also the pair that OVERLAP: the noise bands at 24/35/51/73/106 Hz sit on top
 * of cluster 0's span, so a wind trim that helped the noise can bury the tone
 * component underneath it.  Deciding that by ear needs the two movable
 * separately, which the tone/noise pair cannot do -- the tone gain would move
 * all 7 clusters and change the engine's character, not just the breath. */
#define AVAS_TYPE_LB_CLUSTER_ALL         (0xFFu)
/* No getter, deliberately: the app layer keeps the dB it was asked for (the same
 * shape as the tone/noise gains, whose engine fields are also write-only), and
 * reading it back out of a linear gain would only re-introduce rounding. */
extern void  avas_synth_type_lb_set_cluster_gain_db(avas_synth_type_lb_t *s,
                                                  uint8_t k, float db);

/* The gust RATE, in Hz, with the DEPTH held at the model's 1.5 dB sd.  The drive
 * is a function of the pole (see the definition), so a rate change that did not
 * rescale it would also change the depth -- these two are the axes a listening
 * test must be able to move one at a time.  1.2 Hz is the coefficient set's value
 * and the default; the table's constants are used verbatim until this is called. */
extern void  avas_synth_type_lb_set_gust_hz(avas_synth_type_lb_t *s, float hz);
extern float avas_synth_type_lb_get_gust_hz(const avas_synth_type_lb_t *s);

/* The gust DEPTH, in dB sd per band.  1.5 dB is the model's value; the table's
 * K = ln(10)/20 * 1.5 exactly, so the default is the shipped constant.
 *
 * BEWARE ABOVE ~3 dB: the gain law is `g *= 1 + K*w`, the FIRST-ORDER 10^(x/20),
 * and it breaks asymmetrically -- at K*w = 6 dB the true factor is 2.0 against
 * the approximation's 1.69, and the negative side reaches zero (silence, see the
 * guard in avas_type_lb_noise_update_gusts) at -8.7 dB.  The ear hears that as
 * pumping.  If a deep value wins the audition, replace the law with an exact
 * exponential before shipping it, and note in the design doc why the "no pow per
 * band per block" decision was overturned. */
extern void  avas_synth_type_lb_set_gust_depth_db(avas_synth_type_lb_t *s, float db);
extern float avas_synth_type_lb_get_gust_depth_db(const avas_synth_type_lb_t *s);

/* The band-to-band CORRELATION of the gust, 0 = independent (the model) .. 1 =
 * every band breathing as one.
 *
 * WHY THIS AXIS EXISTS: with 12 independent walks the TOTAL level barely moves,
 * however deep each band's own gust is, because the bands partly cancel.  So a
 * "the wind should swell more slowly" verdict cannot be reached with the rate
 * knob -- measured on hardware, even 25.5 Hz was only faintly audible.  A shared
 * component is what makes a swell exist at all.
 *
 *     w_b = sqrt(1-c^2) * w_ind_b + c * w_common
 *
 * mixes two unit-sd independent walks, so the result is unit sd for ANY c: the
 * correlation moves without the depth moving with it, the same orthogonality
 * set_gust_hz() maintains against the drive.  At c = 0 the common walk is not
 * even advanced, so the independent model is reproduced bit-for-bit. */
extern void  avas_synth_type_lb_set_gust_corr(avas_synth_type_lb_t *s, float corr);
extern float avas_synth_type_lb_get_gust_corr(const avas_synth_type_lb_t *s);

/* Restart every oscillator from its table phase, and the noise bank from its
 * seed.  The phases are only mutually meaningful at t = 0 of the model, so this
 * is how the synth is put back into the exact state the coefficient set
 * describes. */
extern void avas_synth_type_lb_reset_phase(avas_synth_type_lb_t *s);

extern void avas_synth_type_lb_gate_on(avas_synth_type_lb_t *s);
extern void avas_synth_type_lb_gate_off(avas_synth_type_lb_t *s);

/* Request a pitch ratio (1.0 = the table's own pitch).  Cheap and callable from
 * any context: it stores the value and returns; the render context rebuilds the
 * step tables at the next envelope rebuild.  Clamped to
 * +-AVAS_TYPE_LB_PITCH_LIMIT_CENT. */
extern void avas_synth_type_lb_request_pitch_ratio(avas_synth_type_lb_t *s, float ratio);

//===========================================================
// API
//===========================================================

extern void  app_avas_type_lb_init_48k(void);
extern float app_avas_type_lb_process_sample_48k(void);
extern void  app_avas_type_lb_set_enable(bool enable);

/* True while this engine renders anything, release fade included.  The two AVAS
 * sources are exclusive at run time because their loads would add up. */
extern bool  app_avas_type_lb_is_active(void);

/* Wind level, live, for the A/B the noise gain exists for (see its note above).
 * 0 dB is the coefficient set's ratio; the trim is limited to +-12 dB because it
 * is a question, not a mixer.  It survives the 'A' key being toggled, and it is
 * NOT persisted -- a reset returns to AVAS_TYPE_LB_NOISE_GAIN_DB, which ships at
 * -8.0 dB, so a reset does NOT return to the coefficient set's level.
 *
 * app_avas_type_lb_noise_headroom_db() returns the trim at which the conservative
 * (arithmetic) peak sum reaches the output clamp.  Above it the clamp starts
 * working, which the ear reads as the wind stopping getting louder rather than
 * as distortion, so it is worth knowing before dialling. */
extern void  app_avas_type_lb_set_noise_gain_db(float db);
extern float app_avas_type_lb_get_noise_gain_db(void);
extern float app_avas_type_lb_noise_headroom_db(void);

/* Gust rate, live, same purpose and same caveat: 1.2 Hz is the table's value,
 * and this is how "the original's wind moves more slowly than this" gets turned
 * into a number instead of an adjective. */
extern void  app_avas_type_lb_set_gust_hz(float hz);
extern float app_avas_type_lb_get_gust_hz(void);

/* Gust depth and band-to-band correlation, live.  These are the two axes the
 * "the original's wind breathes more slowly" verdict actually needs (see their
 * engine-level notes above); neither is persisted, so a reset returns to the
 * model's 1.5 dB and to independent bands.
 *
 * app_avas_type_lb_gust_depth_warn_db() is the depth above which the first-order
 * gain law starts audibly misbehaving -- printed with the value rather than
 * enforced, because a value past it is a legitimate experiment as long as the law
 * gets replaced before it is shipped. */
extern void  app_avas_type_lb_set_gust_depth_db(float db);
extern float app_avas_type_lb_get_gust_depth_db(void);
extern float app_avas_type_lb_gust_depth_warn_db(void);

extern void  app_avas_type_lb_set_gust_corr(float corr);
extern float app_avas_type_lb_get_gust_corr(void);

/* Per-cluster tone level, live, for the "is the breath being masked by the wind"
 * question (see the engine-level note).  Same rules as the wind trim: 0 dB is the
 * coefficient set's level and the value to come back to, it survives 'A' being
 * toggled, and it is NOT persisted.
 *
 * The range is ASYMMETRIC, -24 .. +12 dB, and the asymmetry is the useful half:
 * app_avas_type_lb_cluster_headroom_db() is only about +1.1 dB, because the tone is
 * 84 % of the measured peak and the output already sits at 0.9 of full scale.  So
 * the way to make one cluster stand out is mostly to DUCK the other six, not to
 * raise it -- hence 24 dB of down and only 12 of up.  The headroom figure is the
 * conservative (arithmetic peak sum) bound for raising ALL clusters; raising a
 * single one cannot exceed it, so it is a safe number to dial against, and being
 * conservative it can be passed a little without the clamp actually working.
 *
 * app_avas_type_lb_get_cluster_carrier_hz() reports the carrier of each cluster, so
 * the console can name what it is about to move instead of an index. */
extern void  app_avas_type_lb_set_cluster_gain_db(uint8_t k, float db);
extern float app_avas_type_lb_get_cluster_gain_db(uint8_t k);
extern float app_avas_type_lb_get_cluster_carrier_hz(uint8_t k);
extern float app_avas_type_lb_cluster_headroom_db(void);

/* Pitch trim in cent, relative to the engine's own tuning.  Absolute set (not a
 * delta), clamped to +-AVAS_TYPE_LB_PITCH_LIMIT_CENT.
 *
 * Reset to 0 on stop -- a session must not inherit the previous one's detune --
 * and that reset is DEFERRED to silence: the getter reports 0 immediately, while
 * the step tables follow once the release fade has gone quiet, so the fading tail
 * keeps the pitch it was sounding at.  Same contract as app_avas_type_ty_*. */
extern void  app_avas_type_lb_set_pitch_cent(float cent);
extern float app_avas_type_lb_get_pitch_cent(void);
extern float app_avas_type_lb_get_pitch_ratio(void);

#endif /* APP_AVAS_SYNTH_TYPE_LB_H */
#endif /* ENA_AVAS_TYPE_LB_SYNTH */
