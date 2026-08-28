#ifndef _TONE_CTRL_H
#define	_TONE_CTRL_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

#define TONE_SLOTS_PER_FS    (STAGE_1_PROC_CH)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    TONE_EQ_PEAKING    = 0,
    TONE_EQ_HIGH_SHELF = 1,
    TONE_EQ_LOW_SHELF  = 2,
} tone_filter_type_t;


typedef struct
{
    int      num_proc_ch;       // channel(slot) of buffer
    uint32_t sample_rate_Hz;    // Sample rate used for coefficient calculation and smoothing

    biquad_t      bq;
    biquad_stat_t bqs[TONE_SLOTS_PER_FS];

    biquad_t      bq_target;
    float         coeff_smooth_g;       // 0..1 (smoothing gain)

    float  Q_or_Slope;     // peaking: Q, shelf: S(slope)
    int    filter_type;    // actually filter type: 0=peaking, 1=high-shelf, 2=low-shelf

    float  DBG_tar_Hz;
    float  DBG_gain_dB;

} tone_t;



//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void   tone_init( tone_t*  ptone,
                         float    rampTime_ms,
                         float    initialGain,
                         float    Q_factor,
                         int      filter_type,
                         int      num_proc_ch,
                         uint32_t sample_rate_Hz );
extern void   tone_set_coeffs(tone_t* ptone, float gain_dB, float cutoff_Hz);
extern void   tone_process(      tone_t* ptone,
                               const float*  in,
                                     float*  out,
                                     int     frameSize );


//===========================================================
// API
//===========================================================

extern void   app_tone_init( uint32_t sample_rate_Hz );
extern void   app_tone_set_coeffs_tre( float gain_dB );
extern void   app_tone_set_coeffs_mid( float gain_dB );
extern void   app_tone_set_coeffs_bas( float gain_dB );
extern void   app_tone_process_tre( const float* in, float* out );
extern void   app_tone_process_mid( const float* in, float* out );
extern void   app_tone_process_bas( const float* in, float* out );



#endif	//!_TONE_CTRL_H

