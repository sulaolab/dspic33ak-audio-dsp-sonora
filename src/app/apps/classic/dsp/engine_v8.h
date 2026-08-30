#if defined(ENA_ENGINE_SYNTH)

#ifndef _ENGINE_V8_H
#define _ENGINE_V8_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

/* Measured V8 engine model: one-cycle wavetables per RPM bin plus one shared
 * residual-noise table, both generated from the recordings. Replaces the
 * hand-tuned additive model in engine_synth.c, which stays in the tree but is
 * excluded from every build configuration.
 *
 * The API below is byte-for-byte the one engine_synth.c exported, so the call
 * sites (fx_domain_48k.c, classic_demo_app.c, classic_console.c,
 * classic_controls.c) differ only in which header they include. There is no new
 * ENA_* switch: which model is built is a configurations.xml question, not a
 * runtime one.
 *
 * Design, measurements and the frozen constants:
 *   [internal] astm_v8_resynthesis_analysis.md
 *              (frozen in section 38, implementation in section 39)
 *   tables:    src/app/apps/classic/dsp/engine_v8_tables.h  (generated)
 */

/* POT full scale (0x0FFF) maps to the top RPM bin, POT 0 to the idle. The old
 * model's ENG_SYNTH_POT_SCALE_FACTOR (1.5) is very nearly this slope, but it
 * started from 0 rpm -- and there is no such thing here: the model is measured
 * between 900 and 6875 rpm and the idle is where it is judged. */
#define ENGINE_V8_IDLE_RPM             (900.0f)
#define ENGINE_V8_MAX_RPM              (6875.0f)
#define ENGINE_V8_POT_FULL_SCALE       (4095.0f)

/* ---- staged bring-up (section 39) --------------------------------------
 * "It sounds like a different thing" is not a bisectable complaint against six
 * elements at once, so each element can be switched off at run time and the
 * ladder walked on the board without a reflash:
 *
 *   NONE   the bare wavetable at a fixed idle -- pitch, bin crossfade, nothing else
 *   +POT   the knob becomes the throttle
 *   +FILT  a deadband on the knob (see below)
 *   +JIT   cycle-to-cycle jitter
 *   +NOISE the residual noise, overlap-added
 *   +DRIFT the idle's +-40 rpm wander
 *   +MASK  the gated x6 band-limited masking noise   = ALL
 *
 * FILT is the one element that is NOT in section 38. It was added by this
 * bring-up: the raw POT reading wanders by tens of ADC LSB, one LSB is 1.46 rpm,
 * and the mask gate differentiates the commanded rpm over one 0.667 ms block --
 * so one LSB of noise is 2190 rpm/s against a scale of 1200, i.e. the gate is
 * held wide open by the ADC and the settled idle is masked, which is the one
 * thing section 38 says must not happen. STAGE_FROZEN reproduces that defect
 * for the A/B; STAGE_ALL is section 38 as it was meant to sound.
 *
 * Random draws are made whether or not the element that consumes them is on, so
 * two stages differ by the element and not by a shifted noise stream.
 */
#define ENGINE_V8_STAGE_POT            (0x01u)
#define ENGINE_V8_STAGE_POTFILT        (0x02u)
#define ENGINE_V8_STAGE_JITTER         (0x04u)
#define ENGINE_V8_STAGE_NOISE          (0x08u)
#define ENGINE_V8_STAGE_DRIFT          (0x10u)
#define ENGINE_V8_STAGE_MASK           (0x20u)

#define ENGINE_V8_STAGE_NONE           (0x00u)
#define ENGINE_V8_STAGE_ALL            (0x3Fu)
#define ENGINE_V8_STAGE_FROZEN         (0x3Du)   /* ALL minus POTFILT */
/* The default (section 42). The owner walked the ladder and judged 0x1F -- every
 * element except the chord mask -- the best balance, and asked for it as the base
 * to converge on. Section 38 adopted the mask to cover the chord on the descent;
 * section 40.3 then measured it as a 4.26 dB octave-rms departure from the
 * reference, the largest of any element, so this is the ladder confirming a
 * measurement rather than overturning one. "*cy7F" still turns it back on. */
#define ENGINE_V8_STAGE_DEFAULT        (0x1Fu)   /* ALL minus MASK */


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// API
//===========================================================

extern void  app_engine_synth_init_48k(void);
extern float app_engine_synth_process_sample_48k(void);

extern void  app_engine_synth_enable( bool enable );
extern bool  app_engine_synth_is_enable( void );
extern void  app_engine_synth_blip_start( void );

/* Bring-up only (console "*cy 40".."*cy 7F" to select, "*cy 07" to report). The
 * stage survives enable/disable; it is not part of the audio contract. */
extern void    app_engine_synth_set_stage( uint8_t stage );
extern uint8_t app_engine_synth_get_stage( void );
extern void    app_engine_synth_report( void );

/* ---- the model-neutral pair the Classic platform calls -------------------
 *
 * Both models declare these, so the platform needs no #ifdef on WHICH model is
 * compiled. That is the point: model-specific knowledge belongs to the model.
 *
 * app_engine_synth_console_subcode() owns every "*cy" subcode that is the engine's
 * and is not one of the shared four (04 blip / 05 on / 06 off). The model returns
 * true if it handled the subcode and false if it does not implement it, and the
 * console maps that to OK / ERR_UNSUPPORTED without knowing what any subcode means.
 *
 * app_engine_synth_rpm() is the rpm being sounded right now, 0 when the engine is
 * off. The monitor line used to recompute this from the raw POT with the model's own
 * ENGINE_V8_* constants, which put a second copy of the POT -> rpm map on the
 * platform side; asking the model removes the copy and reports what is actually
 * playing rather than what the knob asks for. */
extern bool  app_engine_synth_console_subcode( uint8_t subcode );
extern float app_engine_synth_rpm( void );


#endif //!_ENGINE_V8_H
#endif //defined(ENA_ENGINE_SYNTH)
