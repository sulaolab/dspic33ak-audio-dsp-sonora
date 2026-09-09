#!/usr/bin/env python3
"""Two follow-ups to asrc_48_to_32_full_iir_precheck.py.

1. ALIAS SWEEP, SPLIT AT THE GATE EDGE.  The main script's stopband sweep starts
   just above the output Nyquist, so it also reports sources that land BETWEEN
   15 and 16 kHz.  That band is outside what this work protects (the requirement
   is 0-15 kHz), and including it produces a "worst alias" of about -73 dBc at a
   15.9 kHz landing that has nothing to do with the 0-15 kHz gate.  Here the two
   regions are reported separately so neither hides the other.

2. ATTRIBUTION OF THE OFF-NOMINAL SPUR.  Item C shows a component around
   -104.5 dBc that appears only when the step is off nominal (-114.5 dBc at
   exactly 1.5).  Two device-real mechanisms can produce it and they have very
   different consequences:

     * the polyphase inter-phase LINEAR BLEND, which only fires off-nominal --
       inherent to the L=128/M=30 design and not fixable by arithmetic.  The
       MECHANISM is shared by anything that drives this resampler off nominal, but
       that is not a claim that the NUMBER is the same for another front end: what
       reaches the resampler depends on the filter in front of it;
     * the float32 PHASE ACCUMULATOR, whose per-add rounding random-walks the
       sampling instants -- an implementation detail, and in principle fixable.

   The ablation runs the same case four ways: nominal all-float32; off-nominal
   all-float32; off-nominal with a float64 phase accumulator but float32 blend and
   dot; and off-nominal all-float64.

   Measured 2026-09-05 (15 kHz, +100 ppm): -135.92 / -104.45 / -105.43 / -105.43
   dBc.  So -105.43 dBc is the blend (it survives full float64), the remaining
   about 1 dB is the float32 `frac` accumulator (changing ONLY frac recovers it),
   and float32 blend/dot contribute nothing measurable.

Neither part touches hardware.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
MAIN = HERE / "asrc_48_to_32_full_iir_precheck.py"

spec = importlib.util.spec_from_file_location("full_iir_precheck_main", MAIN)
if spec is None or spec.loader is None:
    raise SystemExit(f"cannot load {MAIN}")
pre = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = pre
spec.loader.exec_module(pre)


def df2t(rows: np.ndarray, x: np.ndarray, dtype) -> np.ndarray:
    """The device recurrence at a chosen precision, so float32 and float64 differ
    only in precision and not in operation order."""
    rows = rows.astype(dtype)
    x = x.astype(dtype)
    sections = rows.shape[0]
    d1 = np.zeros((sections, x.shape[0]), dtype=dtype)
    d2 = np.zeros_like(d1)
    y = np.empty_like(x)
    for n in range(x.shape[1]):
        v = x[:, n]
        for s in range(sections):
            b0, b1, b2, na1, na2 = rows[s]
            out = b0 * v + d1[s]
            d1[s] = b1 * v + d2[s] + na1 * out
            d2[s] = b2 * v + na2 * out
            v = out
        y[:, n] = v
    return y


def resample(x: np.ndarray, step: float, table: np.ndarray,
             sample_dtype, frac_dtype) -> np.ndarray:
    """`sample_dtype` is the precision of the blend and the 30-tap dot;
    `frac_dtype` is the precision of the phase accumulator alone."""
    table = table.astype(sample_dtype)
    x = x.astype(sample_dtype)
    step_f = frac_dtype(step)
    one = frac_dtype(1.0)
    ell = frac_dtype(pre.POLY_L)
    frac = frac_dtype(0.0)
    base = 0
    columns = []
    while base + pre.POLY_M <= x.shape[1]:
        pf = frac * ell
        p = int(pf)
        if p >= pre.POLY_L:
            p = pre.POLY_L - 1
        wb = sample_dtype(pf - frac_dtype(p))
        row = table[p] + wb * (table[p + 1] - table[p])
        columns.append(np.sum(x[:, base:base + pre.POLY_M] * row,
                              axis=1, dtype=sample_dtype))
        frac = frac_dtype(frac + step_f)
        while frac >= one:
            frac = frac_dtype(frac - one)
            base += 1
    return np.stack(columns, axis=1)


def one_case(rows, table, tone_hz, step, length, sample_dtype, frac_dtype) -> dict:
    time = np.arange(length, dtype=np.float64) / pre.FS_IN
    x = (pre.AMPLITUDE * np.sin(2.0 * np.pi * tone_hz * time))[None, :]
    filtered = df2t(rows, x, sample_dtype)
    out = resample(filtered, step, table, sample_dtype, frac_dtype)
    fs_out = pre.FS_IN / step
    wanted = float(pre.fit_amplitude(out, tone_hz, fs_out)[0])
    spur = pre.worst_spur(out[0], fs_out, tone_hz)
    return {
        "wanted_amplitude": wanted,
        "wanted_db": pre.db(wanted / pre.AMPLITUDE),
        "worst_spur_hz": spur["hz"],
        "worst_spur_amplitude": spur["amplitude"],
        "worst_spur_dbc": pre.db(spur["amplitude"] / max(wanted, 1e-30)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=pathlib.Path, default=None)
    parser.add_argument("--verify-json", type=pathlib.Path, default=pre.PHASE4_RECORD)
    parser.add_argument("--length-sweep", type=int, default=49_152)
    parser.add_argument("--length-attrib", type=int, default=147_456)
    parser.add_argument("--sweep-step-hz", type=float, default=125.0)
    parser.add_argument("--tone-hz", type=float, default=15_000.0)
    parser.add_argument("--offset-ppm", type=float, default=100.0)
    args = parser.parse_args()

    sos = pre.design_sos(args.verify_json)
    rows = pre.df2t_rows_f32(sos)
    table = pre.load_poly_table()
    record: dict = {}

    # ---- 1. alias sweep, split at 15 kHz -----------------------------------
    fs_out = pre.FS_IN / pre.NOMINAL_STEP
    sources = np.arange(fs_out / 2.0 + 100.0, 24_000.0 + 1.0, args.sweep_step_hz)
    out, _ = pre.chain_f32(sources, pre.NOMINAL_STEP, rows, table, args.length_sweep)
    inside, edge = [], []
    for index, source_hz in enumerate(sources):
        land = pre.landing_hz(source_hz, fs_out)
        if land < 100.0:
            continue
        amp = float(pre.fit_amplitude(out[index:index + 1], land, fs_out)[0])
        row = (float(source_hz), float(land), pre.db(amp / pre.AMPLITUDE))
        (inside if land <= 15_000.0 else edge).append(row)

    def summarise(rows_in, label):
        if not rows_in:
            return {"points": 0}
        worst = max(rows_in, key=lambda item: item[2])
        return {"band": label, "points": len(rows_in),
                "worst_source_hz": worst[0], "worst_landing_hz": worst[1],
                "worst_dbc": worst[2]}

    record["alias_sweep_split"] = {
        "step_hz": args.sweep_step_hz,
        "landing_0_to_15k": summarise(inside, "0-15 kHz (the gate)"),
        "landing_15_to_16k": summarise(edge, "15-16 kHz (not protected)"),
    }

    # ---- 2. attribution of the off-nominal spur ----------------------------
    step = pre.NOMINAL_STEP * (1.0 + args.offset_ppm * 1.0e-6)
    cases = {
        "nominal_all_float32": (pre.NOMINAL_STEP, np.float32, np.float32),
        "offnominal_all_float32": (step, np.float32, np.float32),
        "offnominal_float64_frac_only": (step, np.float32, np.float64),
        "offnominal_all_float64": (step, np.float64, np.float64),
    }
    attribution = {}
    for name, (case_step, sample_dtype, frac_dtype) in cases.items():
        attribution[name] = one_case(rows, table, args.tone_hz, case_step,
                                     args.length_attrib, sample_dtype, frac_dtype)
        attribution[name]["step"] = case_step
        print(f"  {name}: spur {attribution[name]['worst_spur_dbc']:.2f} dBc "
              f"at {attribution[name]['worst_spur_hz']:.0f} Hz")
    record["spur_attribution"] = {
        "tone_hz": args.tone_hz,
        "offset_ppm": args.offset_ppm,
        "cases": attribution,
    }

    text = json.dumps(record, indent=2, sort_keys=True)
    if args.json is not None:
        args.json.write_text(text, encoding="utf-8")
        print(f"wrote {args.json}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
