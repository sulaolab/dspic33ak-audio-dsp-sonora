/*******************************************************************************
*
*******************************************************************************/


/***  Include Files ***********************************************************/

#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "nora_uart.h"
#include "uart_platform_uart1_usb_serial_port.h"
#if defined(ENA_BIQUAD_IIR_CASCADE)
#include "biquad_cascade_4ch.h"   // classic DSP module: present only when the biquad feature is built
#endif


#include "app_biquad_coeff_csv.h"


#if defined(ENA_BIQUAD_IIR_CASCADE)
/***  Module Macros  **********************************************************/

#define APPDBG_UART_MARKER_CHAR                 ('#')
#define APPDBG_UART_ESC_CHAR                    (0x1Bu)
#define APPDBG_UART_MARKER_LINE_MAX             (128u)
#define APPDBG_BIQUAD_CSV_LINE_MAX              (160u)

#define APPDBG_BIQUAD_CSV_BEGIN_MARKER          "#BIQUAD_COEFF_CSV_BEGIN"
#define APPDBG_BIQUAD_CSV_END_MARKER            "#BIQUAD_COEFF_CSV_END"
#define APPDBG_BIQUAD_CSV_VERBOSE_PRINT          (0u)




/***  Module Types  ***********************************************************/

typedef enum {
    APPDBG_BIQUAD_CSV_RX_DATA = 0,
    APPDBG_BIQUAD_CSV_RX_WAIT_END,
} appdbg_biquad_csv_rx_state_t;

typedef enum {
    APPDBG_BIQUAD_COEFF_APPLY_IDLE = 0,
    APPDBG_BIQUAD_COEFF_APPLY_WAIT_BYPASS_ACTIVE,
    APPDBG_BIQUAD_COEFF_APPLY_COPY,
} appdbg_biquad_coeff_apply_state_t;


/***  Module Variables  *******************************************************/

static bool s_biquad_csv_receiving = false;
static bool s_biquad_csv_draining  = false;

static struct {
    appdbg_biquad_csv_rx_state_t state;
    char     line[APPDBG_BIQUAD_CSV_LINE_MAX];
    uint16_t idx;
    uint16_t row_count;
    uint16_t stage_num;
    uint16_t ch_num;
    uint16_t coeff_num;
    uint16_t expected_rows;
    float    staging[APPDBG_BIQUAD_CSV_STAGE_NUM]
                    [APPDBG_BIQUAD_CSV_COEFF_NUM]
                    [APPDBG_BIQUAD_CSV_CH_NUM];
} s_biquad_csv_rx;

static struct {
    bool     request;
    uint16_t stage_num;
    uint16_t ch_num;
    uint16_t coeff_num;
    float    coeff[APPDBG_BIQUAD_CSV_STAGE_NUM]
                 [APPDBG_BIQUAD_CSV_COEFF_NUM]
                 [APPDBG_BIQUAD_CSV_CH_NUM];
} s_biquad_coeff_pending;

static appdbg_biquad_coeff_apply_state_t s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;


/***  Module Function Prototypes  *********************************************/

static bool  local_biquad_csv_start_from_header( const char* line );
static void  local_biquad_csv_process_line( const char* line );
static void  local_biquad_csv_abort( const char* reason );
static void  local_biquad_csv_abort_and_drain( const char* reason );
static bool  local_biquad_csv_drain_char( uint8_t c );
static void  local_biquad_csv_clear_rx_control( void );
static void  local_biquad_csv_print_uart_diag( const char* tag );
static bool  local_biquad_csv_parse_header( const char* line, uint16_t* stage_num, uint16_t* ch_num, uint16_t* coeff_num );
static bool  local_biquad_csv_parse_line( const char* line, float v[APPDBG_BIQUAD_CSV_CH_NUM] );
static void  local_biquad_csv_store_row( uint16_t row, const float v[APPDBG_BIQUAD_CSV_CH_NUM] );
static bool  local_biquad_csv_commit_request( void );
static bool  local_biquad_csv_try_commit_end_marker_no_lf( void );

static bool  local_is_empty_line( const char* line );
static bool  local_str_starts_with( const char* s, const char* prefix );
static bool  local_parse_u16_strict( const char* s, uint16_t* v );




/***  Module Functions  *******************************************************/

/* Authoritative CSV BEGIN-marker predicate. Used by the input-source policy in
 * app_debug (reject a CSV BEGIN arriving on UART2) so the BEGIN string is not
 * duplicated across modules. */
bool app_biquad_coeff_csv_is_begin_marker( const char* line )
{
    return local_str_starts_with( line, APPDBG_BIQUAD_CSV_BEGIN_MARKER );
}

app_biquad_coeff_csv_marker_result_t app_biquad_coeff_csv_process_marker_line( const char* line )
{
    if( !local_str_starts_with( line, APPDBG_BIQUAD_CSV_BEGIN_MARKER ) )
    {
        return APP_BIQUAD_COEFF_CSV_MARKER_NOT_MATCHED;
    }

    if( local_biquad_csv_start_from_header( line ) )
    {
        return APP_BIQUAD_COEFF_CSV_MARKER_STARTED;
    }

    return APP_BIQUAD_COEFF_CSV_MARKER_CONSUMED;
}


bool app_biquad_coeff_csv_feed_char( uint8_t c )
{
    if( s_biquad_csv_draining )
    {
        return local_biquad_csv_drain_char( c );
    }

    if( !s_biquad_csv_receiving )
    {
        return false;
    }

    if( c == APPDBG_UART_ESC_CHAR )
    {
        local_biquad_csv_abort( "ESC" );
        return false;
    }

    if( c == '\r' )
    {
        return s_biquad_csv_receiving;
    }

    if( c != '\n' )
    {
        if( s_biquad_csv_rx.idx < (APPDBG_BIQUAD_CSV_LINE_MAX - 1u) )
        {
            s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = (char)c;
            s_biquad_csv_rx.idx = (uint16_t)(s_biquad_csv_rx.idx + 1u);

            /*
             * Robust END marker handling:
             *
             * Some file-transfer tools do not append a final newline after the
             * last line.  The normal parser finalizes a line only when '\n' is
             * received, so an END marker at EOF used to remain buffered forever
             * in APPDBG_BIQUAD_CSV_RX_WAIT_END.
             *
             * In WAIT_END state, the END marker is self-contained.  Therefore,
             * accept it immediately when the accumulated line exactly matches
             * APPDBG_BIQUAD_CSV_END_MARKER, without waiting for a newline.
             */
            s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = '\0';

            if( local_biquad_csv_try_commit_end_marker_no_lf() )
            {
                return s_biquad_csv_receiving;
            }
        }
        else
        {
            local_biquad_csv_abort_and_drain( "line too long" );
        }
        return s_biquad_csv_receiving;
    }

    s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = '\0';
    s_biquad_csv_rx.idx = 0u;

    local_biquad_csv_process_line( s_biquad_csv_rx.line );

    return s_biquad_csv_receiving;
}


bool app_biquad_coeff_csv_is_receiving(void)
{
    return (s_biquad_csv_receiving || s_biquad_csv_draining);
}


void app_biquad_coeff_csv_task(void)
{
    switch( s_biquad_coeff_apply_state )
    {
    case APPDBG_BIQUAD_COEFF_APPLY_IDLE:
        if( s_biquad_coeff_pending.request )
        {
            /* Safety net: CSV RX normally requests bypass at BEGIN header. */
            app_biquad_cascade_4ch_request_bypass();
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_WAIT_BYPASS_ACTIVE;
        }
        break;

    case APPDBG_BIQUAD_COEFF_APPLY_WAIT_BYPASS_ACTIVE:
        if( app_biquad_cascade_4ch_is_bypass_active() )
        {
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_COPY;
        }
        break;

    case APPDBG_BIQUAD_COEFF_APPLY_COPY:
        app_biquad_coeff_csv_copy_to_active( s_biquad_coeff_pending.coeff,
                                             s_biquad_coeff_pending.stage_num,
                                             s_biquad_coeff_pending.coeff_num,
                                             s_biquad_coeff_pending.ch_num );
        app_biquad_coeff_csv_clear_iir_state();
        s_biquad_coeff_pending.request = false;

        app_biquad_cascade_4ch_request_normal();

        printf("BIQUAD COEFF CSV APPLY OK\n");
        s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;
        break;

    default:
        s_biquad_coeff_pending.request = false;
        app_biquad_cascade_4ch_request_normal();
        s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;
        break;
    }
}


__attribute__((weak)) void app_biquad_coeff_csv_copy_to_active(
        const float coeff[APPDBG_BIQUAD_CSV_STAGE_NUM][APPDBG_BIQUAD_CSV_COEFF_NUM][APPDBG_BIQUAD_CSV_CH_NUM],
        uint16_t stage_num,
        uint16_t coeff_num,
        uint16_t ch_num )
{
//    (void)coeff;
//
//    printf("BIQUAD COEFF CSV COPY TO ACTIVE STUB: stage=%u coeff=%u ch=%u\n",
//           (unsigned)stage_num,
//           (unsigned)coeff_num,
//           (unsigned)ch_num);

    app_biquad_cascade_4ch_load_coeff_from_uart_csv( &coeff[0][0][0],
                                                     stage_num,
                                                     coeff_num,
                                                     ch_num );
}


__attribute__((weak)) void app_biquad_coeff_csv_clear_iir_state(void)
{
    /* Optional hook. Override this function if the IIR state should be cleared. */
}




/***  Local Functions  *******************************************************/ 

static bool local_biquad_csv_start_from_header( const char* line )
{
    uint16_t stage_num;
    uint16_t ch_num;
    uint16_t coeff_num;

    if( s_biquad_coeff_pending.request || (s_biquad_coeff_apply_state != APPDBG_BIQUAD_COEFF_APPLY_IDLE) )
    {
        printf("\nBIQUAD COEFF CSV busy. ignored.\n");
        return false;
    }

    if( !local_biquad_csv_parse_header( line, &stage_num, &ch_num, &coeff_num ) )
    {
        printf("\nBIQUAD COEFF CSV header error: %s\n", line);
        return false;
    }

    /* Request bypass as early as possible to reduce CPU load during CSV RX. */
    app_biquad_cascade_4ch_request_bypass();

    s_biquad_csv_draining = false;
    local_biquad_csv_clear_rx_control();
    /* Biquad CSV bulk transfer is a UART1-only transport feature (UART2 is
     * interactive console only and is rejected at CSV BEGIN in app_debug), so
     * this module intentionally operates on UART1 directly.
     * Backend-aware: clears the ring counters in ISR mode, no-op in polling. */
    (void)nora_uart_rx_status_clear(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST);


    s_biquad_csv_rx.state         = APPDBG_BIQUAD_CSV_RX_DATA;
    s_biquad_csv_rx.stage_num     = stage_num;
    s_biquad_csv_rx.ch_num        = ch_num;
    s_biquad_csv_rx.coeff_num     = coeff_num;
    s_biquad_csv_rx.expected_rows = (uint16_t)(stage_num * coeff_num);

    s_biquad_csv_receiving = true;

    printf("\nBIQUAD COEFF CSV RX START: stage=%u coeff=%u ch=%u rows=%u\n",
           (unsigned)stage_num,
           (unsigned)coeff_num,
           (unsigned)ch_num,
           (unsigned)s_biquad_csv_rx.expected_rows);

    return true;
}


static void local_biquad_csv_process_line( const char* line )
{
    float v[APPDBG_BIQUAD_CSV_CH_NUM];

    if( local_is_empty_line( line ) )
    {
        return;
    }

    if( s_biquad_csv_rx.state == APPDBG_BIQUAD_CSV_RX_WAIT_END )
    {
        if( local_str_starts_with( line, APPDBG_BIQUAD_CSV_END_MARKER ) )
        {
            if( local_biquad_csv_commit_request() )
            {
                printf("BIQUAD COEFF CSV RX DONE. apply requested.\n");
                local_biquad_csv_print_uart_diag( "DONE" );
                local_biquad_csv_clear_rx_control();
                s_biquad_csv_receiving = false;
            }
        }
        else
        {
            local_biquad_csv_abort_and_drain( "END marker expected" );
        }
        return;
    }

    if( local_str_starts_with( line, APPDBG_BIQUAD_CSV_END_MARKER ) )
    {
        printf("\nBIQUAD COEFF CSV row count mismatch. expected=%u actual=%u\n",
               (unsigned)s_biquad_csv_rx.expected_rows,
               (unsigned)s_biquad_csv_rx.row_count);
        local_biquad_csv_abort( "too few rows" );
        return;
    }

    if( line[0] == APPDBG_UART_MARKER_CHAR )
    {
        /* Comment/control line in CSV body. Unknown lines are ignored before all rows arrive. */
        return;
    }

    if( s_biquad_csv_rx.row_count >= s_biquad_csv_rx.expected_rows )
    {
        local_biquad_csv_abort_and_drain( "too many rows" );
        return;
    }

    if( !local_biquad_csv_parse_line( line, v ) )
    {
        printf("\nBIQUAD COEFF CSV parse error at row=%u len=%u head=%.48s\n",
               (unsigned)s_biquad_csv_rx.row_count,
               (unsigned)strlen( line ),
               line);
        local_biquad_csv_abort_and_drain( "parse error" );
        return;
    }

    local_biquad_csv_store_row( s_biquad_csv_rx.row_count, v );
    s_biquad_csv_rx.row_count = (uint16_t)(s_biquad_csv_rx.row_count + 1u);

    if( s_biquad_csv_rx.row_count >= s_biquad_csv_rx.expected_rows )
    {
        s_biquad_csv_rx.state = APPDBG_BIQUAD_CSV_RX_WAIT_END;
#if (APPDBG_BIQUAD_CSV_VERBOSE_PRINT != 0u)
        printf("BIQUAD COEFF CSV rows received. waiting END marker.\n");
#endif
    }
}


static bool local_biquad_csv_try_commit_end_marker_no_lf( void )
{
    if( s_biquad_csv_rx.state != APPDBG_BIQUAD_CSV_RX_WAIT_END )
    {
        return false;
    }

    if( strcmp( s_biquad_csv_rx.line, APPDBG_BIQUAD_CSV_END_MARKER ) != 0 )
    {
        return false;
    }

    if( local_biquad_csv_commit_request() )
    {
        printf("BIQUAD COEFF CSV RX DONE. apply requested.\n");
        local_biquad_csv_print_uart_diag( "DONE_END" );
        local_biquad_csv_clear_rx_control();
        UART1_RxFlush();
        s_biquad_csv_receiving = false;
    }

    return true;
}



static void local_biquad_csv_abort( const char* reason )
{
    printf("\nBIQUAD COEFF CSV RX ABORT: %s\n", (reason != NULL) ? reason : "unknown");
    local_biquad_csv_print_uart_diag( "ABORT" );

    app_biquad_cascade_4ch_request_normal();

    local_biquad_csv_clear_rx_control();
    s_biquad_csv_receiving = false;
    s_biquad_csv_draining  = false;
}


static void local_biquad_csv_abort_and_drain( const char* reason )
{
    local_biquad_csv_abort( reason );
    s_biquad_csv_draining = true;
}


static bool local_biquad_csv_drain_char( uint8_t c )
{
    if( c == APPDBG_UART_ESC_CHAR )
    {
        local_biquad_csv_clear_rx_control();
        s_biquad_csv_draining = false;
        return false;
    }

    if( c == '\r' )
    {
        return true;
    }

    if( c != '\n' )
    {
        if( s_biquad_csv_rx.idx < (APPDBG_BIQUAD_CSV_LINE_MAX - 1u) )
        {
            s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = (char)c;
            s_biquad_csv_rx.idx = (uint16_t)(s_biquad_csv_rx.idx + 1u);
            s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = '\0';

            if( strcmp( s_biquad_csv_rx.line, APPDBG_BIQUAD_CSV_END_MARKER ) == 0 )
            {
                printf("BIQUAD COEFF CSV DRAIN DONE. END marker consumed.\n");
                local_biquad_csv_clear_rx_control();
                s_biquad_csv_draining = false;
                return false;
            }
        }
        else
        {
            /* Keep draining, but reset the temporary line buffer to avoid overflow. */
            s_biquad_csv_rx.idx = 0u;
            s_biquad_csv_rx.line[0] = '\0';
        }

        return true;
    }

    s_biquad_csv_rx.line[s_biquad_csv_rx.idx] = '\0';

    if( local_str_starts_with( s_biquad_csv_rx.line, APPDBG_BIQUAD_CSV_END_MARKER ) )
    {
        printf("BIQUAD COEFF CSV DRAIN DONE. END marker consumed.\n");
        local_biquad_csv_clear_rx_control();
        s_biquad_csv_draining = false;
        return false;
    }

    s_biquad_csv_rx.idx = 0u;
    s_biquad_csv_rx.line[0] = '\0';

    return true;
}


static void local_biquad_csv_clear_rx_control( void )
{
    s_biquad_csv_rx.state         = APPDBG_BIQUAD_CSV_RX_DATA;
    s_biquad_csv_rx.idx           = 0u;
    s_biquad_csv_rx.row_count     = 0u;
    s_biquad_csv_rx.stage_num     = 0u;
    s_biquad_csv_rx.ch_num        = 0u;
    s_biquad_csv_rx.coeff_num     = 0u;
    s_biquad_csv_rx.expected_rows = 0u;
    s_biquad_csv_rx.line[0]       = '\0';
}


static void local_biquad_csv_print_uart_diag( const char* tag )
{
    /* Backend-aware: ISR mode fills the ring counters; polling returns zeros. */
    nora_uart_rx_status_t hal_status;

    if (nora_uart_rx_status_get(UART_PLATFORM_UART1_USB_SERIAL_PORT_INST, &hal_status) != NORA_UART_OK) {
        memset(&hal_status, 0, sizeof(hal_status));
        hal_status.rx_mode = NORA_UART_RX_MODE_POLLING;
    }

    printf("BIQUAD CSV UART DIAG %s: row=%u/%u idx=%u state=%u isr=%lu rx=%lu fifo_ovf=%lu ferr=%lu perr=%lu ring_ovf=%lu max_drain=%u\n",
           (tag != NULL) ? tag : "",
           (unsigned)s_biquad_csv_rx.row_count,
           (unsigned)s_biquad_csv_rx.expected_rows,
           (unsigned)s_biquad_csv_rx.idx,
           (unsigned)s_biquad_csv_rx.state,
           (unsigned long)hal_status.rx_isr_count,
           (unsigned long)hal_status.rx_byte_count,
           (unsigned long)hal_status.rx_fifo_overflow_count,
           (unsigned long)hal_status.framing_error_count,
           (unsigned long)hal_status.parity_error_count,
           (unsigned long)hal_status.rx_ring_overflow_count,
           (unsigned)hal_status.rx_max_drain_count);
}


static bool local_biquad_csv_parse_header( const char* line, uint16_t* stage_num, uint16_t* ch_num, uint16_t* coeff_num )
{
    char     tmp[APPDBG_UART_MARKER_LINE_MAX];
    char*    token;
    uint16_t stage;
    uint16_t chn;
    uint16_t coeff;

    if( line == NULL )      return false;
    if( stage_num == NULL ) return false;
    if( ch_num == NULL )    return false;
    if( coeff_num == NULL ) return false;

    if( !local_str_starts_with( line, APPDBG_BIQUAD_CSV_BEGIN_MARKER ) )
    {
        printf("\nbiquad_csv_parse_header: APPDBG_BIQUAD_CSV_BEGIN_MARKER error. 1\n");
        return false;
    }
    strncpy( tmp, line, sizeof(tmp) );
    tmp[sizeof(tmp) - 1u] = '\0';

    token = strtok( tmp, "," );
    if( token == NULL )
    {
        printf("\nbiquad_csv_parse_header: MULL error. 0\n");
        return false;
    }
    if( strcmp( token, APPDBG_BIQUAD_CSV_BEGIN_MARKER ) != 0 )
    {
        printf("\nbiquad_csv_parse_header: APPDBG_BIQUAD_CSV_BEGIN_MARKER error. 2\n");
        return false;
    }

    token = strtok( NULL, "," );
    if( token == NULL )
    {
        printf("\nbiquad_csv_parse_header: MULL error. 0\n");
        return false;
    }
    if( strcmp( token, "V1" ) != 0 )
    {
        printf("\nbiquad_csv_parse_header: V1 error.\n");
        return false;
    }
    token = strtok( NULL, "," );
    if( !local_parse_u16_strict( token, &stage ) )
    {
        printf("\nbiquad_csv_parse_header: V1 error.\n");
        return false;
    }
    token = strtok( NULL, "," );
    if( !local_parse_u16_strict( token, &chn ) )
    {
        printf("\nbiquad_csv_parse_header: V1 error.\n");
        return false;
    }

    token = strtok( NULL, "," );
    if( !local_parse_u16_strict( token, &coeff ) )
    {
        return false;
    }

    token = strtok( NULL, "," );
    if( token != NULL )
    {
        return false;
    }

    if( stage > APPDBG_BIQUAD_CSV_STAGE_NUM )
    {
        printf("\nbiquad_csv_parse_header: stage num > APPDBG_BIQUAD_CSV_STAGE_NUM(%d)\n", APPDBG_BIQUAD_CSV_STAGE_NUM);
        return false;
    }
    if( chn   != APPDBG_BIQUAD_CSV_CH_NUM )
    {
        printf("\nbiquad_csv_parse_header: ch num != APPDBG_BIQUAD_CSV_CH_NUM(%d)\n", APPDBG_BIQUAD_CSV_CH_NUM);
        return false;
    }
    if( coeff != APPDBG_BIQUAD_CSV_COEFF_NUM )
    {
        printf("\nbiquad_csv_parse_header: coeff num != APPDBG_BIQUAD_CSV_COEFF_NUM(%d)\n", APPDBG_BIQUAD_CSV_COEFF_NUM);
        return false;
    }

    *stage_num = stage;
    *ch_num    = chn;
    *coeff_num = coeff;

    return true;
}


static bool local_biquad_csv_parse_line( const char* line, float v[APPDBG_BIQUAD_CSV_CH_NUM] )
{
    const char* p;
    char*       endp;
    uint16_t    ch;

    if( line == NULL ) return false;
    if( v == NULL )    return false;

    p = line;

    for( ch = 0u; ch < APPDBG_BIQUAD_CSV_CH_NUM; ch++ )
    {
        double d;

        while( (*p == ' ') || (*p == '\t') )
        {
            p++;
        }

        d = strtod( p, &endp );

        if( endp == p )
        {
            return false;
        }

        v[ch] = (float)d;
        p = endp;

        while( (*p == ' ') || (*p == '\t') )
        {
            p++;
        }

        if( ch < (APPDBG_BIQUAD_CSV_CH_NUM - 1u) )
        {
            if( *p != ',' )
            {
                return false;
            }
            p++;
        }
    }

    while( (*p == ' ') || (*p == '\t') )
    {
        p++;
    }

    if( *p != '\0' )
    {
        return false;
    }

    return true;
}


static void local_biquad_csv_store_row( uint16_t row, const float v[APPDBG_BIQUAD_CSV_CH_NUM] )
{
    uint16_t stage;
    uint16_t coeff_idx;
    uint16_t ch;

    stage     = row / s_biquad_csv_rx.coeff_num;
    coeff_idx = row % s_biquad_csv_rx.coeff_num;

    for( ch = 0u; ch < s_biquad_csv_rx.ch_num; ch++ )
    {
        s_biquad_csv_rx.staging[stage][coeff_idx][ch] = v[ch];
    }
}


static bool local_biquad_csv_commit_request( void )
{
    uint16_t stage;
    uint16_t coeff_idx;
    uint16_t ch;

    if( s_biquad_csv_rx.row_count != s_biquad_csv_rx.expected_rows )
    {
        printf("\nBIQUAD COEFF CSV row count mismatch. expected=%u actual=%u\n",
               (unsigned)s_biquad_csv_rx.expected_rows,
               (unsigned)s_biquad_csv_rx.row_count);
        local_biquad_csv_abort( "row count mismatch" );
        return false;
    }

    if( s_biquad_coeff_pending.request || (s_biquad_coeff_apply_state != APPDBG_BIQUAD_COEFF_APPLY_IDLE) )
    {
        local_biquad_csv_abort( "apply busy" );
        return false;
    }

    s_biquad_coeff_pending.stage_num = s_biquad_csv_rx.stage_num;
    s_biquad_coeff_pending.ch_num    = s_biquad_csv_rx.ch_num;
    s_biquad_coeff_pending.coeff_num = s_biquad_csv_rx.coeff_num;

    for( stage = 0u; stage < s_biquad_csv_rx.stage_num; stage++ )
    {
        for( coeff_idx = 0u; coeff_idx < s_biquad_csv_rx.coeff_num; coeff_idx++ )
        {
            for( ch = 0u; ch < s_biquad_csv_rx.ch_num; ch++ )
            {
                s_biquad_coeff_pending.coeff[stage][coeff_idx][ch] = s_biquad_csv_rx.staging[stage][coeff_idx][ch];
            }
        }
    }

    s_biquad_coeff_pending.request = true;

    return true;
}


static bool local_is_empty_line( const char* line )
{
    if( line == NULL ) return true;

    while( (*line == ' ') || (*line == '\t') )
    {
        line++;
    }

    return (*line == '\0');
}


static bool local_str_starts_with( const char* s, const char* prefix )
{
    size_t n;

    if( s == NULL )      return false;
    if( prefix == NULL ) return false;

    n = strlen( prefix );

    return (strncmp( s, prefix, n ) == 0);
}


static bool local_parse_u16_strict( const char* s, uint16_t* v )
{
    char*         endp;
    unsigned long x;

    if( s == NULL ) return false;
    if( v == NULL ) return false;

    x = strtoul( s, &endp, 10 );

    if( endp == s ) return false;
    if( *endp != '\0' ) return false;
    if( x > 65535u ) return false;

    *v = (uint16_t)x;

    return true;
}

#endif //defined(ENA_BIQUAD_IIR_CASCADE)
