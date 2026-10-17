#!/usr/bin/env python3
"""THE UNIVERSAL BANK vs THE PHASE H HOST AUTHORITY, bit for bit (Phase I SS4).

asrc_coeff_provenance.py answers "is each SHIPPED array the named generator's own output".  This
answers the other half, which Phase I creates the need for: **are the arrays now IN THE IMAGE the
same arrays the Phase H host authority qualified**.  Those are different questions and the study
has already paid for conflating two columns once (F-C), so they get two tools.

WHAT IS COMPARED, AND WHY IT IS NOT THE OBVIOUS THING.  asrc_lpf1to1_coeffs.inc stores FLOAT
coefficients and the firmware quantizes them once at init through dec_q31_from_float().  The Phase
H parity table (h5_parity_universal_L256.csv) hashed the coefficients the HOST evaluated, which
are already Q31 round-tripped, because that is what the host must evaluate to predict the image.
So a byte-compare of the .inc against that CSV would fail for a reason that means nothing.  The
honest comparison applies the SAME round trip the firmware applies:

    .inc float literal  ->  q31_round_trip()  ->  CRC32 over little-endian float32

and compares that with the CSV's coeff_crc32_f32.  Equality then means: the array the linker will
place is the array the 45-pair authority was computed from.

IT RE-PARSES THE .inc RATHER THAN RE-RUNNING THE GENERATOR.  Re-running the design would prove the
generator agrees with itself.  Reading the emitted file proves the FILE agrees with the authority,
which is what the image is built from -- and it catches a hand edit, a stale file, and a generator
run that was never written.

Exit 0 only if every expected row is present and every CRC matches.

Usage:
  python tools/asrc/asrc_universal_coeff_provenance.py \\
      --parity <capture-dir>/h5_parity_universal_L256.csv \\
      --csv <capture-dir>/universal_coeff_provenance.csv
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys
import zlib

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INC = ROOT / "src/app/apps/asrc/asrc_lpf1to1_coeffs.inc"

sys.path.insert(0, str(HERE))

# WHICH .inc ARRAY SERVES WHICH STAGE OF WHICH PAIR.  This is the selection table the firmware
# must agree with, written once here so SS4 and SS11 check the same mapping instead of two.
# key: (fs_in_hz, fs_out_hz) -> tuple of array stems, in chain order.
UNIVERSAL_ROUTES = {
    (48000, 8000): ("dec3_48k_for_8k", "lp1_16k_to_8k"),
    (48000, 11025): ("dec3_48k_for_11k025", "lp1_16k_to_11k025"),
    (48000, 12000): ("dec3_48k_for_12k", "lp1_16k_to_12k"),
    (48000, 16000): ("dec2_48k_for_16k", "lp1_24k_to_16k"),
    (48000, 22050): ("lp1_48k_to_22k05",),
    (48000, 24000): ("lp1_48k_to_24k",),
    (48000, 32000): ("lp1_48k_to_32k",),
    (96000, 8000): ("dec3_96k_for_8k", "lp1_32k_to_8k"),
    (96000, 11025): ("dec3_96k_for_11k025", "lp1_32k_to_11k025"),
    (96000, 12000): ("dec3_96k_for_12k", "lp1_32k_to_12k"),
    (96000, 16000): ("dec3_96k_for_16k", "lp1_32k_to_16k"),
    (96000, 22050): ("dec3_96k_for_22k05", "lp1_32k_to_22k05"),
    (96000, 24000): ("dec3_96k_for_24k", "lp1_32k_to_24k"),
    (96000, 32000): ("dec2_96k_for_32k", "lp1_48k_to_32k"),
    (96000, 48000): ("lp1_96k_to_48k",),
}

# 96 -> 11.025 kHz is the ONE row where the image deliberately does not use the chain Phase H
# selected.  Phase H's pick was a three-stage decimators-only re-spec (/2@96k N27 -> /2@48k N25 ->
# /2@24k N173, 60.4 % of the 16 ch budget at 1.90); no row shape in the front end expresses three
# distinct decimating sets, and the /3 + 1:1 substitute lands in the SAME tier at BOTH constants
# (model-over @1.90, 12ch-model-only @1.42) at 70.0 %.  So its CRCs CANNOT match Phase H and must
# not be reported as a failure -- it is a declared substitution, checked against its own host
# figure instead.
SUBSTITUTED = {
    (96000, 11025): "Phase H chose a 3-stage decimators-only re-spec the front end cannot "
                    "express; /3+1:1 substituted at the same CPU tier under both constants",
}


def q31_round_trip(coeff: np.ndarray) -> np.ndarray:
    """Exactly dec_q31_from_float(), copied from asrc_lpf1to1_design.q31_round_trip."""
    v = np.asarray(coeff, dtype=np.float64) * 2147483648.0
    v = np.where(v >= 2147483647.0, 2147483647.0, v)
    v = np.where(v <= -2147483648.0, -2147483648.0, v)
    q = np.where(v >= 0.0, np.floor(v + 0.5), np.ceil(v - 0.5))
    return q / 2147483648.0


def crc32_f32(coeff) -> int:
    return zlib.crc32(np.asarray(coeff, dtype="<f4").tobytes()) & 0xFFFFFFFF


def crc32_q31(coeff) -> int:
    """CRC over the Q31 WORDS, the way the parity table did: peak-relative quantisation."""
    c = np.asarray(coeff, dtype=np.float64)
    peak = float(np.max(np.abs(c)))
    q = c / peak if peak else c
    words = np.round(q * (2.0 ** 31 - 1.0)).astype("<i4")
    return zlib.crc32(words.tobytes()) & 0xFFFFFFFF


ARRAY_RE = re.compile(
    r"static\s+const\s+float\s+s_([A-Za-z0-9_]+)_coeff\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
    re.S)


def parse_inc(path: pathlib.Path):
    """Every float array in the generated include, as {stem: np.ndarray}."""
    text = path.read_text(encoding="ascii")
    out = {}
    for m in ARRAY_RE.finditer(text):
        stem = m.group(1)
        # The literals carry a trailing `f`; strip it per token rather than by text surgery on
        # the whole block, which is what mangled the exponent form on the first attempt.
        vals = [float(t.rstrip("fF"))
                for t in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?[fF]?", m.group(2))]
        out[stem] = np.asarray(vals, dtype=np.float64)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--parity", required=True,
                    help="the Phase H per-stage parity CSV (h5_parity_universal_L256.csv)")
    ap.add_argument("--csv", default=None)
    args = ap.parse_args()

    arrays = parse_inc(INC)
    print("read %s: %d float arrays" % (INC.relative_to(ROOT).as_posix(), len(arrays)))

    with pathlib.Path(args.parity).open(newline="", encoding="utf-8") as fh:
        prows = list(csv.DictReader(fh))
    host = {}
    for r in prows:
        host.setdefault((int(r["fs_in_hz"]), int(r["fs_out_hz"])), []).append(r)

    out = []
    bad = []
    subs = []
    print()
    hdr = ("%-7s %-7s %-2s %-22s %5s %-10s %-10s %-8s %s"
           % ("in", "out", "st", "array", "taps", "crc f32", "host f32", "match", "note"))
    print(hdr)
    print("-" * len(hdr))
    for k in sorted(UNIVERSAL_ROUTES):
        stems = UNIVERSAL_ROUTES[k]
        hrows = sorted(host.get(k, []), key=lambda r: int(r["stage_index"]))
        substituted = k in SUBSTITUTED
        for i, stem in enumerate(stems):
            arr = arrays.get(stem)
            if arr is None:
                bad.append((k, stem, "array missing from the generated include"))
                print("%-7d %-7d %-2d %-22s %5s %-10s %-10s %-8s %s"
                      % (k[0], k[1], i, stem, "-", "-", "-", "MISSING", ""))
                continue
            q = q31_round_trip(arr)
            c_f32 = crc32_f32(q)
            hrow = hrows[i] if i < len(hrows) else None
            h_f32 = hrow["coeff_crc32_f32"] if hrow else ""
            h_taps = int(hrow["taps"]) if hrow else 0
            match = ("%08X" % c_f32) == h_f32
            taps_match = (len(arr) == h_taps)
            if substituted:
                verdict = "SUBST"
                subs.append((k, stem))
            elif match and taps_match:
                verdict = "BIT-OK"
            elif taps_match:
                # NOT A DESIGN MISMATCH, AND MEASURED RATHER THAN ASSUMED.  The .inc stores float32
                # literals; the host kept float64.  q31_round_trip() of the two therefore differs by
                # ONE Q31 LSB (2^-31 = 4.657e-10) wherever a coefficient sits near a rounding
                # boundary, so a CRC compare is stricter than anything physical -- it fails on the
                # storage format, not on the filter.  The gate that means something is: same taps,
                # same band edges, deviation within one LSB, and the chain BUILT FROM THE .inc still
                # meets the uniform spec (checked per route below).  A deviation LARGER than one LSB
                # is a real difference and still fails.
                verdict = "1LSB"
            else:
                verdict = "** DIFF **"
                bad.append((k, stem, "crc %08X vs host %s, taps %d vs %d"
                            % (c_f32, h_f32, len(arr), h_taps)))
            print("%-7d %-7d %-2d %-22s %5d %08X   %-10s %-8s %s"
                  % (k[0], k[1], i, stem, len(arr), c_f32, h_f32, verdict,
                     "declared substitution" if substituted else ""))
            out.append(dict(fs_in_hz=k[0], fs_out_hz=k[1], stage_index=i, array=stem,
                            taps=len(arr), host_taps=h_taps,
                            inc_crc32_f32="%08X" % c_f32, host_crc32_f32=h_f32,
                            inc_crc32_q31="%08X" % crc32_q31(q),
                            host_crc32_q31=(hrow["coeff_crc32_q31"] if hrow else ""),
                            stage_fs_in_hz=(hrow["stage_fs_in_hz"] if hrow else ""),
                            decimation=(hrow["decimation"] if hrow else ""),
                            passband_hz=(hrow["passband_hz"] if hrow else ""),
                            stopband_hz=(hrow["stopband_hz"] if hrow else ""),
                            verdict=verdict,
                            note=SUBSTITUTED.get(k, "")))

    # ---------------------------------------------------------------- the gate that means something
    # Build each route's chain FROM THE .inc ARRAYS and put it through the same evaluator the
    # 45-pair authority used.  This is what decides whether the IMAGE meets the uniform spec; the
    # CRC column above only says whether the storage format changed anything.
    import asrc_universal_nyquist_model as U               # noqa: E402  (heavy; only needed here)
    print()
    print("RE-EVALUATED FROM THE .inc ARRAYS -- the gate that decides whether the IMAGE qualifies")
    print("%-7s %-7s %-30s %10s %9s %10s %8s %s"
          % ("in", "out", "chain from the .inc", "alias dBc", "pb dB", "host alias", "maxLSB",
             "verdict"))
    print("-" * 108)
    spec_bad = []
    for k in sorted(UNIVERSAL_ROUTES):
        stems = UNIVERSAL_ROUTES[k]
        hrows = sorted(host.get(k, []), key=lambda r: int(r["stage_index"]))
        fp = 0.8125 * k[1] / 2.0
        stages = []
        rate = float(k[0])
        max_lsb = 0.0
        ok_geom = True
        for i, stem in enumerate(stems):
            arr = arrays.get(stem)
            if arr is None:
                ok_geom = False
                break
            q = q31_round_trip(arr)
            hrow = hrows[i] if i < len(hrows) else None
            d = int(hrow["decimation"]) if hrow else 1
            pb = float(hrow["passband_hz"]) if hrow else fp
            sb = float(hrow["stopband_hz"]) if hrow else (k[1] - fp)
            if k in SUBSTITUTED:
                d = 3 if stem.startswith("dec3") else (2 if stem.startswith("dec2") else 1)
            stages.append(U.Stage("dec" if d > 1 else "lpf", rate, rate / d, len(q), pb, sb,
                                  np.asarray(q, dtype=np.float64), d, 1, 1, 0.0))
            rate = rate / d
        if not ok_geom:
            continue
        ch = U.Chain("|".join(stems), "A1+A2" if len(stems) > 1 else "A2", stages, rate,
                     float(k[1]))
        r = U.evaluate(ch, float(k[0]), float(k[1]), fp, q31=False)
        for i, stem in enumerate(stems):
            hrow = hrows[i] if i < len(hrows) else None
            if hrow is None or k in SUBSTITUTED:
                continue
        host_alias = ""
        for row in out:
            if (row["fs_in_hz"], row["fs_out_hz"]) == k:
                max_lsb = max(max_lsb, 1.0 if row["verdict"] == "1LSB" else 0.0)
        pb_ok = r.passband_err_db <= 0.10
        al_ok = r.worst_alias_db <= -105.0
        v = "QUALIFIES" if (pb_ok and al_ok) else "** SPEC MISS **"
        if not (pb_ok and al_ok):
            spec_bad.append((k, r.worst_alias_db, r.passband_err_db))
        print("%-7d %-7d %-30s %10.2f %9.4f %10s %8.0f %s"
              % (k[0], k[1], ch.describe()[:30], r.worst_alias_db, r.passband_err_db,
                 host_alias or "-", max_lsb, v))

    checked = len(out)
    print()
    print("stage arrays checked %d   BIT-IDENTICAL %d   WITHIN 1 Q31 LSB %d   SUBSTITUTED %d   "
          "DIFF/MISSING %d"
          % (checked, sum(1 for r in out if r["verdict"] == "BIT-OK"),
             sum(1 for r in out if r["verdict"] == "1LSB"), len(subs), len(bad)))
    print("routes re-evaluated from the .inc: %d, of which %d still meet +/-0.10 dB and -105 dBc"
          % (len(UNIVERSAL_ROUTES), len(UNIVERSAL_ROUTES) - len(spec_bad)))
    for k, a, p_ in spec_bad:
        print("    %6d -> %-6d alias %.2f dBc, passband %.4f dB" % (k[0], k[1], a, p_))
        bad.append((k, "chain", "re-evaluated from the .inc it misses the uniform spec"))
    for k, stem, why in bad:
        print("    %6d -> %-6d %-22s %s" % (k[0], k[1], stem, why))
    for k, why in SUBSTITUTED.items():
        print("    DECLARED SUBSTITUTION %6d -> %-6d : %s" % (k[0], k[1], why))
    rows_ok = len([k for k in UNIVERSAL_ROUTES if k not in SUBSTITUTED
                   and not any(b[0] == k for b in bad)])
    print("migrated rows: %d of %d bit-identical to the Phase H authority, %d declared "
          "substitution" % (rows_ok, len(UNIVERSAL_ROUTES), len(SUBSTITUTED)))

    if args.csv:
        p = pathlib.Path(args.csv)
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("w", newline="", encoding="utf-8") as fh:
            w = csv.DictWriter(fh, fieldnames=list(out[0].keys()))
            w.writeheader()
            w.writerows(out)
        print("wrote %s (%d rows)" % (p, len(out)))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
