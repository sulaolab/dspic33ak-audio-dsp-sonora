#ifndef _REVERB_SMOOTH_H
#define	_REVERB_SMOOTH_H

//===========================================================
// INCLUDES
//===========================================================




//===========================================================
// Definition
//===========================================================

// Number of parallel combs (can be 3-5; 4 is a good default)
#if !defined(RV_NCOMB)
  #define RV_NCOMB  4
#endif //!defined(RV_NCOMB)




//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    int     chPerFs;        // TDM slots per frame
    int     chProcs;        // number of processed channels
    int     fs;             // sampling rate (Hz)

    // External buffers (per-channel total length = MAX_DELAY_SAMPLES)
    float*  delayBuf;       // [chProcs * MAX_DELAY_SAMPLES]
    int     maxDelaySamps;  // MAX_DELAY_SAMPLES

    // Write indices for each comb and channel: size [chProcs * RV_NCOMB]
    int*    wrIdxComb;

    // One-pole LPF state in the feedback path: size [chProcs * RV_NCOMB]
    float*  fbLpfState;

    // Per-comb segment layout inside the per-channel buffer
    // Each comb uses its own circular segment: base offset + length
    int     combBase[RV_NCOMB];   // start index (samples) of each comb segment
    int     combLen [RV_NCOMB];   // length (samples)  of each comb segment

    // Decay and tone parameters
    float   T60_ms;          // target decay time to -60 dB (milliseconds)
    float   fb_g[RV_NCOMB];  // computed feedback gains per comb (linear)

    // Feedback LPF in the loop (simulates air absorption / damping)
    float   fb_lpf_fc;       // cutoff (Hz), e.g., 4000-8000
    float   fb_lpf_alpha;    // 1-pole smoothing factor per sample

    // Wet/Dry
    float   wetMix;          // 0..1
    float   dryMix;          // 1 - wet
} reverb_smooth_t;




//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

extern void   app_reverb2_init(void);
extern void   app_reverb2_process(const float* in, float* out);
extern void   app_reverb2_enabled( int dly_ms );









// Initialize (buffers supplied by caller)
extern void   reverb_smooth_init( reverb_smooth_t* rv,
                                  float* delayBuf,      // [chProcs * MAX_DELAY_SAMPLES]
                                  int*   wrIdxComb,     // [chProcs * RV_NCOMB]
                                  float* fbLpfState,    // [chProcs * RV_NCOMB]
                                  int    fs,            // sampling rate
                                  int    chPerFs,
                                  int    chProcs,
                                  int    maxDelaySamps  // MAX_DELAY_SAMPLES
                                );

// Set perceptual tail parameters
extern void   reverb_smooth_set_decay_T60_ms(reverb_smooth_t* rv, float T60_ms);
extern void   reverb_smooth_set_feedback_lpf(reverb_smooth_t* rv, float fc_hz);
extern void   reverb_smooth_set_wet(reverb_smooth_t* rv, float wetMix);

// Process one interleaved frame block
extern void   reverb_smooth_process(const reverb_smooth_t* rv,
                                    const float* in, float* out, int frameSize);


#endif	//!_REVERB_SMOOTH_H

