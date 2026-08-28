#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "nora_dma.h"
#include "nora_high_res_timer.h"
#include "gain_ctrl.h"
#include "arm_math.h"


#include "biquad_cascade_4ch.h"


#if defined(ENA_BIQUAD_IIR_CASCADE)
//===========================================================
// Definition
//===========================================================

// This module applies a fixed-coefficient biquad cascade to 4ch float audio.
//
// Buffer layout:
//   4ch sample-major interleaved
//   L1, R1, L2, R2, L1, R1, L2, R2, ...
//
// Biquad form:
//   Direct Form II Transposed
//
//   y  = b0 * x + z1
//   z1 = b1 * x - a1 * y + z2
//   z2 = b2 * x - a2 * y
//
// Coefficient rule:
//   a0 is assumed to be 1.0 and is not stored in the table.
//   Coefficients must already be normalized by a0.
//
// Bypass coefficient:
//   { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }


// #define ENA_SELFTEST


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE = 0,
    APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_REQUESTED,
    APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE,
} app_biquad_cascade_4ch_proc_state_t;


//===========================================================
// Function Prototype
//===========================================================

static const char* biquad_cascade_4ch_csv_skip_delim(const char* p);
static int         biquad_cascade_4ch_csv_count_values(const char* text, int* parse_error_index);
static void        biquad_cascade_4ch_csv_update_summary(void);
static void        biquad_cascade_4ch_csv_print_summary(void);
static bool        biquad_cascade_4ch_csv_load_coeff_from_text(void);
static void        biquad_cascade_4ch_cmsis_prepare_coeff(void);
static void        biquad_cascade_4ch_process_bypass(const float* in, float* out, int samples);

static void        app_biquad_cascade_4ch_process_wrapper(const float* in, float* out, int samples);






//===========================================================
// Variables
//===========================================================

// Coefficient table format:
//   [stage][ch]
//   { b0, b1, b2, a1, a2 }
//
// CSV source image per stage:
//
//           ch1,     ch2,     ch3,     ch4
//   b0      ...,     ...,     ...,     ...
//   b1      ...,     ...,     ...,     ...
//   b2      ...,     ...,     ...,     ...
//   a1      ...,     ...,     ...,     ...
//   a2      ...,     ...,     ...,     ...
//
// Manual conversion rule:
//   ch1 -> { b0_ch1, b1_ch1, b2_ch1, a1_ch1, a2_ch1 }
//   ch2 -> { b0_ch2, b1_ch2, b2_ch2, a1_ch2, a2_ch2 }
//   ch3 -> { b0_ch3, b1_ch3, b2_ch3, a1_ch3, a2_ch3 }
//   ch4 -> { b0_ch4, b1_ch4, b2_ch4, a1_ch4, a2_ch4 }

static const biquad_t biquad_bypass_1ch    = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
static const biquad_t biquad_1khz_3dB_1ch  = { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f };
static const biquad_t biquad_1khz_m3dB_1ch = { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f,  0.80227183f };

static biquad_t g_biquad_cascade_4ch_coeff[BIQUAD_CASCADE_4CH_NUM_STAGE][BIQUAD_CASCADE_NUM_CH];




// CSV paste area for coefficient import.
//
// Operation image:
//   1. Open coefficient CSV with a text editor.
//   2. Select all / copy.
//   3. Replace the lines between "paste CSV below" and "paste CSV above".
//
// Expected numeric order:
//   One line = one coefficient kind of one stage.
//   Columns  = ch1,ch2,ch3,ch4
//   Rows     = Stage00 b0,b1,b2,a1,a2, Stage01 b0,b1,b2,a1,a2, ...
//   Header   = none
//
// The data is stringized first, so normal CSV lines do not need a trailing comma.
#define BIQUAD_CSV_TEXT(...)  #__VA_ARGS__


static const char g_biquad_cascade_4ch_csv_text[] = BIQUAD_CSV_TEXT(
/* paste CSV below */
//
// No need to format the CSV text neatly.
//
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////



// ch1           ch2           ch3           ch4          stage
 0.992871106 ,  0.992967129 ,   0.97711885 ,  0.977378011  // 0    b0
 -1.97894061 ,  -1.97968435 ,   -1.9486047 ,  -1.94946957  // 0    b1
 0.986273527 ,  0.986917198 ,  0.971766591 ,  0.972350836  // 0    b2
 -1.97894061 ,  -1.97968435 ,   -1.9486047 ,  -1.94946957  // 0    a1
 0.979144573 ,  0.979884386 ,  0.948885441 ,  0.949728906  // 0    a2
 0.814266562 ,  0.820286274 ,  0.904021382 ,  0.907745063  // 1    b0
 -1.52954948 ,  -1.54891443 ,  -1.67325652 ,  -1.67420924  // 1    b1
 0.750898242 ,  0.760599136 ,  0.803852916 ,  0.802503824  // 1    b2
 -1.52954948 ,  -1.54891443 ,  -1.67325652 ,  -1.67420924  // 1    a1
 0.565164804 ,   0.58088541 ,  0.707874298 ,  0.710249007  // 1    a2
 0.702026665 ,  0.749260366 ,   1.00707543 ,   1.00657451  // 2    b0
-0.056509871 , -0.158265755 ,  -1.97302425 ,  -1.97315097  // 2    b1
 0.382087648 ,  0.384988844 ,  0.973001361 ,  0.973490119  // 2    b2
-0.056509871 , -0.158265755 ,  -1.97302425 ,  -1.97315097  // 2    a1
 0.084114283 ,   0.13424924 ,  0.980076849 ,   0.98006469  // 2    a2
 0.988732278 ,  0.990001917 ,   1.18418324 ,    1.0906955  // 3    b0
 -1.96993399 ,  -1.97245312 , -0.034989715 , -0.302464098  // 3    b1
 0.982993603 ,  0.984245062 ,  0.445696861 ,  0.727050245  // 3    b2
 -1.96993399 ,  -1.97245312 , -0.034989715 , -0.302464098  // 3    a1
 0.971725881 ,  0.974246919 ,  0.629880071 ,  0.817745686  // 3    a2
  1.02711582 ,    1.0290935 ,   1.00042057 ,   1.00040221  // 4    b0
 -1.89464402 ,  -1.89373195 ,  -1.99831581 ,  -1.99839771  // 4    b1
 0.918394566 ,  0.912442684 ,  0.997974694 ,  0.998083472  // 4    b2
 -1.89464402 ,  -1.89373195 ,  -1.99831581 ,  -1.99839771  // 4    a1
 0.945510328 ,  0.941536129 ,  0.998395383 ,  0.998485625  // 4    a2
  1.00141835 ,   1.00186658 ,   0.99872756 ,  0.996901274  // 5    b0
  -1.9967463 ,  -1.97909403 ,   -1.9946059 ,  -1.98451424  // 5    b1
 0.995731294 ,  0.981357098 ,   0.99591893 ,  0.989346862  // 5    b2
  -1.9967463 ,  -1.97909403 ,   -1.9946059 ,  -1.98451424  // 5    a1
 0.997149706 ,  0.983223677 ,   0.99464643 ,  0.986248136  // 5    a2
 0.998435199 ,   1.00128722 ,  0.997150779 ,   1.00019431  // 6    b0
 -1.99298763 ,  -1.99641621 ,   -1.9849472 ,  -1.99919212  // 6    b1
 0.995094597 ,  0.995540679 ,  0.989638627 ,   0.99901861  // 6    b2
 -1.99298763 ,  -1.99641621 ,   -1.9849472 ,  -1.99919212  // 6    a1
 0.993529737 ,  0.996827841 ,  0.986789405 ,  0.999212861  // 6    a2
  1.01084602 ,  0.998640656 ,   1.10284495 ,   0.99976474  // 7    b0
 -1.91917479 ,  -1.99318647 ,  -1.07471144 ,  -1.99840128  // 7    b1
 0.915613294 ,  0.995056689 ,  0.643720567 ,  0.998678863  // 7    b2
 -1.91917479 ,  -1.99318647 ,  -1.07471144 ,  -1.99840128  // 7    a1
 0.926459312 ,  0.993697286 ,  0.746565461 ,  0.998443663  // 7    a2
  0.99976033 ,  0.999783278 ,   1.00015473 ,  0.999367416  // 8    b0
 -1.99844599 ,  -1.99859381 ,  -1.99910915 ,  -1.99395943  // 8    b1
 0.998694718 ,   0.99881959 ,  0.998975039 ,  0.995167553  // 8    b2
 -1.99844599 ,  -1.99859381 ,  -1.99910915 ,  -1.99395943  // 8    a1
 0.998455107 ,  0.998602867 ,  0.999129951 ,  0.994534969  // 8    a2
   0.9167943 ,  0.982661664 ,  0.943612158 ,   1.01713336  // 9    b0
  1.09599614 ,  -1.36437154 , -0.828307927 ,  -1.53878915  // 9    b1
 0.646293938 ,  0.873029828 ,  0.794944823 ,   0.88661015  // 9    b2
  1.09599614 ,  -1.36437154 , -0.828307927 ,  -1.53878915  // 9    a1
 0.563088298 ,  0.855691552 ,  0.738556981 ,  0.903743565  // 9    a2
   1.1856432 ,   0.89669317 ,  0.975446641 ,   1.00692523  // 10   b0
 0.683565259 ,   1.07148886 ,  -1.35408735 ,  -1.90868771  // 10   b1
 0.441302836 ,  0.631443322 ,  0.866257429 ,  0.954168558  // 10   b2
 0.683565259 ,   1.07148886 ,  -1.35408735 ,  -1.90868771  // 10   a1
 0.626946092 ,  0.528136492 ,  0.841704071 ,  0.961093783  // 10   a2
 0.994721711 ,   1.13805616 ,  0.999133289 ,   1.02369082  // 11   b0
 -1.95903838 ,  0.878511488 ,  -1.99368989 ,  -1.14875937  // 11   b1
 0.977030337 ,  0.584516823 ,  0.995131969 ,  0.843213379  // 11   b2
 -1.95903838 ,  0.878511488 ,  -1.99368989 ,  -1.14875937  // 11   a1
 0.971752048 ,  0.722573102 ,  0.994265378 ,   0.86690414  // 11   a2
  0.99626714 ,   0.99626714 ,   1.00883591 ,    1.0914489  // 12   b0
 -1.99253428 ,  -1.99253428 ,  -1.91044867 ,  0.267743289  // 12   b1
  0.99626714 ,   0.99626714 ,   0.95406729 ,  0.624927104  // 12   b2
 -1.99252069 ,  -1.99252069 ,  -1.91044867 ,  0.267743289  // 12   a1
 0.992547989 ,  0.992547989 ,  0.962903082 ,  0.716376066  // 12   a2
 0.687483072 ,  0.687483072 ,   0.99626714 ,   0.99626714  // 13   b0
  1.37496614 ,   1.37496614 ,  -1.99253428 ,  -1.99253428  // 13   b1
 0.687483072 ,  0.687483072 ,   0.99626714 ,   0.99626714  // 13   b2
  1.27624798 ,   1.27624798 ,  -1.99252069 ,  -1.99252069  // 13   a1
 0.473684162 ,  0.473684162 ,  0.992547989 ,  0.992547989  // 13   a2
           1 ,            1 ,  0.687483072 ,  0.687483072  // 14   b0
           0 ,            0 ,   1.37496614 ,   1.37496614  // 14   b1
           0 ,            0 ,  0.687483072 ,  0.687483072  // 14   b2
           0 ,            0 ,   1.27624798 ,   1.27624798  // 14   a1
           0 ,            0 ,  0.473684162 ,  0.473684162  // 14   a2


/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////
/* paste CSV above */
);


volatile int g_biquad_cascade_4ch_csv_load_result      = 0;
volatile int g_biquad_cascade_4ch_csv_load_error_index = -1;
volatile int g_biquad_cascade_4ch_csv_total_values     = 0;
volatile int g_biquad_cascade_4ch_csv_expected_values  = 0;
volatile int g_biquad_cascade_4ch_csv_stage_count      = 0;
volatile int g_biquad_cascade_4ch_csv_remainder_values = 0;

#if 0
// dspic33-cmsis-dsp / MCHP DF2T test path.
// The library API is 1ch block processing, so this wrapper deinterleaves
// 4ch interleaved audio into temporary per-channel buffers.
static arm_biquad_cascade_df2T_instance_f32 g_biquad_cascade_4ch_cmsis_inst[BIQUAD_CASCADE_NUM_CH] __attribute__((aligned(4)));
static float32_t g_biquad_cascade_4ch_cmsis_coeff[BIQUAD_CASCADE_NUM_CH][5u * BIQUAD_CASCADE_4CH_NUM_STAGE] __attribute__((aligned(4)));
static float32_t g_biquad_cascade_4ch_cmsis_state[BIQUAD_CASCADE_NUM_CH][2u * BIQUAD_CASCADE_4CH_NUM_STAGE] __attribute__((aligned(4)));
// static float32_t g_biquad_cascade_4ch_cmsis_ch_in [BIQUAD_CASCADE_NUM_CH][APP_BLOCK_FRAMES] __attribute__((aligned(4)));
// static float32_t g_biquad_cascade_4ch_cmsis_ch_out[BIQUAD_CASCADE_NUM_CH][APP_BLOCK_FRAMES] __attribute__((aligned(4)));
#else
#define BIQUAD_XMEM     __attribute__((section(".xbss"),    aligned(4)))
#define BIQUAD_YMEM     __attribute__((section(".ybss"),    aligned(4)))
#define BIQUAD_CONST_X  __attribute__((section(".const,x"), aligned(4)))

// dspic33-cmsis-dsp / MCHP DF2T test path.
// The library API is 1ch block processing, so this wrapper deinterleaves
// 4ch interleaved audio into temporary per-channel buffers.
static arm_biquad_cascade_df2T_instance_f32 g_biquad_cascade_4ch_cmsis_inst[BIQUAD_CASCADE_NUM_CH] __attribute__((aligned(4)));
static float32_t g_biquad_cascade_4ch_cmsis_coeff[BIQUAD_CASCADE_NUM_CH][5u * BIQUAD_CASCADE_4CH_NUM_STAGE] BIQUAD_XMEM;
static float32_t g_biquad_cascade_4ch_cmsis_state[BIQUAD_CASCADE_NUM_CH][2u * BIQUAD_CASCADE_4CH_NUM_STAGE] BIQUAD_YMEM;
// static float32_t g_biquad_cascade_4ch_cmsis_ch_in [BIQUAD_CASCADE_NUM_CH][APP_BLOCK_FRAMES] BIQUAD_XMEM;
// static float32_t g_biquad_cascade_4ch_cmsis_ch_out[BIQUAD_CASCADE_NUM_CH][APP_BLOCK_FRAMES] BIQUAD_YMEM;
#endif//01




volatile float g_biquad_cascade_4ch_cmsis_test_max_abs_diff = 0.0f;

uint32_t t0, dt;




typedef void (*biquad_cascade_proc_func_t)(const float* in, float* out, int samples);
static void dummy_proc(const float* in, float* out, int samples)
{
    (void)in;
    (void)out;
    (void)samples;
}


/*
 * Note: volatile is not mutual exclusion.
 * Updates must be protected by the DMA0 critical section.
 */
static volatile biquad_cascade_proc_func_t s_biquad_proc = dummy_proc;


static inline bool biquad_cascade_4ch_enter_dma0_critical(void)
{
    bool dma0ie_save;

    dma0ie_save = nora_dma_irq_disable_save(0u);
    return dma0ie_save;
}
static inline void biquad_cascade_4ch_exit_dma0_critical(bool dma0ie_save)
{
    nora_dma_irq_restore(0u, dma0ie_save);
}


static void biquad_cascade_4ch_set_proc_active_critical(void)
{
    bool dma0ie_save = biquad_cascade_4ch_enter_dma0_critical();

#if defined(USE_CMSIS_IIR_DSP_PROCESS)
    /*
     * Directly select the real active processing function.
     * Do not route through an additional active wrapper; this keeps the
     * DMA0 ISR call chain as short as possible:
     *   app_biquad...process() -> s_biquad_proc -> CMSIS/C implementation
     */
    s_biquad_proc = biquad_cascade_4ch_process_cmsis;
#else
    s_biquad_proc = app_biquad_cascade_4ch_process_wrapper;
#endif //defined(USE_CMSIS_IIR_DSP_PROCESS)

    biquad_cascade_4ch_exit_dma0_critical(dma0ie_save);
}


static void biquad_cascade_4ch_set_proc_bypass_critical(void)
{
    bool dma0ie_save = biquad_cascade_4ch_enter_dma0_critical();

    s_biquad_proc = biquad_cascade_4ch_process_bypass;

    biquad_cascade_4ch_exit_dma0_critical(dma0ie_save);
}






//===========================================================
// Global Function
//===========================================================

/**
 * @brief Initialize a 4ch biquad cascade instance.
 *
 * Behavior:
 *   - Clears all delay states.
 *   - Clears NaN/Inf debug counters.
 *
 * @param pbq Pointer to @c biquad_cascade_4ch_t instance.
 */
void biquad_cascade_4ch_init( biquad_cascade_4ch_t* pbq )
{
    if (pbq == NULL)
    {
        return;
    }

    memset( pbq, 0x00, sizeof(biquad_cascade_4ch_t) );

    pbq->last_nan_inf_stage = -1;
    pbq->last_nan_inf_ch    = -1;



    // If CSV parsing fails, unfilled coefficients remain safe bypass values.
    for(uint16_t stage=0; stage<BIQUAD_CASCADE_4CH_NUM_STAGE; stage++ )
    {
        for(uint8_t ch=0; ch<BIQUAD_CASCADE_NUM_CH; ch++ )
        {
            g_biquad_cascade_4ch_coeff[stage][ch] = biquad_bypass_1ch;
            (void)biquad_1khz_3dB_1ch;
            (void)biquad_1khz_m3dB_1ch;
        }
    }


#define ENA_BIQUAD_LOAD_TEST_COEFF

#if defined(ENA_BIQUAD_LOAD_TEST_COEFF)
    // load test
    for(uint16_t stage=0; stage<BIQUAD_CASCADE_4CH_NUM_STAGE; stage++ )
    {
        for(uint8_t ch=0; ch<BIQUAD_CASCADE_NUM_CH; ch++ )
        {
            if( stage%2 )
            {
                g_biquad_cascade_4ch_coeff[stage][ch] = biquad_1khz_m3dB_1ch;
            }
            else
            {
                g_biquad_cascade_4ch_coeff[stage][ch] = biquad_1khz_3dB_1ch;
            }
            (void)biquad_bypass_1ch;
        }
    }
#else
    biquad_cascade_4ch_csv_load_coeff_from_text();
#endif //defined(ENA_BIQUAD_LOAD_TEST_COEFF)

    app_biquad_cascade_4ch_debug_print_coeff(0, 2);
}


void biquad_cascade_4ch_reset( biquad_cascade_4ch_t* pbq )
{
    if (pbq == NULL)
    {
        return;
    }

    memset( pbq->st, 0x00, sizeof(pbq->st) );

    pbq->nan_inf_count     = 0u;
    pbq->last_nan_inf_stage = -1;
    pbq->last_nan_inf_ch    = -1;
}


/**
 * @brief Convert local [stage][ch] coefficient table to MCHP/CMSIS DF2T layout.
 *
 * Local coefficient rule:
 *   y  = b0*x + z1
 *   z1 = b1*x - a1*y + z2
 *   z2 = b2*x - a2*y
 *
 * CMSIS DF2T implementations commonly use +a1/+a2 in the state update.
 * CMSIS_NEGATE_A_COEFF selects whether a1/a2 are negated
 * during conversion. Use the self-test to confirm the correct convention.
 */
static void biquad_cascade_4ch_cmsis_prepare_coeff(void)
{
    for (int ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        for (int stage = 0; stage < BIQUAD_CASCADE_4CH_NUM_STAGE; stage++)
        {
            const biquad_t* bq = &g_biquad_cascade_4ch_coeff[stage][ch];
            float32_t*      pc = &g_biquad_cascade_4ch_cmsis_coeff[ch][stage * 5];

            pc[0] = (float32_t)bq->b0;
            pc[1] = (float32_t)bq->b1;
            pc[2] = (float32_t)bq->b2;
#if CMSIS_NEGATE_A_COEFF
            pc[3] = (float32_t)(-bq->a1);
            pc[4] = (float32_t)(-bq->a2);
#else
            pc[3] = (float32_t)bq->a1;
            pc[4] = (float32_t)bq->a2;
#endif
        }
    }
}


/**
 * @brief Initialize dspic33-cmsis-dsp / MCHP DF2T test instances.
 */
void biquad_cascade_4ch_cmsis_init(void)
{
    biquad_cascade_4ch_cmsis_prepare_coeff();

    memset(g_biquad_cascade_4ch_cmsis_state, 0x00, sizeof(g_biquad_cascade_4ch_cmsis_state));

    for (int ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        arm_biquad_cascade_df2T_init_f32(&g_biquad_cascade_4ch_cmsis_inst[ch],
                                         (uint8_t)BIQUAD_CASCADE_4CH_NUM_STAGE,
                                         g_biquad_cascade_4ch_cmsis_coeff[ch],
                                         g_biquad_cascade_4ch_cmsis_state[ch]);
    }
}


/**
 * @brief Apply dspic33-cmsis-dsp / MCHP DF2T biquad cascade to 4ch audio.
 *
 * Input and output are 4ch channel-major buffers.
 *
 * Buffer layout:
 *   in[0 * samples + n] = L1
 *   in[1 * samples + n] = R1
 *   in[2 * samples + n] = L2
 *   in[3 * samples + n] = R2
 *
 * The CMSIS/MCHP API processes one contiguous channel block, so this layout
 * can be passed directly without the deinterleave/interleave temporary copies
 * used by @c biquad_cascade_4ch_process_cmsis().
 *
 * @param in  Channel-major 4ch input buffer [4][samples].
 * @param out Channel-major 4ch output buffer [4][samples].
 * @param samples Number of samples per channel.
 */
void biquad_cascade_4ch_process_cmsis( const float* in,
                                                 float* out,
                                                 int    samples )
{
    if ((in == NULL) || (out == NULL) || (samples <= 0) || (samples > APP_BLOCK_FRAMES))
    {
        return;
    }

    for (int ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        const float32_t* p_src = &in [ch * samples];
              float32_t* p_dst = &out[ch * samples];

#if APP_TARGET == APP_TARGET_AK512
        t0 = nora_high_res_timer_get_count();
#endif //APP_TARGET == APP_TARGET_AK512

#if 0
        arm_biquad_cascade_df2T_f32(&g_biquad_cascade_4ch_cmsis_inst[ch],
                                     p_src,
                                     p_dst,
                                     (uint32_t)samples);
#else
        extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(const mchp_biquad_cascade_df2T_instance_f32 * S,
                                                             const float32_t * pSrc,
                                                             float32_t * pDst, uint32_t blockSize);
        biquad_cascade_df2T_f32_dspic33ak_opt_v1(
            &g_biquad_cascade_4ch_cmsis_inst[ch],
            p_src,
            p_dst,
            (uint32_t)samples
        );
#endif //01

#if APP_TARGET == APP_TARGET_AK512
        dt = nora_high_res_timer_elapsed_count(t0);
#endif //APP_TARGET == APP_TARGET_AK512
    }
}




/**
 * @brief Apply fixed biquad cascade to 4ch float audio, channel-major buffer version.
 *
 * Input and output are 4ch channel-major buffers.
 *
 * Buffer layout:
 *   in[0 * samples + n] = L1
 *   in[1 * samples + n] = R1
 *   in[2 * samples + n] = L2
 *   in[3 * samples + n] = R2
 *
 * This is the C reference / fallback path for the new channel-major audio
 * pipeline.  It keeps the same DF2T equation and coefficient/state tables as
 * @c biquad_cascade_4ch_process(), but does not need pointer stepping by 4
 * because each channel is already contiguous.
 *
 * In-place processing is allowed, i.e. @p in and @p out may point to
 * the same buffer.
 *
 * @param pbq     Pointer to @c biquad_cascade_4ch_t instance.
 * @param in  Channel-major 4ch input buffer [4][samples].
 * @param out Channel-major 4ch output buffer [4][samples].
 * @param samples Number of samples per channel.
 */
inline void biquad_cascade_4ch_process(       biquad_cascade_4ch_t* pbq,
                                           const float*                in,
                                                 float*                out,
                                                 int                   samples )
{
    if ((pbq == NULL) || (in == NULL) || (out == NULL) || (samples <= 0))
    {
        return;
    }

    // Channel-major buffer processing:
    //   channel -> sample -> stage
    // Each channel block is contiguous, which is friendlier to CMSIS/MCHP and
    // also removes the +4 interleaved pointer stride used by the old buffer.
    for (int ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
    {
        const float* p_in  = &in [ch * samples];
              float* p_out = &out[ch * samples];

        for (int sample_idx = 0; sample_idx < samples; sample_idx++)
        {
            float x = *p_in++;

            for (int stage = 0; stage < BIQUAD_CASCADE_4CH_NUM_STAGE; stage++)
            {
                const biquad_t*      bq = &g_biquad_cascade_4ch_coeff[stage][ch];
                      biquad_stat_t* st = &pbq->st[stage][ch];

                float z1 = st->z1;
                float z2 = st->z2;

                // Direct Form II Transposed Biquad
                float y = bq->b0 * x + z1;
                z1 = bq->b1 * x - bq->a1 * y + z2;
                z2 = bq->b2 * x - bq->a2 * y;

#if BIQUAD_CASCADE_4CH_ENABLE_NAN_INF_PROTECT
                if ((!isfinite(y)) || (!isfinite(z1)) || (!isfinite(z2)))
                {
                    pbq->nan_inf_count++;
                    pbq->last_nan_inf_stage = stage;
                    pbq->last_nan_inf_ch    = ch;

                    y  = 0.0f;
                    z1 = 0.0f;
                    z2 = 0.0f;
                }
#endif

                st->z1 = z1;
                st->z2 = z2;

                x = y;
            }

            *p_out++ = x;
        }
    }
}


#if defined(ENA_SELFTEST)
/**
 * @brief Self-test comparing a fixed local DF2T reference and CMSIS/MCHP DF2T.
 *
 * This self-test is intentionally independent from normal run-time settings.
 * It does not use:
 *   - BIQUAD_CASCADE_4CH_NUM_STAGE
 *   - g_biquad_cascade_4ch_coeff[]
 *   - g_biquad_cascade_4ch_cmsis_inst/state[]
 *   - biquad_cascade_4ch_process_cmsis()
 *
 * Therefore changing the production stage count, CSV coefficients, or normal
 * audio processing state does not change the self-test condition.
 *
 * @return Maximum absolute difference over the fixed impulse test block.
 */
float biquad_cascade_4ch_cmsis_selftest(void)
{
#define SELFTEST_NUM_STAGE      (4)
#define SELFTEST_NUM_CH         (4)
#define SELFTEST_NUM_SAMPLE     (32)

    static const biquad_t selftest_coeff[SELFTEST_NUM_STAGE][SELFTEST_NUM_CH] =
    {
        {
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f },
            { 1.00000000f,  0.00000000f, 0.00000000f,  0.00000000f, 0.00000000f },
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f }
        },
        {
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f },
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },
            { 1.00000000f,  0.00000000f, 0.00000000f,  0.00000000f, 0.00000000f }
        },
        {
            { 1.00000000f,  0.00000000f, 0.00000000f,  0.00000000f, 0.00000000f },
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f },
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f }
        },
        {
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f },
            { 1.00000000f,  0.00000000f, 0.00000000f,  0.00000000f, 0.00000000f },
            { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },
            { 0.97112681f, -1.78656936f, 0.83114562f, -1.78656936f, 0.80227183f }
        }
    };

    static float32_t in_interleaved [SELFTEST_NUM_SAMPLE * SELFTEST_NUM_CH] __attribute__((aligned(4)));
    static float32_t ref_interleaved[SELFTEST_NUM_SAMPLE * SELFTEST_NUM_CH] __attribute__((aligned(4)));
    static float32_t out_interleaved[SELFTEST_NUM_SAMPLE * SELFTEST_NUM_CH] __attribute__((aligned(4)));

    static float32_t coeff_cmsis[SELFTEST_NUM_CH][SELFTEST_NUM_STAGE * 5u] __attribute__((aligned(4)));
    static float32_t state_cmsis[SELFTEST_NUM_CH][SELFTEST_NUM_STAGE * 2u] __attribute__((aligned(4)));
    static float32_t ch_in      [SELFTEST_NUM_CH][SELFTEST_NUM_SAMPLE]      __attribute__((aligned(4)));
    static float32_t ch_out     [SELFTEST_NUM_CH][SELFTEST_NUM_SAMPLE]      __attribute__((aligned(4)));
    static biquad_stat_t ref_state[SELFTEST_NUM_STAGE][SELFTEST_NUM_CH];
    static arm_biquad_cascade_df2T_instance_f32 inst[SELFTEST_NUM_CH];

    float max_abs_diff = 0.0f;
    int max_idx = 0;

    memset(in_interleaved,  0x00, sizeof(in_interleaved));
    memset(ref_interleaved, 0x00, sizeof(ref_interleaved));
    memset(out_interleaved, 0x00, sizeof(out_interleaved));
    memset(coeff_cmsis,     0x00, sizeof(coeff_cmsis));
    memset(state_cmsis,     0x00, sizeof(state_cmsis));
    memset(ch_in,           0x00, sizeof(ch_in));
    memset(ch_out,          0x00, sizeof(ch_out));
    memset(ref_state,       0x00, sizeof(ref_state));

    // Fixed modest impulse test. This must not track production gain/stage settings.
    in_interleaved[0] = 0.03125f;
    in_interleaved[1] = 0.02500f;
    in_interleaved[2] = 0.01875f;
    in_interleaved[3] = 0.01250f;

    //
    // Reference path: local DF2T, fixed stage count, fixed coefficients.
    /////////////////
    for (int ch = 0; ch < SELFTEST_NUM_CH; ch++)
    {
        for (int sample_idx = 0; sample_idx < SELFTEST_NUM_SAMPLE; sample_idx++)
        {
            float x = in_interleaved[(sample_idx * SELFTEST_NUM_CH) + ch];

            for (int stage = 0; stage < SELFTEST_NUM_STAGE; stage++)
            {
                const biquad_t*    bq = &selftest_coeff[stage][ch];
                biquad_stat_t*     st = &ref_state[stage][ch];
                const float old_z1 = st->z1;
                const float old_z2 = st->z2;

                const float y  = (bq->b0 * x) + old_z1;
                const float z1 = (bq->b1 * x) - (bq->a1 * y) + old_z2;
                const float z2 = (bq->b2 * x) - (bq->a2 * y);

                st->z1 = z1;
                st->z2 = z2;
                x = y;
            }

            ref_interleaved[(sample_idx * SELFTEST_NUM_CH) + ch] = x;
        }
    }

    //
    // CMSIS/MCHP path: fixed local instance/state/coefficients.
    /////////////////
    for (int ch = 0; ch < SELFTEST_NUM_CH; ch++)
    {
        for (int stage = 0; stage < SELFTEST_NUM_STAGE; stage++)
        {
            const biquad_t* bq = &selftest_coeff[stage][ch];
            float32_t*      pc = &coeff_cmsis[ch][stage * 5];

            pc[0] = (float32_t)bq->b0;
            pc[1] = (float32_t)bq->b1;
            pc[2] = (float32_t)bq->b2;
#if CMSIS_NEGATE_A_COEFF
            pc[3] = (float32_t)(-bq->a1);
            pc[4] = (float32_t)(-bq->a2);
#else
            pc[3] = (float32_t)bq->a1;
            pc[4] = (float32_t)bq->a2;
#endif
        }

        arm_biquad_cascade_df2T_init_f32(&inst[ch],
                                         (uint8_t)SELFTEST_NUM_STAGE,
                                         coeff_cmsis[ch],
                                         state_cmsis[ch]);
    }

    for (int sample_idx = 0; sample_idx < SELFTEST_NUM_SAMPLE; sample_idx++)
    {
        for (int ch = 0; ch < SELFTEST_NUM_CH; ch++)
        {
            ch_in[ch][sample_idx] = in_interleaved[(sample_idx * SELFTEST_NUM_CH) + ch];
        }
    }

    for (int ch = 0; ch < SELFTEST_NUM_CH; ch++)
    {
#if 0
        arm_biquad_cascade_df2T_f32(&inst[ch],
                                    ch_in[ch],
                                    ch_out[ch],
                                    (uint32_t)SELFTEST_NUM_SAMPLE);
#else
        extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(const mchp_biquad_cascade_df2T_instance_f32 * S,
                                                             const float32_t * pSrc,
                                                             float32_t * pDst, uint32_t blockSize);
        biquad_cascade_df2T_f32_dspic33ak_opt_v1(
                                    &inst[ch],
                                    ch_in[ch],
                                    ch_out[ch],
                                    (uint32_t)SELFTEST_NUM_SAMPLE);
#endif
    }

    for (int sample_idx = 0; sample_idx < SELFTEST_NUM_SAMPLE; sample_idx++)
    {
        for (int ch = 0; ch < SELFTEST_NUM_CH; ch++)
        {
            out_interleaved[(sample_idx * SELFTEST_NUM_CH) + ch] = ch_out[ch][sample_idx];
        }
    }

    for (int i = 0; i < (SELFTEST_NUM_SAMPLE * SELFTEST_NUM_CH); i++)
    {
        const float diff = fabsf(ref_interleaved[i] - out_interleaved[i]);
        if (diff > max_abs_diff)
        {
            max_abs_diff = diff;
            max_idx = i;
        }
    }

    g_biquad_cascade_4ch_cmsis_test_max_abs_diff = max_abs_diff;

    printf("CMSIS DF2T selftest fixed: stage=%d samples=%d max_idx=%d ref=%.9f out=%.9f\n",
           SELFTEST_NUM_STAGE,
           SELFTEST_NUM_SAMPLE,
           max_idx,
           (double)ref_interleaved[max_idx],
           (double)out_interleaved[max_idx]);

    // Leave the production CMSIS path in a clean state for later audio processing.
    biquad_cascade_4ch_cmsis_init();

    return max_abs_diff;

#undef SELFTEST_NUM_STAGE
#undef SELFTEST_NUM_CH
#undef SELFTEST_NUM_SAMPLE
}
#endif //defined(ENA_SELFTEST)












// Local Function
//===========================================================

static const char* biquad_cascade_4ch_csv_skip_delim(const char* p)
{
    while ((*p == ',') || (*p == '\r') || (*p == '\n') || (*p == '\t') || (*p == ' '))
    {
        p++;
    }

    return p;
}


static int biquad_cascade_4ch_csv_count_values(const char* text, int* parse_error_index)
{
    const char* p = text;
    char*       endp;
    int         count = 0;

    if (parse_error_index != NULL)
    {
        *parse_error_index = -1;
    }

    if (p == NULL)
    {
        if (parse_error_index != NULL)
        {
            *parse_error_index = 0;
        }
        return -1;
    }

    while (1)
    {
        p = biquad_cascade_4ch_csv_skip_delim(p);

        if (*p == '\0')
        {
            break;
        }

        (void)strtof(p, &endp);

        if (endp == p)
        {
            if (parse_error_index != NULL)
            {
                *parse_error_index = count;
            }
            return -1;
        }

        p = endp;
        count++;
    }

    return count;
}


static void biquad_cascade_4ch_csv_update_summary(void)
{
    int parse_error_index = -1;
    const int values_per_stage = (int)(BIQUAD_CASCADE_NUM_CH * 5u);
    const int expected_values  = (int)(BIQUAD_CASCADE_4CH_NUM_STAGE * BIQUAD_CASCADE_NUM_CH * 5u);
    const int total_values     = biquad_cascade_4ch_csv_count_values(g_biquad_cascade_4ch_csv_text,
                                                                      &parse_error_index);

    g_biquad_cascade_4ch_csv_expected_values = expected_values;

    if (total_values < 0)
    {
        g_biquad_cascade_4ch_csv_total_values     = -1;
        g_biquad_cascade_4ch_csv_stage_count      = -1;
        g_biquad_cascade_4ch_csv_remainder_values = -1;
        g_biquad_cascade_4ch_csv_load_result      = -2;
        g_biquad_cascade_4ch_csv_load_error_index = parse_error_index;
        return;
    }

    g_biquad_cascade_4ch_csv_total_values     = total_values;
    g_biquad_cascade_4ch_csv_stage_count      = total_values / values_per_stage;
    g_biquad_cascade_4ch_csv_remainder_values = total_values % values_per_stage;
}


static void biquad_cascade_4ch_csv_print_summary(void)
{
#if defined(ENA_CSV_REPORT_PRINTF)
    const int values_per_stage  = (int)(BIQUAD_CASCADE_NUM_CH * 5u);
    const int configured_stages = (int)BIQUAD_CASCADE_4CH_NUM_STAGE;
    const int csv_table_stages  = (int)g_biquad_cascade_4ch_csv_stage_count;
    const int total_values      = (int)g_biquad_cascade_4ch_csv_total_values;
    const int expected_values   = (int)g_biquad_cascade_4ch_csv_expected_values;
    const int extra_values      = total_values - expected_values;

    int loaded_stages  = 0;
    int ignored_stages = 0;
    int missing_stages = 0;
    int ignored_values = 0;
    int missing_values = 0;

    if ((total_values >= 0) && (g_biquad_cascade_4ch_csv_remainder_values == 0))
    {
        if (csv_table_stages >= configured_stages)
        {
            loaded_stages  = configured_stages;
            ignored_stages = csv_table_stages - configured_stages;
            ignored_values = ignored_stages * values_per_stage;
        }
        else
        {
            loaded_stages  = csv_table_stages;
            missing_stages = configured_stages - csv_table_stages;
            missing_values = missing_stages * values_per_stage;
        }
    }

    printf("\n\n");
    printf("-----------------------------------\n");
    printf("BIQUAD CSV: configured_stages=%d\n", configured_stages);
    printf("BIQUAD CSV: csv_table_stages=%d\n",  csv_table_stages);
    printf("BIQUAD CSV: loaded_stages=%d\n",     loaded_stages);
    printf("BIQUAD CSV: ignored_stages=%d\n",    ignored_stages);
    printf("BIQUAD CSV: missing_stages=%d\n",    missing_stages);
    printf("BIQUAD CSV: values_per_stage=%d\n",  values_per_stage);
    printf("BIQUAD CSV: total_values=%d\n",      total_values);
    printf("BIQUAD CSV: expected_values=%d\n",   expected_values);
    printf("BIQUAD CSV: remainder_values=%d\n",  (int)g_biquad_cascade_4ch_csv_remainder_values);
    printf("BIQUAD CSV: extra_values=%d\n",      extra_values);
    printf("BIQUAD CSV: ignored_values=%d\n",    ignored_values);
    printf("BIQUAD CSV: missing_values=%d\n",    missing_values);
    printf("BIQUAD CSV: load_result=%d\n",       (int)g_biquad_cascade_4ch_csv_load_result);
    printf("BIQUAD CSV: load_error_index=%d\n",  (int)g_biquad_cascade_4ch_csv_load_error_index);

    if (g_biquad_cascade_4ch_csv_remainder_values == 0)
    {
        printf("BIQUAD CSV: remainder check OK\n");
    }
    else
    {
        printf("BIQUAD CSV: remainder check NG\n");
    }

    if ((total_values >= 0) && (g_biquad_cascade_4ch_csv_remainder_values == 0))
    {
        if (csv_table_stages > configured_stages)
        {
            printf("BIQUAD CSV: load policy = loaded first %d stage(s), ignored remaining %d stage(s)\n",
                   configured_stages,
                   ignored_stages);
        }
        else if (csv_table_stages == configured_stages)
        {
            printf("BIQUAD CSV: load policy = loaded all %d stage(s)\n",
                   configured_stages);
        }
        else
        {
            printf("BIQUAD CSV: load policy = CSV table is short by %d stage(s)\n",
                   missing_stages);
        }
    }
    else
    {
        printf("BIQUAD CSV: load policy = not available due to parse/remainder error\n");
    }

    printf("-----------------------------------\n");
    printf("\n\n");

#else
    (void)g_biquad_cascade_4ch_csv_total_values;
    (void)g_biquad_cascade_4ch_csv_expected_values;
    (void)g_biquad_cascade_4ch_csv_stage_count;
    (void)g_biquad_cascade_4ch_csv_remainder_values;
    (void)g_biquad_cascade_4ch_csv_load_result;
    (void)g_biquad_cascade_4ch_csv_load_error_index;
#endif //defined(ENA_CSV_REPORT_PRINTF)
}


static bool biquad_cascade_4ch_csv_load_coeff_from_text(void)
{
    const char* p = g_biquad_cascade_4ch_csv_text;
    char*       endp;
    int         value_index = 0;

    g_biquad_cascade_4ch_csv_load_result      = 0;
    g_biquad_cascade_4ch_csv_load_error_index = -1;

    biquad_cascade_4ch_csv_update_summary();

    if (g_biquad_cascade_4ch_csv_total_values < 0)
    {
        biquad_cascade_4ch_csv_print_summary();
        return false;
    }

    for (uint16_t stage = 0; stage < BIQUAD_CASCADE_4CH_NUM_STAGE; stage++)
    {
        float* coeff_table[5u][BIQUAD_CASCADE_NUM_CH] =
        {
            { &g_biquad_cascade_4ch_coeff[stage][0].b0,
              &g_biquad_cascade_4ch_coeff[stage][1].b0,
              &g_biquad_cascade_4ch_coeff[stage][2].b0,
              &g_biquad_cascade_4ch_coeff[stage][3].b0 },
            { &g_biquad_cascade_4ch_coeff[stage][0].b1,
              &g_biquad_cascade_4ch_coeff[stage][1].b1,
              &g_biquad_cascade_4ch_coeff[stage][2].b1,
              &g_biquad_cascade_4ch_coeff[stage][3].b1 },
            { &g_biquad_cascade_4ch_coeff[stage][0].b2,
              &g_biquad_cascade_4ch_coeff[stage][1].b2,
              &g_biquad_cascade_4ch_coeff[stage][2].b2,
              &g_biquad_cascade_4ch_coeff[stage][3].b2 },
            { &g_biquad_cascade_4ch_coeff[stage][0].a1,
              &g_biquad_cascade_4ch_coeff[stage][1].a1,
              &g_biquad_cascade_4ch_coeff[stage][2].a1,
              &g_biquad_cascade_4ch_coeff[stage][3].a1 },
            { &g_biquad_cascade_4ch_coeff[stage][0].a2,
              &g_biquad_cascade_4ch_coeff[stage][1].a2,
              &g_biquad_cascade_4ch_coeff[stage][2].a2,
              &g_biquad_cascade_4ch_coeff[stage][3].a2 }
        };

        for (uint8_t coeff = 0; coeff < 5u; coeff++)
        {
            for (uint8_t ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
            {
                p = biquad_cascade_4ch_csv_skip_delim(p);

//                if (*p == '\0')
//                {
//                    g_biquad_cascade_4ch_csv_load_result      = -1;
//                    g_biquad_cascade_4ch_csv_load_error_index = value_index;
//                    biquad_cascade_4ch_csv_print_summary();
//                    return false;
//                }
                if (*p == '\0')
                {
                    const int values_per_stage = (int)(BIQUAD_CASCADE_NUM_CH * 5u);

                    if ((value_index > 0) && ((value_index % values_per_stage) == 0))
                    {
                        // CSV is shorter than configured stages, but it ends at a clean stage boundary.
                        // Remaining stages keep the safe bypass coefficients initialized before CSV load.
                        g_biquad_cascade_4ch_csv_load_result      = 2;
                        g_biquad_cascade_4ch_csv_load_error_index = -1;
                        biquad_cascade_4ch_csv_print_summary();
                        return true;
                    }

                    g_biquad_cascade_4ch_csv_load_result      = -1;
                    g_biquad_cascade_4ch_csv_load_error_index = value_index;
                    biquad_cascade_4ch_csv_print_summary();
                    return false;
                }

                *coeff_table[coeff][ch] = strtof(p, &endp);

                if (endp == p)
                {
                    g_biquad_cascade_4ch_csv_load_result      = -2;
                    g_biquad_cascade_4ch_csv_load_error_index = value_index;
                    biquad_cascade_4ch_csv_print_summary();
                    return false;
                }

                p = endp;
                value_index++;
            }
        }
    }

    g_biquad_cascade_4ch_csv_load_result      = 1;
    g_biquad_cascade_4ch_csv_load_error_index = -1;

    biquad_cascade_4ch_csv_print_summary();

    return true;
}










//===========================================================
// API
//===========================================================

/*static*/ biquad_cascade_4ch_t My_BiquadCascade4ch;

static volatile app_biquad_cascade_4ch_proc_state_t s_biquad_proc_state =
    APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;


static void biquad_cascade_4ch_process_bypass(const float* in,
                                                  float*       out,
                                                  int          samples)
{
    if( (in != NULL) && (out != NULL) && (samples > 0) )
    {
        if( in != out )
        {
            for( int i = 0; i < (samples * BIQUAD_CASCADE_NUM_CH); i++ )
            {
                out[i] = in[i];
            }
        }
    }

    s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE;
}


void app_biquad_cascade_4ch_clear_state(void)
{
    biquad_cascade_4ch_reset(&My_BiquadCascade4ch);

#if defined(USE_CMSIS_IIR_DSP_PROCESS)
    memset(g_biquad_cascade_4ch_cmsis_state,
           0x00,
           sizeof(g_biquad_cascade_4ch_cmsis_state));
#endif //defined(USE_CMSIS_IIR_DSP_PROCESS)
}


bool app_biquad_cascade_4ch_load_coeff_from_uart_csv( const float* coeff,
                                                      uint16_t     stage_num,
                                                      uint16_t     coeff_num,
                                                      uint16_t     ch_num )
{
    uint16_t stage;
    uint16_t ch;

    if (coeff == NULL)
    {
        printf("BIQUAD UART CSV: coeff is NULL\n");
        return false;
    }

    if (coeff_num != 5u)
    {
        printf("BIQUAD UART CSV: coeff_num NG: %u\n",
               (unsigned)coeff_num);
        return false;
    }

    if (ch_num != BIQUAD_CASCADE_NUM_CH)
    {
        printf("BIQUAD UART CSV: ch_num NG: %u expected=%u\n",
               (unsigned)ch_num,
               (unsigned)BIQUAD_CASCADE_NUM_CH);
        return false;
    }

    if ((stage_num == 0u) || (stage_num > BIQUAD_CASCADE_4CH_NUM_STAGE))
    {
        printf("BIQUAD UART CSV: stage_num NG: %u max=%u\n",
               (unsigned)stage_num,
               (unsigned)BIQUAD_CASCADE_4CH_NUM_STAGE);
        return false;
    }

    if (!app_biquad_cascade_4ch_is_bypass_active())
    {
        printf("BIQUAD UART CSV: rejected, Biquad process is not bypass active\n");
        return false;
    }

    // Validate all values before touching the active coefficient table.
    for (stage = 0u; stage < stage_num; stage++)
    {
        uint16_t coeff_idx;

        for (coeff_idx = 0u; coeff_idx < coeff_num; coeff_idx++)
        {
            for (ch = 0u; ch < ch_num; ch++)
            {
                const uint32_t index = (((uint32_t)stage * (uint32_t)coeff_num * (uint32_t)ch_num) +
                                        ((uint32_t)coeff_idx * (uint32_t)ch_num) +
                                        (uint32_t)ch);

                if (!isfinite(coeff[index]))
                {
                    printf("BIQUAD UART CSV: non-finite value: stage=%u coeff=%u ch=%u\n",
                           (unsigned)stage,
                           (unsigned)coeff_idx,
                           (unsigned)ch);
                    return false;
                }
            }
        }
    }

    /*
     * Runtime load policy:
     *   - Load received stages.
     *   - Remaining configured stages are set to bypass.
     *
     * This matches the existing static CSV policy where shorter CSV data can
     * leave remaining stages as safe bypass stages.
     */
    for(stage=0; stage<BIQUAD_CASCADE_4CH_NUM_STAGE; stage++ )
    {
        for(ch=0; ch<BIQUAD_CASCADE_NUM_CH; ch++ )
        {
            g_biquad_cascade_4ch_coeff[stage][ch] = biquad_bypass_1ch;
        }
    }

    for(stage=0; stage<stage_num; stage++ )
    {
        for(ch=0; ch<ch_num; ch++ )
        {
            const uint32_t base = (((uint32_t)stage * (uint32_t)coeff_num * (uint32_t)ch_num) +
                                   (uint32_t)ch);

            g_biquad_cascade_4ch_coeff[stage][ch].b0 = coeff[base + (0u * ch_num)];
            g_biquad_cascade_4ch_coeff[stage][ch].b1 = coeff[base + (1u * ch_num)];
            g_biquad_cascade_4ch_coeff[stage][ch].b2 = coeff[base + (2u * ch_num)];
            g_biquad_cascade_4ch_coeff[stage][ch].a1 = coeff[base + (3u * ch_num)];
            g_biquad_cascade_4ch_coeff[stage][ch].a2 = coeff[base + (4u * ch_num)];
        }
    }

#if defined(USE_CMSIS_IIR_DSP_PROCESS)
    // The active audio path uses CMSIS/MCHP DF2T.
    // Refresh the CMSIS coefficient layout and clear its state.
    biquad_cascade_4ch_cmsis_init();
#else
    app_biquad_cascade_4ch_clear_state();
#endif //defined(USE_CMSIS_IIR_DSP_PROCESS)

    printf("BIQUAD UART CSV: loaded stage=%u/%u ch=%u coeff=%u\n",
           (unsigned)stage_num,
           (unsigned)BIQUAD_CASCADE_4CH_NUM_STAGE,
           (unsigned)ch_num,
           (unsigned)coeff_num);

    return true;
}

void app_biquad_cascade_4ch_init(void)
{
    biquad_cascade_4ch_init(&My_BiquadCascade4ch);
    biquad_cascade_4ch_cmsis_init();

    biquad_cascade_4ch_set_proc_active_critical();

    s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;

#if defined(ENA_SELFTEST)
    printf("CMSIS DF2T selftest start!\n");
    float diff = biquad_cascade_4ch_cmsis_selftest();
    printf("CMSIS DF2T selftest diff = %.9f\n", diff);
    while(1)
    {
    }
#endif //defined(ENA_SELFTEST)
}


void app_biquad_cascade_4ch_request_bypass(void)
{
    if( s_biquad_proc_state == APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE )
    {
        s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_REQUESTED;

        biquad_cascade_4ch_set_proc_bypass_critical();
    }
}


bool app_biquad_cascade_4ch_is_bypass_active(void)
{
    return (s_biquad_proc_state == APP_BIQUAD_CASCADE_4CH_PROC_BYPASS_ACTIVE);
}


void app_biquad_cascade_4ch_request_normal(void)
{
    biquad_cascade_4ch_set_proc_active_critical();

    s_biquad_proc_state = APP_BIQUAD_CASCADE_4CH_PROC_NORMAL_ACTIVE;
}


void app_biquad_cascade_4ch_debug_print_coeff(uint16_t stage_begin,
                                               uint16_t stage_count)
{
    uint16_t stage_end = stage_begin + stage_count;

    if (stage_begin >= BIQUAD_CASCADE_4CH_NUM_STAGE)
    {
        printf("BIQUAD COEFF DBG: stage_begin out of range: %u max=%u\n",
               (unsigned)stage_begin,
               (unsigned)BIQUAD_CASCADE_4CH_NUM_STAGE);
        return;
    }

    if (stage_end > BIQUAD_CASCADE_4CH_NUM_STAGE)
    {
        stage_end = BIQUAD_CASCADE_4CH_NUM_STAGE;
    }

    printf("\n");
    printf("BIQUAD COEFF DBG: stage %u to %u\n",
           (unsigned)stage_begin,
           (unsigned)(stage_end - 1u));
    printf("  ch0=L1, ch1=R1, ch2=L2, ch3=R2 expected\n");

    for (uint16_t stage = stage_begin; stage < stage_end; stage++)
    {
        printf("stage=%u\n", (unsigned)stage);

        for (uint8_t ch = 0; ch < BIQUAD_CASCADE_NUM_CH; ch++)
        {
            const biquad_t* bq = &g_biquad_cascade_4ch_coeff[stage][ch];

            printf("  ch%u: b0=% .9f b1=% .9f b2=% .9f a1=% .9f a2=% .9f\n",
                   (unsigned)ch,
                   (double)bq->b0,
                   (double)bq->b1,
                   (double)bq->b2,
                   (double)bq->a1,
                   (double)bq->a2);
        }
    }

    printf("\n");
}


void app_biquad_cascade_4ch_process( const float* in, float* out )
{
    biquad_cascade_proc_func_t proc;

    /*
     * Take one stable snapshot of the shared process-function pointer.
     *
     * s_biquad_proc is updated from the main/control path and read from the
     * DMA0 audio ISR path.  The update itself is protected by temporarily
     * disabling DMA0 interrupts, but copying it to a local variable makes the
     * rule explicit: this audio block calls exactly the function pointer value
     * that was observed at this point.  If the main path switches bypass/normal
     * immediately after this read, that new setting simply takes effect from a
     * following block.
     *
     * This also keeps the volatile object read in one obvious place and makes
     * the value easier to inspect in a debugger.
     */
    proc = s_biquad_proc;

    proc( in, out, APP_BLOCK_FRAMES );
}

static void app_biquad_cascade_4ch_process_wrapper(const float* in,
                                                       float*       out,
                                                       int          samples)
{
    biquad_cascade_4ch_process(&My_BiquadCascade4ch, in, out, samples);
}








// static /*const*/ biquad_t g_biquad_cascade_4ch_coeff[BIQUAD_CASCADE_4CH_NUM_STAGE][BIQUAD_CASCADE_NUM_CH] =
// {
//     // Load-test coefficient example
//     // Peaking EQ: Fs=48kHz, F0=1kHz, Q=0.707, Gain=+3dB
// 
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=1)
//     // Stage 00
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=2)
//     // Stage 01
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=3)
//     // Stage 02
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=4)
//     // Stage 03
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=5)
//     // Stage 04
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=6)
//     // Stage 05
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=7)
//     // Stage 06
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=8)
//     // Stage 07
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=9)
//     // Stage 08
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=10)
//     // Stage 09
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=11)
//     // Stage 10
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=12)
//     // Stage 11
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=13)
//     // Stage 12
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=14)
//     // Stage 13
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=15)
//     // Stage 14
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=16)
//     // Stage 15
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=17)
//     // Stage 16
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=18)
//     // Stage 17
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=19)
//     // Stage 18
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=20)
//     // Stage 19
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=21)
//     // Stage 20
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=22)
//     // Stage 21
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=23)
//     // Stage 22
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=24)
//     // Stage 23
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=25)
//     // Stage 24
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=26)
//     // Stage 25
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=27)
//     // Stage 26
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=28)
//     // Stage 27
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=29)
//     // Stage 28
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=30)
//     // Stage 29
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=31)
//     // Stage 30
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=32)
//     // Stage 31
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=33)
//     // Stage 32
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=34)
//     // Stage 33
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=35)
//     // Stage 34
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=36)
//     // Stage 35
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=37)
//     // Stage 36
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=38)
//     // Stage 37
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=39)
//     // Stage 38
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=40)
//     // Stage 39
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=41)
//     // Stage 40
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=42)
//     // Stage 41
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=43)
//     // Stage 42
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=44)
//     // Stage 43
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=45)
//     // Stage 44
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=46)
//     // Stage 45
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=47)
//     // Stage 46
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=48)
//     // Stage 47
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=49)
//     // Stage 48
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=50)
//     // Stage 49
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=51)
//     // Stage 50
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=52)
//     // Stage 51
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=53)
//     // Stage 52
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=54)
//     // Stage 53
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=55)
//     // Stage 54
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=56)
//     // Stage 55
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=57)
//     // Stage 56
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=58)
//     // Stage 57
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=59)
//     // Stage 58
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// #if (BIQUAD_CASCADE_4CH_NUM_STAGE>=60)
//     // Stage 59
//     {
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch1 / L1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch2 / R1
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch3 / L2
//         { 1.02973215f, -1.83998013f, 0.82612509f, -1.83998013f, 0.85585724f },     // ch4 / R2
//     },
// #endif
// };

#endif //defined(ENA_BIQUAD_IIR_CASCADE)
