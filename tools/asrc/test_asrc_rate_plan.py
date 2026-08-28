#!/usr/bin/env python3
"""Host acceptance for the general ASRC feed-forward rate-plan equation."""

from __future__ import annotations

import math


FIFO_FRAMES = 128
FIFO_GUARD = FIFO_FRAMES - 4
BLOCK_FRAMES = 16
POLY_AHEAD_AND_CURRENT = 17


def rate_plan_step(
    source_hz: float, destination_hz: float, fixed_output_num: int, fixed_input_den: int
) -> float:
    if source_hz <= 0.0 or destination_hz <= 0.0:
        return 0.0
    if fixed_output_num <= 0 or fixed_input_den <= 0:
        return 0.0
    return (source_hz / destination_hz) * fixed_output_num / fixed_input_den


def check(label: str, actual: float, expected: float, tolerance: float = 1.0e-9) -> None:
    if not math.isclose(actual, expected, rel_tol=tolerance, abs_tol=tolerance):
        raise AssertionError(f"{label}: {actual:.12f} != {expected:.12f}")
    print(f"{label:28s} step={actual:.9f}")


def runtime_frontend_denominator(b_rate_hz: int) -> int:
    """Mirror production: /6 at 8 kHz, /3 at 11.025 kHz, direct otherwise."""
    if b_rate_hz == 8000:
        return 6
    if b_rate_hz == 11025:
        return 3
    return 1


def direct_fifo_burst_fits(step: float) -> bool:
    """Conservative burst bound including one producer block of phase jitter."""
    consumed = math.ceil(step * BLOCK_FRAMES)
    required = consumed + POLY_AHEAD_AND_CURRENT + BLOCK_FRAMES
    return required <= FIFO_GUARD


def main() -> int:
    check("direct 48k -> 48k", rate_plan_step(48000.0, 48000.0, 1, 1), 1.0)
    check(
        "direct 48k -> 44.1k",
        rate_plan_step(48000.0, 44100.0, 1, 1),
        48000.0 / 44100.0,
    )
    check("direct 48k -> 8k", rate_plan_step(48000.0, 8000.0, 1, 1), 6.0)
    check("decim /6, 48k -> 8k", rate_plan_step(48000.0, 8000.0, 1, 6), 1.0)
    check(
        "decim /6, measured clocks",
        rate_plan_step(48000.0, 7999.98, 1, 6),
        1.00000250000625,
    )
    check("decim /3, 48k -> 8k", rate_plan_step(48000.0, 8000.0, 1, 3), 2.0)
    check("interp x2, 48k -> 48k", rate_plan_step(48000.0, 48000.0, 2, 1), 2.0)
    # 48 -> 32 kHz AUDIO MODE front end (L=2/M=3): the pair, not the denominator alone, is what
    # makes the rear resampler run at step 1.0.  Feeding den=3 with num=1 -- the /3 row's shape --
    # would ask it for 0.5, which is the failure this case exists to catch.
    check("2/3 front end, 48k -> 32k", rate_plan_step(48000.0, 32000.0, 2, 3), 1.0)
    check("den alone would be wrong", rate_plan_step(48000.0, 32000.0, 1, 3), 0.5)
    check("invalid denominator", rate_plan_step(48000.0, 8000.0, 1, 0), 0.0)

    check(
        "runtime path at B=48k",
        rate_plan_step(48000.0, 48000.0, 1, runtime_frontend_denominator(48000)),
        1.0,
    )
    check(
        "runtime path at B=8k",
        rate_plan_step(48000.0, 8000.0, 1, runtime_frontend_denominator(8000)),
        1.0,
    )
    check(
        "runtime path at B=11k025",
        rate_plan_step(48000.0, 11025.0, 1, runtime_frontend_denominator(11025)),
        16000.0 / 11025.0,
    )
    check(
        "runtime scope B=12k direct",
        rate_plan_step(48000.0, 12000.0, 1, runtime_frontend_denominator(12000)),
        4.0,
    )

    assert direct_fifo_burst_fits(4.0), "step 4 must retain FIFO burst margin"
    assert not direct_fifo_burst_fits(6.0), "step 6 must expose the legacy FIFO discontinuity"
    assert direct_fifo_burst_fits(1.0), "fixed /6 front end must restore near-unity FIFO margin"
    print("FIFO geometry: step4=fit step6=unsafe fixed-step1=fit")

    print("PASS: general ASRC rate-plan equation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
