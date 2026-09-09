#!/usr/bin/env python3
"""Emit the SHIPPING-SHAPED six-SOS full-IIR header for the 48 -> 32 kHz trial.

Same filter as tools/asrc/gen_full_iir_precheck_coeffs.py -- re-derived from the
specification, checked bit-for-bit in float64 against the Phase-4 record, and
emitted low-Q-first -- with ONE difference: a fixed headroom gain is folded into
section 0's numerator.

WHY FOLD IT INTO THE COEFFICIENTS instead of scaling the input.  The gain has to
sit ahead of everything that can clip, and the earliest such point on target is
the IIR's own internal state (section 0's d1/d2 hold b1*x and b2*x).  Folding
g into b0/b1/b2 of section 0 scales the whole cascade -- every section's state,
the output, and nothing else -- and costs zero instructions per sample.  The
resulting response is EXACTLY g times the unity-gain response, so the two
quantities the report has to separate ("filter response, headroom gain removed"
and "actual end-to-end gain") differ by a build constant, not by a measurement.

Reported for the integration record:
  * CRC32 of the unity-gain table -- must stay 0x67AE5F41, the coefficient
    identity the Phase-2/3/4 benches were measured with.
  * CRC32 of the gained table, which is what the audio path links.
  * the exact float32 gain, and the peak scaling it implies.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import pathlib
import struct
import sys
import zlib

import numpy as np
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]
PHASE1 = ROOT / "tools" / "asrc" / "asrc_48_to_32_hybrid_filter_study.py"
PHASE4_RECORD = ROOT / "tools" / "asrc" / "asrc_48_to_32_full_iir_feasibility_2026-09-05.json"

ORDER = 12
RIPPLE_DB = 0.25
STOP_DB = 120.0
PASS_HZ = 15_000.0
FS_IN = 48_000.0
SECTION_ORDER = (0, 1, 2, 3, 4, 5)
UNITY_CRC32 = 0x67AE5F41

# Phase-4 host float64 internal peak and the on-target float32 output peak the
# precheck measured, both at 10 kHz full-scale sine.  They set what "enough
# headroom" has to cover; recorded in the header so the number in the source and
# the number in the report cannot drift.
HOST_INTERNAL_PEAK_FS = 1.096476
TARGET_OUTPUT_PEAK_FS = 1.118


def load_phase1():
    spec = importlib.util.spec_from_file_location("full_iir_phase1", PHASE1)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load " + str(PHASE1))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def design_sos() -> np.ndarray:
    phase1 = load_phase1()
    raw = signal.ellip(ORDER, RIPPLE_DB, STOP_DB, PASS_HZ / (FS_IN / 2.0),
                       btype="low", output="sos")
    return phase1.normalize_sos_dc(np.asarray(raw, dtype=np.float64))


def df2t_rows(sos: np.ndarray) -> np.ndarray:
    rows = []
    for b0, b1, b2, a0, a1, a2 in sos[list(SECTION_ORDER)]:
        if a0 != 1.0:
            raise SystemExit("a0 must be 1.0 after normalisation, got " + repr(a0))
        rows.append([b0, b1, b2, -a1, -a2])
    return np.asarray(rows, dtype=np.float64)


def c_float(value: float) -> str:
    text = "{0:+.17g}".format(value)
    if ("." not in text) and ("e" not in text) and ("E" not in text):
        text += ".0"
    return text + "f"


def crc32_le_f32(values: np.ndarray) -> int:
    packed = b"".join(struct.pack("<f", float(np.float32(v))) for v in values.ravel())
    return zlib.crc32(packed) & 0xFFFFFFFF


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path,
                        default=ROOT / "src" / "app" / "apps" / "asrc" /
                                "asrc_full_iir_48_to_32_coeffs.h")
    parser.add_argument("--headroom-db", type=float, default=-1.5,
                        help="fixed headroom gain in dB (negative attenuates); "
                             "-1.5 is the trial value agreed for this integration")
    parser.add_argument("--verify-json", type=pathlib.Path, default=PHASE4_RECORD)
    args = parser.parse_args()

    sos = design_sos()
    record = json.loads(args.verify_json.read_text())
    reference = np.asarray(record["best"]["coefficients"]["sos_float64"], dtype=np.float64)
    delta = float(np.max(np.abs(sos - reference)))
    print("phase-4 record max|delta| = {0:.3e}".format(delta))
    if delta != 0.0:
        raise SystemExit("re-derived SOS does not match the Phase-4 record")

    unity = df2t_rows(sos)
    unity_crc = crc32_le_f32(unity)
    print("unity-gain CRC32 = 0x{0:08X} (precheck/bench identity)".format(unity_crc))
    if unity_crc != UNITY_CRC32:
        raise SystemExit("unity-gain table is not the measured candidate "
                         "(expected 0x{0:08X})".format(UNITY_CRC32))

    radii = [float(np.max(np.abs(np.roots(row[3:])))) for row in sos[list(SECTION_ORDER)]]
    if radii != sorted(radii):
        raise SystemExit("emitted order is not low-Q-first: " + repr(radii))

    # float32 gain: the value the target actually multiplies by, so the host model
    # and the header agree bit-for-bit.
    gain = float(np.float32(10.0 ** (args.headroom_db / 20.0)))
    rows = unity.copy()
    rows[0, 0:3] *= gain
    crc = crc32_le_f32(rows)
    max_abs = float(np.max(np.abs(rows.astype(np.float32))))
    gain_db_exact = 20.0 * math.log10(gain)

    lines = [
        "#ifndef ASRC_FULL_IIR_48_TO_32_COEFFS_H",
        "#define ASRC_FULL_IIR_48_TO_32_COEFFS_H",
        "",
        "/*",
        " * GENERATED by tools/asrc/gen_full_iir_integration_coeffs.py -- do not hand-edit.",
        " *",
        " * The 48 -> 32 kHz TRIAL anti-alias LPF that the A->B audio path links when",
        " * APP_ASRC_FULL_IIR_48_TO_32 is 1 and Full-IIR mode is selected at runtime:",
        " *",
        " *   elliptic order {0}, {1} SOS, fp = {2:g} kHz,".format(ORDER, len(rows), PASS_HZ / 1000.0),
        " *   rp = {0} dB, rs = {1:g} dB, at {2:g} kHz input,".format(RIPPLE_DB, STOP_DB, FS_IN / 1000.0),
        " *   with a fixed {0:+.4f} dB headroom gain folded into section 0.".format(gain_db_exact),
        " *",
        " * Topology: 48 kHz -> this IIR -> unchanged generic ASRC at step 1.5 -> 32 kHz.",
        " *",
        " * THE GAIN IS IN THE COEFFICIENTS ON PURPOSE.  It has to precede everything",
        " * that can clip, and the earliest such thing is section 0's own state (d1/d2",
        " * hold b1*x and b2*x).  Folding it into b0/b1/b2 of section 0 scales every",
        " * section's state and the output by the same constant and costs no",
        " * instructions.  So the response is EXACTLY this gain times the unity-gain",
        " * response, and separating the two in the report is a subtraction of a build",
        " * constant rather than a second measurement.",
        " *",
        " * COEFFICIENT IDENTITY.  The unity-gain table this is derived from has",
        " * CRC32 0x{0:08X} -- the identity the Phase-2/3/4 benches and the *ai".format(unity_crc),
        " * probe were measured with (asrc_full_iir_precheck_coeffs.h).  The generator",
        " * refuses to emit if that no longer holds.",
        " *",
        " * SECTION ORDER IS MANDATORY.  Rows are low-Q-first (pole radii",
        " * " + " < ".join("{0:.6f}".format(r) for r in radii) + ");",
        " * worst internal peak 1.096476 (unity gain) in this order against 334.39",
        " * reversed.",
        " *",
        " * Rows are [b0, b1, b2, -a1, -a2] for the transposed direct-form II",
        " * recurrence, not SciPy's [b0, b1, b2, a0, a1, a2].  X data space, to match",
        " * the biquad primitive's coefficient fetch.",
        " */",
        "",
        "#define ASRC_FULL_IIR_48_TO_32_SOS            ({0}u)".format(len(rows)),
        "#define ASRC_FULL_IIR_48_TO_32_CRC32          (0x{0:08X}u) /* LE float32, gain folded */".format(crc),
        "#define ASRC_FULL_IIR_48_TO_32_UNITY_CRC32    (0x{0:08X}u) /* before the gain */".format(unity_crc),
        "#define ASRC_FULL_IIR_48_TO_32_HEADROOM_GAIN  ({0})".format(c_float(gain)),
        "#define ASRC_FULL_IIR_48_TO_32_HEADROOM_MDB   ({0:d}) /* dB x1000 */".format(int(round(gain_db_exact * 1000.0))),
        "#define ASRC_FULL_IIR_48_TO_32_MAX_ABS_COEFF  ({0:.9f}f)".format(max_abs),
        "",
        "/* Peaks the unity-gain filter reached, x full scale.  Multiply by the gain",
        " * above for what this table is expected to reach. */",
        "#define ASRC_FULL_IIR_48_TO_32_UNITY_PEAK_HOST_MILLI   ({0:d}u)".format(int(round(HOST_INTERNAL_PEAK_FS * 1000.0))),
        "#define ASRC_FULL_IIR_48_TO_32_UNITY_PEAK_TARGET_MILLI ({0:d}u)".format(int(round(TARGET_OUTPUT_PEAK_FS * 1000.0))),
        "",
        "static float asrc_full_iir_48_to_32_df2t_sos[ ASRC_FULL_IIR_48_TO_32_SOS * 5u ]"
        " __attribute__((space(xmemory))) = {",
    ]
    for index, row in enumerate(rows):
        body = ", ".join(c_float(value) for value in row)
        mark = "  (gain folded here)" if index == 0 else ""
        lines.append("    {0},   /* section {1}, pole radius {2:.9f}{3} */".format(
            body, index, radii[index], mark))
    lines += [
        "};",
        "",
        "#endif /* ASRC_FULL_IIR_48_TO_32_COEFFS_H */",
        "",
    ]
    args.out.write_text("\n".join(lines), encoding="utf-8")
    print("wrote " + str(args.out))
    print("headroom gain = {0:.9f} ({1:+.6f} dB)".format(gain, gain_db_exact))
    print("gained CRC32 = 0x{0:08X}   max|coeff| = {1:.9f}".format(crc, max_abs))
    print("expected on-target output peak = {0:.4f} FS (unity {1:.3f})".format(
        TARGET_OUTPUT_PEAK_FS * gain, TARGET_OUTPUT_PEAK_FS))
    print("expected host internal peak    = {0:.4f} FS (unity {1:.6f})".format(
        HOST_INTERNAL_PEAK_FS * gain, HOST_INTERNAL_PEAK_FS))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
