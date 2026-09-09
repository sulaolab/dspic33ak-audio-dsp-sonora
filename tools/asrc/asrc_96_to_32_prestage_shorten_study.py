#!/usr/bin/env python3
"""Host-only study: how short can the 96->48 kHz half-band pre-stage get, and what
does each step actually BUY on leg A?

Why this exists.  The E family of asrc_fir_lightweight_feasibility.py answered a
DIFFERENT question -- "which half-band holds the shipping composed 96->32 numbers
exactly" -- and its answer was 35 taps (19 non-zero).  Its search lattice therefore
started at 27 taps and its accept test was "no worse than the 41-tap dense
pre-stage".  With approximation now permitted (owner ruling 2026-09-08 #1), the
question becomes what the quality/CPU curve looks like BELOW that point, so the
lattice is extended down to 11 taps and nothing is rejected: every point is reported
with its degradation, and the human picks.

QUALITY.  Not re-modelled here.  composed_96_32() and halfband() are imported from
asrc_fir_lightweight_feasibility.py, which in turn drives asrc_96_to_32_audio_gate.py
two folds deep with the unchanged N97 prototype and the unchanged generic rear.  Using
the same model is the point: these numbers are directly comparable to every earlier
row, including the shipping reference.

CPU.  No host timing is used.  A half-band of length 4h-1 costs 2h+1 = nz MACs per
output, the block emits BLOCK_FRAMES/2 outputs per channel, and the per-MAC cost is
the MEASURED 1.012 cycles/MAC at the 200 MHz instruction rate, then scaled by the
MEASURED realisation factor of the HB35 landing (-10.05 us actual against -10.69 us
theoretical for the same 41 -> 19 nz step, = 0.94).  That factor is what keeps this a
projection from hardware rather than from arithmetic.

LEVERAGE.  leg A runs 3x inside one 500 us leg B window, so a saving of D us per A
callback opens 3D us of the leg B wall (`[internal] report_ak512_96k32k_12ch_legb_wall_2026-09-08.md`).
Both columns are printed because only the leg B one decides feasibility.

Writes no coefficient include and no shipping source.  Run from the repository root.
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import pathlib
import sys
from typing import Any

import numpy as np
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]


def _load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load " + relative)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(module)
    return module


lw = _load("shorten_lw", "tools/asrc/asrc_fir_lightweight_feasibility.py")
dsg = lw.dsg

# ---------------------------------------------------------------- CPU anchors
CH = 12
BLOCK_FRAMES = 16
PRE_OUT_PER_CH = BLOCK_FRAMES // 2          # /2 pre-stage, one 96 kHz block
CYCLES_PER_MAC = lw.ANCHORS["q31_cycles_per_mac"]
INSTR_HZ = lw.ANCHORS["instr_hz"]
DENSE_TAPS = 41                             # the shipping dense pre-stage
HB_REF_TAPS = 35                            # the CPU baseline every gain is measured from
MEAS_HB35_GAIN_US = 10.05                   # measured 41 dense -> 35 half-band

A_PER_B_WINDOW = 3.0                        # leg A callbacks inside one leg B window
TDM2_MARGIN_US_NOW = 1.3                    # image 2137 / 0247 at *au03

REALISATION = 1.0                           # set in main() from the two anchors above


def nz_of(taps: int) -> int:
    """Non-zero taps of a length-(4h-1) half-band: 2h+1."""
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    zeros = int(np.sum((np.abs(offset % 2.0) < 1.0e-12) & (np.abs(offset) > 1.0e-12)))
    return taps - zeros


def mac_us(nz: int) -> float:
    """us per 96 kHz block for nz MACs per output, all channels, theoretical."""
    macs = nz * CH * PRE_OUT_PER_CH
    return macs * CYCLES_PER_MAC / INSTR_HZ * 1.0e6


PRE_IN_HZ = 96_000.0
HB_PASS_HZ = 16_000.0       # pinned by the half-band mirror of the 32 kHz fold edge
HB_STOP_HZ = 32_000.0


def halfband_equiripple(taps: int) -> np.ndarray | None:
    """Equiripple (Remez) half-band, same structure and same nz as halfband().

    The E family only ever built WINDOWED-SINC half-bands, which spend their taps on a
    monotone stopband nobody asked for.  For bands that are exactly mirrored about fs/4
    with equal weights, Remez returns a filter whose even-offset taps are already zero
    to numerical precision -- i.e. a genuine half-band -- so the same 2h+1 MACs buy an
    equiripple stopband instead.  The even taps are forced to exact zero afterwards and
    the residual is asserted small, because a "half-band" whose zeros are not actually
    zero would be measured here and then cost full price in the kernel.

    Returns None when Remez does not converge (short lengths at this transition are
    easy, so this is a guard rather than an expected outcome).
    """
    try:
        coeff = signal.remez(taps, [0.0, HB_PASS_HZ, HB_STOP_HZ, PRE_IN_HZ / 2.0],
                             [1.0, 0.0], fs=PRE_IN_HZ)
    except (ValueError, RuntimeError):
        return None
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    even = (np.abs(offset % 2.0) < 1.0e-12) & (np.abs(offset) > 1.0e-12)
    residual = float(np.max(np.abs(coeff[even]))) if even.any() else 0.0
    if residual > 1.0e-6:
        return None                     # not a half-band: refuse rather than mis-price
    coeff = np.asarray(coeff, dtype=np.float64).copy()
    coeff[even] = 0.0
    return coeff / coeff.sum()


# The gate classes are Quality.gate() in asrc_fir_lightweight_feasibility.py -- the
# 2026-09-06 brief thresholds, alias / floor / peak together.  Not re-stated here: a
# second copy of a threshold is a second thing to forget to update.


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", type=pathlib.Path, default=None)
    ap.add_argument("--betas", type=float, nargs="+",
                    default=[4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0, 14.0])
    ap.add_argument("--all-betas", action="store_true",
                    help="print every beta, not only the best per tap count")
    args = ap.parse_args()

    global REALISATION
    theo_hb35 = mac_us(DENSE_TAPS) - mac_us(nz_of(HB_REF_TAPS))
    REALISATION = MEAS_HB35_GAIN_US / theo_hb35

    ship = np.asarray(dsg.design(dsg.PRESTAGE_TAPS, dsg.PRESTAGE_INPUT_HZ,
                                 dsg.PRESTAGE_PASSBAND_HZ, dsg.PRESTAGE_STOPBAND_HZ),
                      dtype=np.float64)
    q_ship = lw.composed_96_32(ship)

    rows: list[dict[str, Any]] = []

    def add(label: str, taps: int, beta: float | None, coeff: np.ndarray,
            printed: bool = True) -> dict[str, Any]:
        q = lw.composed_96_32(coeff)
        nz = int(np.sum(np.abs(coeff) > 1.0e-12))
        gain = (mac_us(nz_of(HB_REF_TAPS)) - mac_us(nz)) * REALISATION
        row = {
            "label": label, "taps": taps, "beta": beta, "nz": nz,
            "alias13": q.alias[13_000.0], "alias15": q.alias[15_000.0],
            "floor13": q.floor[13_000.0], "floor15": q.floor[15_000.0],
            "w13": q.wanted[13_000.0], "w14": q.wanted[14_000.0],
            "w15": q.wanted[15_000.0],
            "legA_gain_us": gain, "legB_gain_us": gain * A_PER_B_WINDOW,
            "tdm2_margin_us": TDM2_MARGIN_US_NOW + gain * A_PER_B_WINDOW,
            "gate13": q.gate(13_000.0)[:4],
            "alias13_source_hz": q.worst_source_hz[13_000.0],
            "printed": printed,
        }
        rows.append(row)
        return row

    add("SHIP dense %dt (reference)" % DENSE_TAPS, DENSE_TAPS, None, ship)

    lattice = [t for t in range(11, 40, 4)]
    for taps in lattice:
        # The ACTIONABLE row: exactly what asrc_decimator_48_to_8_design.py would emit
        # if PRESTAGE_HB_TAPS were changed to this value and the .inc regenerated --
        # same design(), same fixed KAISER_BETA, same float32 rounding.  The swept-beta
        # rows below are only there to show how much the fixed window is leaving on the
        # table; a recommendation that cannot be regenerated is not a recommendation.
        # beta is per set since 2026-09-08 (PRESTAGE_HB_KAISER_BETA), so read the HB's own
        # value rather than the module default -- otherwise this row stops being "what the
        # generator emits", which is the only property that makes it actionable.
        gen_beta = dsg.PRESTAGE_HB_KAISER_BETA
        gen = np.asarray(dsg.design(taps, PRE_IN_HZ, dsg.PRESTAGE_HB_PASSBAND_HZ,
                                    dsg.PRESTAGE_HB_STOPBAND_HZ, gen_beta),
                         dtype=np.float64)
        add("GEN   %dt b%g%s" % (taps, gen_beta,
                                 "  <- generator setting" if taps == dsg.PRESTAGE_HB_TAPS
                                 else ""),
            taps, gen_beta, gen)
        best = None
        for beta in args.betas:
            coeff = lw.halfband(taps, beta)
            q = lw.composed_96_32(coeff)
            # Rank by the deepest alias into 0-13 kHz among the betas whose 0-13 kHz
            # floor has not collapsed.  Passband droop is reported, never traded away
            # silently -- w14 / w15 are printed for every row.
            key = (q.floor[13_000.0] < -1.1, q.alias[13_000.0])
            if best is None or key < best[0]:
                best = (key, beta, coeff)
            if args.all_betas:
                add("  HB %dt b%g" % (taps, beta), taps, beta, coeff)
        assert best is not None
        add("HBwin %dt b%g (best beta)" % (taps, best[1]), taps, best[1], best[2])
        eq = halfband_equiripple(taps)
        if eq is not None:
            add("HBeq  %dt" % taps, taps, None, eq)

    hdr = ("{:<28} {:>3} {:>3} | {:>8} {:>8} | {:>7} {:>7} {:>7} | "
           "{:>7} {:>7} {:>8} {:>5}")
    print("96 -> 48 kHz half-band pre-stage: the shortening curve")
    print("quality model: asrc_96_to_32_audio_gate.py composed, two folds deep "
          "(identical to the E family)")
    print("cpu model:     %.3f cy/MAC at %.0f MHz, realisation %.3f from the "
          "measured HB35 landing (-%.2f us)"
          % (CYCLES_PER_MAC, INSTR_HZ / 1e6, REALISATION, MEAS_HB35_GAIN_US))
    print("board today:   HB35, TDM2 margin +%.1f us" % TDM2_MARGIN_US_NOW)
    print()
    print(hdr.format("candidate", "t", "nz", "alias13", "alias15",
                     "flr13", "w14dB", "w15dB", "dlegA", "dlegB", "TDM2", "g13"))
    print("-" * 126)
    order = sorted(rows, key=lambda r: (r["taps"], r["beta"] or 0.0))
    for r in order:
        print(hdr.format(r["label"], r["taps"], r["nz"],
                         "%.1f" % r["alias13"], "%.1f" % r["alias15"],
                         "%.2f" % r["floor13"], "%.2f" % r["w14"],
                         "%.2f" % r["w15"],
                         "%+.2f" % r["legA_gain_us"],
                         "%+.2f" % r["legB_gain_us"],
                         "%+.1f" % r["tdm2_margin_us"], r["gate13"]))
    print()
    print("hard ceiling: deleting the pre-stage MACs ENTIRELY (nz -> 0, not a real "
          "filter) would be only %+.2f us on leg A / %+.2f us on the leg B wall."
          % (mac_us(nz_of(HB_REF_TAPS)) * REALISATION,
             mac_us(nz_of(HB_REF_TAPS)) * REALISATION * A_PER_B_WINDOW))
    print("so the whole tap-count lever is bounded by that figure.")
    print()

    # ------------------------------------------------------------------ accounting
    # Why the ceiling is that low: MOST of the measured front-end time is not MAC.
    # The measured column is the *au03 leg-A partition telemetry of image ...0247
    # (`[internal] report_ak512_96k32k_12ch_legA_bitexact_2026-09-08.md`).
    r23_out_per_ch = BLOCK_FRAMES * 32.0 / 96.0        # one 96 kHz block, 32 kHz out
    r23_mac_per_out = lw.ANCHORS["n97_proto_taps"] / 2.0
    pre_mac_us = mac_us(nz_of(HB_REF_TAPS)) * REALISATION
    r23_mac_us = (r23_mac_per_out * CH * r23_out_per_ch
                  * CYCLES_PER_MAC / INSTR_HZ * 1.0e6 * REALISATION)
    meas = {"pre": 31.0, "r23": 39.5, "fe": 72.0, "pullBA": 69.4, "cb": 142.3}
    print("leg A per-block accounting (measured vs the MAC floor of the same stage)")
    print("  {:<10} {:>9} {:>9} {:>9}".format("stage", "measured", "MAC", "overhead"))
    for name, mac in (("pre (HB35)", pre_mac_us), ("r23 (N97)", r23_mac_us)):
        key = "pre" if name.startswith("pre") else "r23"
        print("  {:<10} {:>9.1f} {:>9.1f} {:>9.1f}".format(
            name, meas[key], mac, meas[key] - mac))
    print("  {:<10} {:>9.1f} {:>9.1f} {:>9.1f}".format(
        "fe total", meas["fe"], pre_mac_us + r23_mac_us,
        meas["fe"] - pre_mac_us - r23_mac_us))
    print("  {:<10} {:>9.1f} {:>9} {:>9}".format("pullBA", meas["pullBA"], "-", "-"))
    print("  {:<10} {:>9.1f}".format("cb total", meas["cb"]))
    print("  => only %.0f%% of `fe` is arithmetic the tap count can touch; the rest is "
          "ring / loop / call overhead, which the 2026-09-08 bit-exact experiment "
          "showed does not move." % (100.0 * (pre_mac_us + r23_mac_us) / meas["fe"]))

    if args.json:
        args.json.write_text(json.dumps(
            {"anchors": {"cycles_per_mac": CYCLES_PER_MAC, "instr_hz": INSTR_HZ,
                         "realisation": REALISATION, "ch": CH,
                         "block_frames": BLOCK_FRAMES,
                         "meas_hb35_gain_us": MEAS_HB35_GAIN_US,
                         "tdm2_margin_us_now": TDM2_MARGIN_US_NOW},
             "reference_ship": {"alias13": q_ship.alias[13_000.0],
                                "alias15": q_ship.alias[15_000.0],
                                "w13": q_ship.wanted[13_000.0],
                                "w14": q_ship.wanted[14_000.0],
                                "w15": q_ship.wanted[15_000.0]},
             "rows": rows}, indent=1), encoding="utf-8")
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
