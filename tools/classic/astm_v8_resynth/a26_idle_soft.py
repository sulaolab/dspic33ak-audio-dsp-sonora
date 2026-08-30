"""Step 26: the idle the owner actually likes is the *quiet tail* of demo_full_range.

The owner reports that the last moments of `demo_full_range.wav` -- too quiet to
judge comfortably -- are "very close in atmosphere to the real idle". That is a
useful measurement, because it identifies which idle it is:

  demo_full_range's tail is 900 -> 750 rpm rendered from **tables_v3, i.e. the
  EXTRAPOLATED bin** (a17, keys at t=15.2/17.5/19.0 s) -- the same tables as
  `demo_idle_notilt.wav`, which FAILED its listening test.

Same tables, same speed, opposite verdict, so the difference is not the timbre.
Two candidate causes, both testable here:

  level    in the demo the whole file is peak-normalised at ~7000 rpm, so the
           idle sits far below full scale and its high orders fall under the
           listening threshold. `demo_idle_notilt.wav` normalises the idle
           itself to full scale and puts that same content in plain view.
  motion   the tail arrives *decaying* from 900 rpm; the failed demo was dead
           steady from the first sample.

So this step lifts the gain and keeps everything else, which is exactly what
turning the volume up would do, and renders the same idle steady, decaying, and
against the measured-only alternative at a matched level.

Writes (all peak-normalised so they are comparable at one volume setting):
  idle_tail_lifted.wav        the literal tail of demo_full_range, gain-lifted
  idle_extrap_750.wav         tables_v3 (extrapolated bin), steady 750 rpm
  idle_extrap_900.wav         tables_v3 (extrapolated bin), steady 900 rpm
  idle_extrap_900_settle.wav  1500 -> 900 rpm, then held: the decay in the tail
  idle_measured_900.wav       measured-only tables at 900 rpm, rms-matched
"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402
from a22_idle_normal_model import measured_only  # noqa: E402

OUT = os.path.join(HERE, "out")
TAIL_T0, TAIL_T1 = 15.0, 19.0      # a17's ramp down to idle and the held idle
DRIFT, JITTER = 40.0, 0.008        # owner: wobble ON
DUR = 8.0
RNG = np.random.default_rng(1959)


def drift_walk(n, amp):
    w = np.cumsum(RNG.normal(0.0, 1.0, n))
    w -= w.mean()
    return w / (np.abs(w).max() + 1e-12) * amp


def how_quiet():
    """How far below the loud end the idle sits in the demo the owner listened to."""
    path = os.path.join(OUT, "demo_full_range.wav")
    x, fs = read_wav(path)
    m = x.mean(axis=1) if x.ndim > 1 else x
    n = int(0.20 * fs)
    r = 20 * np.log10(np.sqrt(ndimage.uniform_filter1d(m.astype(float) ** 2, n)) + 1e-12)
    idle = np.median(r[int(17.8 * fs):int(18.8 * fs)])
    loud = r.max()
    print("demo_full_range.wav: loudest %.1f dBFS, held idle %.1f dBFS"
          " -> the idle is %.1f dB down" % (loud, idle, idle - loud))
    print("  (that is the model's own doing: the per-bin gains span the range, so"
          "\n   nothing on the target has to script it -- but it does mean the idle"
          "\n   needs its own listening level to be judged at all.)")
    seg = m[int(TAIL_T0 * fs):int(TAIL_T1 * fs)].astype(float)
    write_wav(os.path.join(OUT, "idle_tail_lifted.wav"),
              seg / (np.abs(seg).max() + 1e-9) * 0.9, fs, 3)
    print("  idle_tail_lifted.wav: t=%.1f..%.1f s of that file, lifted %.1f dB"
          % (TAIL_T0, TAIL_T1, -(idle - loud)))
    return fs


FADE_IN = 0.050          # s -- see below


def render(tab, rg, tg, fs, name, ref_rms=None):
    y, _, _ = synth(rg, tg, tab, fs, jitter=JITTER)
    # The first cycle starts abruptly at whatever speed the trajectory begins
    # with, and its noise block only gets the rising half of the overlap-add
    # window, so the file opens with a click plus a burst of hiss -- audible as
    # "noise at the head", and worst in the overrun variant, which starts at
    # 5200 rpm. A short raised-cosine fade removes both; on the target the
    # engine never starts mid-cycle at full level anyway.
    k = int(FADE_IN * fs)
    y[:k] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    g = 0.9 / (np.abs(y).max() + 1e-9)
    if ref_rms is not None:
        g = ref_rms / (y.std() + 1e-12)
    write_wav(os.path.join(OUT, name), np.clip(y * g, -0.99, 0.99), fs, 3)
    return y * g


def main():
    fs = how_quiet()
    ex = EngineTables(os.path.join(OUT, "tables_v3.npz"))
    me = EngineTables(os.path.join(OUT, measured_only()))
    print("\n  extrapolated set: %d bins, lowest %.0f rpm" % (len(ex.rpm), ex.rpm[0]))
    print("  measured-only set: %d bins, lowest %.0f rpm" % (len(me.rpm), me.rpm[0]))

    tg = np.arange(0.0, DUR, 0.002)
    d = drift_walk(len(tg), DRIFT)
    y750 = render(ex, 750.0 + d, tg, fs, "idle_extrap_750.wav")
    y900 = render(ex, 900.0 + d, tg, fs, "idle_extrap_900.wav")

    # the decay that is present in the tail and absent from the failed demo
    tk = np.array([0.0, 2.0, DUR])
    rk = np.array([1500.0, 900.0, 900.0])
    rs = np.interp(tg, tk, rk) + d * np.clip((tg - 1.5) / 1.0, 0.0, 1.0)
    render(ex, rs, tg, fs, "idle_extrap_900_settle.wav")

    # The owner pins the sound to "around 0:15" and to the burble *behind* it.
    # a17's trajectory is 5200 rpm at 14.4 s and 900 rpm at 15.2 s, so what is
    # there is a fast overrun down into idle -- reproduce exactly that shape,
    # then hold, at a level that can actually be judged.
    tk2 = np.array([0.0, 0.8, 6.5, DUR])
    rk2 = np.array([5200.0, 900.0, 900.0, 780.0])
    rd = np.interp(tg, tk2, rk2) + d * np.clip((tg - 1.0) / 1.0, 0.0, 1.0)
    render(ex, rd, tg, fs, "idle_extrap_overrun.wav")

    # the other candidate at the same loudness, so the A/B is about timbre only
    ym = render(me, 900.0 + d, tg, fs, "idle_measured_900.wav", ref_rms=y900.std())
    print("\n  idle_extrap_900 vs idle_measured_900: rms matched to %.1f dBFS"
          % (20 * np.log10(y900.std())))

    o1, edges = octaves(y900, fs)
    o2, _ = octaves(ym, fs)
    print("\n  octave bands at 900 rpm, both re their own 40 Hz band [dB]")
    print("   set          " + "".join("%8d" % e for e in edges[:-1]))
    print("   extrapolated " + "".join("%8.1f" % v for v in (o1 - o1[0])))
    print("   measured     " + "".join("%8.1f" % v for v in (o2 - o2[0])))
    print("   difference   " + "".join("%+8.1f" % v for v in ((o1 - o1[0]) - (o2 - o2[0]))))
    # Measured result, against the expectation: the difference is a nearly
    # constant offset above 40 Hz, not extra high-frequency content. So the two
    # idle candidates have essentially the same spectral *shape* and differ by
    # how much weight sits in the bottom octave -- which is why the earlier
    # full-scale idle failed on loudness/balance, not on invented junk.
    print("\n  The difference is a near-constant offset above 40 Hz (not a rising"
          "\n  curve), i.e. the two candidates share their shape and differ only in"
          "\n  how much of the bottom octave they carry. Judge them at one volume.")


if __name__ == "__main__":
    main()
