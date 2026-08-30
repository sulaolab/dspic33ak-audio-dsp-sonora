"""Step 3: look at the material. Spectrogram of the reference clip(s).

Usage:  python a03_spectrogram.py [wav ...]
Writes out/spec_<name>.png
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from scipy import signal  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")


def plot_one(path):
    x, fs = read_wav(path)
    m = x.mean(axis=1)
    m -= m.mean()
    name = os.path.splitext(os.path.basename(path))[0]

    fig, axes = plt.subplots(3, 1, figsize=(15, 12), constrained_layout=True)

    for ax, (nper, fmax, title) in zip(axes, [
        (8192, 3000.0, "wide view 0-3 kHz (8192-pt window)"),
        (16384, 700.0, "order zoom 0-700 Hz (16384-pt window)"),
        (16384, 200.0, "rumble zoom 0-200 Hz (16384-pt window)"),
    ]):
        f, t, S = signal.spectrogram(m, fs, window="hann", nperseg=nper,
                                     noverlap=nper - nper // 8, scaling="spectrum",
                                     mode="magnitude")
        sel = f <= fmax
        db = 20 * np.log10(S[sel] + 1e-10)
        vmax = db.max()
        ax.pcolormesh(t, f[sel], db, vmin=vmax - 70, vmax=vmax,
                      shading="nearest", cmap="magma")
        ax.set_title("%s -- %s" % (name, title))
        ax.set_ylabel("Hz")
        ax.grid(alpha=0.2, color="w", lw=0.3)
    axes[-1].set_xlabel("time [s]")

    out = os.path.join(OUT, "spec_%s.png" % name)
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("wrote", out, " dur %.2f s" % (len(m) / fs))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    args = sys.argv[1:] or ["astm_stable.wav"]
    for a in args:
        plot_one(a if os.path.isabs(a) else os.path.join(WAVDIR, a))
