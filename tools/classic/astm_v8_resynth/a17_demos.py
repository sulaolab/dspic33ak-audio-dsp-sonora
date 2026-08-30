"""Step 17: the listening set built on tables_v3.

  ab_accl_ref.wav / ab_accl_syn.wav
      The acceleration clip and the synth driven by *that clip's own* RPM
      trajectory (from the order-4 ridge track), level-matched.  This is the
      "does it accelerate like the real thing" test, gear changes included.
  demo_accl_style.wav
      The same trajectory, synth only, no reference to compare against -- for
      judging it as a sound rather than as a copy.
  demo_full_range.wav
      750 -> 7000 -> 750 rpm through the whole table set, with pulls and shifts.
  demo_idle_notilt.wav / demo_idle_tilt.wav
      The two idle variants (see a16); pick one by ear.

The trajectory is median-filtered and slew-limited before use: the ridge track
has a few frames where it loses the line, and an unfiltered RPM spike would be
rendered faithfully as a chirp that is not in the recording.
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
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
ACCL = "astm_accl_001"
MAX_SLEW = 4000.0        # rpm/s allowed in a cleaned trajectory


def clean_traj(t, rpm, med_s=0.10):
    dt = t[1] - t[0]
    r = ndimage.median_filter(rpm, size=max(3, int(med_s / dt) | 1))
    out = r.copy()
    for i in range(1, len(out)):
        d = np.clip(out[i] - out[i - 1], -MAX_SLEW * dt, MAX_SLEW * dt)
        out[i] = out[i - 1] + d
    return out


def main():
    tab = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    fs = tab.fs
    print("tables_v3: %d bins, %.0f..%.0f rpm" % (len(tab.rpm), tab.rpm[0], tab.rpm[-1]))

    # ---------------- A/B on the acceleration clip ----------------
    tr = np.load(os.path.join(OUT, "track_%s_ridge.npz" % ACCL))
    t, rpm = tr["t"], clean_traj(tr["t"], tr["rpm"])
    x, _ = read_wav(os.path.join(WAVDIR, ACCL + ".wav"))
    ref = x.mean(axis=1)
    ref -= ref.mean()
    print("\nA/B trajectory: %.1f..%.0f rpm over %.2f s (peak slew %.0f rpm/s)"
          % (rpm.min(), rpm.max(), t[-1], np.max(np.abs(np.diff(rpm))) / (t[1] - t[0])))
    y, ty, rpm_y = synth(rpm, t, tab, fs, jitter=0.004)
    n = min(len(ref), len(y))
    ref, y = ref[:n], y[:n]
    y *= ref.std() / (y.std() + 1e-15)
    pk = max(np.abs(ref).max(), np.abs(y).max())
    write_wav(os.path.join(OUT, "ab_accl_ref.wav"), ref / pk * 0.9, fs, 3)
    write_wav(os.path.join(OUT, "ab_accl_syn.wav"), y / pk * 0.9, fs, 3)
    write_wav(os.path.join(OUT, "demo_accl_style.wav"),
              y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)
    print("  reference %.1f dBFS rms / synth %.1f dBFS rms (matched before writing)"
          % (20 * np.log10(ref.std()), 20 * np.log10(y.std())))

    print("\n  order content over the passage [dB]")
    print("   order    ref    syn   diff")
    worst = 0.0
    for o in (1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0, 8.0, 10.0, 12.0, 16.0, 20.0):
        vals = []
        for sig in (ref, y):
            f, tt, S = signal.spectrogram(sig, fs, nperseg=4096, noverlap=4096 - 512,
                                          mode="magnitude")
            rp = np.interp(tt, t, rpm)
            amp = [S[max(int(round(o * rp[i] / 60.0 / (f[1] - f[0]))) - 1, 0):
                     int(round(o * rp[i] / 60.0 / (f[1] - f[0]))) + 2, i].max()
                   for i in range(len(tt))]
            vals.append(20 * np.log10(np.mean(amp) + 1e-12))
        d = vals[1] - vals[0]
        worst = max(worst, abs(d))
        print("   %5.1f  %6.1f %6.1f  %+5.1f" % (o, vals[0], vals[1], d))
    print("   worst |diff| = %.1f dB" % worst)

    fig, axes = plt.subplots(3, 1, figsize=(15, 11), constrained_layout=True,
                             height_ratios=[3, 3, 1])
    for ax, sig, ttl in zip(axes[:2], (ref, y), ("reference " + ACCL, "resynthesis")):
        f, tt, S = signal.spectrogram(sig, fs, nperseg=8192, noverlap=8192 - 1024,
                                      mode="magnitude")
        k = f <= 2200
        db = 20 * np.log10(S[k] + 1e-10)
        ax.pcolormesh(tt, f[k], db, vmin=db.max() - 62, vmax=db.max(),
                      shading="nearest", cmap="magma")
        ax.set_title(ttl)
        ax.set_ylabel("Hz")
    axes[2].plot(t, rpm, lw=1.0)
    axes[2].set_ylabel("RPM")
    axes[2].set_xlabel("time [s]")
    axes[2].grid(alpha=0.3)
    fig.savefig(os.path.join(OUT, "ab_accl_compare.png"), dpi=85)
    plt.close(fig)

    # ---------------- full range ----------------
    keys = [(0.0, 750), (2.0, 750), (2.3, 1600), (4.5, 6900), (4.9, 6900),
            (5.3, 4600), (7.4, 7000), (7.8, 7000), (8.2, 5000), (10.4, 6950),
            (10.9, 6950), (11.6, 2600), (13.0, 2600), (13.4, 5200), (14.4, 5200),
            (15.2, 900), (17.5, 750), (19.0, 750)]
    tk = np.array([k[0] for k in keys], float)
    rk = np.array([float(k[1]) for k in keys])
    tg = np.arange(0.0, tk[-1], 0.002)
    rg = np.interp(tg, tk, rk)
    d, _, _ = synth(rg, tg, tab, fs, jitter=0.005)
    write_wav(os.path.join(OUT, "demo_full_range.wav"),
              d / (np.abs(d).max() + 1e-9) * 0.9, fs, 3)
    print("\n  demo_full_range.wav: %.1f s, %.0f..%.0f rpm" % (len(d) / fs, rk.min(), rk.max()))

    # ---------------- idle variants ----------------
    for tag, path in (("notilt", "tables_v3.npz"), ("tilt", "tables_v3_tilt.npz")):
        tb = EngineTables(os.path.join(OUT, path))
        tg = np.arange(0.0, 6.0, 0.002)
        rg = np.full_like(tg, tb.rpm[0])
        yy, _, _ = synth(rg, tg, tb, fs, jitter=0.008)
        write_wav(os.path.join(OUT, "demo_idle_%s.wav" % tag),
                  yy / (np.abs(yy).max() + 1e-9) * 0.9, fs, 3)
        print("  demo_idle_%s.wav: %.0f rpm, %.1f s" % (tag, tb.rpm[0], len(yy) / fs))

    print("\nwrote the listening set to out/")


if __name__ == "__main__":
    main()
