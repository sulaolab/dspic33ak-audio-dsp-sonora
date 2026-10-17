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
# Mirrors asrc_burst_ratio_fits() and asrc_fill_slack_fits()/asrc_fill_law() in integers, so the
# routing table below is checked against the SAME arithmetic the firmware refuses pairs with, rather
# than against a comment.
#
# THE SOFT BOUND CHANGED ON 2026-09-12 (A1, the lower-side producer-block reserve).  It used to be
#   R        = floor(step*(BLOCK-1)) + AHEAD + 1
#   set      = clamp(R + JITTER, TARGET, TARGET_MAX)
#   accepted = set > R and (set-R >= 8 or fs_in*(BLOCK-1) % fs_out == 0)   # exact-step exemption
# and the 8 was measured on 48 <-> 44.1 kHz, whose 147/160 ratio gives 160 block-phase classes, i.e.
# the DENSE limit BLOCK/2.  A pair on a low-order rational loses a whole producer BLOCK instead, so
# 32 -> 12 kHz (8/3) starved on it; the exemption made it worse by waving through exactly the q = 1
# pairs, which are the deepest case.  See asrc_fill_law() and
# [internal] ak512_32to12_starve_analysis_2026-09-11.md.
GATE_AHEAD = 15            # ASRC_POLY_AHEAD
GATE_JITTER = 4            # ASRC_FILL_JITTER -- UPPER bound only since 2026-09-12
GATE_TARGET = 64           # ASRC_FILL_TARGET  = ASRC_FIFO_FRAMES / 2
GATE_TARGET_MAX = 104      # ASRC_FIFO_FRAMES - 4 - APP_BLOCK_FRAMES - ASRC_FILL_JITTER
GATE_RESERVE = BLOCK_FRAMES + 1   # ASRC_FILL_BLOCK_RESERVE: one producer block + 1
GATE_BURST_NUM = 128 - 4 - GATE_AHEAD   # ASRC_BURST_RATIO_LIMIT_NUM
GATE_BURST_DEN = BLOCK_FRAMES - 1       # ASRC_BURST_RATIO_LIMIT_DEN


def fill_law(fs_in_hz: int, num: int, den: int, fs_out_hz: int) -> tuple[int, int, int, int, bool]:
    """(look_safe, r_safe, set_required, set, fits) -- asrc_fill_law(), integer for integer.

    floor(nominal)+1 is ceil() where the product is fractional and ceil+1 where it is an integer,
    which is the frame the servo's upward trim needs.  No step <= 1 shortcut: an up-conversion needs
    the producer-block reserve too, and the expression already collapses for it.
    """
    eff_num = fs_in_hz * num
    eff_den = den * fs_out_hz
    look_safe = (eff_num * (BLOCK_FRAMES - 1)) // eff_den + 1
    r_safe = look_safe + GATE_AHEAD + 1
    set_required = r_safe + GATE_RESERVE
    setpoint = max(set_required, GATE_TARGET)
    return look_safe, r_safe, set_required, setpoint, setpoint <= GATE_TARGET_MAX


# The DIRECT-ROUTE ANTI-ALIAS POLICY (audio_app_asrc.c s_asrc_unsafe_direct_routes[]).
#
# Not a safety rule and no longer a temporary one.  Two independent facts are asserted about this
# same table below, and keeping them separate is the point:
#
#   HISTORY   it is exactly the set A1 newly admitted -- checked against a re-derivation of the
#             pre-A1 accepted set (legacy_gate_accepts below), so the table cannot drift.
#   REASON    the DIRECT route of each of these pairs reads a worst in-band unwanted component of
#             0.00 dBc on host (tools/asrc/asrc_direct_route_policy_matrix.py, 2026-09-15), so it
#             is refused for a reason that does not expire when step > 2.07 is qualified.
#
# And the object refused is a ROUTE, not a physical pair: check_direct_route_policy() asserts both
# directions of that for every entry.
FENCE_UNQUALIFIED_DIRECT = frozenset({
    (22050, 8000), (32000, 11025), (44100, 12000),
    (44100, 16000), (48000, 11025), (96000, 22050),
})

# The FILTERED route each of those physical pairs has in this tree, as (num, den) exactly as
# asrc_audio_path_frontend_plan() publishes it, or None when the pair has no front-end row at all.
# A pair with no row is not a hole in the policy -- it is a pair whose only route today is the one
# the policy refuses, which is what makes "physical pair != route" worth asserting per entry.
FILTERED_ROUTE = {
    # UPDATED 2026-09-19/20 as each row landed.  Three entries that read None until then now name a
    # route, and that is a DELIBERATE WIDENING of the accepted pair set -- stated here as a
    # specification rather than absorbed silently.  What did NOT change is the direct-route table
    # above: every one of these pairs is still refused on its DIRECT route, and each row is exempt
    # because a filtered route is a different (num, den).  check_optin_pair_gates() asserts, per row
    # and for the union, exactly which ordered cells each one adds.
    (22050, 8000): (1, 2),               # /2 + 1:1@11.025k, APP_ASRC_ANTIALIAS_22K05_TO_8K (PR #36)
    (32000, 11025): (1, 2),              # /2 + 1:1@16k,     APP_ASRC_ANTIALIAS_32K_TO_11K025
    (44100, 12000): (1, 2),              # /2 + 1:1@22.05k,  APP_ASRC_ANTIALIAS_44K1_TO_12K
    (44100, 16000): (1, 2),              # /2 + 1:1 composed, APP_ASRC_LPF1TO1_PROTOTYPE
    (48000, 11025): (1, 4),              # 48 kHz divider table
    (96000, 22050): (1, 4),              # 96 kHz divider table -- accepted before A1 and still
}

# THE OPT-IN FRONT-END ROWS, as a specification of which cells each one adds.
#
# WHY THIS TABLE EXISTS RATHER THAN A WEAKENED ASSERTION.  check_qualification_fence() asserts that
# the post-A1 accepted set equals the pre-A1 one, and it still does -- it evaluates the DIRECT route
# only, and no row here touches a direct route.  So the historical property is intact and needed no
# relaxing.  What DID need stating is the new property: each opt-in row admits exactly two ordered
# cells, and the union of the rows admits exactly the union of those pairs.
#
# `blocker` records WHICH gate refused the pair before its row, because the two are asserted
# differently.  An "alias" pair must STAY in FENCE_UNQUALIFIED_DIRECT, because its direct route is
# still refused on spectral grounds.  The "fill" pair was never in that table and must not be added
# to it: its direct step was refused by asrc_fill_law(), and the row LOWERS THE STEP rather than
# relaxing the law -- putting it in the anti-alias table would state the wrong reason.
OPTIN_FRONTEND_ROWS = {
    "APP_ASRC_ANTIALIAS_22K05_TO_8K":   dict(pair=(22050, 8000),  route=(1, 2), blocker="alias"),
    "APP_ASRC_ANTIALIAS_32K_TO_11K025": dict(pair=(32000, 11025), route=(1, 2), blocker="alias"),
    "APP_ASRC_ANTIALIAS_44K1_TO_12K":   dict(pair=(44100, 12000), route=(1, 2), blocker="alias"),
    "APP_ASRC_ANTIALIAS_44K1_TO_8K":    dict(pair=(44100, 8000),  route=(1, 4), blocker="fill"),
}


def fence_allows(fs_in_hz: int, num: int, den: int, fs_out_hz: int) -> bool:
    """Direct path only -- a filtered row is a different effective step and was already qualified."""
    if (num, den) != (1, 1):
        return True
    return (fs_in_hz, fs_out_hz) not in FENCE_UNQUALIFIED_DIRECT


def legacy_gate_accepts(fs_in_hz: int, num: int, den: int, fs_out_hz: int) -> bool:
    """The PRE-A1 soft bound, for one purpose only: proving the fence freezes the surface exactly.

    This is the arithmetic A1 deleted -- setpoint pinned at R + JITTER, a measured-slack threshold of
    8, and the exact-step exemption.  It lives in the TEST because the property under test is
    historical ("the accepted set did not change"), and it must NOT live in the firmware, where it
    would resurrect the gate A1 replaced and state it as a safety rule again.
    """
    eff_num = fs_in_hz * num
    eff_den = den * fs_out_hz
    if eff_num <= eff_den:
        return True
    if eff_num * GATE_BURST_DEN >= GATE_BURST_NUM * eff_den:
        return False
    k = BLOCK_FRAMES - 1
    scale = eff_num * k
    r = (scale // eff_den) + GATE_AHEAD + 1
    setpoint = min(max(r + GATE_JITTER, GATE_TARGET), GATE_TARGET_MAX)
    if setpoint <= r:
        return False
    if (setpoint - r) >= 8:            # the retired ASRC_FILL_SLACK_REQUIRED
        return True
    return scale % eff_den == 0        # the deleted exact-step exemption


def check_qualification_fence() -> None:
    """A1 fixes starve on the pairs already supported; it must not add or remove any."""
    rates = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)
    before, after, fenced = set(), set(), set()
    for fs_in in rates:
        for fs_out in rates:
            if legacy_gate_accepts(fs_in, 1, 1, fs_out):
                before.add((fs_in, fs_out))
            law_ok, _why = gate_accepts(fs_in, 1, 1, fs_out)
            if law_ok and fence_allows(fs_in, 1, 1, fs_out):
                after.add((fs_in, fs_out))
            elif law_ok:
                fenced.add((fs_in, fs_out))
    print(f"fence: pre-A1 accepted {len(before)}  post-A1+fence accepted {len(after)}  "
          f"held out {len(fenced)}")
    assert after == before, (
        f"A1 must not change the accepted set: newly admitted {sorted(after - before)}, "
        f"lost {sorted(before - after)}")
    assert fenced == FENCE_UNQUALIFIED_DIRECT, (
        f"the fence table is not the set the law newly admits: table-only "
        f"{sorted(FENCE_UNQUALIFIED_DIRECT - fenced)}, missing {sorted(fenced - FENCE_UNQUALIFIED_DIRECT)}")
    print(f"fence: held out exactly {sorted(fenced)}")


def check_direct_route_policy() -> None:
    """The policy refuses a ROUTE.  Assert both axes, for every entry -- not just one pair.

    Axis 1 (route): the direct route of each listed pair is refused, and a filtered route for the
    SAME physical pair is allowed.  A regression that fenced on the physical rates alone would
    remove a qualified configuration (96 -> 22.05 kHz /4 is the one that already shipped), and a
    regression that dropped the num/den test would do it silently.

    Axis 2 (pair): no pair OUTSIDE the table is refused on either route, so the policy cannot
    quietly grow.
    """
    for pair in sorted(FENCE_UNQUALIFIED_DIRECT):
        fs_in, fs_out = pair
        assert not fence_allows(fs_in, 1, 1, fs_out), f"{pair}: the DIRECT route must be refused"
        route = FILTERED_ROUTE[pair]
        if route is None:
            # Kept for a FUTURE table entry with no row yet.  As of 2026-09-20 every entry names a
            # route, so this branch is unreached -- deleting it would mean the next pair added to
            # the table silently loses its shape assertion.
            # No row today.  Still assert the shape: were a route added, it must not be refused
            # by this table, because the table is about routes and not about rates.
            assert fence_allows(fs_in, 1, 2, fs_out), (
                f"{pair}: a hypothetical filtered route must not be refused by the direct policy")
            print(f"policy: {fs_in:6d} -> {fs_out:6d}  direct REFUSE, no filtered route in tree")
            continue
        num, den = route
        assert fence_allows(fs_in, num, den, fs_out), (
            f"{pair}: the filtered route {num}/{den} must be ALLOWED -- refusing it removes a "
            f"qualified configuration")
        print(f"policy: {fs_in:6d} -> {fs_out:6d}  direct REFUSE, filtered {num}/{den} ALLOW")

    rates = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)
    for fs_in in rates:
        for fs_out in rates:
            if (fs_in, fs_out) in FENCE_UNQUALIFIED_DIRECT:
                continue
            for num, den in ((1, 1), (1, 2), (1, 4)):
                assert fence_allows(fs_in, num, den, fs_out), (
                    f"({fs_in}, {fs_out}) route {num}/{den} is not in the table and must not be "
                    f"refused by it")
    print("policy: no pair outside the table is refused on any route")


def pair_accepted(fs_in: int, fs_out: int, rows_on: frozenset) -> bool:
    """Is this ORDERED cell accepted, with a named set of opt-in rows enabled?

    Mirrors what audio_app_asrc_rate_pair_is_supported() composes: the planner picks a route for the
    pair, and the fill law and the anti-alias policy then judge THAT route.  Only a row in `rows_on`
    publishes a route; every other pair runs direct.

    BOTH DIRECTIONS of the physical pair are judged, because the firmware instantiates an engine per
    direction and refuses the pair if either fails.  That is exactly why a row has to be asserted to
    move two ordered cells rather than one.
    """
    def route_for(a, b):
        for macro, spec in OPTIN_FRONTEND_ROWS.items():
            if macro not in rows_on:
                continue
            if (a, b) == spec["pair"]:
                return spec["route"]
        return (1, 1)

    for a, b in ((fs_in, fs_out), (fs_out, fs_in)):
        num, den = route_for(a, b)
        law_ok, _why = gate_accepts(a, num, den, b)
        if not law_ok:
            return False
        if not fence_allows(a, num, den, b):
            return False
    return True


def check_optin_pair_gates() -> None:
    """Each opt-in row adds EXACTLY its own two ordered cells; the union adds exactly the union.

    Four properties, each able to fail on its own:
      1  with every row off, the accepted set is the legacy one
      2  each row alone adds exactly two ordered cells -- its pair, both orientations
      3  all rows on adds exactly the union, so no row shadows another in either if-chain
      4  every pair keeps the DIRECT verdict it had, from the gate that was actually refusing it
    """
    rates = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)
    cells = [(a, b) for a in rates for b in rates]

    def accepted_set(rows_on):
        return {c for c in cells if pair_accepted(c[0], c[1], rows_on)}

    base = accepted_set(frozenset())
    print(f"opt-in: baseline (every row off) accepts {len(base)} of {len(cells)} ordered cells")

    for macro, spec in sorted(OPTIN_FRONTEND_ROWS.items()):
        fi, fo = spec["pair"]
        got = accepted_set(frozenset({macro}))
        added, lost = got - base, base - got
        want = {(fi, fo), (fo, fi)}
        assert not lost, f"{macro}: must not remove any accepted cell, lost {sorted(lost)}"
        assert added == want, f"{macro}: must add exactly {sorted(want)}, added {sorted(added)}"
        print(f"opt-in: {macro:34s} adds exactly {sorted(added)}")

    every = frozenset(OPTIN_FRONTEND_ROWS)
    got = accepted_set(every)
    want = base | {c for spec in OPTIN_FRONTEND_ROWS.values()
                   for c in (spec["pair"], spec["pair"][::-1])}
    assert got == want, (f"the union is not the union of the rows: unexpected {sorted(got - want)}, "
                         f"missing {sorted(want - got)}")
    print(f"opt-in: all {len(OPTIN_FRONTEND_ROWS)} rows on accepts {len(got)} cells, "
          f"exactly the union")

    for macro, spec in sorted(OPTIN_FRONTEND_ROWS.items()):
        fi, fo = spec["pair"]
        policy_refuses = not fence_allows(fi, 1, 1, fo)
        law_ok, why = gate_accepts(fi, 1, 1, fo)
        if spec["blocker"] == "alias":
            assert policy_refuses, (
                f"{macro}: the pair DIRECT route must still be refused by the anti-alias policy")
            assert (fi, fo) in FENCE_UNQUALIFIED_DIRECT, (
                f"{macro}: an alias-blocked pair must stay in FENCE_UNQUALIFIED_DIRECT")
            print(f"opt-in: {macro:34s} DIRECT still refused by the policy (blocker alias)")
        else:
            assert not policy_refuses, (
                f"{macro}: this pair was never an anti-alias refusal, so the policy must have no "
                f"opinion about its direct route")
            assert (fi, fo) not in FENCE_UNQUALIFIED_DIRECT, (
                f"{macro}: a fill-blocked pair must NOT be added to the anti-alias table -- that "
                f"would state the wrong reason for the refusal")
            assert not law_ok, (
                f"{macro}: this pair DIRECT route must still be refused by the FILL LAW ({why}); "
                f"the row lowers the rear step, it does not relax the law")
            print(f"opt-in: {macro:34s} DIRECT still refused by the fill law (blocker fill): {why}")

    # A count no row can change, because no row publishes (1, 1): stated so a future row that DID
    # publish a direct route would show up here rather than only in a pair count.
    base_direct = {c for c in cells
                   if gate_accepts(c[0], 1, 1, c[1])[0] and fence_allows(c[0], 1, 1, c[1])}
    print(f"opt-in: {len(base_direct)} cells accept a DIRECT route -- unchanged by every row")


def gate_accepts(fs_in_hz: int, num: int, den: int, fs_out_hz: int) -> tuple[bool, str]:
    """(accepted, why) for one direction, exactly as the two C predicates decide it.

    STARVE SAFETY ONLY.  The qualification fence is a separate question and is applied after this
    in audio_app_asrc_rate_pair_is_supported(); check_gate() below therefore reports the LAW's
    verdict, and check_qualification_fence() covers the surface.
    """
    eff_num = fs_in_hz * num
    eff_den = den * fs_out_hz
    if eff_num > eff_den and eff_num * GATE_BURST_DEN >= GATE_BURST_NUM * eff_den:
        return False, "burst: ring cannot hold the look-ahead"
    _look, r_safe, set_required, setpoint, fits = fill_law(fs_in_hz, num, den, fs_out_hz)
    if not fits:
        return False, f"reserve: set_required {set_required} > TARGET_MAX {GATE_TARGET_MAX}"
    return True, f"reserve {setpoint - r_safe} (R_safe {r_safe}, set {setpoint})"


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


def check_law(label: str, fs_in_hz: int, num: int, den: int, fs_out_hz: int,
              expect_r_safe: int, expect_set: int) -> None:
    """The reserve law's two published numbers for one direction."""
    look, r_safe, req, setpoint, fits = fill_law(fs_in_hz, num, den, fs_out_hz)
    print(f"law  {label:28s} look={look:3d} R_safe={r_safe:3d} req={req:3d} set={setpoint:3d} "
          f"{'fits' if fits else 'DOES NOT FIT'}")
    assert (r_safe, setpoint) == (expect_r_safe, expect_set), (
        f"{label}: R_safe/set = {r_safe}/{setpoint}, expected {expect_r_safe}/{expect_set}")


def check_reserve_law() -> None:
    """A1: the lower reserve is one producer block + 1, from the NOMINAL rates, +1 on the floor.

    The two numbers that matter are the pair that starved and the pair under listening approval.
    32 -> 12 kHz is 8/3, so 15*step = 40 EXACTLY: floor()+1 = 41 is the whole point -- the servo
    trims the applied step above nominal and a pull then advances 41, not 40.  Taking floor() of the
    lock-time MEASURED ratio gave 39 or 40 depending on the ppm of that lock (R 55 or 56, seen on
    consecutive runs of the same pair) and produced set 73 instead of 74.
    """
    check_law("32k -> 12k (the starve pair)", 32000, 1, 1, 12000, 57, 74)
    check_law("12k -> 32k (its up-conv leg)", 12000, 1, 1, 32000, 22, 64)
    # Listening-approved operating points must not move at all.
    check_law("48k -> 44.1k", 48000, 1, 1, 44100, 33, 64)
    check_law("44.1k -> 48k", 44100, 1, 1, 48000, 30, 64)
    check_law("48k -> 48k", 48000, 1, 1, 48000, 32, 64)
    # The pairs the deleted exact-step exemption used to carry, now on a real reserve.
    check_law("48k -> 16k (was exempt)", 48000, 1, 1, 16000, 62, 79)
    check_law("48k -> 12k (was exempt)", 48000, 1, 1, 12000, 77, 94)
    check_law("24k -> 8k  (was exempt)", 24000, 1, 1, 8000, 62, 79)
    # And the ones no reserve can fit in this ring: still refused, now for a stated reason.
    for fs_in, fs_out in ((48000, 8000), (44100, 8000)):
        _l, _r, req, setpoint, fits = fill_law(fs_in, 1, 1, fs_out)
        assert not fits, f"{fs_in} -> {fs_out}: set_required {req} must not fit {GATE_TARGET_MAX}"
        print(f"law  {fs_in} -> {fs_out:<6d} set_required={req} > TARGET_MAX -> refused")
    # A profile-independent property: the reserve is a whole producer block, so every accepted pair
    # has at least BLOCK+1 frames between its setpoint and what a pull needs.
    for fs_in in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000):
        for fs_out in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000):
            ok, _why = gate_accepts(fs_in, 1, 1, fs_out)
            if not ok:
                continue
            _l, r_safe, _req, setpoint, _f = fill_law(fs_in, 1, 1, fs_out)
            assert setpoint - r_safe >= GATE_RESERVE, (
                f"{fs_in} -> {fs_out}: accepted with only {setpoint - r_safe} frames of reserve")
    print(f"law: every accepted direct pair keeps >= {GATE_RESERVE} frames of lower reserve")


def main() -> int:
    check_reserve_law()
    check_qualification_fence()
    check_optin_pair_gates()
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
    # 22.05 kHz is the row this block exists for, and the reason moved without the verdict moving.
    # Pre-A1 the direct path was refused for STARVE (setpoint pinned at R + JITTER = 4 slack).  The
    # reserve law admits it (set 99, a full one-block reserve, inside the 104 cap) -- so check_gate,
    # which reports the LAW, now says accept -- and the QUALIFICATION FENCE holds it out, because
    # admitting a new unfiltered 4.35x decimation is not part of a starve fix.  Both halves are
    # asserted, because "the pair is still refused" and "it is refused for a different reason" are
    # two different facts and a log has to be able to tell them apart.
    check_gate("96k -> 22050 (no row, law)", 96000, 22050, 1, True)
    assert not fence_allows(96000, 1, 1, 22050), "96k -> 22.05k direct must stay out of the surface"
    check_gate("96k -> 22050 (composed /4)", 96000, 22050, 4, True)
    assert fence_allows(96000, 1, 4, 22050), "the filtered /4 row must stay qualified"
    # 24 kHz passed even with no row.  It used to pass only via the exact-step exemption on 4 frames
    # of slack; that exemption is deleted (an exact step is q = 1, the DEEPEST block-phase case), and
    # the pair now passes on a real 17-frame reserve at set 94.  Same verdict, opposite reason.
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
    # 32 kHz still has no row: it runs unprotected (it used to lean on the exact-step exemption for
    # its acceptance; since 2026-09-12 it is accepted on a real reserve at set 79 and is still
    # unprotected -- the gate never modelled band limiting).
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

    check_direct_route_policy()
    check_q31_composed_geometry()

    print("PASS: general ASRC rate-plan equation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
