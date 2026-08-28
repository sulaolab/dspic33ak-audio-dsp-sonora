#ifndef _FLOAT_CONVERSION_H
#define	_FLOAT_CONVERSION_H

//===========================================================
// float_conversion: sample-format conversions for the audio DSP chain.
//
// All DSP buffers are ch-major float (buf[channels][frameSize]); the codec/DMA
// side is interleaved Q31 int32 (WM8904 24-bit, full-scale 0x7FFFFFFF). These
// helpers convert between the two and to the PWM duty domain, applying the audio
// gains owned by this module. (The interleaved-float helpers were removed -- the
// chain is ch-major only -- so the names no longer carry a "" qualifier.)
//===========================================================


//===========================================================
// Fixed-point scale
//===========================================================

// Q31 fixed-point scale: int32 (Q31, WM8904 24-bit, full-scale 0x7FFFFFFF) -> normalized float.
// Single source of truth: convert_codec_int_to_float() AND the LED level meter's int32 input
// path (level_meter_process_i32) both use this so their int->float scaling stays identical.
#define Q31_SCALE_FLOAT           (1.0f / 2147483648.0f)


//===========================================================
// Variables -- audio gains (set by audio_gains_init(), used app-wide)
//===========================================================

/* Only Pre_Gain_CODEC exists in every build. The seven Classic-chain gains are
 * not declared in an ASRC build, so a reference from ASRC-side code is a compile
 * error rather than a gain nothing maintains -- see float_conversion.c. */
extern float  Pre_Gain_CODEC;
/* Defensive form: if this header is reached without app_specific_config_defs.h
 * the macro is invisible to #if (it would evaluate as 0), so fall back to
 * declaring all eight -- a declaration allocates nothing, and a real use from
 * an ASRC build still fails at link. float_conversion.c #errors instead, where
 * a missed macro would silently keep the definitions. */
extern float  Post_Gain_PWM;
extern float  Post_Gain_CODEC;
#if !defined( SONORA_APP_IS_ASRC ) || !SONORA_APP_IS_ASRC
extern float  Pre_Gain_WAV;
extern float  Gain_EngineSynth;
extern float  Gain_AvasSynth;
extern float  Gain_ClickClack;
extern float  Gain_KinKon;
#endif /* !SONORA_APP_IS_ASRC */


//===========================================================
// Function Prototype
//===========================================================

// Build the application-wide audio gains from the compile-time dB constants. Rate
// independent and idempotent, so it belongs in the one-time boot path (each app's
// sonora_app_prepare), NOT in the fs-parameterized transport prepare hook. (Was
// convert_codec_init / convert_tdm_init: it is gain setup, not a conversion -- the
// convert_* functions below are stateless.)
extern void audio_gains_init( void );

// Print the gains built by audio_gains_init. Separate so the caller decides whether
// the boot log carries them; nothing in the audio path depends on this.
extern void audio_gains_print( void );

// Codec interleaved Q31 int32 -> ch-major float (applies Pre_Gain_CODEC, 24-bit mask,
// clip). (Was convert_tdm_int32_to_float.)
extern void convert_codec_int_to_float( const int32_t* int_in,
                                              int      channels_in,
                                              float*   float_out,
                                              int      channels_out,
                                              int      frameSize );

// ch-major float -> codec interleaved Q31 int32 (applies Post_Gain_CODEC, Q31 scale,
// 24-bit mask, clip). (Was convert_tdm_float_to_int32.)
extern void convert_codec_float_to_int( const float*   float_in,
                                              int      channels_in,
                                              int32_t* int_out,
                                              int      channels_out,
                                              int      frameSize );

// ch-major float (one selected channel) -> 20-bit PWM duty values (applies
// Post_Gain_PWM, zero-order-hold upsampling).
extern void convert_float_to_pwm_20bit( const float*    float_in,
                                              int       channels_in,
                                              uint8_t   slot,
                                              int32_t*  output,
                                              size_t    num_samples,
                                              uint8_t   up_sample_factor,
                                              uint32_t  pwm_period );


#endif	//!_FLOAT_CONVERSION_H
