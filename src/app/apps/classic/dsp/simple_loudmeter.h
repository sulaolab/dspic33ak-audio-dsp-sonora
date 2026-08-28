#ifndef _SIMPLE_LOUD_METER_H
#define	_SIMPLE_LOUD_METER_H

/*
 * simple_loudmeter.h
 *
 * Single-slot loudness meter + intuitive gain mapping.
 * Mapping rule:
 *   - If level_dBFS <= low_dBFS   -> target boost = maxBoost_dB
 *   - If level_dBFS >= high_dBFS  -> target boost = 0 dB (bypass)
 *   - Linear map between low..high to 1..0, then scale by maxBoost_dB.
 *
 * Detector:
 *   - Block RMS across first chProcs channels (average across channels).
 *   - Attack/Release smoothing on linear RMS envelope.
 *   - Convert to dBFS with a floor clamp.
 *
 * Output:
 *   - A single boost value in dB per block (smoothed in dB domain).
 *
 * ASCII-only comments (SHIFT-JIS compatible).
 */


//===========================================================
// INCLUDES
//===========================================================




//===========================================================
// Definition
//===========================================================




//===========================================================
// Enum & Struct typedef
//===========================================================

/* Main state/parameter struct */
typedef struct {
    /* Buffer layout */
    int   in_buf_ch;        /* channel(slot) per Fs of buffer */
    float sampleRate;       /* Sampling rate [Hz] */

    /* Detector (linear envelope on RMS) */
    float att_ms;           /* Attack time [ms] (rise speed of envelope) */
    float rel_ms;           /* Release time [ms] (fall speed of envelope) */
    float floor_dBFS;       /* Floor for dBFS conversion (e.g., -96.0) */
    float env_lin;          /* Internal: smoothed linear RMS envelope */

    /* Intuitive mapping range (dBFS) */
    float low_dBFS;         /* Level at/below which boost is maxBoost_dB */
    float high_dBFS;        /* Level at/above which boost is 0 dB */

    /* Boost range */
    float maxBoost_dB;      /* Upper boost limit (e.g., +12.0) */

    /* Boost smoothing (dB domain, asymmetric) */
    float gainSmoothRise_ms;/* Faster rise (small ms -> follow up quickly) */
    float gainSmoothFall_ms;/* Slower fall (larger ms -> natural decay) */
    float currBoost_dB;     /* Internal: current smoothed boost dB */

    /* Optional bypass threshold (dB) for downstream EQ */
    float bypassThresh_dB;  /* If abs(boost) < thresh, you may bypass EQ */


    // debug only
    float DBG_level_dBFS;

} simple_loudmeter_t;




//===========================================================
// Variables
//===========================================================

extern float              Boost_dB;
extern simple_loudmeter_t Slm;



//===========================================================
// Function Prototype
//===========================================================

extern void  app_loudmeter_init(void);
extern void  app_loudmeter_process(const float* in);




extern void  simple_loudmeter_init(simple_loudmeter_t* m, float fs, int in_buf_ch);
extern float simple_loudmeter_process(simple_loudmeter_t* m, const float* in);


#endif	//!_SIMPLE_LOUD_METER_H

