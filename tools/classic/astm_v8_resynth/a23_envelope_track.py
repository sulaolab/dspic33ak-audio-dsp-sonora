"""Step 23: track low RPM through the *envelope*, not the spectrum.

Neither earlier tracker works on the start-up recording.  The comb tracker needs
half orders resolved, which at 900 rpm are 7.5 Hz apart.  The order-4 ridge
tracker needs order 4 to be the strongest line in a band, but at idle order 4 is
53-67 Hz, sitting right on top of the 45/62/100 Hz background tones these
recordings carry, and cranking has no firing line at all.

The envelope does not have either problem.  Each combustion (and, while cranking,
each compression) is an amplitude event, so the *envelope* has a component at the
firing rate whatever the carrier frequency is -- and a constant background tone
has a constant amplitude, so it contributes nothing to the envelope.  Track the
ridge of the envelope spectrum in 4-130 Hz and read the firing rate straight off:

    rpm = firing_rate * 15          (V8, 4-stroke: 4 firings per crank rev)

That covers cranking (~9 Hz), the start-up flare and idle in one pass.
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

F_LO, F_HI = 4.0, 130.0        # firing-rate search range -> 60 .. 1950 rpm
CARRIER = (300.0, 5000.0)      # band the envelope is taken from
SMOOTH = 0.010                 # dB per rpm of frame-to-frame change


def envelope(m, fs):
    b, a = signal.butter(4, [CARRIER[0] / (fs / 2), CARRIER[1] / (fs / 2)], "band")
    e = np.abs(signal.filtfilt(b, a, m))
    b2, a2 = signal.butter(4, 250.0 / (fs / 2), "low")
    return signal.filtfilt(b2, a2, e)


def track(env, fs, win_s, hop_s):
    nper = int(win_s * fs)
    hop = int(hop_s * fs)
    f, t, S = signal.spectrogram(env - env.mean(), fs, window="hann", nperseg=nper,
                                 noverlap=nper - hop, nfft=8 * nper,
                                 mode="magnitude", scaling="spectrum")
    keep = (f >= F_LO) & (f <= F_HI)
    fb, Sb = f[keep], S[keep]
    db = 20 * np.log10(Sb + 1e-12)
    half = int(round(8.0 / (fb[1] - fb[0])))
    db = np.clip(db - ndimage.median_filter(db, size=(2 * half + 1, 1),
                                            mode="nearest"), 0.0, 30.0)
    nf, nt = db.shape
    dfb = fb[1] - fb[0]
    dt = t[1] - t[0]
    max_jump = max(1, int(round(4000.0 / 15.0 * dt / dfb)))   # <= 4000 rpm/s
    dp = np.full((nf, nt), -1e9)
    bk = np.zeros((nf, nt), np.int32)
    dp[:, 0] = db[:, 0]
    offs = np.arange(-max_jump, max_jump + 1)
    pen = np.abs(offs) * dfb * 15.0 * SMOOTH
    ar = np.arange(nf)
    for i in range(1, nt):
        best = np.full(nf, -1e9)
        arg = np.zeros(nf, np.int32)
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
    path = np.zeros(nt, np.int32)
    path[-1] = int(np.argmax(dp[:, -1]))
    for i in range(nt - 1, 0, -1):
        path[i - 1] = bk[path[i], i]
    fr = np.zeros(nt)
    for i in range(nt):
        j = path[i]
        if 0 < j < nf - 1:
            y0, y1, y2 = db[j - 1, i], db[j, i], db[j + 1, i]
            d = y0 - 2 * y1 + y2
            fr[i] = fb[j] + (0.5 * (y0 - y2) / d if abs(d) > 1e-9 else 0.0) * dfb
        else:
            fr[i] = fb[j]
    return fb, t, db, fr, db[path, np.arange(nt)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", nargs="?", default="astm_v8_cell_motor+idling.wav")
    ap.add_argument("--win", type=float, default=0.25)
    ap.add_argument("--hop", type=float, default=0.010)
    ap.add_argument("--suffix", default="env")
    a = ap.parse_args()
    path = a.wav if os.path.isabs(a.wav) else os.path.join(WAVDIR, a.wav)
    stem = os.path.splitext(os.path.basename(path))[0]
    x, fs = read_wav(path)
    m = x.mean(axis=1)
    m -= m.mean()
    env = envelope(m, fs)
    fb, t, db, fr, score = track(env, fs, a.win, a.hop)
    rpm = fr * 15.0
    print("%s: %.2f s, envelope window %.0f ms, %d frames"
          % (stem, len(m) / fs, 1000 * a.win, len(t)))
    print("  firing rate %.1f..%.1f Hz -> rpm %.0f..%.0f, score mean %.1f dB"
          % (fr.min(), fr.max(), rpm.min(), rpm.max(), score.mean()))
    print("\n   t[s]  firing[Hz]    RPM   score[dB]")
    for i in range(0, len(t), max(1, len(t) // 26)):
        print("  %5.2f   %7.2f    %5.0f    %5.1f" % (t[i], fr[i], rpm[i], score[i]))

    np.savez(os.path.join(OUT, "track_%s_%s.npz" % (stem, a.suffix)),
             t=t, fc=rpm / 120.0, rpm=rpm, score=score, fs=fs)

    fig, axes = plt.subplots(3, 1, figsize=(14, 10), constrained_layout=True,
                             height_ratios=[2, 2, 1.4])
    f2, t2, S2 = signal.spectrogram(m, fs, nperseg=8192, noverlap=8192 - 512,
                                   mode="magnitude")
    k = f2 <= 900
    d2 = 20 * np.log10(S2[k] + 1e-10)
    axes[0].pcolormesh(t2, f2[k], d2, vmin=d2.max() - 60, vmax=d2.max(),
                       shading="nearest", cmap="magma")
    for o, c in ((4, "lime"), (8, "cyan")):
        axes[0].plot(t, rpm * o / 60.0, lw=0.9, color=c, label="order %d" % o)
    axes[0].legend(fontsize=8, loc="upper right")
    axes[0].set_ylabel("Hz")
    axes[0].set_title("%s: audio spectrogram with the orders implied by the envelope track" % stem)
    axes[1].pcolormesh(t, fb, db, vmin=0, vmax=18, shading="nearest", cmap="viridis")
    axes[1].plot(t, fr, lw=0.9, color="red")
    axes[1].set_ylabel("envelope freq [Hz]")
    axes[1].set_title("envelope spectrum (= firing rate) with the tracked ridge")
    axes[2].plot(t, rpm, lw=1.2)
    axes[2].set_ylabel("RPM")
    axes[2].set_xlabel("time [s]")
    axes[2].grid(alpha=0.3)
    ax2 = axes[2].twinx()
    ax2.plot(t, score, lw=0.7, color="tab:red", alpha=0.6)
    ax2.set_ylabel("score [dB]", color="tab:red")
    out = os.path.join(OUT, "envtrack_%s.png" % stem)
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("\nwrote", out)


if __name__ == "__main__":
    main()
