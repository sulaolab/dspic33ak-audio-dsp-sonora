#!/usr/bin/env python3
"""Analyze one or more *MEAS decimator_3x2 capture text files."""

from __future__ import annotations

import argparse
import math
import pathlib
import re

import numpy as np


FULL_SCALE = float(2**23 - 1)


def db(value: float) -> float:
    return 20.0 * math.log10(max(abs(value), 1.0e-300))


def metadata(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line))


def analyze(path: pathlib.Path) -> tuple[int, int, float, float, int, str]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    begin = next((line for line in lines if "MEAS_BEGIN" in line), "")
    meta = metadata(begin)
    if meta.get("kernel") != "decimator_3x2":
        raise ValueError(f"{path}: not a decimator_3x2 capture")
    fs = int(meta["fs_out_hz"])
    tone = int(meta["tone_hz"])
    values = np.asarray(
        [int(line.strip()) for line in lines if re.fullmatch(r"-?\d+", line.strip())],
        dtype=np.float64,
    )
    if values.size != int(meta["n"]):
        raise ValueError(f"{path}: sample count {values.size} != {meta['n']}")

    peak_equivalent_dbfs = db(float(np.sqrt(np.mean(values * values))) * math.sqrt(2.0) / FULL_SCALE)
    alias = tone % fs
    if alias > fs // 2:
        alias = fs - alias
    t = np.arange(values.size, dtype=np.float64) / fs
    if alias == 0:
        basis = np.ones((values.size, 1), dtype=np.float64)
    else:
        basis = np.column_stack(
            (np.sin(2.0 * np.pi * alias * t), np.cos(2.0 * np.pi * alias * t))
        )
    fit = basis @ np.linalg.lstsq(basis, values, rcond=None)[0]
    signal_rms = float(np.sqrt(np.mean(fit * fit)))
    residual_rms = float(np.sqrt(np.mean((values - fit) ** 2)))
    thdn = db(residual_rms / max(signal_rms, 1.0e-300))
    return tone, alias, peak_equivalent_dbfs, thdn, int(np.max(np.abs(values))), meta["coeff_crc32"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("captures", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    print("tone_hz alias_hz level_dbfs thdn_db peak_lsb coeff_crc32 file")
    failed = False
    for path in sorted(args.captures):
        tone, alias, level, thdn, peak, crc = analyze(path)
        print(f"{tone:7d} {alias:8d} {level:10.3f} {thdn:8.2f} {peak:8d} {crc} {path}")
        if tone == 1000 and abs(level + 1.0) > 0.05:
            failed = True
        if tone >= 5000 and level > -90.0:
            failed = True
    if failed:
        print("FAIL: one or more acceptance limits were exceeded")
        return 1
    print("PASS: captured gain and alias acceptance")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
