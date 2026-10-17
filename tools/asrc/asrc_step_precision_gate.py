#!/usr/bin/env python3
"""Host gate for the ASRC applied-step precision path.

The shipping step smoother used to keep its state as one binary32 number near
the absolute rate ratio (for example 2.0 for the rear 16 -> 8 kHz stage).  With
beta=0.005, an update smaller than half an ULP of that absolute number is lost.
At step=2.0 the dead zone is therefore about 100 input ULPs (11.9 ppm).

This gate models the C operations explicitly in binary32 and proves three
properties needed by the firmware fix:

  1. the old absolute-state EMA exhibits the measured stick/slip mechanism;
  2. servo slew and smoothing keep the correction separate from the base ratio;
  3. a base-plus-correction EMA continues to move below one absolute-step ULP;
  4. converting base and correction separately to a Q1.31 fractional step
     preserves that motion, while summing them into one float first does not.

It also checks the source spellings that make the model an implementation gate
rather than a detached numerical note.  ``--selftest`` mutates each essential
part and requires the gate to fail.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

import numpy as np


REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE = REPO / "src" / "app" / "apps" / "asrc" / "audio_app_asrc.c"
Q31_SOURCE = REPO / "src" / "app" / "apps" / "asrc" / "asrc_poly_q31.inc"

F32 = np.float32
BETA = F32(0.005)
BASE = F32(2.0)
Q31_SCALE = F32(2147483648.0)
Q31_PERIOD = 1 << 31


def old_ema(target_ulps: int, pulls: int) -> list[int]:
    """Return the old absolute EMA state in ULPs above 2.0."""
    ulp = np.spacing(BASE)
    target = F32(BASE + F32(target_ulps) * ulp)
    state = BASE
    out: list[int] = []
    for _ in range(pulls):
        state = F32(state + F32(BETA * F32(target - state)))
        out.append(round(float(F32(state - BASE) / ulp)))
    return out


def split_ema(target_ulps: int, pulls: int) -> list[float]:
    """Return the correction-domain EMA in absolute-step ULP units."""
    ulp = np.spacing(BASE)
    target_corr = F32(F32(target_ulps) * ulp)
    corr = F32(0.0)
    out: list[float] = []
    for _ in range(pulls):
        corr = F32(corr + F32(BETA * F32(target_corr - corr)))
        out.append(float(corr / ulp))
    return out


def signed_q31(value: np.float32) -> int:
    return int(F32(value * Q31_SCALE))


def split_step_q31(base: np.float32, corr: np.float32) -> tuple[int, int]:
    """Mirror asrc_q31_step_of(): return (whole, unsigned Q1.31 frac)."""
    whole = int(F32(base + F32(0.5)))
    total = signed_q31(F32(base - F32(whole))) + signed_q31(corr)
    while total < 0:
        total += Q31_PERIOD
        whole -= 1
    while total >= Q31_PERIOD:
        total -= Q31_PERIOD
        whole += 1
    return whole, total


def check_model(mutant: str | None = None) -> list[str]:
    failures: list[str] = []
    ulp = float(np.spacing(BASE))
    threshold_ulps = 0.5 / float(BETA)

    stuck = old_ema(99, 16)
    if mutant == "old-not-stuck":
        stuck[0] = 1
    if any(stuck):
        failures.append(f"old EMA should stick for a 99-ULP target: {stuck}")

    one_then_stuck = old_ema(101, 16)
    if one_then_stuck[:3] != [1, 2, 2] or len(set(one_then_stuck[2:])) != 1:
        failures.append(f"old EMA stick/slip signature changed: {one_then_stuck}")

    burst = old_ema(1400, 2)
    if burst != [7, 14]:
        failures.append(f"old EMA no longer reproduces a 14-ULP two-pull burst: {burst}")

    split = split_ema(14, 10)
    if mutant == "split-rounded":
        split = [float(round(x)) for x in split]
    if not (0.0 < split[0] < 0.5 and split[7] > 0.5 and split[-1] < 1.0):
        failures.append(f"split EMA lost sub-ULP motion: {split}")

    corr = F32(split[0] * ulp)
    whole, frac = split_step_q31(BASE, corr)
    collapsed = F32(BASE + corr)
    collapsed_whole, collapsed_frac = split_step_q31(collapsed, F32(0.0))
    if mutant == "sum-before-q31":
        whole, frac = collapsed_whole, collapsed_frac
    if (whole, frac) == (collapsed_whole, collapsed_frac):
        failures.append("split Q31 conversion did not preserve a sub-ULP correction")
    if whole != 2 or frac <= 0:
        failures.append(f"unexpected positive split-step encoding: whole={whole} frac={frac}")

    neg_whole, neg_frac = split_step_q31(BASE, F32(-corr))
    if neg_whole != 1 or not (0 < neg_frac < Q31_PERIOD):
        failures.append(
            f"negative correction normalization failed: whole={neg_whole} frac={neg_frac}"
        )

    print(
        "binary32 step ULP at 2.0: %.9g (%.6f ppm); beta dead zone: %.3f ULP"
        % (ulp, ulp / 2.0 * 1.0e6, threshold_ulps)
    )
    print(f"old 101-ULP target: {one_then_stuck[:8]}")
    print(f"old 1400-ULP target: {burst} (matches a 14-ULP two-pull transition)")
    print("split 14-ULP target: " + ", ".join(f"{x:.6f}" for x in split[:10]))
    print(
        "first split update: q31=(%d,%d), sum-first q31=(%d,%d)"
        % (whole, frac, collapsed_whole, collapsed_frac)
    )
    return failures


SOURCE_CHECKS = (
    (SOURCE, r"float\s+step_smooth_corr\s*;", "split correction state"),
    (SOURCE, r"float\s+step_corr_state\s*;", "correction-domain slew state"),
    (
        SOURCE,
        r"target_corr\s*=\s*ratio\s*\*\s*corr_used",
        "correction-domain servo target",
    ),
    (
        SOURCE,
        r"delta\s*=\s*target_corr\s*-\s*a->step_corr_state",
        "correction-domain slew",
    ),
    (
        SOURCE,
        r"target_corr\s*=\s*\(\s*step_base\s*-\s*a->step_smooth_st\s*\)\s*\+\s*step_corr",
        "split smoother target",
    ),
    (
        SOURCE,
        r"step_smooth_corr\s*\+=\s*ss_beta\s*\*\s*\(\s*target_corr\s*-\s*a->step_smooth_corr\s*\)",
        "correction-domain EMA",
    ),
    (SOURCE, r"asrc_q31_step_of\(\s*step_base\s*,\s*step_corr\s*\)", "split Q31 conversion"),
    (SOURCE, r"asrc_q31_phase_advance\(", "fixed-point phase advance"),
    (Q31_SOURCE, r"asrc_q31_phase_of_fixed\(", "fixed-point Q31 phase select"),
)


def check_source(mutant: str | None = None) -> list[str]:
    failures: list[str] = []
    for path, pattern, label in SOURCE_CHECKS:
        text = path.read_text(encoding="utf-8")
        found = bool(re.search(pattern, text))
        if mutant == "source-missing" and label == "split Q31 conversion":
            found = False
        print(f"source {label:30s}: {'PASS' if found else 'FAIL'}")
        if not found:
            failures.append(f"source does not contain {label}: {path.name}")
    return failures


def run(mutant: str | None = None) -> list[str]:
    failures = check_model(mutant)
    failures.extend(check_source(mutant))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--mutate",
        choices=("old-not-stuck", "split-rounded", "sum-before-q31", "source-missing"),
    )
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    failures = run(args.mutate)
    if args.mutate:
        print(f"mutation {args.mutate}: {len(failures)} failure(s)")
        return 0 if failures else 1
    if failures:
        print("FAIL:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("PASS: split smoother and fixed-point phase preserve sub-ULP control motion")

    if args.selftest:
        print("negative tests:")
        undetected: list[str] = []
        for mutant in ("old-not-stuck", "split-rounded", "sum-before-q31", "source-missing"):
            caught = bool(run(mutant))
            print(f"  {mutant:18s}: {'PASS (detected)' if caught else 'FAIL'}")
            if not caught:
                undetected.append(mutant)
        if undetected:
            print("FAIL: undetected mutations: " + ", ".join(undetected))
            return 1
        print("negative tests PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
