#!/usr/bin/env python3
"""Classify the 45 pairs from the planner's coverage CSV -- EASY / MODERATE / HARD / NO-GO.

Reads the machine-readable output of asrc_universal_nyquist_planner.py (--csv) so the
classification is derived from the numbers, never hand-transcribed.

  python tools/asrc/asrc_universal_nyquist_classify.py <coverage.csv> [--ratio 0.8125] [--pb 0.1]

RULES, stated so a verdict can be argued with:
  EASY      the chain the tree SHIPS today already meets the uniform spec -- no new filter at all.
  MODERATE  a chain meets it using only structures the tree already has (2:1 / 3:1 decimators,
            the L=2/M=3 polyphase) or one added decimate-by-1 FIR, AND its front-end load fits the
            profile budget.
  HARD      a chain meets the spec but its load exceeds the profile budget (up to 2x), i.e. the
            spectral answer exists and the CPU answer does not.
  NO-GO     no chain meets the spec, or the cheapest one costs more than 2x the budget.
"""
from __future__ import annotations

import argparse
import csv
import sys

BUDGET = {16: 40.0, 12: 45.0}      # see asrc_universal_nyquist_model.py, BUDGET_P*_PCT


def profile_for(fs_in: int):
    """Which product profile a pair is judged in: a 96 kHz leg A is the 12 ch stress profile."""
    return (12, "P2 12ch") if fs_in >= 96000 else (16, "P1 16ch")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--ratio", default="0.8125")
    ap.add_argument("--pb", default="0.1")
    a = ap.parse_args()
    key = f"r{a.ratio}_pb{a.pb}"
    pb_target = float(a.pb)

    rows = list(csv.DictReader(open(a.csv, encoding="ascii")))
    if key + "_verdict" not in rows[0]:
        print("ERROR: %s not in the CSV.  Columns present: %s"
              % (key, sorted({c.rsplit('_', 1)[0] for c in rows[0] if c.startswith('r0')})),
              file=sys.stderr)
        return 1

    out = []
    counts = {"EASY": 0, "MODERATE": 0, "HARD": 0, "NO-GO": 0}
    rom_bytes = 0
    structs = {}
    coeff_sets = set()

    for r in rows:
        fs_in, fs_out = int(r["fs_in"]), int(r["fs_out"])
        ch_n, pname = profile_for(fs_in)
        budget = BUDGET[ch_n]
        cur_alias = float(r["current_alias_db"])
        cur_pb = abs(float(r["current_pb_err_db"]))
        verdict = r[key + "_verdict"]

        if verdict != "PASS":
            cls = "NO-GO"
            reason = "no candidate chain meets the spec (see the planner's per-pair scan)"
            load = None
            chain = "-"
        else:
            chain = r[key + "_chain"]
            load = float(r[key + "_load_p1"] if ch_n == 16 else r[key + "_load_p2"])
            if cur_alias <= -100.0 and cur_pb <= pb_target:
                cls = "EASY"
                reason = ("the shipping chain already holds %.2f dB alias and %.3f dB passband"
                          % (cur_alias, cur_pb))
            elif load > 2.0 * budget:
                cls = "NO-GO"
                reason = ("cheapest valid chain needs %.1f %% of %s, over 2x the %.0f %% budget"
                          % (load, pname, budget))
            elif load > budget:
                cls = "HARD"
                reason = ("spectrally solved but %.1f %% of %s against a %.0f %% budget"
                          % (load, pname, budget))
            else:
                cls = "MODERATE"
                need = "new coefficient set(s) only" if "LPF@" not in chain \
                    else "one decimate-by-1 FIR stage"
                reason = "%s, %.1f %% of %s inside the %.0f %% budget" % (need, load, pname, budget)
            for part in chain.split(" -> "):
                if not part or part.startswith("("):
                    continue
                structs[part.split("@")[0]] = structs.get(part.split("@")[0], 0) + 1
                coeff_sets.add(part)
                rom_bytes += 4 * int(part.rsplit("N", 1)[1])

        counts[cls] += 1
        out.append((fs_in, fs_out, cls, chain, load, pname, reason))

    print("=" * 118)
    print("DIFFICULTY CLASSIFICATION at %s * FN, passband +/-%s dB, alias -100 dB"
          % (a.ratio, a.pb))
    print("=" * 118)
    print("%7s %7s %-9s %-30s %8s %-8s %s"
          % ("Fs_in", "Fs_out", "class", "cheapest valid chain", "load", "profile", "reason"))
    order = {"EASY": 0, "MODERATE": 1, "HARD": 2, "NO-GO": 3}
    for fs_in, fs_out, cls, chain, load, pname, reason in sorted(
            out, key=lambda x: (order[x[2]], -(x[4] or 0))):
        print("%7d %7d %-9s %-30s %7s %-8s %s"
              % (fs_in, fs_out, cls, chain[:30],
                 "-" if load is None else "%.1f%%" % load, pname, reason))
    print()
    print("TOTALS: EASY %d   MODERATE %d   HARD %d   NO-GO %d"
          % (counts["EASY"], counts["MODERATE"], counts["HARD"], counts["NO-GO"]))
    print()
    print("REUSE ACCOUNTING (brief section 18)")
    print("  stage structures instantiated:")
    for k in sorted(structs, key=lambda x: -structs[x]):
        have = ("EXISTS in the tree" if k in ("/2", "/3", "2/3")
                else "decim==1 CONFIGURATION of the existing generic stage (routing refuses it "
                     "today; no new kernel)")
        print("     %-8s %3d instances   %s" % (k, structs[k], have))
    print("  distinct coefficient tables required: %d" % len(coeff_sets))
    print("  Q31 flash cost of all of them:        %d bytes (%.1f KiB)"
          % (rom_bytes, rom_bytes / 1024.0))
    print("  (one table per (structure, rate, taps); ASRC_COEFF_STORAGE == FLASH on the shipping")
    print("   preset, and runtime FIR design is out of scope, so every table is baked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
