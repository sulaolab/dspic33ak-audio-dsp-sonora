#!/usr/bin/env python3
"""F-A2: what is the floor of a *MEAS reading, and which floor is it?

A spectral number is only a measurement if the thing it reports is above the floor of
the instrument that read it.  Phase E-A tried to answer this by capturing a -80 dBFS
tone and quoting the result as "-202 dBFS, below 24-bit resolution, therefore the
float64 FFT's numerical floor" -- and every part of that was wrong, because the
reader was 48.16 dB out of scale (see analyze_meas_capture_spur.py) and because the
worst in-band bin of that capture is not a floor at all: it is a real spur sitting at
64.6 Hz that the HOST model predicts within 2 dB.

So this tool separates the three floors that get confused, states each with its
bandwidth normalisation, and prints them next to the capture:

  1. ANALYSIS floor -- float64 rfft of deterministic digital silence.  It is not a
     physical floor; it is proof that the analysis contributes nothing.  Exactly
     zero in, exactly zero out (numerically -inf dBFS, reported as the smallest
     representable), so any nonzero reading below comes from the SIGNAL PATH.
  2. QUANTISATION floor -- an ideal 24-bit quantiser, e = uniform in +/-q/2 with
     q = 2^-23 full scale.  Total in-band power q^2/12 spread over fs/2, so the
     level a single FFT BIN reads depends on the window's ENBW and on n:
         per-bin dBFS = 10*log10(q^2/12 * ENBW_bins / (n/2)) referred to a
                        full-scale sine's 0 dBFS (amplitude 1.0 -> power 1/2)
     This is the floor a PERFECT implementation of this path would have.
  3. PATH floor -- what the capture actually shows away from the tone and away from
     the predicted spur lines: the median in-band bin.  Median, not minimum: the
     minimum is a null and the maximum is a component, so neither is a floor.

The bandwidth statements are not decoration.  ENBW is what makes a per-bin level
comparable to a total-power figure, and it is the step everyone skips: a -146.2 dBFS
"24-bit floor" is a TOTAL over fs/2, and quoting it against one bin of a 2048-point
Kaiser-24 FFT is a 30 dB mistake.

Usage:
  python tools/asrc/meas_floor_authority.py <monitor log> [--block N] [--beta 24]
"""
import argparse
import math
import os
import pathlib
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import analyze_meas_capture_alias as A     # noqa: E402
import asrc_rear_spur_model as S           # noqa: E402


def enbw_bins(w):
    """Equivalent noise bandwidth of a window, in bins."""
    w = np.asarray(w, dtype=np.float64)
    return float(w.size * np.sum(w * w) / (np.sum(w) ** 2))


def quant_floor_dbfs(n, enbw, bits=24):
    """Level ONE BIN reads for an ideal `bits`-bit quantiser, referred to a
    full-scale sine reading 0 dBFS (which is how S.spectrum() normalises)."""
    q = 2.0 ** -(bits - 1)                 # step, full scale = +/-1
    p_total = q * q / 12.0                 # total noise power over 0..fs/2
    p_bin = p_total * enbw / (n / 2.0)     # power in one analysis bin
    # a full-scale sine has amplitude 1.0 and S.spectrum() reports amplitude, so
    # 0 dBFS <-> amplitude 1.0 <-> power 1/2.  Compare powers, not amplitudes.
    return 10.0 * math.log10(p_bin / 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', type=pathlib.Path)
    ap.add_argument('--block', type=int, default=-1)
    ap.add_argument('--beta', type=float, default=24.0)
    ap.add_argument('--protected', type=float, default=None)
    args = ap.parse_args()

    blks = list(A.blocks(args.log))
    hdr, samples = blks[args.block]
    x = np.asarray(samples, dtype=np.int64)
    n = x.size
    fs = float(hdr['fs_out_hz'])
    tone = float(hdr['tone_hz'])
    level = float(hdr['level_dbfs'])
    prot = args.protected if args.protected is not None else 0.8125 * fs / 2.0
    bin_hz = fs / n

    w = np.kaiser(n, args.beta)
    enbw = enbw_bins(w)

    print('capture: tone %.1f Hz -> fs_out %.0f Hz, level %+.0f dBFS, n=%d'
          % (tone, fs, level, n))
    print('  bin %.4f Hz   window Kaiser beta=%.1f   ENBW %.4f bins = %.3f Hz'
          % (bin_hz, args.beta, enbw, enbw * bin_hz))
    print('  a full-scale sine reads 0 dBFS in this normalisation (S.spectrum)')
    print()

    # 1. analysis floor: deterministic digital silence through the same reader
    zeros = np.zeros(n, dtype=np.int64)
    mag_z = S.spectrum(zeros, args.beta, slot_shift=0)
    print('  1 ANALYSIS floor   digital silence, float64 rfft : max bin %.3g '
          '(exactly zero -> -inf dBFS)' % float(np.max(mag_z)))
    # and the same with one LSB of dither, to show the analysis resolves 1 LSB
    one_lsb = np.zeros(n, dtype=np.int64)
    one_lsb[0] = 1
    mag_l = S.spectrum(one_lsb, args.beta, slot_shift=0)
    print('      a single 1-LSB impulse reads %.2f dBFS in one bin, so the analysis '
          'resolves the 24-bit LSB itself' % S.db(float(np.max(mag_l))))

    # 2. quantisation floor of an ideal 24-bit path, per bin
    qf = quant_floor_dbfs(n, enbw, 24)
    print('  2 QUANT floor      ideal 24-bit, per bin                : %8.2f dBFS'
          % qf)
    print('      (total over 0..fs/2 = %.2f dBFS; the per-bin figure is %.2f dB '
          'below it -- this is the step that gets skipped)'
          % (10 * math.log10((2.0 ** -23) ** 2 / 12.0 / 0.5), qf - 10 * math.log10((2.0 ** -23) ** 2 / 12.0 / 0.5)))

    # 3. path floor: the capture's own median in-band bin
    mag = S.spectrum(x, args.beta, slot_shift=0)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    inband = (freqs <= prot) & (np.arange(freqs.size) >= 3)
    guard = S.guard_of(freqs, None)
    quiet = inband & (np.abs(freqs - tone) > guard)
    vals = np.sort(mag[quiet])
    med = S.db(float(np.median(mag[quiet])))
    p90 = S.db(float(vals[int(0.90 * (vals.size - 1))]))
    mx = S.db(float(vals[-1]))
    t_lvl, _ = S.level_at(mag, freqs, tone, window_hz=guard)
    print('  3 PATH floor       this capture, in-band bins off the tone :')
    print('      median %8.2f dBFS   p90 %8.2f dBFS   max %8.2f dBFS   (%d bins)'
          % (med, p90, mx, int(quiet.sum())))
    print('      tone measured %+.2f dBFS, so the median floor is %.2f dBc and the '
          'worst component %.2f dBc' % (t_lvl, med - t_lvl, mx - t_lvl))
    print()
    print('  VERDICT')
    print('      analysis contributes nothing (exact zero on silence)')
    print('      path floor is %+.2f dB relative to an ideal 24-bit quantiser'
          % (med - qf))
    print('      headroom of the worst reported component above the path floor: '
          '%.1f dB' % (mx - med))
    if (mx - med) < 6.0:
        print('      ** the worst component is within 6 dB of the floor: report it as '
              'a BOUND, not a level')
    return 0


if __name__ == '__main__':
    sys.exit(main())
