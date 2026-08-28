#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


#include "app_console.h"


// One statement per line; verbose comments for clarity.

#define LF_CHAR                ('\n')
#define CR_CHAR                ('\r')
#define BS_CHAR                (0x08u)
#define DEL_CHAR               (0x7Fu)
#define MIN_LINE_ASCII_LEN     (4u)      // 3 header + '\n' (zero-payload query, e.g. "?gv\n")


// RX line buffer (ASCII) and index. `discarding` marks an over-long line whose
// remainder is being dropped up to (and including) its newline -- see
// app_console_feed_char().
static struct {
    uint8_t  buf[APP_CONSOLE_MAX_CMD_LINE];
    uint16_t idx;
    bool     discarding;
} s_rx;

// ---- weak default for TX hook (app may override) ----
// __attribute__((weak))
// void app_console_on_response(const char* line, uint16_t len)
// {
//     (void)line;
//     (void)len;
// }

// ---- forward declarations ----
static inline void    line_reset(void);
static inline bool    is_bare_query(void);
static void           print_bare_query_help(void);
static inline bool    is_backspace(uint8_t c);
static inline void    line_backspace(void);
static inline bool    is_hex(uint8_t c);
static inline uint8_t hex_val(uint8_t c);
static bool           hex_pair_to_byte(uint8_t hi, uint8_t lo, uint8_t* out);

// Modern serializer (renamed from legacy local_PrintResponse)
// '=' + module + name + HEX(status) + HEX(payload) + '\n'
static uint16_t      app_console_print_response(const app_console_msg_t* msg,
                                        char* resp_buf,
                                        uint16_t resp_len);

// Small hex encoders for response printing
static size_t        hex_write_byte(uint8_t v, char* out, size_t cap);
static size_t        hex_write_buf(const uint8_t* buf, size_t len, char* out, size_t cap);

// ---- app dispatch ----
static void          dispatch_to_app(app_console_msg_t* msg);


// ----------------------------------------------------------------------------
// Core init
// ----------------------------------------------------------------------------
void app_console_init(void)
{
    s_rx.idx        = 0u;
    s_rx.discarding = false;
}


// True when no partial command line is being assembled (index at start-of-line
// and not mid-way through discarding an over-long line).
// The input arbiter uses this to know when it is safe to switch the RX source
// between the two Windows ports (see app_debug.c source-lock).
bool app_console_is_line_idle(void)
{
    return (s_rx.idx == 0u) && !s_rx.discarding;
}


// ----------------------------------------------------------------------------
// Feed one received char. When '\n' is seen, parse and dispatch.
// After the app handler returns, serialize the response using app_console_print_response()
// and pass it to the optional TX hook.
// ----------------------------------------------------------------------------
bool app_console_feed_char(uint8_t ch)
{
    // Over-long line: drop the REST of it, up to and including its newline,
    // instead of resetting the index mid-line. A mid-line reset would let the
    // tail of the over-long line be parsed as a fresh command (a 3-char header
    // could land anywhere in it), so the whole line is refused instead.
    if (s_rx.idx >= APP_CONSOLE_MAX_CMD_LINE) { s_rx.discarding = true; }

    if (s_rx.discarding)
    {
        putchar(ch);
        if (ch == LF_CHAR)
        {
            s_rx.discarding = false;
            line_reset();               // clears idx and prints the prompt
        }
        return false;
    }

    if (is_backspace(ch))
    {
        line_backspace();
        return false;
    }

    putchar(ch);

    if (ch == CR_CHAR) {                               return false; }

    s_rx.buf[s_rx.idx] = ch;
    s_rx.idx = (uint16_t)(s_rx.idx + 1u);

    if (ch != LF_CHAR) {                               return false; }
    // Bare "?\n" (kind alone, no module/name) can never be a real 3-char header,
    // so it is not a gap in the grammar to special-case here: this is the one
    // input MIN_LINE_ASCII_LEN would otherwise silently discard. Checked before
    // that gate, not added to it, so every other short/malformed line still
    // falls through unchanged.
    if (is_bare_query())
    {
        print_bare_query_help();
        line_reset();
        return true;
    }
    if (s_rx.idx < MIN_LINE_ASCII_LEN) { line_reset(); return false; }

    // Build message from the ASCII line
    app_console_msg_t msg;
    msg.kind     = s_rx.buf[0];
    msg.module   = s_rx.buf[1];
    msg.name     = s_rx.buf[2];
    msg.raw_len  = s_rx.idx;
    msg.status   = APP_CONSOLE_OK;      // optimistic default
    msg.data_len = 0u;
    // Zero-payload queries (e.g. "?gv\n") carry no data bytes. Clear data[0] so a handler that
    // switches on data[0] without checking data_len does not read uninitialized stack.
    msg.data[0]  = 0u;

    // Decode ASCII-HEX payload (two chars per byte)
    {
        uint16_t p = 3u;
        while ((p + 1u) < s_rx.idx)
        {
            uint8_t b;
            if (!hex_pair_to_byte(s_rx.buf[p], s_rx.buf[(uint16_t)(p + 1u)], &b))
            {
                msg.status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            if (msg.data_len >= sizeof(msg.data))
            {
                msg.status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            msg.data[msg.data_len] = b;
            msg.data_len = (uint16_t)(msg.data_len + 1u);
            p = (uint16_t)(p + 2u);
        }
    }

    // 1) Call application handler -- only if the line decoded cleanly. A bad hex pair or an
    // oversized payload already set msg.status to an error above; skip dispatch so no handler
    // ever sees malformed input or gets a chance to overwrite the parser's error with a false OK.
    if (msg.status == APP_CONSOLE_OK)
    {
        dispatch_to_app(&msg);
    }
    else
    {
        // Drop any prefix bytes decoded before the failure; the error response must not echo
        // partial input data.
        msg.data_len = 0u;
    }

    // 2) Serialize a response line and pass it to the TX hook
    char out[64];
    uint16_t n;
    n = app_console_print_response(&msg, out, (uint16_t)sizeof(out));
    // out to console
    fwrite(out, 1, n, stdout);

//    if (n > 0u)
//    {
//        app_console_on_response(out, n);
//    }


    // 3) Prepare for the next line
    line_reset();

    return true;
}


// Legacy name kept for the hotkey/console arbiter in app_debug.c. Same question,
// same answer -- alias rather than a second copy of the condition.
bool app_console_is_idle(void)
{
    return app_console_is_line_idle();
}


// ----------------------------------------------------------------------------
// Local helpers
// ----------------------------------------------------------------------------
static inline void line_reset(void)
{
    s_rx.idx = 0u;

    putchar('$');
}


// "?\n" and nothing else: kind alone, before module/name/payload exist.
static inline bool is_bare_query(void)
{
    return (s_rx.idx == 2u) && (s_rx.buf[0] == '?');
}


// Legend only -- not a walk of the dispatch table, so it lists module letters,
// not individual verbs (there is no registry to enumerate those from). Keep in
// step with app_onmsg()'s switch in app_debug.c if a module letter changes.
static void print_bare_query_help(void)
{
    printf("console: <kind><module><name>[hex payload]\n"
           "  kind: * = set, ? = query\n"
           "  common modules: g general, s system, d diagnostics, x traps,\n"
           "                  k touch, t audio transport, f resident boot\n"
           "  app module (a = ASRC, c = Classic) is build-specific\n");
}


static inline bool is_backspace(uint8_t c)
{
    return ((c == BS_CHAR) || (c == DEL_CHAR));
}


static inline void line_backspace(void)
{
    if (s_rx.idx == 0u)
    {
        return;
    }

    s_rx.idx = (uint16_t)(s_rx.idx - 1u);

    /*
     * Terminal erase sequence:
     *   '\b' : move cursor left
     *   ' '  : overwrite previous character
     *   '\b' : move cursor left again
     *
     * This erases only the command text after '$'.
     * The prompt itself is never erased because idx==0 is blocked above.
     */
    putchar('\b');
    putchar(' ');
    putchar('\b');
}


static inline bool is_hex(uint8_t c)
{
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'f') return true;
    if (c >= 'A' && c <= 'F') return true;
    return false;
}


static inline uint8_t hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    return (uint8_t)(10 + (c - 'A'));
}


static bool hex_pair_to_byte(uint8_t hi, uint8_t lo, uint8_t* out)
{
    if (!is_hex(hi)) return false;
    if (!is_hex(lo)) return false;
    if (!out) return false;
    *out = (uint8_t)((hex_val(hi) << 4) | hex_val(lo));
    return true;
}


static size_t hex_write_byte(uint8_t v, char* out, size_t cap)
{
    static const char* H = "0123456789ABCDEF";
    if (cap < 2u) return 0u;
    out[0] = H[(v >> 4) & 0x0Fu];
    out[1] = H[v & 0x0Fu];
    return 2u;
}


static size_t hex_write_buf(const uint8_t* buf, size_t len, char* out, size_t cap)
{
    size_t w = 0u;
    size_t i = 0u;
    while (i < len)
    {
        if ((cap - w) < 2u) break;
        w += hex_write_byte(buf[i], out + w, cap - w);
        i++;
    }
    return w;
}

// ----------------------------------------------------------------------------
// Modern response printer (legacy-equivalent)
// Format: '$' + HEX(status) + module + name + HEX(data[0..data_len-1]) + '\n'
// Returns number of bytes written (includes '\n', excludes trailing '\0').
// ----------------------------------------------------------------------------
static uint16_t app_console_print_response(const app_console_msg_t* msg,
                                   char* resp_buf,
                                   uint16_t resp_len)
{
    size_t w;
    size_t room;

    if (!msg)      return 0u;
    if (!resp_buf) return 0u;

    w = 0u;

    if (w < resp_len) { resp_buf[w] = '$'; w++; }

    room = (resp_len > w) ? (resp_len - w) : 0u;
    if (room >= 2u)
    {
        w += hex_write_byte((uint8_t)msg->status, resp_buf + w, room);
    }

    if (w < resp_len) { resp_buf[w] = (char)msg->module; w++; }
    if (w < resp_len) { resp_buf[w] = (char)msg->name;   w++; }

    room = (resp_len > w) ? (resp_len - w) : 0u;
    if (msg->data_len > 0u && room >= 2u)
    {
        w += hex_write_buf(msg->data, (size_t)msg->data_len, resp_buf + w, room);
    }

    if (w < resp_len) { resp_buf[w] = '\n'; w++; }
    if (w < resp_len) { resp_buf[w] = '\0'; }

    return (uint16_t)w;
}


// ----------------------------------------------------------------------------
// App dispatch (no functionality moved across files)
// ----------------------------------------------------------------------------
extern void app_onmsg(app_console_msg_t* msg);

static void dispatch_to_app(app_console_msg_t* msg)
{
    app_onmsg(msg);
}
