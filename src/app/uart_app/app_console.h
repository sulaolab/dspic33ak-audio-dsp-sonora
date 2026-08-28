#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H




// ---------------- Config (tie Data[] capacity to line length) --------------
// One ASCII line: kind(1) + module(1) + name(1) + HEX... + '\n' -> 3 + 2*N + 1
// If APP_CONSOLE_MAX_DATA_BYTES is not given, derive it from APP_CONSOLE_MAX_CMD_LINE.
//#define APP_CONSOLE_MAX_CMD_LINE   (384u)
#define APP_CONSOLE_MAX_CMD_LINE   (256u)
#define APP_CONSOLE_MAX_DATA_BYTES (((APP_CONSOLE_MAX_CMD_LINE) > 4u) ? (((APP_CONSOLE_MAX_CMD_LINE) - 4u) / 2u) : 0u)

// Build-time guard: if both are user-defined, ensure consistency
#if defined(APP_CONSOLE_MAX_CMD_LINE) && defined(APP_CONSOLE_MAX_DATA_BYTES)
#if (APP_CONSOLE_MAX_CMD_LINE < (3u + 2u*(APP_CONSOLE_MAX_DATA_BYTES) + 1u))
#warning "APP_CONSOLE_MAX_CMD_LINE is smaller than required for APP_CONSOLE_MAX_DATA_BYTES; payload will be truncated by input line capacity."
#endif //(APP_CONSOLE_MAX_CMD_LINE < (3u + 2u*(APP_CONSOLE_MAX_DATA_BYTES) + 1u))
#endif //defined(APP_CONSOLE_MAX_CMD_LINE) && defined(APP_CONSOLE_MAX_DATA_BYTES)


// ---------------- Status byte (bit7 = OK flag) ----------------
// Bit7 set -> success. Bit7 clear -> error. Lower 7 bits carry reason code.
typedef uint8_t app_console_status_t;
#define APP_CONSOLE_OK_MASK            (0x80u)
#define APP_CONSOLE_OK                 (0x80u)   // generic success
#define APP_CONSOLE_ERR_NONE           (0x00u)   // not used for final status; here for completeness
#define APP_CONSOLE_ERR_NOT_FOUND      (0x01u)
#define APP_CONSOLE_ERR_BAD_DATA       (0x02u)
#define APP_CONSOLE_ERR_UNSUPPORTED    (0x03u)
#define APP_CONSOLE_ERR_BAD_PARM_LEN   (0x04u)
#define APP_CONSOLE_ERR_OPERATION_FAILED (0x05u)
#define APP_CONSOLE_IS_OK(s)           (((s) & APP_CONSOLE_OK_MASK) != 0u)


// ---------------- Message (renamed from CMD_HDR_TYPE) ----------------
// Raw ASCII line: kind(1) + module(1) + name(1) + HEX... + '\n'
// data[] holds decoded binary payload; raw_len is original ASCII length.
typedef struct app_console_msg_s {
    uint8_t      kind;       // '*' set / '?' query / '!' async
    uint8_t      module;     // 'g','i','n','m', ...
    uint8_t      name;       // sub-letter
    app_console_status_t status;     // bit7=OK, else reason in low bits
    uint16_t     raw_len;    // ASCII length including '\n'
    uint8_t      data[APP_CONSOLE_MAX_DATA_BYTES];  // decoded payload
    uint16_t     data_len;   // number of valid bytes in data[]
} app_console_msg_t;


// ---------------- Core API ----------------
void   app_console_init(void);
// feed exactly one received character; returns true if a full line was consumed
bool   app_console_feed_char(uint8_t ch);
// true when no command line is being built (line buffer empty). The RX dispatcher uses this to
// suppress single-key hotkeys while a '*'/'?' command line is in progress (see app_debug.c).
bool   app_console_is_idle(void);
// true when no partial command line is in progress (safe to switch RX source)
bool   app_console_is_line_idle(void);

// ---------------- App hook prototypes (implement these in your app) --------
void   app_g_onmsg(app_console_msg_t* msg);
void   app_i_onmsg(app_console_msg_t* msg);
void   app_n_onmsg(app_console_msg_t* msg);
void   app_m_onmsg(app_console_msg_t* msg);


#endif // APP_CONSOLE_H


