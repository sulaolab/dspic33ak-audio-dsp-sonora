#if defined(ENA_BIQUAD_IIR_CASCADE)
/*******************************************************************************
*
*******************************************************************************/

#if !defined(__APP_BIQUAD_COEFF_CSV_H__)
#define      __APP_BIQUAD_COEFF_CSV_H__

#include <stdint.h>
#include <stdbool.h>



/***  Module Macros  **********************************************************/

#define APPDBG_BIQUAD_CSV_STAGE_NUM       (30u)
#define APPDBG_BIQUAD_CSV_COEFF_NUM       (5u)
#define APPDBG_BIQUAD_CSV_CH_NUM          (4u)



/***  Module Types  ***********************************************************/

typedef enum {
    APP_BIQUAD_COEFF_CSV_MARKER_NOT_MATCHED = 0,
    APP_BIQUAD_COEFF_CSV_MARKER_CONSUMED,
    APP_BIQUAD_COEFF_CSV_MARKER_STARTED,
} app_biquad_coeff_csv_marker_result_t;


/***  Module Variables  *******************************************************/


/***  Module Function Prototypes  *********************************************/

extern app_biquad_coeff_csv_marker_result_t app_biquad_coeff_csv_process_marker_line( const char* line );
extern bool  app_biquad_coeff_csv_is_begin_marker( const char* line );
extern bool  app_biquad_coeff_csv_feed_char( uint8_t c );
extern void  app_biquad_coeff_csv_task(void);
extern bool  app_biquad_coeff_csv_is_receiving(void);

extern void  app_biquad_coeff_csv_copy_to_active(
        const float coeff[APPDBG_BIQUAD_CSV_STAGE_NUM][APPDBG_BIQUAD_CSV_COEFF_NUM][APPDBG_BIQUAD_CSV_CH_NUM],
        uint16_t stage_num,
        uint16_t coeff_num,
        uint16_t ch_num );
extern void  app_biquad_coeff_csv_clear_iir_state(void);




#endif //!defined(__APP_BIQUAD_COEFF_CSV_H__)
#endif //defined(ENA_BIQUAD_IIR_CASCADE)