"""Step 21: how few points per engine cycle are enough?

This is the question that sets the ROM budget, and the doc flagged it as
untested.  A table of M points per cycle band-limits that table to crank order
M/4, so the honest way to ask is: at what point count does the *measured content*
start being thrown away, and does it show up in the result?

The wavetable and the noise table are asked separately, because they do not carry
the same bandwidth: the cycle-locked part rolls off fast with order, while the
residual noise still has content past order 60.  Sizing them together wastes ROM
on whichever one is cheaper.

Writes size_wave<M>.wav / size_noise<L>.wav for auditioning.
"""
import os
import sys

import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables  # noqa: E402
from a17_demos import clean_traj  # noqa: E402
from a18_noise_calib import harmonicity  # noqa: E402
from a20_portable_synth import build_noise_table, synth_portable, octaves  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
ACCL = "astm_accl_001"
T_END = 8.0
WAVE_PTS = [2048, 1024, 512, 256, 128, 64]
NOISE_CYC = 8
ROLLOFF, CORNER = 12.0, 16.0     # fitted in a20


def band_limit(w, keep_pts):
    """Emulate storing this cycle in keep_pts samples: drop every order above
    keep_pts/4, then evaluate back on the original grid so only the *stored*
    resolution differs and the comparison isolates that."""
    M = len(w)
    W = np.fft.rfft(w)
    W[keep_pts // 2 + 1:] = 0.0
    return np.fft.irfft(W, M)


class Shrunk(object):
    """A table set whose waveforms are band-limited as if stored at `pts`."""

    def __init__(self, tab, pts):
        self.__dict__.update({k: getattr(tab, k) for k in
                              ("rpm", "gain", "nrms", "nspec", "nenv", "M", "fs")})
        self.wave = np.array([band_limit(w, pts) for w in tab.wave])
        self.interp_at = tab.__class__.interp_at.__get__(self)


def main():
    tab = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    fs = tab.fs
    tr = np.load(os.path.join(OUT, "track_%s_ridge.npz" % ACCL))
    sel = tr["t"] <= T_END
    t = tr["t"][sel]
    rpm = clean_traj(tr["t"], tr["rpm"])[sel]
    x, _ = read_wav(os.path.join(WAVDIR, ACCL + ".wav"))
    ref = x.mean(axis=1)[:int(T_END * fs)]
    ref -= ref.mean()

    print("--- how much cycle-locked energy survives each point count ---")
    print("   pts   max order   energy kept [%]   worst bin [%]")
    for pts in WAVE_PTS:
        keep = []
        for w in tab.wave:
            b = band_limit(w, pts)
            keep.append(b.var() / (w.var() + 1e-20))
        print("  %5d   %8d       %8.2f        %8.2f"
              % (pts, pts // 4, 100 * np.mean(keep), 100 * np.min(keep)))

    tbl = build_noise_table(tab, cycles=NOISE_CYC, rolloff_db_per_oct=ROLLOFF,
                            corner_order=CORNER, quiet=True)
    print("\n--- rendered result vs the full-size table (2048 pts) ---")
    print("   pts   octave rms err   harmonicity err   vs reference (octave rms)")
    base = None
    for pts in WAVE_PTS:
        tb = Shrunk(tab, pts) if pts < tab.M else tab
        y, _ = synth_portable(rpm, t, tb, fs, tbl, jitter=0.004)
        n = min(len(ref), len(y))
        y = y[:n]
        y *= ref.std() / (y.std() + 1e-15)
        o, _ = octaves(y, fs)
        h = np.array(harmonicity(y, fs, t, rpm))
        o_ref, _ = octaves(ref[:n], fs)
        if base is None:
            base = (o, h)
        print("  %5d      %8.2f dB       %8.2f dB          %8.2f dB"
              % (pts, np.sqrt(((o - base[0]) ** 2).mean()),
                 np.sqrt(((h - base[1]) ** 2).mean()),
                 np.sqrt(((o - o_ref) ** 2).mean())))
        write_wav(os.path.join(OUT, "size_wave%d.wav" % pts),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)

    print("\n--- noise table size (its own bandwidth) ---")
    print("   cycles x pts   ROM[kB]   octave rms err vs the 8x2048 table")
    ref_tbl = tbl
    y0, _ = synth_portable(rpm, t, tab, fs, ref_tbl, jitter=0.004)
    n = min(len(ref), len(y0))
    y0 = y0[:n] * (ref.std() / y0[:n].std())
    o0, _ = octaves(y0, fs)
    for cyc, pts in ((8, 2048), (4, 2048), (8, 1024), (4, 1024), (8, 512), (4, 512)):
        t2 = build_noise_table(tab, cycles=cyc, rolloff_db_per_oct=ROLLOFF,
                               corner_order=CORNER, quiet=True)
        if pts < tab.M:                      # band-limit the table itself
            L = len(t2)
            T = np.fft.rfft(t2)
            T[(pts // 2 + 1) * cyc:] = 0.0
            t2 = np.fft.irfft(T, L)
            t2 /= t2.std()
        y, _ = synth_portable(rpm, t, tab, fs, t2, jitter=0.004)
        y = y[:n] * (ref.std() / y[:n].std())
        o, _ = octaves(y, fs)
        print("   %d x %-5d       %6.1f      %8.2f dB"
              % (cyc, pts, cyc * pts * 2 / 1024.0, np.sqrt(((o - o0) ** 2).mean())))
        write_wav(os.path.join(OUT, "size_noise%dx%d.wav" % (cyc, pts)),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)

    print("\nwrote size_wave*.wav and size_noise*.wav to out/ for auditioning")


if __name__ == "__main__":
    main()
