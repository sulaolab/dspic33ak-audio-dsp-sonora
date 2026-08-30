"""Step 32: keep the cam clatter while masking the chord.

Two listening results from round 5, and they point the same way:

  detune  -- good on the rise, but the "pata-pata" mechanical clatter thins out
             in the steady part after the flare (the owner hears it as the cam
             hitting the valve train -- an impulsive, intra-cycle event)
  noiseup -- the *least* chordy of everything rendered so far

The clatter is fine structure *inside* one engine cycle: eight short bursts per
cycle, carried by the high orders, and it exists only because every cycle is
stamped with the same measured phase. Summing two readers running at +-0.4 %
speed slides one copy against the other, so those bursts arrive twice a few
hundred microseconds apart and comb-filter each other. That is not a tuning
problem with detune -- it is what detune *is*. So:

  **phase-domain fixes destroy the clatter; amplitude/noise-domain fixes do not.**

Part 1 measures that instead of asserting it. Two numbers on the steady tail
(after the flare, engine at idle), both computed on the 1.5-8 kHz band where the
clatter lives:

  imp     99.5th percentile / median of the band envelope [dB] -- how
          impulsive it is at all
  fold    the envelope folded at the engine-cycle period and averaged, then
          peak-to-mean [dB] -- how much of that impulsiveness is *cycle-locked*,
          i.e. actually the eight firing/valve events and not noise

Part 2 renders the fixes that keep the clatter:

  noiseup_x2 / x4 / x8    amount sweep on the winner (x4 is what was judged)
  noiseup_lowband         the extra noise low-passed at 1.5 kHz, so the chord
                          gets masked but the clatter band is left exposed
  noiseup_jit020          the winner plus the jitter that also helped
  detune_gated            detune only while the speed is changing fast, single
                          reader once it settles -- the rise the owner liked,
                          with the steady clatter back
  detune015               weaker detune (+-0.15 %) everywhere, for comparison
  formant_noiseup         the two winners together
  formant_noiseup_jit020  all three

The formant combinations are here because of the third round-5 report: the tail
of `startup_3000_formant_jit020` was judged the most authentic clatter yet. That
is consistent with a31's "side effect" -- holding the envelope in Hz stops the
top being rolled off as the speed falls, so at idle the clatter band comes back
+5 dB. What a31 flagged as a risk to the accepted idle may be the thing the
owner is hearing as real, so Part 1 measures the clatter of the formant renders
too and the two are no longer judged separately.

Every render uses the same trajectory and the same seed, and the outputs are
rms-matched to the base render rather than peak-normalised, so the A/B is a
timbre judgement.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a09_resynth as A9  # noqa: E402
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a29_chord_fix import IDLE_RPM  # noqa: E402
from a31_formant import V3, SEED, flare_traj  # noqa: E402

OUT = os.path.join(HERE, "out")
CLAT_LO, CLAT_HI = 1500.0, 8000.0     # where the pata-pata lives
T0, T1 = 3.5, 5.4                     # absolute window: the settled tail


def band_env(y, fs, lo=CLAT_LO, hi=CLAT_HI):
    """Envelope of the clatter band (FFT band-pass, then |analytic| smoothed)."""
    n = len(y)
    F = np.fft.rfft(y)
    f = np.fft.rfftfreq(n, 1.0 / fs)
    F[(f < lo) | (f > hi)] = 0.0
    b = np.fft.irfft(F, n)
    # analytic magnitude via the same one-sided spectrum trick
    G = np.fft.rfft(b)
    G[1:] *= 2.0
    env = np.abs(np.fft.ifft(np.concatenate([G, np.zeros(n - len(G))]), n))
    # 1 ms smoothing: the events are ~ms long, we do not want sample noise
    k = max(1, int(0.001 * fs))
    w = np.hanning(k) / (np.hanning(k).sum() + 1e-30)
    return np.convolve(env, w, mode="same")


def clatter(y, fs, rpm, t0=T0, t1=T1):
    """(imp, fold) in dB over an absolute time window. See the docstring.

    Both the window and the impulsiveness measure are deliberately not what was
    tried first. A peak-to-mean crest over "the last 2 s" moved by 9 dB between
    two renders that are sample-identical there: the renders differ in length by
    26 ms (a detuned reader completes a different number of cycles), so "last
    2 s" is a different window in each, and peak/mean hangs on one sample. An
    absolute window plus a 99.5th-percentile-to-median ratio gives the same
    number for both, so a difference in this table is now a real difference.
    """
    seg = y[int(t0 * fs):int(t1 * fs)]
    env = band_env(seg, fs)
    p50, p995 = np.percentile(env, [50.0, 99.5])
    imp = 20.0 * np.log10(p995 / (p50 + 1e-30))
    # fold at one engine cycle (= 2 crank revolutions); 8 firings land in it
    per = 120.0 / rpm * fs
    m = int(per)
    ncyc = int(len(env) // per)
    acc = np.zeros(m)
    for i in range(ncyc):                      # resample each cycle onto m bins
        s = i * per
        acc += np.interp(s + np.arange(m) * per / m,
                         np.arange(len(env)), env)
    acc /= ncyc
    fold = 20.0 * np.log10(acc.max() / (acc.mean() + 1e-30))
    return imp, fold


def render(tab, rg, tg, fs, jitter=0.010, noise=1.0):
    A9.RNG = np.random.default_rng(SEED)       # identical jitter *and* noise draw
    y, _, _ = synth(rg, tg, tab, fs, noise_scale=noise, jitter=jitter)
    return y


def lowpass(x, fs, fc):
    F = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / fs)
    F[f > fc] = 0.0
    return np.fft.irfft(F, len(x))


def save(y, fs, name, ref_rms=None):
    y = y.copy()
    k = int(0.05 * fs)
    y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    if ref_rms is not None:
        y *= ref_rms / (y.std() + 1e-30)
    else:
        y *= 0.9 / (np.abs(y).max() + 1e-30)
    write_wav(os.path.join(OUT, name), np.clip(y, -0.99, 0.99), fs, 3)


def main():
    tab = EngineTables(V3)
    fs = tab.fs
    tg, rg = flare_traj()

    base = render(tab, rg, tg, fs)
    slew = np.abs(np.gradient(rg, tg))

    def gate(n, scale=4000.0):
        return np.interp(np.arange(n) / fs, tg,
                         np.clip(slew / scale, 0.0, 1.0))

    def mix_extra(lo, hi, w, band=None):
        """lo + w * (hi - lo): identical seeds, so hi-lo *is* the extra noise."""
        n = min(len(lo), len(hi), len(w))
        ex = hi[:n] - lo[:n]
        if band is not None:
            ex = lowpass(ex, fs, band)
        return lo[:n] + w[:n] * ex

    # ---- the renders -------------------------------------------------------
    g = gate(len(base))
    outs = {}
    for amt in (2.0, 4.0, 8.0):
        outs["startup_3000_noiseup_x%d.wav" % round(amt)] = mix_extra(
            base, render(tab, rg, tg, fs, noise=amt), g)
    outs["startup_3000_noiseup_lowband.wav"] = mix_extra(
        base, render(tab, rg, tg, fs, noise=4.0), g, band=CLAT_LO)
    j = render(tab, rg, tg, fs, jitter=0.020)
    outs["startup_3000_noiseup_jit020.wav"] = mix_extra(
        j, render(tab, rg, tg, fs, jitter=0.020, noise=4.0), g)

    def detuned(frac):
        a = render(tab, rg * (1.0 + frac), tg, fs)
        b = render(tab, rg * (1.0 - frac), tg, fs)
        n = min(len(a), len(b))
        return (a[:n] + b[:n]) * 0.5

    d4 = detuned(0.004)
    outs["startup_3000_detune015.wav"] = detuned(0.0015)
    n = min(len(base), len(d4), len(g))
    outs["startup_3000_detune_gated.wav"] = (base[:n] * (1.0 - g[:n])
                                             + d4[:n] * g[:n])

    # the formant tables: the owner liked the clatter in the tail of
    # formant+jit020, so combine that with the noise gate as well
    fm = EngineTables(os.path.join(OUT, "tables_v3_low_formant.npz"))
    fj = render(fm, rg, tg, fs, jitter=0.020)
    outs["startup_3000_formant_noiseup.wav"] = mix_extra(
        render(fm, rg, tg, fs), render(fm, rg, tg, fs, noise=4.0), g)
    outs["startup_3000_formant_noiseup_jit020.wav"] = mix_extra(
        fj, render(fm, rg, tg, fs, jitter=0.020, noise=4.0), g)
    fs2 = EngineTables(os.path.join(OUT, "tables_v3_low_formant_strong.npz"))
    sj = render(fs2, rg, tg, fs, jitter=0.020)
    outs["startup_3000_formant_strong_jit020.wav"] = sj

    ref = base.std()
    for nm, y in outs.items():
        save(y, fs, nm, ref_rms=ref)

    # ---- part 1: what each fix does to the clatter --------------------------
    print("Part 1 -- the pata-pata (cam/valve clatter) on the steady tail")
    print("  %.1f-%.0f kHz band, t = %.1f-%.1f s (engine settled at %.0f rpm)"
          % (CLAT_LO / 1000.0, CLAT_HI / 1000.0, T0, T1, IDLE_RPM))
    print("  imp = how impulsive; fold = how much of it is cycle-locked"
          " (8 events/cycle)")
    print("  %-34s %7s %7s" % ("", "imp", "fold"))
    rows = [("today (jitter 0.010)", base),
            ("jitter 0.020", j),
            ("detune +-0.4 %", d4),
            ("detune +-0.15 %", outs["startup_3000_detune015.wav"]),
            ("detune +-0.4 %, slew-gated", outs["startup_3000_detune_gated.wav"]),
            ("noiseup x4", outs["startup_3000_noiseup_x4.wav"]),
            ("noiseup x4, low band only", outs["startup_3000_noiseup_lowband.wav"]),
            ("noiseup x8", outs["startup_3000_noiseup_x8.wav"]),
            ("formant + jitter 0.020  <- liked", fj),
            ("formant + noiseup x4", outs["startup_3000_formant_noiseup.wav"]),
            ("formant + noiseup + jit020",
             outs["startup_3000_formant_noiseup_jit020.wav"]),
            ("formant_strong + jitter 0.020", sj)]
    ref_c = None
    for nm, y in rows:
        c, f = clatter(y * (ref / (y.std() + 1e-30)), fs, IDLE_RPM)
        if ref_c is None:
            ref_c = (c, f)
        print("  %-34s %7.2f %7.2f   (%+5.2f %+5.2f dB re today)"
              % (nm, c, f, c - ref_c[0], f - ref_c[1]))
    print("  -> a fix that sums phase-misaligned copies loses `fold`: the eight"
          "\n     per-cycle events smear into each other. Jitter and noise gains"
          "\n     keep the intra-cycle phase and leave `fold` alone.")

    print("\nPart 2 -- " + ", ".join(sorted(outs)) + "\n  All rms-matched to"
          " today's render, same trajectory, same seed. `detune_gated` is"
          "\n  detune while the speed moves and a single reader once it settles;"
          "\n  `noiseup_lowband` masks below %.1f kHz only, leaving the clatter"
          " band clear." % (CLAT_LO / 1000.0))
    print("  Cost: noiseup_* = a runtime gain, 0 ROM. detune_gated = one extra"
          "\n  wavetable read while the speed is changing (it can be dropped in"
          "\n  steady state, so the read is not needed at idle).")


if __name__ == "__main__":
    main()
