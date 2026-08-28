#include "app_specific_config_defs.h"
#if defined(ENA_BIQUAD_IIR_CASCADE)
#ifndef _BIQUAD_CASCADE_4CH_H
#define _BIQUAD_CASCADE_4CH_H

//===========================================================
// INCLUDES
//===========================================================
#include <stdbool.h>
#include <stdint.h>
#include "app_utils.h"  /* biquad_stat_t */

//===========================================================
// Definition
//===========================================================

// to print CSV coefficient load summary at initialization.
#define ENA_CSV_REPORT_PRINTF

//#define BIQUAD_CASCADE_NUM_CH       (4)
#define BIQUAD_CASCADE_NUM_CH       (STAGE_2_PROC_CH)

// to route app_biquad_cascade_4ch_process() to the CMSIS/MCHP path.
#define USE_CMSIS_IIR_DSP_PROCESS


// Stage count. The value and its measured history are chosen in ONE place --
// app_specific_config_defs.h, section (2.5f), the DRC branch. Set it there, not here.
// This is only a fallback for a build that reaches this header without that definition;
// keep it conservative and do not grow a list of past values here again.
#if !defined(BIQUAD_CASCADE_4CH_NUM_STAGE)
#define BIQUAD_CASCADE_4CH_NUM_STAGE    (22)
#endif //!defined(BIQUAD_CASCADE_4CH_NUM_STAGE)



// for debug safety.
//#define  ENA_NAN_INF_PROTECT

// CMSIS DF2T convention commonly uses +a1/+a2 in state update.
// Current local implementation uses: z1 = b1*x - a1*y + z2; z2 = b2*x - a2*y.
// Therefore 1 is expected to match the current local coefficient table.
// If the self-test difference is large, try setting this to 0.
#define CMSIS_NEGATE_A_COEFF            (1)


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    biquad_stat_t st[BIQUAD_CASCADE_4CH_NUM_STAGE][BIQUAD_CASCADE_NUM_CH];

    uint32_t nan_inf_count;
    int      last_nan_inf_stage;
    int      last_nan_inf_ch;

} biquad_cascade_4ch_t;


//===========================================================
// Variables
//===========================================================

// CSV coefficient load debug information.
extern volatile int g_biquad_cascade_4ch_csv_load_result;
extern volatile int g_biquad_cascade_4ch_csv_load_error_index;
extern volatile int g_biquad_cascade_4ch_csv_total_values;
extern volatile int g_biquad_cascade_4ch_csv_expected_values;
extern volatile int g_biquad_cascade_4ch_csv_stage_count;
extern volatile int g_biquad_cascade_4ch_csv_remainder_values;


//===========================================================
// Function Prototype
//===========================================================

extern void     biquad_cascade_4ch_init( biquad_cascade_4ch_t* pbq );
extern void     biquad_cascade_4ch_reset( biquad_cascade_4ch_t* pbq );
extern void     biquad_cascade_4ch_process(       biquad_cascade_4ch_t* pbq,
                                                const float*                in,
                                                      float*                out,
                                                      int                   samples );
extern void     biquad_cascade_4ch_cmsis_init(void);
extern void     biquad_cascade_4ch_process_cmsis( const float* in,
                                                            float* out,
                                                            int    samples );
extern float    biquad_cascade_4ch_cmsis_selftest(void);



//===========================================================
// API
//===========================================================

extern void     app_biquad_cascade_4ch_init(void);
extern void     app_biquad_cascade_4ch_process( const float* in, float* out );
extern bool     app_biquad_cascade_4ch_load_coeff_from_uart_csv( const float* coeff,
                                                                 uint16_t     stage_num,
                                                                 uint16_t     coeff_num,
                                                                 uint16_t     ch_num );
extern void     app_biquad_cascade_4ch_clear_state(void);
extern void     app_biquad_cascade_4ch_request_bypass(void);
extern bool     app_biquad_cascade_4ch_is_bypass_active(void);
extern void     app_biquad_cascade_4ch_request_normal(void);

extern void     app_biquad_cascade_4ch_debug_print_coeff( uint16_t stage_begin,
                                                          uint16_t stage_count );




#endif //!_BIQUAD_CASCADE_4CH_H
#endif //defined(ENA_BIQUAD_IIR_CASCADE)
