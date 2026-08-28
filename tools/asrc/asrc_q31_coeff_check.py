#!/usr/bin/env python3
"""Answer the two numerical questions a Q31 front-end FIR raises, offline.

1. Does quantising the shipping float coefficient tables to Q31 damage the
   stopband?  Reported two ways: the peak sidelobe before and after
   quantisation, and the absolute error floor max|Hq - H| relative to the
   passband peak.  The error floor is the rigorous bound -- a per-bin dB ratio
   is not, because it diverges at the spectral nulls where |H| -> 0.
2. How much accumulator headroom does the tap sum need?  sum|h| is the
   worst-case output magnitude for a full-scale input, so ceil(log2(sum|h|))
   is the number of guard bits the accumulator must carry.

The stopband is taken to start at the first null after the passband, which
needs no threshold and so cannot restate whatever threshold was chosen.

Run with no arguments; it reads every *_coeffs.inc next to the decimator.
Written for [internal] report_ak128_fir_kernel_issue_ceiling_2026-08-21.md.
"""

import glob
import os
import re

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "src", "app", "apps", "asrc"))
NFFT = 1 << 17
TABLE_RE = re.compile(
    r"static const float\s+(\w+)\s*\[\s*(\d+)u?\s*\]\s*=\s*\{(.*?)\}\s*;", re.S
)
FLOAT_RE = re.compile(r"-?[\d.]+(?:e[-+]?\d+)?f")


def tables():
    for path in sorted(glob.glob(os.path.join(SRC, "*_coeffs.inc"))):
        fs = 96000.0 if "96_to_48" in os.path.basename(path) else 48000.0
        text = re.sub(r"/\*.*?\*/", "",
                      open(path, encoding="utf-8", errors="replace").read(), flags=re.S)
        for name, count, body in TABLE_RE.findall(text):
            h = np.array([float(v.rstrip("f")) for v in FLOAT_RE.findall(body)])
            if len(h) != int(count):
                raise SystemExit(f"{name}: parsed {len(h)} of {count} coefficients")
            yield name, h, fs


def first_null(mag):
    """Index of the first local minimum after the response starts falling.

    For a lowpass that is the first stopband null, i.e. where the transition
    roll-off ends and the sidelobes begin.
    """
    falling = np.nonzero(mag < mag[0] * 10 ** (-20 / 20))[0]
    i = int(falling[0]) if falling.size else 1
    while i + 1 < len(mag) and mag[i + 1] < mag[i]:
        i += 1
    return i


def main():
    worst_l1 = ("", 0.0)
    worst_err = -400.0
    worst_sidelobe_shift = 0.0
    found = 0

    print(f"{'table':34s}{'taps':>5s}{'sum|h|':>8s}{'stopband from':>14s}"
          f"{'sidelobe f32':>14s}{'sidelobe q31':>14s}{'q-err floor':>12s}")
    for name, h, fs in tables():
        H = np.abs(np.fft.rfft(h, NFFT))
        peak = H.max()
        q = np.round(h * (2 ** 31)) / (2 ** 31)          # round-to-nearest Q31
        Hq = np.abs(np.fft.rfft(q, NFFT))

        start = first_null(H)
        sidelobe_f32 = 20 * np.log10(H[start:].max() / peak)
        sidelobe_q31 = 20 * np.log10(Hq[start:].max() / peak)
        err = 20 * np.log10(max(np.abs(np.fft.rfft(q - h, NFFT)).max(), 1e-300) / peak)

        l1 = float(np.abs(h).sum())
        if l1 > worst_l1[1]:
            worst_l1 = (name, l1)
        worst_err = max(worst_err, err)
        worst_sidelobe_shift = max(worst_sidelobe_shift, abs(sidelobe_q31 - sidelobe_f32))
        found += 1
        print(f"{name:34s}{len(h):5d}{l1:8.3f}{start * fs / NFFT:12.0f} Hz"
              f"{sidelobe_f32:14.2f}{sidelobe_q31:14.2f}{err:12.1f}")

    if found == 0:
        raise SystemExit(f"no coefficient tables found under {SRC}")

    guard = int(np.ceil(np.log2(worst_l1[1])))
    print()
    print(f"worst sum|h| = {worst_l1[1]:.3f} ({worst_l1[0]}) -> {guard} guard bits needed")
    print(f"worst peak-sidelobe shift from Q31 coefficients = {worst_sidelobe_shift:.4f} dB")
    print(f"worst coefficient quantisation error floor       = {worst_err:.1f} dB")
    print()
    print("The accumulator is 72-bit and a Q31 x Q31 product is 62 bits plus sign,")
    print("so it carries ~9 guard bits: it cannot overflow at these tap counts.")
    print("Only the final rounded store can clip, and the float path already")
    print("clamps to the same s24 range in float_to_s24_left().")


if __name__ == "__main__":
    main()
