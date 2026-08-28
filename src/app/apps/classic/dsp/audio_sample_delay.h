#if defined(ENA_SAMPLE_DELAY)
#ifndef _AUDIO_SAMPLE_DELAY_H
#define _AUDIO_SAMPLE_DELAY_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

#define AUDIO_SAMPLE_DELAY_NUM_CH             (4)
#define AUDIO_SAMPLE_DELAY_MAX_CH             (4)

// Delay memory pool size for all channels.
// Current pool is equivalent to old 4ch x 1024 samples x 4 bytes.
#if !defined(AUDIO_SAMPLE_DELAY_POOL_BYTES)
#if !defined(ENA_FIR_FILTER)
//  #define AUDIO_SAMPLE_DELAY_POOL_BYTES         (41000)    // in case of using far memory
  #define AUDIO_SAMPLE_DELAY_POOL_BYTES         (25u * 1024u) // 25*1024=25600
//  #define AUDIO_SAMPLE_DELAY_POOL_BYTES         (16u * 1024u)
//  #define AUDIO_SAMPLE_DELAY_POOL_BYTES         (4u * 1024u)
#else
  #define AUDIO_SAMPLE_DELAY_POOL_BYTES         (8u * 1024u)
#endif //!defined(ENA_FIR_FILTER)
#endif //!defined(AUDIO_SAMPLE_DELAY_POOL_BYTES)


#define AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT   (AUDIO_SAMPLE_DELAY_POOL_BYTES / 4u)

// Maximum delay that can be assigned to one channel when all pool memory is free.
#define AUDIO_SAMPLE_DELAY_MAX_SAMPLES        (AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT)


//===========================================================
// Enum & Struct typedef
//===========================================================


//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================


//===========================================================
// API
//===========================================================

/*
 * Initialize the sample-delay module.
 *
 * sample_rate_hz:
 *   Runtime audio sample rate in Hz.
 *   If 0 is passed, SAMPLE_RATE is used internally.
 *
 * This sample rate is used for ms <-> samples conversion and debug printout.
 * The actual delay processing itself is sample-based.
 *
 * Example:
 *   app_audio_sample_delay_init(SAMPLE_RATE);
 */
extern void     app_audio_sample_delay_init( uint32_t sample_rate_hz );

extern void     app_audio_sample_delay_clear_state(void);
extern bool     app_audio_sample_delay_set_delay_samples( uint16_t channel,
                                                          uint16_t delay_samples );
extern bool     app_audio_sample_delay_get_delay_samples( uint16_t  channel,
                                                          uint16_t* delay_samples );

extern void     app_audio_sample_delay_process( float* buf );
extern void     app_audio_sample_delay_debug_print_status(void);


#endif //!_AUDIO_SAMPLE_DELAY_H
#endif //defined(ENA_SAMPLE_DELAY)
