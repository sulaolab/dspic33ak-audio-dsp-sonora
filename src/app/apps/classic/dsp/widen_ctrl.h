// ======================================
// widen_ctrl.h
// Minimal stereo-width (M/S) with optional Delay & All-pass
// ======================================
#ifndef _WIDEN_CTRL_H
#define _WIDEN_CTRL_H


#if defined(ENA_WIDEN_CTRL)
//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

/*
 * SIDE SCALING -- the internal Side signal carries a factor of 2
 *
 * Textbook M/S is Mid = 0.5*(L+R), Side = 0.5*(L-R), and the re-mix multiplies
 * by out_gain.  That spends one multiply per sample on each of the two 0.5
 * factors, and the compiler cannot fold them away because Side passes through
 * the delay line and the all-pass in between.
 *
 * So the module carries Mid and Side at twice the textbook amplitude
 *
 *     Mid  = L + R                 (no 0.5)
 *     Side = (L - R) * side_gain    (no 0.5)
 *
 * and takes the factor back out once, at the re-mix, via out_gain_half:
 *
 *     L' = (Mid + Side) * out_gain_half,  out_gain_half = 0.5 * out_gain
 *
 * Two multiplies per sample disappear and the OUTPUT IS BIT-IDENTICAL: 0.5 and
 * 2 are exact in binary floating point, so scaling a value by 2 commutes with
 * rounding, and every stage the doubled Side passes through (HPF, delay line,
 * all-pass, the re-mix sum) is linear.  Only overflow/subnormal corners could
 * differ, i.e. sample magnitudes near 1e38 or 1e-38.
 *
 * Consequence for the state fields: hpf_z, delay_buf[] and ap_x1/ap_y1 all hold
 * DOUBLED values.  Anything that inspects or seeds them must use the same
 * convention -- both widen_process() paths already do.
 */

// ---- Module handle ----
typedef struct
{
    // Sample rate
    uint32_t sample_rate_Hz;

    // Buffer layout
    int    num_proc_ch;     // interleaved slots per frame (e.g., STAGE_1_PROC_CH)
    int    l_slot;          // which slot index is Left
    int    r_slot;          // which slot index is Right

    // Master enable
    bool   enabled;         // overall bypass

    // Output gain (for widen_ctrl)
    float  out_gain;        // 10^(out_gain_db/20), kept for readback
    float  out_gain_half;   // 0.5 * out_gain -- the value the loops use (see SIDE SCALING)

    // --- M/S core (always available when enabled) ---
    float  side_gain;       // 1.0 = no change, 1.2~1.5 widens
    float  side_hpf_hz;     // 0 to skip; >0 applies 1st-order HPF only to Side

    // HPF state (for Side)
    float  hpf_a;           // z^-1 coeff (derived)
    float  hpf_z;           // state for 1st-order HPF on Side

    // --- Optional small delay (Haas) applied to Right only ---
    bool   use_delay;
    float  delay_ms;        // 0.0 to skip; typical 0.5~3.0 ms
    float *delay_buf;       // circular buffer (Right channel only)
    int    delay_len;       // in samples
    int    delay_w;         // write index
    int    delay_samp;      // cached delay in samples

    // --- Optional all-pass on Right only ---
    bool   use_allpass;
    float  ap_a;            // 0.0~0.9 typical (phase rotation strength)
    float  ap_x1;           // x[n-1]
    float  ap_y1;           // y[n-1]

} widen_t;




//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void    widen_init( widen_t *w,
                           uint32_t sample_rate_Hz,
                           int num_proc_ch,
                           int l_slot,
                           int r_slot,
                           float *delay_buf,
                           int   delay_buf_samples );

extern void    widen_set_params( widen_t *w,
                                 bool  enabled,
                                 float out_gain_db,
                                 float side_gain,
                                 float side_hpf_hz,
                                 bool  use_delay,
                                 float delay_ms,
                                 bool  use_allpass,
                                 float ap_a );


extern void    widen_process( widen_t *w,
                                  const float *in,
                                  float *out,
                                  int samples );




//===========================================================
// API
//===========================================================

extern void    app_widen_init(uint32_t sample_rate_Hz);
extern void    app_widen_disable(void);
extern void    app_widen_enable(void);
extern void    app_widen_process(const float* in, float* out);




#endif //defined(ENA_WIDEN_CTRL)
#endif //_WIDEN_CTRL_H
