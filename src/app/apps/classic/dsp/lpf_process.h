#ifndef _LPF_PROCESS_H
#define	_LPF_PROCESS_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    int   num_proc_ch;     // channel(slot) per Fs of buffer

    // One biquad's coefficients (normalized a0=1.0)
    float  b0, b1, b2;   // numerator
    float  a1, a2;       // denominator

    // Filter state buffer: 4 floats per channel [num_ch * 4]
    // layout per channel: [ z1a, z2a, z1b, z2b ]
    float* filtStates;

    // Gain control
    float  gain_dB;      // user-specified gain (dB)
    float  gain_lin;     // cached linear gain (pow(10, dB/20))

    // mix control (0=dry/bypass, 1=wet/filtered)
    float  wet;

    // Debug (optional)
    float  DBG_fs;
    float  DBG_fc;

} lpf_t;




//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void   app_lpf_init(void);
extern void   app_lpf_process(const float* in, float* out);

// Set user gain in dB (updates both dB and linear)
extern void   app_lpf_set_gain( float gain_dB );
extern void   app_lpf_enable( bool enable );


// Initialize struct and bind external state buffer (must be chPerFs*4 floats)
extern void lpf_init( lpf_t* plpf, float* stateBuffer, int num_proc_ch );

// Set LR4(=Butterworth 2nd * 2) low-pass by computing one Butterworth biquad at fc
extern void lpf_set_coeffs_lr4(lpf_t* plpf, float fs, float fc);

// Process: Linkwitz-Riley 4th-order LPF (two cascaded identical biquads)
extern void lpf_process24(      lpf_t* plpf,
                          const float* in,
                                float* out,
                                int    samples );






#endif	//!_LPF_PROCESS_H

