"""Step 22: idle from the *normal* model, at 800 / 900 / 1000 rpm.

The extrapolated idle bin failed its listening test.  The owner's original
proposal is to drop the special-case idle bin entirely and simply run the
measured model at idle speed -- the tables are order-locked, so advancing the
phase at 900 rpm scales every feature down with it.

This build therefore uses the measured bins only (2125-6875 rpm, no extrapolated
bin).  `interp_at` clamps to the lowest bin, so at 800-1000 rpm the timbre is the
2125 rpm measurement and only the speed differs.  Nothing is invented.

Writes, for each speed:
  idle_normal_<rpm>.wav          dead steady, as asked
  idle_normal_<rpm>_wobble.wav   with +-20 rpm of slow drift, because a real
                                 idle never sits still and a dead-steady
                                 wavetable can read as a synthesiser
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402

OUT = os.path.join(HERE, "out")
SPEEDS = (800.0, 900.0, 1000.0)
DUR = 6.0
RNG = np.random.default_rng(90210)


def measured_only(src="tables_v3.npz", dst="tables_v3_measured.npz"):
    """Drop the extrapolated idle bin, keeping the measured pull bins only."""
    d = dict(np.load(os.path.join(OUT, src), allow_pickle=True))
    keep = np.array(["EXTRAPOLATED" not in str(s) for s in d["src"]])
    for k in ("rpm", "wave", "gain", "noise_rms", "noise_spec", "noise_env",
              "ncyc", "src"):
        d[k] = d[k][keep]
    np.savez(os.path.join(OUT, dst), **d)
    print("measured-only table set: %d bins, %.0f..%.0f rpm (dropped %d extrapolated)"
          % (len(d["rpm"]), d["rpm"][0], d["rpm"][-1], (~keep).sum()))
    return dst


def main():
    dst = measured_only()
    tab = EngineTables(os.path.join(OUT, dst))
    fs = tab.fs
    print("lowest measured bin = %.0f rpm; below that the timbre is held and only"
          " the speed changes" % tab.rpm[0])

    edges = None
    rows = []
    for rpm in SPEEDS:
        tg = np.arange(0.0, DUR, 0.002)
        rg = np.full_like(tg, rpm)
        y, _, _ = synth(rg, tg, tab, fs, jitter=0.006)
        write_wav(os.path.join(OUT, "idle_normal_%.0f.wav" % rpm),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)

        # slow wobble: a random walk of +-20 rpm, ~1 Hz
        w = np.cumsum(RNG.normal(0.0, 1.0, len(tg)))
        w = w / (np.abs(w).max() + 1e-9) * 20.0
        yw, _, _ = synth(rpm + w, tg, tab, fs, jitter=0.008)
        write_wav(os.path.join(OUT, "idle_normal_%.0f_wobble.wav" % rpm),
                  yw / (np.abs(yw).max() + 1e-9) * 0.9, fs, 3)

        o, edges = octaves(y, fs)
        rows.append((rpm, y, o))
        print("  %.0f rpm: firing (order 4) = %.1f Hz, cycle %.1f ms,"
              " %d cycles in %.0f s -> idle_normal_%.0f.wav (+ _wobble)"
              % (rpm, rpm * 4 / 60.0, 120000.0 / rpm, DUR * rpm / 120.0, DUR, rpm))

    print("\n  octave-band levels, each normalised to its own peak [dB]")
    print("   rpm    " + "".join("%8d" % e for e in edges[:-1]))
    for rpm, _, o in rows:
        print("  %5.0f   " % rpm + "".join("%8.1f" % v for v in (o - o.max())))
    print("\n  (These three differ only in speed: same tables, same timbre. If none"
          " of them works, the missing ingredient is content the recordings do not"
          " contain at idle, not a tuning parameter.)")


if __name__ == "__main__":
    main()
