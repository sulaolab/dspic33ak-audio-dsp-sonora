#!/usr/bin/env python3
"""Read a MEAS capture the way the SS 26.1 adoption rule reads it, and put the HOST
model beside it at the SAME frozen step.

analyze_meas_capture_alias.py answers "how big is the component at this expected
alias frequency", which is the right question when the tone is OUT of band.  It is
the wrong question for a rear phase-interpolation spur: there the tone is INSIDE the
protected band, so the strongest in-band component is the tone itself and the alias
analyser dutifully reports it (-0.17 dBc, which is the tone).

This reader instead applies the rule the 45-pair study adopted:

    worst unwanted component in 0 .. protected_hz, excluding DC leakage
    and the tone's own main lobe

and it does so with asrc_rear_spur_model's OWN spectrum() and worst_in_band(), not
a re-implementation -- so the hardware number and the model number are produced by
the same code and can be compared directly.

It then runs the model at the capture's own `freeze_applied_bits`, because the spur
frequency moves with the exact float32 step (study SS 27.3): comparing hardware at
one step against a model at another is how you get a "disagreement" that is really
two different experiments.

THREE THINGS THIS FILE GOT WRONG ON 2026-09-15, ALL FIXED HERE, ALL WORTH KEEPING
IN VIEW BECAUSE THEY ALL PRODUCED CONFIDENT, PLAUSIBLE, WRONG NUMBERS (Phase F F-A):

  1. SCALE.  Sharing the model's spectrum() also inherited the model's SLOT
     CONVENTION: its arms emit s24-LEFT samples (24-bit << 8) and spectrum()
     divides by 256 to undo that.  A *MEAS capture is plain 24-bit counts, so every
     hardware spectrum came out 256x = 48.16 dB low.  Now: slot_shift=0.
  2. REFERENCE.  dBc was taken against the header's NOMINAL level_dbfs, so the scale
     error did not cancel -- it went straight into the answer (-161.92 dBc reported
     where the truth is -113.6).  Now: dBc is referred to the tone MEASURED IN THE
     SAME SPECTRUM, which is invariant to any gain, and the nominal level is printed
     beside it as a cross-check instead of being trusted.
  3. GUARD.  --guard-hz reached this file's own reading but was NEVER passed to the
     model arm, which therefore always ran at the old 120 Hz default: 5.6 bins at
     (44.1 kHz, n=2048), inside the Kaiser-24 main lobe.  So the model column read
     -65.83 dBc with all three arms identical -- the signature of reading the tone,
     not the spur.  Now: the guard is counted in BINS by default, it is passed to
     the model, and guard_plateau() prints whether widening it 1.5x moves the
     answer.  If it does, the number is not a spur level.

With all three fixed the 48 -> 44.1 kHz Phase E-A captures agree with the model to
0.10 dB (L=256) and 0.14 dB (L=128), at the model's predicted spur frequency.

Usage:
  python tools/asrc/analyze_meas_capture_spur.py <monitor log> [--block N]
      [--phases 256] [--protected 17915.6] [--no-model]
"""
import argparse
import math
import os
import pathlib
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import analyze_meas_capture_alias as A     # noqa: E402  (block parser)
import asrc_rear_spur_model as S           # noqa: E402  (spectrum + worst_in_band)
import asrc_q31_resampler_model as Q       # noqa: E402  (geometry globals)

# A *MEAS capture carries plain 24-bit counts, NOT the s24-left slots the model's
# arms emit.  See fault 1 in the module docstring.
CAPTURE_SLOT_SHIFT = 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', type=pathlib.Path)
    ap.add_argument('--block', type=int, default=None,
                    help='which MEAS block (default: last)')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--protected', type=float, default=None,
                    help='protected band edge in Hz (default 0.8125 x FN_out)')
    ap.add_argument('--guard-hz', type=float, default=None,
                    help="half width excluded around the tone's own main lobe "
                         "(default: %g bins of this capture's FFT)" % S.GUARD_BINS)
    ap.add_argument('--beta', type=float, default=24.0)
    ap.add_argument('--phases', type=int, default=None,
                    help='ASRC_POLY_L the image was built with (for the model arm)')
    ap.add_argument('--taps', type=int, default=None,
                    help='ASRC_POLY_M the image was built with (default %d)' % Q.M)
    ap.add_argument('--no-model', action='store_true')
    args = ap.parse_args()

    blks = list(A.blocks(args.log))
    if not blks:
        sys.exit('no *MEAS_BEGIN/_END block in %s' % args.log)
    sel = blks if args.all else [blks[args.block if args.block is not None else -1]]

    off_plateau = 0
    for meta, samples in sel:
        hdr = meta                     # blocks() yields (header dict, samples array)
        x = np.asarray(samples, dtype=np.int64)
        fs_out = float(hdr['fs_out_hz'])
        tone = float(hdr['tone_hz'])
        level = float(hdr['level_dbfs'])
        prot = args.protected if args.protected is not None else 0.8125 * fs_out / 2.0
        n = x.size
        bin_hz = fs_out / n

        mag = S.spectrum(x, args.beta, slot_shift=CAPTURE_SLOT_SHIFT)
        freqs = np.fft.rfftfreq(n, 1.0 / fs_out)
        guard = S.guard_of(freqs, args.guard_hz)
        lvl, f = S.worst_in_band(mag, freqs, prot, tone, guard_hz=guard)
        # The tone as this very spectrum sees it.  Referring dBc to THIS instead of
        # to `level` is what makes the reading immune to a scale or gain mistake.
        t_lvl, t_f = S.level_at(mag, freqs, tone, window_hz=guard)

        print('capture: tone %.1f Hz @ fs_in %s -> fs_out %.0f Hz, level %.0f dBFS, '
              'n=%d, bin %.3f Hz' % (tone, hdr.get('fs_in_hz', '?'), fs_out, level,
                                     n, bin_hz))
        print('  protected band 0 .. %.1f Hz   tone guard +/-%.1f Hz (%.1f bins)'
              % (prot, guard, guard / bin_hz))
        if t_lvl is None:
            print('  NO TONE FOUND within the guard of %.1f Hz -- wrong header?' % guard)
            continue
        print('  tone measured            %9.2f dBFS at %9.1f Hz   (header says %+.0f)'
              % (t_lvl, t_f, level))
        if abs(t_lvl - level) > 0.5:
            print('  ** tone is %+.2f dB off the header level.  A whole-spectrum scale '
                  'error looks exactly like this; dBc below is referred to the '
                  'MEASURED tone, so it stays correct either way.' % (t_lvl - level))
        if lvl is None:
            print('  NO BAND LEFT after the guard -- widen the band or move the tone')
            continue
        print('  worst unwanted in band   %9.2f dBFS at %9.1f Hz   = %8.2f dBc'
              % (lvl, f, lvl - t_lvl))

        ok, lo, hi = S.guard_plateau(mag, freqs, prot, tone, guard_hz=guard)
        if ok:
            print('  guard plateau            OK (%.2f dBFS at the guard, %.2f at 1.5x)'
                  % (lo[0], hi[0]))
        else:
            off_plateau += 1
            print('  guard plateau            ** NOT ON PLATEAU: %.2f dBFS at the guard, '
                  '%.2f at 1.5x. The argmax is inside the tone lobe -- this is NOT a '
                  'spur level.' % (lo[0], hi[0]))

        bits = hdr.get('freeze_applied_bits')
        if args.no_model or not bits:
            continue
        step = float(struct.unpack('<f', struct.pack('<I', int(str(bits), 16)))[0])
        print('  frozen step (from freeze_applied_bits %s) = %.10f' % (bits, step))
        if args.phases:
            Q.L = int(args.phases)
        if args.taps:
            Q.M = int(args.taps)
        Q.MH = (Q.M // 2) - 1
        print('  model geometry           L=%d M=%d fc=%.4f beta=%.2f'
              % (Q.L, Q.M, Q.FC, Q.BETA))
        out = S.evaluate(step, fs_out, tone, level, n, 0.0, prot, guard_hz=guard)
        for arm in ('q31', 'lin64', 'cont'):
            if arm in out and out[arm][0] is not None:
                print('  model %-6s             %9.2f dBc at %9.1f Hz'
                      % (arm, out[arm][0], out[arm][1]))
        if out.get('q31', (None,))[0] is not None:
            print('  HARDWARE - MODEL(q31)    %+9.2f dB' % ((lvl - t_lvl) - out['q31'][0]))
    return 1 if off_plateau else 0


if __name__ == '__main__':
    sys.exit(main())
