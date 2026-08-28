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

#include "nora_high_res_timer.h"

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

// float32, eight channels per pass, frame-major history: 17 instructions + one DTB per 8 MACs.
extern void fir_ring_wide8_f32( const float* hist, const float* coeff, uint32_t taps, float* out8 );

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

#endif /* ASRC_FIR_KERNEL_BENCH_AVAILABLE */
