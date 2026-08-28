#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#include <stdint.h>
#include <stddef.h>

#include "flip4_keepalive.h"

#if defined(ENA_FLIP4_KEEPALIVE)

/*
 * JBL Flip4 auto-mute workaround.
 *
 * Keep this module intentionally small and product-specific.
 * No enable/disable.
 * No frequency API.
 * No phase API.
 * No sine calculation.
 */

#define FLIP4_KEEPALIVE_TARGET_HZ   (24000UL)
#define FLIP4_KEEPALIVE_MIN_HALF    (1U)

static uint16_t s_half_period_samples = 1U;
static uint16_t s_half_count          = 0U;
static int8_t   s_sign                = 1;

static uint16_t local_calc_half_period(uint32_t sample_rate_Hz)
{
    uint32_t half_period;

    if (sample_rate_Hz == 0U)
    {
        return FLIP4_KEEPALIVE_MIN_HALF;
    }

    /*
     * half period [samples] = Fs / (2 * target_freq)
     *
     * 48 kHz -> 1 sample
     * 96 kHz -> 2 samples
     *
     * Rounded to nearest integer.
     */
    half_period = (sample_rate_Hz + FLIP4_KEEPALIVE_TARGET_HZ)
                / (2UL * FLIP4_KEEPALIVE_TARGET_HZ);

    if (half_period < FLIP4_KEEPALIVE_MIN_HALF)
    {
        half_period = FLIP4_KEEPALIVE_MIN_HALF;
    }

    if (half_period > 65535UL)
    {
        half_period = 65535UL;
    }

    return (uint16_t)half_period;
}

void flip4_keepalive_init(uint32_t sample_rate_Hz)
{
    s_half_period_samples = local_calc_half_period(sample_rate_Hz);
    s_half_count          = 0U;
    s_sign                = 1;
}

void flip4_keepalive_process(const float* in,
                                 float*       out,
                                 int          num_proc_ch,
                                 int          samples)
{
    const float gain = FLIP4_KEEPALIVE_GAIN_LIN;

    uint16_t half0;
    uint16_t count0;
    int8_t   sign0;

    uint16_t next_count = 0U;
    int8_t   next_sign  = 1;

    if ((in == NULL) || (out == NULL) || (num_proc_ch <= 0) || (samples <= 0))
    {
        return;
    }

    half0  = s_half_period_samples;
    count0 = s_half_count;
    sign0  = s_sign;

    /*
     * Fast path for 48 kHz Fs:
     *   +gain, -gain, +gain, -gain, ...
     */
    if (half0 <= 1U)
    {
        for (int ch = 0; ch < num_proc_ch; ch++)
        {
            const float* src  = &in [ch * samples];
            float*       dst  = &out[ch * samples];
            int8_t       sign = sign0;

            for (int n = 0; n < samples; n++)
            {
                dst[n] = src[n] + ((sign > 0) ? gain : -gain);
                sign = (int8_t)-sign;
            }

            if (ch == 0)
            {
                next_sign = sign;
            }
        }

        s_sign       = next_sign;
        s_half_count = 0U;
        return;
    }

    /*
     * Generic path for 96 kHz or other sample rates:
     *   96 kHz Fs -> +gain, +gain, -gain, -gain, ...
     */
    for (int ch = 0; ch < num_proc_ch; ch++)
    {
        const float* src   = &in [ch * samples];
        float*       dst   = &out[ch * samples];
        uint16_t     count = count0;
        int8_t       sign  = sign0;

        for (int n = 0; n < samples; n++)
        {
            dst[n] = src[n] + ((sign > 0) ? gain : -gain);

            count++;
            if (count >= half0)
            {
                count = 0U;
                sign  = (int8_t)-sign;
            }
        }

        if (ch == 0)
        {
            next_count = count;
            next_sign  = sign;
        }
    }

    s_half_count = next_count;
    s_sign       = next_sign;
}

#endif /* defined(ENA_FLIP4_KEEPALIVE) */
