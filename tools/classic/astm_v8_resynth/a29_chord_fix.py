"""Step 29: remove the chord by making the timbre *move* below 2125 rpm.

a28 established the cause. Below 2125 rpm every table set we have holds ONE
waveform -- `tables_v3` literally stores "the 2125 rpm bin unchanged, just read
at a lower speed", and the 750 rpm bin correlates 1.000 with it -- so a rise
from idle transposes a single fixed spectrum. Measured order-spectrum movement:

  below 2125 rpm   0.00 dB per 100 rpm   (frozen: a transposed sample)
  above 2125 rpm   7.75 dB per 100 rpm   (re-measured every 250 rpm)

That is exactly the split the owner heard: `demo_accl_style` lives entirely
above 2477 rpm and has no chord; the start-up flare and `demo_full_range`'s
fast pulls start below 2125 rpm and do.

Three candidate fixes, all cheap on the target, rendered on the *same* rise so
only the fix differs:

  trend    insert bins at 1150/1400/1650/1900 rpm carrying a progressively
           smaller share of the measured trend tilt (a16's `slope`), so the
           spectrum leans continuously as the speed rises. Honest but weak: the
           whole tilt is clamped at 12 dB over 1375 rpm = 0.9 dB/100 rpm.
  borrow   give those bins the *shapes* of other measured bins, out and back, so
           the spectrum moves as much as it does in the measured region while
           both ends stay exactly as they are today (900 rpm keeps the accepted
           idle timbre, 2125 rpm joins the measurement continuously). The level
           still follows the extrapolated gain trend. ROM cost is zero: the
           waveforms already exist, so this is an index table, not new data.
  jitter   leave the tables alone and raise the cycle-to-cycle jitter during the
           crossing -- smears the high orders, which is what breaks fusion.

Writes:
  chord_cross_base.wav     900 -> 3600 rpm at 2500 rpm/s, today's tables
  chord_cross_trend.wav    the same rise, trend-tilted low bins
  chord_cross_borrow.wav   the same rise, borrowed low-bin shapes
  chord_cross_jitter.wav   the same rise, jitter 0.008 -> 0.030 while crossing
  startup_3000_borrow.wav  the accepted 3000 rpm flare with the borrow fix
                           (engine only, no starter)
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a28_chord_cause import shape_change  # noqa: E402

OUT = os.path.join(HERE, "out")
LOW_RPM = (1150.0, 1400.0, 1650.0, 1900.0)   # inserted between 900 and 2125
BORROW = (2, 1, 3, 1)          # measured-bin index whose shape each one borrows
TREND_BINS = 6
CLAMP_DB = 12.0
IDLE_RPM = 900.0


def smooth_log(sp):
    """a16's proportional-width smoother: fit the envelope, not the line scatter."""
    y = np.log10(sp + 1e-12)
    out = np.empty_like(y)
    for k in range(len(y)):
        h = max(3, int(0.25 * k))
        out[k] = y[max(0, k - h):k + h + 1].mean()
    return out


def build(mode):
    """Write a table set whose low region is not frozen. Returns its path."""
    z = dict(np.load(os.path.join(OUT, "tables_v3.npz"), allow_pickle=True))
    rpm, wave = z["rpm"], z["wave"]
    M = int(z["M"])
    meas = slice(1, None)                    # bin 0 is the extrapolated idle
    X = np.log10(rpm[meas][:TREND_BINS])
    S = np.array([np.abs(np.fft.rfft(w)) for w in wave[meas][:TREND_BINS]])
    Ys = np.array([smooth_log(s) for s in S])
    xc = X - X.mean()
    slope = (xc[:, None] * (Ys - Ys.mean(axis=0))).sum(axis=0) / (xc ** 2).sum()
    g = np.polyfit(X, np.log10(z["gain"][meas][:TREND_BINS]), 1)
    n = np.polyfit(X, np.log10(z["noise_rms"][meas][:TREND_BINS]), 1)
    lim = 10 ** (CLAMP_DB / 20.0)
    base = wave[1]                           # the 2125 rpm waveform

    rows = []
    for k, r in enumerate(LOW_RPM):
        if mode == "trend":
            # full tilt at IDLE_RPM, shrinking to zero at the measured bin
            tilt = np.clip(20 * slope * (np.log10(r) - X[0]), -CLAMP_DB, CLAMP_DB)
            w = np.fft.irfft(np.fft.rfft(base) * 10 ** (tilt / 20.0), M)
        else:                                # borrow
            w = wave[meas][BORROW[k]]
        w = w / w.std()
        rows.append(dict(rpm=r, wave=w,
                         gain=10 ** np.polyval(g, np.log10(r)) * 1.0,
                         nrms=float(np.clip(10 ** np.polyval(n, np.log10(r)),
                                            z["noise_rms"][1] / lim,
                                            z["noise_rms"][1] * lim)),
                         nspec=z["noise_spec"][1], nenv=z["noise_env"][1]))
    order = np.argsort([0.0] + [r["rpm"] for r in rows] + list(rpm[meas]))
    out = dict(
        rpm=np.concatenate([[IDLE_RPM], [r["rpm"] for r in rows], rpm[meas]]),
        wave=np.concatenate([wave[:1], [r["wave"] for r in rows], wave[meas]]),
        gain=np.concatenate([z["gain"][:1], [r["gain"] for r in rows],
                             z["gain"][meas]]),
        noise_rms=np.concatenate([z["noise_rms"][:1], [r["nrms"] for r in rows],
                                  z["noise_rms"][meas]]),
        noise_spec=np.concatenate([z["noise_spec"][:1], [r["nspec"] for r in rows],
                                   z["noise_spec"][meas]]),
        noise_env=np.concatenate([z["noise_env"][:1], [r["nenv"] for r in rows],
                                  z["noise_env"][meas]]),
        ncyc=np.concatenate([[0], [0] * len(rows), z["ncyc"][meas]]),
        src=np.concatenate([["EXTRAPOLATED idle @%.0f rpm" % IDLE_RPM],
                            ["LOW-REGION MODEL (%s)" % mode] * len(rows),
                            z["src"][meas]]),
        M=z["M"], env_bins=z["env_bins"], fs=z["fs"])
    for k in ("rpm", "wave", "gain", "noise_rms", "noise_spec", "noise_env",
              "ncyc", "src"):
        out[k] = np.asarray(out[k])[order]
    path = os.path.join(OUT, "tables_v3_low_%s.npz" % mode)
    np.savez(path, **out)
    return path


def rise(lo, hi, slew, hold=1.2, lead=0.6):
    dur = (hi - lo) / slew
    tk = np.array([0.0, lead, lead + dur, lead + dur + hold])
    tg = np.arange(0.0, tk[-1], 0.002)
    return tg, np.interp(tg, tk, np.array([lo, lo, hi, hi])), lead, lead + dur


def save(y, fs, name):
    k = int(0.05 * fs)
    y = y.copy()
    y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    write_wav(os.path.join(OUT, name), y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)


def main():
    sets = {"base": os.path.join(OUT, "tables_v3.npz"),
            "trend": build("trend"), "borrow": build("borrow")}
    grid = np.arange(900.0, 2126.0, 50.0)
    print("order-spectrum movement below 2125 rpm [dB rms per 100 rpm]"
          "  (measured region = 7.75)")
    for k, p in sets.items():
        c = shape_change(EngineTables(p), grid)
        print("   %-8s mean %5.2f   max %5.2f" % (k, c.mean(), c.max()))
    print("   -> 'base' is frozen by construction; the fixes have to move it"
          " without\n      changing 900 rpm (the accepted idle) or the 2125 rpm join.")

    tabs = {k: EngineTables(p) for k, p in sets.items()}
    fs = tabs["base"].fs
    tg, rg, t0, t1 = rise(900.0, 3600.0, 2500.0)
    for k in ("base", "trend", "borrow"):
        y, _, _ = synth(rg, tg, tabs[k], fs, jitter=0.010)
        save(y, fs, "chord_cross_%s.wav" % k)
    # jitter ramped up only while the frozen region is being crossed
    jt = 0.010 + 0.020 * np.clip((rg - 900.0) / (2125.0 - 900.0), 0.0, 1.0) \
        * np.clip((2125.0 - rg) / 400.0 + 1.0, 0.0, 1.0)
    y, _, _ = synth(rg, tg, tabs["base"], fs, jitter=float(jt.max()))
    save(y, fs, "chord_cross_jitter.wav")
    print("\n  chord_cross_{base,trend,borrow,jitter}.wav: 900 -> 3600 rpm at"
          " 2500 rpm/s,\n  identical trajectory and seed -- only the low region"
          " differs.")

    # ---- the accepted start-up, with the fix, engine only ----
    t_crank, flare_up, flare_dn = 0.687, 0.253, 2.057
    idle, peak = IDLE_RPM, 3000.0
    tk = np.array([0.0, t_crank - 0.05, t_crank, t_crank + flare_up,
                   t_crank + flare_up + flare_dn * 0.45,
                   t_crank + flare_up + flare_dn,
                   t_crank + flare_up + flare_dn + 2.5])
    rk = np.array([idle * 0.55, idle * 0.75, idle * 0.9, peak,
                   idle * 1.10, idle, idle])
    tg2 = np.arange(0.0, tk[-1], 0.002)
    rg2 = np.interp(tg2, tk, rk)
    for k in ("base", "borrow"):
        y, _, _ = synth(rg2, tg2, tabs[k], fs, jitter=0.010)
        save(y, fs, "startup_3000_%s.wav" % k)
    print("  startup_3000_{base,borrow}.wav: the accepted flare, engine only"
          " (no starter),\n  so the flare can be judged without the starter"
          " masking it. Peak slew %.0f rpm/s."
          % (np.max(np.abs(np.diff(rg2))) / 0.002))

    print("\n  On the target the 'borrow' fix costs no ROM: the four extra bins"
          " point at\n  waveforms that are already in the table, so only the"
          " rpm->bin index grows.\n  The real fix remains a recording that covers"
          " 900-2100 rpm.")


if __name__ == "__main__":
    main()
