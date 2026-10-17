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
#include "nora_high_res_timer.h"   // parse-burst duration (us), diagnostics only
#include "nora_spi_i2s_tdm.h"      // authoritative transport running/stopped predicate
#include "timer_app.h"             // GetTicks() ms tick: CSV transport inactivity deadline
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

/*
 * Deferred parse (transport / semantics split).
 *
 * 0 = legacy: every byte is parsed on arrival, so CSV semantics (strtod, row
 *     validation, staging writes) are executed inside the UART drain loop.
 * 1 = deferred: the receive path only frames the transaction and copies raw
 *     bytes; the whole CSV is parsed once, from the task, after END.
 *
 * Kept as a switch so the two can be A/B'd on the same hardware and so a
 * regression can be answered by rebuilding rather than by reverting.
 */
#ifndef APP_CSV_DEFER_PARSE
#error "APP_CSV_DEFER_PARSE must be defined by app_specific_config_defs.h (0 or 1). No default: an accidental 1 silently allocates the raw buffer below and gives up the RAM the sample-delay pool needs it for -- see [internal] csv_deferred_parse_2026-09-18.md section 9."
#endif

#if APP_CSV_DEFER_PARSE
/*
 * Raw transaction buffer size.
 *
 * Sized from the actual generator output, not from the protocol's absolute
 * ceiling: real 4-channel CSVs run 46-52 bytes per data row (largest fixture in
 * the tree: 5231 bytes / 110 rows), so the protocol maximum of
 * APPDBG_BIQUAD_CSV_STAGE_NUM * APPDBG_BIQUAD_CSV_COEFF_NUM = 150 rows lands
 * near 7.8 kB. 8 kB covers that, and covers every fixture that exists with 57%
 * to spare.
 *
 * It is deliberately NOT the protocol worst case: a CSV whose every row used
 * the full APPDBG_BIQUAD_CSV_LINE_MAX would need 24 kB, which this part does
 * not have. Exceeding this buffer is therefore a supported outcome -- it aborts
 * with a named reason and the byte count, it does not corrupt or wedge.
 */
#ifndef APP_CSV_RAW_BUF_BYTES
#define APP_CSV_RAW_BUF_BYTES                   (8u * 1024u)
#endif
#endif //APP_CSV_DEFER_PARSE

/*
 * Inactivity deadline for the UART1 CSV transport (both receive and drain).
 *
 * Without one, a transfer that loses bytes -- ring overflow at high DSP load --
 * parks in the receive state waiting for rows that will never arrive, holding
 * the console's input-source lock: the console stops answering and only a raw
 * ESC byte recovers it. The UART2 reject-drain path already had a deadline
 * (UART2_CSV_REJECT_DRAIN_TIMEOUT_MS); this is the UART1 side of the same idea.
 *
 * 2 s is far longer than any gap a file transfer produces at 230400 baud
 * (43.4 us/byte, and Tera Term's per-line transmit delay is single-digit ms),
 * so it cannot fire on a healthy transfer.
 */
#ifndef APPDBG_BIQUAD_CSV_RX_TIMEOUT_MS
#define APPDBG_BIQUAD_CSV_RX_TIMEOUT_MS         (2000u)
#endif

/*
 * How long the apply state machine waits for the audio ISR to acknowledge the
 * bypass swap while the transport is still running.
 *
 * The acknowledgement is the ISR's own proof that it is not inside the
 * coefficient table. With the transport stopped (*ts, or before the first
 * start) no block ever runs, so the proof never arrives and the apply used to
 * park here forever -- CSV parsed perfectly, APPLY OK never printed.
 *
 * A stopped transport is detected directly with nora_spi_i2s_tdm_is_running(),
 * the audio transport's authoritative stream predicate, and is safe to copy
 * immediately.  This timeout is therefore only the fail-safe for a transport
 * that reports running but does not acknowledge bypass.
 */
#ifndef APPDBG_BIQUAD_CSV_BYPASS_ACK_TIMEOUT_MS
#define APPDBG_BIQUAD_CSV_BYPASS_ACK_TIMEOUT_MS (200u)
#endif




/***  Module Types  ***********************************************************/

typedef enum {
    APPDBG_BIQUAD_CSV_RX_DATA = 0,
    APPDBG_BIQUAD_CSV_RX_WAIT_END,
    /* Deferred parse only. Kept in the same enum because the DIAG line prints
     * this value as state=%u and one numbering is easier to read than two. */
    APPDBG_BIQUAD_CSV_RX_RAW,             /* capturing raw bytes, nothing interpreted   */
    APPDBG_BIQUAD_CSV_RX_PARSE_PENDING,   /* END framed, parse owed to the task         */
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

/*
 * Apply request.
 *
 * This used to carry its own full copy of the coefficient cube, filled from
 * s_biquad_csv_rx.staging at END -- 2400 bytes of RAM and a 600-float copy for
 * no reachable benefit: a second BEGIN is refused for as long as request is set
 * (see local_biquad_csv_start_from_header()), so the two cubes were never live
 * with different contents. The staging cube IS the transaction now, and this
 * holds only its shape.
 */
static struct {
    bool     request;
    uint16_t stage_num;
    uint16_t ch_num;
    uint16_t coeff_num;
} s_biquad_coeff_pending;

static appdbg_biquad_coeff_apply_state_t s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;

/* Tick at which WAIT_BYPASS_ACTIVE was entered (bypass-acknowledgement deadline). */
static uint32_t s_biquad_coeff_apply_wait_start_ms = 0u;

/*
 * Transport-level bookkeeping and diagnostics.
 *
 * last_rx_ms is refreshed per byte and is the only thing the inactivity
 * deadline reads, so it must stay this cheap: at 98% DSP load the whole
 * per-byte budget is ~0.7 us.
 */
static uint32_t s_biquad_csv_last_rx_ms   = 0u;
static uint32_t s_biquad_csv_rx_start_ms  = 0u;

/*
 * Progress counter, incremented once per byte handed to this module, and the
 * value the task last saw.
 *
 * The deadline needs to know "did anything arrive since my last pass", and this
 * is the cheapest possible way to say it: one increment per byte instead of a
 * GetTicks() call per byte. At 98% DSP load the main loop gets ~1.6% of the
 * block period, and 230400 baud is 43.4 us per byte, so the whole per-byte
 * budget is well under a microsecond -- a tick read per byte is a measurable
 * share of it, and the only thing the deadline actually needs is a change
 * detector.
 */
static uint32_t s_biquad_csv_rx_progress      = 0u;
static uint32_t s_biquad_csv_rx_progress_seen = 0u;

/*
 * UART RX baseline for reporting every transaction's counters as a delta.
 *
 * This is NOT a snapshot taken at BEGIN. It replaced a nora_uart_rx_status_clear()
 * call at BEGIN first (zeroing the counters there destroyed the evidence of the
 * failure it was meant to help diagnose -- see the git history for that one), and
 * a BEGIN-time snapshot has the same defect one level up: the header line takes
 * only ~1.5 ms of wire time to arrive, and this module has no way to run any
 * earlier than "the main loop happened to notice the trailing newline" -- under
 * load that recognition itself lags behind the wire by some amount, and loss
 * occurring in that lag is baked into a BEGIN-time snapshot as if it had already
 * happened before the transfer, becoming invisible to every delta this module
 * computes for the rest of the transaction.
 *
 * So instead this baseline is refreshed continuously by
 * local_biquad_csv_track_idle_baseline() every main-loop pass while the module is
 * idle, and is deliberately NOT refreshed again once a transaction starts. BEGIN
 * recognition and the first idle-refresh that is skipped because of it happen
 * inside the SAME app_uart_process() pass (start_from_header() runs synchronously
 * off the trailing byte of the header line, before the task's refresh call for
 * that pass), so the value in place when a transaction starts is whatever the
 * PREVIOUS pass left here -- at most one main-loop iteration old, not "however
 * long BEGIN recognition took under load".
 *
 * That one-iteration residual is conservative, not a remaining blind spot: it
 * can only attribute a fault from just before the transfer TO the transfer (a
 * false-positive reject of what was otherwise a clean transfer), because a
 * stale-but-not-yet-refreshed baseline is always <= the fault count at the
 * moment the transaction actually starts. It can never HIDE a fault that
 * occurs after the transaction starts, which is the failure mode this baseline
 * exists to catch.
 */
static nora_uart_rx_status_t s_biquad_csv_rx_status_base;

/*
 * Companion to the struct above: the combined integrity-fault count (ring
 * overflow / RX FIFO overflow / framing / parity -- see
 * nora_uart_rx_integrity_fault_count()), tracked the same way. This is what the
 * transport watchdog polls every pass; a single 32-bit delta is cheap enough to
 * check unconditionally, where re-checking the whole status struct every pass
 * would not be.
 */
static uint32_t s_biquad_csv_integrity_base;

static struct {
    uint32_t raw_bytes;          /* bytes captured in the current transaction   */
    uint32_t raw_peak_bytes;     /* high-water mark over all transactions       */
    uint32_t raw_overflow_count;
    uint32_t receive_ms;         /* BEGIN -> END wall time of the last transfer */
    uint32_t parse_us;           /* END -> parse complete, last transfer        */
    uint32_t rx_timeout_count;
    uint32_t integrity_abort_count;   /* was ring_overflow_abort_count: this now
                                        * counts every RX-integrity-fault abort,
                                        * not only ring overflow -- see
                                        * nora_uart_rx_integrity_fault_count(). */
    uint32_t parse_error_count;
    uint32_t success_count;
} s_biquad_csv_diag;

#if APP_CSV_DEFER_PARSE
/*
 * Raw transaction buffer. Written by the receive path, read once by the parse
 * burst in the task; never touched by an ISR.
 */
static uint8_t  s_biquad_csv_raw[APP_CSV_RAW_BUF_BYTES];
static uint32_t s_biquad_csv_raw_len        = 0u;
static uint32_t s_biquad_csv_raw_line_start = 0u;   /* start of the line in progress */
#endif //APP_CSV_DEFER_PARSE


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

static void  local_biquad_csv_transport_watchdog( void );
static void  local_biquad_csv_track_idle_baseline( void );

#if APP_CSV_DEFER_PARSE
static void  local_biquad_csv_frame_end( void );
static bool  local_biquad_csv_feed_char_raw( uint8_t c );
static void  local_biquad_csv_parse_raw_burst( void );
#endif //APP_CSV_DEFER_PARSE




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
    /* One increment per byte; the task turns a change here into a refreshed
     * deadline. Counted for the drain path too: a drain that never sees END must
     * also time out rather than hold the input-source lock forever. */
    s_biquad_csv_rx_progress++;

    if( s_biquad_csv_draining )
    {
        return local_biquad_csv_drain_char( c );
    }

    if( !s_biquad_csv_receiving )
    {
        return false;
    }

#if APP_CSV_DEFER_PARSE
    if( (s_biquad_csv_rx.state == APPDBG_BIQUAD_CSV_RX_RAW) ||
        (s_biquad_csv_rx.state == APPDBG_BIQUAD_CSV_RX_PARSE_PENDING) )
    {
        return local_biquad_csv_feed_char_raw( c );
    }
#endif //APP_CSV_DEFER_PARSE

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
    /*
     * Must run before anything below can flip s_biquad_csv_receiving to true
     * this pass (a fresh BEGIN's start_from_header() call happened synchronously
     * during the RX drain, earlier in this same app_uart_process() invocation --
     * see the comment on s_biquad_csv_rx_status_base for why this ordering is
     * what makes the baseline at most one pass stale rather than "however long
     * BEGIN recognition took under load").
     */
    local_biquad_csv_track_idle_baseline();

#if APP_CSV_DEFER_PARSE
    /*
     * Deferred parse burst. Deliberately here and not in the receive path: the
     * receive path runs inside the UART drain loop, and the whole point of the
     * split is that no CSV semantics execute while bytes are still arriving.
     */
    if( s_biquad_csv_receiving &&
        (s_biquad_csv_rx.state == APPDBG_BIQUAD_CSV_RX_PARSE_PENDING) )
    {
        local_biquad_csv_parse_raw_burst();
    }
#endif //APP_CSV_DEFER_PARSE

    local_biquad_csv_transport_watchdog();

    switch( s_biquad_coeff_apply_state )
    {
    case APPDBG_BIQUAD_COEFF_APPLY_IDLE:
        if( s_biquad_coeff_pending.request )
        {
            /* Safety net: CSV RX normally requests bypass at BEGIN header. */
            app_biquad_cascade_4ch_request_bypass();
            s_biquad_coeff_apply_wait_start_ms = GetTicks();
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_WAIT_BYPASS_ACTIVE;
        }
        break;

    case APPDBG_BIQUAD_COEFF_APPLY_WAIT_BYPASS_ACTIVE:
        if( app_biquad_cascade_4ch_is_bypass_active() )
        {
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_COPY;
        }
        else if( !nora_spi_i2s_tdm_is_running() )
        {
            /*
             * The transport is authoritatively stopped, so no ISR can be
             * reading the coefficient table.  This is the stopped-transport
             * equivalent of the ISR's BYPASS_ACTIVE acknowledgement.
             */
            printf("BIQUAD COEFF CSV: transport stopped -- applying safely.\n");
            app_biquad_cascade_4ch_confirm_bypass_stopped();
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_COPY;
        }
        else if( (uint32_t)(GetTicks() - s_biquad_coeff_apply_wait_start_ms) >=
                 APPDBG_BIQUAD_CSV_BYPASS_ACK_TIMEOUT_MS )
        {
            /*
             * The transport says it is still running, so copying would be
             * unsafe.  Abort the pending apply and restore normal processing
             * rather than preserving a perpetual WAIT_BYPASS_ACTIVE state.
             */
            printf("BIQUAD COEFF CSV APPLY ABORT: bypass acknowledgement timeout while transport runs.\n");
            s_biquad_coeff_pending.request = false;
            app_biquad_cascade_4ch_request_normal();
            s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;
        }
        break;

    case APPDBG_BIQUAD_COEFF_APPLY_COPY:
    {
        /*
         * The load can still fail here: local_biquad_csv_commit_request() only
         * checks row count and s_biquad_coeff_pending's shape fields, not every
         * coefficient value. isfinite() rejection (NaN/Inf -- strtod() accepts
         * both as valid float text, so the CSV parser cannot catch this) lives
         * one layer down, in app_biquad_cascade_4ch_load_coeff_from_uart_csv().
         * Its bool result used to be discarded here, so a rejected load still
         * printed APPLY OK. The loader validates every value before writing any
         * of them, so on false the active table is provably untouched -- no
         * corrective action is needed beyond restoring normal audio and saying
         * so honestly.
         */
        bool ok = app_biquad_coeff_csv_copy_to_active( s_biquad_csv_rx.staging,
                                                       s_biquad_coeff_pending.stage_num,
                                                       s_biquad_coeff_pending.coeff_num,
                                                       s_biquad_coeff_pending.ch_num );
        if( ok )
        {
            app_biquad_coeff_csv_clear_iir_state();
        }
        s_biquad_coeff_pending.request = false;

        /* Always restore normal audio processing, load or no load -- bypass is
         * not a state this module may leave the DSP path parked in. */
        app_biquad_cascade_4ch_request_normal();

        if( ok )
        {
            printf("BIQUAD COEFF CSV APPLY OK\n");
        }
        else
        {
            printf("BIQUAD COEFF CSV APPLY FAILED: loader rejected the data. active table unchanged.\n");
        }
        s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;
        break;
    }

    default:
        s_biquad_coeff_pending.request = false;
        app_biquad_cascade_4ch_request_normal();
        s_biquad_coeff_apply_state = APPDBG_BIQUAD_COEFF_APPLY_IDLE;
        break;
    }
}


__attribute__((weak)) bool app_biquad_coeff_csv_copy_to_active(
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

    return app_biquad_cascade_4ch_load_coeff_from_uart_csv( &coeff[0][0][0],
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
    /*
     * Biquad CSV bulk transfer is a UART1-only transport feature (UART2 is
     * interactive console only and is rejected at CSV BEGIN in app_debug), so
     * this module intentionally operates on UART1 directly.
     *
     * Deliberately NO snapshot here. s_biquad_csv_rx_status_base and
     * s_biquad_csv_integrity_base are already current -- see their declarations
     * -- because local_biquad_csv_track_idle_baseline() keeps them refreshed
     * every idle pass, and this function runs synchronously off the header
     * line's trailing byte within the SAME app_uart_process() pass whose task()
     * call will find s_biquad_csv_receiving already true and skip that refresh.
     * Taking a fresh snapshot here instead of trusting that one would reintroduce
     * exactly the blind spot both baselines exist to close: this function itself
     * cannot run any earlier than "the main loop noticed the trailing newline",
     * and under load that recognition lags the wire, so a snapshot taken HERE
     * bakes in whatever loss happened during that lag as if it pre-dated the
     * transfer.
     */
    s_biquad_csv_rx.state         = APPDBG_BIQUAD_CSV_RX_DATA;
    s_biquad_csv_rx.stage_num     = stage_num;
    s_biquad_csv_rx.ch_num        = ch_num;
    s_biquad_csv_rx.coeff_num     = coeff_num;
    s_biquad_csv_rx.expected_rows = (uint16_t)(stage_num * coeff_num);

    s_biquad_csv_rx_start_ms      = GetTicks();
    s_biquad_csv_last_rx_ms       = s_biquad_csv_rx_start_ms;
    s_biquad_csv_diag.raw_bytes   = 0u;
    s_biquad_csv_diag.receive_ms  = 0u;
    s_biquad_csv_diag.parse_us    = 0u;

#if APP_CSV_DEFER_PARSE
    s_biquad_csv_raw_len          = 0u;
    s_biquad_csv_raw_line_start   = 0u;
    s_biquad_csv_rx.state         = APPDBG_BIQUAD_CSV_RX_RAW;
#endif //APP_CSV_DEFER_PARSE

    s_biquad_csv_receiving = true;

    printf("\nBIQUAD COEFF CSV RX START: stage=%u coeff=%u ch=%u rows=%u%s\n",
           (unsigned)stage_num,
           (unsigned)coeff_num,
           (unsigned)ch_num,
           (unsigned)s_biquad_csv_rx.expected_rows,
#if APP_CSV_DEFER_PARSE
           " (deferred parse)"
#else
           ""
#endif
           );

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
        s_biquad_csv_diag.parse_error_count++;
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



#if APP_CSV_DEFER_PARSE
/* END has been framed: stop interpreting, hand the parse to the task. */
static void local_biquad_csv_frame_end( void )
{
    s_biquad_csv_diag.raw_bytes  = s_biquad_csv_raw_len;
    /* Once per transaction, so a tick read is free here -- unlike per byte. */
    s_biquad_csv_diag.receive_ms = (uint32_t)(GetTicks() - s_biquad_csv_rx_start_ms);
    s_biquad_csv_rx.state        = APPDBG_BIQUAD_CSV_RX_PARSE_PENDING;
}


/*
 * Deferred-parse receive path: transport and framing only.
 *
 * Everything this function does per byte is a store plus at most one compare.
 * No strtod, no row validation, no staging write, no per-line print. That is
 * the whole point: at 98% DSP load the main loop gets ~1.65% of the block
 * period, which is about 0.7 us per byte at 230400 baud, and the legacy path's
 * per-line parse does not fit in it.
 */
static bool local_biquad_csv_feed_char_raw( uint8_t c )
{
    static const uint32_t end_len = (uint32_t)(sizeof(APPDBG_BIQUAD_CSV_END_MARKER) - 1u);

    if( c == APPDBG_UART_ESC_CHAR )
    {
        local_biquad_csv_abort( "ESC" );
        return false;
    }

    if( s_biquad_csv_rx.state == APPDBG_BIQUAD_CSV_RX_PARSE_PENDING )
    {
        /*
         * END is framed and the parse is owed to the task. Trailing bytes belong
         * to no transaction; swallow them here rather than let them reach the
         * console command parser. Normal transfers send nothing after END.
         */
        return true;
    }

    if( c == '\r' )
    {
        return true;   /* CRLF host: the LF frames the line; CR is not stored */
    }

    /*
     * One byte is always kept in reserve so the parse burst can NUL-terminate a
     * final line that arrived without a newline.
     */
    if( s_biquad_csv_raw_len >= (uint32_t)(APP_CSV_RAW_BUF_BYTES - 1u) )
    {
        s_biquad_csv_diag.raw_overflow_count++;
        printf("\nBIQUAD COEFF CSV raw buffer full at %lu bytes (limit %lu).\n",
               (unsigned long)s_biquad_csv_raw_len,
               (unsigned long)(APP_CSV_RAW_BUF_BYTES - 1u));
        local_biquad_csv_abort_and_drain( "raw buffer full" );
        return s_biquad_csv_receiving;
    }

    s_biquad_csv_raw[s_biquad_csv_raw_len] = c;
    s_biquad_csv_raw_len++;

    if( s_biquad_csv_raw_len > s_biquad_csv_diag.raw_peak_bytes )
    {
        s_biquad_csv_diag.raw_peak_bytes = s_biquad_csv_raw_len;
    }

    /*
     * END framing, two ways -- the second because some transfer tools omit the
     * final newline, which used to leave an END at EOF buffered forever:
     *   - at a '\n', look back at the line it just terminated;
     *   - at the marker's own last character, so EOF needs no newline.
     * Both are guarded by a single-character test so the memcmp stays off the
     * ~98% of bytes that cannot possibly complete the marker.
     */
    if( c == '\n' )
    {
        const uint32_t len = (s_biquad_csv_raw_len - 1u) - s_biquad_csv_raw_line_start;

        if( (len >= end_len) &&
            (s_biquad_csv_raw[s_biquad_csv_raw_line_start] == (uint8_t)APPDBG_UART_MARKER_CHAR) &&
            (memcmp( &s_biquad_csv_raw[s_biquad_csv_raw_line_start],
                     APPDBG_BIQUAD_CSV_END_MARKER, (size_t)end_len ) == 0) )
        {
            local_biquad_csv_frame_end();
            return true;
        }

        s_biquad_csv_raw_line_start = s_biquad_csv_raw_len;
    }
    else if( c == (uint8_t)'D' )
    {
        const uint32_t len = s_biquad_csv_raw_len - s_biquad_csv_raw_line_start;

        if( (len == end_len) &&
            (memcmp( &s_biquad_csv_raw[s_biquad_csv_raw_line_start],
                     APPDBG_BIQUAD_CSV_END_MARKER, (size_t)end_len ) == 0) )
        {
            local_biquad_csv_frame_end();
        }
    }

    return true;
}


/*
 * Parse the whole captured transaction in one pass, after the transfer.
 *
 * The existing line parser is re-used verbatim: each line is NUL-terminated in
 * place and handed to local_biquad_csv_process_line(). "Same CSV -> same active
 * configuration" is therefore a property of one code path, not of two that have
 * to be kept in step -- which is also why this is the cheapest possible change
 * to the semantic side.
 */
static void local_biquad_csv_parse_raw_burst( void )
{
    const uint32_t t0_count   = nora_high_res_timer_get_count();
    uint32_t       line_start = 0u;
    uint32_t       i;

    s_biquad_csv_rx.state = APPDBG_BIQUAD_CSV_RX_DATA;

    for( i = 0u; i < s_biquad_csv_raw_len; i++ )
    {
        if( s_biquad_csv_raw[i] != (uint8_t)'\n' )
        {
            continue;
        }

        s_biquad_csv_raw[i] = 0u;   /* the buffer is discarded after this pass */
        local_biquad_csv_process_line( (const char*)&s_biquad_csv_raw[line_start] );
        line_start = i + 1u;

        if( !s_biquad_csv_receiving )
        {
            break;   /* END committed, or the line parser aborted */
        }
    }

    /* END without a trailing newline: the tail is a line the loop never saw. */
    if( s_biquad_csv_receiving && (line_start < s_biquad_csv_raw_len) )
    {
        s_biquad_csv_raw[s_biquad_csv_raw_len] = 0u;   /* reserved byte */
        local_biquad_csv_process_line( (const char*)&s_biquad_csv_raw[line_start] );
    }

    s_biquad_csv_diag.parse_us = nora_high_res_timer_elapsed_us( t0_count );

    if( s_biquad_csv_receiving )
    {
        /* Neither committed nor aborted: END framed but the body was short. */
        s_biquad_csv_diag.parse_error_count++;
        local_biquad_csv_abort( "END framed but transaction incomplete" );
    }

    printf("BIQUAD CSV DEFER DIAG: raw=%lu/%lu peak=%lu rx=%lu ms parse=%lu us "
           "ok=%lu perr=%lu ovf=%lu tmo=%lu intfault=%lu\n",
           (unsigned long)s_biquad_csv_diag.raw_bytes,
           (unsigned long)(APP_CSV_RAW_BUF_BYTES - 1u),
           (unsigned long)s_biquad_csv_diag.raw_peak_bytes,
           (unsigned long)s_biquad_csv_diag.receive_ms,
           (unsigned long)s_biquad_csv_diag.parse_us,
           (unsigned long)s_biquad_csv_diag.success_count,
           (unsigned long)s_biquad_csv_diag.parse_error_count,
           (unsigned long)s_biquad_csv_diag.raw_overflow_count,
           (unsigned long)s_biquad_csv_diag.rx_timeout_count,
           (unsigned long)s_biquad_csv_diag.integrity_abort_count);

    s_biquad_csv_raw_len        = 0u;
    s_biquad_csv_raw_line_start = 0u;
}
#endif //APP_CSV_DEFER_PARSE


/*
 * Refresh the RX-integrity baselines while the module is idle. See the comments
 * on s_biquad_csv_rx_status_base and s_biquad_csv_integrity_base for why this is
 * a continuous refresh and not a one-shot snapshot at BEGIN.
 */
static void local_biquad_csv_track_idle_baseline( void )
{
    if( s_biquad_csv_receiving || s_biquad_csv_draining )
    {
        return;
    }

    if( nora_uart_rx_status_get( UART_PLATFORM_UART1_USB_SERIAL_PORT_INST,
                                 &s_biquad_csv_rx_status_base ) != NORA_UART_OK )
    {
        memset( &s_biquad_csv_rx_status_base, 0, sizeof(s_biquad_csv_rx_status_base) );
    }
    s_biquad_csv_integrity_base =
        nora_uart_rx_integrity_fault_count( UART_PLATFORM_UART1_USB_SERIAL_PORT_INST );
}


/*
 * Transport-level watchdog, run once per main-loop pass while a CSV transaction
 * is open. Two failure modes that used to be silent hangs end here instead.
 */
static void local_biquad_csv_transport_watchdog( void )
{
    if( !s_biquad_csv_receiving && !s_biquad_csv_draining )
    {
        return;
    }

    /* Change detector, not a clock: any byte consumed since the last pass means
     * the transfer is alive, so the deadline restarts from now. */
    if( s_biquad_csv_rx_progress != s_biquad_csv_rx_progress_seen )
    {
        s_biquad_csv_rx_progress_seen = s_biquad_csv_rx_progress;
        s_biquad_csv_last_rx_ms       = GetTicks();
    }

    if( s_biquad_csv_receiving )
    {
        /*
         * Integrity fault (ring overflow / RX FIFO overflow / framing / parity)
         * is fatal to the transaction: a dropped byte means the expected row
         * count can never be reached and END may itself have been eaten, and a
         * framing/parity byte means some already-accepted row may hold a value
         * the UART never actually delivered -- see
         * nora_uart_rx_integrity_fault_count(). The baseline this compares
         * against is refreshed continuously up to the last idle pass before
         * this transaction started (see local_biquad_csv_track_idle_baseline()),
         * not taken at BEGIN, so loss occurring while the header line was still
         * being recognized is caught too, not baked into the baseline as if it
         * pre-dated the transfer.
         *
         * One 32-bit load: cheap enough to poll unconditionally every pass,
         * where re-checking the whole status struct every pass would not be.
         */
        const uint32_t faults =
            nora_uart_rx_integrity_fault_count( UART_PLATFORM_UART1_USB_SERIAL_PORT_INST ) -
            s_biquad_csv_integrity_base;

        if( faults != 0u )
        {
            s_biquad_csv_diag.integrity_abort_count++;
            printf("\nBIQUAD COEFF CSV RX integrity fault: %lu event(s) since before this transfer -- transfer cannot be trusted.\n",
                   (unsigned long)faults);
            /* Drain rather than abort outright: the host is still streaming, and
             * those bytes must not reach the console command parser. The drain
             * has the same inactivity deadline, so it cannot hang either. */
            local_biquad_csv_abort_and_drain( "rx integrity fault" );
            return;
        }
    }

    if( (uint32_t)(GetTicks() - s_biquad_csv_last_rx_ms) >= APPDBG_BIQUAD_CSV_RX_TIMEOUT_MS )
    {
        s_biquad_csv_diag.rx_timeout_count++;

        if( s_biquad_csv_draining )
        {
            /* Ordinary end of a post-abort drain: the sender ran out of file.
             * Named separately because a drain deadline follows almost every
             * rejected transfer, and reading it as a second failure sends the
             * next reader looking for a second bug. */
            printf("BIQUAD COEFF CSV drain complete (line quiet %u ms).\n",
                   (unsigned)APPDBG_BIQUAD_CSV_RX_TIMEOUT_MS);
            local_biquad_csv_clear_rx_control();
            s_biquad_csv_draining = false;
        }
        else
        {
            printf("\nBIQUAD COEFF CSV inactive for %u ms.\n",
                   (unsigned)APPDBG_BIQUAD_CSV_RX_TIMEOUT_MS);
            /* The line is quiet by definition, so there is nothing left to drain. */
            local_biquad_csv_abort( "rx timeout" );
        }
    }
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
#if APP_CSV_DEFER_PARSE
    /* Release the capture. s_biquad_csv_diag.raw_bytes keeps its size -- it is
     * recorded when END is framed, so it survives into the reports below. */
    s_biquad_csv_raw_len          = 0u;
    s_biquad_csv_raw_line_start   = 0u;
#endif //APP_CSV_DEFER_PARSE

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

    /* Deltas against the BEGIN baseline: these numbers describe THIS transfer,
     * without a counter clear that would have destroyed what came before it. */
    hal_status.rx_isr_count           -= s_biquad_csv_rx_status_base.rx_isr_count;
    hal_status.rx_byte_count          -= s_biquad_csv_rx_status_base.rx_byte_count;
    hal_status.rx_fifo_overflow_count -= s_biquad_csv_rx_status_base.rx_fifo_overflow_count;
    hal_status.framing_error_count    -= s_biquad_csv_rx_status_base.framing_error_count;
    hal_status.parity_error_count     -= s_biquad_csv_rx_status_base.parity_error_count;
    hal_status.rx_ring_overflow_count -= s_biquad_csv_rx_status_base.rx_ring_overflow_count;

    printf("BIQUAD CSV UART DIAG %s: raw=%lu row=%u/%u idx=%u state=%u isr=%lu rx=%lu fifo_ovf=%lu ferr=%lu perr=%lu ring_ovf=%lu max_drain=%u\n",
           (tag != NULL) ? tag : "",
#if APP_CSV_DEFER_PARSE
           (unsigned long)s_biquad_csv_raw_len,
#else
           0uL,
#endif
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

    /*
     * Raw UART1 hardware state, because the counters above cannot describe the
     * one failure that matters here: a reception that has stopped. Every counter
     * lives in the RX ISR, so "the ISR is not running" reads as all-zeroes and
     * looks exactly like "nothing was sent". ie/if/stat say which.
     *   ie=0            -> something masked the RX interrupt and did not restore it
     *   ie=1 if=0 stat  -> the UART itself has stopped presenting bytes; read the
     *                      latched error bits in stat (RXFOIF and friends)
     * Read directly rather than through the HAL: this is a post-mortem of the
     * HAL's own state, so it must not go through it.
     */
    printf("BIQUAD CSV UART HW %s: ie=%u if=%u u1stat=%08lX\n",
           (tag != NULL) ? tag : "",
           (unsigned)_U1RXIE,
           (unsigned)_U1RXIF,
           (unsigned long)U1STAT);
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

    /*
     * The staging cube is handed over as-is. It used to be copied into a second
     * identical cube here; that cube is gone (see s_biquad_coeff_pending). The
     * handover is safe because request is what refuses the next BEGIN, so
     * nothing can overwrite staging until the apply has consumed it.
     */
    s_biquad_coeff_pending.request = true;
    s_biquad_csv_diag.success_count++;

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
