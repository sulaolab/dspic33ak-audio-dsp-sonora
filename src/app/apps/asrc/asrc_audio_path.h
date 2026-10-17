#ifndef ASRC_AUDIO_PATH_H
#define ASRC_AUDIO_PATH_H

#include <stdint.h>
#include <stdbool.h>

// ASRC App-owned real-time audio path. The shared audio transport calls these
// entry points for the A (SPI1) and B (SPI2) clock domains.
void asrc_audio_path_leg_a_callback( const int32_t* src, int32_t* dst, void* user );
void asrc_audio_path_leg_b_callback( const int32_t* src, int32_t* dst, void* user );

// Reset optional path-owned fixed-rate state on every transport (re)start.
void asrc_audio_path_reset( void );
void asrc_audio_path_dbg_print( void );

// Deterministic rate change before the ASRC, per direction, as the COMPOSED ratio of every fixed
// stage ahead of the resampler -- which is what the feed-forward plan divides by.
//
// From a 48 kHz input leg one stage does the job: 6 for the 8 kHz path, 4 for 11.025 and 12 kHz,
// 3 for 16 kHz, 2 for 22.05 and 24 kHz, and 1 for direct ASRC operation.
//
// From a 96 kHz input leg a fixed 96 -> 48 kHz /2 pre-stage runs first and the existing chain
// behind it, so the ratio is the product: 12 for 8 kHz, 8 for 11.025 and 12 kHz, 6 for 16 kHz.
// 22.05 kHz and above stay direct at 1 -- their look-ahead already fits the ring, so they get no
// stage.  (This is why the value here can exceed any single stage's divider.)
//
// At most one of the two is ever != 1: only the DOWN-sampling direction gets a front end, since
// the up-sampling direction's step is below 1 and its look-ahead shrinks rather than clamping.
// One decimator instance is shared on that basis.
/* The active front end's exact rational ratio, num/den, per leg.  num is 1 for every integer
 * decimating chain and 2 for the 48 -> 32 kHz AUDIO MODE front end (L=2/M=3); den is 1 when that
 * leg has no front end.  Both are needed by the rate plan -- step_ff scales by num/den -- and
 * den alone no longer identifies a chain, because 2/3 and 1/3 share den == 3. */
/*
 * Front-end plan for a rate PAIR: what num/den each engine would get.  Filled by
 * asrc_audio_path_frontend_plan(), which is pure -- it selects, it does not apply.
 * den == 1 && num == 1 means "no front end on this engine" (the direct variable-ratio path).
 */
/*
 * WHAT KIND of front end, as a fact separate from its RATIO.
 *
 * Until 2026-09-14 `num == den == 1` carried two meanings at once: "the ratio is unity" and
 * "there is no stage".  Those were the same thing only because every stage this tree had was
 * also a rate change.  The 1:1 source-rate anti-alias LPF breaks that -- ratio 1/1, filter
 * PRESENT -- so the two facts are now stated separately, and every consumer that means one of
 * them says which.  num/den keep their old meaning exactly: THE RATIO, nothing else.
 *
 * Deliberately an enum and not a bool: FIXED_RATIO and RATIONAL are distinguishable from
 * num/den today, but a reader (and the telemetry) should not have to re-derive from arithmetic
 * what the planner already decided, and the next kind added must not have to squeeze itself
 * into a spare value of `num`.
 */
typedef enum
{
    ASRC_FRONTEND_NONE = 0,      /* no stage: the direct variable-ratio path.  num=den=1     */
    ASRC_FRONTEND_FILTER_1TO1,   /* a stage, ratio 1/1.  Band-limits at the SOURCE rate      */
    ASRC_FRONTEND_FIXED_RATIO,   /* an integer decimating chain, 1/den                       */
    ASRC_FRONTEND_RATIONAL,      /* an L/M polyphase chain (today L=2, M=3)                  */
    /* A COMPOSITION: an integer decimating chain, 1/den, and then a 1:1 band-limit at the
     * intermediate rate.  The RATIO is 1/den exactly as FIXED_RATIO -- what this kind adds is
     * that the chain does not stop at the divider, so a reader cannot recover it from num/den.
     * Which is the whole reason the kind is a separate field: 44.1 -> 16 kHz publishes 1/2 like
     * any /2 row, and runs a stage the /2 rows do not have. */
    ASRC_FRONTEND_FIXED_THEN_1TO1,
} asrc_frontend_kind_t;

typedef struct
{
    uint32_t num_ab;
    uint32_t den_ab;
    uint32_t num_ba;
    uint32_t den_ba;
    uint32_t low_rate_hz;   /* the non-48 kHz side the front end targets, 0 if none */
    uint32_t in_rate_hz;    /* the leg the front end sits on, 0 if none */
    /* WHICH KIND on each leg.  At most one is ever != ASRC_FRONTEND_NONE, the same invariant
     * num/den have always carried.  A consumer asking "is there a stage" must test these and
     * NOT `den != 1`; a consumer asking "what step will the resampler see" must keep using
     * num/den, because a 1:1 stage leaves the step untouched -- which is why it cannot rescue
     * a pair the ring refuses on step alone. */
    asrc_frontend_kind_t kind_ab;
    asrc_frontend_kind_t kind_ba;
} asrc_frontend_plan_t;

/* Pure: ask what the front end WOULD be for this pair, without changing anything. */
void asrc_audio_path_frontend_plan( uint32_t a_rate_hz, uint32_t b_rate_hz,
                                    asrc_frontend_plan_t* plan );

/* Whether the front-end implementation this build compiled can carry the ASRC logical width
 * (ASRC_CH).  False disqualifies it outright: a pair whose plan asks for a front end must be
 * REFUSED rather than served by a narrower one, because an anti-alias stage that filters fewer
 * channels than the resampler converts is a missing stage.  Pairs that need no front end are
 * unaffected.  There is no override -- see the disqualification note in asrc_audio_path.c. */
bool asrc_audio_path_frontend_can_serve_logical_width( void );

uint32_t asrc_audio_path_ab_fixed_rate_num( void );
uint32_t asrc_audio_path_ba_fixed_rate_num( void );
uint32_t asrc_audio_path_ab_fixed_rate_den( void );
uint32_t asrc_audio_path_ba_fixed_rate_den( void );

/* One engine's NOMINAL step, as the exact rational the fill-reserve law needs:
 * step = (fs_in_hz * num) / (den * fs_out_hz), with num/den the live front-end rational.
 *
 * `ab` selects the A->B engine.  The rates are the CONFIGURED ones, not a measured ratio: the
 * reserve a pull needs must bound the servo-trimmed step, and a measured value sits a few ppm on
 * either side of the configured rational (see asrc_fill_law() in audio_app_asrc.c).  Returns false
 * while either leg's rate is still unknown, which leaves the caller on its degraded path rather
 * than on a guessed rate. */
bool asrc_audio_path_engine_nominal( bool ab, uint32_t* fs_in_hz, uint32_t* num,
                                     uint32_t* den, uint32_t* fs_out_hz );

// Which coefficient set the active front end loaded, as a suffix for the telemetry `fe=`
// field: ":11k" / ":12k" for the two /4 sets, "" when the divider is unambiguous on its own.
// The divider cannot identify a /4 by itself -- both output rates use 4 -- and the two sets
// have different band edges, so without this a mis-selected set is invisible in the log.
const char* asrc_audio_path_frontend_tag( void );

/* WHICH KIND of front end is currently ARMED (not merely planned).  ASRC_FRONTEND_NONE when
 * none is.  Exists because `den == 1` can no longer answer it: a 1:1 stage has ratio 1/1 and a
 * filter, so a telemetry field or a gate that tested the divider would call it `direct` --
 * true about the ratio and a lie about the anti-alias stage.  Same reason the Full-IIR trial
 * needed its own naming hook. */
asrc_frontend_kind_t asrc_audio_path_frontend_kind( void );

/* THE SAME QUESTION, PER DIRECTION.  `asrc_audio_path_frontend_kind()` above is ONE value for
 * the armed chain, and the chain belongs to exactly one direction -- so a per-direction reader
 * that called it got the OTHER direction's kind.  Measured, not feared: on 2026-09-14 the BA
 * leg of 44.1 -> 16 kHz printed `fe=/1+lp1:16k`, borrowing the AB leg's composed kind and
 * formatting its own den of 1 as a nonsensical "divide by 1", while `fe exec: BA=0` said in the
 * same block that nothing ran there.  Any consumer that is per-direction must use THIS.
 *
 * Returns ASRC_FRONTEND_NONE for a direction with no stage, which is the honest answer: that
 * direction runs the direct variable-ratio path, and `direct` is what its telemetry says. */
asrc_frontend_kind_t asrc_audio_path_frontend_kind_dir( bool ab );

/* Blocks the front end has actually FILTERED since the last rate commit, per direction.
 * Zero on a direction with no stage.  This is the evidence `fe=` cannot give: that field names
 * the armed chain, and on 2026-09-14 it named one while the ISR gate kept the FIR out of the
 * audio path for twelve minutes.  It is also how the 'at most one direction owns the shared
 * front end' invariant is checked as a number -- the other direction must read exactly 0. */
uint32_t asrc_audio_path_frontend_runs_ab( void );
uint32_t asrc_audio_path_frontend_runs_ba( void );

/* Frames the front end's STAGE `stage` has produced since the last rate commit; 0 for a stage the
 * live chain does not have and 0 for one that never ran.
 *
 * The two counters above answer "did the front end run, and in which direction".  Inside a
 * COMPOSED chain that is not enough: /2 then 1:1 can run its divider and skip its band-limit, and
 * the direction counter grows either way -- the same gap between "armed" and "processed" that let
 * `fe=lp1` print for twelve minutes on 2026-09-14 with the FIR out of the path.  A /2 followed by
 * a 1:1 must report the SAME count from both stages.
 *
 * Here rather than straight out of the decimator because the telemetry's supplier for everything
 * about the front end is this module; audio_app_asrc.c does not include the decimator header. */
uint32_t asrc_audio_path_frontend_stage_frames( uint8_t stage );



// Apply rate-monotonic RX-ISR priorities for the committed rate pair: the higher-rate leg has
// the shorter deadline and gets the higher priority. Task level, called while the transport is
// stopped (see the definition for why it is not dynamic). No-op when the rates are equal or
// APP_ASRC_RATE_MONOTONIC_ISR is 0.
/* True once either leg callback has executed at least once; never goes back to false.
 * Callers that must not touch live engine state -- the push selftests, which use
 * s_asrc[ASRC_ENGINE_AB] as their scratch -- gate on this instead of on a call count.
 * See the definition for why a call count is not equivalent. */
bool asrc_audio_path_isr_has_started( void );

void asrc_audio_path_apply_isr_priorities( void );

// --- Declick pop measurement (see [internal] manual_wm8904_mute_restart_declick.md) ---
// While armed, the A-leg callback observes A's raw ADC block and accumulates peak-abs and sum-of-squares
// over channels 0/1. With B HPOUT looped to A LINE-IN by an external cable, a B-only codec restart's pop
// is scored objectively (no listener). Producer = block ISR (observe, internal); consumer = main loop.
void asrc_audio_path_pop_meas_reset( void );
void asrc_audio_path_pop_meas_set_active( bool on );
void asrc_audio_path_pop_meas_read( int32_t* out_peak, uint64_t* out_sumsq, uint32_t* out_frames );

#endif // ASRC_AUDIO_PATH_H
