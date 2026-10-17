#!/usr/bin/env python3
"""WHO MADE EACH SHIPPED COEFFICIENT SET, AND TO WHICH SPECIFICATION (Phase G, G-B).

WHY THIS FILE EXISTS
====================
Phase F-2 compared a planner row against an image and found they were two different chains
(F-C).  The owner decision that followed was to make the AS-BUILT GENERATOR LOGIC the
qualification authority.  That decision is only safe for a coefficient set whose generator chain
is ESTABLISHED -- otherwise "as-built authority" is another unverified claim, in a different row.

So this script answers three separate questions per set, and keeps them in separate columns
because they have different answers:

  1  PROVENANCE  -- does the named generator, run with ITS OWN spec constants, reproduce the
                    bytes in the shipped include, and does the include's CRC32 match?
                    (Bit-identity, not similarity.)
  2  SPECIFICATION -- WHICH spec was it designed to?  The uniform one the prototype family uses
                    (protected 0..0.8125 x FN, Fstop = Fs_out - Fp, owner 2026-09-14), or a
                    LEGACY per-rate specification signed off before that, with its own passband
                    edge and its own fold edge?
  3  ROUTE       -- which shipping route loads it (asrc_audio_path_frontend_plan + the variant
                    resolver in asrc_audio_path.c).

The headline finding this script exists to make un-missable: PROVENANCE is established for every
shipped set, and the SPECIFICATIONS ARE NOT ONE SPECIFICATION.  The 48 kHz and 96 kHz families
were each signed off against their own passband edge (0.70 .. 0.83 of their output Nyquist), so
an as-built number for those rows answers "does the image meet the spec it was built to", NOT
"does the image meet the uniform spec".  A single PASS/FAIL column over both would be the F-C
mistake with a new coat of paint.

Nothing here writes to src/.  It reads the includes and re-runs the generators in memory.

Usage:
  python tools/asrc/asrc_coeff_provenance.py                # the ledger
  python tools/asrc/asrc_coeff_provenance.py --csv out.csv  # machine-readable, mode-stamped
  python tools/asrc/asrc_coeff_provenance.py --strict       # exit 1 if any provenance is unproven
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import importlib.util
import io
import pathlib
import re
import sys
import zlib

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INC_DIR = ROOT / "src" / "app" / "apps" / "asrc"
sys.path.insert(0, str(HERE))

MODE = "qualification"          # this ledger is only ever about the as-built image


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


G = _load("gen48", "tools/asrc/asrc_decimator_48_to_8_design.py")     # the decimator generator
L = _load("gen11", "tools/asrc/asrc_lpf1to1_design.py")               # the 1:1 generator

UNIFORM_RATIO = L.PROTECT_RATIO          # 0.8125
GEN_DEC = "asrc_decimator_48_to_8_design.py"
GEN_LP1 = "asrc_lpf1to1_design.py"

# ------------------------------------------------------------------------------------ the registry
#
# One row per SHIPPED coefficient array.  Every spec field is a REFERENCE INTO THE GENERATOR
# MODULE, never a re-typed number: if a generator constant moves, this ledger moves with it, and
# a set whose spec was silently edited shows up as a provenance failure rather than as agreement.
#
# fields: (key, inc file, C array, taps, fs_design, passband, stopband, beta, fs_out_served,
#          generator, crc group, routes)
def _row(key, inc, arr, taps, fs, pb, sb, beta, fs_out, gen, crcgrp, routes, final=True):
    # `final` = this stage is the LAST front-end stage before the rear resampler, so its stopband
    # is the one the uniform Fstop can be compared against.  An INTERMEDIATE stage has a
    # deliberately relaxed stopband (it only suppresses what would fold after the stages behind
    # it), and comparing that against the final Fstop is the F-C mistake in miniature.
    return dict(key=key, inc=inc, arr=arr, taps=int(taps), fs=float(fs), pb=float(pb),
                sb=float(sb), beta=float(beta), fs_out=float(fs_out), gen=gen,
                crcgrp=crcgrp, routes=routes, final=bool(final))


REGISTRY = [
    # ---- /6 chain: 48 -> 8 kHz, two stages, ONE spec (passband 3200, final fold 4000) --------
    _row("48to8.s1", "asrc_decimator_48_to_8_coeffs.inc", "s_stage1_coeff",
         G.STAGE1_TAPS, G.STAGE1_INPUT_HZ, G.PASSBAND_HZ, G.STAGE1_STOPBAND_HZ, G.KAISER_BETA,
         8000.0, GEN_DEC, "48to8", "48->8 (den 6) stage 1; 96->8 behind the pre-stage",
         final=False),
    _row("48to8.s2", "asrc_decimator_48_to_8_coeffs.inc", "s_stage2_coeff",
         G.STAGE2_TAPS, G.STAGE2_INPUT_HZ, G.PASSBAND_HZ, G.STAGE2_STOPBAND_HZ, G.KAISER_BETA,
         8000.0, GEN_DEC, "48to8", "48->8 (den 6) stage 2; 96->8 behind the pre-stage"),
    # ---- /3: 48 -> 16 kHz, single stage, forced (report section 12.1) ------------------------
    _row("48to16", "asrc_decimator_48_to_16_coeffs.inc", "s_48_to_16_coeff",
         G.MIDRATE_TAPS, G.STAGE1_INPUT_HZ, G.MIDRATE_PASSBAND_HZ, G.MIDRATE_STOPBAND_HZ,
         G.KAISER_BETA, 16000.0, GEN_DEC, "48to16", "48->16 (den 3); 96->16 (composed den 6)"),
    # ---- /2: two coefficient sets over ONE structure ----------------------------------------
    _row("48to24", "asrc_decimator_48_to_24_coeffs.inc", "s_48_to_24_coeff",
         G.HALFRATE_TAPS, G.STAGE1_INPUT_HZ, G.HALFRATE_PASSBAND_HZ, G.HALFRATE_STOPBAND_HZ,
         G.KAISER_BETA, 24000.0, GEN_DEC, "48to24", "48->24 (den 2); 96->24 (composed den 4)"),
    _row("48to24:22k", "asrc_decimator_48_to_24_coeffs.inc", "s_48_to_24_out22k_coeff",
         G.HALFRATE_TAPS, G.STAGE1_INPUT_HZ, G.HALFRATE22K_PASSBAND_HZ,
         G.HALFRATE22K_STOPBAND_HZ, G.KAISER_BETA, 22050.0, GEN_DEC, "48to24:22k",
         "48->22.05 (den 2, rear step 1.08843); 96->22.05 (composed den 4)"),
    # ---- /4: two coefficient sets over ONE structure, two stages each -----------------------
    _row("48to12:11k.s1", "asrc_decimator_48_to_12_coeffs.inc", "s_48_to_12_stage1_coeff",
         G.QUARTER_STAGE1_TAPS, G.QUARTER_STAGE1_INPUT_HZ, G.QUARTER_PASSBAND_HZ,
         G.QUARTER_STAGE1_STOPBAND_HZ, G.KAISER_BETA, 11025.0, GEN_DEC, "48to12:11k",
         "48->11.025 (den 4) stage 1; 96->11.025 (composed den 8)", final=False),
    _row("48to12:11k.s2", "asrc_decimator_48_to_12_coeffs.inc", "s_48_to_12_stage2_coeff",
         G.QUARTER_STAGE2_TAPS, G.QUARTER_STAGE2_INPUT_HZ, G.QUARTER_PASSBAND_HZ,
         G.QUARTER_STAGE2_STOPBAND_HZ, G.KAISER_BETA, 11025.0, GEN_DEC, "48to12:11k",
         "48->11.025 (den 4) stage 2; 96->11.025 (composed den 8)"),
    _row("48to12:12k.s1", "asrc_decimator_48_to_12_coeffs.inc", "s_48_to_12_out12k_stage1_coeff",
         G.QUARTER_STAGE1_TAPS, G.QUARTER_STAGE1_INPUT_HZ, G.QUARTER12K_PASSBAND_HZ,
         G.QUARTER12K_STAGE1_STOPBAND_HZ, G.KAISER_BETA, 12000.0, GEN_DEC, "48to12:12k",
         "48->12 (den 4, rear step 1.0) stage 1; 96->12 (composed den 8)", final=False),
    _row("48to12:12k.s2", "asrc_decimator_48_to_12_coeffs.inc", "s_48_to_12_out12k_stage2_coeff",
         G.QUARTER_STAGE2_TAPS, G.QUARTER_STAGE2_INPUT_HZ, G.QUARTER12K_PASSBAND_HZ,
         G.QUARTER12K_STAGE2_STOPBAND_HZ, G.KAISER_BETA, 12000.0, GEN_DEC, "48to12:12k",
         "48->12 (den 4) stage 2; 96->12 (composed den 8)"),
    # ---- the 96 -> 48 kHz pre-stage: THREE sets, and their tap counts differ on purpose -----
    _row("96to48:shared", "asrc_decimator_96_to_48_coeffs.inc", "s_96_to_48_coeff",
         G.PRESTAGE_TAPS, G.PRESTAGE_INPUT_HZ, G.PRESTAGE_PASSBAND_HZ, G.PRESTAGE_STOPBAND_HZ,
         G.KAISER_BETA, 32000.0, GEN_DEC, "96to48:shared",
         "every COMPOSED 96 kHz row (8/11.025/12/16/22.05/24/32 kHz); binding case 32 kHz",
         final=False),
    _row("96to48:44k1", "asrc_decimator_96_to_48_coeffs.inc", "s_96_to_48_out44k1_coeff",
         G.PRESTAGE44K1_TAPS, G.PRESTAGE_INPUT_HZ, G.PRESTAGE44K1_PASSBAND_HZ,
         G.PRESTAGE44K1_STOPBAND_HZ, G.KAISER_BETA, 44100.0, GEN_DEC, "96to48:44k1",
         "96->44.1 (den 2, pre-stage IS the whole front end) -- variant FOR_44100"),
    _row("96to48:48k", "asrc_decimator_96_to_48_coeffs.inc", "s_96_to_48_out48k_coeff",
         G.PRESTAGE48K_TAPS, G.PRESTAGE_INPUT_HZ, G.PRESTAGE48K_PASSBAND_HZ,
         G.PRESTAGE48K_STOPBAND_HZ, G.KAISER_BETA, 48000.0, GEN_DEC, "96to48:48k",
         "96->48 (den 2, pre-stage IS the whole front end) -- variant FOR_48000"),
    _row("96to48:hb", "asrc_decimator_96_to_48_hb_coeffs.inc", "s_96_to_48_hb_coeff",
         G.PRESTAGE_HB_TAPS, G.PRESTAGE_INPUT_HZ, G.PRESTAGE_HB_PASSBAND_HZ,
         G.PRESTAGE_HB_STOPBAND_HZ, G.PRESTAGE_HB_KAISER_BETA, 32000.0, GEN_DEC, "96to48:hb",
         "OPT-IN only (APP_ASRC_Q31_PRE_HALFBAND) -- not in the default image",
         final=False),
    # ---- the L=2/M=3 audio-mode set: rows are phases of a 97-tap prototype -------------------
    _row("48to32:phase0", "asrc_decimator_48_to_32_coeffs.inc", "s_48_to_32_phase0_coeff",
         G.POLY32_PHASE0_TAPS, G.POLY32_PROTO_HZ, G.POLY32_PASSBAND_HZ, G.POLY32_STOPBAND_HZ,
         G.KAISER_BETA, 32000.0, GEN_DEC, "48to32",
         "48->32 (num 2/den 3, AUDIO MODE); 96->32 (composed 2/6)"),
    _row("48to32:phase1", "asrc_decimator_48_to_32_coeffs.inc", "s_48_to_32_phase1_coeff",
         G.POLY32_PHASE1_TAPS, G.POLY32_PROTO_HZ, G.POLY32_PASSBAND_HZ, G.POLY32_STOPBAND_HZ,
         G.KAISER_BETA, 32000.0, GEN_DEC, "48to32",
         "48->32 (num 2/den 3, AUDIO MODE); 96->32 (composed 2/6)"),
]

# The 1:1 family: spec is DERIVED from the uniform rule, so the rows carry the rule, not numbers.
LPF1TO1 = [
    ("lp1:32k->16k", "s_lp1_32k_to_16k_coeff", 32000.0, 16000.0, 1,
     "32->16 (PROTOTYPE, APP_ASRC_LPF1TO1_PROTOTYPE) -- the 1:1 stage IS the front end"),
    ("lp1:22k05->16k", "s_lp1_22k05_to_16k_coeff", 22050.0, 16000.0, 1,
     "44.1->16 (PROTOTYPE) second stage, behind the /2"),
    ("lp1:16k->11k025", "s_lp1_16k_to_11k025_coeff", 16000.0, 11025.0, 1,
     "32->11.025 (PROTOTYPE, P2) second stage -- NO ROUTE PUBLISHES IT YET"),
    ("dec2:44k1_for_16k", "s_dec2_44k1_for_16k_coeff", 44100.0, 16000.0, 2,
     "44.1->16 (PROTOTYPE) FIRST stage, partner-aware /2"),
]


# ------------------------------------------------------------------------------------ include I/O
def read_array(inc_name: str, arr: str) -> np.ndarray:
    """Pull one float array out of a generated include, exactly as the compiler would see it."""
    txt = (INC_DIR / inc_name).read_text(encoding="ascii")
    m = re.search(r"static const float %s\[[^\]]*\]\s*=\s*\{(.*?)\};" % re.escape(arr), txt, re.S)
    if not m:
        raise SystemExit("array %s not found in %s" % (arr, inc_name))
    vals = re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?f", m.group(1))
    return np.asarray([float(v[:-1]) for v in vals], dtype="<f4")


def read_macro(inc_name: str, macro: str):
    txt = (INC_DIR / inc_name).read_text(encoding="ascii")
    m = re.search(r"#define\s+%s\s*\(?(0[xX][0-9A-Fa-f]+|\d+)[uU]?L?\)?" % re.escape(macro), txt)
    return None if m is None else int(m.group(1), 0)


def crc(*sets) -> int:
    return zlib.crc32(b"".join(np.asarray(s, dtype="<f4").tobytes() for s in sets)) & 0xFFFFFFFF


# CRC macro per group, and which arrays it covers, in the order the generator concatenates them.
CRC_GROUPS = {
    "48to8": ("asrc_decimator_48_to_8_coeffs.inc", "ASRC_DECIMATOR_COEFF_CRC32",
              ["48to8.s1", "48to8.s2"]),
    "48to16": ("asrc_decimator_48_to_16_coeffs.inc", "ASRC_DECIMATOR_48_TO_16_GENERATED_CRC32",
               ["48to16"]),
    "48to24": ("asrc_decimator_48_to_24_coeffs.inc", "ASRC_DECIMATOR_48_TO_24_GENERATED_CRC32",
               ["48to24"]),
    "48to24:22k": ("asrc_decimator_48_to_24_coeffs.inc",
                   "ASRC_DECIMATOR_48_TO_24_OUT22K_GENERATED_CRC32", ["48to24:22k"]),
    "48to12:11k": ("asrc_decimator_48_to_12_coeffs.inc",
                   "ASRC_DECIMATOR_48_TO_12_GENERATED_CRC32",
                   ["48to12:11k.s1", "48to12:11k.s2"]),
    "48to12:12k": ("asrc_decimator_48_to_12_coeffs.inc",
                   "ASRC_DECIMATOR_48_TO_12_OUT12K_GENERATED_CRC32",
                   ["48to12:12k.s1", "48to12:12k.s2"]),
    "96to48:shared": ("asrc_decimator_96_to_48_coeffs.inc",
                      "ASRC_DECIMATOR_96_TO_48_GENERATED_CRC32", ["96to48:shared"]),
    "96to48:44k1": ("asrc_decimator_96_to_48_coeffs.inc",
                    "ASRC_DECIMATOR_96_TO_48_OUT44K1_GENERATED_CRC32", ["96to48:44k1"]),
    "96to48:48k": ("asrc_decimator_96_to_48_coeffs.inc",
                   "ASRC_DECIMATOR_96_TO_48_OUT48K_GENERATED_CRC32", ["96to48:48k"]),
    "96to48:hb": ("asrc_decimator_96_to_48_hb_coeffs.inc",
                  "ASRC_DECIMATOR_96_TO_48_HB_GENERATED_CRC32", ["96to48:hb"]),
    "48to32": ("asrc_decimator_48_to_32_coeffs.inc",
               "ASRC_DECIMATOR_48_TO_32_GENERATED_CRC32", ["48to32:phase0", "48to32:phase1"]),
}


# ------------------------------------------------------------------------------------ the checks
def spec_class(row) -> tuple:
    """Which SPECIFICATION this set was designed to, and how far its passband edge sits from the
    uniform one.  Returns (class, fp_ratio, note)."""
    fn = row["fs_out"] * 0.5
    ratio = row["pb"] / fn
    fstop_uniform = row["fs_out"] - UNIFORM_RATIO * fn
    uniform_pb = abs(ratio - UNIFORM_RATIO) < 1e-9
    uniform_sb = abs(row["sb"] - fstop_uniform) < 1e-6 or not row.get("final", True)
    if uniform_pb and uniform_sb:
        return "UNIFORM", ratio, ""
    if row["key"].startswith("48to32") or row["key"] == "96to48:hb":
        return "LEGACY (partial)", ratio, ("AUDIO MODE: 0-13 kHz protected, 13-16 kHz relaxed "
                                           "-- a deliberately partial spec, owner 2026-09-03")
    if not row.get("final", True):
        return "LEGACY (per-rate)", ratio, ("passband edge %.4g x FN, uniform asks %.4g.  "
                                            "INTERMEDIATE stage: its %.1f Hz stopband is "
                                            "relaxed on purpose and is NOT comparable to the "
                                            "final Fstop" % (ratio, UNIFORM_RATIO, row["sb"]))
    return "LEGACY (per-rate)", ratio, ("passband edge %.4g x FN, uniform asks %.4g; fold edge "
                                        "%.1f Hz, uniform asks %.1f"
                                        % (ratio, UNIFORM_RATIO, row["sb"], fstop_uniform))


def check_row(row) -> dict:
    """Regenerate this set from ITS OWN generator constants and compare bit-for-bit."""
    have = read_array(row["inc"], row["arr"])
    if row["key"].startswith("48to32"):
        p0, p1, _proto = G.design_32k_polyphase()
        want = p0 if row["key"].endswith("phase0") else p1
    else:
        want = G.design(row["taps"], row["fs"], row["pb"], row["sb"], row["beta"])
    n_ok = have.size == want.size == row["taps"]
    err = float(np.max(np.abs(have.astype(np.float64) - want.astype(np.float64)))) \
        if have.size == want.size else float("inf")
    cls, ratio, note = spec_class(row)
    return dict(row, have=have, want=want, n_ok=n_ok, err=err, ident=(n_ok and err == 0.0),
                cls=cls, ratio=ratio, note=note)


def check_lpf1to1() -> list:
    """The 1:1 family, against the tool that WRITES it.  Its taps are SEARCHED, not pinned, so
    provenance here also proves the search still lands on the shipped tap count."""
    out = []
    for key, arr, fs, fs_out, d, routes in LPF1TO1:
        is_final = (d == 1)
        have = read_array("asrc_lpf1to1_coeffs.inc", arr)
        if d == 1:
            chosen, _rows, _n0, fp, fstop = L.design_one(fs, fs_out, verbose=False)
        else:
            chosen, _rows, _n0, fp, fstop = L.design_prestage(fs, d, fs_out, verbose=False)
        if chosen is None:
            out.append(dict(key=key, arr=arr, inc="asrc_lpf1to1_coeffs.inc", taps=have.size,
                            fs=fs, fs_out=fs_out, pb=0.0, sb=0.0, beta=L.M.KAISER_BETA,
                            gen=GEN_LP1, routes=routes, have=have, want=have, n_ok=False,
                            err=float("inf"), ident=False, cls="UNKNOWN", ratio=0.0,
                            note="design search failed"), )
            continue
        n, coeff = chosen[0], np.asarray(chosen[1], dtype="<f4")
        err = float(np.max(np.abs(have.astype(np.float64) - coeff.astype(np.float64)))) \
            if have.size == coeff.size else float("inf")
        out.append(dict(key=key, arr=arr, inc="asrc_lpf1to1_coeffs.inc", taps=int(n),
                        fs=fs, fs_out=fs_out, pb=fp, sb=fstop, beta=L.M.KAISER_BETA,
                        gen=GEN_LP1, routes=routes, have=have, want=coeff,
                        n_ok=(have.size == n), err=err, ident=(have.size == n and err == 0.0),
                        cls="UNIFORM", ratio=fp / (fs_out * 0.5), final=is_final,
                        note="tap count SEARCHED (smallest odd N passing the Q31 composed gate)"))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", type=str, default="")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 unless every shipped set is bit-identical to its generator")
    a = ap.parse_args()

    print("=" * 118)
    print("COEFFICIENT PROVENANCE LEDGER -- mode = %s (as-built image only)" % MODE)
    print("=" * 118)
    print("uniform spec for reference: protected 0..%.4g x FN, Fstop = Fs_out - Fp, "
          "passband +/-%.2f dB, alias <= %.0f dB"
          % (UNIFORM_RATIO, L.PASSBAND_LIMIT_DB, L.ALIAS_PRODUCT_DB))
    print()

    rows = [check_row(r) for r in REGISTRY] + check_lpf1to1()

    print("  %-18s %5s %9s %9s %9s %6s %-9s %-18s" %
          ("set", "taps", "fs_des", "pass Hz", "stop Hz", "beta", "identity", "specification"))
    print("  " + "-" * 114)
    bad = 0
    for r in rows:
        ident = "BIT-EXACT" if r["ident"] else ("N mismatch" if not r["n_ok"]
                                               else "DIFF %.3g" % r["err"])
        if not r["ident"]:
            bad += 1
        print("  %-18s %5d %9.0f %9.1f %9.1f %6.1f %-9s %-18s"
              % (r["key"], r["taps"], r["fs"], r["pb"], r["sb"], r["beta"], ident, r["cls"]))
    print()

    # ---- CRC32, the include's own self-declaration -----------------------------------------
    print("  CRC32 declared in the include vs the regenerated bytes:")
    by_key = {r["key"]: r for r in rows}
    crc_bad = 0
    for grp, (inc, macro, keys) in CRC_GROUPS.items():
        declared = read_macro(inc, macro)
        got = crc(*[by_key[k]["want"] for k in keys])
        ok = declared == got
        crc_bad += 0 if ok else 1
        print("    %-16s %-46s 0x%08X %s" % (grp, macro, got,
                                             "MATCH" if ok else "** 0x%08X in file **" % declared))
    lp_declared = read_macro("asrc_lpf1to1_coeffs.inc", "ASRC_LPF1TO1_GENERATED_CRC32")
    lp_keys = [k for k, *_ in LPF1TO1]
    lp_got = crc(*[by_key[k]["want"] for k in lp_keys])
    lp_ok = lp_declared == lp_got
    crc_bad += 0 if lp_ok else 1
    print("    %-16s %-46s 0x%08X %s" % ("lpf1to1", "ASRC_LPF1TO1_GENERATED_CRC32", lp_got,
                                         "MATCH" if lp_ok else "** 0x%08X in file **" % lp_declared))
    print()

    # ---- the point of the exercise: the specifications are not one specification -------------
    print("  SPECIFICATION SPREAD -- passband edge as a fraction of the served output Nyquist:")
    for r in sorted(rows, key=lambda x: x["ratio"]):
        mark = "  uniform" if r["cls"] == "UNIFORM" else ""
        print("    %-18s %-16s %.4f x FN  (Fs_out %7.1f Hz)%s"
              % (r["key"], r["cls"], r["ratio"], r["fs_out"], mark))
    print()
    print("    %-18s %s" % ("uniform asks", "%.4f x FN" % UNIFORM_RATIO))
    legacy = [r for r in rows if r["cls"] != "UNIFORM"]
    print("    %d of %d shipped sets were designed to a LEGACY per-rate specification."
          % (len(legacy), len(rows)))
    print("    An as-built number for those rows answers 'does the image meet the spec it was")
    print("    BUILT to', not 'does the image meet the uniform spec'.  Keep the two columns apart.")
    print()

    for r in rows:
        if r["note"]:
            print("    %-18s %s" % (r["key"], r["note"]))
    print()

    print("  ROUTES")
    for r in rows:
        print("    %-18s %s" % (r["key"], r["routes"]))
    print()

    print("VERDICT")
    print("  provenance: %d of %d sets are bit-identical to their named generator run with its "
          "own spec constants" % (len(rows) - bad, len(rows)))
    print("  CRC32     : %d of %d declared checksums match" % (len(CRC_GROUPS) + 1 - crc_bad,
                                                               len(CRC_GROUPS) + 1))
    if bad == 0 and crc_bad == 0:
        print("  => the GENERATOR CHAIN is ESTABLISHED for every shipped coefficient set.")
        print("     G-B is satisfied for provenance.  It is NOT satisfied for 'one spec': the")
        print("     LEGACY rows above must be reported against their own specification.")
    else:
        print("  => at least one set is NOT reproducible from its generator.  Do not call any")
        print("     number for that row 'as-built authority' until it is.")

    if a.csv:
        with open(a.csv, "w", newline="", encoding="ascii") as fh:
            w = csv.writer(fh)
            w.writerow(["mode", "set", "inc", "array", "taps", "fs_design_hz", "passband_hz",
                        "stopband_hz", "beta", "fs_out_hz", "generator", "identity",
                        "max_abs_diff", "specification", "fp_over_fn", "routes"])
            for r in rows:
                w.writerow([MODE, r["key"], r["inc"], r["arr"], r["taps"], "%.0f" % r["fs"],
                            "%.1f" % r["pb"], "%.1f" % r["sb"], "%.1f" % r["beta"],
                            "%.1f" % r["fs_out"], r["gen"],
                            "BIT-EXACT" if r["ident"] else "DIFFERENT", "%.6g" % r["err"],
                            r["cls"], "%.6f" % r["ratio"], r["routes"]])
        print()
        print("wrote %s (mode column = %s)" % (a.csv, MODE))

    return 1 if (a.strict and (bad or crc_bad)) else 0


if __name__ == "__main__":
    raise SystemExit(main())
