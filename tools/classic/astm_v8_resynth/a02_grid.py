"""Step 2: find the true harmonic spacing of the reference recording.

Why this step is separate: a naive comb fit is wide open to an octave error.
For a 4-stroke V8 the spectrum is a comb spaced at the *engine cycle* rate
(rpm/120 = crank order 0.5).  If we mistake that comb for one spaced at the
*crank* rate (rpm/60 = order 1) we silently throw away every half-order line --
which is exactly the cross-plane "rumble" this project is missing.  So the
spacing is decided by an explicit octave test, not by whoever scores highest.

Outputs (printed):
  * RPM stability over time (is astm_stable.wav really steady?)
  * octave test: prominence of the grid lines for spacing D, D/2, D/4
  * final f_cycle / RPM
Saves out/grid.json for the later steps.
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

# `or`, not a get() default: the default must not be evaluated when ASTM_WAV is
# set, because source_dir() refuses to guess and exits.
SRC = os.environ.get("ASTM_WAV") or os.path.join(source_dir(), "astm_stable.wav")
OUT = os.path.join(HERE, "out")

# analysis band used for every comb / grid decision
F_LO, F_HI = 30.0, 2000.0


def mag_spectrum(sig, fs, nfft):
    w = np.hanning(len(sig))
    X = np.abs(np.fft.rfft(sig * w, nfft)) / (np.sum(w) / 2.0)
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    return f, X


def local_floor(X, half_bins):
    """Median-filtered magnitude = local noise floor (cheap running median)."""
    n = len(X)
    pad = np.pad(X, half_bins, mode="edge")
    # strided window median
    idx = np.arange(n)[:, None] + np.arange(2 * half_bins + 1)[None, :]
    return np.median(pad[idx], axis=1)


def grid_prominence(f, X, floor, fs, nfft, spacing, fmax=F_HI):
    """For a candidate spacing, return per-line prominence over local floor [dB]."""
    df = fs / nfft
    proms = []
    k = 1
    while spacing * k <= fmax:
        ft = spacing * k
        if ft >= F_LO:
            i = int(round(ft / df))
            wgt = max(2, int(round(0.004 * ft / df)) + 2)  # +-0.4% of freq
            amp = X[max(i - wgt, 0): i + wgt + 1].max()
            fl = floor[max(i - wgt, 0): i + wgt + 1].mean()
            proms.append((k, ft, 20 * np.log10(amp / (fl + 1e-15))))
        k += 1
    return proms


def comb_score(f, X, floor, fs, nfft, spacing):
    """Mean prominence over the fixed band -- comparable across spacings."""
    p = grid_prominence(f, X, floor, fs, nfft, spacing)
    if not p:
        return -99.0
    return float(np.mean([d for _, _, d in p]))


def main():
    os.makedirs(OUT, exist_ok=True)
    x, fs = read_wav(SRC)
    m = x.mean(axis=1)
    m -= m.mean()
    print("src: %s  (%.2f s @ %d Hz)" % (os.path.basename(SRC), len(m) / fs, fs))

    # ---------- 1) is it steady?  track spacing frame by frame ----------
    nfft = 1 << 16          # 0.73 Hz/bin -- plenty for a 0.5-order grid
    win = fs                 # 1.0 s frames
    hop = fs // 2
    print("\n--- RPM stability (1.0 s frames, spectral-autocorrelation spacing) ---")
    print("   t[s]   spacing[Hz]  rpm(cycle-rate)   rpm(crank-rate)")
    tracks = []
    for st in range(0, len(m) - win, hop):
        seg = m[st:st + win]
        f, X = mag_spectrum(seg, fs, nfft)
        band = (f >= F_LO) & (f <= F_HI)
        S = X[band]
        S = S / (S.max() + 1e-15)
        S = S - S.mean()
        ac = np.correlate(S, S, mode="full")[len(S) - 1:]
        ac /= ac[0] + 1e-15
        df = fs / nfft
        lo = int(15.0 / df)      # spacing >= 15 Hz  -> rpm >= 1800 (cycle rate)
        hi = int(90.0 / df)      # spacing <= 90 Hz
        j = lo + int(np.argmax(ac[lo:hi]))
        sp = j * df
        tracks.append(sp)
        print("  %5.1f   %8.2f     %10.1f      %10.1f"
              % (st / fs, sp, sp * 120.0, sp * 60.0))
    sp_med = float(np.median(tracks))
    print("  median spacing = %.2f Hz   (spread %.2f Hz)"
          % (sp_med, max(tracks) - min(tracks)))

    # ---------- 2) refine on the whole file ----------
    nfft = 1 << 20
    f, X = mag_spectrum(m, fs, nfft)
    floor = local_floor(X, half_bins=int(round(6.0 / (fs / nfft))))  # +-6 Hz median
    best = (sp_med, comb_score(f, X, floor, fs, nfft, sp_med))
    for sp in np.arange(sp_med - 1.0, sp_med + 1.0, 0.002):
        s = comb_score(f, X, floor, fs, nfft, sp)
        if s > best[1]:
            best = (float(sp), s)
    D = best[0]
    print("\n--- refined spacing on full file: %.3f Hz (mean prominence %.1f dB) ---"
          % (D, best[1]))

    # ---------- 3) octave test ----------
    print("\n--- octave test: which spacing is the real comb? ---")
    print("  hypothesis      spacing[Hz]  lines  mean_prom  >6dB   only-new-lines")
    results = {}
    for name, div in (("D  (as found)", 1.0), ("D/2", 2.0), ("D/4", 4.0)):
        sp = D / div
        p = grid_prominence(f, X, floor, fs, nfft, sp)
        mean_p = np.mean([d for _, _, d in p])
        hit = sum(1 for _, _, d in p if d > 6.0)
        # "only-new-lines": grid points that do NOT coincide with the coarser grid
        if div == 1.0:
            new = p
        else:
            new = [(k, ft, d) for (k, ft, d) in p if (k % int(div)) != 0]
        mean_new = np.mean([d for _, _, d in new]) if new else float("nan")
        hit_new = sum(1 for _, _, d in new if d > 6.0)
        results[name] = dict(spacing=sp, mean_prom=float(mean_p),
                             mean_new=float(mean_new), n_new=len(new),
                             hit_new=int(hit_new))
        print("  %-14s %8.3f   %4d   %6.1f dB  %3d/%-3d  %5.1f dB (%d/%d lines)"
              % (name, sp, len(p), mean_p, hit, len(p), mean_new, hit_new, len(new)))
    print("\n  Read as: if the *only-new-lines* of D/2 still stand clearly above the")
    print("  local floor, then D was an octave too high and the true comb is D/2.")

    with open(os.path.join(OUT, "grid.json"), "w") as fp:
        json.dump(dict(src=SRC, fs=fs, spacing_found=D,
                       spacing_track=tracks, octave=results), fp, indent=1)


if __name__ == "__main__":
    main()
