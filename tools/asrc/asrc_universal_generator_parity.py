#!/usr/bin/env python3
"""PLANNER / GENERATOR PARITY for the UNIVERSAL profile, as a machine check (Phase H SS5).

WHAT WENT WRONG BEFORE, AND WHAT THIS FORBIDS.  Phase F-2 found the planner's 45-pair table and
the firmware image sizing the same front-end stage by two different rules and then comparing the
two numbers as if they described one chain: the planner said 44.1 -> 16 kHz needed a 35-tap
pre-stage, the image shipped 51, and the gap was 9.45 dB of alias and 18.2 % of MACs.  Nothing in
the tooling noticed, because nothing re-derived the planner's own stage constants and asked whether
a generator could produce them.

The universal profile makes that failure MORE likely, not less: most of its rows are not in any
image, so there is no coefficient array to diff against.  What can still be checked is that the
planner's chain is FULLY SPECIFIED and REPRODUCIBLE -- that every stage names the rate it runs at,
its decimation factor, its band edges, its tap count, and WHICH RULE set its stopband, and that
re-deriving the stage from those constants reproduces the planner's own coefficients bit for bit.
A row that passes this check can be handed to the generator as a row; a row that fails it is a
number with no chain behind it, which is exactly what F-C was.

So, per pair, this tool
  1  re-builds the chosen chain from its stated origin using the SAME authority the driver used
     (asrc_universal_nyquist_model.current_chain for shipped, asrc_asbuilt_sizing for the two
     sized origins) -- not from the CSV's text,
  2  checks the rebuilt chain against the CSV's own `chain`, `taps` and `macs_per_sec` columns,
  3  prints every stage's (fs, d, N, Fp, Fstop) plus the partner constraint that set Fstop, and
  4  CRC32s the float32 coefficients and the Q31 words, so a later generator run can be compared
     against this table the way asrc_coeff_provenance.py compares the shipped sets.

Exit 0 only if every row reproduces.  Exit 1 names the rows that did not.

Usage:
    python tools/asrc/asrc_universal_generator_parity.py --in <capture-dir>/h6_universal_L256.csv
    python tools/asrc/asrc_universal_generator_parity.py --in <csv> --csv out.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import asrc_universal_nyquist_model as U          # noqa: E402
import asrc_asbuilt_sizing as A                   # noqa: E402
import asrc_universal_nyquist_requalify as R      # noqa: E402


def crc32_f32(coeff) -> int:
    return zlib.crc32(np.asarray(coeff, dtype="<f4").tobytes()) & 0xFFFFFFFF


def crc32_q31(coeff) -> int:
    q, _peak, _sa = U.q31_quantize(np.asarray(coeff, dtype=np.float64))
    words = np.round(q * (2.0 ** 31 - 1.0)).astype("<i4")
    return zlib.crc32(words.tobytes()) & 0xFFFFFFFF


def rebuild(fs_in: int, fs_out: int, origin: str, fp: float):
    """The chain the driver chose, rebuilt from its stated origin.  Never parsed from text."""
    if origin == "shipped":
        ch, tag = U.current_chain(fs_in, fs_out, fp)
        return ch, "shipped table (%s)" % tag
    if origin == "universal-respec":
        combo = R.shipped_combo(fs_in, fs_out, fp)
        if not combo:
            return None, "no shipped decimator structure to re-spec"
        ch, per = A.size_decimator_only(float(fs_in), combo, float(fs_out))
        return ch, ("size_decimator_only combo=%s" % (combo,)) if ch is not None \
            else "size_decimator_only refused: %s" % per[0][0]
    if origin == "no-front-end-needed":
        # THE PRECONDITION IS RE-DERIVED HERE, not trusted from the CSV.  This origin is the only
        # one with no coefficients to check, so the thing that has to reproduce is the INEQUALITY
        # that admitted it: the input Nyquist must be at or below the uniform fold edge, i.e. the
        # input cannot carry anything whose image lands in [0, Fp].  A row that reaches this
        # branch without satisfying it would be a free pass, so it fails parity instead.
        fold = fs_out - fp
        if fs_in * 0.5 > fold + 1e-9:
            return None, ("precondition FAILS: input Nyquist %.1f Hz is above the fold edge "
                          "%.1f Hz, so a front end IS required" % (fs_in * 0.5, fold))
        return (U.Chain("no front end (input Nyquist %.4g kHz <= fold edge %.4g kHz)"
                        % (fs_in / 2000.0, fold / 1000.0), "direct", [], float(fs_in),
                        float(fs_out)),
                "precondition re-derived: input Nyquist %.1f Hz <= Fs_out - Fp = %.1f Hz"
                % (fs_in * 0.5, fold))
    if origin == "asbuilt-sized":
        best = None
        for combo in R.ASBUILT_COMBOS:
            d = 1
            for c in combo:
                d *= c
            if fs_in / d < fs_out:
                continue
            ch, _per = A.size_cascade(float(fs_in), combo, float(fs_out))
            if ch is not None and (best is None or ch.macs_per_sec < best[0].macs_per_sec):
                best = (ch, "size_cascade combo=%s" % (combo,))
        return best if best else (None, "size_cascade produced nothing")
    return None, "origin %r is not a qualification origin" % origin


def partner_note(chain, i: int, fs_out: float, fp: float) -> str:
    """WHICH RULE set stage i's stopband -- the thing F-2 got wrong, said out loud per stage."""
    s = chain.stages[i]
    if s.kind == "rat":
        return "rational L=%d/M=%d prototype, stopband = the output Nyquist" % (s.L, s.M)
    if i == len(chain.stages) - 1:
        if s.d == 1:
            return ("final 1:1 stage: Fstop IS the uniform fold edge Fs_out - Fp = %.1f Hz"
                    % (fs_out - fp))
        return ("final decimating stage: partner is the REAR resampler, so Fstop = its own output "
                "rate %.1f - (Fs_out - Fp) %.1f = %.1f Hz"
                % (s.fs_out, fs_out - fp, s.fs_out - (fs_out - fp)))
    nxt = chain.stages[i + 1]
    return ("partner-aware: Fstop = own output rate %.1f - next stage Fstop %.1f = %.1f Hz"
            % (s.fs_out, nxt.sb, s.fs_out - nxt.sb))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="inp", required=True,
                    help="a 45-pair CSV written by asrc_universal_nyquist_requalify.py")
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    with open(args.inp, newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    if not rows:
        print("no rows in %s" % args.inp)
        return 1
    profile = rows[0].get("spec_profile", "?")
    mode = rows[0].get("planner_mode", "?")
    print("parity check of %s  (mode=%s, profile=%s, L=%s)"
          % (args.inp, mode, profile, rows[0].get("phases_L", "?")))
    print("a row PASSES only if its chain rebuilds from its stated origin and the rebuild's "
          "tap list and MAC rate\nmatch the CSV exactly -- i.e. the row is a chain a generator "
          "could be asked for, not a number.\n")

    out = []
    bad = []
    for r in rows:
        fs_in, fs_out = int(r["fs_in_hz"]), int(r["fs_out_hz"])
        fp = float(r["protected_edge_hz"])
        origin = r["chain_origin"]
        ch, how = rebuild(fs_in, fs_out, origin, fp)
        if ch is None:
            bad.append((fs_in, fs_out, "could not rebuild: %s" % how))
            print("%7d -> %-7d  %-16s  ** NOT REBUILT: %s" % (fs_in, fs_out, origin, how))
            continue
        taps_csv = r["taps"]
        taps_new = "|".join(str(t) for t in ch.taps_list)
        macs_csv = int(r["macs_per_sec"])
        macs_new = int(ch.macs_per_sec)
        desc_csv = r["chain"]
        desc_new = ch.describe()
        ok = (taps_csv == taps_new) and (macs_csv == macs_new) and (desc_csv == desc_new)
        if not ok:
            bad.append((fs_in, fs_out,
                        "chain %r vs %r / taps %s vs %s / MACs %d vs %d"
                        % (desc_csv, desc_new, taps_csv, taps_new, macs_csv, macs_new)))
        print("%7d -> %-7d  %-16s  %-34s taps %-12s MACs %9d  %s"
              % (fs_in, fs_out, origin, desc_new, taps_new, macs_new,
                 "PARITY" if ok else "** MISMATCH **"))
        print("%22s rebuilt via %s" % ("", how))
        for i, s in enumerate(ch.stages):
            out.append(dict(fs_in_hz=fs_in, fs_out_hz=fs_out, spec_profile=profile,
                            chain_origin=origin, stage_index=i, stage_kind=s.kind,
                            stage_fs_in_hz="%.4f" % s.fs_in, stage_fs_out_hz="%.4f" % s.fs_out,
                            decimation=s.d, rational_L=s.L, rational_M=s.M, taps=s.taps,
                            passband_hz="%.4f" % s.pb, stopband_hz="%.4f" % s.sb,
                            partner_constraint=partner_note(ch, i, float(fs_out), fp),
                            coeff_crc32_f32="%08X" % crc32_f32(s.coeff),
                            coeff_crc32_q31="%08X" % crc32_q31(s.coeff),
                            parity="yes" if ok else "NO"))
            print("%22s  stage %d  %-4s %9.1f Hz /%d  N%-4d  Fp %9.1f  Fstop %9.1f  "
                  "crc32 f32 %08X  q31 %08X"
                  % ("", i, s.kind, s.fs_in, s.d, s.taps, s.pb, s.sb,
                     crc32_f32(s.coeff), crc32_q31(s.coeff)))
            print("%22s           %s" % ("", partner_note(ch, i, float(fs_out), fp)))
        if not ch.stages:
            print("%22s  (no front-end stage: the rear resampler does the whole ratio)" % "")

    print()
    print("rows checked %d   PARITY %d   MISMATCH/NOT-REBUILT %d"
          % (len(rows), len(rows) - len(bad), len(bad)))
    for fs_in, fs_out, why in bad:
        print("    %7d -> %-7d %s" % (fs_in, fs_out, why))
    if not bad:
        print("PARITY CLOSED for this profile: every chosen chain is a fully specified, "
              "reproducible generator row.")

    if args.csv:
        cols = ["fs_in_hz", "fs_out_hz", "spec_profile", "chain_origin", "stage_index",
                "stage_kind", "stage_fs_in_hz", "stage_fs_out_hz", "decimation", "rational_L",
                "rational_M", "taps", "passband_hz", "stopband_hz", "partner_constraint",
                "coeff_crc32_f32", "coeff_crc32_q31", "parity"]
        with open(args.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=cols)
            w.writeheader()
            w.writerows(out)
        print("wrote %s (%d stage rows)" % (args.csv, len(out)))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
