"""Step 28: the chord on fast rises is the *hole in the measurement*, not the ramp.

a27 guessed that the chord came from the trajectory being too clean, and measured
harmonicity to prove it. The measurement said the opposite (the chord-free
`demo_accl_style` scored *higher*, i.e. more fused, than the chordy ramp), so that
metric is not measuring the percept -- at 2500 rpm/s an 8192-point window smears
every line, which dominates the score. Guess discarded.

The owner's three observations discriminate a different cause perfectly:

  startup_3000rpm    900 -> 3000 rpm   CHORD
  demo_full_range    750 -> 1600 -> 6900 rpm, and the fast pulls from low rpm  CHORD
  demo_accl_style    2477 -> 6301 rpm  NO CHORD

The table set has bins at 750 rpm (extrapolated) and then nothing until 2125 rpm,
above which they are 250 rpm apart. So:

  * below 2125 rpm with the measured-only set, `interp_at` clamps: the order
    spectrum is *frozen* and only its frequency moves. That is exactly a sample
    being transposed, and a transposed harmonic stack is heard as a pitch -- a
    chord when several orders are strong.
  * between 750 and 2125 rpm with the v3 set, the model crossfades two waveforms
    across a 1375 rpm hole, which is not a measurement either.
  * above 2125 rpm the timbre is re-measured every 250 rpm, so the spectrum
    *changes shape* as the speed rises and never reads as one transposed sample.

The chord therefore marks the region where there is no data. This step measures
that directly from the tables -- no rendering, no perceptual metric -- and renders
an A/B at matched slew to confirm it by ear.

Writes:
  chord_rise_inside.wav   2500 -> 5200 rpm   (inside the measured region)
  chord_rise_cross.wav     900 -> 3600 rpm   (starts in the frozen region)
  chord_rise_cross_slow.wav same, at the fastest slew ever measured (1815 rpm/s)
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a22_idle_normal_model import measured_only  # noqa: E402

OUT = os.path.join(HERE, "out")
MEAS_MAX_SLEW = 1815.0        # a27: fastest slew in the measured trajectory


def shape_change(tab, rpm_grid, n_ord=40):
    """dB rms change of the *order* spectrum per 100 rpm.

    Zero means the timbre is frozen and the speed only transposes it.
    """
    sh = []
    for r in rpm_grid:
        w = tab.interp_at(r)[0]
        sp = np.abs(np.fft.rfft(w))[: 2 * n_ord + 1]     # bin k = order k/2
        db = 20 * np.log10(sp + 1e-12)
        sh.append(db - db.max())
    sh = np.array(sh)
    d = np.diff(sh, axis=0) / (np.diff(rpm_grid)[:, None] / 100.0)
    return np.sqrt((d ** 2).mean(axis=1))


def main():
    ex = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    me = EngineTables(os.path.join(OUT, measured_only()))
    fs = me.fs
    grid = np.arange(800.0, 6000.0, 50.0)
    ce, cm = shape_change(ex, grid), shape_change(me, grid)
    mid = grid[:-1] + 25.0

    print("order-spectrum change per 100 rpm [dB rms] -- 0.00 = frozen timbre,"
          " pure transposition")
    print("    rpm   v3 (750 + 2125..) measured-only (2125..)")
    for r in (900, 1200, 1600, 2000, 2100, 2200, 2600, 3000, 3400, 4000, 5000):
        i = int(np.argmin(np.abs(mid - r)))
        print("   %5d        %6.2f              %6.2f" % (r, ce[i], cm[i]))
    below = mid < 2125
    print("\n  below 2125 rpm: v3 mean %.2f dB/100rpm, measured-only mean %.2f"
          % (ce[below].mean(), cm[below].mean()))
    print("  above 2125 rpm: v3 mean %.2f dB/100rpm, measured-only mean %.2f"
          % (ce[~below].mean(), cm[~below].mean()))
    print("  -> with the measured-only set the timbre below 2125 rpm is exactly"
          "\n     frozen (a transposed sample); with v3 it morphs across a 1375 rpm"
          "\n     hole. Neither is a measurement, and both are where the chord is"
          "\n     reported. Above 2125 rpm the shape moves continuously.")

    # ---- A/B at matched slew: only the RPM *region* differs ----
    slew = 2500.0
    for name, lo, hi, sl in (("chord_rise_inside", 2500.0, 5200.0, slew),
                             ("chord_rise_cross", 900.0, 3600.0, slew),
                             ("chord_rise_cross_slow", 900.0, 3600.0, MEAS_MAX_SLEW)):
        dur = (hi - lo) / sl
        tk = np.array([0.0, 0.6, 0.6 + dur, 0.6 + dur + 1.2])
        rk = np.array([lo, lo, hi, hi])
        tg = np.arange(0.0, tk[-1], 0.002)
        rg = np.interp(tg, tk, rk)
        y, _, _ = synth(rg, tg, me, fs, jitter=0.010)
        k = int(0.05 * fs)
        y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
        write_wav(os.path.join(OUT, name + ".wav"),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)
        print("  %s.wav: %.0f -> %.0f rpm at %.0f rpm/s (%.2f s of rise)"
              % (name, lo, hi, sl, dur))

    print("\n  If 'inside' has no chord and 'cross' does, at the same slew, the"
          "\n  cause is the missing 900-2100 rpm measurement, not the ramp shape,"
          "\n  and the fixes are: cross that region more slowly, start the flare"
          "\n  inside the measured range, or measure 900-2100 rpm for real.")


if __name__ == "__main__":
    main()
