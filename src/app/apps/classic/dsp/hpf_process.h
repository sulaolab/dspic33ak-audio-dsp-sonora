#ifndef _HPF_PROCESS_H
#define _HPF_PROCESS_H

//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    int   in_buf_ch;     // channel(slot) per Fs of buffer

    // One biquad's coefficients (normalized a0 = 1.0)
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

} hpf_t;

//===========================================================
// Function Prototype
//===========================================================

extern void   app_hpf_init(void);
extern void   app_hpf_process(const float* in, float* out);

// Set user gain in dB (updates both dB and linear)
extern void   app_hpf_set_gain(float gain_dB);
extern void   app_hpf_enable( bool enable );

// Initialize struct and bind external state buffer (must be chPerFs*4 floats)
extern void hpf_init( hpf_t* phpf, float* stateBuffer, int in_buf_ch );

// Set LR4(=Butterworth 2nd * 2) high-pass by computing one Butterworth biquad at fc
extern void hpf_set_coeffs_lr4(hpf_t* phpf, float fs, float fc);

// Process: Linkwitz-Riley 4th-order HPF (two cascaded identical biquads)
extern void hpf_process24(      hpf_t* phpf,
                          const float* in,
                                float* out,
                                int    samples );

#endif //!_HPF_PROCESS_H
