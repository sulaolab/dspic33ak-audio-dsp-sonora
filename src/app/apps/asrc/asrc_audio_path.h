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
typedef struct
{
    uint32_t num_ab;
    uint32_t den_ab;
    uint32_t num_ba;
    uint32_t den_ba;
    uint32_t low_rate_hz;   /* the non-48 kHz side the front end targets, 0 if none */
    uint32_t in_rate_hz;    /* the leg the front end sits on, 0 if none */
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

// Which coefficient set the active front end loaded, as a suffix for the telemetry `fe=`
// field: ":11k" / ":12k" for the two /4 sets, "" when the divider is unambiguous on its own.
// The divider cannot identify a /4 by itself -- both output rates use 4 -- and the two sets
// have different band edges, so without this a mis-selected set is invisible in the log.
const char* asrc_audio_path_frontend_tag( void );

// Apply rate-monotonic RX-ISR priorities for the committed rate pair: the higher-rate leg has
// the shorter deadline and gets the higher priority. Task level, called while the transport is
// stopped (see the definition for why it is not dynamic). No-op when the rates are equal or
// APP_ASRC_RATE_MONOTONIC_ISR is 0.
void asrc_audio_path_apply_isr_priorities( void );

// --- Declick pop measurement (see [internal] manual_wm8904_mute_restart_declick.md) ---
// While armed, the A-leg callback observes A's raw ADC block and accumulates peak-abs and sum-of-squares
// over channels 0/1. With B HPOUT looped to A LINE-IN by an external cable, a B-only codec restart's pop
// is scored objectively (no listener). Producer = block ISR (observe, internal); consumer = main loop.
void asrc_audio_path_pop_meas_reset( void );
void asrc_audio_path_pop_meas_set_active( bool on );
void asrc_audio_path_pop_meas_read( int32_t* out_peak, uint64_t* out_sumsq, uint32_t* out_frames );

#endif // ASRC_AUDIO_PATH_H
