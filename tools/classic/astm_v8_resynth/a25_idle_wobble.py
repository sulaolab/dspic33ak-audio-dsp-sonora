"""Step 25: how unstable should the 900 rpm idle be?

The owner picked 900 rpm and asked for the wobble to be ON, with a reason worth
recording: this is an old design and a real one does not sit still. So the
question is no longer *whether* but *how much*, and there are two independent
knobs, which sound different:

  drift   slow speed variation (a random walk, well under 1 Hz). Heard as the
          whole engine breathing -- pitch and rumble move together.
  jitter  cycle-to-cycle period variation (already in `synth`). Heard as
          roughness *within* the idle, not as a pitch change.

Both are properties of the RPM input, not of the tables, so whatever is chosen
here costs nothing to implement on the target beyond an LFO/noise source.

Writes (8 s each, 900 rpm, measured-only tables):
  idle_900_drift20.wav        +-20 rpm, jitter 0.008   (what was auditioned)
  idle_900_drift40.wav        +-40 rpm, jitter 0.008
  idle_900_drift60.wav        +-60 rpm, jitter 0.008
  idle_900_drift40_rough.wav  +-40 rpm, jitter 0.018   (same drift, rougher firing)
"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a22_idle_normal_model import measured_only  # noqa: E402

OUT = os.path.join(HERE, "out")
SPEED = 900.0
DUR = 8.0
VARIANTS = (("drift20", 20.0, 0.008),
            ("drift40", 40.0, 0.008),
            ("drift60", 60.0, 0.008),
            ("drift40_rough", 40.0, 0.018))
RNG = np.random.default_rng(4711)


def random_walk(n, amp):
    """Brownian drift, scaled to +-amp. 1/f^2 spectrum, so it is slow by
    construction -- no filter needed, and no periodicity to latch onto."""
    w = np.cumsum(RNG.normal(0.0, 1.0, n))
    w -= w.mean()
    return w / (np.abs(w).max() + 1e-12) * amp


def cycle_level_spread(y, fs, rpm):
    """Std of the level, measured in one-engine-cycle windows, in dB.

    This is the audible consequence of both knobs together: an idle that is
    'unstable' is one whose per-cycle loudness moves, whatever the cause.
    """
    n = max(8, int(round(120.0 / rpm * fs)))
    r = np.sqrt(ndimage.uniform_filter1d(y.astype(float) ** 2, n))
    r = r[n:-n]
    return float(np.std(20 * np.log10(r + 1e-12)))


def main():
    tab = EngineTables(os.path.join(OUT, measured_only()))
    fs = tab.fs
    tg = np.arange(0.0, DUR, 0.002)
    print("%.0f rpm, %.1f s, tables %.0f..%.0f rpm (measured only)"
          % (SPEED, DUR, tab.rpm[0], tab.rpm[-1]))
    print("\n  variant            drift[rpm]  jitter   rpm std   per-cycle level std")
    for name, amp, jit in VARIANTS:
        rg = SPEED + random_walk(len(tg), amp)
        y, _, ry = synth(rg, tg, tab, fs, jitter=jit)
        write_wav(os.path.join(OUT, "idle_900_%s.wav" % name),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)
        print("  idle_900_%-14s  %+5.0f    %5.3f    %5.1f rpm    %5.2f dB"
              % (name, amp, jit, np.std(ry), cycle_level_spread(y, fs, SPEED)))

    print("\n  Pick by ear. drift is a pitch/rumble movement, jitter is roughness;"
          "\n  both are RPM-side, so neither changes the tables or the ROM cost.")


if __name__ == "__main__":
    main()
