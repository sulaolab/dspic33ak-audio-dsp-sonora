#!/usr/bin/env python3
"""The rear M30 resampler own passband droop, as a law in the rear STEP.

Phase H SS3.  asrc_universal_passband_decompose.py showed that 6 of the 15 uniform-passband
failures are owned by the REAR prototype, not by any front-end stage, and that the rear
contribution is -0.3892 dB at rear step 1.00000 and only -0.0184 dB at rear step 1.08844.
That is not a coincidence and it is not a per-pair property: with the tone pinned to
0.8125 * FN_out, the rear prototype sees it at

    f / fs_rear_in  =  0.8125 * fs_out / 2 / (step * fs_out)  =  0.40625 / step

so the rear droop depends on the STEP ALONE, exactly like the phase-interpolation spur
does (that is why one sweep serves every pair sharing a rear ratio).  This tool prints
that law, so the SS4 redesign can be stated as a constraint on the step instead of being
re-discovered per pair:

    step_min(budget)   the smallest rear step whose rear-only droop fits a given dB budget

The rear prototype is ONE SHIPPED TABLE shared by every rate.  A front-end redesign
cannot change it.  So a pair whose rear droop alone exceeds the uniform +/-0.10 dB has
only two outcomes: a chain topology that raises its rear step, or the pair does not meet
the uniform spec at all.

The prototype is read from the firmware coefficient set through
asrc_universal_nyquist_model.rear_prototype(), i.e. the same authority the 45-pair driver
uses -- this tool re-reads nothing and re-designs nothing.

Usage:
    python tools/asrc/asrc_rear_passband_law.py
    python tools/asrc/asrc_rear_passband_law.py --csv out.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import asrc_universal_nyquist_model as U          # noqa: E402

PROTECTED_FRACTION = 0.8125 / 2.0                 # of fs_out
PB_LIMIT_DB = 0.10
MARGIN_DB = 0.05                                  # what SS4 can afford to leave to the rear


def rear_droop_db(step: float) -> float:
    """DC-referenced rear-prototype response at 0.8125*FN_out, for this rear step.

    Everything is normalised to fs_out = 1, which is what makes the answer a function of
    the step alone.
    """
    rear = U.rear_prototype()
    rate = float(step)                            # rear input rate, fs_out = 1
    f = np.array([0.0, PROTECTED_FRACTION])
    d = U.db(U.mag_at(rear, float(U.L_REAR) * rate, f))
    return float(d[1] - d[0])


def step_for_budget(budget_db: float) -> float:
    """Smallest step whose |rear droop| <= budget_db.  Monotone in step, so bisect."""
    lo, hi = 1.0, 8.0
    if abs(rear_droop_db(lo)) <= budget_db:
        return 1.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if abs(rear_droop_db(mid)) <= budget_db:
            hi = mid
        else:
            lo = mid
    return hi


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    print("rear M30 prototype: L_REAR=%d, taps=%d, prototype length=%d"
          % (U.L_REAR, U.REAR.taps, len(U.rear_prototype())))
    print("tone pinned at 0.8125*FN_out, so the rear sees it at 0.40625/step of its input rate\n")

    steps = [1.0, 1.02, 1.04, 1.05, 1.06, 1.07, 1.08, 1.08844, 1.09, 1.10, 1.125, 1.15,
             1.20, 1.25, 1.33333, 1.37812, 1.45, 1.5, 1.6, 1.75, 2.0, 2.2, 2.4, 2.75,
             3.0, 4.0, 6.0]
    rows = []
    print("%10s %14s %12s %-14s" % ("rear step", "f/fs_rear_in", "rear droop", "vs +/-0.10 dB"))
    print("-" * 56)
    for s in steps:
        d = rear_droop_db(s)
        v = "PASS" if abs(d) <= PB_LIMIT_DB else "FAIL"
        rows.append(dict(rear_step=s, f_over_fs=PROTECTED_FRACTION / s,
                         rear_droop_db=d, verdict=v))
        print("%10.5f %14.6f %+12.4f %-14s" % (s, PROTECTED_FRACTION / s, d, v))

    print()
    for b in (0.10, 0.05, 0.02, 0.01):
        s = step_for_budget(b)
        print("smallest rear step with |rear droop| <= %.2f dB : %.5f  (droop %+.4f dB)"
              % (b, s, rear_droop_db(s)))
    print()
    print("READ THIS AS THE SS4 CONSTRAINT.  A chain whose rear step is below %.5f cannot meet"
          % step_for_budget(PB_LIMIT_DB))
    print("the uniform +/-0.10 dB no matter what its front end does, because the rear prototype")
    print("alone already spends more than the whole budget there.  Leaving the front end")
    print("%.2f dB of the budget needs step >= %.5f." % (PB_LIMIT_DB - MARGIN_DB,
                                                        step_for_budget(MARGIN_DB)))

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["rear_step", "f_over_fs", "rear_droop_db",
                                              "verdict"])
            w.writeheader()
            w.writerows(rows)
        print("\nwrote %s (%d rows)" % (args.csv, len(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
