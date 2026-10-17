#!/usr/bin/env python3
"""PROFILE ISOLATION: what the universal coefficient sets cost, and who pays (Phase H SS9).

THE QUESTION SS9 ASKS is not "how big are the new tables" but "does adding them move a profile
that was not supposed to change".  Three profiles are in play and the owner fixed all three:

    standard AK512 Universal   rear L = 256
    96K_12CH_32K  (the stress) rear L = 128, unchanged
    AK128                      rear L = 128, unchanged

So isolation has two independent parts, and they fail for different reasons:

  1  REAR TABLE.  The rear resampler's phase table is L x M words.  L = 256 doubles it against
     L = 128.  This is charged to the STANDARD profile only if the stress and AK128 profiles keep
     their own L -- i.e. if L is a per-profile build constant and not one global.  This tool
     prints the three sizes so the doubling is visible as a number rather than assumed away.
  2  FRONT-END TABLES.  Each universal row that MIGRATES needs a coefficient array the image does
     not have today.  A profile that does not select a migrated row must not link its table.  The
     ROM figure below is therefore reported twice: the total of all new tables, and the subset a
     given profile actually selects.  A build that links the total is a linker finding
     (gc-sections / who-keeps-it-alive), not a specification one.

WHAT THIS TOOL DOES NOT CLAIM.  It computes table sizes from tap counts and word widths, which is
arithmetic on the authority CSVs.  It does NOT read a .map file and does not claim a measured
image size: no build is run in Phase H, and quoting a computed size as a measured one is the kind
of substitution this study has had to withdraw before.

Usage:
  python tools/asrc/asrc_universal_profile_isolation.py \\
      --legacy <capture-dir>/h6_legacy_L256.csv \\
      --universal <capture-dir>/h6_universal_L256.csv [--csv out.csv]
"""
from __future__ import annotations

import argparse
import csv
import pathlib

Q31_BYTES = 4
REAR_TAPS = 30                  # M, the rear polyphase row length

# The rates each profile actually selects.  Read as: which OUTPUT rates does this build serve.
PROFILES = {
    "standard AK512 Universal": dict(L=256, note="the profile this migration is for"),
    "96K_12CH_32K stress": dict(L=128, note="owner: L128 held"),
    "AK128": dict(L=128, note="owner: L128 held"),
}


def load(path):
    with pathlib.Path(path).open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def taps_of(row):
    return [int(t) for t in row["taps"].split("|") if t]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--legacy", required=True)
    ap.add_argument("--universal", required=True)
    ap.add_argument("--csv", default=None)
    a = ap.parse_args()

    leg = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.legacy)}
    uni = {(int(r["fs_in_hz"]), int(r["fs_out_hz"])): r for r in load(a.universal)}

    print("1  REAR PHASE TABLE, per profile (L x M x %d bytes, M = %d)" % (Q31_BYTES, REAR_TAPS))
    for name, p in PROFILES.items():
        b = p["L"] * REAR_TAPS * Q31_BYTES
        print("     %-26s L=%-4d  %7d bytes (%6.1f KiB)   %s"
              % (name, p["L"], b, b / 1024.0, p["note"]))
    d = (256 - 128) * REAR_TAPS * Q31_BYTES
    print("     -> the L256 profile pays %d bytes (%.1f KiB) more than an L128 one.  The stress "
          "and AK128\n        profiles pay it ONLY if L is a single global constant instead of a "
          "per-profile one." % (d, d / 1024.0))

    print()
    print("2  FRONT-END TABLES.  New arrays the universal profile needs that the image has not got")
    rows = []
    new_bytes = 0
    for k in sorted(uni):
        ur = uni[k]
        if ur.get("universal_migration") != "yes":
            continue
        t = taps_of(ur)
        b = sum(t) * Q31_BYTES
        new_bytes += b
        lr = leg.get(k)
        lb = sum(taps_of(lr)) * Q31_BYTES if lr else 0
        rows.append(dict(fs_in_hz=k[0], fs_out_hz=k[1],
                         legacy_chain=(lr["chain"] if lr else ""),
                         legacy_taps=(lr["taps"] if lr else ""),
                         legacy_rom_bytes=lb,
                         universal_chain=ur["chain"], universal_taps=ur["taps"],
                         universal_rom_bytes=b, rom_delta_bytes=b - lb))
        print("     %6.4g -> %-7.4g  legacy %-24s %5d B   universal %-26s %5d B   %+6d B"
              % (k[0] / 1000.0, k[1] / 1000.0, (lr["taps"] if lr else "-"), lb,
                 ur["taps"], b, b - lb))
    print("     -> %d migrated rows, %d bytes (%.1f KiB) of universal front-end tables in total."
          % (len(rows), new_bytes, new_bytes / 1024.0))
    print("        A profile that does not select a migrated row must not link its table; if a "
          "build does,\n        that is a linker finding (which symbol keeps it alive), not a "
          "specification one.")

    print()
    print("3  SHARED ARRAYS -- the isolation risk that is NOT about size.")
    print("     The shipped table reuses ONE array across several routes (asrc_coeff_provenance:")
    print("     `96to48:shared` serves every composed 96 kHz row, and `48to24` serves both 48->24")
    print("     and the composed 96->24).  Re-specifying such an array in place would move every")
    print("     route that shares it, including rows on the held L128 profiles.  So a migrated row")
    print("     must get a NEW array, never an edit of a shared one -- which is the same owner")
    print("     decision as 'do not silently rewrite an existing profile', restated as a linker")
    print("     constraint.")
    shared = [k for k in sorted(uni)
              if uni[k].get("universal_migration") == "yes" and k[0] == 96000]
    print("     migrated rows whose LEGACY chain runs behind the shared 96->48 pre-stage: %s"
          % (", ".join("%.4g->%.4g k" % (x[0] / 1000.0, x[1] / 1000.0) for x in shared) or "none"))

    if a.csv and rows:
        with pathlib.Path(a.csv).open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print("\nwrote %s (%d rows)" % (a.csv, len(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
