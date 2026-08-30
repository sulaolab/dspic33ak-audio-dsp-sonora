"""Step 33: the adopted configuration, and the chord that is left on the way down.

Round 6 settled the table set: **`formant_strong` + jitter 0.020 is adopted**
(low-rpm bins whose envelope is held in Hz at full strength, x1.5 / +-18 dB,
drag 0.60 against the car's 0.57 -- a31 -- and +5.66 dB of cycle-locked cam
clatter -- a32). `noiseup` was liked on top of it, more so band-limited, and more
again with the jitter.

The new information is *where* the chord now is: **the falling side**. That is
measurable, and it turns out to be a flaw in how `noiseup` is gated, not a new
mystery. The gate is `clip(|d rpm/dt| / 4000)`, and on this trajectory:

  rise, through the frozen region      8656 rpm/s   gate 0.91   for 0.20 s
  fall, through the frozen region      2171 rpm/s   gate 0.54   for 0.52 s
  fall, the last creep to idle          ~80 rpm/s   gate ~0     for 1.63 s

The descent crosses the frozen region 2.6x more slowly than the rise, so it gets
*half* the masking, for *twice* as long -- and the final settle gets none at all.
The chord survives the way down because the masking fades out exactly where the
model is weakest. Part 1 measures that instead of assuming 4000 was a sensible
scale, and reports it per direction.

Part 2 renders the adopted configuration with the gate re-scaled, plus a fall-only
demo so the descent can be judged without the rise in the way:

  adopt_base              formant_strong + jitter 0.020, nothing else (reference)
  adopt_mask4000          + band-limited noiseup on today's gate
  adopt_mask1200          + the same on a gate that reaches 1.0 on the descent
  adopt_mask1200_full     the same, extra noise not band-limited
  adopt_mask1200_x6       the same, x6 instead of x4
  adopt_maskfall          asymmetric: full masking while decelerating, today's
                          scale while accelerating -- tests whether the descent
                          is the only place that needs it
  adopt_maskcreep         the same as mask1200 but the gate keeps a floor of 0.5
                          while the speed is still moving at all inside the
                          frozen region, so the slow settle at the bottom is
                          covered too; it still reaches 0 at the settled idle,
                          so the accepted idle is untouched
  fall_3000_900_base      3000 -> 900 rpm overrun alone, adopted tables
  fall_3000_900_mask1200  the same with the re-scaled masking

All rms-matched to `adopt_base`, same trajectory and same seed, so a difference
is the fix.

Known limitation, stated rather than modelled: on a real overrun there is no
combustion, so the descent is not simply the rise played backwards. Nothing in
the measured set covers a closed-throttle descent, so every render here treats
up and down as the same tables. If the descent still sounds wrong after the
masking is fixed, that is the next thing to measure -- and it needs a recording.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from a09_resynth import EngineTables  # noqa: E402
from a29_chord_fix import IDLE_RPM  # noqa: E402
from a31_formant import flare_traj  # noqa: E402
from a32_clatter import CLAT_LO, clatter, lowpass, render, save  # noqa: E402

OUT = os.path.join(HERE, "out")
STRONG = os.path.join(OUT, "tables_v3_low_formant_strong.npz")
JIT = 0.020                  # adopted
FROZEN = 2125.0              # below this the low-region model is in charge


MOVING = 300.0               # rpm/s: below this the speed is not really moving


def traverse(tg, rg, scale):
    """(seconds, mean gate) while *moving through* the frozen region, per direction.

    A mean gate over the whole sub-2125 rpm part of the trajectory was tried
    first and is the wrong number: most of that time is the settled idle and the
    slow pre-flare creep, where the speed barely moves, no chord is audible and
    the gate is *supposed* to be 0 -- it diluted the figure to 0.18/0.28 and hid
    what the gate does on the traverse. A pass/fail count against a gate
    threshold was tried second and is also wrong: this trajectory is piecewise
    linear, so the slew is constant across the whole descent and any threshold
    turns into all-or-nothing. The mean gate over the traverse is the number that
    says what happens.
    """
    dt = float(np.diff(tg)[0])
    slew = np.gradient(rg, tg)
    g = np.clip(np.abs(slew) / scale, 0.0, 1.0)
    trav = (rg < FROZEN) & (np.abs(slew) > MOVING)
    return [(sel.sum() * dt, g[sel].mean() if sel.any() else float("nan"))
            for sel in (trav & (slew > 0), trav & (slew < 0))]


def main():
    tab = EngineTables(STRONG)
    fs = tab.fs
    tg, rg = flare_traj()
    slew = np.gradient(rg, tg)

    print("Part 1 -- where does the masking actually reach?")
    print("  peak slew on the rise %+7.0f rpm/s, on the fall %+7.0f rpm/s"
          % (slew.max(), slew.min()))
    print("  traverse of the frozen region (< %.0f rpm, moving > %.0f rpm/s):"
          % (FROZEN, MOVING))
    up0, dn0 = traverse(tg, rg, 4000.0)
    ratio = dn0[0] / max(up0[0], 1e-9)
    print("    rising  %.2f s     falling  %.2f s   <- the fall takes %.1fx"
          " longer" % (up0[0], dn0[0], ratio))
    print("  mean gate over that traverse (1.0 = fully masked):")
    print("  %-24s %8s %8s" % ("gate scale", "rising", "falling"))
    for sc in (4000.0, 2000.0, 1200.0, 800.0):
        up, dn = traverse(tg, rg, sc)
        print("  clip(|slew|/%-8.0f) %8.2f %8.2f" % (sc, up[1], dn[1]))
    slow = float(np.diff(tg)[0]) * ((rg < FROZEN) & (slew < 0)
                                    & (np.abs(slew) <= MOVING)).sum()
    print("  -> the fall crosses the frozen region %.1fx more slowly than the"
          " rise, so\n     at scale 4000 it gets %.2f of the masking where the"
          " rise gets %.2f --\n     and it gets it for %.1fx as long. That is"
          " where the chord is now being\n     heard. Any scale <= 2000 masks the"
          " descent fully and still leaves the\n     settled idle untouched,"
          " where the slew is 0 by construction."
          % (ratio, dn0[1], up0[1], ratio))
    print("  a further %.2f s of the descent moves slower than %.0f rpm/s and is"
          "\n  excluded from the table; the gate is near 0 there at any scale, so"
          " if a\n  chord is heard right at the bottom, that is the place."
          % (slow, MOVING))

    base = render(tab, rg, tg, fs, jitter=JIT)
    ref = base.std()

    def gated(scale, amt=4.0, band=CLAT_LO, fall_only=False, creep=False):
        hi = render(tab, rg, tg, fs, jitter=JIT, noise=amt)
        n = min(len(base), len(hi))
        s = np.clip(np.abs(slew) / scale, 0.0, 1.0)
        if fall_only:
            s = np.clip(np.where(slew < 0.0, np.abs(slew) / 800.0,
                                 np.abs(slew) / 4000.0), 0.0, 1.0)
        if creep:
            # floor of 0.5 wherever the speed is still moving inside the frozen
            # region; 0 at the settled idle, so the accepted idle is untouched
            s = np.maximum(s, 0.5 * ((rg < FROZEN) & (np.abs(slew) > 20.0)))
        w = np.interp(np.arange(n) / fs, tg, s)
        ex = hi[:n] - base[:n]
        if band is not None:
            ex = lowpass(ex, fs, band)
        return base[:n] + w * ex

    outs = {
        "adopt_base.wav": base,
        "adopt_mask4000.wav": gated(4000.0),
        "adopt_mask1200.wav": gated(1200.0),
        "adopt_mask1200_full.wav": gated(1200.0, band=None),
        "adopt_mask1200_x6.wav": gated(1200.0, amt=6.0),
        "adopt_maskfall.wav": gated(1200.0, fall_only=True),
        "adopt_maskcreep.wav": gated(1200.0, creep=True),
    }

    # the descent on its own: 3000 rpm, closed throttle, down to idle
    tk = np.array([0.0, 0.4, 0.4 + 1.9, 0.4 + 1.9 + 2.0])
    rk = np.array([3000.0, 3000.0, IDLE_RPM * 1.05, IDLE_RPM])
    tg2 = np.arange(0.0, tk[-1], 0.002)
    rg2 = np.interp(tg2, tk, rk)
    slew2 = np.gradient(rg2, tg2)
    f_base = render(tab, rg2, tg2, fs, jitter=JIT)
    f_hi = render(tab, rg2, tg2, fs, jitter=JIT, noise=4.0)
    n = min(len(f_base), len(f_hi))
    w = np.interp(np.arange(n) / fs, tg2, np.clip(np.abs(slew2) / 1200.0, 0.0, 1.0))
    outs["fall_3000_900_base.wav"] = f_base
    outs["fall_3000_900_mask1200.wav"] = (
        f_base[:n] + w * lowpass(f_hi[:n] - f_base[:n], fs, CLAT_LO))

    for nm, y in outs.items():
        save(y, fs, nm, ref_rms=ref if nm.startswith("adopt") else f_base.std())

    print("\nPart 2 -- the clatter is unaffected by any of the masking"
          " (t = 3.5-5.4 s tail)")
    print("  %-26s %7s %7s" % ("", "imp", "fold"))
    for nm in ("adopt_base.wav", "adopt_mask1200.wav", "adopt_mask1200_x6.wav"):
        i, f = clatter(outs[nm] * (ref / (outs[nm].std() + 1e-30)), fs, IDLE_RPM)
        print("  %-26s %7.2f %7.2f" % (nm[:-4], i, f))
    print("  (the gate is 0 at the settled idle, so these are the same signal"
          " there\n   -- the table is a check that nothing leaked, not a result.)")

    print("\n  files: " + ", ".join(sorted(outs)))
    print("  adopted so far: tables_v3_low_formant_strong (4 x 256-point tables,"
          "\n  2.0 kB ROM) + jitter %.3f. The masking on top is a runtime gain on"
          "\n  the residual-noise generator: no ROM, no extra table read."
          % JIT)


if __name__ == "__main__":
    main()
