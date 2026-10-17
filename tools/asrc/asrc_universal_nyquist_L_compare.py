#!/usr/bin/env python3
"""Reduce the three collision-aware 45-pair runs to the four columns a production decision
actually needs (owner, 2026-09-15).

    L128            what the product planner picks at the shipping phase count
    L256 same-chain what rear phase resolution ALONE buys, front ends untouched (--hold-chains)
    L256 reselected how much the planner can SIMPLIFY the front end once L is 256
    CPU             the tier of the chosen chain, because a spectral win outside the budget is
                    not a product option

The distinction between the two L256 columns is the whole point.  "Same-chain" answers "can we
fix this without touching a front end that is already Hard-RT PASS on hardware?" -- which is the
question a shipping product asks.  "Reselected" answers "if we were designing from scratch at
L=256, how cheap could the front end get?" -- which is a cost question for later.  Where the
same-chain column already clears the internal margin inside the CPU budget, the reselected
column's heavier or different chain has no production value for that pair.

Usage:
  python tools/asrc/asrc_universal_nyquist_L_compare.py \
      --l128 <L128.csv> --l256-held <L256_heldchains.csv> --l256 <L256.csv> [--md <output-file>]
"""
from __future__ import annotations

import argparse
import csv
import io
import pathlib

TARGET_DB = -105.0
GUARANTEE_DB = -100.0


def load(p: pathlib.Path) -> dict:
    with io.open(p, encoding='utf-8', newline='') as fh:
        return {(int(r['fs_in_hz']), int(r['fs_out_hz'])): r for r in csv.DictReader(fh)}


def verdict(dbc: float) -> str:
    if dbc <= TARGET_DB:
        return 'PASS'
    return 'WATCH' if dbc <= GUARANTEE_DB else 'FAIL'


def cell(row) -> str:
    if row is None:
        return '-'
    return '%+.2f %s' % (float(row['overall_dbc']), verdict(float(row['overall_dbc'])))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--l128', type=pathlib.Path, required=True)
    ap.add_argument('--l256-held', type=pathlib.Path, required=True)
    ap.add_argument('--l256', type=pathlib.Path, required=True)
    ap.add_argument('--md', type=pathlib.Path, default=None)
    args = ap.parse_args()

    a, bh, b = load(args.l128), load(args.l256_held), load(args.l256)
    keys = sorted(a, key=lambda k: (-float(a[k]['overall_dbc'])))

    hdr = ("%15s | %-16s | %-16s | %-16s | %-11s | %s"
           % ("pair", "L128 selected", "L256 same-chain", "L256 reselected", "CPU (L128)",
              "chain held at L128"))
    print(hdr)
    print("-" * (len(hdr) + 24))
    solved_by_L_alone = []
    still_watch = []
    still_fail = []
    for k in keys:
        ra, rh, rb = a[k], bh.get(k), b.get(k)
        va = float(ra['overall_dbc'])
        vh = None if rh is None else float(rh['overall_dbc'])
        print("%7.4g->%-6.4g | %-16s | %-16s | %-16s | %-11s | %s"
              % (k[0] / 1000.0, k[1] / 1000.0, cell(ra), cell(rh), cell(rb),
                 ra['cpu_budget'], ra['chain'][:44]))
        if vh is None:
            continue
        if va > TARGET_DB and vh <= TARGET_DB and ra['cpu_budget'] == 'ok':
            solved_by_L_alone.append((k, va, vh))
        elif vh > TARGET_DB and vh <= GUARANTEE_DB:
            still_watch.append((k, vh))
        elif vh > GUARANTEE_DB:
            still_fail.append((k, vh))

    def count(d, field='overall_dbc'):
        n = {'PASS': 0, 'WATCH': 0, 'FAIL': 0}
        for r in d.values():
            n[verdict(float(r[field]))] += 1
        return n

    print()
    for label, d in (("L128 selected", a), ("L256 same-chain (held)", bh),
                     ("L256 reselected", b)):
        n = count(d)
        cpuok = sum(1 for r in d.values() if r['cpu_budget'] == 'ok')
        prodok = sum(1 for r in d.values()
                     if verdict(float(r['overall_dbc'])) != 'FAIL' and r['cpu_budget'] == 'ok')
        print("%-24s : %2d pairs   PASS %2d  MARGIN WATCH %2d  FAIL %2d   |  inside 16 ch budget "
              "%2d   product-viable (>= -100 dBc AND 16 ch) %2d"
              % (label, len(d), n['PASS'], n['WATCH'], n['FAIL'], cpuok, prodok))

    print()
    print("THE PRODUCTION QUESTION -- pairs fixed to the -105 dBc internal margin by rear phase")
    print("resolution ALONE, on the front end that is already chosen at L=128 (%d):"
          % len(solved_by_L_alone))
    for k, va, vh in solved_by_L_alone:
        print("   %7.4g -> %-7.4g  %+8.2f -> %+8.2f dBc  (%+.2f dB)  chain unchanged, CPU ok"
              % (k[0] / 1000.0, k[1] / 1000.0, va, vh, vh - va))
    if still_watch:
        print()
        print("still MARGIN WATCH at L256 on the held chain (%d):" % len(still_watch))
        for k, vh in still_watch:
            print("   %7.4g -> %-7.4g  %+8.2f dBc" % (k[0] / 1000.0, k[1] / 1000.0, vh))
    if still_fail:
        print()
        print("still FAIL at L256 on the held chain (%d):" % len(still_fail))
        for k, vh in still_fail:
            print("   %7.4g -> %-7.4g  %+8.2f dBc" % (k[0] / 1000.0, k[1] / 1000.0, vh))

    if args.md:
        L = ["| pair | L128 selected | L256 same-chain | L256 reselected | CPU (L128) | "
             "chain (L128) |", "|---|---:|---:|---:|---|---|"]
        for k in keys:
            ra, rh, rb = a[k], bh.get(k), b.get(k)
            L.append("| %.4g -> %.4g k | %s | %s | %s | %s | `%s` |"
                     % (k[0] / 1000.0, k[1] / 1000.0, cell(ra), cell(rh), cell(rb),
                        ra['cpu_budget'], ra['chain']))
        args.md.write_text("\n".join(L) + "\n", encoding='utf-8')
        print("\nmd   -> %s" % args.md)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
