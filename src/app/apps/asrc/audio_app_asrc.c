//===========================================================
// ASRC App engine implementation (audio_app_asrc.c)
//
// A<->B asynchronous sample-rate converter ENGINE (see audio_app_asrc.h). Owns the two
// direction instances (ab / ba), the cross-domain ring FIFOs, the variable-ratio resampler
// (cubic or windowed-sinc polyphase FIR, selected by APP_ASRC_INTERP), the FIFO-fill drift
// control, and the telemetry. Route wiring + app-side output DSP stay in audio_transport.c.
//===========================================================

#include "audio_app_asrc.h"
// asrc_frontend_plan_t is DECLARED here unconditionally: the *ar pair gate below builds a plan
// in every build -- seeding it 1/1 (no front end) and only asking
// asrc_audio_path_frontend_plan() to fill it when a front end exists -- so the type is needed
// even where APP_ASRC_RUNTIME_48K_TO_8 is 0 and nothing fills it.
#include "asrc_audio_path.h"

#if !SONORA_APP_IS_ASRC
#  error "audio_app_asrc.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#if APP_ASRC_MEAS
#include "audio_app_meas.h"   // R10 Q10: measurement-only control-variable trace hook
#endif

#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC

#include <stdint.h>
#include <stdio.h>                     // printf (telemetry)
#include <math.h>                      // lrintf
#include "nora_high_res_timer.h"  // read-only: time asrc_pull
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
#include "arm_math.h"                  // arm_dot_prod_f32 -> mchp_dot_prod_f32 (optimized dot)
// Load-reduction V1: fused dual dot -- one window vs both sub-filters (c0, c1) in a single call
// (window loaded once, two independent accumulators, one FCR/call). See mchp_dot_prod2_f32.s.
extern void mchp_dot_prod2_f32( const float32_t* w, const float32_t* c0, const float32_t* c1,
                                uint32_t blockSize, float32_t* r0, float32_t* r1 );
// V2a: same dual dot, 2-tap unrolled (halved loop overhead, bit-equivalent). blockSize even.
extern void mchp_dot_prod2_f32_v2a( const float32_t* w, const float32_t* c0, const float32_t* c1,
                                    uint32_t blockSize, float32_t* r0, float32_t* r1 );
// V2b: V2a + even/odd 4-accumulator split (diagnostic; NOT bit-equivalent -- add order differs).
extern void mchp_dot_prod2_f32_v2b( const float32_t* w, const float32_t* c0, const float32_t* c1,
                                    uint32_t blockSize, float32_t* r0, float32_t* r1 );
#if   (ASRC_DUAL_KERNEL == ASRC_DUAL_V2B)
#define ASRC_DOT2  mchp_dot_prod2_f32_v2b
#elif (ASRC_DUAL_KERNEL == ASRC_DUAL_V2A)
#define ASRC_DOT2  mchp_dot_prod2_f32_v2a
#else
#define ASRC_DOT2  mchp_dot_prod2_f32
#endif
// V3: coeff-shared 2-channel fused dual-dot. out4 = {aA0,aA1,aB0,aB1}. Loads c0/c1 once for
// both channels (attacks the load-bound cost V2b exposed). Requires ASRC_CH even.
extern void mchp_dot_prod2x2_f32( const float32_t* wA, const float32_t* wB,
                                  const float32_t* c0, const float32_t* c1,
                                  uint32_t blockSize, float32_t* out4 );
// A0 load-scheduling experiment: same counts, loads-first-then-MACs (bit-equivalent).
extern void mchp_dot_prod2x2_f32_sched_v1( const float32_t* wA, const float32_t* wB,
                                           const float32_t* c0, const float32_t* c1,
                                           uint32_t blockSize, float32_t* out4 );
// A0 schedule + 4-tap unroll (bit-equivalent; blockSize multiple of 4).
extern void mchp_dot_prod2x2_f32_sched_v2( const float32_t* wA, const float32_t* wB,
                                           const float32_t* c0, const float32_t* c1,
                                           uint32_t blockSize, float32_t* out4 );
// A0 schedule + 8-tap unroll (bit-equivalent; blockSize multiple of 8).
extern void mchp_dot_prod2x2_f32_sched_v3( const float32_t* wA, const float32_t* wB,
                                           const float32_t* c0, const float32_t* c1,
                                           uint32_t blockSize, float32_t* out4 );
// Bit-identical SCHED_V2 duplicate placed by XC-DSC's standard ramfunc machinery.
// The declaration must carry ramfunc so XC-DSC emits an absolute call to executable RAM.
extern __attribute__((ramfunc))
void mchp_dot_prod2x2_f32_sched_v2_ramtest( const float32_t* wA, const float32_t* wB,
                                            const float32_t* c0, const float32_t* c1,
                                            uint32_t blockSize, float32_t* out4 );
#if   (ASRC_2X2_KERNEL == ASRC_2X2_SCHED_V3)
#define ASRC_DOT2X  mchp_dot_prod2x2_f32_sched_v3
#elif (ASRC_2X2_KERNEL == ASRC_2X2_SCHED_V2)
#if APP_ASRC_KERNEL_RAMTEST
#define ASRC_DOT2X  mchp_dot_prod2x2_f32_sched_v2_ramtest
#else
#define ASRC_DOT2X  mchp_dot_prod2x2_f32_sched_v2
#endif
#elif (ASRC_2X2_KERNEL == ASRC_2X2_SCHED_V1)
#define ASRC_DOT2X  mchp_dot_prod2x2_f32_sched_v1
#else
#define ASRC_DOT2X  mchp_dot_prod2x2_f32
#endif
// B0/wide4: coeff-shared 4-channel fused dual-dot. out8={A*c0,A*c1,B*c0,B*c1,C..,D..}.
// base=&ch[c][wbase], strideBytes = per-channel row stride. Requires ASRC_CH multiple of 4.
extern void mchp_dot_prod4x2_f32( const float32_t* wA, uint32_t strideBytes,
                                  const float32_t* c0, const float32_t* c1,
                                  uint32_t blockSize, float32_t* out8 );
// Experimental wide8: coeff-shared 8-channel dual-dot. out16 holds c0/c1 pairs
// for channels 0..7. Same math/add order as DUAL4X, but c0/c1 load once for 8ch.
extern void mchp_dot_prod8x2_f32( const float32_t* w0, uint32_t strideBytes,
                                  const float32_t* c0, const float32_t* c1,
                                  uint32_t blockSize, float32_t* out16 );
// STREAM-CEFF: coeff-blended 8-channel SINGLE-accumulator dot. Blends c0/c1 -> ce
// per tap in registers and fans it to 8 channels with ONE MAC each (half the MACs
// of wide4). out8[c] is the FINAL blended channel sample (no post-blend in C).
// base=&ch[c][wbase], strideBytes = per-channel row stride, wb = blend weight.
// Requires ASRC_CH multiple of 8. Class B (per-tap blend) -> SFDR-verified.
extern void mchp_stream8_f32( const float32_t* wbase0, uint32_t strideBytes,
                              const float32_t* c0, const float32_t* c1,
                              uint32_t blockSize, float32_t* out8, float32_t wb );
// A1: software-pipelined STREAM8 (ce for tap k+1 built during tap k MACs). Same
// signature, same result (Class B, identical to base STREAM8) -- schedule only.
extern void mchp_stream8_f32_p1( const float32_t* wbase0, uint32_t strideBytes,
                                 const float32_t* c0, const float32_t* c1,
                                 uint32_t blockSize, float32_t* out8, float32_t wb );
// TILE8 history variant: samples for one tap are contiguous lanes [ch0..ch7].
extern void mchp_stream8_interleaved_f32( const float32_t* xbase,
                                          const float32_t* c0, const float32_t* c1,
                                          uint32_t blockSize, float32_t* out8, float32_t wb );
// Two adjacent outputs whose history windows differ by exactly one frame. The
// kernel shares the overlapping M-1 interior frames and returns [out0][out1].
extern void mchp_stream8_pair_f32( const float32_t* wbase0, uint32_t strideBytes,
                                   const float32_t* c00, const float32_t* c01,
                                   uint32_t blockSize, float32_t* out16,
                                   const float32_t* c10, const float32_t* c11,
                                   float32_t wb0, float32_t wb1 );
// Same pair FIR arithmetic, with clamp/truncate/left-justify fused into its output tail.
extern void mchp_stream8_pair_slot_f32( const float32_t* wbase0, uint32_t strideBytes,
                                        const float32_t* c00, const float32_t* c01,
                                        uint32_t blockSize, int32_t* out16,
                                        const float32_t* c10, const float32_t* c11,
                                        float32_t wb0, float32_t wb1 );
// Two 8-channel groups sharing one register save/restore frame.  Dedicated
// entry points keep the hot tap count immediate in assembly.
extern void mchp_stream16_pair_slot32_f32( const float32_t* wbase0, uint32_t strideBytes,
                                           const float32_t* c00, const float32_t* c01,
                                           int32_t* out_group0, int32_t* out_group1,
                                           const float32_t* c10, const float32_t* c11,
                                           float32_t wb0, float32_t wb1 );
extern void mchp_stream16_pair_slot30_f32( const float32_t* wbase0, uint32_t strideBytes,
                                           const float32_t* c00, const float32_t* c01,
                                           int32_t* out_group0, int32_t* out_group1,
                                            const float32_t* c10, const float32_t* c11,
                                            float32_t wb0, float32_t wb1 );
extern void mchp_stream16_pair_slot28_f32( const float32_t* wbase0, uint32_t strideBytes,
                                           const float32_t* c00, const float32_t* c01,
                                           int32_t* out_group0, int32_t* out_group1,
                                           const float32_t* c10, const float32_t* c11,
                                           float32_t wb0, float32_t wb1 );
// Eight adjacent output pairs can share the large register frame as well. Keep this descriptor's
// assembly-visible layout explicit and verified below.
typedef struct {
    const float32_t* wbase0; // +0
    const float32_t* c00;    // +4
    const float32_t* c01;    // +8
    const float32_t* c10;    // +12
    const float32_t* c11;    // +16
    float32_t        wb0;    // +20
    float32_t        wb1;    // +24
} mchp_stream16_pair_desc_t;
_Static_assert( sizeof(mchp_stream16_pair_desc_t) == 28u, "pair descriptor ABI" );
_Static_assert( __builtin_offsetof(mchp_stream16_pair_desc_t, wb0) == 20u,
                "pair descriptor wb0 ABI" );
extern void mchp_stream16_block_slot32_f32( const mchp_stream16_pair_desc_t* desc,
                                             uint32_t pairCount, uint32_t strideBytes,
                                             int32_t* out, int32_t* hidden16 );
extern void mchp_stream16_block_slot30_f32( const mchp_stream16_pair_desc_t* desc,
                                             uint32_t pairCount, uint32_t strideBytes,
                                             int32_t* out, int32_t* hidden16 );
extern void mchp_stream16_block_slot28_f32( const mchp_stream16_pair_desc_t* desc,
                                             uint32_t pairCount, uint32_t strideBytes,
                                             int32_t* out, int32_t* hidden16 );
#if (ASRC_POLY_M == 32u)
#define ASRC_HAVE_FIXED_STREAM16 (1)
#define ASRC_STREAM16_PAIR_SLOT  mchp_stream16_pair_slot32_f32
#define ASRC_STREAM16_BLOCK_SLOT mchp_stream16_block_slot32_f32
#elif (ASRC_POLY_M == 30u)
#define ASRC_HAVE_FIXED_STREAM16 (1)
#define ASRC_STREAM16_PAIR_SLOT  mchp_stream16_pair_slot30_f32
#define ASRC_STREAM16_BLOCK_SLOT mchp_stream16_block_slot30_f32
#elif (ASRC_POLY_M == 28u)
#define ASRC_HAVE_FIXED_STREAM16 (1)
#define ASRC_STREAM16_PAIR_SLOT  mchp_stream16_pair_slot28_f32
#define ASRC_STREAM16_BLOCK_SLOT mchp_stream16_block_slot28_f32
#else
#define ASRC_HAVE_FIXED_STREAM16 (0)
#endif
// Generalized output stride: the pair kernels above require the second output's history window
// to start exactly ONE frame after the first (rd1 == rd0+1), which only happens near ratio 1.
// This entry point evaluates both outputs over one padded UNION window of `utaps` taps and so
// accepts any advance d = rd1 - rd0 in 0..ASRC_PAIRD_DMAX -- see the "ced" block in
// mchp_stream8_pair_slot_f32.s. One symbol serves every M (the tap count is in the descriptor).
#define ASRC_PAIRD_DMAX  (8u)
#define ASRC_PAIRD_UMAX  (44u)                      // must match ASRC_CED_MAX_TAPS in the asm
typedef struct {
    const float32_t* wbase0; // +0   channel-0 window base of output 0
    const float32_t* c00;    // +4
    const float32_t* c01;    // +8
    const float32_t* c10;    // +12
    const float32_t* c11;    // +16
    float32_t        wb0;    // +20
    float32_t        wb1;    // +24
    uint32_t         dstep;  // +28  d = rd1 - rd0
    uint32_t         utaps;  // +32  union window length, odd, M+d <= utaps <= ASRC_PAIRD_UMAX
    uint32_t         pad0;   // +36  utaps - M      (trailing zeros of the output-0 row)
    uint32_t         pad1;   // +40  utaps - M - d  (trailing zeros of the output-1 row)
    uint32_t         mbytes; // +44  M * 4
} mchp_stream16_paird_desc_t;
_Static_assert( sizeof(mchp_stream16_paird_desc_t) == 48u, "paird descriptor ABI" );
_Static_assert( __builtin_offsetof(mchp_stream16_paird_desc_t, wb0) == 20u,
                "paird descriptor wb0 ABI" );
_Static_assert( __builtin_offsetof(mchp_stream16_paird_desc_t, dstep) == 28u,
                "paird descriptor dstep ABI" );
_Static_assert( __builtin_offsetof(mchp_stream16_paird_desc_t, mbytes) == 44u,
                "paird descriptor mbytes ABI" );
_Static_assert( ASRC_POLY_M + ASRC_PAIRD_DMAX + 1u <= ASRC_PAIRD_UMAX,
                "union window does not fit the kernel's coefficient frame" );
extern void mchp_stream16_paird_f32( const mchp_stream16_paird_desc_t* desc,
                                     uint32_t strideBytes,
                                     int32_t* out_group0, int32_t* out_group1 );
// Fill everything in the descriptor that follows from d. utaps is rounded UP to an odd value
// with one extra all-zero tap so the kernel needs a single entry (one peeled seeding tap plus an
// even remainder) for every d.
#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
#define ASRC_PAIRD_DSTEP_OK(d)  ( (d) <= ASRC_PAIRD_DMAX )
#else
#define ASRC_PAIRD_DSTEP_OK(d)  ( (d) == 1u )   // only the d==1 pair kernel exists here
#endif
static inline void asrc_paird_fill( mchp_stream16_paird_desc_t* pd, uint32_t d )
{
    const uint32_t u = ( ( ASRC_POLY_M + d ) | 1u );
    pd->dstep  = d;
    pd->utaps  = u;
    pd->pad0   = u - ASRC_POLY_M;
    pd->pad1   = u - ASRC_POLY_M - d;
    pd->mbytes = ASRC_POLY_M * (uint32_t)sizeof(float32_t);
}
// Batched clamp/truncate conversion for the two 8-lane outputs returned by the pair kernel.
extern void mchp_f32_to_slot8_pair( const float32_t* src16, int32_t* dst0, int32_t* dst1 );
// Stored-phase ceiling experiment: one selected coefficient row, no per-tap blend.
extern void mchp_stream8_single_f32( const float32_t* wbase0, uint32_t strideBytes,
                                     const float32_t* coeff, uint32_t blockSize,
                                     float32_t* out8 );
#if   (ASRC_STREAM8_KERNEL == ASRC_STREAM8_P1)
#define ASRC_STREAM8  mchp_stream8_f32_p1
#else
#define ASRC_STREAM8  mchp_stream8_f32
#endif
#endif


//===========================================================
// Definitions
//===========================================================
// A (48.000 kHz, codec-A master) and B (~43.403 kHz, dsPIC SPI2 master) run on INDEPENDENT
// oscillators. Each direction is one asrc_t instance: a cross-domain stereo ring FIFO fed by
// the producer domain's RX-block ISR and drained by the consumer domain's RX-block ISR
// through a variable-ratio cubic resampler.
//
// Ratio: NO compile-time nominal. The in/out ratio is a live FEED-FORWARD value set from
// outside (audio_app_asrc_set_ratio_*), measured from the two clock domains. Until a valid
// ratio arrives, the resampler emits silence (see asrc_pull). The per-block fill error still
// trims the step (proportional control) to absorb residual drift. Codec data is 24-bit
// left-justified in the 32-bit slot -> work on (s>>8) in float, output <<8.
#ifdef APP_ASRC_FIFO_FRAMES
#define ASRC_FIFO_FRAMES   (APP_ASRC_FIFO_FRAMES)      // ring depth override (config; power of 2)
#else
#define ASRC_FIFO_FRAMES   (512u)                     // ring depth in stereo frames (power of 2)
#endif
#define ASRC_FIFO_MASK     (ASRC_FIFO_FRAMES - 1u)
// Fill setpoint. Default = geometric center FIFO/2. Define APP_ASRC_FILL_TARGET to override with an
// INDEPENDENT (possibly asymmetric) operating point -- the servo is purely proportional about the
// target (see the fill-error term below), so any setpoint holds. It must sit above the poly read
// floor (ASRC_POLY_AHEAD+1 = 17) and below the overflow guard (ASRC_FIFO_FRAMES-4), with room for the
// ~22-frame servo excursion on each side.
#ifdef APP_ASRC_FILL_TARGET
_Static_assert( (APP_ASRC_FILL_TARGET) >= 17u && (APP_ASRC_FILL_TARGET) <= (ASRC_FIFO_FRAMES - 4u),
                "APP_ASRC_FILL_TARGET out of range: need 17 <= target <= ASRC_FIFO_FRAMES-4" );
#define ASRC_FILL_TARGET   ((float)(APP_ASRC_FILL_TARGET))
#else
#define ASRC_FILL_TARGET   ((float)(ASRC_FIFO_FRAMES / 2u))
#endif
// RATIO-AWARE fill setpoint (the geometric centre above is only the DEFAULT / lower bound).
//
// The consumer drains a whole block inside ONE pull, and the producer CANNOT advance wr while
// that pull runs (both TDM DMA ISRs sit at PRIO_TDM_DMA, so neither preempts the other). So the
// last output of a block reads a window that ends ASRC_POLY_AHEAD frames past an rd already
// advanced by step*(APP_BLOCK_FRAMES-1) input frames. The look-ahead a pull needs at its START is
//
//     R(step) = floor( step * (APP_BLOCK_FRAMES - 1) ) + ASRC_POLY_AHEAD + 1
//
// which is PROPORTIONAL TO THE RATE RATIO, while ASRC_FILL_TARGET is a compile-time constant. For
// fs_B << fs_A the servo therefore holds the ring BELOW what the block needs: the tail samples of
// each block fail the window test, emit zeros and hold rd (audible "record dust" clicks), and the
// fill ratchets up to R anyway. Measured on d211161 at fs_B=12 kHz (step 4, R=76): fill 77-79 with
// a setpoint of 64, i.e. a standing servo error of +12.4 frames -- see
// [internal] report_asrc_lowrate_click_2026-07-28.md §5/§10.2.
//
// Fix: derive the setpoint from the live ratio at ratio-lock instead of hardcoding FIFO/2. The
// servo is purely proportional about the setpoint, so any value holds; the setpoint only has to
// satisfy  R(step) + jitter <= target  and  target + one producer block + jitter <= the producer
// overflow guard (ASRC_FIFO_FRAMES-4). Rates whose R fits under FIFO/2 (>= ~16 kHz against a
// 48 kHz peer, and every up-conversion) keep the calibrated 64-frame operating point byte-exactly.
// Overridable per profile: the slack a build needs depends on its MEASURED worst pull-start
// fill, not on the block size alone (see the AK128 bidir block in asrc_app_build_config.h).
#ifndef ASRC_FILL_JITTER
#define ASRC_FILL_JITTER      (4u)   // block-alignment slack on both bounds (fill is integer-quantised)
#endif
// Largest setpoint that still leaves room for one producer block under the overflow guard.
#define ASRC_FILL_TARGET_MAX  ( ASRC_FIFO_FRAMES - 4u - APP_BLOCK_FRAMES - ASRC_FILL_JITTER )
_Static_assert( (uint32_t)(ASRC_FIFO_FRAMES / 2u) <= ASRC_FILL_TARGET_MAX,
                "ring too small for the default fill setpoint plus one producer block" );

// Largest rate RATIO a burst pull can carry in this ring, as an exact integer test.
//
// From the look-ahead law above, R(step) has to stay at or under the producer's overflow guard
// (ASRC_FIFO_FRAMES-4).  floor(x) <= N is exactly x < N+1, so with step = fs_in/fs_out the whole
// condition is one comparison with no float and no division:
//
//     fs_in * (APP_BLOCK_FRAMES - 1)  <  ( ASRC_FIFO_FRAMES - 4 - ASRC_POLY_AHEAD ) * fs_out
//
// For the shipped AK128 geometry (FIFO 64, block 16, AHEAD 15) that reduces to fs_in < 3*fs_out:
// 48 kHz reaches 22.05 kHz, but not 16 kHz (R=61 against a guard of 60 -- short by ONE frame),
// and 8 kHz would need 106.  Beyond the bound the pair cannot be served at all: the setpoint is
// clamped (fill_target_capped, printed as a trailing mark on set=), fill then ratchets to R and
// stops at the guard, and the tail of every block zero-fills with rd held -- permanently.  It does
// NOT converge with time, which is why callers are given a way to refuse the pair up front
// instead of a servo that never settles.
//
// Deriving the bound from the same three constants the law uses is the point: a deeper ring or a
// shorter kernel relaxes it automatically, with no second place to remember to edit.
#define ASRC_BURST_RATIO_LIMIT_NUM  ( ASRC_FIFO_FRAMES - 4u - ASRC_POLY_AHEAD )
#define ASRC_BURST_RATIO_LIMIT_DEN  ( (uint32_t)APP_BLOCK_FRAMES - 1u )

/*
 * One engine, one direction: can a resampler living in this ring serve this step?
 *
 * The step arrives as an effective fraction (fs_in * num) / (den * fs_out) so a caller can fold in
 * a FRONT END contribution instead of the bare rate ratio -- a decimating front end is exactly
 * what turns an unservable ratio into a servable one, so the gate has to judge the step the
 * resampler will actually see, not the one the codecs are clocked at.  num == den == 1 is direct.
 */
static bool asrc_step_num_den( uint32_t fs_in_hz, uint32_t num, uint32_t den, uint32_t fs_out_hz,
                               uint32_t* eff_num, uint32_t* eff_den )
{
    if( ( fs_in_hz == 0u ) || ( fs_out_hz == 0u ) || ( num == 0u ) || ( den == 0u ) )
    {
        return false;   // not known yet -- callers treat "unknown" as acceptable
    }
    *eff_num = fs_in_hz * num;
    *eff_den = den * fs_out_hz;
    return true;
}

// Hard bound: the ring must PHYSICALLY hold the look-ahead a burst pull needs (overflow guard).
static bool asrc_burst_ratio_fits( uint32_t fs_in_hz, uint32_t num,
                                   uint32_t den, uint32_t fs_out_hz )
{
    uint32_t eff_num = 0u, eff_den = 0u;
    if( !asrc_step_num_den( fs_in_hz, num, den, fs_out_hz, &eff_num, &eff_den ) ) { return true; }
    if( eff_num <= eff_den ) { return true; }   // up-conversion (step <= 1, so R only shrinks)
    return ( eff_num * ASRC_BURST_RATIO_LIMIT_DEN ) <
           ( ASRC_BURST_RATIO_LIMIT_NUM * eff_den );
}

/*
 * Soft bound, and the one that actually bites: the servo needs SLACK above the look-ahead.
 *
 * asrc_set_fill_target() picks set = clamp(R(step) + ASRC_FILL_JITTER, ASRC_FILL_TARGET,
 * ASRC_FILL_TARGET_MAX), so the room the servo has above R is set - R, and ASRC_FILL_JITTER is the
 * floor of it.  Fitting the ring is therefore NOT sufficient: a pair can clear the hard bound
 * above and still sit so high that set collapses onto R + ASRC_FILL_JITTER, at which point fill
 * ratchets down to R, rd is held, and the tail of every block zero-fills -- permanently, with sat
 * and miss both staying 0.  That is why this needs a gate and not a counter.
 *
 * MEASURED on AK512 (FIFO 128, block 16), 2026-08-24, all 100 rate pairs on hardware: a slack of
 * 7 frames or fewer starves CONTINUOUSLY whenever the step is fractional, and a slack of 8 does
 * not.  So ASRC_FILL_JITTER = 4 is not enough on its own, and any pair landing in the
 * R + ASRC_FILL_JITTER branch (R >= ASRC_FILL_TARGET - ASRC_FILL_JITTER) has exactly 4 and cannot
 * be served.  See [internal] report_ak512_16ch_closure_2026-08-24.md section 5.4.
 *
 * The exception is an EXACT step: R uses floor(step * (BLOCK-1)), which under-counts a pull that
 * starts on an unlucky phase by one frame -- except where step * (BLOCK-1) is an integer, where
 * floor == ceil and per-block consumption is constant.  Measured: 8 <-> 24 kHz (step 3) is
 * starve-free on the same 4 frames of slack that 44.1 <-> 8 kHz (step 5.5125) starves on, so the
 * exemption is the mechanism, not a carve-out to keep a rate.
 *
 * PER-PROFILE, like ASRC_FILL_JITTER above it: the required slack is the servo's worst downward
 * excursion from its setpoint plus one, and that excursion is a property of the ring geometry
 * (FIFO depth and APP_BLOCK_FRAMES), not of the fleet.  8 is the AK512 number and is the default
 * only because this file's default ring is AK512's; a profile with a different ring overrides it
 * in asrc_app_build_config.h next to its ASRC_FILL_JITTER, citing its own measurement.  Leaving it
 * fleet-wide would over-tighten the gate on the compact rings and reject pairs that run clean --
 * on AK128 (FIFO 64, block 8) every pair with R > 36 would be refused.
 */
#ifndef ASRC_FILL_SLACK_REQUIRED
#define ASRC_FILL_SLACK_REQUIRED  (8u)   // AK512, measured; not derived -- see the comment above
#endif

static bool asrc_fill_slack_fits( uint32_t fs_in_hz, uint32_t num,
                                  uint32_t den, uint32_t fs_out_hz )
{
    uint32_t eff_num = 0u, eff_den = 0u;
    if( !asrc_step_num_den( fs_in_hz, num, den, fs_out_hz, &eff_num, &eff_den ) ) { return true; }
    if( eff_num <= eff_den ) { return true; }   // step <= 1: R shrinks, so slack only grows

    const uint32_t k     = (uint32_t)APP_BLOCK_FRAMES - 1u;
    const uint32_t scale = eff_num * k;
    // Same formula as asrc_set_fill_target(), in integers so the gate cannot drift from it.
    const uint32_t R     = ( scale / eff_den ) + ASRC_POLY_AHEAD + 1u;

    uint32_t set = R + ASRC_FILL_JITTER;
    if( set < (uint32_t)ASRC_FILL_TARGET ) { set = (uint32_t)ASRC_FILL_TARGET; }
    if( set > ASRC_FILL_TARGET_MAX )       { set = ASRC_FILL_TARGET_MAX; }
    if( set <= R )                         { return false; }

    if( ( set - R ) >= ASRC_FILL_SLACK_REQUIRED ) { return true; }
    return ( scale % eff_den ) == 0u;   // exact step: floor == ceil, ASRC_FILL_JITTER is enough
}

bool audio_app_asrc_rate_pair_is_supported( uint32_t rate_a_hz, uint32_t rate_b_hz,
                                            const char** reason )
{
    /*
     * Ask the front end FIRST, then judge the step the resampler will actually see.
     *
     * This used to `return true` unconditionally in a front-end build, reasoning that a decimating
     * front end collapses the step to ~1.0 "for every rate it covers".  The clause that matters is
     * `it covers`: the table only has rows for a 48 kHz INPUT, so every pair without 48 kHz on the
     * producer leg falls through to the direct path at the full rate ratio -- and the unconditional
     * true waved those through.  Measured 2026-08-24: 10 such pairs ran with a permanent starve.
     * Neither gate was judging them; now this one does.
     *
     * Judge every engine this route instantiates -- a low rate on leg A breaks the B->A engine
     * exactly as a low rate on leg B breaks A->B.
     */
    asrc_frontend_plan_t plan;
    plan.num_ab = 1u; plan.den_ab = 1u;
    plan.num_ba = 1u; plan.den_ba = 1u;
    plan.low_rate_hz = 0u; plan.in_rate_hz = 0u;
#if APP_ASRC_RUNTIME_48K_TO_8 || APP_ASRC_48K_TO_8_INTEGRATION
    asrc_audio_path_frontend_plan( rate_a_hz, rate_b_hz, &plan );
#endif

    if( !asrc_burst_ratio_fits( rate_a_hz, plan.num_ab, plan.den_ab, rate_b_hz )
#if APP_B_ROUTE_USES_BA
        || !asrc_burst_ratio_fits( rate_b_hz, plan.num_ba, plan.den_ba, rate_a_hz )
#endif
      )
    {
        if( reason != NULL )
        {
            *reason = "ratio needs more FIFO look-ahead than this image has";
        }
        return false;
    }

    if( !asrc_fill_slack_fits( rate_a_hz, plan.num_ab, plan.den_ab, rate_b_hz )
#if APP_B_ROUTE_USES_BA
        || !asrc_fill_slack_fits( rate_b_hz, plan.num_ba, plan.den_ba, rate_a_hz )
#endif
      )
    {
        if( reason != NULL )
        {
            *reason = "no front-end row for this pair, and the direct step leaves the fill servo "
                      "no slack above the look-ahead (it would starve permanently)";
        }
        return false;
    }
    return true;
}

// Fill-drift control. The clocks differ by only ppm, so this loop must be SLOW: reacting to
// per-block fill wobble modulates the step (= playback speed) and FM-modulates the audio into
// audible sidebands (measured: the old fast loop capped 18 kHz SFDR at ~-33 dBc; the kernel
// itself is ~-93 dBc -- see [internal] asrc_quality_measurement_results.md). So: small gain, then a
// 1st-order LPF on the correction, then a per-block slew limit on the applied step.
#ifndef ASRC_KP                                        // -Define ASRC_KP=2.0e-5f for an A/B gain sweep
#define ASRC_KP            (1.0e-5f)                   // fill-error -> step correction gain. Q45: adopted
                                                       // 2x the old 5.0e-6 -- with step-smoothing carrying
                                                       // the limit-cycle rejection, the higher loop gain
                                                       // makes warm-up MONOTONIC and locks -127 dB by ~10 s
                                                       // (vs the old gain's -112..-127 swings to ~22 s);
                                                       // steady-state THD+N unchanged. See asrc_q45_*.md.
// corr-LPF coefficient. Q33: adopted the 3x-slower "tau x3" default (was 4.0e-3f) -- corner ~0.29 Hz
// (tau ~0.55 s) instead of ~0.86 Hz. Rejects the residual ~10 Hz fill-fractional-wrap component that
// the servo would otherwise pass into the applied step (Q26-Q31: it FM-modulates the carrier). KP is
// unchanged, so DC/ppm-drift authority is preserved. Q31: 8-13 Hz applied_step ->~1/3, THD+N +~6 dB on
// the coherent XTAL system, slow follow error unchanged. Q32: verified under real FRC rate wander the
// slow follow error stays unchanged (6.2 ppm), fill stays bounded, miss=0 -- i.e. NOT too sluggish for
// a moving rate. See [internal] asrc_q31_servo_bandwidth_tuning.md and
// [internal] asrc_q32_lpf_adoption_decision.md.
#endif // ASRC_KP
#ifndef ASRC_CORR_ALPHA                                // -Define ASRC_CORR_ALPHA=... for an A/B tau sweep
#define ASRC_CORR_ALPHA    (1.3333333e-3f)             // LPF on the correction (tau x3; ~0.29 Hz BW)
#endif // ASRC_CORR_ALPHA
#define ASRC_STEP_SLEW     (2.0e-6f)                   // max |step| change per block (limits FM depth)
// Q44: 1st-order LPF on the APPLIED step (~1.1 Hz corner at the ~1356 Hz pull rate). The applied step
// only needs to carry the slow rate correction (corr-LPF ~0.29 Hz); the >1 Hz producer limit cycle
// (historical Q38 measurement: 32-frame chunk delivery -> 7/65 super-period ~20.87 Hz) on it is pure
// FM noise. Smoothing it
// here removes the limit cycle at the resampler drive -> stable -127.7 dB (frozen-kernel ceiling).
// beta = 1 - exp(-2*pi*fc/pull_rate); ~1.1 Hz -> 0.005. See [internal] asrc_q44_stable_adi.md.
#define ASRC_STEP_SMOOTH_BETA (0.005f)
#if APP_ASRC_MEAS
// R15 Q15: 1st-order LP coeff for the corr_lpf band-split. beta = 1 - exp(-2*pi*fc/fs),
// fc = 1.0 Hz, fs = pull rate = FsB/32 = 390625/(APP_SPI2_MASTER_BRG+1)/32 = 1356.336 Hz (BRG8).
// 1 - exp(-2*pi/1356.336) = 0.00462185. MEAS-only; does not affect the shipping servo.
#define Q15_CORR_BETA      (0.00462185f)
#endif
#define ASRC_STEP_LO       (0.95f)                     // step clamp, fraction of the feed-forward ratio
#define ASRC_STEP_HI       (1.05f)
#define ASRC_SAMP_MAX      ( 8388607.0f)               // +2^23-1 (24-bit)
#define ASRC_SAMP_MIN      (-8388608.0f)               // -2^23

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
// Windowed-sinc polyphase FIR kernel. A prototype lowpass of length M*L is decomposed into L
// sub-filters (phases); the fractional read phase picks a sub-filter and we LINEARLY INTERP
// between adjacent sub-filters -> effective phase resolution ~= many times L at 1/N the table.
// Table = (L+1) x M floats, shared by both directions, generated once at startup.
// ASRC_POLY_M/L/MH/AHEAD/FC and the window selector are override-friendly
// configuration facts from asrc_app_config.h.
// R8 Q6: prototype window selector. BLACKMAN = shipping default (byte-identical). BLACKMAN_HARRIS
// deepens the first-spectral-image stopband ~+21 dB @18k / ~+32 dB @1k for the SAME M=32 (4-term vs
// 3-term cosine, computed once at startup -> zero audio-ISR / RAM cost). Host-validated, see §18.9.
#if (ASRC_POLY_WINDOW == ASRC_WINDOW_KAISER_11)
#define ASRC_KERNEL_NAME   ASRC_POLY_KAISER_NAME
#else
#define ASRC_KERNEL_NAME   "poly"
#endif
// Physical FIFO length: ring depth + a mirror overhang of M samples so the poly read window
// (base .. base+M-1, base in [0,FRAMES)) is always a CONTIGUOUS span. asrc_push writes the
// first M frames a second time at [FRAMES .. FRAMES+M-1]; the extra RAM is only M*4 B/channel.
#define ASRC_FIFO_PHYS     (ASRC_FIFO_FRAMES + ASRC_POLY_M)
#else
#define ASRC_KERNEL_NAME   "cubic"
#define ASRC_FIFO_PHYS     (ASRC_FIFO_FRAMES)
#endif


//===========================================================
// Enum & Struct typedef
//===========================================================
#if (ASRC_CH < 1u) || (ASRC_CH > 16u)
#error "ASRC_CH must be between 1 and 16"
#endif

/*
 * Channels the block buffer can carry between pulls.  A frame in the block buffer is
 * APP_SLOTS_PER_FS words wide, so anything read out of it must be bounded by THAT, not by
 * ASRC_CH -- with ASRC_CH > APP_SLOTS_PER_FS the extra channels never reach the buffer at all
 * (they go to the kernel's hidden_slots[]).  Named so the bound reads as a deliberate choice
 * rather than a typo'd loop limit.
 */
#define ASRC_CARRY_CH \
    ( ( (uint8_t)ASRC_CH < (uint8_t)APP_SLOTS_PER_FS ) ? (uint8_t)ASRC_CH : (uint8_t)APP_SLOTS_PER_FS )

#if APP_ASRC_TDM8_ONE_TO_ONE
_Static_assert( ASRC_CH == 8u,
                "TDM8 one-to-one ASRC mapping requires ASRC_CH == 8" );
_Static_assert( APP_SLOTS_PER_FS == 8u,
                "TDM8 one-to-one ASRC mapping requires eight physical slots" );
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
#if (ASRC_CH & 7u)
#error "ASRC_HISTORY_TILE8 requires ASRC_CH multiple of 8"
#endif
#if (ASRC_POLY_METHOD != ASRC_POLY_STREAM8)
#error "ASRC_HISTORY_TILE8 currently supports ASRC_POLY_STREAM8 only"
#endif
#endif
#if (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_HISTORY_LAYOUT != ASRC_HISTORY_CH_MAJOR)
#error "ASRC_POLY_STREAM8_PAIR currently requires ASRC_HISTORY_CH_MAJOR"
#endif
/*
 * STREAM8_PAIR two-output fast path vs the physical slot count.
 *
 * The pair kernels write a FIXED 16-int32 contiguous block per channel group:
 * out16[0..7] = frame0 and out16[8..15] = frame1 (see the header comment in
 * mchp_stream8_pair_slot_f32.s).  The layout is NOT strided by the physical slot
 * count.  Passing &d[c] therefore only lands correctly when a physical frame has
 * exactly 8 slots -- there d[0..7] and d[8..15] happen to be frame0 and frame1.
 * At 2 slots the frames are d[0..1] and d[2..3], so a direct call would both
 * mis-place the results and store past the DMA block.
 *
 * That does not require giving up the fused two-output kernel on a narrow bus.
 * The kernel writes through an int32_t*, and this file already exploits that for
 * channel groups beyond the slot count (the hidden_slots path below).  The same
 * mechanism serves the narrow-bus case: run the kernel into a local 16-int32
 * buffer and copy out only the slots the physical frame actually has.  The
 * arithmetic, the phase evolution and the emitted samples are unchanged; only
 * the destination of the store moves.
 *
 * ASRC_PAIR_SLOT_DIRECT_OK marks the 8-slot case where the kernel may still
 * write straight into the DMA block with no copy at all, so that build keeps its
 * exact previous instruction sequence.
 */
#define ASRC_PAIR_SLOT_FASTPATH_OK  (1)
#define ASRC_PAIR_SLOT_DIRECT_OK    (APP_SLOTS_PER_FS == 8u)

// Producer-side fixed block writers.  Keep the shipping stereo-replication entry
// on its aligned fast path, while also compiling and self-testing the TDM8 1:1
// primitive that a future physical 8-channel input path can call directly.
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_CH_MAJOR) && \
    (APP_ASRC_INTERP == ASRC_INTERP_POLY) && (ASRC_CH == 16u) && \
    (ASRC_FIFO_FRAMES == 128u) && \
    ( (ASRC_POLY_M == 30u) || (ASRC_POLY_M == 28u) ) && \
    (APP_BLOCK_FRAMES == 16u) && (APP_SLOTS_PER_FS == 8u)
#define ASRC_HAVE_FIXED_PUSH_BLOCK16 (1)
extern void mchp_asrc_push16_stereo30_aligned_f32( float32_t* history0,
                                                    const int32_t* src, uint32_t start_idx );
extern void mchp_asrc_push16_stereo30_f32( float32_t* history0,
                                            const int32_t* src, uint32_t start_idx );
extern void mchp_asrc_push8_tdm30_f32( float32_t* history0,
                                       const int32_t* src, uint32_t start_idx );
extern void mchp_asrc_push16_stereo28_f32( float32_t* history0,
                                            const int32_t* src, uint32_t start_idx );
extern void mchp_asrc_push8_tdm28_f32( float32_t* history0,
                                       const int32_t* src, uint32_t start_idx );
#if (ASRC_POLY_M == 30u)
#define ASRC_PUSH16_STEREO mchp_asrc_push16_stereo30_aligned_f32
#define ASRC_PUSH8_TDM     mchp_asrc_push8_tdm30_f32
#else
#define ASRC_PUSH16_STEREO mchp_asrc_push16_stereo28_f32
#define ASRC_PUSH8_TDM     mchp_asrc_push8_tdm28_f32
#endif
#else
#define ASRC_HAVE_FIXED_PUSH_BLOCK16 (0)
#endif

/* The assembly block push converts signed-24 to float as it stores, so it has no
 * Q31 meaning.  The Q31 build takes the C per-frame push below, where the whole
 * conversion is one mask.  Noted in the report: any push-side difference between
 * the two builds is a hand-written-asm-vs-C difference, not an arithmetic one. */
#if ASRC_SAMPLE_Q31
#undef  ASRC_HAVE_FIXED_PUSH_BLOCK16
#define ASRC_HAVE_FIXED_PUSH_BLOCK16 (0)
#endif

/*
 * The ASRC sample type.  Float is the shipping representation; Q31 makes the
 * ring, the coefficients and the accumulator integer.  s24-left IS Q31, so the
 * Q31 ring stores the codec word itself and the float path's (float)(slot >> 8)
 * conversion disappears rather than moving somewhere else.
 */
#if ASRC_SAMPLE_Q31
typedef int32_t asrc_samp_t;
#define ASRC_SAMP_ZERO  ((int32_t)0)
#else
typedef float   asrc_samp_t;
#define ASRC_SAMP_ZERO  (0.0f)
#endif

typedef struct {
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
    float             tile[ASRC_CH / 8u][ASRC_FIFO_PHYS][8u]; // [tile][frame][lane]
#else
    asrc_samp_t       ch[ASRC_CH][ASRC_FIFO_PHYS];   // per-channel ring (+ mirror overhang)
#endif
    volatile uint32_t wr;      // producer frame count
    /*
     * Consumer frame count -- WRITTEN ONLY BY THE CONSUMER (asrc_pull and the foreground
     * reset/re-seed paths).  The producer reads it for nothing and writes it never.
     *
     * It used to be written by both sides: the producer's overflow guard advanced rd to
     * discard the oldest frames it had just overwritten.  That was safe only because the two
     * TDM RX ISRs share PRIO_TDM_DMA and cannot preempt each other -- the consumer's batch
     * path takes a LOCAL copy of rd, runs the DSP kernel, then stores it back ABSOLUTELY, so
     * a producer-side advance landing in that window was erased and the consumer went on
     * reading frames the producer had already overwritten (a sample discontinuity on every
     * channel at once, not merely a wrong counter).  The guard therefore lives in the
     * consumer now (see asrc_pull), which makes rd single-writer and the ring lock-free
     * without depending on the two ISRs never preempting each other.
     * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 14.2.
     */
    volatile uint32_t rd;
    /*
     * Which producer last wrote this ring: 0 = asrc_push (direct), 1 = asrc_push_frames
     * (front-end decimated).  Producer-owned, single writer.  It exists only so the
     * consumer-side guard can charge a discard to the same counter the producer-side guard
     * used to charge it to (dbg_guard_drops vs dbg_intermediate_overflow); the two producers
     * are mutually exclusive for a given configuration (front-end den == 1 or != 1).
     */
    volatile uint8_t  prod_frames;
    float             frac;    // fractional read phase [0,1) (consumer only)
    volatile float    ratio;   // live feed-forward in/out ratio (0 = not yet measured -> silence)
    float             corr_lpf;   // low-pass-filtered fill correction (slow drift trim, consumer)
    float             step_state; // slew-limited resample step actually applied (consumer)
    float             step_smooth_st; // Q44: LPF state for the applied-step smoother (~1.1 Hz); strips
                                      // the producer limit cycle before it FMs the carrier. 0 = seed.
    float             fill_target;    // ratio-aware fill setpoint in frames (see ASRC_FILL_TARGET_MAX);
                                      // recomputed at each ratio-lock, == ASRC_FILL_TARGET when step<=1
    uint8_t           fill_target_capped; // 1 = R(step) did not fit the ring; setpoint clamped (telemetry)
    // --- telemetry (foundation): sampled in asrc_pull, read by audio_app_asrc_dbg_print ---
    uint32_t          dbg_fill;             // FIFO fill (frames) at last pull
    /*
     * Minimum of that SAME pull-start fill over the window between two telemetry prints, and the
     * look-ahead R(step) a pull needs at its start.  dbg_fill on its own is ONE asynchronous
     * sample per print (~2 s apart), so it cannot say how close a pull came to starving -- the
     * 32..49 spread on record came from those samples, not from the worst pull.  fmin against R
     * is the direct read of the margin ASRC_FILL_JITTER has to cover.
     */
    uint16_t          dbg_fill_min;
    uint16_t          dbg_R;
    /*
     * Starve-feasibility bound for the locked ratio, derived from the ring geometry at each
     * ratio-lock:  Jmax(step) = (FIFO - BLOCK - 20 - floor(step*(BLOCK-1))) / 2.  starve==0 is
     * reachable only while ASRC_FILL_JITTER <= Jmax.  It exists so the acceptance criterion can
     * be one sentence on every part instead of a hard-coded "starve==0".  Printed as the `!J` flag.
     * NOTE (2026-08-28): the "FIFO=64 cannot meet starve==0 -- 48->44.1 gives Jmax=6 against a
     * required 7" figure that used to stand here was the BLOCK=16 arithmetic.  The shipping AK128
     * profile runs APP_BLOCK_FRAMES=8, so 48->44.1 reports Jmax=14 and J=8 fits with room, and `!J`
     * is correctly absent there.  See [internal] report_ak128_starve_trace_2026-08-28.md.
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md T1.
     */
    uint8_t           dbg_jmax;
    float             dbg_step;             // resample step at last pull
    uint32_t          dbg_pull_ticks_max;   // peak raw timer ticks in asrc_pull, cleared on report
    volatile uint32_t dbg_rx_tick;          // high-res-timer count latched at the last asrc_push (producer
                                            // RX block arrival). Shipping fast-acquire (Q57/Q58 intra-block
                                            // phase at ratio-lock) + Q40 continuous-fill est (MEAS) use it.
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    uint32_t          dbg_intermediate_overflow;
    uint32_t          dbg_intermediate_underrun;
#endif
    /*
     * Frames discarded by the producer overflow guard in asrc_push(), cumulative and never
     * cleared on report.  Each discarded frame is a hard sample discontinuity across EVERY
     * channel at once -- one audible click -- and until 2026-08-20 the guard dropped them
     * silently, so an intermittent click was invisible to telemetry: `miss` counts consumer
     * starvation, and `sat` is the TDM transport's ISR-saturation counter
     * (audio_transport.c, sum.saturated_count), neither of which observes this path.  It has
     * to be cumulative rather than per-report because the telemetry line prints every ~2 s
     * and a guard excursion lasts one block.  Written only by the producer ISR and only read
     * for display, so a torn 32-bit read can misprint one sample but cannot corrupt the count
     * -- deliberately left non-atomic to keep the audio ISR free of a guard (same treatment as
     * dbg_intermediate_overflow above).
     */
    uint32_t          dbg_guard_drops;
    /*
     * Output frames the poly read window could not serve, so the previous frame was HELD
     * (wr - rd < ASRC_POLY_AHEAD + 1).  Until 2026-08-20 the starved branch fell through and
     * shipped the zero-initialised out[] -- digital silence on every channel at once, i.e. one
     * audible click, which is what the 44.1 kHz crackle was.  It now holds the previous frame,
     * so this counts stalls rather than clicks; it must still be driven to zero, because a
     * held frame is distortion even when it is not a click.  The only witness used to be
     * underrun_this_pull, compiled out unless the 48k->8k decimator is in the build (both
     * switches are 0 on AK128), so on this part the event was invisible.
     * Cumulative like dbg_guard_drops, same non-atomic reasoning.
     */
    uint32_t          dbg_starve_frames;
    /*
     * Last frame actually emitted by the previous pull, so a starve on the FIRST frame of a
     * block has something to hold.  Within a block the held frame is read straight back out
     * of the DMA buffer (d - APP_SLOTS_PER_FS), which is correct whichever kernel produced
     * it; only the block boundary needs carried state.  Updated once per pull, not per frame.
     */
    int32_t           last_out[ASRC_CH];
#if APP_ASRC_MEAS
    volatile float    freeze_step;          // bench: >0 = hold this CONSTANT step (no fill trim,
                                            // no feed-forward) to isolate control-loop jitter
    float             freeze_base_step;     // Q3: unperturbed converged step latched at freeze;
                                            // every perturbation is derived NON-cumulatively from it
    // R12 Q12: intentional FEED-FORWARD ratio freeze (NOT Mode-K). When ff_freeze, the controller
    // uses ff_frozen_ratio in place of the live a->ratio, but corr_lpf / clamp / slew / step_state
    // stay fully live -- so the step still moves via corr_lpf (~0.5 ppm) while the noisy CCP ratio
    // dither is removed. a->ratio keeps updating (live CCP) so telemetry can show it still moving.
    volatile uint8_t  ff_freeze;            // 1 = use ff_frozen_ratio instead of live a->ratio
    float             ff_frozen_ratio;      // latched a->ratio at freeze time
    // R13 Q13: hold corr_lpf constant (stop the fill-servo update) WITHOUT freezing step_state.
    // fill/raw_corr are still computed; target_step becomes constant (with FF ratio also frozen) but
    // clamp/slew/step_state stay live -- isolates whether corr_lpf motion is the -104 dB limiter.
    volatile uint8_t  corr_hold;            // 1 = skip the corr_lpf += ... update (hold current value)
    uint16_t          dbg_wraps;            // Q34: consumer wraps (rd advances) during the LAST block =
                                            // input frames consumed that block. Read-only telemetry.
    uint32_t          dbg_wr_prev;          // Q35: a->wr snapshot at the previous pull (for wr_advance)
    uint16_t          dbg_wr_adv;           // Q35: producer frames pushed since the previous pull
                                            // (a->wr delta over the same inter-pull interval). Read-only.
    // R14 Q14 MA64 + R15 Q15 corr-split state are SINGLETONS below (a MEAS build runs one-way A->B,
    // so asrc_pull only operates on the A->B engine -- one copy, not per-instance, to fit the RAM budget).
#endif
} asrc_t;

#if APP_ASRC_MEAS
// R14 Q14: N=64 full-rate moving average of raw fill, OBSERVATION path only (singleton -- AB only in
// MEAS). Runs every active pull (warm in all states); selector s_q14_use_ma chooses averaged vs raw
// fill for the servo. Controller equations after fill_used unchanged.
static uint8_t  s_q14_fill_hist[64];        // ring of last 64 raw fills (integer frames <=252 -> u8)
static float    s_q14_fill_sum   = 0.0f;    // running sum of s_q14_fill_hist
static uint8_t  s_q14_fill_pos   = 0u;      // ring write index
static uint8_t  s_q14_ma_ready   = 0u;      // 0 until history seeded to the first valid fill
static volatile uint8_t s_q14_use_ma = 0u;  // 1 = servo uses MA64 fill; 0 = raw fill
// Q40: continuous-fill estimator toggle (MEAS-only, servo-observation ONLY). 1 = feed the servo a
// de-staircased fill that interpolates each producer block between RX callbacks; 0 = integer fill.
static volatile uint8_t s_cfill_en = 0u;
// Q41: automatic phase-centering. Q40 used a fixed center of BLOCK/2, but the observed phase
// can average below BLOCK/2 (pulls land soon after pushes when RX rate > pull rate), biasing the fill setpoint
// bias. When s_pc_auto=1, the center is the auto-measured mean phase over a CAL-second window (sample
// count derived from the configured servo/pull rate, not a per-rate constant), then held fixed.
#define Q41_CAL_SECONDS   (1.0f)
// Servo/pull update rate from config: FsB/BLOCK = (390625/(BRG+1))/BLOCK. No hand-tuned per-rate value.
#define Q41_PULL_RATE_HZ  ( 390625.0f / (float)( APP_SPI2_MASTER_BRG + 1 ) / (float)APP_BLOCK_FRAMES )
#define Q41_CAL_N         ( (uint32_t)( Q41_CAL_SECONDS * Q41_PULL_RATE_HZ ) )
static volatile uint8_t s_pc_auto  = 0u;    // 1 = use auto-measured phase center; 0 = fixed BLOCK/2
static volatile uint8_t s_pc_ready = 0u;    // 1 once the calibration window has closed
static float            s_pc_sum   = 0.0f;  // running sum of observed phase during calibration
static uint32_t         s_pc_cnt   = 0u;    // calibration sample counter
static float            s_pc_center = 0.0f; // measured mean phase (frames); valid when s_pc_ready
// Q42: high-fidelity producer-phase slope from the MEASURED RX-block period (vs Q40/Q41's fixed
// elapsed*fs_A slope). Stage 1: average the producer RX-block interval over 1 s -> s_pp_period (us x10),
// then hold fixed. phase = BLOCK * elapsed_since_last_rx / s_pp_period. When ON, the phase_center
// calibration (Q41) is deferred until s_pp_ready so it averages the final slope. No per-rate constant.
#define Q42_CAL_US10      (1.0e7f)          // 1.0 s expressed in (microseconds x 10)
static volatile uint8_t s_hf_en    = 0u;    // 1 = measured-period slope; 0 = Q41 fixed fs_A slope
static volatile uint8_t s_pp_ready = 0u;    // 1 once the producer-period window has closed
static float            s_pp_sum   = 0.0f;  // sum of RX-block intervals (us x10) during calibration
static float            s_pp_total = 0.0f;  // total elapsed (us x10) accumulated -> 1 s window gate
static uint32_t         s_pp_cnt   = 0u;    // interval count
static float            s_pp_period = 0.0f; // measured mean RX-block interval (us x10); valid if ready
static uint32_t         s_pp_prev  = 0u;    // previous asrc_push tick (for the interval)

// R15 Q15: complementary 1 Hz split of live corr_lpf + motion-only band selector. SINGLETON state
// (a MEAS build runs one-way A->B, so asrc_pull only ever operates on the A->B engine -- one copy, not
// per-instance, to stay inside the tight MEAS RAM/stack budget). corr_slow = 1 Hz LP of corr_lpf;
// corr_fast = corr_lpf - corr_slow (local). FULL uses corr_lpf; SLOW uses corr_slow + hold(fast);
// FAST uses hold(slow) + corr_fast; HOLD uses hold(full). hold_val captured at the switch pull
// (continuous by construction). corr_lpf always updates -- this is NOT the Q13 corr_hold.
// g_ = global (extern'd by audio_app_meas.c so the sel=7 trace reads them without extra trace_tick
// args -- keeps the hot ISR call frame small). corr_used is recomputed in the trace from these.
float           g_q15_corr_slow    = 0.0f;   // 1 Hz LP of corr_lpf
float           g_q15_hold_val     = 0.0f;   // held component captured at the A->S/F/H transition
volatile uint8_t g_q15_corr_mode   = 0u;     // 0=FULL,1=SLOW-motion,2=FAST-motion,3=HOLD
static volatile uint8_t s_q15_capture_pending  = 0u;  // 1 = capture hold_val on the next pull
static uint8_t          s_q15_split_ready      = 0u;  // 0 until corr_slow seeded to corr_lpf

// R16 Q16: freeze-age instrumentation. s_q16_pull_ctr = monotonic per-pull counter (control rate);
// s_q16_freeze_epoch = the pull counter at the FIRST pull that uses the frozen FF (t=0); age_pulls =
// pull_ctr - freeze_epoch. Lets the host time-lock the aging trajectory to the firmware freeze event
// (not the serial command arrival). MEAS-only; reads/counts only, no controller change.
static uint32_t s_q16_pull_ctr      = 0u;
static uint32_t s_q16_freeze_epoch  = 0u;
static uint8_t  s_q16_freeze_latched = 0u;
#endif


//===========================================================
// Variables
//===========================================================
/*
 * The resampler engines are a table indexed by direction, not a set of
 * individually named variables.
 *
 * One engine converts one direction between one leg pair, so a topology with
 * more leg pairs simply declares more entries here and nothing else about the
 * engine changes.  Two rules make that widening free for the builds that do not
 * use it:
 *
 *   - ASRC_ENGINE_COUNT is compile-time, so an unused engine is never
 *     allocated.  asrc_t is about 10 KB (the FIFO rings dominate), which is why
 *     this must never become a runtime maximum.
 *   - every index below is a named compile-time constant, so `s_asrc[i]` folds
 *     to a direct address.  sizeof(asrc_t) is not a power of two, so a *runtime*
 *     index would cost a multiply at each use -- negligible per pull, but it
 *     would also cost the constant-folding that the kernels depend on.
 *
 * tools/asrc/hotpath_invariance.py checks both rules by comparing the hot-path
 * disassembly against a stored baseline.
 */
#define ASRC_ENGINE_AB  (0u)                    // A -> B
#if APP_B_ROUTE_USES_BA
#define ASRC_ENGINE_BA  (1u)                    // B -> A (cross direction: BIDIR / LIGHT)
#define ASRC_ENGINE_COUNT  (2u)
#else
#define ASRC_ENGINE_COUNT  (1u)
#endif

static asrc_t s_asrc[ASRC_ENGINE_COUNT];

#if APP_ASRC_LOAD_TEST
static volatile uint8_t s_load_mult = 1u;       // bench: emulate mult*ASRC_CH channels of interp load
static volatile float   s_load_sink = 0.0f;     // discard target (blocks dead-code elimination)
#endif
#if (ASRC_CH > APP_SLOTS_PER_FS)
static volatile uint32_t s_hidden_output_sink; // retain converted channels beyond physical TDM8
#endif


//===========================================================
// Local Functions (engine)
//===========================================================
static void asrc_reset( asrc_t* a )
{
    a->wr   = 0u;
    a->rd   = 0u;
    a->frac = 0.0f;
    a->ratio = 0.0f;   // invalid until a feed-forward ratio is set -> asrc_pull emits silence
    a->corr_lpf   = 0.0f;
    a->step_state = 0.0f;   // seeded to ratio on the first valid ratio (asrc_apply_ratio)
    a->step_smooth_st = 0.0f; // restart must not retain the previous rate's applied-step LPF state
    a->fill_target       = ASRC_FILL_TARGET;   // ratio-aware value latched at the next ratio-lock
    a->fill_target_capped = 0u;
    a->dbg_fill          = 0u;
    a->dbg_fill_min      = 0xFFFFu;
    a->dbg_R             = 0u;
    a->dbg_jmax          = 0u;
    a->dbg_step          = 0.0f;
    a->dbg_pull_ticks_max = 0u;
    a->dbg_guard_drops   = 0u;
    a->dbg_starve_frames = 0u;
    for( uint8_t c = 0u; c < ASRC_CH; c++ ) { a->last_out[c] = 0; }
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    a->dbg_intermediate_overflow = 0u;
    a->dbg_intermediate_underrun = 0u;
#endif
#if APP_ASRC_MEAS
    a->freeze_step       = 0.0f;   // not frozen
    a->freeze_base_step  = 0.0f;
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
    for( uint8_t t = 0u; t < ( ASRC_CH / 8u ); t++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ )
        {
            for( uint8_t l = 0u; l < 8u; l++ ) { a->tile[t][i][l] = 0.0f; }
        }
    }
#else
    for( uint8_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ ) { a->ch[c][i] = ASRC_SAMP_ZERO; }
    }
#endif
}

// Producer side: push this block's input frames into the per-channel rings.  The
// ordinary multi-channel profiles replicate physical L/R (ch c <- input slot
// c&1) to model a wider workload.  The explicit AK128 TDM8 profile maps slot c
// directly to channel c.  On overflow drop the oldest frame. The mirror keeps
// the poly read window contiguous (see asrc_poly_interp).
static void asrc_push( asrc_t* a, const int32_t* src )
{
    // Latch the producer RX-block arrival time. Used by the shipping fast-acquire (Q57/Q58: intra-block
    // phase at ratio-lock) AND the Q40 continuous-fill estimator (MEAS). One timer read + store per RX
    // block -- cheap enough for the producer ISR. (Was MEAS-only for Q40; promoted for fast-acquire.)
    const uint32_t now_tick = nora_high_res_timer_get_count();
    a->dbg_rx_tick = now_tick;
#if APP_ASRC_MEAS
    // Q42 stage 1: average the RX-block interval over a 1 s window -> producer_period, then hold fixed.
    if( s_hf_en && !s_pp_ready && s_pp_prev != 0u )
    {
        const uint32_t dt10 = nora_high_res_timer_elapsed_us_x10( s_pp_prev );  // interval, us x10
        s_pp_sum   += (float)dt10;
        s_pp_total += (float)dt10;
        s_pp_cnt++;
        if( s_pp_total >= Q42_CAL_US10 ) { s_pp_period = s_pp_sum / (float)s_pp_cnt; s_pp_ready = 1u; }
    }
    s_pp_prev     = now_tick;
#endif
#if ASRC_HAVE_FIXED_PUSH_BLOCK16
    const uint32_t wr      = a->wr;
    const uint32_t wr_next = wr + APP_BLOCK_FRAMES;

    // History writes complete before wr is published.  The assembly routine is
    // one call per 16-frame block; it preserves the exact signed-24 -> float and
    // main-ring/mirror semantics of the C fallback below.
    ASRC_PUSH16_STEREO( &a->ch[0][0], src, wr & ASRC_FIFO_MASK );

    // The producer publishes wr and NOTHING ELSE.  The overflow guard that used to advance rd
    // from here now runs in the consumer (asrc_pull), so rd has a single writer -- see the rd
    // declaration.  Overrunning the consumer is still detected and still counted, one pull
    // later, at which point the overwritten frames are equally gone either way.
    a->prod_frames = 0u;
    a->wr = wr_next;
#else
    const int32_t* p = src;
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        const uint32_t idx = a->wr & ASRC_FIFO_MASK;
        for( uint8_t c = 0u; c < ASRC_CH; c++ )
        {
#if APP_ASRC_TDM8_ONE_TO_ONE
            const uint8_t slot = c;
#else
            const uint8_t slot = c & 1u;
#endif
#if ASRC_SAMPLE_Q31
            // s24-left IS Q31: keep the codec word, drop only the sub-LSBs.
            const asrc_samp_t v = (int32_t)( (uint32_t)p[slot] & 0xFFFFFF00u );
#else
            const asrc_samp_t v = (float)( p[slot] >> 8 );   // 24-bit source slot
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
            a->tile[c >> 3u][idx][c & 7u] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
            if( idx < ASRC_POLY_M ) { a->tile[c >> 3u][idx + ASRC_FIFO_FRAMES][c & 7u] = v; }
#endif
#else
            a->ch[c][idx] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
            if( idx < ASRC_POLY_M ) { a->ch[c][idx + ASRC_FIFO_FRAMES] = v; }   // mirror overhang
#endif
#endif
        }
        a->wr++;   // overflow guard lives in the consumer now (see the rd declaration)
        p += APP_SLOTS_PER_FS;
    }
#endif
}

#if ASRC_HAVE_FIXED_PUSH_BLOCK16
// Validate both producer mappings over the mirror edges and the ring wrap.  The
// test uses the real history scratch, then audio_app_asrc_reset_all() clears it.
// Comparing raw float bits makes the assembly contract stricter than ==.
static uint8_t asrc_push_block_find_frame( uint32_t start_idx, uint32_t phys_idx,
                                           uint32_t* frame_out )
{
    for( uint32_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        const uint32_t idx = ( start_idx + n ) & ASRC_FIFO_MASK;
        if( ( phys_idx == idx ) ||
            ( ( phys_idx == idx + ASRC_FIFO_FRAMES ) && ( idx < ASRC_POLY_M ) ) )
        {
            *frame_out = n;
            return 1u;
        }
    }
    return 0u;
}

static uint8_t asrc_push_block_check( const int32_t* src, uint32_t start_idx,
                                      uint8_t tdm8_mapping )
{
    union { float f; uint32_t u; } sentinel = { .u = 0x4F123456u };
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ )
        {
            s_asrc[ASRC_ENGINE_AB].ch[c][i] = sentinel.f;
        }
    }

    if( tdm8_mapping )
    {
        ASRC_PUSH8_TDM( &s_asrc[ASRC_ENGINE_AB].ch[0][0], src, start_idx );
    }
    else
    {
        ASRC_PUSH16_STEREO( &s_asrc[ASRC_ENGINE_AB].ch[0][0], src, start_idx );
    }

    uint8_t fail = 0u;
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ )
        {
            uint32_t n = 0u;
            const uint8_t lane_is_written = ( !tdm8_mapping || ( c < 8u ) );
            const uint8_t position_is_written =
                lane_is_written && asrc_push_block_find_frame( start_idx, i, &n );
            union { float f; uint32_t u; } expected;
            if( position_is_written )
            {
                const uint32_t slot = tdm8_mapping ? c : ( c & 1u );
                expected.f = (float)( src[n * APP_SLOTS_PER_FS + slot] >> 8 );
            }
            else
            {
                expected.u = sentinel.u;
            }
            union { float f; uint32_t u; } got = { .f = s_asrc[ASRC_ENGINE_AB].ch[c][i] };
            if( got.u != expected.u ) { fail = 1u; }
        }
    }
    return fail;
}

static void asrc_push_block_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    static const uint32_t seed[8] = {
        0x80000000u, 0x7FFFFFFFu, 0x00000000u, 0xFFFFFF00u,
        0x00000100u, 0x7FFFFF00u, 0x80000100u, 0x12345678u
    };
    int32_t src[APP_BLOCK_FRAMES * APP_SLOTS_PER_FS];
    for( uint32_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
        for( uint32_t s = 0u; s < APP_SLOTS_PER_FS; s++ )
        {
            src[n * APP_SLOTS_PER_FS + s] =
                (int32_t)( seed[s] ^ ( n * 0x01010100u ) );
        }
    }

#if (ASRC_POLY_M == 30u)
    static const uint8_t start_idx[] = { 0u, 16u, 29u, 30u, 32u, 112u, 127u };
#else
    static const uint8_t start_idx[] = { 0u, ASRC_POLY_M - 1u,
                                         ASRC_POLY_M, ASRC_FIFO_FRAMES - 1u };
#endif
    uint8_t fail = 0u;
    for( uint32_t i = 0u; i < ( sizeof(start_idx) / sizeof(start_idx[0]) ); i++ )
    {
        fail |= asrc_push_block_check( src, start_idx[i], 0u );
        fail |= asrc_push_block_check( src, start_idx[i], 1u );
    }

    /*
     * The producer no longer touches rd, so what there is to prove here is the CONSUMER-side
     * guard: after one block is pushed and one pull corrects, fill is exactly
     * min(fill_before + block, FIFO-4) and rd advanced by exactly the number of frames the
     * ring could not hold.  Checked over normal, threshold and already-overfull starting fills
     * -- the last of which the old producer-side guard could not recover from at all.
     */
    for( uint32_t initial_fill = 0u; initial_fill <= ASRC_FIFO_FRAMES + 8u; initial_fill++ )
    {
        const uint32_t wr0 = 1000u;
        const uint32_t rd0 = wr0 - initial_fill;

        const uint32_t wr_after = wr0 + APP_BLOCK_FRAMES;     // producer: publish wr only
        const uint32_t fill_raw = wr_after - rd0;             // consumer: correct, then read
        const uint32_t excess   = ( fill_raw > ( ASRC_FIFO_FRAMES - 4u ) )
                                      ? ( fill_raw - ( ASRC_FIFO_FRAMES - 4u ) ) : 0u;
        const uint32_t rd_after = rd0 + excess;
        const uint32_t fill_after = wr_after - rd_after;

        const uint32_t fill_expect = ( fill_raw < ( ASRC_FIFO_FRAMES - 4u ) )
                                         ? fill_raw : ( ASRC_FIFO_FRAMES - 4u );
        if( fill_after != fill_expect ) { fail = 1u; }
        if( rd_after < rd0 )            { fail = 1u; }   // rd never moves backwards
    }
    printf( " ASRC push16/stereo + push8/TDM selftest: %s\n", fail ? "FAIL" : "pass" );
    if( fail ) { while( 1 ) { } }
}
#endif

#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
// Producer variant for fixed-decimator output. Unlike asrc_push(), frame count
// varies by the active front end (48->8 repeats 2,3,3; 48->16 repeats 5,5,6
// for 16-frame A blocks) and input stride is stereo. FIFO/fill/rd/wr units
// beyond this boundary are frames at that front end's intermediate rate.
static void asrc_push_frames( asrc_t* a, const int32_t* src,
                              size_t frames, size_t stride )
{
    a->dbg_rx_tick = nora_high_res_timer_get_count();
    a->prod_frames = 1u;   // charge a consumer-side discard to dbg_intermediate_overflow
    const int32_t* p = src;
    for( size_t n = 0u; n < frames; n++ )
    {
        const uint32_t idx = a->wr & ASRC_FIFO_MASK;
        for( uint8_t c = 0u; c < ASRC_CH; c++ )
        {
            /* Amplitude representation MUST match asrc_push()'s Q31 arm exactly: the two
             * producers write the same ring and a mismatch is silent (values 2^-8 too small,
             * = -48.1648 dB, with every build/selftest still passing).  Bit-exact equivalence
             * is enforced by asrc_push_frames_selftest() below. */
#if ASRC_SAMPLE_Q31
            // s24-left IS Q31: keep the codec word, drop only the sub-LSBs.
            const asrc_samp_t v = (int32_t)( (uint32_t)p[c & 1u] & 0xFFFFFF00u );
#else
            const asrc_samp_t v = (float)( p[c & 1u] >> 8 );
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
            a->tile[c >> 3u][idx][c & 7u] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
            if( idx < ASRC_POLY_M ) { a->tile[c >> 3u][idx + ASRC_FIFO_FRAMES][c & 7u] = v; }
#endif
#else
            a->ch[c][idx] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
            if( idx < ASRC_POLY_M ) { a->ch[c][idx + ASRC_FIFO_FRAMES] = v; }
#endif
#endif
        }
        a->wr++;   // overflow guard lives in the consumer now (see the rd declaration)
        p += stride;
    }
}

#if (ASRC_HISTORY_LAYOUT != ASRC_HISTORY_TILE8)
/* Regression guard for the defect this selftest was written after: asrc_push_frames() carried
 * no ASRC_SAMPLE_Q31 arm, so under Q31 it stored (float)(word >> 8) into an int32 Q31 ring --
 * every sample 2^-8 = -48.1648 dB too small, with the build, the boot selftests and every
 * functional counter still passing.  The only visible symptom was a quiet output, which is
 * exactly the kind of failure a numeric selftest has to catch.
 *
 * Runs at boot on the real AB engine (as asrc_push_block_check() does) because a scratch
 * asrc_t is ~20 KB and RAM is the binding constraint here; the engine is reset afterwards.
 * Bit-exact comparison via the raw word, never via float ==. */
static uint32_t asrc_ring_word( uint32_t c, uint32_t i )
{
    union { asrc_samp_t v; uint32_t u; } t;
    t.v = s_asrc[ASRC_ENGINE_AB].ch[c][i];
    return t.u;
}

/* The one place that defines "the correct stored representation": asrc_push()'s arm, quoted. */
static uint32_t asrc_expected_ring_word( int32_t slot_word )
{
    union { asrc_samp_t v; uint32_t u; } t;
#if ASRC_SAMPLE_Q31
    t.v = (int32_t)( (uint32_t)slot_word & 0xFFFFFF00u );
#else
    t.v = (float)( slot_word >> 8 );
#endif
    return t.u;
}

static uint32_t asrc_ring_digest( uint32_t frames_from, uint32_t frames_to )
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = frames_from; i < frames_to; i++ )
        {
            h = ( h ^ asrc_ring_word( c, i ) ) * 16777619u;
        }
    }
    return h;
}

static void asrc_ring_sentinel( void )
{
    union { asrc_samp_t v; uint32_t u; } s;
    s.u = 0x4F123456u;
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ )
        {
            s_asrc[ASRC_ENGINE_AB].ch[c][i] = s.v;
        }
    }
}

static void asrc_push_frames_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    asrc_t* const a = &s_asrc[ASRC_ENGINE_AB];
    uint8_t fail_value = 0u;   /* Test A/B: stored representation + ring position */
    uint8_t fail_index = 0u;   /* Test A/B: wr / rd advancement */
    uint8_t fail_equiv = 0u;   /* push() vs push_frames() bit-exact ring digest */

    /* Test C input patterns, in s24-left representation (1 LSB = 0x00000100). */
    static const uint32_t pat[10] = {
        0x00000000u, 0x00000100u, 0xFFFFFF00u, 0x00010000u, 0xFFFF0000u,
        0x7FFFFF00u, 0x80000000u, 0x7FFFFFFFu, 0x80000100u, 0x12345678u
    };

    int32_t src2[16 * 2];   /* stride-2 stereo source, the front end's own layout */
    uint32_t lcg = 0x1234567u;

    /* ---- Test A (N=1) and Test B (N=2,3,7,16), each over the Test C patterns ---- */
    static const uint32_t counts[5] = { 1u, 2u, 3u, 7u, 16u };
    for( uint32_t ci = 0u; ci < 5u; ci++ )
    {
        const uint32_t frames = counts[ci];
        for( uint32_t pi = 0u; pi < 12u; pi++ )
        {
            for( uint32_t n = 0u; n < frames; n++ )
            {
                for( uint32_t s = 0u; s < 2u; s++ )
                {
                    uint32_t w;
                    if( pi < 10u )
                    {
                        w = pat[pi];
                        if( ( ( n + s ) & 1u ) != 0u ) { w = (uint32_t)( -(int32_t)w ); }  /* alternating +/- */
                    }
                    else
                    {
                        lcg = ( lcg * 1664525u ) + 1013904223u;
                        w = lcg & 0xFFFFFF00u;
                    }
                    src2[( n * 2u ) + s] = (int32_t)w;
                }
            }

            /* Known, non-overflowing start state at a wrap-crossing write index. */
            const uint32_t wr0 = ASRC_FIFO_FRAMES - 3u;
            a->wr = wr0;
            a->rd = wr0;
            a->ratio = 0.0f;
            asrc_ring_sentinel();

            asrc_push_frames( a, src2, frames, 2u );

            /* wr advances by exactly the frames pushed; rd is the CONSUMER's and this
             * producer must not have touched it (no reference loop: the producer has no
             * overflow guard any more -- see the rd declaration). */
            if( ( a->wr != ( wr0 + (uint32_t)frames ) ) || ( a->rd != wr0 ) ) { fail_index = 1u; }

            /* Every written position holds asrc_push()'s representation; nothing else moved. */
            union { asrc_samp_t v; uint32_t u; } sent;
            sent.u = 0x4F123456u;
            for( uint32_t c = 0u; c < ASRC_CH; c++ )
            {
                for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ )
                {
                    uint32_t want = sent.u;
                    for( uint32_t n = 0u; n < frames; n++ )
                    {
                        const uint32_t idx = ( wr0 + n ) & ASRC_FIFO_MASK;
                        const uint8_t hit = ( i == idx ) ||
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
                                            ( ( idx < ASRC_POLY_M ) && ( i == idx + ASRC_FIFO_FRAMES ) );
#else
                                            0u;
#endif
                        if( hit )
                        {
                            want = asrc_expected_ring_word( src2[( n * 2u ) + ( c & 1u )] );
                        }
                    }
                    if( asrc_ring_word( c, i ) != want ) { fail_value = 1u; }
                }
            }
        }
    }

    /* ---- push() vs push_frames() bit-exact equivalence over a full block ----
     * Only comparable in the non-TDM8 mapping, where push() also takes slot = c & 1u.
     * Counters are deliberately NOT compared: the two producers account overflow
     * differently by design (dbg_guard_drops vs dbg_intermediate_overflow). */
#if !APP_ASRC_TDM8_ONE_TO_ONE
    {
        int32_t srcb[APP_BLOCK_FRAMES * APP_SLOTS_PER_FS];
        for( uint32_t n = 0u; n < (uint32_t)APP_BLOCK_FRAMES; n++ )
        {
            for( uint32_t s = 0u; s < (uint32_t)APP_SLOTS_PER_FS; s++ )
            {
                lcg = ( lcg * 1664525u ) + 1013904223u;
                srcb[( n * (uint32_t)APP_SLOTS_PER_FS ) + s] = (int32_t)( lcg & 0xFFFFFF00u );
            }
            src2[( n * 2u ) + 0u] = srcb[( n * (uint32_t)APP_SLOTS_PER_FS ) + 0u];
            src2[( n * 2u ) + 1u] = srcb[( n * (uint32_t)APP_SLOTS_PER_FS ) + 1u];
        }

        const uint32_t wr0 = ASRC_FIFO_FRAMES - 3u;
        a->wr = wr0; a->rd = wr0; a->ratio = 0.0f;
        asrc_ring_sentinel();
        asrc_push( a, srcb );
        const uint32_t d_push = asrc_ring_digest( 0u, ASRC_FIFO_PHYS );
        const uint32_t wr_push = a->wr, rd_push = a->rd;

        a->wr = wr0; a->rd = wr0; a->ratio = 0.0f;
        asrc_ring_sentinel();
        asrc_push_frames( a, src2, (size_t)APP_BLOCK_FRAMES, 2u );
        if( asrc_ring_digest( 0u, ASRC_FIFO_PHYS ) != d_push ) { fail_equiv = 1u; }
        if( ( a->wr != wr_push ) || ( a->rd != rd_push ) )     { fail_equiv = 1u; }
    }
#endif

    /* Leave the engine clean for the real stream. */
    a->wr = 0u; a->rd = 0u; a->ratio = 0.0f;
    a->dbg_intermediate_overflow = 0u;
    a->dbg_guard_drops = 0u;
    asrc_ring_sentinel();
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ ) { s_asrc[ASRC_ENGINE_AB].ch[c][i] = ASRC_SAMP_ZERO; }
    }

    printf( " ASRC push_frames selftest: %s (value %u, index %u, push-equiv %u, arm %s)\n",
            ( fail_value || fail_index || fail_equiv ) ? "FAIL" : "pass",
            (unsigned)fail_value, (unsigned)fail_index, (unsigned)fail_equiv,
#if ASRC_SAMPLE_Q31
            "Q31" );
#else
            "float" );
#endif
}
#endif // ASRC_HISTORY_LAYOUT != ASRC_HISTORY_TILE8
#endif

// Catmull-Rom (4-point cubic Hermite): interpolate between y1 and y2 at t in [0,1).
static inline float asrc_cubic( float y0, float y1, float y2, float y3, float t )
{
    const float a0 = -0.5f*y0 + 1.5f*y1 - 1.5f*y2 + 0.5f*y3;
    const float a1 =       y0 - 2.5f*y1 + 2.0f*y2 - 0.5f*y3;
    const float a2 = -0.5f*y0            + 0.5f*y2;
    return ( ( a0*t + a1 )*t + a2 )*t + y1;
}

static inline int32_t asrc_to_slot( float y )
{
    if( y > ASRC_SAMP_MAX ) { y = ASRC_SAMP_MAX; }
    if( y < ASRC_SAMP_MIN ) { y = ASRC_SAMP_MIN; }
#if ASRC_FAST_SLOT_CONVERT
    return (int32_t)( (uint32_t)(int32_t)y << 8 ); // fast truncation; error < one 24-bit LSB
#else
    return ( (int32_t)lrintf( y ) ) << 8;   // back to 32-bit left-justified
#endif
}

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR)
// One-time scalar-reference check for the hand-written batch converter. Guard words also catch an
// off-by-one loop bound. This runs before transport start and has no steady-state cost.
static void asrc_slot_batch_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    const float src[16] = {
        0.0f, 1.9f, -1.9f, 12345.75f, -12345.75f, 8388607.0f, -8388608.0f, 9000000.0f,
        -9000000.0f, 0.5f, -0.5f, 4194303.5f, -4194304.5f, 77.25f, -88.75f, 42.0f
    };
    struct {
        uint32_t pre;
        int32_t  y0[8];
        uint32_t middle;
        int32_t  y1[8];
        uint32_t post;
    } out = { .pre = 0x13579BDFu, .middle = 0x2468ACE0u, .post = 0xA55A5AA5u };

    mchp_f32_to_slot8_pair( src, out.y0, out.y1 );
    uint8_t fail = ( out.pre != 0x13579BDFu ) || ( out.middle != 0x2468ACE0u ) ||
                   ( out.post != 0xA55A5AA5u );
    for( uint8_t i = 0u; i < 8u; i++ )
    {
        if( out.y0[i] != asrc_to_slot( src[i] ) ) { fail = 1u; }
        if( out.y1[i] != asrc_to_slot( src[8u + i] ) ) { fail = 1u; }
    }
    printf( " ASRC slot batch selftest: %s\n", fail ? "FAIL" : "pass" );
}
#endif

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
// --- R8 Q2: coefficient storage selector (bench feasibility spike; RAM is the shipping default) ---
//   RAM   : windowed-sinc generated into writable RAM at startup (existing behaviour).
//   FLASH : the SAME 32-bit float bits, extracted exactly and compiled as a const table resident
//           in program flash (zero-copy, directly addressed by the dot-product asm). Read-only.
//   The ONLY thing that changes is where c0/c1 point; kernel arithmetic is byte-identical.
#define ASRC_COEFF_STORAGE_RAM   (0)
#define ASRC_COEFF_STORAGE_FLASH (1)
#ifndef ASRC_COEFF_STORAGE
#define ASRC_COEFF_STORAGE  (ASRC_COEFF_STORAGE_RAM)
#endif

// Polyphase coefficient table (L+1 phases x M taps), shared by both directions. Row p holds
// the sub-filter for read phase p/L; the guard row [L] lets the inter-phase interp reach p+1.
/* The float coefficient table.  The Q31 build has its own (asrc_poly_q31.inc) and
 * cannot afford both in RAM -- 2 x 15,480 B against ~3.3 kB free -- so the float
 * one is compiled out entirely rather than kept as dead weight. */
#if !ASRC_SAMPLE_Q31
#if (ASRC_COEFF_STORAGE == ASRC_COEFF_STORAGE_FLASH)
// One table per (L, M) geometry. Each generated header declares its own array plus an
// <NAME>_N element count; the assert below cross-checks that count against the ASRC_POLY_*
// macros this build resolved to, so a table paired with the wrong geometry cannot link.
// Add an arm here after generating a table with tools/gen_asrc_poly_flash_table.py.
#if (ASRC_POLY_L == 128u) && (ASRC_POLY_M == 32u)
#  include "audio_app_asrc_poly_l128_flash.h"
#  define ASRC_POLY_FLASH_TABLE  asrc_poly_l128_flash
#  define ASRC_POLY_FLASH_N      ASRC_POLY_L128_FLASH_N
#elif (ASRC_POLY_L == 128u) && (ASRC_POLY_M == 30u)
#  include "audio_app_asrc_poly_l128m30_flash.h"
#  define ASRC_POLY_FLASH_TABLE  asrc_poly_l128m30_flash
#  define ASRC_POLY_FLASH_N      ASRC_POLY_L128M30_FLASH_N
#elif (ASRC_POLY_L == 64u) && (ASRC_POLY_M == 30u)
#  include "audio_app_asrc_poly_l64m30_flash.h"
#  define ASRC_POLY_FLASH_TABLE  asrc_poly_l64m30_flash
#  define ASRC_POLY_FLASH_N      ASRC_POLY_L64M30_FLASH_N
#else
#  error "No flash coefficient table for this (ASRC_POLY_L, ASRC_POLY_M). Generate one with tools/gen_asrc_poly_flash_table.py --L <L> --M <M> --fc <fc> --window <w>, add it to the MPLAB project, and add an arm above."
#endif
_Static_assert( ( ( ASRC_POLY_L + 1u ) * ASRC_POLY_M ) == ASRC_POLY_FLASH_N,
                "flash coefficient table size does not match ASRC_POLY_L/ASRC_POLY_M" );
// Reinterpret the exact 32-bit words as float rows. Same size/alignment (4 B); the dot asm just
// loads 32-bit words, so this is bit-exact and the extern asm is opaque to alias analysis.
#define ASRC_POLY_ROW(p)  ((const float*)&ASRC_POLY_FLASH_TABLE[(uint32_t)(p) * ASRC_POLY_M])
#else
static float   s_poly[ASRC_POLY_L + 1u][ASRC_POLY_M];
static uint8_t s_poly_ready = 0u;
#define ASRC_POLY_ROW(p)  (s_poly[p])
#endif
#endif /* !ASRC_SAMPLE_Q31 */

#if (ASRC_POLY_WINDOW == ASRC_WINDOW_KAISER_11)
// Modified-Bessel I0 for the startup-only Kaiser coefficient generator.  The
// positive power series converges comfortably for beta=11 in float precision.
static float asrc_bessel_i0( float x )
{
    const float y = 0.25f * x * x;
    float term = 1.0f;
    float sum  = 1.0f;
    for( uint8_t k = 1u; k <= 24u; k++ )
    {
        const float fk = (float)k;
        term *= y / ( fk * fk );
        sum  += term;
        if( term <= sum * 1.0e-7f ) { break; }
    }
    return sum;
}
#endif

// Build the windowed-sinc prototype and fold it into the polyphase table (once, at startup,
// app context -- NOT in the ISR). For phase p, tap k: the tap sits at input-sample distance
// d = k - MH - p/L from the output position; coefficient = 2*fc*sinc(2*fc*d) * Blackman(d).
// Each phase row is normalised to unity DC gain so the passband level is exact.
#if !ASRC_SAMPLE_Q31
static void asrc_poly_build( void )
{
#if (ASRC_COEFF_STORAGE == ASRC_COEFF_STORAGE_FLASH)
    return;   // coefficients are resident in flash; nothing to generate into RAM
#else
    if( s_poly_ready ) { return; }
    const float pi = 3.14159265358979f;
#if (ASRC_POLY_WINDOW == ASRC_WINDOW_KAISER_11)
    const float kaiser_beta = ASRC_POLY_KAISER_BETA;
    const float kaiser_norm = 1.0f / asrc_bessel_i0( kaiser_beta );
#endif
    for( uint32_t p = 0u; p <= ASRC_POLY_L; p++ )
    {
        float sum = 0.0f;
        for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
        {
            const float d = (float)k - (float)ASRC_POLY_MH - (float)p / (float)ASRC_POLY_L;
            const float x = 2.0f * ASRC_POLY_FC * d;          // sinc argument
            float sinc;
            if( ( x < 1.0e-6f ) && ( x > -1.0e-6f ) ) { sinc = 1.0f; }
            else { const float px = pi * x; sinc = sinf( px ) / px; }
            const float wpos = ( d + (float)ASRC_POLY_MH + 1.0f ) / (float)ASRC_POLY_M;  // ~[0,1]
#if (ASRC_POLY_WINDOW == ASRC_WINDOW_BLACKMAN_HARRIS)
            const float win  = 0.35875f - 0.48829f*cosf( 2.0f*pi*wpos )
                             + 0.14128f*cosf( 4.0f*pi*wpos ) - 0.01168f*cosf( 6.0f*pi*wpos );
#elif (ASRC_POLY_WINDOW == ASRC_WINDOW_KAISER_11)
            const float kx = 2.0f * wpos - 1.0f;
            const float kr = sqrtf( fmaxf( 0.0f, 1.0f - kx * kx ) );
            const float win = asrc_bessel_i0( kaiser_beta * kr ) * kaiser_norm;
#else
            const float win  = 0.42f - 0.5f*cosf( 2.0f*pi*wpos ) + 0.08f*cosf( 4.0f*pi*wpos );
#endif
            const float c    = 2.0f * ASRC_POLY_FC * sinc * win;
            s_poly[p][k] = c;
            sum += c;
        }
        if( ( sum > 1.0e-9f ) || ( sum < -1.0e-9f ) )         // normalise row to unity DC
        {
            const float inv = 1.0f / sum;
            for( uint32_t k = 0u; k < ASRC_POLY_M; k++ ) { s_poly[p][k] *= inv; }
        }
    }
    s_poly_ready = 1u;
#endif  // ASRC_COEFF_STORAGE
}
#endif /* !ASRC_SAMPLE_Q31 */

#if ASRC_SAMPLE_Q31
#include "asrc_poly_q31.inc"
#endif

#if (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR)
// Verify that the fused FIR+slot kernel is exactly the existing float pair kernel followed by the
// already-verified batch converter. The scratch history is cleared by asrc_reset immediately after.
static void asrc_pair_slot_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    for( uint8_t c = 0u; c < 8u; c++ )
    {
        for( uint8_t i = 0u; i <= ASRC_POLY_M; i++ )
        {
            s_asrc[ASRC_ENGINE_AB].ch[c][i] = (float)( (int32_t)( c + 1u ) * 1003 + (int32_t)i * 37 - 4096 );
        }
    }

    float   ref_f[16];
    int32_t ref_i[16];
    struct {
        uint32_t pre;
        int32_t  slot[16];
        uint32_t post;
    } got = { .pre = 0xA17E5A5Au, .post = 0x5AA57E1Au };
    const float* c00 = ASRC_POLY_ROW( 7u );
    const float* c01 = ASRC_POLY_ROW( 8u );
    const float* c10 = ASRC_POLY_ROW( 9u );
    const float* c11 = ASRC_POLY_ROW( 10u );

    mchp_stream8_pair_f32( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                           (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                           c00, c01, ASRC_POLY_M, ref_f, c10, c11, 0.25f, 0.75f );
    mchp_f32_to_slot8_pair( ref_f, ref_i, &ref_i[8] );
    mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                c00, c01, ASRC_POLY_M, got.slot, c10, c11, 0.25f, 0.75f );

    uint8_t fail = ( got.pre != 0xA17E5A5Au ) || ( got.post != 0x5AA57E1Au );
    for( uint8_t i = 0u; i < 16u; i++ )
    {
        if( got.slot[i] != ref_i[i] ) { fail = 1u; }
    }
    printf( " ASRC fused pair-slot selftest: %s\n", fail ? "FAIL" : "pass" );
}

#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
// Verify the 16-channel wrapper against two calls to the already-checked 8-channel fused kernel.
// Exercise both coefficient-prep branches: distinct phase rows, then one shared row pair with
// different blend weights for the two outputs (the usual near-ratio-1 case).
static uint8_t s_pair16_selftest_fail;
static void asrc_pair16_slot_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    for( uint8_t c = 0u; c < 16u; c++ )
    {
        for( uint8_t i = 0u; i <= ASRC_POLY_M; i++ )
        {
            s_asrc[ASRC_ENGINE_AB].ch[c][i] =
                (float)( (int32_t)( c + 1u ) * 997 + (int32_t)i * 41 - 8192 );
        }
    }

    int32_t ref[32];
    struct {
        uint32_t pre;
        int32_t  slot[32];
        uint32_t post;
    } got = { .pre = 0x16A55A16u, .post = 0x615AA561u };
    const float* c00 = ASRC_POLY_ROW( 11u );
    const float* c01 = ASRC_POLY_ROW( 12u );
    const float* c10 = ASRC_POLY_ROW( 13u );
    const float* c11 = ASRC_POLY_ROW( 14u );

    mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                c00, c01, ASRC_POLY_M, &ref[0],
                                c10, c11, 0.375f, 0.625f );
    mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[8][0],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                c00, c01, ASRC_POLY_M, &ref[16],
                                c10, c11, 0.375f, 0.625f );
    ASRC_STREAM16_PAIR_SLOT( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                                   (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                   c00, c01, &got.slot[0], &got.slot[16],
                                   c10, c11, 0.375f, 0.625f );

    uint8_t fail = ( got.pre != 0x16A55A16u ) || ( got.post != 0x615AA561u );
    for( uint8_t i = 0u; i < 32u; i++ )
    {
        if( got.slot[i] != ref[i] ) { fail = 1u; }
    }

    c10 = c00;
    c11 = c01;
    got.pre  = 0x16A55A16u;
    got.post = 0x615AA561u;
    mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                c00, c01, ASRC_POLY_M, &ref[0],
                                c10, c11, 0.1875f, 0.8125f );
    mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[8][0],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                c00, c01, ASRC_POLY_M, &ref[16],
                                c10, c11, 0.1875f, 0.8125f );
    ASRC_STREAM16_PAIR_SLOT( &s_asrc[ASRC_ENGINE_AB].ch[0][0],
                             (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                             c00, c01, &got.slot[0], &got.slot[16],
                             c10, c11, 0.1875f, 0.8125f );
    if( ( got.pre != 0x16A55A16u ) || ( got.post != 0x615AA561u ) ) { fail = 1u; }
    for( uint8_t i = 0u; i < 32u; i++ )
    {
        if( got.slot[i] != ref[i] ) { fail = 1u; }
    }
    s_pair16_selftest_fail = fail;
}

// Verify descriptor iteration and output stride against two independently checked wrapper calls.
// Descriptor 0 uses distinct rows; descriptor 1 uses the shared-row prep branch.
static void asrc_block16_slot_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    const mchp_stream16_pair_desc_t desc[2] = {
        { &s_asrc[ASRC_ENGINE_AB].ch[0][0], ASRC_POLY_ROW( 3u ), ASRC_POLY_ROW( 4u ),
          ASRC_POLY_ROW( 5u ), ASRC_POLY_ROW( 6u ), 0.125f, 0.875f },
        { &s_asrc[ASRC_ENGINE_AB].ch[0][1], ASRC_POLY_ROW( 15u ), ASRC_POLY_ROW( 16u ),
          ASRC_POLY_ROW( 15u ), ASRC_POLY_ROW( 16u ), 0.625f, 0.375f }
    };
    int32_t ref_out[32];
    int32_t ref_hidden[16];
    int32_t scratch_hidden[16];
    struct {
        uint32_t pre;
        int32_t  slot[32];
        uint32_t mid;
        int32_t  hidden[16];
        uint32_t post;
    } got = { .pre = 0xB10CB10Cu, .mid = 0xC01DC01Du, .post = 0xE16DE16Du };

    for( uint8_t i = 0u; i < 2u; i++ )
    {
        ASRC_STREAM16_PAIR_SLOT(
            desc[i].wbase0, (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
            desc[i].c00, desc[i].c01, &ref_out[16u * i],
            ( i == 1u ) ? ref_hidden : scratch_hidden,
            desc[i].c10, desc[i].c11, desc[i].wb0, desc[i].wb1 );
    }
    ASRC_STREAM16_BLOCK_SLOT(
        desc, 2u, (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ), got.slot, got.hidden );

    uint8_t fail = ( got.pre != 0xB10CB10Cu ) || ( got.mid != 0xC01DC01Du ) ||
                   ( got.post != 0xE16DE16Du );
    for( uint8_t i = 0u; i < 32u; i++ )
    {
        if( got.slot[i] != ref_out[i] ) { fail = 1u; }
    }
    for( uint8_t i = 0u; i < 16u; i++ )
    {
        if( got.hidden[i] != ref_hidden[i] ) { fail = 1u; }
    }
    fail |= s_pair16_selftest_fail;
    printf( " ASRC fused 16ch/block selftest: %s\n", fail ? "FAIL" : "pass" );
    if( fail ) { while( 1 ) { } }
}

// Verify the union-window kernel (any output stride d) against the already-checked 8-channel
// fused kernel, whose second output is always one frame ahead: the reference for output 1 at
// window base+d is that kernel's output-1 half evaluated at base+d-1. Both halves therefore come
// from independent code (in-loop blend, no zero padding, no mul.s seeding), so a pass means the
// hoisted blend, the zero padding and the odd-length rounding are all bit-exact -- multiplying a
// history word by +0.0 can only turn a +0.0 partial sum into -0.0, which packs to the same int32.
static void asrc_paird16_slot_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    const uint32_t stride = (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) );
    const uint32_t base   = 1u;          // d == 0 needs base >= 1 for the reference call
    const uint32_t last   = ASRC_POLY_M + ASRC_PAIRD_DMAX + 2u;
    for( uint8_t c = 0u; c < 16u; c++ )
    {
        for( uint32_t i = 0u; i <= last; i++ )
        {
            s_asrc[ASRC_ENGINE_AB].ch[c][i] =
                (float)( (int32_t)( c + 1u ) * 1013 - (int32_t)i * 37 + 4096 );
        }
    }

    static const uint8_t dsweep[] = { 0u, 2u, 3u, 4u, 6u, 8u };
    const float* c00 = ASRC_POLY_ROW( 11u );
    const float* c01 = ASRC_POLY_ROW( 12u );
    const float* c10 = ASRC_POLY_ROW( 13u );
    const float* c11 = ASRC_POLY_ROW( 14u );
    uint8_t fail = 0u;

    for( uint8_t k = 0u; k < (uint8_t)( sizeof(dsweep) / sizeof(dsweep[0]) ); k++ )
    {
        const uint32_t dstep = dsweep[k];
        int32_t ref[32];
        int32_t tmp[16];

        for( uint8_t g = 0u; g < 2u; g++ )
        {
            const uint8_t c0 = (uint8_t)( 8u * g );
            mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[c0][base], stride,
                                        c00, c01, ASRC_POLY_M, tmp,
                                        c10, c11, 0.375f, 0.625f );
            for( uint8_t l = 0u; l < 8u; l++ ) { ref[16u * g + l] = tmp[l]; }
            mchp_stream8_pair_slot_f32( &s_asrc[ASRC_ENGINE_AB].ch[c0][base + dstep - 1u], stride,
                                        c00, c01, ASRC_POLY_M, tmp,
                                        c10, c11, 0.375f, 0.625f );
            for( uint8_t l = 0u; l < 8u; l++ ) { ref[16u * g + 8u + l] = tmp[8u + l]; }
        }

        struct {
            uint32_t pre;
            int32_t  slot[16];
            uint32_t mid;
            int32_t  hidden[16];
            uint32_t post;
        } got = { .pre = 0xD5A50D5Au, .mid = 0x0DD5A55Au, .post = 0xA55A0DD5u };

        mchp_stream16_paird_desc_t pd;
        pd.wbase0 = &s_asrc[ASRC_ENGINE_AB].ch[0][base];
        pd.c00    = c00;
        pd.c01    = c01;
        pd.c10    = c10;
        pd.c11    = c11;
        pd.wb0    = 0.375f;
        pd.wb1    = 0.625f;
        asrc_paird_fill( &pd, dstep );
        mchp_stream16_paird_f32( &pd, stride, got.slot, got.hidden );

        if( ( got.pre != 0xD5A50D5Au ) || ( got.mid != 0x0DD5A55Au ) ||
            ( got.post != 0xA55A0DD5u ) ) { fail = 1u; }
        if( ( pd.utaps & 1u ) == 0u || pd.utaps > ASRC_PAIRD_UMAX ) { fail = 1u; }
        for( uint8_t l = 0u; l < 16u; l++ )
        {
            if( got.slot[l]   != ref[l] )        { fail = 1u; }
            if( got.hidden[l] != ref[16u + l] )  { fail = 1u; }
        }
    }

    printf( " ASRC union-window (any-stride) pair selftest: %s\n", fail ? "FAIL" : "pass" );
    if( fail ) { while( 1 ) { } }
}

// --- Kernel micro-benchmark ("*az") -------------------------------------------------------------
// The telemetry `pull=`/`cbA=` numbers are ISR peaks: they include whatever nested inside the block
// ISR (ADC, UART TX) and vary by ~6% window to window -- the same order as the kernel changes worth
// making.  This times the kernel itself from the foreground and reports the MINIMUM over `trials`.
// The minimum is the trial that no interrupt hit, so it is a clean instruction-count metric.
//
// It runs against the live A->B history but only READS it (and writes local scratch), so it is safe
// to call while audio is streaming.  `near` counts trials within 1/16 of the minimum: a healthy
// sample has many, which is the evidence that the reported floor is the real uninterrupted cost and
// not a timer artefact.
void audio_app_asrc_kernel_bench( uint32_t trials )
{
    if( trials == 0u ) { trials = 200u; }

    const uint32_t stride = (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) );
    const float* c00 = ASRC_POLY_ROW( 11u );
    const float* c01 = ASRC_POLY_ROW( 12u );
    const float* c10 = ASRC_POLY_ROW( 13u );
    const float* c11 = ASRC_POLY_ROW( 14u );
    float* const base = &s_asrc[ASRC_ENGINE_AB].ch[0][0];

    int32_t out_lo[16], out_hi[16];
    uint32_t best8 = 0xFFFFFFFFu, best16 = 0xFFFFFFFFu, bestd = 0xFFFFFFFFu;
    uint32_t near8 = 0u, near16 = 0u, neard = 0u;

    // Same 16 channels x 2 outputs through the union-window kernel at the 48k->16k stride (d=3),
    // i.e. the case that used to run as four single-output STREAM8 passes plus 32 C conversions.
    mchp_stream16_paird_desc_t pd;
    pd.wbase0 = base;
    pd.c00    = c00;
    pd.c01    = c01;
    pd.c10    = c10;
    pd.c11    = c11;
    pd.wb0    = 0.375f;
    pd.wb1    = 0.625f;
    asrc_paird_fill( &pd, 3u );

    for( uint32_t t = 0u; t < trials; t++ )
    {
        uint32_t t0 = nora_high_res_timer_get_count();
        mchp_stream8_pair_slot_f32( base, stride, c00, c01, ASRC_POLY_M, out_lo,
                                   c10, c11, 0.375f, 0.625f );
        const uint32_t d8 = nora_high_res_timer_get_count() - t0;

        t0 = nora_high_res_timer_get_count();
        ASRC_STREAM16_PAIR_SLOT( base, stride, c00, c01, out_lo, out_hi,
                                 c10, c11, 0.375f, 0.625f );
        const uint32_t d16 = nora_high_res_timer_get_count() - t0;

        t0 = nora_high_res_timer_get_count();
        mchp_stream16_paird_f32( &pd, stride, out_lo, out_hi );
        const uint32_t dd = nora_high_res_timer_get_count() - t0;

        if( d8 < best8 )   { best8 = d8;   near8 = 0u; }
        if( d16 < best16 ) { best16 = d16; near16 = 0u; }
        if( dd  < bestd )  { bestd  = dd;  neard  = 0u; }
        if( d8 <= best8 + ( best8 >> 4 ) )    { near8++; }
        if( d16 <= best16 + ( best16 >> 4 ) ) { near16++; }
        if( dd  <= bestd + ( bestd >> 4 ) )   { neard++; }
    }

    const uint32_t us8  = nora_high_res_timer_count_to_us_x10( best8 );
    const uint32_t us16 = nora_high_res_timer_count_to_us_x10( best16 );
    const uint32_t usd  = nora_high_res_timer_count_to_us_x10( bestd );
    // The block fast path issues APP_BLOCK_FRAMES/2 pair16 calls, so 8*pair16 is the kernel-only
    // share of one pull -- compare it with the `pull=` peak to see the descriptor + servo overhead.
    printf( " *az kernel bench M=%u trials=%lu (min of):\n"
            "    pair8  (8ch x2out): %5lu ticks  %lu.%luus  near=%lu\n"
            "    pair16 (16chx2out): %5lu ticks  %lu.%luus  near=%lu\n"
            "    paird3 (16chx2out): %5lu ticks  %lu.%luus  near=%lu  U=%lu\n"
            "    => block16 estimate (%u x pair16): %lu.%luus\n",
            (unsigned)ASRC_POLY_M, (unsigned long)trials,
            (unsigned long)best8,  (unsigned long)( us8 / 10u ),  (unsigned long)( us8 % 10u ),
            (unsigned long)near8,
            (unsigned long)best16, (unsigned long)( us16 / 10u ), (unsigned long)( us16 % 10u ),
            (unsigned long)near16,
            (unsigned long)bestd,  (unsigned long)( usd / 10u ),  (unsigned long)( usd % 10u ),
            (unsigned long)neard,  (unsigned long)pd.utaps,
            (unsigned)( APP_BLOCK_FRAMES / 2u ),
            (unsigned long)( ( us16 * ( APP_BLOCK_FRAMES / 2u ) ) / 10u ),
            (unsigned long)( ( us16 * ( APP_BLOCK_FRAMES / 2u ) ) % 10u ) );
}
#endif
#endif

#if APP_ASRC_MEAS
// R8 Q2: dump the EXACT 32-bit float bit pattern of every polyphase coefficient (whatever backs
// ASRC_POLY_ROW -- RAM-generated or Flash-resident) + a CRC32 over those bytes. Host tools extract
// the flash table from this; run it again on a Flash build to self-check CRC_FLASH == CRC_RAM.
void audio_app_asrc_dump_poly_bits( void )
{
#if ASRC_SAMPLE_Q31
    // The Q31 build has no float polyphase table: ASRC_POLY_ROW is defined only in the float
    // arm, so there is nothing here to dump.  Say so rather than fail to link -- the Q31
    // generic resampler pins its own coefficients, and the 48 -> 32 audio-mode front end pins
    // the N=97 taps with _Static_assert + CRC32 in asrc_decimator_48_to_8.c, which is where
    // this build's coefficient evidence lives.
    printf( "\n*POLY_BITS_NA resampler=q31 (no float polyphase table in this build)\n" );
#else
    const uint32_t rows = ASRC_POLY_L + 1u;
    const uint32_t n    = rows * ASRC_POLY_M;
    printf( "\n*POLY_BITS_BEGIN L=%lu M=%lu n=%lu storage=%s\n",
            (unsigned long)ASRC_POLY_L, (unsigned long)ASRC_POLY_M, (unsigned long)n,
            (ASRC_COEFF_STORAGE == ASRC_COEFF_STORAGE_FLASH) ? "flash" : "ram" );
    uint32_t crc = 0xFFFFFFFFu;
    for( uint32_t p = 0u; p < rows; p++ )
    {
        const float* row = ASRC_POLY_ROW( p );
        for( uint32_t k = 0u; k < ASRC_POLY_M; k++ )
        {
            union { float f; uint32_t u; } bits;
            bits.f = row[k];
            printf( "%08lX\n", (unsigned long)bits.u );
            for( uint32_t b = 0u; b < 4u; b++ )      // CRC32 (poly 0xEDB88320) over little-endian bytes
            {
                crc ^= (uint32_t)( ( bits.u >> (8u * b) ) & 0xFFu );
                for( uint32_t i = 0u; i < 8u; i++ )
                {
                    crc = ( crc & 1u ) ? ( ( crc >> 1 ) ^ 0xEDB88320u ) : ( crc >> 1 );
                }
            }
        }
    }
    printf( "*POLY_BITS_END crc32=%08lX\n", (unsigned long)( crc ^ 0xFFFFFFFFu ) );
#endif
}
#endif

// Per-frame phase state: which sub-filters (c0,c1), the blend weight, and the window base index.
// These depend ONLY on rd/frac, so all channels share them -- computed ONCE per output frame
// (asrc_pll) instead of once per channel (the phase-share optimization).
typedef struct {
    const float* c0;    // sub-filter p
    const float* c1;    // sub-filter p+1
    float        wb;    // inter-phase blend weight
    uint32_t     wbase; // window start index (rd-MH), masked into the ring
} asrc_phase_t;

/* Float-only: these all dereference ASRC_POLY_ROW, which the Q31 build does not
 * define.  Excluding them here is what makes a stray float branch a compile
 * error instead of a silent second coefficient table. */
#if !ASRC_SAMPLE_Q31
static inline void asrc_poly_phase( uint32_t rd, float frac, asrc_phase_t* ph )
{
    const float pf = frac * (float)ASRC_POLY_L;      // 0 .. L
    // pf is non-negative and <=L; the headroom candidate's signed conversion
    // selects native f2li on XC-DSC. Keep the established preset as the control.
#if APP_ASRC_HEADROOM_INSTRUMENT
    uint32_t    p  = (uint32_t)(int32_t)pf;
#else
    uint32_t    p  = (uint32_t)pf;
#endif
    if( p >= ASRC_POLY_L ) { p = ASRC_POLY_L - 1u; } // guard the frac->1.0 edge
    ph->wb    = pf - (float)p;
    ph->c0    = ASRC_POLY_ROW( p );
    ph->c1    = ASRC_POLY_ROW( p + 1u );
    ph->wbase = ( rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
}

// Select the nearest stored phase without c0/c1 interpolation. This intentionally changes the
// coefficient trajectory and is used only to measure the stored-phase STREAM8 performance ceiling.
static inline void asrc_poly_phase_nearest( uint32_t rd, float frac,
                                            const float** coeff, uint32_t* wbase )
{
#if APP_ASRC_HEADROOM_INSTRUMENT
    uint32_t p = (uint32_t)(int32_t)( frac * (float)ASRC_POLY_L + 0.5f );
#else
    uint32_t p = (uint32_t)( frac * (float)ASRC_POLY_L + 0.5f );
#endif
    if( p > ASRC_POLY_L ) { p = ASRC_POLY_L; }
    *coeff = ASRC_POLY_ROW( p );
    *wbase = ( rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
}

// Interpolate one channel with a PRE-COMPUTED phase. The window is a CONTIGUOUS span (the ring
// mirrors its first M samples past the end -- see asrc_push), dotted against both sub-filters in
// one fused call (V1) and blended by the sub-phase weight.
static inline float asrc_poly_at( const float* buf, const asrc_phase_t* ph )
{
    float a0, a1;
    ASRC_DOT2( &buf[ph->wbase], ph->c0, ph->c1, ASRC_POLY_M, &a0, &a1 );   // V1 or V2a
    return a0 * ( 1.0f - ph->wb ) + a1 * ph->wb;
}

#if (ASRC_POLY_METHOD == ASRC_POLY_CEFF)
// Alternative: fold the two sub-filters into ONE phase-blended coefficient vector c_eff, once per
// frame (shared by all channels), so each channel is a single M-tap dot (half the per-channel MAC
// of the dual-dot, at the cost of a shared 32-lerp prep). Wins as channel count grows.
static inline void asrc_poly_phase_ceff( uint32_t rd, float frac, uint32_t* wbase, float* c_eff )
{
    const float pf = frac * (float)ASRC_POLY_L;
#if APP_ASRC_HEADROOM_INSTRUMENT
    uint32_t    p  = (uint32_t)(int32_t)pf;
#else
    uint32_t    p  = (uint32_t)pf;
#endif
    if( p >= ASRC_POLY_L ) { p = ASRC_POLY_L - 1u; }
    const float  wb  = pf - (float)p;
    const float  wb1 = 1.0f - wb;
    const float* c0  = ASRC_POLY_ROW( p );
    const float* c1  = ASRC_POLY_ROW( p + 1u );
    for( uint32_t k = 0u; k < ASRC_POLY_M; k++ ) { c_eff[k] = c0[k] * wb1 + c1[k] * wb; }
    *wbase = ( rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
}
#endif /* !ASRC_SAMPLE_Q31 */
#endif
#endif // APP_ASRC_INTERP == ASRC_INTERP_POLY

#if APP_ASRC_MEAS
// Q22: optional downstream first-order low-pass on the APPLIED step, to attenuate its 8-13 Hz
// content (the fill-servo motion that Q21 showed FM-modulates the carrier, coherence^2 ~0.9) while
// preserving the DC/slow rate-correction (fill authority). EMA corner ~= block_hz*beta/(2*pi) ~=
// 1356*0.02/6.283 ~= 4.3 Hz -- above the ~1 Hz fill correction, below the 8-13 Hz hunting. In-loop
// (the smoothed step drives both the read phase and the fill), so it is a bench A/B knob only.
// Runtime-toggled; default OFF -> byte-identical shipping (APP_ASRC_MEAS=0 compiles this out).
// Q23: beta is runtime-selectable from a small table so the corner can be swept in one build.
// corner ~= block_hz*beta/(2*pi): {0.005,0.01,0.02,0.04} -> ~{1.1,2.1,4.3,8.6} Hz.
static const float s_step_smooth_beta_tab[4] = { 0.005f, 0.01f, 0.02f, 0.04f };
static uint8_t s_step_smooth_en  = 0u;
static uint8_t s_step_smooth_idx = 2u;   // default index -> 0.02 (~4.3 Hz), the Q22 point
void  audio_app_asrc_step_smooth( uint8_t on, uint8_t beta_idx )
{
    s_step_smooth_idx = ( beta_idx < 4u ) ? beta_idx : 2u;
    s_step_smooth_en  = on ? 1u : 0u;
    // filter state is per-instance (asrc_t.step_smooth_st); it re-seeds lazily on the next pull.
}
uint8_t audio_app_asrc_get_step_smooth( void )      { return s_step_smooth_en; }
float   audio_app_asrc_get_step_smooth_beta( void ) { return s_step_smooth_beta_tab[s_step_smooth_idx]; }

// Q50 Fast-Acquisition Servo v1 (MEAS-only; shipping unchanged). State machine ACQUIRE->HANDOVER->TRACK.
// ACQUIRE: the ONLY change vs TRACK is the applied-step path -- step-smoothing is BYPASSED so the step
// tracks the servo immediately (KP/corr-LPF/clamp/slew are NOT changed). TRACK: the proven steady path
// (step-smoothing ~1.1 Hz on). HANDOVER (bumpless): on lock, seed the TRACK step-smoothing state with the
// current applied step so there is no step jump. Lock = fill moving-avg near target + slope~0 + no
// clamp/slew, held ~0.3 s (not a fixed timer). Enabled by *nt39 (candidate); off = current servo (baseline).
#define Q50_PULL_HZ            ( 390625.0f / (float)( APP_SPI2_MASTER_BRG + 1 ) / (float)APP_BLOCK_FRAMES )
#define Q50_LOCK_HOLD_PULLS    ( (uint32_t)( 0.30f * Q50_PULL_HZ ) )   /* ~0.3 s continuous */
#define Q50_FILL_TOL           ( 6.0f )     /* |fill_ma - target| lock window (frames) */
#define Q50_SLOPE_TOL          ( 1.5f )     /* |fast_ma - slow_ma| ~ 0 slope (frames) */
enum { Q50_ACQUIRE = 0u, Q50_HANDOVER = 1u, Q50_TRACK = 2u };
static uint8_t  s_q50_en       = APP_Q50_DEFAULT_ON;  // *nt39 / boot default: 1 = run ACQUIRE machine
static uint8_t  s_q50_state    = Q50_TRACK;
static float    s_q50_fill_fast = 0.0f;  // fill EMA ~0.1 s
static float    s_q50_fill_slow = 0.0f;  // fill EMA ~0.4 s (slope reference)
static float    s_q50_step_slow  = 0.0f; // applied-step slow  EMA (tau ~0.15 s)
static float    s_q50_step_vslow = 0.0f; // applied-step vslow EMA (tau ~1.05 s); slow-vslow = drift trend
static float    s_q50_strend_dbg = 0.0f; // last step-trend value (telemetry, to calibrate STEP_TOL)
static uint32_t s_q50_lockcnt  = 0u;   // consecutive in-range pulls
static uint32_t s_q50_pulls    = 0u;   // pulls since ACQUIRE start
static uint32_t s_q50_lock_pulls = 0u; // pulls-to-lock (0 = not locked yet)
static float    s_q50_ho_stepdiff = 0.0f;  // applied-step delta imposed at handover (should be ~0)
// Q51 Feed-Forward Rate Seed (layered on Q50 candidate)
#define Q51_FS_B_HZ  ( 390625.0f / (float)( APP_SPI2_MASTER_BRG + 1 ) )  /* known consumer rate (exact) */
static uint8_t  s_q51_seed_en   = APP_Q51_DEFAULT_ON;  // *nt3C: seed servo state from ACQUIRE rate estimate
static uint32_t s_q51_wr0       = 0u;   // producer pointer at ACQUIRE start
static uint32_t s_q51_t0        = 0u;   // high-res-timer count at ACQUIRE start
static float    s_q51_est_step  = 0.0f; // last rate estimate = fs_A_meas / fs_B (telemetry)
static float    s_q51_corr_seed = 0.0f; // corr_lpf seed applied at handover (telemetry)
static uint8_t  s_q51_applied   = 0u;   // 1 = last handover actually seeded (passed sanity), 0 = fell back
// Q52 Differential fill-drift rate seed (replaces the Q50/Q51 handover when enabled)
#define Q52_SKIP_PULLS ( 256u )    // (legacy Q52 drift telemetry) skip the post-recenter startup transient
#define Q52_WIN_PULLS  ( 950u )    // Q56: muted hold+recenter window (~0.70 s; burst is in first ~0.2 s)
static uint8_t  s_q52_seed_en   = APP_Q52_DEFAULT_ON;  // *nt3D: differential fill-drift seed
static float    s_q52_fill0     = 0.0f; // fill at window start
static float    s_q52_ratio0    = 0.0f; // frozen FF ratio held over the window
static float    s_q52_est_step  = 0.0f; // step_true = ratio0 + delta_fill/n_out (telemetry)
static float    s_q52_dfill      = 0.0f; // delta_fill over the window (telemetry)
void  audio_app_asrc_q50_enable( uint8_t on ) { s_q50_en = on ? 1u : 0u; }
uint8_t  audio_app_asrc_q50_get_en( void )    { return s_q50_en; }
uint8_t  audio_app_asrc_q50_state( void )     { return s_q50_state; }
uint32_t audio_app_asrc_q50_lock_pulls( void ){ return s_q50_lock_pulls; }
float    audio_app_asrc_q50_pull_hz( void )   { return Q50_PULL_HZ; }
float    audio_app_asrc_q50_ho_stepdiff( void ){ return s_q50_ho_stepdiff; }
float    audio_app_asrc_q50_strend( void )    { return s_q50_strend_dbg; }
void     audio_app_asrc_q51_enable( uint8_t on ){ s_q51_seed_en = on ? 1u : 0u; }
uint8_t  audio_app_asrc_q51_get_en( void )    { return s_q51_seed_en; }
float    audio_app_asrc_q51_est_step( void )  { return s_q51_est_step; }
float    audio_app_asrc_q51_corr_seed( void ) { return s_q51_corr_seed; }
uint8_t  audio_app_asrc_q51_applied( void )   { return s_q51_applied; }
void     audio_app_asrc_q52_enable( uint8_t on ){ s_q52_seed_en = on ? 1u : 0u; }
uint8_t  audio_app_asrc_q52_get_en( void )    { return s_q52_seed_en; }
uint32_t audio_app_asrc_get_wr( void )        { return s_asrc[ASRC_ENGINE_AB].wr; }
uint32_t audio_app_asrc_get_rd( void )        { return s_asrc[ASRC_ENGINE_AB].rd; }
static uint8_t s_q55_prime_en = 1u;   // Q55: hold silent-startup FIFO at TARGET (default on in MEAS)
#ifndef APP_Q57_LOCK_OFF
#define APP_Q57_LOCK_OFF (0)
#endif
static int16_t s_q57_lock_off = APP_Q57_LOCK_OFF;   // Q57: ratio-lock fill pre-bias (fill_start=TARGET-off)
void    audio_app_asrc_q57_lock_off( int16_t off ) { s_q57_lock_off = off; }
int16_t audio_app_asrc_q57_get_lock_off( void )    { return s_q57_lock_off; }
// Q58: block-phase-aware lock offset. At ratio-lock the intra-block phase (elapsed since last asrc_push
// x fs_A, 0..BLOCK) says where in the producer sawtooth the lock lands; off = center - phase re-centres
// the sawtooth to TARGET regardless of phase, removing the per-boot residual dips of the FIXED Q57 off.
#ifndef APP_Q58_DEFAULT_ON
#define APP_Q58_DEFAULT_ON (0)
#endif
#ifndef APP_Q58_CENTER
#define APP_Q58_CENTER (16)
#endif
static uint8_t s_q58_pa_en   = APP_Q58_DEFAULT_ON;   // *nt42: 1 = phase-aware offset (overrides Q57 fixed)
static float   s_q58_center  = (float)APP_Q58_CENTER; // sawtooth centre (frames); ~BLOCK/2, tunable
static float   s_q58_last_phase = 0.0f;  // telemetry: phase at the last lock
static int16_t s_q58_last_off   = 0;     // telemetry: dynamic offset applied at the last lock
void    audio_app_asrc_q58_cfg( uint8_t on, float center ) { s_q58_pa_en = on ? 1u : 0u; if( center > 0.0f ) s_q58_center = center; }
uint8_t audio_app_asrc_q58_get_en( void ) { return s_q58_pa_en; }
float   audio_app_asrc_q58_last_phase( void ) { return s_q58_last_phase; }
int16_t audio_app_asrc_q58_last_off( void )   { return s_q58_last_off; }
// Q55 early-boot logger: sample (fill, step, ratio) every LOG_DECIM pulls from ratio-lock, so the
// t<5 s disturbance (hidden behind the boot banner on the console) can be dumped and inspected later.
#define Q55_LOG_N      ( 64u )
#define Q55_LOG_DECIM  ( 8u )     // Q57: fine (8-pull ~5.9 ms) sampling -> 64 * 8 / 1356 ~= 0.38 s (burst)
static uint32_t s_q55log_wr[Q55_LOG_N];    // Q57: raw producer/consumer pointers -> attribute the burst
static uint32_t s_q55log_rd[Q55_LOG_N];
static float    s_q55log_step[Q55_LOG_N];
static uint16_t s_q55log_n   = 0u;
static uint32_t s_q55log_ctr = 0u;
static uint8_t  s_q55log_arm = 0u;   // set at ratio-lock; sampling runs while n<N
void audio_app_asrc_q55log_dump( void )
{
    const uint32_t wr0 = ( s_q55log_n > 0u ) ? s_q55log_wr[0] : 0u;
    const uint32_t rd0 = ( s_q55log_n > 0u ) ? s_q55log_rd[0] : 0u;
    for( uint16_t i = 0u; i < s_q55log_n; i++ )
        printf(" *MEAS q55log i=%u t=%.4f dwr=%ld drd=%ld fill=%ld step_ppm=%+.1f\n",
               (unsigned)i, (double)( (float)i * (float)Q55_LOG_DECIM / 1356.34f ),
               (long)( s_q55log_wr[i] - wr0 ), (long)( s_q55log_rd[i] - rd0 ),
               (long)( s_q55log_wr[i] - s_q55log_rd[i] ),
               (double)( ( s_q55log_step[i] / 1.107692306f - 1.0f ) * 1e6f ) );
}
void    audio_app_asrc_q55_prime_en( uint8_t on ) { s_q55_prime_en = on ? 1u : 0u; }
uint8_t audio_app_asrc_q55_get_prime_en( void )   { return s_q55_prime_en; }
// Q55 probe: elapsed us_x10 since the first call (latched), for host fs_A(t) = delta_wr / delta_t.
static uint32_t s_wrt_ref = 0u; static uint8_t s_wrt_ref_set = 0u;
uint32_t audio_app_asrc_wrt_elapsed_us10( void )
{
    const uint32_t now = nora_high_res_timer_get_count();
    if( !s_wrt_ref_set ) { s_wrt_ref = now; s_wrt_ref_set = 1u; return 0u; }
    return nora_high_res_timer_count_to_us_x10( now - s_wrt_ref );
}
float    audio_app_asrc_q52_est_step( void )  { return s_q52_est_step; }
float    audio_app_asrc_q52_dfill( void )     { return s_q52_dfill; }
float    audio_app_asrc_q52_ratio0( void )    { return s_q52_ratio0; }
float    audio_app_asrc_q52_fill0( void )     { return s_q52_fill0; }
// Called from asrc_apply_ratio on the invalid->valid transition: (re)arm the ACQUIRE detector. The
// detector ALWAYS runs in MEAS (it is read-only wrt the servo) so baseline (s_q50_en=0, no actuation)
// and candidate (s_q50_en=1) report the SAME lock-time metric; only the applied-step actuation differs.
static void q50_on_ratio_lock( void )
{
    s_q50_state = Q50_ACQUIRE;
    s_q50_fill_fast = 0.0f; s_q50_fill_slow = 0.0f;
    s_q50_step_slow = 0.0f; s_q50_step_vslow = 0.0f; s_q50_strend_dbg = 0.0f;
    s_q50_lockcnt = 0u; s_q50_pulls = 0u; s_q50_lock_pulls = 0u; s_q50_ho_stepdiff = 0.0f;
}

// Q26: single-point diagnostic injection (see the servo-error add site in asrc_pull). Zero-mean sine
// at a set frequency/amplitude; used only to probe the 8-13 Hz mechanism. Controller is unchanged.
static const float s_q26_amp_tab[4] = { 5.0e-6f, 1.0e-5f, 2.0e-5f, 5.0e-5f };
static uint8_t s_q26_en  = 0u;
static float   s_q26_amp = 0.0f;
static float   s_q26_w   = 0.0f;    // rad per pull
static float   s_q26_ph  = 0.0f;
float          g_q26_inj_val = 0.0f;   // current injection sample (0 when off); trace sel=9 records it
void audio_app_asrc_inject( uint8_t en, uint8_t freq_hz, uint8_t amp_idx )
{
    const float blk_hz = ( 390625.0f / (float)( APP_SPI2_MASTER_BRG + 1 ) ) / (float)APP_BLOCK_FRAMES;
    s_q26_amp = s_q26_amp_tab[ ( amp_idx < 4u ) ? amp_idx : 2u ];
    s_q26_w   = ( freq_hz > 0u ) ? ( 2.0f * 3.14159265358979f * (float)freq_hz / blk_hz ) : 0.0f;
    s_q26_en  = en ? 1u : 0u;
    if( !en ) { s_q26_ph = 0.0f; g_q26_inj_val = 0.0f; }
}
uint8_t audio_app_asrc_get_inject( void )      { return s_q26_en; }
float   audio_app_asrc_get_inject_amp( void )  { return s_q26_amp; }

// Q30 (bench sensitivity screen): runtime multipliers on the servo gain KP and the corr-LPF
// coefficient ALPHA, so the 8-13 Hz residual's dependence on each can be ranked / practically tuned
// WITHOUT rebuilding and WITHOUT changing the shipping constants. Scales ASRC_KP / ASRC_CORR_ALPHA
// only on the MEAS path (asrc_pull). No slew/clamp/structure change.
//   which=0 (KP):    code 0=x1, 1=x0.5, 2=x2.0                                    (Q30 gain screen)
//   which=1 (ALPHA): code is a corr-LPF TIME-CONSTANT multiplier tau -> ALPHA x (1/tau):
//                    0=tau x1 (ALPHA x1), 1=tau x1.5 (x0.667), 2=tau x2 (x0.5), 3=tau x3 (x0.333)
//                    (Q31 bandwidth tuning -- slower LPF rejects the ~10 Hz fill-wrap component).
static uint8_t s_q30_kp_code    = 0u;
static uint8_t s_q30_alpha_code = 0u;   // corr-LPF tau index (see above)
static float q30_kp_mult_of( uint8_t code )   // 1=x0.5; 2/3/4=x2/x4/x8 (Q45); 5/6/7=x16/x32/x64 (Q53 acquire)
{ return ( code == 1u ) ? 0.5f : ( code == 2u ) ? 2.0f : ( code == 3u ) ? 4.0f : ( code == 4u ) ? 8.0f
       : ( code == 5u ) ? 16.0f : ( code == 6u ) ? 32.0f : ( code == 7u ) ? 64.0f : 1.0f; }
static float q30_alpha_mult_of( uint8_t code )   // ALPHA multiplier (base ASRC_CORR_ALPHA = tau x3)
{                                                // 0..3 = slower (x1..x0.333); 4..6 = FASTER (x3/x6/x12
    switch( code ) {                             // = tau x1 / x0.5 / x0.25) -- Q45 fast-acquire test.
    case 1u: return 0.6666667f;   case 2u: return 0.5f;    case 3u: return 0.3333333f;
    case 4u: return 3.0f;         case 5u: return 6.0f;    case 6u: return 12.0f;
    default: return 1.0f;
    }
}
void audio_app_asrc_set_servo_mult( uint8_t which, uint8_t code )
{
    if( which == 0u )      { s_q30_kp_code    = ( code < 5u ) ? code : 0u; }
    else if( which == 1u ) { s_q30_alpha_code = ( code < 7u ) ? code : 0u; }
}
float audio_app_asrc_get_kp_mult( void )    { return q30_kp_mult_of( s_q30_kp_code ); }
float audio_app_asrc_get_alpha_mult( void ) { return q30_alpha_mult_of( s_q30_alpha_code ); }
#endif

// Consumer side: produce one block by resampling the ring; stereo to slots 0/1, rest 0.
// On underrun emit silence and hold the read phase until the FIFO refills.
static void asrc_pull( asrc_t* a, int32_t* dst )
{
    const uint32_t t0_cnt = nora_high_res_timer_get_count();   // telemetry: time this pull

#if APP_ASRC_MEAS
    // R12 Q12: intentional FF-ratio freeze -- controller uses the latched ratio, but a->ratio keeps
    // updating (live CCP) and corr_lpf/clamp/slew/step_state below stay fully live.
    const float ratio = ( a->ff_freeze ) ? a->ff_frozen_ratio : a->ratio;
    // R16 Q16: monotonic pull counter + one-time freeze-epoch latch (t=0 = first pull on frozen FF).
    s_q16_pull_ctr++;
    if( a->ff_freeze && !s_q16_freeze_latched ) { s_q16_freeze_epoch = s_q16_pull_ctr; s_q16_freeze_latched = 1u; }
#else
    const float ratio = a->ratio;   // live feed-forward ratio (0 until first measurement)
#endif
    const uint32_t wr_now = a->wr;                 // one read; used for both fill and wr_advance

    /*
     * OVERFLOW GUARD (consumer side).  The producer writes the ring unconditionally and
     * publishes wr; it does not touch rd (see the rd declaration).  So if the producer has run
     * ahead by more than the ring physically holds, the oldest frames under rd are already
     * overwritten and rd has to be moved up to the oldest frame still present before anything
     * reads through it.  Doing it here, once per pull, is what makes rd single-writer.
     *
     * Jumping straight to the boundary also RECOVERS from the already-overfull case, which the
     * old producer-side guard did not: that one advanced rd in lockstep with wr (and clamped
     * the block form to one block), so a ring that came out of startup at guard+8 stayed at
     * guard+8 forever.  Here fill after the correction is exactly min(fill, FIFO-4).
     *
     * Attribution follows the producer that filled the ring, so the two existing counters keep
     * their meanings (dbg_intermediate_overflow stays gated on a valid ratio, as before).
     */
    {
        const uint32_t fill_raw = wr_now - a->rd;
        if( fill_raw > ( ASRC_FIFO_FRAMES - 4u ) )
        {
            const uint32_t excess = fill_raw - ( ASRC_FIFO_FRAMES - 4u );
            a->rd += excess;
            if( a->prod_frames )
            {
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
                if( a->ratio > 0.0f ) { a->dbg_intermediate_overflow += excess; }
#endif
            }
            else
            {
                /* Gated on a valid ratio exactly like the front-end counter above. Without the
                 * gate this charged the whole pre-lock window (the consumer holds rd while
                 * ratio<=0, so the ring saturates and this guard fires on every pull), which is
                 * muted output being re-centred, not a dropout. */
                if( a->ratio > 0.0f ) { a->dbg_guard_drops += excess; }
            }
        }
    }

    const float fill  = (float)( wr_now - a->rd );
    a->dbg_fill = (uint32_t)fill;   // telemetry snapshot
    if( (uint16_t)fill < a->dbg_fill_min ) { a->dbg_fill_min = (uint16_t)fill; }   // min-hold until the next print
#if APP_ASRC_MEAS
    // Q35: producer advance over the SAME inter-pull interval as dbg_wraps (consumer advance). Together
    // they satisfy the identity  fill[n]-fill[n-1] = wr_adv[n] - wraps[n]  (rd only advances in the
    // pull wrap-loop in steady state; the consumer-side overflow guard never fires while fill<<FIFO-4).
    // Updated every pull (incl. silent startup) so the delta stays a true consecutive-pull difference.
    a->dbg_wr_adv  = (uint16_t)( wr_now - a->dbg_wr_prev );
    a->dbg_wr_prev = wr_now;
#endif

    // No valid ratio yet (startup, before the first measured rate): emit a silent block and
    // hold the read phase -- do NOT resample against a guessed rate. The FIFO keeps filling;
    // resampling begins the moment a feed-forward ratio is set.
    if( ratio <= 0.0f )
    {
        a->dbg_step = 0.0f;
        int32_t* z = dst;
        for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
        {
            for( uint8_t s = 0u; s < APP_SLOTS_PER_FS; s++ ) { z[s] = 0; }
            z += APP_SLOTS_PER_FS;
        }
#if APP_ASRC_MEAS
        // Q55: during silent startup, hold the FIFO at TARGET depth (drop oldest beyond TARGET) instead
        // of letting it saturate to FIFO-4 and then snapping rd=wr-TARGET at ratio-lock. This starts
        // resampling from a steady, phase-consistent 256-deep ring -> tests whether the ~10 s warm-up is
        // the post-lock settle of the saturate-then-snap startup discontinuity. *nt3F toggles it.
        if( s_q55_prime_en && ( a->wr - a->rd ) > (uint32_t)a->fill_target )
        { a->rd = a->wr - (uint32_t)a->fill_target; }
#endif
        const uint32_t elapsed_ticks = nora_high_res_timer_get_count() - t0_cnt;
        if( elapsed_ticks > a->dbg_pull_ticks_max ) { a->dbg_pull_ticks_max = elapsed_ticks; }
        return;
    }

#if APP_ASRC_MEAS
    // R14 Q14: update the N=64 raw-fill moving average EVERY active pull (warm in all states); the
    // selector chooses whether the servo below observes the averaged or the raw fill. Seed the
    // history to the first valid fill (no false startup ramp). Observation path only -- the servo
    // equations after fill_used are unchanged.
    const uint8_t fill_u8 = (uint8_t)fill;   // fill is integer frames, capped <FIFO-4 (<=252)
    if( !s_q14_ma_ready )
    {
        for( uint8_t i = 0u; i < 64u; i++ ) { s_q14_fill_hist[i] = fill_u8; }
        s_q14_fill_sum = 64.0f * (float)fill_u8;
        s_q14_fill_pos = 0u;
        s_q14_ma_ready = 1u;
    }
    else
    {
        s_q14_fill_sum   += (float)fill_u8 - (float)s_q14_fill_hist[s_q14_fill_pos];
        s_q14_fill_hist[s_q14_fill_pos] = fill_u8;
        s_q14_fill_pos    = (uint8_t)( ( s_q14_fill_pos + 1u ) & 63u );
    }
    const float fill_ma   = s_q14_fill_sum * ( 1.0f / 64.0f );
    float fill_used;
    if( s_cfill_en )
    {
    // Q40: continuous fill = integer_fill + phase_frames - BLOCK/2, where phase_frames is the elapsed
        // time since the last producer RX block scaled by the configured fs_A (frames delivered so far
        // this producer-block-interval), clamped 0..APP_BLOCK_FRAMES. This cancels the producer
        // +APP_BLOCK_FRAMES
        // jump against the phase reset, de-staircasing the servo's fill observation WITHOUT changing
        // the mean (average phase over an interval = BLOCK/2). Uses the existing high-res timer
        // and APP_MEAS_FS_A_HZ -- no new tuning constant. Servo-observation only; wr/rd untouched.
        const uint32_t el10 = nora_high_res_timer_elapsed_us_x10( a->dbg_rx_tick );  // us x10
        // Q42: phase slope from the MEASURED producer period (once ready), else Q40/Q41 fixed fs_A slope.
        float phase;
        if( s_hf_en && s_pp_ready )
        {
            phase = (float)APP_BLOCK_FRAMES * (float)el10 / s_pp_period;   // BLOCK * elapsed / period
        }
        else
        {
            phase = (float)el10 * ( APP_MEAS_FS_A_HZ * 1.0e-7f );          // us_x10 -> s -> frames
        }
        if( phase < 0.0f )                        { phase = 0.0f; }
        else if( phase > (float)APP_BLOCK_FRAMES ) { phase = (float)APP_BLOCK_FRAMES; }
        // Q41: choose the centering. Fixed BLOCK/2 (Q40), or the auto-measured mean phase once the
        // calibration window has closed. Q42: when high-fidelity, defer the center calibration until the
        // producer period is fixed (s_pp_ready) so it averages the final slope (stage 2 after stage 1).
        float center = (float)APP_BLOCK_FRAMES * 0.5f;
        if( s_pc_auto )
        {
            if( !s_pc_ready )
            {
                if( !s_hf_en || s_pp_ready )   // Q42: hold center-cal until producer period is fixed
                {
                    s_pc_sum += phase;
                    s_pc_cnt++;
                    if( s_pc_cnt >= Q41_CAL_N ) { s_pc_center = s_pc_sum / (float)s_pc_cnt; s_pc_ready = 1u; }
                }
            }
            else
            {
                center = s_pc_center;
            }
        }
        fill_used = fill + phase - center;
    }
    else
    {
        fill_used = ( s_q14_use_ma ) ? fill_ma : fill;
    }
#else
    const float fill_used = fill;
#endif

    // Per-block step = feed-forward ratio, fill error trims it (proportional; holds ~target).
    float step;
#if APP_ASRC_MEAS
    // Q29 Q10: servo internal-state snapshot for trace sel=10 (fill/raw_corr/clamp/slew). Stays 0 on
    // the freeze_step (Mode-K) path below since that path bypasses the fill servo entirely.
    float   q29_raw_corr  = 0.0f;
    uint8_t q29_clamp_hit = 0u;
    uint8_t q29_slew_hit  = 0u;
    if( a->freeze_step > 0.0f )
    {
        step = a->freeze_step;   // bench: CONSTANT step -- no fill trim, no feed-forward jitter
    }
    else
#endif
    {
        if( a->step_state <= 0.0f ) { a->step_state = ratio; }   // defensive seed

        // Gentle fill correction -> 1st-order LPF -> slew-limited applied step. This keeps the
        // loop bandwidth ~1 Hz so it tracks ppm clock drift without FM-ing the audio.
#if APP_ASRC_MEAS
        float raw_corr = ( ASRC_KP * q30_kp_mult_of(s_q30_kp_code) ) * ( fill_used - a->fill_target );   // Q30 KP screen
        // Q26: ADD a zero-mean diagnostic sine to the servo ERROR (before the corr LPF) to probe the
        // 8-13 Hz generation mechanism (linear resonance vs nonlinear limit cycle). Phase advances per
        // pull; g_q26_inj_val is logged synchronously by trace sel=9. The controller
        // (KP/ALPHA/SLEW/clamp/deadband) is UNCHANGED -- this only adds a zero-mean perturbation. OFF by default.
        if( s_q26_en )
        {
            g_q26_inj_val = s_q26_amp * sinf( s_q26_ph );
            s_q26_ph += s_q26_w;
            if( s_q26_ph > 6.28318530717959f ) { s_q26_ph -= 6.28318530717959f; }
            raw_corr += g_q26_inj_val;
        }
        q29_raw_corr = raw_corr;   // Q29 Q10: snapshot for the servo-state trace (sel=10)
#else
        const float raw_corr = ASRC_KP * ( fill_used - a->fill_target );
#endif
#if APP_ASRC_MEAS
        // R13 Q13: when corr_hold, keep corr_lpf constant (fill/raw_corr still computed above; only
        // the servo integration is skipped). step_state/clamp/slew below stay live -- NOT Mode-K.
        // Q30: the corr-LPF coefficient is scaled by s_q30_alpha_mult here (bench sensitivity screen).
        if( !a->corr_hold )
        {
            a->corr_lpf += ( ASRC_CORR_ALPHA * q30_alpha_mult_of(s_q30_alpha_code) ) * ( raw_corr - a->corr_lpf );
        }
#else
        a->corr_lpf += ASRC_CORR_ALPHA * ( raw_corr - a->corr_lpf );
#endif

#if APP_ASRC_MEAS
        // R15 Q15: complementary 1 Hz split of the LIVE corr_lpf + motion-only band selector.
        // beta = 1 - exp(-2*pi*1.0Hz / pull_rate); pull_rate = FsB/32 = 390625/(BRG+1)/32 Hz.
        // ONLY corr_used (below) is selected; corr_lpf itself always updates.
        if( !s_q15_split_ready ) { g_q15_corr_slow = a->corr_lpf; s_q15_split_ready = 1u; }
        else { g_q15_corr_slow += Q15_CORR_BETA * ( a->corr_lpf - g_q15_corr_slow ); }
        const float corr_fast = a->corr_lpf - g_q15_corr_slow;
        if( s_q15_capture_pending )
        {
            g_q15_hold_val = ( g_q15_corr_mode == 1u ) ? corr_fast        // SLOW: hold fast component
                           : ( g_q15_corr_mode == 2u ) ? g_q15_corr_slow  // FAST: hold slow component
                                                       : a->corr_lpf;     // HOLD: hold full corr_used
            s_q15_capture_pending = 0u;
        }
        float corr_used;
        switch( g_q15_corr_mode )
        {
            case 1u:  corr_used = g_q15_corr_slow + g_q15_hold_val; break;  // SLOW motion only
            case 2u:  corr_used = g_q15_hold_val + corr_fast;       break;  // FAST motion only
            case 3u:  corr_used = g_q15_hold_val;                   break;  // all corr motion held
            default:  corr_used = a->corr_lpf;                      break;  // FULL
        }
#else
        const float corr_used = a->corr_lpf;
#endif
        float target_step = ratio * ( 1.0f + corr_used );
        const float lo = ratio * ASRC_STEP_LO;                   // safety envelope
        const float hi = ratio * ASRC_STEP_HI;
#if APP_ASRC_MEAS
        // Q29 Q10: clamp-hit iff the PRE-clamp target actually fell outside the envelope (read-only;
        // the clamp itself below is unchanged).
        q29_clamp_hit = ( target_step < lo || target_step > hi ) ? 1u : 0u;
#endif
        if( target_step < lo ) { target_step = lo; }
        if( target_step > hi ) { target_step = hi; }

        float delta = target_step - a->step_state;               // slew-limit the change/block
#if APP_ASRC_MEAS
        // Q29 Q10: slew-hit iff the PRE-slew delta actually exceeded the limit (read-only).
        q29_slew_hit = ( delta > ASRC_STEP_SLEW || delta < -ASRC_STEP_SLEW ) ? 1u : 0u;
#endif
        if( delta >  ASRC_STEP_SLEW ) { delta =  ASRC_STEP_SLEW; }
        if( delta < -ASRC_STEP_SLEW ) { delta = -ASRC_STEP_SLEW; }
        a->step_state += delta;

        step = a->step_state;
    }

#if APP_ASRC_MEAS
    // Q50 Fast-Acquisition state machine (ACQUIRE lock-detect + bumpless HANDOVER). Read-only wrt the
    // servo (KP/corr-LPF/clamp/slew already ran above); it only decides the applied-step path below.
    // Runs for both baseline and candidate so the lock-time metric is identical; actuation is gated by
    // s_q50_en at the step-smoothing/mute sites below.
    if( s_q50_state == Q50_ACQUIRE )
    {
        if( s_q50_pulls == 0u )   // seed EMAs to the first sample (no warm-up ramp from zero)
        { s_q50_fill_fast = fill; s_q50_fill_slow = fill;
          s_q50_step_slow = step; s_q50_step_vslow = step;
          s_q51_wr0 = wr_now;     // Q51: latch producer pointer + timer at ACQUIRE start (rate-estimate base)
          s_q51_t0  = nora_high_res_timer_get_count();
          if( s_q50_en && s_q52_seed_en )
          {
              /* Q56 (Q52 path repurposed): HOLD + CONTINUOUS RE-CENTRE. The early-boot log pinned the
               * cause: a one-time ~12-frame producer startup burst in the first ~0.2 s offsets the fill;
               * the servo (step from a correct ratio0) then chases that offset to +72 ppm and rings it
               * out over ~8 s. Fix: FREEZE the servo (ff_freeze+corr_hold -> step == ratio0, never winds
               * up) AND re-centre the FIFO every muted pull so the burst is absorbed as it arrives. After
               * a short window the burst is gone; release with step already at the true ratio + fill
               * centred -> no wind-up to ring out. */
              a->ff_freeze = 1u; a->ff_frozen_ratio = ratio; a->corr_hold = 1u;
              a->corr_lpf = 0.0f; a->step_state = ratio;
              s_q52_ratio0 = ratio; s_q52_fill0 = fill;
          } }
        s_q50_pulls++;
        /* Q52: (re)latch fill0 AFTER the post-recenter startup transient has settled, so the measured
         * drift is clean (the first ~0.2 s shows a ~15-frame producer burst that would bias the estimate). */
        if( s_q50_en && s_q52_seed_en && s_q50_pulls == Q52_SKIP_PULLS ) { s_q52_fill0 = fill; }
        s_q50_fill_fast += 0.02f  * ( fill - s_q50_fill_fast );   /* ~0.1 s EMA */
        s_q50_fill_slow += 0.005f * ( fill - s_q50_fill_slow );   /* ~0.4 s EMA (slope ref) */
        /* Two smoothed step EMAs; their DIFFERENCE (trend) filters out the ~20.87 Hz producer limit
         * cycle that contaminates the raw step, so it reflects the slow warm-up drift, not the jitter.
         * step_slow leads step_vslow while the step is still drifting; both equal once converged. */
        s_q50_step_slow  += 0.005f  * ( step - s_q50_step_slow );   /* tau ~0.15 s */
        s_q50_step_vslow += 0.0007f * ( step - s_q50_step_vslow );  /* tau ~1.05 s */
        const float dev    = s_q50_fill_fast - a->fill_target;
        const float slope  = s_q50_fill_fast - s_q50_fill_slow;
        /* strend (step slow-vslow trend) is TELEMETRY only: it floors at ~1.1e-5 (the raw step's
         * ~20.87 Hz limit cycle beating between the two EMA bandwidths), never reaching a tight band,
         * so it cannot gate a hard lock. The HANDOVER trigger is fill-centering (audio-safe to unmute);
         * the fine step/THD convergence (~13 s, KP/corr-LPF limited, forbidden to change) is measured
         * host-side via THD-vs-time. strend is logged so that fine convergence is still observable. */
        s_q50_strend_dbg = s_q50_step_slow - s_q50_step_vslow;
        const uint8_t clean = ( q29_clamp_hit == 0u ) && ( q29_slew_hit == 0u );
        if( ( dev > -Q50_FILL_TOL && dev < Q50_FILL_TOL )
            && ( slope > -Q50_SLOPE_TOL && slope < Q50_SLOPE_TOL ) && clean )
        { s_q50_lockcnt++; } else { s_q50_lockcnt = 0u; }

        if( s_q50_en && s_q52_seed_en )
        {
            /* Q56: while held, re-centre the FIFO every muted pull -- rd tracks wr so the startup burst
             * is dropped as it arrives and fill cannot wind the (held) step. */
            a->rd = a->wr - (uint32_t)a->fill_target;
            a->frac = 0.0f;
            s_q52_dfill    = fill - s_q52_fill0;   /* telemetry */
            s_q52_est_step = ratio;
            if( s_q50_pulls >= Q52_WIN_PULLS )
            {
                /* release: step==ratio0 (true), fill centred, burst absorbed -> no wind-up to settle */
                a->corr_hold = 0u; a->ff_freeze = 0u;
                a->step_state     = s_q52_ratio0;
                s_q50_ho_stepdiff = ( a->step_smooth_st > 0.0f ) ? ( step - a->step_smooth_st ) : 0.0f;
                a->step_smooth_st = s_q52_ratio0;   /* bumpless: TRACK smoothing starts at the true step */
                s_q50_lock_pulls  = s_q50_pulls;
                s_q50_state       = Q50_TRACK;
            }
        }
        else if( s_q50_lockcnt >= Q50_LOCK_HOLD_PULLS )
        {
            /* HANDOVER (bumpless): seed the TRACK step-smoothing state with the current applied step so
             * TRACK's first smoothed output equals `step` (no jump, no corr re-convergence, no fill kick).
             * The seed WRITE is candidate-only (s_q50_en): in baseline, smoothing ran through ACQUIRE, so
             * step_smooth_st is already valid and must stay untouched (pure shipping path). The diff is
             * recorded for both to report the seam size. */
            s_q50_ho_stepdiff = ( a->step_smooth_st > 0.0f ) ? ( step - a->step_smooth_st ) : 0.0f;
            /* Q51 Feed-Forward Rate Seed: estimate the true rate from the ACQUIRE-window frame counts
             * (producer frames delta_wr over consumer output frames n_out). wr is frame-granular, so at
             * ~1.9 s / ~91k frames the quantization is ~11 ppm -- well under the CCP feed-forward's
             * ~130 ppm residual. Seed step_state + smoothing + corr_lpf at that estimate so TRACK starts
             * at the true step instead of integrating the 130 ppm bias over ~10 s. The jump happens on
             * this (still-muted) pull, so it is inaudible. corr_lpf = est/ratio - 1 makes the TRACK
             * target_step = ratio*(1+corr_lpf) = est exactly (no slew, servo already at rest). */
            s_q51_applied = 0u;
            if( s_q50_en && s_q51_seed_en )
            {
                /* fs_A_meas = delta_wr / delta_t (wr frame-granular, delta_t from the high-res timer,
                 * both sampled at pull start -> clean window, no pull-count quantization). est_step =
                 * fs_A_meas / fs_B_known. */
                const uint32_t dt10 = nora_high_res_timer_elapsed_us_x10( s_q51_t0 );  // us x10
                if( dt10 > 0u )
                {
                    const float dt_s = (float)dt10 * 1.0e-7f;
                    const float est  = (float)( wr_now - s_q51_wr0 ) / ( dt_s * Q51_FS_B_HZ );
                    s_q51_est_step = est;
                    /* Sanity: only seed if the estimate is within +-0.5 % of the feed-forward ratio;
                     * otherwise a startup-transient/quantization outlier would make TRACK worse, so fall
                     * back to the plain Q50 bumpless seed. */
                    if( ratio > 0.0f && est > ratio * 0.995f && est < ratio * 1.005f )
                    {
                        s_q51_corr_seed   = est / ratio - 1.0f;
                        a->step_state     = est;
                        a->step_smooth_st = est;
                        a->corr_lpf       = s_q51_corr_seed;
                        s_q51_applied     = 1u;
                    }
                }
            }
            if( !s_q51_applied && s_q50_en ) { a->step_smooth_st = step; }  /* Q50 bumpless seed */
            s_q50_lock_pulls  = s_q50_pulls;
            s_q50_state       = Q50_TRACK;
        }
    }
#endif

    // Q44 (shipping default): low-pass the applied step to strip the producer limit cycle (>1 Hz)
    // before it drives the read phase (and thus the carrier FM). DC/slow rate correction passes ->
    // fill authority kept. On the freeze/Mode-K bench path above this leaves the frozen step untouched
    // only when the MEAS toggle disables it (bench default OFF); the shipping build always smooths.
    {
        float   ss_beta = ASRC_STEP_SMOOTH_BETA;
        uint8_t ss_en   = 1u;
#if APP_ASRC_MEAS
        // Bench (Q22/Q23): the *nt32 knob can turn smoothing OFF for A/B and sweep the corner.
        ss_en   = s_step_smooth_en;
        ss_beta = s_step_smooth_beta_tab[s_step_smooth_idx];
        // Q50 ACQUIRE: bypass step-smoothing so the applied step tracks the servo with no LPF lag.
        // Q53 exception: KEEP smoothing ON during the boosted acquire so the applied step (and the
        // handover seed captured from it) stays clean despite the high KP -- otherwise the amplified
        // 20.87 Hz limit cycle poisons the handover value.
        if( s_q50_en && s_q50_state == Q50_ACQUIRE && !s_q52_seed_en ) { ss_en = 0u; }
#endif
        if( ss_en )
        {
            if( a->step_smooth_st <= 0.0f ) { a->step_smooth_st = step; }   // seed on first use
            a->step_smooth_st += ss_beta * ( step - a->step_smooth_st );
            step = a->step_smooth_st;
        }
    }

    a->dbg_step = step;

#if APP_ASRC_MEAS
    // Q55 early-boot logger: sample fill/step/ratio every LOG_DECIM pulls from ratio-lock.
    if( s_q55log_arm && s_q55log_n < Q55_LOG_N )
    {
        if( ( s_q55log_ctr % Q55_LOG_DECIM ) == 0u )
        {
            s_q55log_wr[s_q55log_n]   = wr_now;   // producer pointer at this pull's fill sample
            s_q55log_rd[s_q55log_n]   = a->rd;    // consumer pointer (post-servo, pre-resample-advance)
            s_q55log_step[s_q55log_n] = step;
            s_q55log_n++;
        }
        s_q55log_ctr++;
    }
#endif

#if APP_ASRC_MEAS
    // R10 Q10: sample the control state into the trace buffer (measurement-only; reads state, does
    // not change the loop). A MEAS build forces one-way A->B, so asrc_pull only ever runs for A->B.
    // Q34: a->frac is the consumer fractional read phase ENTERING this block (the per-sample loop
    // below advances it and wraps rd); sampled here it is the block-k fractional-wrap state, on the
    // same block index as fill/corr_lpf/step. Read-only.
    // a->dbg_wraps carries the PREVIOUS block's consumer-wrap count (rd advances) -- a properly
    // block-rate-sampled observable of the step->wrap-rate->rd->fill loop half (unlike frac, which
    // advances ~32x/block and is aliased at block rate). One-block lag vs fill/step is constant.
    audio_app_meas_trace_tick( step, a->corr_lpf, (uint32_t)fill, a->ratio, fill_ma,
                               q29_raw_corr, (uint8_t)( q29_clamp_hit | ( q29_slew_hit << 1u ) ),
                               a->frac, a->dbg_wraps, a->dbg_wr_adv );
#endif

#if APP_ASRC_MEAS
    const uint32_t rd_block0 = a->rd;   // Q34: count consumer wraps produced by THIS block (read-only)
#endif
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    uint8_t underrun_this_pull = 0u;
#endif
    int32_t* d = dst;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16 && \
    (APP_BLOCK_FRAMES == 16u) && !APP_ASRC_MEAS && ASRC_PAIR_SLOT_DIRECT_OK
    // Steady-state whole-block fast path. Requires ASRC_PAIR_SLOT_DIRECT_OK (8 physical
    // slots): unlike the per-frame pair path, this kernel writes the ENTIRE block of
    // output frames into the DMA buffer itself, so its frame stride is baked in and a
    // local-buffer copy is not applicable.
    // Precompute the eight pair descriptors without touching
    // live state, then let assembly share one register frame across all 16 output frames. If any
    // pair has an unusual 0/2-frame advance or insufficient look-ahead, the ordinary loop below
    // handles the complete block from the unchanged a->rd/a->frac state.
    {
        mchp_stream16_pair_desc_t desc[APP_BLOCK_FRAMES / 2u];
        uint32_t rd_batch = a->rd;
        float    frac_batch = a->frac;
        uint8_t  batch_ok = 1u;
        for( uint8_t p = 0u; p < ( APP_BLOCK_FRAMES / 2u ); p++ )
        {
            const uint32_t rd0 = rd_batch;
            const float frac0 = frac_batch;
            uint32_t rd1 = rd0;
            float frac1 = frac0 + step;
            while( frac1 >= 1.0f ) { frac1 -= 1.0f; rd1++; }
            if( rd1 != ( rd0 + 1u ) ||
                ( a->wr - rd1 ) < ( ASRC_POLY_AHEAD + 1u ) )
            {
                batch_ok = 0u;
                break;
            }

            asrc_phase_t ph0, ph1;
            asrc_poly_phase( rd0, frac0, &ph0 );
            asrc_poly_phase( rd1, frac1, &ph1 );
            desc[p].wbase0 = &a->ch[0][ph0.wbase];
            desc[p].c00 = ph0.c0;
            desc[p].c01 = ph0.c1;
            desc[p].c10 = ph1.c0;
            desc[p].c11 = ph1.c1;
            desc[p].wb0 = ph0.wb;
            desc[p].wb1 = ph1.wb;

            frac_batch = frac1 + step;
            rd_batch = rd1;
            while( frac_batch >= 1.0f ) { frac_batch -= 1.0f; rd_batch++; }
        }

        if( batch_ok )
        {
            int32_t hidden_slots[16];
            ASRC_STREAM16_BLOCK_SLOT(
                desc, APP_BLOCK_FRAMES / 2u,
                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ), dst, hidden_slots );
            a->rd = rd_batch;
            a->frac = frac_batch;
            s_hidden_output_sink =
                (uint32_t)hidden_slots[0] ^ (uint32_t)hidden_slots[15];
            goto asrc_output_done;
        }
    }
#endif
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && ASRC_PAIR_SLOT_FASTPATH_OK
#if (ASRC_CH & 7u)
#error "ASRC_POLY_STREAM8_PAIR requires ASRC_CH multiple of 8"
#endif
        // Fast path for two adjacent outputs. The second output's history window starts d =
        // rd1-rd0 frames after the first: d == 1 for virtually every sample at step ~= 1, d == 3
        // for a 48k->16k pull, d in {0,1} for any up-conversion. The 16-channel build handles
        // every d up to ASRC_PAIRD_DMAX with the union-window kernel; other builds keep the
        // d == 1 pair kernel and let the rest fall through to the one-output STREAM8 path below
        // (which does not change phase evolution either way).
        if( ( n + 1u ) < APP_BLOCK_FRAMES )
        {
            const uint32_t rd0   = a->rd;
            const float    frac0 = a->frac;
            uint32_t       rd1   = rd0;
            float          frac1 = frac0 + step;
            while( frac1 >= 1.0f ) { frac1 -= 1.0f; rd1++; }
            const uint32_t dstep = rd1 - rd0;
            const uint32_t utaps = ( ( ASRC_POLY_M + dstep ) | 1u );   // == asrc_paird_fill()

            asrc_phase_t ph0, ph1;
            asrc_poly_phase( rd0, frac0, &ph0 );
            asrc_poly_phase( rd1, frac1, &ph1 );

            // Every kernel here reads ONE contiguous window wbase..wbase+utaps-1 out of the
            // mirrored ring (asrc_push duplicates the first ASRC_POLY_M frames at +FIFO_FRAMES),
            // so the union window must stay inside the physical row: past ASRC_FIFO_PHYS the
            // mirror stops and the read would leave the channel. wbase(rd1) == wbase(rd0)+d
            // follows from wbase = (rd-MH) & MASK, so no separate wrap test is needed. At d == 1
            // (utaps = M+1 = 31) the test is always true, which is why the ratio-1 acceptance
            // rate is unchanged.
            if( ASRC_PAIRD_DSTEP_OK( dstep ) &&
                ( ph0.wbase + utaps ) <= ASRC_FIFO_PHYS &&
                ( a->wr - rd1 ) >= ( ASRC_POLY_AHEAD + 1u ) )
            {
#if APP_ASRC_MEAS
                const uint8_t pair_mute =
                    ( s_q50_en && s_q50_state == Q50_ACQUIRE ) ? 1u : 0u;
#endif
#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
                int32_t hidden_slots[16];
                if( dstep == 1u )
                {
                    ASRC_STREAM16_PAIR_SLOT(
                        &a->ch[0][ph0.wbase],
                        (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                        ph0.c0, ph0.c1, &d[0], hidden_slots,
                        ph1.c0, ph1.c1, ph0.wb, ph1.wb );
                }
                else
                {
                    mchp_stream16_paird_desc_t pd;
                    pd.wbase0 = &a->ch[0][ph0.wbase];
                    pd.c00    = ph0.c0;
                    pd.c01    = ph0.c1;
                    pd.c10    = ph1.c0;
                    pd.c11    = ph1.c1;
                    pd.wb0    = ph0.wb;
                    pd.wb1    = ph1.wb;
                    asrc_paird_fill( &pd, dstep );
                    mchp_stream16_paird_f32(
                        &pd, (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                        &d[0], hidden_slots );
                }
#if APP_ASRC_MEAS
                if( pair_mute )
                {
                    for( uint8_t l = 0u; l < 16u; l++ ) { d[l] = 0; }
                }
#endif
                s_hidden_output_sink =
                    (uint32_t)hidden_slots[0] ^ (uint32_t)hidden_slots[15];
#else
#if (ASRC_CH > APP_SLOTS_PER_FS)
                uint32_t hidden_sink = 0u;
#endif
                for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
                {
                    if( c < APP_SLOTS_PER_FS )
                    {
#if ASRC_PAIR_SLOT_DIRECT_OK
                        // 8 physical slots: out16[0..7]/[8..15] coincide with
                        // frame0/frame1 of the DMA block, so store in place.
                        mchp_stream8_pair_slot_f32(
                            &a->ch[c][ph0.wbase],
                            (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                            ph0.c0, ph0.c1, ASRC_POLY_M, &d[c],
                            ph1.c0, ph1.c1, ph0.wb, ph1.wb );
#if APP_ASRC_MEAS
                        if( pair_mute )
                        {
                            for( uint8_t l = 0u; l < 8u; l++ )
                            {
                                d[c + l] = 0;
                                d[APP_SLOTS_PER_FS + c + l] = 0;
                            }
                        }
#endif
#else
                        // Narrow bus (e.g. 96 kHz I2S, 2 slots): the kernel's fixed
                        // 16-int32 layout does not match the frame stride, so land it
                        // in a local buffer and copy out only the real slots. Same
                        // arithmetic and same emitted samples as the direct case.
                        int32_t pair_slots[16];
                        mchp_stream8_pair_slot_f32(
                            &a->ch[c][ph0.wbase],
                            (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                            ph0.c0, ph0.c1, ASRC_POLY_M, pair_slots,
                            ph1.c0, ph1.c1, ph0.wb, ph1.wb );
                        for( uint8_t s = 0u; s < APP_SLOTS_PER_FS; s++ )
                        {
                            const uint8_t ch = (uint8_t)( c + s );
#if APP_ASRC_MEAS
                            const int32_t v0 = pair_mute ? 0 : pair_slots[s];
                            const int32_t v1 = pair_mute ? 0 : pair_slots[8u + s];
#else
                            const int32_t v0 = pair_slots[s];
                            const int32_t v1 = pair_slots[8u + s];
#endif
                            // Channels past ASRC_CH cannot occur here (c < SLOTS and
                            // ASRC_CH is a multiple of 8), but keep the same
                            // "slot beyond the computed width emits silence" rule
                            // the single-output emit loop uses.
                            d[ch]                     = ( ch < ASRC_CH ) ? v0 : 0;
                            d[APP_SLOTS_PER_FS + ch]  = ( ch < ASRC_CH ) ? v1 : 0;
                        }
#endif // ASRC_PAIR_SLOT_DIRECT_OK
                    }
#if (ASRC_CH > APP_SLOTS_PER_FS)
                    else
                    {
                        int32_t hidden_slots[16];
                        mchp_stream8_pair_slot_f32(
                            &a->ch[c][ph0.wbase],
                            (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                            ph0.c0, ph0.c1, ASRC_POLY_M, hidden_slots,
                            ph1.c0, ph1.c1, ph0.wb, ph1.wb );
                        hidden_sink ^= (uint32_t)hidden_slots[0];
                        hidden_sink ^= (uint32_t)hidden_slots[15];
                    }
#endif
                }
#if (ASRC_CH > APP_SLOTS_PER_FS)
                s_hidden_output_sink = hidden_sink;
#endif
#endif
#if APP_ASRC_LOAD_TEST
                {
                    volatile int32_t osink = 0;
#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
                    int32_t slot0[16];
                    int32_t slot1[16];
#else
                    int32_t slot16[16];
#endif
                    for( uint8_t m = 1u; m < s_load_mult; m++ )
                    {
#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
                        ASRC_STREAM16_PAIR_SLOT(
                            &a->ch[0][ph0.wbase],
                            (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                            ph0.c0, ph0.c1, slot0, slot1,
                            ph1.c0, ph1.c1, ph0.wb, ph1.wb );
                        for( uint8_t l = 0u; l < 16u; l++ )
                        {
                            osink += slot0[l] + slot1[l];
                        }
#else
                        for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
                        {
                            mchp_stream8_pair_slot_f32(
                                &a->ch[c][ph0.wbase],
                                (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                ph0.c0, ph0.c1, ASRC_POLY_M, slot16,
                                ph1.c0, ph1.c1, ph0.wb, ph1.wb );
                            for( uint8_t l = 0u; l < 16u; l++ )
                            {
                                osink += slot16[l];
                            }
                        }
#endif
                    }
                    s_load_sink = (float)osink;
                }
#endif
                float frac2 = frac1 + step;
                uint32_t rd2 = rd1;
                while( frac2 >= 1.0f ) { frac2 -= 1.0f; rd2++; }
                a->frac = frac2;
                a->rd   = rd2;

                for( uint8_t s = ASRC_CH; s < APP_SLOTS_PER_FS; s++ )
                {
                    d[s] = 0;
                    d[APP_SLOTS_PER_FS + s] = 0;
                }
                d += 2u * APP_SLOTS_PER_FS;
                n++;
                continue;
            }
        }
#endif
        int32_t out[ASRC_CH];
        for( uint8_t c = 0u; c < ASRC_CH; c++ ) { out[c] = 0; }
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
        if( ( a->wr - a->rd ) >= ( ASRC_POLY_AHEAD + 1u ) )   // rd-MH .. rd+AHEAD window available
        {
#if (ASRC_POLY_METHOD == ASRC_POLY_Q31)
            /* One blended coefficient row per OUTPUT FRAME, shared by all 16
             * channels, then one plain 30-tap Q31 dot per channel.  No pair, no
             * d, no union window -- so, unlike the float pair path, this costs
             * the same at step == 1 and at step != 1.  The mask on out[] is the
             * s24-left store; sacr.l already rounded and saturated. */
            float          wb;
            const uint32_t pq    = asrc_q31_phase_of( a->frac, &wb );
            const uint32_t wbase = ( a->rd - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
            mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( pq ),
                                     ASRC_POLY_Q31_ROW( pq + 1u ),
                                     s_ceff_q31, ASRC_POLY_M, asrc_q31_wb( wb ) );
            mchp_asrc_q31_row16( &a->ch[0][wbase],
                                 (uint32_t)( ASRC_FIFO_PHYS * sizeof( asrc_samp_t ) ),
                                 s_ceff_q31, ASRC_POLY_M, out, ASRC_CH );
            for( uint8_t c = 0u; c < ASRC_CH; c++ )
            {
                out[c] = asrc_q31_to_slot( out[c] );
            }
#elif (ASRC_POLY_METHOD == ASRC_POLY_CEFF)
            float    c_eff[ASRC_POLY_M];
            uint32_t wbase;
            asrc_poly_phase_ceff( a->rd, a->frac, &wbase, c_eff );   // blend coeff ONCE, shared
            for( uint8_t c = 0u; c < ASRC_CH; c++ )
            {
                float acc;
                arm_dot_prod_f32( &a->ch[c][wbase], c_eff, ASRC_POLY_M, &acc );
                out[c] = asrc_to_slot( acc );
            }
#if APP_ASRC_LOAD_TEST
            {
                float sink = 0.0f, acc;
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c++ )
                    {
                        arm_dot_prod_f32( &a->ch[c][wbase], c_eff, ASRC_POLY_M, &acc );
                        sink += acc;
                    }
                }
                s_load_sink = sink;
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_DUAL4X)
#if (ASRC_CH & 3u)
#error "ASRC_POLY_DUAL4X requires ASRC_CH multiple of 4"
#endif
            asrc_phase_t ph;
            asrc_poly_phase( a->rd, a->frac, &ph );   // c0/c1/wb/wbase shared by all channels
            for( uint8_t c = 0u; c < ASRC_CH; c += 4u )   // coeff loaded once per 4-channel group
            {
                float o8[8];
                mchp_dot_prod4x2_f32( &a->ch[c][ph.wbase],
                                      (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                      ph.c0, ph.c1, ASRC_POLY_M, o8 );
                out[c]      = asrc_to_slot( o8[0] * ( 1.0f - ph.wb ) + o8[1] * ph.wb );
                out[c + 1u] = asrc_to_slot( o8[2] * ( 1.0f - ph.wb ) + o8[3] * ph.wb );
                out[c + 2u] = asrc_to_slot( o8[4] * ( 1.0f - ph.wb ) + o8[5] * ph.wb );
                out[c + 3u] = asrc_to_slot( o8[6] * ( 1.0f - ph.wb ) + o8[7] * ph.wb );
            }
#if APP_ASRC_LOAD_TEST
            {
                // D'-exact 16ch-equivalent compute benchmark: the extra pass mirrors the
                // FULL per-output cost of the real path (dot + inter-phase blend +
                // asrc_to_slot clamp/lrintf/shift), NOT dot-only. So mult=2 on an 8ch
                // build measures the TRUE 16ch compute wall, not a lower bound.
                volatile int32_t osink = 0;
                float o8[8];
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c += 4u )
                    {
                        mchp_dot_prod4x2_f32( &a->ch[c][ph.wbase],
                                              (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                              ph.c0, ph.c1, ASRC_POLY_M, o8 );
                        osink += asrc_to_slot( o8[0] * ( 1.0f - ph.wb ) + o8[1] * ph.wb );
                        osink += asrc_to_slot( o8[2] * ( 1.0f - ph.wb ) + o8[3] * ph.wb );
                        osink += asrc_to_slot( o8[4] * ( 1.0f - ph.wb ) + o8[5] * ph.wb );
                        osink += asrc_to_slot( o8[6] * ( 1.0f - ph.wb ) + o8[7] * ph.wb );
                    }
                }
                s_load_sink = (float)osink;
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_DUAL8X)
#if (ASRC_CH & 7u)
#error "ASRC_POLY_DUAL8X requires ASRC_CH multiple of 8"
#endif
            asrc_phase_t ph;
            asrc_poly_phase( a->rd, a->frac, &ph );
            for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
            {
                float o16[16];
                mchp_dot_prod8x2_f32( &a->ch[c][ph.wbase],
                                      (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                      ph.c0, ph.c1, ASRC_POLY_M, o16 );
                out[c]      = asrc_to_slot( o16[0]  * ( 1.0f - ph.wb ) + o16[1]  * ph.wb );
                out[c + 1u] = asrc_to_slot( o16[2]  * ( 1.0f - ph.wb ) + o16[3]  * ph.wb );
                out[c + 2u] = asrc_to_slot( o16[4]  * ( 1.0f - ph.wb ) + o16[5]  * ph.wb );
                out[c + 3u] = asrc_to_slot( o16[6]  * ( 1.0f - ph.wb ) + o16[7]  * ph.wb );
                out[c + 4u] = asrc_to_slot( o16[8]  * ( 1.0f - ph.wb ) + o16[9]  * ph.wb );
                out[c + 5u] = asrc_to_slot( o16[10] * ( 1.0f - ph.wb ) + o16[11] * ph.wb );
                out[c + 6u] = asrc_to_slot( o16[12] * ( 1.0f - ph.wb ) + o16[13] * ph.wb );
                out[c + 7u] = asrc_to_slot( o16[14] * ( 1.0f - ph.wb ) + o16[15] * ph.wb );
            }
#if APP_ASRC_LOAD_TEST
            {
                volatile int32_t osink = 0;
                float o16[16];
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
                    {
                        mchp_dot_prod8x2_f32( &a->ch[c][ph.wbase],
                                              (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                              ph.c0, ph.c1, ASRC_POLY_M, o16 );
                        osink += asrc_to_slot( o16[0]  * ( 1.0f - ph.wb ) + o16[1]  * ph.wb );
                        osink += asrc_to_slot( o16[2]  * ( 1.0f - ph.wb ) + o16[3]  * ph.wb );
                        osink += asrc_to_slot( o16[4]  * ( 1.0f - ph.wb ) + o16[5]  * ph.wb );
                        osink += asrc_to_slot( o16[6]  * ( 1.0f - ph.wb ) + o16[7]  * ph.wb );
                        osink += asrc_to_slot( o16[8]  * ( 1.0f - ph.wb ) + o16[9]  * ph.wb );
                        osink += asrc_to_slot( o16[10] * ( 1.0f - ph.wb ) + o16[11] * ph.wb );
                        osink += asrc_to_slot( o16[12] * ( 1.0f - ph.wb ) + o16[13] * ph.wb );
                        osink += asrc_to_slot( o16[14] * ( 1.0f - ph.wb ) + o16[15] * ph.wb );
                    }
                }
                s_load_sink = (float)osink;
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_SINGLE)
#if (ASRC_CH & 7u)
#error "ASRC_POLY_STREAM8_SINGLE requires ASRC_CH multiple of 8"
#endif
            const float* coeff;
            uint32_t     wbase;
            asrc_poly_phase_nearest( a->rd, a->frac, &coeff, &wbase );
            for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
            {
                float o8[8];
                mchp_stream8_single_f32( &a->ch[c][wbase],
                                         (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                         coeff, ASRC_POLY_M, o8 );
                out[c]      = asrc_to_slot( o8[0] );
                out[c + 1u] = asrc_to_slot( o8[1] );
                out[c + 2u] = asrc_to_slot( o8[2] );
                out[c + 3u] = asrc_to_slot( o8[3] );
                out[c + 4u] = asrc_to_slot( o8[4] );
                out[c + 5u] = asrc_to_slot( o8[5] );
                out[c + 6u] = asrc_to_slot( o8[6] );
                out[c + 7u] = asrc_to_slot( o8[7] );
            }
#if APP_ASRC_LOAD_TEST
            {
                volatile int32_t osink = 0;
                float o8[8];
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
                    {
                        mchp_stream8_single_f32( &a->ch[c][wbase],
                                                 (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                                 coeff, ASRC_POLY_M, o8 );
                        osink += asrc_to_slot( o8[0] ) + asrc_to_slot( o8[1] )
                               + asrc_to_slot( o8[2] ) + asrc_to_slot( o8[3] )
                               + asrc_to_slot( o8[4] ) + asrc_to_slot( o8[5] )
                               + asrc_to_slot( o8[6] ) + asrc_to_slot( o8[7] );
                    }
                }
                s_load_sink = (float)osink;
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_STREAM8) || \
      (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR)
#if (ASRC_CH & 7u)
#error "ASRC_POLY_STREAM8 requires ASRC_CH multiple of 8"
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8) && (ASRC_STREAM8_KERNEL != ASRC_STREAM8_BASE)
#error "ASRC_HISTORY_TILE8 currently supports ASRC_STREAM8_BASE only"
#endif
            asrc_phase_t ph;
            asrc_poly_phase( a->rd, a->frac, &ph );   // c0/c1/wb/wbase shared by all channels
            for( uint8_t c = 0u; c < ASRC_CH; c += 8u ) // ce blended once/tap, fanned to 8 ch
            {
                float o8[8];
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
                mchp_stream8_interleaved_f32( &a->tile[c >> 3u][ph.wbase][0],
                                               ph.c0, ph.c1, ASRC_POLY_M, o8, ph.wb );
#else
                ASRC_STREAM8( &a->ch[c][ph.wbase],
                                  (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                  ph.c0, ph.c1, ASRC_POLY_M, o8, ph.wb );
#endif
                out[c]      = asrc_to_slot( o8[0] );   // already blended in-kernel
                out[c + 1u] = asrc_to_slot( o8[1] );
                out[c + 2u] = asrc_to_slot( o8[2] );
                out[c + 3u] = asrc_to_slot( o8[3] );
                out[c + 4u] = asrc_to_slot( o8[4] );
                out[c + 5u] = asrc_to_slot( o8[5] );
                out[c + 6u] = asrc_to_slot( o8[6] );
                out[c + 7u] = asrc_to_slot( o8[7] );
            }
#if APP_ASRC_LOAD_TEST
            {
                // 16ch-equivalent bench (see DUAL4X note): full per-output path, not dot-only.
                volatile int32_t osink = 0;
                float o8[8];
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c += 8u )
                    {
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
                        mchp_stream8_interleaved_f32( &a->tile[c >> 3u][ph.wbase][0],
                                                       ph.c0, ph.c1, ASRC_POLY_M, o8, ph.wb );
#else
                        ASRC_STREAM8( &a->ch[c][ph.wbase],
                                          (uint32_t)( ASRC_FIFO_PHYS * sizeof(float) ),
                                          ph.c0, ph.c1, ASRC_POLY_M, o8, ph.wb );
#endif
                        osink += asrc_to_slot( o8[0] ) + asrc_to_slot( o8[1] )
                               + asrc_to_slot( o8[2] ) + asrc_to_slot( o8[3] )
                               + asrc_to_slot( o8[4] ) + asrc_to_slot( o8[5] )
                               + asrc_to_slot( o8[6] ) + asrc_to_slot( o8[7] );
                    }
                }
                s_load_sink = (float)osink;
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_DUAL2X)
#if (ASRC_CH & 1u)
#error "ASRC_POLY_DUAL2X requires an even ASRC_CH"
#endif
            asrc_phase_t ph;
            asrc_poly_phase( a->rd, a->frac, &ph );   // c0/c1/wb/wbase shared by all channels
            for( uint8_t c = 0u; c < ASRC_CH; c += 2u )   // coeff loaded once per channel PAIR
            {
                float o4[4];
                ASRC_DOT2X( &a->ch[c][ph.wbase], &a->ch[c + 1u][ph.wbase],
                            ph.c0, ph.c1, ASRC_POLY_M, o4 );
                out[c]      = asrc_to_slot( o4[0] * ( 1.0f - ph.wb ) + o4[1] * ph.wb );
                out[c + 1u] = asrc_to_slot( o4[2] * ( 1.0f - ph.wb ) + o4[3] * ph.wb );
            }
#if APP_ASRC_LOAD_TEST
            {
                float sink = 0.0f, o4[4];
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c += 2u )
                    {
                        ASRC_DOT2X( &a->ch[c][ph.wbase], &a->ch[c + 1u][ph.wbase],
                                    ph.c0, ph.c1, ASRC_POLY_M, o4 );
                        sink += o4[0] + o4[2];
                    }
                }
                s_load_sink = sink;
            }
#endif
#else  // ASRC_POLY_DUAL
            asrc_phase_t ph;
            asrc_poly_phase( a->rd, a->frac, &ph );   // phase calc ONCE, shared by all channels
            for( uint8_t c = 0u; c < ASRC_CH; c++ )
            {
                out[c] = asrc_to_slot( asrc_poly_at( a->ch[c], &ph ) );
            }
#if APP_ASRC_LOAD_TEST
            // Bench: emulate (mult-1) extra passes of the full channel set (results discarded via
            // the volatile sink) to project even higher channel-count CPU cost.
            {
                float sink = 0.0f;
                for( uint8_t m = 1u; m < s_load_mult; m++ )
                {
                    for( uint8_t c = 0u; c < ASRC_CH; c++ )
                    {
                        sink += asrc_poly_at( a->ch[c], &ph );
                    }
                }
                s_load_sink = sink;
            }
#endif
#endif  // ASRC_POLY_METHOD
            a->frac += step;
            while( a->frac >= 1.0f )
            {
                a->frac -= 1.0f;
                a->rd++;
            }
        }
        else
        {
            /*
             * Starved: the poly window is short, so no new sample can be computed.  HOLD the
             * previous frame rather than shipping the zero-initialised out[] -- digital silence
             * is a full-scale discontinuity on every channel at once, i.e. an audible click,
             * while a held sample is a one-frame stall.  rd/frac deliberately do not advance,
             * so no phase is lost; the next pull resumes from here.
             */
            /*
             * Two sources, two widths.  a->last_out is ASRC_CH wide, but a frame inside the block
             * buffer is only APP_SLOTS_PER_FS wide, so reading ASRC_CH words out of
             * `d - APP_SLOTS_PER_FS` ran off the end of the frame -- and off the block itself on
             * the last iteration.  Take the in-buffer channels from the buffer and the rest --
             * which the buffer never held -- from the carry.
             */
            const int32_t* const prev = ( n == 0u ) ? a->last_out
                                                    : ( d - APP_SLOTS_PER_FS );
            const uint8_t prev_ch = ( n == 0u ) ? (uint8_t)ASRC_CH : (uint8_t)ASRC_CARRY_CH;
            for( uint8_t c = 0u; c < prev_ch; c++ )      { out[c] = prev[c]; }
            for( uint8_t c = prev_ch; c < ASRC_CH; c++ ) { out[c] = a->last_out[c]; }
            a->dbg_starve_frames++;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
            underrun_this_pull = 1u;
#endif
        }
#else
        if( ( a->wr - a->rd ) >= 3u )   // rd-1..rd+2 window available
        {
            const uint32_t i0 = ( a->rd - 1u ) & ASRC_FIFO_MASK;
            const uint32_t i1 = ( a->rd      ) & ASRC_FIFO_MASK;
            const uint32_t i2 = ( a->rd + 1u ) & ASRC_FIFO_MASK;
            const uint32_t i3 = ( a->rd + 2u ) & ASRC_FIFO_MASK;
            for( uint8_t c = 0u; c < ASRC_CH; c++ )
            {
                out[c] = asrc_to_slot( asrc_cubic( a->ch[c][i0], a->ch[c][i1],
                                                   a->ch[c][i2], a->ch[c][i3], a->frac ) );
            }
            a->frac += step;
            while( a->frac >= 1.0f )
            {
                a->frac -= 1.0f;
                a->rd++;
            }
        }
        else
        {
            /*
             * Starved: the poly window is short, so no new sample can be computed.  HOLD the
             * previous frame rather than shipping the zero-initialised out[] -- digital silence
             * is a full-scale discontinuity on every channel at once, i.e. an audible click,
             * while a held sample is a one-frame stall.  rd/frac deliberately do not advance,
             * so no phase is lost; the next pull resumes from here.
             */
            /*
             * Two sources, two widths.  a->last_out is ASRC_CH wide, but a frame inside the block
             * buffer is only APP_SLOTS_PER_FS wide, so reading ASRC_CH words out of
             * `d - APP_SLOTS_PER_FS` ran off the end of the frame -- and off the block itself on
             * the last iteration.  Take the in-buffer channels from the buffer and the rest --
             * which the buffer never held -- from the carry.
             */
            const int32_t* const prev = ( n == 0u ) ? a->last_out
                                                    : ( d - APP_SLOTS_PER_FS );
            const uint8_t prev_ch = ( n == 0u ) ? (uint8_t)ASRC_CH : (uint8_t)ASRC_CARRY_CH;
            for( uint8_t c = 0u; c < prev_ch; c++ )      { out[c] = prev[c]; }
            for( uint8_t c = prev_ch; c < ASRC_CH; c++ ) { out[c] = a->last_out[c]; }
            a->dbg_starve_frames++;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
            underrun_this_pull = 1u;
#endif
        }
#endif
#if APP_ASRC_MEAS
        // Q50 ACQUIRE: mute the output while the fast-acquire transient settles (audio comes up clean
        // only at HANDOVER->TRACK). Servo/FIFO advance normally above; only the emitted slots are zeroed.
        const uint8_t q50_mute = ( s_q50_en && s_q50_state == Q50_ACQUIRE ) ? 1u : 0u;
        for( uint8_t s = 0u; s < APP_SLOTS_PER_FS; s++ )
        {
            d[s] = ( ( s < ASRC_CH ) && !q50_mute ) ? out[s] : 0;
        }
#else
        for( uint8_t s = 0u; s < APP_SLOTS_PER_FS; s++ )
        {
            d[s] = ( s < ASRC_CH ) ? out[s] : 0; // ch0/ch1 = real L/R; extra ch -> extra slots
        }
#endif
        d += APP_SLOTS_PER_FS;
    }

    /*
     * Carry the frame just emitted so a starve on frame 0 of the NEXT pull can hold it.
     *
     * Bound the copy by the slots the block buffer actually HAS, not by ASRC_CH: a frame in `d`
     * is APP_SLOTS_PER_FS words wide, so with ASRC_CH = 16 and 8 physical slots the old
     * `c < ASRC_CH` walked 8 words (32 B) past the frame -- and past the whole block on the last
     * iteration, reading the neighbouring ping-pong half or, when `dst` was the pong half, the
     * next object in .bss.  Channels at or above APP_SLOTS_PER_FS are never stored in `d` (they
     * go to the caller's hidden_slots[], which is out of scope here), so their carry keeps its
     * previous value; they are not emitted to any slot, so a stale hold has no audible effect.
     */
    for( uint8_t c = 0u; c < ASRC_CARRY_CH; c++ ) { a->last_out[c] = ( d - APP_SLOTS_PER_FS )[c]; }

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16 && \
    (APP_BLOCK_FRAMES == 16u) && !APP_ASRC_MEAS && ASRC_PAIR_SLOT_DIRECT_OK
asrc_output_done:
#endif

#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    if( underrun_this_pull ) { a->dbg_intermediate_underrun++; }
#endif

#if APP_ASRC_MEAS
    a->dbg_wraps = (uint16_t)( a->rd - rd_block0 );   // Q34: wraps this block (for next tick, read-only)
#endif

    // telemetry: running peak of this pull's compute time (resampler only).
    {
        const uint32_t elapsed_ticks = nora_high_res_timer_get_count() - t0_cnt;
        if( elapsed_ticks > a->dbg_pull_ticks_max ) { a->dbg_pull_ticks_max = elapsed_ticks; }
    }
}


//===========================================================
// Global Functions (public API -- direction wrappers over the engine)
//===========================================================
void audio_app_asrc_reset_all( void )
{
    /* UNCONDITIONAL image identity line.  Two things made the wrong image get measured
     * before: the "Q31 ..." selftest lines only exist when Q31 is on, so their ABSENCE is
     * ambiguous (float build, or a build with the selftest compiled out); and the boot
     * banner's __DATE__/__TIME__ does not change on an incremental rebuild, so a build that
     * only changed -Define reports the previous image's timestamp.  This line always prints
     * and always names the arm, so a capture log identifies its own image. */
    {
        static uint8_t arm_line_done = 0u;
        if( !arm_line_done )
        {
            arm_line_done = 1u;
            printf( " ASRC build: sample arm=%s poly=%d frontend=%s meas=%s\n",
#if ASRC_SAMPLE_Q31
                    "Q31",
#else
                    "float",
#endif
                    (int)ASRC_POLY_METHOD,
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
                    "runtime-decimator",
#else
                    "none",
#endif
#if APP_ASRC_MEAS
                    "on" );
#else
                    "off" );
#endif
        }
    }
#if ASRC_HAVE_FIXED_PUSH_BLOCK16
    asrc_push_block_selftest();
#endif
#if ( APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8 ) && \
    ( ASRC_HISTORY_LAYOUT != ASRC_HISTORY_TILE8 )
    // Borrows s_asrc[AB].ch as scratch, like the Q31 poly selftest below; the reset
    // at the end of this function clears it.
    asrc_push_frames_selftest();
#endif
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
#if ASRC_SAMPLE_Q31
    asrc_poly_build_q31();      // one-time: generate the Q31 polyphase table (app context)
#if APP_ASRC_Q31_SELFTEST
    // Runs BEFORE asrc_reset(): it borrows s_asrc[AB].ch as 16-channel scratch and
    // the reset below clears it.  Failures are latched in s_poly_q31_fail.
    s_poly_q31_fails = asrc_poly_q31_selftest();
    // `ties` is the count of sacr.l rounding ties (exactly +-256 in LSB24 terms)
    // between the assembly kernels and the portable C reference.  A tie is not a
    // failure, but a count that starts moving is worth seeing, so it is printed
    // rather than hidden -- same policy as the Q31 front-end selftest.
    printf( " ASRC Q31 poly selftest: %s (fails %lu, rounding ties %lu)\n",
            ( s_poly_q31_fails == 0u ) ? "pass" : "FAIL",
            (unsigned long)s_poly_q31_fails,
            (unsigned long)asrc_poly_q31_selftest_ties() );
    if( s_poly_q31_fails != 0u )
    {
        const asrc_poly_q31_fail_t* f = asrc_poly_q31_selftest_fail();
        printf( "  %s: vec %lu idx %lu got %ld want %ld\n",
                f->what, (unsigned long)f->vec, (unsigned long)f->idx,
                (long)f->got, (long)f->expect );
    }
#endif
#else
    asrc_poly_build();          // one-time: generate the polyphase table (guarded, app context)
#endif
#if (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR)
    asrc_slot_batch_selftest();
    asrc_pair_slot_selftest();
#if (ASRC_CH == 16u) && ASRC_HAVE_FIXED_STREAM16
    asrc_pair16_slot_selftest();
    asrc_block16_slot_selftest();
    asrc_paird16_slot_selftest();
#endif
#endif
#endif
    asrc_reset( &s_asrc[ASRC_ENGINE_AB] );   // ratio invalid until a feed-forward value is set
#if APP_B_ROUTE_USES_BA
    asrc_reset( &s_asrc[ASRC_ENGINE_BA] );
#endif
}

// Apply a feed-forward ratio to one instance. On the invalid->valid transition (first valid
// ratio after (re)start), CENTRE the FIFO: while silent the producer kept filling the ring
// (consumer held rd), so fill sits near full. Snap rd = wr - target and reset the phase so
// resampling starts at mid-fill (target latency) instead of draining full->target over ~2 s.
// App context; the consumer ISR holds rd while ratio<=0, so this rd write is race-free (the
// producer only advances wr). Set ratio LAST so the next pull sees the centred rd.
// Ratio-aware fill setpoint for one instance: the block look-ahead R(step) plus jitter slack, never
// below the calibrated default and never so deep that the producer's overflow guard could fire.
// Called at ratio-lock only (app context, consumer holds rd while ratio<=0) -- not in the hot path.
static void asrc_set_fill_target( asrc_t* a, float step )
{
    /* R(step) = floor(step*(BLOCK-1)) + AHEAD + 1; +ASRC_FILL_JITTER because fill is observed
     * integer-quantised at block boundaries and the applied step may sit a hair above `ratio`. */
    const uint32_t look = (uint32_t)( step * (float)( APP_BLOCK_FRAMES - 1u ) );
    uint32_t need = look + ASRC_POLY_AHEAD + 1u + ASRC_FILL_JITTER;
    a->dbg_R = (uint16_t)( need - ASRC_FILL_JITTER );   // R(step) itself, before the jitter slack
    /* T1: the starve-feasibility bound for THIS ratio (see dbg_jmax). Integer, no float compare;
     * clamps to 0 rather than wrapping when the look-ahead alone exceeds the ring. */
    {
        const uint32_t spent = (uint32_t)APP_BLOCK_FRAMES + 20u + look;
        a->dbg_jmax = ( (uint32_t)ASRC_FIFO_FRAMES > spent )
                    ? (uint8_t)( ( (uint32_t)ASRC_FIFO_FRAMES - spent ) / 2u )
                    : 0u;
    }
    if( need < (uint32_t)ASRC_FILL_TARGET ) { need = (uint32_t)ASRC_FILL_TARGET; }
    if( need > ASRC_FILL_TARGET_MAX )
    {
        /* The ring cannot hold this ratio's look-ahead AND a producer block. Clamp (best effort:
         * fewer, shallower dropouts than an unclamped setpoint that would trip the overflow guard)
         * and flag it -- the honest fix for such a ratio is a decimating front end, see
         * asrc_audio_path_ab_fixed_rate_den(). */
        need = ASRC_FILL_TARGET_MAX;
        a->fill_target_capped = 1u;
    }
    else
    {
        a->fill_target_capped = 0u;
    }
    a->fill_target = (float)need;
}

static void asrc_apply_ratio( asrc_t* a, float ratio )
{
#if APP_ASRC_MEAS
    if( a->freeze_step > 0.0f ) { return; }   // bench freeze: ignore feed-forward updates
#endif
    if( ratio <= 0.0f ) { a->ratio = 0.0f; return; }
    if( a->ratio <= 0.0f )   // invalid -> valid: centre the ring + seed the control state
    {
        asrc_set_fill_target( a, ratio );   // BEFORE the rd snap below: it centres on the setpoint
        a->rd         = a->wr - (uint32_t)a->fill_target;
#if APP_ASRC_FAST_ACQUIRE || APP_ASRC_MEAS
        {
            /* Fast-acquire startup pre-bias. The producer delivers APP_BLOCK_FRAMES-frame blocks while the consumer
             * reads smoothly; right after ratio-lock the fill sawtooth mean sits well above TARGET (a
             * fixed startup kick set by the producer/consumer block alignment), and the KP-limited servo
             * would ring that out over ~14 s. Pre-biasing rd by that fixed offset starts the sawtooth at
             * TARGET, so there is no wind-up to settle -> audible warm-up ~14 s -> ~4 s. The offset is a
             * property of the build's per-block timing (deterministic; measured via the *nt40 early-boot
             * logger). APP_ASRC_FAST_ACQUIRE_OFFSET is calibrated for the ASRC profile (demo DSP absent);
             * re-measure it with *nt40 if the per-block processing load changes. Steady state is untouched
             * (this only sets the initial rd). NOTE: a phase-aware variant (off = center - intra_block
             * phase) was tried but is timing-fragile -- the phase-at-lock is itself deterministic per
             * build, so a fixed offset is simpler and robust; see docs asrc §13/§14. */
            int16_t lock_off = (int16_t)APP_ASRC_FAST_ACQUIRE_OFFSET;   /* shipping: fixed, calibrated */
#if APP_ASRC_RUNTIME_48K_TO_8
            /* The +28 direct-path pre-bias was calibrated near 48 kHz.  Fixed
             * low-rate front ends use intermediate-rate FIFO units and start
             * at the geometric centre.  Direction-symmetric: whichever engine
             * carries the active front end starts unbiased (at most one does). */
            const bool front_end_engine =
                ( ( a == &s_asrc[ASRC_ENGINE_AB] ) && ( asrc_audio_path_ab_fixed_rate_den() != 1u ) )
#if APP_B_ROUTE_USES_BA
                || ( ( a == &s_asrc[ASRC_ENGINE_BA] ) && ( asrc_audio_path_ba_fixed_rate_den() != 1u ) )
#endif
                ;
            if( front_end_engine )
            {
                lock_off = 0;
            }
#endif
#if APP_ASRC_MEAS
            /* MEAS experiment overrides: Q58 phase-aware (tunable center) or Q57 fixed offset knob. */
            const uint32_t el10 = nora_high_res_timer_elapsed_us_x10( a->dbg_rx_tick );
            float phase = (float)el10 * ( APP_MEAS_FS_A_HZ * 1.0e-7f );   /* nominal fs_A; block-phase only */
            if( phase < 0.0f ) { phase = 0.0f; }
            else if( phase > (float)APP_BLOCK_FRAMES ) { phase = (float)APP_BLOCK_FRAMES; }
            if( s_q58_pa_en ) { lock_off = (int16_t)( s_q58_center - phase ); s_q58_last_phase = phase; s_q58_last_off = lock_off; }
            else              { lock_off = s_q57_lock_off; }
#endif
            a->rd += (uint32_t)(int32_t)lock_off;
        }
#endif
        a->frac       = 0.0f;
        a->corr_lpf   = 0.0f;
        a->step_state = ratio;   // start the slew-limited step at the measured ratio
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
        /* Discard silent priming history from the active-path acceptance
         * counter. Any loss after this ratio-lock/recentre remains visible. */
        a->dbg_intermediate_overflow = 0u;
        a->dbg_intermediate_underrun = 0u;
#endif
        /*
         * Same discard for the DIRECT path's counter, which this block used to miss: the rd snap
         * above throws away exactly the frames the consumer-side guard charged to dbg_guard_drops
         * while it was holding rd (ratio invalid), so leaving them counted made `drop` report the
         * length of the pre-lock window instead of audible dropouts. Measured 2026-08-27: drop ==
         * pre-lock seconds x producer Hz, on both AK128 and AK512.  [internal]
         * [internal] report_ak128_main_sanity_and_drop_step_2026-08-27.md section 3.
         * Race-free for the same reason as the rd write above (the consumer holds rd while
         * ratio<=0, and `ratio` is only set at the end of this function).
         */
        a->dbg_guard_drops = 0u;
#if APP_ASRC_MEAS
        q50_on_ratio_lock();     // Q50: (re)arm the fast-acquire state machine (ACQUIRE if enabled)
        s_q55log_n = 0u; s_q55log_ctr = 0u; s_q55log_arm = 1u;   // Q55: arm the early-boot logger
#endif
        /*
         * One line per ratio-lock carrying the DESIGN values, so the periodic line can stay
         * health-only (P1). Everything here is constant between restarts: printing it every
         * ~2 s is what made the periodic line unreadable. `!` = the setpoint did not fit the
         * ring (clamped); `!J` = starve cannot reach 0 at this ratio (J > Jmax), which is the
         * derived form of the acceptance criterion rather than a per-part constant.
         * App context, once per (re)start.
         */
        printf(" ASRC %s lock: step=%.5f R=%lu set=%lu%s J=%u/Jmax=%u%s\n",
               ( a == &s_asrc[ASRC_ENGINE_AB] ) ? "AB" : "BA",
               (double)ratio,
               (unsigned long)a->dbg_R,
               (unsigned long)a->fill_target,
               a->fill_target_capped ? "!" : "",
               (unsigned)ASRC_FILL_JITTER,
               (unsigned)a->dbg_jmax,
               ( (uint32_t)ASRC_FILL_JITTER > (uint32_t)a->dbg_jmax ) ? "!J" : "" );
    }
    a->ratio = ratio;
}

// Feed-forward: set the live in/out ratio for a direction. ab = fs_A/fs_B (A->B), ba = its
// reciprocal (B->A). A non-positive value leaves the resampler silent (startup). Written
// here (app context), read in the RX-block ISR -- a 32-bit float store is atomic on this core.
void audio_app_asrc_set_ratio_ab( float ratio ) { asrc_apply_ratio( &s_asrc[ASRC_ENGINE_AB], ratio ); }

#if APP_ASRC_LOAD_TEST
// Bench: set the interp load multiplier (1..MAX). Effective work is mult*ASRC_CH per direction.
void audio_app_asrc_set_load_mult( uint8_t mult )
{
    if( mult < 1u ) { mult = 1u; }
    if( mult > ASRC_LOAD_MULT_MAX ) { mult = ASRC_LOAD_MULT_MAX; }
    s_load_mult = mult;
}
uint8_t audio_app_asrc_get_load_mult( void ) { return s_load_mult; }
#endif

#if APP_ASRC_MEAS
// Bench: latch the CONVERGED resample step as a CONSTANT (disables fill trim + feed-forward)
// so a capture sees only the interpolation kernel, not control-loop step jitter.
//
// Freeze step_state (= ratio*(1+corr_lpf), slew-limited -- the step the loop ACTUALLY converged
// to and applies every block), NOT the raw feed-forward ratio. Freezing ratio throws away the
// corr_lpf residual the loop settled on, so the held rate is slightly off the true clock ratio
// and the FIFO fill walks; step_state holds the corrected rate so the fill stays put. Falls back
// to ratio only if step_state has not been seeded yet.
static void asrc_freeze_one( asrc_t* a, const char* tag )
{
    const float chosen = ( a->step_state > 0.0f ) ? a->step_state
                       : ( a->ratio      > 0.0f ) ? a->ratio : 0.0f;
    if( chosen > 0.0f ) { a->freeze_step = chosen; a->freeze_base_step = chosen; }  // Q3: latch base
    printf(" *MEAS freeze %s: ratio=%.6f corr_lpf=%.2e step_state=%.6f -> freeze=%.6f\n",
           tag, (double)a->ratio, (double)a->corr_lpf,
           (double)a->step_state, (double)a->freeze_step );
}
void audio_app_asrc_freeze( void )
{
    asrc_freeze_one( &s_asrc[ASRC_ENGINE_AB], "ab" );
#if APP_B_ROUTE_USES_BA
    asrc_freeze_one( &s_asrc[ASRC_ENGINE_BA], "ba" );
#endif
}
void audio_app_asrc_freeze_ratio_ab( void )
{
    asrc_t* a = &s_asrc[ASRC_ENGINE_AB];
    if( a->ratio > 0.0f )
    {
        a->freeze_step = a->ratio;
        a->freeze_base_step = a->ratio;
        a->rd = a->wr - (uint32_t)a->fill_target;
        a->frac = 0.0f;
    }
    printf(" *MEAS ratio-freeze ab: ratio=%.7f fill=%lu/%lu -> freeze=%.7f, FIFO centred\n",
           (double)a->ratio, (unsigned long)( a->wr - a->rd ),
           (unsigned long)ASRC_FIFO_FRAMES, (double)a->freeze_step );
}
void audio_app_asrc_unfreeze( void )
{
    s_asrc[ASRC_ENGINE_AB].freeze_step = 0.0f;
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].freeze_step = 0.0f;
#endif
}
float audio_app_asrc_get_freeze_step_ab( void ) { return s_asrc[ASRC_ENGINE_AB].freeze_step; }

// --- R12 Q12: intentional FEED-FORWARD ratio freeze (NOT Mode-K) --------------------------------
// Latch the current live feed-forward ratio and make the controller use it, while corr_lpf / clamp /
// slew / applied step stay fully live. a->ratio keeps updating from the live CCP estimator so
// telemetry can confirm the raw ratio still moves while the controller's ratio is constant.
static void asrc_ff_freeze_one( asrc_t* a, const char* tag )
{
    a->ff_frozen_ratio = a->ratio;
    a->ff_freeze       = 1u;
    printf(" *MEAS FF-freeze %s: frozen_ratio=%.7f (live a->ratio still updates; corr_lpf/slew/step LIVE)\n",
           tag, (double)a->ff_frozen_ratio );
}
void audio_app_asrc_ff_freeze( void )
{
    asrc_ff_freeze_one( &s_asrc[ASRC_ENGINE_AB], "ab" );
#if APP_B_ROUTE_USES_BA
    asrc_ff_freeze_one( &s_asrc[ASRC_ENGINE_BA], "ba" );
#endif
}
void audio_app_asrc_ff_unfreeze( void )
{
    s_asrc[ASRC_ENGINE_AB].ff_freeze = 0u;
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].ff_freeze = 0u;
#endif
    s_q16_freeze_latched = 0u;   // R16: arm a fresh freeze-epoch latch for the next freeze
    printf(" *MEAS FF-unfreeze (controller back to live ratio)\n");
}
float   audio_app_asrc_get_ff_frozen_ratio_ab( void ) { return s_asrc[ASRC_ENGINE_AB].ff_frozen_ratio; }
uint8_t audio_app_asrc_get_ff_freeze_ab( void )       { return s_asrc[ASRC_ENGINE_AB].ff_freeze; }
// R16 Q16: freeze-age accessors. age_pulls = pulls since the frozen-FF t=0 (0 if not yet frozen).
uint32_t audio_app_asrc_get_q16_pull_ctr( void )     { return s_q16_pull_ctr; }
uint32_t audio_app_asrc_get_q16_freeze_epoch( void ) { return s_q16_freeze_epoch; }
uint32_t audio_app_asrc_get_q16_age_pulls( void )    { return s_q16_freeze_latched ? ( s_q16_pull_ctr - s_q16_freeze_epoch ) : 0u; }
float    audio_app_asrc_get_ratio_live_ab( void )    { return s_asrc[ASRC_ENGINE_AB].ratio; }

// --- R13 Q13: hold corr_lpf constant (fill-servo integration stopped), step path still live ------
void audio_app_asrc_corr_hold( void )
{
    s_asrc[ASRC_ENGINE_AB].corr_hold = 1u;
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].corr_hold = 1u;
#endif
    printf(" *MEAS corr-HOLD ab: corr_lpf held=%.3e (fill still moves; step slew/clamp LIVE; NOT Mode-K)\n",
           (double)s_asrc[ASRC_ENGINE_AB].corr_lpf );
}
void audio_app_asrc_corr_release( void )
{
    s_asrc[ASRC_ENGINE_AB].corr_hold = 0u;
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].corr_hold = 0u;
#endif
    printf(" *MEAS corr-RELEASE (fill servo back to live)\n");
}
uint8_t audio_app_asrc_get_corr_hold_ab( void ) { return s_asrc[ASRC_ENGINE_AB].corr_hold; }

// --- R14 Q14: select MA64 vs raw fill for the servo observation (MA runs continuously either way) --
void audio_app_asrc_fill_use_ma( uint8_t on )
{
    s_q14_use_ma = on ? 1u : 0u;
    printf(" *MEAS fill observation = %s (MA64 runs continuously; servo eq unchanged)\n",
           on ? "MA64" : "RAW" );
}
uint8_t audio_app_asrc_get_fill_use_ma_ab( void ) { return s_q14_use_ma; }
// Q40: continuous-fill estimator toggle (servo-observation only; wr/rd/FIFO untouched).
void audio_app_asrc_cfill_en( uint8_t on )
{
    s_cfill_en = on ? 1u : 0u;
    printf(" *MEAS continuous-fill est = %s (servo fill obs only; wr/rd unchanged)\n",
           on ? "ON" : "OFF" );
}
uint8_t audio_app_asrc_get_cfill_en( void ) { return s_cfill_en; }
// Q41: automatic phase-centering toggle. Enabling (re)starts the calibration window.
void audio_app_asrc_pc_auto( uint8_t on )
{
    if( on )
    {
        s_pc_sum = 0.0f; s_pc_cnt = 0u; s_pc_ready = 0u; s_pc_center = 0.0f;   // fresh calibration
        s_pc_auto = 1u;
    }
    else
    {
        s_pc_auto = 0u;
    }
    printf(" *MEAS phase-center = %s (cal=%.2fs -> %lu pulls; fixed center=%.1f)\n",
           on ? "AUTO" : "FIXED(BLOCK/2)", (double)Q41_CAL_SECONDS,
           (unsigned long)Q41_CAL_N, (double)( (float)APP_BLOCK_FRAMES * 0.5f ) );
}
// Q41/Q42: status readout (host reads after settle to log the measured center + producer period).
void audio_app_asrc_pc_status( void )
{
    printf(" *MEAS pc_status: cfill=%u auto=%u ready=%u center=%.4f cal_n=%lu hf=%u pp_ready=%u"
           " pp_period_us10=%.2f pp_cnt=%lu\n",
           (unsigned)s_cfill_en, (unsigned)s_pc_auto, (unsigned)s_pc_ready,
           (double)s_pc_center, (unsigned long)Q41_CAL_N, (unsigned)s_hf_en, (unsigned)s_pp_ready,
           (double)s_pp_period, (unsigned long)s_pp_cnt );
}
// Q42: high-fidelity measured-period toggle. Enabling (re)starts BOTH calibrations: stage 1 producer
// period, then stage 2 phase center (with the new slope). Requires s_pc_auto for the center stage.
void audio_app_asrc_hf_en( uint8_t on )
{
    if( on )
    {
        s_pp_sum = 0.0f; s_pp_total = 0.0f; s_pp_cnt = 0u; s_pp_ready = 0u; s_pp_period = 0.0f; s_pp_prev = 0u;
        s_pc_sum = 0.0f; s_pc_cnt = 0u; s_pc_ready = 0u; s_pc_center = 0.0f;   // re-center with new slope
        s_hf_en = 1u;
    }
    else
    {
        s_hf_en = 0u;
    }
    printf(" *MEAS hf-phase = %s (measured producer period; cal %.2fs stage1 + %.2fs stage2)\n",
           on ? "MEASURED" : "FIXED(fs_A)", (double)Q41_CAL_SECONDS, (double)Q41_CAL_SECONDS );
}
float   audio_app_asrc_get_fill_ma_ab( void )     { return s_q14_ma_ready ? s_q14_fill_sum * ( 1.0f / 64.0f ) : 0.0f; }

// --- R15 Q15: corr_lpf 1 Hz band-split MOTION selector (0=FULL,1=SLOW,2=FAST,3=HOLD) -----------
// Sets the mode and arms a one-pull capture of the held component so the A->S/F/H transition is
// continuous by construction. corr_lpf / corr_slow keep running in all modes.
void audio_app_asrc_corr_mode( uint8_t mode )
{
    g_q15_corr_mode       = ( mode <= 3u ) ? mode : 0u;
    s_q15_capture_pending = ( g_q15_corr_mode != 0u ) ? 1u : 0u;   // FULL needs no hold capture
    printf(" *MEAS corr band-mode = %s (1Hz split; corr_full/slow live; motion-only; NOT Mode-K)\n",
           ( g_q15_corr_mode == 1u ) ? "SLOW-motion" : ( g_q15_corr_mode == 2u ) ? "FAST-motion"
         : ( g_q15_corr_mode == 3u ) ? "HOLD" : "FULL" );
}
uint8_t audio_app_asrc_get_corr_mode_ab( void )      { return g_q15_corr_mode; }
float   audio_app_asrc_get_corr_slow_ab( void )      { return g_q15_corr_slow; }

// --- Q3 (±64 Hz forensic): MEAS-only frozen-step perturbation, NON-cumulative from the base ---
// Every call derives the applied step from freeze_base_step (latched once at freeze), never from the
// current applied value, so ±ULP / ±ppm points are all offsets from the SAME converged base. This
// does NOT change freeze semantics; it only replaces the held constant with base ± a tiny offset.
static int      s_step_delta_mode = 0;   // 0=base, 1=ULP, 2=ppm
static int32_t  s_step_delta_req  = 0;   // requested signed ULP count or ppm

// mode: 0=return to base (req ignored), 1=ULP (req=signed ULP count), 2=ppm (req=signed ppm).
void audio_app_asrc_set_step_delta( int mode, int32_t req )
{
    asrc_t* a = &s_asrc[ASRC_ENGINE_AB];
    if( a->freeze_step <= 0.0f ) { printf(" *MEAS_STEP reject: not frozen\n"); return; }
    const float base = a->freeze_base_step;
    union { float f; uint32_t u; } bb; bb.f = base;
    union { float f; uint32_t u; } ap;
    float applied;
    if( mode == 1 )                          // exact float ULP offset via bitcast
    {
        ap.u = (uint32_t)( (int32_t)bb.u + req );
        applied = ap.f;
    }
    else if( mode == 2 )                     // engineering ppm offset from base
    {
        if( ( req > 1000 ) || ( req < -1000 ) ) { printf(" *MEAS_STEP reject: |ppm|>1000\n"); return; }
        applied = base * ( 1.0f + (float)req * 1.0e-6f );
        ap.f = applied;
    }
    else                                     // return to unperturbed base
    {
        applied = base; ap.f = base; req = 0; mode = 0;
    }
    if( !( applied > 0.0f ) || !( applied < 10.0f ) || ( applied != applied ) )  // >0, in-range, non-NaN
    {
        printf(" *MEAS_STEP reject: unsafe applied=%.7f\n", (double)applied ); return;
    }
    a->freeze_step    = applied;
    s_step_delta_mode = mode;
    s_step_delta_req  = req;
    const float ppm_actual = ( base > 0.0f ) ? ( applied / base - 1.0f ) * 1.0e6f : 0.0f;
    printf(" *MEAS_STEP mode=%s req=%ld base=%.7f bits=0x%08lX applied=%.7f bits=0x%08lX "
           "delta_ppm=%.4f fill=%lu\n",
           (mode==1)?"ULP":(mode==2)?"ppm":"base", (long)req,
           (double)base, (unsigned long)bb.u, (double)applied, (unsigned long)ap.u,
           (double)ppm_actual, (unsigned long)( a->wr - a->rd ) );
}
float   audio_app_asrc_get_freeze_base_step_ab( void ) { return s_asrc[ASRC_ENGINE_AB].freeze_base_step; }
int     audio_app_asrc_get_step_delta_mode( void )     { return s_step_delta_mode; }
int32_t audio_app_asrc_get_step_delta_req( void )      { return s_step_delta_req; }
uint32_t audio_app_asrc_get_fill_ab( void )            { return s_asrc[ASRC_ENGINE_AB].wr - s_asrc[ASRC_ENGINE_AB].rd; }
// Q19: narrow read-only accessors for the two live control values the causal map needs (the same
// two already passed to audio_app_meas_trace_tick): the low-pass-filtered fill correction and the
// slew-limited applied resample step. Read-only; do not expose the rest of asrc_t.
float   audio_app_asrc_get_corr_lpf_ab( void )         { return s_asrc[ASRC_ENGINE_AB].corr_lpf; }
float   audio_app_asrc_get_step_state_ab( void )       { return s_asrc[ASRC_ENGINE_AB].step_state; }
#endif

void audio_app_asrc_push_ab( const int32_t* src ) { asrc_push( &s_asrc[ASRC_ENGINE_AB], src ); }
void audio_app_asrc_pull_ab( int32_t* dst )       { asrc_pull( &s_asrc[ASRC_ENGINE_AB], dst ); }
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
void audio_app_asrc_push_ab_frames( const int32_t* src, size_t frames, size_t stride )
{
    if( ( src != NULL ) && ( stride >= 2u ) )
    {
        asrc_push_frames( &s_asrc[ASRC_ENGINE_AB], src, frames, stride );
    }
}
uint32_t audio_app_asrc_intermediate_overflow_count( void )
{
    return s_asrc[ASRC_ENGINE_AB].dbg_intermediate_overflow;
}
uint32_t audio_app_asrc_intermediate_underrun_count( void )
{
    return s_asrc[ASRC_ENGINE_AB].dbg_intermediate_underrun;
}
#endif

#if APP_B_ROUTE_USES_BA
void audio_app_asrc_set_ratio_ba( float ratio ) { asrc_apply_ratio( &s_asrc[ASRC_ENGINE_BA], ratio ); }
void audio_app_asrc_push_ba( const int32_t* src ) { asrc_push( &s_asrc[ASRC_ENGINE_BA], src ); }
void audio_app_asrc_pull_ba( int32_t* dst )       { asrc_pull( &s_asrc[ASRC_ENGINE_BA], dst ); }
#if APP_ASRC_RUNTIME_48K_TO_8
/* Same producer as the A->B side, for the mirror-image case: leg B at 48 kHz feeding a
 * low-rate leg A, so B->A is the down-sampling direction and owns the front end. */
void audio_app_asrc_push_ba_frames( const int32_t* src, size_t frames, size_t stride )
{
    if( ( src != NULL ) && ( stride >= 2u ) )
    {
        asrc_push_frames( &s_asrc[ASRC_ENGINE_BA], src, frames, stride );
    }
}
uint32_t audio_app_asrc_intermediate_overflow_count_ba( void )
{
    return s_asrc[ASRC_ENGINE_BA].dbg_intermediate_overflow;
}
uint32_t audio_app_asrc_intermediate_underrun_count_ba( void )
{
    return s_asrc[ASRC_ENGINE_BA].dbg_intermediate_underrun;
}
#endif

#if APP_ASRC_MEAS
/* B->A mirrors of the A->B freeze/fill forensics the MEAS dump header reports.
 *
 * They exist because a MEAS_DIR_BA capture measures the B->A engine, but the header used the _ab
 * getters unconditionally -- so every preset-23 file recorded the OTHER leg's frozen step, constant
 * across captures and impossible to reconcile with the spectrum. Read-only, cold path.
 *
 * TWO gates, both necessary: APP_B_ROUTE_USES_BA because ASRC_ENGINE_COUNT is 1 without the B->A
 * engine, so s_asrc[ASRC_ENGINE_BA] would be out of bounds rather than merely unused; and
 * APP_ASRC_MEAS because freeze_step / freeze_base_step / ff_frozen_ratio are themselves MEAS-only
 * members of asrc_t. Dropping the second gate compiles everywhere except the shipping BIDIR image.
 *
 * The step-delta knob (*ax 00) is deliberately NOT mirrored: it perturbs the A->B step only, so its
 * mode/req stay single-valued and the header keeps reporting them as-is. */
float    audio_app_asrc_get_freeze_base_step_ba( void ) { return s_asrc[ASRC_ENGINE_BA].freeze_base_step; }
float    audio_app_asrc_get_freeze_step_ba( void )      { return s_asrc[ASRC_ENGINE_BA].freeze_step; }
uint32_t audio_app_asrc_get_fill_ba( void )             { return s_asrc[ASRC_ENGINE_BA].wr - s_asrc[ASRC_ENGINE_BA].rd; }
float    audio_app_asrc_get_ratio_live_ba( void )       { return s_asrc[ASRC_ENGINE_BA].ratio; }
float    audio_app_asrc_get_ff_frozen_ratio_ba( void )  { return s_asrc[ASRC_ENGINE_BA].ff_frozen_ratio; }
#endif /* APP_ASRC_MEAS */
#endif

// Trailing `fe=` field of an AB/BA telemetry line: which fixed decimator, if any, runs ahead
// of the resampler in that direction. `fe=direct` means none -- the poly stage resamples the
// full band and its cutoff is ASRC_POLY_FC of the *input* rate, so any output whose Nyquist is
// below that cutoff aliases. `fe=/4` / `fe=/6` name the divider, and the intermediate ring's
// ovf/udf counters follow: those are the evidence that the stage is really in the path and
// keeping up, so the divider identity and the counters both stay in the line (a bare 0/1 flag
// would drop them). Printed as a continuation of the line above -- no leading newline.
//
// The divider is followed by a coefficient-set tag where it needs one (`fe=/4:12k`): /4 serves
// both 11.025 kHz and 12 kHz with different band edges, so the number alone cannot tell a
// correctly selected set from a silently wrong one. The tag is a SUFFIX on the divider token,
// which keeps `^/(\d+)` parses (tools/asrc/lowrate_sweep.py) working unchanged.
// `num` is 1 for every divider chain and 2 for the 48 -> 32 kHz AUDIO MODE front end, which is a
// 2/3 polyphase resampler rather than a decimator and prints as `fe=2/3:audio`.
static void asrc_dbg_print_frontend_field( uint32_t num, uint32_t den, const char* tag,
                                           uint32_t ovf, uint32_t udf )
{
    if( den == 1u )
    {
        printf(" fe=direct\n");
        return;
    }
    if( num != 1u )
    {
        // A rational front end prints its full ratio: `fe=2/3:audio`.  Not `fe=/3` with a tag,
        // because /3 already means the 48 -> 16 kHz decimator in every log ever captured, and not
        // `fe=3:2` either -- the notation for this front end is L=2/M=3, numerator first.
        printf(" fe=%lu/%lu%s ovf=%lu udf=%lu\n",
               (unsigned long)num, (unsigned long)den, ( tag != NULL ) ? tag : "",
               (unsigned long)ovf, (unsigned long)udf );
        return;
    }
    printf(" fe=/%lu%s ovf=%lu udf=%lu\n",
           (unsigned long)den, ( tag != NULL ) ? tag : "",
           (unsigned long)ovf, (unsigned long)udf );
}

// Per-direction wrappers: which of the divider and the counters even exist is a build-time
// question (the runtime selector, the fixed one-way integration preset, and the plain builds
// each expose a different subset), while the printed field is deliberately the same shape in
// all of them -- a build with no front-end capability at all reports `fe=direct`.
static void asrc_dbg_print_frontend_ab( void )
{
    // Only the runtime selector has more than one coefficient set per divider, so only it can
    // produce a non-empty tag; the fixed presets are unambiguous by construction.
#if APP_ASRC_RUNTIME_48K_TO_8
    const uint32_t num = asrc_audio_path_ab_fixed_rate_num();
    const uint32_t den = asrc_audio_path_ab_fixed_rate_den();
    const char* const tag = asrc_audio_path_frontend_tag();
#else
    const uint32_t num = (uint32_t)APP_ASRC_AB_FIXED_RATE_NUM;
    const uint32_t den = (uint32_t)APP_ASRC_AB_FIXED_RATE_DEN;
    const char* const tag = "";
#endif
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    asrc_dbg_print_frontend_field( num, den, tag,
                                   s_asrc[ASRC_ENGINE_AB].dbg_intermediate_overflow,
                                   s_asrc[ASRC_ENGINE_AB].dbg_intermediate_underrun );
#else
    asrc_dbg_print_frontend_field( num, den, tag, 0u, 0u );   // no intermediate ring here
#endif
}

#if APP_B_ROUTE_USES_BA
static void asrc_dbg_print_frontend_ba( void )
{
#if APP_ASRC_RUNTIME_48K_TO_8
    const uint32_t num = asrc_audio_path_ba_fixed_rate_num();
    const uint32_t den = asrc_audio_path_ba_fixed_rate_den();
    const char* const tag = asrc_audio_path_frontend_tag();
#else
    const uint32_t num = (uint32_t)APP_ASRC_BA_FIXED_RATE_NUM;
    const uint32_t den = (uint32_t)APP_ASRC_BA_FIXED_RATE_DEN;
    const char* const tag = "";
#endif
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    asrc_dbg_print_frontend_field( num, den, tag,
                                   s_asrc[ASRC_ENGINE_BA].dbg_intermediate_overflow,
                                   s_asrc[ASRC_ENGINE_BA].dbg_intermediate_underrun );
#else
    asrc_dbg_print_frontend_field( num, den, tag, 0u, 0u );
#endif
}
#endif

// Telemetry line(s), printed with the 2 s TDM report. fsA_hz/fsB_hz are the measured
// per-domain block rates (from main.c's block-count deltas). Prints fill, resample step,
// and peak asrc_pull time per direction; clears the peak for the next window.
// ab is consumed by the B ISR (out = fs_B); ba is consumed by the A ISR (out = fs_A).
// Each line ends with `fe=` (see above), which is why asrc_audio_path_dbg_print() no longer
// emits the separate "ASRCpath <dir> front-end:" pair: the state now sits next to the engine
// it belongs to, one line per direction instead of two.
void audio_app_asrc_dbg_print( uint32_t fsA_hz, uint32_t fsB_hz )
{
    const uint32_t p_ab_ticks = s_asrc[ASRC_ENGINE_AB].dbg_pull_ticks_max; s_asrc[ASRC_ENGINE_AB].dbg_pull_ticks_max = 0u;
    const uint32_t p_ab = nora_high_res_timer_count_to_us_x10( p_ab_ticks );
#if APP_ASRC_48K_TO_8_INTEGRATION
    // The ovf/udf counters this line used to spell out as decim_ovf=/decim_udf= now arrive in
    // the common `fe=` field below, so both presets report the front end the same way.
    printf("[%s x%uch]AB fill=%lu/%lu intermediate_8k_frames step=%.7f ff=%.7f pull=%lu.%luus "
           "fs:in=%luHz out=%luHz",
           ASRC_KERNEL_NAME, (unsigned)ASRC_CH,
           (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_fill, (unsigned long)ASRC_FIFO_FRAMES,
           (double)s_asrc[ASRC_ENGINE_AB].dbg_step, (double)s_asrc[ASRC_ENGINE_AB].ratio,
           (unsigned long)(p_ab / 10u), (unsigned long)(p_ab % 10u),
           (unsigned long)(fsA_hz / 6u), (unsigned long)fsB_hz);
    asrc_dbg_print_frontend_ab();
#else
    /*
     * hr = fmin - R: the worst pull's HEADROOM in frames, which is what the acceptance criterion
     * cares about and the only form that reads the same on FIFO=64 and FIFO=128 (T2). It replaces
     * `fill=`/`fmin=/R=`: `fill` was one asynchronous sample per print window and `fmin`/`R` had to
     * be subtracted by eye -- the merged-main run sat at hr=0 and nobody could see it. Signed on
     * purpose: hr<0 is the starving case. set= keeps its trailing '!' (setpoint clamped), and
     * starve carries '!J' when the ring provably cannot reach starve=0 at this ratio.
     * Rates, R, J and Jmax now print once per ratio-lock instead (see asrc_apply_ratio).
     * [internal] design_brief_2026-08-27_telemetry_derived_thresholds_and_diet.md T2/P1.
     */
    printf("[%s x%uch]AB hr=%ld set=%lu%s step=%.5f pull=%lu.%luus drop=%lu starve=%lu%s",
           ASRC_KERNEL_NAME, (unsigned)ASRC_CH,
           (long)( (int32_t)s_asrc[ASRC_ENGINE_AB].dbg_fill_min - (int32_t)s_asrc[ASRC_ENGINE_AB].dbg_R ),
           (unsigned long)s_asrc[ASRC_ENGINE_AB].fill_target, s_asrc[ASRC_ENGINE_AB].fill_target_capped ? "!" : "",
           (double)s_asrc[ASRC_ENGINE_AB].dbg_step,
           (unsigned long)(p_ab / 10u), (unsigned long)(p_ab % 10u),
           (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_guard_drops,
           (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_starve_frames,
           ( (uint32_t)ASRC_FILL_JITTER > (uint32_t)s_asrc[ASRC_ENGINE_AB].dbg_jmax ) ? "!J" : "");
    (void)fsA_hz; (void)fsB_hz;
    s_asrc[ASRC_ENGINE_AB].dbg_fill_min = 0xFFFFu;   // min-hold restarts each print window
    asrc_dbg_print_frontend_ab();
#endif
#if APP_B_ROUTE_USES_BA
    const uint32_t p_ba_ticks = s_asrc[ASRC_ENGINE_BA].dbg_pull_ticks_max; s_asrc[ASRC_ENGINE_BA].dbg_pull_ticks_max = 0u;
    const uint32_t p_ba = nora_high_res_timer_count_to_us_x10( p_ba_ticks );
    printf("[%s x%uch]BA hr=%ld set=%lu%s step=%.5f pull=%lu.%luus drop=%lu starve=%lu%s",
           ASRC_KERNEL_NAME, (unsigned)ASRC_CH,
           (long)( (int32_t)s_asrc[ASRC_ENGINE_BA].dbg_fill_min - (int32_t)s_asrc[ASRC_ENGINE_BA].dbg_R ),
           (unsigned long)s_asrc[ASRC_ENGINE_BA].fill_target, s_asrc[ASRC_ENGINE_BA].fill_target_capped ? "!" : "",
           (double)s_asrc[ASRC_ENGINE_BA].dbg_step,
           (unsigned long)(p_ba / 10u), (unsigned long)(p_ba % 10u),
           (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_guard_drops,
           (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_starve_frames,
           ( (uint32_t)ASRC_FILL_JITTER > (uint32_t)s_asrc[ASRC_ENGINE_BA].dbg_jmax ) ? "!J" : "");
    s_asrc[ASRC_ENGINE_BA].dbg_fill_min = 0xFFFFu;   // min-hold restarts each print window
    asrc_dbg_print_frontend_ba();
#endif
}

#endif // APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC
