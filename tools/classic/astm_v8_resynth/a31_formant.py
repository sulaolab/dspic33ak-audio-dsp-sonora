"""Step 31: is the envelope fixed in Hz or fixed in order? (and four more fixes)

a28/a29 got the *location* right -- the chord lives below 2125 rpm, where the
model transposes one waveform -- but the fix that followed from "make the shape
move" (a29 `borrow`) does NOT remove it: `startup_3000_borrow` was judged still
chordy, while raising the jitter maybe helped. So shape *movement* is not the
cure, and the mechanism must be something else about transposition.

The obvious candidate: a real car has resonances that are fixed in **Hz** --
exhaust pipe modes, body and cabin cavities -- not fixed in engine order. Every
measured bin was recorded at its own speed, so those resonances sit at the same
Hz in each of them, and sweeping through the bins leaves them standing still
while the harmonics slide past. Transposing a single bin instead drags them down
with the pitch, which turns them into a handful of strong, widely spaced,
pitch-locked peaks -- a chord.

Part 1 tests that on the measurement itself, with no synthesis in the loop: for
every pair of measured bins, compare their smoothed log spectra over the Hz range
they share, aligned by Hz and aligned by order. Whichever alignment gives the
smaller disagreement is how this engine's spectrum is actually organised.

Part 2 builds low-rpm bins by formant-preserving transposition: the order
amplitudes at speed R are read off the 2125 rpm bin's *Hz* envelope at
k*R/240 Hz, so the envelope stays put and only the harmonics move.

Part 3 renders the accepted 3000 rpm flare (engine only, no starter) five more
ways, all cheap on the target:

  jit010/020/030/045  jitter sweep -- the direction the owner picked
  formant             formant-preserving low bins
  formant_jit020      both
  detune              two wavetable readers at +-0.4 % speed, summed (beating)
  noiseup             residual noise raised x4 while the speed is changing fast
                      (masking, and a runtime gain on the target)
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a09_resynth as A9  # noqa: E402
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402
from a28_chord_cause import shape_change  # noqa: E402
from a29_chord_fix import smooth_log, LOW_RPM, TREND_BINS, CLAMP_DB, IDLE_RPM  # noqa: E402

OUT = os.path.join(HERE, "out")
V3 = os.path.join(OUT, "tables_v3.npz")
K_LO, K_HI = 4, 120           # rfft bins: bin k = order k/2, so orders 2..60
SEED = 3101


def hz_of(k, rpm):
    """Frequency of rfft bin k at a given speed (bin k = order k/2)."""
    return np.asarray(k) * rpm / 240.0


def part1(z):
    rpm, wave = z["rpm"][1:], z["wave"][1:]        # measured bins only
    k = np.arange(K_LO, K_HI + 1)
    L = np.array([smooth_log(np.abs(np.fft.rfft(w)))[k] * 20.0 for w in wave])
    L = L - L.mean(axis=1, keepdims=True)          # level is a separate matter
    d_hz, d_ord, n = [], [], 0
    for i in range(len(rpm)):
        for j in range(i + 1, len(rpm)):
            f_i, f_j = hz_of(k, rpm[i]), hz_of(k, rpm[j])
            lo, hi = max(f_i[0], f_j[0]), min(f_i[-1], f_j[-1])
            if hi <= lo * 1.2:
                continue
            g = np.exp(np.linspace(np.log(lo), np.log(hi), 60))
            a = np.interp(g, f_i, L[i])
            # aligned by Hz: same frequency in both
            b_hz = np.interp(g, f_j, L[j])
            # aligned by order: same order in both, i.e. bin j read at the
            # frequency its own speed puts that order at
            b_or = np.interp(g * rpm[j] / rpm[i], f_j, L[j])
            for arr, dst in ((b_hz, d_hz), (b_or, d_ord)):
                r = (a - a.mean()) - (arr - arr.mean())
                dst.append(np.sqrt((r ** 2).mean()))
            n += 1
    d_hz, d_ord = np.array(d_hz), np.array(d_ord)
    print("Part 1 -- how is the spectral envelope organised? (%d bin pairs,"
          " orders %g-%g)" % (n, K_LO / 2, K_HI / 2))
    print("  disagreement between two measured bins, aligned by Hz    : %.2f dB rms"
          % d_hz.mean())
    print("  disagreement between two measured bins, aligned by order : %.2f dB rms"
          % d_ord.mean())
    better = "Hz" if d_hz.mean() < d_ord.mean() else "order"
    print("  -> the envelope is better described as fixed in **%s** (%.2f dB"
          " better,\n     %d of %d pairs agree)"
          % (better, abs(d_hz.mean() - d_ord.mean()),
             int((d_hz < d_ord).sum() if better == "Hz" else (d_ord < d_hz).sum()), n))
    print("     Transposing one bin holds the envelope in *order*, so a fixed-Hz"
          "\n     envelope gets dragged with the pitch -- the suspected chord.")
    return better


def drag(tab, rpm_grid):
    """How much the spectral envelope follows the pitch.

    d log(envelope centroid in Hz) / d log(rpm): 1.0 = dragged along exactly
    (pure transposition), 0.0 = nailed to fixed Hz. The measured region's own
    value is the number a model of the missing region should reproduce.
    """
    c = []
    for r in rpm_grid:
        w = tab.interp_at(r)[0]
        a = np.abs(np.fft.rfft(w))[K_LO:K_HI + 1]
        f = hz_of(np.arange(K_LO, K_HI + 1), r)
        c.append((a * f).sum() / (a.sum() + 1e-30))
    return float(np.polyfit(np.log(rpm_grid), np.log(c), 1)[0])


def scan_strength(z, grid, target):
    """Scan correction strength and clamp; report, do not silently pick.

    A bisection was tried first and was wrong to: drag is **not monotonic** in
    strength. Past about x1 the widened clamp distorts the envelope enough that
    the centroid starts moving back up, so there is no strength that reaches the
    car's own 0.57 by this route. The scan is printed so that limit is visible.
    """
    print("\n  scan: correction strength vs drag (target = the car's %.2f)" % target)
    best = None
    for clamp in (12.0, 18.0):
        row = []
        for s in (0.5, 1.0, 1.5, 2.0, 3.0):
            d = drag(EngineTables(build_formant(z, s, "formant_tmp", clamp)), grid)
            row.append(d)
            if best is None or abs(d - target) < abs(best[2] - target):
                best = (s, clamp, d)
        print("   clamp +-%2.0f dB  " % clamp
              + "".join("x%.1f %.2f  " % (s, d)
                        for s, d in zip((0.5, 1.0, 1.5, 2.0, 3.0), row)))
    print("   closest to the car: x%.1f at +-%.0f dB -> drag %.2f (not 0.57;"
          " this route\n   cannot get there, and pushing harder makes the idle"
          " side effect worse)" % best)
    return best


def build_formant(z, strength=1.0, tag="formant", clamp=CLAMP_DB):
    """Low bins whose envelope stays where the 2125 rpm bin put it, in Hz."""
    rpm, wave = z["rpm"], z["wave"]
    M = int(z["M"])
    base = wave[1]
    sp = np.fft.rfft(base)
    env = smooth_log(np.abs(sp)) * 20.0                 # dB vs bin index
    k = np.arange(len(sp))
    f_ref = hz_of(k, rpm[1])
    X = np.log10(rpm[1:1 + TREND_BINS])
    g = np.polyfit(X, np.log10(z["gain"][1:1 + TREND_BINS]), 1)
    n = np.polyfit(X, np.log10(z["noise_rms"][1:1 + TREND_BINS]), 1)
    lim = 10 ** (CLAMP_DB / 20.0)

    rows = []
    for r in (IDLE_RPM,) + LOW_RPM:
        want = np.interp(hz_of(k, r), f_ref, env, left=env[1], right=env[-1])
        adj = np.clip((want - env) * strength, -clamp, clamp)   # stated clamp
        adj[0] = 0.0
        w = np.fft.irfft(sp * 10 ** (adj / 20.0), M)     # phases untouched
        rows.append((r, w / w.std(),
                     10 ** np.polyval(g, np.log10(r)),
                     float(np.clip(10 ** np.polyval(n, np.log10(r)),
                                   z["noise_rms"][1] / lim,
                                   z["noise_rms"][1] * lim))))
    out = dict(
        rpm=np.array([r[0] for r in rows] + list(rpm[1:])),
        wave=np.array([r[1] for r in rows] + list(wave[1:])),
        gain=np.array([r[2] for r in rows] + list(z["gain"][1:])),
        noise_rms=np.array([r[3] for r in rows] + list(z["noise_rms"][1:])),
        noise_spec=np.array([z["noise_spec"][1]] * len(rows)
                            + list(z["noise_spec"][1:])),
        noise_env=np.array([z["noise_env"][1]] * len(rows)
                           + list(z["noise_env"][1:])),
        ncyc=np.array([0] * len(rows) + list(z["ncyc"][1:])),
        src=np.array(["LOW-REGION MODEL (formant %.2f)" % strength] * len(rows)
                     + list(z["src"][1:])),
        M=z["M"], env_bins=z["env_bins"], fs=z["fs"])
    path = os.path.join(OUT, "tables_v3_low_%s.npz" % tag)
    np.savez(path, **out)
    return path


def flare_traj(dt=0.002, peak=3000.0):
    t_crank, up, dn = 0.687, 0.253, 2.057
    tk = np.array([0.0, t_crank - 0.05, t_crank, t_crank + up,
                   t_crank + up + dn * 0.45, t_crank + up + dn,
                   t_crank + up + dn + 2.5])
    rk = np.array([IDLE_RPM * 0.55, IDLE_RPM * 0.75, IDLE_RPM * 0.9, peak,
                   IDLE_RPM * 1.10, IDLE_RPM, IDLE_RPM])
    tg = np.arange(0.0, tk[-1], dt)
    return tg, np.interp(tg, tk, rk)


def render(tab, rg, tg, fs, name, jitter=0.010, noise=1.0):
    A9.RNG = np.random.default_rng(SEED)      # identical jitter *and* noise draw
    y, _, _ = synth(rg, tg, tab, fs, noise_scale=noise, jitter=jitter)
    return y


def save(y, fs, name):
    k = int(0.05 * fs)
    y = y.copy()
    y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    write_wav(os.path.join(OUT, name), y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)


def main():
    z = dict(np.load(V3, allow_pickle=True))
    part1(z)

    base = EngineTables(V3)
    fs = base.fs
    grid = np.arange(900.0, 2126.0, 50.0)
    hi_grid = np.arange(2200.0, 6876.0, 50.0)
    d_meas = drag(base, hi_grid)
    tab = EngineTables(build_formant(z))
    print("\nPart 2 -- does the envelope get dragged along with the pitch?"
          "\n  (1.00 = pure transposition, 0.00 = nailed to fixed Hz)")
    print("   measured region, 2200-6875 rpm      %5.2f   <- what the car does"
          % d_meas)
    for nm, p in (("today, below 2125 rpm", V3),
                  ("a29 borrow", os.path.join(OUT, "tables_v3_low_borrow.npz")),
                  ("a29 trend", os.path.join(OUT, "tables_v3_low_trend.npz")),
                  ("formant, full (x1.0, +-12 dB)",
                   os.path.join(OUT, "tables_v3_low_formant.npz"))):
        t = EngineTables(p)
        print("   %-35s %5.2f" % (nm, drag(t, grid)))
    print("  -> this is why `borrow` did not help: swapping in another measured"
          "\n     *shape* leaves the drag at 0.96, because each borrowed shape is"
          "\n     still being transposed. Only re-weighting the envelope moves it.")
    s_best, c_best, d_best = scan_strength(z, grid, d_meas)
    strong = EngineTables(build_formant(z, s_best, "formant_strong", c_best))
    print("\n  order-spectrum movement below 2125 rpm [dB rms per 100 rpm]:"
          " today 0.00,"
          "\n  borrow 9.02, formant %.2f, strong %.2f (measured region 7.75)"
          % (shape_change(tab, grid).mean(), shape_change(strong, grid).mean()))
    ti = np.arange(400) * 0.002
    oc = {}
    for nm, t in (("today", base), ("formant", tab), ("strong", strong)):
        o, edges = octaves(render(t, np.full(400, IDLE_RPM), ti, fs, "x"), fs)
        oc[nm] = o - o[0]
    print("\n  side effect at the accepted idle -- octaves re 40 Hz [dB]")
    print("   band       " + "".join("%7d" % e for e in edges[:-1]))
    for nm in ("today", "formant", "strong"):
        print("   %-10s " % nm + "".join("%7.1f" % v for v in oc[nm]))
    print("  (the idle timbre moves with it -- judge idle_900_formant{,_strong}"
          ".wav\n   against idle_900_noise+0.wav before adopting either.)")
    part = strong

    tg, rg = flare_traj()
    for jit in (0.010, 0.020, 0.030, 0.045):
        save(render(base, rg, tg, fs, "x", jitter=jit), fs,
             "startup_3000_jit%03d.wav" % round(jit * 1000))
    save(render(tab, rg, tg, fs, "x"), fs, "startup_3000_formant.wav")
    save(render(part, rg, tg, fs, "x"), fs, "startup_3000_formant_strong.wav")
    save(render(tab, rg, tg, fs, "x", jitter=0.020), fs,
         "startup_3000_formant_jit020.wav")

    # two readers, +-0.4 % speed: every order beats at a rate proportional to it
    a = render(base, rg * 1.004, tg, fs, "x")
    b = render(base, rg * 0.996, tg, fs, "x")
    n = min(len(a), len(b))
    save((a[:n] + b[:n]) * 0.5, fs, "startup_3000_detune.wav")

    # noise raised x4 while the speed is changing fast; identical seeds mean the
    # difference between the two renders is exactly the extra noise, so this is a
    # clean time-varying noise gain and not a crossfade of two realisations
    lo = render(base, rg, tg, fs, "x")
    hi = render(base, rg, tg, fs, "x", noise=4.0)
    n = min(len(lo), len(hi))
    slew = np.abs(np.gradient(rg, tg))
    w = np.interp(np.arange(n) / fs, tg, np.clip(slew / 4000.0, 0.0, 1.0))
    save(lo[:n] + w * (hi[:n] - lo[:n]), fs, "startup_3000_noiseup.wav")

    # the idle with the formant tables, for the side-effect check. The owner
    # picked noise "+0", i.e. the measured bin's noise/coherent ratio, so scale
    # the extrapolated trend's noise back to that -- otherwise this A/B would
    # carry the noise decision as a second variable.
    zf = np.load(os.path.join(OUT, "tables_v3_low_formant.npz"), allow_pickle=True)
    ns = ((z["noise_rms"][1] / z["gain"][1])
          / (zf["noise_rms"][0] / zf["gain"][0]))
    tgi = np.arange(0.0, 8.0, 0.002)
    rng = np.random.default_rng(3009)
    d = np.cumsum(rng.normal(0.0, 1.0, len(tgi)))
    d -= d.mean()
    d *= 40.0 / (np.abs(d).max() + 1e-12)
    for nm, t in (("idle_900_formant.wav", tab),
                  ("idle_900_formant_strong.wav", part)):
        save(render(t, IDLE_RPM + d, tgi, fs, "x", jitter=0.018, noise=ns), fs, nm)
    print("\n  (idle_900_formant*.wav use noise x%.3f so their noise/coherent"
          " ratio\n   matches the '+0' the owner picked)" % ns)

    print("\nPart 3 -- startup_3000_{jit010,jit020,jit030,jit045,formant,"
          "formant_strong,\n  formant_jit020,detune,noiseup}.wav plus"
          " idle_900_formant{,_strong}.wav.\n  Engine only, no starter, same"
          " trajectory and seed throughout.")
    print("  detune costs one extra wavetable read; noiseup is a runtime gain;"
          "\n  formant is four extra 256-point tables (~2 kB) that cannot be"
          " aliased to\n  existing ones the way `borrow` could.")


if __name__ == "__main__":
    main()
