#if defined(ENA_AVAS_TYPE_TY_SYNTH)

#ifndef _APP_AVAS_TYPE_TY_H
#define _APP_AVAS_TYPE_TY_H


//===========================================================
// This module is a 48 kHz mono source for fx_domain_48k.
 // It does not directly process the system ch-major DMA buffer.

 // Sound engine: L1 line model, synthesised as cluster carriers with a
 // decimated complex envelope (see avas_synth_type_ty.c header comment).
 //   y(t) = sum_j AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])
 //        = sum_k Re{ e^{i 2 pi FC[k] t} * Z_k(t) }
 // Coefficients live in avas_synth_type_ty_tables.h (generated, do not edit).


//===========================================================
// Definition
//===========================================================

#include "avas_synth_type_ty_tables.h"

/* Envelope decimation: each cluster's complex envelope Z_k is rebuilt once every
 * AVAS_TYPE_TY_DEC samples and linearly interpolated in between, while the carriers
 * run at the full 48 kHz.  This is the only load knob that matters.
 *
 * Cost is  AVAS_TYPE_TY_L1_CLUSTERS  full-rate carriers  +
 *          AVAS_TYPE_TY_L1_TABLE_LINES / AVAS_TYPE_TY_DEC  baseband oscillators per sample.
 *
 * Measured (float32, same parabolic sine as the firmware, 2 s of signal, error
 * expressed as level of the 13 bands that actually contain lines):
 *      D =  8   54.0 dB below signal   band error mean 0.004 dB
 *      D = 16   53.1 dB                band error mean 0.008 dB
 *      D = 32   48.0 dB                band error mean 0.041 dB   <- default
 *      D = 64   36.5 dB                band error mean 0.184 dB   (too coarse)
 *   185 full-rate oscillators (the direct engine this replaced, 137 % of the
 *   per-sample budget):  49.2 dB, band error mean 0.003 dB.
 * So D = 32 is quality parity with the direct engine at ~1/7 of the cost.
 *
 * Measured on hardware (TDMsum, 32-sample block): AVAS costs 304.8 us of the
 * 666.6 us window = 45.8 %, i.e. ~1900 cycles per sample -- 2.4x the ~790
 * instructions counted from the assembly, because float ops are not 1 cycle.
 * It no longer overflows (margin 180.5 us, miss 0), but the split is carriers
 * ~31 % / envelope ~14 %, so RAISING THIS KNOB BARELY HELPS ANY MORE: D = 64
 * would save ~7 % and cost 48.0 -> 36.5 dB.  What is left to win is the 22
 * fast sin/cos calls per sample in the carrier loop (a per-carrier rotator, i.e.
 * a complex multiply, replaces them).  See docs_public/avas_type_ty_l1_line_model.md.
 *
 * Do NOT trim the line count instead.  Measured: keeping the strongest 32 lines
 * silences 7 of the 13 occupied bands (mean band error 64 dB), and even an
 * optimal per-band quota still leaves 5.2 dB mean / 8.8 dB max.  The clustering
 * costs 0.04 dB.  Override from app_specific_config_defs.h or a build -Define. */
#if !defined(AVAS_TYPE_TY_DEC)
#define AVAS_TYPE_TY_DEC                    (32u)
#endif

#if (AVAS_TYPE_TY_DEC < 2u) || (AVAS_TYPE_TY_DEC > 64u)
#error "AVAS_TYPE_TY_DEC must be 2..64 (envelope bandwidth vs load)"
#endif

/* ---------------------------------------------------------------------------
 * Run-time pitch trim, driven by the POT while this engine is the sounding one
 * (classic_controls.c).  There is no hotkey: an absolute knob and incremental
 * keys writing one value needs takeover arbitration, for a second way to do the
 * same thing.
 *
 * The L1 table is a fixed-pitch coefficient set, so the engine has no pitch of
 * its own to follow.  The trim exists to place that fixed pitch by ear over
 * roughly half a semitone of range, NOT to transpose the model; the numbers
 * below are sized for that use.
 *
 * A ratio r multiplies EVERY line's frequency, carriers and baseband offsets
 * alike, so it is an exact pitch shift of the whole model (see
 * avas_type_ty_set_steps()).  Because the steps are rebuilt from the const table
 * rather than scaled in place, applying a ratio is idempotent -- which is what
 * makes the request/apply handshake below race-free.
 *
 * Accuracy cost of the range: r scales each cluster's baseband span, and the
 * envelope's linear-interpolation error grows roughly with r^2.  At the +200 cent
 * top (r = 1.1225) that is +26 %, i.e. about 1 dB off the measured 48.0 dB at
 * AVAS_TYPE_TY_DEC = 32 -- irrelevant.  A transposition of an octave (r = 2, 4x the
 * error, 6 dB) is where this stops being free, and AVAS_TYPE_TY_DEC would have to come
 * down to pay for it. */
#if !defined(AVAS_TYPE_TY_PITCH_LIMIT_CENT)
#define AVAS_TYPE_TY_PITCH_LIMIT_CENT       (200.0f)   /* clamp, both directions */
#endif

/* POT mapping: fully COUNTER-clockwise (0) is the engine's own pitch, and turning
 * clockwise raises it to +this at full travel.  Unipolar, not centred -- the
 * reference pitch then sits at a mechanical end stop, so it is reachable by feel
 * without a centre detent, and the whole travel is useful range.
 *
 * That end is also below ENG_SYNTH_POT_ACTIVE_VAL, i.e. the engine-synth OFF
 * zone, which is consistent: the knob's rest position means "nothing extra".
 * See docs_public/avas_pitch_pot_design.md. */
#if !defined(AVAS_TYPE_TY_POT_TOP_CENT)
#define AVAS_TYPE_TY_POT_TOP_CENT           (200.0f)
#endif

/* Normalisation baked into the tone gain: brings the measured 60 s peak of the
 * full sum to 0.9, which is the coefficient set's own reference level.  The peak
 * is taken over 60 s and not over a short window: the sum is quasi-periodic, so
 * its peak keeps growing outside any short window and a short-window
 * normalisation clips once the engine is left running. */
#define AVAS_TYPE_TY_L1_NORM                (0.9f / AVAS_TYPE_TY_L1_PEAK_ABS)

//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct avas_type_ty_synth_s avas_type_ty_synth_t;

struct avas_type_ty_synth_s
{
    float fs;    // internal float sample rate used by oscillator/gate math

    /* Baseband oscillators, one per line, running at fs / AVAS_TYPE_TY_DEC.  Split
     * into two arrays rather than one struct-of-2: the envelope rebuild touches
     * phase (read/write) and step (read) only, and the amplitude is read
     * straight out of the const table. */
    float bb_phase[AVAS_TYPE_TY_L1_TABLE_LINES];   // 0..2pi
    float bb_step[AVAS_TYPE_TY_L1_TABLE_LINES];    // 2*pi*(f - fc)*AVAS_TYPE_TY_DEC/fs, signed

    /* Full-rate carriers, one per cluster. */
    float car_phase[AVAS_TYPE_TY_L1_CLUSTERS];     // 0..2pi
    float car_step[AVAS_TYPE_TY_L1_CLUSTERS];      // 2*pi*fc/fs

    /* Current complex envelope and its per-sample slope towards the next
     * rebuild.  env_* reaches the newly built value exactly at the next
     * rebuild, which is why the rebuild computes the envelope one block AHEAD:
     * interpolating towards a value already reached delays the envelope by a
     * whole block and that delay alone costs ~33 dB of accuracy (measured:
     * 48.0 dB -> 15.3 dB below signal at D = 32). */
    float env_i[AVAS_TYPE_TY_L1_CLUSTERS];
    float env_q[AVAS_TYPE_TY_L1_CLUSTERS];
    float env_di[AVAS_TYPE_TY_L1_CLUSTERS];
    float env_dq[AVAS_TYPE_TY_L1_CLUSTERS];

    uint16_t dec_count;  // samples left before the next envelope rebuild

    /* Pitch trim.  pitch_ratio is what the step tables currently hold;
     * pitch_ratio_req is written by the caller's context (main loop) and picked
     * up by the render context at the next envelope rebuild, so the tables are
     * never rewritten while a sample is being computed from them.  Latency is
     * at most AVAS_TYPE_TY_DEC samples (0.67 ms at 48 kHz / D = 32).
     *
     * The flag is cleared BEFORE the request is read, and applying a ratio is
     * idempotent, so a request landing inside the handshake is either seen now
     * or seen at the next rebuild -- it cannot be lost, and re-applying the same
     * value changes nothing. */
    float pitch_ratio;
    float pitch_ratio_req;
    volatile uint8_t pitch_req_pending;

    /* Same request, but "apply only once nothing is being rendered".  Stopping
     * the engine resets the trim (a session must not inherit the previous one's
     * detune), and that reset must NOT be heard: after gate_off the release fade
     * is still running for up to ~2.9 s, and re-pitching it mid-fade is an
     * audible slide on the way out.  So the stop marks the reset here and the
     * fully-gated-off early-out applies it, where by definition nothing can be
     * heard. */
    volatile uint8_t pitch_req_on_silence;

    float tone_gain;     // AVAS_TYPE_TY_L1_NORM * user tone gain
    float master_gain;

    float gate;
    float gate_target;
    float gate_attack_alpha;
    float gate_release_alpha;
};

//===========================================================
// Function Prototype
//===========================================================

extern void  avas_type_ty_synth_init(avas_type_ty_synth_t *s, float fs);
extern float avas_type_ty_synth_process_sample(avas_type_ty_synth_t *s);

/* 0 dB = the coefficient set's own normalised level (0.9 peak). */
extern void avas_type_ty_synth_set_tone_gain_db(avas_type_ty_synth_t *s, float db);
extern void avas_type_ty_synth_set_master_gain_db(avas_type_ty_synth_t *s, float db);

/* Restart every oscillator from its table phase.  The phases are only mutually
 * meaningful at t = 0 of the model, so this is how the synth is put back into
 * the exact state the coefficient set describes. */
extern void avas_type_ty_synth_reset_phase(avas_type_ty_synth_t *s);

extern void avas_type_ty_synth_gate_on(avas_type_ty_synth_t *s);
extern void avas_type_ty_synth_gate_off(avas_type_ty_synth_t *s);

/* Request a pitch ratio (1.0 = the table's own pitch).  Cheap and callable from
 * any context: it stores the value and returns; the render context rebuilds the
 * step tables at the next envelope rebuild.  Clamped to
 * +-AVAS_TYPE_TY_PITCH_LIMIT_CENT. */
extern void  avas_type_ty_synth_request_pitch_ratio(avas_type_ty_synth_t *s, float ratio);

//===========================================================
// API
//===========================================================

extern void  app_avas_type_ty_init_48k(void);
extern float app_avas_type_ty_process_sample_48k(void);
extern void  app_avas_type_ty_set_enable(bool enable);

/* True while this engine renders anything, release fade included.  The two AVAS
 * sources are exclusive at run time because their loads would add up. */
extern bool  app_avas_type_ty_is_active(void);

/* Pitch trim in cent, relative to the table's own pitch.  Absolute set (not a
 * delta) so the caller owns the UI state and this stays a pure sink; the value
 * is clamped here, and the clamped result is what the getter returns.
 *
 * Does NOT survive a stop: app_avas_type_ty_set_enable(false) puts the trim back to
 * 0 cent so a new start never inherits the previous session's detune (the getter
 * reports 0 immediately; the step tables follow once the release fade has gone
 * silent, so the fading tail keeps the pitch it was sounding at). */
extern void  app_avas_type_ty_set_pitch_cent(float cent);
extern float app_avas_type_ty_get_pitch_cent(void);
extern float app_avas_type_ty_get_pitch_ratio(void);


#endif  //!_APP_AVAS_TYPE_TY_H
#endif  //defined(ENA_AVAS_TYPE_TY_SYNTH)
