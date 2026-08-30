#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "avas_synth_type_ty.h"
#include "avas_synth_type_lb.h"
#include "pinger_synth.h"
#include "clickclack_synth.h"
#include "kinkon_synth.h"
#include "engine_select.h"


#include "fx_domain_48k.h"




//===========================================================
// Definition
//===========================================================

#define FX_DOMAIN_FRAC_BITS       (16u)
#define FX_DOMAIN_ONE_Q16         (1UL << FX_DOMAIN_FRAC_BITS)
#define FX_DOMAIN_FRAC_MASK       (FX_DOMAIN_ONE_Q16 - 1UL)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    uint32_t output_sample_rate_Hz;

    uint32_t phase_q16;
    uint32_t step_q16;

    float s0;
    float s1;
    bool  src_valid;

} fx_domain_48k_t;


//===========================================================
// Variables
//===========================================================

static fx_domain_48k_t g_fx_domain_48k;


//===========================================================
// Local Function
//===========================================================

static inline uint32_t local_get_valid_sample_rate(uint32_t sample_rate_Hz)
{
    if( sample_rate_Hz != 0u )
    {
        return sample_rate_Hz;
    }

    /* Fallback only. Normal operation should pass sample_rate_Hz via init. */
    return (uint32_t)SAMPLE_RATE;
}


static inline uint32_t local_calc_step_q16(uint32_t source_sample_rate_Hz,
                                           uint32_t output_sample_rate_Hz)
{
    uint64_t step_q16;

    source_sample_rate_Hz = local_get_valid_sample_rate(source_sample_rate_Hz);
    output_sample_rate_Hz = local_get_valid_sample_rate(output_sample_rate_Hz);

    /*
     * phase_step = round(source_rate / output_rate * 65536)
     */
    step_q16  = ((uint64_t)source_sample_rate_Hz << FX_DOMAIN_FRAC_BITS);
    step_q16 += ((uint64_t)output_sample_rate_Hz / 2ULL);
    step_q16 /= (uint64_t)output_sample_rate_Hz;

    if( step_q16 == 0ULL )
    {
        step_q16 = 1ULL;
    }

    if( step_q16 > 0xFFFFFFFFULL )
    {
        step_q16 = 0xFFFFFFFFULL;
    }

    return (uint32_t)step_q16;
}




static inline float fx_domain_48k_process_sample(fx_domain_48k_t *fx)
{
    float y = 0.0f;

    (void)fx;

    // EngineSynth and AVAS are exclusive because of processing load.
    #if defined(ENA_ENGINE_SYNTH)
    if( app_engine_synth_is_enable() )
    {
        y += app_engine_synth_process_sample_48k();
    }
    else
    #endif
    {
        /* Both AVAS sources are called unconditionally; they are kept exclusive
         * at run time by classic_controls (their loads would add up).  The idle
         * one returns 0 immediately from its fully-gated-off early-out. */
        #if defined(ENA_AVAS_TYPE_TY_SYNTH)
        y += app_avas_type_ty_process_sample_48k();
        #endif
        #if defined(ENA_AVAS_TYPE_LB_SYNTH)
        y += app_avas_type_lb_process_sample_48k();
        #endif
    }

    #if defined(ENA_PINGER_SOUND)
    y += app_pinger_process_sample_48k();
    #endif

    #if defined(ENA_CLICK_CLACK)
    y += app_clickclack_process_sample_48k();
    #endif

    #if defined(ENA_KINKON)
    y += app_kinkon_process_sample_48k();
    #endif

    /*
     * Future 48 kHz FX sources can be added here.
     * Each source owns its own enable/gate state and returns 0.0f when silent.
     */

    return y;
}


static inline void fx_domain_48k_src_reset(fx_domain_48k_t *fx)
{
    fx->phase_q16 = 0u;
    fx->s0        = 0.0f;
    fx->s1        = 0.0f;
    fx->src_valid = false;
}


static inline void fx_domain_48k_src_prime(fx_domain_48k_t *fx)
{
    fx->phase_q16 = 0u;
    fx->s0        = fx_domain_48k_process_sample(fx);
    fx->s1        = fx_domain_48k_process_sample(fx);
    fx->src_valid = true;
}


static inline float fx_domain_48k_src_read(fx_domain_48k_t *fx)
{
    uint32_t frac_q16;
    float    frac;
    float    y;

    if( !fx->src_valid )
    {
        fx_domain_48k_src_prime(fx);
    }

    while( fx->phase_q16 >= FX_DOMAIN_ONE_Q16 )
    {
        fx->s0 = fx->s1;
        fx->s1 = fx_domain_48k_process_sample(fx);
        fx->phase_q16 -= FX_DOMAIN_ONE_Q16;
    }

    frac_q16 = fx->phase_q16 & FX_DOMAIN_FRAC_MASK;
    frac     = (float)frac_q16 * (1.0f / (float)FX_DOMAIN_ONE_Q16);

    y = fx->s0 + ((fx->s1 - fx->s0) * frac);

    fx->phase_q16 += fx->step_q16;

    return y;
}


static inline void fx_domain_48k_add_to_sample(const float *in,
                                                   float       *out,
                                                   uint16_t     n,
                                                   float        y)
{
#if (STAGE_1_PROC_CH == 2)
    const uint32_t idx0 = (uint32_t)n;
    const uint32_t idx1 = ((uint32_t)APP_BLOCK_FRAMES) + (uint32_t)n;

    out[idx0] = in[idx0] + y;
    out[idx1] = in[idx1] + y;
#else
    for( uint16_t ch = 0; ch < (uint16_t)STAGE_1_PROC_CH; ch++ )
    {
        uint32_t idx = ((uint32_t)ch * (uint32_t)APP_BLOCK_FRAMES) + (uint32_t)n;
        out[idx] = in[idx] + y;
    }
#endif
}


static inline void fx_domain_48k_advance_src(fx_domain_48k_t *fx)
{
    fx->s0 = fx->s1;
    fx->s1 = fx_domain_48k_process_sample(fx);
    fx->phase_q16 -= FX_DOMAIN_ONE_Q16;
}


static void fx_domain_48k_process_48k_fast(float *in, float *out)
{
    fx_domain_48k_t *fx = &g_fx_domain_48k;

    if( !fx->src_valid )
    {
        fx_domain_48k_src_prime(fx);
    }

    for( uint16_t n = 0; n < (uint16_t)APP_BLOCK_FRAMES; n++ )
    {
        float y;

        if( fx->phase_q16 >= FX_DOMAIN_ONE_Q16 )
        {
            fx_domain_48k_advance_src(fx);
        }

        /*
         * 48 kHz source -> 48 kHz output:
         * No interpolation is required. Keep the same one-sample look-ahead
         * state style as the generic SRC path, but avoid frac calculation.
         */
        y = fx->s0;
        fx->phase_q16 += FX_DOMAIN_ONE_Q16;

        fx_domain_48k_add_to_sample(in, out, n, y);
    }
}


static void fx_domain_48k_process_96k_fast(float *in, float *out)
{
    fx_domain_48k_t *fx = &g_fx_domain_48k;
    const uint32_t half_q16 = (FX_DOMAIN_ONE_Q16 >> 1);

    if( !fx->src_valid )
    {
        fx_domain_48k_src_prime(fx);
    }

    for( uint16_t n = 0; n < (uint16_t)APP_BLOCK_FRAMES; n++ )
    {
        float y;

        if( fx->phase_q16 >= FX_DOMAIN_ONE_Q16 )
        {
            fx_domain_48k_advance_src(fx);
        }

        /*
         * 48 kHz source -> 96 kHz output:
         * step_q16 is exactly 0.5 sample. The generic linear SRC output is:
         *   s0, (s0+s1)/2, s1, (s1+s2)/2, ...
         */
        if( fx->phase_q16 == 0u )
        {
            y = fx->s0;
        }
        else
        {
            y = 0.5f * (fx->s0 + fx->s1);
        }

        fx->phase_q16 += half_q16;

        fx_domain_48k_add_to_sample(in, out, n, y);
    }
}


//===========================================================
// API
//===========================================================

void app_fx_domain_48k_init(uint32_t sample_rate_Hz)
{
    uint32_t valid_sample_rate_Hz = local_get_valid_sample_rate(sample_rate_Hz);

    memset(&g_fx_domain_48k, 0, sizeof(g_fx_domain_48k));

    g_fx_domain_48k.output_sample_rate_Hz = valid_sample_rate_Hz;
    g_fx_domain_48k.step_q16              = local_calc_step_q16(FX_DOMAIN_48K_SAMPLE_RATE_HZ,
                                                                valid_sample_rate_Hz);
    fx_domain_48k_src_reset(&g_fx_domain_48k);


    #if defined(ENA_ENGINE_SYNTH)
    app_engine_synth_init_48k();
    #endif //defined(ENA_ENGINE_SYNTH)

    #if defined(ENA_AVAS_TYPE_TY_SYNTH)
    app_avas_type_ty_init_48k();
    #endif //defined(ENA_AVAS_TYPE_TY_SYNTH)

    #if defined(ENA_AVAS_TYPE_LB_SYNTH)
    app_avas_type_lb_init_48k();
    #endif //defined(ENA_AVAS_TYPE_LB_SYNTH)

    #if defined(ENA_PINGER_SOUND)
    app_pinger_init_48k();
    #endif //defined(ENA_PINGER_SOUND)

    #if defined(ENA_CLICK_CLACK)
    app_clickclack_init_48k();
    #endif //defined(ENA_CLICK_CLACK)

    #if defined(ENA_KINKON)
    app_kinkon_init_48k();
    #endif //defined(ENA_KINKON)
}


void app_fx_domain_48k_process(float *in, float *out)
{
    /*
     * Fast paths for the two expected rates:
     * - 48 kHz output: direct 48 kHz FX sample add
     * - 96 kHz output: 2x linear interpolation without generic frac math
     *
     * Keep the generic SRC path for other rates.
     */
    if( g_fx_domain_48k.step_q16 == FX_DOMAIN_ONE_Q16 )
    {
        fx_domain_48k_process_48k_fast(in, out);
        return;
    }

    if( g_fx_domain_48k.step_q16 == (FX_DOMAIN_ONE_Q16 >> 1) )
    {
        fx_domain_48k_process_96k_fast(in, out);
        return;
    }

    for( uint16_t n = 0; n < (uint16_t)APP_BLOCK_FRAMES; n++ )
    {
        float y = fx_domain_48k_src_read(&g_fx_domain_48k);

        fx_domain_48k_add_to_sample(in, out, n, y);
    }
}
