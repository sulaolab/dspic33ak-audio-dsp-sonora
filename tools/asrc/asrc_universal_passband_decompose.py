#!/usr/bin/env python3
"""Why a pair misses the UNIVERSAL uniform passband, decomposed per stage.

Phase H SS3.  The 45-pair driver reports ONE number per pair -- max |deviation| over
[0, 0.8125*FN_out], DC-referenced, through the whole chain INCLUDING the rear M30
resampler.  A single number cannot say which stage spent it, and the redesign in SS4
depends entirely on that: a droop owned by a front-end stage whose DESIGN passband edge
sits below the protected edge is curable by re-specifying that stage band edges; a
droop owned by the rear prototype is not curable by any front-end change at all, because
the rear prototype is one shipped table shared by every rate.

So this tool takes the SAME chain objects the driver qualifies (asrc_universal_nyquist_model
.current_chain for a shipped row, asrc_asbuilt_sizing for a sized one) and evaluates the
through-path response stage by stage, reporting for each stage:

    design_pb_hz   the band edge the stage was DESIGNED at (generator constant, not re-derived)
    edge_deficit   fp - design_pb_hz.  Positive means the protected edge is OUTSIDE the
                   stage own passband, i.e. the stage was never asked to be flat there.
    droop_at_fp    that stage own dB deviation at fp, DC-referenced
    droop_max      that stage own max |deviation| over [0, fp]

and for the chain: the rear prototype contribution, and the total.  The per-stage dB
figures sum to the total at fp by construction (a cascade multiplies magnitudes), which
is the check printed at the end of each row.

CLASSIFICATION emitted per pair:

    LEGACY_EDGE      at least one front-end stage has edge_deficit > 0 and owns most of
                     the droop -- a per-rate specification predating the uniform one.
                     Curable: re-specify that stage passband edge, re-run the generator.
    REAR_PROTOTYPE   the rear M30 prototype owns most of the droop.  NOT curable per rate.
    STAGE_SHAPE      every front-end stage edge is already at or beyond fp, yet the
                     droop is still the front end -- the stage is flat-enough nowhere
                     near its edge, so it is a Kaiser-shape/taps question, not an edge one.
    RATIONAL_STAGE   the droop is owned by the L=2/M=3 polyphase prototype.

Usage:
    python tools/asrc/asrc_universal_passband_decompose.py                 # the failing rows
    python tools/asrc/asrc_universal_passband_decompose.py --all           # all 45 pairs
    python tools/asrc/asrc_universal_passband_decompose.py --only 48000:22050
    python tools/asrc/asrc_universal_passband_decompose.py --csv out.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import asrc_universal_nyquist_model as U          # noqa: E402
import asrc_asbuilt_sizing as A                   # noqa: E402

PB_LIMIT_DB = 0.10
NGRID = 4001

RATES = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)


def all_pairs():
    out = []
    for fi in RATES:
        for fo in RATES:
            if fo < fi:
                out.append((fi, fo))
    return out


def protected_edge(fs_out: int) -> float:
    return 0.8125 * fs_out / 2.0


def chain_for(fs_in: int, fs_out: int, fp: float):
    """The qualification chain: the shipped image if the route publishes one, else as-built sized."""
    try:
        ship, tag = U.current_chain(int(fs_in), int(fs_out), fp)
    except Exception:                                              # noqa: BLE001
        ship, tag = None, "direct"
    if ship is not None and tag != "direct" and ship.stages:
        return ship, "shipped"
    best = None
    for combo in ((), (2,), (3,), (2, 2), (2, 3), (3, 2), (2, 2, 2), (2, 2, 3), (2, 3, 2)):
        d = 1
        for c in combo:
            d *= c
        if fs_in / d < fs_out:
            continue
        ch, _per = A.size_cascade(float(fs_in), combo, float(fs_out))
        if ch is not None and (best is None or ch.macs_per_sec < best.macs_per_sec):
            best = ch
    if best is None:
        return U.Chain("direct", "direct", [], float(fs_in), float(fs_out)), "no-front-end"
    return best, "asbuilt-sized"


def decompose(fs_in: int, fs_out: int):
    fp = protected_edge(fs_out)
    chain, origin = chain_for(fs_in, fs_out, fp)
    pf = np.linspace(0.0, fp, NGRID)

    stages = []
    rate = float(fs_in)
    total_db = np.zeros_like(pf)
    for s in chain.stages:
        c = np.asarray(s.coeff, dtype=np.float64)
        if s.kind == "rat":
            proto_fs = rate * s.L
            m = U.mag_at(c, proto_fs, pf)
            rate = proto_fs / s.M
        else:
            m = U.mag_at(c, rate, pf)
            rate = rate / s.d
        d = U.db(m)
        d = d - d[0]
        total_db = total_db + d
        stages.append(dict(
            kind=s.kind, taps=s.taps, fs_in=s.fs_in, fs_out=s.fs_out,
            design_pb_hz=s.pb, design_sb_hz=s.sb,
            edge_deficit_hz=fp - s.pb,
            droop_at_fp_db=float(d[-1]),
            droop_max_db=float(np.max(np.abs(d))),
        ))

    rear = U.rear_prototype()
    rd = U.db(U.mag_at(rear, float(U.L_REAR) * rate, pf))
    rd = rd - rd[0]
    total_db = total_db + rd

    # front-end share vs rear share, measured at fp (where the deviation is largest here)
    front_at_fp = float(sum(s["droop_at_fp_db"] for s in stages))
    rear_at_fp = float(rd[-1])
    total_at_fp = float(total_db[-1])
    total_max = float(np.max(np.abs(total_db)))

    # who owns it
    owner = "NONE"
    if abs(total_max) > PB_LIMIT_DB:
        if abs(rear_at_fp) >= abs(front_at_fp):
            owner = "REAR_PROTOTYPE"
        else:
            worst = max(stages, key=lambda s: abs(s["droop_at_fp_db"]))
            if worst["kind"] == "rat":
                owner = "RATIONAL_STAGE"
            elif worst["edge_deficit_hz"] > 0.0:
                owner = "LEGACY_EDGE"
            else:
                owner = "STAGE_SHAPE"

    return dict(
        fs_in=fs_in, fs_out=fs_out, fp_hz=fp, origin=origin, chain=chain.label,
        stage_desc=" -> ".join("%s%g/%g N%d pb%.0f sb%.0f"
                               % (s["kind"], s["fs_in"] / 1000.0, s["fs_out"] / 1000.0,
                                  s["taps"], s["design_pb_hz"], s["design_sb_hz"])
                               for s in stages) or "(no front end)",
        rear_step=(rate / fs_out) if fs_out else 0.0,
        passband_err_db=total_max,
        passband_at_fp_db=total_at_fp,
        front_at_fp_db=front_at_fp,
        rear_at_fp_db=rear_at_fp,
        n_stages=len(stages),
        max_edge_deficit_hz=max([s["edge_deficit_hz"] for s in stages], default=0.0),
        owner=owner,
        verdict="PASS" if total_max <= PB_LIMIT_DB else "FAIL",
        stages=stages,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="every pair, not just the failing ones")
    ap.add_argument("--only", default=None, help="comma-separated fs_in:fs_out in Hz")
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    if args.only:
        pairs = []
        for tok in args.only.split(","):
            a, b = tok.split(":")
            pairs.append((int(a), int(b)))
    else:
        pairs = all_pairs()

    rows = [decompose(a, b) for a, b in pairs]
    if not args.only and not args.all:
        rows = [r for r in rows if r["verdict"] == "FAIL"]

    print("UNIVERSAL uniform passband: |deviation| <= %.2f dB over 0 .. 0.8125*FN_out\n"
          % PB_LIMIT_DB)
    hdr = ("%-7s %-7s %9s %-13s %8s %8s %8s %9s %-14s %s"
           % ("in", "out", "fp_Hz", "origin", "total", "front", "rear", "edge_def",
              "owner", "chain"))
    print(hdr)
    print("-" * 132)
    for r in rows:
        print("%-7d %-7d %9.1f %-13s %8.4f %8.4f %8.4f %9.1f %-14s %s"
              % (r["fs_in"], r["fs_out"], r["fp_hz"], r["origin"], r["passband_err_db"],
                 r["front_at_fp_db"], r["rear_at_fp_db"], r["max_edge_deficit_hz"],
                 r["owner"], r["chain"]))
    print()
    for r in rows:
        print("%d -> %d   fp=%.1f Hz  total=%+.4f dB (%s, owner %s)"
              % (r["fs_in"], r["fs_out"], r["fp_hz"], r["passband_at_fp_db"],
                 r["verdict"], r["owner"]))
        for s in r["stages"]:
            print("    %-4s %6.1fk -> %6.1fk  N%-4d  design pb %8.1f / sb %8.1f  "
                  "edge deficit %+8.1f Hz  droop@fp %+8.4f dB  max %7.4f dB"
                  % (s["kind"], s["fs_in"] / 1000.0, s["fs_out"] / 1000.0, s["taps"],
                     s["design_pb_hz"], s["design_sb_hz"], s["edge_deficit_hz"],
                     s["droop_at_fp_db"], s["droop_max_db"]))
        print("    rear M30 prototype at rear step %.5f          droop@fp %+8.4f dB"
              % (r["rear_step"], r["rear_at_fp_db"]))
        print("    check: front %+.4f + rear %+.4f = %+.4f  (total@fp %+.4f)"
              % (r["front_at_fp_db"], r["rear_at_fp_db"],
                 r["front_at_fp_db"] + r["rear_at_fp_db"], r["passband_at_fp_db"]))
        print()

    if args.csv:
        cols = ["fs_in", "fs_out", "fp_hz", "origin", "chain", "stage_desc", "rear_step",
                "n_stages", "passband_err_db", "passband_at_fp_db", "front_at_fp_db",
                "rear_at_fp_db", "max_edge_deficit_hz", "owner", "verdict"]
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            for r in rows:
                w.writerow(r)
        print("wrote %s (%d rows)" % (args.csv, len(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
