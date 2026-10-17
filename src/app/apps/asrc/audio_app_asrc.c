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
#if APP_ASRC_MEAS || APP_ASRC_QUIET_TRACK
#include "timer_app.h"        // P1 + two-stage FF handover: wrap-safe millisecond event timebase
#endif

#if APP_ASRC_FULL_IIR_48_TO_32
#include "asrc_full_iir_48_to_32.h"   // trial 48->32 anti-alias stage: `fe=` naming + its own line
#endif

#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC

#include <stdint.h>
#include <stdio.h>                     // printf (telemetry)
#include <math.h>                      // lrintf
#include "nora_high_res_timer.h"  // read-only: time asrc_pull
#if APP_ASRC_MEAS
#include "asrc_clock_control.h"   // P0 step 5: leg-B pull deferral (*af08)
#endif
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
// The consumer drains a whole block inside ONE pull, and the producer does not advance wr during
// it.  At EQUAL rates that is a priority fact (both TDM DMA ISRs sit at PRIO_TDM_DMA, so neither
// preempts the other).  At MIXED rates it is not -- asrc_audio_path_apply_isr_priorities() makes
// the shorter-deadline leg higher priority, so the producer CAN preempt the pull of the slower leg
// -- but it is still the right bound, because a block that has not been captured yet cannot be
// published early: measured at 32 -> 12 kHz, no producer block landed inside the starving pull.
// Do not "relax" R on the strength of the priority relation.  So the
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
// Fix: derive the setpoint from the RATIO at ratio-lock instead of hardcoding FIFO/2. The servo is
// purely proportional about the setpoint, so any value holds; the setpoint has to satisfy
//   R + one producer block + 1 <= setpoint   (asrc_fill_law, the LOWER reserve)
//   setpoint + one producer block + jitter <= the producer overflow guard (ASRC_FIFO_FRAMES-4)
// Rates whose requirement fits under FIFO/2 (>= ~23 kHz against a 48 kHz peer, and every
// up-conversion) keep the calibrated 64-frame operating point byte-exactly.
//
// ASRC_FILL_JITTER now serves the UPPER bound only.  Until 2026-09-12 it was also the lower slack
// (setpoint = R + JITTER), which is the bug asrc_fill_law() replaces: 4 frames -- and even the *ar
// gate's 8 -- is less than the one producer block a pull-start fill can lose when the block phase
// slips.  See the law's comment for the q-dependence and the 32 -> 12 kHz measurement.
#ifndef ASRC_FILL_JITTER
#define ASRC_FILL_JITTER      (4u)   // UPPER-bound headroom only, above the setpoint (see the law)
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
 * THE LOWER-SIDE RESERVE -- one law, and the pair gate and the setpoint are both its consumers.
 *
 * A pull needs R(step) frames of look-ahead at its START (see the setpoint block above).  The ring's
 * UPPER bound has always reserved a whole producer block for that: ASRC_FILL_TARGET_MAX =
 * FIFO-4-BLOCK-JITTER.  The LOWER bound reserved ASRC_FILL_JITTER = 4, and this gate asked for 8.
 *
 * 8 is BLOCK/2, and BLOCK/2 is the amplitude of the pull-start fill sawtooth only while the
 * producer/consumer block phase is DENSE.  That is the regime the 100-pair sweep of 2026-08-24
 * measured -- 48 <-> 44.1 kHz is 147/160, i.e. 160 phase classes, and its worst excursion was 5-6.
 *
 * It is not what a pull sees when the phase is COARSE.  The producer publishes wr in whole
 * APP_BLOCK_FRAMES blocks and cannot publish one during a pull, so with the step equal to p/q in
 * lowest terms the pull-start fill takes exactly q values, spaced BLOCK/q, whose MEAN the servo
 * holds at the setpoint:
 *
 *     steady classes    = set +/- (BLOCK/2) * (q-1)/q
 *
 * and when the block phase slips by one producer block, the class whose phase is nearest zero --
 * necessarily the one currently at the TOP -- loses a whole block:
 *
 *     worst pull-start  = set - BLOCK + (BLOCK/2) * (q-1)/q
 *
 * That is set-BLOCK at q = 1 (an exact integer step) and only approaches set-BLOCK/2 as q grows.
 * So the measured 8 is the LIMIT of this expression, not its worst case, and the pairs it
 * under-serves are the LOW-ORDER RATIONALS.  MEASURED on AK512 (FIFO 128, block 16) at
 * 32 -> 12 kHz = 8/3, q = 3: predicted 64-10.67 = 53.3, observed pull-start fill 54 against a
 * window need of 56 -> starve, in bursts of ~2 minutes every ~10 minutes as the applied step
 * wanders across the exact 8/3 and freezes and unfreezes the phase.  Raising the lower reserve to
 * one block took the same board to 0 starve over 45 minutes with the worst pull-start fill at 63,
 * where this law predicts 74-10.67 = 63.3.
 * [internal] ak512_32to12_starve_analysis_2026-09-11.md.
 *
 * So the lower reserve is ONE PRODUCER BLOCK plus one frame.  It covers q = 1 as well, which is why
 * there is no per-q arithmetic at runtime.  Overridable per profile like ASRC_FILL_JITTER, but note
 * it is DERIVED, not measured: a profile that lowers it is claiming its producer cannot defer a
 * whole block, which is a claim about the transport, not about a servo excursion.
 */
#ifdef ASRC_FILL_SLACK_REQUIRED
#error "ASRC_FILL_SLACK_REQUIRED is retired: the lower reserve is ASRC_FILL_BLOCK_RESERVE (one producer block + 1) and the exact-step exemption it guarded is deleted -- see asrc_fill_law()."
#endif
#ifndef ASRC_FILL_BLOCK_RESERVE
#define ASRC_FILL_BLOCK_RESERVE  ( (uint32_t)APP_BLOCK_FRAMES + 1u )
#endif

/*
 * R FROM THE NOMINAL RATES, IN INTEGERS, WITH +1 -- not from a measured float ratio.
 *
 * floor(step*(BLOCK-1)) evaluated on the LOCK-TIME measured ratio is wrong in both directions at
 * once: the measured value sits a few ppm on EITHER side of the configured rational, while the step
 * the pull actually applies is the servo-trimmed one and moves ABOVE it.  At 32 -> 12 kHz the
 * product 15 * 8/3 = 40 is exactly an integer, so a lock 7.6 ppm below nominal reads floor = 39
 * (R = 55) where the applied step needs 41 (R = 57) -- and the next lock of the same pair printed
 * R = 56.  A quantity that changes with the ppm of one measurement is not a bound.
 *
 * floor(nominal) + 1 is ceil() where the product is fractional and ceil+1 where it is an integer,
 * which is exactly the frame the servo's upward trim needs, with no exception case to carve out.
 * It does NOT claim to cover the whole ASRC_STEP_HI = 1.05 clamp (that would be ceil(look*0.05)+1):
 * a servo saturated at +5 % means the configured pair is wrong, and fill is not the counter that
 * reports that.
 *
 * Integer throughout, same shape as asrc_burst_ratio_fits() above, so the gate and the setpoint
 * cannot drift apart -- that drift is what let a setpoint of 64 be accepted for a pull needing 57.
 */
typedef struct
{
    uint16_t look_safe;      /* floor(nominal step * (BLOCK-1)) + 1                        */
    uint16_t r_safe;         /* look_safe + ASRC_POLY_AHEAD + 1 -- what one pull needs      */
    uint16_t set_required;   /* r_safe + ASRC_FILL_BLOCK_RESERVE                            */
    uint16_t set;            /* max(set_required, ASRC_FILL_TARGET) -- the setpoint to use   */
    bool     fits;           /* set <= ASRC_FILL_TARGET_MAX: the ring can actually hold it   */
} asrc_fill_law_t;

static bool asrc_fill_law( uint32_t fs_in_hz, uint32_t num, uint32_t den, uint32_t fs_out_hz,
                           asrc_fill_law_t* out )
{
    uint32_t eff_num = 0u, eff_den = 0u;
    if( ( out == NULL ) ||
        !asrc_step_num_den( fs_in_hz, num, den, fs_out_hz, &eff_num, &eff_den ) )
    {
        return false;   /* not known yet -- callers treat "unknown" as acceptable */
    }
    /* No step <= 1 shortcut, unlike the two predicates this replaces: the reserve is a producer
     * BLOCK, and an up-conversion's pull-start fill loses one of those just the same.  The
     * expression needs no special case -- at 12 -> 32 kHz it simply yields look_safe = 6, and the
     * setpoint then lands on ASRC_FILL_TARGET anyway. */
    const uint32_t look = ( ( eff_num * ( (uint32_t)APP_BLOCK_FRAMES - 1u ) ) / eff_den ) + 1u;
    const uint32_t r    = look + ASRC_POLY_AHEAD + 1u;
    const uint32_t req  = r + ASRC_FILL_BLOCK_RESERVE;
    const uint32_t set  = ( req < (uint32_t)ASRC_FILL_TARGET ) ? (uint32_t)ASRC_FILL_TARGET : req;

    out->look_safe    = (uint16_t)look;
    out->r_safe       = (uint16_t)r;
    out->set_required = (uint16_t)req;
    out->set          = (uint16_t)set;
    out->fits         = ( set <= (uint32_t)ASRC_FILL_TARGET_MAX );
    return true;
}

/*
 * Soft bound, and the one that actually bites: can this ring give this step its lower reserve?
 *
 * It is NOT a clamp-and-accept.  asrc_set_fill_target() still clamps to ASRC_FILL_TARGET_MAX as a
 * last resort, because it also runs for pairs that never passed through this gate (boot defaults),
 * but a pair whose required setpoint does not fit is REFUSED here: a clamped setpoint leaves fill
 * under what a pull needs, the tail of every block zero-fills, rd is held, and it does not converge
 * with time -- permanently, with sat and miss both staying 0.  That is why this needs a gate and
 * not a counter.
 *
 * THE EXACT-STEP EXEMPTION THAT USED TO LIVE HERE IS GONE, and deleting it is part of this law
 * rather than a separate cleanup.  It waved a pair through on 4 frames of slack whenever
 * fs_in*(BLOCK-1) was a multiple of fs_out, reasoning that floor == ceil so per-block consumption is
 * constant.  Both halves fail: the premise is about the NOMINAL rates while the runtime step is two
 * independent clocks and is never exactly rational, and an exact step is q = 1, which by the law
 * above is the DEEPEST excursion (set - BLOCK), not the safest -- it exempted precisely the pairs
 * that need the most reserve.  Nothing depends on it: the step-3 and step-4 rows it used to admit
 * are admitted here on a real one-block reserve instead (see the delta report in
 * [internal tool] a1_fill_reserve_gate_delta.py).
 */
static bool asrc_fill_slack_fits( uint32_t fs_in_hz, uint32_t num,
                                  uint32_t den, uint32_t fs_out_hz )
{
    asrc_fill_law_t law;
    if( !asrc_fill_law( fs_in_hz, num, den, fs_out_hz, &law ) ) { return true; }
    return law.fits;
}

/*
 * ROUTE-AWARE ANTI-ALIAS POLICY for direct down-conversions -- NOT a starvation rule.
 *
 * Two different questions, deliberately answered by two different pieces of code:
 *
 *   asrc_fill_law()          -- is this rate pair mathematically starve-safe in this ring?
 *   this policy              -- may this pair run its DIRECT route, i.e. with no band limiting?
 *
 * MEASURED, 2026-09-15 (tools/asrc/asrc_direct_route_policy_matrix.py, the same authority as
 * the 45-pair spectral study, Q31 on, protected band 0 .. 0.8125 * FN_out): the DIRECT route of
 * every pair listed below reads a worst in-band unwanted component of 0.00 dBc -- full scale.
 * The rear resampler's cutoff is 0.465 of ITS OWN input rate, so at these steps it passes
 * content far above the output Nyquist and folds it into the protected band at unity gain.  The
 * rear phase-interpolation spur is -129.6 .. -137.5 dBc across the six and decides none of them,
 * which also means L cannot help: L moves the spur only.  So the refusal is about the AUDIO and
 * for a reason that does not expire.
 *
 * THAT IS WHY THIS IS A POLICY AND NO LONGER A FENCE.  It began (2026-09-12) as a temporary hold
 * on the pre-A1 qualified surface, with an exit condition -- qualification of step > 2.07 -- that
 * has nothing to do with band limiting.  The A1 history is kept below because the TABLE is still
 * exactly the set A1 newly admitted and test_asrc_rate_plan.py still asserts that.  What changed
 * is the reason, and with it the exit condition: when step > 2.07 is qualified these six pairs
 * still cannot run DIRECT.  What that qualification unlocks is a FILTERED route for them, which
 * this predicate already allows and always did.
 *
 * PHYSICAL PAIR != IMPLEMENTATION ROUTE, and the num/den test in the predicate below is where
 * that distinction lives.  Filtered routes that exist in this tree today: 44.1 -> 16 kHz as
 * /2 + 1:1 (prototype; the as-built chain reads -114.38 dBc), and 48 -> 11.025 kHz / 96 -> 22.05
 * kHz as /4 from the divider tables in asrc_audio_path_frontend_plan().  22.05 -> 8, 32 -> 11.025
 * and 44.1 -> 12 kHz have no front-end row at all, so for those three this policy is the whole
 * answer today.
 *
 * A1 raised the lower reserve to one producer block, which fixed 32 -> 12 kHz.  A side effect is
 * that the setpoint now RISES with R instead of being pinned at R + ASRC_FILL_JITTER, and six
 * AK512 direct pairs that the pre-A1 gate refused become starve-safe under the new law.  They very
 * probably are servable -- the law says so and the law is now the only thing that judges starving.
 * But A1's purpose is to FIX STARVE ON THE PAIRS THIS PRODUCT ALREADY SUPPORTS, not to widen the
 * supported rate set, and two of the six (22.05 -> 8 and 32 -> 11.025 kHz) do not even raise the
 * "this pair needs an anti-alias front end and this build has none" notice, because that notice
 * only covers 48 and 96 kHz producers.  Admitting them here would attach an unevaluated
 * rate-coverage change to a starve fix.
 *
 * So the fence holds the pre-A1 qualified surface while A1 itself is qualified.  The refusal says
 * NOT QUALIFIED, not unsafe, and the two must not be conflated in a log either.
 *
 * THE OPEN QUESTION IS NOW ANSWERED, so this block is NOT deleted by the step > 2.07
 * qualification.  It used to say: at that point either these six are measured and admitted, or
 * they are refused for a reason of their own (band limiting).  It is the second -- measured on
 * host 2026-09-15, see the paragraph above.  What the step > 2.07 qualification still gates is
 * whether a FILTERED route for these pairs may be admitted to the product surface; it does not
 * gate this block.
 *
 * THE PAIR COUNTS, because an earlier version of this comment got them wrong twice and both
 * wrong versions cited the generator.  A1 affects 17 directed AK512 pairs
 * ([internal tool] a1_fill_reserve_gate_delta.py is the authority), split into two groups that are
 * not interchangeable:
 *   - the SIX listed below, which A1 newly made starve-safe: the subject of this block, now
 *     dispositioned as above.
 *   - ELEVEN more whose verdict A1 leaves accepted and whose setpoint it moves: a regression
 *     follow-up, and never a blocker for this block.
 * "15" appears in neither group and was never the generator's number.
 *
 * Dated so the lineage stays visible: fence added 2026-09-12, restated as policy 2026-09-15.
 *
 * IT IS A TABLE, NOT THE OLD PREDICATE.  Copying the pre-A1 arithmetic in here would resurrect the
 * gate A1 exists to replace, and would state it as a safety rule again.  What is frozen is a
 * PRODUCT SURFACE, so it is written as the list of pairs that surface does not contain.  The list
 * was computed once, from the pre-A1 gate and the A1 law over the whole `*ar` rate menu, by
 * [internal tool] a1_fill_reserve_gate_delta.py; test_asrc_rate_plan.py re-derives it on the host and
 * asserts that the post-A1 accepted set equals the pre-A1 one exactly, so the list cannot drift
 * from its own definition without a test failure.
 */
/* AHEAD 17 is admitted ONLY together with APP_ASRC_REAR_POLY_M34 (M34 rear, 2026-09-30): the list
 * below was re-derived at AHEAD 17 with [internal tool] a1_fill_reserve_gate_delta.py and is the same
 * six pairs (the law only raises R_safe and the setpoint by 2 frames on 24 down-conversion pairs).
 * Any other AHEAD still switches the fence off, and the family assert below still fires. */
#if ( ASRC_FIFO_FRAMES == 128u ) && ( APP_BLOCK_FRAMES == 16 ) && \
    ( ( ASRC_POLY_AHEAD == 15u ) || ( ( ASRC_POLY_AHEAD == 17u ) && APP_ASRC_REAR_POLY_M34 ) ) && \
    ( ASRC_FILL_JITTER == 4u ) && !defined( APP_ASRC_FILL_TARGET )
/*
 * ACTIVE ONLY ON THE GEOMETRY THE LIST WAS DERIVED FOR.  Which pairs A1 newly admits is a function
 * of FIFO depth, block length, look-ahead and the setpoint floor; on any other ring the list
 * describes nothing, so the fence switches OFF rather than applying a table that no longer follows
 * from anything.  That is also why AK128 is unaffected: its own ring refuses all six by the law
 * (checked -- see the AK128 note in asrc_app_build_config.h), so there is nothing to freeze there.
 */
#define ASRC_DIRECT_ROUTE_POLICY_ACTIVE  (1)

static const struct
{
    uint32_t fs_in_hz;
    uint32_t fs_out_hz;
} s_asrc_unsafe_direct_routes[] =
{
    { 22050u,  8000u },   /* step 2.756 -- pre-A1: slack 7, refused */
    { 32000u, 11025u },   /* step 2.902 -- pre-A1: slack 5, refused */
    { 44100u, 12000u },   /* step 3.675 -- pre-A1: slack 4, refused */
    { 44100u, 16000u },   /* step 2.756 -- pre-A1: slack 7, refused */
    { 48000u, 11025u },   /* step 4.354 -- pre-A1: slack 4, refused */
    { 96000u, 22050u },   /* step 4.354 -- pre-A1: slack 4, refused; a filtered /4 row EXISTS and
                           * stays accepted -- see the num/den condition below */
};
#else
#define ASRC_DIRECT_ROUTE_POLICY_ACTIVE  (0)
#endif

/*
 * A SILENTLY INACTIVE FENCE IS THE ONE FAILURE THIS BLOCK CANNOT TOLERATE: it would widen the
 * qualified rate set by six pairs and nothing would say so -- the build succeeds either way,
 * because both arms of the #if above compile.  So the images that are SUPPOSED to run the ring the
 * list was derived for are required to have the fence on, and moving ASRC_POLY_AHEAD,
 * ASRC_FILL_JITTER, the FIFO depth, the block length or the setpoint floor on one of them without
 * revisiting the list is a build error rather than a quiet re-widening.
 *
 * ARMED ON PRESET IDENTITY, NOT ON PART OF THE GEOMETRY, and that is the whole point.  This used
 * to read `#if (ASRC_FIFO_FRAMES == 128u) && (APP_BLOCK_FRAMES == 16)` on the stated premise that
 * "this FIFO depth and block length identify it".  They do not: eight ASRC presets share FIFO 128
 * / block 16 while sitting outside the fence's own five-term test above (they reach
 * ASRC_POLY_AHEAD == 16 through the default ASRC_POLY_M of 32, so they correctly get fence = 0),
 * and were rejected here anyway -- ASRC dsPIC BI, dsPIC light, codec A->B / B->A only, the SPI3/4
 * bank, the decimator and 48->8 measurement profiles, and the M32 headroom candidate all failed to
 * compile.  The old two-term condition is a strict SUPERSET of the fence's five-term condition:
 * true whenever the fence is active, but also true on those eight presets where it correctly is
 * not, so the assert fired in exactly that gap.  The premise came from
 * [internal tool] a1_fill_reserve_gate_delta.py, which models two geometries (AK512 FIFO 128 / block
 * 16 and AK128 FIFO 64 / block 8) and inside that pair FIFO really does identify the ring; in a
 * tree with 17 presets it does not.
 *
 * No purely geometry-derived arming condition fixes this.  The fence's own #if already IS the
 * exact five-term qualified-geometry test, so an arming condition equal to it -- or any stricter
 * narrowing of it -- always implies the fence and can therefore never actually fire (a tautology,
 * useless as a check); anything looser, like the old two-term condition, is again a superset that
 * also catches presets legitimately outside the fence, which is this same bug.  The arming
 * condition therefore has to come from something the fence does not itself test, and preset
 * identity is that something: APP_ASRC_RING_IS_AK512_M30_FAMILY is defined by the block in
 * asrc_app_build_config.h that already enumerates the eight members, so the two cannot drift.
 */
#if APP_ASRC_RING_IS_AK512_M30_FAMILY
_Static_assert( ASRC_DIRECT_ROUTE_POLICY_ACTIVE == 1,
                "the direct-route anti-alias policy must be ACTIVE on this image: it is one of the AK512 "
                "M30 product presets, whose pair list was derived for FIFO 128 / block 16 / "
                "AHEAD 15 / JITTER 4 / setpoint floor FIFO/2, and one of those moved.  (M28 used "
                "to be the way to move AHEAD to 14 here; it is now retired with its own #error in "
                "asrc_app_build_config.h, so this is a ring change, not that.)  Re-derive the list "
                "with [internal tool] a1_fill_reserve_gate_delta.py for the ring you actually want "
                "instead of letting the fence switch itself off" );
#endif

/*
 * One direction: may this pair run the route this plan selected?
 *
 * DIRECT PATH ONLY (num == den == 1), and that is the policy, not an exemption bolted onto it.
 * What the table forbids is a ROUTE -- a down-conversion with nothing in front of the rear
 * resampler -- so a front-end row for the same physical pair is a different object: it band
 * limits, it produces a different effective step, and it is judged on its own spectral evidence
 * (96 -> 22.05 kHz composed /4 and the 44.1 -> 16 kHz /2 + 1:1 prototype are both that case).
 * Refusing on the physical rates alone would remove a qualified configuration.
 */
static bool asrc_direct_route_is_allowed( uint32_t fs_in_hz, uint32_t num,
                                                uint32_t den, uint32_t fs_out_hz )
{
#if ASRC_DIRECT_ROUTE_POLICY_ACTIVE
    if( ( num != 1u ) || ( den != 1u ) ) { return true; }   /* a filtered row is its own pair */
    for( uint8_t i = 0u;
         i < (uint8_t)( sizeof( s_asrc_unsafe_direct_routes ) / sizeof( s_asrc_unsafe_direct_routes[0] ) );
         i++ )
    {
        if( ( s_asrc_unsafe_direct_routes[i].fs_in_hz  == fs_in_hz ) &&
            ( s_asrc_unsafe_direct_routes[i].fs_out_hz == fs_out_hz ) )
        {
            return false;
        }
    }
    return true;
#else
    (void)fs_in_hz; (void)num; (void)den; (void)fs_out_hz;
    return true;
#endif
}

#if !( APP_ASRC_RUNTIME_48K_TO_8 || APP_ASRC_48K_TO_8_INTEGRATION )
/*
 * F1(b), 2026-09-04: does this pair contain a down-conversion that NEEDS the anti-alias front end?
 *
 * WARNING-ONLY MIRROR, and it says so because a mirror of a rate table is exactly the kind of
 * duplicate that drifts.  The AUTHORITY is asrc_audio_path_frontend_plan()'s table.
 *
 * Why a mirror is needed at all, stated precisely (the first write-up of this said "the plan
 * function is compiled out", which is NOT true -- corrected 2026-09-04):
 *   - asrc_audio_path_frontend_plan() itself is always compiled.  What is compiled out in this
 *     build is the front-end IMPLEMENTATION, and with it the CALL: the pair gate below only asks
 *     the plan under #if APP_ASRC_RUNTIME_48K_TO_8 || APP_ASRC_48K_TO_8_INTEGRATION, because a
 *     plan for stages that do not exist cannot be applied.
 *   - and the table could not answer this question even if it were asked, because several of its
 *     rows are themselves conditional on APP_ASRC_RUNTIME_48K_TO_8 (the 2/3 chain rows: 48 -> 32
 *     and the composed 96 -> 32).  In a front-end-less build those rows fall through to num 1 /
 *     den 1 -- the same answer a pair that needs nothing gets.  The table encodes "what this
 *     build will put in front of the resampler", so it structurally cannot express "a stage is
 *     NEEDED here and this build has none".  That distinction is what this mirror carries.
 *
 * Nothing here refuses anything.  Some presets measure the unfiltered path ON PURPOSE (the
 * HEADROOM studies do), and a MEAS image that could not reach the bad case would be less useful,
 * not safer.  What must not happen is measuring it BY ACCIDENT and recording the number as if a
 * front end had been in the path.  So this prints, once per pair commit, and the log carries the
 * sentence instead of the reader having to know that `fe=direct` is ambiguous.
 *
 * DRIFT IS NOT HYPOTHETICAL: this mirror had already drifted by the time it was first reviewed --
 * it was missing 96 -> 44.1 and 96 -> 48 kHz, both of which took a den 2 pre-stage-only row on
 * 2026-09-03.  When a row is added to the plan table, add it here.  The rate menu is per producer,
 * because the two producer legs do not cover the same set: 44.1 kHz has NO 48 kHz row (declined on
 * purpose -- its fold is inaudible) but it DOES have a 96 kHz one (its 25950 Hz fold edge is not).
 */
static bool asrc_pair_wants_absent_frontend( uint32_t rate_a_hz, uint32_t rate_b_hz,
                                             uint32_t* producer_hz, uint32_t* consumer_hz )
{
    uint32_t hi = ( rate_a_hz > rate_b_hz ) ? rate_a_hz : rate_b_hz;
    uint32_t lo = ( rate_a_hz > rate_b_hz ) ? rate_b_hz : rate_a_hz;
    const bool low_rate_row = ( lo == 8000u ) || ( lo == 11025u ) || ( lo == 12000u ) ||
                              ( lo == 16000u ) || ( lo == 22050u ) || ( lo == 24000u ) ||
                              ( lo == 32000u );
    bool wants;
    if( hi == 48000u )
    {
        wants = low_rate_row;                       // 48 kHz table: 8 .. 32 kHz
    }
    else if( hi == 96000u )
    {
        // 96 kHz table: the same low rates PLUS the two pre-stage-only rows added 2026-09-03.
        wants = low_rate_row || ( lo == 44100u ) || ( lo == 48000u );
    }
    else
    {
        wants = false;                              // no producer leg has rows for this rate
    }
    if( !wants ) { return false; }
    if( producer_hz != NULL ) { *producer_hz = hi; }
    if( consumer_hz != NULL ) { *consumer_hz = lo; }
    return true;
}
#endif  /* no front end compiled in -- the helper has no other caller */

bool audio_app_asrc_rate_pair_is_supported( uint32_t rate_a_hz, uint32_t rate_b_hz,
                                            const char** reason )
{
#if defined(APP_ASRC_AK128_FIXED_48K_PAIR) && APP_ASRC_AK128_FIXED_48K_PAIR
    /* This image intentionally ships one cell only.  Keep this gate callable
     * by every reconfiguration path (not just *ar), so an attempted change is
     * refused before transport reset and cannot leave a half-reconfigured
     * stream running. */
    if( ( rate_a_hz == 48000u ) && ( rate_b_hz == 48000u ) )
    {
        return true;
    }
    if( reason != NULL )
    {
        *reason = "AK128 Bi-Codec is built for the 48 kHz <-> 48 kHz cell only";
    }
    return false;
#else
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

    /*
     * Refuse a pair that NEEDS a front end when the implementation this build compiled cannot
     * carry the ASRC logical width.  The narrow implementation is disqualified rather than used:
     * an anti-alias stage that filters fewer channels than the resampler converts is a MISSING
     * stage, and down-conversion without one is not correct at any channel count -- so serving the
     * pair anyway would be the "reduce the width until it fits" answer, wearing a different hat.
     *
     * Only pairs that ask for one are affected, which is why this is here and not an #error: a
     * unity ratio, an up-conversion, or any ratio the resampler serves directly needs no front end
     * and is untouched.  Checked before the burst and slack bounds so the reason names the real
     * cause instead of whatever those would have said about the direct step.
     */
    if( ( ( plan.den_ab != 1u ) || ( plan.num_ab != 1u )
#if APP_B_ROUTE_USES_BA
          || ( plan.den_ba != 1u ) || ( plan.num_ba != 1u )
#endif
        ) && !asrc_audio_path_frontend_can_serve_logical_width() )
    {
        if( reason != NULL )
        {
            *reason = "this pair needs the anti-alias front end, and the front-end implementation "
                      "in this build processes fewer channels than ASRC_CH (it is disqualified, "
                      "not narrowed)";
        }
        return false;
    }
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
                      "no room for one producer block above the look-ahead (it would starve)";
        }
        return false;
    }

    /*
     * QUALIFICATION, not safety, and LAST of the three so the two cannot be read as one.  Every
     * bound above says the ring cannot serve the pair; this one says the pair is starve-safe and
     * simply outside what this image has been qualified for.  Temporary -- see the fence.
     */
    if( !asrc_direct_route_is_allowed( rate_a_hz, plan.num_ab, plan.den_ab, rate_b_hz )
#if APP_B_ROUTE_USES_BA
        || !asrc_direct_route_is_allowed( rate_b_hz, plan.num_ba, plan.den_ba, rate_a_hz )
#endif
      )
    {
        if( reason != NULL )
        {
            *reason = "DIRECT route refused by the anti-alias policy: this down-conversion runs "
                      "with no band limiting, so its worst in-band unwanted component is 0 dBc "
                      "(host-measured 2026-09-15). NOT a starvation refusal, and NOT the physical "
                      "pair being refused -- a FILTERED route for the same pair is allowed. See "
                      "the direct-route policy in audio_app_asrc.c";
        }
        return false;
    }

#if !( APP_ASRC_RUNTIME_48K_TO_8 || APP_ASRC_48K_TO_8_INTEGRATION )
    /*
     * NO FRONT END IS COMPILED INTO THIS BUILD.  Say it here, where a human just asked for the
     * pair, instead of letting `fe=direct` in the telemetry carry two opposite meanings:
     * "no filter is needed" and "a filter is needed and is not there".  The pair is still
     * ALLOWED -- see asrc_pair_wants_absent_frontend() for why refusing would be wrong.
     *
     * LAST, after every gate has passed, and that placement is the point: the sentence says "the
     * pair is allowed and will run band-unlimited", which would be a LIE on any path that then
     * returns false.  Printing it before the burst/slack bounds produced an "allowed" line
     * immediately followed by a refusal (found in review, 2026-09-04).  A function whose whole
     * purpose is to stop a log from being misread must not be the thing that misleads.
     */
    {
        uint32_t producer_hz = 0u, consumer_hz = 0u;
        if( asrc_pair_wants_absent_frontend( rate_a_hz, rate_b_hz, &producer_hz, &consumer_hz ) )
        {
            printf( " ASRC front end: %lu -> %lu Hz NEEDS an anti-alias stage and this build has"
                    " NONE compiled in (APP_ASRC_RUNTIME_48K_TO_8=0). The pair is allowed and the"
                    " resampler will run it band-unlimited: `fe=direct` below therefore means"
                    " UNPROTECTED, not 'not needed'. Any alias / DR / THD+N number taken here is"
                    " the DIRECT path -- do not record it as a filtered one. Rebuild with"
                    " -Define APP_ASRC_RUNTIME_48K_TO_8=1 to measure the filtered path.\n",
                    (unsigned long)producer_hz, (unsigned long)consumer_hz );
        }
    }
#endif

    return true;
#endif
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

/* The two-stage FF handover exists in every MEAS image (float too) and wherever QUIET_TRACK is
 * compiled, because QUIET_TRACK only enters after it. */
#if APP_ASRC_MEAS || APP_ASRC_QUIET_TRACK
#define ASRC_FF_REFINE  (1)
#else
#define ASRC_FF_REFINE  (0)
#endif
#if ASRC_FF_REFINE
/* P0 two-stage FF handover, one per engine (p0r_handover()).  The clock control publishes the
 * 256-period FF at ratio lock and later hands the co-timed stage-2 ratio over through
 * audio_app_asrc_refine_ratio_*(); the engine's next pull adopts it. */
typedef struct
{
    volatile float   ratio_new;             /* stage-2 ratio, written before pending */
    volatile uint8_t pending;
    uint8_t          done;
    uint8_t          fill_seeded;
    float            fill_ema;              /* smoothed fill_used, the adopted setpoint */
    float            ratio_old, target_old; /* last handover, for ?af */
    uint32_t         done_ms;               /* GetTicks() at the handover */
#if APP_ASRC_MEAS
    uint32_t         at_ms;                 /* handover time after the P1 command */
#endif
} asrc_p0r_t;
#endif
#if APP_ASRC_QUIET_TRACK
/* P0 QUIET_TRACK state, one per engine (p0t_step()).  The preset selection (s_p0t_mode) is one
 * per image.  MEAS restarts the counters with each P1 epoch; a product image counts since boot. */
typedef struct
{
    uint8_t  active;
    uint16_t entry_ok;
    float    entry_fill0;                   /* fill EMA on the first pull of the entry window */
    float    entry_slope;                   /* last measured entry-window slope, frame/s */
    uint32_t slope_rejects;                 /* entry windows refused for slope */
    int32_t  entry_gap;                     /* applied - servo step on the entry pull, LSB */
    uint32_t entry_whole, entry_frac;       /* applied step on the entry pull */
    float    ratio;                         /* FF ratio at entry; a change ends the track */
    float    setp, acc, integ, tgt, e;
    int32_t  out;                           /* applied offset from the entry step, Q31 LSB */
    uint16_t n, rc;
    uint32_t updates, ramps, aborts, at_ms, aw;
} asrc_p0t_t;
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
    float             frac;    // fractional read phase [0,1) (consumer only; float arms + telemetry)
#if ASRC_SAMPLE_Q31
    uint32_t          frac_q31; // Q1.31 fractional phase; keeps sub-binary32 step motion in Q31 arms
#endif
    volatile float    ratio;   // live feed-forward in/out ratio (0 = not yet measured -> silence)
    float             corr_lpf;   // low-pass-filtered fill correction (slow drift trim, consumer)
    float             step_state; // slew-limited resample step actually applied (consumer)
#if ASRC_SAMPLE_Q31
    float             step_corr_state; // slew-limited correction relative to ratio; preserves sub-ULP motion
#endif
    float             step_smooth_st; // Q44: LPF state for the applied-step smoother (~1.1 Hz); strips
                                      // the producer limit cycle before it FMs the carrier. Q31: base. 0 = seed.
#if ASRC_SAMPLE_Q31
    float             step_smooth_corr; // correction-domain EMA; stays precise below one base-step ULP
#endif
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
     * The reserve law's own R (asrc_fill_law): floor(NOMINAL step*(BLOCK-1)) + 1 + AHEAD + 1, which
     * is what the setpoint is built from.  dbg_R above keeps its old meaning -- floor of the
     * LOCK-TIME measured ratio -- so `hr` stays comparable with every log recorded before this
     * change; the two differ by 1 to 2 frames and dbg_R is the optimistic one.  Both are printed on
     * the lock line so a log never has to guess which produced `set`.  Making `hr` itself honest is
     * a separate change (B1) and is deliberately not folded in here.
     */
    uint16_t          dbg_R_safe;
    uint8_t           fill_law_nominal;   // 1 = setpoint came from the nominal rates, 0 = from the
                                          // measured lock ratio (degraded; `?` on the lock line)
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
#if APP_ASRC_STAGE_PROFILE
    /*
     * Per-STAGE tick totals inside asrc_pull, peak-held over the print window (see
     * APP_ASRC_STAGE_PROFILE in asrc_app_config.h).  SUMMED over the block's output frames,
     * not peaked per frame, so coef+mac+conv from one block is comparable with THAT block's
     * dbg_pull_ticks_max -- a per-frame peak would not be.  Q31 arm only; the float arms leave
     * them at 0, and a 0 here means "not measured", never "free".
     */
    uint32_t          dbg_stg_coef_max;     // asrc_q31_phase_of + mchp_asrc_q31_blend_row
    uint32_t          dbg_stg_mac_max;      // mchp_asrc_q31_row16 (the FIR MACs)
    uint32_t          dbg_stg_conv_max;     // asrc_q31_to_slot over ASRC_CH
    /*
     * F1 sub-split.  ph+bl == coef by construction (same two probes plus one in the middle),
     * and coef is kept so this window stays comparable with the E1-era reports.  st is the
     * slot scatter, which lives OUTSIDE the poly arm and so is measured on every arm.
     */
    uint32_t          dbg_stg_ph_max;       // asrc_q31_phase_of + wbase + asrc_q31_wb
    uint32_t          dbg_stg_bl_max;       // mchp_asrc_q31_blend_row alone
    uint32_t          dbg_stg_st_max;       // d[s] = (s<ASRC_CH) ? out[s] : 0 scatter
    /*
     * MIN-hold of the same block totals, and it is not a curiosity: for a direction whose
     * pull runs in the LOWER-priority leg (at 96/32 that is A->B, in leg B at IPL 3), every
     * bucket total is WALL time -- leg A preempts inside it, so the MAX is the block that got
     * robbed hardest and `rest` goes hugely negative.  The MIN over the print window is the
     * block where that bucket's 16 short windows all escaped preemption, which does happen:
     * a bucket of b us inside a 500 us block with 3 leg-A blocks escapes with probability
     * ~(1-b/500)^3, so at ~19 blocks/s over 10 s the minimum is a clean CPU figure.  For the
     * direction that runs in the TOP-priority leg (B->A, leg A) nothing preempts it and MAX
     * is already CPU; the MIN is then just the least-work block and the two nearly agree.
     */
    uint32_t          dbg_stg_coef_min;
    uint32_t          dbg_stg_mac_min;
    uint32_t          dbg_stg_conv_min;
    uint32_t          dbg_stg_ph_min;
    uint32_t          dbg_stg_bl_min;
    uint32_t          dbg_stg_st_min;
    /*
     * How often the blended coefficient row would be BYTE-IDENTICAL to the previous output
     * frame's, i.e. (pq, wbq) unchanged.  Not an optimisation, a measurement: it sizes a memo
     * that would skip mchp_asrc_q31_blend_row entirely on a hit, which is bit-exact by
     * definition (same two rows, same weight -> same 30 words).  Expected to differ hugely by
     * direction: at 96->32 the step is ~3.0 so frac barely moves and pq should almost never
     * change; at 32->96 the step is ~1/3 so pq jumps ~42.7 of 128 every frame and never repeats.
     */
    uint32_t          dbg_memo_hit;
    /*
     * REGRESSION CHECK, and it must now read 0.  It was the proof-of-race counter for the
     * blended-row scratch when s_ceff_q31 was ONE array shared by both engines: written by
     * blend_row, read by row16, and at MIXED rates the two leg ISRs no longer share a
     * priority, so leg A preempted leg B between those two calls and leg B's dot product ran
     * against leg A's coefficient row.  It counted 4.51 per cent of A->B frames and exactly 0
     * on B->A, matching the priority relation.  s_ceff_q31 is per-engine as of 2026-09-06, so
     * nothing outside this engine can reach this buffer and a nonzero count means the split
     * was undone or a third caller appeared.  The tag lives in the buffer's own extra word
     * (ASRC_CEFF_OWNER_SLOT), stamped BEFORE blend_row -- so a preemption INSIDE the blend
     * would be caught too -- and checked before row16.  It never proved a clean run: a
     * preemption inside row16, which re-reads the row per channel, was never detectable, so
     * the old 4.51 per cent was a lower bound.
     */
    uint32_t          dbg_ceff_steal;
    uint32_t          dbg_memo_tot;
    uint32_t          dbg_prev_pq;
    int32_t           dbg_prev_wbq;
    uint8_t           dbg_prev_valid;
#endif
    volatile uint32_t dbg_rx_tick;          // high-res-timer count published after the last producer batch
                                             // and wr are complete. Shipping fast-acquire (Q57/Q58 intra-block
                                             // phase at ratio-lock) + continuous-fill estimator use it.
    /* Producer publishes the FIFO pointer and its timestamp as one bounded
     * lock-free snapshot.  The consumer never spins: if it preempts an odd
     * publication it keeps the preceding coherent pair for this pull. */
    volatile uint16_t prod_pub_seq;
    volatile uint32_t prod_pub_wr;
    volatile uint32_t prod_pub_tick;
    uint32_t          prod_cons_wr;
    uint32_t          prod_cons_tick;
    uint32_t          dbg_prod_snapshot_miss;
    uint8_t           prod_cons_valid;
    /*
     * Continuous producer-position estimate.  FIFO writes arrive in ISR-sized chunks, while the
     * ASRC consumes continuously.  Feeding the raw chunked fill to the servo turns that batching
     * into step FM.  The producer owns these fields and publishes ready last after a 250 ms
     * frame/time average; the consumer then interpolates from dbg_rx_tick at the measured effective
     * producer rate.  Counting the frames actually pushed is essential: a front end may publish
     * 5/5/6 or another rational pattern instead of APP_BLOCK_FRAMES.  A short callback warm-up
     * keeps the transport restart at ratio-lock out of the held rate estimate.
     */
    uint32_t          prod_phase_elapsed_ticks;
    uint32_t          prod_phase_frames;
    uint16_t          prod_phase_events;
    uint16_t          prod_phase_batch_max;
    float             prod_phase_rate_per_tick;
    float             prod_phase_center;
    volatile uint8_t  prod_phase_ready;
    uint8_t           prod_phase_warm_events;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    uint32_t          dbg_intermediate_overflow;
    uint32_t          dbg_intermediate_underrun;
    /* Ratio lock is published between producer and consumer block phases, and
     * a front end can also publish an intermediate chunk before the first
     * active pull. Re-centre once in the consumer (the sole rd writer) after
     * all data visible at that pull has arrived. */
    uint8_t           startup_recenter_pending;
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
    /* Foreground publishes the payload before pending. The consumer owns the
     * rd/phase transaction and clears pending only after it has applied it. */
    volatile float    freeze_recenter_step;
    volatile uint8_t  freeze_recenter_pending;
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
#if ASRC_FF_REFINE
    asrc_p0r_t        p0r;                  // two-stage FF handover (appended: no kernel offset moves)
#endif
#if APP_ASRC_QUIET_TRACK
    asrc_p0t_t        p0t;                  // QUIET_TRACK
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
// Continuous-fill estimator toggle (MEAS-only override, servo-observation ONLY). Product builds
// always use the per-engine estimator once calibrated. MEAS defaults to the product behaviour and
// retains the switch only for causal A/B work.
static volatile uint8_t s_cfill_en = 1u;
/* Product behaviour is ON.  The MEAS switch exists only to prove the causal
 * delta against the old producer-only occupancy estimate in one ROM image. */
static volatile uint8_t s_cfill_consumer_phase_en = 1u;
/* P0 step 5 (*af08, MEAS only, boot = APP_P0_CFILL_HW_INSTANT_DEFAULT): take the continuous-fill observation at the pull's
 * hardware instant (A->B: leg B's CCP2 edge grid; B->A of a BA MEAS image: leg A's CCP1 grid) instead of at the pull's
 * execution time.  See
 * asrc_clock_control_legb_defer_ticks() for the measured defect this removes. */
static volatile uint8_t s_cfill_hw_instant_en = APP_P0_CFILL_HW_INSTANT_DEFAULT;

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
 *
 * A BA MEAS image (!APP_ASRC_HAS_AB_ENGINE, see asrc_app_config.h) has NO A->B engine: B->A is the
 * only entry, still a named constant (index 0), and ASRC_ENGINE_AB is deliberately left undefined so
 * any A->B use that was not compiled out is a build error rather than an alias of the B->A engine.
 */
#if APP_ASRC_HAS_AB_ENGINE
#define ASRC_ENGINE_AB  (0u)                    // A -> B
#if APP_B_ROUTE_USES_BA
#define ASRC_ENGINE_BA  (1u)                    // B -> A (cross direction: BIDIR / LIGHT)
#define ASRC_ENGINE_COUNT  (2u)
#else
#define ASRC_ENGINE_COUNT  (1u)
#endif
#else
#if !APP_B_ROUTE_USES_BA
#error "An image without the A->B engine must use the B->A engine (APP_B_ROUTE_USES_BA)."
#endif
#define ASRC_ENGINE_BA  (0u)                    // B -> A, the only engine of a BA MEAS image
#define ASRC_ENGINE_COUNT  (1u)
#endif

/*
 * THE ENGINE STATE IS PINNED TO X SPACE IN THE Q31 ARM, NOT LEFT WHERE IT LANDS.
 *
 * mchp_asrc_q31_row16 reads this history through one AGU and the blended coefficient
 * row (s_ceff_q31, space(ymemory)) through the other, so the two MUST be in different
 * spaces or every MAC costs an extra cycle with no other symptom.  buildtools/build.ps1
 * (Assert-Q31ResamplerPlacement) gates exactly that: s_asrc entirely below
 * __YDATA_BASE.
 *
 * The gate used to be a coin flip.  This object is ~20 KB and the Q31 BiDir build sits
 * at 92 % of data, so whether the linker put it low in X or straddling 0xC000 depended
 * on allocation order -- nothing the source controlled.  Measured 2026-09-04: two builds
 * of the SAME configuration with the IDENTICAL total (60,508 B) placed it at
 * 0xBFD8..0xE7FC (REFUSED, recorded as F2 on 2026-09-03) and at 0x4590..0x95B0 (PASS).
 * So F2 was never "X is ~10.2 KB short" -- the bytes fit either way; the ADDRESS was
 * luck.  space(xmemory) removes the luck: the operand contract is now a link-time
 * constraint, and if X genuinely cannot hold it the linker says so instead of the build
 * passing on some machines and failing on others.
 *
 * Only the Q31 arm is pinned.  The float kernel has no AGU-pair contract (both operands
 * come through the same bus), so constraining a 20 KB object there would buy nothing and
 * could refuse a build that is otherwise fine.
 */
#if ASRC_SAMPLE_Q31
static asrc_t s_asrc[ASRC_ENGINE_COUNT] __attribute__(( space(xmemory) ));
#else
static asrc_t s_asrc[ASRC_ENGINE_COUNT];
#endif

/* "Is this the A->B engine?" -- a compile-time false in a BA MEAS image, which has none.  It
 * expands to exactly the comparison it replaces everywhere else, so AB MEAS and the product
 * compile to the same code. */
#if APP_ASRC_HAS_AB_ENGINE
#define ASRC_IS_AB_ENGINE( a )  ( (a) == &s_asrc[ASRC_ENGINE_AB] )
#else
#define ASRC_IS_AB_ENGINE( a )  ( false )
#endif
/* Boot-only selftest scratch (asrc_push_frames_selftest, the rear isolation oracle): the A->B
 * engine where it exists, else the B->A one.  Same gate either way (no audio ISR has run yet) and
 * the engine is asrc_reset() right after, see audio_app_asrc_reset_all(). */
#if APP_ASRC_HAS_AB_ENGINE
#define ASRC_SELFTEST_ENGINE    ASRC_ENGINE_AB
#else
#define ASRC_SELFTEST_ENGINE    ASRC_ENGINE_BA
#endif

#if APP_ASRC_MEAS
/* P1 and the Q50 acquisition detector are a single observation/control epoch,
 * not one instance per engine.  Bind that singleton to the direction the MEAS
 * image actually captures.  In a bidirectional image the other engine keeps
 * running the normal servo, but its reset, ratio publication and pulls must
 * not overwrite the selected direction's timestamps or Q50 state. */
#if APP_MEAS_DIR == MEAS_DIR_BA
#define ASRC_P1_ENGINE        ASRC_ENGINE_BA
#define ASRC_P1_PATH_NAME     "BA"
#define ASRC_P1_DIRECTION_AB  false
#elif APP_MEAS_DIR == MEAS_DIR_AB
#define ASRC_P1_ENGINE        ASRC_ENGINE_AB
#define ASRC_P1_PATH_NAME     "AB"
#define ASRC_P1_DIRECTION_AB  true
#else
#error "APP_MEAS_DIR must be MEAS_DIR_AB or MEAS_DIR_BA."
#endif

/* Leg B is a controller-clocked consumer in AB and its exact block rate is
 * available from the SPI2 BRG.  Leg A is the consumer in BA; its codec-master
 * rate changes with the accepted rate command, so retain that command's A rate
 * for Q50/Q51 diagnostics and the optional Q51 seed.  A 32-bit update is atomic
 * on this core and occurs before the transport restart publishes a ratio. */
#if APP_MEAS_DIR == MEAS_DIR_BA
static volatile uint32_t s_p1_consumer_rate_hz = (uint32_t)APP_SAMPLE_RATE_HZ;
#endif

static inline float asrc_p1_consumer_rate_hz( void )
{
#if APP_MEAS_DIR == MEAS_DIR_BA
    return (float)s_p1_consumer_rate_hz;
#else
    return 390625.0f / (float)( APP_SPI2_MASTER_BRG + 1 );
#endif
}

static inline bool asrc_p1_engine_selected( const asrc_t* a )
{
    return a == &s_asrc[ASRC_P1_ENGINE];
}
#endif

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
#if ASRC_SAMPLE_Q31
    a->frac_q31 = 0u;
#endif
    a->ratio = 0.0f;   // invalid until a feed-forward ratio is set -> asrc_pull emits silence
    a->corr_lpf   = 0.0f;
    a->step_state = 0.0f;   // seeded to ratio on the first valid ratio (asrc_apply_ratio)
#if ASRC_SAMPLE_Q31
    a->step_corr_state = 0.0f;
#endif
    a->step_smooth_st = 0.0f; // restart must not retain the previous rate's applied-step LPF state
#if ASRC_SAMPLE_Q31
    a->step_smooth_corr = 0.0f;
#endif
    a->fill_target       = ASRC_FILL_TARGET;   // ratio-aware value latched at the next ratio-lock
    a->fill_target_capped = 0u;
    a->dbg_fill          = 0u;
    a->dbg_fill_min      = 0xFFFFu;
    a->dbg_R             = 0u;
    a->dbg_R_safe        = 0u;
    a->fill_law_nominal  = 0u;
    a->dbg_jmax          = 0u;
    a->dbg_step          = 0.0f;
    a->dbg_pull_ticks_max = 0u;
    a->dbg_rx_tick              = 0u;
    a->prod_pub_seq             = 0u;
    a->prod_pub_wr              = 0u;
    a->prod_pub_tick            = 0u;
    a->prod_cons_wr             = 0u;
    a->prod_cons_tick           = 0u;
    a->dbg_prod_snapshot_miss   = 0u;
    a->prod_cons_valid          = 0u;
    a->prod_phase_elapsed_ticks = 0u;
    a->prod_phase_frames        = 0u;
    a->prod_phase_events        = 0u;
    a->prod_phase_batch_max     = 0u;
    a->prod_phase_rate_per_tick = 0.0f;
    a->prod_phase_center        = 0.0f;
    a->prod_phase_ready         = 0u;
    a->prod_phase_warm_events   = 0u;
#if APP_ASRC_STAGE_PROFILE
    a->dbg_stg_coef_max  = 0u;
    a->dbg_stg_mac_max   = 0u;
    a->dbg_stg_conv_max  = 0u;
    a->dbg_stg_ph_max    = 0u;
    a->dbg_stg_bl_max    = 0u;
    a->dbg_stg_st_max    = 0u;
    a->dbg_stg_coef_min  = 0xFFFFFFFFu;
    a->dbg_stg_mac_min   = 0xFFFFFFFFu;
    a->dbg_stg_conv_min  = 0xFFFFFFFFu;
    a->dbg_stg_ph_min    = 0xFFFFFFFFu;
    a->dbg_stg_bl_min    = 0xFFFFFFFFu;
    a->dbg_stg_st_min    = 0xFFFFFFFFu;
    a->dbg_memo_hit      = 0u;
    a->dbg_ceff_steal    = 0u;
    a->dbg_memo_tot      = 0u;
    a->dbg_prev_valid    = 0u;
#endif
    a->dbg_guard_drops   = 0u;
    a->dbg_starve_frames = 0u;
    for( uint8_t c = 0u; c < ASRC_CH; c++ ) { a->last_out[c] = 0; }
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    a->dbg_intermediate_overflow = 0u;
    a->dbg_intermediate_underrun = 0u;
    a->startup_recenter_pending = 0u;
#endif
#if APP_ASRC_MEAS
    a->freeze_step       = 0.0f;   // not frozen
    a->freeze_base_step  = 0.0f;
    a->freeze_recenter_step = 0.0f;
    a->freeze_recenter_pending = 0u;
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

/*
 * ------------------------------------------------------------------
 * Producer ring write -- ONE definition, shared by every int32 producer.
 * ------------------------------------------------------------------
 *
 * asrc_push() and asrc_push_frames() used to carry their own copy of this loop, and the
 * two copies had to agree bit-for-bit.  asrc_push_frames()'s own comment records what a
 * divergence costs: values 2^-8 too small (-48.1648 dB), silently, with every build and
 * selftest still passing.  One definition retires that whole class of bug --
 * asrc_push_frames_selftest() still checks the equivalence, and now it is checking one
 * function against itself.
 *
 * It is also where the producer's cost lives, so the loop is written for the ISR rather
 * than for symmetry with the reader.  Three things move OUT of the per-channel body:
 *
 *   - The source word and its conversion.  Under the replication mapping (channel c <-
 *     input slot c & 1) two distinct words feed all ASRC_CH channels, so the old form did
 *     ASRC_CH loads and ASRC_CH masks per frame to produce two values.  Now two of each,
 *     with the channel loop unrolled by two so both stores issue per iteration.
 *   - The destination address.  `a->ch[c][idx]` is a multiply by ASRC_FIFO_PHYS per
 *     channel; walking a pointer by that constant stride is an add.
 *   - The mirror test.  `idx < ASRC_POLY_M` depends only on the frame, so it selects
 *     between two loops instead of being re-evaluated ASRC_CH times per frame.
 *
 * Nothing about the STORED VALUES moves: the s24-left -> Q31 mask, the float scaling, the
 * ring wrap and the mirror overhang are bit-for-bit what they were.  Measured cost of the
 * old form at ASRC_CH=12 / APP_SLOTS_PER_FS=2 was 17 cycles per stored sample (pushAB =
 * 16.4 us per 16-frame block), against 6 cycles for the scatter on the way out.
 */
static inline asrc_samp_t asrc_ring_conv( int32_t w )
{
#if ASRC_SAMPLE_Q31
    // s24-left IS Q31: keep the codec word, drop only the sub-LSBs.
    return (asrc_samp_t)(int32_t)( (uint32_t)w & 0xFFFFFF00u );
#else
    return (asrc_samp_t)( (float)( w >> 8 ) );   // 24-bit source slot
#endif
}

// Channel c <- input slot (c & 1): the ordinary multi-channel profiles replicate physical
// L/R to model a wider workload.  `p` points at this frame's first slot; `idx` is the
// already-masked ring index.
static inline void asrc_ring_write_replicate( asrc_t* a, const int32_t* p, uint32_t idx )
{
    const asrc_samp_t v0 = asrc_ring_conv( p[0] );
    const asrc_samp_t v1 = asrc_ring_conv( p[1] );
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
    for( uint16_t c = 0u; c < ASRC_CH; c++ )
    {
        const asrc_samp_t v = ( ( c & 1u ) != 0u ) ? v1 : v0;
        a->tile[c >> 3u][idx][c & 7u] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
        if( idx < ASRC_POLY_M ) { a->tile[c >> 3u][idx + ASRC_FIFO_FRAMES][c & 7u] = v; }
#endif
    }
#else
    asrc_samp_t* d = &a->ch[0][idx];
    uint16_t c = 0u;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
    if( idx < ASRC_POLY_M )
    {
        for( ; ( c + 1u ) < ASRC_CH; c += 2u )
        {
            d[0]                                 = v0;
            d[ASRC_FIFO_FRAMES]                  = v0;   // mirror overhang
            d[ASRC_FIFO_PHYS]                    = v1;
            d[ASRC_FIFO_PHYS + ASRC_FIFO_FRAMES] = v1;
            d += 2u * ASRC_FIFO_PHYS;
        }
        // An odd width leaves the LAST channel index even, so it takes slot 0.
        if( c < ASRC_CH ) { d[0] = v0; d[ASRC_FIFO_FRAMES] = v0; }
        return;
    }
#endif
    for( ; ( c + 1u ) < ASRC_CH; c += 2u )
    {
        d[0]              = v0;
        d[ASRC_FIFO_PHYS] = v1;
        d += 2u * ASRC_FIFO_PHYS;
    }
    if( c < ASRC_CH ) { d[0] = v0; }
#endif
}

#if APP_ASRC_TDM8_ONE_TO_ONE
// Channel c <- input slot c: the explicit AK128 TDM8 profile.  Every channel has its own
// source word, so only the addressing and the mirror test hoist out of the body.
static inline void asrc_ring_write_direct( asrc_t* a, const int32_t* p, uint32_t idx )
{
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
    for( uint16_t c = 0u; c < ASRC_CH; c++ )
    {
        const asrc_samp_t v = asrc_ring_conv( p[c] );
        a->tile[c >> 3u][idx][c & 7u] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
        if( idx < ASRC_POLY_M ) { a->tile[c >> 3u][idx + ASRC_FIFO_FRAMES][c & 7u] = v; }
#endif
    }
#else
    asrc_samp_t* d = &a->ch[0][idx];
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
    if( idx < ASRC_POLY_M )
    {
        for( uint16_t c = 0u; c < ASRC_CH; c++ )
        {
            const asrc_samp_t v = asrc_ring_conv( p[c] );
            d[0]                = v;
            d[ASRC_FIFO_FRAMES] = v;   // mirror overhang
            d += ASRC_FIFO_PHYS;
        }
        return;
    }
#endif
    for( uint16_t c = 0u; c < ASRC_CH; c++ )
    {
        d[0] = asrc_ring_conv( p[c] );
        d += ASRC_FIFO_PHYS;
    }
#endif
}
#endif  // APP_ASRC_TDM8_ONE_TO_ONE

/* 250 ms spans hundreds of producer callbacks at every supported rate.  It averages timer/ISR
 * placement noise and complete rational-decimator frame patterns while leaving the three-second
 * readiness budget intact.  Keep raw FCY ticks in the ISR: converting every callback to time
 * would execute the HAL's 64-bit division on the audio path. */
#if APP_ASRC_MEAS && ASRC_SAMPLE_Q31
static volatile uint32_t s_p0q_base_bits, s_p0q_corr_bits, s_p0q_ratio_bits;
static volatile uint32_t s_p0q_whole, s_p0q_frac, s_p0q_frac_prev, s_p0q_changes;
/* P0 probe (§2 step 5): the continuous-fill terms behind fill_used, snapshotted at the same
 * AB pull as base/corr/ratio above so a capture's kick can be matched to producer/consumer
 * phase motion instead of just the servo's output. */
static volatile uint32_t s_p0q_fill_used_bits, s_p0q_phase_bits, s_p0q_consumer_phase_bits;
/* P0 kick log (item 2): the fill observation and the applied step across a MEAS capture.
 * Circular and free-running once armed: the first capture arm starts it, capture complete freezes
 * it for "?af01", and the next capture arm resumes it WITHOUT clearing, so each dump holds the
 * ~8 s before the capture (512 records x 8 pulls at ~500 pulls/s) as well as the window itself.
 * One record every P0K_DECIM-th pull of the measured engine (asrc_p1_engine_selected) holds that
 * pull's values plus the min/max fill error of the pulls in between, so a one-pull spike is not
 * lost to decimation.  16 B/record, MEAS Q31 builds only (stack free was 1 208 B with 9 KB, so
 * keep this small).  NOT COMPILED in a BA image (P0K_LOG 0): the B->A instance adds ~10.4 KB of
 * RAM, the 8 KB log did not link (857 B short), and a 4 KB log + 1024-sample capture linked but
 * trapped STACK ERROR on the first `*ar` (2026-09-29, SP-at-trap = SPLIM + 168 B). */
#define P0K_LOG    ( ( APP_MEAS_DIR != MEAS_DIR_BA ) || APP_P0K_LOG_BA )   /* opt-in on BA: asrc_app_config.h */
#define P0K_N      ( 512u )
#define P0K_DECIM  ( 8u )
typedef struct
{
    int16_t  fe;       /* (fill_used - fill_target) * 256 at this pull */
    int16_t  fe_lo;    /* min fe over the P0K_DECIM pulls ending here */
    int16_t  fe_hi;    /* max fe over the same pulls */
    uint16_t ph;       /* producer phase * 4096; 0xFFFF = continuous fill not used */
    uint16_t cp;       /* consumer phase * 65536 */
    uint8_t  pub;      /* published integer fill (pub_fill); 0xFF = none */
    uint8_t  age;      /* (pull time - last coherent producer publication) >> 7 ticks, 255 = sat */
    int16_t  dfrac;    /* applied Q31 frac - frac at the first logged pull, in LSB, saturated */
    uint8_t  late;     /* (pull time - this pull's leg-B RX ISR entry) >> 7 ticks, 255 = sat */
    uint8_t  pad2;     /* *af08 leg-B pull deferral >> 7 ticks (0 when *af08 is off), 255 = sat */
} p0k_rec_t;
#if P0K_LOG
static p0k_rec_t s_p0k[P0K_N];
#endif
static volatile uint8_t s_p0k_run    = 0u;
volatile uint32_t s_p0k_legb_isr_tick = 0u;   /* written by asrc_audio_path leg-B callback entry */
static volatile uint8_t s_p0k_frozen = 0u;
static uint32_t s_p0k_pulls = 0u;              /* measured-engine pulls logged since arm */
static uint32_t s_p0k_mark  = 0xFFFFFFFFu;     /* s_p0k_pulls when the first sample was captured */
static uint32_t s_p0k_end   = 0u;              /* s_p0k_pulls when the capture completed */
static uint32_t s_p0k_frac0  = 0u;             /* applied Q31 frac/whole at the first logged pull */
static uint32_t s_p0k_whole0 = 0u;
static int16_t  s_p0k_lo = 0, s_p0k_hi = 0;
#endif
#if ASRC_FF_REFINE
/* P0 two-stage FF (per engine: asrc_t.p0r; MEAS, or APP_ASRC_QUIET_TRACK).  Stage 1 is the 512 ms
 * count-window FF, published at ratio lock so audio starts early.  Stage 2 is the co-timed FF,
 * handed over inside asrc_pull() without a step: the ratio becomes the precise one, the fill
 * currently held becomes the setpoint, and the fill correction restarts from zero, so there is
 * no error left for the loop to ring out.  QUALITY_READY (MEAS) and QUIET_TRACK entry wait for it. */
#define P0R_QUIET_MS        ( 300u )    /* hold after the handover before QUALITY_READY */
#define P0R_ADOPT_FRAMES    ( 16.0f )   /* adopt the held fill only this close to the old setpoint */
/* The B->A engine of a MEAS image that has one (BA direction) gets its own P0 constants, tuned for
 * the B->A leg of A48->B8 (8 k -> 48 k direct, step 1/6, leg A pulls 3000/s = 48 kHz / 16 frames,
 * computed, not measured).  Everything else -- product, and the AB engine of any image -- keeps the
 * A48->B8 AB values, which were tuned at 500 pulls/s.  Other rate pairs on the B->A engine are
 * not tuned (NEXT card: route order BA -> direct -> 1:1). */
#define P0_BA_ROUTE         ( APP_ASRC_MEAS && APP_B_ROUTE_USES_BA )
#if P0_BA_ROUTE
#define P0_IS_BA( a )       ( (a) == &s_asrc[ASRC_ENGINE_BA] )
/* Fill EMA per pull, 0.005 = ~0.4 s at 500 pulls/s; / 6 keeps ~0.4 s at 3000 pulls/s. */
#define P0R_EMA_ALPHA( a )  ( P0_IS_BA( a ) ? ( 0.005f / 6.0f ) : 0.005f )
#else
#define P0R_EMA_ALPHA( a )  ( 0.005f )
#endif
static uint8_t        s_p0r_armed;            /* two-stage selected for this rate epoch (clock control) */
/* The MEAS bench states the handover must not cross (Mode-K constant step, FF freeze). */
static inline bool p0r_bench_hold( const asrc_t* a )
{
#if APP_ASRC_MEAS
    return a->ff_freeze || ( a->freeze_step > 0.0f );
#else
    (void)a;
    return false;
#endif
}
#if APP_ASRC_MEAS
/* *ax07: stage-2 FF bias in 0.1 ppm steps (MEAS Step 7 fault knob).  Zero at every boot. */
static volatile int8_t s_p0b_tenths;
static float           s_p0b_base, s_p0b_applied;   /* last handover: ratio before / after the bias */
#endif
#endif
#if APP_ASRC_QUIET_TRACK
/* P0 QUIET_TRACK (Q31, every engine, *ax06 VV in MEAS, boot = APP_P0T_DEFAULT_MODE).  Once QUALITY_READY is
 * reached the applied Q31 step stops following the fast fill servo and is driven instead by a
 * slow type-2 loop: the fill error averaged over M pulls (~10 s) feeds a PI whose output is a
 * target offset in Q31 LSB; the applied step moves toward it by at most 1 LSB every R pulls.
 * Entry is bumpless (offset 0 = the applied step of the entry pull) and the fill held at entry is
 * the setpoint.  The normal servo keeps running underneath and takes over again on exit/abort.
 * Kick logs (2026-09-28) put every THD+N fail on >20 LSB of step motion inside a 0.26 s window;
 * the ramp bounds that to 128/R LSB.  Host model: [internal tool] p0/qt_sim.py.
 * A product image (APP_ASRC_QUIET_TRACK=1, no P1) enters instead once the stage-2 handover is
 * P0R_QUIET_MS old; the gap / fill / slope window below is the same.  The product hands stage 2
 * over only on A->B at APP_P0_POINT_A_HZ -> _B_HZ (asrc_clock_control.c ccpdet_p0_point_ok()).
 * Presets, the entry window and P0T_PULL_HZ are counted in pulls and were tuned on A48->B8
 * (500 pulls/s).  On a route with another pull rate M, R, the ramp rate and the slope scale all
 * move with it: re-derive them there before selecting a preset (route order BA -> direct -> 1:1). */
typedef struct { uint16_t m; uint16_t r; float kp; float ki; } p0t_preset_t;
static const p0t_preset_t s_p0t_presets[5] =
{
    { 0u,    0u,  0.0f,    0.0f    },   /* 0 = OFF (normal servo) */
    { 5000u, 23u, 4000.0f, 1000.0f },   /* 1 = 10 s mean, <= 21.7 LSB/s (0.005 ppm/s) */
    { 2500u, 12u, 8000.0f, 1000.0f },   /* 2 = 5 s mean, <= 41.7 LSB/s */
    { 5000u, 46u, 4000.0f, 1000.0f },   /* 3 = 10 s mean, <= 10.9 LSB/s */
    { 5000u, 23u, 4000.0f, 250.0f  },   /* 4 = preset 1 with KI/4: damps the ~8 min fill limit
                                         *     cycle after a one-step fill jump (qt_sim.py --jump) */
};
#define P0T_ABORT_FRAMES   ( 4.0f )       /* block-mean fill error that hands control back */
#define P0T_OUT_MAX        ( 214748 )     /* |offset| limit, LSB (~50 ppm of a step of 2) */
/* Entry waits until the applied (smoothed) step is within P0T_ENTRY_LSB of the servo's own
 * unsmoothed step and the fill EMA is within P0T_ENTRY_FILL of the servo setpoint, both for
 * P0T_ENTRY_PULLS pulls in a row.  Measured 2026-09-28: entering at QUALITY_READY itself (300 ms
 * after the two-stage handover) caught the smoother mid-transient ~10 ppm off; the ramp cannot
 * absorb that (fill walked 0.15 frame/s and the track aborted every 20-50 s).  qt_sim.py: an
 * entry error E costs ~0.5*8000*E^2*2^-31/ramp frames of fill, so E must stay << ~4800 LSB. */
#define P0T_ENTRY_LSB      ( 2000 )       /* ~0.47 ppm */
#define P0T_ENTRY_FILL     ( 0.5f )
#define P0T_ENTRY_PULLS    ( 500u )       /* 1 s */
/* ...and the fill EMA must not be walking.  Measured 2026-09-28 07:47 (bad epoch): gap and level
 * both passed while the true step was ~2.5 ppm away, i.e. the fill walked ~0.04 frame/s (16 kHz
 * intermediate: 2.5e-6 * 16000); frozen there, the track aborted every ~113 s.  The fill EMA's
 * noise over one entry window is ~0.001 frame, so 0.01 frame/s is well above noise and well
 * below that walk.  AB pull rate is 500 Hz (16 frames at 8 kHz). */
#define P0T_ENTRY_SLOPE    ( 0.01f )      /* frame/s over the entry window */
#define P0T_PULL_HZ        ( 500.0f )
#define P0T_PRESETS        s_p0t_presets
#if P0_BA_ROUTE
/* The same presets for the B->A engine at 8 k -> 48 k (see P0_BA_ROUTE), converted so that time (s)
 * and step (ppm) behave as on A48->B8; fill thresholds stay in frames (ring hygiene).
 *   1 Q31 LSB = 2^-31 / step: 0.00279 ppm at step 1/6 vs 0.000233 ppm at step 2 (x12).
 *   Pull rate 3000/s vs 500/s (x6).  The plant moves the fill fs_in x ppm: 8 k vs 16 k frames/s (x1/2).
 *   m (10 s)          -> m x 6
 *   ramp (ppm/s)      -> r x 6 x 12 = r x 72   (1656 pulls/LSB = 0.00506 ppm/s, as 23 at AB)
 *   kp, ki (ppm per frame of error) x 2, in LSB x 2 / 12 -> / 6
 *   anti-windup budget m / r = 18.1 LSB = 0.0506 ppm, as 217 LSB at AB.
 * So the loop from ppm error to ppm correction is AB's exactly; only the fill excursion (frames) is half.
 * Host model: python [internal tool] p0/qt_sim.py --route ba. */
static const p0t_preset_t s_p0t_presets_ba[5] =
{
    { 0u,     0u,    0.0f,        0.0f       },   /* 0 = OFF */
    { 30000u, 1656u, 4000.0f/6.0f, 1000.0f/6.0f },  /* 1 */
    { 15000u, 864u,  8000.0f/6.0f, 1000.0f/6.0f },  /* 2 */
    { 30000u, 3312u, 4000.0f/6.0f, 1000.0f/6.0f },  /* 3 */
    { 30000u, 1656u, 4000.0f/6.0f, 250.0f/6.0f  },  /* 4 */
};
#define P0T_BA_PRESETS      s_p0t_presets_ba
#define P0T_BA_ENTRY_LSB    ( 167 )        /* 0.47 ppm, as 2000 at step 2 */
#define P0T_BA_ENTRY_PULLS  ( 3000u )      /* 1 s */
#define P0T_BA_ENTRY_SLOPE  ( 0.005f )     /* 0.625 ppm x 8 k, as 0.01 frame/s = 0.625 ppm x 16 k */
#define P0T_BA_PULL_HZ      ( 3000.0f )
#define P0T_BA_OUT_MAX      ( 17895 )      /* 50 ppm of step 1/6 */
#define P0T_PICK( a, name ) ( P0_IS_BA( a ) ? P0T_BA_##name : P0T_##name )
/* The BA presets above hold ONLY at the point they were converted for: leg B 8 k in, leg A 48 k
 * out.  At any other pair the B->A engine stays on the normal servo (verified point only, as the
 * product does), and each refused entry pull is counted, so a direct / 1:1 study on this image
 * sees `ba_rate_rejects` climb instead of a 3000-pulls/s tuning running silently.  The rates are
 * the CONFIGURED ones (asrc_audio_path_engine_nominal); a codec not brought up yet reads as the
 * seeded 48000 on leg B and is refused too, which is the safe side. */
#define P0T_BA_FS_IN_HZ     ( 8000u )
#define P0T_BA_FS_OUT_HZ    ( 48000u )
static uint32_t s_p0t_ba_rate_rejects;
static inline bool p0t_ba_rate_ok( void )
{
    uint32_t fs_in = 0u, num = 0u, den = 0u, fs_out = 0u;
    return asrc_audio_path_engine_nominal( false, &fs_in, &num, &den, &fs_out ) &&
           ( fs_in == P0T_BA_FS_IN_HZ ) && ( fs_out == P0T_BA_FS_OUT_HZ );
}
#else
#define P0T_PICK( a, name ) ( P0T_##name )
#endif
static volatile uint8_t s_p0t_mode = APP_P0T_DEFAULT_MODE;   /* preset index; *ax06 overrides */
#endif
#if P0_BA_ROUTE
/* BA producer stamp (2026-10-01, MEAS BA engine only, gated by *af08 = s_cfill_hw_instant_en).
 * Leg B's callback is the B->A producer and cannot start while leg A's (higher-priority)
 * callback runs, so asrc_note_producer()'s software time is late by leg B's deferral; measured
 * on HW 2026-10-01 (B1): legb_defer jumped 27 -> 11386 / 10501 ticks exactly at the e = -0.907 /
 * -0.824 fill jumps and decayed with them.  The leg-B callback stores the deferral it measured
 * at its top (ONE ccpdet_defer_ticks() call per callback, high-res ticks, >= 0) here before it
 * pushes; the push of the SAME callback subtracts it, so the publication is stamped at leg B's
 * hardware instant.  Single writer and single reader are both that callback: no race. */
static volatile int32_t s_p0_ba_prod_defer = 0;
void audio_app_asrc_p0_ba_prod_defer_note( int32_t defer_ticks ) { s_p0_ba_prod_defer = defer_ticks; }
#endif

#define ASRC_PROD_PHASE_CAL_TICKS  ((uint32_t)( FCY / 4u ))
/* Ratio publication and the codec/transport restart are not atomic with a producer callback.  The
 * first post-lock interval can therefore contain about 1 ms of stopped time (measured at 48 -> 8),
 * which biased a 250 ms estimate by roughly 0.4 per cent.  Eight callbacks are only 16 ms even at
 * the slowest supported 8 kHz / 16-frame producer and are long enough to leave that handover behind. */
#define ASRC_PROD_PHASE_WARM_EVENTS    (8u)
/* Every supported producer callback is at most 16 / 8 kHz = 2 ms apart.  This absolute fence is
 * deliberately 2.5x wider; after the first accepted interval, the rolling 0.5x..2x fence below is
 * route-relative and catches shorter restart gaps on the high-rate routes. */
#define ASRC_PROD_PHASE_MAX_DT_TICKS   ((uint32_t)( FCY / 200u ))

static inline void asrc_publish_producer_snapshot( asrc_t* a, uint32_t now_tick )
{
    const uint16_t seq = (uint16_t)( a->prod_pub_seq & 0xFFFEu );
    a->prod_pub_seq = (uint16_t)( seq + 1u );
    __asm__ volatile ( "" ::: "memory" );
    a->prod_pub_wr   = a->wr;
    a->prod_pub_tick = now_tick;
    __asm__ volatile ( "" ::: "memory" );
    a->prod_pub_seq = (uint16_t)( seq + 2u );
    a->dbg_rx_tick = now_tick;
}

static inline void asrc_note_producer( asrc_t* a, uint32_t frames )
{
    if( frames == 0u ) { return; }

    const uint32_t previous_tick = a->dbg_rx_tick;
    uint32_t       now_tick      = nora_high_res_timer_get_count();
#if P0_BA_ROUTE
    /* BA producer stamp at the hardware instant (see s_p0_ba_prod_defer).  Both are high-res
     * ticks; the stamp moves EARLIER by the deferral, so the consumer's elapsed time and hence
     * its extrapolated producer phase grow by the same amount -- cancelling the negative fill
     * step.  *af0800 restores the software stamp for an on-HW A/B. */
    if( s_cfill_hw_instant_en && P0_IS_BA( a ) )
    {
        now_tick -= (uint32_t)s_p0_ba_prod_defer;
    }
#endif

    /* Boot and rate-change setup can pause callbacks while ratio is invalid.  Those gaps made a
     * pre-lock calibration read about 0.5% low even though the steady TDM rate was correct.  Keep
     * the timestamp fresh, but start the held estimate only after this engine's ratio lock. */
    if( a->ratio <= 0.0f )
    {
        a->prod_phase_elapsed_ticks = 0u;
        a->prod_phase_frames        = 0u;
        a->prod_phase_events        = 0u;
        a->prod_phase_batch_max     = 0u;
        a->prod_phase_ready         = 0u;
        a->prod_phase_warm_events   = 0u;
        asrc_publish_producer_snapshot( a, now_tick );
        return;
    }

    if( !a->prod_phase_ready && a->prod_phase_warm_events < ASRC_PROD_PHASE_WARM_EVENTS )
    {
        a->prod_phase_warm_events++;
        asrc_publish_producer_snapshot( a, now_tick );
        return;
    }

    if( !a->prod_phase_ready && previous_tick != 0u )
    {
        const uint32_t dt_ticks = now_tick - previous_tick;
        uint8_t interval_ok = ( dt_ticks > 0u && dt_ticks <= ASRC_PROD_PHASE_MAX_DT_TICKS ) ? 1u : 0u;
        if( interval_ok && a->prod_phase_events != 0u )
        {
            const uint32_t mean_dt = a->prod_phase_elapsed_ticks / (uint32_t)a->prod_phase_events;
            if( ( dt_ticks * 2u < mean_dt ) || ( dt_ticks > mean_dt * 2u ) )
            {
                interval_ok = 0u;
            }
        }
        /* Reject a discontinuity rather than teaching the held estimator that a stopped/restarted
         * stream is an extremely slow clock.  Re-enter the short warm-up so a burst of disturbed
         * callbacks cannot make the very next interval the new reference. */
        if( interval_ok )
        {
            a->prod_phase_elapsed_ticks += dt_ticks;
            a->prod_phase_frames       += frames;
            a->prod_phase_events++;
            if( frames > a->prod_phase_batch_max )
            {
                a->prod_phase_batch_max = (uint16_t)frames;
            }
            if( a->prod_phase_elapsed_ticks >= ASRC_PROD_PHASE_CAL_TICKS )
            {
                a->prod_phase_rate_per_tick =
                    (float)a->prod_phase_frames / (float)a->prod_phase_elapsed_ticks;
                a->prod_phase_center = 0.5f * (float)a->prod_phase_frames /
                                       (float)a->prod_phase_events;
                /* Single-writer publication: all 32-bit values above are complete before the
                 * consumer is allowed to use them, and they remain held for this rate epoch. */
                __asm__ volatile ( "" ::: "memory" );
                a->prod_phase_ready = 1u;
            }
        }
        else
        {
            a->prod_phase_elapsed_ticks = 0u;
            a->prod_phase_frames       = 0u;
            a->prod_phase_events       = 0u;
            a->prod_phase_batch_max    = 0u;
            a->prod_phase_warm_events  = 0u;
        }
    }

    asrc_publish_producer_snapshot( a, now_tick );
}

// Producer side: push this block's input frames into the per-channel rings.  The
// ordinary multi-channel profiles replicate physical L/R (ch c <- input slot
// c&1) to model a wider workload.  The explicit AK128 TDM8 profile maps slot c
// directly to channel c.  On overflow drop the oldest frame. The mirror keeps
// the poly read window contiguous (see asrc_poly_interp).
static void asrc_push( asrc_t* a, const int32_t* src )
{
#if ASRC_HAVE_FIXED_PUSH_BLOCK16
    const uint32_t wr      = a->wr;
    const uint32_t wr_next = wr + APP_BLOCK_FRAMES;

    // History writes complete before wr is published.  The assembly routine is
    // one call per 16-frame block; it preserves the exact signed-24 -> float and
    // main-ring/mirror semantics of the C fallback below.
    ASRC_PUSH16_STEREO( &a->ch[0][0], src, wr & ASRC_FIFO_MASK );

    // The producer publishes wr only after the block, then publishes the matching wr/timestamp
    // snapshot below.  The overflow guard that used to advance rd from here now runs in the
    // consumer (asrc_pull), so rd has a single writer -- see the rd declaration.  Overrunning the
    // consumer is still detected and counted one pull later, when the overwritten frames are gone.
    a->prod_frames = 0u;
    a->wr = wr_next;
#else
    uint32_t wr = a->wr;
    const int32_t* p = src;
    for( uint16_t n = 0u; n < APP_BLOCK_FRAMES; n++ )
    {
#if APP_ASRC_TDM8_ONE_TO_ONE
        asrc_ring_write_direct( a, p, wr & ASRC_FIFO_MASK );
#else
        asrc_ring_write_replicate( a, p, wr & ASRC_FIFO_MASK );
#endif
        wr++;
        p += APP_SLOTS_PER_FS;
    }
    a->prod_frames = 0u;
    a->wr = wr;   // publish the complete block once; rd remains consumer-owned
#endif
    // Publish the matching FIFO pointer/time only after every sample is visible.
    asrc_note_producer( a, APP_BLOCK_FRAMES );
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
    a->prod_frames = 1u;   // charge a consumer-side discard to dbg_intermediate_overflow
    uint32_t wr = a->wr;
    const int32_t* p = src;
    /* The amplitude representation MUST match asrc_push()'s exactly: the two producers write
     * the same ring and a mismatch is silent (values 2^-8 too small, = -48.1648 dB, with every
     * build/selftest still passing).  That is why the write itself is now the SHARED
     * asrc_ring_write_replicate() rather than a second copy of it; asrc_push_frames_selftest()
     * below still checks the equivalence.  This producer has no TDM8 one-to-one arm -- its
     * source is always the stereo intermediate stream. */
    for( size_t n = 0u; n < frames; n++ )
    {
        asrc_ring_write_replicate( a, p, wr & ASRC_FIFO_MASK );
        wr++;
        p += stride;
    }
    a->wr = wr;   // publish once after the complete variable-size batch
    asrc_note_producer( a, (uint32_t)frames );
}

#if APP_ASRC_FULL_IIR_48_TO_32
/* Producer for a stage that has ALREADY produced the ring's own float representation:
 * one block of APP_BLOCK_FRAMES frames, channel-major, `ch_stride` floats apart, values
 * in 24-bit counts.  It is a THIRD producer beside asrc_push() and asrc_push_frames(),
 * and the reason it exists is that the Full-IIR stage is a float filter feeding a float
 * ring: routing it through the int32 interface would insert a 24-bit requantisation and
 * a clip point that the host qualification does not have, in the one place the peak is
 * above full scale.
 *
 * Frame accounting matches asrc_push(), NOT asrc_push_frames(): the stage does not
 * resample, so a whole block goes in and prod_frames stays 0 (a consumer-side discard
 * is charged to dbg_guard_drops, the direct path's counter, not to the front end's
 * intermediate-overflow counter).
 *
 * Amplitude representation is asrc_push()'s float arm, quoted: (float)(word >> 8).  The
 * caller converts; this writer must not scale.  The mismatch this mirrors cost -48 dB
 * once with every selftest still passing -- see asrc_push_frames_selftest().
 */
static void asrc_push_block_f32( asrc_t* a, const float* src, uint32_t ch_stride,
                                 uint32_t frames, uint8_t from_frontend )
{
    /* Attribution only: 1 = a front end fed this block (charge a consumer-side discard to
     * dbg_intermediate_overflow), 0 = raw TDM block, as before. */
    a->prod_frames = from_frontend;
    const uint32_t wr = a->wr;
    for( uint32_t c = 0u; c < ASRC_CH; c++ )
    {
        const float* p = &src[ c * ch_stride ];
        for( uint32_t n = 0u; n < frames; n++ )
        {
            const uint32_t idx = ( wr + n ) & ASRC_FIFO_MASK;
            const asrc_samp_t v = p[n];
            a->ch[c][idx] = v;
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
            if( idx < ASRC_POLY_M ) { a->ch[c][idx + ASRC_FIFO_FRAMES] = v; }   // mirror overhang
#endif
        }
    }
    a->wr = wr + frames;   // published last, as in asrc_push()
    asrc_note_producer( a, frames );
}
#endif /* APP_ASRC_FULL_IIR_48_TO_32 */

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
    t.v = s_asrc[ASRC_SELFTEST_ENGINE].ch[c][i];
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
            s_asrc[ASRC_SELFTEST_ENGINE].ch[c][i] = s.v;
        }
    }
}

static void asrc_push_frames_selftest( void )
{
    static uint8_t done = 0u;
    if( done ) { return; }
    done = 1u;

    asrc_t* const a = &s_asrc[ASRC_SELFTEST_ENGINE];
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
        for( uint32_t i = 0u; i < ASRC_FIFO_PHYS; i++ ) { s_asrc[ASRC_SELFTEST_ENGINE].ch[c][i] = ASRC_SAMP_ZERO; }
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
// positive power series converges comfortably for beta=11.3 in float precision.
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

#if APP_ASRC_REAR_ISOLATION_SELFTEST
/* Test-only, default off (see the gate in asrc_app_config.h).  After asrc_poly_q31.inc because it
 * uses that file's coefficient table, its two assembly kernels and its phase/slot helpers, and it
 * must see the engine (s_asrc) and the ring geometry, which are declared above. */
#if (ASRC_POLY_METHOD != ASRC_POLY_Q31)
#error "APP_ASRC_REAR_ISOLATION_SELFTEST is a test for the Q31 polyphase rear: it has nothing to verify unless ASRC_POLY_METHOD == ASRC_POLY_Q31.  Turn the gate off for this profile rather than building a selftest that checks a resampler the image does not contain."
#endif
#if (ASRC_HISTORY_LAYOUT == ASRC_HISTORY_TILE8)
#error "APP_ASRC_REAR_ISOLATION_SELFTEST reads the ring as ch[c][frame] at a per-channel stride; the TILE8 layout interleaves eight lanes per frame instead, so the oracle's indexing would be wrong rather than independent.  A TILE8 version needs its own indexing."
#endif
#include "asrc_rear_isolation_selftest.inc"
#endif
#endif

#if (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR)
#if !APP_ASRC_HAS_AB_ENGINE
/* Float BA MEAS image (e.g. APP_BUILD_ASRC_MEAS_96K_B_TO_A) without the A->B engine: the float
 * selftests below (boot-only, cleared by asrc_reset) and the read-only *az kernel bench use the
 * B->A ring instead.  Scoped to this block and #undef'd at its end, so everywhere else an A->B use
 * in such an image is still a build error. */
#define ASRC_ENGINE_AB  ASRC_SELFTEST_ENGINE
#define ASRC_ENGINE_AB_IS_SCRATCH_ALIAS 1
#endif
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
#ifdef ASRC_ENGINE_AB_IS_SCRATCH_ALIAS
#undef ASRC_ENGINE_AB
#undef ASRC_ENGINE_AB_IS_SCRATCH_ALIAS
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
// clamp, held for 0.3 s of wall time. The slew limiter is ordinary steady-state control activity, not
// a readiness fault; it remains diagnosed below but does not reset the hold. Enabled by *nt39 (candidate);
// off = current servo (baseline).
#define Q50_PULL_HZ            ( asrc_p1_consumer_rate_hz() / (float)APP_BLOCK_FRAMES )
#define Q50_LOCK_HOLD_MS       ( 300u )      /* rate-independent continuous hold */
#define Q50_FILL_TOL           ( 6.0f )     /* |fill_ma - target| lock window (frames) */
#define Q50_SLOPE_TOL          ( 1.5f )     /* |fast_ma - slow_ma| ~ 0 slope (frames) */
/* Fine step-convergence threshold in applied-step units. The trend detector
 * runs on the centred correction (step - ratio), so its float representation
 * resolves motion well below this threshold instead of acquiring an artificial
 * ULP floor near an absolute step of 1.0. P1 timestamps the first in-band pull;
 * no external measurement or additional fixed wait participates. */
#define Q50_STEP_TREND_TOL     ( 1.5e-5f )
enum { Q50_ACQUIRE = 0u, Q50_HANDOVER = 1u, Q50_TRACK = 2u };
static uint8_t  s_q50_en       = APP_Q50_DEFAULT_ON;  // *nt39 / boot default: 1 = run ACQUIRE machine
static uint8_t  s_q50_state    = Q50_TRACK;
static float    s_q50_fill_fast = 0.0f;  // continuous-fill (servo input) EMA ~0.1 s -- the gate
static float    s_q50_fill_slow = 0.0f;  // continuous-fill EMA ~0.4 s (slope reference)
static float    s_q50_raw_fill_fast = 0.0f;  // raw ring occupancy EMA ~0.1 s -- ring-safety telemetry only
static float    s_q50_step_slow  = 0.0f; // centred step-correction slow EMA (tau ~0.15 s)
static float    s_q50_step_vslow = 0.0f; // centred correction vslow EMA (tau ~1.05 s); slow-vslow = trend
static float    s_q50_strend_dbg = 0.0f; // last step-trend value (P1 fine-convergence input)
static uint8_t  s_q50_trend_seeded = 0u;
static uint32_t s_q50_lockcnt  = 0u;   // consecutive in-range pulls
static uint32_t s_q50_lock_start_ms = 0u;
static uint32_t s_q50_lock_hold_ms = 0u;
static uint32_t s_q50_lock_peak_ms = 0u;
static float    s_q50_fill_dev_dbg = 0.0f;
static float    s_q50_raw_fill_dev_dbg = 0.0f;
static float    s_q50_fill_slope_dbg = 0.0f;
static uint8_t  s_q50_gate_block_dbg = 0u;    // readiness blockers: bit0=fill, bit1=slope, bit2=clamp
static uint8_t  s_q50_gate_activity_dbg = 0u; // observed activity: blockers plus bit3=slew
static uint8_t  s_q50_gate_last_reset_dbg = 0u;
static uint32_t s_q50_gate_reset_count = 0u;
static uint32_t s_q50_gate_reset_fill = 0u;
static uint32_t s_q50_gate_reset_slope = 0u;
static uint32_t s_q50_gate_reset_clamp = 0u;
static uint32_t s_q50_gate_reset_slew = 0u;
static uint32_t s_q50_gate_slew_events = 0u;
static uint32_t s_q50_pulls    = 0u;   // pulls since ACQUIRE start
static uint32_t s_q50_lock_pulls = 0u; // pulls-to-lock (0 = not locked yet)
static float    s_q50_ho_stepdiff = 0.0f;  // applied-step delta imposed at handover (should be ~0)
// Q51 Feed-Forward Rate Seed (layered on Q50 candidate)
#define Q51_CONSUMER_FS_HZ  ( asrc_p1_consumer_rate_hz() )
static uint8_t  s_q51_seed_en   = APP_Q51_DEFAULT_ON;  // *nt3C: seed servo state from ACQUIRE rate estimate
static uint32_t s_q51_wr0       = 0u;   // producer pointer at ACQUIRE start
static uint32_t s_q51_t0        = 0u;   // high-res-timer count at ACQUIRE start
static float    s_q51_est_step  = 0.0f; // last rate estimate = fs_A_meas / fs_B (telemetry)
static float    s_q51_corr_seed = 0.0f; // corr_lpf seed applied at handover (telemetry)
static uint8_t  s_q51_applied   = 0u;   // 1 = last handover actually seeded (passed sanity), 0 = fell back
// Q52 Differential fill-drift rate seed (replaces the Q50/Q51 handover when enabled).
// The startup burst is a wall-time event.  The original 256/950-pull values were
// about 95/350 ms at the AB consumer rate and APP_BLOCK_FRAMES=16, but became
// 0.51/1.90 s for a BA consumer at 8 kHz.  Keep the established AB timing while
// making the selected MEAS direction independent of its output rate.
#define Q52_SKIP_MS  ( 95u )
#define Q52_WIN_MS   ( 350u )
static uint8_t  s_q52_seed_en   = APP_Q52_DEFAULT_ON;  // *nt3D: differential fill-drift seed
static float    s_q52_fill0     = 0.0f; // fill at window start
static float    s_q52_ratio0    = 0.0f; // frozen FF ratio held over the window
static float    s_q52_est_step  = 0.0f; // step_true = ratio0 + delta_fill/n_out (telemetry)
static float    s_q52_dfill      = 0.0f; // delta_fill over the window (telemetry)
static uint32_t s_q52_start_ms  = 0u;
static uint8_t  s_q52_fill0_latched = 0u;
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
uint32_t audio_app_asrc_get_wr( void )        { return s_asrc[ASRC_P1_ENGINE].wr; }
uint32_t audio_app_asrc_get_rd( void )        { return s_asrc[ASRC_P1_ENGINE].rd; }
/*
 * P1 — rate/re-arm to control-ready and quality-ready telemetry
 *
 * A normal DR/THD+N campaign deliberately waits a fixed dwell, freezes, and
 * records one capture.  That snapshot cannot tell when a new rate actually
 * obtained a valid ratio, when the closed-loop fill control first became
 * healthy, or when the internal step trend reached its specified floor band.
 * The P1 record is intentionally small and event-based so `?as` answers
 * immediately; it neither emits periodic UART traffic nor allocates a
 * Q34-sized trace buffer.
 *
 * `control_ready` is the Q50 read-only detector: centred continuous fill (the servo's
 * fill_used, not the raw ring occupancy), negligible
 * fill slope, and no safety clamp for Q50_LOCK_HOLD_MS of wall time. Slew
 * limiting remains visible as normal control activity but is not a blocker.
 * `quality_ready` follows it when |strend| first reaches
 * Q50_STEP_TREND_TOL.  Both are firmware events; neither depends on UART poll
 * cadence or a host-side time wait.  An ASRC harm counter increment invalidates
 * the epoch, including after either ready event.
 *
 * One MEAS image observes the engine selected by APP_MEAS_DIR, hence one
 * record.  Keeping it out of asrc_t avoids growing the shipping/BIDIR engine
 * objects and makes the state unavailable outside APP_ASRC_MEAS by
 * construction.
 */
enum
{
    P1_IDLE       = 0u,
    P1_COMMAND    = 1u,  /* accepted command; wait until its restart has reset the engine */
    P1_WAIT_RATIO = 2u,
    P1_SETTLING   = 3u,
    P1_CONTROL_READY = 4u,
    P1_QUALITY_READY = 5u,
    P1_FAULT         = 6u,
    P1_REJECTED      = 7u,
};

enum
{
    P1_HARM_DROP   = 0x01u,
    P1_HARM_STARVE = 0x02u,
    P1_HARM_FE_OVF = 0x04u,
    P1_HARM_FE_UDF = 0x08u,
};

typedef struct
{
    volatile uint32_t epoch;
    volatile uint32_t command_ms;
    volatile uint32_t ratio_lock_ms;
    volatile uint32_t control_ready_ms;
    volatile uint32_t quality_ready_ms;
    volatile float    quality_ready_strend;
    uint32_t          rate_a_hz;
    uint32_t          rate_b_hz;
    uint32_t          frontend_num;
    uint32_t          frontend_den;
    const char*       frontend_tag;
    uint32_t          guard_drops_at_lock;
    uint32_t          starve_frames_at_lock;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    uint32_t          frontend_ovf_at_lock;
    uint32_t          frontend_udf_at_lock;
#endif
    volatile uint8_t  state;
    volatile uint8_t  fault_mask;
    uint8_t           frontend_kind;
    uint8_t           frontend_armed;
} asrc_p1_t;

static asrc_p1_t s_p1;

static const char* p1_state_name( uint8_t state )
{
    switch( state )
    {
        case P1_COMMAND:    return "COMMAND";
        case P1_WAIT_RATIO: return "WAIT_RATIO";
        case P1_SETTLING:   return "SETTLING";
        case P1_CONTROL_READY: return "CONTROL_READY";
        case P1_QUALITY_READY: return "QUALITY_READY";
        case P1_FAULT:      return "HARM";
        case P1_REJECTED:   return "REJECTED";
        default:            return "IDLE";
    }
}

static uint32_t p1_ms_to_us10( uint32_t elapsed_ms )
{
    /* Keep the established wire unit while using the application's 32-bit
     * millisecond tick for long-lived epochs.  The previous raw 100 MHz Timer2
     * subtraction wrapped every 42.95 s.  Saturation is explicit at the wire
     * format's 429.5 s limit instead of silently returning a small age. */
    return ( elapsed_ms <= ( UINT32_MAX / 10000u ) )
         ? elapsed_ms * 10000u : UINT32_MAX;
}

static uint8_t p1_harm_mask( const asrc_t* a )
{
    uint8_t mask = 0u;
    if( a->dbg_guard_drops != s_p1.guard_drops_at_lock ) { mask |= P1_HARM_DROP; }
    if( a->dbg_starve_frames != s_p1.starve_frames_at_lock ) { mask |= P1_HARM_STARVE; }
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    if( a->dbg_intermediate_overflow != s_p1.frontend_ovf_at_lock ) { mask |= P1_HARM_FE_OVF; }
    if( a->dbg_intermediate_underrun != s_p1.frontend_udf_at_lock ) { mask |= P1_HARM_FE_UDF; }
#endif
    return mask;
}

static const char* p1_frontend_kind_name( uint8_t kind )
{
    switch( (asrc_frontend_kind_t)kind )
    {
        case ASRC_FRONTEND_FILTER_1TO1:     return "lp1";
        case ASRC_FRONTEND_FIXED_RATIO:     return "dec";
        case ASRC_FRONTEND_RATIONAL:        return "rational";
        case ASRC_FRONTEND_FIXED_THEN_1TO1: return "dec_lp1";
        default:                             return "direct";
    }
}

void audio_app_asrc_p1_begin( uint32_t rate_a_hz, uint32_t rate_b_hz )
{
    /* State is written last so a concurrently running pull only sees either
     * the old epoch or a fully initialized new one.  This is telemetry only;
     * like the existing counter snapshots, one transient read may be stale
     * but it cannot change audio control. */
    s_p1.state = P1_IDLE;
    s_p1.epoch++;
#if APP_ASRC_QUIET_TRACK
    {
        asrc_p0t_t* const q = &s_asrc[ASRC_P1_ENGINE].p0t;
        q->active = 0u;   /* P0 QUIET_TRACK: every rate epoch starts on the normal servo */
        q->aborts = 0u;
        q->entry_ok = 0u;
        q->slope_rejects = 0u;
        q->entry_slope = 0.0f;
    }
#endif
    s_p1.command_ms       = GetTicks();
    s_p1.ratio_lock_ms    = 0u;
    s_p1.control_ready_ms = 0u;
    s_p1.quality_ready_ms = 0u;
    s_p1.quality_ready_strend = 0.0f;
    s_p1.rate_a_hz          = rate_a_hz;
    s_p1.rate_b_hz          = rate_b_hz;
#if APP_MEAS_DIR == MEAS_DIR_BA
    if( rate_a_hz != 0u ) { s_p1_consumer_rate_hz = rate_a_hz; }
#endif
    s_p1.frontend_num       = 1u;
    s_p1.frontend_den       = 1u;
    s_p1.frontend_tag       = "";
    s_p1.frontend_kind      = (uint8_t)ASRC_FRONTEND_NONE;
    s_p1.frontend_armed     = 0u;
    /* The pair table is pure, so it is safe to make the epoch identity useful
     * immediately after command acceptance.  At ratio lock these planned
     * fields are replaced by the armed-path facts below. */
    {
        asrc_frontend_plan_t plan;
        asrc_audio_path_frontend_plan( rate_a_hz, rate_b_hz, &plan );
        if( ASRC_P1_DIRECTION_AB )
        {
            s_p1.frontend_num  = plan.num_ab;
            s_p1.frontend_den  = plan.den_ab;
            s_p1.frontend_kind = (uint8_t)plan.kind_ab;
        }
        else
        {
            s_p1.frontend_num  = plan.num_ba;
            s_p1.frontend_den  = plan.den_ba;
            s_p1.frontend_kind = (uint8_t)plan.kind_ba;
        }
    }
    s_p1.guard_drops_at_lock = 0u;
    s_p1.starve_frames_at_lock = 0u;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    s_p1.frontend_ovf_at_lock = 0u;
    s_p1.frontend_udf_at_lock = 0u;
#endif
    s_p1.fault_mask = 0u;
    s_p1.state = P1_COMMAND;
}

void audio_app_asrc_p1_reject( void )
{
    /* A late transport/codec failure must not masquerade as a command that is
     * merely still waiting for a ratio.  Keep its epoch/t=0 for diagnosis. */
    if( s_p1.state == P1_COMMAND || s_p1.state == P1_WAIT_RATIO ||
        s_p1.state == P1_SETTLING )
    {
        s_p1.state = P1_REJECTED;
    }
}

/* Called only after asrc_reset() has invalidated the live engine.  This gate
 * prevents a ratio publication from the pre-command stream being counted as
 * the new command's lock event. */
static void p1_note_engine_reset( void )
{
    if( s_p1.state == P1_COMMAND ) { s_p1.state = P1_WAIT_RATIO; }
}

/* Called at the invalid->valid transition after the reset-at-lock counter
 * baselines have been taken. */
static void p1_note_ratio_lock( const asrc_t* a )
{
    if( !asrc_p1_engine_selected( a ) || s_p1.state != P1_WAIT_RATIO ) { return; }

    s_p1.ratio_lock_ms = GetTicks() - s_p1.command_ms;
    /* These are the selected direction's ARMED facts after the command's transport restart,
     * not a fresh query of a later command.  The tag is a static coefficient
     * label owned by asrc_audio_path, so retaining its pointer costs no copy or
     * dynamic storage. */
    s_p1.frontend_num  = ASRC_P1_DIRECTION_AB ? asrc_audio_path_ab_fixed_rate_num()
                                                  : asrc_audio_path_ba_fixed_rate_num();
    s_p1.frontend_den  = ASRC_P1_DIRECTION_AB ? asrc_audio_path_ab_fixed_rate_den()
                                                  : asrc_audio_path_ba_fixed_rate_den();
    s_p1.frontend_kind = (uint8_t)asrc_audio_path_frontend_kind_dir( ASRC_P1_DIRECTION_AB );
    s_p1.frontend_tag  = ( s_p1.frontend_kind != (uint8_t)ASRC_FRONTEND_NONE )
                       ? asrc_audio_path_frontend_tag() : "";
    s_p1.frontend_armed = 1u;
    s_p1.guard_drops_at_lock = a->dbg_guard_drops;
    s_p1.starve_frames_at_lock = a->dbg_starve_frames;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    s_p1.frontend_ovf_at_lock = a->dbg_intermediate_overflow;
    s_p1.frontend_udf_at_lock = a->dbg_intermediate_underrun;
#endif
    s_p1.state = P1_SETTLING;
}

/* Called once per pull of the APP_MEAS_DIR-selected engine, after the read-only
 * Q50 detector has updated.  It reads Q50's established hold criterion rather
 * than introducing a second servo tuning law. */
static void p1_note_control_sample( const asrc_t* a )
{
    if( !asrc_p1_engine_selected( a ) ||
        ( s_p1.state != P1_SETTLING &&
        s_p1.state != P1_CONTROL_READY &&
        s_p1.state != P1_QUALITY_READY ) ) { return; }

    const uint8_t harm = p1_harm_mask( a );
    if( harm != 0u )
    {
        s_p1.fault_mask |= harm;
        s_p1.state = P1_FAULT;
        return;
    }

    /* Continue watching harm after either ready event, but each readiness
     * timestamp is immutable once recorded. */
    if( s_p1.state == P1_QUALITY_READY ) { return; }

    /* A Q52 experiment may deliberately hold the loop while it recentres. A
     * held loop cannot be called settled; wait until normal control resumes. */
    if( a->ff_freeze || a->corr_hold ) { return; }

    /* Continuous-fill becomes part of the live controller 250 ms after ratio
     * lock.  Do not publish either readiness event before that final control
     * topology is active; otherwise a host could capture across the handover. */
    if( s_cfill_en && !a->prod_phase_ready ) { return; }

    if( s_p1.state == P1_SETTLING && s_q50_lock_hold_ms >= Q50_LOCK_HOLD_MS )
    {
        s_p1.control_ready_ms = GetTicks() - s_p1.command_ms;
        s_p1.state = P1_CONTROL_READY;
    }

    /* Timestamp the first pull inside the fixed 1.5e-5 band. The centred
     * strend representation has resolution below the threshold, so no external
     * measurement or compensating time hold is required. */
    /* P0 two-stage FF: quality is not guaranteed on the provisional stage-1 FF. */
    if( s_p0r_armed &&
        ( !a->p0r.done || ( ( GetTicks() - a->p0r.done_ms ) < P0R_QUIET_MS ) ) ) { return; }
    if( s_p1.state == P1_CONTROL_READY &&
        s_q50_strend_dbg >= -Q50_STEP_TREND_TOL &&
        s_q50_strend_dbg <=  Q50_STEP_TREND_TOL )
    {
        s_p1.quality_ready_ms = GetTicks() - s_p1.command_ms;
        s_p1.quality_ready_strend = s_q50_strend_dbg;
        s_p1.state = P1_QUALITY_READY;
    }
}

void audio_app_asrc_p1_status( void )
{
    const uint8_t  state = s_p1.state;
    const uint8_t  lock_seen = ( s_p1.ratio_lock_ms != 0u ) ? 1u : 0u;
    const uint8_t  ready_seen = ( s_p1.control_ready_ms != 0u ) ? 1u : 0u;
    const uint8_t  quality_seen = ( s_p1.quality_ready_ms != 0u ) ? 1u : 0u;
    const uint32_t age = ( state != P1_IDLE )
                       ? p1_ms_to_us10( GetTicks() - s_p1.command_ms ) : 0u;
    /* Keep every printf shallow: the constrained Universal MEAS image is close
     * to the linker's stack boundary, and one long variadic formatting call
     * costs considerably more stack than these independently readable records.
     * Every line carries epoch, so a host can join the immutable conversion
     * identity with the timing result without depending on UART cadence. */
    printf(" *MEAS p1 epoch=%lu state=%s path=%s a_hz=%lu b_hz=%lu\n",
           (unsigned long)s_p1.epoch, p1_state_name( state ),
           ASRC_P1_PATH_NAME,
           (unsigned long)s_p1.rate_a_hz, (unsigned long)s_p1.rate_b_hz);
    printf(" *MEAS p1_path epoch=%lu fe_armed=%u fe_kind=%s fe_num=%lu fe_den=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned)s_p1.frontend_armed,
           p1_frontend_kind_name( s_p1.frontend_kind ),
           (unsigned long)s_p1.frontend_num, (unsigned long)s_p1.frontend_den);
    printf(" *MEAS p1_tag epoch=%lu fe_tag=%s\n",
           (unsigned long)s_p1.epoch,
           ( s_p1.frontend_tag != NULL ) ? s_p1.frontend_tag : "" );
    printf(" *MEAS p1_time epoch=%lu age_us10=%lu ratio_lock=%u ratio_lock_us10=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned long)age, (unsigned)lock_seen,
           (unsigned long)p1_ms_to_us10( s_p1.ratio_lock_ms ));
    printf(" *MEAS p1_ready epoch=%lu control_ready=%u control_ready_us10=%lu harm=0x%02X q50_hold=%lu hold_ms=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned)ready_seen,
           (unsigned long)p1_ms_to_us10( s_p1.control_ready_ms ),
           (unsigned)s_p1.fault_mask, (unsigned long)s_q50_lockcnt,
           (unsigned long)s_q50_lock_hold_ms);
    printf(" *MEAS p1_gate epoch=%lu q50_state=%u block=0x%02X activity=0x%02X hold_peak_ms=%lu hold_need_ms=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned)s_q50_state,
           (unsigned)s_q50_gate_block_dbg, (unsigned)s_q50_gate_activity_dbg,
           (unsigned long)s_q50_lock_peak_ms, (unsigned long)Q50_LOCK_HOLD_MS);
    printf(" *MEAS p1_gate_value epoch=%lu fill_dev=%.6f fill_slope=%.6f raw_fill_dev=%.6f\n",
           (unsigned long)s_p1.epoch, (double)s_q50_fill_dev_dbg,
           (double)s_q50_fill_slope_dbg, (double)s_q50_raw_fill_dev_dbg);
    printf(" *MEAS p1_gate_reset epoch=%lu total=%lu last=0x%02X fill=%lu slope=%lu clamp=%lu slew=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned long)s_q50_gate_reset_count,
           (unsigned)s_q50_gate_last_reset_dbg, (unsigned long)s_q50_gate_reset_fill,
           (unsigned long)s_q50_gate_reset_slope, (unsigned long)s_q50_gate_reset_clamp,
           (unsigned long)s_q50_gate_reset_slew);
    printf(" *MEAS p1_gate_event epoch=%lu slew=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned long)s_q50_gate_slew_events);
    printf(" *MEAS p1_quality epoch=%lu quality_ready=%u quality_ready_us10=%lu\n",
           (unsigned long)s_p1.epoch, (unsigned)quality_seen,
           (unsigned long)p1_ms_to_us10( s_p1.quality_ready_ms ));
    printf(" *MEAS p1_trend epoch=%lu strend=%.8e threshold=%.8e\n",
           (unsigned long)s_p1.epoch, (double)s_q50_strend_dbg,
           (double)Q50_STEP_TREND_TOL);
    printf(" *MEAS p1_mode epoch=%lu smooth=%u smooth_beta=%.8e\n",
           (unsigned long)s_p1.epoch, (unsigned)s_step_smooth_en,
           (double)s_step_smooth_beta_tab[s_step_smooth_idx]);
    printf(" *MEAS p1_at_ready epoch=%lu strend=%.8e\n",
           (unsigned long)s_p1.epoch, (double)s_p1.quality_ready_strend);
}

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
#if APP_MEAS_DIR == MEAS_DIR_BA
#define Q55_LOG_N      ( 1u )     // BA image: RAM goes to the stack (see P0K_LOG); one sample kept
#else
#define Q55_LOG_N      ( 64u )
#endif
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
    s_q50_fill_fast = 0.0f; s_q50_fill_slow = 0.0f; s_q50_raw_fill_fast = 0.0f;
    s_q50_step_slow = 0.0f; s_q50_step_vslow = 0.0f; s_q50_strend_dbg = 0.0f;
    s_q50_trend_seeded = 0u;
    s_q50_lockcnt = 0u; s_q50_lock_start_ms = 0u;
    s_q50_lock_hold_ms = 0u; s_q50_lock_peak_ms = 0u;
    s_q50_fill_dev_dbg = 0.0f; s_q50_fill_slope_dbg = 0.0f;
    s_q50_gate_block_dbg = 0u; s_q50_gate_activity_dbg = 0u;
    s_q50_gate_last_reset_dbg = 0u; s_q50_gate_reset_count = 0u;
    s_q50_gate_reset_fill = 0u; s_q50_gate_reset_slope = 0u;
    s_q50_gate_reset_clamp = 0u; s_q50_gate_reset_slew = 0u; s_q50_gate_slew_events = 0u;
    s_q50_pulls = 0u; s_q50_lock_pulls = 0u; s_q50_ho_stepdiff = 0.0f;
    s_q52_start_ms = 0u; s_q52_fill0_latched = 0u;
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
//                    0=tau x1 (ALPHA x1), 1=tau x1.5 (x0.667), 2=tau x2 (x0.5), 3=tau x3 (x0.333),
//                    4..6=ALPHA x3/x6/x12, 7..9=ALPHA x1/64, x1/256, x1/1024
//                    (settled-loop quality diagnostics).
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
    case 7u: return 0.015625f;    case 8u: return 0.00390625f;
    case 9u: return 0.0009765625f;
    default: return 1.0f;
    }
}
void audio_app_asrc_set_servo_mult( uint8_t which, uint8_t code )
{
    if( which == 0u )      { s_q30_kp_code    = ( code < 8u ) ? code : 0u; }
    else if( which == 1u ) { s_q30_alpha_code = ( code < 10u ) ? code : 0u; }
}
float audio_app_asrc_get_kp_mult( void )    { return q30_kp_mult_of( s_q30_kp_code ); }
float audio_app_asrc_get_alpha_mult( void ) { return q30_alpha_mult_of( s_q30_alpha_code ); }
#endif

// Consumer side: produce one block by resampling the ring; stereo to slots 0/1, rest 0.
// On underrun emit silence and hold the read phase until the FIFO refills.
#if ASRC_FF_REFINE
static void p0r_on_ratio_lock( asrc_t* a )
{
    a->p0r.pending = 0u;
    a->p0r.done = 0u;
    a->p0r.fill_seeded = 0u;
#if APP_ASRC_MEAS
    a->p0r.at_ms = 0u;
#endif
#if APP_ASRC_QUIET_TRACK
    a->p0t.active = 0u;
#endif
}

/* Runs in the pull, before the pull reads a->ratio, so this pull already uses the new FF. */
static void p0r_handover( asrc_t* a )
{
    const float r_old = a->ratio;
    float r_new = a->p0r.ratio_new;
    a->p0r.pending = 0u;
    if( r_new <= 0.0f ) { return; }
#if APP_ASRC_MEAS
    /* Step 7 fault knob: a known FF-ratio error at the handover, so the QUIET_TRACK entry guard
     * can be shown to wait while the servo absorbs it.  Added as a separate delta: 1 ppm is only
     * ~8 ULP of (1 + x) but ~8 ULP of r_new too, so r_new * (1 + x) would round x twice.
     * Measured direction only. */
    if( asrc_p1_engine_selected( a ) )
    {
        s_p0b_base = r_new;
        if( s_p0b_tenths != 0 ) { r_new += r_new * ( (float)s_p0b_tenths * 1.0e-7f ); }
        s_p0b_applied = r_new;
    }
#endif
    a->p0r.ratio_old  = r_old;
    a->p0r.target_old = a->fill_target;
    a->ratio = r_new;
    if( a->p0r.fill_seeded )
    {
        const float dev = a->p0r.fill_ema - a->fill_target;
        if( ( dev <= P0R_ADOPT_FRAMES ) && ( dev >= -P0R_ADOPT_FRAMES ) )
        {
            a->fill_target = a->p0r.fill_ema;
        }
    }
    a->corr_lpf   = 0.0f;
    a->step_state = r_new;
#if ASRC_SAMPLE_Q31
    a->step_corr_state = 0.0f;
#endif
#if APP_ASRC_MEAS
    /* The P1 trend detector tracks step - ratio; rebase it so the FF change itself does not
     * read as step motion. */
    if( asrc_p1_engine_selected( a ) )
    {
        s_q50_step_slow  -= ( r_new - r_old );
        s_q50_step_vslow -= ( r_new - r_old );
    }
#endif
    a->p0r.done    = 1u;
    a->p0r.done_ms = GetTicks();
#if APP_ASRC_MEAS
    a->p0r.at_ms   = a->p0r.done_ms - s_p1.command_ms;
#endif
}

void audio_app_asrc_p0_refine_arm( uint8_t on ) { s_p0r_armed = on ? 1u : 0u; }

#if APP_ASRC_MEAS
void   audio_app_asrc_p0_ff_bias_set( int8_t tenths_ppm ) { s_p0b_tenths = tenths_ppm; }
int8_t audio_app_asrc_p0_ff_bias_get( void )              { return s_p0b_tenths; }
#endif

/* App context (clock control); the ratio is stored before the flag the pull tests. */
static void p0r_refine( asrc_t* a, float ratio )
{
    a->p0r.ratio_new = ratio;
    a->p0r.pending = 1u;
}
#if APP_ASRC_HAS_AB_ENGINE
void audio_app_asrc_refine_ratio_ab( float ratio ) { p0r_refine( &s_asrc[ASRC_ENGINE_AB], ratio ); }
#else
void audio_app_asrc_refine_ratio_ab( float ratio ) { (void)ratio; }   /* no A->B engine (BA MEAS) */
#endif
#if APP_B_ROUTE_USES_BA
void audio_app_asrc_refine_ratio_ba( float ratio ) { p0r_refine( &s_asrc[ASRC_ENGINE_BA], ratio ); }
#endif
#endif

#if APP_ASRC_QUIET_TRACK
void    audio_app_asrc_quiet_track_set( uint8_t mode ) { s_p0t_mode = ( mode < 5u ) ? mode : 0u; }
uint8_t audio_app_asrc_quiet_track_get( void )         { return s_p0t_mode; }

/* MEAS: the P1 engine enters at QUALITY_READY.  Otherwise (product, or the other MEAS
 * direction): the stage-2 handover is done and P0R_QUIET_MS old -- the same hold the P1 gate
 * puts in front of QUALITY_READY, without P1's step-trend test. */
static inline bool p0t_ready( const asrc_t* a )
{
#if APP_ASRC_MEAS
    if( asrc_p1_engine_selected( a ) ) { return s_p1.state == P1_QUALITY_READY; }
#endif
    return a->p0r.done && ( ( GetTicks() - a->p0r.done_ms ) >= P0R_QUIET_MS );
}

/* The MEAS bench states under which the track must not run (see p0r_bench_hold()). */
static inline bool p0t_bench_hold( const asrc_t* a )
{
#if APP_ASRC_MEAS
    return p0r_bench_hold( a ) || a->corr_hold;
#else
    return p0r_bench_hold( a );
#endif
}

/* entry + offset, normalised; |off| < 2^30 so no 64-bit arithmetic is needed. */
static asrc_q31_step_t p0t_offset( const asrc_p0t_t* q, int32_t off )
{
    asrc_q31_step_t e;
    e.whole = q->entry_whole;
    e.frac  = q->entry_frac;
    if( off >= 0 )
    {
        uint32_t f = e.frac + (uint32_t)off;
        if( f >= 0x80000000u ) { f -= 0x80000000u; e.whole++; }
        e.frac = f;
    }
    else
    {
        const uint32_t m = (uint32_t)( -off );
        if( e.frac >= m ) { e.frac -= m; }
        else              { e.frac = e.frac + 0x80000000u - m; e.whole--; }
    }
    return e;
}

/* Every engine, once per pull, after the normal servo has produced *step.  OFF: returns at once. */
static void p0t_step( asrc_t* a, float fill_used, float ratio, asrc_q31_step_t* step )
{
    asrc_p0t_t* const q = &a->p0t;
    const uint8_t mode = s_p0t_mode;
    if( mode == 0u ) { q->active = 0u; return; }
    const p0t_preset_t* const p = &P0T_PICK( a, PRESETS )[mode];
    if( !q->active )
    {
        if( !p0t_ready( a ) || p0t_bench_hold( a ) || !a->p0r.fill_seeded ) { q->entry_ok = 0u; return; }
#if P0_BA_ROUTE
        if( P0_IS_BA( a ) && !p0t_ba_rate_ok() ) { q->entry_ok = 0u; s_p0t_ba_rate_rejects++; return; }
#endif
        const asrc_q31_step_t ref = asrc_q31_step_of( ratio, a->step_corr_state );
        const int32_t gap = (int32_t)( ( ( step->whole - ref.whole ) << 31u ) +
                                       ( step->frac - ref.frac ) );   /* mod 2^32, |gap| < 2^31 */
        const float fdev = a->p0r.fill_ema - a->fill_target;
        const int32_t elsb = P0T_PICK( a, ENTRY_LSB );
        if( gap > elsb || gap < -elsb ||
            fdev > P0T_ENTRY_FILL || fdev < -P0T_ENTRY_FILL ) { q->entry_ok = 0u; return; }
        if( ++q->entry_ok == 1u ) { q->entry_fill0 = a->p0r.fill_ema; }
        if( q->entry_ok < P0T_PICK( a, ENTRY_PULLS ) ) { return; }
        q->entry_ok = 0u;
        q->entry_slope = ( a->p0r.fill_ema - q->entry_fill0 ) *
                            ( P0T_PICK( a, PULL_HZ ) / (float)( P0T_PICK( a, ENTRY_PULLS ) - 1u ) );
        const float eslope = P0T_PICK( a, ENTRY_SLOPE );
        if( q->entry_slope > eslope || q->entry_slope < -eslope )
        {
            q->slope_rejects++;   /* walking fill: the step is not the true one yet */
            return;
        }
        q->entry_gap = gap;
        q->entry_whole = step->whole;
        q->entry_frac  = step->frac;
        q->ratio = ratio;
        q->setp  = a->p0r.fill_ema;
        q->acc = 0.0f; q->integ = 0.0f; q->tgt = 0.0f; q->e = 0.0f;
        q->out = 0; q->n = 0u; q->rc = 0u;
        q->updates = 0u; q->ramps = 0u; q->aw = 0u;
#if APP_ASRC_MEAS
        q->at_ms = GetTicks() - s_p1.command_ms;
#else
        q->at_ms = GetTicks() - a->p0r.done_ms;   /* product: after the stage-2 handover */
#endif
        q->active = 1u;
    }
    if( ratio != q->ratio || p0t_bench_hold( a ) )
    {
        q->active = 0u;   /* FF or bench state changed under the track: hand back */
        q->aborts++;
        return;
    }
    q->acc += fill_used - q->setp;
    if( ++q->n >= p->m )
    {
        const float e = q->acc / (float)q->n;
        q->acc = 0.0f;
        q->n = 0u;
        q->e = e;
        q->updates++;
        if( e > P0T_ABORT_FRAMES || e < -P0T_ABORT_FRAMES )
        {
            q->active = 0u;
            q->aborts++;
            return;
        }
        /* Anti-windup: while the ramp still lags the previous target by more than one block of
         * ramp budget (m/r LSB), the integrator would only wind up.  Measured 2026-09-28 03:10:
         * a +1.4 frame fill jump without this overshot to -1.5 frames 3 min later (qt_sim.py). */
        const float lag = q->tgt - (float)q->out;
        const float budget = (float)p->m / (float)p->r;
        if( lag <= budget && lag >= -budget ) { q->integ += p->ki * e; }
        else                                  { q->aw++; }
        q->tgt = q->integ + p->kp * e;
    }
    if( ++q->rc >= p->r )
    {
        q->rc = 0u;
        const float d = q->tgt - (float)q->out;
        const int32_t omax = P0T_PICK( a, OUT_MAX );
        if( d > 0.5f && q->out < omax )        { q->out++; q->ramps++; }
        else if( d < -0.5f && q->out > -omax ) { q->out--; q->ramps++; }
    }
    *step = p0t_offset( q, q->out );
}

#if !APP_ASRC_MEAS
/* Product telemetry, one line per engine after its `fe=` line.  Silent while mode is 0, so an
 * image with the track compiled in but not selected prints exactly what it printed before. */
static void p0t_dbg_print( const char* name, const asrc_t* a )
{
    const asrc_p0t_t* const q = &a->p0t;
    if( s_p0t_mode == 0u ) { return; }
    printf(" QT %s mode=%u handover=%u active=%u at_ms=%lu out=%ld e=%.3f upd=%lu aborts=%lu"
           " slope_rejects=%lu gap=%ld\n",
           name, (unsigned)s_p0t_mode, (unsigned)a->p0r.done, (unsigned)q->active,
           (unsigned long)q->at_ms, (long)q->out, (double)q->e, (unsigned long)q->updates,
           (unsigned long)q->aborts, (unsigned long)q->slope_rejects, (long)q->entry_gap );
}
#endif
#endif

static void asrc_pull( asrc_t* a, int32_t* dst )
{
#if ASRC_SAMPLE_Q31
    /*
     * This engine's own blended-row scratch (asrc_poly_q31.inc).  Resolved once per pull
     * rather than per frame, and by comparison instead of pointer arithmetic: sizeof(asrc_t)
     * is not a power of two, so `a - &s_asrc[0]` would put a division in the block.  Both
     * arms are compile-time addresses, so this is a compare and a select.
     */
#if APP_B_ROUTE_USES_BA && APP_ASRC_HAS_AB_ENGINE
    int32_t* const ceff = ( a == &s_asrc[ASRC_ENGINE_BA] ) ? s_ceff_q31[ASRC_ENGINE_BA]
                                                           : s_ceff_q31[ASRC_ENGINE_AB];
#elif APP_B_ROUTE_USES_BA
    (void)a;
    int32_t* const ceff = s_ceff_q31[ASRC_ENGINE_BA];   /* the only engine and the only row */
#else
    int32_t* const ceff = s_ceff_q31[ASRC_ENGINE_AB];
#endif
#endif
    const uint32_t t0_cnt = nora_high_res_timer_get_count();   // telemetry: time this pull
#if APP_ASRC_STAGE_PROFILE
    // Block-local stage totals; peak-held into a->dbg_stg_*_max at the end of this pull.
    uint32_t stg_coef = 0u, stg_mac = 0u, stg_conv = 0u;
    uint32_t stg_ph = 0u, stg_bl = 0u, stg_st = 0u;
#endif

#if ASRC_FF_REFINE
    if( a->p0r.pending && ( a->ratio > 0.0f ) && !p0r_bench_hold( a ) ) { p0r_handover( a ); }
#endif
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
    /* Take one bounded producer publication snapshot.  Never spin here: in a
     * BA build this consumer can preempt the producer while seq is odd.  The
     * previous coherent pair is at most one producer batch old and remains a
     * safe observation for the slow fill controller. */
    const uint16_t prod_seq0 = a->prod_pub_seq;
    __asm__ volatile ( "" ::: "memory" );
    const uint32_t prod_pub_wr   = a->prod_pub_wr;
    const uint32_t prod_pub_tick = a->prod_pub_tick;
    __asm__ volatile ( "" ::: "memory" );
    const uint16_t prod_seq1 = a->prod_pub_seq;
    if( prod_seq0 == prod_seq1 && ( prod_seq0 & 1u ) == 0u && prod_pub_tick != 0u )
    {
        a->prod_cons_wr    = prod_pub_wr;
        a->prod_cons_tick  = prod_pub_tick;
        a->prod_cons_valid = 1u;
    }
    else if( prod_seq0 != 0u || prod_seq1 != 0u )
    {
        a->dbg_prod_snapshot_miss++;
    }
    const uint32_t wr_now = a->wr;                 // live data-availability/overflow snapshot

#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    /* Ratio publication and the first active consumer pull are separated by a
     * route-dependent producer phase; a front end may additionally publish an
     * intermediate chunk in that interval. Re-centering at ratio lock therefore
     * leaves a startup fill error on direct, 1:1 LPF, and fixed-rate routes.
     * Snap here, after everything visible to the first pull has arrived and
     * before any valid output or phase advance. This remains single-writer for
     * rd and is not Q56's repeated TRACK-time re-centering. */
    if( ratio > 0.0f && a->startup_recenter_pending )
    {
        a->rd = wr_now - (uint32_t)a->fill_target;
        a->frac = 0.0f;
#if ASRC_SAMPLE_Q31
        a->frac_q31 = 0u;
#endif
        a->startup_recenter_pending = 0u;
    }
#endif

#if APP_ASRC_MEAS
    /* MEAS_FREEZE_RECENTER_BEGIN: apply once at the consumer's data snapshot.
     * A foreground snap can accumulate step*BLOCK producer frames before the
     * next pull (48/64 frames at step 3/4), overflowing a high setpoint ring.
     * The command waits for this acknowledgement before accepting a capture.
     * Counters and the starve-safe fill law are deliberately preserved. */
    if( ratio > 0.0f && a->freeze_recenter_pending )
    {
        __asm__ volatile ( "" ::: "memory" );
        const float requested_step = a->freeze_recenter_step;
        a->freeze_base_step = requested_step;
        a->freeze_step = requested_step;
        a->rd = wr_now - (uint32_t)a->fill_target;
        a->frac = 0.0f;
#if ASRC_SAMPLE_Q31
        a->frac_q31 = 0u;
#endif
        __asm__ volatile ( "" ::: "memory" );
        a->freeze_recenter_pending = 0u;   // ACK: all cursor/step stores are complete
    }
    /* MEAS_FREEZE_RECENTER_END */
#endif

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

    float fill_used = fill;
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
    const float fill_ma = s_q14_fill_sum * ( 1.0f / 64.0f );
    if( !s_cfill_en ) { fill_used = ( s_q14_use_ma ) ? fill_ma : fill; }
#endif

    /*
     * De-staircase the FIFO occupancy with the producer's measured EFFECTIVE rate.  The old Q40
     * experiment used APP_MEAS_FS_A_HZ and APP_BLOCK_FRAMES here.  That is correct only for a direct
     * path: 48 -> 8, for example, actually pushes a repeating 5/5/6-frame stream at 16 kHz after its
     * /3 front end.  Treating those callbacks as 16 frames at 48 kHz left a false sawtooth in the
     * servo and occasional -100 dB-class close-in modulation.
     *
     * The producer-side 250 ms calibration counts the frames that really entered this engine and
     * measures their elapsed wall time.  phase therefore advances in this FIFO's frame units for
     * direct, 1:1 LPF, fixed-decimator, composed, AB, and BA routes alike.  A one-batch cap prevents
     * a stopped producer from inventing frames.  Product builds always use this once ready; MEAS can
     * still disable it with *af0100 for causal comparisons.
     */
#if APP_ASRC_MEAS
    const uint8_t use_continuous_fill = s_cfill_en;
#else
    const uint8_t use_continuous_fill = 1u;
#endif
    const uint8_t prod_phase_ready = a->prod_phase_ready;
#if APP_ASRC_MEAS && ASRC_SAMPLE_Q31
    float   p0k_phase  = -1.0f;   /* stays -1 when the continuous-fill term is not used this pull */
    uint8_t p0k_age    = 255u;
    uint8_t p0k_late   = 255u;
    {
        const uint32_t late = t0_cnt - s_p0k_legb_isr_tick;
        p0k_late = ( late >= ( 255u << 7 ) ) ? 255u : (uint8_t)( late >> 7 );
    }
    float   p0k_cphase = 0.0f;
    uint8_t p0k_pub    = 0xFFu;
    uint8_t p0k_defer  = 0u;
#endif
    __asm__ volatile ( "" ::: "memory" );
    if( use_continuous_fill && prod_phase_ready && a->prod_cons_valid )
    {
        const uint32_t pub_lag  = wr_now - a->prod_cons_wr;
        const uint32_t pub_fill = a->prod_cons_wr - a->rd;
        /* A one-publication fallback is coherent and useful.  Reject anything
         * older: after a reset or repeated collision, unsigned pointer
         * subtraction could otherwise look like an enormous valid fill. */
        if( pub_lag <= (uint32_t)a->prod_phase_batch_max &&
            pub_fill <= ( ASRC_FIFO_FRAMES - 4u ) )
        {
            const uint32_t elapsed_ticks = nora_high_res_timer_get_count() - a->prod_cons_tick;
            float phase = (float)elapsed_ticks * a->prod_phase_rate_per_tick;
            const float cap = (float)a->prod_phase_batch_max;
#if ASRC_SAMPLE_Q31
            float consumer_phase = (float)a->frac_q31 * 4.656612873077392578125e-10f;
#else
            float consumer_phase = a->frac;
#endif
#if APP_ASRC_MEAS
            if( !s_cfill_consumer_phase_en ) { consumer_phase = 0.0f; }
#endif
#if APP_ASRC_MEAS
#if P0_BA_ROUTE
            const bool hw_ba = P0_IS_BA( a );
#else
            const bool hw_ba = false;
#endif
            if( s_cfill_hw_instant_en && ( ASRC_IS_AB_ENGINE( a ) || hw_ba ) )
            {
                /* Evaluate the producer at the instant rd/frac stand for.  A publication made
                 * during the deferral (the other leg's callback) is newer than that instant, so
                 * the phase may go negative, by at most one batch.  Each engine is timed on the
                 * grid of the leg its pull runs on: A->B on leg B, B->A on leg A. */
                const int32_t defer = hw_ba ? asrc_clock_control_lega_defer_ticks()
                                            : asrc_clock_control_legb_defer_ticks();
                phase -= (float)defer * a->prod_phase_rate_per_tick;
#if ASRC_SAMPLE_Q31
                p0k_defer = ( defer >= (int32_t)( 255u << 7 ) ) ? 255u : (uint8_t)( defer >> 7 );
#endif
                if( phase < -cap )      { phase = -cap; }
                else if( phase > cap )  { phase = cap; }
            }
            else
#endif
            if( phase < 0.0f )      { phase = 0.0f; }
            else if( phase > cap )  { phase = cap; }
            /* Occupancy is producer position minus the complete consumer
             * position.  `rd` is only its integer part; omitting frac leaves
             * a 0..1-frame sawtooth in the servo input and phase-modulates the
             * resampler step.  Keep safety guards on integer wr-rd above. */
            fill_used = (float)pub_fill + phase - a->prod_phase_center - consumer_phase;
#if APP_ASRC_MEAS && ASRC_SAMPLE_Q31
            p0k_phase  = phase;
            p0k_age    = ( elapsed_ticks >= ( 255u << 7 ) ) ? 255u : (uint8_t)( elapsed_ticks >> 7 );
            p0k_cphase = consumer_phase;
            p0k_pub    = (uint8_t)pub_fill;
            /* P0 probe (§2 step 5): the two terms folded into fill_used, captured on the SAME
             * pull as the p0q base/corr/ratio snapshot below so a kick can be attributed to
             * producer phase motion vs. consumer fractional phase. Left stale (previous hit's
             * values) on a miss pull below -- read dbg_prod_snapshot_miss's delta between two
             * ?af polls to know whether a miss happened in between; do not clear it (see
             * diag-counter-clear-destroys-evidence). */
            if( asrc_p1_engine_selected( a ) )
            {
                union { float f; uint32_t u; } pph, pcp;
                pph.f = phase; pcp.f = consumer_phase;
                s_p0q_phase_bits          = pph.u;
                s_p0q_consumer_phase_bits = pcp.u;
            }
#endif
        }
        else
        {
            a->dbg_prod_snapshot_miss++;
        }
    }
#if APP_ASRC_MEAS && ASRC_SAMPLE_Q31
    if( asrc_p1_engine_selected( a ) )
    {
        union { float f; uint32_t u; } pfu;
        pfu.f = fill_used;
        s_p0q_fill_used_bits = pfu.u;   // valid every pull (hit: continuous; miss/off: raw fallback)
    }
#endif

    // Per-block step = feed-forward ratio, fill error trims it (proportional; holds ~target).
    float step;
#if ASRC_SAMPLE_Q31
    /* Preserve the small correction before it reaches the Q31 smoother.  Forming
     * ratio*(1+corr), slewing that absolute binary32 value, and only then
     * splitting it loses slow motion below one ULP near a step of 2 or 6.
     * These two locals carry base and correction separately through the rest of
     * the pull; their sum is only for telemetry and the readiness detector. */
    float control_step_base = 0.0f;
    float control_step_corr = 0.0f;
#endif
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
        if( a->step_state <= 0.0f )
        {
            a->step_state = ratio;   // defensive seed
#if ASRC_SAMPLE_Q31
            a->step_corr_state = 0.0f;
#endif
        }

        // Gentle fill correction -> 1st-order LPF -> slew-limited applied step. This keeps the
        // loop bandwidth ~1 Hz so it tracks ppm clock drift without FM-ing the audio.
#if ASRC_FF_REFINE
        if( !a->p0r.fill_seeded ) { a->p0r.fill_ema = fill_used; a->p0r.fill_seeded = 1u; }
        else { a->p0r.fill_ema += P0R_EMA_ALPHA( a ) * ( fill_used - a->p0r.fill_ema ); }
#endif
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
        const float lo = ratio * ASRC_STEP_LO;                   // safety envelope
        const float hi = ratio * ASRC_STEP_HI;
#if ASRC_SAMPLE_Q31
        float target_corr = ratio * corr_used;
        const float lo_corr = lo - ratio;
        const float hi_corr = hi - ratio;
#if APP_ASRC_MEAS
        // Q29 Q10: clamp-hit iff the PRE-clamp target actually fell outside the envelope (read-only;
        // the clamp itself below is unchanged).
        q29_clamp_hit = ( target_corr < lo_corr || target_corr > hi_corr ) ? 1u : 0u;
#endif
        if( target_corr < lo_corr ) { target_corr = lo_corr; }
        if( target_corr > hi_corr ) { target_corr = hi_corr; }

        float delta = target_corr - a->step_corr_state;          // slew-limit in correction domain
#if APP_ASRC_MEAS
        // Q29 Q10: slew-hit iff the PRE-slew delta actually exceeded the limit (read-only).
        q29_slew_hit = ( delta > ASRC_STEP_SLEW || delta < -ASRC_STEP_SLEW ) ? 1u : 0u;
#endif
        if( delta >  ASRC_STEP_SLEW ) { delta =  ASRC_STEP_SLEW; }
        if( delta < -ASRC_STEP_SLEW ) { delta = -ASRC_STEP_SLEW; }
        a->step_corr_state += delta;
        control_step_base = ratio;
        control_step_corr = a->step_corr_state;
        step = control_step_base + control_step_corr;
        a->step_state = step;                                   // compatibility telemetry / Mode-K latch
#else
        float target_step = ratio * ( 1.0f + corr_used );
#if APP_ASRC_MEAS
        q29_clamp_hit = ( target_step < lo || target_step > hi ) ? 1u : 0u;
#endif
        if( target_step < lo ) { target_step = lo; }
        if( target_step > hi ) { target_step = hi; }

        float delta = target_step - a->step_state;
#if APP_ASRC_MEAS
        q29_slew_hit = ( delta > ASRC_STEP_SLEW || delta < -ASRC_STEP_SLEW ) ? 1u : 0u;
#endif
        if( delta >  ASRC_STEP_SLEW ) { delta =  ASRC_STEP_SLEW; }
        if( delta < -ASRC_STEP_SLEW ) { delta = -ASRC_STEP_SLEW; }
        a->step_state += delta;
        step = a->step_state;
#endif
    }

#if APP_ASRC_MEAS
    // Q50 Fast-Acquisition state machine (ACQUIRE lock-detect + bumpless HANDOVER). Read-only wrt the
    // servo (KP/corr-LPF/clamp/slew already ran above); it only decides the applied-step path below.
    // Runs for both baseline and candidate so the lock-time metric is identical; actuation is gated by
    // s_q50_en at the step-smoothing/mute sites below.
    /* Fine convergence remains observable after the coarse Q50 handover.
     * Track the correction about `ratio`, not the absolute step near 1.0. In
     * exact arithmetic the slow-minus-vslow result is identical because the
     * same constant cancels. In float, however, absolute-value EMA increments
     * can fall below one ULP and strand the two states tens of ppm apart even
     * after the real servo has stopped moving. The centred form preserves the
     * established 1.5e-5 criterion without a false numerical floor. */
    if( asrc_p1_engine_selected( a ) )
    {
        const float step_correction = step - ratio;
        if( !s_q50_trend_seeded )
        {
            s_q50_step_slow = step_correction;
            s_q50_step_vslow = step_correction;
            s_q50_strend_dbg = 0.0f;
            s_q50_trend_seeded = 1u;
        }
        else
        {
            s_q50_step_slow  += 0.005f  * ( step_correction - s_q50_step_slow );   /* tau ~0.15 s */
            s_q50_step_vslow += 0.0007f * ( step_correction - s_q50_step_vslow );  /* tau ~1.05 s */
            s_q50_strend_dbg = s_q50_step_slow - s_q50_step_vslow;
        }

        if( s_q50_state == Q50_ACQUIRE )
        {
            if( s_q50_pulls == 0u )   // seed EMAs to the first sample (no warm-up ramp from zero)
            { s_q50_fill_fast = fill_used; s_q50_fill_slow = fill_used; s_q50_raw_fill_fast = fill;
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
#if ASRC_SAMPLE_Q31
                   a->step_corr_state = 0.0f;
#endif
                  s_q52_ratio0 = ratio; s_q52_fill0 = fill;
                  s_q52_start_ms = GetTicks();
              } }
            s_q50_pulls++;
            /* Q52: (re)latch fill0 after the post-recenter startup transient has
             * settled.  Wall time is intentional: BA's consumer pull rate follows
             * A, unlike AB's fixed SPI2-controller rate. */
            if( s_q50_en && s_q52_seed_en && !s_q52_fill0_latched &&
                ( GetTicks() - s_q52_start_ms ) >= Q52_SKIP_MS )
            {
                s_q52_fill0 = fill;
                s_q52_fill0_latched = 1u;
            }
            /* The gate observes the same continuous fill the servo regulates (fill_used).  The raw
             * integer occupancy wr-rd differs from it by the producer-phase term, which settles at a
             * per-entry constant of ~0 or ~6 frames: with the servo holding fill_used on target, a
             * raw-fill gate then sat at dev 5.2..6.5 against Q50_FILL_TOL and missed CONTROL_READY
             * (A44k1_B24, 2/40 entries).  Raw fill stays as ring-safety telemetry; the physical ring
             * limits are still enforced by the guard-drop / starve counters (P1 harm). */
            s_q50_fill_fast += 0.02f  * ( fill_used - s_q50_fill_fast );   /* ~0.1 s EMA */
            s_q50_fill_slow += 0.005f * ( fill_used - s_q50_fill_slow );   /* ~0.4 s EMA (slope ref) */
            s_q50_raw_fill_fast += 0.02f * ( fill - s_q50_raw_fill_fast );
            s_q50_raw_fill_dev_dbg = s_q50_raw_fill_fast - a->fill_target;
            const float dev    = s_q50_fill_fast - a->fill_target;
            const float slope  = s_q50_fill_fast - s_q50_fill_slow;
            /* Coarse handover remains a fill-centering decision.  Fine step
             * convergence is a separate P1 event using Q50_STEP_TREND_TOL. */
            s_q50_fill_dev_dbg = dev;
            s_q50_fill_slope_dbg = slope;
            s_q50_gate_activity_dbg = (uint8_t)(
                ( ( dev <= -Q50_FILL_TOL || dev >= Q50_FILL_TOL ) ? 0x01u : 0u ) |
                ( ( slope <= -Q50_SLOPE_TOL || slope >= Q50_SLOPE_TOL ) ? 0x02u : 0u ) |
                ( q29_clamp_hit ? 0x04u : 0u ) |
                ( q29_slew_hit  ? 0x08u : 0u ) );
            /* A slew hit only says the limiter is doing its normal job. It can
             * persist in the steady limit cycle, so requiring zero slew made
             * CONTROL_READY structurally unreachable (A48->B8: ~40 events/s).
             * Clamp remains a blocker because it means the safety envelope is
             * active. Keep full activity telemetry for diagnosis. */
            s_q50_gate_block_dbg = s_q50_gate_activity_dbg & 0x07u;
            if( q29_slew_hit ) { s_q50_gate_slew_events++; }
            if( s_q50_gate_block_dbg == 0u )
            {
                const uint32_t now_ms = GetTicks();
                if( s_q50_lockcnt == 0u ) { s_q50_lock_start_ms = now_ms; }
                s_q50_lockcnt++;
                s_q50_lock_hold_ms = now_ms - s_q50_lock_start_ms;
                if( s_q50_lock_hold_ms > s_q50_lock_peak_ms )
                {
                    s_q50_lock_peak_ms = s_q50_lock_hold_ms;
                }
            }
            else
            {
                /* A periodic blocker can be gone again by the time foreground `?as`
                 * samples the current mask.  Latch the reason that actually ended a
                 * non-empty clean run, plus per-reason counts, without changing the
                 * detector or its hold decision. */
                if( s_q50_lockcnt != 0u )
                {
                    s_q50_gate_reset_count++;
                    s_q50_gate_last_reset_dbg = s_q50_gate_block_dbg;
                    if( s_q50_gate_block_dbg & 0x01u ) { s_q50_gate_reset_fill++; }
                    if( s_q50_gate_block_dbg & 0x02u ) { s_q50_gate_reset_slope++; }
                    if( s_q50_gate_block_dbg & 0x04u ) { s_q50_gate_reset_clamp++; }
                    if( s_q50_gate_block_dbg & 0x08u ) { s_q50_gate_reset_slew++; }
                }
                s_q50_lockcnt = 0u;
                s_q50_lock_start_ms = 0u;
                s_q50_lock_hold_ms = 0u;
            }

            if( s_q50_en && s_q52_seed_en )
            {
                /* Q56: while held, re-centre the FIFO every muted pull -- rd tracks wr so the startup burst
                 * is dropped as it arrives and fill cannot wind the (held) step. */
                a->rd = wr_now - (uint32_t)a->fill_target;
                a->frac = 0.0f;
#if ASRC_SAMPLE_Q31
                a->frac_q31 = 0u;
#endif
                s_q52_dfill    = fill - s_q52_fill0;   /* telemetry */
                s_q52_est_step = ratio;
                if( ( GetTicks() - s_q52_start_ms ) >= Q52_WIN_MS )
                {
                    /* Experimental release: step==the held feed-forward ratio, fill centred, burst
                     * absorbed.  The held ratio is an estimate, not necessarily the true clock ratio. */
                    a->corr_hold = 0u; a->ff_freeze = 0u;
                    a->step_state     = s_q52_ratio0;
#if ASRC_SAMPLE_Q31
                    a->step_corr_state = 0.0f;
#endif
                    s_q50_ho_stepdiff = ( a->step_smooth_st > 0.0f ) ? ( step - a->step_smooth_st ) : 0.0f;
                    a->step_smooth_st = s_q52_ratio0;   /* bumpless: TRACK smoothing starts at the true step */
#if ASRC_SAMPLE_Q31
                    a->step_smooth_corr = 0.0f;
#endif
                    s_q50_lock_pulls  = s_q50_pulls;
                    s_q50_state       = Q50_TRACK;
                }
            }
            else if( s_q50_lock_hold_ms >= Q50_LOCK_HOLD_MS )
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
                    /* producer_fs_meas = delta_wr / delta_t (wr frame-granular, delta_t from the
                     * high-res timer, both sampled at pull start -> clean window, no pull-count
                     * quantization). est_step = producer_fs_meas / selected_consumer_fs. */
                    const uint32_t dt10 = nora_high_res_timer_elapsed_us_x10( s_q51_t0 );  // us x10
                    if( dt10 > 0u )
                    {
                        const float dt_s = (float)dt10 * 1.0e-7f;
                        const float consumer_fs = Q51_CONSUMER_FS_HZ;
                        const float est  = ( consumer_fs > 0.0f )
                                         ? (float)( wr_now - s_q51_wr0 ) / ( dt_s * consumer_fs )
                                         : 0.0f;
                        s_q51_est_step = est;
                        /* Sanity: only seed if the estimate is within +-0.5 % of the feed-forward ratio;
                         * otherwise a startup-transient/quantization outlier would make TRACK worse, so fall
                         * back to the plain Q50 bumpless seed. */
                        if( ratio > 0.0f && est > ratio * 0.995f && est < ratio * 1.005f )
                        {
                            s_q51_corr_seed   = est / ratio - 1.0f;
                            a->step_state     = est;
#if ASRC_SAMPLE_Q31
                            a->step_corr_state = est - ratio;
                            a->step_smooth_st = ratio;
#else
                            a->step_smooth_st = est;
#endif
#if ASRC_SAMPLE_Q31
                            a->step_smooth_corr = a->step_corr_state;
#endif
                            a->corr_lpf       = s_q51_corr_seed;
                            s_q51_applied     = 1u;
                        }
                    }
                }
                if( !s_q51_applied && s_q50_en )
                {
#if ASRC_SAMPLE_Q31
                    a->step_smooth_st = ratio;
                    a->step_smooth_corr = a->step_corr_state;
#else
                    a->step_smooth_st = step;  /* Q50 bumpless seed */
#endif
                }
                s_q50_lock_pulls  = s_q50_pulls;
                s_q50_state       = Q50_TRACK;
            }
        }
    }
#endif

#if APP_ASRC_MEAS
    /* P1 shares Q50's already-established read-only fill-centre detector;
     * this adds no control branch and is silent until the host asks `?as`. */
    p1_note_control_sample( a );
#endif

    // Q44 (shipping default): low-pass the applied step to strip the producer limit cycle (>1 Hz)
    // before it drives the read phase (and thus the carrier FM). DC/slow rate correction passes ->
    // fill authority kept. On the freeze/Mode-K bench path above this leaves the frozen step untouched
    // only when the MEAS toggle disables it (bench default OFF); the shipping build always smooths.
#if ASRC_SAMPLE_Q31
    /* Keep the Q31 arm's absolute step split through phase accumulation.  At a base near 2.0,
     * beta=0.005 makes an absolute binary32 EMA blind to target changes below about 100 ULP:
     * it sticks, then moves in an integer-ULP burst.  The correction is close to zero, so the
     * same EMA has ample relative precision.  Do not add base+corr until telemetry; the Q31
     * phase path converts the two terms independently below. */
    float step_base = ( control_step_base > 0.0f ) ? control_step_base : step;
    float step_corr = ( control_step_base > 0.0f ) ? control_step_corr : 0.0f;
#endif
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
        if( asrc_p1_engine_selected( a ) && s_q50_en &&
            s_q50_state == Q50_ACQUIRE && !s_q52_seed_en ) { ss_en = 0u; }
#endif
        if( ss_en )
        {
#if ASRC_SAMPLE_Q31
            if( a->step_smooth_st <= 0.0f )
            {
                a->step_smooth_st = step_base;
                a->step_smooth_corr = step_corr;
            }
            /* Both terms on the right stay small when the rate epoch is
             * stable.  In particular, do not form step_base+step_corr before
             * subtracting the smoother base: that recreates the absolute-float
             * dead zone this split representation is meant to remove. */
            const float target_corr = ( step_base - a->step_smooth_st ) + step_corr;
            a->step_smooth_corr += ss_beta * ( target_corr - a->step_smooth_corr );
            step_base = a->step_smooth_st;
            step_corr = a->step_smooth_corr;
            step = step_base + step_corr; /* diagnostic float only; Q31 does not consume this sum */
#else
            if( a->step_smooth_st <= 0.0f ) { a->step_smooth_st = step; }   // seed on first use
            a->step_smooth_st += ss_beta * ( step - a->step_smooth_st );
            step = a->step_smooth_st;
#endif
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
    /*
     * CANDIDATE 4 (APP_ASRC_Q31_HOIST_RDFRAC): keep the consumer read cursor and read phase in
     * LOCALS for the whole block instead of in the engine struct.  ASRC_RD_ / ASRC_FRAC_ are the
     * only names the POLY frame body uses for these two words, so the two arms cannot drift:
     * with the switch off they expand to the struct members and the generated code is unchanged.
     * The arithmetic is bit-identical either way -- same float32 additions, same repeated
     * `- 1.0f`, same order, same values; only WHERE the two words live between output frames
     * changes.  Written back once, immediately after the loop.
     *
     * a->wr stays a memory read on purpose: the producer ISR updates it mid-block.
     */
#if ASRC_SAMPLE_Q31
#if APP_ASRC_QUIET_TRACK
    asrc_q31_step_t q31_step = asrc_q31_step_of( step_base, step_corr );
    p0t_step( a, fill_used, ratio, &q31_step );
#else
    const asrc_q31_step_t q31_step = asrc_q31_step_of( step_base, step_corr );
#endif
#if APP_ASRC_MEAS
    /* P0 probe: the exact operands and result of the Q31 step conversion for this pull,
     * so a capture's carrier offset can be matched to the integer increment actually used.
     * Read-only; printed by ?af.  Measured engine only (AB, or BA in a BA image). */
    if( asrc_p1_engine_selected( a ) )
    {
        union { float f; uint32_t u; } pb, pc, pr;
        pb.f = step_base; pc.f = step_corr; pr.f = ratio;
        s_p0q_base_bits  = pb.u;
        s_p0q_corr_bits  = pc.u;
        s_p0q_ratio_bits = pr.u;
        s_p0q_whole      = q31_step.whole;
        s_p0q_frac       = q31_step.frac;
        if( q31_step.frac != s_p0q_frac_prev )
        {
            s_p0q_changes++;
            s_p0q_frac_prev = q31_step.frac;
        }
#if P0K_LOG
        if( s_p0k_run && !s_p0k_frozen )
        {
            float fe = ( fill_used - a->fill_target ) * 256.0f;
            if( fe > 32767.0f )       { fe = 32767.0f; }
            else if( fe < -32768.0f ) { fe = -32768.0f; }
            const int16_t fe16 = (int16_t)fe;
            const uint32_t j = s_p0k_pulls % P0K_DECIM;
            if( s_p0k_pulls == 0u ) { s_p0k_frac0 = q31_step.frac; s_p0k_whole0 = q31_step.whole; }
            /* frac0 is the first logged pull of the whole run, so dfrac is comparable across dumps. */
            if( j == 0u )            { s_p0k_lo = fe16; s_p0k_hi = fe16; }
            else
            {
                if( fe16 < s_p0k_lo ) { s_p0k_lo = fe16; }
                if( fe16 > s_p0k_hi ) { s_p0k_hi = fe16; }
            }
            if( j == ( P0K_DECIM - 1u ) )
            {
                p0k_rec_t* const r = &s_p0k[( s_p0k_pulls / P0K_DECIM ) % P0K_N];
                float ph = p0k_phase * 4096.0f;
                float cp = p0k_cphase * 65536.0f;
                if( ph > 65534.0f ) { ph = 65534.0f; }
                if( cp > 65535.0f ) { cp = 65535.0f; }
                if( cp < 0.0f )     { cp = 0.0f; }
                int32_t df = (int32_t)( q31_step.frac - s_p0k_frac0 );
                if( q31_step.whole != s_p0k_whole0 ) { df = ( q31_step.whole > s_p0k_whole0 ) ? 32767 : -32768; }
                if( df > 32767 )  { df = 32767; }
                if( df < -32768 ) { df = -32768; }
                r->fe    = fe16;
                r->fe_lo = s_p0k_lo;
                r->fe_hi = s_p0k_hi;
                r->ph    = ( p0k_phase < 0.0f ) ? 0xFFFFu : (uint16_t)ph;
                r->cp    = (uint16_t)cp;
                r->pub   = p0k_pub;
                r->age   = p0k_age;
                r->late  = p0k_late;
                r->pad2  = p0k_defer;
                r->dfrac = (int16_t)df;
            }
            s_p0k_pulls++;
        }
#endif
    }
#endif
#endif
#if APP_ASRC_Q31_HOIST_RDFRAC
    uint32_t rd_hoist   = a->rd;
    uint32_t frac_q31_hoist = a->frac_q31;
#define ASRC_RD_    rd_hoist
#define ASRC_FRAC_Q31_ frac_q31_hoist
#else
#define ASRC_RD_    a->rd
#if ASRC_SAMPLE_Q31
#define ASRC_FRAC_Q31_ a->frac_q31
#endif
#endif
#define ASRC_FRAC_  a->frac
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
                    ( asrc_p1_engine_selected( a ) && s_q50_en &&
                      s_q50_state == Q50_ACQUIRE ) ? 1u : 0u;
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
        if( ( a->wr - ASRC_RD_ ) >= ( ASRC_POLY_AHEAD + 1u ) )   // rd-MH .. rd+AHEAD window available
        {
#if (ASRC_POLY_METHOD == ASRC_POLY_Q31)
            /* One blended coefficient row per OUTPUT FRAME, shared by all 16
             * channels, then one plain 30-tap Q31 dot per channel.  No pair, no
             * d, no union window -- so, unlike the float pair path, this costs
             * the same at step == 1 and at step != 1.  The mask on out[] is the
             * s24-left store; sacr.l already rounded and saturated. */
#if APP_ASRC_STAGE_PROFILE
            const uint32_t stg_t0 = nora_high_res_timer_get_count();
#endif
            float          wb;
            const uint32_t pq    = asrc_q31_phase_of_fixed( ASRC_FRAC_Q31_, &wb );
            const uint32_t wbase = ( ASRC_RD_ - ASRC_POLY_MH ) & ASRC_FIFO_MASK;
#if APP_ASRC_STAGE_PROFILE
            /*
             * wb -> Q31 hoisted so the probe can land between the phase/weight bookkeeping
             * and the row blend.  Hoisted UNDER THE GATE ONLY: doing it unconditionally would
             * change the APP_ASRC_STAGE_PROFILE=0 image, and that image is the one every
             * absolute deadline figure (E1) came from.  Same value, same order -- the
             * arithmetic is untouched either way.
             */
            const int32_t  wbq_p   = asrc_q31_wb( wb );
            /* Counted BEFORE the probe so it is charged to `rest`, not to `ph`. */
            a->dbg_memo_tot++;
            if( a->dbg_prev_valid && ( pq == a->dbg_prev_pq ) && ( wbq_p == a->dbg_prev_wbq ) )
            {
                a->dbg_memo_hit++;
            }
            a->dbg_prev_pq    = pq;
            a->dbg_prev_wbq   = wbq_p;
            a->dbg_prev_valid = 1u;
            const uint32_t stg_t0b = nora_high_res_timer_get_count();
            ( (volatile int32_t*)ceff )[ASRC_CEFF_OWNER_SLOT] = (int32_t)(uintptr_t)a;
            mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( pq ),
                                     ASRC_POLY_Q31_ROW( pq + 1u ),
                                     ceff, ASRC_POLY_M, wbq_p );
#elif APP_ASRC_Q31_OPT_KERNELS
            /*
             * ONE IMAGE, FOUR PATHS (`*au 00`..`*au 03`, one bit per candidate).  The
             * test-and-branch is paid by BOTH arms once per output frame, so it cancels
             * in the difference apart from the branch itself, and what it does not
             * cancel it charges to the optimised arm -- the conservative direction for
             * a GO decision.
             */
            if( ( s_q31_opt & ASRC_Q31_OPT_BIT_BLEND ) != 0u )
            {
                mchp_asrc_q31_blend_row_opt( ASRC_POLY_Q31_ROW( pq ),
                                             ASRC_POLY_Q31_ROW( pq + 1u ),
                                             ceff, ASRC_POLY_M, asrc_q31_wb( wb ) );
            }
            else
            {
                mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( pq ),
                                         ASRC_POLY_Q31_ROW( pq + 1u ),
                                         ceff, ASRC_POLY_M, asrc_q31_wb( wb ) );
            }
#elif APP_ASRC_REAR_QUAD_BLEND
            ASRC_Q31_QUAD_BLEND_FN( ASRC_POLY_Q31_ROW( pq ),
                                          ASRC_POLY_Q31_ROW( pq + 1u ),
                                          ASRC_POLY_Q31_ROW( pq + 2u ),
                                          ceff, ASRC_POLY_M, asrc_q31_wb( wb ), asrc_q31_wq2( wb ) );
#elif APP_ASRC_REAR_LIN4
            mchp_asrc_q31_blend_row_lin4( ASRC_POLY_Q31_ROW( pq ),
                                          ASRC_POLY_Q31_ROW( pq + 1u ),
                                          ceff, ASRC_POLY_M, asrc_q31_wb( wb ) );
#else
            mchp_asrc_q31_blend_row( ASRC_POLY_Q31_ROW( pq ),
                                     ASRC_POLY_Q31_ROW( pq + 1u ),
                                     ceff, ASRC_POLY_M, asrc_q31_wb( wb ) );
#endif
#if APP_ASRC_STAGE_PROFILE
            const uint32_t stg_t1 = nora_high_res_timer_get_count();
#endif
#if APP_ASRC_STAGE_PROFILE
            if( ( (volatile int32_t*)ceff )[ASRC_CEFF_OWNER_SLOT] != (int32_t)(uintptr_t)a )
            {
                a->dbg_ceff_steal++;
            }
#endif
#if APP_ASRC_Q31_OPT_KERNELS
            if( ( s_q31_opt & ASRC_Q31_OPT_BIT_ROW16 ) != 0u )
            {
                /*
                 * The s24-left mask is folded into this kernel's store, so the
                 * conversion loop MUST NOT run for this arm.  Running it would be
                 * arithmetically harmless -- the mask is idempotent -- but it would
                 * leave the cost being measured inside the measurement, which is the
                 * one mistake that cannot be spotted afterwards in the numbers.
                 */
                mchp_asrc_q31_row16_opt( &a->ch[0][wbase],
                                         (uint32_t)( ASRC_FIFO_PHYS * sizeof( asrc_samp_t ) ),
                                         ceff, ASRC_POLY_M, out, ASRC_CH );
            }
            else
            {
                mchp_asrc_q31_row16( &a->ch[0][wbase],
                                     (uint32_t)( ASRC_FIFO_PHYS * sizeof( asrc_samp_t ) ),
                                     ceff, ASRC_POLY_M, out, ASRC_CH );
                for( uint8_t c = 0u; c < ASRC_CH; c++ )
                {
                    out[c] = asrc_q31_to_slot( out[c] );
                }
            }
#else
            mchp_asrc_q31_row16( &a->ch[0][wbase],
                                 (uint32_t)( ASRC_FIFO_PHYS * sizeof( asrc_samp_t ) ),
                                 ceff, ASRC_POLY_M, out, ASRC_CH );
#if APP_ASRC_STAGE_PROFILE
            const uint32_t stg_t2 = nora_high_res_timer_get_count();
#endif
            for( uint8_t c = 0u; c < ASRC_CH; c++ )
            {
                out[c] = asrc_q31_to_slot( out[c] );
            }
#endif
#if APP_ASRC_STAGE_PROFILE
            {
                const uint32_t stg_t3 = nora_high_res_timer_get_count();
                stg_coef += ( stg_t1 - stg_t0 );
                stg_ph   += ( stg_t0b - stg_t0 );
                stg_bl   += ( stg_t1 - stg_t0b );
                stg_mac  += ( stg_t2 - stg_t1 );
                stg_conv += ( stg_t3 - stg_t2 );
            }
#endif
#elif (ASRC_POLY_METHOD == ASRC_POLY_CEFF)
            float    c_eff[ASRC_POLY_M];
            uint32_t wbase;
            asrc_poly_phase_ceff( ASRC_RD_, ASRC_FRAC_, &wbase, c_eff );   // blend coeff ONCE, shared
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
            asrc_poly_phase( ASRC_RD_, ASRC_FRAC_, &ph );   // c0/c1/wb/wbase shared by all channels
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
            asrc_poly_phase( ASRC_RD_, ASRC_FRAC_, &ph );
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
            asrc_poly_phase_nearest( ASRC_RD_, ASRC_FRAC_, &coeff, &wbase );
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
            asrc_poly_phase( ASRC_RD_, ASRC_FRAC_, &ph );   // c0/c1/wb/wbase shared by all channels
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
            asrc_poly_phase( ASRC_RD_, ASRC_FRAC_, &ph );   // c0/c1/wb/wbase shared by all channels
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
            asrc_poly_phase( ASRC_RD_, ASRC_FRAC_, &ph );   // phase calc ONCE, shared by all channels
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
#if (ASRC_POLY_METHOD == ASRC_POLY_Q31)
            asrc_q31_phase_advance( &ASRC_RD_, &ASRC_FRAC_Q31_, q31_step );
#else
            ASRC_FRAC_ += step;
            while( ASRC_FRAC_ >= 1.0f )
            {
                ASRC_FRAC_ -= 1.0f;
                ASRC_RD_++;
            }
#endif
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
#if APP_ASRC_STAGE_PROFILE
        const uint32_t stg_t4 = nora_high_res_timer_get_count();
#endif
#if APP_ASRC_MEAS
        // Q50 ACQUIRE: mute the output while the fast-acquire transient settles (audio comes up clean
        // only at HANDOVER->TRACK). Servo/FIFO advance normally above; only the emitted slots are zeroed.
        const uint8_t q50_mute =
            ( asrc_p1_engine_selected( a ) && s_q50_en &&
              s_q50_state == Q50_ACQUIRE ) ? 1u : 0u;
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
#if APP_ASRC_STAGE_PROFILE
        stg_st += ( nora_high_res_timer_get_count() - stg_t4 );
#endif
        d += APP_SLOTS_PER_FS;
    }
#if APP_ASRC_Q31_HOIST_RDFRAC
    a->rd   = rd_hoist;
    a->frac_q31 = frac_q31_hoist;
#endif
#if ASRC_SAMPLE_Q31
    /* Legacy trace/console fields remain readable without putting a float phase add in the
     * per-output hot path.  This conversion runs once per output block. */
    a->frac = (float)a->frac_q31 * 4.656612873077392578125e-10f;
#endif
#undef ASRC_RD_
#undef ASRC_FRAC_
#if ASRC_SAMPLE_Q31
#undef ASRC_FRAC_Q31_
#endif

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
#if APP_ASRC_STAGE_PROFILE
    // Peak-hold the three BLOCK TOTALS independently. They can therefore come from three
    // different blocks, which is the right conservative reading for an optimisation estimate
    // and the reason their sum may slightly exceed any single pull.
    if( stg_coef > a->dbg_stg_coef_max ) { a->dbg_stg_coef_max = stg_coef; }
    if( stg_mac  > a->dbg_stg_mac_max  ) { a->dbg_stg_mac_max  = stg_mac;  }
    if( stg_conv > a->dbg_stg_conv_max ) { a->dbg_stg_conv_max = stg_conv; }
    if( stg_ph   > a->dbg_stg_ph_max   ) { a->dbg_stg_ph_max   = stg_ph;   }
    if( stg_bl   > a->dbg_stg_bl_max   ) { a->dbg_stg_bl_max   = stg_bl;   }
    if( stg_st   > a->dbg_stg_st_max   ) { a->dbg_stg_st_max   = stg_st;   }
    if( stg_coef < a->dbg_stg_coef_min ) { a->dbg_stg_coef_min = stg_coef; }
    if( stg_mac  < a->dbg_stg_mac_min  ) { a->dbg_stg_mac_min  = stg_mac;  }
    if( stg_conv < a->dbg_stg_conv_min ) { a->dbg_stg_conv_min = stg_conv; }
    if( stg_ph   < a->dbg_stg_ph_min   ) { a->dbg_stg_ph_min   = stg_ph;   }
    if( stg_bl   < a->dbg_stg_bl_min   ) { a->dbg_stg_bl_min   = stg_bl;   }
    if( stg_st   < a->dbg_stg_st_min   ) { a->dbg_stg_st_min   = stg_st;   }
#endif
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
            printf( " ASRC build: sample arm=%s poly=%d coeff=%s frontend=%s meas=%s\n",
#if ASRC_SAMPLE_Q31
                    "Q31",
#else
                    "float",
#endif
                    (int)ASRC_POLY_METHOD,
                    // Which table is actually resident, so the board says it rather than
                    // the reader inferring it from the preset.  The two arms have separate
                    // knobs: ASRC_Q31_COEFF_STORAGE (default RAM) and ASRC_COEFF_STORAGE.
#if ASRC_SAMPLE_Q31
                    ASRC_POLY_Q31_STORAGE_NAME,
#else
                    ( ASRC_COEFF_STORAGE == ASRC_COEFF_STORAGE_FLASH ) ? "flash" : "ram",
#endif
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
    /* PUSH SELFTESTS: BOOT-TIME ONLY, AND THE GATE SAYS SO STRUCTURALLY.
     *
     * Both tests below use s_asrc[ASRC_ENGINE_AB] as their scratch, and
     * asrc_push_frames_selftest() does not merely borrow it -- it drives the real engine
     * (writes a->wr / a->rd / a->ratio, pushes into the live ring, then zero-clears all
     * ASRC_CH channels).  That is only safe before any audio ISR has run.
     *
     * This function is not boot-only: the boot sequence calls it twice, a console `*ar` rate
     * re-commit re-enters it via asrc_transport_reset(), and `*as 07` calls it directly.  On
     * the `*ar` path leg A keeps streaming (it carries the AB push and the whole BA pull), so
     * running these here would zero a live ring under the pull ISR.  Giving them dedicated
     * scratch is not affordable: a second asrc_t is ~26 KB.  So they are gated -- and gated
     * on "has an audio ISR ever executed", not on "is this the first call", because a call
     * count is only the same guard by accident and stops being one the moment the boot
     * sequence gains a third reset.
     *
     * The skip is PRINTED, not silent: a capture log must never be ambiguous about whether a
     * selftest passed or never ran.  (The Q31 poly selftest below needs no gate -- since
     * 2026-09-06 it owns its scratch outright.) */
    if( !asrc_audio_path_isr_has_started() )
    {
#if ASRC_HAVE_FIXED_PUSH_BLOCK16
        asrc_push_block_selftest();
#endif
#if ( APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8 ) && \
    ( ASRC_HISTORY_LAYOUT != ASRC_HISTORY_TILE8 )
        asrc_push_frames_selftest();
#endif
    }
    else
    {
#if ASRC_HAVE_FIXED_PUSH_BLOCK16
        printf( " ASRC push16/stereo + push8/TDM selftest: skipped"
                " (runtime reset -- audio ISRs already live)\n" );
#endif
#if ( APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8 ) && \
    ( ASRC_HISTORY_LAYOUT != ASRC_HISTORY_TILE8 )
        printf( " ASRC push_frames selftest: skipped"
                " (runtime reset -- audio ISRs already live)\n" );
#endif
    }
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY)
#if ASRC_SAMPLE_Q31
    asrc_poly_build_q31();      // one-time: generate the Q31 polyphase table (app context)
#if APP_ASRC_Q31_SELFTEST
    // Coefficients + vectors own their scratch and run every time.  The 16-channel part
    // (check_16ch) borrows s_asrc[0].ch since P1 (2026-10-01) and therefore runs only while
    // !asrc_audio_path_isr_has_started(); with the ISRs live (`*ar`, `*as 07`) it is skipped
    // and the line below says so.  Borrowing that ring WHILE streaming is what made the
    // second run FAIL intermittently on 2026-09-06.  Failures are latched in s_poly_q31_fail.
    s_poly_q31_fails = asrc_poly_q31_selftest();
    if( asrc_poly_q31_selftest_16ch_skipped() )
    {
        printf( " ASRC Q31 poly 16ch selftest: skipped (runtime reset -- audio ISRs already live)\n" );
    }
    // `ties` is the count of sacr.l rounding ties (exactly +-256 in LSB24 terms)
    // between the assembly kernels and the portable C reference.  A tie is not a
    // failure, but a count that starts moving is worth seeing, so it is printed
    // rather than hidden -- same policy as the Q31 front-end selftest.
#if APP_ASRC_REAR_QUAD_BLEND
    printf( " ASRC Q31 rear blend: QUADRATIC (research, APP_ASRC_REAR_QUAD_BLEND=1); quad selftest %lu/%lu taps exact\n",
            (unsigned long)( asrc_q31_quad_selftest_cases() - asrc_q31_quad_selftest_fails() ),
            (unsigned long)asrc_q31_quad_selftest_cases() );
    asrc_q31_bench_quad_print();
#endif
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
#if APP_ASRC_REAR_ISOLATION_SELFTEST
    /* THE REAR PER-CHANNEL ORACLE.  Gated on "has an audio ISR ever executed", exactly like the
     * push selftests above and for the same reason: it borrows s_asrc[ASRC_ENGINE_AB]'s ring, and
     * this function is not boot-only (a `*ar` rate re-commit and `*as 07` both re-enter it with leg
     * A streaming).  The skip is PRINTED -- a log that silently omits the line cannot be told from
     * one where the test passed.
     *
     * AFTER the Q31 poly selftest on purpose: that one proves the coefficient table and the kernels
     * against the host model, so if the table is wrong this line does not also fail and turn one
     * fault into two findings. */
    if( !asrc_audio_path_isr_has_started() )
    {
        const uint32_t rear_fails = asrc_rear_isolation_selftest();
        printf( " ASRC Q31 rear isolation selftest: %s (%lu of %u channels,"
                " %lu comparisons, fails %lu)\n",
                ( rear_fails == 0u ) ? "pass" : "FAIL",
                (unsigned long)asrc_rear_isolation_selftest_channels(),
                (unsigned)ASRC_CH,
                (unsigned long)asrc_rear_isolation_selftest_checks(),
                (unsigned long)rear_fails );
        if( rear_fails != 0u )
        {
            const asrc_rear_iso_fail_t* const rf = asrc_rear_isolation_selftest_fail();
            /* Everything needed to reproduce the case on a host: which case, which channel, which
             * output frame, which coefficient row and blend weight, and the XOR -- because "one
             * LSB" and "a different channel's sample" look identical as two decimal numbers. */
            printf( "  %s: case %u(%s) ch %u other %u frame %u phase %lu wbq %lu"
                    " got %ld want %ld xor %08lX\n",
                    rf->what, (unsigned)rf->kase, asrc_rear_iso_case_name( rf->kase ),
                    (unsigned)rf->channel, (unsigned)rf->other, (unsigned)rf->frame,
                    (unsigned long)rf->phase, (unsigned long)rf->wbq,
                    (long)rf->got, (long)rf->want, (unsigned long)rf->xor_bits );
        }
    }
    else
    {
        printf( " ASRC Q31 rear isolation selftest: skipped"
                " (runtime reset -- audio ISRs already live)\n" );
    }
#endif
#if APP_ASRC_Q31_OPT_KERNELS
    /* The A/B measurement is only meaningful if the fast pair computes the SAME samples,
     * so prove it here rather than assume it.  Own local scratch (no engine, no s_ceff_q31
     * row), so this needs no boot-time gate and re-proves on every `*ar` re-commit. */
    s_q31_opt_fails = asrc_q31_opt_selftest();
    printf( " ASRC Q31 opt-kernel bit-exactness selftest: %s (%lu cases, blend fails %lu,"
            " row16 fails %lu, of which nch %lu)\n",
            ( s_q31_opt_fails == 0u ) ? "pass" : "FAIL",
            (unsigned long)s_q31_opt_cases,
            (unsigned long)s_q31_opt_blend_fails,
            (unsigned long)s_q31_opt_row16_fails,
            (unsigned long)s_q31_opt_nch_fails );
    if( s_q31_opt_fails != 0u )
    {
        printf( "  %s: case %lu idx %lu got %ld want %ld\n",
                s_q31_opt_fail.what, (unsigned long)s_q31_opt_fail.caze,
                (unsigned long)s_q31_opt_fail.idx,
                (long)s_q31_opt_fail.got, (long)s_q31_opt_fail.expect );
    }
    /* Selectable = proven, per candidate: one bad kernel must not block pricing the
     * other, and a delta between two DIFFERENT filters must never be measurable. */
    printf( "  selectable: blend=%s row16=%s (`*au SS`, SS = bitmask)\n",
            ( ( s_q31_opt_allow & ASRC_Q31_OPT_BIT_BLEND ) != 0u ) ? "yes" : "REFUSED",
            ( ( s_q31_opt_allow & ASRC_Q31_OPT_BIT_ROW16 ) != 0u ) ? "yes" : "REFUSED" );
    s_q31_opt = (uint8_t)( s_q31_opt & s_q31_opt_allow );
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
#if APP_ASRC_HAS_AB_ENGINE
    asrc_reset( &s_asrc[ASRC_ENGINE_AB] );   // ratio invalid until a feed-forward value is set
#endif
#if APP_B_ROUTE_USES_BA
    asrc_reset( &s_asrc[ASRC_ENGINE_BA] );
#endif
#if APP_ASRC_MEAS
    p1_note_engine_reset();
#endif
}

// Apply a feed-forward ratio to one instance. On the invalid->valid transition (first valid
// ratio after (re)start), CENTRE the FIFO: while silent the producer kept filling the ring
// (consumer held rd), so fill sits near full. Snap rd = wr - target and reset the phase so
// resampling starts at mid-fill (target latency) instead of draining full->target over ~2 s.
// App context; the consumer ISR holds rd while ratio<=0, so this rd write is race-free (the
// producer only advances wr). Set ratio LAST so the next pull sees the centred rd.
// Ratio-aware fill setpoint for one instance: the look-ahead one pull needs plus ONE PRODUCER
// BLOCK of lower reserve (asrc_fill_law), never below the calibrated default and never so deep
// that the producer's overflow guard could fire.  ASRC_FILL_JITTER no longer appears here -- it
// holds the upper bound only, see the law.
// Called at ratio-lock only (app context, consumer holds rd while ratio<=0) -- not in the hot path.
static void asrc_set_fill_target( asrc_t* a, float step )
{
    /*
     * The setpoint comes from asrc_fill_law() -- the SAME law the *ar pair gate refuses with, fed
     * the same nominal rates.  One authority: a setpoint built from a different expression than the
     * gate checks is exactly how a setpoint of 64 came to be accepted for a pull needing 57.
     *
     * `step` (the measured lock ratio) is used only when the nominal rates are not knowable here.
     * That is a DEGRADED path, not a second law: it reads floor()+1 off a value that can sit below
     * nominal, so it can still under-count by one frame where the product is an integer -- which is
     * the case the nominal path exists for.  The lock line marks it `?`.
     *
     * AND THE NOMINAL RATES ARE CROSS-CHECKED AGAINST `step`, because "not knowable" is not a state
     * the rate source can report.  wm8904_get_rate_hz() returns a cached 48000 until the codec is
     * configured (wm8904_note_configured_rate() has no callers, so the cache is only written by the
     * configure path itself), i.e. a stale rate arrives as a plausible number, not as a 0 -- and a
     * stale nominal rate produces a silently WRONG setpoint, which is the whole failure class this
     * change exists to remove.  The measured lock ratio is useless as a BOUND (that is why the law
     * does not use it) but it is an excellent CONSISTENCY CHECK: it agrees with the nominal step to
     * within clock ppm, while any confusion of rates is at least 8 % away (12 vs 11.025 kHz is the
     * closest pair in the menu).  2 % therefore separates them with orders of magnitude to spare.
     */
    asrc_fill_law_t law;
    uint32_t fs_in = 0u, num = 1u, den = 1u, fs_out = 0u;
    bool nominal =
        asrc_audio_path_engine_nominal( ASRC_IS_AB_ENGINE( a ),
                                        &fs_in, &num, &den, &fs_out )
        && asrc_fill_law( fs_in, num, den, fs_out, &law );
    if( nominal && ( step > 0.0f ) )
    {
        /* Cross-multiplied so there is no division: eff_num/eff_den vs step becomes eff_num vs
         * step*eff_den, with the 2 % band taken on eff_num.  Same test, and it matters on the
         * compact part -- AK128 ships with ~1.3 kB of program memory left. */
        const float eff_num = (float)fs_in  * (float)num;
        const float eff_den = (float)den    * (float)fs_out;
        const float dev     = eff_num - ( step * eff_den );
        const float tol     = 0.02f * eff_num;
        if( ( dev > tol ) || ( dev < -tol ) )
        {
            nominal = false;   /* stale or wrong nominal rates -- fall back and say so */
        }
    }
    if( !nominal )
    {
        const uint32_t look_m = (uint32_t)( step * (float)( APP_BLOCK_FRAMES - 1u ) ) + 1u;
        const uint32_t r_m    = look_m + ASRC_POLY_AHEAD + 1u;
        const uint32_t req_m  = r_m + ASRC_FILL_BLOCK_RESERVE;
        law.look_safe    = (uint16_t)look_m;
        law.r_safe       = (uint16_t)r_m;
        law.set_required = (uint16_t)req_m;
        law.set          = (uint16_t)( ( req_m < (uint32_t)ASRC_FILL_TARGET )
                                       ? (uint32_t)ASRC_FILL_TARGET : req_m );
        law.fits         = ( law.set <= (uint32_t)ASRC_FILL_TARGET_MAX );
    }
    a->fill_law_nominal = nominal ? 1u : 0u;
    a->dbg_R_safe       = law.r_safe;

    /* dbg_R keeps its pre-A1 meaning on purpose: floor of the LOCK-TIME ratio, so `hr` in the
     * periodic line stays comparable with logs recorded before this change. See dbg_R_safe. */
    const uint32_t look = (uint32_t)( step * (float)( APP_BLOCK_FRAMES - 1u ) );
    a->dbg_R = (uint16_t)( look + ASRC_POLY_AHEAD + 1u );
    /* T1: the starve-feasibility bound for THIS ratio (see dbg_jmax). Integer, no float compare;
     * clamps to 0 rather than wrapping when the look-ahead alone exceeds the ring. */
    {
        const uint32_t spent = (uint32_t)APP_BLOCK_FRAMES + 20u + look;
        a->dbg_jmax = ( (uint32_t)ASRC_FIFO_FRAMES > spent )
                    ? (uint8_t)( ( (uint32_t)ASRC_FIFO_FRAMES - spent ) / 2u )
                    : 0u;
    }
    uint32_t need = law.set;
    if( need > ASRC_FILL_TARGET_MAX )
    {
        /* The ring cannot hold this ratio's look-ahead AND its lower reserve.  The *ar gate REFUSES
         * such a pair now (asrc_fill_slack_fits -> law.fits), so reaching here means the pair was
         * never gated -- a boot default, or a rate set by something other than *ar.  Clamp as a
         * best effort (fewer, shallower dropouts than an unclamped setpoint that would trip the
         * overflow guard) and flag it; the honest fix for such a ratio is a decimating front end,
         * see asrc_audio_path_ab_fixed_rate_den(). */
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
            uint8_t consumer_recenter = 0u;
#if APP_ASRC_48K_TO_8_INTEGRATION
            if( ASRC_IS_AB_ENGINE( a ) ) { consumer_recenter = 1u; }
#endif
#if APP_ASRC_RUNTIME_48K_TO_8
            /* Runtime routes can change between direct, 1:1 LPF, and fixed-rate
             * front ends. Their first-pull producer phase is observable exactly
             * at the consumer, so use that phase-independent one-shot for every
             * active runtime engine instead of the old direct-path +28 pre-bias. */
            const bool runtime_engine =
                ASRC_IS_AB_ENGINE( a )
#if APP_B_ROUTE_USES_BA
                || ( a == &s_asrc[ASRC_ENGINE_BA] )
#endif
                ;
            if( runtime_engine )
            {
                lock_off = 0;
                consumer_recenter = 1u;
            }
#endif
#if APP_ASRC_MEAS
            /* Q57/Q58 remain available to legacy, non-runtime MEAS profiles.
             * Runtime profiles use the consumer-time one-shot below. */
            if( !consumer_recenter )
            {
                const uint32_t el10 = nora_high_res_timer_elapsed_us_x10( a->dbg_rx_tick );
                float phase = (float)el10 * ( APP_MEAS_FS_A_HZ * 1.0e-7f );   /* nominal fs_A; block-phase only */
                if( phase < 0.0f ) { phase = 0.0f; }
                else if( phase > (float)APP_BLOCK_FRAMES ) { phase = (float)APP_BLOCK_FRAMES; }
                if( s_q58_pa_en ) { lock_off = (int16_t)( s_q58_center - phase ); s_q58_last_phase = phase; s_q58_last_off = lock_off; }
                else              { lock_off = s_q57_lock_off; }
            }
#endif
            a->rd += (uint32_t)(int32_t)lock_off;
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
            a->startup_recenter_pending = consumer_recenter;
#endif
        }
#endif
        a->frac       = 0.0f;
#if ASRC_SAMPLE_Q31
        a->frac_q31   = 0u;
#endif
        a->corr_lpf   = 0.0f;
        a->step_state = ratio;   // start the slew-limited step at the measured ratio
#if ASRC_SAMPLE_Q31
        a->step_corr_state = 0.0f;
#endif
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
#if ASRC_FF_REFINE
        p0r_on_ratio_lock( a );      // P0: stage-1 FF is live; stage 2 not yet handed over
#endif
#if APP_ASRC_MEAS
        if( asrc_p1_engine_selected( a ) )
        {
            q50_on_ratio_lock();     // Q50: (re)arm the selected MEAS direction
            s_q55log_n = 0u; s_q55log_ctr = 0u; s_q55log_arm = 1u;   // Q55: arm the early-boot logger
            /* P1 starts only after the selected engine's rate-restart reset and
             * takes its harm-counter baselines after silent priming discards. */
            p1_note_ratio_lock( a );
        }
#endif
        /*
         * One line per ratio-lock carrying the DESIGN values, so the periodic line can stay
         * health-only (P1). Everything here is constant between restarts: printing it every
         * ~2 s is what made the periodic line unreadable. `!` = the setpoint did not fit the
         * ring (clamped); `!J` = starve cannot reach 0 at this ratio (J > Jmax), which is the
         * derived form of the acceptance criterion rather than a per-part constant.
         * App context, once per (re)start.
         *
         * `Rs=` is the reserve law's R (dbg_R_safe), which is what `set` is built from; `R=` stays
         * the lock-ratio floor that `hr` is measured against, so both halves of a log are readable.
         * A `?` on Rs means the law fell back to the measured ratio because the nominal rates were
         * not available -- see asrc_set_fill_target().  Field APPENDED, not replaced: existing
         * parsers key on `R=` and `set=`, which still read the same as before.
         */
        printf(" ASRC %s lock: step=%.5f R=%lu Rs=%lu%s set=%lu%s J=%u/Jmax=%u%s\n",
               ASRC_IS_AB_ENGINE( a ) ? "AB" : "BA",
               (double)ratio,
               (unsigned long)a->dbg_R,
               (unsigned long)a->dbg_R_safe,
               a->fill_law_nominal ? "" : "?",
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
#if APP_ASRC_HAS_AB_ENGINE
void audio_app_asrc_set_ratio_ab( float ratio ) { asrc_apply_ratio( &s_asrc[ASRC_ENGINE_AB], ratio ); }
#else
/* BA MEAS: no A->B engine.  The clock control still publishes both directions every 20 ms; the
 * A->B half has nowhere to go.  Kept as a no-op so the shared callers stay unconditional. */
void audio_app_asrc_set_ratio_ab( float ratio ) { (void)ratio; }
#endif

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

#if APP_ASRC_Q31_OPT_KERNELS
/*
 * Kernel-pair select for the A/B measurement (`*au 00` baseline, `*au 01` optimised).
 * No reset, no gate, no ratio disturbance: both pairs produce identical samples, proven
 * at boot by asrc_q31_opt_selftest(), so a switch mid-stream is inaudible and the two
 * arms can be timed minutes apart in the same running image.
 */
void audio_app_asrc_q31_opt_set( uint8_t mask )
{
    /* Drop bits the selftest did not prove.  The console refuses them with a message
     * first; this is the backstop that makes the refusal structural. */
    s_q31_opt = (uint8_t)( mask & s_q31_opt_allow & ASRC_Q31_OPT_BIT_ALL );
}
uint8_t audio_app_asrc_q31_opt_get( void )    { return s_q31_opt; }
uint8_t audio_app_asrc_q31_opt_allow( void )  { return s_q31_opt_allow; }
uint32_t audio_app_asrc_q31_opt_fails( void ) { return s_q31_opt_fails; }
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
    a->freeze_recenter_pending = 0u;
    const float chosen = ( a->step_state > 0.0f ) ? a->step_state
                       : ( a->ratio      > 0.0f ) ? a->ratio : 0.0f;
    if( chosen > 0.0f ) { a->freeze_step = chosen; a->freeze_base_step = chosen; }  // Q3: latch base
    printf(" *MEAS freeze %s: ratio=%.6f corr_lpf=%.2e step_state=%.6f -> freeze=%.6f\n",
           tag, (double)a->ratio, (double)a->corr_lpf,
           (double)a->step_state, (double)a->freeze_step );
}
void audio_app_asrc_freeze( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    asrc_freeze_one( &s_asrc[ASRC_ENGINE_AB], "ab" );
#endif
#if APP_B_ROUTE_USES_BA
    asrc_freeze_one( &s_asrc[ASRC_ENGINE_BA], "ba" );
#endif
}
#if !APP_ASRC_HAS_AB_ENGINE
bool audio_app_asrc_freeze_ratio_ab( void )
{
    printf(" *MEAS ratio-freeze rejected: no A->B engine in this BA MEAS image\n");
    return false;
}
#else
bool audio_app_asrc_freeze_ratio_ab( void )
{
    asrc_t* a = &s_asrc[ASRC_ENGINE_AB];
    const float requested_step = a->ratio;
    if( !( requested_step > 0.0f ) )
    {
        printf(" *MEAS ratio-freeze rejected: no valid A->B ratio\n");
        return false;
    }

    a->freeze_recenter_step = requested_step;
    __asm__ volatile ( "" ::: "memory" );
    a->freeze_recenter_pending = 1u;
    /* Foreground only; interrupts stay enabled. Even at 8 kHz the next
     * consumer block arrives within 2 ms. Bound a stopped transport at 20 ms. */
    const uint32_t started = nora_high_res_timer_get_count();
    while( a->freeze_recenter_pending &&
           nora_high_res_timer_elapsed_us_x10( started ) < 200000u ) { }
    if( a->freeze_recenter_pending )
    {
        a->freeze_recenter_pending = 0u;
        printf(" *MEAS ratio-freeze rejected: consumer acknowledgement timed out\n");
        return false;
    }
    __asm__ volatile ( "" ::: "memory" );
    printf(" *MEAS ratio-freeze ab: ratio=%.7f fill=%lu/%lu -> freeze=%.7f, FIFO centred at consumer pull\n",
           (double)a->freeze_base_step, (unsigned long)a->fill_target,
           (unsigned long)ASRC_FIFO_FRAMES, (double)a->freeze_step );
    return true;
}
#endif
void audio_app_asrc_unfreeze( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    s_asrc[ASRC_ENGINE_AB].freeze_recenter_pending = 0u;
    s_asrc[ASRC_ENGINE_AB].freeze_step = 0.0f;
#endif
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].freeze_recenter_pending = 0u;
    s_asrc[ASRC_ENGINE_BA].freeze_step = 0.0f;
#endif
}
#if APP_ASRC_HAS_AB_ENGINE
float audio_app_asrc_get_freeze_step_ab( void ) { return s_asrc[ASRC_ENGINE_AB].freeze_step; }
#endif

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
#if APP_ASRC_HAS_AB_ENGINE
    asrc_ff_freeze_one( &s_asrc[ASRC_ENGINE_AB], "ab" );
#endif
#if APP_B_ROUTE_USES_BA
    asrc_ff_freeze_one( &s_asrc[ASRC_ENGINE_BA], "ba" );
#endif
}
/* Hi-Fi axis D: hold the FF ratio at the live value times (1 + ppm * 1e-6), as if the feed-forward
 * had been captured with that error or the clocks had drifted that far since.  The fill servo stays
 * live and has to carry the whole offset, so this is the tracking test for "hold FF after
 * acquisition".  MEAS only, measured direction(s) only, like the plain freeze above. */
static void asrc_ff_freeze_offset_one( asrc_t* a, const char* tag, int8_t ppm )
{
    a->ff_frozen_ratio = a->ratio + a->ratio * ( (float)ppm * 1.0e-6f );
    a->ff_freeze       = 1u;
    printf(" *MEAS FF-freeze %s offset %+d ppm: live=%.9f frozen=%.9f\n",
           tag, (int)ppm, (double)a->ratio, (double)a->ff_frozen_ratio );
}
void audio_app_asrc_ff_freeze_offset( int8_t ppm )
{
#if APP_ASRC_HAS_AB_ENGINE
    asrc_ff_freeze_offset_one( &s_asrc[ASRC_ENGINE_AB], "ab", ppm );
#endif
#if APP_B_ROUTE_USES_BA
    asrc_ff_freeze_offset_one( &s_asrc[ASRC_ENGINE_BA], "ba", ppm );
#endif
}
void audio_app_asrc_ff_unfreeze( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    s_asrc[ASRC_ENGINE_AB].ff_freeze = 0u;
#endif
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].ff_freeze = 0u;
#endif
    s_q16_freeze_latched = 0u;   // R16: arm a fresh freeze-epoch latch for the next freeze
    printf(" *MEAS FF-unfreeze (controller back to live ratio)\n");
}
#if APP_ASRC_HAS_AB_ENGINE
float   audio_app_asrc_get_ff_frozen_ratio_ab( void ) { return s_asrc[ASRC_ENGINE_AB].ff_frozen_ratio; }
uint8_t audio_app_asrc_get_ff_freeze_ab( void )       { return s_asrc[ASRC_ENGINE_AB].ff_freeze; }
#endif
// R16 Q16: freeze-age accessors. age_pulls = pulls since the frozen-FF t=0 (0 if not yet frozen).
uint32_t audio_app_asrc_get_q16_pull_ctr( void )     { return s_q16_pull_ctr; }
uint32_t audio_app_asrc_get_q16_freeze_epoch( void ) { return s_q16_freeze_epoch; }
uint32_t audio_app_asrc_get_q16_age_pulls( void )    { return s_q16_freeze_latched ? ( s_q16_pull_ctr - s_q16_freeze_epoch ) : 0u; }
#if APP_ASRC_HAS_AB_ENGINE
float    audio_app_asrc_get_ratio_live_ab( void )    { return s_asrc[ASRC_ENGINE_AB].ratio; }
#endif

// --- R13 Q13: hold corr_lpf constant (fill-servo integration stopped), step path still live ------
void audio_app_asrc_corr_hold( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    s_asrc[ASRC_ENGINE_AB].corr_hold = 1u;
#endif
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].corr_hold = 1u;
#endif
#if APP_ASRC_HAS_AB_ENGINE
    printf(" *MEAS corr-HOLD ab: corr_lpf held=%.3e (fill still moves; step slew/clamp LIVE; NOT Mode-K)\n",
           (double)s_asrc[ASRC_ENGINE_AB].corr_lpf );
#else
    printf(" *MEAS corr-HOLD ba: corr_lpf held=%.3e (fill still moves; step slew/clamp LIVE; NOT Mode-K)\n",
           (double)s_asrc[ASRC_ENGINE_BA].corr_lpf );
#endif
}
void audio_app_asrc_corr_release( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    s_asrc[ASRC_ENGINE_AB].corr_hold = 0u;
#endif
#if APP_B_ROUTE_USES_BA
    s_asrc[ASRC_ENGINE_BA].corr_hold = 0u;
#endif
    printf(" *MEAS corr-RELEASE (fill servo back to live)\n");
}
#if APP_ASRC_HAS_AB_ENGINE
uint8_t audio_app_asrc_get_corr_hold_ab( void ) { return s_asrc[ASRC_ENGINE_AB].corr_hold; }
#endif

// --- R14 Q14: select MA64 vs raw fill for the servo observation (MA runs continuously either way) --
void audio_app_asrc_fill_use_ma( uint8_t on )
{
    s_q14_use_ma = on ? 1u : 0u;
    printf(" *MEAS fill observation = %s (MA64 runs continuously; servo eq unchanged)\n",
           on ? "MA64" : "RAW" );
}
uint8_t audio_app_asrc_get_fill_use_ma_ab( void ) { return s_q14_use_ma; }
// Continuous-fill estimator toggle (servo-observation only; wr/rd/FIFO untouched).
void audio_app_asrc_cfill_en( uint8_t on )
{
    s_cfill_en = on ? 1u : 0u;
    printf(" *MEAS continuous-fill est = %s (servo fill obs only; wr/rd unchanged)\n",
           on ? "ON" : "OFF" );
}
uint8_t audio_app_asrc_get_cfill_en( void ) { return s_cfill_en; }
void audio_app_asrc_cfill_consumer_phase_en( uint8_t on )
{
    s_cfill_consumer_phase_en = on ? 1u : 0u;
    printf(" *MEAS continuous-fill consumer phase = %s (servo fill obs only)\n",
           on ? "ON" : "OFF" );
}
uint8_t audio_app_asrc_get_cfill_consumer_phase_en( void )
{
    return s_cfill_consumer_phase_en;
}
void audio_app_asrc_cfill_hw_instant_en( uint8_t on )
{
    s_cfill_hw_instant_en = on ? 1u : 0u;
    printf(" *MEAS continuous-fill hw instant = %s (servo fill obs: A->B on leg B, B->A on leg A; takes effect next pull)\n",
           on ? "ON" : "OFF" );
    asrc_clock_control_legb_defer_print();
}
uint8_t audio_app_asrc_get_cfill_hw_instant_en( void ) { return s_cfill_hw_instant_en; }

static void asrc_print_prod_phase_status( const char* engine, const asrc_t* a )
{
    const uint32_t cal_us10 = nora_high_res_timer_count_to_us_x10( a->prod_phase_elapsed_ticks );
    printf(" *MEAS cfill: engine=%s enabled=%u ready=%u rate_hz=%.3f center=%.4f"
           " batch_max=%u cal_frames=%lu cal_events=%u cal_us10=%lu snapshot_fallback=%lu"
           " consumer_phase_sub=%u hw_instant=%u\n",
           engine, (unsigned)s_cfill_en, (unsigned)a->prod_phase_ready,
           (double)( a->prod_phase_rate_per_tick * (float)FCY ),
           (double)a->prod_phase_center, (unsigned)a->prod_phase_batch_max,
           (unsigned long)a->prod_phase_frames, (unsigned)a->prod_phase_events,
           (unsigned long)cal_us10, (unsigned long)a->dbg_prod_snapshot_miss,
           (unsigned)s_cfill_consumer_phase_en, (unsigned)s_cfill_hw_instant_en );
    if( s_cfill_hw_instant_en ) { asrc_clock_control_legb_defer_print(); }
#if P0_BA_ROUTE
    if( P0_IS_BA( a ) )
    {
        printf(" *MEAS ba_prod: prod_stamp=%s defer_last=%ld (high-res ticks; *af08 gates it)\n",
               s_cfill_hw_instant_en ? "hw" : "sw", (long)s_p0_ba_prod_defer );
    }
#endif
#if ASRC_SAMPLE_Q31
    if( asrc_p1_engine_selected( a ) )
    {
        printf(" *MEAS p0q: base=0x%08lX corr=0x%08lX ratio=0x%08lX whole=%lu frac=0x%08lX changes=%lu\n",
               (unsigned long)s_p0q_base_bits, (unsigned long)s_p0q_corr_bits,
               (unsigned long)s_p0q_ratio_bits, (unsigned long)s_p0q_whole,
               (unsigned long)s_p0q_frac, (unsigned long)s_p0q_changes );
        /* P0 probe (§2 step 5): fill_used and its two continuous-fill terms, same pull as the
         * p0q line above. snapshot_miss is a->dbg_prod_snapshot_miss's raw cumulative count (do
         * NOT clear it here -- diag-counter-clear-destroys-evidence); take the delta between two
         * ?af polls to know whether a miss landed inside the window between them. phase/cphase
         * are stale (previous hit's value) on a pull that misses. */
        printf(" *MEAS p0q2: fill_used=0x%08lX phase=0x%08lX cphase=0x%08lX snapshot_miss=%lu\n",
               (unsigned long)s_p0q_fill_used_bits, (unsigned long)s_p0q_phase_bits,
               (unsigned long)s_p0q_consumer_phase_bits, (unsigned long)a->dbg_prod_snapshot_miss );
        union { float f; uint32_t u; } po, pn;
        po.f = a->p0r.ratio_old; pn.f = a->ratio;
        printf(" *MEAS p0r: armed=%u done=%u at_ms=%lu r_old=0x%08lX r_now=0x%08lX"
               " tgt_old=%.3f tgt=%.3f fill_ema=%.3f\n",
               (unsigned)s_p0r_armed, (unsigned)a->p0r.done, (unsigned long)a->p0r.at_ms,
               (unsigned long)po.u, (unsigned long)pn.u, (double)a->p0r.target_old,
               (double)a->fill_target, (double)a->p0r.fill_ema );
#if APP_ASRC_QUIET_TRACK
        printf(" *MEAS p0t: mode=%u active=%u at_ms=%lu setp=%.3f out=%ld tgt=%.1f",
               (unsigned)s_p0t_mode, (unsigned)a->p0t.active, (unsigned long)a->p0t.at_ms,
               (double)a->p0t.setp, (long)a->p0t.out, (double)a->p0t.tgt );
        printf(" integ=%.1f e=%.5f n=%u upd=%lu ramps=%lu aborts=%lu gap=%ld aw=%lu\n",
               (double)a->p0t.integ, (double)a->p0t.e, (unsigned)a->p0t.n,
               (unsigned long)a->p0t.updates, (unsigned long)a->p0t.ramps,
               (unsigned long)a->p0t.aborts, (long)a->p0t.entry_gap, (unsigned long)a->p0t.aw );
        printf(" *MEAS p0t_entry: slope=%.5f limit=%.5f slope_rejects=%lu",
               (double)a->p0t.entry_slope, (double)P0T_PICK( a, ENTRY_SLOPE ),
               (unsigned long)a->p0t.slope_rejects );
#if P0_BA_ROUTE
        if( P0_IS_BA( a ) ) { printf(" ba_rate_rejects=%lu", (unsigned long)s_p0t_ba_rate_rejects ); }
#endif
        printf(" ff_bias=%+.1f applied_ppm=%+.3f\n", (double)s_p0b_tenths * 0.1,
               ( s_p0b_base > 0.0f ) ? ( (double)s_p0b_applied / (double)s_p0b_base - 1.0 ) * 1.0e6
                                     : 0.0 );
#endif
    }
#endif
}

void audio_app_asrc_cfill_status( void )
{
#if APP_ASRC_HAS_AB_ENGINE
    asrc_print_prod_phase_status( "AB", &s_asrc[ASRC_ENGINE_AB] );
#endif
#if APP_B_ROUTE_USES_BA
    asrc_print_prod_phase_status( "BA", &s_asrc[ASRC_ENGINE_BA] );
#endif
}

#if APP_ASRC_MEAS && ASRC_SAMPLE_Q31
/* Called from the MEAS capture path (ISR context, same priority as asrc_pull). */
void audio_app_asrc_p0k_capture_arm( void )
{
    s_p0k_mark   = 0xFFFFFFFFu;
    s_p0k_end    = 0u;
    s_p0k_frozen = 0u;   /* resume; history before this arm is kept */
    s_p0k_run    = 1u;
}
void audio_app_asrc_p0k_capture_first( void )
{
    if( s_p0k_run && ( s_p0k_mark == 0xFFFFFFFFu ) ) { s_p0k_mark = s_p0k_pulls; }
}
void audio_app_asrc_p0k_capture_done( void )
{
    if( s_p0k_run ) { s_p0k_end = s_p0k_pulls; s_p0k_frozen = 1u; }
}

/* "?af01": dump the kick log, oldest -> newest.  pull = running AB pull index of the record's last
 * pull (counted from the first arm, paused while frozen); the captured samples start at pull=mark
 * and the capture completed at pull=end.  dfrac is the applied Q31 step fraction minus its value
 * at the first logged pull, in Q31 LSB (header dfrac_lsb=1). */
void audio_app_asrc_p0k_dump( void )
{
#if !P0K_LOG
    printf(" *MEAS p0k: not compiled in this image (BA: RAM goes to the B->A instance)\n");
    printf("*MEAS_END\n");
    return;
#else
    if( !s_p0k_frozen )
    {
        printf(" *MEAS p0k: not frozen (run=%u pulls=%lu) -- arm a capture (*ac) and wait for it\n",
               (unsigned)s_p0k_run, (unsigned long)s_p0k_pulls );
        return;
    }
    const uint32_t nrec  = s_p0k_end / P0K_DECIM;
    const uint32_t first = ( nrec > P0K_N ) ? ( nrec - P0K_N ) : 0u;
    union { float f; uint32_t u; } tg; tg.f = s_asrc[ASRC_P1_ENGINE].fill_target;
    printf("\n*MEAS_BEGIN kernel=p0k decim=%u records=%lu mark=%lu end=%lu frac0=0x%08lX whole0=%lu"
           " fill_target_bits=0x%08lX fe_scale=256 ph_scale=4096 cp_scale=65536 dfrac_lsb=1 age_late_ticks=128\n",
           (unsigned)P0K_DECIM, (unsigned long)( nrec - first ), (unsigned long)s_p0k_mark,
           (unsigned long)s_p0k_end, (unsigned long)s_p0k_frac0, (unsigned long)s_p0k_whole0,
           (unsigned long)tg.u );
    for( uint32_t i = first; i < nrec; i++ )
    {
        const p0k_rec_t* const r = &s_p0k[i % P0K_N];
        printf("%lu %d %d %d %u %u %u %d %u %u %u\n", (unsigned long)( i * P0K_DECIM + P0K_DECIM - 1u ),
               (int)r->fe, (int)r->fe_lo, (int)r->fe_hi, (unsigned)r->ph, (unsigned)r->cp,
               (unsigned)r->pub, (int)r->dfrac, (unsigned)r->age, (unsigned)r->late,
               (unsigned)r->pad2 );
    }
    printf("*MEAS_END\n");
#endif
}
#endif
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
#if !APP_ASRC_HAS_AB_ENGINE
void audio_app_asrc_set_step_delta( int mode, int32_t req )
{
    (void)mode; (void)req;
    printf(" *MEAS_STEP reject: perturbs A->B only, and this BA MEAS image has no A->B engine\n");
}
#else
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
#endif
int     audio_app_asrc_get_step_delta_mode( void )     { return s_step_delta_mode; }
int32_t audio_app_asrc_get_step_delta_req( void )      { return s_step_delta_req; }
/* The A->B read-only forensics below have no engine to read in a BA MEAS image; their MEAS callers
 * (audio_app_meas.c) take the _ba mirrors there, and the two that would not (the CTRL_TRACE and
 * UART2 stream paths) are refused by #error / asrc_app_validate.h for that image. */
#if APP_ASRC_HAS_AB_ENGINE
float   audio_app_asrc_get_freeze_base_step_ab( void ) { return s_asrc[ASRC_ENGINE_AB].freeze_base_step; }
uint32_t audio_app_asrc_get_fill_ab( void )          { return s_asrc[ASRC_ENGINE_AB].wr - s_asrc[ASRC_ENGINE_AB].rd; }
uint32_t audio_app_asrc_get_fill_pull_start_ab( void ) { return s_asrc[ASRC_ENGINE_AB].dbg_fill; }
uint32_t audio_app_asrc_get_fill_target_ab( void )     { return (uint32_t)s_asrc[ASRC_ENGINE_AB].fill_target; }
// Q19: narrow read-only accessors for the two live control values the causal map needs (the same
// two already passed to audio_app_meas_trace_tick): the low-pass-filtered fill correction and the
// slew-limited applied resample step. Read-only; do not expose the rest of asrc_t.
float   audio_app_asrc_get_corr_lpf_ab( void )         { return s_asrc[ASRC_ENGINE_AB].corr_lpf; }
float   audio_app_asrc_get_step_state_ab( void )       { return s_asrc[ASRC_ENGINE_AB].step_state; }
#endif /* APP_ASRC_HAS_AB_ENGINE */
#endif

#if APP_ASRC_HAS_AB_ENGINE
void audio_app_asrc_push_ab( const int32_t* src ) { asrc_push( &s_asrc[ASRC_ENGINE_AB], src ); }
void audio_app_asrc_pull_ab( int32_t* dst )       { asrc_pull( &s_asrc[ASRC_ENGINE_AB], dst ); }
#if APP_ASRC_FULL_IIR_48_TO_32
void audio_app_asrc_push_ab_block_f32( const float* src, uint32_t ch_stride,
                                      uint32_t frames, uint8_t from_frontend )
{
    if( ( src != NULL ) && ( ch_stride >= (uint32_t)APP_BLOCK_FRAMES ) &&
        ( frames > 0u ) && ( frames <= ch_stride ) )
    {
        asrc_push_block_f32( &s_asrc[ASRC_ENGINE_AB], src, ch_stride, frames, from_frontend );
    }
}
#endif
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
#endif /* APP_ASRC_HAS_AB_ENGINE */

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
uint32_t audio_app_asrc_get_fill_pull_start_ba( void )  { return s_asrc[ASRC_ENGINE_BA].dbg_fill; }
uint32_t audio_app_asrc_get_fill_target_ba( void )      { return (uint32_t)s_asrc[ASRC_ENGINE_BA].fill_target; }
float    audio_app_asrc_get_ratio_live_ba( void )       { return s_asrc[ASRC_ENGINE_BA].ratio; }
float    audio_app_asrc_get_ff_frozen_ratio_ba( void )  { return s_asrc[ASRC_ENGINE_BA].ff_frozen_ratio; }
#endif /* APP_ASRC_MEAS */
#endif

// Trailing `fe=` field of an AB/BA telemetry line: which fixed decimator, if any, runs ahead
// of the resampler in that direction. `fe=direct` means none -- the poly stage resamples the
// full band and its cutoff is ASRC_POLY_FC of the *input* rate, so any output whose Nyquist is
// below that cutoff aliases.
//
// `fe=direct` IS AMBIGUOUS ON PURPOSE, AND THAT IS WHY IT IS ANNOUNCED ELSEWHERE.  It says only
// that no stage ran; it does NOT say whether one was WANTED.  The two cases it covers are:
//   (1) nothing was needed -- unity, an up-conversion, or a ratio the resampler serves directly;
//   (2) something was needed and this build has none compiled in
//       (APP_ASRC_RUNTIME_48K_TO_8 == 0), so the output IS aliased.
// The field cannot tell them apart, because in case (2) the plan table -- which is compiled, but
// is not consulted and has its 2/3 rows disabled -- answers den 1 for both cases alike.
// audio_app_asrc_rate_pair_is_supported() therefore prints the sentence at pair-commit time (see
// asrc_pair_wants_absent_frontend()).  Read a `fe=direct` line together with that announcement
// before recording an alias / DR / THD+N figure from it -- F1(b), 2026-09-04. `fe=/4` / `fe=/6` name the divider, and the intermediate ring's
// ovf/udf counters follow: those are the evidence that the stage is really in the path and
// keeping up, so the divider identity and the counters both stay in the line (a bare 0/1 flag
// would drop them). Printed as a continuation of the line above -- no leading newline.
//
// The divider is followed by a coefficient-set tag where it needs one (`fe=/4:12k`): /4 serves
// both 11.025 kHz and 12 kHz with different band edges, so the number alone cannot tell a
// correctly selected set from a silently wrong one. The tag is a SUFFIX on the divider token,
// which keeps `^/(\d+)` parses (tools/asrc/lowrate_sweep.py) working unchanged.
// `num` is 1 for every divider chain and 2 for the 32 kHz AUDIO MODE front end, which is a 2/3
// polyphase resampler rather than a decimator: it prints as `fe=2/3:audio` from a 48 kHz leg and
// `fe=2/6:audio` from a 96 kHz one, where den is the COMPOSED ratio of the /2 pre-stage and the
// same 2/3 rear stage.  Both are the same partially-protecting filter and the same tag.
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
#if APP_ASRC_HAS_AB_ENGINE
static void asrc_dbg_print_frontend_ab( void )
{
    /* Per direction -- see the note in asrc_dbg_print_frontend_ba(), which is the leg the shared
     * accessor actually misreported.  This leg reads the same way so the two cannot diverge. */
#if APP_ASRC_FULL_IIR_48_TO_32
    /* The trial stage's ratio IS 1/1, so the shared field would print `fe=direct` -- true about
     * the resampling ratio and a lie about the anti-alias stage, which is exactly the confusion
     * that makes a log unreadable months later. Name it instead. */
    if( asrc_full_iir_48_to_32_armed() )
    {
        printf(" fe=iir6@48k\n");
        return;
    }
#endif
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
#if APP_ASRC_LPF1TO1_PROTOTYPE
    /* THE EXACTLY-ONE CHECK, as two numbers on one line.
     *
     * `fe=` names the chain the plan ARMED.  On 2026-09-14 it named a 1:1 LPF on both directions
     * of a 32 <-> 16 kHz pair while the block ISR gate -- which still tested `den != 1` -- kept
     * the FIR out of the audio path entirely, and a twelve-minute dwell looked healthy.  So the
     * counters below come from where the filtering happens, and the invariant this architecture
     * depends on is printed rather than argued: the front end is ONE shared object, so exactly
     * one of these must grow and the other must stay at 0.
     *
     * Printed from the A->B wrapper only, so it appears once per telemetry window with both
     * directions side by side -- two separate lines would invite reading one and not the other.
     *
     * `fe exec`, not `lp1 exec`, which is what it said until 2026-09-14: the counters come from
     * the front-end wrappers and count whatever front end is armed, so at `fe=/3` this line was
     * reporting the DIVIDER's runs under a name that reads as the 1:1 filter's.  That is the
     * same conflation `fe=` itself had -- a name promising more than the number knows -- and it
     * matters most in exactly the logs the remaining rate pairs are to be measured from.  WHICH
     * chain ran is on the `fe=` line right below; this line answers HOW MANY and IN WHICH
     * DIRECTION, for whichever chain that is. */
    {
        const uint32_t r_ab = asrc_audio_path_frontend_runs_ab();
        const uint32_t r_ba = asrc_audio_path_frontend_runs_ba();
        printf( " fe exec: AB=%lu BA=%lu %s\n",
                (unsigned long)r_ab, (unsigned long)r_ba,
                ( ( r_ab != 0u ) && ( r_ba != 0u ) ) ? "** BOTH DIRECTIONS FILTERING -- the shared"
                                                       " front end cannot serve two **"
                              : ( ( r_ab == 0u ) && ( r_ba == 0u ) ) ? "(no front end ran)" : "(one direction, as required)" );
    }
    /* PER STAGE, for a COMPOSED chain -- because `fe exec` above cannot see inside one.
     *
     * A composition can run its first stage and skip its second, which is the same class of
     * failure as the 2026-09-14 ISR gate one level up: the chain is armed, one counter grows,
     * and nothing says the band-limit at the intermediate rate never happened.  Frames
     * PRODUCED, not calls, so the expected relation is exact and checkable in the log: a /2
     * followed by a 1:1 emits the same number of frames from both stages.  A stage reading 0
     * names the half that did not run.
     *
     * Only for the composed kind: on every other chain these counters would repeat `fe exec`
     * in different units, and a line that adds nothing is a line that stops being read.
     *
     * ASKED OF BOTH DIRECTIONS, fixed 2026-09-20.  This used to ask `kind_dir( true )` only, so
     * the line appeared when the composed chain sat on the A->B leg and VANISHED when it sat on
     * B->A -- and a check that is silent in half the configurations is the failure mode this very
     * block exists to catch, one level up.  MEASURED, not hypothesised: the 22.05 <-> 8 kHz
     * mirror dwell (A = 8 k, B = 22.05 k) ran a healthy composed chain for 16 minutes with
     * `fe=/2+lp1:8k` and `fe exec: BA=1383616` in the log and not one `fe stages:` line, so a
     * stage that had never run would have been invisible in exactly that orientation.  The stage
     * counters themselves are direction-agnostic (the front end is ONE shared object, and `fe
     * exec` above is what says which direction drives it), so asking either direction is the
     * whole fix -- there is no second set of counters to add.
     *
     * The 44.1 <-> 16 kHz prototype had the same hole: its mirror orientation printed no stage
     * line either.  That pair's first light happened to be measured A = 44.1 k, which is why the
     * gap survived. */
    if( ( asrc_audio_path_frontend_kind_dir( true )  == ASRC_FRONTEND_FIXED_THEN_1TO1 ) ||
        ( asrc_audio_path_frontend_kind_dir( false ) == ASRC_FRONTEND_FIXED_THEN_1TO1 ) )
    {
        const uint32_t s0 = asrc_audio_path_frontend_stage_frames( 0u );
        const uint32_t s1 = asrc_audio_path_frontend_stage_frames( 1u );
        printf( " fe stages: s0=%lu s1=%lu %s\n",
                (unsigned long)s0, (unsigned long)s1,
                ( ( s0 == 0u ) || ( s1 == 0u ) )
                    ? "** A STAGE NEVER RAN -- the composition is not in the audio path **"
                    : ( ( s0 == s1 ) ? "(both stages ran, frame counts agree)"
                                     : "** STAGE FRAME COUNTS DISAGREE -- a /2 then 1:1 must"
                                       " emit the same count from both **" ) );
    }
#endif
#if APP_ASRC_LPF1TO1_PROTOTYPE
    /* A 1:1 stage's RATIO is 1/1, so the shared field would print `fe=direct` -- true about the
     * resampling ratio and a lie about the anti-alias stage.  Same problem the Full-IIR trial
     * has, same fix: name it, and name the rate it protects, because the coefficient set is
     * chosen by (source rate, output rate) and the source rate alone does not identify it. */
    /* The composed chain publishes den = 2, so the shared field WOULD print a
     * truthful-looking `fe=/2` -- the ratio is right and the 1:1 stage behind the divider
     * is missing from it, which is the same class of half-truth as `fe=direct` for a 1:1
     * stage.  Name the whole chain. */
    if( asrc_audio_path_frontend_kind_dir( true ) == ASRC_FRONTEND_FIXED_THEN_1TO1 )
    {
        printf( " fe=/%lu+lp1%s ovf=%lu udf=%lu\n", (unsigned long)den,
                asrc_audio_path_frontend_tag(),
                (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_intermediate_overflow,
                (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_intermediate_underrun );
        return;
    }
    if( asrc_audio_path_frontend_kind_dir( true ) == ASRC_FRONTEND_FILTER_1TO1 )
    {
        printf( " fe=lp1%s ovf=%lu udf=%lu\n", asrc_audio_path_frontend_tag(),
                (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_intermediate_overflow,
                (unsigned long)s_asrc[ASRC_ENGINE_AB].dbg_intermediate_underrun );
        return;
    }
#endif
#if APP_ASRC_48K_TO_8_INTEGRATION || APP_ASRC_RUNTIME_48K_TO_8
    asrc_dbg_print_frontend_field( num, den, tag,
                                   s_asrc[ASRC_ENGINE_AB].dbg_intermediate_overflow,
                                   s_asrc[ASRC_ENGINE_AB].dbg_intermediate_underrun );
#else
    asrc_dbg_print_frontend_field( num, den, tag, 0u, 0u );   // no intermediate ring here
#endif
}
#endif /* APP_ASRC_HAS_AB_ENGINE */

#if APP_B_ROUTE_USES_BA
static void asrc_dbg_print_frontend_ba( void )
{
    /* EVERY kind test in this function is PER DIRECTION (`_kind_dir( false )`), not the shared
     * `asrc_audio_path_frontend_kind()`.  The shared value is one kind for the armed chain and the
     * chain lives on ONE leg, so this leg was reading the OTHER leg's kind: on 2026-09-14 the BA
     * leg of 44.1 -> 16 kHz printed `fe=/1+lp1:16k`, borrowing the AB leg's composed kind and
     * formatting its own den of 1 as a nonsensical "divide by 1", while `fe exec: BA=0` in the
     * same block said nothing had run here.  A leg with no stage now falls through to the shared
     * field and says `fe=direct`, which is what it is. */
#if APP_ASRC_RUNTIME_48K_TO_8
    const uint32_t num = asrc_audio_path_ba_fixed_rate_num();
    const uint32_t den = asrc_audio_path_ba_fixed_rate_den();
    const char* const tag = asrc_audio_path_frontend_tag();
#else
    const uint32_t num = (uint32_t)APP_ASRC_BA_FIXED_RATE_NUM;
    const uint32_t den = (uint32_t)APP_ASRC_BA_FIXED_RATE_DEN;
    const char* const tag = "";
#endif
#if APP_ASRC_LPF1TO1_PROTOTYPE
    /* A 1:1 stage's RATIO is 1/1, so the shared field would print `fe=direct` -- true about the
     * resampling ratio and a lie about the anti-alias stage.  Same problem the Full-IIR trial
     * has, same fix: name it, and name the rate it protects, because the coefficient set is
     * chosen by (source rate, output rate) and the source rate alone does not identify it. */
    /* The composed chain publishes den = 2, so the shared field WOULD print a
     * truthful-looking `fe=/2` -- the ratio is right and the 1:1 stage behind the divider
     * is missing from it, which is the same class of half-truth as `fe=direct` for a 1:1
     * stage.  Name the whole chain. */
    if( asrc_audio_path_frontend_kind_dir( false ) == ASRC_FRONTEND_FIXED_THEN_1TO1 )
    {
        printf( " fe=/%lu+lp1%s ovf=%lu udf=%lu\n", (unsigned long)den,
                asrc_audio_path_frontend_tag(),
                (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_intermediate_overflow,
                (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_intermediate_underrun );
        return;
    }
    if( asrc_audio_path_frontend_kind_dir( false ) == ASRC_FRONTEND_FILTER_1TO1 )
    {
        printf( " fe=lp1%s ovf=%lu udf=%lu\n", asrc_audio_path_frontend_tag(),
                (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_intermediate_overflow,
                (unsigned long)s_asrc[ASRC_ENGINE_BA].dbg_intermediate_underrun );
        return;
    }
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
#if APP_ASRC_STAGE_PROFILE
/*
 * One extra line per direction with the per-stage BLOCK TOTALS (see APP_ASRC_STAGE_PROFILE in
 * asrc_app_config.h).  The buckets, in the order the pull executes them:
 *
 *   ph    asrc_q31_phase_of + wbase + wb->Q31          phase/ratio bookkeeping
 *   bl    mchp_asrc_q31_blend_row                      one 30-tap row blended per FRAME
 *   mac   mchp_asrc_q31_row16                          one 30-tap dot per CHANNEL
 *   conv  asrc_q31_to_slot over ASRC_CH                output mask/saturate
 *   st    d[s] = (s<ASRC_CH) ? out[s] : 0              scatter into the TDM slot frame
 *   coef  ph + bl, kept only so this line stays comparable with the E1-era reports
 *
 * `rest` is what pull costs on top of ph+bl+mac+conv+st: out[] init, the ring-window test,
 * frac/rd advance, the servo/telemetry tail and the loop itself -- plus this instrumentation's
 * own SEVEN timer reads per output frame (four before F1).  So `rest` is an upper bound on the
 * real remainder, and absolute deadline figures must come from a APP_ASRC_STAGE_PROFILE=0
 * image.  The instrument's own cost is not guessed: it is pull(profile) - pull(profile=0) for
 * the same defines, which is why both images get flashed.
 *
 * Each bucket is peak-held INDEPENDENTLY, so they can come from different blocks; that is the
 * conservative reading for an optimisation estimate, and the reason their sum may exceed any
 * single pull and `rest` may go negative.  Printed signed rather than clamped, because a
 * clamped 0 would hide exactly that.
 */
static void asrc_dbg_print_stage_profile( const char* dir, uint8_t engine, uint32_t pull_ticks )
{
    const uint32_t coef = s_asrc[engine].dbg_stg_coef_max;
    const uint32_t mac  = s_asrc[engine].dbg_stg_mac_max;
    const uint32_t conv = s_asrc[engine].dbg_stg_conv_max;
    const uint32_t ph   = s_asrc[engine].dbg_stg_ph_max;
    const uint32_t bl   = s_asrc[engine].dbg_stg_bl_max;
    const uint32_t st   = s_asrc[engine].dbg_stg_st_max;
    const uint32_t c10  = nora_high_res_timer_count_to_us_x10( coef );
    const uint32_t m10  = nora_high_res_timer_count_to_us_x10( mac );
    const uint32_t v10  = nora_high_res_timer_count_to_us_x10( conv );
    const uint32_t p10  = nora_high_res_timer_count_to_us_x10( ph );
    const uint32_t b10  = nora_high_res_timer_count_to_us_x10( bl );
    const uint32_t s10  = nora_high_res_timer_count_to_us_x10( st );
    const int32_t  r10  = (int32_t)nora_high_res_timer_count_to_us_x10( pull_ticks )
                        - (int32_t)( p10 + b10 + m10 + v10 + s10 );
    printf("[stg x%uch]%s coef=%lu.%luus (ph=%lu.%luus bl=%lu.%luus) mac=%lu.%luus"
           " conv=%lu.%luus st=%lu.%luus rest=%s%ld.%ldus\n",
           (unsigned)ASRC_CH, dir,
           (unsigned long)(c10 / 10u), (unsigned long)(c10 % 10u),
           (unsigned long)(p10 / 10u), (unsigned long)(p10 % 10u),
           (unsigned long)(b10 / 10u), (unsigned long)(b10 % 10u),
           (unsigned long)(m10 / 10u), (unsigned long)(m10 % 10u),
           (unsigned long)(v10 / 10u), (unsigned long)(v10 % 10u),
           (unsigned long)(s10 / 10u), (unsigned long)(s10 % 10u),
           ( r10 < 0 ) ? "-" : "",
           (long)( ( ( r10 < 0 ) ? -r10 : r10 ) / 10 ),
           (long)( ( ( r10 < 0 ) ? -r10 : r10 ) % 10 ));
    s_asrc[engine].dbg_stg_coef_max = 0u;
    s_asrc[engine].dbg_stg_mac_max  = 0u;
    s_asrc[engine].dbg_stg_conv_max = 0u;
    /*
     * Second line: the MIN-held totals, i.e. the block in which each bucket escaped leg-A
     * preemption.  For a pull that runs in the lower-priority leg this is the only CPU figure
     * on offer here.  No `rest` on this line -- the minima come from different blocks, so
     * subtracting them from any single pull would not mean anything; `sum` is printed instead.
     */
    {
        const uint32_t pn10 = nora_high_res_timer_count_to_us_x10( s_asrc[engine].dbg_stg_ph_min );
        const uint32_t bn10 = nora_high_res_timer_count_to_us_x10( s_asrc[engine].dbg_stg_bl_min );
        const uint32_t mn10 = nora_high_res_timer_count_to_us_x10( s_asrc[engine].dbg_stg_mac_min );
        const uint32_t vn10 = nora_high_res_timer_count_to_us_x10( s_asrc[engine].dbg_stg_conv_min );
        const uint32_t sn10 = nora_high_res_timer_count_to_us_x10( s_asrc[engine].dbg_stg_st_min );
        const uint32_t tn10 = pn10 + bn10 + mn10 + vn10 + sn10;
        printf("[stg x%uch]%s min ph=%lu.%luus bl=%lu.%luus mac=%lu.%luus conv=%lu.%luus"
               " st=%lu.%luus sum=%lu.%luus\n",
               (unsigned)ASRC_CH, dir,
               (unsigned long)(pn10 / 10u), (unsigned long)(pn10 % 10u),
               (unsigned long)(bn10 / 10u), (unsigned long)(bn10 % 10u),
               (unsigned long)(mn10 / 10u), (unsigned long)(mn10 % 10u),
               (unsigned long)(vn10 / 10u), (unsigned long)(vn10 % 10u),
               (unsigned long)(sn10 / 10u), (unsigned long)(sn10 % 10u),
               (unsigned long)(tn10 / 10u), (unsigned long)(tn10 % 10u));
        printf("[stg x%uch]%s memo hit=%lu of %lu frames\n", (unsigned)ASRC_CH, dir,
               (unsigned long)s_asrc[engine].dbg_memo_hit,
               (unsigned long)s_asrc[engine].dbg_memo_tot);
        printf("[stg x%uch]%s ceff steal=%lu of %lu frames\n", (unsigned)ASRC_CH, dir,
               (unsigned long)s_asrc[engine].dbg_ceff_steal,
               (unsigned long)s_asrc[engine].dbg_memo_tot);
        s_asrc[engine].dbg_ceff_steal = 0u;
        s_asrc[engine].dbg_memo_hit = 0u;
        s_asrc[engine].dbg_memo_tot = 0u;
    }
    s_asrc[engine].dbg_stg_ph_max   = 0u;
    s_asrc[engine].dbg_stg_bl_max   = 0u;
    s_asrc[engine].dbg_stg_st_max   = 0u;
    s_asrc[engine].dbg_stg_coef_min = 0xFFFFFFFFu;
    s_asrc[engine].dbg_stg_mac_min  = 0xFFFFFFFFu;
    s_asrc[engine].dbg_stg_conv_min = 0xFFFFFFFFu;
    s_asrc[engine].dbg_stg_ph_min   = 0xFFFFFFFFu;
    s_asrc[engine].dbg_stg_bl_min   = 0xFFFFFFFFu;
    s_asrc[engine].dbg_stg_st_min   = 0xFFFFFFFFu;
}
#endif

// ab is consumed by the B ISR (out = fs_B); ba is consumed by the A ISR (out = fs_A).
// Each line ends with `fe=` (see above), which is why asrc_audio_path_dbg_print() no longer
// emits the separate "ASRCpath <dir> front-end:" pair: the state now sits next to the engine
// it belongs to, one line per direction instead of two.
#if APP_ASRC_LEG_PROFILE
/*
 * The pull peak of the window that was just printed, kept because the LEG partition is
 * printed by asrc_audio_path_dbg_print() and that runs AFTER this function, which clears the
 * live peak.  Latching here rather than peeking there is what makes the two lines describe
 * the same window: a peek would read the cleared 0.  Foreground-only, both sides.
 */
static uint32_t s_dbg_pull_ticks_last[2];

uint32_t audio_app_asrc_dbg_pull_ticks_last( uint8_t engine )
{
    return ( engine < 2u ) ? s_dbg_pull_ticks_last[engine] : 0u;
}
#endif

void audio_app_asrc_dbg_print( uint32_t fsA_hz, uint32_t fsB_hz )
{
#if !APP_ASRC_HAS_AB_ENGINE
    (void)fsA_hz; (void)fsB_hz;   /* BA MEAS: no A->B engine, so no `]AB hr=` line */
#else
    const uint32_t p_ab_ticks = s_asrc[ASRC_ENGINE_AB].dbg_pull_ticks_max; s_asrc[ASRC_ENGINE_AB].dbg_pull_ticks_max = 0u;
#if APP_ASRC_LEG_PROFILE
    s_dbg_pull_ticks_last[ASRC_ENGINE_AB] = p_ab_ticks;
#endif
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
#if APP_ASRC_QUIET_TRACK && !APP_ASRC_MEAS
    p0t_dbg_print( "AB", &s_asrc[ASRC_ENGINE_AB] );
#endif
#if APP_ASRC_STAGE_PROFILE
    asrc_dbg_print_stage_profile( "AB", ASRC_ENGINE_AB, p_ab_ticks );
#endif
#endif /* APP_ASRC_HAS_AB_ENGINE */
#if APP_ASRC_FULL_IIR_48_TO_32
    asrc_full_iir_48_to_32_dbg_print();   // silent unless the trial stage is armed
#endif
#if APP_B_ROUTE_USES_BA
    const uint32_t p_ba_ticks = s_asrc[ASRC_ENGINE_BA].dbg_pull_ticks_max; s_asrc[ASRC_ENGINE_BA].dbg_pull_ticks_max = 0u;
#if APP_ASRC_LEG_PROFILE
    s_dbg_pull_ticks_last[ASRC_ENGINE_BA] = p_ba_ticks;
#endif
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
#if APP_ASRC_QUIET_TRACK && !APP_ASRC_MEAS
    p0t_dbg_print( "BA", &s_asrc[ASRC_ENGINE_BA] );
#endif
#if APP_ASRC_STAGE_PROFILE
    asrc_dbg_print_stage_profile( "BA", ASRC_ENGINE_BA, p_ba_ticks );
#endif
#endif
}

#endif // APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC
