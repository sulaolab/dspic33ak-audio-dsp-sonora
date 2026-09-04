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


# --- the *ar pair gate, as audio_app_asrc.c implements it (AK512: FIFO 128, BLOCK 16) ----------
#
# Mirrors asrc_burst_ratio_fits() and asrc_fill_slack_fits() in integers, so the routing table
# below is checked against the SAME arithmetic the firmware refuses pairs with, rather than against
# a comment.  Written when 96 k -> 22.05 kHz was admitted (2026-09-02): that pair is the one the
# soft bound rejects on the direct path, and the whole point of its composed /4 row is to move it.
GATE_AHEAD = 15            # ASRC_POLY_AHEAD
GATE_JITTER = 4            # ASRC_FILL_JITTER
GATE_TARGET = 64           # ASRC_FILL_TARGET  = ASRC_FIFO_FRAMES / 2
GATE_TARGET_MAX = 104      # ASRC_FIFO_FRAMES - 4 - APP_BLOCK_FRAMES - ASRC_FILL_JITTER
GATE_SLACK_REQUIRED = 8    # ASRC_FILL_SLACK_REQUIRED (measured, 100 pairs)
GATE_BURST_NUM = 128 - 4 - GATE_AHEAD   # ASRC_BURST_RATIO_LIMIT_NUM
GATE_BURST_DEN = BLOCK_FRAMES - 1       # ASRC_BURST_RATIO_LIMIT_DEN


def gate_accepts(fs_in_hz: int, num: int, den: int, fs_out_hz: int) -> tuple[bool, str]:
    """(accepted, why) for one direction, exactly as the two C predicates decide it."""
    eff_num = fs_in_hz * num
    eff_den = den * fs_out_hz
    if eff_num <= eff_den:
        # Both predicates return true here before any arithmetic: at step <= 1 the look-ahead R only
        # shrinks, so the slack only grows.  Covers every up-conversion AND every composed chain the
        # front end lands exactly on 1.0.
        return True, "step <= 1"
    if eff_num * GATE_BURST_DEN >= GATE_BURST_NUM * eff_den:
        return False, "burst: ring cannot hold the look-ahead"
    k = BLOCK_FRAMES - 1
    scale = eff_num * k
    r = (scale // eff_den) + GATE_AHEAD + 1
    setpoint = min(max(r + GATE_JITTER, GATE_TARGET), GATE_TARGET_MAX)
    if setpoint <= r:
        return False, f"slack: set {setpoint} <= R {r}"
    if (setpoint - r) >= GATE_SLACK_REQUIRED:
        return True, f"slack {setpoint - r} (R {r}, set {setpoint})"
    if scale % eff_den == 0:
        return True, f"slack {setpoint - r} but exact step (R {r})"
    return False, f"slack {setpoint - r} < {GATE_SLACK_REQUIRED} and step not exact (R {r})"


def frontend_denominator_96k(b_rate_hz: int) -> int:
    """The 96 kHz row of the routing table in asrc_audio_path_frontend_plan().

    44100 and 48000 are den 2 -- the pre-stage ALONE, no second stage -- and they are the only
    rows whose pre-stage coefficient set is not the shared one (see PRE_VARIANTS below).
    32000 still has no row; it is held until the 48 -> 32 kHz N=97 decision closes.
    """
    return {8000: 12, 11025: 8, 12000: 8, 16000: 6, 22050: 4, 24000: 4,
            44100: 2, 48000: 2}.get(b_rate_hz, 1)


def prestage_variant_96k(b_rate_hz: int) -> str:
    """Which pre-stage coefficient set the 96 kHz row for `b_rate_hz` loads.

    Mirrors the arm in path_decimator_init(): a composed chain always takes the shared set, and
    the two rows where the pre-stage is the whole chain take a wide set named for the FINAL rate.
    A row with den != 1 and no entry here would be a chain running an unnamed filter, so this is
    exhaustive over the table above rather than defaulted.
    """
    den = frontend_denominator_96k(b_rate_hz)
    if den == 1:
        return "none"
    return {44100: "FOR_44100", 48000: "FOR_48000"}.get(b_rate_hz, "SHARED")


def gate_accepts_with_width(fs_in_hz: int, num: int, den: int, fs_out_hz: int,
                           frontend_serves_width: bool) -> tuple[bool, str]:
    """The pair gate including the front-end WIDTH DISQUALIFICATION.

    An implementation that cannot carry ASRC_CH is not used at a reduced width: the pairs that need
    a front end are refused, and the pairs that need none are unaffected.  There is no
    acknowledgement macro to model, on purpose -- the firmware has no override either.
    """
    if ((num != 1) or (den != 1)) and not frontend_serves_width:
        return False, "front end needed but the implementation is narrower than ASRC_CH"
    return gate_accepts(fs_in_hz, num, den, fs_out_hz)


# ---------------------------------------------------------------------------
# The Q31 front end's composed-chain GEOMETRY, mirroring the _Static_asserts in
# asrc_decimator_q31.inc.  A 96 kHz leg puts a fixed /2 pre-stage in front of one of
# the 48 kHz chains, so the front end's per-channel history arena is now two parts
# and the deepest chain is three stages.  Duplicated here on purpose: the C asserts
# fail at build time on the real device, and this fails on the host for anyone who
# changes the block size or a tap count without building.
# ---------------------------------------------------------------------------
Q31_SECOND_ARENA = 209        # the 48 kHz chains' fixed budget, samples/channel
Q31_MAX_MID = 8               # frames per inter-stage buffer
Q31_MAX_BATCH = 16            # outputs per kernel call
Q31_COEFF_MAX_BASE = 211      # X working buffer, before the pre-stage's reserve
PRE_TAPS = 27                 # ASRC_DECIMATOR_96_TO_48_TAPS, the SHARED set
PRE_DECIM = 2
# The wide variants, which are never composed: each serves a chain whose second stage is empty,
# so its ring sits at offset 0 and spends the 48 kHz chains' arena instead of the reserve above
# it.  That is why adding them cost zero history RAM, and it is what the geometry check below
# asserts -- ring at 0 must fit the WHOLE arena, and the coefficients must fit the X buffer with
# no room reserved for a second stage.
PRE_VARIANTS = {"SHARED": 27, "FOR_44100": 113, "FOR_48000": 169}
# The 48 kHz chains, as (name, [(taps, decim, ring, ring_off), ...]).
Q31_CHAINS = {
    2: [(107, 2, Q31_SECOND_ARENA, 0)],
    3: [(161, 3, Q31_SECOND_ARENA, 0)],
    4: [(27, 2, 43, 0), (129, 2, 147, 43)],
    6: [(43, 3, 58, 0), (147, 2, 151, 58)],
}


def check_q31_composed_geometry() -> None:
    pre_out = -(-BLOCK_FRAMES // PRE_DECIM)          # ceil, one call per block
    pre_ring = PRE_TAPS + (pre_out - 1) * PRE_DECIM
    hist = Q31_SECOND_ARENA + pre_ring
    assert pre_ring >= PRE_TAPS, "a reserve shorter than its tap count is not a ring"
    assert pre_out <= Q31_MAX_BATCH, "the pre-stage batch must fit the kernel scratch"
    assert pre_out <= Q31_MAX_MID, "the pre-stage's block output must fit one buffer"
    print(f"q31 pre-stage: taps={PRE_TAPS} decim={PRE_DECIM} out/block={pre_out} "
          f"ring={pre_ring} arena={Q31_SECOND_ARENA}+{pre_ring}={hist}")

    coeff_max = Q31_COEFF_MAX_BASE + PRE_TAPS
    for den, stages in sorted(Q31_CHAINS.items()):
        # Every ring must hold its own taps, and no ring may reach into the reserve.
        for taps, decim, ring, off in stages:
            assert taps <= ring, f"/{den}: ring {ring} shorter than {taps} taps"
            assert off + ring <= Q31_SECOND_ARENA, (
                f"/{den}: ring at {off}+{ring} runs into the pre-stage reserve")
        # Composed: pre + this chain.  Stage count, coefficients, and the frame
        # count each intermediate buffer has to hold.
        n_stages = 1 + len(stages)
        assert n_stages <= 1 + 2, f"composed /{den * 2} is deeper than three stages"
        assert PRE_TAPS + sum(t for t, _, _, _ in stages) <= coeff_max, (
            f"composed /{den * 2} coefficients exceed the X buffer")
        n = pre_out
        shape = [f"pre /{PRE_DECIM}"]
        for taps, decim, _, _ in stages[:-1]:
            n = n // decim
            assert n <= Q31_MAX_MID, (
                f"composed /{den * 2}: {n} intermediate frames exceed {Q31_MAX_MID}")
            shape.append(f"/{decim}")
        shape.append(f"/{stages[-1][1]}")
        out = n // stages[-1][1]
        print(f"q31 composed /{den * 2:<2d} = {' + '.join(shape):<18} "
              f"stages={n_stages} out/block={out} "
              f"coeff={PRE_TAPS + sum(t for t, _, _, _ in stages)}/{coeff_max}")
    # The pre-stage alone (96 -> 48 kHz, nothing behind it), once per variant.  The wide ones are
    # the whole chain, so they are checked against the whole arena and against a coefficient
    # buffer with nothing else in it -- not against the 41-sample reserve, which is the mistake
    # that would show up as wrong audio rather than as a build error.
    assert pre_out <= Q31_MAX_MID + 1, "pre-only output must fit the caller's scratch"
    for name, taps in sorted(PRE_VARIANTS.items(), key=lambda kv: kv[1]):
        ring = taps + (pre_out - 1) * PRE_DECIM
        off = Q31_SECOND_ARENA if name == "SHARED" else 0
        assert taps <= ring, f"{name}: ring {ring} shorter than {taps} taps"
        assert off + ring <= hist, (
            f"{name}: ring at {off}+{ring} runs past the {hist}-sample arena")
        assert taps <= coeff_max, f"{name}: {taps} coefficients exceed the X buffer"
        assert taps % 2 == 1, f"{name}: the symmetric FIR helper needs an odd tap count"
        print(f"q31 pre-stage alone {name:<9} taps={taps:3d} ring={ring:3d} at {off:3d} "
              f"stages=1 out/block={pre_out} coeff={taps}/{coeff_max}")
    assert PRE_VARIANTS["SHARED"] == PRE_TAPS, "the shared set is the composed chains' set"
    assert max(PRE_VARIANTS.values()) == PRE_VARIANTS["FOR_48000"], (
        "storage is sized by the widest variant, which is the 48 kHz one")

def check_gate(label: str, fs_in_hz: int, fs_out_hz: int, den: int, expect: bool) -> None:
    accepted, why = gate_accepts(fs_in_hz, 1, den, fs_out_hz)
    verdict = "accept" if accepted else "REFUSE"
    print(f"gate {label:28s} den={den:<3d} {verdict}  ({why})")
    assert accepted == expect, (
        f"{label}: gate {verdict}s with den {den}, expected "
        f"{'accept' if expect else 'refuse'} -- {why}"
    )


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

    # Every 96 kHz down-conversion the rate menu offers, judged with the table's own denominator.
    for b_hz in (8000, 11025, 12000, 16000, 22050, 24000):
        check_gate(f"96k -> {b_hz}", 96000, b_hz, frontend_denominator_96k(b_hz), True)
    # 22.05 kHz is the row this block exists for: refused on the direct path, accepted composed.
    check_gate("96k -> 22050 (no row)", 96000, 22050, 1, False)
    check_gate("96k -> 22050 (composed /4)", 96000, 22050, 4, True)
    # 24 kHz passed even with no row -- but only via the exact-step exemption, on 4 frames of slack.
    # Both facts are asserted so the composed row is seen to REPLACE a dependency, not add one.
    check_gate("96k -> 24000 (no row)", 96000, 24000, 1, True)
    check_gate("96k -> 24000 (composed /4)", 96000, 24000, 4, True)
    # 44.1 and 48 kHz JOINED ON 2026-09-03 (den 2, the pre-stage alone).  Both were accepted
    # before the row and are accepted with it -- the row is not there to fix a refusal, it is
    # there because the direct path had NO BAND LIMIT.  The gate says nothing about band limiting,
    # so what these two assertions carry is that the row does not cost the pair its acceptance.
    check_gate("96k -> 44100 (no row)", 96000, 44100, 1, True)
    check_gate("96k -> 44100 (pre-stage)", 96000, 44100, 2, True)
    check_gate("96k -> 48000 (no row)", 96000, 48000, 1, True)
    check_gate("96k -> 48000 (pre-stage)", 96000, 48000, 2, True)
    # 32 kHz still has no row: it leans on the exact-step exemption and runs unprotected.
    for b_hz in (32000, 44100, 48000, 96000):
        check_gate(f"96k -> {b_hz}", 96000, b_hz, frontend_denominator_96k(b_hz), True)
    # Every 96 kHz row names the coefficient set it loads.  A row that resolves to a bare
    # denominator with no set behind it is a chain filtering for the wrong band.
    for b_hz in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000):
        den = frontend_denominator_96k(b_hz)
        vpre = prestage_variant_96k(b_hz)
        print(f"96k -> {b_hz:<6d} den={den:<3d} pre-stage={vpre}")
        assert (den == 1) == (vpre == "none"), f"96k -> {b_hz}: a row must name its set"
        if vpre != "none":
            assert vpre in PRE_VARIANTS, f"96k -> {b_hz}: unknown set {vpre}"
            # A wide set is only ever the whole chain: composed den must be exactly the
            # pre-stage's own /2, i.e. nothing behind it.
            assert (vpre == "SHARED") == (den != PRE_DECIM), (
                f"96k -> {b_hz}: a wide set must be den {PRE_DECIM} and SHARED must not be")
    # Up-conversion: both predicates return true before any arithmetic, at every rate.
    for a_hz in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
        check_gate(f"{a_hz} -> 96k", a_hz, 96000, 1, True)

    # Width disqualification.  A build whose front-end implementation is narrower than ASRC_CH must
    # refuse exactly the pairs that need a front end, and must leave the others alone.
    for b_hz in (8000, 11025, 12000, 16000, 22050, 24000, 44100, 48000):
        den = frontend_denominator_96k(b_hz)
        ok, why = gate_accepts_with_width(96000, 1, den, b_hz, frontend_serves_width=False)
        print(f"width 96k -> {b_hz:<6d} den={den:<3d} {'accept' if ok else 'REFUSE'}  ({why})")
        assert not ok, f"96k -> {b_hz} needs a front end and must be refused when it is too narrow"
        ok, _ = gate_accepts_with_width(96000, 1, den, b_hz, frontend_serves_width=True)
        assert ok, f"96k -> {b_hz} must be accepted once the front end carries ASRC_CH"
    # Pairs that need no front end are untouched by the disqualification.  96k -> 44.1/48 kHz
    # LEFT THIS LIST on 2026-09-03: they have rows now, so they are in the loop above instead.
    # Which is the behaviour change worth stating plainly -- a narrow front end no longer lets
    # those two pairs through unprotected, it refuses them, the same as every other 96 kHz row.
    for a_hz, b_hz in ((96000, 96000), (8000, 96000), (48000, 96000)):
        ok, why = gate_accepts_with_width(a_hz, 1, 1, b_hz, frontend_serves_width=False)
        print(f"width {a_hz} -> {b_hz:<6d} den=1   {'accept' if ok else 'REFUSE'}  ({why})")
        assert ok, f"{a_hz} -> {b_hz} needs no front end and must not be refused for its width"

    check_q31_composed_geometry()

    print("PASS: general ASRC rate-plan equation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
