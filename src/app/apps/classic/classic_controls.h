#ifndef SONORA_CLASSIC_CONTROLS_H
#define SONORA_CLASSIC_CONTROLS_H

// Classic application: button/touch dispatch and the UsrOperate_* actions they (and the
// app_debug.c raw hotkey handler) invoke. Owns My_Gain/My_Tone* debug printf access.

void classic_controls_process( void );   // call once per main loop tick (button + touch)

void UsrOperate_mute( void );
void UsrOperate_treble( void );
void UsrOperate_bass( void );
void UsrOperate_clickclack( void );
void UsrOperate_clickclack_toggle( void );
void UsrOperate_kinkon( void );
void UsrOperate_pinger( void );
void UsrOperate_engine_synth( void );      /* SW2 (treble) long press: on/off toggle */
void UsrOperate_avas_synth( void );        /* TYPE_TY       ('a', *cy00) */
void UsrOperate_avas_synth_type_lb( void );  /* LAMB  ('A') */
void UsrOperate_avas_synth_button( void ); /* Mute long press: start alternates TY/Type_LB */

/* AVAS pitch trim status line ('?cs'), for whichever engine is switched on.
 * The trim itself has no key: the POT is its only control, sampled inside
 * classic_controls_process(): fully counter-clockwise is the engine's own pitch,
 * clockwise raises it.  Range/clamp live with each engine
 * (AVAS_TYPE_TY_POT_TOP_CENT / AVAS_TYPE_LB_POT_TOP_CENT). */
void UsrOperate_avas_pitch_print( void );
void UsrOperate_surround( void );
void UsrOperate_Bmode( void );

#endif /* SONORA_CLASSIC_CONTROLS_H */
