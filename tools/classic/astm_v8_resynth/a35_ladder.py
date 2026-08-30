"""Step 35: why the firmware sounds like "a different thing" -- one rung at a time.

The listening verdict on the AK512 was that the engine is not a partly-wrong
version of `demo_full_range.wav`, it is a different thing. a34 had already
measured the firmware's structure against the approved prototype at 1.38 dB
octave rms with idle imp/fold within 0.26 dB, which cannot produce that verdict --
so the difference is somewhere a34 did not look. Two places, and this step renders
both so they can be heard rather than argued about:

1  `demo_full_range.wav` is OLD. It was rendered (a17) from `tables_v3`, jitter
   0.005, no idle drift and no chord masking. Everything adopted afterwards --
   `formant_strong` (section 34/36), jitter 0.020 (36), drift +-40 rpm (25), and
   the x6 gated mask (38) -- changed the sound *on purpose*, and each change was
   judged on its own render, never against this file. The owner's "completion
   image" therefore predates four accepted decisions. Part A walks them.

2  On the board the mask was on permanently (section 39). `local_update_block()`
   read the POT every 0.667 ms with no filter, the knob at rest wanders +-30 ADC
   LSB = +-44 rpm, and after the slew limiter that is a steady 3000 rpm/s into a
   gate whose full scale is 1200 -- measured on the bench at 975/1000 mean. So the
   x6 band-limited noise that the design applies only while the speed moves was
   applied to everything, idle included. Part B renders that defect against the
   fixed firmware, over the same trajectory, so the owner can confirm from the
   file whether that is what they heard.

Part C is the ladder the board can walk live (`*cy 40`..`*cy 7F`), rendered here
so the offline and on-board ladders are the same experiment. Two rungs have no
offline twin and are marked as such: the throttle and its deadband exist only
where there is an ADC.

Every render here rms-matches to the D0 reference and shares one trajectory, so
any pair is a fair A/B; and no rung changes the random stream of another (a34's
`noise=` and `pot_noise_rpm=` are scale switches, and the ADC noise has its own
generator), so what is heard between two rungs is the element, not a reroll.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from a09_resynth import EngineTables  # noqa: E402
from a20_portable_synth import octaves  # noqa: E402
from a32_clatter import CLAT_LO, clatter, lowpass, render, save  # noqa: E402
import a34_impl_check as A34  # noqa: E402
import gen_engine_v8_tables as GEN  # noqa: E402

OUT = os.path.join(HERE, "out")
V3 = os.path.join(OUT, "tables_v3.npz")
STRONG = os.path.join(OUT, "tables_v3_low_formant_strong.npz")

# a17's `demo_full_range` keys, verbatim -- this is the trajectory of the file the
# owner calls the completion image, so every rung is judged on it and not on the
# flare a34 used. Copied rather than imported because a17's main() renders eight
# other demos on the way.
FULL_KEYS = [(0.0, 750), (2.0, 750), (2.3, 1600), (4.5, 6900), (4.9, 6900),
             (5.3, 4600), (7.4, 7000), (7.8, 7000), (8.2, 5000), (10.4, 6950),
             (10.9, 6950), (11.6, 2600), (13.0, 2600), (13.4, 5200),
             (14.4, 5200), (15.2, 900), (17.5, 750), (19.0, 750)]
REF_JIT = 0.005      # what a17 used; 0.020 was adopted later (section 36)

# The settled tail: 750 rpm from 17.5 s to the end. This is the window where the
# design says the mask must be OFF, so it is where part B's defect has to show.
TAIL_T0, TAIL_T1, TAIL_RPM = 17.7, 18.9, 750.0

# The bench measurement, converted: 4095 counts span IDLE..MAX rpm, and the knob
# at rest wandered 0..59 counts, i.e. +-30.
POT_LSB_RPM = (6875.0 - 900.0) / 4095.0
ADC_NOISE_RPM = 30.0 * POT_LSB_RPM


def full_traj():
    tk = np.array([k[0] for k in FULL_KEYS], float)
    rk = np.array([float(k[1]) for k in FULL_KEYS])
    tg = np.arange(0.0, tk[-1], 0.002)
    return tg, np.interp(tg, tk, rk)


def with_drift(tg, rg, fs, seed=A34.SEED):
    """The idle wander, as a trajectory -- the prototype has no block loop."""
    n = int(tg[-1] * fs / A34.BLOCK)
    d = A34.ou_drift(n, A34.Lcg(seed), fs_upd=fs / A34.BLOCK)
    tb = np.arange(n) * (A34.BLOCK / fs)
    return rg + np.interp(tg, tb, d)


def proto(tab, tg, rg, fs, jitter, mask_amt=1.0):
    """A prototype render, optionally masked the way a33 masks."""
    y = render(tab, rg, tg, fs, jitter=jitter)
    if mask_amt <= 1.0:
        return y
    hi = render(tab, rg, tg, fs, jitter=jitter, noise=mask_amt)
    n = min(len(y), len(hi))
    w = np.interp(np.arange(n) / fs, tg,
                  np.clip(np.abs(np.gradient(rg, tg)) / A34.MASK_SCALE, 0.0, 1.0))
    return y[:n] + w * lowpass(hi[:n] - y[:n], fs, CLAT_LO)


class Rung(object):
    """One render, its file, and the two numbers that separate the rungs."""

    def __init__(self, key, label, y, fs, ref=None):
        self.key, self.label, self.fs = key, label, fs
        self.rms = y.std()
        self.o, _ = octaves(y / (self.rms + 1e-30), fs)
        self.imp, self.fold = clatter(y, fs, TAIL_RPM, t0=TAIL_T0, t1=TAIL_T1)
        save(y, fs, "ladder_%s.wav" % key,
             ref_rms=(ref.rms if ref is not None else None))

    def vs(self, other):
        return float(np.sqrt(((self.o - other.o) ** 2).mean()))


def head(title):
    print("\n%s" % title)
    print("  %-26s %-26s %13s %6s %6s"
          % ("rung", "file", "oct rms vs D0", "imp", "fold"))


def show(r, d0):
    print("  %-26s %-26s %10.2f dB %6.2f %6.2f"
          % (r.key + " " + r.label, "ladder_%s.wav" % r.key,
             r.vs(d0) if d0 is not None else 0.0, r.imp, r.fold))


def main():
    v3, strong = EngineTables(V3), EngineTables(STRONG)
    fs = v3.fs
    tg, rg = full_traj()
    rg_d = with_drift(tg, rg, fs)
    b = GEN.build(STRONG, GEN.DEFAULT_SCHEDULE, GEN.NOISE_PTS, GEN.NOISE_CYCLES)

    print(__doc__.split("\n")[0])
    print("\ntrajectory: a17 demo_full_range, %.1f s, %.0f..%.0f rpm; tail "
          "%.1f-%.1f s at %.0f rpm" % (tg[-1], rg.min(), rg.max(),
                                       TAIL_T0, TAIL_T1, TAIL_RPM))
    print("ADC noise for part B: +-30 counts = +-%.1f rpm (bench, section 39)"
          % ADC_NOISE_RPM)

    # ---- part A: the design decisions taken after the reference was rendered --
    head("Part A -- what was adopted AFTER demo_full_range.wav (prototype "
         "renders,\n           so a difference here is a design decision, not the C)")
    d0 = Rung("d0", "= demo_full_range", proto(v3, tg, rg, fs, REF_JIT), fs)
    show(d0, None)
    for key, label, tab, jit, rgx, amt in (
            ("d1", "+ formant_strong", strong, REF_JIT, rg, 1.0),
            ("d2", "+ jitter 0.020", strong, A34.JIT, rg, 1.0),
            ("d3", "+ drift +-40 rpm", strong, A34.JIT, rg_d, 1.0),
            ("d4", "+ mask x6 (= sec 38)", strong, A34.JIT, rg_d, A34.MASK_AMT)):
        show(Rung(key, label, proto(tab, tg, rgx, fs, jit, amt), fs, d0), d0)

    # ---- part B: the firmware, with and without the bench defect -------------
    head("Part B -- the firmware itself, and the defect section 39 found on the\n"
         "           board (an unfiltered POT holding the mask gate open)")
    for key, label, kw in (
            ("d5", "firmware (fixed)", {}),
            ("d6", "firmware + ADC defect", {"pot_noise_rpm": ADC_NOISE_RPM})):
        y = A34.impl_render(b, tg, rg, fs, drift=A34.DRIFT_RPM, **kw)
        show(Rung(key, label, y, fs, d0), d0)

    # ---- part C: the ladder the board walks live ----------------------------
    head("Part C -- the stage ladder, same masks as the board's *cy subcodes\n"
         "           (41/43 -- throttle and its deadband -- need an ADC: board only)")
    for key, label, kw in (
            ("s40", "wave only", dict(jitter=0.0, noise=0.0, mask_amt=1.0)),
            ("s47", "+ jitter", dict(noise=0.0, mask_amt=1.0)),
            ("s4f", "+ noise", dict(mask_amt=1.0)),
            ("s5f", "+ drift", dict(mask_amt=1.0, drift=A34.DRIFT_RPM)),
            ("s7f", "+ mask = all", dict(drift=A34.DRIFT_RPM))):
        show(Rung(key, label, A34.impl_render(b, tg, rg, fs, **kw), fs, d0), d0)

    print("\nHow to read this: oct rms is distance from the owner's reference in"
          "\neight octave bands, on rms-matched signals. imp/fold are section 32's"
          "\nclatter numbers measured in the settled tail -- fold is the part that"
          "\nis cycle-locked, i.e. the cam clatter, and it is what the mask covers"
          "\nup. A rung that moves fold in the tail is audible as a change of"
          "\ncharacter at the idle, which is the complaint.")


if __name__ == "__main__":
    main()
