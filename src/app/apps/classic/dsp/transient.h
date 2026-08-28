#ifndef _TRANSIENT_H
#define	_TRANSIENT_H

//===========================================================
// INCLUDES
//===========================================================




//===========================================================
// Definition
//===========================================================

//#define MAX_TRANSIENT_CHANNELS   8
#define MAX_TRANSIENT_CHANNELS   STAGE_1_PROC_CH




//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct {
    int      ena_transient;
    int      num_proc_ch;                           // channel(slot) per Fs of buffer

    float    env_short[MAX_TRANSIENT_CHANNELS];   // Fast envelope follower
    float    env_long[MAX_TRANSIENT_CHANNELS];    // Slow envelope follower
    float    gain[MAX_TRANSIENT_CHANNELS];        // Block-wise gain value
    float    lpf_env[MAX_TRANSIENT_CHANNELS];     // RC lowpass for absolute signal

//    uint16_t num_samples;                         // Total samples per buffer
    uint8_t  divisions;                           // Number of divisions per buffer
    uint16_t samples_per_blk;                     // Samples per block

    float    alpha_short;                         // Fast envelope smoothing
    float    alpha_long;                          // Slow envelope smoothing
    float    shape_gain;                          // Gain multiplier for delta
    float    threshold;                           // Delta threshold
} transient_t;






//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

extern void   app_transient_init( void );
extern void   app_transient_toggle( void );
extern void   app_transient_process(float* out, const float* in);

extern void   transient_init(transient_t* t);
extern void   transient_process_dynamic(transient_t* t, float* out, const float* in);


#endif	//!_TRANSIENT_H

