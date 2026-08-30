"""Step 27: why a fast rise sounds like a *chord*, and how to remove it.

The owner's observation, and it is a sharp one:

  startup_3000rpm.wav   the flare has a strong "chord" character
  demo_full_range.wav   the same on its rapid rises from low rpm
  demo_accl_style.wav   none at all -- and it is the best of the three for
                        violent RPM changes

Those three come from the *same tables* and the *same synthesiser*. The only
thing that differs is where the RPM trajectory came from:

  demo_accl_style   the measured ridge track of astm_accl_001 (a real car)
  the other two     hand-written key frames, linearly interpolated

A perfectly linear ramp moves every order at exactly proportional rates, so the
whole half-order stack stays in exact frequency ratio for the entire rise, with
no relative modulation whatsoever. That is the definition of a chord glissando.
A real engine's speed is never that clean: combustion-to-combustion torque
variation, driveline compliance and load make it wander around its trend, and
that wander frequency-modulates every order by a different absolute amount,
which is what breaks the fused chord percept.

So: measure the wander in the real trajectory, put the same amount back into the
synthetic ones, and check with a metric rather than by ear alone. Harmonicity
(a18: how far the half-order grid stands above the midpoints between its teeth)
is the right metric here -- a fused chord means the teeth stand very proud.

Writes:
  rise_base.wav        linear ramp 1200 -> 5200 rpm in 1.6 s (today's behaviour)
  rise_fluct.wav       + speed wander matched to the measured trajectory
  rise_jitter.wav      + extra cycle-to-cycle jitter instead of wander
  rise_fluct_ease.wav  wander + an eased (S-curve) ramp
  startup_3000rpm_fluct.wav   the accepted start-up with the wander applied
"""
import os
import sys

import numpy as np
from scipy import signal, ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a17_demos import clean_traj  # noqa: E402
from a18_noise_calib import harmonicity, BANDS  # noqa: E402
from a22_idle_normal_model import measured_only  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
ACCL = "astm_accl_001"
TREND_S = 0.30            # anything slower than this is "the driver", not wander
BAND = (0.5, 20.0)        # wander band [Hz]
RNG = np.random.default_rng(80808)


def wander_stats(t, rpm):
    dt = t[1] - t[0]
    trend = ndimage.gaussian_filter1d(rpm, TREND_S / dt, mode="nearest")
    res = rpm - trend
    pct = 100.0 * res.std() / trend.mean()
    f, P = signal.welch(res, 1.0 / dt, nperseg=min(len(res), 512))
    tot = P.sum() + 1e-30
    lo = P[(f >= 0.5) & (f < 5.0)].sum() / tot
    hi = P[(f >= 5.0) & (f <= 20.0)].sum() / tot
    slew = np.max(np.abs(np.diff(trend))) / dt
    return dict(pct=pct, lo=lo, hi=hi, slew=slew, rms=res.std())


def make_wander(t, pct, rpm_ref):
    """Band-limited random speed wander, pct % rms of the reference speed."""
    dt = t[1] - t[0]
    w = RNG.normal(0.0, 1.0, len(t))
    b, a = signal.butter(2, [BAND[0] / (0.5 / dt), min(BAND[1] / (0.5 / dt), 0.99)],
                         "band")
    w = signal.filtfilt(b, a, w)
    return w / (w.std() + 1e-12) * (pct / 100.0) * rpm_ref


def rise_traj(t, lo, hi, t0, t1, ease=False):
    u = np.clip((t - t0) / (t1 - t0), 0.0, 1.0)
    if ease:
        u = u * u * (3.0 - 2.0 * u)        # smoothstep: no slew discontinuity
    return lo + (hi - lo) * u


def main():
    # ---- 1. what the real trajectory does that a key-frame ramp does not ----
    tr = np.load(os.path.join(OUT, "track_%s_ridge.npz" % ACCL))
    t_r, rpm_r = tr["t"], clean_traj(tr["t"], tr["rpm"])
    s = wander_stats(t_r, rpm_r)
    print("measured trajectory (%s, order-4 ridge, cleaned)" % ACCL)
    print("  speed wander about a %.2f s trend: %.2f %% rms (%.0f rpm)"
          % (TREND_S, s["pct"], s["rms"]))
    print("  of that wander, %.0f %% is 0.5-5 Hz and %.0f %% is 5-20 Hz"
          % (100 * s["lo"], 100 * s["hi"]))
    print("  peak slew of the trend: %.0f rpm/s" % s["slew"])
    print("  (upper bound: the tracker's own noise is in there too, so treat this"
          "\n   as 'at most this much', which is the safe direction for a demo)")

    tab = EngineTables(os.path.join(OUT, measured_only()))
    fs = tab.fs

    # ---- 2. the same rise, four ways ----
    dt = 0.002
    tg = np.arange(0.0, 3.6, dt)
    base = rise_traj(tg, 1200.0, 5200.0, 1.0, 2.6)
    ease = rise_traj(tg, 1200.0, 5200.0, 1.0, 2.6, ease=True)
    wan = make_wander(tg, s["pct"], 3000.0)
    print("\n  key-frame ramp wander before/after: %.2f %% -> %.2f %%"
          % (wander_stats(tg, base)["pct"], wander_stats(tg, base + wan)["pct"]))

    cases = (("rise_base.wav", base, 0.008),
             ("rise_fluct.wav", base + wan, 0.008),
             ("rise_jitter.wav", base, 0.030),
             ("rise_fluct_ease.wav", ease + wan, 0.008))
    print("\n  harmonicity over the rise (grid minus midpoint, dB -- higher = more"
          " fused/chordy)")
    print("   case                  " + "".join("%12s" % ("%d-%d Hz" % b) for b in BANDS))
    i0, i1 = int(1.0 * fs), int(2.6 * fs)
    for name, rg, jit in cases:
        y, _, _ = synth(rg, tg, tab, fs, jitter=jit)
        k = int(0.05 * fs)
        y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
        write_wav(os.path.join(OUT, name), y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)
        # the slice starts at t = 1.0 s, so shift the trajectory's clock to match
        h = harmonicity(y[i0:i1], fs, tg - 1.0, rg)
        print("   %-20s  " % name[:-4] + "".join("%12.2f" % v for v in h))

    # the two references the owner already judged, measured the same way
    x, _ = read_wav(os.path.join(WAVDIR, ACCL + ".wav"))
    ref = x.mean(axis=1)
    ref -= ref.mean()
    n = min(len(ref), int(t_r[-1] * fs))
    h = harmonicity(ref[:n], fs, t_r, rpm_r)
    print("   %-20s  " % "REAL recording" + "".join("%12.2f" % v for v in h))
    ysyn, _, _ = synth(rpm_r, t_r, tab, fs, jitter=0.004)
    h = harmonicity(ysyn, fs, t_r, rpm_r)
    print("   %-20s  " % "measured traj (=accl)" + "".join("%12.2f" % v for v in h))

    # ---- 3. apply it to the accepted start-up ----
    t_crank, flare_up, flare_dn = 0.687, 0.253, 2.057
    idle, peak = 900.0, 3000.0
    keys = [(0.0, idle * 0.55), (t_crank - 0.05, idle * 0.75), (t_crank, idle * 0.9),
            (t_crank + flare_up, peak),
            (t_crank + flare_up + flare_dn * 0.45, idle * 1.10),
            (t_crank + flare_up + flare_dn, idle),
            (t_crank + flare_up + flare_dn + 2.5, idle)]
    tk = np.array([k[0] for k in keys])
    rk = np.array([k[1] for k in keys])
    tg2 = np.arange(0.0, tk[-1], dt)
    rg2 = np.interp(tg2, tk, rk)
    rg2 = rg2 + make_wander(tg2, s["pct"], 1.0) * np.maximum(rg2, 600.0)
    y2, _, _ = synth(rg2, tg2, tab, fs, jitter=0.010)
    k = int(0.05 * fs)
    y2[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    write_wav(os.path.join(OUT, "rise_startup3000_fluct.wav"),
              y2 / (np.abs(y2).max() + 1e-9) * 0.9, fs, 3)
    print("\n  rise_startup3000_fluct.wav: the accepted 3000 rpm flare, engine only"
          " (no starter), with the same wander applied")
    print("\n  The wander is an RPM-input effect: on the target it is one"
          " band-limited\n  noise source added to the speed, no table or ROM change.")


if __name__ == "__main__":
    main()
