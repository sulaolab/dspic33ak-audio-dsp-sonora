#ifndef ASRC_FIR_KERNEL_BENCH_H
#define ASRC_FIR_KERNEL_BENCH_H

#include <stdint.h>

// The bench is compiled for the AK512 only, and the console must agree with configurations.xml about
// that or it references a symbol that was excluded from the build.  Two device-specific reasons:
//
//   * It addresses a fixed scratch arena in Y data space, which is 0xC000..0x13FFF on the AK512 and
//     0x6000..0x7FFF on the AK128 -- the AK512 address is not RAM at all on the smaller part.
//   * It needs X data space for the coefficients, and it is measured on a profile that has some.
//
// Anywhere else "*aq" answers ERR_UNSUPPORTED, which is the honest response: the measurement cannot
// be made in that image.  Widening this to the AK128 (report item M9) means giving the arena a
// per-device address and re-checking the X-space budget there, not just flipping this macro.
//
// DEFAULT OFF, EVERYWHERE -- opt in per build, the same way APP_ASRC_MEAS does it.
//
// The bench costs 1,184 B of data memory that it holds whether or not anyone runs it (1,024 B of
// X-space coefficients + 160 B of outputs).  It used to default to 1 on the AK512, which made the
// shipping ASRC configuration the ONLY one of the four that compiled measurement-only code into a
// production image: the other three exclude the .c in configurations.xml, and the AK512 ASRC one
// does not.  1,184 B is not a rounding error on a part whose link-time stack leftover is a few
// kilobytes, and it bought nothing -- "*aq" is a bench-once number, already recorded in section 10.
//
// So the polarity is inverted.  Measurement-only code is opt-in:
//
//     buildtools/build.ps1 -Full -Define ASRC_FIR_KERNEL_BENCH_AVAILABLE=1
//
// -Full matters: changing only a -Define does not recompile, and changing this header does not
// either.  With the macro at 0 this file compiles to nothing and the console's own guard removes the
// only caller, so "*aq" answers ERR_UNSUPPORTED -- the honest response, since the measurement really
// cannot be made in that image.
//
// Enabling it is still AK512-only in practice, for the two device reasons above; widening it to the
// AK128 (report item M9) means giving the arena a per-device address and re-checking the X-space
// budget there, not just passing the -Define.
#ifndef ASRC_FIR_KERNEL_BENCH_AVAILABLE
#  define ASRC_FIR_KERNEL_BENCH_AVAILABLE  0
#endif

/* Phase 2/3's H2 probe has several 16ch float work buffers.  It normally
 * follows the parent bench switch, preserving every existing H2 measurement
 * build.  A short-FIR-only image may override this to 0: the two probes are
 * foreground-only and never need to coexist in one ROM image. */
#ifndef ASRC_H2_KERNEL_BENCH_AVAILABLE
#  define ASRC_H2_KERNEL_BENCH_AVAILABLE ASRC_FIR_KERNEL_BENCH_AVAILABLE
#endif

#if ASRC_FIR_KERNEL_BENCH_AVAILABLE && !defined(__dsPIC33AK512MPS512__)
#  error "ASRC_FIR_KERNEL_BENCH_AVAILABLE=1 is AK512-only: the Y scratch arena address and the X-space coefficient budget are both device-specific.  See the comment above."
#endif

// Measure the three candidate front-stage FIR kernels on hardware, in CPU cycles per MAC.
// See asrc_fir_kernel_bench.c for what each part reports and why it is measured that way.
//
// trials == 0 selects the default trial count.  Runs in the caller's context (the console's
// main-loop foreground), takes a few tens of milliseconds, and touches no streaming state.
void asrc_fir_kernel_bench_run( uint32_t trials );

/* Phase-2 H2 component timing: float FIR49, five DF2T SOS, and their actual
 * serial composition over 16 channels x 16 frames.  Like *aq this is an
 * opt-in foreground probe and never enters the streaming audio path. */
void asrc_h2_timing_bench_run( uint32_t trials );

/* Phase-3 H2 feasibility probe.  This is distinct from the Phase-2 baseline
 * above: it compares only already-present DSP kernels and conversion cost in
 * probe-local buffers.  It never enters the streaming audio path. */
void asrc_h2_optimized_kernel_bench_run( uint32_t trials );

/* Measurement-only calibration of the existing Q31 /2 batch kernel at
 * 107/81/65/49/33 taps.  It does not change a live frontend or implement a
 * Hybrid; its results price how many IIR SOS a shorter FIR could buy. */
void asrc_fir_tradeoff_bench_run( uint32_t trials );

/* Full-IIR precheck (Phase-4 follow-up): the six-SOS 48 kHz anti-alias LPF timed
 * with the Phase-3 optimized DF2T kernel, so the 6 x 25.7 us linear estimate is
 * replaced by a measurement of the sixth section's real marginal cost.  It times
 * the IIR only; the front-end removal and the generic ASRC's step 1.0 -> 1.5 move
 * are measured as runtime telemetry across two images instead.  Foreground-only
 * and never part of the audio path, like the probes above. */
void asrc_full_iir_precheck_bench_run( uint32_t trials );

/* Candidate E (half-band /2 pre-stage): measures the BLOCK DIFFERENCE between the
 * shipping dense 41-tap /2 kernel and a 35-tap half-band prototype run by a
 * kernel that walks only the 19 structurally non-zero taps.  It exists because
 * the host CPU model's -10.69 us/block assumes the measured 1.012 cycles/MAC
 * survives a strided inner loop, which only hardware can settle.  It changes no
 * default, no rate and no live filter, and the coefficients are synthetic: it
 * times a geometry, it does not qualify a response.
 *
 * It needs its own assembler, src/app/apps/asrc/asrc_fir_hb_kernel_dspic33ak.s,
 * whose body is guarded by .ifdef -- so this probe additionally needs
 *
 *     buildtools/build.ps1 -Full -Define ASRC_FIR_KERNEL_BENCH_AVAILABLE=1 \
 *                                -AsDefine ASRC_FIR_KERNEL_BENCH_AVAILABLE=1
 *
 * and without the -AsDefine the link fails on the missing kernel rather than
 * measuring something else. */
void asrc_hb_kernel_bench_run( uint32_t trials );

/* Third-band 3-path polyphase allpass 96 -> 32 kHz decimator kernel ("*an").
 * Host feasibility put the filter at K=16 first-order allpass sections for
 * -110.33 dBc worst alias into 0-15 kHz -- 16 multiplies per 32 kHz output per
 * channel against the shipping front end's 110.  The Full-IIR trial already
 * showed a kernel with fewer multiplies losing on hardware, so this measures the
 * real kernel: 16ch channel-major, state load/store, coefficient load, the
 * arithmetic, the commutator fetch, the branch sum and every loop the form needs.
 * Q31 is the primary format and float32 the comparison.  Prints a slope (marginal
 * cost of one section) next to the absolute per-output cost, and re-derives the
 * tick-to-cycle constant from fir_ring_q31 in the same run so a wrong constant
 * cannot pass silently.  Foreground-only, changes no default, no rate and no live
 * filter; the ring and the M30 resampler are outside the measured window on
 * purpose and are added separately. */
void asrc_thirdband_kernel_bench_run( uint32_t trials );

#endif /* ASRC_FIR_KERNEL_BENCH_H */
