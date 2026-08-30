"""Step 6: angular (crank-phase) resampling, cycle averaging, order phases.

This is the step that actually answers "where does the rumble come from".

Resample the recording onto a uniform *engine-cycle phase* grid, then:
  * average many cycles coherently  -> the deterministic per-cycle waveform,
    i.e. the 8 firing pulses and how much they differ from each other.
    Cylinder-to-cylinder differences inside one cycle ARE the half-order
    family, and their relative phase is what makes it read as rumble rather
    than as extra pitch.
  * FFT of the cycle-averaged waveform -> amplitude AND phase per 0.5 order.
  * per-cycle residual (cycle minus average) -> the incoherent noise part.

Bins are taken over RPM so the result is a table amplitude(order, rpm), which
is exactly what a synthesiser needs.
"""
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")

M = 1024                 # samples per engine cycle (=> orders up to 256)
T_START = 1.30           # skip the region where the track is not locked
RPM_BINS = [(3550, 3750), (3750, 4150), (4150, 4550), (4550, 4900), (4900, 5200)]
ORDERS_REPORT = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0,
                 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 10.0, 12.0, 14.0, 16.0, 20.0, 24.0]


def angular_resample(m, fs, t_frames, fc_frames):
    """Return (ang, cyc_index_of_each_sample, rpm_per_cycle)."""
    tt = np.arange(len(m)) / fs
    fc_s = np.interp(tt, t_frames, fc_frames)
    # cycle count c(t) = integral of f_cycle dt
    c = np.cumsum(fc_s) / fs
    c -= np.interp(T_START, tt, c)
    ncyc = int(np.floor(c[-1]))
    if ncyc < 8:
        raise SystemExit("not enough cycles")
    grid = np.arange(0, ncyc, 1.0 / M)          # in cycles
    ts = np.interp(grid, c, tt)                  # time of each phase point
    ang = np.interp(ts, tt, m)
    rpm_cyc = np.interp(np.arange(ncyc) + 0.5, c, fc_s) * 120.0
    return ang.reshape(ncyc, M), rpm_cyc


def main():
    stem = sys.argv[1] if len(sys.argv) > 1 else "astm_stable"
    tr = np.load(os.path.join(OUT, "track_%s.npz" % stem))
    x, fs = read_wav(os.path.join(WAVDIR, stem + ".wav"))
    m = x.mean(axis=1)
    m -= m.mean()

    cyc, rpm_cyc = angular_resample(m, fs, tr["t"], tr["fc"])
    print("%s: %d engine cycles on a %d-point phase grid, RPM %.0f..%.0f"
          % (stem, cyc.shape[0], M, rpm_cyc.min(), rpm_cyc.max()))

    result = {}
    fig, axes = plt.subplots(len(RPM_BINS), 2, figsize=(15, 3.0 * len(RPM_BINS)),
                             constrained_layout=True)
    for bi, (lo, hi) in enumerate(RPM_BINS):
        sel = (rpm_cyc >= lo) & (rpm_cyc < hi)
        if sel.sum() < 6:
            print("  bin %d-%d rpm: only %d cycles, skipped" % (lo, hi, sel.sum()))
            continue
        blk = cyc[sel]
        # level-normalise each cycle so a rising level does not blur the average
        rms = blk.std(axis=1, keepdims=True) + 1e-12
        blkn = blk / rms
        avg = blkn.mean(axis=0)               # coherent, cycle-locked waveform
        res = blkn - avg                      # incoherent residual
        coh = avg.std() ** 2
        inc = res.std() ** 2
        tonal_frac = coh / (coh + inc)

        # order spectrum of the coherent part (bin k <-> crank order k/2)
        Aw = np.fft.rfft(avg) / (M / 2.0)
        Rw = np.abs(np.fft.rfft(res, axis=1)).mean(axis=0) / (M / 2.0)

        print("\n--- RPM %d..%d  (%d cycles) ---" % (lo, hi, sel.sum()))
        print("  cycle-locked share of energy: %.1f %%   (noise-like %.1f %%)"
              % (100 * tonal_frac, 100 * (1 - tonal_frac)))
        print("  order   coh[dB]  phase[deg]  noise[dB]  coh-noise[dB]")
        tab = {}
        for o in ORDERS_REPORT:
            k = int(round(o * 2))
            a = abs(Aw[k])
            ph = np.degrees(np.angle(Aw[k]))
            nz = Rw[k]
            adb = 20 * np.log10(a + 1e-12)
            ndb = 20 * np.log10(nz + 1e-12)
            tab["%.1f" % o] = dict(coh_db=round(float(adb), 2),
                                   phase_deg=round(float(ph), 1),
                                   noise_db=round(float(ndb), 2))
            print("  %5.1f   %7.2f   %8.1f   %8.2f   %8.2f"
                  % (o, adb, ph, ndb, adb - ndb))
        result["%d-%d" % (lo, hi)] = dict(n_cycles=int(sel.sum()),
                                          tonal_frac=round(float(tonal_frac), 4),
                                          orders=tab)

        ax = axes[bi, 0]
        ph_ax = np.arange(M) / M * 720.0     # crank degrees, 2 revs per cycle
        ax.plot(ph_ax, avg, lw=0.9)
        for d in range(0, 720, 90):
            ax.axvline(d, color="r", lw=0.5, alpha=0.4)
        ax.set_title("%d-%d rpm: cycle-averaged waveform (red = 90 deg firing marks)"
                     % (lo, hi))
        ax.set_xlim(0, 720)
        ax.set_xlabel("crank angle [deg]")

        ax = axes[bi, 1]
        oax = np.arange(len(Aw)) / 2.0
        ax.plot(oax, 20 * np.log10(np.abs(Aw) + 1e-12), lw=0.8, label="cycle-locked")
        ax.plot(oax, 20 * np.log10(Rw + 1e-12), lw=0.8, alpha=0.7, label="residual noise")
        ax.set_xlim(0, 32)
        ax.set_ylim(-60, 10)
        ax.set_xticks(np.arange(0, 33, 2))
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        ax.set_title("order spectrum")
        ax.set_xlabel("crank order")

    out = os.path.join(OUT, "angular_%s.png" % stem)
    fig.savefig(out, dpi=80)
    plt.close(fig)
    print("\nwrote", out)
    with open(os.path.join(OUT, "orders_%s.json" % stem), "w") as fp:
        json.dump(result, fp, indent=1)
    np.savez(os.path.join(OUT, "angular_%s.npz" % stem), cyc=cyc, rpm=rpm_cyc)


if __name__ == "__main__":
    main()
