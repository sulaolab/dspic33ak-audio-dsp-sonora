#ifndef SONORA_ASRC_RATE_PLAN_H
#define SONORA_ASRC_RATE_PLAN_H

#include <stdint.h>

/*
 * Fixed-rate stages before an ASRC are represented by their exact rational
 * output/input rate L/M.  The ASRC step is input frames advanced per output
 * frame, so its feed-forward value is:
 *
 *   step_ff = measured_source_fs / measured_destination_fs * L / M
 *
 * The measured clock ratio remains responsible for oscillator mismatch; the
 * rational plan describes only deterministic rate changes before the ASRC.
 */
typedef struct
{
    uint32_t fixed_output_num; /* L: fixed-stage output-rate numerator */
    uint32_t fixed_input_den;  /* M: fixed-stage input-rate denominator */
} asrc_rate_plan_t;

static inline float asrc_rate_plan_step( float measured_source_per_destination,
                                         const asrc_rate_plan_t* plan )
{
    if( ( plan == 0 ) ||
        ( measured_source_per_destination <= 0.0f ) ||
        ( plan->fixed_output_num == 0u ) ||
        ( plan->fixed_input_den == 0u ) )
    {
        return 0.0f;
    }

    return ( measured_source_per_destination * (float)plan->fixed_output_num ) /
           (float)plan->fixed_input_den;
}

#endif /* SONORA_ASRC_RATE_PLAN_H */
