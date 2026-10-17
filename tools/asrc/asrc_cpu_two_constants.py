#!/usr/bin/env python3
"""THE TWO CPU CONSTANTS OVER THE AS-BUILT MAC COUNTS (Phase G, G-E).

The owner decision after F-2 was: keep BOTH 1.90 and 1.42 cyc/MAC, and re-evaluate them on the
ACTUAL MAC COUNTS once the as-built sizing is the authority.  This script does exactly that and
nothing more.  In particular it does NOT propose a new default -- the owner rule is that two or
three points do not rewrite the planner constant, and this run adds points, not measurements.

WHAT IT READS
=============
The two CSVs the G-D re-sweep writes, one per planner mode.  Both carry macs_per_sec for the
chosen chain, and cpu% is exactly linear in cycles/MAC, so both constants are computed from one
run rather than from two runs that could differ for another reason.

WHAT IT REPORTS, IN SEPARATE COLUMNS
====================================
  1  per pair, the front-end load at 1.90 and at 1.42, in each mode, with the CPU tier
  2  how many pairs CHANGE TIER between the two constants -- which separates
     "architecturally impossible inside the 16-channel budget" from "excluded by the
     conservative constant"
  3  the MODE DELTA in MACs: as-built minus experimental, per pair.  F-C measured this once, on
     44.1 -> 16 kHz, at +18.2 %.  Over 45 pairs it is a distribution, and its SIGN matters: a
     positive delta is CPU the experimental table was not charging.

WHAT IT REFUSES TO DO
=====================
Divide a model column by a hardware DSPload.  The model column is the FRONT-END FIR chain only;
DSPload is the whole image (both engines, TDM push/pull, meter, console).  That division is a
category error this study has already had to withdraw once.  There is no hardware column here at
all: Phase G touches no hardware until F-F is complete.

Usage:
  python tools/asrc/asrc_cpu_two_constants.py \\
      --qualification <capture-dir>/gd_qualification.csv \\
      --experimental  <capture-dir>/gd_experimental.csv \\
      [--csv out.csv]
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import sys

CYC_CONSERVATIVE = 1.90         # composed-chain model
CYC_SINGLE_STAGE = 1.42         # measured single-stage dense-Q31 anchor
CYC_FLOOR = 1.012               # pure kernel slope -- a FLOOR, never an estimate
CPU_HZ = 200e6
BUDGET_P1_PCT = 40.0            # 16 ch at 48 kHz, reserve included
BUDGET_P2_PCT = 45.0            # 12 ch at 96 kHz, reserve included


def load(path):
    with pathlib.Path(path).open(newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    mode = {r.get("planner_mode", "") for r in rows}
    if len(mode) != 1:
        sys.exit("%s carries %d planner modes (%s) -- one file, one mode"
                 % (path, len(mode), sorted(mode)))
    return mode.pop(), {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in rows}


def load_pct(macs: float, channels: int, cyc: float) -> float:
    return 100.0 * cyc * channels * macs / CPU_HZ


def tier(macs: float, cyc: float) -> str:
    if load_pct(macs, 16, cyc) <= BUDGET_P1_PCT:
        return "ok"
    if load_pct(macs, 12, cyc) <= BUDGET_P2_PCT:
        return "12ch-only"
    return "OVER"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--qualification", required=True)
    ap.add_argument("--experimental", required=True)
    ap.add_argument("--csv", default="")
    a = ap.parse_args()

    qmode, qual = load(a.qualification)
    emode, expl = load(a.experimental)
    if qmode != "qualification" or emode != "experimental":
        sys.exit("expected one qualification CSV and one experimental CSV, got %r and %r"
                 % (qmode, emode))

    print("=" * 120)
    print("CPU: TWO CONSTANTS OVER THE AS-BUILT MAC COUNTS  (G-E)")
    print("=" * 120)
    print("  %.2f cyc/MAC  conservative COMPOSED-chain model (carries ring push, intermediate "
          "buffer, phase schedule, scatter)" % CYC_CONSERVATIVE)
    print("  %.2f cyc/MAC  measured SINGLE-STAGE dense-Q31 anchor (factor %.2f against the "
          "above)" % (CYC_SINGLE_STAGE, CYC_SINGLE_STAGE / CYC_CONSERVATIVE))
    print("  %.3f cyc/MAC pure kernel slope -- an absolute FLOOR, reported, never used as a "
          "tier" % CYC_FLOOR)
    print("  budgets: %.0f %% front end at 16 ch / %.0f %% at 12 ch, both already containing the "
          "5 %% reserve" % (BUDGET_P1_PCT, BUDGET_P2_PCT))
    print("  every column below is FRONT-END FIR cost per channel-set.  It is NOT DSPload and "
          "must not be divided by it.")
    print()

    hdr = ("  %-15s %-30s %12s %7s %7s %-10s %-10s   %12s %8s"
           % ("pair", "as-built chain", "MACs/s/ch", "@1.90", "@1.42", "tier@1.90",
              "tier@1.42", "exp MACs", "delta"))
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))

    out = []
    moved = []
    deltas = []
    for key in sorted(qual, key=lambda k: (k[0], -k[1])):
        q = qual[key]
        e = expl.get(key)
        qm = float(q["macs_per_sec"])
        t90, t42 = tier(qm, CYC_CONSERVATIVE), tier(qm, CYC_SINGLE_STAGE)
        if t90 != t42:
            moved.append((key, t90, t42, qm))
        em = float(e["macs_per_sec"]) if e else None
        d = None
        if em is not None and em > 0.0:
            d = 100.0 * (qm - em) / em
            deltas.append((key, d, qm, em))
        elif em == 0.0 and qm > 0.0:
            d = float("inf")
        print("  %-15s %-30s %12.0f %6.1f%% %6.1f%% %-10s %-10s   %12s %8s"
              % ("%.4g -> %.4g k" % (key[0] / 1000.0, key[1] / 1000.0),
                 q["chain"][:30], qm,
                 load_pct(qm, 16, CYC_CONSERVATIVE), load_pct(qm, 16, CYC_SINGLE_STAGE),
                 t90, t42,
                 "-" if em is None else "%.0f" % em,
                 "-" if d is None else ("+inf" if d == float("inf") else "%+.1f%%" % d)))
        out.append(dict(fs_in_hz=key[0], fs_out_hz=key[1], asbuilt_chain=q["chain"],
                        asbuilt_origin=q.get("chain_origin", ""),
                        asbuilt_macs_per_sec=int(qm),
                        cpu_pct_16ch_1p90="%.2f" % load_pct(qm, 16, CYC_CONSERVATIVE),
                        cpu_pct_16ch_1p42="%.2f" % load_pct(qm, 16, CYC_SINGLE_STAGE),
                        cpu_pct_16ch_floor="%.2f" % load_pct(qm, 16, CYC_FLOOR),
                        tier_1p90=t90, tier_1p42=t42,
                        experimental_chain="" if e is None else e["chain"],
                        experimental_macs_per_sec="" if em is None else int(em),
                        macs_delta_pct="" if d is None or d == float("inf") else "%.2f" % d,
                        overall_dbc_asbuilt=q["overall_dbc"],
                        overall_dbc_experimental="" if e is None else e["overall_dbc"],
                        verdict_asbuilt=q["verdict"],
                        verdict_experimental="" if e is None else e["verdict"]))
    print()

    print("  TIER CHANGE between the two constants: %d of %d pair(s)%s"
          % (len(moved), len(qual), ":" if moved else
             " -- so no tier here is an artefact of the conservative constant"))
    for key, t90, t42, qm in moved:
        print("    %-15s %-10s -> %-10s   (16 ch %.1f %% -> %.1f %%)"
              % ("%.4g -> %.4g k" % (key[0] / 1000.0, key[1] / 1000.0), t90, t42,
                 load_pct(qm, 16, CYC_CONSERVATIVE), load_pct(qm, 16, CYC_SINGLE_STAGE)))
    print()

    if deltas:
        pos = [d for _k, d, _a, _b in deltas if d > 0.5]
        neg = [d for _k, d, _a, _b in deltas if d < -0.5]
        same = len(deltas) - len(pos) - len(neg)
        print("  MODE DELTA in MACs (as-built minus experimental), over %d comparable pair(s):"
              % len(deltas))
        print("    as-built COSTS MORE on %d pair(s), LESS on %d, within +/-0.5 %% on %d"
              % (len(pos), len(neg), same))
        if pos:
            print("    worst understatement by the experimental table: %+.1f %%" % max(pos))
        if neg:
            print("    worst overstatement  by the experimental table: %+.1f %%" % min(neg))
        print("    the F-C reference point, 44.1 -> 16 kHz, measured +18.2 %:")
        for key, d, qm, em in deltas:
            if key == (44100, 16000):
                print("      44.1 -> 16 k  as-built %.0f vs experimental %.0f MACs/s/ch = %+.1f %%"
                      % (qm, em, d))
        print()
        print("    a POSITIVE delta is front-end CPU the experimental table was not charging.")
        print("    It is a reason to read the qualification column for product decisions -- NOT")
        print("    a reason to change the cycles/MAC constant, which these rows do not measure.")
    print()
    print("  NO DEFAULT IS PROPOSED.  Both constants stay (owner, after F-2): 1.90 remains the")
    print("  tier default so margin cannot be bought with an optimistic constant, and 1.42 is")
    print("  reported beside it because charging composition overhead to a single-stage chain")
    print("  overstates it.  Re-deciding needs measurement, and Phase G touches no hardware.")

    if a.csv:
        with pathlib.Path(a.csv).open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(out[0].keys()))
            w.writeheader()
            w.writerows(out)
        print()
        print("  csv -> %s" % a.csv)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
