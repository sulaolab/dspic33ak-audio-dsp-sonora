#ifndef _DEESSER_H
#define _DEESSER_H


#if defined(ENA_DEESSER)
//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

/**
 * 1-band De-esser (HPF 6k detector) with split-band processing.
 * - Split: low = LPF(x), high = x - low
 * - Detect: d = HPF(x, 6k), env = AR(abs(d))
 * - Gain: compute gain_target from env, then smooth gain (AR) to avoid clicking
 * - Apply: y = low + high * gain
 *
 * Notes:
 * - Uses DF2T biquad states per channel.
 * - Designed to be stable with APP_BLOCK_FRAMES=32 and also works with APP_BLOCK_FRAMES=1.
 */
#define DEESSER_MAX_CH   (STAGE_1_PROC_CH)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    int num_proc_ch;

    // --- split-band LPF (for low extraction) ---
    biquad_t      lp;                     // current LPF
    biquad_t      lp_target;              // target LPF (optional smoothing)
    biquad_stat_t lp_s[DEESSER_MAX_CH];

    // --- detector HPF (fixed 6k default) ---
    biquad_t      det;                    // current detector HPF
    biquad_t      det_target;             // target HPF
    biquad_stat_t det_s[DEESSER_MAX_CH];

    // --- envelope + gain states per channel ---
    float env[DEESSER_MAX_CH];            // envelope state (linear)
    float gain[DEESSER_MAX_CH];           // smoothed gain applied to high band (linear)

    // --- smoothing gains (0..1) for block update ---
    float env_att_g;
    float env_rel_g;
    float gain_att_g;
    float gain_rel_g;
    float coeff_smooth_g;                 // optional coeff smoothing for lp/det

    // --- params (targets) ---
    float thr_dB;                         // threshold in dBFS-ish for detector envelope
    float ratio;                          // compression ratio (>=1)
    float max_gr_dB;                      // maximum gain reduction (>=0)
    float split_fc_Hz;                    // LPF cutoff for split (e.g., 5k)
    float det_fc_Hz;                      // HPF cutoff for detector (e.g., 6k)
} deesser_t;


//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

extern void deesser_init(deesser_t* ds,
                         int num_proc_ch,
                         float rampTime_ms);     // used for coeff_smooth_g

extern void deesser_set_params(deesser_t* ds,
                               float thr_dB,
                               float ratio,
                               float max_gr_dB,
                               float attack_ms,
                               float release_ms,
                               float split_fc_Hz,
                               float det_fc_Hz /* = 6000 */);


extern void deesser_process(deesser_t* ds,
                                const float* in,
                                float* out,
                                int samples);

// Helper: reset states (env/gain/biquad z's)
extern void deesser_reset_states(deesser_t* ds);






//===========================================================
// API
//===========================================================

extern void  app_deesser_init(void);
extern void  app_deesser_process(const float* in, float* out);



#endif // defined(ENA_DEESSER)
#endif // _DEESSER_H


