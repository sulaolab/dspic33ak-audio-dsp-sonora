"""Step 20: prove a portable synthesis structure before anyone writes C.

The prototype in a09 generates its noise by inverse-FFT-ing the measured residual
magnitude with random phase, once per engine cycle.  That is fine in Python and
impossible on the target: an FFT per engine cycle in the audio path is not a
sensible thing to ship.  So this step replaces it with something a dsPIC can do
and *measures whether the substitution is audible*, rather than assuming it.

Portable structure under test:
  * ROM: one pre-shaped noise table (shaped once, offline, to the measured
    average residual order spectrum), plus the per-bin cycle wavetables.
  * Runtime, per output sample: advance the phase; read two neighbouring RPM
    wavetables and crossfade; read the noise table at a rate proportional to
    f_cycle so the noise stays order-locked; crossfade the noise at each cycle
    boundary from a fresh random offset so it never repeats audibly; scale by the
    per-bin noise level.
  * No FFT, no per-bin noise tables, no filter design at runtime.

The question this step answers: how much does the single shared noise shape cost,
given the per-bin shapes are not identical?
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth as synth_fft  # noqa: E402
from a17_demos import clean_traj  # noqa: E402
from a18_noise_calib import harmonicity, BANDS  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")
ACCL = "astm_accl_001"
T_END = 8.0
NOISE_TABLE_CYCLES = 8           # length of the ROM noise table, in engine cycles
RNG = np.random.default_rng(4242)


def build_noise_table(tab, cycles=NOISE_TABLE_CYCLES,
                      rolloff_db_per_oct=0.0, corner_order=16.0, quiet=False):
    """Average the measured residual order spectra, then realise one random noise
    table with that shape.  Also report how far the per-bin shapes stray from the
    average -- that spread is the price of sharing one table."""
    sp = tab.nspec                                     # (nbin, M/2+1)
    ref = sp / (sp.sum(axis=1, keepdims=True) + 1e-15)  # normalise per bin
    # Average the *shapes* in the log domain.  A linear mean is dominated by
    # whichever bins have the most high-order content, which showed up as +7.3 dB
    # at 2.5 kHz against the FFT prototype.
    avg = 10 ** (np.log10(ref + 1e-15).mean(axis=0))
    db = 20 * np.log10(ref + 1e-15) - 20 * np.log10(avg + 1e-15)
    k = np.arange(sp.shape[1])
    band = (k >= 2) & (k <= 2 * 60)                     # orders 1..60
    spread = np.abs(db[:, band]).mean()
    worst = np.abs(db[:, band]).max()
    if not quiet:
        print("  per-bin noise shape vs the average, orders 1-60:"
              " mean |deviation| %.1f dB, worst %.1f dB" % (spread, worst))

    M = tab.M
    L = cycles * M
    mag = np.interp(np.arange(L // 2 + 1) / float(cycles), np.arange(len(avg)), avg)
    if rolloff_db_per_oct > 0:
        order = np.arange(L // 2 + 1) / (2.0 * cycles)
        ex = np.log2(np.maximum(order, corner_order) / corner_order)
        mag = mag * 10 ** (-rolloff_db_per_oct * ex / 20.0)
    ph = RNG.uniform(-np.pi, np.pi, L // 2 + 1)
    tbl = np.fft.irfft(mag * np.exp(1j * ph), L)
    tbl /= tbl.std()
    return tbl


def synth_portable(rpm_t, t_grid, tab, fs, noise_tbl, noise_scale=1.0, jitter=0.004):
    """The structure described in the module docstring."""
    M = tab.M
    L = len(noise_tbl)
    tt = np.arange(0.0, t_grid[-1], 1.0 / fs)
    fc = np.interp(tt, t_grid, rpm_t) / 120.0
    c = np.cumsum(fc) / fs
    ncyc = int(np.floor(c[-1]))
    if jitter > 0:
        edges = np.arange(ncyc + 1, dtype=float)
        edges[1:-1] += RNG.normal(0.0, jitter, ncyc - 1)
        edges = np.maximum.accumulate(edges)
        c = np.interp(c, edges, np.arange(ncyc + 1, dtype=float))
    rpm_cyc = np.interp(np.arange(ncyc) + 0.5, c, np.interp(tt, t_grid, rpm_t))

    ang = np.zeros(ncyc * M + 2 * M)
    win = np.hanning(2 * M)
    idx2 = np.arange(2 * M)
    for k in range(ncyc):
        w, _, nenv, g, nr = tab.interp_at(rpm_cyc[k])
        ang[k * M:(k + 1) * M] += g * w
        # noise: read 2 cycles from a random offset in the ROM table, windowed,
        # so consecutive blocks overlap-add into something non-repeating
        off = int(RNG.integers(0, L))
        blk = noise_tbl[(off + idx2) % L].copy()
        env = np.interp((idx2 % M) / M * len(nenv), np.arange(len(nenv)), nenv,
                        period=len(nenv))
        env /= env.mean() + 1e-15
        blk *= env * win
        blk *= noise_scale * g * nr / (blk.std() + 1e-15) * np.sqrt(0.5)
        ang[k * M:k * M + 2 * M] += blk
    ang = ang[:ncyc * M]
    pos = c * M
    keep = pos < (ncyc * M - 2)
    return np.interp(pos[keep], np.arange(len(ang)), ang), tt[keep]


def octaves(sig, fs):
    edges = np.array([40, 80, 160, 315, 630, 1250, 2500, 5000, 10000], float)
    F = np.abs(np.fft.rfft(sig * np.hanning(len(sig)))) ** 2
    f = np.fft.rfftfreq(len(sig), 1.0 / fs)
    return np.array([10 * np.log10(F[(f >= lo) & (f < hi)].sum() + 1e-20)
                     for lo, hi in zip(edges[:-1], edges[1:])]), edges


def main():
    tab = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    fs = tab.fs
    print("tables_v3: %d bins, %.0f..%.0f rpm, M=%d" %
          (len(tab.rpm), tab.rpm[0], tab.rpm[-1], tab.M))

    print("\n--- ROM noise table ---")
    tbl = build_noise_table(tab)
    print("  table = %d cycles x %d points = %d samples (%.1f kB as int16)"
          % (NOISE_TABLE_CYCLES, tab.M, len(tbl), len(tbl) * 2 / 1024.0))

    tr = np.load(os.path.join(OUT, "track_%s_ridge.npz" % ACCL))
    sel = tr["t"] <= T_END
    t = tr["t"][sel]
    rpm = clean_traj(tr["t"], tr["rpm"])[sel]
    x, _ = read_wav(os.path.join(WAVDIR, ACCL + ".wav"))
    ref = x.mean(axis=1)[:int(T_END * fs)]
    ref -= ref.mean()

    y_fft, _, _ = synth_fft(rpm, t, tab, fs, jitter=0.004)
    y_por, _ = synth_portable(rpm, t, tab, fs, tbl, jitter=0.004)
    n = min(len(ref), len(y_fft), len(y_por))
    ref, y_fft, y_por = ref[:n], y_fft[:n], y_por[:n]
    y_fft *= ref.std() / y_fft.std()
    y_por *= ref.std() / y_por.std()

    print("\n--- portable vs FFT prototype vs reference ---")
    h_ref = np.array(harmonicity(ref, fs, t, rpm))
    h_fft = np.array(harmonicity(y_fft, fs, t, rpm))
    h_por = np.array(harmonicity(y_por, fs, t, rpm))
    print("  harmonicity [dB]      " + "".join("%12s" % ("%d-%d Hz" % b) for b in BANDS))
    for name, h in (("reference", h_ref), ("FFT prototype", h_fft), ("portable", h_por)):
        print("   %-20s" % name + "".join("%11.2f" % v for v in h))
    print("   %-20s" % "portable - FFT" + "".join("%+11.2f" % v for v in (h_por - h_fft)))

    o_ref, edges = octaves(ref, fs)
    o_fft, _ = octaves(y_fft, fs)
    o_por, _ = octaves(y_por, fs)
    print("\n  octave-band levels [dB]  " + "".join("%8d" % e for e in edges[:-1]))
    for name, o in (("reference", o_ref), ("FFT prototype", o_fft), ("portable", o_por)):
        print("   %-22s" % name + "".join("%8.1f" % v for v in o))
    print("   %-22s" % "portable - FFT" + "".join("%+8.1f" % v for v in (o_por - o_fft)))
    print("   %-22s" % "portable - reference" + "".join("%+8.1f" % v for v in (o_por - o_ref)))
    print("\n  rms difference portable vs FFT: %.2f dB across octave bands,"
          " %.2f dB across harmonicity bands"
          % (np.sqrt(((o_por - o_fft) ** 2).mean()),
             np.sqrt(((h_por - h_fft) ** 2).mean())))

    # ---- fit a high-order rolloff on the shared noise shape ----
    # One shared shape costs 5.4 dB of mean per-bin deviation, and both the
    # prototype and the portable version sit above the reference at 2.5 kHz and
    # up.  A single first-order rolloff on the ROM table is the cheapest possible
    # correction, so fit its corner and slope to the reference octave bands.
    print("\n--- fitting a high-order rolloff on the ROM noise table ---")
    print("  corner  slope   octave-band rms err vs reference [dB]   2500 Hz  5000 Hz")
    best = None
    for corner in (8.0, 12.0, 16.0, 24.0):
        for slope in (0.0, 3.0, 6.0, 9.0, 12.0):
            tb = build_noise_table(tab, rolloff_db_per_oct=slope,
                                   corner_order=corner, quiet=True)
            yy, _ = synth_portable(rpm, t, tab, fs, tb, jitter=0.004)
            yy = yy[:n]
            yy *= ref.std() / yy.std()
            o, _ = octaves(yy, fs)
            err = np.sqrt(((o - o_ref) ** 2).mean())
            if best is None or err < best[0]:
                best = (err, corner, slope, o, yy.copy())
            print("   o%-4.0f  %4.0f dB/oct        %8.2f                 %+6.1f  %+6.1f"
                  % (corner, slope, err, o[6] - o_ref[6], o[7] - o_ref[7]))
    err, corner, slope, o_best, y_best = best
    print("\n  best: rolloff %.0f dB/oct above order %.0f -> octave rms err %.2f dB"
          % (slope, corner, err))
    h_best = np.array(harmonicity(y_best, fs, t, rpm))
    print("  harmonicity with that rolloff: "
          + " ".join("%+.2f" % v for v in h_best)
          + "   (reference " + " ".join("%+.2f" % v for v in h_ref) + ")")
    write_wav(os.path.join(OUT, "ab_portable_tuned.wav"),
              y_best / (np.abs(y_best).max() + 1e-9) * 0.9, fs, 3)
    print("  wrote out/ab_portable_tuned.wav")

    write_wav(os.path.join(OUT, "ab_portable_syn.wav"),
              y_por / (np.abs(y_por).max() + 1e-9) * 0.9, fs, 3)
    write_wav(os.path.join(OUT, "ab_portable_fft.wav"),
              y_fft / (np.abs(y_fft).max() + 1e-9) * 0.9, fs, 3)

    # ---- cost of the whole thing ----
    print("\n--- ROM / CPU estimate for the portable structure ---")
    for pts in (2048, 1024, 512):
        for nb in (21, 13, 8):
            kb = (nb * pts + NOISE_TABLE_CYCLES * pts) * 2 / 1024.0
            print("   %2d bins x %4d pts + noise table: %6.1f kB"
                  % (nb, pts, kb))
    print("   per output sample: 1 phase add, 2 wavetable reads + linear interp,"
          " 1 noise read + interp, 1 crossfade, 2 gains")
    print("   ~ 12-16 MAC/sample -> %.2f MMAC/s at 48 kHz" % (14 * 48000 / 1e6))
    print("   (the dsPIC33AK core does that in well under 2 %% of one core at"
          " 200 MHz; the real cost is ROM, decided by bins x points)")
    print("\nwrote out/ab_portable_syn.wav and out/ab_portable_fft.wav")


if __name__ == "__main__":
    main()
