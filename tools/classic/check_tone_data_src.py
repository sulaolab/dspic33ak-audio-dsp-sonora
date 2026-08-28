#!/usr/bin/env python3
"""
check_tone_data_src.py -- quality check for the reduced-rate Classic tone tables.

Emulates the runtime SRC of snd_effect_play.c bit-for-bit at the phase level
(Q16 phase accumulator + linear interpolation between adjacent source samples)
and compares the result against the 48 kHz master:

  * RMS error and worst-case sample error, in dB relative to the master peak
  * per-octave-band magnitude error, so HF droop is visible separately
  * peak level change

Usage:
    python tools/classic/check_tone_data_src.py                 # 48 kHz output
    python tools/classic/check_tone_data_src.py --out-rate 96000 44100
"""

import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_tone_data_int16 import (  # noqa: E402
    C_PATH,
    MASTER_RATE_HZ,
    SRC_DIR,
    TONES,
    parse_c_arrays,
    read_wav,
)

PHASE_BITS = 16
PHASE_ONE = 1 << PHASE_BITS


def runtime_src(src, src_rate, out_rate, out_frames):
    """Q16 phase + linear interpolation, as snd_effect_play.c does it."""
    step = (int(src_rate) << PHASE_BITS) + out_rate // 2
    step //= int(out_rate)
    step = max(step, 1)

    phase = np.arange(out_frames, dtype=np.int64) * step
    idx = phase >> PHASE_BITS
    frac = (phase & (PHASE_ONE - 1)).astype(np.float64) / PHASE_ONE

    valid = idx < len(src)
    idx = np.clip(idx, 0, len(src) - 1)
    nxt = np.clip(idx + 1, 0, len(src) - 1)
    y = src[idx] * (1.0 - frac) + src[nxt] * frac
    return np.where(valid, y, 0.0), step


def band_error_db(ref, tst, fs):
    """Magnitude error per band, dB, weighted by the reference's own energy."""
    n = 1 << int(math.ceil(math.log2(max(len(ref), len(tst)))))
    R = np.fft.rfft(np.pad(ref, (0, n - len(ref))) * 1.0)
    T = np.fft.rfft(np.pad(tst, (0, n - len(tst))) * 1.0)
    f = np.fft.rfftfreq(n, 1.0 / fs)
    rows = []
    edges = [0, 250, 500, 1000, 2000, 4000, 8000, 16000, fs / 2]
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (f >= lo) & (f < hi)
        if not m.any():
            continue
        er = np.sqrt((np.abs(R[m]) ** 2).sum())
        et = np.sqrt((np.abs(T[m]) ** 2).sum())
        if er <= 0:
            continue
        rows.append((lo, hi, 20.0 * math.log10(et / er + 1e-30),
                     20.0 * math.log10(er / np.abs(R).max() + 1e-30)))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-rate", type=int, nargs="+", default=[48000])
    args = ap.parse_args()

    arrays = parse_c_arrays(open(C_PATH).read())

    worst = float("-inf")
    for sym, wav_name, _, rate, _, _ in TONES:
        master, m_rate = read_wav(os.path.join(SRC_DIR, wav_name))
        assert m_rate == MASTER_RATE_HZ
        stored = arrays[sym].astype(np.float64)
        m_peak = np.max(np.abs(master))

        print("\n%s  stored %u Hz, %u samples (master %u @ %u Hz)"
              % (sym, rate, len(stored), len(master), MASTER_RATE_HZ))
        print("  peak: master %.0f -> stored %.0f (%+.2f dB)"
              % (m_peak, np.max(np.abs(stored)),
                 20.0 * math.log10(np.max(np.abs(stored)) / m_peak)))

        for out_rate in args.out_rate:
            out_frames = int(math.ceil(len(master) * out_rate / MASTER_RATE_HZ))
            got, step = runtime_src(stored, rate, out_rate, out_frames)
            ref, _ = runtime_src(master, MASTER_RATE_HZ, out_rate, out_frames)

            err = got - ref
            rms = 20.0 * math.log10(np.sqrt(np.mean(err ** 2)) / m_peak + 1e-30)
            mx = 20.0 * math.log10(np.max(np.abs(err)) / m_peak + 1e-30)
            worst = max(worst, rms)
            print("  out %6u Hz  step=0x%05X  err RMS %6.1f dBpk   worst sample %6.1f dBpk"
                  % (out_rate, step, rms, mx))

            if out_rate == args.out_rate[0]:
                for lo, hi, d, ref_lvl in band_error_db(ref, got, out_rate):
                    flag = "   <-- band is %.0f dB down in the source" % -ref_lvl if ref_lvl < -50 else ""
                    print("      %5.0f-%5.0f Hz : %+6.2f dB  (ref %+6.1f dB)%s"
                          % (lo, hi, d, ref_lvl, flag))

    print("\nworst-case RMS error across all tones: %.1f dB below peak" % worst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
