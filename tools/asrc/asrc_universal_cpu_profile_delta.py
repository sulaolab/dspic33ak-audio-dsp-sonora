#!/usr/bin/env python3
"""WHAT THE UNIVERSAL MIGRATION COSTS IN CPU, at both constants (Phase H SS7).

The uniform passband is not bought with taps.  asrc_universal_passband_decompose.py showed that
11 of the 15 failing rows fail because their shipped structure decimates all the way down to
Fs_out, which leaves the rear resampler running at step 1.0 where its own prototype droops
-0.3892 dB at 0.8125 x FN -- already 3.9x the whole budget, and not curable by any front-end
change (asrc_rear_passband_law.py).  Those rows can only migrate by CHANGING TOPOLOGY: stopping
the front end higher and letting the rear do more.  A front-end stage that stops higher runs at a
higher rate, so the migration is paid for in MAC RATE, and that is what this tool measures.

IT COMPARES TWO PROFILE RUNS OF THE SAME DRIVER, NOT TWO MODES.  Reading a legacy number in a
universal column (or the reverse) is the F-C mistake with new labels, so every column here is
stamped with its profile and the two are never subtracted without both being named.

MAC COUNTS COME FROM THE CHOSEN CHAIN OF EACH RUN -- never carried over from the other profile,
which is the SS7 instruction: "do not reuse the old planner MAC values".  cpu% is exactly linear
in cycles/MAC, so both constants are computed from one MAC count rather than from two runs that
could differ for another reason.

THE COLUMN NAMES ARE MODEL STATUSES, NOT PRODUCT CHANNEL LIMITS (SS7).  `16ch-model-pass` means
the model at that constant puts the front end inside the 16-channel budget; it does not say the
product supports 16 channels, and `12ch-model-only` does not say a rate is 12-channel-only.

Usage:
  python tools/asrc/asrc_universal_cpu_profile_delta.py \\
      --legacy <capture-dir>/h6_legacy_L256.csv \\
      --universal <capture-dir>/h6_universal_L256.csv [--csv out.csv]
"""
from __future__ import annotations

import argparse
import csv
import pathlib

CYC_CONSERVATIVE = 1.90
CYC_SINGLE_STAGE = 1.42
CPU_HZ = 200e6
BUDGET_P1_PCT = 40.0            # 16 ch at 48 kHz, reserve included
BUDGET_P2_PCT = 45.0            # 12 ch at 96 kHz, reserve included

TIER_NAME = {0: "16ch-model-pass", 1: "12ch-model-only", 2: "model-over"}


def load(path):
    with pathlib.Path(path).open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def pct(macs, ch, cyc):
    return 100.0 * cyc * ch * macs / CPU_HZ


def tier(macs, cyc):
    if pct(macs, 16, cyc) <= BUDGET_P1_PCT:
        return 0
    if pct(macs, 12, cyc) <= BUDGET_P2_PCT:
        return 1
    return 2


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--legacy", required=True)
    ap.add_argument("--universal", required=True)
    ap.add_argument("--csv", default=None)
    a = ap.parse_args()

    leg = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.legacy)}
    uni = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.universal)}
    print("LEGACY   %s  (%d rows)" % (a.legacy, len(leg)))
    print("UNIVERSAL %s  (%d rows)" % (a.universal, len(uni)))
    print("budgets: 16 ch <= %.0f %% of the core, 12 ch <= %.0f %%; constants %.2f and %.2f "
          "cyc/MAC\n" % (BUDGET_P1_PCT, BUDGET_P2_PCT, CYC_CONSERVATIVE, CYC_SINGLE_STAGE))

    out = []
    hdr = ("%-7s %-7s | %-26s %8s %8s %-16s | %-26s %8s %8s %-16s | %9s %s"
           % ("in", "out", "legacy chain", "@1.90", "@1.42", "tier@1.90",
              "universal chain", "@1.90", "@1.42", "tier@1.90", "dMACs", "mig"))
    print(hdr)
    print("-" * len(hdr))
    n_tier_moves_const = 0
    n_tier_worse_profile = 0
    for k in sorted(set(leg) | set(uni)):
        lr, ur = leg.get(k), uni.get(k)
        lm = int(lr["macs_per_sec"]) if lr else None
        um = int(ur["macs_per_sec"]) if ur else None
        lt = tier(lm, CYC_CONSERVATIVE) if lm is not None else None
        ut = tier(um, CYC_CONSERVATIVE) if um is not None else None
        ut42 = tier(um, CYC_SINGLE_STAGE) if um is not None else None
        if ut is not None and ut42 is not None and ut != ut42:
            n_tier_moves_const += 1
        if lt is not None and ut is not None and ut > lt:
            n_tier_worse_profile += 1
        dm = ("%+8.1f%%" % (100.0 * (um - lm) / lm)) if (lm and um) else ("n/a" if lm else "new")
        print("%-7d %-7d | %-26s %7.1f%% %7.1f%% %-16s | %-26s %7.1f%% %7.1f%% %-16s | %9s %s"
              % (k[0], k[1],
                 (lr["chain"][:26] if lr else "(no legacy row)"),
                 pct(lm, 16, CYC_CONSERVATIVE) if lm else 0.0,
                 pct(lm, 16, CYC_SINGLE_STAGE) if lm else 0.0,
                 TIER_NAME[lt] if lt is not None else "-",
                 (ur["chain"][:26] if ur else "(no universal row)"),
                 pct(um, 16, CYC_CONSERVATIVE) if um else 0.0,
                 pct(um, 16, CYC_SINGLE_STAGE) if um else 0.0,
                 TIER_NAME[ut] if ut is not None else "-",
                 dm, (ur.get("universal_migration", "") if ur else "")))
        out.append(dict(
            fs_in_hz=k[0], fs_out_hz=k[1],
            legacy_chain=(lr["chain"] if lr else ""),
            legacy_macs_per_sec=(lm if lm is not None else ""),
            legacy_cpu_pct_16ch_1p90=("%.2f" % pct(lm, 16, CYC_CONSERVATIVE)) if lm else "",
            legacy_cpu_pct_16ch_1p42=("%.2f" % pct(lm, 16, CYC_SINGLE_STAGE)) if lm else "",
            legacy_cpu_pct_12ch_1p90=("%.2f" % pct(lm, 12, CYC_CONSERVATIVE)) if lm else "",
            legacy_cpu_pct_12ch_1p42=("%.2f" % pct(lm, 12, CYC_SINGLE_STAGE)) if lm else "",
            legacy_tier_1p90=(TIER_NAME[lt] if lt is not None else ""),
            legacy_tier_1p42=(TIER_NAME[tier(lm, CYC_SINGLE_STAGE)] if lm else ""),
            universal_chain=(ur["chain"] if ur else ""),
            universal_origin=(ur["chain_origin"] if ur else ""),
            universal_migration=(ur.get("universal_migration", "") if ur else ""),
            universal_macs_per_sec=(um if um is not None else ""),
            universal_cpu_pct_16ch_1p90=("%.2f" % pct(um, 16, CYC_CONSERVATIVE)) if um else "",
            universal_cpu_pct_16ch_1p42=("%.2f" % pct(um, 16, CYC_SINGLE_STAGE)) if um else "",
            universal_cpu_pct_12ch_1p90=("%.2f" % pct(um, 12, CYC_CONSERVATIVE)) if um else "",
            universal_cpu_pct_12ch_1p42=("%.2f" % pct(um, 12, CYC_SINGLE_STAGE)) if um else "",
            universal_tier_1p90=(TIER_NAME[ut] if ut is not None else ""),
            universal_tier_1p42=(TIER_NAME[ut42] if ut42 is not None else ""),
            macs_delta_pct=("%.2f" % (100.0 * (um - lm) / lm)) if (lm and um) else "",
        ))

    def counts(rows, key):
        c = {v: 0 for v in TIER_NAME.values()}
        for r in rows:
            if r[key]:
                c[r[key]] += 1
        return c

    print()
    for label, k90, k42 in (("LEGACY   ", "legacy_tier_1p90", "legacy_tier_1p42"),
                            ("UNIVERSAL", "universal_tier_1p90", "universal_tier_1p42")):
        c90, c42 = counts(out, k90), counts(out, k42)
        print("%s @1.90  %s" % (label, "   ".join("%s %d" % (k, v) for k, v in c90.items())))
        print("%s @1.42  %s" % (label, "   ".join("%s %d" % (k, v) for k, v in c42.items())))
    print()
    print("pairs whose UNIVERSAL tier changes between the two constants: %d" % n_tier_moves_const)
    print("pairs whose tier is WORSE under the universal profile than the legacy one: %d"
          % n_tier_worse_profile)
    print()
    print("READ THE TIER NAMES AS MODEL STATUSES.  `12ch-model-only` is what the CPU model says "
          "about a\nfront-end chain at a constant; it is NOT a product statement that the rate "
          "supports 12 channels.")

    if a.csv:
        with pathlib.Path(a.csv).open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(out[0].keys()))
            w.writeheader()
            w.writerows(out)
        print("\nwrote %s (%d rows)" % (a.csv, len(out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
