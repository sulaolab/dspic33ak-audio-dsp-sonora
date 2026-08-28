#ifndef _SINE_GEN_H
#define _SINE_GEN_H

#if defined(ENA_SINE_GEN)
//===========================================================
// Simple sine (NCO) generator for "keep-alive" AUX detection
//===========================================================
// - Interleaved float audio buffer: [samples * num_proc_ch]
// - ch-major float audio buffer:    [num_proc_ch][samples]
// - Generates a sine and optionally adds it to an input stream.
// - Intended for ultrasonic tone injection (e.g., JBL Flip4 mute avoidance).
// - The sample rate is stored in sine_gen_t and used for phase increment
//   calculation when frequency is initialized or updated.

//===========================================================
// Struct
//===========================================================

typedef struct
{
    int   num_proc_ch;

    float sample_rate_Hz; // sample rate used for phase increment calculation
    float freq_Hz;        // requested frequency [Hz]
    float freq_eff_Hz;    // effective frequency after clamp [Hz]

    float gain_lin;       // linear gain (peak amplitude)
    float phase_rad;      // current phase [rad]
    float phase_inc;      // phase increment per sample [rad]

} sine_gen_t;


//===========================================================
// API
//===========================================================

extern void app_keepalive_init(uint32_t sample_rate_Hz);
extern void app_keepalive_set(float freq_Hz, float gain_lin);
extern void app_keepalive_set_sample_rate(uint32_t sample_rate_Hz);
extern void app_keepalive_process(const float* in, float* out, int num_proc_ch);


// init with defaults
extern void sine_gen_init( sine_gen_t* p,
                           float      initialFreq_Hz,
                           float      initialGain_lin,
                           int        num_proc_ch,
                           uint32_t   sample_rate_Hz );

// set sample rate and recalculate phase increment from stored requested frequency
extern void sine_gen_set_sample_rate( sine_gen_t* p, uint32_t sample_rate_Hz );

// set frequency only and recalculate phase increment from stored sample rate
extern void sine_gen_set_freq( sine_gen_t* p, float freq_Hz );

// set frequency (Hz) and gain (linear, peak)
extern void sine_gen_set_params( sine_gen_t* p, float freq_Hz, float gain_lin );

// optional: set current phase (rad). Useful when freq=Fs/2 (e.g., 24 kHz @ 48 kHz)
extern void sine_gen_set_phase( sine_gen_t* p, float phase_rad );

// generate tone only (no input), interleaved layout

// generate tone only (no input), ch-major layout, using p->num_proc_ch
extern void sine_gen_process( sine_gen_t* p, float* out, int samples );

// add tone to input (out = in + tone), interleaved layout

// add tone to input (out = in + tone), ch-major layout, using p->num_proc_ch
extern void sine_gen_process_add( sine_gen_t*  p,
                                      const float* in,
                                      float*       out,
                                      int          samples );

// add tone to input (out = in + tone), ch-major layout, using runtime num_proc_ch
extern void sine_gen_process_add_nch( sine_gen_t*  p,
                                          const float* in,
                                          float*       out,
                                          int          num_proc_ch,
                                          int          samples );


#endif //defined(ENA_SINE_GEN)
#endif // !_SINE_GEN_H
