"""Step 9: resynthesise from the measured tables, in Python, and A/B it.

Everything happens on the crank-angle grid, which is what makes the model
simple: the wavetable read *is* the harmonic structure (all orders, half orders
included, with their measured phases), and the noise inherits the engine's
angular statistics for free.

  1. phase:  c(t) = integral of f_cycle dt        (1 cycle = 2 crank revs)
  2. wave:   read wave[rpm] at frac(c) * M, interpolating between RPM bins
  3. noise:  angular white noise, shaped per cycle to the measured residual
             order spectrum (overlap-add, 2-cycle window), times the measured
             (weak) angular envelope
  4. render: resample the angular signal back onto the 48 kHz time grid

Outputs in out/:
  ab_ref.wav / ab_syn.wav   the reference passage and the synth on the *same*
                            RPM trajectory -- the A/B pair to listen to
  demo_sweep.wav            3400 -> 6800 -> 3400 rpm, plus held revs
  ab_compare.png            spectrogram of both, same scale
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
from wavio import read_wav, write_wav, source_dir  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
STEM = "astm_v8_vantage_all"
AB_T0, AB_T1 = 94.42, 100.54      # the reference segment the table was built on

RNG = np.random.default_rng(20260817)
JITTER = 0.004                    # per-cycle period jitter, fraction of a cycle


class EngineTables(object):
    def __init__(self, path=None):
        d = np.load(path or os.path.join(OUT, "tables_v1.npz"))
        self.rpm = d["rpm"]
        self.wave = d["wave"]
        self.gain = d["gain"]
        self.nrms = d["noise_rms"]
        self.nspec = d["noise_spec"]
        self.nenv = d["noise_env"]
        self.M = int(d["M"])
        self.fs = float(d["fs"])

    def interp_at(self, rpm):
        """Blend the two neighbouring RPM bins. Tables are phase-aligned, so a
        plain crossfade is legitimate."""
        r = np.clip(rpm, self.rpm[0], self.rpm[-1])
        j = int(np.searchsorted(self.rpm, r, side="right") - 1)
        j = min(max(j, 0), len(self.rpm) - 2)
        a = (r - self.rpm[j]) / (self.rpm[j + 1] - self.rpm[j])
        mix = lambda A: (1 - a) * A[j] + a * A[j + 1]  # noqa: E731
        return (mix(self.wave), mix(self.nspec), mix(self.nenv),
                (1 - a) * self.gain[j] + a * self.gain[j + 1],
                (1 - a) * self.nrms[j] + a * self.nrms[j + 1])


def synth(rpm_t, t_grid, tab, fs, noise_scale=1.0, jitter=JITTER):
    """Render the model for an RPM trajectory. Returns a time-domain signal."""
    M = tab.M
    tt = np.arange(0.0, t_grid[-1], 1.0 / fs)
    fc = np.interp(tt, t_grid, rpm_t) / 120.0
    c = np.cumsum(fc) / fs                       # engine cycles elapsed
    ncyc = int(np.floor(c[-1]))
    if ncyc < 4:
        raise SystemExit("trajectory too short")

    # per-cycle jitter: perturb the cycle boundaries slightly
    if jitter > 0:
        edges = np.arange(ncyc + 1, dtype=float)
        edges[1:-1] += RNG.normal(0.0, jitter, ncyc - 1)
        edges = np.maximum.accumulate(edges)
        c = np.interp(c, edges, np.arange(ncyc + 1, dtype=float))

    rpm_cyc = np.interp(np.arange(ncyc) + 0.5, c, np.interp(tt, t_grid, rpm_t))

    # ---- build the angular-domain signal, cycle by cycle ----
    ang = np.zeros(ncyc * M + 2 * M)
    win = np.hanning(2 * M)
    for k in range(ncyc):
        w, nsp, nenv, g, nr = tab.interp_at(rpm_cyc[k])
        ang[k * M:(k + 1) * M] += g * w
        # noise: random phase on the measured residual magnitude, 2-cycle
        # window with 50 % overlap so cycles join without a seam
        # nsp is an M-point (1-cycle) spectrum: its bin j is crank order j/2.
        # This block is a 2M-point IFFT spanning 2 cycles, so *its* bin k is
        # crank order k/4.  Getting that factor wrong stretches the noise
        # spectrum by an octave -- too much rumble-band hiss, too little at
        # order 16-20.
        mag = np.interp(np.arange(M + 1) / 4.0, np.arange(len(nsp)) / 2.0, nsp)
        ph = RNG.uniform(-np.pi, np.pi, M + 1)
        blk = np.fft.irfft(mag * np.exp(1j * ph), 2 * M) * (2 * M) / 2.0
        # the angular envelope repeats once per cycle, i.e. twice in this block
        env = np.interp((np.arange(2 * M) % M) / M * len(nenv),
                        np.arange(len(nenv)), nenv, period=len(nenv))
        env /= env.mean() + 1e-15
        blk *= env * win
        blk *= noise_scale * g * nr / (blk.std() + 1e-15) * np.sqrt(0.5)
        ang[k * M:k * M + 2 * M] += blk
    ang = ang[:ncyc * M]

    # ---- angular -> time ----
    pos = c * M
    keep = pos < (ncyc * M - 2)
    y = np.interp(pos[keep], np.arange(len(ang)), ang)
    return y, tt[keep], np.interp(tt[keep], t_grid, rpm_t)


def main():
    tab = EngineTables()
    fs = tab.fs
    print("tables: %d RPM bins %.0f..%.0f rpm, M=%d"
          % (len(tab.rpm), tab.rpm[0], tab.rpm[-1], tab.M))

    # ---------------- A/B on the reference trajectory ----------------
    tr = np.load(os.path.join(OUT, "track_%s.npz" % STEM))
    sel = (tr["t"] >= AB_T0) & (tr["t"] <= AB_T1)
    t_seg = tr["t"][sel] - AB_T0
    rpm_seg = tr["rpm"][sel]
    x, _ = read_wav(os.path.join(WAVDIR, STEM + ".wav"))
    ref = x.mean(axis=1)[int(AB_T0 * fs):int(AB_T1 * fs)]
    ref -= ref.mean()

    y, ty, rpm_y = synth(rpm_seg, t_seg, tab, fs)
    n = min(len(ref), len(y))
    ref, y = ref[:n], y[:n]
    print("A/B: %.2f s, RPM %.0f..%.0f" % (n / fs, rpm_seg.min(), rpm_seg.max()))
    print("  reference RMS %.1f dBFS / synth RMS %.1f dBFS"
          % (20 * np.log10(ref.std()), 20 * np.log10(y.std())))
    y *= ref.std() / y.std()          # match level for a fair listen
    pk = max(np.abs(ref).max(), np.abs(y).max())
    write_wav(os.path.join(OUT, "ab_ref.wav"), ref / pk * 0.9, fs, 3)
    write_wav(os.path.join(OUT, "ab_syn.wav"), y / pk * 0.9, fs, 3)

    # ---- objective check: order content of both, same analysis ----
    print("\n  order content, reference vs synth (dB, mean over the passage)")
    print("   order    ref    syn   diff")
    for o in [1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0, 8.0, 10.0,
              12.0, 14.0, 16.0, 20.0]:
        vals = []
        for sig in (ref, y):
            f, t, S = signal.spectrogram(sig, fs, nperseg=8192,
                                         noverlap=8192 - 1024, mode="magnitude")
            rp = np.interp(t, t_seg, rpm_seg)
            amp = []
            for i in range(len(t)):
                ft = o * rp[i] / 60.0
                bi = int(round(ft / (f[1] - f[0])))
                amp.append(S[max(bi - 1, 0):bi + 2, i].max())
            vals.append(20 * np.log10(np.mean(amp) + 1e-12))
        print("   %5.1f  %6.1f %6.1f  %+5.1f" % (o, vals[0], vals[1], vals[1] - vals[0]))

    fig, axes = plt.subplots(2, 1, figsize=(14, 8), constrained_layout=True)
    for ax, sig, ttl in zip(axes, (ref, y), ("reference", "resynthesis")):
        f, t, S = signal.spectrogram(sig, fs, nperseg=16384, noverlap=16384 - 2048,
                                     mode="magnitude")
        k = f <= 2500
        db = 20 * np.log10(S[k] + 1e-10)
        ax.pcolormesh(t, f[k], db, vmin=db.max() - 65, vmax=db.max(),
                      shading="nearest", cmap="magma")
        ax.set_title("%s  (%.0f-%.0f rpm)" % (ttl, rpm_seg.min(), rpm_seg.max()))
        ax.set_ylabel("Hz")
    axes[-1].set_xlabel("time [s]")
    fig.savefig(os.path.join(OUT, "ab_compare.png"), dpi=85)
    plt.close(fig)

    # ---------------- demo: sweep + held revs ----------------
    keys = [(0.0, 3400), (1.0, 3400), (5.0, 6800), (6.0, 6800),
            (9.0, 3600), (10.5, 3600), (12.0, 5000), (14.0, 5000),
            (17.0, 3400), (18.0, 3400)]
    tk = np.array([k[0] for k in keys])
    rk = np.array([float(k[1]) for k in keys])
    tg = np.arange(0.0, tk[-1], 0.01)
    rg = np.interp(tg, tk, rk)
    d, _, _ = synth(rg, tg, tab, fs)
    d *= 0.9 / (np.abs(d).max() + 1e-12)
    write_wav(os.path.join(OUT, "demo_sweep.wav"), d, fs, 3)

    print("\nwrote out/ab_ref.wav, out/ab_syn.wav, out/demo_sweep.wav,"
          " out/ab_compare.png")


if __name__ == "__main__":
    main()
