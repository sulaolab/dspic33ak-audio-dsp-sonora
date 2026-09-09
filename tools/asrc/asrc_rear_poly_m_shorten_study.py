#!/usr/bin/env python3
"""Host-only study: shortening the GENERIC REAR polyphase (M taps, L=128 phases).

Why this stage.  The 2026-09-06 F1 dissection of asrc_pull
(`[internal] report_ak512_96k32k_12ch_F1_asrc_pull_dissect_2026-09-06.md`) priced every bucket
inside the 69.1 us `pullBA`, and two of them scale with the rear tap count M:

    mac  40.1 us  58 %   mchp_asrc_q31_row16, 12ch x M-tap Q31 dot per output frame
    bl   22.5 us  33 %   mchp_asrc_q31_blend_row, ONE M-tap row interpolation per frame

Everything else (conv 3.4 / ph 1.7 / st 0.7 / rest 0.7) is fixed.  So M is worth
~1.62 us per tap on a leg A callback -- roughly FOUR TIMES the pre-stage's 0.45 us
per non-zero tap, because the rear runs at the 96 kHz output rate across 12 channels
AND pays a per-frame row blend on top.  The pre-stage lever topped out at +8.68 us/A
in total (`[internal] report_ak512_96k32k_12ch_prestage_shorten_2026-09-08.md`); ONE rear tap is +1.62.

The three bit-exact candidates of the F1 report are already spent -- they are the
`*au` bits -- and the blended-row memo is dead (hit = 0).  With approximation
permitted (owner ruling 2026-09-08 #1) the tap count is the remaining lever here.

WHICH GATE JUDGES THIS, and why it is NOT the one that judged the pre-stage.
=========================================================================
The composed 96 -> 32 kHz gate (asrc_96_to_32_audio_gate.py, and its two-fold
wrapper composed_96_32() in asrc_fir_lightweight_feasibility.py) was written to judge
the PRE-STAGE and the RATIONAL stage with the rear held fixed -- its own banner says
"rear end : ... (UNCHANGED)".  Structurally it applies the rear's magnitude to the
alias term at the FOLDED LANDING frequency (`mag_at(rear, ..., land)`, gate line 182),
which is inside the rear's passband, so the rear contributes ~0 dB to every alias
number that model produces.

    Consequence, measured: sweeping M from 30 down to 18 moves composed alias13 by
    0.2 dB (-100.5 -> -100.7).  That flatness is the MODEL BEING BLIND TO THIS
    FILTER, not evidence that an 18-tap rear is safe.  Do not quote composed alias
    figures as authority for a rear tap count.

What the composed model does capture for the rear is the PASSBAND: the wanted-signal
term applies the rear at `band`, so floor13 / peak13 / w14 / w15 are valid, and they
are the 32 kHz-rate-correct view of the droop.  Both are printed, labelled.

The authority for the rear's own stopband already exists and is used here unchanged:
asrc_headroom_filter_check.analyze(), the gate the shipping rear was accepted under --
worst image over the first image band and the passband edge, with the shipped
thresholds max_image_db = -105.0 and max_edge_loss_db = -1.1 carried on the Candidate
itself.  Its normalisation (AUDIO_EDGE = 20 kHz of 48 kHz) is that gate's own
convention and is left alone; the rear's shape is rate-agnostic in normalised terms.

Unlike the pre-stage, shortening this filter moves the passband too: its cutoff is
0.465 of 32 kHz = 14.88 kHz, so 13-15 kHz sits on the shoulder.  cutoff and Kaiser
beta are therefore re-chosen for every M -- the pre-stage study's lesson (a window
parameter picked for the long filter is badly mistuned once it gets short) applies
verbatim.

Writes no coefficient table and no shipping source.  Run from the repository root.
"""
from __future__ import annotations

import argparse
import contextlib
import dataclasses
import importlib.util
import io
import json
import pathlib
import sys
from typing import Any

import numpy as np

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


lw = _load("rear_lw", "tools/asrc/asrc_fir_lightweight_feasibility.py")
g96 = lw._g96()
chk = g96.chk
SHIPPED = g96.SHIPPED               # headroom-m30-kaiser11: M=30, fc 0.465, beta 11
L_REAR = g96.L_REAR                 # 128 phases

# ------------------------------------------------------------------ CPU anchors
# The F1 buckets, instrument-corrected, for the profile=0 image (69.1 us total).
MEAS_MAC_US = 40.1
MEAS_BL_US = 22.5
MEAS_OTHER_US = 3.4 + 1.7 + 0.7 + 0.7      # conv + ph + st + rest, all M-independent
MEAS_TOTAL_US = MEAS_MAC_US + MEAS_BL_US + MEAS_OTHER_US

CH = 12
FRAMES = 16                                 # output frames per 96 kHz block
CYCLES_PER_MAC = lw.ANCHORS["q31_cycles_per_mac"]
INSTR_HZ = lw.ANCHORS["instr_hz"]
REALISATION = 0.940                         # measured on the HB35 landing (10.05 / 10.69)
A_PER_B_WINDOW = 3.0                        # leg A callbacks inside one 500 us leg B window
TDM2_MARGIN_US_NOW = 1.3                    # image 2137 / 0247 at *au03, HB35


def mac_floor_us(taps: int) -> float:
    """The Q31 dot itself: 12ch x taps MACs per frame, 16 frames."""
    return taps * CH * FRAMES * CYCLES_PER_MAC / INSTR_HZ * 1.0e6


# The dot bucket is floor + a per-channel-dot prologue that does NOT scale with M;
# the blend bucket is treated as fully proportional (16 calls per block, so its own
# call overhead is under a microsecond).  Splitting it this way is the CONSERVATIVE
# direction: any fixed part hiding inside `bl` makes the projected saving smaller
# than modelled, never larger.
MAC_FIXED_US = MEAS_MAC_US - mac_floor_us(SHIPPED.taps)
BL_PER_TAP_US = MEAS_BL_US / SHIPPED.taps
US_PER_TAP_A = (mac_floor_us(1) + BL_PER_TAP_US) * REALISATION


def pull_us(taps: int) -> float:
    return mac_floor_us(taps) + MAC_FIXED_US + BL_PER_TAP_US * taps + MEAS_OTHER_US


def rear_prototype(cand) -> np.ndarray:
    """Same construction as asrc_96_to_32_audio_gate.rear_prototype, any tap count."""
    rows = chk.phase_rows(cand)
    proto = np.zeros(cand.taps * L_REAR)
    for phase in range(L_REAR):
        for tap in range(cand.taps):
            proto[tap * L_REAR - phase + (L_REAR - 1)] = rows[phase, tap]
    return proto / proto.sum()


def composed(cand, prestage: np.ndarray):
    """composed 96 -> 32 with THIS rear substituted.  PASSBAND ONLY is meaningful."""
    saved = lw._G96_REAR
    try:
        lw._G96_REAR = rear_prototype(cand)
        return lw.composed_96_32(prestage)
    finally:
        lw._G96_REAR = saved


def rear_gate(cand) -> tuple[float, float, float, str]:
    """The rear's OWN gate: chk.analyze + the shipped thresholds on the Candidate.

    'pass' is that gate's OWN test, unaltered -- image <= max_image_db and
    edge >= max_edge_loss_db, exactly as asrc_headroom_filter_check.main() applies it,
    which is why the shipping M=30 rear reads 'pass' here despite its -0.74 dB edge.
    'marg' relaxes the image bar by 5 dB, the same relaxation the composed gate's
    'acceptable' tier allows on alias, and is a candidate for the owner rather than a
    number this study may treat as approved.  No threshold is invented.
    """
    ripple, edge, image = chk.analyze(cand)
    if image <= cand.max_image_db and edge >= cand.max_edge_loss_db:
        tier = "pass"
    elif image <= cand.max_image_db + 5.0 and edge >= cand.max_edge_loss_db:
        tier = "marg"
    else:
        tier = "fail"
    return ripple, edge, image, tier


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", type=pathlib.Path, default=None)
    ap.add_argument("--taps", type=int, nargs="+",
                    default=[30, 28, 26, 24, 22, 20, 18])
    # The rear's passband edge is measured by chk at AUDIO_EDGE = 20/48 = 0.4167 of the
    # input rate, so a cutoff much below ~0.43 cannot meet edge >= -1.1 dB at all: the
    # transition available to the stopband is pinned between ~0.43 and the image band's
    # start at 1 - 0.4167 = 0.5833.  The lattice therefore spans that whole usable range
    # rather than a few hand-picked points -- the attenuation a given M can reach over a
    # fixed transition is what the cliff in this table is made of.
    ap.add_argument("--cutoffs", type=float, nargs="+",
                    default=[0.420, 0.425, 0.430, 0.435, 0.440, 0.445, 0.450, 0.455,
                             0.460, 0.4632, 0.465, 0.470, 0.475, 0.480])
    ap.add_argument("--betas", type=float, nargs="+",
                    default=[6.0, 7.0, 8.0, 9.0, 10.0, 10.55, 11.0, 12.0, 13.0, 14.0,
                             15.0, 16.0, 18.0])
    ap.add_argument("--all", action="store_true",
                    help="print every (cutoff, beta), not only the best per tap count")
    args = ap.parse_args()

    dsg = lw.dsg
    # The half-band that is now the production pre-stage (31 taps, beta 10, commit
    # 1d56ada) -- the rear is judged behind the filter it will actually sit behind.
    hb = np.asarray(dsg.design(dsg.PRESTAGE_HB_TAPS, dsg.PRESTAGE_INPUT_HZ,
                               dsg.PRESTAGE_HB_PASSBAND_HZ,
                               dsg.PRESTAGE_HB_STOPBAND_HZ,
                               dsg.PRESTAGE_HB_KAISER_BETA),
                    dtype=np.float64)

    rows: list[dict[str, Any]] = []

    def add(label: str, cand) -> dict[str, Any]:
        q = composed(cand, hb)
        ripple, edge, image, tier = rear_gate(cand)
        d_a = (pull_us(SHIPPED.taps) - pull_us(cand.taps)) * REALISATION
        row = {
            "label": label, "taps": cand.taps, "cutoff": cand.cutoff,
            "beta": cand.kaiser_beta,
            "rear_image_db": image, "rear_edge_db": edge, "rear_ripple_db": ripple,
            "rear_gate": tier,
            "floor13": q.floor[13_000.0], "peak13": q.peak[13_000.0],
            "w13": q.wanted[13_000.0], "w14": q.wanted[14_000.0],
            "w15": q.wanted[15_000.0],
            "composed_alias13_BLIND": q.alias[13_000.0],
            "composed_alias15_BLIND": q.alias[15_000.0],
            "pull_us": pull_us(cand.taps),
            "legA_gain_us": d_a, "legB_gain_us": d_a * A_PER_B_WINDOW,
            "tdm2_margin_us": TDM2_MARGIN_US_NOW + d_a * A_PER_B_WINDOW,
        }
        rows.append(row)
        return row

    add("SHIP M30 fc0.4650 b11", SHIPPED)

    for taps in args.taps:
        best = None
        for cutoff in args.cutoffs:
            for beta in args.betas:
                cand = dataclasses.replace(SHIPPED, name="m%d" % taps, taps=taps,
                                           cutoff=cutoff, kaiser_beta=beta)
                ripple, edge, image, _ = rear_gate(cand)
                # Rank by the deepest first-image rejection among the (cutoff, beta)
                # pairs whose passband edge has not collapsed past the shipped bar.
                # Droop is reported for every row, never traded away silently.
                key = (edge < cand.max_edge_loss_db, image)
                if best is None or key < best[0]:
                    best = (key, cand)
                if args.all:
                    add("  M%d fc%.4f b%g" % (taps, cutoff, beta), cand)
        assert best is not None
        add("M%-2d fc%.4f b%-5g best" % (best[1].taps, best[1].cutoff,
                                         best[1].kaiser_beta), best[1])

    hdr = ("{:<24} {:>3} {:>7} {:>5} | {:>8} {:>7} {:>7} {:>5} | "
           "{:>7} {:>7} {:>7} | {:>7} {:>7} {:>7} | {:>8}")
    print("generic rear polyphase (L=%d): the tap-count shortening curve" % L_REAR)
    print("rear gate:     asrc_headroom_filter_check.analyze() -- the gate the M=%d rear "
          "was accepted under (image <= %.0f dB, edge >= %.1f dB)"
          % (SHIPPED.taps, SHIPPED.max_image_db, SHIPPED.max_edge_loss_db))
    print("passband:      composed 96 -> 32 kHz, rear substituted (VALID for droop)")
    print("BLIND column:  composed alias -- the composed model applies the rear at the "
          "FOLDED landing freq, so it CANNOT see this filter's stopband.")
    print("               printed only to show it is flat; NOT authority for a rear "
          "tap count.  See the module docstring.")
    print("cpu model:     F1 buckets mac %.1f + bl %.1f + fixed %.1f = %.1f us at M=%d, "
          "realisation %.3f" % (MEAS_MAC_US, MEAS_BL_US, MEAS_OTHER_US, MEAS_TOTAL_US,
                                SHIPPED.taps, REALISATION))
    print("               %.3f us/tap dot floor + %.3f us/tap row blend = %.3f us/tap "
          "on leg A, x%.0f on the leg B wall"
          % (mac_floor_us(1), BL_PER_TAP_US, US_PER_TAP_A, A_PER_B_WINDOW))
    print("board today:   M=%d, HB35 pre-stage, TDM2 margin +%.1f us"
          % (SHIPPED.taps, TDM2_MARGIN_US_NOW))
    print()
    print(hdr.format("candidate", "M", "fc", "beta",
                     "image", "edge", "ripple", "gate",
                     "flr13", "w14dB", "w15dB",
                     "dlegA", "dlegB", "TDM2", "alias13"))
    print("-" * 160)
    for r in rows:
        print(hdr.format(r["label"], r["taps"], "%.4f" % r["cutoff"],
                         "%g" % (r["beta"] if r["beta"] is not None else 0),
                         "%.1f" % r["rear_image_db"], "%.2f" % r["rear_edge_db"],
                         "%.2f" % r["rear_ripple_db"], r["rear_gate"],
                         "%.2f" % r["floor13"], "%.2f" % r["w14"], "%.2f" % r["w15"],
                         "%+.2f" % r["legA_gain_us"], "%+.2f" % r["legB_gain_us"],
                         "%+.1f" % r["tdm2_margin_us"],
                         "(%.1f)" % r["composed_alias13_BLIND"]))
    print()
    print("this lever does NOT top out the way the pre-stage did -- at M=%d the two "
          "M-proportional buckets are still %.1f of the %.1f us pull, and the whole "
          "pre-stage lever was +8.68 us/A."
          % (SHIPPED.taps, mac_floor_us(SHIPPED.taps) + MEAS_BL_US, MEAS_TOTAL_US))

    if args.json:
        args.json.write_text(json.dumps(
            {"anchors": {"meas_mac_us": MEAS_MAC_US, "meas_bl_us": MEAS_BL_US,
                         "meas_other_us": MEAS_OTHER_US, "mac_fixed_us": MAC_FIXED_US,
                         "bl_per_tap_us": BL_PER_TAP_US,
                         "us_per_tap_legA": US_PER_TAP_A, "ch": CH, "frames": FRAMES,
                         "cycles_per_mac": CYCLES_PER_MAC, "instr_hz": INSTR_HZ,
                         "realisation": REALISATION,
                         "tdm2_margin_us_now": TDM2_MARGIN_US_NOW,
                         "l_rear": L_REAR, "shipped_taps": SHIPPED.taps,
                         "rear_gate": "asrc_headroom_filter_check.analyze",
                         "composed_alias_is_blind_to_rear": True},
             "rows": rows}, indent=1), encoding="utf-8")
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
