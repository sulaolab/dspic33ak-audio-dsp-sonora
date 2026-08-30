"""Step 18: calibrate the synthesised noise level against the reference.

The A/B spectrogram of the acceleration clip shows the model's low orders in the
right place but its 750-2200 Hz region smeared where the reference has distinct
harmonic lines.  That is a line-to-noise ratio error, not a level error, and it
has a measurable cause: the "residual" measured per bin is *everything* that does
not repeat cycle to cycle, which includes the recording's own noise, wind and
room.  Using it as the engine's noise level therefore overstates it.

So measure harmonicity directly -- how far the half-order grid stands above the
midpoints between its teeth, per frequency band -- for the reference and for the
synth at several noise scalings, and pick the scaling that matches.  One global
parameter, fitted to a measurement rather than to taste.
"""
import os
import sys

import numpy as np
from scipy import signal, ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a17_demos import clean_traj  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
ACCL = "astm_accl_001"
T_END = 8.0
BANDS = [(250.0, 750.0), (750.0, 1500.0), (1500.0, 2200.0)]
SCALES = [0.2, 0.35, 0.5, 0.7, 1.0]


def harmonicity(sig, fs, t_traj, rpm_traj):
    """Per band: mean prominence on the half-order grid minus at the midpoints."""
    f, t, S = signal.spectrogram(sig, fs, window="hann", nperseg=8192,
                                 noverlap=8192 - 2048, nfft=16384, mode="magnitude")
    db = 20 * np.log10(S + 1e-10)
    half = int(round(60.0 / (f[1] - f[0])))
    W = np.clip(db - ndimage.median_filter(db, size=(2 * half + 1, 1), mode="nearest"),
                0.0, 24.0)
    rp = np.interp(t, t_traj, rpm_traj)
    df = f[1] - f[0]
    out = []
    for (lo, hi) in BANDS:
        g, mid = [], []
        for i in range(len(t)):
            fc = rp[i] / 120.0
            ks = np.arange(max(1, int(lo / fc)), int(hi / fc) + 1)
            if len(ks) < 4:
                continue
            gi = np.round(ks * fc / df).astype(int)
            mi = np.round((ks - 0.5) * fc / df).astype(int)
            gi = gi[(gi > 0) & (gi < len(f))]
            mi = mi[(mi > 0) & (mi < len(f))]
            g.append(np.maximum.reduce([W[np.clip(gi + d, 0, len(f) - 1), i]
                                        for d in (-1, 0, 1)]).mean())
            mid.append(np.maximum.reduce([W[np.clip(mi + d, 0, len(f) - 1), i]
                                          for d in (-1, 0, 1)]).mean())
        out.append(float(np.mean(g) - np.mean(mid)))
    return out


def main():
    tr = np.load(os.path.join(OUT, "track_%s_ridge.npz" % ACCL))
    sel = tr["t"] <= T_END
    t = tr["t"][sel]
    rpm = clean_traj(tr["t"], tr["rpm"])[sel]
    x, fs = read_wav(os.path.join(WAVDIR, ACCL + ".wav"))
    ref = x.mean(axis=1)[:int(T_END * fs)]
    ref -= ref.mean()

    h_ref = harmonicity(ref, fs, t, rpm)
    print("reference harmonicity (grid minus midpoint) [dB]")
    print("   " + "  ".join("%d-%d Hz: %+5.2f" % (b[0], b[1], v)
                            for b, v in zip(BANDS, h_ref)))

    tab = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    print("\nsynth at several noise scalings")
    print("  scale   " + "".join("%12s" % ("%d-%d Hz" % b) for b in BANDS)
          + "     rms err")
    best = None
    for s in SCALES:
        y, ty, ry = synth(rpm, t, tab, fs, noise_scale=s, jitter=0.004)
        h = harmonicity(y[:len(ref)], fs, t, rpm)
        err = np.sqrt(np.mean((np.array(h) - np.array(h_ref)) ** 2))
        print("  %5.2f   " % s + "".join("%+11.2f" % v for v in h)
              + "   %8.2f dB" % err)
        if best is None or err < best[0]:
            best = (err, s, h)
    print("\n  best noise scale = %.2f (rms harmonicity error %.2f dB)"
          % (best[1], best[0]))
    print("  -> pass noise_scale=%.2f to synth(), or scale noise_rms in the tables"
          % best[1])
    with open(os.path.join(OUT, "noise_scale.txt"), "w") as fp:
        fp.write("%.3f\n" % best[1])


if __name__ == "__main__":
    main()
