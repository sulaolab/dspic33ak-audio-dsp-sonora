#ifndef _GAIN_CTRL_H
#define	_GAIN_CTRL_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

// Enum to represent ramping status
typedef enum
{
    RAMP_IDLE = 0,
    RAMPING_UP,
    RAMPING_DOWN

}ramp_status_t;

// Structure to hold gain control parameters
typedef struct
{
    int   samples;              // frame size
    int   num_proc_ch;          // channel(slot) per Fs of buffer

    int   mute_on;              // 1: output disabled  0: output enabled
    float storedGain;           // gain setting from user app

    float prevGain;             // Current gain value / gain value after previous sample
    float targetGain;           // Target gain to reach
    float minGain;              // Minimum allowed gain for initialization clamp
    float snapThresh;           // Threshold for snapping directly to target gain

    uint32_t sample_rate_Hz;    // Sample rate used to convert ramp time to samples
    float    rampTime_ms;       // Requested ramp time
    float    rampStep;          // Linear gain step per sample
    uint32_t rampRemainSamples;
    uint32_t rampTotalSamples;

    ramp_status_t status;       // Current ramping status

    float DBG_ramp_ms;
    float DBG_rampSamples;

} audiogain_t;

//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void   app_gain_init( uint32_t sample_rate_Hz );
extern void   app_gain_process( float* p_in, float* p_out );
extern void   app_gain_set( uint8_t vol_step );
extern void   app_mute_set( bool mute, int rampTime_ms );

extern void   mute_set( audiogain_t* pgain, int mute_on, int rampTime_ms );
extern void   gain_set( audiogain_t* pgain, uint8_t vol_step, int rampTime_ms );
// extern void   gain_set_db( audiogain_t* pgain, float dB );
extern void   gain_set_ramptime( audiogain_t* pgain, int rampTime_ms );

extern void   gain_init( audiogain_t* pgain,
                         float        initialGain,
                         int          frameSize,
                         int          num_proc_ch,
                         uint32_t     sample_rate_Hz );

extern void   gain_process( audiogain_t* pgain, float* p_in, float* p_out );


#endif	//!_GAIN_CTRL_H

