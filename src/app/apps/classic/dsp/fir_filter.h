#if defined(ENA_FIR_FILTER)
#ifndef _FIR_FILTER_H
#define _FIR_FILTER_H

//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================

#define FIR_FILTER_MAX_CH       (4u)
#define FIR_FILTER_MAX_TAPS     (256u)

#define FIR_FILTER_DEFAULT_CH   (4u)

// to route app_fir_filter_process() to the MCHP/CMSIS FIR path.
#define USE_CMSIS_FIR_DSP_PROCESS

// to enable C reference vs MCHP/CMSIS FIR multi-block self-test.
// #define ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    FIR_FILTER_SMOKE_TEST_BYPASS = 0,
    FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_8,
    FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_16,
    FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_32,
    FIR_FILTER_SMOKE_TEST_MOVING_AVERAGE_64,
    FIR_FILTER_SMOKE_TEST_EDGE_2,
    FIR_FILTER_SMOKE_TEST_DELAY_MIX_16,
    FIR_FILTER_SMOKE_TEST_DELAY_MIX_32,
} fir_filter_smoke_test_t;


typedef struct
{
    uint16_t num_ch;
    uint16_t num_taps;

    /*
     * Coefficient order:
     *
     *   coeff[0] = b[0] for x[n]
     *   coeff[1] = b[1] for x[n-1]
     *   ...
     *   coeff[num_taps - 1] = b[num_taps - 1] for x[n-num_taps+1]
     *
     * This order is intentionally aligned with the current MCHP FIR f32 ASM
     * operation:
     *   y[n] = sum_(m=0:M-1){h[m] * x[n-m]}
     */
    float coeff[FIR_FILTER_MAX_TAPS];

    /*
     * Per-channel circular delay line.
     *
     * Each channel has an independent write index.  The delay line length used
     * by the process function is num_taps, not FIR_FILTER_MAX_TAPS.
     */
    float state[FIR_FILTER_MAX_CH][FIR_FILTER_MAX_TAPS];
    uint16_t write_index[FIR_FILTER_MAX_CH];

} fir_filter_t;


//===========================================================
// Variables
//===========================================================

// Last app_fir_filter_process() processing time in high-res timer counts.
// Intended for DMA debug print only.
extern volatile uint32_t g_fir_filter_process_dt;
extern volatile uint16_t g_fir_filter_process_num_ch;
extern volatile uint16_t g_fir_filter_process_num_taps;


//===========================================================
// Function Prototype
//===========================================================

extern void     fir_filter_init( fir_filter_t* pfir );
extern void     fir_filter_reset( fir_filter_t* pfir );
extern bool     fir_filter_set_config( fir_filter_t* pfir,
                                       const float*  coeff,
                                       uint16_t      num_taps,
                                       uint16_t      num_ch );
extern bool     fir_filter_get_config( const fir_filter_t* pfir,
                                       float*              coeff,
                                       uint16_t            coeff_array_size,
                                       uint16_t*           num_taps,
                                       uint16_t*           num_ch );
extern void     fir_filter_process(       fir_filter_t* pfir,
                                        const float*        in,
                                              float*        out,
                                              uint16_t      samples );
extern bool     fir_filter_apply_smoke_test( fir_filter_t*            pfir,
                                             fir_filter_smoke_test_t smoke_test,
                                             uint16_t                num_ch );
extern float    fir_filter_selftest(void);
#if defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)
extern float    fir_filter_c_vs_cmsis_selftest(void);
#endif //defined(ENA_FIR_FILTER_C_VS_CMSIS_SELFTEST)


//===========================================================
// API
//===========================================================

extern void     app_fir_filter_init(void);
extern void     app_fir_filter_clear_state(void);
extern bool     app_fir_filter_set_config( const float* coeff,
                                           uint16_t     num_taps,
                                           uint16_t     num_ch );
extern bool     app_fir_filter_get_config( float*    coeff,
                                           uint16_t  coeff_array_size,
                                           uint16_t* num_taps,
                                           uint16_t* num_ch );
extern void     app_fir_filter_process( const float* in,
                                                  float* out );
extern bool     app_fir_filter_apply_smoke_test( fir_filter_smoke_test_t smoke_test,
                                                 uint16_t                num_ch );
extern bool     app_fir_filter_set_smoke_test_bypass( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_moving_average_8( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_moving_average_16( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_moving_average_32( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_moving_average_64( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_edge_2( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_delay_mix_16( uint16_t num_ch );
extern bool     app_fir_filter_set_smoke_test_delay_mix_32( uint16_t num_ch );


#endif //!_FIR_FILTER_H
#endif //defined(ENA_FIR_FILTER)
