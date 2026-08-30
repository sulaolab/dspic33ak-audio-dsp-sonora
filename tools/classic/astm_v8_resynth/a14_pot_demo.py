"""Step 14: POT-style demos -- what the model does under fast throttle moves.

Uses the design-locked table set only (3375-6875 rpm), so nothing here depends on
the unresolved idle work.  The point is the *transitions*: a wavetable read at a
fast-slewing phase rate is where a synth usually gives itself away (zipper noise
on the RPM crossfade, smeared rumble, or a chirp artefact).

Writes out/pot_<name>.wav plus a spectrogram sheet so the transitions can be
inspected as well as heard.
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
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402

OUT = os.path.join(HERE, "out")
LO, HI = 3400.0, 6800.0

# (name, [(t, rpm), ...])  -- linear between keys, so the slope is the slew rate
DEMOS = [
    ("snap", [(0.0, LO), (0.8, LO), (0.95, HI), (1.6, HI), (1.75, LO), (2.6, LO),
              (2.7, HI), (3.4, HI), (3.5, LO), (4.3, LO)]),
    ("blip", [(0.0, LO), (0.5, LO), (0.62, HI), (0.80, HI), (0.95, LO), (1.6, LO),
              (1.70, 6200), (1.85, LO), (2.5, LO), (2.58, HI), (2.70, LO), (3.4, LO)]),
    ("wiggle", [(0.0, 4000)] + [(0.5 + 0.25 * i, 4000 if i % 2 else 6000)
                                for i in range(12)] + [(3.6, 4000), (4.2, 4000)]),
    ("slew_ladder", [(0.0, LO), (0.4, LO),
                     (1.4, HI), (1.9, HI),          # 3400 rpm/s
                     (2.4, LO), (2.9, LO),
                     (3.15, HI), (3.65, HI),        # 13600 rpm/s
                     (3.9, LO), (4.4, LO),
                     (4.45, HI), (4.95, HI),        # 68000 rpm/s (step-like)
                     (5.0, LO), (5.6, LO)]),
]


def main():
    tab = EngineTables()
    fs = tab.fs
    print("tables %.0f..%.0f rpm" % (tab.rpm[0], tab.rpm[-1]))
    fig, axes = plt.subplots(len(DEMOS), 1, figsize=(14, 3.1 * len(DEMOS)),
                             constrained_layout=True)
    for ax, (name, keys) in zip(np.atleast_1d(axes), DEMOS):
        tk = np.array([k[0] for k in keys], float)
        rk = np.array([float(k[1]) for k in keys])
        tg = np.arange(0.0, tk[-1], 0.002)
        rg = np.interp(tg, tk, rk)
        slew = np.max(np.abs(np.diff(rg)) / 0.002)
        y, ty, rpm_y = synth(rg, tg, tab, fs, jitter=0.004)
        pk = np.abs(y).max()
        y = y / (pk + 1e-12) * 0.9
        write_wav(os.path.join(OUT, "pot_%s.wav" % name), y, fs, 3)
        print("  %-12s %5.2f s  peak slew %7.0f rpm/s  rms %.1f dBFS -> out/pot_%s.wav"
              % (name, len(y) / fs, slew, 20 * np.log10(y.std()), name))

        f, t, S = signal.spectrogram(y, fs, nperseg=4096, noverlap=4096 - 512,
                                     mode="magnitude")
        k = f <= 2200
        db = 20 * np.log10(S[k] + 1e-10)
        ax.pcolormesh(t, f[k], db, vmin=db.max() - 60, vmax=db.max(),
                      shading="nearest", cmap="magma")
        ax2 = ax.twinx()
        ax2.plot(ty, rpm_y, color="cyan", lw=0.9, alpha=0.8)
        ax2.set_ylabel("RPM", color="cyan")
        ax.set_title("pot_%s  (peak slew %.0f rpm/s)" % (name, slew))
        ax.set_ylabel("Hz")
    np.atleast_1d(axes)[-1].set_xlabel("time [s]")
    out = os.path.join(OUT, "pot_demos.png")
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    main()
