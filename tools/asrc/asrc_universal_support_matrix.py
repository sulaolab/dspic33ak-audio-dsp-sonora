#!/usr/bin/env python3
"""THE 45-PAIR SUPPORT MATRIX, on separate axes (Phase H SS10) -- and the SS14 deliverable CSVs.

THE ONE RULE THIS FILE EXISTS TO ENFORCE: do not collapse the axes into a single QUALIFIED
boolean.  A pair can be spectrally perfect and not fit the CPU model; a pair can fit easily and
miss the uniform passband; a pair can satisfy every host axis and still have no hardware behind
it.  Merging those produces a column that is wrong in three different directions at once, and the
migration decision needs to know WHICH.  So six axes, always separate:

    UNIFORM_PASSBAND    |deviation| <= 0.10 dB over 0 .. 0.8125 x FN_out          host, computed
    ALIAS_PASS          worst unwanted component <= -100 dBc                       host, simulated
    CPU_MODEL_STATUS    the front-end tier, at BOTH constants, named as a MODEL    host, computed
                        status (16ch-model-pass / 12ch-model-only / model-over)
    POLICY_ALLOWED      does the owner's profile policy allow this row at all      owner decision
    HW_RT_STATUS        has hardware run this chain at rate, Hard-RT               measurement
    HW_SPECTRAL_STATUS  has hardware measured this chain's spectrum                measurement

POLICY_ALLOWED is the axis that is NOT a measurement and NOT a computation -- it is the owner's,
and Phase H's owner decisions put exactly two kinds of row outside it: 96 -> 48 kHz and
96 -> 44.1 kHz are "CPU unresolved / unsupported candidate" in the standard universal profile
(explicitly NOT declared 12ch-only, and explicitly not removed), and every row on the two held
L128 profiles keeps its legacy coefficients.  Everything else the owner declared in scope.

THE HARDWARE AXES ARE ALMOST ENTIRELY 'NOT MEASURED' AND THAT IS THE POINT.  Phase H is a HOST
phase: no board was touched, nothing was flashed, no lease was taken.  A row whose universal chain
is not the shipped chain has no hardware evidence by construction, and writing anything else in
those columns would manufacture it.

Usage:
  python tools/asrc/asrc_universal_support_matrix.py \\
      --legacy <capture-dir>/h6_legacy_L256.csv \\
      --universal <capture-dir>/h6_universal_L256.csv \\
      --decompose <capture-dir>/h3_decompose.csv \\
      --outdir <capture-dir>
"""
from __future__ import annotations

import argparse
import csv
import pathlib

# Owner decisions, Phase H SS0.  Named here so the policy axis cites a decision, not a threshold.
POLICY_UNRESOLVED = {
    (96000, 48000): "CPU unresolved: standard universal candidate, NOT declared 12ch-only, "
                    "NOT removed (owner SS0)",
    (96000, 44100): "CPU unresolved: standard universal candidate, NOT declared 12ch-only, "
                    "NOT removed (owner SS0)",
}


def load(p):
    with pathlib.Path(p).open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--legacy", required=True)
    ap.add_argument("--universal", required=True)
    ap.add_argument("--decompose", required=True)
    ap.add_argument("--outdir", required=True)
    a = ap.parse_args()

    out = pathlib.Path(a.outdir)
    leg = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.legacy)}
    uni = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.universal)}
    dec = {(int(r["fs_in"]), int(r["fs_out"])): r for r in load(a.decompose)}
    keys = sorted(set(leg) | set(uni))

    # ---------------------------------------------------------------- the matrix
    matrix = []
    for k in keys:
        lr, ur = leg[k], uni[k]
        policy = POLICY_UNRESOLVED.get(k, "in scope (owner SS0: uniform spec on every supported "
                                          "row)")
        hw_rt = ur["hw_evidence"] if "hw_evidence" in ur else ""
        matrix.append(dict(
            fs_in_hz=k[0], fs_out_hz=k[1],
            UNIFORM_PASSBAND=ur["uniform_passband_pass"],
            ALIAS_PASS=ur["alias_pass"],
            CPU_MODEL_STATUS_1p90=ur["cpu_tier_1p90"],
            CPU_MODEL_STATUS_1p42=ur["cpu_tier_1p42"],
            POLICY_ALLOWED=policy,
            HW_RT_STATUS=("same chain as shipped -- see the legacy column"
                          if ur["chain_origin"] == "shipped" else "NOT MEASURED (host-only phase)"),
            HW_SPECTRAL_STATUS=("measured" if k == (44100, 16000)
                                else "NOT MEASURED (host-only phase)"),
            universal_chain=ur["chain"], universal_origin=ur["chain_origin"],
            universal_migration=ur["universal_migration"],
            universal_passband_err_db=ur["passband_err_db"],
            universal_overall_dbc=ur["overall_dbc"], universal_verdict=ur["verdict"],
            universal_cpu_pct_16ch_1p90=ur["cpu_pct_16ch_1p90"],
            universal_cpu_pct_16ch_1p42=ur["cpu_pct_16ch_1p42"],
            legacy_chain=lr["chain"], legacy_passband_err_db=lr["passband_err_db"],
            legacy_overall_dbc=lr["overall_dbc"], legacy_verdict=lr["verdict"],
            legacy_uniform_passband_pass=lr["uniform_passband_pass"],
            legacy_cpu_tier_1p90=lr["cpu_tier_1p90"],
            phases_L=ur["phases_L"],
        ))

    hdr = ("%-7s %-7s %-9s %-6s %-16s %-16s %-9s %s"
           % ("in", "out", "UNIF_PB", "ALIAS", "CPU@1.90", "CPU@1.42", "POLICY", "universal chain"))
    print("THE 45-PAIR UNIVERSAL SUPPORT MATRIX (L = %s).  Six axes, never merged.\n"
          % matrix[0]["phases_L"])
    print(hdr)
    print("-" * len(hdr))
    for m in matrix:
        print("%-7d %-7d %-9s %-6s %-16s %-16s %-9s %s"
              % (m["fs_in_hz"], m["fs_out_hz"], m["UNIFORM_PASSBAND"], m["ALIAS_PASS"],
                 m["CPU_MODEL_STATUS_1p90"], m["CPU_MODEL_STATUS_1p42"],
                 "UNRESOLV" if m["fs_in_hz"] == 96000
                 and (m["fs_in_hz"], m["fs_out_hz"]) in POLICY_UNRESOLVED else "in scope",
                 m["universal_chain"]))

    def w(name, rows, cols=None):
        p = out / name
        cols = cols or list(rows[0].keys())
        with p.open("w", newline="", encoding="utf-8") as fh:
            ww = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
            ww.writeheader()
            ww.writerows(rows)
        print("wrote %s (%d rows)" % (p, len(rows)))

    print()
    w("universal_45pair_authority.csv", matrix)

    # ------------------------------------------------- legacy vs universal, one row per pair
    lvu = [dict(fs_in_hz=m["fs_in_hz"], fs_out_hz=m["fs_out_hz"],
                legacy_chain=m["legacy_chain"],
                legacy_passband_err_db=m["legacy_passband_err_db"],
                legacy_uniform_passband_pass=m["legacy_uniform_passband_pass"],
                legacy_overall_dbc=m["legacy_overall_dbc"],
                legacy_verdict=m["legacy_verdict"],
                legacy_cpu_tier_1p90=m["legacy_cpu_tier_1p90"],
                universal_chain=m["universal_chain"],
                universal_origin=m["universal_origin"],
                universal_migration=m["universal_migration"],
                universal_passband_err_db=m["universal_passband_err_db"],
                universal_overall_dbc=m["universal_overall_dbc"],
                universal_verdict=m["universal_verdict"],
                universal_cpu_tier_1p90=m["CPU_MODEL_STATUS_1p90"],
                universal_cpu_tier_1p42=m["CPU_MODEL_STATUS_1p42"],
                same_chain_both_profiles=("yes" if m["legacy_chain"] == m["universal_chain"]
                                         else "no"))
           for m in matrix]
    w("legacy_vs_universal_rows.csv", lvu)

    # --------------------------------------- the migrated rows, with the SS3 root cause attached
    red = []
    for m in matrix:
        if m["universal_migration"] != "yes":
            continue
        d = dec.get((m["fs_in_hz"], m["fs_out_hz"]), {})
        red.append(dict(
            fs_in_hz=m["fs_in_hz"], fs_out_hz=m["fs_out_hz"],
            legacy_chain=m["legacy_chain"],
            legacy_passband_err_db=m["legacy_passband_err_db"],
            root_cause_owner=d.get("owner", ""),
            front_end_share_db=d.get("front_at_fp_db", ""),
            rear_prototype_share_db=d.get("rear_at_fp_db", ""),
            legacy_rear_step=d.get("rear_step", ""),
            max_design_edge_deficit_hz=d.get("max_edge_deficit_hz", ""),
            universal_chain=m["universal_chain"],
            universal_origin=m["universal_origin"],
            universal_passband_err_db=m["universal_passband_err_db"],
            universal_overall_dbc=m["universal_overall_dbc"],
            universal_verdict=m["universal_verdict"],
            universal_cpu_pct_16ch_1p90=m["universal_cpu_pct_16ch_1p90"],
            universal_cpu_pct_16ch_1p42=m["universal_cpu_pct_16ch_1p42"],
            universal_cpu_tier_1p90=m["CPU_MODEL_STATUS_1p90"],
            universal_cpu_tier_1p42=m["CPU_MODEL_STATUS_1p42"],
        ))
    w("uniform_passband_redesign_15rows.csv", red)

    print()
    print("counts on each axis, separately:")
    for axis, key in (("UNIFORM_PASSBAND", "UNIFORM_PASSBAND"), ("ALIAS_PASS", "ALIAS_PASS")):
        n = sum(1 for m in matrix if m[key] == "yes")
        print("    %-20s pass %d / %d" % (axis, n, len(matrix)))
    for c in ("1p90", "1p42"):
        cc = {}
        for m in matrix:
            cc[m["CPU_MODEL_STATUS_" + c]] = cc.get(m["CPU_MODEL_STATUS_" + c], 0) + 1
        print("    CPU_MODEL_STATUS @%s  %s"
              % (c.replace("p", "."), "   ".join("%s %d" % kv for kv in cc.items())))
    print("    POLICY_ALLOWED       in scope %d / %d, CPU-unresolved %d"
          % (len(matrix) - len(POLICY_UNRESOLVED), len(matrix), len(POLICY_UNRESOLVED)))
    print("    HW_RT_STATUS         measured 0 / %d -- Phase H touched no hardware" % len(matrix))
    print("    HW_SPECTRAL_STATUS   measured 1 / %d (44.1 -> 16 kHz only)" % len(matrix))
    print()
    print("migrated rows: %d.  A migrated row's hardware columns are NOT MEASURED by "
          "construction --\nit is not the chain any image runs." % len(red))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
