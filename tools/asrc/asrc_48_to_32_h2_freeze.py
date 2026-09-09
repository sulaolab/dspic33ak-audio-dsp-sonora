#!/usr/bin/env python3
"""Freeze and verify the single Phase-2 H2 coefficient set.

This is deliberately not another filter search.  It reconstructs only the
already-selected H2 candidate, records its float32 representation used by the
temporary dsPIC timing probe, and checks the host response before hardware is
used:

    48 kHz -> Kaiser FIR49/fc16 -> elliptic order-10 (five SOS) -> generic ASRC

The generic ASRC magnitude accounting is reused from the Phase-1 study.  Group
delay is the H2 prefilter transfer, matching that study's definition; a generic
fractional ASRC's phase is intentionally not invented here.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
import zlib
from typing import Any

import numpy as np
from scipy import signal


ROOT = pathlib.Path(__file__).resolve().parents[2]
PHASE1_PATH = ROOT / "tools" / "asrc" / "asrc_48_to_32_hybrid_filter_study.py"

EXPECTED_FIR_CRC32 = 0xC7CA9014
EXPECTED_DF2T_CRC32 = 0x335F52ED
EXPECTED_COMBINED_CRC32 = 0x2B0FB181
EXPECTED_ORDER = (0, 2, 1, 3, 4)
FS_HZ = 48_000.0


def load_phase1() -> Any:
    spec = importlib.util.spec_from_file_location("phase1_h2_freeze", PHASE1_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {PHASE1_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def crc32_f32(values: np.ndarray) -> int:
    return zlib.crc32(np.asarray(values, dtype="<f4").tobytes()) & 0xFFFFFFFF


def make_h2(phase1: Any, as_float32: bool) -> tuple[Any, np.ndarray, np.ndarray, np.ndarray]:
    fir = phase1.make_gentle_fir(49, 16_000.0)
    raw_sos = phase1.normalize_sos_dc(
        signal.ellip(10, 0.1, 100.0, 15_000.0, btype="low", fs=FS_HZ, output="sos")
    )
    prototype_iir = phase1.IirDesign(
        "ellip-fixed-n10-fp15-rp0.1-rs100.0", "ellip", raw_sos,
        15_000.0, 16_000.0, 0.1, 100.0, "fixed-order")
    prototype = phase1.Candidate(
        "H2/ellip-fixed-n10-fp15-rp0.1-rs100.0/pre49-fc16", "H2 gentle FIR + IIR",
        prototype_iir, fir, phase1.IDENTITY_FIR, 16_000.0, None, None)
    order = np.asarray(phase1.headroom(prototype)["peak_minimizing_order"], dtype=np.intp)
    if tuple(int(x) for x in order) != EXPECTED_ORDER:
        raise RuntimeError(f"H2 SOS order drifted: {order.tolist()} != {list(EXPECTED_ORDER)}")
    sos = raw_sos[order]
    if as_float32:
        fir = fir.astype(np.float32).astype(np.float64)
        sos = sos.astype(np.float32).astype(np.float64)
    iir = phase1.IirDesign(prototype_iir.label, prototype_iir.family, sos,
                            prototype_iir.pass_edge, prototype_iir.stop_edge,
                            prototype_iir.ripple, prototype_iir.attenuation,
                            prototype_iir.design_mode)
    candidate = phase1.Candidate(prototype.label, prototype.topology, iir, fir,
                                 phase1.IDENTITY_FIR, 16_000.0, None, None)
    df2t = np.column_stack((sos[:, 0], sos[:, 1], sos[:, 2], -sos[:, 4], -sos[:, 5]))
    return candidate, fir, sos, df2t


def group_delay(phase1: Any, candidate: Any) -> dict[str, Any]:
    grid = np.linspace(0.0, 15_000.0, 15_001)
    response = phase1.fir_response(candidate.fir1, FS_HZ, grid) * phase1.sos_response(candidate.iir.sos, grid)
    delay_samples = -np.gradient(np.unwrap(np.angle(response)), 2.0 * np.pi * grid / FS_HZ)
    points_hz = (1_000.0, 5_000.0, 10_000.0, 12_000.0, 13_000.0, 14_000.0, 14_500.0, 15_000.0)
    bands_hz = (5_000.0, 10_000.0, 12_000.0, 14_000.0, 15_000.0)
    return {
        "points": {
            f"{int(freq / 1000)}k" if (freq % 1000.0) == 0.0 else "14.5k": {
                "samples": float(np.interp(freq, grid, delay_samples)),
                "us": float(np.interp(freq, grid, delay_samples) * 1.0e6 / FS_HZ),
            }
            for freq in points_hz
        },
        "bands": {
            f"0-{int(edge / 1000)}k": {
                "min_samples": float(np.min(delay_samples[grid <= edge])),
                "max_samples": float(np.max(delay_samples[grid <= edge])),
                "pp_samples": float(np.ptp(delay_samples[grid <= edge])),
                "pp_us": float(np.ptp(delay_samples[grid <= edge]) * 1.0e6 / FS_HZ),
            }
            for edge in bands_hz
        },
    }


def report() -> dict[str, Any]:
    phase1 = load_phase1()
    candidate64, fir64, sos64, df2t64 = make_h2(phase1, as_float32=False)
    candidate32, fir32, sos32, df2t32 = make_h2(phase1, as_float32=True)
    crc = {
        "fir49_le_float32": crc32_f32(fir32),
        "ordered_sos6_le_float32": crc32_f32(sos32),
        "df2t_sos5_le_float32": crc32_f32(df2t32),
        "fir_then_df2t_le_float32": crc32_f32(np.concatenate((fir32, df2t32.reshape(-1)))),
    }
    if (crc["fir49_le_float32"] != EXPECTED_FIR_CRC32 or
            crc["df2t_sos5_le_float32"] != EXPECTED_DF2T_CRC32 or
            crc["fir_then_df2t_le_float32"] != EXPECTED_COMBINED_CRC32):
        raise RuntimeError(f"frozen coefficient CRC drift: {crc}")
    result64 = phase1.evaluate(candidate64)
    result32 = phase1.evaluate(candidate32)
    robustness = phase1.robustness(candidate64)
    return {
        "candidate": candidate64.label,
        "fir": {"taps": 49, "cutoff_hz": 16_000.0, "window": "Kaiser beta=5.0"},
        "iir": {
            "family": "ellip", "order": 10, "sos": 5, "pass_edge_hz": 15_000.0,
            "ripple_db": 0.1, "stop_edge_hz": 16_000.0, "attenuation_db": 100.0,
            "sos_order": list(EXPECTED_ORDER),
            "df2t_layout": "[b0, b1, b2, -a1, -a2]",
        },
        "crc32": {name: f"0x{value:08X}" for name, value in crc.items()},
        "float64": {
            "alias_0_to_15k_dbc": result64.aliases[15_000.0][0],
            "wanted_15k_db": result64.wanted_db[15_000.0],
            "wanted_floor_0_to_15k_db": result64.min_0_to_band[15_000.0],
            "wanted_peak_db": result64.peak_db,
        },
        "float32": {
            "alias_0_to_15k_dbc": result32.aliases[15_000.0][0],
            "wanted_15k_db": result32.wanted_db[15_000.0],
            "wanted_floor_0_to_15k_db": result32.min_0_to_band[15_000.0],
            "wanted_peak_db": result32.peak_db,
        },
        "robustness": robustness,
        "group_delay": group_delay(phase1, candidate64),
        "coefficients": {
            "fir49_float32": [float(x) for x in fir32],
            "df2t_sos5_float32": [[float(x) for x in row] for row in df2t32],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=pathlib.Path, help="write the complete frozen-candidate record")
    args = parser.parse_args()
    data = report()
    print("H2 freeze PASS")
    print(f"candidate: {data['candidate']}")
    print("CRC32: " + ", ".join(f"{name}={value}" for name, value in data["crc32"].items()))
    for precision in ("float64", "float32"):
        metrics = data[precision]
        print(f"{precision}: alias0-15={metrics['alias_0_to_15k_dbc']:.6f} dBc "
              f"wanted15={metrics['wanted_15k_db']:.6f} dB "
              f"floor={metrics['wanted_floor_0_to_15k_db']:.6f} dB "
              f"peak={metrics['wanted_peak_db']:.6f} dB")
    robust = data["robustness"]
    print(f"float32 stability: {robust['float32_stable']} max pole radius={robust['float32_max_pole_radius']:.9f}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
