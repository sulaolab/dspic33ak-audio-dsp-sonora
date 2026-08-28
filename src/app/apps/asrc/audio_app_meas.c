//===========================================================
// ASRC App measurement implementation (audio_app_meas.c)
//
// ASRC quality measurement harness (see audio_app_meas.h). Bench-only: CLEAN precomputed 24-bit
// sine source for the A->B (or B->A) ASRC input + one-shot digital capture of the output + dump.
//
// The tone source is a precomputed int24 table (tools/asrc/gen_asrc_meas_tones.py, generated into
// audio_app_meas_tones.c), NOT a runtime sinf/float-phase/lrintf generator: the old runtime
// source had a ~-109 dB THD+N floor that would cap kernel measurement toward the -120/-123 dB
// targets. The table is SAMPLE-DOMAIN exact and advances once per SOURCE-domain input frame; the
// destination-domain FRC clock stays asynchronous (this removes numerical source distortion, NOT
// the ASRC clock-domain problem).
//
// Sample-domain exact means the tone frequency follows the rate it is played at (f = fs * cycles
// / len), so a band-edge test needs a row per source rate -- see audio_app_meas_set_tone_high().
//===========================================================

#include "audio_app_meas.h"
#include "audio_app_asrc.h"   // Q3: freeze base/applied step + step-delta getters for the header

#if !SONORA_APP_IS_ASRC
#  error "audio_app_meas.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#if APP_ASRC_MEAS

// R11 Q11 (BCLK dedicated-timer rate observer) needs TWO spare 32-bit timers with external-clock
// inputs, and the two BCLK RP numbers it routes are AK512 board facts.  AK128 has neither: the
// T2/T3 pair does not exist on that part (SCCP/MCCP instead), and its BCLK reaches the SPI blocks
// on different RPs.  Q11 is an OBSERVER -- it never feeds the applied step, and no DR/THD+N
// measurement reads it -- so it is a feature of the harness, not a fixture of it.  With the
// feature off, trace sel=4 records zeros and the isolation probe reports that it is absent.
#ifndef APP_MEAS_Q11_BCLK_OBSERVER
#  define APP_MEAS_Q11_BCLK_OBSERVER  (1)
#endif

#if APP_MEAS_Q11_BCLK_OBSERVER
#include "nora_pps.h"    // R11 Q11: PPS unlock/lock for the BCLK-observer TxCK routing
#endif

#include <stdint.h>
#include <stdio.h>
#include "audio_app_meas_tones.h"
#include "board/devices/wm8904.h"   // live leg rates for the dump header (see meas_leg_rates)

#if APP_ASRC_48K_TO_8_DECIMATOR
#include "asrc_decimator_48_to_8.h"
#include "asrc_decimator_48_to_8_meas_tone.inc"
/* /6-SPECIFIC, and deliberately not the same number as the identically-named macro in
 * asrc_audio_path.c.  This buffer only ever feeds asrc_decimator_48_to_8_* below, which emits
 * ceil(APP_BLOCK_FRAMES / 6) frames; the routing buffer over there has to cover the smallest
 * divider the runtime gate can select and is therefore larger.  Do not copy a value between
 * the two files -- size each from the divider it actually serves. */
#define DECIMATED_BLOCK_CAPACITY (((uint32_t)APP_BLOCK_FRAMES + 5u) / 6u)
#endif

#if APP_ASRC_MEAS_UART2_STREAM
#include "uart_platform_uart2_usb_serial_device.h"   // UART2_Stream* dedicated binary DATA port
static void meas_stream_produce( const int32_t* out_block );   // producer, runs in the SPI2 ISR
#endif


//===========================================================
// Variables
//===========================================================
// What the bench ASKED for, kept separately from the row it resolves to. "High" is a request for
// 18 kHz REAL, and which table row delivers that depends on the source-leg rate at the moment the
// tone is used -- so the request is stored and re-resolved in meas_reselect(), not baked in here.
#define MEAS_TONE_REQ_PINNED     (0u)   // an explicit row (*at 00 / *at 04): never re-resolved
#define MEAS_TONE_REQ_HIGH_AUTO  (1u)   // *at 01: pick the HF row matching the live source rate
static uint8_t  s_tone_req  = MEAS_TONE_REQ_PINNED;
// Resolved tone ROW (MEAS_TONE_ROW_* in audio_app_meas_tones.h): 0 = LOW, 1 = HIGH_48, 2 = HIGH_96.
static uint8_t  s_tone_row  = MEAS_TONE_ROW_LOW;
static uint8_t  s_level_idx = 0u;                 // *nt2A index: 0=-1,1=-60,2=-20,3=-40,4=-80,5=-6 dBFS
// Q3: perturbation-state snapshot taken at capture-complete (see audio_app_meas_capture).
static uint32_t s_snap_base_bits = 0u, s_snap_applied_bits = 0u;
static int      s_snap_mode = 0;
static int32_t  s_snap_req  = 0;
static float    s_snap_ppm  = 0.0f;
static uint32_t s_snap_fill = 0u;
static uint32_t s_snap_age_pulls = 0u;                       // R16: freeze-age of the captured window
static float    s_snap_ff_live = 0.0f, s_snap_ff_frozen = 0.0f;

static const meas_tone_table_t* s_tbl = 0;        // active clean-table (source-domain sequence)
static uint16_t s_tbl_idx   = 0u;                 // advances once per SOURCE-domain input sample

static int32_t  s_cap[APP_MEAS_CAP_LEN];          // captured output L (24-bit signed)
static uint32_t s_cap_idx    = 0u;
static uint32_t s_discard    = 0u;                // output frames still to skip (start transient)
static uint8_t  s_capturing  = 0u;
static uint8_t  s_ready      = 0u;

#if APP_ASRC_48K_TO_8_DECIMATOR
static const uint16_t s_decim_tone_hz[9] =
    { 1000u, 3000u, 3500u, 3900u, 4100u, 5000u, 8000u, 12000u, 18000u };
static const uint8_t s_decim_tone_step[9] =
    { 10u, 30u, 35u, 39u, 41u, 50u, 80u, 120u, 180u };
static asrc_decimator_48_to_8_t s_decimator;
static uint16_t s_decim_tone_phase = 0u;
static uint8_t s_decim_tone_idx = 0u;
static uint8_t s_decimator_initialized = 0u;
#endif

#if APP_MEAS_CTRL_TRACE
// R10 Q10: MEASUREMENT-ONLY control-variable trace. Reuses s_cap[] (never runs at the same time as
// the audio capture -- different commands). Records ONE control variable per A->B pull (block rate
// ~=1356 Hz, decimatable) so the time-domain nature of the Mode-S close-in modulation can be seen
// AT ITS SOURCE (the applied step / corr_lpf) rather than inferred from a short audio FFT. It only
// READS control state -- it does NOT alter the loop. sel: 0=applied step, 1=corr_lpf, 2=fill.
static uint8_t  s_trace_active = 0u;
static uint8_t  s_trace_ready  = 0u;
static uint8_t  s_trace_sel    = 0u;
static uint16_t s_trace_decim  = 1u;              // store every Nth pull (>=1)
static uint16_t s_trace_dc     = 0u;              // decimation counter
static uint32_t s_trace_idx    = 0u;
static uint32_t s_trace_start_epoch = 0u;         // R16: pull-counter latched at trace arm (age origin)

// Q34: fractional-wrap causality trace (sel=11). Dedicated packed buffer (NOT aliased on s_cap) so the
// per-block record {fill, frac, corr_lpf, applied_step} spans enough time to hold >=20 cycles of the
// ~10 Hz applied_step wobble AND resolve the ~145 Hz fill transition. Packing: word0 = (fill_u16<<16)|
// frac_u16 (frac_u16 = frac*65536, phase in 1/65536-sample units); word1 = corr_lpf float bits; word2 =
// applied_step float bits. raw_corr is exactly KP*(fill-TARGET) -> host-derivable, not stored. At
// decim=3 the effective rate is ~452 Hz (Nyquist 226 > 145) and Q34_REC_CAP records span ~2.26 s
// (~22 cycles of 10 Hz). Measurement-only: written from trace_tick, never fed back to the loop.
#if APP_ASRC_48K_TO_8_DECIMATOR || (APP_ASRC_MEAS && (APP_MEAS_DIR == MEAS_DIR_BA)) || APP_ASRC_RUNTIME_48K_TO_8
// Presets that combine a decimator instance with MEAS run out of RAM for the link-time stack
// (2048 B + 64 B guard) at the full trace depth.  DECIMATOR_MEAS was the first: its extra
// ~1.5 KB asrc_decimator_48_to_8_t instance (s_decimator, below) pushed total data usage from
// 95% (CODEC_MEAS) to 99%.
//
// The 96 kHz B->A MEAS preset joined on 2026-08-02 for the same reason and with a nastier
// symptom.  It needs the B->A resampler instance AND the runtime front end, landing at 95%
// data.  That links, but asrc_decimator_selftest() then allocates ~5 KB of stack in one frame
// (asrc_decimator_48_to_8_t `opt` alone is ~3 KB: 172 + 588 floats of mirrored history), which
// overflows into the last free bytes.  A stack overflow cannot report itself, so the board
// simply STOPS mid-boot -- observed hanging right after "ASRC fused pair-slot selftest: pass",
// with the decimator selftest line never printed and no telemetry.
//
// Halve this diagnostic-only trace depth for those combinations to free 8 KB of headroom;
// every other MEAS preset keeps the full 1024-record depth.  The trace buffer is written only
// by the *ag trace path and is never touched by the *ac/?ac capture used for THD+N and DR, so
// shrinking it cannot affect a measurement.
//
// The runtime Nyquist front end (APP_ASRC_RUNTIME_48K_TO_8) joined the list on 2026-08-21 for
// exactly the first reason: a MEAS image that measures the shipping front end carries the Q31
// coefficient workspace AND the 190-tap x ASRC_CH history on top of a full MEAS build, and at the
// 1024-record depth the link fails on the stack, not on a section ("Not enough memory for stack").
// This 8 KB is what buys the DR/THD+N instrument image its FULL capture depth (APP_MEAS_CAP_LEN
// 2048), which is the one thing the measurement cannot trade away -- the reference numbers in
// [internal] asrc_meas_test_operations.md 5.1.1 were taken at n=2048, so a shorter capture is not comparable.
// What that image DOES trade is width (ASRC_CH=8, still a whole STREAM8 group, so the shipping
// polyphase kernel and coefficients are unchanged); channel count moves load, not quality, and the
// 16-channel load was signed off separately on the shipping BIDIR image.
#define Q34_REC_CAP     (512u)                    // records; 4 int32 each -> 8 KB dedicated .bss
#else
#define Q34_REC_CAP     (1024u)                   // records; 4 int32 each -> 16 KB dedicated .bss
#endif
#define Q34_WORDS       (4u)
static int32_t  s_q34_cap[Q34_REC_CAP * Q34_WORDS];
static uint32_t s_q34_idx   = 0u;                 // records written this run
static uint16_t s_q34_clamp = 0u;                 // saturating count of clamp_hit during the trace
static uint16_t s_q34_slew  = 0u;                 // saturating count of slew_hit during the trace
static uint32_t s_q34_wr_acc   = 0u;              // Q35: producer wr_adv summed over the decim window
static uint32_t s_q34_wrap_acc = 0u;              // Q35: consumer wraps summed over the decim window
#endif // APP_MEAS_CTRL_TRACE

#if APP_ASRC_MEAS_UART2_STREAM
//===========================================================
// Q19 base: long-coherent binary stream of the REAL A->B ASRC output over UART2 -> PKOB4.
//
// The producer (meas_stream_produce, SPI2 ISR) turns each completed A->B output block into one
// 100-byte binary frame published to a bounded SPSC ring that ALIASES the 8 KB s_cap[] buffer
// (RAM-neutral: the one-shot audio capture and the stream are different commands, never live at
// once). The consumer (audio_app_meas_stream_service, main loop) drains the ring to the
// dedicated UART2 DATA port and prints *STREAM_ARM/BEGIN/END on UART1. Frame geometry matches
// the historical Q18 streamer so the host verifier semantics carry over:
//   byte 0      : 0xA5 magic
//   bytes 1..3  : 24-bit little-endian absolute frame sequence
//   remaining bytes: APP_BLOCK_FRAMES x 24-bit little-endian signed audio samples
//                    (mono L = out_block[0] >> 8)
//===========================================================
#define STR_MAGIC           (0xA5u)
#define STR_SAMP_PER_FRAME  ((uint16_t)APP_BLOCK_FRAMES)          // current ASRC build: 16
#define STR_FRAME_BYTES     (4u + STR_SAMP_PER_FRAME * 3u)        // current ASRC build: 52
#define STR_RING_BYTES      ((uint32_t)APP_MEAS_CAP_LEN * 4u)     // alias s_cap[], 8192 bytes
// blk_hz = frame rate = fs_b / APP_BLOCK_FRAMES; fs_b = 390625/(BRG+1) (TDM8 256-BCLK frame).
#define STR_BLK_HZ          ( ( 390625.0 / (double)( APP_SPI2_MASTER_BRG + 1 ) ) / (double)APP_BLOCK_FRAMES )

static volatile uint32_t s_str_head   = 0u;       // producer publishes here (ISR)
static volatile uint32_t s_str_tail   = 0u;       // consumer advances here (main loop)
static volatile uint8_t  s_str_active = 0u;       // producer running
static volatile uint8_t  s_str_ovf    = 0u;       // ring overflow = hard discontinuity (latched)
static uint32_t s_str_seq        = 0u;            // next frame sequence to emit
static uint32_t s_str_target     = 0u;            // frames to produce (0 => idle)
static uint32_t s_str_bytes      = 0u;            // bytes handed to UART2
static uint32_t s_str_peak       = 0u;            // peak ring occupancy (bytes)
static uint32_t s_str_start_pull = 0u;            // ASRC pull counter latched at frame 0
static uint8_t  s_str_begin_sent = 0u;            // *STREAM_BEGIN emitted
static uint8_t  s_str_end_sent   = 0u;            // *STREAM_END emitted
static uint32_t s_str_inflight   = 0u;            // bytes of the async run currently in flight (0=idle)

// --- Q19 synchronized freeze-state telemetry sideband (rides the SAME UART2 binary stream) ------
// A fixed 32-byte 0xA6 record is interleaved every TELEM_DECIM audio frames, carrying the control
// state snapshot taken right after the ASRC pull that produced the accompanying audio frame. The
// audio 0xA5 records are unchanged; the host demuxes by magic. Low bandwidth (~2.7 kB/s) vs the
// proven ~195 kB/s PKOB4 zero-loss envelope. See [internal] asrc_q19_freeze_state_causal_map.md.
#define TELEM_MAGIC         (0xA6u)
#define TELEM_VERSION       (1u)
#define TELEM_BYTES         (32u)
#define TELEM_DECIM         (16u)     // one telemetry record per 16 audio frames (~84.8 rec/s)
static uint32_t s_str_telem = 0u;     // telemetry records published this run

// Kernel geometry for the *STREAM_BEGIN metadata line only. These live in audio_app_asrc.c; the
// Q19 eval profile overrides them via the config header (visible here too). When not overridden
// (plain stream build) they are not visible to this TU, so mirror the engine defaults so the
// BEGIN line always reports the actual kernel (config override -> here; otherwise engine default).
#ifndef ASRC_POLY_L
#define ASRC_POLY_L         (64u)
#endif
#ifndef ASRC_POLY_WINDOW
#define ASRC_POLY_WINDOW    (0)       /* 0 = Blackman (engine default), 1 = Blackman-Harris */
#endif

// CRC16-CCITT (poly 0x1021, init 0xFFFF) over the first 30 bytes of a telemetry record.
static uint16_t str_crc16_ccitt( const uint8_t* p, uint32_t n )
{
    uint16_t crc = 0xFFFFu;
    for( uint32_t i = 0u; i < n; i++ )
    {
        crc ^= (uint16_t)( (uint16_t)p[i] << 8 );
        for( uint8_t b = 0u; b < 8u; b++ )
        {
            crc = ( crc & 0x8000u ) ? (uint16_t)( ( crc << 1 ) ^ 0x1021u ) : (uint16_t)( crc << 1 );
        }
    }
    return crc;
}
#endif // APP_ASRC_MEAS_UART2_STREAM


//===========================================================
// Local Functions
//===========================================================
/* Live source/destination rates.  Used for the dump header and for resolving which HF tone row
 * suits the source leg.
 *
 * APP_MEAS_FS_A_HZ is a compile-time constant (48000) used for the sine phase step, so it is
 * NOT the leg rate once a 96 kHz preset exists -- reporting it would mislabel every 96 kHz
 * capture as 48 kHz.  The tone frequency moves for the same reason: the tables are
 * SAMPLE-domain exact (cycles per len samples, see audio_app_meas_tones.h), so a 48-sample
 * 1-cycle table is 1 kHz at 48 kHz but 2 kHz at 96 kHz.  Derive both from the live rate.
 *
 * fs_out is emitted too, which the ASRC path previously omitted -- that omission is why
 * asrc_shootout.py needs a manual --fs argument. */
/* The B-side I2C instance is BOARD-dependent (DIM-P4/P6 wires codec-B to I2C1, the plain
 * MikroBUS-B to I2C3) and MUST be resolved the same way audio_transport.c:51-56 and
 * asrc_audio_path.c:21-26 resolve it.  This file hard-coded 3u until 2026-08-20, so on a
 * J3 board it read wm8904's rate cache at the wrong index and every dump header reported
 * fs_out_hz=48000 (the cache's init default) no matter what *ar had set leg B to.  Left
 * silent it makes the analyzer use the wrong fs and meas_warn_if_aliasing() miss its case,
 * so refuse to build rather than guess if the topology macro ever stops reaching here. */
#ifndef APP_AK128_J3_TDM_B
#  error "APP_AK128_J3_TDM_B not in scope -- cannot resolve the codec-B I2C instance (see above)"
#endif
#define I2C_INST_A (2u)   // I2C2 -- WM8904-A on MikroBUS-A
#if APP_AK128_J3_TDM_B
#define I2C_INST_B (1u)   // I2C1 -- WM8904-B on MikroBUS-B, DIM-P4/P6
#else
#define I2C_INST_B (3u)   // I2C3 -- WM8904-B on MikroBUS-B
#endif
static void meas_leg_rates( uint32_t* fs_src, uint32_t* fs_dst )
{
    const uint32_t fs_a = wm8904_get_rate_hz( I2C_INST_A );
    const uint32_t fs_b = wm8904_get_rate_hz( I2C_INST_B );
#if (APP_MEAS_DIR == MEAS_DIR_BA)
    *fs_src = fs_b;   /* B-in sine -> B->A resample -> captured on A */
    *fs_dst = fs_a;
#else
    *fs_src = fs_a;   /* A-in sine -> A->B resample -> captured on B */
    *fs_dst = fs_b;
#endif
}

/* Frequency a sample-domain-exact table actually produces at a given source rate:
 * f = fs_src * cycles / len.  Integer by construction for every shipped row. */
static uint32_t meas_tone_actual_hz( const meas_tone_table_t* t, uint32_t fs_src )
{
    if( ( t == 0 ) || ( t->len == 0u ) ) { return 0u; }
    return (uint32_t)( ( (uint64_t)fs_src * t->cycles ) / t->len );
}

/* Pick the active table for (tone request, level), RESOLVING the request against the live source
 * rate every time.  No printf -- also called from the audio block ISR lazy-init path; the rate
 * lookup is a cached array read (wm8904_get_rate_hz), not I2C traffic.
 *
 * Resolving here rather than once inside set_tone_high() is what makes "high" mean 18 kHz REAL no
 * matter when the rate moved.  The earlier version latched the row at selection time, so
 *   *at01 ; *ar0108 ; *ac
 * captured 9 kHz (or aliased at 36 kHz) with nothing in the log to say so.  A PINNED request is
 * deliberately never re-resolved -- that is what *at 04 is for, including demonstrating the alias. */
static void meas_reselect( void )
{
    if( s_tone_req == MEAS_TONE_REQ_HIGH_AUTO )
    {
        uint32_t fs_src = 0u, fs_dst = 0u;
        meas_leg_rates( &fs_src, &fs_dst );
        /* 72 kHz is just the midpoint of the two rows' nominal rates, so any plausible leg rate
         * lands on the right side of it. */
        s_tone_row = ( fs_src >= 72000u ) ? MEAS_TONE_ROW_HIGH_96 : MEAS_TONE_ROW_HIGH_48;
    }
    s_tbl     = audio_app_meas_table( s_tone_row, s_level_idx );
    s_tbl_idx = 0u;
}

/* Shared by the selection print and the arm print: the tone can only be measured as THD+N if it is
 * below the OUTPUT Nyquist. Warn rather than refuse -- a pinned mismatched row is a legitimate
 * bench request (it is how the alias failure mode is demonstrated). */
static void meas_warn_if_aliasing( uint32_t actual_hz, uint32_t fs_dst )
{
    if( ( fs_dst != 0u ) && ( ( actual_hz * 2u ) >= fs_dst ) )
    {
        printf(" *MEAS WARNING: tone is at or above the OUTPUT Nyquist (%luHz) -- it will alias;"
               " this measures stopband rejection, not THD+N\n", (unsigned long)( fs_dst / 2u ) );
    }
}

/* Report the tone the table WILL produce on the live source leg, not just its nominal value.
 * Without this a 96 kHz capture reads "tone=18000Hz" from the 48 kHz HIGH row while actually
 * emitting 36 kHz -- the mislabelling that hid the same class of bug in the dump header. */
static void meas_print_source( const char* tag )
{
    uint32_t fs_src = 0u, fs_dst = 0u;
    meas_leg_rates( &fs_src, &fs_dst );
    const uint32_t actual = meas_tone_actual_hz( s_tbl, fs_src );
    printf(" *MEAS %s source=clean_table row=%u%s tone=%luHz (nominal %luHz at %luHz) "
           "level=%ddBFS len=%u cycles=%u fs_in=%luHz fs_out=%luHz\n",
           tag, (unsigned)s_tone_row,
           ( s_tone_req == MEAS_TONE_REQ_HIGH_AUTO ) ? "(auto)" : "(pinned)",
           (unsigned long)actual,
           (unsigned long)s_tbl->tone_hz, (unsigned long)s_tbl->nominal_fs_hz,
           (int)s_tbl->level_dbfs, (unsigned)s_tbl->len, (unsigned)s_tbl->cycles,
           (unsigned long)fs_src, (unsigned long)fs_dst );
    meas_warn_if_aliasing( actual, fs_dst );
}


//===========================================================
// Global Functions
//===========================================================
void audio_app_meas_gen_input( int32_t* block )
{
#if APP_ASRC_48K_TO_8_DECIMATOR
    uint16_t phase = s_decim_tone_phase;
    const uint16_t step = s_decim_tone_step[s_decim_tone_idx];
    int32_t* p = block;
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        const int32_t sample =
            (int32_t)( (uint32_t)s_decimator_meas_tone[phase] << 8 );
        p[0] = sample;
        p[1] = sample;
        for( uint8_t slot = 2u; slot < APP_SLOTS_PER_FS; slot++ ) { p[slot] = 0; }
        p += APP_SLOTS_PER_FS;
        phase = (uint16_t)( phase + step );
        if( phase >= ASRC_DECIMATOR_MEAS_TONE_TABLE_LENGTH )
        {
            phase = (uint16_t)( phase - ASRC_DECIMATOR_MEAS_TONE_TABLE_LENGTH );
        }
    }
    s_decim_tone_phase = phase;
#else
    if( s_tbl == 0 ) { meas_reselect(); }              // lazy init (silent: runs in the block ISR)
    const int32_t* t   = s_tbl->samp;
    const uint16_t len = s_tbl->len;
    uint16_t       idx = s_tbl_idx;
    int32_t*       p   = block;
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        const int32_t s24 = t[idx];                    // clean precomputed 24-bit sample
        if( ++idx >= len ) { idx = 0u; }               // wraps every `cycles` whole periods
        p[0] = (int32_t)( (uint32_t)s24 << 8 );         // 32-bit left-justified, L = R
        p[1] = (int32_t)( (uint32_t)s24 << 8 );
        for( uint8_t s = 2u; s < APP_SLOTS_PER_FS; s++ ) { p[s] = 0; }
        p += APP_SLOTS_PER_FS;
    }
    s_tbl_idx = idx;                                   // 1 source-domain frame == 1 table advance
#endif
}

#if APP_ASRC_48K_TO_8_DECIMATOR
void audio_app_meas_decimator_process_block( void )
{
    static int32_t input[APP_SLOTS_PER_FS * APP_BLOCK_FRAMES];
    int32_t output[DECIMATED_BLOCK_CAPACITY * 2u];
    size_t produced = 0u;

    if( !s_decimator_initialized )
    {
        (void)asrc_decimator_48_to_8_init( &s_decimator, 2u );
        s_decimator_initialized = 1u;
    }
    audio_app_meas_gen_input( input );
    if( !asrc_decimator_48_to_8_process_s24_left(
            &s_decimator, input, APP_BLOCK_FRAMES, APP_SLOTS_PER_FS,
            output, DECIMATED_BLOCK_CAPACITY, 2u, &produced ) )
    {
        return;
    }

    if( !s_capturing ) { return; }
    for( size_t frame = 0u; frame < produced; frame++ )
    {
        if( s_discard > 0u ) { s_discard--; continue; }
        if( s_cap_idx >= APP_MEAS_CAP_LEN ) { break; }
        s_cap[s_cap_idx++] = output[frame * 2u] >> 8;
    }
    if( s_cap_idx >= APP_MEAS_CAP_LEN )
    {
        s_capturing = 0u;
        s_ready = 1u;
    }
}

void audio_app_meas_set_decimator_tone_idx( uint8_t idx )
{
    if( idx >= 9u ) { idx = 0u; }
    s_decim_tone_idx = idx;
    s_decim_tone_phase = 0u;
    printf(" *MEAS decimator tone index=%u tone=%uHz level=-1dBFS\n",
           (unsigned)idx, (unsigned)s_decim_tone_hz[idx] );
}
#endif

void audio_app_meas_capture( const int32_t* out_block )
{
#if APP_ASRC_MEAS_UART2_STREAM
    // Q19 base: while a long stream is armed, this same A->B output block feeds the frame
    // producer instead of the one-shot RAM capture (they never run at once). One block ->
    // one complete 100-byte frame published to the SPSC ring.
    if( s_str_active ) { meas_stream_produce( out_block ); return; }
#endif
    if( !s_capturing ) { return; }
    const int32_t* p = out_block;
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        if( s_discard > 0u ) { s_discard--; p += APP_SLOTS_PER_FS; continue; } // skip start transient
        if( s_cap_idx >= APP_MEAS_CAP_LEN ) { break; }
        s_cap[s_cap_idx++] = p[0] >> 8;                // 24-bit L (arithmetic shift keeps sign)
        p += APP_SLOTS_PER_FS;
    }
    if( s_cap_idx >= APP_MEAS_CAP_LEN )
    {
        s_capturing = 0u; s_ready = 1u;
        // Q3: snapshot the perturbation state at CAPTURE-COMPLETE (still perturbed here; the sweep
        // returns to base right after, so the long dump runs at base rate and fill does not drift).
        // Read the engine THIS capture measured. Using the _ab getters unconditionally (as this did
        // until 2026-08-02) meant every MEAS_DIR_BA file recorded the other leg's frozen step:
        // constant across captures, and irreconcilable with the spectrum. The dump header now also
        // emits freeze_leg= so a raw file says which engine these fields describe.
        union { float f; uint32_t u; } b, a;
#if (APP_MEAS_DIR == MEAS_DIR_BA)
        b.f = audio_app_asrc_get_freeze_base_step_ba();
        a.f = audio_app_asrc_get_freeze_step_ba();
        s_snap_fill      = audio_app_asrc_get_fill_ba();
        s_snap_ff_live   = audio_app_asrc_get_ratio_live_ba();
        s_snap_ff_frozen = audio_app_asrc_get_ff_frozen_ratio_ba();
#else
        b.f = audio_app_asrc_get_freeze_base_step_ab();
        a.f = audio_app_asrc_get_freeze_step_ab();
        s_snap_fill      = audio_app_asrc_get_fill_ab();
        s_snap_ff_live   = audio_app_asrc_get_ratio_live_ab();
        s_snap_ff_frozen = audio_app_asrc_get_ff_frozen_ratio_ab();
#endif
        s_snap_base_bits = b.u; s_snap_applied_bits = a.u;
        // *ax 00 perturbs the A->B step ONLY, so on a B->A capture there is no perturbation
        // request to report: report none. Reading the getters unconditionally (as this did
        // until 2026-08-02) put the A->B request into a B->A header next to
        // step_delta_ppm_actual, which is computed from the fields above and therefore
        // describes the CAPTURED leg -- so a leftover `*ax 00 ppm 10` could produce
        // `freeze_leg=BA step_delta_mode=ppm step_delta_req=10 step_delta_ppm_actual=0.0000`,
        // two of whose fields describe an engine this file did not measure. The point of the
        // header is that a raw file reads correctly on its own, which that defeats.
#if (APP_MEAS_DIR == MEAS_DIR_BA)
        s_snap_mode = 0;   // prints as "base"
        s_snap_req  = 0;
#else
        s_snap_mode = audio_app_asrc_get_step_delta_mode();
        s_snap_req  = audio_app_asrc_get_step_delta_req();
#endif
        s_snap_ppm  = ( b.f > 0.0f ) ? ( a.f / b.f - 1.0f ) * 1.0e6f : 0.0f;
        // R16 Q16: freeze-age of THIS captured window (pulls since t=0). Counted on the A->B pull
        // counter, which is the only one instrumented -- direction-independent by construction.
        s_snap_age_pulls  = audio_app_asrc_get_q16_age_pulls();
    }
}

void audio_app_meas_arm( void )
{
#if APP_ASRC_48K_TO_8_DECIMATOR
    (void)asrc_decimator_48_to_8_init( &s_decimator, 2u );
    s_decimator_initialized = 1u;
    s_decim_tone_phase = 0u;
    s_cap_idx   = 0u;
    s_ready     = 0u;
    s_discard   = APP_MEAS_DISCARD_FRAMES;
    s_capturing = 1u;
    printf(" *MEAS arm: kernel=decimator_3x2 discard %lu + capture %lu "
           "tone=%uHz level=-1dBFS fs_out=8000Hz\n",
           (unsigned long)APP_MEAS_DISCARD_FRAMES,
           (unsigned long)APP_MEAS_CAP_LEN,
           (unsigned)s_decim_tone_hz[s_decim_tone_idx] );
#else
    /* Re-resolve unconditionally, not just when s_tbl is unset: an *ar rate change between the
     * tone selection and the arm must move an auto HF request onto the right row. */
    meas_reselect();
    s_cap_idx   = 0u;
    s_ready     = 0u;
    s_discard   = APP_MEAS_DISCARD_FRAMES;              // drop the start transient first
    s_capturing = 1u;
    /* Report the ACTUAL tone, derived from the live source rate -- the row's nominal tone_hz was
     * printed here before, which meant a stale or pinned-mismatched row still armed saying
     * "tone=18000Hz" while emitting 36 kHz. This print is the last chance to notice. */
    {
        uint32_t fs_src = 0u, fs_dst = 0u;
        meas_leg_rates( &fs_src, &fs_dst );
        const uint32_t actual = meas_tone_actual_hz( s_tbl, fs_src );
        printf(" *MEAS arm: discard %lu + capture %lu (clean_table row=%u tone=%luHz"
               " (nominal %luHz at %luHz) level=%ddBFS fs_in=%luHz fs_out=%luHz)\n",
               (unsigned long)APP_MEAS_DISCARD_FRAMES, (unsigned long)APP_MEAS_CAP_LEN,
               (unsigned)s_tone_row, (unsigned long)actual,
               (unsigned long)s_tbl->tone_hz, (unsigned long)s_tbl->nominal_fs_hz,
               (int)s_tbl->level_dbfs, (unsigned long)fs_src, (unsigned long)fs_dst );
        meas_warn_if_aliasing( actual, fs_dst );
    }
#endif
}

void audio_app_meas_dump( void )
{
    if( !s_ready )
    {
        printf(" *MEAS not ready (idx=%lu/%lu). Arm with *ac and wait a moment.\n",
               (unsigned long)s_cap_idx, (unsigned long)APP_MEAS_CAP_LEN );
        return;
    }
#if APP_ASRC_48K_TO_8_DECIMATOR
    printf("\n*MEAS_BEGIN kernel=decimator_3x2 tone_hz=%u fs_in_hz=48000 "
           "fs_out_hz=8000 n=%lu source=table level_dbfs=-1 "
           "coeff_crc32=0x%08lX input_frames=%lu output_frames=%lu\n",
           (unsigned)s_decim_tone_hz[s_decim_tone_idx],
           (unsigned long)APP_MEAS_CAP_LEN,
           (unsigned long)ASRC_DECIMATOR_48_TO_8_COEFF_CRC32,
           (unsigned long)s_decimator.input_frames,
           (unsigned long)s_decimator.output_frames );
    for( uint32_t i = 0u; i < APP_MEAS_CAP_LEN; i++ )
    {
        printf("%ld\n", (long)s_cap[i] );
    }
    printf("*MEAS_END\n");
    return;
#endif
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
    const char* kname = "poly";
#else
    const char* kname = "cubic";
#endif
    // Header/footer markers bracket the block so the PC FFT script can extract it from the log.
    // Q3 step-forensic fields = the capture-complete SNAPSHOT (matches the captured samples; the
    // sweep returns to base before this long dump). key=value on the SAME line (parser-safe).
    uint32_t fs_src = 0u;
    uint32_t fs_dst = 0u;
    meas_leg_rates( &fs_src, &fs_dst );
    const uint32_t tone_actual_hz = meas_tone_actual_hz( s_tbl, fs_src );
    printf("\n*MEAS_BEGIN kernel=%s tone_hz=%lu fs_a_hz=%lu fs_in_hz=%lu fs_out_hz=%lu "
           "n=%lu source=table level_dbfs=%d freeze_leg=%s "
           "table_row=%u table_len=%u table_cycles=%u table_nominal_fs_hz=%lu "
           "freeze_base_bits=0x%08lX freeze_applied_bits=0x%08lX "
           "step_delta_mode=%s step_delta_req=%ld step_delta_ppm_actual=%.4f fill_at_cap=%lu "
           "q16_age_pulls=%lu q16_ff_live=%.7f q16_ff_frozen=%.7f\n",
           kname, (unsigned long)tone_actual_hz, (unsigned long)fs_src,
           (unsigned long)fs_src, (unsigned long)fs_dst,
           (unsigned long)APP_MEAS_CAP_LEN, (int)s_tbl->level_dbfs,
#if (APP_MEAS_DIR == MEAS_DIR_BA)
           "BA",
#else
           "AB",
#endif
           (unsigned)s_tone_row, (unsigned)s_tbl->len, (unsigned)s_tbl->cycles,
           (unsigned long)s_tbl->nominal_fs_hz,
           (unsigned long)s_snap_base_bits, (unsigned long)s_snap_applied_bits,
           (s_snap_mode==1)?"ULP":(s_snap_mode==2)?"ppm":"base", (long)s_snap_req,
           (double)s_snap_ppm, (unsigned long)s_snap_fill,
           (unsigned long)s_snap_age_pulls, (double)s_snap_ff_live, (double)s_snap_ff_frozen );
    for( uint32_t i = 0u; i < APP_MEAS_CAP_LEN; i++ )
    {
        printf("%ld\n", (long)s_cap[i] );
    }
    printf("*MEAS_END\n");
}

//===========================================================
// R11 Q11: measurement-only BCLK dedicated-Timer rate observer. Two currently-unused 32-bit
// timers count BCLK edges directly (T2 is the high-res timer; T1/T3 are free). PPS input fan-out
// routes the SAME BCLK RP that already feeds the SPI clock input ALSO to the timer's TxCK input --
// no board change. Both sides are TDM8/32-bit => BCLK = 256*Fs, so the per-frame factor N is equal
// A and B and cancels: the BCLK-derived ratio C_A/C_B is directly comparable to the FS-CCP
// feed-forward ratio (perB/perA = FsA/FsB). OBSERVER ONLY -- never feeds the applied step.
//   T1CK <- RP75 = BCLK-A (codec-A master, /RE10, shared with SPI1 SCK input)
//   T3CK <- RP90 = BCLK-B (dsPIC-B master, /RF9, shared with SPI2 SCK output)
// External SYNCHRONOUS count (TCS=1, TECS=0 -> TxCK pin, TSYNC=1, 1:1). Peripheral clock = FCY =
// 100 MHz >> BCLK ~11-12 MHz (~8x margin), so synchronous mode is valid; T01 confirms empirically.
#if APP_MEAS_Q11_BCLK_OBSERVER
#define Q11_BCLK_A_RP   (75u)   // BCLK-A -> T1CK
#define Q11_BCLK_B_RP   (90u)   // BCLK-B -> T3CK
#else
// Kept defined so every printf that reports the routing compiles unchanged; 0 = "not routed".
#define Q11_BCLK_A_RP   (0u)
#define Q11_BCLK_B_RP   (0u)
#endif

#if APP_MEAS_Q11_BCLK_OBSERVER
static uint8_t s_bclk_inited = 0u;

static void q11_bclk_observer_init( void )
{
    if( s_bclk_inited ) { return; }
    // PPS is IOLOCK-protected after boot -- unlock, add the two TxCK input mappings (fan-out; the
    // SPI SCK input mappings on the same RPs are untouched), re-lock.
    nora_pps_unlock();
    _T1CKR = Q11_BCLK_A_RP;              // PPS: T1CK <- RP75 (fan-out; SPI1 SCK still routed)
    _T3CKR = Q11_BCLK_B_RP;              // PPS: T3CK <- RP90 (fan-out; SPI2 SCK still routed)
    nora_pps_lock();
    // Timer1 = BCLK-A counter
    T1CON = 0u; T1CONbits.ON = 0;
    T1CONbits.TCS   = 1;                 // external clock
    T1CONbits.TECS  = 0;                 // external source = T1CK pin (only option)
    T1CONbits.TSYNC = 1;                 // synchronous external count
    T1CONbits.TCKPS = 0;                 // 1:1
    TMR1 = 0u; PR1 = 0xFFFFFFFFu;
    // Timer3 = BCLK-B counter
    T3CON = 0u; T3CONbits.ON = 0;
    T3CONbits.TCS   = 1;
    T3CONbits.TECS  = 0;
    T3CONbits.TSYNC = 1;
    T3CONbits.TCKPS = 0;
    TMR3 = 0u; PR3 = 0xFFFFFFFFu;
    T1CONbits.ON = 1; T3CONbits.ON = 1;  // free-running, never reset again (delta = wrap-safe)
    s_bclk_inited = 1u;
    printf(" *Q11 BCLK observer: T1CK<-RP%u(BCLK-A) T3CK<-RP%u(BCLK-B) ext-sync 1:1 32-bit\n",
           (unsigned)Q11_BCLK_A_RP, (unsigned)Q11_BCLK_B_RP );
}

// R11 Q11 isolation probe (measurement-only): the full observer init perturbs the FS-CCP-driven
// Mode-S audio. This applies ONLY part of the init to find which action is responsible.
//   mode 0 = PPS IOLOCK unlock+lock, NO writes         (pure lock toggle)
//   mode 1 = mode 0 + write T1CKR/T3CKR                (add the TxCK input routing, timers OFF)
//   mode 2 = mode 1 + configure & start T1/T3 counting (full observer)
void audio_app_meas_q11_isolate( uint8_t mode )
{
    nora_pps_unlock();
    if( mode >= 1u ) { _T1CKR = Q11_BCLK_A_RP; _T3CKR = Q11_BCLK_B_RP; }
    nora_pps_lock();
    if( mode >= 2u )
    {
        T1CON = 0u; T1CONbits.TCS = 1; T1CONbits.TECS = 0; T1CONbits.TSYNC = 1; T1CONbits.TCKPS = 0;
        TMR1 = 0u; PR1 = 0xFFFFFFFFu;
        T3CON = 0u; T3CONbits.TCS = 1; T3CONbits.TECS = 0; T3CONbits.TSYNC = 1; T3CONbits.TCKPS = 0;
        TMR3 = 0u; PR3 = 0xFFFFFFFFu;
        T1CONbits.ON = 1; T3CONbits.ON = 1;
    }
    printf(" *Q11 isolate mode=%u (0=lock-toggle,1=+route,2=+timers)\n", (unsigned)mode );
}
#else  // !APP_MEAS_Q11_BCLK_OBSERVER
void audio_app_meas_q11_isolate( uint8_t mode )
{
    (void)mode;
    printf(" *Q11 unavailable: this build has no BCLK observer (needs two spare 32-bit"
           " external-count timers)\n" );
}
#endif // APP_MEAS_Q11_BCLK_OBSERVER

//===========================================================
// R10 Q10: control-variable trace (measurement-only; see note by s_trace_* declarations).
// R11 Q11: sel=4 = estimator record {BCLK countA, BCLK countB, FS-CCP ratio bits} per snapshot.
//===========================================================
#if APP_MEAS_CTRL_TRACE
void audio_app_meas_trace_arm( uint8_t sel, uint16_t decim )
{
    s_trace_sel    = ( sel <= 11u ) ? sel : 0u;
    s_trace_decim  = ( decim >= 1u ) ? decim : 1u;
    s_trace_dc     = 0u;
    s_trace_idx    = 0u;
    s_trace_ready  = 0u;
    s_trace_start_epoch = audio_app_asrc_get_q16_pull_ctr();   // R16: pull-counter at arm (age origin)
#if APP_MEAS_Q11_BCLK_OBSERVER
    if( s_trace_sel == 4u ) { q11_bclk_observer_init(); }   // lazy: start the BCLK counters
#endif
    if( s_trace_sel == 11u ) { s_q34_idx = 0u; s_q34_clamp = 0u; s_q34_slew = 0u;
                               s_q34_wr_acc = 0u; s_q34_wrap_acc = 0u; }   // Q34/Q35 reset
    s_trace_active = 1u;
    const unsigned cap = ( s_trace_sel == 11u ) ? (unsigned)Q34_REC_CAP : (unsigned)APP_MEAS_CAP_LEN;
    printf(" *TRACE arm: sel=%u (0=step,1=corr_lpf,2=fill,3=ratio,4=estim{cA,cB,ratio},"
           "10=servo{fill,raw_corr,corr_lpf,step,flags},11=q34{fill,frac,corr_lpf,step}) decim=%u n=%u\n",
           (unsigned)s_trace_sel, (unsigned)s_trace_decim, cap );
}

// Called once per A->B pull with the CURRENT (already-computed) control state. No effect unless
// armed. Stores the selected variable's raw 32-bit pattern into s_cap (float bits for sel 0/1,
// integer for sel 2); host reinterprets per the dump header.
// R15 Q15: sel=7 reads the corr-split telemetry via these globals (defined in audio_app_asrc.c) so
// this hot ISR call needs no extra args.
extern float            g_q15_corr_slow;
extern float            g_q15_hold_val;
extern volatile uint8_t g_q15_corr_mode;
extern float            g_q26_inj_val;   // Q26: current servo-error injection sample (trace sel=9)

void audio_app_meas_trace_tick( float applied_step, float corr_lpf, uint32_t fill, float ratio,
                                 float fill_ma, float raw_corr, uint8_t flags, float frac,
                                 uint16_t wraps, uint16_t wr_adv )
{
    if( !s_trace_active ) { return; }

    if( s_trace_sel == 11u )
    {
        // Q34/Q35 record. fill/frac/corr_lpf/applied_step are INSTANTANEOUS at the record boundary; the
        // consumer wraps and producer wr_adv are ACCUMULATED across the whole decimation window (every
        // block, incl. skipped ones) so they span the SAME interval as the decimated fill delta -- i.e.
        // fill[n]-fill[n-1] == (sum wr_adv) - (sum wraps) holds exactly for any decim. clamp/slew hits
        // are counted every block too. No feedback to the loop -- pure capture.
        s_q34_wr_acc   += wr_adv;
        s_q34_wrap_acc += wraps;
        if( flags & 0x1u ) { if( s_q34_clamp < 0xFFFFu ) { s_q34_clamp++; } }
        if( flags & 0x2u ) { if( s_q34_slew  < 0xFFFFu ) { s_q34_slew++;  } }
        if( ++s_trace_dc < s_trace_decim ) { return; }   // still inside the decimation window
        s_trace_dc = 0u;
        if( s_q34_idx >= Q34_REC_CAP ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        uint32_t fill_u16 = ( fill > 0xFFFFu ) ? 0xFFFFu : fill;
        float    fq       = frac * 65536.0f;
        if( fq < 0.0f ) { fq = 0.0f; } else if( fq > 65535.0f ) { fq = 65535.0f; }
        uint32_t frac_u16 = (uint32_t)fq;
        uint32_t wr_sum   = ( s_q34_wr_acc   > 0xFFFFu ) ? 0xFFFFu : s_q34_wr_acc;
        uint32_t wrap_sum = ( s_q34_wrap_acc > 0xFFFFu ) ? 0xFFFFu : s_q34_wrap_acc;
        union { float f; int32_t s; } cl34, st34;
        cl34.f = corr_lpf; st34.f = applied_step;
        int32_t* rec = &s_q34_cap[ s_q34_idx * Q34_WORDS ];
        rec[0] = (int32_t)( ( fill_u16 << 16 ) | frac_u16 );
        rec[1] = cl34.s;
        rec[2] = st34.s;
        rec[3] = (int32_t)( ( wr_sum << 16 ) | wrap_sum );   // window-summed wr_adv + wraps
        s_q34_idx++;
        s_q34_wr_acc = 0u; s_q34_wrap_acc = 0u;              // reset for the next window
        if( s_q34_idx >= Q34_REC_CAP ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }

    if( ++s_trace_dc < s_trace_decim ) { return; }
    s_trace_dc = 0u;

    union { float f; uint32_t u; int32_t s; } b;
    if( s_trace_sel == 4u )
    {
        // R11 estimator record: 3 int32 per snapshot {BCLK countA, BCLK countB, FS-CCP ratio bits}.
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } rb; rb.f = ratio;  // FS-CCP feed-forward ratio at this snapshot
        // Snapshot both free-running BCLK counters back-to-back (adjacent SFR reads). No interrupt
        // masking: we must not perturb the CCP/audio path we are characterizing; the A/B read skew
        // is just the inter-read instruction latency (a few CPU cycles << 1 BCLK edge), plus the rare
        // case of a higher-priority ISR landing between the reads (bounded + estimated on host).
#if APP_MEAS_Q11_BCLK_OBSERVER
        uint32_t cA = TMR1;
        uint32_t cB = TMR3;
#else
        uint32_t cA = 0u;   // observer absent: the record keeps its shape, the counts read zero
        uint32_t cB = 0u;
#endif
        s_cap[s_trace_idx++] = (int32_t)cA;
        s_cap[s_trace_idx++] = (int32_t)cB;
        s_cap[s_trace_idx++] = rb.s;                     // simultaneous FS-CCP feed-forward ratio
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 5u )
    {
        // R13 Q13: simultaneous {fill, corr_lpf, applied step} per snapshot -> offline controller replay.
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } c, p;
        c.f = corr_lpf; p.f = applied_step;
        s_cap[s_trace_idx++] = (int32_t)fill;
        s_cap[s_trace_idx++] = c.s;
        s_cap[s_trace_idx++] = p.s;
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 6u )
    {
        // R14 Q14: simultaneous {raw fill, MA64 fill, corr_lpf} -> beat-rejection + modified-ctrl replay.
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } m, c;
        m.f = fill_ma; c.f = corr_lpf;
        s_cap[s_trace_idx++] = (int32_t)fill;
        s_cap[s_trace_idx++] = m.s;
        s_cap[s_trace_idx++] = c.s;
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 7u )
    {
        // R15 Q15: simultaneous {corr_full(=corr_lpf), corr_slow, corr_used} -> split identity +
        // split replay + selector replay (corr_fast = full - slow offline). corr_used recomputed
        // here from the split globals + current mode (matches the servo's corr_used exactly).
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        const float cfast = corr_lpf - g_q15_corr_slow;
        const float corr_used = ( g_q15_corr_mode == 1u ) ? g_q15_corr_slow + g_q15_hold_val
                              : ( g_q15_corr_mode == 2u ) ? g_q15_hold_val + cfast
                              : ( g_q15_corr_mode == 3u ) ? g_q15_hold_val
                                                          : corr_lpf;
        union { float f; int32_t s; } cf, cs, cu;
        cf.f = corr_lpf; cs.f = g_q15_corr_slow; cu.f = corr_used;
        s_cap[s_trace_idx++] = cf.s;
        s_cap[s_trace_idx++] = cs.s;
        s_cap[s_trace_idx++] = cu.s;
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 8u )
    {
        // R16 Q16: aging trajectory {live_ff(=ratio), corr_full(=corr_lpf), step} per snapshot; host
        // derives per-sample freeze-age from start_epoch/freeze_epoch/decim in the dump header.
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } lf, cf2, st;
        lf.f = ratio; cf2.f = corr_lpf; st.f = applied_step;
        s_cap[s_trace_idx++] = lf.s;
        s_cap[s_trace_idx++] = cf2.s;
        s_cap[s_trace_idx++] = st.s;
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 9u )
    {
        // Q26: {injection, applied step, fill} per snapshot -> injection->actual & injection->fill.
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } ij, st9;
        ij.f = g_q26_inj_val; st9.f = applied_step;
        s_cap[s_trace_idx++] = ij.s;
        s_cap[s_trace_idx++] = st9.s;
        s_cap[s_trace_idx++] = (int32_t)fill;
        if( s_trace_idx + 3u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }
    if( s_trace_sel == 10u )
    {
        // Q29 Q10: servo internal-state record {fill, raw_corr, corr_lpf, applied_step, flags} per
        // snapshot -> reconstructs the fill-quantization / clamp / slew sequence that drives the
        // 8-13 Hz limit cycle. flags bit0=clamp_hit, bit1=slew_hit (read-only; no controller change).
        if( s_trace_idx + 5u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
        union { float f; int32_t s; } rc, cl, st10;
        rc.f = raw_corr; cl.f = corr_lpf; st10.f = applied_step;
        s_cap[s_trace_idx++] = (int32_t)fill;
        s_cap[s_trace_idx++] = rc.s;
        s_cap[s_trace_idx++] = cl.s;
        s_cap[s_trace_idx++] = st10.s;
        s_cap[s_trace_idx++] = (int32_t)flags;
        if( s_trace_idx + 5u > APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
        return;
    }

    if( s_trace_idx >= APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; return; }
    if( s_trace_sel == 1u )      { b.f = corr_lpf; }
    else if( s_trace_sel == 2u ) { b.u = fill; }
    else if( s_trace_sel == 3u ) { b.f = ratio; }        // feed-forward ratio (perB/perA)
    else                         { b.f = applied_step; }
    s_cap[s_trace_idx++] = b.s;

    if( s_trace_idx >= APP_MEAS_CAP_LEN ) { s_trace_active = 0u; s_trace_ready = 1u; }
}

void audio_app_meas_trace_dump( void )
{
    if( !s_trace_ready )
    {
        printf(" *TRACE not ready (idx=%lu/%lu). Arm with *ag and wait.\n",
               (unsigned long)s_trace_idx, (unsigned long)APP_MEAS_CAP_LEN );
        return;
    }
    // block rate = FsB / APP_BLOCK_FRAMES ; effective trace rate = block rate / decim.
    const double blk_hz = ( 390625.0 / (double)( APP_SPI2_MASTER_BRG + 1 ) ) / (double)APP_BLOCK_FRAMES;
    // Reuse the *MEAS_BEGIN/_END framing so the existing capture pipeline grabs it; kernel=trace_*
    // tells the host to reinterpret each int32 line as float bits (step/corr) or integer (fill).
    if( s_trace_sel == 4u )
    {
        // R11 estimator record: n records of {countA countB ratio_bits}, one record per line.
        const uint32_t nrec = s_trace_idx / 3u;
        printf("\n*MEAS_BEGIN kernel=trace_estim sel=4 decim=%u blk_hz=%.4f trace_hz=%.4f "
               "records=%lu bclk_a_rp=%u bclk_b_rp=%u bclk_per_frame=256 fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned)Q11_BCLK_A_RP, (unsigned)Q11_BCLK_B_RP,
               (unsigned long)APP_MEAS_FS_A_HZ, (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            // countA and countB as UNSIGNED (wrap-safe deltas done on host); ratio as float bits.
            printf("%lu %lu %ld\n", (unsigned long)(uint32_t)s_cap[3u*r],
                   (unsigned long)(uint32_t)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 5u )
    {
        // R13 Q13: n records of {fill(int) corr_lpf_bits step_bits}, one per line.
        const uint32_t nrec = s_trace_idx / 3u;
        // kp/alpha mirror audio_app_asrc.c (ASRC_KP / ASRC_CORR_ALPHA -- file-local there); fill_target
        // = APP_ASRC_FIFO_FRAMES/2. Q13 forbids changing them, so these literals stay in sync.
        printf("\n*MEAS_BEGIN kernel=trace_fcs sel=5 decim=%u blk_hz=%.4f trace_hz=%.4f "
               "records=%lu kp=5.0e-6 alpha=4.0e-3 fill_target=%u fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned)( APP_ASRC_FIFO_FRAMES / 2u ),
               (unsigned long)APP_MEAS_FS_A_HZ,
               (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld\n", (long)s_cap[3u*r], (long)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 6u )
    {
        // R14 Q14: n records of {raw_fill(int) MA64_fill_bits corr_lpf_bits}, one per line.
        const uint32_t nrec = s_trace_idx / 3u;
        printf("\n*MEAS_BEGIN kernel=trace_fma sel=6 decim=%u blk_hz=%.4f trace_hz=%.4f "
               "records=%lu ma_n=64 kp=5.0e-6 alpha=4.0e-3 fill_target=%u fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned)( APP_ASRC_FIFO_FRAMES / 2u ),
               (unsigned long)APP_MEAS_FS_A_HZ,
               (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld\n", (long)s_cap[3u*r], (long)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 7u )
    {
        // R15 Q15: n records of {corr_full_bits corr_slow_bits corr_used_bits}, one per line.
        const uint32_t nrec = s_trace_idx / 3u;
        printf("\n*MEAS_BEGIN kernel=trace_csplit sel=7 decim=%u blk_hz=%.4f trace_hz=%.4f "
               "records=%lu split_fc_hz=1.0 beta=0.00462185 fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned long)APP_MEAS_FS_A_HZ,
               (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld\n", (long)s_cap[3u*r], (long)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 8u )
    {
        // R16 Q16: aging trajectory records {live_ff_bits corr_full_bits step_bits}. Per-sample age =
        // (start_epoch + r*decim - freeze_epoch)/trace_hz. frozen_ff constant -> stale_gap=live-frozen.
        const uint32_t nrec = s_trace_idx / 3u;
        union { float f; int32_t s; } fz; fz.f = audio_app_asrc_get_ff_frozen_ratio_ab();
        printf("\n*MEAS_BEGIN kernel=trace_age sel=8 decim=%u blk_hz=%.4f trace_hz=%.4f records=%lu "
               "start_epoch=%lu freeze_epoch=%lu frozen_ff_bits=0x%08lX fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned long)s_trace_start_epoch,
               (unsigned long)audio_app_asrc_get_q16_freeze_epoch(), (unsigned long)fz.s,
               (unsigned long)APP_MEAS_FS_A_HZ, (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld\n", (long)s_cap[3u*r], (long)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 9u )
    {
        // Q26: injection-transfer records {inj_bits applied_step_bits fill}. Host lock-in demods the
        // response (applied_step, fill) at the known injection frequency for gain/phase vs injection.
        const uint32_t nrec = s_trace_idx / 3u;
        printf("\n*MEAS_BEGIN kernel=trace_inject sel=9 decim=%u blk_hz=%.4f trace_hz=%.4f records=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim, (unsigned long)nrec );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld\n", (long)s_cap[3u*r], (long)s_cap[3u*r+1u], (long)s_cap[3u*r+2u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 10u )
    {
        // Q29 Q10: servo internal-state records {fill, raw_corr_bits, corr_lpf_bits, step_bits,
        // flags(bit0=clamp_hit,bit1=slew_hit)}. frozen_ff_bits lets the host derive target_step
        // (=frozen_ff*(1+corr_lpf)) and the clamp bounds (frozen_ff*ASRC_STEP_LO/HI) exactly.
        const uint32_t nrec = s_trace_idx / 5u;
        union { float f; int32_t s; } fz10; fz10.f = audio_app_asrc_get_ff_frozen_ratio_ab();
        printf("\n*MEAS_BEGIN kernel=trace_servo sel=10 decim=%u blk_hz=%.4f trace_hz=%.4f records=%lu "
               "frozen_ff_bits=0x%08lX\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim,
               (unsigned long)nrec, (unsigned long)fz10.s );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            printf("%ld %ld %ld %ld %ld\n", (long)s_cap[5u*r], (long)s_cap[5u*r+1u],
                   (long)s_cap[5u*r+2u], (long)s_cap[5u*r+3u], (long)s_cap[5u*r+4u] );
        }
        printf("*MEAS_END\n");
        return;
    }
    if( s_trace_sel == 11u )
    {
        // Q34/Q35 records {fill(int) frac_u16 corr_lpf_bits step_bits wraps wr_adv}. word0 packs
        // fill<<16|frac_u16; word3 packs wr_adv<<16|wraps -> unpacked here to 6 columns. raw_corr =
        // kp*(fill-target) and target_step = ratio*(1+corr_lpf) are host-derivable; frac=frac_u16/65536.
        // wraps = consumer rd-advances / block; wr_adv = producer frames pushed / same interval; the
        // host checks fill[n]-fill[n-1] == wr_adv[n]-wraps[n]. clamp_cnt/slew_cnt attest the limiters
        // stayed inactive. alpha mirrors the shipping ASRC_CORR_ALPHA default (file-local in asrc.c).
        const uint32_t nrec = s_q34_idx;
        printf("\n*MEAS_BEGIN kernel=trace_q34 sel=11 decim=%u blk_hz=%.4f trace_hz=%.4f records=%lu "
               "kp=5.0e-6 alpha=1.3333333e-3 fill_target=%u fifo_frames=%u block_frames=%u frac_scale=65536 "
               "clamp_cnt=%u slew_cnt=%u fs_a_hz=%lu tone_hz=%lu\n",
               (unsigned)s_trace_decim, blk_hz, blk_hz / (double)s_trace_decim, (unsigned long)nrec,
               (unsigned)( APP_ASRC_FIFO_FRAMES / 2u ), (unsigned)APP_ASRC_FIFO_FRAMES,
               (unsigned)APP_BLOCK_FRAMES, (unsigned)s_q34_clamp, (unsigned)s_q34_slew,
               (unsigned long)APP_MEAS_FS_A_HZ, (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ) );
        for( uint32_t r = 0u; r < nrec; r++ )
        {
            const uint32_t w0 = (uint32_t)s_q34_cap[Q34_WORDS * r];
            const uint32_t w3 = (uint32_t)s_q34_cap[Q34_WORDS * r + 3u];
            printf("%lu %lu %ld %ld %lu %lu\n", (unsigned long)( w0 >> 16 ), (unsigned long)( w0 & 0xFFFFu ),
                   (long)s_q34_cap[Q34_WORDS * r + 1u], (long)s_q34_cap[Q34_WORDS * r + 2u],
                   (unsigned long)( w3 & 0xFFFFu ), (unsigned long)( w3 >> 16 ) );
        }
        printf("*MEAS_END\n");
        return;
    }
    const char* sname = ( s_trace_sel == 1u ) ? "trace_corr_lpf"
                      : ( s_trace_sel == 2u ) ? "trace_fill"
                      : ( s_trace_sel == 3u ) ? "trace_ratio"
                                              : "trace_step";
    printf("\n*MEAS_BEGIN kernel=%s sel=%u decim=%u blk_hz=%.4f trace_hz=%.4f n=%lu "
           "tone_hz=%lu fs_a_hz=%lu level_dbfs=%d\n",
           sname, (unsigned)s_trace_sel, (unsigned)s_trace_decim,
           blk_hz, blk_hz / (double)s_trace_decim, (unsigned long)APP_MEAS_CAP_LEN,
           (unsigned long)( s_tbl ? s_tbl->tone_hz : 0u ), (unsigned long)APP_MEAS_FS_A_HZ,
           (int)( s_tbl ? s_tbl->level_dbfs : 0 ) );
    for( uint32_t i = 0u; i < APP_MEAS_CAP_LEN; i++ )
    {
        printf("%ld\n", (long)s_cap[i] );
    }
    printf("*MEAS_END\n");
}
#else  // !APP_MEAS_CTRL_TRACE
// Servo-diagnostic trace omitted (see APP_MEAS_CTRL_TRACE).  The symbols stay so the console and
// the pull path need no #if of their own; a 16 KB part cannot afford the buffers or the printf
// bodies, and nothing in a DR / THD+N measurement reads them.
void audio_app_meas_trace_arm( uint8_t sel, uint16_t decim )
{
    (void)sel; (void)decim;
    printf(" *trace unavailable: this build omits the control-variable trace\n" );
}
void audio_app_meas_trace_tick( float applied_step, float corr_lpf, uint32_t fill, float ratio,
                                 float fill_ma, float raw_corr, uint8_t flags, float frac,
                                 uint16_t wraps, uint16_t wr_adv )
{
    (void)applied_step; (void)corr_lpf; (void)fill; (void)ratio; (void)fill_ma;
    (void)raw_corr; (void)flags; (void)frac; (void)wraps; (void)wr_adv;
}
void audio_app_meas_trace_dump( void )
{
    printf(" *trace unavailable: this build omits the control-variable trace\n" );
}
#endif // APP_MEAS_CTRL_TRACE

void audio_app_meas_set_tone_low( void )
{
#if APP_ASRC_48K_TO_8_DECIMATOR
    audio_app_meas_set_decimator_tone_idx( 0u );
#else
    s_tone_req = MEAS_TONE_REQ_PINNED;
    s_tone_row = MEAS_TONE_ROW_LOW;
    meas_reselect();
    meas_print_source( "tone=low" );
#endif
}

/* "High" is a band-edge STRESS request, not a fixed table: the 18 kHz tone has to be 18 kHz on
 * whatever rate the source leg is actually running.  The tables are sample-domain exact, so one
 * geometry cannot serve both -- the 48 kHz row (8 samples / 3 cycles) becomes 36 kHz on a 96 kHz
 * leg, above a 48 kHz output's Nyquist, and would alias instead of stressing the band edge.
 *
 * This records the REQUEST; meas_reselect() resolves it against the live rate, and does so again at
 * arm time, so a rate change after this call is followed correctly.  Use
 * audio_app_meas_set_tone_row() to pin a row deliberately (e.g. to demonstrate that aliasing). */
void audio_app_meas_set_tone_high( void )
{
#if APP_ASRC_48K_TO_8_DECIMATOR
    audio_app_meas_set_decimator_tone_idx( 8u );
#else
    s_tone_req = MEAS_TONE_REQ_HIGH_AUTO;
    meas_reselect();
    meas_print_source( "tone=high" );
#endif
}

// Bench escape hatch: PIN a tone row (MEAS_TONE_ROW_*), bypassing the rate-matching in
// set_tone_high and any later re-resolution. Deliberately allows a mismatched row so the aliasing
// failure mode can be demonstrated rather than only reasoned about; the prints warn when it happens.
void audio_app_meas_set_tone_row( uint8_t row )
{
    if( row >= (uint8_t)MEAS_TABLE_N_TONES ) { row = MEAS_TONE_ROW_LOW; }
    s_tone_req = MEAS_TONE_REQ_PINNED;
    s_tone_row = row;
    meas_reselect();
    meas_print_source( "tone=row" );
}

// Bench: set the input tone level by INDEX (0=-1, 1=-60, 2=-20, 3=-40, 4=-80, 5=-6 dBFS) -- the
// *nt2A order. Picks the matching precomputed table (amplitude is baked into the table samples).
void audio_app_meas_set_level_idx( uint8_t idx )
{
    if( idx >= (uint8_t)MEAS_TABLE_N_LEVELS ) { idx = 0u; }
    s_level_idx = idx;
    meas_reselect();
    meas_print_source( "level" );
}

#if APP_ASRC_MEAS_UART2_STREAM
//===========================================================
// Q19 base: long-coherent binary stream (see the state block above).
//===========================================================

// Producer -- runs in the SPI2 RX-block ISR (via audio_app_meas_capture). Builds ONE complete
// 100-byte audio frame from the just-resampled A->B output block and, every TELEM_DECIM frames,
// appends a 32-byte 0xA6 telemetry record snapshotting the control state right after this block's
// ASRC pull. The audio+telemetry group is capacity-checked and published atomically (single head
// store at the end) so the consumer never sees a partial record and audio is never published
// without its due telemetry. On no room it latches overflow and stops (hard discontinuity).
static void meas_stream_produce( const int32_t* out_block )
{
    if( !s_str_active ) { return; }
    if( s_str_seq >= s_str_target ) { s_str_active = 0u; return; }   // target reached -> stop cleanly

    const uint8_t  telem_due = ( ( s_str_seq % TELEM_DECIM ) == 0u ) ? 1u : 0u;
    const uint32_t need      = STR_FRAME_BYTES + ( telem_due ? TELEM_BYTES : 0u );

    uint32_t h    = s_str_head;
    uint32_t t    = s_str_tail;
    uint32_t used = ( h >= t ) ? ( h - t ) : ( STR_RING_BYTES - ( t - h ) );
    // Group capacity check (keep 1 byte free so head==tail is unambiguously EMPTY): the whole
    // audio(+telemetry) group must fit, else it is a hard discontinuity.
    if( ( STR_RING_BYTES - 1u - used ) < need )
    {
        s_str_ovf = 1u; s_str_active = 0u; return;   // hard discontinuity: counted, producer halts
    }

    if( s_str_seq == 0u )
    {
        // Latch the ASRC pull counter at frame 0. The SPI2 ISR runs asrc_pull() (which advances
        // the counter) BEFORE this capture hook, so this is exactly the pull index of frame 0:
        // host maps frame k -> pull (start_pull + k), freeze at (freeze_epoch - start_pull).
        s_str_start_pull = audio_app_asrc_get_q16_pull_ctr();
    }

    uint8_t* ring = (uint8_t*)s_cap;
    // --- audio 0xA5 frame ---
    ring[h] = STR_MAGIC;                       if( ++h >= STR_RING_BYTES ) { h = 0u; }
    ring[h] = (uint8_t)( s_str_seq       );    if( ++h >= STR_RING_BYTES ) { h = 0u; }
    ring[h] = (uint8_t)( s_str_seq >> 8  );    if( ++h >= STR_RING_BYTES ) { h = 0u; }
    ring[h] = (uint8_t)( s_str_seq >> 16 );    if( ++h >= STR_RING_BYTES ) { h = 0u; }
    const int32_t* p = out_block;
    for( uint16_t n = 0u; n < STR_SAMP_PER_FRAME; n++ )
    {
        const int32_t s24 = p[0] >> 8;         // 24-bit L (arithmetic shift keeps sign): same tap as s_cap
        ring[h] = (uint8_t)( s24       );      if( ++h >= STR_RING_BYTES ) { h = 0u; }
        ring[h] = (uint8_t)( s24 >> 8  );      if( ++h >= STR_RING_BYTES ) { h = 0u; }
        ring[h] = (uint8_t)( s24 >> 16 );      if( ++h >= STR_RING_BYTES ) { h = 0u; }
        p += APP_SLOTS_PER_FS;                 // stride past the other TDM slots -> mono L only
    }

    // --- optional 0xA6 telemetry record: snapshot the just-computed control state (pull-aligned) ---
    if( telem_due )
    {
        union { float f; uint32_t u; } cv;
        uint8_t  tb[TELEM_BYTES];
        const uint32_t pull = audio_app_asrc_get_q16_pull_ctr();
        const uint32_t fill = audio_app_asrc_get_fill_ab();
        tb[0] = TELEM_MAGIC;
        tb[1] = (uint8_t)TELEM_VERSION;
        tb[2]=(uint8_t)pull;      tb[3]=(uint8_t)(pull>>8);  tb[4]=(uint8_t)(pull>>16);  tb[5]=(uint8_t)(pull>>24);
        cv.f = audio_app_asrc_get_ratio_live_ab();      tb[6] =(uint8_t)cv.u; tb[7] =(uint8_t)(cv.u>>8); tb[8] =(uint8_t)(cv.u>>16); tb[9] =(uint8_t)(cv.u>>24);
        cv.f = audio_app_asrc_get_ff_frozen_ratio_ab(); tb[10]=(uint8_t)cv.u; tb[11]=(uint8_t)(cv.u>>8); tb[12]=(uint8_t)(cv.u>>16); tb[13]=(uint8_t)(cv.u>>24);
        cv.f = audio_app_asrc_get_corr_lpf_ab();        tb[14]=(uint8_t)cv.u; tb[15]=(uint8_t)(cv.u>>8); tb[16]=(uint8_t)(cv.u>>16); tb[17]=(uint8_t)(cv.u>>24);
        cv.f = audio_app_asrc_get_step_state_ab();      tb[18]=(uint8_t)cv.u; tb[19]=(uint8_t)(cv.u>>8); tb[20]=(uint8_t)(cv.u>>16); tb[21]=(uint8_t)(cv.u>>24);
        cv.f = audio_app_asrc_get_fill_ma_ab();         tb[22]=(uint8_t)cv.u; tb[23]=(uint8_t)(cv.u>>8); tb[24]=(uint8_t)(cv.u>>16); tb[25]=(uint8_t)(cv.u>>24);
        tb[26]=(uint8_t)fill;     tb[27]=(uint8_t)(fill>>8);
        uint8_t flags = 0u;
        if( audio_app_asrc_get_ff_freeze_ab() )   { flags |= 0x01u; }             // bit0 FF frozen
        if( audio_app_asrc_get_fill_use_ma_ab() ) { flags |= 0x02u; }             // bit1 MA64 enabled
        if( audio_app_asrc_get_corr_hold_ab() )   { flags |= 0x04u; }             // bit2 corr hold
        flags |= (uint8_t)( ( audio_app_asrc_get_corr_mode_ab() & 0x03u ) << 3 ); // bits3-4 corr mode
        // bit5 (Mode-K/open-loop freeze) has no getter; Mode-K is OFF in the Q19 protocol -> 0.
        tb[28] = flags;
        tb[29] = 0u;                                                             // reserved
        const uint16_t crc = str_crc16_ccitt( tb, 30u );
        tb[30] = (uint8_t)crc;    tb[31] = (uint8_t)( crc >> 8 );
        for( uint8_t i = 0u; i < TELEM_BYTES; i++ )
        {
            ring[h] = tb[i];                       if( ++h >= STR_RING_BYTES ) { h = 0u; }
        }
        s_str_telem++;
    }

    s_str_head = h;                            // PUBLISH the whole group (single store, last)
    s_str_seq++;

    used += need;
    if( used > s_str_peak ) { s_str_peak = used; }
}

void audio_app_meas_stream_arm( uint8_t seconds )
{
    if( s_tbl == 0 ) { meas_reselect(); }
    const uint32_t target = (uint32_t)( (double)seconds * STR_BLK_HZ + 0.5 );

    s_str_head = 0u; s_str_tail = 0u;
    s_str_seq  = 0u; s_str_bytes = 0u; s_str_peak = 0u; s_str_ovf = 0u;
    s_str_start_pull = 0u; s_str_telem = 0u; s_str_inflight = 0u;
    s_str_begin_sent = 0u; s_str_end_sent = 0u;
    s_str_target = target;
    s_str_active = 1u;                          // producer starts on the next SPI2 block

    // *STREAM_ARM goes on the CONTROL port (UART1); the DATA port (UART2) stays binary-only.
    printf(" *STREAM_ARM data_uart=2 data_baud=%lu secs=%u target_frames=%lu frame_bytes=%u "
           "samples_per_frame=%u magic=0xA5 fmt=bin24le\n",
           (unsigned long)UART2_StreamBaud(), (unsigned)seconds,
           (unsigned long)s_str_target, (unsigned)STR_FRAME_BYTES, (unsigned)STR_SAMP_PER_FRAME );
}

int audio_app_meas_stream_service( void )
{
    if( ( s_str_target == 0u ) || s_str_end_sent ) { return 0; }    // idle

    if( !s_str_begin_sent )
    {
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
        const char* kname = "poly";
#else
        const char* kname = "cubic";
#endif
        printf(" *STREAM_BEGIN kernel=%s fmt=bin24le magic=0xA5 seq_bytes=3 frame_bytes=%u "
               "samples_per_frame=%u target_frames=%lu blk_hz=%.4f fs_b_hz=%.4f data_baud=%lu "
               "telemetry_magic=0xA6 telemetry_version=%u telemetry_bytes=%u telemetry_decim=%u "
               "telemetry_crc=crc16_ccitt poly_l=%u poly_window=%s fifo_frames=%u cap_len=%u\n",
               kname, (unsigned)STR_FRAME_BYTES, (unsigned)STR_SAMP_PER_FRAME,
               (unsigned long)s_str_target, (double)STR_BLK_HZ,
               (double)( STR_BLK_HZ * (double)APP_BLOCK_FRAMES ),
               (unsigned long)UART2_StreamBaud(),
               (unsigned)TELEM_VERSION, (unsigned)TELEM_BYTES, (unsigned)TELEM_DECIM,
               (unsigned)ASRC_POLY_L,
#if (ASRC_POLY_WINDOW == 1)
               "blackman_harris",
#else
               "blackman",
#endif
               (unsigned)APP_ASRC_FIFO_FRAMES, (unsigned)APP_MEAS_CAP_LEN );
        s_str_begin_sent = 1u;
    }

    // Interrupt-driven drain. A run submitted earlier is shifted out by the IPL5 U2TX ISR (which
    // preempts the SPI2 block ISR so the FIFO never starves). We keep ONE run in flight at a time:
    // wait for it to finish, advance the tail past it, then submit the next contiguous run. The
    // ring keeps [tail..tail+inflight) occupied until the run completes, so the producer never
    // overwrites in-flight bytes. Return 1 so the caller skips telemetry prints during the capture.
    if( UART2_StreamTxBusy() ) { return 1; }        // previous run still draining to the FIFO
    if( s_str_inflight != 0u )                      // it just finished -> free that ring span
    {
        uint32_t tl = s_str_tail + s_str_inflight;
        if( tl >= STR_RING_BYTES ) { tl -= STR_RING_BYTES; }
        s_str_tail    = tl;
        s_str_bytes  += s_str_inflight;
        s_str_inflight = 0u;
    }
    {
        const uint32_t head = s_str_head;
        const uint32_t tail = s_str_tail;
        if( head != tail )
        {
            const uint32_t run = ( head > tail ) ? ( head - tail ) : ( STR_RING_BYTES - tail );
            if( UART2_StreamSubmit( (const uint8_t*)s_cap + tail, (size_t)run ) != 0u )
            {
                s_str_inflight = run;               // handed to the async engine; tail advances when done
            }
            return 1;
        }
    }

    // Ring empty + nothing in flight. When the producer has stopped AND the last byte has
    // physically left the wire, close the capture with the *STREAM_END metadata and return to normal.
    if( !s_str_active && UART2_StreamTxDone() )
    {
        printf(" *STREAM_END audio_frames=%lu target_frames=%lu telemetry_records=%lu bytes=%lu "
               "overflow=%u peak_ring=%lu/%lu q16_start_pull=%lu q16_freeze_epoch=%lu samples_per_frame=%u\n",
               (unsigned long)s_str_seq, (unsigned long)s_str_target, (unsigned long)s_str_telem,
               (unsigned long)s_str_bytes, (unsigned)s_str_ovf,
               (unsigned long)s_str_peak, (unsigned long)STR_RING_BYTES,
               (unsigned long)s_str_start_pull,
               (unsigned long)audio_app_asrc_get_q16_freeze_epoch(),
               (unsigned)STR_SAMP_PER_FRAME );
        s_str_end_sent = 1u;
        s_str_target   = 0u;    // back to idle
        return 0;               // let the normal loop (prints) resume
    }

    return 1;   // still producing, or waiting for the wire to drain: keep owning the loop
}
#endif // APP_ASRC_MEAS_UART2_STREAM

#endif // APP_ASRC_MEAS
