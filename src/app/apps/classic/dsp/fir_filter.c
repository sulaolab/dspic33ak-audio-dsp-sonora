#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include "nora_high_res_timer.h"

#include "fir_filter.h"


#if defined(ENA_FIR_FILTER)
//===========================================================
// Definition
//===========================================================

// This module applies a normal FIR filter to channel-major float audio.
//
// Buffer layout:
//   in[0 * samples + n] = ch0 sample n
//   in[1 * samples + n] = ch1 sample n
//   in[2 * samples + n] = ch2 sample n
//   ...
//
// FIR form:
//   y[n] = b[0] * x[n] + b[1] * x[n-1] + ... + b[M-1] * x[n-M+1]
//
// Coefficient rule:
//   coeff[0] is applied to the current input sample x[n].
//   coeff[1] is applied to the previous input sample x[n-1].
//   This is intentionally aligned with the current MCHP FIR f32 ASM behavior.
//
// Bypass coefficient:
//   { 1.0f }


//===========================================================
// Enum & Struct typedef
//===========================================================

#if defined(USE_CMSIS_FIR_DSP_PROCESS) || defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
typedef struct
{
    uint32_t numTaps;
    float*   pCoeffs;
    float*   pState;
    float*   pStateStart;
} fir_filter_cmsis_instance_f32_t;
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS) || defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)


//===========================================================
// Function Prototype
//===========================================================

static void     fir_filter_clear_state_only( fir_filter_t* pfir );
static void     fir_filter_set_default_bypass( fir_filter_t* pfir );
static const float* fir_filter_get_smoke_test_coeff( fir_filter_smoke_test_t smoke_test,
                                                     uint16_t*               num_taps );
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
static void     fir_filter_init_cmsis_instances( const fir_filter_t* pfir );
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)

#if defined(USE_CMSIS_FIR_DSP_PROCESS)
static void     fir_filter_process_cmsis(       fir_filter_t* pfir,
                                             const float*        in,
                                                   float*        out,
                                                   uint16_t      samples );
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)

#if defined(USE_CMSIS_FIR_DSP_PROCESS) || defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
extern void     mchp_fir_init_f32_ex( fir_filter_cmsis_instance_f32_t* S,
                                   uint32_t                         numTaps,
                                   float*                           pCoeffs,
                                   float*                           pState );
extern void     mchp_fir_f32_ex( fir_filter_cmsis_instance_f32_t* S,
                              const float*                     pSrc,
                              float*                           pDst,
                              uint32_t                         blockSize );
extern void     mchp_fir_f32_ex_opt_v1( fir_filter_cmsis_instance_f32_t* S,
                                        const float*                     pSrc,
                                        float*                           pDst,
                                        uint32_t                         blockSize );
extern void     mchp_fir_f32_ex_opt_v2( fir_filter_cmsis_instance_f32_t* S,
                                        const float*                     pSrc,
                                        float*                           pDst,
                                        uint32_t                         blockSize );
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS) || defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)


//===========================================================
// Variables
//===========================================================

static const float fir_filter_smoke_test_bypass_coeff[1] =
{
    1.0f
};


static const float fir_filter_smoke_test_moving_average_8_coeff[8] =
{
    0.125f, 0.125f, 0.125f, 0.125f,
    0.125f, 0.125f, 0.125f, 0.125f
};


static const float fir_filter_smoke_test_moving_average_16_coeff[16] =
{
    0.0625f, 0.0625f, 0.0625f, 0.0625f,
    0.0625f, 0.0625f, 0.0625f, 0.0625f,
    0.0625f, 0.0625f, 0.0625f, 0.0625f,
    0.0625f, 0.0625f, 0.0625f, 0.0625f
};


static const float fir_filter_smoke_test_moving_average_32_coeff[32] =
{
    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f,

    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f,
    0.03125f, 0.03125f, 0.03125f, 0.03125f
};


static const float fir_filter_smoke_test_moving_average_64_coeff[64] =
{
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,

    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,

    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,

    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f,
    0.015625f, 0.015625f, 0.015625f, 0.015625f
};


static const float fir_filter_smoke_test_edge_2_coeff[2] =
{
    0.5f, -0.45f
};


static const float fir_filter_smoke_test_delay_mix_16_coeff[16] =
{
    0.7f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.3f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f
};


static const float fir_filter_smoke_test_delay_mix_32_coeff[32] =
{
    0.7f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,

    0.3f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f
};


#if defined(USE_CMSIS_FIR_DSP_PROCESS)
static fir_filter_cmsis_instance_f32_t g_fir_filter_cmsis_inst[FIR_FILTER_MAX_CH] __attribute__((aligned(4)));
static float g_fir_filter_cmsis_coeff[FIR_FILTER_MAX_TAPS] __attribute__((section(".xbss"), aligned(1024)));
static float g_fir_filter_cmsis_state[FIR_FILTER_MAX_CH][FIR_FILTER_MAX_TAPS] __attribute__((section(".ybss"), aligned(1024)));
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)

volatile uint32_t g_fir_filter_process_dt       = 0u;
volatile uint16_t g_fir_filter_process_num_ch   = 0u;
volatile uint16_t g_fir_filter_process_num_taps = 0u;


//===========================================================
// Global Function
//===========================================================

/**
 * @brief Initialize a FIR filter instance.
 *
 * Behavior:
 *   - Sets the default configuration to 4ch bypass.
 *   - Clears all delay states.
 *
 * @param pfir Pointer to @c fir_filter_t instance.
 */
void fir_filter_init( fir_filter_t* pfir )
{
    if (pfir == NULL)
    {
        return;
    }

    fir_filter_set_default_bypass(pfir);
    fir_filter_clear_state_only(pfir);
}


/**
 * @brief Clear all FIR delay states.
 *
 * Coefficients, num_ch, and num_taps are not changed.
 *
 * @param pfir Pointer to @c fir_filter_t instance.
 */
void fir_filter_reset( fir_filter_t* pfir )
{
    if (pfir == NULL)
    {
        return;
    }

    fir_filter_clear_state_only(pfir);
}


/**
 * @brief Set FIR coefficients and active channel count.
 *
 * Coefficient order:
 *   coeff[0] = b[0] for x[n]
 *   coeff[1] = b[1] for x[n-1]
 *   ...
 *
 * State is cleared after accepting the new configuration.  This avoids
 * carrying old delay samples across coefficient or tap-count changes.
 *
 * @param pfir     Pointer to @c fir_filter_t instance.
 * @param coeff    Coefficient array in natural FIR order.
 * @param num_taps Number of active taps.
 * @param num_ch   Number of active channel-major channels.
 *
 * @return true if accepted, otherwise false.
 */
bool fir_filter_set_config( fir_filter_t* pfir,
                            const float*  coeff,
                            uint16_t      num_taps,
                            uint16_t      num_ch )
{
    if (pfir == NULL)
    {
        return false;
    }

    if (coeff == NULL)
    {
        return false;
    }

    if ((num_taps == 0u) || (num_taps > FIR_FILTER_MAX_TAPS))
    {
        return false;
    }

    if ((num_ch == 0u) || (num_ch > FIR_FILTER_MAX_CH))
    {
        return false;
    }

    for (uint16_t tap = 0u; tap < num_taps; tap++)
    {
        if (!isfinite(coeff[tap]))
        {
            return false;
        }
    }

    pfir->num_ch   = num_ch;
    pfir->num_taps = num_taps;

    for (uint16_t tap = 0u; tap < FIR_FILTER_MAX_TAPS; tap++)
    {
        if (tap < num_taps)
        {
            pfir->coeff[tap] = coeff[tap];
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
            g_fir_filter_cmsis_coeff[tap] = coeff[tap];
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)
        }
        else
        {
            pfir->coeff[tap] = 0.0f;
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
            g_fir_filter_cmsis_coeff[tap] = 0.0f;
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)
        }
    }

    fir_filter_clear_state_only(pfir);

#if defined(USE_CMSIS_FIR_DSP_PROCESS)
    fir_filter_init_cmsis_instances(pfir);
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)

    return true;
}


/**
 * @brief Get current FIR coefficients and active channel count.
 *
 * @param pfir             Pointer to @c fir_filter_t instance.
 * @param coeff            Destination coefficient array.
 * @param coeff_array_size Number of float entries in @p coeff.
 * @param num_taps         Destination for active tap count.
 * @param num_ch           Destination for active channel count.
 *
 * @return true if all requested information was copied, otherwise false.
 */
bool fir_filter_get_config( const fir_filter_t* pfir,
                            float*              coeff,
                            uint16_t            coeff_array_size,
                            uint16_t*           num_taps,
                            uint16_t*           num_ch )
{
    if (pfir == NULL)
    {
        return false;
    }

    if (num_taps != NULL)
    {
        *num_taps = pfir->num_taps;
    }

    if (num_ch != NULL)
    {
        *num_ch = pfir->num_ch;
    }

    if (coeff != NULL)
    {
        if (coeff_array_size < pfir->num_taps)
        {
            return false;
        }

        for (uint16_t tap = 0u; tap < pfir->num_taps; tap++)
        {
            coeff[tap] = pfir->coeff[tap];
        }
    }

    return true;
}


/**
 * @brief Apply FIR filter to channel-major float audio.
 *
 * Input and output are channel-major buffers.
 *
 * Buffer layout:
 *   in[0 * samples + n] = ch0 sample n
 *   in[1 * samples + n] = ch1 sample n
 *   ...
 *
 * In-place processing is allowed, i.e. @p in and @p out may point to
 * the same buffer.
 *
 * @param pfir    Pointer to @c fir_filter_t instance.
 * @param in  Channel-major input buffer [num_ch][samples].
 * @param out Channel-major output buffer [num_ch][samples].
 * @param samples Number of samples per channel.
 */
void fir_filter_process(       fir_filter_t* pfir,
                             const float*        in,
                                   float*        out,
                                   uint16_t      samples )
{
    if ((pfir == NULL) || (in == NULL) || (out == NULL) || (samples == 0u))
    {
        return;
    }

    if ((pfir->num_ch == 0u) || (pfir->num_ch > FIR_FILTER_MAX_CH))
    {
        return;
    }

    if ((pfir->num_taps == 0u) || (pfir->num_taps > FIR_FILTER_MAX_TAPS))
    {
        return;
    }

    for (uint16_t ch = 0u; ch < pfir->num_ch; ch++)
    {
        const float* p_in  = &in [((uint32_t)ch * (uint32_t)samples)];
              float* p_out = &out[((uint32_t)ch * (uint32_t)samples)];

        uint16_t write_index = pfir->write_index[ch];

        if (write_index >= pfir->num_taps)
        {
            write_index = 0u;
        }

        for (uint16_t sample_idx = 0u; sample_idx < samples; sample_idx++)
        {
            const float x = *p_in++;

            pfir->state[ch][write_index] = x;

            float acc = 0.0f;
            uint16_t state_index = write_index;

            for (uint16_t tap = 0u; tap < pfir->num_taps; tap++)
            {
                acc += pfir->coeff[tap] * pfir->state[ch][state_index];

                if (state_index == 0u)
                {
                    state_index = (uint16_t)(pfir->num_taps - 1u);
                }
                else
                {
                    state_index--;
                }
            }

            *p_out++ = acc;

            write_index++;
            if (write_index >= pfir->num_taps)
            {
                write_index = 0u;
            }
        }

        pfir->write_index[ch] = write_index;
    }
}
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
static void fir_filter_process_cmsis(       fir_filter_t* pfir,
                                          const float*        in,
                                                float*        out,
                                                uint16_t      samples )
{
    if ((pfir == NULL) || (in == NULL) || (out == NULL) || (samples == 0u))
    {
        return;
    }

    if ((pfir->num_ch == 0u) || (pfir->num_ch > FIR_FILTER_MAX_CH))
    {
        return;
    }

    if ((pfir->num_taps == 0u) || (pfir->num_taps > FIR_FILTER_MAX_TAPS))
    {
        return;
    }

    /*
     * The original MCHP FIR ASM expects M >= 2 because its inner loop uses
     * M-2.  Keep bypass/tap=1 safe by using the C reference path.
     */
    if (pfir->num_taps < 2u)
    {
        fir_filter_process(pfir,
                               in,
                               out,
                               samples);
        return;
    }

    /*
     * Original MCHP/CMSIS FIR bring-up path.
     *
     * This path uses mchp_fir_f32_ex.s / mchp_fir_init_f32_ex.s.
     * The EX instance has pStateStart, so each channel can keep its own
     * modulo start address and pState can continue across blocks.
     */
    for (uint16_t ch = 0u; ch < pfir->num_ch; ch++)
    {
        const float* p_src = &in [((uint32_t)ch * (uint32_t)samples)];
              float* p_dst = &out[((uint32_t)ch * (uint32_t)samples)];
#if 1
        mchp_fir_f32_ex_opt_v4_tail_v1(&g_fir_filter_cmsis_inst[ch],
                                p_src,
                                p_dst,
                                (uint32_t)samples);
#else
        mchp_fir_f32_ex_opt_v4(&g_fir_filter_cmsis_inst[ch],
                                p_src,
                                p_dst,
                                (uint32_t)samples);
#endif //01
    }
}
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)




/**
 * @brief Apply one of the built-in FIR smoke-test coefficient sets.
 *
 * These coefficient sets are intended for quick bring-up and listening checks.
 * They are not intended as final audio tuning coefficients.
 *
 * Available tests:
 *   - FIR_FILTER_SMOKE_TEST_BYPASS
 *       Pass-through.
 *   - FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_8
 *       Clearly audible high-frequency roll-off.
 *   - FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_16
 *       Stronger high-frequency roll-off.
 *   - FIR_FILTER_SMOKE_TEST_EDGE_2
 *       High-pass-ish / edge-emphasis check.  Use low volume first.
 *   - FIR_FILTER_SMOKE_TEST_DELAY_MIX_16
 *       Current sample plus 8-sample delayed mix.
 *   - FIR_FILTER_SMOKE_TEST_DELAY_MIX_32
 *       Current sample plus 16-sample delayed mix.
 *
 * @param pfir       Pointer to @c fir_filter_t instance.
 * @param smoke_test Smoke-test coefficient selector.
 * @param num_ch     Number of active channel-major channels.
 *
 * @return true if accepted, otherwise false.
 */
bool fir_filter_apply_smoke_test( fir_filter_t*            pfir,
                                  fir_filter_smoke_test_t smoke_test,
                                  uint16_t                num_ch )
{
    uint16_t num_taps = 0u;
    const float* coeff = fir_filter_get_smoke_test_coeff(smoke_test, &num_taps);

    if (coeff == NULL)
    {
        return false;
    }

    return fir_filter_set_config(pfir,
                                 coeff,
                                 num_taps,
                                 num_ch);
}


/**
 * @brief Small impulse self-test for coefficient order and channel isolation.
 *
 * Fixed condition:
 *   coeff = { 0.1, 0.2, 0.3, 0.4 }
 *   ch0 impulse = 1.0
 *   ch1 impulse = 0.5
 *
 * Expected output:
 *   ch0 = { 0.1, 0.2, 0.3, 0.4, 0, ... }
 *   ch1 = { 0.05, 0.1, 0.15, 0.2, 0, ... }
 *
 * @return Maximum absolute difference.
 */
float fir_filter_selftest(void)
{
//#define SELFTEST_NUM_CH         (2u)
#define SELFTEST_NUM_CH         (4u)
#define SELFTEST_NUM_TAPS       (4u)
#define SELFTEST_NUM_SAMPLE     (16u)

    static fir_filter_t test_fir;
    static const float selftest_coeff[SELFTEST_NUM_TAPS] =
    {
        0.1f, 0.2f, 0.3f, 0.4f
    };
    static float in [SELFTEST_NUM_CH * SELFTEST_NUM_SAMPLE];
    static float out[SELFTEST_NUM_CH * SELFTEST_NUM_SAMPLE];

    float max_abs_diff = 0.0f;

    for (uint16_t i = 0u; i < (SELFTEST_NUM_CH * SELFTEST_NUM_SAMPLE); i++)
    {
        in[i]  = 0.0f;
        out[i] = 0.0f;
    }

    in[(0u * SELFTEST_NUM_SAMPLE) + 0u] = 1.0f;
    in[(1u * SELFTEST_NUM_SAMPLE) + 0u] = 0.5f;

    fir_filter_init(&test_fir);
    (void)fir_filter_set_config(&test_fir,
                                selftest_coeff,
                                SELFTEST_NUM_TAPS,
                                SELFTEST_NUM_CH);

    fir_filter_process(&test_fir,
                           in,
                           out,
                           SELFTEST_NUM_SAMPLE);

    for (uint16_t ch = 0u; ch < SELFTEST_NUM_CH; ch++)
    {
        for (uint16_t sample_idx = 0u; sample_idx < SELFTEST_NUM_SAMPLE; sample_idx++)
        {
            float expected = 0.0f;

            if (sample_idx < SELFTEST_NUM_TAPS)
            {
                const float impulse = (ch == 0u) ? 1.0f : 0.5f;
                expected = impulse * selftest_coeff[sample_idx];
            }

            const float diff = fabsf(out[(ch * SELFTEST_NUM_SAMPLE) + sample_idx] - expected);

            if (diff > max_abs_diff)
            {
                max_abs_diff = diff;
            }
        }
    }

    return max_abs_diff;

#undef SELFTEST_NUM_CH
#undef SELFTEST_NUM_TAPS
#undef SELFTEST_NUM_SAMPLE
}


#if defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
/**
 * @brief Compare the C FIR path and MCHP/CMSIS FIR EX ASM path.
 *
 * This self-test runs the C reference path and the CMSIS EX path over multiple
 * consecutive blocks, then returns the maximum absolute difference.
 *
 * This checks:
 *   - coefficient order
 *   - pState continuity across block boundaries
 *   - pStateStart per-instance behavior in mchp_fir_f32_ex.s
 *   - channel-major multi-channel independence
 *
 * Required project files when this test is enabled:
 *   mchp_fir_f32_ex.s
 *   mchp_fir_init_f32_ex.s
 *
 * @return Maximum absolute difference between C output and CMSIS EX output.
 */
float fir_filter_c_vs_cmsis_selftest(void)
{
#define SELFTEST_NUM_CH         (2u)
#define SELFTEST_NUM_TAPS       (16u)
#define SELFTEST_BLOCK_SIZE     (32u)
#define SELFTEST_NUM_BLOCKS     (8u)

    static fir_filter_t test_fir_c;

    static const float selftest_coeff[SELFTEST_NUM_TAPS] =
    {
        0.0700f, -0.0150f, 0.0320f, 0.1100f,
       -0.0210f,  0.0450f, 0.0080f, -0.0060f,
        0.0250f,  0.0130f, -0.0170f, 0.0040f,
        0.0310f, -0.0090f, 0.0180f, 0.0020f
    };

    static float in        [SELFTEST_NUM_CH * SELFTEST_BLOCK_SIZE];
    static float out_c     [SELFTEST_NUM_CH * SELFTEST_BLOCK_SIZE];
    static float out_cmsis [SELFTEST_NUM_CH * SELFTEST_BLOCK_SIZE];

    static fir_filter_cmsis_instance_f32_t cmsis_inst[SELFTEST_NUM_CH] __attribute__((aligned(4)));
    static float cmsis_coeff[SELFTEST_NUM_TAPS] __attribute__((section(".xbss"), aligned(64)));
    static float cmsis_state[SELFTEST_NUM_CH][SELFTEST_NUM_TAPS] __attribute__((section(".ybss"), aligned(64)));

    float max_abs_diff = 0.0f;

    for (uint16_t tap = 0u; tap < SELFTEST_NUM_TAPS; tap++)
    {
        cmsis_coeff[tap] = selftest_coeff[tap];
    }

    for (uint16_t ch = 0u; ch < SELFTEST_NUM_CH; ch++)
    {
        for (uint16_t tap = 0u; tap < SELFTEST_NUM_TAPS; tap++)
        {
            cmsis_state[ch][tap] = 0.0f;
        }
    }

    fir_filter_init(&test_fir_c);

    (void)fir_filter_set_config(&test_fir_c,
                                selftest_coeff,
                                SELFTEST_NUM_TAPS,
                                SELFTEST_NUM_CH);

    for (uint16_t ch = 0u; ch < SELFTEST_NUM_CH; ch++)
    {
        mchp_fir_init_f32_ex(&cmsis_inst[ch],
                             (uint32_t)SELFTEST_NUM_TAPS,
                             cmsis_coeff,
                             cmsis_state[ch]);
    }

    for (uint16_t block = 0u; block < SELFTEST_NUM_BLOCKS; block++)
    {
        for (uint16_t ch = 0u; ch < SELFTEST_NUM_CH; ch++)
        {
            for (uint16_t sample_idx = 0u; sample_idx < SELFTEST_BLOCK_SIZE; sample_idx++)
            {
                const uint16_t n = (uint16_t)((block * SELFTEST_BLOCK_SIZE) + sample_idx);
                const uint32_t idx = ((uint32_t)ch * SELFTEST_BLOCK_SIZE) + sample_idx;

                /*
                 * Deterministic non-impulse input.
                 * Use a different sequence per channel to verify ch-major
                 * channel independence and per-instance state continuity.
                 */
                in[idx] = ((n == 0u) ? (1.0f + (0.25f * (float)ch)) : 0.0f)
                            + (0.001f * (float)(((n + (ch * 3u)) % 17u) - 8u))
                            + (0.0001f * (float)((n + ch) & 0x0007u));

                out_c[idx]     = 0.0f;
                out_cmsis[idx] = 0.0f;
            }
        }

        fir_filter_process(&test_fir_c,
                               in,
                               out_c,
                               SELFTEST_BLOCK_SIZE);

        for (uint16_t ch = 0u; ch < SELFTEST_NUM_CH; ch++)
        {
            const float* p_src = &in[(uint32_t)ch * SELFTEST_BLOCK_SIZE];
                  float* p_dst = &out_cmsis[(uint32_t)ch * SELFTEST_BLOCK_SIZE];
#if 1
            mchp_fir_f32_ex_opt_v2(&cmsis_inst[ch],
                                    p_src,
                                    p_dst,
                                    (uint32_t)SELFTEST_BLOCK_SIZE);
#else
            mchp_fir_f32_ex_opt_v1(&cmsis_inst[ch],
                                    p_src,
                                    p_dst,
                                    (uint32_t)SELFTEST_BLOCK_SIZE);
#endif //01
        }

        for (uint16_t i = 0u; i < (SELFTEST_NUM_CH * SELFTEST_BLOCK_SIZE); i++)
        {
            const float diff = fabsf(out_c[i] - out_cmsis[i]);

            if (diff > max_abs_diff)
            {
                max_abs_diff = diff;
            }
        }
    }

    printf("FIR C vs CMSIS EX multi-block diff = %.12e", (double)max_abs_diff);

    return max_abs_diff;

#undef SELFTEST_NUM_CH
#undef SELFTEST_NUM_TAPS
#undef SELFTEST_BLOCK_SIZE
#undef SELFTEST_NUM_BLOCKS
}
#endif //defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)


// Local Function
//===========================================================

static void fir_filter_clear_state_only( fir_filter_t* pfir )
{
    if (pfir == NULL)
    {
        return;
    }

    for (uint16_t ch = 0u; ch < FIR_FILTER_MAX_CH; ch++)
    {
        pfir->write_index[ch] = 0u;

        for (uint16_t tap = 0u; tap < FIR_FILTER_MAX_TAPS; tap++)
        {
            pfir->state[ch][tap] = 0.0f;
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
            g_fir_filter_cmsis_state[ch][tap] = 0.0f;
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)
        }
    }
}


static void fir_filter_set_default_bypass( fir_filter_t* pfir )
{
    if (pfir == NULL)
    {
        return;
    }

    pfir->num_ch   = FIR_FILTER_DEFAULT_CH;
    pfir->num_taps = 1u;

    pfir->coeff[0] = 1.0f;

    for (uint16_t tap = 1u; tap < FIR_FILTER_MAX_TAPS; tap++)
    {
        pfir->coeff[tap] = 0.0f;
    }

    for (uint16_t ch = 0u; ch < FIR_FILTER_MAX_CH; ch++)
    {
        pfir->write_index[ch] = 0u;
    }
}


static const float* fir_filter_get_smoke_test_coeff( fir_filter_smoke_test_t smoke_test,
                                                     uint16_t*               num_taps )
{
    if (num_taps == NULL)
    {
        return NULL;
    }

    switch (smoke_test)
    {
        case FIR_FILTER_SMOKE_TEST_BYPASS:
            *num_taps = 1u;
            return fir_filter_smoke_test_bypass_coeff;

        case FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_8:
            *num_taps = 8u;
            return fir_filter_smoke_test_moving_average_8_coeff;

        case FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_16:
            *num_taps = 16u;
            return fir_filter_smoke_test_moving_average_16_coeff;

        case FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_32:
            *num_taps = 32u;
            return fir_filter_smoke_test_moving_average_32_coeff;

        case FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_64:
            *num_taps = 64u;
            return fir_filter_smoke_test_moving_average_64_coeff;

        case FIR_FILTER_SMOKE_TEST_EDGE_2:
            *num_taps = 2u;
            return fir_filter_smoke_test_edge_2_coeff;

        case FIR_FILTER_SMOKE_TEST_DELAY_MIX_16:
            *num_taps = 16u;
            return fir_filter_smoke_test_delay_mix_16_coeff;

        case FIR_FILTER_SMOKE_TEST_DELAY_MIX_32:
            *num_taps = 32u;
            return fir_filter_smoke_test_delay_mix_32_coeff;

        default:
            break;
    }

    *num_taps = 0u;
    return NULL;
}


#if defined(USE_CMSIS_FIR_DSP_PROCESS)
static void fir_filter_init_cmsis_instances( const fir_filter_t* pfir )
{
    if (pfir == NULL)
    {
        return;
    }

    if ((pfir->num_taps < 2u) || (pfir->num_taps > FIR_FILTER_MAX_TAPS))
    {
        return;
    }

    if ((pfir->num_ch == 0u) || (pfir->num_ch > FIR_FILTER_MAX_CH))
    {
        return;
    }

    for (uint16_t ch = 0u; ch < pfir->num_ch; ch++)
    {
        mchp_fir_init_f32_ex(&g_fir_filter_cmsis_inst[ch],
                             (uint32_t)pfir->num_taps,
                             g_fir_filter_cmsis_coeff,
                             g_fir_filter_cmsis_state[ch]);
    }
}
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)


//===========================================================
// API
//===========================================================

static fir_filter_t My_FirFilter;


void app_fir_filter_init(void)
{
    fir_filter_init(&My_FirFilter);



// test only
#if defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
    fir_filter_c_vs_cmsis_selftest();
    while(1);
#endif //defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
// test only


}


void app_fir_filter_clear_state(void)
{
    fir_filter_reset(&My_FirFilter);
}


bool app_fir_filter_set_config( const float* coeff,
                                uint16_t     num_taps,
                                uint16_t     num_ch )
{
    return fir_filter_set_config(&My_FirFilter,
                                 coeff,
                                 num_taps,
                                 num_ch);
}


bool app_fir_filter_get_config( float*    coeff,
                                uint16_t  coeff_array_size,
                                uint16_t* num_taps,
                                uint16_t* num_ch )
{
    return fir_filter_get_config(&My_FirFilter,
                                 coeff,
                                 coeff_array_size,
                                 num_taps,
                                 num_ch);
}


void app_fir_filter_process( const float* in,
                                       float* out )
{
    uint32_t t0;

    t0 = nora_high_res_timer_get_count();

#if defined(USE_CMSIS_FIR_DSP_PROCESS)
    fir_filter_process_cmsis(&My_FirFilter,
                                 in,
                                 out,
                                 APP_BLOCK_FRAMES);
#else
    fir_filter_process(&My_FirFilter,
                           in,
                           out,
                           APP_BLOCK_FRAMES);
#endif //defined(USE_CMSIS_FIR_DSP_PROCESS)

    g_fir_filter_process_dt       = nora_high_res_timer_elapsed_count(t0);
    g_fir_filter_process_num_ch   = My_FirFilter.num_ch;
    g_fir_filter_process_num_taps = My_FirFilter.num_taps;
}


bool app_fir_filter_apply_smoke_test( fir_filter_smoke_test_t smoke_test,
                                        uint16_t                num_ch )
{
    return fir_filter_apply_smoke_test(&My_FirFilter,
                                       smoke_test,
                                       num_ch);
}


bool app_fir_filter_set_smoke_test_bypass( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_BYPASS,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_moving_average_8( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_8,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_moving_average_16( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_16,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_moving_average_32( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_32,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_moving_average_64( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_64,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_edge_2( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_EDGE_2,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_delay_mix_16( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_DELAY_MIX_16,
                                           num_ch);
}


bool app_fir_filter_set_smoke_test_delay_mix_32( uint16_t num_ch )
{
    return app_fir_filter_apply_smoke_test(FIR_FILTER_SMOKE_TEST_DELAY_MIX_32,
                                           num_ch);
}

#endif //defined(ENA_FIR_FILTER)
