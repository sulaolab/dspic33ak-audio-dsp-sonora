#ifndef ASRC_FULL_IIR_48_TO_32_H
#define ASRC_FULL_IIR_48_TO_32_H

/*
 * TRIAL 48 -> 32 kHz anti-alias stage: a six-SOS elliptic IIR in place of the N97
 * rational (2/3) FIR front end.
 *
 * WHAT IT REPLACES, AND WHAT IT DOES NOT.  The N97 front end RESAMPLES (48 -> 32 kHz
 * at 2/3, leaving the generic ASRC at step about 1.0).  This does NOT resample: it
 * is a pure 48 kHz filter, frame count in == frame count out, and the generic ASRC
 * then does the whole 48 -> 32 conversion itself at step 1.5.  That is why removing
 * the front end and adding this is one change, not two:
 *
 *     N97:       48k ADC -> [2/3 FIR resample] -> 32k -> ASRC step ~1.0 -> 32k out
 *     Full-IIR:  48k ADC -> [6 SOS LPF, 48k]   -> 48k -> ASRC step ~1.5 -> 32k out
 *
 * SELECTION IS EXPLICIT AND RUNTIME.  APP_ASRC_FULL_IIR_48_TO_32 compiles it in
 * (default 0 everywhere, so every existing image is unchanged); `*aj` selects
 * between N97 and Full-IIR at runtime, and N97 is what boots.  One image therefore
 * measures both under identical DSP/TDM/console conditions, which is the only way
 * the two CPU numbers are comparable.
 *
 * The stage is A->B only, and only for the pair A = 48 kHz / B = 32 kHz.  Every
 * other rate pair, and the B->A direction, keep the path they have.
 *
 * Peaks: the unity-gain filter reaches 1.118 x full scale on target (10 kHz sine),
 * so the coefficient table carries a fixed headroom gain folded into section 0 --
 * see asrc_full_iir_48_to_32_coeffs.h for why it lives in the coefficients.
 */

#include "app_specific_config_defs.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef APP_ASRC_FULL_IIR_48_TO_32
#define APP_ASRC_FULL_IIR_48_TO_32 (0)
#endif

#if APP_ASRC_FULL_IIR_48_TO_32

/* Peak / FS-exceedance observation over the live stream.  ON by default in this
 * experimental path because §4 of the integration brief requires the accounting to be
 * measured rather than assumed; build with -Define APP_ASRC_FULL_IIR_OBSERVE=0 to
 * measure the stage without it and so report the observer's own cost. */
#ifndef APP_ASRC_FULL_IIR_OBSERVE
#define APP_ASRC_FULL_IIR_OBSERVE (1)
#endif

/* Which A->B 48 kHz anti-alias stage is selected.  Boot value is N97. */
typedef enum
{
    ASRC_FULL_IIR_MODE_N97      = 0,   /* the shipping 2/3 rational FIR front end */
    ASRC_FULL_IIR_MODE_FULL_IIR = 1,   /* this six-SOS LPF, ASRC at step 1.5      */
} asrc_full_iir_mode_t;

/* Main-loop context only.  Takes effect at the next stream reset -- the caller is
 * expected to re-apply leg A's rate so the change goes through the ordinary
 * mute -> teardown -> reset -> prefill -> unmute sequence. */
void                 asrc_full_iir_48_to_32_set_mode( asrc_full_iir_mode_t mode );
asrc_full_iir_mode_t asrc_full_iir_48_to_32_mode( void );

/* True when the mode is Full-IIR AND the committed pair is A = 48 kHz / B = 32 kHz, or
 * A = 96 kHz / B = 32 kHz (the 96 kHz pair reaches this stage through the existing
 * 96 -> 48 kHz pre-stage; the stage itself is unchanged and still runs at 48 kHz in).
 * Read by the front-end plan (main-loop) to publish den_ab = 1 for that pair, and
 * by the leg-A block callback (ISR) to route the block through this stage. */
bool asrc_full_iir_48_to_32_armed( void );
void asrc_full_iir_48_to_32_arm( bool on );

/* Zero the 16 channels' state and re-init the DF2T instances.  Called from the
 * stream reset hook only -- never per block. */
void asrc_full_iir_48_to_32_reset( void );

/* Filter one 48 kHz TDM block (APP_BLOCK_FRAMES frames, APP_SLOTS_PER_FS slots per
 * frame, s24 left-justified) and hand the filtered float block to the A->B engine.
 * Block ISR context.  Replaces path_push_ab() for this pair. */
void asrc_full_iir_48_to_32_process_push_ab( const int32_t* src );

/* Same stage, fed from the 96 -> 48 kHz pre-stage instead of the raw TDM block: `frames`
 * frames of `stride`-word interleaved s24-left words (frames = APP_BLOCK_FRAMES / 2 for
 * the 96 kHz pair, stride = ASRC_CH).  Channel mapping, coefficients, SOS order and
 * headroom are identical to the raw-TDM entry point -- only the source geometry differs.
 * Block ISR context. */
void asrc_full_iir_48_to_32_process_push_ab_frames( const int32_t* src, uint32_t frames,
                                                   uint32_t stride );

/* Build identity of the linked coefficient table.  Exposed as a call, not as macros:
 * asrc_full_iir_48_to_32_coeffs.h DEFINES the table (a `static float` array), so a
 * second translation unit including it would get a second copy of it. */
void asrc_full_iir_48_to_32_identity( uint32_t* crc32, uint32_t* unity_crc32,
                                      int32_t* headroom_mdb, uint32_t* sos );

/* Telemetry, main-loop context.  Peaks are x1000 of full scale (24-bit counts).
 *
 * `over_fs` counts samples of the stage's FLOAT work buffer at or beyond full scale --
 * i.e. FS *exceedance* of a 48 kHz intermediate, in a place where nothing clamps: the
 * value goes on into the float ring as-is (asrc_push_block_f32() documents why that
 * ring deliberately has no clip point).  It is NOT a clip count, which is why it is
 * not called one.  The only real saturation is the 32 kHz float->int24 slot
 * conversion: asrc_to_slot() in audio_app_asrc.c, and in a 16-channel build the
 * ASRC_STREAM16_PAIR_SLOT / mchp_stream16_paird_f32 hand-written kernels that write
 * the int32 slots directly -- neither carries a counter.  A real-clamp count comes
 * from the output side instead, via audio_app_meas_out_fs_stats() in a MEAS build.
 *
 * Cleared by asrc_full_iir_48_to_32_stats_clear() so a steady-state delta can be taken
 * separately from the start/stop transient. */
void asrc_full_iir_48_to_32_stats( uint32_t* out_peak_milli, uint32_t* state_peak_milli,
                                   uint32_t* over_fs, uint32_t* nonfinite, uint32_t* blocks );
void asrc_full_iir_48_to_32_stats_clear( void );

/* One line on the 10 s report, next to the engine it belongs to. */
void asrc_full_iir_48_to_32_dbg_print( void );

#endif /* APP_ASRC_FULL_IIR_48_TO_32 */

#endif /* ASRC_FULL_IIR_48_TO_32_H */
