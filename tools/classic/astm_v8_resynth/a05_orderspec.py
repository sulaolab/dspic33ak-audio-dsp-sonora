"""Step 5: order-domain spectrogram -- the decisive check on the RPM track,
and the first real measurement of the harmonic structure.

Frequency axis is warped to crank order:  f = 2 * f_cycle * order.
If the track from a04 is right, every engine line becomes horizontal and lands
on an integer or half-integer order.  Anything that stays sloped in this view
is not engine-locked (background tone, music, wind).
"""
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

NPERSEG = 16384
HOP = 2048
NFFT = 65536
ORDER_MAX = 32.0
ORDER_STEP = 0.02


def main():
    stem = sys.argv[1] if len(sys.argv) > 1 else "astm_stable"
    tr = np.load(os.path.join(OUT, "track_%s.npz" % stem))
    fc_t, fs = tr["fc"], float(tr["fs"])

    x, _ = read_wav(os.path.join(WAVDIR, stem + ".wav"))
    m = x.mean(axis=1)
    m -= m.mean()

    f, t, S = signal.spectrogram(m, fs, window="hann", nperseg=NPERSEG,
                                 noverlap=NPERSEG - HOP, nfft=NFFT,
                                 mode="magnitude", scaling="spectrum")
    db = 20 * np.log10(S + 1e-10)
    n = min(len(t), len(fc_t))
    t, db, fc_t = t[:n], db[:, :n], fc_t[:n]

    df = f[1] - f[0]
    orders = np.arange(ORDER_STEP, ORDER_MAX, ORDER_STEP)
    img = np.zeros((len(orders), n))
    for i in range(n):
        idx = np.round(2.0 * fc_t[i] * orders / df).astype(int)
        idx = np.clip(idx, 0, len(f) - 1)
        img[:, i] = db[idx, i]

    # local floor along the order axis -> prominence, so "is there a line here"
    half = int(round(0.35 / ORDER_STEP))
    floor = ndimage.median_filter(img, size=(2 * half + 1, 1), mode="nearest")
    prom = img - floor

    fig, axes = plt.subplots(2, 1, figsize=(15, 10), constrained_layout=True)
    vm = img.max()
    axes[0].pcolormesh(t, orders, img, vmin=vm - 65, vmax=vm, shading="nearest",
                       cmap="magma")
    axes[0].set_title("%s: order-domain magnitude (horizontal lines = engine-locked)" % stem)
    axes[1].pcolormesh(t, orders, np.clip(prom, 0, 20), vmin=0, vmax=18,
                       shading="nearest", cmap="viridis")
    axes[1].set_title("prominence over local floor [dB] -- shows which orders exist")
    for ax in axes:
        ax.set_ylabel("crank order")
        ax.set_yticks(np.arange(0, ORDER_MAX + 1, 2))
        ax.grid(alpha=0.25, color="w", lw=0.3)
    axes[-1].set_xlabel("time [s]")
    out = os.path.join(OUT, "orderspec_%s.png" % stem)
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("wrote", out)

    # ---- per-order table, averaged over the whole clip ----
    print("\n--- order content, mean over clip (prominence over local floor) ---")
    print("  order   mean_prom[dB]   mean_mag[dBFS]   kind")
    rows = []
    for o in np.arange(0.5, 24.5, 0.5):
        j = int(round((o - ORDER_STEP) / ORDER_STEP))
        j = np.clip(j, 1, len(orders) - 2)
        p = prom[j - 1:j + 2].max(axis=0).mean()
        g = img[j - 1:j + 2].max(axis=0)
        g = 20 * np.log10(np.mean(10 ** (g / 20)))
        kind = "int " if abs(o - round(o)) < 1e-6 else "HALF"
        rows.append((o, p, g, kind))
        bar = "#" * int(max(0, p))
        print("  %5.1f      %6.2f        %7.1f     %s %s" % (o, p, g, kind, bar))
    np.save(os.path.join(OUT, "orderprom_%s.npy" % stem), np.array([r[:3] for r in rows]))

    ints = [r[1] for r in rows if r[3] == "int "]
    halves = [r[1] for r in rows if r[3] == "HALF"]
    print("\n  mean prominence: integer orders %.2f dB / half orders %.2f dB"
          % (np.mean(ints), np.mean(halves)))
    print("  -> half orders present" if np.mean(halves) > 3.0 else
          "  -> half orders essentially absent")


if __name__ == "__main__":
    main()
