#ifndef _AUDIO_FAST_MATH_H
#define _AUDIO_FAST_MATH_H


//===========================================================
// INCLUDES
//===========================================================
#include "app_specific_config_defs.h"
#include <stdint.h>
#include <stdbool.h>





//===========================================================
// Definition
//===========================================================

#if !defined(AUDIO_FAST_MATH_PI)
#define AUDIO_FAST_MATH_PI             (3.14159265358979323846f)
#endif //!defined(AUDIO_FAST_MATH_PI)

#define AUDIO_FAST_MATH_TWO_PI         (2.0f * AUDIO_FAST_MATH_PI)
#define AUDIO_FAST_MATH_HALF_PI        (0.5f * AUDIO_FAST_MATH_PI)
#define AUDIO_FAST_MATH_INV_TWO_PI     (1.0f / AUDIO_FAST_MATH_TWO_PI)





//===========================================================
// Local Function
//===========================================================

static inline float audio_fast_absf(float x)
{
    return (x < 0.0f) ? -x : x;
}


static inline float audio_fast_wrap_0_to_2pi(float phase_rad)
{
    if( phase_rad >= AUDIO_FAST_MATH_TWO_PI )
    {
        phase_rad -= AUDIO_FAST_MATH_TWO_PI;
    }
    else if( phase_rad < 0.0f )
    {
        phase_rad += AUDIO_FAST_MATH_TWO_PI;
    }

    return phase_rad;
}


static inline float audio_fast_sinf_mpi_to_pi(float phase_rad)
{
    /*
     * Fast sine approximation for -pi..+pi.
     * This is intended for audio oscillators/LFOs, not for coefficient
     * generation that requires strict libm precision.
     */
    const float b =  4.0f / AUDIO_FAST_MATH_PI;
    const float c = -4.0f / (AUDIO_FAST_MATH_PI * AUDIO_FAST_MATH_PI);
    const float p =  0.225f;

    float y = (b * phase_rad) + (c * phase_rad * audio_fast_absf(phase_rad));
    y = (p * ((y * audio_fast_absf(y)) - y)) + y;

    return y;
}


static inline float audio_fast_sinf_0_to_2pi(float phase_rad)
{
    /*
     * Fastest path for oscillator phases already wrapped to 0..2pi.
     */
    if( phase_rad > AUDIO_FAST_MATH_PI )
    {
        phase_rad -= AUDIO_FAST_MATH_TWO_PI;
    }

    return audio_fast_sinf_mpi_to_pi(phase_rad);
}


static inline float audio_fast_sinf(float phase_rad)
{
    /*
     * Lightweight range reduction for normal audio-control phase values.
     * This is not a full libm replacement for huge arbitrary inputs.
     */
    int32_t k = (int32_t)(phase_rad * AUDIO_FAST_MATH_INV_TWO_PI);

    phase_rad -= (float)k * AUDIO_FAST_MATH_TWO_PI;

    if( phase_rad < 0.0f )
    {
        phase_rad += AUDIO_FAST_MATH_TWO_PI;
    }
    else if( phase_rad >= AUDIO_FAST_MATH_TWO_PI )
    {
        phase_rad -= AUDIO_FAST_MATH_TWO_PI;
    }

    return audio_fast_sinf_0_to_2pi(phase_rad);
}


static inline float audio_fast_cosf_0_to_2pi(float phase_rad)
{
    phase_rad += AUDIO_FAST_MATH_HALF_PI;

    if( phase_rad >= AUDIO_FAST_MATH_TWO_PI )
    {
        phase_rad -= AUDIO_FAST_MATH_TWO_PI;
    }

    return audio_fast_sinf_0_to_2pi(phase_rad);
}


static inline float audio_fast_cosf(float phase_rad)
{
    return audio_fast_sinf(phase_rad + AUDIO_FAST_MATH_HALF_PI);
}


#endif //!_AUDIO_FAST_MATH_H
