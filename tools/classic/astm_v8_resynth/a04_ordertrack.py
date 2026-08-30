"""Step 4: order tracking -- recover RPM(t) from the recording.

The reference clips are pulls, not steady revs (see out/spec_*.png), so nothing
can be measured against a fixed frequency grid.  Everything downstream (order
amplitudes, half-order rumble, residual noise) needs the instantaneous engine
phase first.

Method
  1. STFT, whiten each frame against its own local median floor
     -> "how far does a line stand out here", independent of overall level.
  2. For every candidate cycle rate f_c (crank order 0.5) score the whole
     half-order comb  k*f_c, k = 1..K  up to F_TRACK_MAX.
  3. Viterbi over frames with a slew penalty, so the track cannot jump an
     octave for one frame the way a per-frame argmax does.
  4. Save rpm(t) *and the per-frame score of the chosen path*.  The score is
     what later steps use to reject frames where the track is not actually
     locked (a long compilation contains gear changes, idle, silence, speech).

V8, 4-stroke:  f_cycle = rpm/120,  firing (order 4) = 8 * f_cycle.

Usage:
  python a04_ordertrack.py <wav-name> [--rpm-lo 600] [--rpm-hi 7400]
                                      [--nperseg 16384] [--hop 2048]
"""
import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal, ndimage  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")

F_TRACK_MAX = 2000.0          # comb scored up to here
FC_STEP = 0.02                # cycle-rate search step [Hz]
SLEW_PENALTY = 0.5            # score units per Hz of frame-to-frame change
MAX_SLEW_HZ_PER_S = 40.0      # cycle-rate slew limit (40 Hz/s = 4800 rpm/s);
                              # real pulls in this material exceed 3000 rpm/s,
                              # and a tight limit makes the tracker settle on a
                              # smooth wrong path instead of failing visibly.
JUMP_COST = 3.0              # one-off cost to teleport anywhere in the search
                              # range: the compilation is spliced, so at a clip
                              # boundary the engine really does jump.


def whitened_frames(m, fs, nperseg, hop):
    f, t, S = signal.spectrogram(m, fs, window="hann", nperseg=nperseg,
                                 noverlap=nperseg - hop, nfft=4 * nperseg,
                                 mode="magnitude", scaling="spectrum")
    keep = f <= F_TRACK_MAX * 1.05
    f, S = f[keep], S[keep]
    db = 20 * np.log10(S + 1e-10)
    half = int(round(25.0 / (f[1] - f[0])))     # local floor over +-25 Hz
    floor = ndimage.median_filter(db, size=(2 * half + 1, 1), mode="nearest")
    W = np.clip(db - floor, 0.0, 18.0)
    return f, t, W, db


def comb_scores(f, W, cands):
    """score[cand, frame] for a half-order comb at spacing `cand`.

    A plain "mean prominence on the grid" is biased towards *twice* the true
    cycle rate: dropping every half-order line raises the mean, because the
    half orders are the weaker half of the family.  That bias is what makes a
    tracker climb to the top of its search range.

    So score both ways at once:
        explained = sum(W on grid) / sum(W in band)   -- punishes 2*fc, which
                                                         leaves half the real
                                                         lines unaccounted for
        density   = sum(W on grid) / number of slots  -- punishes fc/2, which
                                                         adds empty slots
        score     = explained * density = sum^2 / (total * count)
    """
    df = f[1] - f[0]
    nf, nt = W.shape
    total = W.sum(axis=0) + 1e-9
    out = np.zeros((len(cands), nt))
    for ci, fc in enumerate(cands):
        ks = np.arange(1, int(F_TRACK_MAX / fc) + 1)
        idx = np.round(ks * fc / df).astype(int)
        idx = idx[idx < nf]
        acc = np.maximum.reduce([W[np.clip(idx + d, 0, nf - 1)] for d in (-1, 0, 1)])
        s = acc.sum(axis=0)
        out[ci] = s * s / (total * len(idx))
    return out


def viterbi(score, cands, dt):
    ncand, nt = score.shape
    step = cands[1] - cands[0]
    max_jump = max(1, int(round(MAX_SLEW_HZ_PER_S * dt / step)))
    dp = np.full((ncand, nt), -1e9)
    bk = np.zeros((ncand, nt), dtype=np.int32)
    dp[:, 0] = score[:, 0]
    offs = np.arange(-max_jump, max_jump + 1)
    pen = np.abs(offs) * step * SLEW_PENALTY
    ar = np.arange(ncand)
    for t in range(1, nt):
        best = np.full(ncand, -1e9)
        arg = np.zeros(ncand, dtype=np.int32)
        for off, p in zip(offs, pen):
            prev = np.roll(dp[:, t - 1], off)
            if off > 0:
                prev[:off] = -1e9
            elif off < 0:
                prev[off:] = -1e9
            v = prev - p
            upd = v > best
            best[upd] = v[upd]
            arg[upd] = (ar - off)[upd]
        # allow a splice: teleport from the globally best previous state
        gi = int(np.argmax(dp[:, t - 1]))
        gv = dp[gi, t - 1] - JUMP_COST
        tele = gv > best
        best[tele] = gv
        arg[tele] = gi
        dp[:, t] = best + score[:, t]
        bk[:, t] = np.clip(arg, 0, ncand - 1)
    path = np.zeros(nt, dtype=np.int32)
    path[-1] = int(np.argmax(dp[:, -1]))
    for t in range(nt - 1, 0, -1):
        path[t - 1] = bk[path[t], t]
    return cands[path], score[path, np.arange(nt)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?", default="astm_stable.wav")
    ap.add_argument("--rpm-lo", type=float, default=1500.0)
    ap.add_argument("--rpm-hi", type=float, default=7400.0)
    ap.add_argument("--nperseg", type=int, default=16384)
    ap.add_argument("--hop", type=int, default=2048)
    a = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    path = a.wav if os.path.isabs(a.wav) else os.path.join(WAVDIR, a.wav)
    stem = os.path.splitext(os.path.basename(path))[0]

    x, fs = read_wav(path)
    m = x.mean(axis=1)
    m -= m.mean()

    f, t, W, db = whitened_frames(m, fs, a.nperseg, a.hop)
    cands = np.arange(a.rpm_lo / 120.0, a.rpm_hi / 120.0, FC_STEP)
    sc = comb_scores(f, W, cands)
    dt = t[1] - t[0]
    fc_t, score_t = viterbi(sc, cands, dt)
    rpm_t = fc_t * 120.0

    print("%s: %.2f s, %d frames, dt=%.3f s, window %d (%.0f ms)"
          % (stem, len(m) / fs, len(t), dt, a.nperseg, 1000.0 * a.nperseg / fs))
    print("search %.0f..%.0f rpm | track %.0f..%.0f rpm | score mean %.2f"
          % (a.rpm_lo, a.rpm_hi, rpm_t.min(), rpm_t.max(), score_t.mean()))
    print("\n   t[s]     RPM   score")
    for i in range(0, len(t), max(1, len(t) // 24)):
        print("  %6.2f   %5.0f    %5.1f" % (t[i], rpm_t[i], score_t[i]))

    np.savez(os.path.join(OUT, "track_%s.npz" % stem),
             t=t, fc=fc_t, rpm=rpm_t, score=score_t, fs=fs,
             nperseg=a.nperseg, hop=a.hop)

    fig, axes = plt.subplots(3, 1, figsize=(16, 11), constrained_layout=True,
                             height_ratios=[2, 2, 1])
    for ax, fmax in zip(axes[:2], (700.0, 2000.0)):
        sel = f <= fmax
        vm = db[sel].max()
        ax.pcolormesh(t, f[sel], db[sel], vmin=vm - 65, vmax=vm,
                      shading="nearest", cmap="magma")
        for k in range(1, int(fmax / max(fc_t.max(), 1e-6)) + 2):
            ax.plot(t, fc_t * k, lw=0.6,
                    color="cyan" if k % 2 == 0 else "lime",
                    ls="-" if k % 2 == 0 else "--", alpha=0.7)
        ax.set_ylim(0, fmax)
        ax.set_ylabel("Hz")
    axes[0].set_title("%s: tracked grid (lime dashed = half orders, cyan = integer crank orders)" % stem)
    axes[2].plot(t, rpm_t, lw=1.0, label="RPM")
    axes[2].set_ylabel("RPM")
    ax2 = axes[2].twinx()
    ax2.plot(t, score_t, lw=0.8, color="tab:red", alpha=0.7, label="score")
    ax2.set_ylabel("score [dB]", color="tab:red")
    axes[2].grid(alpha=0.3)
    axes[2].set_xlabel("time [s]")
    out = os.path.join(OUT, "track_%s.png" % stem)
    fig.savefig(out, dpi=80)
    plt.close(fig)
    print("\nwrote", out)


if __name__ == "__main__":
    main()
