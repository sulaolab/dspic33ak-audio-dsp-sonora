// asrc_fir_kernel_bench.c
//
// "*aq" -- measure the three candidate front-stage FIR kernels ON HARDWARE, in CPU cycles per MAC.
//
// The offline work ([internal] report_ak128_fir_kernel_issue_ceiling_2026-08-21.md) fixed the ISA ceiling from
// the instruction tables: float cannot go below 2 instructions per MAC because the FPU mac.s takes
// register operands only, while Q31 + REPEAT + MAC.l is 1 instruction per MAC.  Everything below
// exists to answer what an instruction table cannot:
//
//   M1  does 1 cycle/MAC actually come out, with the coefficients in X RAM and the history in Y?
//       And what does it cost when both land in X space -- the failure mode with no functional
//       symptom (4.3.17 charges "typically one cycle" extra per MAC).
//   M6  is a NON-power-of-two modulo ring legal, and at which start addresses?  4.3.18 says an
//       incrementing buffer has "certain restrictions on the buffer start address" and gives no
//       numeric rule; the vendor init routine enforces nothing.  This sweeps the start address and
//       lets the hardware answer.  It decides whether a 16ch 190-tap history costs 12,160 B or
//       16,384 B.
//
// HOW THE NUMBER IS TAKEN.  The high-res timer is Timer2 clocked at FCY with a 1:1 prescale, so one
// count is one instruction cycle and no conversion is involved.  Two things pollute a single
// reading: the TDM ISRs (~75% duty in the 16ch build) and DMA bus stealing.  Both are handled by
// reporting the MINIMUM over `trials` -- the trial that nothing interrupted -- with `near` counting
// the trials within 1/16 of it, which is the evidence that the floor is real and not a timer
// artefact.
//
// The headline per-MAC figure is a SLOPE, not a division: each kernel is measured at two tap counts
// and the per-MAC cost is (cycles_hi - cycles_lo) / (taps_hi - taps_lo).  That cancels the call
// overhead, the prologue, the timer read and the loop setup exactly, instead of hoping they are
// small.  The absolute per-call cost is printed next to it so the fixed overhead stays visible.
//
// RUNNING IT IS ITSELF A TEST of the interrupt-safety claim (report section 11): this runs in the
// foreground (CTX0) using ACCA, RCOUNT and the CORCON DSP bits while the TDM ISRs run at IPL4
// (CTX4) using the same resources.  If that context banking did not hold, the correctness checks
// below would fail rather than the numbers merely drifting.

// app_specific_config_defs.h comes FIRST, and the order matters: the build profile
// sets ASRC_FIR_KERNEL_BENCH_AVAILABLE (the shipping BiDir profile sets it to 0 to
// get its 1,184 B of data memory back), and asrc_fir_kernel_bench.h only respects
// that with #ifndef -- reached the other way round, this file would compile the
// bench in while asrc_console.c, which does include the app config first, compiles
// its caller out. Nothing errors in that state; the RAM just comes back. Any
// measurement of this profile's data memory catches it (see
// [internal] report_ak512_asrc_ram_gate_2026-08-22.md).
#include "app_specific_config_defs.h"

#include "asrc_fir_kernel_bench.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_fir_kernel_bench.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

// Not available in this image -- either the device cannot make the measurement (AK512-only, see the
// header) or a build switched it off to reclaim its RAM.  The whole file compiles to nothing rather
// than erroring, because the switch has to work from -Define, where configurations.xml cannot
// exclude a source.  Other devices still exclude it there; this guard and that exclusion agree.
#if !ASRC_FIR_KERNEL_BENCH_AVAILABLE
typedef int asrc_fir_kernel_bench_unavailable_t;   // keep the translation unit non-empty (C11 6.9)
#else

#include <xc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "arm_math.h"
#include "nora_high_res_timer.h"
#include "asrc_h2_timing_coeffs.h"
#include "asrc_fir_tradeoff_coeffs.h"

// ---- the kernels under test -------------------------------------------------------------------
// src/app/dspic33-cmsis-dsp/Source/FilteringFunctions/fir_ring_*.s .  Each file's header block is
// the specification; only the C-visible signature is repeated here.

// Mirrored history, no modulo at all: `taps + 5` instructions for `taps` MACs.
extern void fir_ring_q31( const int32_t* coeff, const int32_t* hist, uint32_t taps, int32_t* out );

// Hardware Y-modulo ring, no mirrored copy.  Emits `outputs` results, stepping the window by
// `decim_bytes` between them, and returns the updated window start.
extern int32_t* fir_ring_q31_ymod_block( const int32_t* coeff, const int32_t* hist, uint32_t taps,
                                         int32_t* out, uint32_t outputs, uint32_t decim_bytes,
                                         const int32_t* ring, uint32_t ring_bytes );

// Same, with Y modulo only and X modulo off, so it cannot fold a preempting context's ordinary loads
// into its own ring.  The coefficient pointer is reloaded per output instead of rewinding itself,
// which also frees the coefficients from the modulo start-address rules -- i.e. lets them be in flash.
extern int32_t* fir_ring_q31_ymod_yonly_block( const int32_t* coeff, const int32_t* hist,
                                               uint32_t taps, int32_t* out, uint32_t outputs,
                                               uint32_t decim_bytes, const int32_t* ring,
                                               uint32_t ring_bytes );

// MEASUREMENT ONLY, src/app/apps/asrc/asrc_fir_hb_kernel_dspic33ak.s (that file's header is the
// specification).  Same shape as the yonly kernel above, but for a HALF-BAND prototype: it walks
// only the non-zero taps, so the Y pointer's stride is 2 samples over most of the run.  `half`
// replaces `taps`; the filter length is 4*half - 1 and coeff[] holds its 2*half+1 non-zero taps in
// ascending index order.  Nothing on the audio path calls it.
extern int32_t* fir_ring_q31_hb_ymod_yonly_block( const int32_t* coeff, const int32_t* hist,
                                                  uint32_t half, int32_t* out, uint32_t outputs,
                                                  uint32_t decim_bytes, const int32_t* ring,
                                                  uint32_t ring_bytes );

// float32, eight channels per pass, frame-major history: 17 instructions + one DTB per 8 MACs.
extern void fir_ring_wide8_f32( const float* hist, const float* coeff, uint32_t taps, float* out8 );

/* Project-owned DF2T scheduling variants.  They deliberately share the
 * vendor-facing instance ABI and are already part of this configuration; the
 * Phase-3 probe only calls them, it does not add any assembler. */
extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
    const mchp_biquad_cascade_df2T_instance_f32* s,
    const float32_t* src, float32_t* dst, uint32_t block_size );

// ---- geometry ----------------------------------------------------------------------------------
// 107 and 190 taps are the two shipping cases the report costs out: 107 is the AK128 `/2` front end,
// 190 the AK512 16ch `/6` one.  The slope between them is the per-MAC number.
#define FIRB_TAPS_LO        107u
#define FIRB_TAPS_HI        190u
#define FIRB_TAPS_SPAN      ( FIRB_TAPS_HI - FIRB_TAPS_LO )
#define FIRB_WIDE           8u                          /* channels per float pass                */
#define FIRB_RING_BYTES     ( FIRB_TAPS_HI * 4u )       /* 760 -- deliberately NOT a power of two */
#define FIRB_RING_P2_BYTES  1024u                       /* the safe-under-either-reading size     */

// ---- where the operands live --------------------------------------------------------------------
// The 1 cycle/MAC claim rests entirely on the two MAC operands arriving over different buses, so
// placement IS the experiment.  Two hard constraints shape how it is done here, and both are
// findings in their own right rather than implementation noise:
//
//  * X data space is 0x4000..0xBFFF and Y is 0xC000..0x13FFF.  The 16-channel BIDIRECTIONAL profile
//    fills X completely -- 2 bytes free, one BSS object running across the boundary -- so this bench
//    cannot run there at all, and neither can a Q31 front end: Q31 needs its coefficients in X and
//    that image has no X space to put them in.  Measured on the one-way profile, which frees an
//    engine and leaves X with room.
//  * A DECLARED object in Y space breaks the serial-update layout gate either way it is placed.  A
//    floating space(ymemory) object lands at the top of Y, between the stack and the reset-diagnostic
//    block; a fixed address near the base of Y leaves a hole that the stack is given instead.  Both
//    stop the stack from ending exactly at the diagnostic block, which the gate in buildtools/build.ps1
//    correctly refuses -- above that block the stack would overwrite the trap record on its way to
//    reporting it.
//
// So the Y-side operands are NOT declared.  They are addressed directly in the top 3.5 KiB of the
// region the linker already gave the stack, immediately below the diagnostic block.  Nothing is added
// to the image layout -- it is byte-identical to a build without this bench -- and the stack would
// have to grow past 18 KiB to reach it, which the run-time check below reports rather than assumes.
// This is a measurement device and is deliberately not how shipping code should get Y storage: a
// shipping Q31 front end must own its Y history properly, which means the gate and the linker script
// have to be taught about it.
#define FIRB_Y_ARENA      0x11000u                      /* 2048-aligned; 0x11000..0x13A00          */
#define FIRB_RING_POOL    ( (int32_t*)FIRB_Y_ARENA )                /* 2048 B: modulo ring arena   */
#define FIRB_HIST_Y       ( (int32_t*)( FIRB_Y_ARENA + 0x0800u ) )  /* 1520 B: mirrored history    */
#define FIRB_FCOEFF       ( (float*)( FIRB_Y_ARENA + 0x0E00u ) )    /*  760 B: float coefficients  */
#define FIRB_FHIST        ( (float*)( FIRB_Y_ARENA + 0x1100u ) )    /* 6080 B: frame-major float   */

_Static_assert( FIRB_Y_ARENA >= 0xC000u, "the Y scratch arena is not in Y data space" );
_Static_assert( ( FIRB_Y_ARENA + 0x1100u + ( 190u * 8u * 4u ) ) <= 0x13E00u,
                "the Y scratch arena runs into the reset-diagnostic block" );

// The coefficients are the one thing that MUST be in X, and space(xmemory) is what forces it.  Left to
// the best-fit allocator they land wherever there is room, which on this application means Y -- and the
// measurement would then silently report the both-operands-in-one-space figure instead of the split
// one.  X is 32 KiB and this application needs 37 KiB of data, so X is full either way; what differs is
// that the application's own .bss may sit anywhere and this may not, so asking for X moves 1 KiB of
// application data into Y rather than failing.  1024-aligned because the Y-modulo kernel runs an
// X-modulo over these, so their start address is a variable of the same experiment.
//
// The float kernel needs no split at all -- mac.s takes register operands, so both sides arrive by
// mov.l through the X AGU whatever the address -- so its buffers live in the Y scratch above, where
// they cost the X space nothing.
static int32_t firb_coeff[256] __attribute__((space(xmemory), aligned(1024)));

// ---- the same coefficients, but resident in PROGRAM FLASH -----------------------------------------
// The plan wants ~844 B of X RAM for the front-stage coefficients, and the shipping 16ch profile has
// none to give.  Before moving 16.5 KiB of polyphase table out of RAM to make room, ask the cheaper
// question: does the X-side MAC operand have to be RAM at all?
//
// On dsPIC33A a plain `const` lands in program flash with no startup copy (same mechanism the
// polyphase flash table uses), and 4.3.16 notes that X space "also provides the pointers into program
// space".  So MAC.l [w0]+=4 may well prefetch from flash -- at whatever flash access cost the core
// charges, which is exactly what has to be measured.  If it lands under the 1.5 cycles/MAC that 16
// channels need, the X RAM requirement disappears and nothing else has to move.
//
// Values are the same shape firb_fill_coeff() generates for 190 taps (parabolic envelope, symmetric
// sign pattern, sum|h| = 0.5), emitted as literals because a flash array cannot be filled at run
// time.  The 107-tap measurement uses the first 107 of them; the reference reads the same array, so
// the correctness check still means something.
static const int32_t firb_coeff_flash[FIRB_TAPS_HI] = {
         175677,      349506,      521485,      691615,      859895,    -1026327,
       -1190909,    -1353642,    -1514526,    -1673560,     1830746,     1986082,
        2139568,     2291206,     2440994,    -2588933,    -2735023,    -2879264,
       -3021655,    -3162197,     3300890,     3437734,     3572728,     3705873,
        3837169,    -3966616,    -4094213,    -4219962,    -4343861,    -4465910,
        4586111,     4704462,     4820964,     4935617,     5048421,    -5159375,
       -5268480,    -5375736,    -5481142,    -5584700,     5686408,     5786267,
        5884276,     5980437,     6074748,    -6167210,    -6257822,    -6346586,
       -6433500,    -6518565,     6601781,     6683147,     6762665,     6840333,
        6916151,    -6990121,    -7062241,    -7132512,    -7200934,    -7267507,
        7332230,     7395104,     7456129,     7515305,     7572631,    -7628108,
       -7681736,    -7733515,    -7783444,    -7831524,     7877755,     7922137,
        7964670,     8005353,     8044187,    -8081172,    -8116307,    -8149593,
       -8181030,    -8210618,     8238357,     8264246,     8288286,     8310477,
        8330819,    -8349311,    -8365954,    -8380748,    -8393693,    -8404788,
        8414035,     8421431,     8426979,     8430678,     8432527,     8432527,
        8430678,     8426979,     8421431,    -8414035,    -8404788,    -8393693,
       -8380748,    -8365954,     8349311,     8330819,     8310477,     8288286,
        8264246,    -8238357,    -8210618,    -8181030,    -8149593,    -8116307,
        8081172,     8044187,     8005353,     7964670,     7922137,    -7877755,
       -7831524,    -7783444,    -7733515,    -7681736,     7628108,     7572631,
        7515305,     7456129,     7395104,    -7332230,    -7267507,    -7200934,
       -7132512,    -7062241,     6990121,     6916151,     6840333,     6762665,
        6683147,    -6601781,    -6518565,    -6433500,    -6346586,    -6257822,
        6167210,     6074748,     5980437,     5884276,     5786267,    -5686408,
       -5584700,    -5481142,    -5375736,    -5268480,     5159375,     5048421,
        4935617,     4820964,     4704462,    -4586111,    -4465910,    -4343861,
       -4219962,    -4094213,     3966616,     3837169,     3705873,     3572728,
        3437734,    -3300890,    -3162197,    -3021655,    -2879264,    -2735023,
        2588933,     2440994,     2291206,     2139568,     1986082,    -1830746,
       -1673560,    -1514526,    -1353642,    -1190909,     1026327,      859895,
         691615,      521485,      349506,     -175677,
};

static int32_t firb_out[FIRB_WIDE * 4u];
static float   firb_fout[FIRB_WIDE];

// ---- test vectors ------------------------------------------------------------------------------
// No libm: a parabolic envelope with a symmetric sign pattern is symmetric (h[i] == h[taps-1-i]),
// has the sign changes a real lowpass has, and needs no cosf().  Scaled so that sum|h| = 0.5, which
// bounds the Q1.63 accumulator at 2^62 and so lets the int64 reference below stand in for the 72-bit
// accumulator without emulating it.
static void firb_fill_coeff( uint32_t taps )
{
    float shape[FIRB_TAPS_HI];
    const uint32_t centre = ( taps - 1u ) / 2u;
    float sum = 0.0f;

    for( uint32_t i = 0u; i < taps; i++ )
    {
        const uint32_t d = ( i < centre ) ? ( centre - i ) : ( i - centre );
        const float    env = (float)( ( i + 1u ) * ( taps - i ) );
        shape[i] = ( ( ( d / 5u ) & 1u ) != 0u ) ? -env : env;
        sum += ( shape[i] < 0.0f ) ? -shape[i] : shape[i];
    }

    const float scale = 0.5f / sum;
    for( uint32_t i = 0u; i < taps; i++ )
    {
        const float h = shape[i] * scale;
        firb_coeff[i] = (int32_t)( h * 2147483648.0f );   /* Q31 */
    }
}

// Deterministic full-scale samples.  A ring is filled the same way regardless of where it starts, so
// a wrap that reads the wrong element cannot accidentally read an equal value.
static void firb_fill_samples( int32_t* dst, uint32_t n, uint32_t seed )
{
    uint32_t s = seed | 1u;
    for( uint32_t i = 0u; i < n; i++ )
    {
        s = ( s * 1664525u ) + 1013904223u;
        dst[i] = (int32_t)s;
    }
}

// The 72-bit accumulator, in int64: a fractional MAC of two Q31 values adds the product shifted left
// by one (Q2.62 -> Q1.63), and sacr.l rounds at bit 31 and stores bits 63:32.  Valid only while the
// running sum stays inside +-2^62, which the coefficient scaling above guarantees.
static int32_t firb_ref_q31( const int32_t* coeff, const int32_t* hist, uint32_t taps )
{
    int64_t acc = 0;
    for( uint32_t k = 0u; k < taps; k++ )
    {
        acc += ( (int64_t)coeff[k] * (int64_t)hist[k] ) * 2;
    }
    acc += 0x80000000LL;
    return (int32_t)( acc >> 32 );
}

// Same, but reading the history through a modulo ring.  This function DEFINES the wrap the hardware
// is being asked to perform, so comparing the kernel against it is the M6 test itself.
static int32_t firb_ref_q31_ring( const int32_t* coeff, const int32_t* ring,
                                  uint32_t ring_entries, uint32_t start_idx, uint32_t taps )
{
    int64_t acc = 0;
    for( uint32_t k = 0u; k < taps; k++ )
    {
        acc += ( (int64_t)coeff[k] * (int64_t)ring[( start_idx + k ) % ring_entries] ) * 2;
    }
    acc += 0x80000000LL;
    return (int32_t)( acc >> 32 );
}

// float views of the same arenas.  The Q31 and float measurements never run at the same time, so they
// share storage and each refills what it needs immediately before measuring.

static void firb_fill_float( uint32_t taps )
{
    float* const c = FIRB_FCOEFF;
    float* const h = FIRB_FHIST;
    for( uint32_t i = 0u; i < taps; i++ )
    {
        c[i] = (float)firb_coeff[i] * ( 1.0f / 2147483648.0f );
    }
    uint32_t s = 0x1234567u;
    for( uint32_t i = 0u; i < ( taps * FIRB_WIDE ); i++ )
    {
        s = ( s * 1664525u ) + 1013904223u;
        h[i] = (float)(int32_t)s * ( 1.0f / 2147483648.0f );
    }
}

static void firb_ref_wide8( uint32_t taps, float* out8 )
{
    const float* const c = FIRB_FCOEFF;
    const float* const h = FIRB_FHIST;
    for( uint32_t ch = 0u; ch < FIRB_WIDE; ch++ ) { out8[ch] = 0.0f; }
    for( uint32_t m = 0u; m < taps; m++ )
    {
        for( uint32_t ch = 0u; ch < FIRB_WIDE; ch++ )
        {
            out8[ch] += c[m] * h[( m * FIRB_WIDE ) + ch];
        }
    }
}

// ---- timing ------------------------------------------------------------------------------------
// One count is one instruction cycle (Timer2 at FCY, 1:1).  `min` over the trials with `near`
// counting the trials within 1/16 of it: with interrupts masked below, near should be essentially all
// of them, and a low near means something else is stealing cycles (DMA) rather than an ISR.
//
// Interrupts are masked around each individual timed call, for two separate reasons:
//
//  * It makes the reading an instruction count rather than a sample of a distribution.
//  * MODCON, XMODSRT/XMODEND and YMODSRT/YMODEND are NOT part of the per-IPL register context.
//    Table 4-2 lists W0-W7, ACCA/ACCB, RCOUNT and CORCON, and describes the Modulo Addressing control
//    registers as separate from the programmer's model.  So while the Y-modulo kernel runs with
//    XMODEN/YMODEN set, an ISR that preempts it and uses W0 or W1 as a pointer has ITS OWN accesses
//    folded into this kernel's ring.  That is a real hazard for running a modulo kernel in one context
//    while other contexts run, and the accumulator/RCOUNT/CORCON banking that closes the rest of the
//    ISR-safety question does not cover it.  The mirrored kernel programs no modulo at all and so has
//    no such exposure -- a reason to prefer it that has nothing to do with speed.
//
// One masked window is one kernel call: ~1-2 us at 200 MHz against the 84.9 us of TDM margin this
// build reports, so the audio path does not notice.  Masking the whole trial loop would not be safe.
#define FIRB_MEASURE( stmt, out_min, out_near )                                     \
    do {                                                                            \
        uint32_t firb_best = 0xFFFFFFFFu, firb_near = 0u;                           \
        for( uint32_t firb_t = 0u; firb_t < trials; firb_t++ )                      \
        {                                                                           \
            __builtin_disable_interrupts();                                         \
            const uint32_t firb_t0 = nora_high_res_timer_get_count();               \
            stmt;                                                                   \
            const uint32_t firb_d = nora_high_res_timer_get_count() - firb_t0;      \
            __builtin_enable_interrupts();                                          \
            if( firb_d < firb_best ) { firb_best = firb_d; firb_near = 0u; }         \
            if( firb_d <= ( firb_best + ( firb_best >> 4 ) ) ) { firb_near++; }      \
        }                                                                           \
        ( out_min ) = firb_best;                                                    \
        ( out_near ) = firb_near;                                                    \
    } while( 0 )

// ONE TIMER TICK IS TWO INSTRUCTION CYCLES on this core, and getting that wrong halves every number
// below.  The project spells PLL1_CLK_HZ = 200 MHz and FCY = PLL1_CLK_HZ / 2 = 100 MHz, labelling FCY
// the "instruction-cycle frequency" -- which is the classic dsPIC 2-clocks-per-instruction model.
// dsPIC33A is not that core: 4.3 states the CPU "can issue ... no more than one instruction per clock
// cycle", so the instruction rate is the full 200 MHz while the timer, clocked from FCY, ticks at
// 100 MHz.  Two independent checks pin it down rather than one assumption:
//
//   * The mirrored kernel issues taps+9 instructions -- 199 for 190 taps -- and cannot beat one per
//     cycle.  It measures 113 ticks.  199 instructions cannot fit in 113 ticks unless a tick is more
//     than one cycle, and 2 is the only ratio the clock tree offers.
//   * The existing telemetry converts these same ticks to microseconds against FCY and lands on the
//     333.3 us TDM window, which is 16 frames at 48 kHz.  So the tick really is 100 MHz, and it is the
//     instruction rate that is 200 MHz.
//
// Derived from the two constants rather than written as 2, so a clock-tree change cannot leave this
// silently stale.
#define FIRB_CYC_PER_TICK   ( PLL1_CLK_HZ / FCY )

// Per-MAC cost as a slope, x1000.  Cancels the call, the prologue, the timer read and the loop setup
// instead of assuming they are negligible.  `macs_span` is the MAC count difference between the two
// measurements, not the tap count difference -- they differ by the channel width.
static uint32_t firb_slope_x1000( uint32_t cyc_lo, uint32_t cyc_hi, uint32_t macs_span )
{
    if( ( cyc_hi <= cyc_lo ) || ( macs_span == 0u ) ) { return 0u; }
    return (uint32_t)( ( (uint64_t)( cyc_hi - cyc_lo ) * FIRB_CYC_PER_TICK * 1000u ) / macs_span );
}

// ---- the bench ---------------------------------------------------------------------------------
void asrc_fir_kernel_bench_run( uint32_t trials )
{
    if( trials == 0u ) { trials = 2000u; }

    uint32_t cyc_lo = 0u, cyc_hi = 0u, near_lo = 0u, near_hi = 0u, ovh = 0u, ovh_near = 0u;
    int32_t  ref;

    printf( "\n *aq FIR kernel bench  CPU=%luMHz  timer=%luMHz  1 tick = %lu cycles  trials=%lu\n",
            (unsigned long)( PLL1_CLK_HZ / 1000000UL ), (unsigned long)( FCY / 1000000UL ),
            (unsigned long)FIRB_CYC_PER_TICK, (unsigned long)trials );

    // Placement is not cosmetic here: the whole 1 cycle/MAC claim rests on the two MAC operands
    // coming from different spaces, and space(ymemory) placement is a linker outcome, not a promise.
    // Y data space is 0xC000..0x14000 on this device.
    printf( "    placement: coeff=0x%05lx (X)  histY=0x%05lx  ringY=0x%05lx  fhist=0x%05lx  %s\n",
            (unsigned long)(uintptr_t)&firb_coeff[0],
            (unsigned long)(uintptr_t)FIRB_HIST_Y,
            (unsigned long)(uintptr_t)FIRB_RING_POOL,
            (unsigned long)(uintptr_t)FIRB_FHIST,
            ( ( (uintptr_t)&firb_coeff[0] <  0xC000u ) &&
              ( (uintptr_t)FIRB_HIST_Y    >= 0xC000u ) &&
              ( (uintptr_t)FIRB_RING_POOL >= 0xC000u ) ) ? "X/Y split OK" : "*** NOT SPLIT ***" );

    // The Y scratch is addressed inside the stack's region, so report how much room stands between the
    // live stack and it.  A report, not an assumption: if this ever goes small, the bench is unsafe.
    uint32_t sp_probe = 0u;
    printf( "    Y scratch 0x%05lx..0x13A00, stack now near 0x%05lx (%lu B headroom)  taps %lu/%lu\n",
            (unsigned long)FIRB_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe,
            (unsigned long)( FIRB_Y_ARENA - (uintptr_t)&sp_probe ),
            (unsigned long)FIRB_TAPS_LO, (unsigned long)FIRB_TAPS_HI );

    if( ( FIRB_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u )
    {
        // Refuse rather than measure.  With less than 4 KiB between the live stack and the scratch, a
        // deep call inside this bench overwrites the very ring it is timing, and the failure mode is a
        // plausible-looking number rather than a crash.  This is not hypothetical: the 16-channel
        // BIDIRECTIONAL profile's data reaches 0x1064C, leaving only ~2.5 KiB below the arena, so *aq
        // must not be run there until FIRB_Y_ARENA is made profile-aware.
        printf( "    REFUSING to run: only %lu B between the stack and the Y scratch (4096 needed).\n"
                "    Use a profile whose data ends lower, or move FIRB_Y_ARENA down.\n",
                (unsigned long)( FIRB_Y_ARENA - (uintptr_t)&sp_probe ) );
        return;
    }

    // What one timer read pair costs, so the absolute per-call numbers can be read as kernel cost.
    // The slopes do not need it; it is printed so the fixed overhead is not a mystery.
    FIRB_MEASURE( (void)0, ovh, ovh_near );
    printf( "    timer read pair: %lu tk (near=%lu)\n",
            (unsigned long)ovh, (unsigned long)ovh_near );

    // ---- M1: Q31 mirrored ring, coefficients in X, history in Y --------------------------------
    firb_fill_samples( FIRB_HIST_Y, 2u * FIRB_TAPS_HI, 0xA5A5u );
    firb_fill_coeff( FIRB_TAPS_LO );
    FIRB_MEASURE( fir_ring_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_LO, firb_out ), cyc_lo, near_lo );
    ref = firb_ref_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_LO );
    const int32_t err_lo = firb_out[0] - ref;

    firb_fill_coeff( FIRB_TAPS_HI );
    FIRB_MEASURE( fir_ring_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_HI, firb_out ), cyc_hi, near_hi );
    ref = firb_ref_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_HI );
    const int32_t err_hi = firb_out[0] - ref;

    printf( "  [1] q31 mirrored  X coeff / Y hist\n"
            "      %3lutap %4lu tk (near=%lu)   %3lutap %4lu tk (near=%lu)\n"
            "      => %lu.%03lu cycles/MAC (slope)   err vs ref: %ld / %ld LSB\n",
            (unsigned long)FIRB_TAPS_LO, (unsigned long)cyc_lo, (unsigned long)near_lo,
            (unsigned long)FIRB_TAPS_HI, (unsigned long)cyc_hi, (unsigned long)near_hi,
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) / 1000u ),
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) % 1000u ),
            (long)err_lo, (long)err_hi );

    // ---- M1b: the same kernel with BOTH operands in X space ------------------------------------
    // 4.3.17 charges "typically one cycle" extra per MAC when the two reads cannot go down the X and
    // Y buses concurrently.  There is no functional symptom, so this is measured deliberately: it is
    // the number that says how much a misplaced history costs whoever wires this up later.  The
    // coefficient buffer stands in as its own history: only the timing is wanted, and that way the
    // probe needs no second X-space buffer -- which is exactly what a full X space cannot spare.
    firb_fill_coeff( FIRB_TAPS_LO );
    FIRB_MEASURE( fir_ring_q31( firb_coeff, firb_coeff, FIRB_TAPS_LO, firb_out ), cyc_lo, near_lo );
    firb_fill_coeff( FIRB_TAPS_HI );
    FIRB_MEASURE( fir_ring_q31( firb_coeff, firb_coeff, FIRB_TAPS_HI, firb_out ), cyc_hi, near_hi );
    printf( "  [2] q31 mirrored  X coeff / X hist (the silent misplacement)\n"
            "      %3lutap %4lu tk    %3lutap %4lu tk    => %lu.%03lu cycles/MAC\n",
            (unsigned long)FIRB_TAPS_LO, (unsigned long)cyc_lo,
            (unsigned long)FIRB_TAPS_HI, (unsigned long)cyc_hi,
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) / 1000u ),
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) % 1000u ) );

    // ---- M1c: Q31 over a HARDWARE Y-modulo ring, no mirrored copy -------------------------------
    // Power-of-two ring first (1024 B, 1024-aligned): legal under either reading of 4.3.18, so this
    // separates "does the modulo path keep 1 cycle/MAC" from "which start addresses are legal", which
    // the M6 sweep below asks separately.  The start index is mid-ring so every pass really wraps.
    const uint32_t p2_entries = FIRB_RING_P2_BYTES / 4u;
    const uint32_t p2_start   = p2_entries / 2u;
    firb_fill_samples( FIRB_RING_POOL, p2_entries, 0x5A5Au );

    firb_fill_coeff( FIRB_TAPS_LO );
    FIRB_MEASURE( (void)fir_ring_q31_ymod_block( firb_coeff, &FIRB_RING_POOL[p2_start], FIRB_TAPS_LO,
                                                 firb_out, 1u, 4u,
                                                 FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  cyc_lo, near_lo );
    const int32_t ym_err_lo = firb_out[0] -
                              firb_ref_q31_ring( firb_coeff, FIRB_RING_POOL, p2_entries, p2_start, FIRB_TAPS_LO );

    firb_fill_coeff( FIRB_TAPS_HI );
    FIRB_MEASURE( (void)fir_ring_q31_ymod_block( firb_coeff, &FIRB_RING_POOL[p2_start], FIRB_TAPS_HI,
                                                 firb_out, 1u, 4u,
                                                 FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  cyc_hi, near_hi );
    const int32_t ym_err_hi = firb_out[0] -
                              firb_ref_q31_ring( firb_coeff, FIRB_RING_POOL, p2_entries, p2_start, FIRB_TAPS_HI );

    // outputs=8 at the same tap count isolates the PER-OUTPUT cost, which cancels the 28-instruction
    // SFR setup completely -- this is the number that matters when one call emits a whole block.
    uint32_t cyc_o8 = 0u, near_o8 = 0u;
    FIRB_MEASURE( (void)fir_ring_q31_ymod_block( firb_coeff, &FIRB_RING_POOL[p2_start], FIRB_TAPS_HI,
                                                 firb_out, 8u, 24u,
                                                 FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  cyc_o8, near_o8 );
    const uint32_t per_out = ( cyc_o8 > cyc_hi ) ? ( ( cyc_o8 - cyc_hi ) / 7u ) : 0u;
    const uint32_t p2_out8  = cyc_o8;   // kept: cyc_o8 is reused by the sweep below

    printf( "  [3] q31 Y-modulo ring (%lu B, power of two, aligned), no mirror\n"
            "      %3lutap %4lu tk (near=%lu)   %3lutap %4lu tk (near=%lu)\n"
            "      => %lu.%03lu cycles/MAC (tap slope)   err vs ref: %ld / %ld LSB\n"
            "      out=8 %4lu tk (near=%lu) => %lu tk/output = %lu.%03lu cycles/MAC (setup cancelled)\n",
            (unsigned long)FIRB_RING_P2_BYTES,
            (unsigned long)FIRB_TAPS_LO, (unsigned long)cyc_lo, (unsigned long)near_lo,
            (unsigned long)FIRB_TAPS_HI, (unsigned long)cyc_hi, (unsigned long)near_hi,
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) / 1000u ),
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN ) % 1000u ),
            (long)ym_err_lo, (long)ym_err_hi,
            (unsigned long)cyc_o8, (unsigned long)near_o8, (unsigned long)per_out,
            (unsigned long)( ( per_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI / 1000u ),
            (unsigned long)( ( ( per_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI ) % 1000u ) );

    // ---- M1d: float32, eight channels per pass -------------------------------------------------
    // Two instructions per MAC is the float floor on this core; the width is what gets it there.
    // The slope is divided by 8 MACs per tap, not by the tap count.
    firb_fill_coeff( FIRB_TAPS_LO );
    firb_fill_float( FIRB_TAPS_LO );
    FIRB_MEASURE( fir_ring_wide8_f32( FIRB_FHIST, FIRB_FCOEFF, FIRB_TAPS_LO, firb_fout ),
                  cyc_lo, near_lo );
    float fref[FIRB_WIDE];
    firb_ref_wide8( FIRB_TAPS_LO, fref );
    float fmax = 0.0f;
    for( uint32_t ch = 0u; ch < FIRB_WIDE; ch++ )
    {
        float d = firb_fout[ch] - fref[ch];
        if( d < 0.0f ) { d = -d; }
        if( d > fmax ) { fmax = d; }
    }

    firb_fill_coeff( FIRB_TAPS_HI );
    firb_fill_float( FIRB_TAPS_HI );
    FIRB_MEASURE( fir_ring_wide8_f32( FIRB_FHIST, FIRB_FCOEFF, FIRB_TAPS_HI, firb_fout ),
                  cyc_hi, near_hi );

    printf( "  [4] float wide8 (frame-major history)\n"
            "      %3lutap %4lu tk (near=%lu)   %3lutap %4lu tk (near=%lu)\n"
            "      => %lu.%03lu cycles/MAC (slope / 8 MAC per tap)   max abs err vs ref: %ld e-9\n",
            (unsigned long)FIRB_TAPS_LO, (unsigned long)cyc_lo, (unsigned long)near_lo,
            (unsigned long)FIRB_TAPS_HI, (unsigned long)cyc_hi, (unsigned long)near_hi,
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN * FIRB_WIDE ) / 1000u ),
            (unsigned long)( firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN * FIRB_WIDE ) % 1000u ),
            (long)( fmax * 1.0e9f ) );

    // ---- M2b: coefficients in PROGRAM FLASH, history still in Y ---------------------------------
    // The decisive number for the X-RAM question.  Same kernel, same Y history, only the X-side
    // operand moves from X RAM to flash.  Compared against [1] and against the 1.5 cycles/MAC that 16
    // channels need: if this passes, the front stage needs no X RAM and the polyphase table can stay
    // where it is.  The reference reads the same flash array, so a wrong or unreadable fetch shows up
    // as an LSB error rather than as a plausible-looking cycle count.
    printf( "  [6] q31 mirrored  FLASH coeff (0x%06lx) / Y hist   -- the X-RAM question\n",
            (unsigned long)(uintptr_t)&firb_coeff_flash[0] );
    firb_fill_samples( FIRB_HIST_Y, 2u * FIRB_TAPS_HI, 0xA5A5u );
    FIRB_MEASURE( fir_ring_q31( firb_coeff_flash, FIRB_HIST_Y, FIRB_TAPS_LO, firb_out ),
                  cyc_lo, near_lo );
    const int32_t fl_err_lo = firb_out[0] -
                              firb_ref_q31( firb_coeff_flash, FIRB_HIST_Y, FIRB_TAPS_LO );
    FIRB_MEASURE( fir_ring_q31( firb_coeff_flash, FIRB_HIST_Y, FIRB_TAPS_HI, firb_out ),
                  cyc_hi, near_hi );
    const int32_t fl_err_hi = firb_out[0] -
                              firb_ref_q31( firb_coeff_flash, FIRB_HIST_Y, FIRB_TAPS_HI );
    const uint32_t fl_slope = firb_slope_x1000( cyc_lo, cyc_hi, FIRB_TAPS_SPAN );
    printf( "      %3lutap %4lu tk (near=%lu)   %3lutap %4lu tk (near=%lu)\n"
            "      => %lu.%03lu cycles/MAC (slope)   err vs ref: %ld / %ld LSB   16ch bar 1.500: %s\n",
            (unsigned long)FIRB_TAPS_LO, (unsigned long)cyc_lo, (unsigned long)near_lo,
            (unsigned long)FIRB_TAPS_HI, (unsigned long)cyc_hi, (unsigned long)near_hi,
            (unsigned long)( fl_slope / 1000u ), (unsigned long)( fl_slope % 1000u ),
            (long)fl_err_lo, (long)fl_err_hi,
            ( ( fl_slope != 0u ) && ( fl_slope < 1500u ) ) ? "PASS" : "FAIL" );

    // ---- M10: Y modulo ONLY, and then Y-only with the coefficients in flash ----------------------
    // The X modulo in [3] is what makes the ring unsafe next to other contexts; Y modulo alone cannot
    // touch ordinary code (4.3.16: the Y AGU serves the DSP MAC class only).  So this is the variant
    // that keeps the single-copy 12,160 B history without an unwritable convention.  Statically it is
    // 43 instructions against [3]'s 50 -- seven fewer fixed, one more per output -- so it should be
    // cheaper than [3] for any block of fewer than eight outputs and equal at eight.
    //
    // Then the same kernel with the coefficients in FLASH.  Only Y-only can express that: with X
    // modulo off the coefficients no longer have to satisfy the modulo start-address rules.  If this
    // one passes 1.5 cycles/MAC, the front stage needs neither X RAM for coefficients nor the
    // polyphase table moved out of RAM.
    uint32_t yo1 = 0u, yo8 = 0u, yon1 = 0u, yon8 = 0u, yf8 = 0u, yfn8 = 0u;
    firb_fill_samples( FIRB_RING_POOL, p2_entries, 0x5A5Au );
    firb_fill_coeff( FIRB_TAPS_HI );

    FIRB_MEASURE( (void)fir_ring_q31_ymod_yonly_block( firb_coeff, &FIRB_RING_POOL[p2_start],
                                                       FIRB_TAPS_HI, firb_out, 1u, 4u,
                                                       FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  yo1, yon1 );
    const int32_t yo_err = firb_out[0] -
                           firb_ref_q31_ring( firb_coeff, FIRB_RING_POOL, p2_entries,
                                              p2_start, FIRB_TAPS_HI );
    FIRB_MEASURE( (void)fir_ring_q31_ymod_yonly_block( firb_coeff, &FIRB_RING_POOL[p2_start],
                                                       FIRB_TAPS_HI, firb_out, 8u, 24u,
                                                       FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  yo8, yon8 );
    const uint32_t yo_out = ( yo8 > yo1 ) ? ( ( yo8 - yo1 ) / 7u ) : 0u;

    FIRB_MEASURE( (void)fir_ring_q31_ymod_yonly_block( firb_coeff_flash, &FIRB_RING_POOL[p2_start],
                                                       FIRB_TAPS_HI, firb_out, 8u, 24u,
                                                       FIRB_RING_POOL, FIRB_RING_P2_BYTES ),
                  yf8, yfn8 );
    const int32_t yf_err = firb_out[0] -
                           firb_ref_q31_ring( firb_coeff_flash, FIRB_RING_POOL, p2_entries,
                                              p2_start, FIRB_TAPS_HI );
    const uint32_t yf_out = ( yf8 > yo1 ) ? ( ( yf8 - yo1 ) / 7u ) : 0u;

    printf( "  [7] q31 Y-modulo ONLY (X modulo off), X coeff, %lutap\n"
            "      out=1 %4lu tk (near=%lu)   out=8 %4lu tk (near=%lu)\n"
            "      => %lu tk/output = %lu.%03lu cycles/MAC   err vs ref: %ld LSB\n"
            "  [8] q31 Y-modulo ONLY, FLASH coeff, %lutap   -- the minimal-change candidate\n"
            "      out=8 %4lu tk (near=%lu) => %lu tk/output = %lu.%03lu cycles/MAC"
            "   err %ld LSB   16ch bar 1.500: %s\n",
            (unsigned long)FIRB_TAPS_HI,
            (unsigned long)yo1, (unsigned long)yon1, (unsigned long)yo8, (unsigned long)yon8,
            (unsigned long)yo_out,
            (unsigned long)( ( yo_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI / 1000u ),
            (unsigned long)( ( ( yo_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI ) % 1000u ),
            (long)yo_err,
            (unsigned long)FIRB_TAPS_HI,
            (unsigned long)yf8, (unsigned long)yfn8, (unsigned long)yf_out,
            (unsigned long)( ( yf_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI / 1000u ),
            (unsigned long)( ( ( yf_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI ) % 1000u ),
            (long)yf_err,
            ( ( yf_out != 0u ) &&
              ( ( ( yf_out * FIRB_CYC_PER_TICK * 1000u ) / FIRB_TAPS_HI ) < 1500u ) ) ? "PASS" : "FAIL" );

    // ---- M6: which modulo ring START ADDRESSES are legal ----------------------------------------
    // 4.3.18 says an incrementing buffer has "certain restrictions on the buffer start address",
    // names power-of-two length as the exception that lifts them, and gives no numeric rule.  So ask
    // the hardware: place the SAME ring length at a range of start addresses and check the wrap.  The
    // reference computes the wrap in C, so a row that fails has read the wrong element, which is the
    // only thing that matters.  Every row starts mid-ring, so the wrap is exercised, not skipped.
    //
    // What the answer buys: a 190-tap 16ch history is 12,160 B if a 760 B ring is legal where it can
    // be placed, and 16,384 B if it has to be padded to a power of two.
    static const uint16_t m6_bytes[]  = { 760u, 760u, 760u, 760u, 760u, 760u,  760u,  760u,
                                          1024u, 1024u, 1024u };
    static const uint16_t m6_offset[] = {   0u,   4u,   8u,  16u,  64u, 256u,  512u, 1020u,
                                             0u,    4u,  512u };
    uint32_t m6_pass = 0u, m6_fail = 0u;

    printf( "  [5] M6 modulo start-address sweep (taps=%lu, every row wraps)\n",
            (unsigned long)FIRB_TAPS_HI );
    for( uint32_t r = 0u; r < ( sizeof( m6_bytes ) / sizeof( m6_bytes[0] ) ); r++ )
    {
        const uint32_t bytes   = m6_bytes[r];
        const uint32_t entries = bytes / 4u;
        int32_t* const ring    = &FIRB_RING_POOL[m6_offset[r] / 4u];
        const uint32_t start   = entries / 2u;

        firb_fill_samples( ring, entries, 0x3C3Cu + r );
        firb_out[0] = 0;
        __builtin_disable_interrupts();          /* MODCON is not context-banked -- see above */
        (void)fir_ring_q31_ymod_block( firb_coeff, &ring[start], FIRB_TAPS_HI,
                                       firb_out, 1u, 4u, ring, bytes );
        __builtin_enable_interrupts();
        const int32_t d = firb_out[0] - firb_ref_q31_ring( firb_coeff, ring, entries, start, FIRB_TAPS_HI );
        const bool ok = ( d >= -4 ) && ( d <= 4 );
        if( ok ) { m6_pass++; } else { m6_fail++; }

        // The absolute address is printed because the answer is expected to be an alignment rule, and
        // an alignment rule is only readable off the absolute address.
        printf( "      %4lu B @ 0x%05lx (%lu-align) start=%3lu  err=%+8ld  %s\n",
                (unsigned long)bytes, (unsigned long)(uintptr_t)ring,
                (unsigned long)( (uintptr_t)ring & 2047u ),
                (unsigned long)start, (long)d, ok ? "pass" : "FAIL" );
    }

    // Speed of a non-power-of-two ring, for the case it turns out to be legal: the saving is only
    // worth taking if it costs nothing.
    firb_fill_samples( FIRB_RING_POOL, FIRB_TAPS_HI, 0x7788u );
    FIRB_MEASURE( (void)fir_ring_q31_ymod_block( firb_coeff, &FIRB_RING_POOL[FIRB_TAPS_HI / 2u],
                                                 FIRB_TAPS_HI, firb_out, 8u, 24u,
                                                 FIRB_RING_POOL, FIRB_RING_BYTES ),
                  cyc_o8, near_o8 );
    printf( "      %lu B ring, out=8: %lu tk (near=%lu) vs %lu B %lu tk -> %s\n",
            (unsigned long)FIRB_RING_BYTES, (unsigned long)cyc_o8, (unsigned long)near_o8,
            (unsigned long)FIRB_RING_P2_BYTES, (unsigned long)p2_out8,
            ( m6_fail == 0u ) ? "non-power-of-two ring usable" : "see the sweep" );

    printf( "    M6: %lu pass / %lu FAIL\n", (unsigned long)m6_pass, (unsigned long)m6_fail );
}

#if ASRC_H2_KERNEL_BENCH_AVAILABLE
/* ---- Phase 2: H2 component timing --------------------------------------------------------------
 *
 * This deliberately remains a foreground microbenchmark.  It has exactly the
 * candidate's 16 x 16 workload, but it neither reads from nor writes to the
 * live ASRC engines.  The FIR is the project-owned wide8 implementation.  The
 * The Phase-2 vendor DF2T attempt faulted because it bypassed this library's
 * required instance initializer.  Therefore the timing below deliberately
 * remains ordinary local float C: it is the conventional baseline.  Phase 3
 * separately retests existing primitives with the documented initializer.
 */
#define H2B_CHANNELS          (16u)
#define H2B_BLOCK_FRAMES      (16u)
#define H2B_WIDE               (8u)
#define H2B_FIR_HISTORY_FRAMES (2u * ASRC_H2_FIR49_TAPS)
#define H2B_FIR_HISTORY_GROUP_FLOATS (H2B_FIR_HISTORY_FRAMES * H2B_WIDE)
#define H2B_FIR_HISTORY_FLOATS (2u * H2B_FIR_HISTORY_GROUP_FLOATS)
#define H2B_IIR_STATE_FLOATS   (H2B_CHANNELS * ASRC_H2_IIR_SOS * 2u)
#define H2B_DEFAULT_TRIALS    (10000u)
#define H2B_CYC_PER_TICK      (PLL1_CLK_HZ / FCY)
#define H2B_FULL_SCALE        (8388607.0f)  /* float ASRC sample domain: signed 24-bit counts */
#define H2B_TWO_PI            (6.2831853071795864769f)

/* The existing wide8 float FIR requires a contiguous frame-major window.  It
 * uses no modulo registers, so unlike mchp_fir_f32 it remains safe while the
 * IPL4 streaming kernels execute.  Keep its two mirrored eight-channel
 * histories in the same checked Y scratch arena as *aq: the two probes never
 * run concurrently, and this avoids changing the serial-update image's
 * permanent data layout. */
#define H2B_Y_ARENA            (0x12240u)
/* The reset diagnostics begin at 0x13e00.  The Phase-3 probe needs the same
 * 6.9 KiB float history/state arena as Phase 2, but its additional validation
 * is time-multiplexed instead of reserving a second state bank. */
#define H2B_Y_ARENA_LIMIT      (0x13E00u)
#define H2B_FIR_HISTORY        ( (float*)(uintptr_t)H2B_Y_ARENA )
#define H2B_IIR_STATE          ( (float*)(uintptr_t)( H2B_Y_ARENA + ( H2B_FIR_HISTORY_FLOATS * sizeof(float) ) ) )
#define H2B_Y_ARENA_END        ( H2B_Y_ARENA + ( ( H2B_FIR_HISTORY_FLOATS + H2B_IIR_STATE_FLOATS ) * sizeof(float) ) )
#define H2B_OPT_IIR_STATE      H2B_IIR_STATE
#define H2B_OPT_Y_ARENA_END    H2B_Y_ARENA_END
#define H2B_Q31_HISTORY        ( (int32_t*)H2B_FIR_HISTORY )
#define H2B_Q31_HISTORY_WORDS  (2u * ASRC_H2_FIR49_TAPS)
#define H2B_Q31_SCALE          (256.0f)  /* float H2 counts -> signed-24-left Q31 */
#define H2B_Q31_INV_SCALE      (1.0f / H2B_Q31_SCALE)
#define H2B_VALIDATE_TOL_COUNTS (64.0f)

_Static_assert( H2B_CHANNELS == ( 2u * H2B_WIDE ), "H2 FIR groups must cover all channels" );
_Static_assert( H2B_Y_ARENA_END <= H2B_Y_ARENA_LIMIT, "H2 scratch exceeds the checked Y arena" );
_Static_assert( H2B_OPT_Y_ARENA_END <= H2B_Y_ARENA_LIMIT, "H2 optimized IIR state exceeds checked Y arena" );

typedef struct
{
    uint32_t min_ticks;
    uint32_t max_ticks;
    uint64_t sum_ticks;
    uint32_t n;
} h2b_stats_t;

/* The local DF2T uses ordinary pointers.  Its fixed coefficients stay in X
 * and each channel's two-float-per-SOS state stays in the checked Y scratch
 * arena.  The vendor DF2T primitive cannot be used here: on this board it
 * faults in a foreground run while IPL4 streaming remains enabled. */
static float h2b_input[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static float h2b_prefir[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static float h2b_output[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static float h2b_opt_output[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static float h2b_reference[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static float h2b_wide_out[ H2B_WIDE ] __attribute__((space(xmemory)));
static int32_t h2b_q31_input[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static int32_t h2b_q31_prefir[ H2B_CHANNELS ][ H2B_BLOCK_FRAMES ] __attribute__((space(xmemory)));
static int32_t h2b_q31_coeff[ ASRC_H2_FIR49_TAPS ] __attribute__((space(xmemory)));
static mchp_biquad_cascade_df2T_instance_f32 h2b_iir_instance[ H2B_CHANNELS ] __attribute__((space(xmemory)));
static uint32_t h2b_fir_oldest;
static uint32_t h2b_q31_fir_oldest;
static uint32_t h2b_q31_saturations;

static void h2b_stats_reset( h2b_stats_t* s )
{
    s->min_ticks = UINT32_MAX;
    s->max_ticks = 0u;
    s->sum_ticks = 0u;
    s->n = 0u;
}

static void h2b_stats_add( h2b_stats_t* s, uint32_t ticks )
{
    if( ticks < s->min_ticks ) { s->min_ticks = ticks; }
    if( ticks > s->max_ticks ) { s->max_ticks = ticks; }
    s->sum_ticks += ticks;
    s->n++;
}

static uint32_t h2b_stats_mean( const h2b_stats_t* s )
{
    return ( s->n != 0u ) ? (uint32_t)( s->sum_ticks / s->n ) : 0u;
}

static uint32_t h2b_subtract_overhead( uint32_t ticks, uint32_t overhead )
{
    return ( ticks > overhead ) ? ( ticks - overhead ) : 0u;
}

static const char* h2b_data_space( const void* p )
{
    return ( (uintptr_t)p >= 0xC000u ) ? "Y" : "X";
}

static void h2b_filters_reset( void )
{
    memset( H2B_FIR_HISTORY, 0, H2B_FIR_HISTORY_FLOATS * sizeof(float) );
    memset( H2B_IIR_STATE, 0, H2B_IIR_STATE_FLOATS * sizeof(float) );
    h2b_fir_oldest = 0u;
}

/* The shipping generic ASRC is float in this configuration and stores signed
 * 24-bit counts in its float ring.  The existing Q31 FIR instead expects
 * signed-24-left samples.  Keep both boundary conversions explicit: the
 * Phase-3 timing prints them independently and the Q31 combined path includes
 * both, rather than claiming that an isolated FIR timing is a frontend cost. */
static int32_t h2b_count_to_q31( float counts )
{
    const float scaled = counts * H2B_Q31_SCALE;
    if( scaled >= 2147483520.0f ) { return INT32_MAX; }
    if( scaled <= -2147483648.0f ) { return INT32_MIN; }
    return (int32_t)( scaled + ( ( scaled >= 0.0f ) ? 0.5f : -0.5f ) );
}

static int32_t h2b_coeff_to_q31( float coefficient )
{
    const float scaled = coefficient * 2147483648.0f;
    if( scaled >= 2147483520.0f ) { return INT32_MAX; }
    if( scaled <= -2147483648.0f ) { return INT32_MIN; }
    return (int32_t)( scaled + ( ( scaled >= 0.0f ) ? 0.5f : -0.5f ) );
}

static void h2b_q31_prepare_coefficients( void )
{
    for( uint32_t tap = 0u; tap < ASRC_H2_FIR49_TAPS; tap++ )
    {
        h2b_q31_coeff[tap] = h2b_coeff_to_q31( asrc_h2_fir49[tap] );
    }
}

static void h2b_q31_filters_reset( void )
{
    /* The Q31 channel-major mirrored histories deliberately overlay the
     * float-wide FIR history.  The two candidate paths are serial probes, not
     * a simultaneous shipping implementation, so this keeps the probe-local
     * Y scratch within its already checked arena. */
    memset( H2B_Q31_HISTORY, 0,
            H2B_CHANNELS * H2B_Q31_HISTORY_WORDS * sizeof(int32_t) );
    h2b_q31_fir_oldest = 0u;
    h2b_q31_saturations = 0u;
}

static void h2b_float_to_q31_block( void )
{
    for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
    {
        for( uint32_t frame = 0u; frame < H2B_BLOCK_FRAMES; frame++ )
        {
            h2b_q31_input[channel][frame] = h2b_count_to_q31( h2b_input[channel][frame] );
        }
    }
}

static void h2b_q31_fir_only( void )
{
    for( uint32_t frame = 0u; frame < H2B_BLOCK_FRAMES; frame++ )
    {
        for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
        {
            int32_t* const history = &H2B_Q31_HISTORY[channel * H2B_Q31_HISTORY_WORDS];
            int32_t* const write = &history[h2b_q31_fir_oldest];
            const int32_t sample = h2b_q31_input[channel][frame];

            write[0] = sample;
            write[ASRC_H2_FIR49_TAPS] = sample;
            /* The filter is symmetric.  Just as the float-wide path passes
             * write + wide, this passes the contiguous next-oldest..newest
             * window to the existing 1-MAC/cycle Q31 primitive. */
            fir_ring_q31( h2b_q31_coeff, write + 1u, ASRC_H2_FIR49_TAPS,
                          &h2b_q31_prefir[channel][frame] );
            if( ( h2b_q31_prefir[channel][frame] == INT32_MAX ) ||
                ( h2b_q31_prefir[channel][frame] == INT32_MIN ) )
            {
                h2b_q31_saturations++;
            }
        }
        h2b_q31_fir_oldest++;
        if( h2b_q31_fir_oldest == ASRC_H2_FIR49_TAPS ) { h2b_q31_fir_oldest = 0u; }
    }
}

static void h2b_q31_to_float_block( void )
{
    for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
    {
        for( uint32_t frame = 0u; frame < H2B_BLOCK_FRAMES; frame++ )
        {
            h2b_prefir[channel][frame] = (float)h2b_q31_prefir[channel][frame] * H2B_Q31_INV_SCALE;
        }
    }
}

/* The public DF2T struct must be initialized through this library's routine.
 * Its assembler loads the word at the initializer's pState offset as the
 * coefficient pointer and the next word as state.  Calling the initializer
 * writes that ABI order; direct C field assignment was the Phase-2 BUS ERROR
 * path.  Coefficients and instances are X, while the time-multiplexed state
 * array is in the checked Y scratch arena; source and destination blocks are X. */
static void h2b_optimized_iir_reset( void )
{
    memset( H2B_OPT_IIR_STATE, 0, H2B_IIR_STATE_FLOATS * sizeof(float) );
    for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
    {
        mchp_biquad_cascade_df2T_init_f32(
            &h2b_iir_instance[channel], (uint8_t)ASRC_H2_IIR_SOS,
            asrc_h2_df2t_sos,
            &H2B_OPT_IIR_STATE[channel * ASRC_H2_IIR_SOS * 2u] );
    }
}

static void h2b_fill_stimulus( uint8_t kind, uint32_t block_index )
{
    static const float sine_hz[] = { 1000.0f, 10000.0f, 15000.0f, 17000.0f };
    for( uint32_t ch = 0u; ch < H2B_CHANNELS; ch++ )
    {
        for( uint32_t n = 0u; n < H2B_BLOCK_FRAMES; n++ )
        {
            const float t = (float)( block_index * H2B_BLOCK_FRAMES + n ) / 48000.0f;
            const float phase = 0.03125f * (float)ch;
            float x;
            if( kind < 4u )
            {
                x = 0.999f * sinf( H2B_TWO_PI * sine_hz[kind] * t + phase );
            }
            else if( kind == 4u )
            {
                x = 0.999f * 0.25f *
                    ( sinf( H2B_TWO_PI * 1000.0f * t + phase ) +
                      sinf( H2B_TWO_PI * 7000.0f * t + 0.7f + phase ) +
                      sinf( H2B_TWO_PI * 13000.0f * t + 1.1f + phase ) +
                      sinf( H2B_TWO_PI * 15000.0f * t + 2.0f + phase ) );
            }
            else
            {
                x = ( ( block_index == 0u ) && ( n == 0u ) ) ? 0.999f : 0.0f;
            }
            h2b_input[ch][n] = x * H2B_FULL_SCALE;
        }
    }
}

static void h2b_fir_only( void )
{
    for( uint32_t n = 0u; n < H2B_BLOCK_FRAMES; n++ )
    {
        for( uint32_t group = 0u; group < ( H2B_CHANNELS / H2B_WIDE ); group++ )
        {
            float* const write = &H2B_FIR_HISTORY[
                ( group * H2B_FIR_HISTORY_GROUP_FLOATS ) + ( h2b_fir_oldest * H2B_WIDE )];
            float* const mirror = write + ( ASRC_H2_FIR49_TAPS * H2B_WIDE );
            const uint32_t channel_base = group * H2B_WIDE;
            for( uint32_t lane = 0u; lane < H2B_WIDE; lane++ )
            {
                const float x = h2b_input[channel_base + lane][n];
                write[lane] = x;
                mirror[lane] = x;
            }

            /* `write + 8` begins at the next-oldest frame and runs contiguously
             * through the duplicate just written above.  Scatter back to the
             * channel-major block shape that the existing DF2T primitive uses. */
            fir_ring_wide8_f32( write + H2B_WIDE, asrc_h2_fir49, ASRC_H2_FIR49_TAPS, h2b_wide_out );
            for( uint32_t lane = 0u; lane < H2B_WIDE; lane++ )
            {
                h2b_prefir[channel_base + lane][n] = h2b_wide_out[lane];
            }
        }
        h2b_fir_oldest++;
        if( h2b_fir_oldest == ASRC_H2_FIR49_TAPS ) { h2b_fir_oldest = 0u; }
    }
}

static void h2b_iir_df2t_block( const float* src, float* dst, float* state )
{
    for( uint32_t n = 0u; n < H2B_BLOCK_FRAMES; n++ )
    {
        const float* coeff = asrc_h2_df2t_sos;
        float* section_state = state;
        float x = src[n];

        for( uint32_t section = 0u; section < ASRC_H2_IIR_SOS; section++ )
        {
            const float d1 = section_state[0];
            const float d2 = section_state[1];
            const float y = coeff[0] * x + d1;

            /* Coefficients are [b0, b1, b2, -a1, -a2], so the two
             * feedback terms are additions here.  This is the normal
             * transposed direct-form II recurrence, kept intentionally
             * straightforward for the Stage-1 timing baseline. */
            section_state[0] = coeff[1] * x + d2 + coeff[3] * y;
            section_state[1] = coeff[2] * x + coeff[4] * y;
            x = y;
            coeff += 5u;
            section_state += 2u;
        }
        dst[n] = x;
    }
}

static void h2b_iir_only( void )
{
    for( uint32_t ch = 0u; ch < H2B_CHANNELS; ch++ )
    {
        h2b_iir_df2t_block( h2b_prefir[ch], h2b_output[ch],
                             &H2B_IIR_STATE[ch * ASRC_H2_IIR_SOS * 2u] );
    }
}

/* Keep these as direct calls, rather than measuring through a function-pointer
 * dispatch.  A production selection would also call one known primitive, and
 * the extra indirect-call cost would conceal the kernel difference we need to
 * measure. */
static void h2b_iir_vendor_only( void )
{
    for( uint32_t ch = 0u; ch < H2B_CHANNELS; ch++ )
    {
        mchp_biquad_cascade_df2T_f32( &h2b_iir_instance[ch], h2b_prefir[ch],
                                       h2b_opt_output[ch], H2B_BLOCK_FRAMES );
    }
}

static void h2b_iir_opt_v1_only( void )
{
    for( uint32_t ch = 0u; ch < H2B_CHANNELS; ch++ )
    {
        biquad_cascade_df2T_f32_dspic33ak_opt_v1(
            &h2b_iir_instance[ch], h2b_prefir[ch], h2b_opt_output[ch], H2B_BLOCK_FRAMES );
    }
}

static void h2b_combined( void )
{
    h2b_fir_only();
    h2b_iir_only();
}

static void h2b_float_opt_v1_combined( void )
{
    h2b_fir_only();
    h2b_iir_opt_v1_only();
}

static void h2b_q31_vendor_combined( void )
{
    h2b_float_to_q31_block();
    h2b_q31_fir_only();
    h2b_q31_to_float_block();
    h2b_iir_vendor_only();
}

static void h2b_q31_opt_v1_combined( void )
{
    h2b_float_to_q31_block();
    h2b_q31_fir_only();
    h2b_q31_to_float_block();
    h2b_iir_opt_v1_only();
}

static void h2b_print_timing( const char* name, const h2b_stats_t* s, uint32_t overhead )
{
    const uint32_t min_ticks  = h2b_subtract_overhead( s->min_ticks, overhead );
    const uint32_t mean_ticks = h2b_subtract_overhead( h2b_stats_mean( s ), overhead );
    const uint32_t max_ticks  = h2b_subtract_overhead( s->max_ticks, overhead );
    printf( "    %-14s n=%lu cycles min/mean/max=%lu/%lu/%lu  us=%lu.%02lu/%lu.%02lu/%lu.%02lu\n",
            name, (unsigned long)s->n,
            (unsigned long)( min_ticks * H2B_CYC_PER_TICK ),
            (unsigned long)( mean_ticks * H2B_CYC_PER_TICK ),
            (unsigned long)( max_ticks * H2B_CYC_PER_TICK ),
            (unsigned long)( min_ticks / 100u ), (unsigned long)( min_ticks % 100u ),
            (unsigned long)( mean_ticks / 100u ), (unsigned long)( mean_ticks % 100u ),
            (unsigned long)( max_ticks / 100u ), (unsigned long)( max_ticks % 100u ) );
}

static void h2b_headroom_report( void )
{
    static const char* const names[] = { "1k", "10k", "15k", "17k", "multitone", "impulse" };
    for( uint8_t kind = 0u; kind < 6u; kind++ )
    {
        float max_output = 0.0f;
        float max_state = 0.0f;
        uint8_t nonfinite = 0u;
        h2b_filters_reset();
        for( uint32_t block = 0u; block < 128u; block++ )
        {
            h2b_fill_stimulus( kind, block );
            h2b_combined();
            for( uint32_t ch = 0u; ch < H2B_CHANNELS; ch++ )
            {
                for( uint32_t n = 0u; n < H2B_BLOCK_FRAMES; n++ )
                {
                    const float a = fabsf( h2b_output[ch][n] );
                    if( !isfinite( h2b_output[ch][n] ) ) { nonfinite = 1u; }
                    if( a > max_output ) { max_output = a; }
                }
                for( uint32_t s = 0u; s < ( ASRC_H2_IIR_SOS * 2u ); s++ )
                {
                    const float state = H2B_IIR_STATE[( ch * ASRC_H2_IIR_SOS * 2u ) + s];
                    const float a = fabsf( state );
                    if( !isfinite( state ) ) { nonfinite = 1u; }
                    if( a > max_state ) { max_state = a; }
                }
            }
        }
        printf( "    headroom %-9s out=%lu.%03lu FS state=%lu.%03lu FS finite=%s\n",
                names[kind],
                (unsigned long)( max_output / H2B_FULL_SCALE ),
                (unsigned long)( ( ( max_output / H2B_FULL_SCALE ) * 1000.0f ) ) % 1000u,
                (unsigned long)( max_state / H2B_FULL_SCALE ),
                (unsigned long)( ( ( max_state / H2B_FULL_SCALE ) * 1000.0f ) ) % 1000u,
                nonfinite ? "FAIL" : "pass" );
    }
}

void asrc_h2_timing_bench_run( uint32_t trials )
{
    h2b_stats_t empty, fir, iir, combined;
    uint32_t sp_probe = 0u;
    if( trials == 0u ) { trials = H2B_DEFAULT_TRIALS; }

    if( ( H2B_Y_ARENA <= (uintptr_t)&sp_probe ) ||
        ( ( H2B_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u ) )
    {
        printf( "\n *ah REFUSING to run: Y scratch 0x%05lx, stack near 0x%05lx; 4096 B required.\n",
                (unsigned long)H2B_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe );
        return;
    }

    h2b_filters_reset();
    h2b_fill_stimulus( 4u, 0u );
    h2b_stats_reset( &empty );
    h2b_stats_reset( &fir );
    h2b_stats_reset( &iir );
    h2b_stats_reset( &combined );

    /* No IPL masking: min is the uncontended floor while mean/max retain the
     * real foreground preemption by TDM/DMA.  A p99 would require retaining a
     * 10,000-element sample array, so this probe reports the full extrema and
     * mean without adding that persistent RAM to the measurement image. */
    for( uint32_t i = 0u; i < trials; i++ )
    {
        uint32_t t0 = nora_high_res_timer_get_count();
        h2b_stats_add( &empty, nora_high_res_timer_get_count() - t0 );
    }
    for( uint32_t i = 0u; i < trials; i++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        h2b_fir_only();
        h2b_stats_add( &fir, nora_high_res_timer_get_count() - t0 );
    }
    for( uint32_t i = 0u; i < trials; i++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        h2b_iir_only();
        h2b_stats_add( &iir, nora_high_res_timer_get_count() - t0 );
    }
    h2b_filters_reset();
    h2b_fill_stimulus( 4u, 0u );
    for( uint32_t i = 0u; i < trials; i++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        h2b_combined();
        h2b_stats_add( &combined, nora_high_res_timer_get_count() - t0 );
    }

    printf( "\n *ah H2 component timing  FIR49/fc16 + elliptic 5-SOS DF2T (no EQ)\n" );
    printf( "    coeff crc=0x%08lx fir=0x%08lx sos=0x%08lx  CPU=%luMHz timer=%luMHz tick=0.01us cycles/tick=%lu\n",
            (unsigned long)ASRC_H2_COEFF_CRC32, (unsigned long)ASRC_H2_FIR49_CRC32,
            (unsigned long)ASRC_H2_DF2T_SOS_CRC32,
            (unsigned long)( PLL1_CLK_HZ / 1000000UL ), (unsigned long)( FCY / 1000000UL ),
            (unsigned long)H2B_CYC_PER_TICK );
    printf( "    geometry=16ch x 16frames = 256 samples/block; FIR=12544 MAC/block; IIR=6400 mul + 5120 add/block\n" );
    printf( "    flow=float ASRC-domain -> FIR49/wide8 -> float DF2T -> float generic ASRC; sample conversion=0\n" );
    printf( "    placement: fir-coeff=0x%05lx (%s) fir-history=0x%05lx (%s)\n",
            (unsigned long)(uintptr_t)&asrc_h2_fir49[0],
            h2b_data_space( &asrc_h2_fir49[0] ),
            (unsigned long)(uintptr_t)H2B_FIR_HISTORY, h2b_data_space( H2B_FIR_HISTORY ) );
    printf( "               iir-coeff=0x%05lx (%s) iir-state=0x%05lx (%s), scratch end=0x%05lx\n",
            (unsigned long)(uintptr_t)&asrc_h2_df2t_sos[0], h2b_data_space( &asrc_h2_df2t_sos[0] ),
            (unsigned long)(uintptr_t)H2B_IIR_STATE, h2b_data_space( H2B_IIR_STATE ),
            (unsigned long)H2B_Y_ARENA_END );
    printf( "    raw timer pair ticks min/mean/max=%lu/%lu/%lu; printed component results subtract its minimum.\n",
            (unsigned long)empty.min_ticks, (unsigned long)h2b_stats_mean( &empty ),
            (unsigned long)empty.max_ticks );
    h2b_print_timing( "FIR49", &fir, empty.min_ticks );
    h2b_print_timing( "IIR 5-SOS", &iir, empty.min_ticks );
    h2b_print_timing( "FIR+IIR", &combined, empty.min_ticks );
    h2b_headroom_report();
}

/* ---- Phase 3: H2 existing optimized-kernel feasibility ---------------------------------------
 *
 * This remains a foreground microbenchmark.  It neither attaches the H2
 * frontend to audio nor calls the generic ASRC.  Its narrow question is whether
 * the existing DSP primitives plus all required float/Q31 boundaries leave a
 * credible amount of a 333.33 us block for those unmeasured shipping steps.
 */
typedef void (*h2b_runner_t)( void );

typedef struct
{
    float max_error;
    float peak;
    uint32_t saturations;
    uint8_t nonfinite;
} h2b_validation_t;

static void h2b_validation_add( h2b_validation_t* v, float reference, float actual )
{
    const float error = fabsf( reference - actual );
    const float magnitude = fabsf( actual );
    if( !isfinite( actual ) || !isfinite( reference ) ) { v->nonfinite = 1u; }
    if( error > v->max_error ) { v->max_error = error; }
    if( magnitude > v->peak ) { v->peak = magnitude; }
}

static void h2b_validation_check_opt_state( h2b_validation_t* v )
{
    for( uint32_t word = 0u; word < H2B_IIR_STATE_FLOATS; word++ )
    {
        if( !isfinite( H2B_OPT_IIR_STATE[word] ) ) { v->nonfinite = 1u; }
    }
}

static void h2b_validation_print( const char* name, const h2b_validation_t* v )
{
    const uint32_t error_milli_fs = (uint32_t)( ( v->max_error * 1000.0f ) / H2B_FULL_SCALE );
    const uint32_t peak_milli_fs = (uint32_t)( ( v->peak * 1000.0f ) / H2B_FULL_SCALE );
    const uint8_t pass = ( !v->nonfinite ) && ( v->saturations == 0u ) &&
                         ( v->max_error <= H2B_VALIDATE_TOL_COUNTS );
    printf( "    validate %-13s err<=%lu counts (%lu.%03lu FS) peak=%lu.%03lu FS sat=%lu finite=%s %s\n",
            name,
            (unsigned long)v->max_error,
            (unsigned long)( error_milli_fs / 1000u ), (unsigned long)( error_milli_fs % 1000u ),
            (unsigned long)( peak_milli_fs / 1000u ), (unsigned long)( peak_milli_fs % 1000u ),
            (unsigned long)v->saturations, v->nonfinite ? "FAIL" : "pass", pass ? "PASS" : "FAIL" );
}

/* IIR-only numerical check.  The Phase-3 state shares the Phase-2 state arena
 * so that the probe remains below the reset diagnostics.  Replay deterministic
 * checkpoints from reset: reference and candidate then see identical FIR input
 * and IIR history without needing a second permanent Y-state bank. */
static void h2b_validate_float_iir( const char* name, h2b_runner_t runner )
{
    static const uint8_t kinds[] = { 0u, 1u, 2u, 3u, 5u };  /* 1k, 10k, 15k, 17k, impulse */
    static const uint8_t checkpoints[] = { 0u, 1u, 8u, 32u, 127u };
    h2b_validation_t v = { 0.0f, 0.0f, 0u, 0u };

    for( uint32_t test = 0u; test < ( sizeof(kinds) / sizeof(kinds[0]) ); test++ )
    {
        for( uint32_t point = 0u; point < ( sizeof(checkpoints) / sizeof(checkpoints[0]) ); point++ )
        {
            const uint32_t last = checkpoints[point];
            h2b_filters_reset();
            for( uint32_t block = 0u; block <= last; block++ )
            {
                h2b_fill_stimulus( kinds[test], block );
                h2b_fir_only();
                h2b_iir_only();
            }
            memcpy( h2b_reference, h2b_output, sizeof(h2b_reference) );

            h2b_filters_reset();
            h2b_optimized_iir_reset();
            for( uint32_t block = 0u; block <= last; block++ )
            {
                h2b_fill_stimulus( kinds[test], block );
                h2b_fir_only();
                runner();
            }
            for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
            {
                for( uint32_t frame = 0u; frame < H2B_BLOCK_FRAMES; frame++ )
                {
                    h2b_validation_add( &v, h2b_reference[channel][frame], h2b_opt_output[channel][frame] );
                }
            }
            h2b_validation_check_opt_state( &v );
        }
    }
    h2b_validation_print( name, &v );
}

/* The Q31 FIR overlays the float-wide history, so compare deterministic
 * checkpoints by replaying the short sequence from reset.  This covers the
 * impulse onset and tail as well as steady 1/10/15/17 kHz states without adding
 * a large persistent reference buffer to this measurement-only image. */
static void h2b_validate_q31_frontend( const char* name, h2b_runner_t runner )
{
    static const uint8_t kinds[] = { 0u, 1u, 2u, 3u, 5u };
    static const uint8_t checkpoints[] = { 0u, 1u, 8u, 32u, 127u };
    h2b_validation_t v = { 0.0f, 0.0f, 0u, 0u };

    for( uint32_t test = 0u; test < ( sizeof(kinds) / sizeof(kinds[0]) ); test++ )
    {
        for( uint32_t point = 0u; point < ( sizeof(checkpoints) / sizeof(checkpoints[0]) ); point++ )
        {
            const uint32_t last = checkpoints[point];
            h2b_filters_reset();
            for( uint32_t block = 0u; block <= last; block++ )
            {
                h2b_fill_stimulus( kinds[test], block );
                h2b_fir_only();
                h2b_iir_only();
            }
            memcpy( h2b_reference, h2b_output, sizeof(h2b_reference) );

            h2b_q31_filters_reset();
            h2b_optimized_iir_reset();
            for( uint32_t block = 0u; block <= last; block++ )
            {
                h2b_fill_stimulus( kinds[test], block );
                h2b_float_to_q31_block();
                h2b_q31_fir_only();
                h2b_q31_to_float_block();
                runner();
            }
            for( uint32_t channel = 0u; channel < H2B_CHANNELS; channel++ )
            {
                for( uint32_t frame = 0u; frame < H2B_BLOCK_FRAMES; frame++ )
                {
                    h2b_validation_add( &v, h2b_reference[channel][frame], h2b_opt_output[channel][frame] );
                }
            }
            v.saturations += h2b_q31_saturations;
            h2b_validation_check_opt_state( &v );
        }
    }
    h2b_validation_print( name, &v );
}

static void h2b_print_speedup( const char* name, const h2b_stats_t* s,
                                uint32_t overhead, uint32_t baseline_ticks )
{
    const uint32_t mean_ticks = h2b_subtract_overhead( h2b_stats_mean( s ), overhead );
    const uint32_t speedup_x100 = ( mean_ticks != 0u ) ?
        (uint32_t)( ( (uint64_t)baseline_ticks * 100u ) / mean_ticks ) : 0u;
    printf( "    speedup %-14s vs baseline = %lu.%02lux\n", name,
            (unsigned long)( speedup_x100 / 100u ), (unsigned long)( speedup_x100 % 100u ) );
}

#define H2B_MEASURE_DIRECT(stats_, trials_, call_) \
    do { \
        h2b_stats_reset( &(stats_) ); \
        for( uint32_t h2b_trial = 0u; h2b_trial < (trials_); h2b_trial++ ) \
        { \
            const uint32_t h2b_t0 = nora_high_res_timer_get_count(); \
            call_; \
            h2b_stats_add( &(stats_), nora_high_res_timer_get_count() - h2b_t0 ); \
        } \
    } while(0)

void asrc_h2_optimized_kernel_bench_run( uint32_t trials )
{
    h2b_stats_t empty;
    h2b_stats_t iir_v1;
    h2b_stats_t float_v1;
    uint32_t sp_probe = 0u;
    if( trials == 0u ) { trials = H2B_DEFAULT_TRIALS; }

    if( ( H2B_Y_ARENA <= (uintptr_t)&sp_probe ) ||
        ( ( H2B_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u ) )
    {
        printf( "\n *ao REFUSING to run: Y scratch 0x%05lx, stack near 0x%05lx; 4096 B required.\n",
                (unsigned long)H2B_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe );
        return;
    }

    h2b_q31_prepare_coefficients();
    h2b_fill_stimulus( 4u, 0u );

    printf( "\n *ao H2 Phase-3 existing-kernel feasibility  FIR49/fc16 + fixed elliptic 5-SOS (no EQ)\n" );
    printf( "    geometry=16ch x 16frames = 256 samples/block; FIR=12544 MAC/block; IIR=1280 SOS/block\n" );
    printf( "    flow=shipping float ASRC counts -> [float FIR | float-to-Q31 -> Q31 FIR -> Q31-to-float] -> float DF2T -> float ASRC\n" );
    printf( "    DF2T setup=library initializer; coeff/instance=X, state=Y, src/dst=X; trials=%lu\n",
            (unsigned long)trials );

    /* Do this before timing.  A BUS ERROR here is the requested one-shot
     * vendor feasibility probe; no shipping code or library source changes. */
    h2b_validate_float_iir( "vendor DF2T", h2b_iir_vendor_only );
    h2b_validate_float_iir( "opt DF2T v1", h2b_iir_opt_v1_only );
    /* opt v2 is deliberately excluded after its one-shot validation caused a
     * BUS ERROR at _biquad_cascade_df2T_f32_dspic33ak_opt_v2+0x40.  Phase 3
     * records it as unavailable rather than debugging or modifying it. */

    h2b_stats_reset( &empty );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        h2b_stats_add( &empty, nora_high_res_timer_get_count() - t0 );
    }

    /* The vendor DF2T passed the short numerical validation above but one
     * 16ch x 16-sample timed call raised BUS ERROR at
     * _mchp_biquad_cascade_df2T_f32+0x34.  Do not invoke it again: this phase
     * classifies it as unavailable rather than debugging the library. */
    h2b_optimized_iir_reset();
    H2B_MEASURE_DIRECT( iir_v1, trials, h2b_iir_opt_v1_only() );

    h2b_filters_reset();
    h2b_optimized_iir_reset();
    H2B_MEASURE_DIRECT( float_v1, trials, h2b_float_opt_v1_combined() );

    printf( "    timer pair ticks min/mean/max=%lu/%lu/%lu; all component values subtract the minimum.\n",
            (unsigned long)empty.min_ticks, (unsigned long)h2b_stats_mean( &empty ),
            (unsigned long)empty.max_ticks );
    printf( "    Phase-2 float-C baseline mean: FIR49=261.92 us  IIR5=330.01 us  combined=592.14 us\n" );
    printf( "  Q31 FIR candidate: REJECT before timing: validation has saturation/quantization error; prior 10000-trial attempt trapped in fir_ring_q31.\n" );
    printf( "  vendor DF2T: UNAVAILABLE: short validation PASS, but one 16ch x 16 timed call BUS ERROR at +0x34; excluded without debug.\n" );
    printf( "  opt DF2T v2: UNAVAILABLE: one validation caused BUS ERROR at its +0x40; excluded without debug.\n" );
    printf( "  5-SOS IIR candidates:\n" );
    h2b_print_timing( "opt DF2T v1", &iir_v1, empty.min_ticks );
    h2b_print_speedup( "opt DF2T v1", &iir_v1, empty.min_ticks, 33001u );
    printf( "  Combined float frontend candidates:\n" );
    h2b_print_timing( "float + opt v1", &float_v1, empty.min_ticks );
    h2b_print_speedup( "float + opt v1", &float_v1, empty.min_ticks, 59214u );
    printf( "    generic ASRC step~=1.5 = NOT RUN (no full-chain or ASRC integration in Phase 3)\n" );
    printf( " *ao complete\n" );
}

#undef H2B_MEASURE_DIRECT
#endif /* ASRC_H2_KERNEL_BENCH_AVAILABLE */

/* ---- Short /2 FIR versus IIR CPU trade-off -----------------------------------------------------
 *
 * This is deliberately a kernel-only calibration.  It reproduces the existing
 * Q31 /2 front end's one-call-per-channel batch geometry, but does not attach a
 * coefficient set to any live rate or alter the audio path.  Its answer is
 * therefore "what CPU does a shorter existing kernel buy?", not "is this a
 * 48->32 filter?"  The latter requires its own topology and spectral gate.
 */
#define FIRT_CHANNELS       (16u)
#define FIRT_INPUT_FRAMES   (16u)
#define FIRT_DECIMATION     (2u)
#define FIRT_OUTPUTS        (FIRT_INPUT_FRAMES / FIRT_DECIMATION)
#define FIRT_BLOCK_CALLS    (FIRT_CHANNELS)
#define FIRT_RING_SAMPLES   (209u)  /* shipping Q31 /2 ring allocation */
#define FIRT_DEFAULT_TRIALS (10000u)
#define FIRT_Y_ARENA        (0x12240u) /* same checked probe-local Y region as H2 */
#define FIRT_Y_ARENA_LIMIT  (0x13E00u)
#define FIRT_Y_RING         ((int32_t*)(uintptr_t)FIRT_Y_ARENA)
#define FIRT_Y_RING_END     (FIRT_Y_ARENA + (FIRT_RING_SAMPLES * sizeof(int32_t)))
#define FIRT_CYC_PER_TICK   (PLL1_CLK_HZ / FCY)

_Static_assert( FIRT_OUTPUTS == 8u, "16-frame /2 probe must emit eight outputs" );
_Static_assert( FIRT_Y_RING_END <= FIRT_Y_ARENA_LIMIT, "trade-off ring exceeds checked Y scratch" );

typedef struct
{
    uint32_t min_ticks;
    uint32_t max_ticks;
    uint64_t sum_ticks;
    uint32_t n;
} firt_stats_t;

static void firt_stats_reset( firt_stats_t* s )
{
    s->min_ticks = UINT32_MAX;
    s->max_ticks = 0u;
    s->sum_ticks = 0u;
    s->n = 0u;
}

static void firt_stats_add( firt_stats_t* s, uint32_t ticks )
{
    if( ticks < s->min_ticks ) { s->min_ticks = ticks; }
    if( ticks > s->max_ticks ) { s->max_ticks = ticks; }
    s->sum_ticks += ticks;
    s->n++;
}

static uint32_t firt_stats_mean( const firt_stats_t* s )
{
    return ( s->n != 0u ) ? (uint32_t)( s->sum_ticks / s->n ) : 0u;
}

static uint32_t firt_subtract_overhead( uint32_t ticks, uint32_t overhead )
{
    return ( ticks > overhead ) ? ( ticks - overhead ) : 0u;
}

static void firt_print_timing( const char* name, const firt_stats_t* s, uint32_t overhead )
{
    const uint32_t min_ticks  = firt_subtract_overhead( s->min_ticks, overhead );
    const uint32_t mean_ticks = firt_subtract_overhead( firt_stats_mean( s ), overhead );
    const uint32_t max_ticks  = firt_subtract_overhead( s->max_ticks, overhead );
    printf( "    %-6s n=%lu cycles/block min/mean/max=%lu/%lu/%lu  us/block=%lu.%02lu/%lu.%02lu/%lu.%02lu\n",
            name, (unsigned long)s->n,
            (unsigned long)( min_ticks * FIRT_CYC_PER_TICK ),
            (unsigned long)( mean_ticks * FIRT_CYC_PER_TICK ),
            (unsigned long)( max_ticks * FIRT_CYC_PER_TICK ),
            (unsigned long)( min_ticks / 100u ), (unsigned long)( min_ticks % 100u ),
            (unsigned long)( mean_ticks / 100u ), (unsigned long)( mean_ticks % 100u ),
            (unsigned long)( max_ticks / 100u ), (unsigned long)( max_ticks % 100u ) );
}

typedef struct
{
    const char* name;
    uint16_t taps;
    const int32_t* coeff_flash;
    int64_t sum_q31;
} firt_candidate_t;

static const firt_candidate_t firt_candidates[] = {
    { "FIR107", 107u, firt_coeff_107, FIRT_COEFF_107_SUM_Q31 },
    { "FIR81",   81u, firt_coeff_81,  FIRT_COEFF_81_SUM_Q31  },
    { "FIR65",   65u, firt_coeff_65,  FIRT_COEFF_65_SUM_Q31  },
    { "FIR49",   49u, firt_coeff_49,  FIRT_COEFF_49_SUM_Q31  },
    { "FIR33",   33u, firt_coeff_33,  FIRT_COEFF_33_SUM_Q31  },
};

static volatile int32_t firt_sink;

static uint8_t firt_coefficients_valid( const firt_candidate_t* c )
{
    uint32_t nonzero = 0u;
    int64_t sum = 0;
    for( uint32_t k = 0u; k < c->taps; k++ )
    {
        if( c->coeff_flash[k] != c->coeff_flash[c->taps - 1u - k] ) { return 0u; }
        if( c->coeff_flash[k] != 0 ) { nonzero++; }
        sum += c->coeff_flash[k];
    }
    return ( nonzero == c->taps ) && ( sum == c->sum_q31 ) &&
           ( c->coeff_flash[c->taps / 2u] != INT32_MAX );
}

/* The shipping /2 stage has 16 input frames, emits eight outputs per channel,
 * and calls the Y-modulo kernel once per channel.  Its static history arena is
 * 209 samples/channel even though the active /2 batch spans only taps+14;
 * retaining that ring size preserves the real modulo setup and call shape. */
static void firt_prepare_candidate( const firt_candidate_t* c )
{
    memcpy( firb_coeff, c->coeff_flash, (size_t)c->taps * sizeof(firb_coeff[0]) );
    firb_fill_samples( FIRT_Y_RING, FIRT_RING_SAMPLES, 0xF1A24000u + c->taps );
}

static void firt_q31_half_block( uint32_t taps )
{
    int32_t tmp[FIRT_OUTPUTS];
    const uint32_t span = taps + ( ( FIRT_OUTPUTS - 1u ) * FIRT_DECIMATION );
    const int32_t* const window = FIRT_Y_RING + ( FIRT_RING_SAMPLES - span );
    int32_t checksum = 0;

    for( uint32_t channel = 0u; channel < FIRT_CHANNELS; channel++ )
    {
        (void)fir_ring_q31_ymod_yonly_block( firb_coeff, window, taps, tmp,
                                              FIRT_OUTPUTS, FIRT_DECIMATION * 4u,
                                              FIRT_Y_RING, FIRT_RING_SAMPLES * 4u );
        checksum ^= tmp[channel & ( FIRT_OUTPUTS - 1u )];
    }
    /* The external assembly call itself cannot be removed, but keep an explicit
     * observable dependency on all sixteen calls.  This one store is tap
     * invariant and is outside the inner FIR loops. */
    firt_sink ^= checksum;
}

static int32_t firt_validate_candidate( uint32_t taps )
{
    int32_t tmp[FIRT_OUTPUTS];
    const uint32_t span = taps + ( ( FIRT_OUTPUTS - 1u ) * FIRT_DECIMATION );
    const int32_t* const window = FIRT_Y_RING + ( FIRT_RING_SAMPLES - span );
    const int32_t reference = firb_ref_q31_ring( firb_coeff, FIRT_Y_RING,
                                                  FIRT_RING_SAMPLES,
                                                  FIRT_RING_SAMPLES - span, taps );
    (void)fir_ring_q31_ymod_yonly_block( firb_coeff, window, taps, tmp,
                                          FIRT_OUTPUTS, FIRT_DECIMATION * 4u,
                                          FIRT_Y_RING, FIRT_RING_SAMPLES * 4u );
    return tmp[0] - reference;
}

void asrc_fir_tradeoff_bench_run( uint32_t trials )
{
    firt_stats_t empty;
    uint32_t sp_probe = 0u;
    if( trials == 0u ) { trials = FIRT_DEFAULT_TRIALS; }

    if( ( FIRT_Y_ARENA <= (uintptr_t)&sp_probe ) ||
        ( ( FIRT_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u ) )
    {
        printf( "\n *ad REFUSING to run: Y scratch 0x%05lx, stack near 0x%05lx; 4096 B required.\n",
                (unsigned long)FIRT_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe );
        return;
    }

    firt_stats_reset( &empty );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        firt_stats_add( &empty, nora_high_res_timer_get_count() - t0 );
    }

    printf( "\n *ad short-/2 FIR CPU trade-off (calibration only; no live filter change)\n" );
    printf( "    geometry=16ch x 16 input frames -> 8 outputs/ch; calls/block=16, MAC/block=128*taps\n" );
    printf( "    kernel=fir_ring_q31_ymod_yonly_block; coeff=X RAM, history=Y modulo, ring=209 samples/ch\n" );
    printf( "    candidate family=48kHz, pass=8850Hz, stop=12000Hz, Kaiser beta=11; trials=%lu\n",
            (unsigned long)trials );
    printf( "    timer pair ticks min/mean/max=%lu/%lu/%lu; values subtract the minimum.\n",
            (unsigned long)empty.min_ticks, (unsigned long)firt_stats_mean( &empty ),
            (unsigned long)empty.max_ticks );

    for( uint32_t candidate = 0u;
         candidate < ( sizeof(firt_candidates) / sizeof(firt_candidates[0]) ); candidate++ )
    {
        const firt_candidate_t* const c = &firt_candidates[candidate];
        firt_stats_t stats;
        const uint32_t macs = FIRT_CHANNELS * FIRT_OUTPUTS * c->taps;
        const uint8_t coeff_ok = firt_coefficients_valid( c );

        if( !coeff_ok )
        {
            printf( "    %-6s INVALID coefficient bank; not timed.\n", c->name );
            continue;
        }
        firt_prepare_candidate( c );
        const int32_t error = firt_validate_candidate( c->taps );
        if( ( error < -4 ) || ( error > 4 ) )
        {
            printf( "    %-6s validation err=%ld LSB; not timed.\n", c->name, (long)error );
            continue;
        }

        /* Warm code and the modulo path outside the measured set.  Interrupts stay
         * enabled: min captures the uncontended floor and mean/max retain normal
         * foreground preemption, exactly as the Stage-1 timing probe does. */
        for( uint32_t warm = 0u; warm < 8u; warm++ ) { firt_q31_half_block( c->taps ); }
        firt_stats_reset( &stats );
        for( uint32_t trial = 0u; trial < trials; trial++ )
        {
            const uint32_t t0 = nora_high_res_timer_get_count();
            firt_q31_half_block( c->taps );
            firt_stats_add( &stats, nora_high_res_timer_get_count() - t0 );
        }
        printf( "    %-6s taps=%lu calls=%lu outputs=%lu MAC=%lu q31sum=unit err=%ld LSB\n",
                c->name, (unsigned long)c->taps, (unsigned long)FIRT_BLOCK_CALLS,
                (unsigned long)( FIRT_CHANNELS * FIRT_OUTPUTS ), (unsigned long)macs,
                (long)error );
        firt_print_timing( c->name, &stats, empty.min_ticks );
    }
    printf( "    FIR-only scope: history push and output format stores are tap-invariant and excluded.\n" );
    printf( " *ad complete sink=%ld\n", (long)firt_sink );
}


/* ---- E-family half-band /2 kernel: measured block difference -----------------------------------
 *
 * MEASUREMENT ONLY.  Nothing here changes a live filter, a rate, or a default.
 *
 * WHAT QUESTION THIS ANSWERS.  The host CPU model
 * ([internal] study_fir_lightweight_96k32k_2026-09-06.md, candidate E) says that
 * replacing the shipping 41-tap /2 pre-stage with a 35-tap HALF-BAND prototype
 * drops 22 of its 35 taps as structural zeros and should save 10.69 us/block at
 * 12 channels.  That figure assumes the measured 1.012 cycles/MAC applies to the
 * shorter run too -- i.e. that the fixed cost per output does not grow when the
 * Y pointer has to stride two samples and step around the lone odd-index tap.
 * The model cannot settle that; only the hardware can.  So this probe measures
 * the BLOCK DIFFERENCE directly, which is the number the 500 us TDM chain
 * actually cares about, and reports cycles/MAC only as secondary evidence.
 *
 * It is not a filter design and makes no claim about the response.  The
 * coefficients are the same synthetic parabolic shape the rest of this file
 * uses, with the half-band structural zeros forced exactly to zero; the kernels
 * have no data-dependent branches, so timing is a function of the geometry only
 * and synthetic taps time identically to a real 35-tap beta=11 design.  The
 * SPECTRAL side of candidate E stays on the host, where it already is.
 *
 * THREE CANDIDATES, so the two effects can be separated:
 *
 *   DENSE41  the shipping /2 geometry.  The baseline the chain is measured against.
 *   DENSE35  the same half-band filter, run densely -- 35 MACs, 16 of them by zero.
 *            Its gap to DENSE41 is what merely shortening the filter buys.
 *   HB35     the same filter again, run by the half-band kernel -- 19 MACs.
 *            Its gap to DENSE35 is what skipping the structural zeros buys.
 *
 * DENSE35 and HB35 must agree BIT FOR BIT: they are one filter computed two
 * ways.  That equality, swept over every window start in the ring, is also the
 * correctness test for the thing that could not be reasoned about -- whether the
 * Y AGU's modulo folds correctly when an increment of 8 can step OVER the ring
 * end rather than landing on it.  The earlier M6 sweep only exercised increment
 * 4.  The sweep therefore runs on the shipping 209-sample ring, whose 836 bytes
 * are NOT a multiple of 8, and again on a 210-sample ring, whose 840 bytes are.
 */
#define FIRE_TAPS_DENSE     (41u)                               /* shipping /2 pre-stage */
#define FIRE_TAPS_HB        (35u)                               /* candidate E */
#define FIRE_HB_HALF        ((FIRE_TAPS_HB + 1u) / 4u)          /* kernel argument, = 9 */
#define FIRE_HB_NONZERO     ((2u * FIRE_HB_HALF) + 1u)          /* = 19 */
#define FIRE_DECIMATION     (2u)
#define FIRE_INPUT_FRAMES   (16u)                               /* APP_BLOCK_FRAMES */
#define FIRE_OUTPUTS        (FIRE_INPUT_FRAMES / FIRE_DECIMATION)
#define FIRE_CH_TARGET      (12u)   /* the 96->32 kHz decision geometry */
#define FIRE_CH_LEGACY      (16u)   /* keeps continuity with *ad's 16 ch numbers */
#define FIRE_DEFAULT_TRIALS (10000u)
#define FIRE_RING_ODD       (209u)  /* shipping allocation: 836 B, NOT a multiple of 8 */
#define FIRE_RING_EVEN      (210u)  /* 840 B = 105 * 8, so stride 8 always LANDS on the end */
#define FIRE_Y_ARENA        (0x12240u) /* same checked probe-local Y region as *ad */
#define FIRE_Y_ARENA_LIMIT  (0x13E00u)
#define FIRE_Y_RING         ((int32_t*)(uintptr_t)FIRE_Y_ARENA)

/* All three banks live in X space at once, at fixed offsets in the shared
 * coefficient array, so the wrap sweep can call both kernels for the same window
 * start without swapping banks between them.  X modulo is off in both kernels,
 * so no start-address rule applies to these offsets. */
#define FIRE_X_DENSE41      (&firb_coeff[0])
#define FIRE_X_DENSE35      (&firb_coeff[64])
#define FIRE_X_HB35         (&firb_coeff[128])

_Static_assert( FIRE_OUTPUTS == 8u, "16-frame /2 probe must emit eight outputs" );
_Static_assert( ( 4u * FIRE_HB_HALF ) - 1u == FIRE_TAPS_HB,
                "a half-band length must be 4*half-1 so its centre index is odd" );
_Static_assert( 64u >= FIRE_TAPS_DENSE, "dense41 bank overruns the dense35 bank" );
_Static_assert( 128u >= ( 64u + FIRE_TAPS_HB ), "dense35 bank overruns the half-band bank" );
_Static_assert( ( 128u + FIRE_HB_NONZERO ) <= 256u, "half-band bank overruns firb_coeff" );
_Static_assert( ( FIRE_Y_ARENA + ( FIRE_RING_EVEN * sizeof(int32_t) ) ) <= FIRE_Y_ARENA_LIMIT,
                "half-band ring exceeds checked Y scratch" );

static volatile int32_t fire_sink;
static uint32_t fire_ring_samples = FIRE_RING_ODD;

/* Builds the three banks.  Returns the number of non-zero taps actually packed,
 * which the caller checks against FIRE_HB_NONZERO -- that equality is the proof
 * that the bank's zero pattern is the one the kernel's stride assumes, and it is
 * derived from the half-band definition here rather than hard-coded twice. */
static uint32_t fire_build_banks( void )
{
    float          shape[FIRE_TAPS_HB];
    const uint32_t centre = ( FIRE_TAPS_HB - 1u ) / 2u;   /* 17, odd by construction */
    float          sum    = 0.0f;
    uint32_t       packed = 0u;

    /* DENSE41 goes to firb_coeff[0 ..] by construction of firb_fill_coeff(). */
    firb_fill_coeff( FIRE_TAPS_DENSE );

    for( uint32_t i = 0u; i < FIRE_TAPS_HB; i++ )
    {
        const uint32_t d = ( i < centre ) ? ( centre - i ) : ( i - centre );
        /* A half-band prototype is exactly zero at every EVEN offset from the
         * centre, the centre itself excepted.  Force that, do not approximate
         * it: it is the structure the kernel skips over. */
        if( ( d != 0u ) && ( ( d & 1u ) == 0u ) )
        {
            shape[i] = 0.0f;
            continue;
        }
        const float env = (float)( ( i + 1u ) * ( FIRE_TAPS_HB - i ) );
        shape[i] = ( ( ( d / 5u ) & 1u ) != 0u ) ? -env : env;
        sum += ( shape[i] < 0.0f ) ? -shape[i] : shape[i];
    }

    /* sum|h| = 0.5 keeps the int64 reference below an accumulator overflow, the
     * same bound firb_fill_coeff() maintains. */
    const float scale = 0.5f / sum;
    for( uint32_t i = 0u; i < FIRE_TAPS_HB; i++ )
    {
        const int32_t q = (int32_t)( shape[i] * scale * 2147483648.0f );
        const uint32_t d = ( i < centre ) ? ( centre - i ) : ( i - centre );
        FIRE_X_DENSE35[i] = q;
        if( ( d == 0u ) || ( ( d & 1u ) != 0u ) )
        {
            /* ascending index order, which puts the centre between taps 16 and
             * 18 -- exactly the order the kernel walks with its plain +=4 */
            FIRE_X_HB35[packed] = q;
            packed++;
        }
    }
    return packed;
}

static void fire_dense_block( uint32_t channels, const int32_t* xcoeff, uint32_t taps )
{
    int32_t              tmp[FIRE_OUTPUTS];
    const uint32_t       span   = taps + ( ( FIRE_OUTPUTS - 1u ) * FIRE_DECIMATION );
    const int32_t* const window = FIRE_Y_RING + ( fire_ring_samples - span );
    int32_t              checksum = 0;

    for( uint32_t ch = 0u; ch < channels; ch++ )
    {
        (void)fir_ring_q31_ymod_yonly_block( xcoeff, window, taps, tmp, FIRE_OUTPUTS,
                                             FIRE_DECIMATION * 4u, FIRE_Y_RING,
                                             fire_ring_samples * 4u );
        checksum ^= tmp[ch & ( FIRE_OUTPUTS - 1u )];
    }
    fire_sink ^= checksum;
}

static void fire_hb_block( uint32_t channels )
{
    int32_t              tmp[FIRE_OUTPUTS];
    const uint32_t       span   = FIRE_TAPS_HB + ( ( FIRE_OUTPUTS - 1u ) * FIRE_DECIMATION );
    const int32_t* const window = FIRE_Y_RING + ( fire_ring_samples - span );
    int32_t              checksum = 0;

    for( uint32_t ch = 0u; ch < channels; ch++ )
    {
        (void)fir_ring_q31_hb_ymod_yonly_block( FIRE_X_HB35, window, FIRE_HB_HALF, tmp,
                                                FIRE_OUTPUTS, FIRE_DECIMATION * 4u,
                                                FIRE_Y_RING, fire_ring_samples * 4u );
        checksum ^= tmp[ch & ( FIRE_OUTPUTS - 1u )];
    }
    fire_sink ^= checksum;
}

/* Sweeps EVERY window start in the current ring.  At each start it runs the same
 * filter through both kernels and against the int64 reference, so it reports two
 * independent facts: whether the dense path still matches arithmetic done
 * without any AGU (max_ref_err), and whether the stride-8 path matches the
 * stride-4 path bit for bit (mismatches).  A wrap that the AGU folded wrongly
 * shows up in the second even if the first is clean. */
static void fire_wrap_sweep( uint32_t* max_ref_err, uint32_t* mismatches, uint32_t* first_bad_start )
{
    int32_t        od[FIRE_OUTPUTS];
    int32_t        oh[FIRE_OUTPUTS];
    const uint32_t n     = fire_ring_samples;
    uint32_t       worst = 0u;
    uint32_t       bad   = 0u;
    uint32_t       first = UINT32_MAX;

    for( uint32_t start = 0u; start < n; start++ )
    {
        (void)fir_ring_q31_ymod_yonly_block( FIRE_X_DENSE35, FIRE_Y_RING + start, FIRE_TAPS_HB,
                                             od, FIRE_OUTPUTS, FIRE_DECIMATION * 4u,
                                             FIRE_Y_RING, n * 4u );
        (void)fir_ring_q31_hb_ymod_yonly_block( FIRE_X_HB35, FIRE_Y_RING + start, FIRE_HB_HALF,
                                                oh, FIRE_OUTPUTS, FIRE_DECIMATION * 4u,
                                                FIRE_Y_RING, n * 4u );
        for( uint32_t j = 0u; j < FIRE_OUTPUTS; j++ )
        {
            const uint32_t s = ( start + ( j * FIRE_DECIMATION ) ) % n;
            const int32_t  r = firb_ref_q31_ring( FIRE_X_DENSE35, FIRE_Y_RING, n, s,
                                                  FIRE_TAPS_HB );
            const int32_t  e = od[j] - r;
            const uint32_t a = (uint32_t)( ( e < 0 ) ? -e : e );

            if( a > worst ) { worst = a; }
            if( oh[j] != od[j] )
            {
                bad++;
                if( first == UINT32_MAX ) { first = start; }
            }
        }
    }
    *max_ref_err     = worst;
    *mismatches      = bad;
    *first_bad_start = first;
}

/* Ticks are 100 MHz, so one tick is exactly 0.01 us and the split below is not a
 * conversion.  Cycles are 200 MHz (1 tick = 2 instruction cycles). */
static void fire_print_delta( const char* label, int32_t delta_ticks, const char* note )
{
    const uint32_t mag = (uint32_t)( ( delta_ticks < 0 ) ? -delta_ticks : delta_ticks );
    printf( "      %-22s %s%lu.%02lu us/block   %s\n", label,
            ( delta_ticks < 0 ) ? "-" : "+",
            (unsigned long)( mag / 100u ), (unsigned long)( mag % 100u ), note );
}

static void fire_print_per_mac( const char* name, const firt_stats_t* s, uint32_t overhead,
                                uint32_t macs )
{
    const uint32_t ticks  = firt_subtract_overhead( s->min_ticks, overhead );
    const uint32_t cycles = ticks * FIRT_CYC_PER_TICK;
    const uint32_t milli  = ( macs != 0u ) ? (uint32_t)( ( (uint64_t)cycles * 1000u ) / macs ) : 0u;
    printf( "      %-8s MAC/block=%-5lu cycles/MAC=%lu.%03lu\n", name, (unsigned long)macs,
            (unsigned long)( milli / 1000u ), (unsigned long)( milli % 1000u ) );
}

static void fire_time_one( const char* name, uint32_t channels, uint32_t taps, uint32_t macs,
                           uint8_t half_band, uint32_t trials, uint32_t overhead,
                           firt_stats_t* out )
{
    /* Warm the code and the modulo path outside the measured set.  Interrupts stay
     * enabled, as in *ad: min is the uncontended floor, mean/max keep normal
     * foreground preemption. */
    for( uint32_t warm = 0u; warm < 8u; warm++ )
    {
        if( half_band ) { fire_hb_block( channels ); }
        else            { fire_dense_block( channels, ( taps == FIRE_TAPS_DENSE ) ? FIRE_X_DENSE41
                                                                                 : FIRE_X_DENSE35,
                                            taps ); }
    }
    firt_stats_reset( out );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        if( half_band ) { fire_hb_block( channels ); }
        else            { fire_dense_block( channels, ( taps == FIRE_TAPS_DENSE ) ? FIRE_X_DENSE41
                                                                                 : FIRE_X_DENSE35,
                                            taps ); }
        firt_stats_add( out, nora_high_res_timer_get_count() - t0 );
    }
    printf( "      %-8s taps=%lu non-zero=%lu calls=%lu outputs=%lu\n", name,
            (unsigned long)taps,
            (unsigned long)( half_band ? FIRE_HB_NONZERO : taps ),
            (unsigned long)channels, (unsigned long)( channels * FIRE_OUTPUTS ) );
    firt_print_timing( name, out, overhead );
    fire_print_per_mac( name, out, overhead, macs );
}

void asrc_hb_kernel_bench_run( uint32_t trials )
{
    firt_stats_t empty;
    uint32_t     sp_probe = 0u;
    if( trials == 0u ) { trials = FIRE_DEFAULT_TRIALS; }

    if( ( FIRE_Y_ARENA <= (uintptr_t)&sp_probe ) ||
        ( ( FIRE_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u ) )
    {
        printf( "\n *ae REFUSING to run: Y scratch 0x%05lx, stack near 0x%05lx; 4096 B required.\n",
                (unsigned long)FIRE_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe );
        return;
    }

    const uint32_t packed = fire_build_banks();

    printf( "\n *ae half-band /2 kernel, measured block difference (measurement only; no live filter change)\n" );
    printf( "    geometry=%luch and %luch x %lu input frames -> %lu outputs/ch, decimation %lu\n",
            (unsigned long)FIRE_CH_TARGET, (unsigned long)FIRE_CH_LEGACY,
            (unsigned long)FIRE_INPUT_FRAMES, (unsigned long)FIRE_OUTPUTS,
            (unsigned long)FIRE_DECIMATION );
    printf( "    coeff=X RAM, history=Y modulo; dense=fir_ring_q31_ymod_yonly_block  half-band=fir_ring_q31_hb_ymod_yonly_block\n" );
    printf( "    candidate E: %lu tap half-band, half=%lu, non-zero=%lu of %lu (even indices + centre %lu)\n",
            (unsigned long)FIRE_TAPS_HB, (unsigned long)FIRE_HB_HALF, (unsigned long)packed,
            (unsigned long)FIRE_TAPS_HB, (unsigned long)( ( FIRE_TAPS_HB - 1u ) / 2u ) );
    printf( "    coefficients are synthetic: this probe times the geometry, it does not qualify a response.\n" );

    if( packed != FIRE_HB_NONZERO )
    {
        printf( "    REFUSING to run: bank packed %lu non-zero taps, kernel stride assumes %lu.\n",
                (unsigned long)packed, (unsigned long)FIRE_HB_NONZERO );
        return;
    }

    firt_stats_reset( &empty );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        firt_stats_add( &empty, nora_high_res_timer_get_count() - t0 );
    }
    printf( "    trials=%lu; timer pair ticks min/mean/max=%lu/%lu/%lu; values subtract the minimum.\n",
            (unsigned long)trials, (unsigned long)empty.min_ticks,
            (unsigned long)firt_stats_mean( &empty ), (unsigned long)empty.max_ticks );

    /* ---- correctness first, on both ring lengths ---- */
    static const uint32_t fire_rings[2] = { FIRE_RING_ODD, FIRE_RING_EVEN };
    uint32_t              wrap_ok = 1u;

    printf( "    stride-8 modulo wrap sweep (every window start; dense and half-band are one filter):\n" );
    for( uint32_t r = 0u; r < 2u; r++ )
    {
        uint32_t max_ref_err = 0u;
        uint32_t mismatches  = 0u;
        uint32_t first_bad   = UINT32_MAX;

        fire_ring_samples = fire_rings[r];
        firb_fill_samples( FIRE_Y_RING, fire_ring_samples, 0xE1B24000u + fire_ring_samples );
        fire_wrap_sweep( &max_ref_err, &mismatches, &first_bad );

        printf( "      ring=%lu samples (%lu B%s): starts=%lu  dense-vs-int64ref max|err|=%lu LSB  hb-vs-dense mismatches=%lu",
                (unsigned long)fire_ring_samples,
                (unsigned long)( fire_ring_samples * 4u ),
                ( ( ( fire_ring_samples * 4u ) % 8u ) == 0u ) ? ", a multiple of 8"
                                                              : ", NOT a multiple of 8",
                (unsigned long)fire_ring_samples, (unsigned long)max_ref_err,
                (unsigned long)mismatches );
        if( mismatches != 0u ) { printf( " first at start=%lu", (unsigned long)first_bad ); }
        printf( "\n" );

        if( ( mismatches != 0u ) || ( max_ref_err > 4u ) ) { wrap_ok = 0u; }
    }

    if( !wrap_ok )
    {
        printf( "    NOT TIMED: the half-band kernel does not reproduce the dense result, so any\n" );
        printf( "    cycle count would be the cost of a wrong answer.  The stride-8 AGU wrap is the\n" );
        printf( "    first suspect; see the first failing start above.\n" );
        printf( " *ae complete sink=%ld\n", (long)fire_sink );
        return;
    }

    /* ---- timing, on the shipping ring only (tap geometry, not ring length, sets the cost) ---- */
    fire_ring_samples = FIRE_RING_ODD;
    firb_fill_samples( FIRE_Y_RING, fire_ring_samples, 0xE1B24000u + fire_ring_samples );

    static const uint32_t fire_channel_sets[2] = { FIRE_CH_TARGET, FIRE_CH_LEGACY };
    for( uint32_t c = 0u; c < 2u; c++ )
    {
        const uint32_t ch = fire_channel_sets[c];
        firt_stats_t   d41;
        firt_stats_t   d35;
        firt_stats_t   hb;

        printf( "    %luch, ring=%lu samples:\n", (unsigned long)ch,
                (unsigned long)fire_ring_samples );
        fire_time_one( "DENSE41", ch, FIRE_TAPS_DENSE, ch * FIRE_OUTPUTS * FIRE_TAPS_DENSE, 0u,
                       trials, empty.min_ticks, &d41 );
        fire_time_one( "DENSE35", ch, FIRE_TAPS_HB, ch * FIRE_OUTPUTS * FIRE_TAPS_HB, 0u,
                       trials, empty.min_ticks, &d35 );
        fire_time_one( "HB35", ch, FIRE_TAPS_HB, ch * FIRE_OUTPUTS * FIRE_HB_NONZERO, 1u,
                       trials, empty.min_ticks, &hb );

        const int32_t t41 = (int32_t)firt_subtract_overhead( d41.min_ticks, empty.min_ticks );
        const int32_t t35 = (int32_t)firt_subtract_overhead( d35.min_ticks, empty.min_ticks );
        const int32_t thb = (int32_t)firt_subtract_overhead( hb.min_ticks, empty.min_ticks );

        fire_print_delta( "HB35 - DENSE41", thb - t41,
                          "PRIMARY: what candidate E buys against the shipping pre-stage" );
        fire_print_delta( "HB35 - DENSE35", thb - t35,
                          "the structural-zero saving alone, same filter both sides" );
        fire_print_delta( "DENSE35 - DENSE41", t35 - t41,
                          "the shortening alone, before any zero is skipped" );
    }

    printf( "    host model for comparison (%luch): DENSE41 19.92 us, HB35 9.23 us, difference -10.69 us\n",
            (unsigned long)FIRE_CH_TARGET );
    printf( "    host model bands, secondary evidence only: cycles/MAC <=1.088 -> about 10 us saved,\n" );
    printf( "    <=1.307 -> about 8 us saved, >1.307 -> the half-band gain thins out.\n" );
    printf( "    The judgement is the measured HB35 - DENSE41 block delta above, not cycles/MAC.\n" );
    printf( "    Scope: FIR kernel only.  History push, format stores and the rest of the leg are\n" );
    printf( "    tap-invariant and excluded, so this delta transfers to the chain but the absolute\n" );
    printf( "    figures are not a leg time.\n" );
    printf( " *ae complete sink=%ld\n", (long)fire_sink );
}


/* ---- Full-IIR precheck: six-SOS 48 kHz anti-alias LPF ------------------------------------------
 *
 * Phase-4 qualified a six-SOS elliptic LPF for the topology
 *
 *     48 kHz -> IIR only -> unchanged generic ASRC at step about 1.5 -> 32 kHz
 *
 * and estimated its cost as 6 x 25.7 = 154.2 us/block by scaling the Phase-3
 * FIVE-SOS measurement linearly.  That estimate is what this probe replaces: it
 * times SIX sections with the same kernel, the same 16 ch x 16 frame geometry and
 * the same placement, so the sixth section's real marginal cost is measured
 * rather than assumed.
 *
 * It measures the IIR ONLY.  The rest of the Full-IIR budget -- removing the N97
 * front end and moving the generic ASRC from step about 1.0 to step about 1.5 --
 * is not a kernel question and is deliberately not modelled here: it is measured
 * as the difference between two runtime images (APP_ASRC_RUNTIME_48K_TO_8 = 1
 * and 0) at A = 48 kHz / B = 32 kHz, from the same max_demand / pull telemetry
 * the existing CPU-margin study used.  Timing a bench replica of either would
 * answer a different question than "what does the shipping system cost".
 *
 * Like every probe in this file it is foreground-only, opt-in, touches no
 * streaming state, and never enters the audio path.
 */
#if ASRC_H2_KERNEL_BENCH_AVAILABLE

#include "asrc_full_iir_precheck_coeffs.h"

#define FIP_CHANNELS         H2B_CHANNELS
#define FIP_BLOCK_FRAMES     H2B_BLOCK_FRAMES
#define FIP_DEFAULT_TRIALS   (10000u)
#define FIP_STATE_FLOATS     (FIP_CHANNELS * ASRC_FULL_IIR_SOS * 2u)
#define FIP_STATE            ( (float*)(uintptr_t)( H2B_Y_ARENA + \
                               ( H2B_FIR_HISTORY_FLOATS * sizeof(float) ) ) )
#define FIP_Y_ARENA_END      ( H2B_Y_ARENA + \
                               ( ( H2B_FIR_HISTORY_FLOATS + FIP_STATE_FLOATS ) * sizeof(float) ) )
#define FIP_EXPECTED_CRC32   (0x67AE5F41u)
#define FIP_PHASE3_FIVE_SOS_MEAN_TICKS (12864u)

/* The six-SOS state needs 128 B more than the five-SOS H2 bank it shares the
 * arena with.  Assert it rather than trust the arithmetic: the region above is
 * the reset diagnostics, and overrunning it would corrupt them silently. */
_Static_assert( FIP_Y_ARENA_END <= H2B_Y_ARENA_LIMIT,
                "full-IIR six-SOS state exceeds the checked Y scratch arena" );
_Static_assert( ASRC_FULL_IIR_SOS == 6u, "the Phase-4 candidate is six sections" );
_Static_assert( ASRC_FULL_IIR_DF2T_SOS_CRC32 == FIP_EXPECTED_CRC32,
                "generated coefficient header is not the Phase-4 six-SOS candidate" );

/* NO new X-space buffers.  The AK512 ASRC image links with the data spaces full:
 * a 16x16 float output block plus sixteen DF2T instances (about 1.15 KB) is enough
 * to fail the link outright.  The H2 probe already owns exactly those two objects,
 * the two probes are foreground-only and never run concurrently, and both re-init
 * the instances before use -- so they are shared rather than duplicated. */
#define fip_instance  h2b_iir_instance
#define fip_output    h2b_opt_output

static void fip_reset( void )
{
    memset( FIP_STATE, 0, FIP_STATE_FLOATS * sizeof(float) );
    for( uint32_t channel = 0u; channel < FIP_CHANNELS; channel++ )
    {
        /* Through the library initializer, never by field assignment: direct
         * assignment was the Phase-2 BUS ERROR path (see h2b_optimized_iir_reset). */
        mchp_biquad_cascade_df2T_init_f32(
            &fip_instance[channel], (uint8_t)ASRC_FULL_IIR_SOS,
            asrc_full_iir_df2t_sos,
            &FIP_STATE[ channel * ASRC_FULL_IIR_SOS * 2u ] );
    }
}

/* Reuses the H2 probe's input block and stimulus generator: same channel count,
 * same block length, same 48 kHz time base, same full-scale convention. */
static void fip_opt_v1_only( void )
{
    for( uint32_t ch = 0u; ch < FIP_CHANNELS; ch++ )
    {
        biquad_cascade_df2T_f32_dspic33ak_opt_v1(
            &fip_instance[ch], h2b_input[ch], fip_output[ch], FIP_BLOCK_FRAMES );
    }
}

static void fip_local_c_only( void )
{
    for( uint32_t ch = 0u; ch < FIP_CHANNELS; ch++ )
    {
        float* state = &FIP_STATE[ ch * ASRC_FULL_IIR_SOS * 2u ];
        for( uint32_t n = 0u; n < FIP_BLOCK_FRAMES; n++ )
        {
            const float* coeff = asrc_full_iir_df2t_sos;
            float* section_state = state;
            float x = h2b_input[ch][n];
            for( uint32_t section = 0u; section < ASRC_FULL_IIR_SOS; section++ )
            {
                const float d1 = section_state[0];
                const float d2 = section_state[1];
                const float y  = coeff[0] * x + d1;
                section_state[0] = coeff[1] * x + d2 + coeff[3] * y;
                section_state[1] = coeff[2] * x + coeff[4] * y;
                x = y;
                coeff += 5u;
                section_state += 2u;
            }
            fip_output[ch][n] = x;
        }
    }
}

/* Peak and finiteness over the same stimulus set the H2 headroom report used, so
 * the host float64 internal peak 1.096476 and the final-output peak have an
 * on-target counterpart in the arithmetic the MCU actually performs. */
static void fip_headroom_report( void )
{
    static const char* const names[] = { "1k-sine", "10k-sine", "15k-sine",
                                         "17k-sine", "multitone", "impulse" };
    printf( "    numerical headroom, opt v1, six SOS (values are x full scale):\n" );
    for( uint8_t kind = 0u; kind < 6u; kind++ )
    {
        float peak_out   = 0.0f;
        float peak_state = 0.0f;
        uint8_t finite   = 1u;
        fip_reset();
        for( uint32_t block = 0u; block < 64u; block++ )
        {
            h2b_fill_stimulus( kind, block );
            fip_opt_v1_only();
            for( uint32_t ch = 0u; ch < FIP_CHANNELS; ch++ )
            {
                for( uint32_t n = 0u; n < FIP_BLOCK_FRAMES; n++ )
                {
                    const float v = fip_output[ch][n];
                    if( !isfinite( v ) ) { finite = 0u; }
                    else if( fabsf( v ) > peak_out ) { peak_out = fabsf( v ); }
                }
            }
            for( uint32_t k = 0u; k < FIP_STATE_FLOATS; k++ )
            {
                const float v = FIP_STATE[k];
                if( !isfinite( v ) ) { finite = 0u; }
                else if( fabsf( v ) > peak_state ) { peak_state = fabsf( v ); }
            }
        }
        {
            const uint32_t out_milli   = (uint32_t)( ( peak_out / H2B_FULL_SCALE ) * 1000.0f );
            const uint32_t state_milli = (uint32_t)( ( peak_state / H2B_FULL_SCALE ) * 1000.0f );
            printf( "      %-10s out=%lu.%03lu FS  state=%lu.%03lu FS  finite=%s\n",
                    names[kind],
                    (unsigned long)( out_milli / 1000u ), (unsigned long)( out_milli % 1000u ),
                    (unsigned long)( state_milli / 1000u ), (unsigned long)( state_milli % 1000u ),
                    ( finite != 0u ) ? "PASS" : "FAIL" );
        }
    }
}

void asrc_full_iir_precheck_bench_run( uint32_t trials )
{
    h2b_stats_t empty;
    h2b_stats_t opt_v1;
    h2b_stats_t local_c;
    uint32_t sp_probe = 0u;

    if( trials == 0u ) { trials = FIP_DEFAULT_TRIALS; }

    if( ( H2B_Y_ARENA <= (uintptr_t)&sp_probe ) ||
        ( ( H2B_Y_ARENA - (uintptr_t)&sp_probe ) < 4096u ) )
    {
        printf( "\n *ai REFUSING to run: Y scratch 0x%05lx, stack near 0x%05lx; 4096 B required.\n",
                (unsigned long)H2B_Y_ARENA, (unsigned long)(uintptr_t)&sp_probe );
        return;
    }

    h2b_stats_reset( &empty );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        h2b_stats_add( &empty, nora_high_res_timer_get_count() - t0 );
    }

    printf( "\n *ai Full-IIR precheck: six-SOS 48 kHz anti-alias LPF, IIR only\n" );
    printf( "    candidate=elliptic n12 fp15k rp0.25 rs120; SOS=%u; order=low-Q-first [0..5]\n",
            (unsigned)ASRC_FULL_IIR_SOS );
    printf( "    coeff CRC32(LE f32)=0x%08lX; max|coeff| x1e6=%lu\n",
            (unsigned long)ASRC_FULL_IIR_DF2T_SOS_CRC32,
            (unsigned long)( ASRC_FULL_IIR_MAX_ABS_COEFF * 1000000.0f ) );
    printf( "    geometry=%uch x %u frames/block; 48 kHz block deadline=333.33 us; trials=%lu\n",
            (unsigned)FIP_CHANNELS, (unsigned)FIP_BLOCK_FRAMES, (unsigned long)trials );
    printf( "    SOS evaluations/block=%lu; state=Y 0x%05lx..0x%05lx, coeff=X\n",
            (unsigned long)( FIP_CHANNELS * FIP_BLOCK_FRAMES * ASRC_FULL_IIR_SOS ),
            (unsigned long)(uintptr_t)FIP_STATE, (unsigned long)FIP_Y_ARENA_END );
    printf( "    timer pair ticks min/mean/max=%lu/%lu/%lu; values subtract the minimum.\n",
            (unsigned long)empty.min_ticks, (unsigned long)h2b_stats_mean( &empty ),
            (unsigned long)empty.max_ticks );

    /* Interrupts stay enabled, as in Phase 2/3: min is the uncontended floor and
     * mean/max keep real foreground preemption, so these numbers are directly
     * comparable to the five-SOS 79.00 / 128.64 / 213.65 us reference. */
    h2b_fill_stimulus( 1u, 0u );   /* 10 kHz sine: the host worst-headroom stimulus */
    fip_reset();
    for( uint32_t warm = 0u; warm < 8u; warm++ ) { fip_opt_v1_only(); }
    h2b_stats_reset( &opt_v1 );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        fip_opt_v1_only();
        h2b_stats_add( &opt_v1, nora_high_res_timer_get_count() - t0 );
    }

    fip_reset();
    for( uint32_t warm = 0u; warm < 8u; warm++ ) { fip_local_c_only(); }
    h2b_stats_reset( &local_c );
    for( uint32_t trial = 0u; trial < trials; trial++ )
    {
        const uint32_t t0 = nora_high_res_timer_get_count();
        fip_local_c_only();
        h2b_stats_add( &local_c, nora_high_res_timer_get_count() - t0 );
    }

    printf( "  six-SOS IIR, all %u channels:\n", (unsigned)FIP_CHANNELS );
    h2b_print_timing( "opt v1 6SOS", &opt_v1, empty.min_ticks );
    h2b_print_timing( "localC 6SOS", &local_c, empty.min_ticks );
    printf( "    reference, Phase-3 opt v1 FIVE SOS: 79.00 / 128.64 / 213.65 us (min/mean/max)\n" );
    printf( "    reference, Phase-2 local C FIVE SOS: 220.30 / 330.22 / 446.76 us\n" );
    printf( "    Phase-4 estimated six SOS by linear scaling: 154.20 us mean\n" );
    {
        /* The marginal cost of the sixth section against the Phase-3 five-SOS
         * mean, differenced in ticks so the printed value keeps the 0.01 us
         * resolution of the rows above rather than a re-rounded difference. */
        const uint32_t mean_ticks = h2b_subtract_overhead( h2b_stats_mean( &opt_v1 ),
                                                           empty.min_ticks );
        const uint32_t per_sos    = mean_ticks / ASRC_FULL_IIR_SOS;
        const uint32_t delta      = ( mean_ticks >= FIP_PHASE3_FIVE_SOS_MEAN_TICKS )
                                    ? ( mean_ticks - FIP_PHASE3_FIVE_SOS_MEAN_TICKS )
                                    : ( FIP_PHASE3_FIVE_SOS_MEAN_TICKS - mean_ticks );
        printf( "    measured mean=%lu.%02lu us -> %lu.%02lu us/SOS; sixth-section marginal "
                "vs Phase-3 five-SOS mean=%c%lu.%02lu us\n",
                (unsigned long)( mean_ticks / 100u ), (unsigned long)( mean_ticks % 100u ),
                (unsigned long)( per_sos / 100u ), (unsigned long)( per_sos % 100u ),
                ( mean_ticks >= FIP_PHASE3_FIVE_SOS_MEAN_TICKS ) ? '+' : '-',
                (unsigned long)( delta / 100u ), (unsigned long)( delta % 100u ) );
    }

    fip_headroom_report();
    printf( "    scope: IIR only.  Front-end removal and generic ASRC step 1.0 -> 1.5 are\n" );
    printf( "           measured as runtime telemetry across two images, not benched here.\n" );
    printf( " *ai complete\n" );
}

#endif /* ASRC_H2_KERNEL_BENCH_AVAILABLE */

// ================================================================================================
// "*an" -- THIRD-BAND (3-path polyphase allpass) 96 -> 32 kHz DECIMATOR KERNEL COST
//
// Host feasibility ([internal] report_96to32_thirdband_stage0_host_gate_2026-09-10.md) put the
// filter at
// K = 16 first-order allpass sections for -110.33 dBc worst alias into 0-15 kHz, i.e. 16
// multiplies per 32 kHz output per channel against the shipping front end's 110.  That is a
// MULTIPLY count.  The Full-IIR trial already showed a kernel with FEWER multiplies losing on
// hardware, so the multiply ratio is not the answer -- this probe measures the real thing.
//
// WHAT IS INSIDE THE MEASURED WINDOW (deliberately, per owner instruction 2026-09-09):
//   16ch channel-major layout, state load, coefficient load, the allpass arithmetic, state
//   store, the 3-branch commutator input fetch, the branch sum, the output store, and every
//   loop / address update the C form needs.  NOT an extracted MAC instruction count.
//
// WHAT IS OUTSIDE: the ring, the M30 resampler, gather/scatter from TDM.  Those are measured
// separately and added -- new_front_end_measured + existing_M30_measured -- exactly so this
// number cannot absorb someone else's cost and hide it.
//
// TWO NUMBERS, BOTH PRINTED.  The SLOPE between 8 and 16 sections cancels the per-frame fixed
// work (branch fetch, sum, output store, call, timer read) and is the marginal cost of one
// section.  The ABSOLUTE per-output cost at 16 sections includes that fixed work and is the
// figure the CPU budget takes.  Reporting only the slope would flatter the structure; only the
// absolute would hide where the cost sits.
//
// SELF-CALIBRATION.  Every cycles figure here depends on FIRB_CYC_PER_TICK, and getting that
// constant wrong halves or doubles the verdict (it has flipped a conclusion before).  So the
// probe re-derives it from hardware in the same run: fir_ring_q31 costs `taps + 9`
// instructions for `taps` MACs, so its measured slope MUST come out at 1.000 cycles/MAC.  If
// the printed calibration is not ~1.0, every other line in this block is suspect and the run
// says so instead of being quietly wrong.
//
// The allpass coefficients are the host design's (K=16, split 6/5/5).  They are here so the
// arithmetic is the real one -- same magnitudes, same pole radii -- not to qualify a response;
// the response was qualified on host and is not re-measured on hardware by this probe.
// ================================================================================================

#define FIRN_CH              16u     /* the shipping channel count; the interleave IS the point */
#define FIRN_SEC_HI          16u     /* K = 16: the -110.33 dBc design                          */
#define FIRN_SEC_LO           8u     /* second point for the slope                              */
#define FIRN_FRAMES          16u     /* 32 kHz output frames per measured block (one TDM block) */
#define FIRN_DEFAULT_TRIALS  2000u

/* State as [section][channel][{x1, y1}].  Section-major with the channels contiguous is the
 * layout the kernel wants: the inner loop walks 16 independent chains, so the serial dependency
 * inside one channel's allpass is covered by the other 15 and the addressing is sequential.
 * Q31 and float32 never run at the same time (both foreground, one after the other), so they
 * share the storage -- X and Y data space are both full in this profile and a second 2 KiB
 * array does not link.  Re-initialised before every timed run. */
static union
{
    int32_t q[FIRN_SEC_HI][FIRN_CH][2];
    float   f[FIRN_SEC_HI][FIRN_CH][2];
} firn_state;

static union { int32_t q[3][FIRN_CH];    float f[3][FIRN_CH];    } firn_in;    /* commutator      */
static union { int32_t q[FIRN_CH];       float f[FIRN_CH];       } firn_out;
static union { int32_t q[FIRN_SEC_HI];   float f[FIRN_SEC_HI];   } firn_coeff;
static int32_t firn_sink;

/* K=16 host design, split (6,5,5), worst alias -110.33 dBc.  a in (0,1), max radius 0.985785. */
static const float firn_a_ref[FIRN_SEC_HI] = {
    0.011490812985f, 0.447101337588f, 0.874610039108f, 0.985785414145f, 0.167166189967f,
    0.698656770620f,                                              /* A0: 6 sections */
    0.255731339137f, 0.765400415593f, 0.918382305447f, 0.040009283243f, 0.538698856130f,
                                                                  /* A1: 5 sections */
    0.350601409952f, 0.823087316318f, 0.959753700903f, 0.623425043330f, 0.094518171863f
                                                                  /* A2: 5 sections */
};

static void firn_split( uint32_t sections, uint32_t nb[3] )
{
    nb[0] = ( sections + 2u ) / 3u;
    nb[1] = ( sections + 1u ) / 3u;
    nb[2] = sections - nb[0] - nb[1];
}

/* HEADROOM is handled by input scaling, not by per-sample shifts.  The host study measured the
 * 1-multiplier allpass internal node at 2.09x the input amplitude, so the Q31 form needs ~1.1
 * guard bits.  The shipping path already folds a constant gain into coefficients at zero
 * instruction cost (the -1.500 dB headroom fold in the Full-IIR work), so the realistic kernel
 * pays nothing per sample for this and the probe must not either -- adding two shifts per
 * section here would measure a choice nobody would make. */
static void firn_prepare( uint32_t sections, int use_float )
{
    uint32_t s = 0u;
    (void)sections;
    for( s = 0u; s < FIRN_SEC_HI; s++ )
    {
        if( use_float ) { firn_coeff.f[s] = firn_a_ref[s]; }
        else            { firn_coeff.q[s] = (int32_t)( firn_a_ref[s] * 2147483648.0f ); }
    }
    for( s = 0u; s < FIRN_SEC_HI; s++ )
    {
        for( uint32_t c = 0u; c < FIRN_CH; c++ )
        {
            if( use_float ) { firn_state.f[s][c][0] = 0.0f; firn_state.f[s][c][1] = 0.0f; }
            else            { firn_state.q[s][c][0] = 0;    firn_state.q[s][c][1] = 0;    }
        }
    }
    for( uint32_t c = 0u; c < FIRN_CH; c++ ) { firn_out.q[c] = 0; }
    for( uint32_t b = 0u; b < 3u; b++ )
    {
        for( uint32_t c = 0u; c < FIRN_CH; c++ )
        {
            /* 0.25 full scale: 6 dB of headroom for the 2.09x internal node, deterministic and
             * different per branch/channel so nothing cancels by accident. */
            const float v = 0.25f * ( ( ( (int32_t)( ( b * 37u ) + ( c * 11u ) ) % 19 ) - 9 ) / 9.0f );
            if( use_float ) { firn_in.f[b][c] = v; }
            else            { firn_in.q[b][c] = (int32_t)( v * 2147483648.0f ); }
        }
    }
}

// ---- the kernels under test ---------------------------------------------------------------------
// Q31: y = a*(x - y1) + x1, one multiply and two adds per section, states x1/y1 in Q31.
static void firn_kernel_q31( uint32_t sections, uint32_t frames )
{
    uint32_t nb[3];
    firn_split( sections, nb );

    for( uint32_t f = 0u; f < frames; f++ )
    {
        int32_t  acc[FIRN_CH];
        uint32_t s = 0u;

        for( uint32_t b = 0u; b < 3u; b++ )
        {
            int32_t sig[FIRN_CH];
            for( uint32_t c = 0u; c < FIRN_CH; c++ ) { sig[c] = firn_in.q[b][c]; }

            for( uint32_t k = 0u; k < nb[b]; k++, s++ )
            {
                const int32_t a = firn_coeff.q[s];
                for( uint32_t c = 0u; c < FIRN_CH; c++ )
                {
                    const int32_t x  = sig[c];
                    const int32_t y1 = firn_state.q[s][c][1];
                    const int32_t w  = x - y1;
                    const int32_t y  = (int32_t)( ( (int64_t)a * (int64_t)w ) >> 31 )
                                       + firn_state.q[s][c][0];
                    firn_state.q[s][c][0] = x;
                    firn_state.q[s][c][1] = y;
                    sig[c]                = y;
                }
            }
            for( uint32_t c = 0u; c < FIRN_CH; c++ )
            { acc[c] = ( b == 0u ) ? sig[c] : ( acc[c] + sig[c] ); }
        }
        /* The 1/3 is folded into the downstream resampler's coefficients in the real chain, so
         * it costs no instruction here and none is charged. */
        for( uint32_t c = 0u; c < FIRN_CH; c++ ) { firn_out.q[c] = acc[c]; }
    }
}

static void firn_kernel_f32( uint32_t sections, uint32_t frames )
{
    uint32_t nb[3];
    firn_split( sections, nb );

    for( uint32_t f = 0u; f < frames; f++ )
    {
        float    acc[FIRN_CH];
        uint32_t s = 0u;

        for( uint32_t b = 0u; b < 3u; b++ )
        {
            float sig[FIRN_CH];
            for( uint32_t c = 0u; c < FIRN_CH; c++ ) { sig[c] = firn_in.f[b][c]; }

            for( uint32_t k = 0u; k < nb[b]; k++, s++ )
            {
                const float a = firn_coeff.f[s];
                for( uint32_t c = 0u; c < FIRN_CH; c++ )
                {
                    const float x  = sig[c];
                    const float y1 = firn_state.f[s][c][1];
                    const float y  = ( a * ( x - y1 ) ) + firn_state.f[s][c][0];
                    firn_state.f[s][c][0] = x;
                    firn_state.f[s][c][1] = y;
                    sig[c]                = y;
                }
            }
            for( uint32_t c = 0u; c < FIRN_CH; c++ )
            { acc[c] = ( b == 0u ) ? sig[c] : ( acc[c] + sig[c] ); }
        }
        for( uint32_t c = 0u; c < FIRN_CH; c++ ) { firn_out.f[c] = acc[c]; }
    }
}

// ---- correctness, so the probe cannot time a wrong answer ----------------------------------------
// One channel, one frame, double reference over the same coefficients and the same cascade order.
/* Defined with part 2 below, and used by BOTH parts so there is exactly one double model of this
 * cascade in the file.  It was duplicated here at first; two copies of a reference is a way to have
 * one arm silently held to a different filter from another. */
static double firn_ref_ch0( uint32_t sections );

static void firn_check( uint32_t sections, float* q31_err, float* f32_err )
{
    /* PREPARE FIRST, then build the reference from the SAME inputs.  Reading firn_in
     * before firn_prepare() filled it made the reference garbage -- this project places
     * plenty of statics in .nbss (no startup clear), so "it is a static, it is zero" does
     * not hold here.  The first run of this probe reported f32 err = Q31 err = 1.9e16,
     * identical for both formats, which is the signature of a broken SHARED reference
     * rather than two broken kernels. */
    firn_prepare( sections, 1 );

    const double ref_acc = firn_ref_ch0( sections );

    firn_prepare( sections, 1 );          /* re-zero the state the reference just modelled */
    firn_kernel_f32( sections, 1u );
    *f32_err = (float)( (double)firn_out.f[0] - ref_acc );

    firn_prepare( sections, 0 );
    firn_kernel_q31( sections, 1u );
    *q31_err = (float)( ( (double)firn_out.q[0] / 2147483648.0 ) - ref_acc );
}

// ---- statistics ---------------------------------------------------------------------------------
typedef struct
{
    uint32_t min_t;
    uint32_t max_t;
    uint64_t sum_t;
    uint32_t n;
} firn_stats_t;

/* Interrupts off inside the window: this is a KERNEL floor, not a system measurement, and the
 * TDM ISRs at ~75% duty would otherwise be what min/mean/max describes.  The window is one
 * 16-frame block, so it is bounded well inside a TDM period.  min is the uncontended floor;
 * mean/max show DMA bus stealing, which cannot be masked. */
#define FIRN_MEASURE( stmt, st )                                                      \
    do {                                                                              \
        ( st ).min_t = 0xFFFFFFFFu; ( st ).max_t = 0u; ( st ).sum_t = 0u; ( st ).n = 0u; \
        for( uint32_t firn_i = 0u; firn_i < trials; firn_i++ )                        \
        {                                                                             \
            __builtin_disable_interrupts();                                           \
            const uint32_t firn_t0 = nora_high_res_timer_get_count();                 \
            stmt;                                                                     \
            const uint32_t firn_d = nora_high_res_timer_get_count() - firn_t0;         \
            __builtin_enable_interrupts();                                            \
            if( firn_d < ( st ).min_t ) { ( st ).min_t = firn_d; }                     \
            if( firn_d > ( st ).max_t ) { ( st ).max_t = firn_d; }                     \
            ( st ).sum_t += firn_d;                                                   \
            ( st ).n++;                                                               \
        }                                                                             \
    } while( 0 )

static uint32_t firn_mean( const firn_stats_t* s )
{
    return ( s->n == 0u ) ? 0u : (uint32_t)( s->sum_t / s->n );
}

/* x1000 to keep two decimals without floating point in the report path. */
static uint32_t firn_per_x1000( uint32_t ticks, uint32_t units )
{
    if( units == 0u ) { return 0u; }
    return (uint32_t)( ( (uint64_t)ticks * FIRB_CYC_PER_TICK * 1000u ) / units );
}

static void firn_print_x1000( const char* label, uint32_t v_x1000, const char* unit )
{
    printf( "      %-28s %lu.%03lu %s\n", label, (unsigned long)( v_x1000 / 1000u ),
            (unsigned long)( v_x1000 % 1000u ), unit );
}

// ================================================================================================
// "*an" PART 2 -- the three kernels the 2026-09-09 owner revision asks for.
//
//   A  hand-written Q31 asm, 1 multiply / 2 state words per section
//   B  hand-written Q31 asm, 2 multiplies / 1 state word per section
//   C  the SHIPPING 96 -> 32 kHz front end, in the SAME measured window
//
// WHY PART 2 EXISTS AT ALL.  Part 1 (above) measured the C form and lost: 18.641
// cycles/section in Q31, 11.323 in float32, against a shipping front end priced at 110.0
// multiplies x 1.012 measured cycles/MAC = 111.3 cycles per 32 kHz output per channel.  Two
// things were wrong with stopping there, and the owner named both:
//
//   1. THE COMPARISON WAS C AGAINST ASSEMBLER.  111.3 comes from fir_ring_q31, a hand-written
//      REPEAT + MAC.l loop sitting on the ISA floor.  Part 1's kernels are plain C, and the Q31
//      arm in particular never reached the accumulator at all -- `(int32_t)(((int64_t)a*w)>>31)`
//      compiles to a 64-bit multiply and a shift sequence, which is why Q31 measured SLOWER than
//      float32 there.
//   2. THE COMPARISON WAS NOT APPLE-TO-APPLE.  111.3 is a MULTIPLY price for the FIR arithmetic
//      only.  It excludes the 41-tap stage's ring push, the intermediate 48 kHz buffer, the 2/3
//      phase schedule and the s24-left scatter -- all of which the shipping chain really pays and
//      none of which the third-band structure needs.  Part 1's 205.6/324.0, by contrast, already
//      included its commutator, branch sum and output store.  So the sign of the comparison was
//      not actually known.
//
// Hence kernel C: the shipping chain, driven by this same harness, over this same 16-output
// window, with its ring push and its intermediate buffer inside the timer.  It is the number the
// verdict is taken against, and it is the only figure here that can be compared to A and B
// without an argument about what is in whose window.
//
// ★ THE FINDING THAT CAME OUT OF WRITING THE ASSEMBLER, and it reframes the study: dsPIC33A has
// MIXED register x memory MAC forms.  `mac.l [w11], w4, A` and `msc.l [w11], [w9], A` are each ONE
// 4-byte instruction (assembled and disassembled to check, not assumed).  So a memory operand of a
// multiply costs no separate load.  Part 1 concluded that an allpass pays "four memory accesses per
// multiply against the FIR's two" and called that the structural floor.  On this ISA those four
// accesses are only TWO instructions -- the two stores -- because both loads ride inside the
// accumulator ops.  That is why kernel B, the deliberate counter-bet that halves the memory traffic
// by doubling the multiplies, is SLOWER than A in static instruction count (7.0 against 6.0 per
// section per channel) rather than faster: B saves one store and pays one extra sacr.l and one
// extra lac.l for it.  B is measured anyway, because instruction count is not cycle count when the
// load/store unit is the contended resource -- which was the whole hypothesis.
// ================================================================================================

// ---- the assembler kernels (asrc_thirdband_kernel_dspic33ak.s) ---------------------------------
// state:  Y space.  A is [ch][section][{x1,y1}] (8 B/section); B is [ch][section] (4 B/section).
// coeff:  X space, `sections` Q31 entries.  X-vs-Y is not a preference -- two MAC operands in ONE
//         space serialise and the instruction silently costs an extra cycle (DS70005591C 4.3.17,
//         measured elsewhere in this tree as 1.012 -> 2.000 cycles/MAC).  Checked below.
// io:     in[3][FIRN_CH] followed by out[FIRN_CH], one contiguous buffer -- the kernels address all
//         six commutator reads and both output writes off one register by displacement.
extern void asrc_tb_a_q31_k16( int32_t* state, const int32_t* coeff, int32_t* io, uint32_t frames );
extern void asrc_tb_a_q31_k8 ( int32_t* state, const int32_t* coeff, int32_t* io, uint32_t frames );
extern void asrc_tb_b_q31_k16( int32_t* state, const int32_t* coeff, int32_t* io, uint32_t frames );
extern void asrc_tb_b_q31_k8 ( int32_t* state, const int32_t* coeff, int32_t* io, uint32_t frames );

// ---- the shipping FIR kernel, declared here for the same reason asrc_decimator_q31.inc gives ----
extern int32_t* fir_ring_q31_ymod_yonly_block( const int32_t* coeff, const int32_t* hist,
                                               uint32_t taps, int32_t* out, uint32_t outputs,
                                               uint32_t decim_bytes, const int32_t* ring,
                                               uint32_t ring_bytes );

// ---- the shipping coefficients, not a transcription of them -------------------------------------
// Included rather than copied so the baseline runs the REAL filter: 41 taps for 96 -> 48 kHz and
// the two L=2 phase rows of the 97-tap 48 -> 32 kHz prototype.  The 96 -> 48 header also carries
// the two WIDE variants (113 and 169 taps) that serve rows where the pre-stage is the whole chain;
// this bench composes a second stage behind it and so uses the SHARED set, exactly as
// asrc_decimator_q31_init() does for a 96 -> 32 kHz row.  The wide sets are therefore unused here,
// which is what the diagnostic pragma is for -- suppressed narrowly, over the includes only, so a
// genuinely dead static anywhere else in this file still warns.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "asrc_decimator_96_to_48_coeffs.inc"
#include "asrc_decimator_48_to_32_coeffs.inc"
#pragma GCC diagnostic pop

// ---- where part 2's operands live ---------------------------------------------------------------
// SAME arrangement, and the same reasons, as the FIRB arena above: a declared object in Y breaks
// the serial-update layout gate either way it is placed, so the Y-side operands are addressed
// directly inside the region the linker already gave the stack, below the reset-diagnostic block.
// Nothing is added to the image layout.
//
// The asm arms and the baseline OVERLAP deliberately.  They are separate arms of one console
// command, run strictly one after another, and each re-initialises every byte it reads before it
// reads it.  X and Y are both full in this profile (part 1's notes record that a second 2 KiB array
// does not link), so overlapping is not a shortcut here -- it is the only way both fit.
// * PART 2 HAS ITS OWN ARENA BASE, AND THE REASON IS A REAL HAZARD IN THIS PROFILE.
// FIRB_Y_ARENA is 0x11000, and in APP_BUILD_ASRC_CODEC_MEAS_HEADROOM_M30 the application's .bss
// ends at 0x11038 and SPLIM is 0x13DC0 -- i.e. THE STACK IS the region 0x11038..0x13E00, and
// FIRB_RING_POOL at 0x11000 sits exactly where the live stack is.  Part 1's arms never noticed
// because they use declared statics, not the arena.  Part 2's first attempt (2026-09-09 19:08) took
// the arena from offset 0 and died with a BUS ERROR at PC=840BE4, SP=0x117D8 -- the measurement
// overwrote its own return addresses.  The bench's existing headroom check did not catch it because
// `FIRB_Y_ARENA - (uintptr_t)&sp_probe` UNDERFLOWS when the stack is above the arena base, and it
// printed 4294966712 B of "headroom".
//
// So part 2 works DOWNWARD from the top of the region instead, leaving the whole low end to the
// stack, and it verifies that with a CANARY rather than an estimate (firn_p2_canary_* below).  The
// part 1 constants are left exactly as they are: moving FIRB_Y_ARENA would relocate every other
// arm's operands, and in this tree a placement change moves even an untouched stage by several
// microseconds -- their published numbers would stop being comparable.  The hazard is reported.
#define FIRN_P2_END      0x13E00u                       /* .resident_far_sentinel starts here     */
#define FIRN_P2_BYTES    0x2580u                        /* 9600 B; the map is below               */
#define FIRN_P2_ARENA    ( FIRN_P2_END - FIRN_P2_BYTES ) /* 0x11880                                */
#define FIRN_P2_AT(off)  ( (int32_t*)( FIRN_P2_ARENA + (off) ) )

#define FIRN_ASM_STATE   FIRN_P2_AT( 0x0000u )   /* 2048 B: A's [16ch][16sec][{x1,y1}]            */
#define FIRN_ASM_IO      FIRN_P2_AT( 0x0800u )   /*  256 B: in[3][16] then out[16]                */

#define FIRC_NCH      16u      /* the shipping channel count, as in part 1                       */
#define FIRC_T1       41u      /* 96 -> 48 kHz, ASRC_DECIMATOR_96_TO_48_COEFF_TAPS               */
#define FIRC_D1        2u
#define FIRC_R1LEN    56u      /* = span+1: 41 taps + 7 hops of 2, the shipping batch of 8       */
#define FIRC_BLK      16u      /* 96 kHz frames per shipping block (DEC_Q31_R23_MAX_IN)          */
#define FIRC_B1        8u      /* 48 kHz frames one block yields (DEC_Q31_MAX_MID)               */
#define FIRC_T_P0     49u      /* ASRC_DECIMATOR_48_TO_32_PHASE0_COEFF_TAPS                      */
#define FIRC_T_P1     48u      /* ASRC_DECIMATOR_48_TO_32_PHASE1_COEFF_TAPS                      */
#define FIRC_D2        3u      /* ASRC_DECIMATOR_48_TO_32_M                                      */
#define FIRC_R2LEN    64u      /* DEC_Q31_R23_RING -- one block plus the longest row's window    */
#define FIRC_BLOCKS    3u      /* 48 in -> 24 mid -> 16 out: part 1's window, exactly            */
#define FIRC_OUTS      6u      /* out32 BUFFER depth: outputs one shipping block yields at most */
/* ...and the number of 32 kHz outputs the whole measured window produces, which is what the
 * per-output figures divide by.  3 blocks of 8 intermediate frames at 2/3 = 6+5+5.  Keeping these
 * two apart is not pedantry: they were the same symbol until 2026-09-09, and shrinking the buffer to
 * fit the arena then multiplied every baseline figure by 2.667 without changing a unit label. */
#define FIRC_WINDOW_OUTS  16u
_Static_assert( FIRC_WINDOW_OUTS == FIRN_FRAMES,
                "the baseline window must be the same 16 outputs part 1 and the asm arms measure" );

/* The map.  The two RINGS are the only objects here that MUST be in Y -- they are the FIR kernel's
 * Y-modulo `hist` operand, read opposite coefficients in X.  The rest are reached by plain mov.l and
 * could live anywhere; they are here because nowhere else is free (X is full and .bss ends at
 * 0x11038).  The asm arms' state and io overlay R1: they never run at the same time as the baseline,
 * and every arm re-initialises each byte it reads before reading it. */
#define FIRC_R1       FIRN_P2_AT( 0x0000u )   /* 16 x 56 x 4 = 3584 B  Y, stage-1 modulo ring      */
#define FIRC_R2       FIRN_P2_AT( 0x0E00u )   /* 16 x 64 x 4 = 4096 B  Y, the 2/3 rows' ring       */
#define FIRC_MID      FIRN_P2_AT( 0x1E00u )   /*  8 x 16 x 4 =  512 B  intermediate 48 kHz block   */
#define FIRC_IN96     FIRN_P2_AT( 0x2000u )   /* 16 x 16 x 4 = 1024 B  the 96 kHz input block      */
#define FIRC_OUT32    FIRN_P2_AT( 0x2400u )   /*  6 x 16 x 4 =  384 B  ONE block's 32 kHz outputs  */
/* The reference's arrays, overlaying R1: firc_check() runs after the chain has produced its outputs
 * and reads only FIRC_IN96 and FIRC_OUT32, so R1 is dead by then.  They are here rather than on the
 * stack because in this profile the STACK is the scarce thing, not the arena.
 *
 * * THE BASE GOES THROUGH A VOLATILE, AND THAT IS A TOOLCHAIN WORKAROUND, NOT A STYLE CHOICE.
 * Every other pointer here is a folded absolute address and compiles fine.  A FLOATING-POINT store
 * to one does not: `yr[i] = 0.0` made xc-dsc v3.31.01 emit
 *     unrecognizable insn: (set (mem:SF (const_int 72384 [0x11ac0])) (reg:SF 3626))
 * and then `internal compiler error: in extract_insn, at recog.c:2339` during the vregs pass.
 * Reading the base from a volatile keeps the address out of the instruction, which the backend can
 * encode, at the cost of one load per array. */
static volatile uintptr_t firn_p2_fp_base = FIRN_P2_ARENA;

#define FIRC_REF_X    ( (double*)( firn_p2_fp_base + 0x0000u ) )   /* 48 doubles */
#define FIRC_REF_U    ( (double*)( firn_p2_fp_base + 0x0180u ) )   /* 24 doubles */
#define FIRC_REF_Y    ( (double*)( firn_p2_fp_base + 0x0240u ) )   /*  6 doubles */

_Static_assert( FIRN_P2_ARENA >= 0xC000u, "part 2's arena is not in Y data space" );
_Static_assert( ( FIRN_P2_ARENA + FIRN_P2_BYTES ) <= 0x13E00u,
                "part 2's arena runs into the reset-diagnostic block" );
_Static_assert( ( FIRN_P2_ARENA % 8u ) == 0u, "the double reference arrays need 8-byte alignment" );

/* THE CANARY.  Everything above is an argument that the stack cannot reach 0x11880 while a
 * measurement is running; this is the check that the argument held.  256 bytes immediately below the
 * arena are stamped before the runs and verified after, so a stack that grew into them is reported
 * as a broken measurement instead of becoming a plausible number -- which is the failure mode this
 * arena arrangement has, and the one that already cost a build once. */
#define FIRN_P2_CANARY_BYTES  256u
#define FIRN_P2_CANARY   ( (int32_t*)( FIRN_P2_ARENA - FIRN_P2_CANARY_BYTES ) )
#define FIRN_P2_CANARY_W  ( FIRN_P2_CANARY_BYTES / 4u )

static void firn_p2_canary_set( void )
{
    for( uint32_t i = 0u; i < FIRN_P2_CANARY_W; i++ )
    { FIRN_P2_CANARY[i] = (int32_t)( 0xC0DE0000u + i ); }
}

static uint32_t firn_p2_canary_worst( void )
{
    /* Returns how many bytes of the canary the stack reached, 0 if none.  Counted from the TOP of
     * the canary (nearest the arena) so the number is "how close the stack came to the data". */
    uint32_t reached = 0u;
    for( uint32_t i = 0u; i < FIRN_P2_CANARY_W; i++ )
    {
        if( FIRN_P2_CANARY[i] != (int32_t)( 0xC0DE0000u + i ) )
        { reached = ( FIRN_P2_CANARY_W - i ) * 4u; }
    }
    return reached;
}
_Static_assert( FIRN_CH == FIRC_NCH, "the asm kernels are assembled for FIRN_CH channels" );
_Static_assert( FIRN_SEC_HI == 16u, "the asm entry points are the K=16 / K=8 pair" );
_Static_assert( FIRN_SEC_LO == 8u,  "the asm entry points are the K=16 / K=8 pair" );
_Static_assert( FIRC_R1LEN >= ( FIRC_T1 + ( ( FIRC_B1 - 1u ) * FIRC_D1 ) ),
                "stage 1's ring cannot hold the window its batch reads" );
_Static_assert( FIRC_BLOCKS * FIRC_BLK == 48u, "the window is 48 input frames at 96 kHz" );

// Coefficients, all in X, laid into firb_coeff[] AFTER the calibration has finished with it (the
// calibration uses [0..189]).  A separate space(xmemory) array would be cleaner and does not link:
// X is full, and asking for another 616 B moves that much application data into Y instead.
#define FIRN_XC_ALLPASS    0u    /* 16 allpass a, Q31            */
#define FIRC_XC_S1        16u    /* 41 taps, 96 -> 48 kHz        */
#define FIRC_XC_P0        57u    /* 49 taps, phase 0             */
#define FIRC_XC_P1       106u    /* 48 taps, phase 1  -> 154     */
_Static_assert( ( FIRC_XC_P1 + FIRC_T_P1 ) <= 256u, "part 2's coefficients do not fit firb_coeff" );

/* Same conversion the shipping front end uses (dec_q31_from_float): round-half-away-from-zero with
 * explicit saturation.  Copied rather than shared because that one is static inside the .inc; a
 * different rounding here would make the baseline a different filter from the shipping one. */
static int32_t firc_q31( float c )
{
    const float v = c * 2147483648.0f;
    if( v >=  2147483647.0f ) { return (int32_t)0x7FFFFFFF; }
    if( v <= -2147483648.0f ) { return (int32_t)0x80000000; }
    return (int32_t)( ( v >= 0.0f ) ? ( v + 0.5f ) : ( v - 0.5f ) );
}

static void firn_load_x_coeff( void )
{
    for( uint32_t s = 0u; s < FIRN_SEC_HI; s++ )
    { firb_coeff[FIRN_XC_ALLPASS + s] = firc_q31( firn_a_ref[s] ); }
    for( uint32_t k = 0u; k < FIRC_T1;   k++ )
    { firb_coeff[FIRC_XC_S1 + k] = firc_q31( s_96_to_48_coeff[k] ); }
    for( uint32_t k = 0u; k < FIRC_T_P0; k++ )
    { firb_coeff[FIRC_XC_P0 + k] = firc_q31( s_48_to_32_phase0_coeff[k] ); }
    for( uint32_t k = 0u; k < FIRC_T_P1; k++ )
    { firb_coeff[FIRC_XC_P1 + k] = firc_q31( s_48_to_32_phase1_coeff[k] ); }
}

// ---- the asm arms' state and input -------------------------------------------------------------
// `shift` exists for kernel B and is a FINDING, not a convenience.  B's single-state form carries
// its internal node through 1/(1 + a z^-1), whose peak magnitude is 1/(1-a).  The K=16 design's
// deepest pole is a = 0.985785, so that node has 70.4x = +37 dB of gain at Nyquist -- against the
// 2.09x = +6.4 dB the host study measured for A's node.  B therefore needs about seven guard bits
// where A needs two, and at Q31 it simply cannot carry a 0.25 FS input: it saturates, and the
// correctness gate would (correctly) refuse to time it.  So B is driven 8 bits down and judged on
// RELATIVE error.  Cycles are unaffected -- sacr.l saturates in constant time -- but a shipping
// version of B would have to widen the state, which is a second, independent reason it loses.
static void firn_asm_prepare( uint32_t shift )
{
    int32_t* st = FIRN_ASM_STATE;
    int32_t* io = FIRN_ASM_IO;
    for( uint32_t i = 0u; i < ( FIRN_CH * FIRN_SEC_HI * 2u ); i++ ) { st[i] = 0; }
    for( uint32_t b = 0u; b < 3u; b++ )
    {
        for( uint32_t c = 0u; c < FIRN_CH; c++ )
        { io[( b * FIRN_CH ) + c] = firn_in.q[b][c] >> shift; }
    }
    for( uint32_t c = 0u; c < FIRN_CH; c++ ) { io[( 3u * FIRN_CH ) + c] = 0; }
}

/* The double reference for one channel, one frame, of the same cascade -- extracted from
 * firn_check() so the asm arms are held to the SAME model the C arms were, rather than to the C
 * arms' output.  Requires firn_prepare(sections, 1) to have filled firn_in.f first. */
static double firn_ref_ch0( uint32_t sections )
{
    uint32_t nb[3];
    firn_split( sections, nb );

    double ref_state[FIRN_SEC_HI][2];
    for( uint32_t i = 0u; i < FIRN_SEC_HI; i++ ) { ref_state[i][0] = 0.0; ref_state[i][1] = 0.0; }

    double   acc = 0.0;
    uint32_t s   = 0u;
    for( uint32_t b = 0u; b < 3u; b++ )
    {
        double sig = (double)firn_in.f[b][0];
        for( uint32_t k = 0u; k < nb[b]; k++, s++ )
        {
            const double a  = (double)firn_a_ref[s];
            const double x  = sig;
            const double y1 = ref_state[s][1];
            const double y  = ( a * ( x - y1 ) ) + ref_state[s][0];
            ref_state[s][0] = x;
            ref_state[s][1] = y;
            sig             = y;
        }
        acc += sig;
    }
    return acc;
}

// ---- kernel C: the shipping chain, replayed ----------------------------------------------------
// This is the shipping structure, not an approximation of it: dec_q31_hist_push()'s channel-outer
// constant-stride push, dec_q31_stage_run()'s one-kernel-call-per-channel batch of 8, and
// dec_q31_r23_run()'s two phase rows with their d/parity schedule and interleaved output slots.
// The geometry is fixed at compile time here because this bench serves ONE rate pair, where the
// shipping code serves every one; nothing else differs, and the coefficients are the same objects.
//
// It cannot call asrc_decimator_q31_process_s24_left() directly, which would have been better: the
// 96 -> 48 kHz pre-stage is compiled only under APP_USE_96K_RATE, and the MEAS_HEADROOM_M30 preset
// this bench must run in (part 1: the BIDIR preset has no data space for the bench) does not set
// it.  Building the bench into a 96 kHz preset instead would change ASRC_CH to 8 and move every
// placement, which is exactly the "an untouched stage moves several us on a Y-placement change"
// trap this tree recorded on 2026-09-06.
static uint16_t firc_wr1;
static uint16_t firc_wr2;
static int8_t   firc_d;
static uint8_t  firc_par;

static void firc_push( int32_t* base, uint16_t R, uint16_t* wr,
                       const int32_t* in, uint32_t n, uint32_t in_stride )
{
    const uint16_t w0 = *wr;
    int32_t*       b  = base;
    for( uint32_t c = 0u; c < FIRC_NCH; c++ )
    {
        const int32_t* src = in + c;
        int32_t*       dst = b + w0;
        int32_t* const end = b + R;
        for( uint32_t k = 0u; k < n; k++ )
        {
            *dst = *src;
            src += in_stride;
            ++dst;
            if( dst == end ) { dst = b; }
        }
        b += R;
    }
    *wr = (uint16_t)( ( (uint32_t)w0 + n ) % (uint32_t)R );
}

static void firc_stage1_block( void )
{
    firc_push( FIRC_R1, FIRC_R1LEN, &firc_wr1, FIRC_IN96, FIRC_BLK, FIRC_NCH );

    const uint16_t span = (uint16_t)( FIRC_T1 + ( ( FIRC_B1 - 1u ) * FIRC_D1 ) );
    const uint16_t w0   = (uint16_t)( ( (uint32_t)firc_wr1 + FIRC_R1LEN - span ) % FIRC_R1LEN );
    int32_t*       ring = FIRC_R1;
    int32_t*       dst  = FIRC_MID;

    for( uint32_t c = 0u; c < FIRC_NCH; c++ )
    {
        int32_t tmp[FIRC_B1];
        (void)fir_ring_q31_ymod_yonly_block( &firb_coeff[FIRC_XC_S1], ring + w0, FIRC_T1,
                                             tmp, FIRC_B1, FIRC_D1 * 4u,
                                             ring, FIRC_R1LEN * 4u );
        /* Never masked: the phase rows are downstream and must get full-precision Q31. */
        int32_t* o = dst;
        for( uint32_t k = 0u; k < FIRC_B1; k++ ) { *o = tmp[k]; o += FIRC_NCH; }
        ring += FIRC_R1LEN;
        ++dst;
    }
}

static uint32_t firc_r23_block( int32_t* out )
{
    firc_push( FIRC_R2, FIRC_R2LEN, &firc_wr2, FIRC_MID, FIRC_B1, FIRC_NCH );

    /* The 2/3 schedule, verbatim from dec_q31_r23_run(): an output is due exactly while the
     * deficit is <= -1, and the two rows advance it by 1 and 2 respectively. */
    int16_t        d        = (int16_t)( (int16_t)firc_d - (int16_t)FIRC_B1 );
    const uint8_t  p0      = firc_par;
    uint8_t        p       = p0;
    uint16_t       rows[2] = { 0u, 0u };
    int16_t        first[2] = { 0, 0 };
    uint32_t       produced = 0u;

    while( d <= -1 )
    {
        if( rows[p] == 0u ) { first[p] = d; }
        rows[p]++;
        ++produced;
        d = (int16_t)( d + ( ( p == 0u ) ? 1 : 2 ) );
        p ^= 1u;
    }
    firc_d   = (int8_t)d;
    firc_par = p;

    for( uint32_t r = 0u; r < 2u; r++ )
    {
        if( rows[r] == 0u ) { continue; }
        const uint32_t taps  = ( r == 0u ) ? FIRC_T_P0 : FIRC_T_P1;
        const uint32_t coff  = ( r == 0u ) ? FIRC_XC_P0 : FIRC_XC_P1;
        const uint16_t back  = (uint16_t)( (uint16_t)( -first[r] ) + (uint16_t)( taps - 1u ) );
        const uint16_t start = (uint16_t)( ( (uint32_t)firc_wr2 + FIRC_R2LEN - back ) % FIRC_R2LEN );
        const uint32_t slot0 = (uint32_t)( ( r ^ p0 ) & 1u );
        int32_t*       ring  = FIRC_R2;
        int32_t*       dst   = &out[slot0 * FIRC_NCH];

        for( uint32_t c = 0u; c < FIRC_NCH; c++ )
        {
            int32_t tmp[6];
            (void)fir_ring_q31_ymod_yonly_block( &firb_coeff[coff], ring + start, taps,
                                                 tmp, (uint32_t)rows[r], FIRC_D2 * 4u,
                                                 ring, FIRC_R2LEN * 4u );
            int32_t* o = dst;
            for( uint32_t j = 0u; j < (uint32_t)rows[r]; j++ )
            {
                *o = (int32_t)( (uint32_t)tmp[j] & 0xFFFFFF00u );   /* dec_q31_to_s24_left */
                o += FIRC_NCH * 2u;
            }
            ring += FIRC_R2LEN;
            ++dst;
        }
    }
    return produced;
}

/* The timed statement.  Three shipping blocks, because ONE block yields 5 or 6 outputs depending on
 * the 2/3 phase and three of them yield exactly 16 -- the same window part 1 measured.  Resetting
 * the phase makes every trial byte-identical work; the ring write pointers are deliberately NOT
 * reset, since where the AGU wraps changes nothing about what it costs. */
static void firc_baseline_run( void )
{
    uint32_t produced = 0u;
    firc_d   = 0;
    firc_par = 0u;
    for( uint32_t b = 0u; b < FIRC_BLOCKS; b++ )
    {
        firc_stage1_block();
        /* Every block writes the SAME six output slots.  out32 is one block wide because the arena
         * is 9600 B and this is the object that could give a kilobyte back; the reference therefore
         * checks the LAST block, which is a steady-state one (both rings full).  The work timed is
         * unchanged -- same kernel calls, same scatter, same stride. */
        produced += firc_r23_block( FIRC_OUT32 );
    }
    firn_sink += (int32_t)produced;
}

static void firc_fill_input( void )
{
    for( uint32_t f = 0u; f < FIRC_BLK; f++ )
    {
        for( uint32_t c = 0u; c < FIRC_NCH; c++ )
        {
            /* 0.25 full scale, deterministic, different per frame and channel so nothing cancels
             * by accident -- the same shape and level part 1's commutator input uses. */
            const float v = 0.25f *
                ( ( (float)( ( (int32_t)( ( f * 23u ) + ( c * 7u ) ) % 19 ) - 9 ) ) / 9.0f );
            FIRC_IN96[( f * FIRC_NCH ) + c] = (int32_t)( v * 2147483648.0f );
        }
    }
    for( uint32_t i = 0u; i < ( FIRC_OUTS * FIRC_NCH ); i++ ) { FIRC_OUT32[i] = 0; }
    for( uint32_t i = 0u; i < ( FIRC_NCH * FIRC_R1LEN ); i++ ) { FIRC_R1[i] = 0; }
    for( uint32_t i = 0u; i < ( FIRC_NCH * FIRC_R2LEN ); i++ ) { FIRC_R2[i] = 0; }
    for( uint32_t i = 0u; i < ( FIRC_B1 * FIRC_NCH ); i++ )    { FIRC_MID[i] = 0; }
    firc_wr1 = 0u;
    firc_wr2 = 0u;
    firc_d   = 0;
    firc_par = 0u;
}

/* Channel 0 of the baseline against a double model of the same two-stage cascade.
 *
 * The model is LINEAR -- plain arrays indexed by global sample number, no ring, no modulo -- and it
 * recovers the alignment from the SAME schedule variables the runner uses rather than from a
 * hand-derived offset.  The rule, which is worth stating because it is the one thing a replay of a
 * ring-based kernel can get quietly wrong: after a push the window's NEWEST sample sits at global
 * index (total pushed) + first[row], and the window reaches taps-1 further back.  Stage 1's batch of
 * 8 is the same statement with first = -(span - (taps-1)) = -14 ... 0 in steps of 2.
 *
 * What this catches is plumbing: a window off by one sample, a phase row reading the other row's
 * slots, a channel stride wrong, a stage running at the wrong rate.  It is not a filter
 * qualification -- the response was qualified on host and the kernel on hardware years of commits
 * ago; it is here so the bench cannot report the cycle cost of a wrong chain. */
static void firc_check( float* err_out, uint32_t* checked_out )
{
    /* In the arena, not on the stack and not static: .bss cannot take 624 B in this profile, and
     * the stack is what part 2 is short of (see the canary).  They overlay R1, which is dead once
     * the chain has run. */
    double* const xr = FIRC_REF_X;
    double* const ur = FIRC_REF_U;
    double* const yr = FIRC_REF_Y;

    for( uint32_t n = 0u; n < ( FIRC_BLOCKS * FIRC_BLK ); n++ )
    { xr[n] = (double)FIRC_IN96[( ( n % FIRC_BLK ) * FIRC_NCH ) + 0u] / 2147483648.0; }
    for( uint32_t i = 0u; i < FIRC_OUTS; i++ ) { yr[i] = 0.0; }

    /* stage 1: u[J] newest sample is global 2J+1, window 41 taps back from there */
    for( uint32_t j = 0u; j < ( FIRC_BLOCKS * FIRC_B1 ); j++ )
    {
        double acc = 0.0;
        for( uint32_t k = 0u; k < FIRC_T1; k++ )
        {
            const int32_t n = (int32_t)( 2u * j ) + 1 - (int32_t)( FIRC_T1 - 1u ) + (int32_t)k;
            if( n >= 0 ) { acc += (double)s_96_to_48_coeff[k] * xr[n]; }
        }
        ur[j] = acc;
    }

    /* stage 2: the same d/parity walk, one block at a time */
    int16_t  d    = 0;
    uint8_t  p    = 0u;
    uint32_t made = 0u;          /* of the LAST block: how many of the six slots it wrote */
    for( uint32_t b = 0u; b < FIRC_BLOCKS; b++ )
    {
        const uint32_t g       = ( b + 1u ) * FIRC_B1;      /* 48 kHz samples pushed so far */
        const uint8_t  p0      = p;
        uint16_t       rows[2] = { 0u, 0u };
        int16_t        first[2] = { 0, 0 };

        made = 0u;
        d = (int16_t)( d - (int16_t)FIRC_B1 );
        while( d <= -1 )
        {
            if( rows[p] == 0u ) { first[p] = d; }
            rows[p]++;
            ++made;
            d = (int16_t)( d + ( ( p == 0u ) ? 1 : 2 ) );
            p ^= 1u;
        }

        for( uint32_t r = 0u; r < 2u; r++ )
        {
            const uint32_t taps = ( r == 0u ) ? FIRC_T_P0 : FIRC_T_P1;
            const float*   h    = ( r == 0u ) ? s_48_to_32_phase0_coeff : s_48_to_32_phase1_coeff;
            const uint32_t slot0 = (uint32_t)( ( r ^ p0 ) & 1u );
            for( uint32_t i = 0u; i < (uint32_t)rows[r]; i++ )
            {
                const int32_t newest = (int32_t)g + (int32_t)first[r] + (int32_t)( FIRC_D2 * i );
                double        acc    = 0.0;
                for( uint32_t k = 0u; k < taps; k++ )
                {
                    const int32_t m = newest - (int32_t)( taps - 1u ) + (int32_t)k;
                    if( m >= 0 ) { acc += (double)h[k] * ur[m]; }
                }
                /* Every block writes the same six slots, exactly as the runner does now, so this
                 * naturally ends up holding the LAST block's outputs -- which is what out32 holds. */
                const uint32_t slot = slot0 + ( 2u * i );
                if( slot < FIRC_OUTS ) { yr[slot] = acc; }
            }
        }
    }

    float worst = 0.0f;
    for( uint32_t i = 0u; i < made; i++ )
    {
        const double got = (double)FIRC_OUT32[( i * FIRC_NCH ) + 0u] / 2147483648.0;
        float        e   = (float)( got - yr[i] );
        if( e < 0.0f ) { e = -e; }
        if( e > worst ) { worst = e; }
    }
    *err_out     = worst;
    *checked_out = made;
}

// ================================================================================================
// Part 2 lives in its OWN function, and not for tidiness: with it inlined into
// asrc_thirdband_kernel_bench_run() the compiler stopped being able to allocate registers --
// `internal compiler error: in extract_insn, at recog.c:2339` during the vregs pass, on a function
// that had grown past 24 KB of code.  Splitting it is the fix, and it also means part 1's numbers
// come from a function whose code generation did not change.
//
// `ovh_min` is part 1's measured timer-pair floor, passed in rather than re-measured so both parts
// subtract exactly the same overhead.
// ================================================================================================
static void firn_part2( uint32_t trials, uint32_t ovh_min )
{
    // =============================================================================================
    // PART 2 -- A / B / baseline, the three kernels of the owner's 2026-09-09 revision.
    //
    // ★ THE JUDGEMENT IS THE ABSOLUTE, NOT THE SLOPE.  Part 1's own numbers are why: the C form's
    // fixed cost per output was 24.4 cycles (float32) and 25.8 (Q31), i.e. about six operations of
    // loop and commutator management at roughly 4 cycles each.  A break-even taken on slope alone
    // silently assumed that cost was immovable.  It is not -- the asm kernels below carry the frame
    // loop, the commutator, the branch sum and the output store themselves -- so there are TWO
    // levers and only the absolute figure sees both.  The slope is still printed, as a diagnostic
    // for where the cost sits.
    // =============================================================================================
    firn_load_x_coeff();

    {
        const uintptr_t xc = (uintptr_t)&firb_coeff[FIRN_XC_ALLPASS];
        const uintptr_t ys = (uintptr_t)FIRN_ASM_STATE;
        const bool      ok = ( xc < 0xC000u ) && ( ys >= 0xC000u );
        printf( "    part 2 placement: coeff X=0x%05lX  state Y=0x%05lX  ->  %s\n",
                (unsigned long)xc, (unsigned long)ys,
                ok ? "split, MAC prefetch parallel"
                   : "*** SAME SPACE: every MAC below costs an extra cycle, distrust part 2 ***" );
    }

    /* Part 2's arena sits at the TOP of the stack's region, so the question is not "is there room"
     * but "did the stack stay out of it".  That is answered by a canary, checked after the runs --
     * the SIGNED headroom is printed too, because the unsigned form of exactly this subtraction is
     * what hid the collision that crashed the first attempt. */
    {
        uint32_t sp_probe = 0u;
        const int32_t room = (int32_t)( (int32_t)FIRN_P2_ARENA - (int32_t)(uintptr_t)&sp_probe );
        printf( "    part 2 arena 0x%05lX..0x%05lX, stack near 0x%05lX (%ld B below the arena)\n",
                (unsigned long)FIRN_P2_ARENA, (unsigned long)FIRN_P2_END,
                (unsigned long)(uintptr_t)&sp_probe, (long)room );
        if( room < 1024 )
        {
            printf( "    PART 2 REFUSING to run: the stack is within 1024 B of the arena (%ld).\n",
                    (long)room );
            return;
        }
        firn_p2_canary_set();
    }

    /* ---- correctness of both asm kernels, against the SAME double model part 1 used ---- */
    firn_prepare( FIRN_SEC_HI, 1 );
    const double firn_ref_hi = firn_ref_ch0( FIRN_SEC_HI );
    firn_prepare( FIRN_SEC_HI, 0 );

    float a_err = 0.0f;
    float b_rel = 0.0f;
    {
        firn_asm_prepare( 0u );
        asrc_tb_a_q31_k16( FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS], FIRN_ASM_IO, 1u );
        a_err = (float)( ( (double)FIRN_ASM_IO[3u * FIRN_CH] / 2147483648.0 ) - firn_ref_hi );

        /* B is driven 8 bits down.  Not a convenience: its single-state form carries the internal
         * node through 1/(1 + a z^-1), peak magnitude 1/(1-a) = 70.4 at the K=16 design's deepest
         * pole (a = 0.985785), against 2.09 for A's node.  At 0.25 FS it saturates, so the honest
         * comparison is at a level it can carry, judged on RELATIVE error.  Cycles are unaffected
         * -- sacr.l saturates in constant time -- but the guard-bit requirement is a real, separate
         * disqualification for a Q31 shipping version and is reported as one. */
        firn_asm_prepare( 8u );
        asrc_tb_b_q31_k16( FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS], FIRN_ASM_IO, 1u );
        const double want = firn_ref_hi / 256.0;
        const double got  = (double)FIRN_ASM_IO[3u * FIRN_CH] / 2147483648.0;
        const double mag  = ( want >= 0.0 ) ? want : -want;
        b_rel = (float)( ( got - want ) / ( ( mag > 1e-9 ) ? mag : 1e-9 ) );
    }
    /* ---- the baseline: fill, run once, check the chain before timing it ---- */
    firc_fill_input();
    firc_baseline_run();
    float    c_err     = 0.0f;
    uint32_t c_checked = 0u;
    firc_check( &c_err, &c_checked );
    /* Read before any printf: see the note above the canary macros. */
    const uint32_t canary_check = firn_p2_canary_worst();

    printf( "    asm correctness vs double reference, ch0 1 frame: A abs err=%e   B rel err=%e\n",
            (double)a_err, (double)b_rel );
    printf( "    baseline correctness vs double cascade, ch0, last block's %lu outputs: worst err=%e\n",
            (unsigned long)c_checked, (double)c_err );

    if( ( a_err > 1e-4f ) || ( a_err < -1e-4f ) ||
        ( b_rel > 1e-3f ) || ( b_rel < -1e-3f ) ||
        ( c_err > 1e-4f ) || ( c_err < -1e-4f ) )
    {
        printf( "    PART 2 NOT TIMED: a kernel does not reproduce its cascade, so any cycle count\n" );
        printf( "    would be the cost of a wrong answer.\n" );
        return;
    }

    /* ---- timing.  Same macro, same window, same trials as part 1. ---- */
    firn_stats_t a_lo, a_hi, b_lo, b_hi, c_all, c_s1;

    /* Re-stamped so the timed windows are bracketed on their own: the printfs above are the deepest
     * calls in this function and their stack use is harmless, because nothing in the arena is live
     * across them (every arm re-initialises what it reads). */
    firn_p2_canary_set();

    firn_asm_prepare( 0u );
    FIRN_MEASURE( asrc_tb_a_q31_k8(  FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS],
                                     FIRN_ASM_IO, FIRN_FRAMES ), a_lo );
    firn_asm_prepare( 0u );
    FIRN_MEASURE( asrc_tb_a_q31_k16( FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS],
                                     FIRN_ASM_IO, FIRN_FRAMES ), a_hi );
    firn_asm_prepare( 8u );
    FIRN_MEASURE( asrc_tb_b_q31_k8(  FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS],
                                     FIRN_ASM_IO, FIRN_FRAMES ), b_lo );
    firn_asm_prepare( 8u );
    FIRN_MEASURE( asrc_tb_b_q31_k16( FIRN_ASM_STATE, &firb_coeff[FIRN_XC_ALLPASS],
                                     FIRN_ASM_IO, FIRN_FRAMES ), b_hi );

    firc_fill_input();
    FIRN_MEASURE( firc_baseline_run(), c_all );
    /* Stage 1 alone, so the baseline's own split is visible rather than inferred: the 2/3 phase
     * rows are then (all - stage1) and the reader can see which half the shipping chain spends in.
     * Diagnostic only; the verdict is taken on the whole chain. */
    firc_fill_input();
    FIRN_MEASURE( { firc_stage1_block(); firc_stage1_block(); firc_stage1_block(); }, c_s1 );

    const uint32_t canary_timed = firn_p2_canary_worst();

    /* ---- report ---- */
    static const char* const p2_names[2] = { "A  asm Q31 1-mult / 2-state",
                                             "B  asm Q31 2-mult / 1-state" };
    const firn_stats_t* p2_lo[2] = { &a_lo, &b_lo };
    const firn_stats_t* p2_hi[2] = { &a_hi, &b_hi };
    uint32_t            p2_abs[2] = { 0u, 0u };

    for( uint32_t v = 0u; v < 2u; v++ )
    {
        const firn_stats_t* lo = p2_lo[v];
        const firn_stats_t* hi = p2_hi[v];
        const uint32_t lo_min  = ( lo->min_t > ovh_min ) ? ( lo->min_t - ovh_min ) : 0u;
        const uint32_t hi_min  = ( hi->min_t > ovh_min ) ? ( hi->min_t - ovh_min ) : 0u;
        const uint32_t hi_mean = ( firn_mean( hi ) > ovh_min )
                                 ? ( firn_mean( hi ) - ovh_min ) : 0u;
        const uint32_t hi_max  = ( hi->max_t > ovh_min ) ? ( hi->max_t - ovh_min ) : 0u;

        printf( "    %s\n", p2_names[v] );
        printf( "      K=16 block ticks min/mean/max=%lu/%lu/%lu -> cycles %lu/%lu/%lu\n",
                (unsigned long)hi_min, (unsigned long)hi_mean, (unsigned long)hi_max,
                (unsigned long)( hi_min * FIRB_CYC_PER_TICK ),
                (unsigned long)( hi_mean * FIRB_CYC_PER_TICK ),
                (unsigned long)( hi_max * FIRB_CYC_PER_TICK ) );
        printf( "      K=8  block ticks min=%lu -> cycles %lu\n",
                (unsigned long)lo_min, (unsigned long)( lo_min * FIRB_CYC_PER_TICK ) );
        firn_print_x1000( "cycles/section (slope)",
                          firn_per_x1000( hi_min - lo_min,
                                          ( FIRN_SEC_HI - FIRN_SEC_LO ) * FIRN_FRAMES * FIRN_CH ),
                          "cycles  [diagnostic]" );
        p2_abs[v] = firn_per_x1000( hi_min, FIRN_FRAMES * FIRN_CH );
        firn_print_x1000( "cycles/32k output/ch", p2_abs[v], "cycles  <-- THE JUDGEMENT" );
        {
            /* absolute minus slope x K: what the frame loop, commutator, branch sum and output
             * store cost, i.e. the second lever.  Clamped because both terms are measurements and
             * a K=8 point a hair above half the K=16 point would otherwise wrap the subtraction. */
            const uint32_t slope = firn_per_x1000( hi_min - lo_min,
                          ( FIRN_SEC_HI - FIRN_SEC_LO ) * FIRN_FRAMES * FIRN_CH );
            const uint32_t sxk   = slope * FIRN_SEC_HI;
            firn_print_x1000( "fixed cost per output",
                              ( p2_abs[v] > sxk ) ? ( p2_abs[v] - sxk ) : 0u, "cycles" );
        }
        firn_print_x1000( "us per 16-frame block",
                          ( hi_min * FIRB_CYC_PER_TICK * 1000u ) / ( PLL1_CLK_HZ / 1000000UL ),
                          "us" );
    }

    {
        const uint32_t all_min = ( c_all.min_t > ovh_min ) ? ( c_all.min_t - ovh_min ) : 0u;
        const uint32_t s1_min  = ( c_s1.min_t  > ovh_min ) ? ( c_s1.min_t  - ovh_min ) : 0u;
        const uint32_t all_mean = ( firn_mean( &c_all ) > ovh_min )
                                  ? ( firn_mean( &c_all ) - ovh_min ) : 0u;
        const uint32_t all_max = ( c_all.max_t > ovh_min ) ? ( c_all.max_t - ovh_min ) : 0u;
        const uint32_t c_abs   = firn_per_x1000( all_min, FIRC_WINDOW_OUTS * FIRC_NCH );

        printf( "    C  SHIPPING 96->32 front end, same window (41-tap /2 -> mid 48k -> 97-tap 2/3)\n" );
        printf( "      inside: ring push, asm FIR kernel x2 stages, intermediate buffer, phase\n" );
        printf( "              schedule, s24-left scatter.  outside: M30 resampler, TDM gather.\n" );
        printf( "      block ticks min/mean/max=%lu/%lu/%lu -> cycles %lu/%lu/%lu\n",
                (unsigned long)all_min, (unsigned long)all_mean, (unsigned long)all_max,
                (unsigned long)( all_min * FIRB_CYC_PER_TICK ),
                (unsigned long)( all_mean * FIRB_CYC_PER_TICK ),
                (unsigned long)( all_max * FIRB_CYC_PER_TICK ) );
        firn_print_x1000( "cycles/32k output/ch", c_abs, "cycles  <-- THE BASELINE" );
        firn_print_x1000( "  of which 96->48 stage",
                          firn_per_x1000( s1_min, FIRC_WINDOW_OUTS * FIRC_NCH ),
                          "cycles  [diagnostic]" );
        firn_print_x1000( "  of which 2/3 rows",
                          firn_per_x1000( ( all_min > s1_min ) ? ( all_min - s1_min ) : 0u,
                                          FIRC_WINDOW_OUTS * FIRC_NCH ),
                          "cycles  [diagnostic]" );
        firn_print_x1000( "us per 16-frame block",
                          ( all_min * FIRB_CYC_PER_TICK * 1000u ) / ( PLL1_CLK_HZ / 1000000UL ),
                          "us" );

        /* ---- the verdict, stated so it cannot be read the wrong way round ---- */
        printf( "    VERDICT (absolute cycles per 32 kHz output per channel, same window):\n" );
        for( uint32_t v = 0u; v < 2u; v++ )
        {
            const bool     faster = ( p2_abs[v] < c_abs );
            const uint32_t d      = faster ? ( c_abs - p2_abs[v] ) : ( p2_abs[v] - c_abs );
            const uint32_t pct    = ( c_abs == 0u ) ? 0u : (uint32_t)( ( (uint64_t)d * 1000u ) / c_abs );
            printf( "      %.1s: %lu.%03lu vs baseline %lu.%03lu -> %s by %lu.%01lu %% -> %s\n",
                    p2_names[v],
                    (unsigned long)( p2_abs[v] / 1000u ), (unsigned long)( p2_abs[v] % 1000u ),
                    (unsigned long)( c_abs / 1000u ),     (unsigned long)( c_abs % 1000u ),
                    faster ? "FASTER" : "slower",
                    (unsigned long)( pct / 10u ), (unsigned long)( pct % 10u ),
                    faster ? "GO on CPU" : "NO-GO on CPU" );
        }
        printf( "      part 1 C forms, same unit: Q31 324.015  float32 205.578  (see the report)\n" );
        printf( "      M30 resampler is common to both structures and cancels in this comparison.\n" );
    }

    {
        const uint32_t reached = ( canary_check > canary_timed ) ? canary_check : canary_timed;
        if( reached == 0u )
        {
            printf( "    stack canary intact: the stack never came within %lu B of part 2's arena,\n"
                    "    so nothing above overwrote its own operands.\n",
                    (unsigned long)FIRN_P2_CANARY_BYTES );
        }
        else
        {
            printf( "    *** STACK CANARY BROKEN: the stack came within %lu B of the arena. Part 2's\n"
                    "    *** numbers above are NOT trustworthy -- move FIRN_P2_ARENA up or shrink it.\n",
                    (unsigned long)( FIRN_P2_CANARY_BYTES - reached ) );
        }
    }
}

// ---- the bench ----------------------------------------------------------------------------------
void asrc_thirdband_kernel_bench_run( uint32_t trials )
{
    if( trials == 0u ) { trials = FIRN_DEFAULT_TRIALS; }

    firn_stats_t ovh, q_lo, q_hi, f_lo, f_hi, cal_lo, cal_hi;
    float        q31_err = 0.0f, f32_err = 0.0f;

    printf( "\n *an third-band 3-path allpass kernel  CPU=%luMHz timer=%luMHz 1 tick=%lu cycles"
            " trials=%lu\n",
            (unsigned long)( PLL1_CLK_HZ / 1000000UL ), (unsigned long)( FCY / 1000000UL ),
            (unsigned long)FIRB_CYC_PER_TICK, (unsigned long)trials );
    printf( "    geometry: %luch channel-major, K=%lu sections (split 6/5/5), %lu output frames"
            "/block, 3 branches\n",
            (unsigned long)FIRN_CH, (unsigned long)FIRN_SEC_HI, (unsigned long)FIRN_FRAMES );
    printf( "    inside the window: state load/store, coeff load, arithmetic, commutator fetch,"
            " branch sum, output store, loops\n" );
    printf( "    outside: ring, M30 resampler, TDM gather/scatter (measured separately and added)\n" );

    /* ---- timer-pair overhead, subtracted from every figure ---- */
    FIRN_MEASURE( firn_sink += 0, ovh );
    printf( "    timer pair ticks min/mean/max=%lu/%lu/%lu (subtracted below)\n",
            (unsigned long)ovh.min_t, (unsigned long)firn_mean( &ovh ),
            (unsigned long)ovh.max_t );

    /* ---- self-calibration: fir_ring_q31 is taps+9 instructions for taps MACs, so its slope
     * MUST read 1.000 cycles/MAC.  If it does not, FIRB_CYC_PER_TICK is wrong and so is
     * everything else printed here. ---- */
    firb_fill_coeff( FIRB_TAPS_HI );
    firb_fill_samples( FIRB_HIST_Y, 2u * FIRB_TAPS_HI, 0xA5A5u );
    FIRN_MEASURE( fir_ring_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_LO, firb_out ), cal_lo );
    FIRN_MEASURE( fir_ring_q31( firb_coeff, FIRB_HIST_Y, FIRB_TAPS_HI, firb_out ), cal_hi );
    {
        const uint32_t cal = firn_per_x1000( cal_hi.min_t - cal_lo.min_t, FIRB_TAPS_SPAN );
        printf( "    CALIBRATION fir_ring_q31 slope over %lu..%lu taps: ",
                (unsigned long)FIRB_TAPS_LO, (unsigned long)FIRB_TAPS_HI );
        printf( "%lu.%03lu cycles/MAC (expect 1.000; %s)\n",
                (unsigned long)( cal / 1000u ), (unsigned long)( cal % 1000u ),
                ( ( cal > 900u ) && ( cal < 1150u ) ) ? "tick conversion CONFIRMED"
                                                      : "*** OUT OF RANGE: distrust every cycles figure below ***" );
    }

    /* ---- correctness before timing ---- */
    firn_check( FIRN_SEC_HI, &q31_err, &f32_err );
    printf( "    correctness vs double reference, ch0 1 frame: f32 err=%e  Q31 err=%e\n",
            (double)f32_err, (double)q31_err );
    if( ( f32_err > 1e-4f ) || ( f32_err < -1e-4f ) ||
        ( q31_err > 1e-4f ) || ( q31_err < -1e-4f ) )
    {
        printf( "    NOT TIMED: a kernel does not reproduce the cascade, so any cycle count would\n" );
        printf( "    be the cost of a wrong answer.\n *an complete\n" );
        return;
    }

    /* ---- timing: two section counts per format ---- */
    firn_prepare( FIRN_SEC_LO, 0 );
    FIRN_MEASURE( firn_kernel_q31( FIRN_SEC_LO, FIRN_FRAMES ), q_lo );
    firn_prepare( FIRN_SEC_HI, 0 );
    FIRN_MEASURE( firn_kernel_q31( FIRN_SEC_HI, FIRN_FRAMES ), q_hi );
    firn_prepare( FIRN_SEC_LO, 1 );
    FIRN_MEASURE( firn_kernel_f32( FIRN_SEC_LO, FIRN_FRAMES ), f_lo );
    firn_prepare( FIRN_SEC_HI, 1 );
    FIRN_MEASURE( firn_kernel_f32( FIRN_SEC_HI, FIRN_FRAMES ), f_hi );

    /* ---- report ---- */
    static const char* const names[2] = { "Q31 (primary)", "float32 (comparison)" };
    const firn_stats_t* lo_all[2] = { &q_lo, &f_lo };
    const firn_stats_t* hi_all[2] = { &q_hi, &f_hi };

    for( uint32_t v = 0u; v < 2u; v++ )
    {
        const firn_stats_t* lo = lo_all[v];
        const firn_stats_t* hi = hi_all[v];
        const uint32_t lo_min  = ( lo->min_t > ovh.min_t ) ? ( lo->min_t - ovh.min_t ) : 0u;
        const uint32_t hi_min  = ( hi->min_t > ovh.min_t ) ? ( hi->min_t - ovh.min_t ) : 0u;
        const uint32_t hi_mean = ( firn_mean( hi ) > ovh.min_t )
                                 ? ( firn_mean( hi ) - ovh.min_t ) : 0u;
        const uint32_t hi_max  = ( hi->max_t > ovh.min_t ) ? ( hi->max_t - ovh.min_t ) : 0u;

        printf( "    %s\n", names[v] );
        printf( "      K=%lu  block ticks min/mean/max=%lu/%lu/%lu -> cycles %lu/%lu/%lu\n",
                (unsigned long)FIRN_SEC_HI, (unsigned long)hi_min, (unsigned long)hi_mean,
                (unsigned long)hi_max, (unsigned long)( hi_min * FIRB_CYC_PER_TICK ),
                (unsigned long)( hi_mean * FIRB_CYC_PER_TICK ),
                (unsigned long)( hi_max * FIRB_CYC_PER_TICK ) );
        printf( "      K=%lu  block ticks min=%lu -> cycles %lu\n",
                (unsigned long)FIRN_SEC_LO, (unsigned long)lo_min,
                (unsigned long)( lo_min * FIRB_CYC_PER_TICK ) );

        /* slope: marginal cost of one section for one channel for one output sample */
        firn_print_x1000( "cycles/section (slope)",
                          firn_per_x1000( hi_min - lo_min,
                                          ( FIRN_SEC_HI - FIRN_SEC_LO ) * FIRN_FRAMES * FIRN_CH ),
                          "cycles" );
        /* absolute, K=16, includes all per-frame fixed work -- the budget figure */
        firn_print_x1000( "cycles/32k output/ch (abs)",
                          firn_per_x1000( hi_min, FIRN_FRAMES * FIRN_CH ), "cycles" );
        firn_print_x1000( "cycles/16ch output (abs)",
                          firn_per_x1000( hi_min, FIRN_FRAMES ), "cycles" );
        firn_print_x1000( "us per 16-frame block",
                          ( hi_min * FIRB_CYC_PER_TICK * 1000u ) / ( PLL1_CLK_HZ / 1000000UL ),
                          "us" );
    }

    firn_part2( trials, ovh.min_t );

    printf( " *an complete sink=%ld\n", (long)firn_sink );
}
#endif /* ASRC_FIR_KERNEL_BENCH_AVAILABLE */
