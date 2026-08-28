#ifndef _REVERB_H
#define	_REVERB_H

//===========================================================
// INCLUDES
//===========================================================




//===========================================================
// Definition
//===========================================================




//===========================================================
// Enum & Struct typedef
//===========================================================

/**
 * @brief Structure to hold reverb (comb filter) parameters and state.
 *
 *        This structure manages delay buffer, feedback gain, wet/dry mix ratio,
 *        and per-channel buffer indices for a simple comb filter reverb effect.
 */
typedef struct
{
    int   num_proc_ch;          // channel(slot) per Fs of buffer

    int   delaySamples;       ///< Delay in samples (e.g., 2400 samples for ~50ms at 48kHz)
    float feedbackGain;       ///< Feedback gain (typically 0.2 to 0.8)
    float wetMix;             ///< Wet signal mix ratio (0.0 to 1.0)
    float dryMix;             ///< Dry signal mix ratio (1.0 - wetMix)

    float* delayBuf;          ///< Pointer to delay buffer: [chProcs][MAX_DELAY_SAMPLES]
    int*   wrIdx;             ///< Write index for each channel: [chProcs]

    float   wetMix_orig;

} reverb_t;




//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

extern void    app_reverb_init(void);
extern void    app_reverb_process( const float* in, float* out );
extern void    app_reverb_enabled( int dly_ms );


extern void    reverb_set_enabled_drywet(reverb_t* rv, int dly_ms);
extern void    reverb_init(reverb_t* rv, float* buf, int* wridx, float feedbackGain, float wetMix, int num_proc_ch);
extern void    reverb_process(reverb_t* rv, const float* in, float* out, int samples);


#endif	//!_REVERB_H

