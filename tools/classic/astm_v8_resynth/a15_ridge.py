"""Step 15: track RPM by following the order-4 ridge, not the whole comb.

Why this exists.  a04 scores the entire half-order comb, which is what makes it
trustworthy in the middle of the range but also what makes it fail at low RPM on
a fast pull: at 2500 rpm the half orders are 20.8 Hz apart, and a window short
enough to follow a 780 rpm/s ramp has a wider main lobe than that (see §11 of the
analysis doc).  But nothing in the *demo* or in the *phase integration* needs the
half orders to be resolved -- it only needs f_cycle(t).  And order 4, the firing
order of a V8, is a single strong ridge with no octave ambiguity to resolve as
long as it is followed continuously.

So: band-limit to where order 4 can be, whiten per frame, Viterbi the ridge, then
refine each frame with a parabolic peak fit.  f_cycle = f_order4 / 8.

Validation is not optional here: run with --check against a segment whose RPM is
already known from a04 and the printed error must stay small.

Usage:
  python a15_ridge.py <wav> [--rpm-lo 2000] [--rpm-hi 7600] [--nperseg 4096]
                            [--check t0,t1,stem]   compare with a04's track
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

MAX_SLEW_RPM_PER_S = 6000.0
SMOOTH_PENALTY = 0.004        # dB per rpm of frame-to-frame change


def notch_static_lines(db, fb, thresh_db=6.0):
    """Zero rows that hold a *stationary* line.

    Needed for the start-up flare: order 4 passes straight through the 100 Hz
    background tone that every one of these recordings carries, and the ridge
    would rather sit on the tone than follow the engine.  Taking the median over
    time kills a sweeping line but keeps a constant one, so a line that survives
    the time-median is by definition not engine-locked.

    Only safe when the engine speed is *moving*; on steady material the engine
    lines are stationary too and this would notch them out.
    """
    med_t = np.median(db, axis=1)
    half = int(round(25.0 / (fb[1] - fb[0])))
    floor_f = ndimage.median_filter(med_t, size=2 * half + 1, mode="nearest")
    hit = np.where(med_t - floor_f > thresh_db)[0]
    kill = set()
    for i in hit:
        kill.update((i - 1, i, i + 1))
    kill = np.array(sorted(k for k in kill if 0 <= k < len(fb)), dtype=int)
    if len(kill):
        db = db.copy()
        db[kill] = 0.0
    return db, fb[hit] if len(hit) else np.array([])


def ridge_track(m, fs, rpm_lo, rpm_hi, nperseg, hop, notch_static=False):
    f, t, S = signal.spectrogram(m, fs, window="hann", nperseg=nperseg,
                                 noverlap=nperseg - hop, nfft=4 * nperseg,
                                 mode="magnitude", scaling="spectrum")
    flo, fhi = rpm_lo * 4 / 60.0, rpm_hi * 4 / 60.0
    keep = (f >= flo * 0.9) & (f <= fhi * 1.1)
    fb, Sb = f[keep], S[keep]
    db = 20 * np.log10(Sb + 1e-10)
    # whiten: subtract a wide median along frequency so a loud frame does not
    # simply win, and a broadband hump does not look like a ridge
    half = int(round(60.0 / (fb[1] - fb[0])))
    db = db - ndimage.median_filter(db, size=(2 * half + 1, 1), mode="nearest")
    db = np.clip(db, 0.0, 30.0)
    if notch_static:
        db, killed = notch_static_lines(db, fb)
        if len(killed):
            print("  notched %d stationary line(s): %s Hz"
                  % (len(killed), ", ".join("%.1f" % v for v in killed[:12])))

    nf, nt = db.shape
    dfb = fb[1] - fb[0]
    dt = t[1] - t[0]
    max_jump = max(1, int(round(MAX_SLEW_RPM_PER_S * 4 / 60.0 * dt / dfb)))
    dp = np.full((nf, nt), -1e9)
    bk = np.zeros((nf, nt), dtype=np.int32)
    dp[:, 0] = db[:, 0]
    offs = np.arange(-max_jump, max_jump + 1)
    pen = np.abs(offs) * dfb * 15.0 * SMOOTH_PENALTY   # dfb*15 = rpm per bin
    ar = np.arange(nf)
    for i in range(1, nt):
        best = np.full(nf, -1e9)
        arg = np.zeros(nf, dtype=np.int32)
        for off, p in zip(offs, pen):
            prev = np.roll(dp[:, i - 1], off)
            if off > 0:
                prev[:off] = -1e9
            elif off < 0:
                prev[off:] = -1e9
            v = prev - p
            u = v > best
            best[u] = v[u]
            arg[u] = (ar - off)[u]
        dp[:, i] = best + db[:, i]
        bk[:, i] = np.clip(arg, 0, nf - 1)
    path = np.zeros(nt, dtype=np.int32)
    path[-1] = int(np.argmax(dp[:, -1]))
    for i in range(nt - 1, 0, -1):
        path[i - 1] = bk[path[i], i]

    # parabolic refinement on the whitened surface
    fr = np.zeros(nt)
    for i in range(nt):
        j = path[i]
        if 0 < j < nf - 1:
            y0, y1, y2 = db[j - 1, i], db[j, i], db[j + 1, i]
            d = y0 - 2 * y1 + y2
            shift = 0.5 * (y0 - y2) / d if abs(d) > 1e-9 else 0.0
            fr[i] = fb[j] + np.clip(shift, -1, 1) * dfb
        else:
            fr[i] = fb[j]
    score = db[path, np.arange(nt)]
    return fb, t, db, fr, score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--rpm-lo", type=float, default=2000.0)
    ap.add_argument("--rpm-hi", type=float, default=7600.0)
    ap.add_argument("--nperseg", type=int, default=4096)
    ap.add_argument("--hop", type=int, default=512)
    ap.add_argument("--check", default=None,
                    help="t0,t1 -- compare against a04's track over that window")
    ap.add_argument("--suffix", default="ridge")
    ap.add_argument("--notch-static", action="store_true",
                    help="zero stationary lines first; only valid when the engine"
                         " speed is moving throughout")
    a = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    path = a.wav if os.path.isabs(a.wav) else os.path.join(WAVDIR, a.wav)
    stem = os.path.splitext(os.path.basename(path))[0]
    x, fs = read_wav(path)
    m = x.mean(axis=1)
    m -= m.mean()

    fb, t, db, f4, score = ridge_track(m, fs, a.rpm_lo, a.rpm_hi, a.nperseg,
                                       a.hop, a.notch_static)
    rpm = f4 * 15.0
    fc = rpm / 120.0
    print("%s: %.2f s, %d frames, dt=%.1f ms, window %.0f ms"
          % (stem, len(m) / fs, len(t), 1000 * (t[1] - t[0]),
             1000.0 * a.nperseg / fs))
    print("  ridge rpm %.0f..%.0f, score mean %.1f dB" % (rpm.min(), rpm.max(), score.mean()))

    if a.check:
        t0, t1 = [float(v) for v in a.check.split(",")]
        ref = np.load(os.path.join(OUT, "track_%s.npz" % stem))
        sel = (t >= t0) & (t <= t1)
        rr = np.interp(t[sel], ref["t"], ref["rpm"])
        err = rpm[sel] - rr
        print("  CHECK vs a04 over t=%.2f..%.2f s: mean %+.0f rpm, rms %.0f rpm,"
              " max |err| %.0f rpm  (%.1f %% of mean rpm)"
              % (t0, t1, err.mean(), np.sqrt((err ** 2).mean()), np.abs(err).max(),
                 100 * np.sqrt((err ** 2).mean()) / rr.mean()))

    np.savez(os.path.join(OUT, "track_%s_%s.npz" % (stem, a.suffix)),
             t=t, fc=fc, rpm=rpm, score=score, fs=fs,
             nperseg=a.nperseg, hop=a.hop)

    print("\n   t[s]     RPM   score[dB]")
    for i in range(0, len(t), max(1, len(t) // 20)):
        print("  %6.2f   %5.0f    %5.1f" % (t[i], rpm[i], score[i]))

    fig, axes = plt.subplots(2, 1, figsize=(15, 9), constrained_layout=True,
                             height_ratios=[3, 1])
    f, tt, S = signal.spectrogram(m, fs, nperseg=8192, noverlap=8192 - 1024,
                                  mode="magnitude")
    k = f <= 900
    d2 = 20 * np.log10(S[k] + 1e-10)
    axes[0].pcolormesh(tt, f[k], d2, vmin=d2.max() - 62, vmax=d2.max(),
                       shading="nearest", cmap="magma")
    for o, c in ((2, "deepskyblue"), (4, "lime"), (8, "orange")):
        axes[0].plot(t, rpm * o / 60.0, lw=0.8, color=c, label="order %d" % o)
    axes[0].legend(fontsize=8, loc="upper left")
    axes[0].set_ylim(0, 900)
    axes[0].set_ylabel("Hz")
    axes[0].set_title("%s: order-4 ridge track (lime), with orders 2 and 8 drawn from it" % stem)
    axes[1].plot(t, rpm, lw=1.0)
    axes[1].set_ylabel("RPM")
    ax2 = axes[1].twinx()
    ax2.plot(t, score, lw=0.7, color="tab:red", alpha=0.7)
    ax2.set_ylabel("ridge score [dB]", color="tab:red")
    axes[1].grid(alpha=0.3)
    axes[1].set_xlabel("time [s]")
    out = os.path.join(OUT, "ridge_%s.png" % stem)
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("\nwrote", out)


if __name__ == "__main__":
    main()
