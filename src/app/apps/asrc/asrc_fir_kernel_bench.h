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

#if ASRC_FIR_KERNEL_BENCH_AVAILABLE && !defined(__dsPIC33AK512MPS512__)
#  error "ASRC_FIR_KERNEL_BENCH_AVAILABLE=1 is AK512-only: the Y scratch arena address and the X-space coefficient budget are both device-specific.  See the comment above."
#endif

// Measure the three candidate front-stage FIR kernels on hardware, in CPU cycles per MAC.
// See asrc_fir_kernel_bench.c for what each part reports and why it is measured that way.
//
// trials == 0 selects the default trial count.  Runs in the caller's context (the console's
// main-loop foreground), takes a few tens of milliseconds, and touches no streaming state.
void asrc_fir_kernel_bench_run( uint32_t trials );

#endif /* ASRC_FIR_KERNEL_BENCH_H */
