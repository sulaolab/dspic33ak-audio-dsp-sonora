#!/usr/bin/env python3
"""Design and gate the 1:1 source-rate anti-alias LPF stages, and emit their coefficient include.

WHAT A 1:1 STAGE IS, AND WHY IT EXISTS
======================================
Every front-end stage in this tree so far CHANGES the rate: /2, /3, /4, /6, or the L=2/M=3
polyphase.  A 1:1 stage changes none -- it is an anti-alias low-pass at the SOURCE rate, with the
existing rear asynchronous ASRC doing the whole rate change behind it.

The reason is not cost, it is the PASSBAND.  The rear M30 resampler's cutoff is
ASRC_POLY_FC = 0.464 of its OWN input rate, one constant per build, so its -0.1 dB point sits at
0.389 of that rate.  A front end that decimates all the way to Fs_out leaves the resampler at
step 1.0, where 0.389 * Fs_out = 0.778 * FN -- below the 0.8125 * FN the uniform specification
asks for.  Keeping the input rate high is what keeps the rear end flat over the protected band.
Derivation and the 45-pair sweep: [internal]
study_ak512_universal_nyquist_planner_2026-09-13.md sections 3 and 4.

DESIGN RULE
===========
    Fp    = 0.8125 * (Fs_out / 2)            protected passband edge (owner-fixed)
    Fstop = Fs_out - Fp                      the first fold that lands inside the protected band
    alias <= -105 dB into [0, Fp]            INTERNAL design target (product candidate -100 dB)
    |passband error| <= 0.10 dB over [0, Fp] including the rear resampler

QUANTIZATION
============
The firmware stores these as `const float` in the include and converts them at init with
dec_q31_from_float() -- round-to-nearest, ABSOLUTE scale 2^31, not peak-relative.  This tool
models exactly that, which is a correction to the Phase 0 study's peak-relative model (the
difference turned out to be immaterial, but the number reported here is the shipped one).

Run from the repository root.  Only NumPy is required.

  python tools/asrc/asrc_lpf1to1_design.py            # gate every configured stage
  python tools/asrc/asrc_lpf1to1_design.py --write    # also (re)write the coefficient include
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import pathlib
import struct
import sys
import zlib

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "src/app/apps/asrc/asrc_lpf1to1_coeffs.inc"


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


M = _load("unm", "tools/asrc/asrc_universal_nyquist_model.py")

PROTECT_RATIO = 0.8125          # owner-fixed 2026-09-14
PASSBAND_LIMIT_DB = 0.10        # owner-fixed
ALIAS_PRODUCT_DB = -100.0       # product candidate
ALIAS_DESIGN_DB = -105.0        # internal design target, this tool's gate

# The stages this build knows about.  `name` becomes the C identifier stem.
#
#   32 -> 16 kHz  is the Phase 1 prototype: the 1:1 stage IS the whole front end.
#   22.05 kHz     is the second stage of the 44.1 -> 16 kHz chain (/2 then 1:1), P1 of the brief.
#   16 kHz        is the second stage of the 32 -> 11.025 kHz chain (/2 then 1:1), P2.
#
# Each row is (name, stage rate, final Fs_out).  Fp and Fstop are DERIVED from the uniform spec --
# never written per row, so a rate cannot quietly get its own specification.
STAGES = (
    ("lp1_32k_to_16k", 32000.0, 16000.0),
    ("lp1_22k05_to_16k", 22050.0, 16000.0),
    ("lp1_16k_to_11k025", 16000.0, 11025.0),

    # ---- THE UNIVERSAL AK512 BANK (Phase I SS3).  The 1:1 stages of the 15 rows whose SHIPPED
    # coefficients were signed off against a LEGACY per-rate specification and therefore miss the
    # uniform passband (Phase H SS3/SS4).  Every row here is a NEW SYMBOL in this file; not one
    # legacy array is touched, which is the owner's "do not silently rewrite an existing profile"
    # expressed as a linker fact rather than a promise.
    #
    # WHY THEY ARE 1:1 STAGES AT ALL is the Phase H finding: 11 of the 15 shipped chains decimate
    # all the way down to Fs_out, which leaves the rear resampler at step 1.0, where its own
    # prototype droops -0.3892 dB at 0.8125 * FN -- 3.9x the whole +/-0.10 dB budget, and not
    # curable by any front-end change because the rear prototype is one table shared by every
    # rate (tools/asrc/asrc_rear_passband_law.py).  So the migration is TOPOLOGICAL: stop the
    # front end high enough that the rear has work left to do.  That is exactly what this file's
    # banner has always said, now applied to the whole 45-pair matrix instead of three rows.
    #
    # Fp and Fstop stay DERIVED from the uniform spec, so no row here can quietly acquire its own
    # specification -- which is the very defect being migrated away from.
    ("lp1_16k_to_8k", 16000.0, 8000.0),            # 48 -> 8 k,      behind /3@48k
    ("lp1_16k_to_12k", 16000.0, 12000.0),          # 48 -> 12 k,     behind /3@48k
    ("lp1_24k_to_16k", 24000.0, 16000.0),          # 48 -> 16 k,     behind /2@48k
    ("lp1_48k_to_22k05", 48000.0, 22050.0),        # 48 -> 22.05 k,  1:1 IS the front end
    ("lp1_48k_to_24k", 48000.0, 24000.0),          # 48 -> 24 k,     1:1 IS the front end
    ("lp1_48k_to_32k", 48000.0, 32000.0),          # 48 -> 32 k and 96 -> 32 k (behind /2@96k)
    ("lp1_32k_to_8k", 32000.0, 8000.0),            # 96 -> 8 k,      behind /3@96k
    ("lp1_32k_to_11k025", 32000.0, 11025.0),       # 96 -> 11.025 k, behind /3@96k
    ("lp1_32k_to_12k", 32000.0, 12000.0),          # 96 -> 12 k,     behind /3@96k
    ("lp1_32k_to_22k05", 32000.0, 22050.0),        # 96 -> 22.05 k,  behind /3@96k
    ("lp1_32k_to_24k", 32000.0, 24000.0),          # 96 -> 24 k,     behind /3@96k
    ("lp1_96k_to_48k", 96000.0, 48000.0),          # 96 -> 48 k,     1:1 IS the front end

    # ---- 44.1 -> 12 kHz, added 2026-09-20.  TWO partners, one per candidate shape, because the
    # choice between /2 and /3 is a CPU/arena question this tool's own numbers answer -- generating
    # only the shape that was guessed first would decide it by typing order.
    #   /2 reaches 22.05 kHz, /3 reaches 14.7 kHz.  14.7 kHz is a rate no other row uses; it is a
    #   legal stage rate because the composed front end takes its decimation as a runtime value.
    #   [internal] study_remaining_rate_pairs_20260920.md, Phase 2.
    ("lp1_22k05_to_12k", 22050.0, 12000.0),        # 44.1 -> 12 k,   behind /2@44.1k
    ("lp1_14k7_to_12k", 14700.0, 12000.0),         # 44.1 -> 12 k,   behind /3@44.1k

    # ---- 44.1 -> 8 kHz, added 2026-09-20.  THIS PAIR IS REFUSED BY THE FILL LAW, not by the
    # anti-alias policy: its direct step 5.5125 needs set_required 116 against TARGET_MAX 104.  A
    # front end therefore has to clear TWO gates, and the candidate scan
    # ([internal tool] asrc_44k1_to_8k_candidate_scan.py) found /4 the best of the shapes that clear
    # both -- 11.025 kHz intermediate, rear step 1.378125, and 12.5 dB of spectral margin where the
    # /3 shape has 0.47 dB.  The /3 partner is generated too, so the choice stays re-checkable.
    #   [internal] study_remaining_rate_pairs_20260920.md, Phase 3.
    ("lp1_11k025_to_8k", 11025.0, 8000.0),         # 44.1 -> 8 k,    behind /4@44.1k  <- chosen
    ("lp1_14k7_to_8k", 14700.0, 8000.0),           # 44.1 -> 8 k,    behind /3@44.1k  (alternative)
)

# The DECIMATING FIRST STAGE of a composed chain, and why it is generated here rather than
# reused from asrc_decimator_48_to_8_design.py.
#
# 44.1 -> 16 kHz is /2 then 1:1: the rate reaches 22.05 kHz and the 1:1 set above band-limits
# it there.  A first stage for that chain does NOT have to be a stand-alone /2 anti-alias
# filter, because the 1:1 stage behind it removes 9500 .. 11025 Hz -- so its stopband only has
# to start at Fs_mid - Fstop(1:1) = 12550 Hz, not at 11025 Hz.  That is the whole economy of a
# composition: the two stages share ONE specification instead of each meeting it alone.
#
# Measured consequence (tools/asrc/asrc_lp1_chain_44k1_to_16k_gate.py, 2026-09-14): the shipped
# /2 :22k05 set run at 44.1 kHz also clears the spectral gate (-124.28 dB, passband 0.000 dB) --
# it is simply 107 taps where 51 suffice, which is 40.1 % of a 200 MHz core for 16 channels
# against a 40 % front-end budget.  So the reason for a dedicated set is CPU, not spectrum, and
# reuse stays a documented fallback if flash ever matters more than cycles.
#
# Each row is (name, source rate, decimation, FINAL Fs_out).  Fp and Fstop are DERIVED from the
# uniform spec and the partner stage, never written per row.
PRESTAGES = (
    ("dec2_44k1_for_16k", 44100.0, 2, 16000.0),

    # 32 -> 11.025 kHz, added 2026-09-20.  The pre-stage is the ONLY new coefficient set this pair
    # needs: its partner is lp1_16k_to_11k025 above, which already ships as the second stage of the
    # 48 -> 11.025 kHz composed row, and design_prestage() finds that partner by (stage rate, final
    # Fs_out) rather than being told -- so the partner-relaxed stopband this row is gated against is
    # derived from the partner actually in the image, not from a number written here.
    #   [internal] study_remaining_rate_pairs_20260920.md, Phase 1.
    ("dec2_32k_for_11k025", 32000.0, 2, 11025.0),

    # ---- THE UNIVERSAL AK512 BANK, decimating first stages (Phase I SS3).
    #
    # NOTE THE /3 STAGES AT 96 kHz.  The shipped 96 kHz routes all begin with a /2 pre-stage to
    # 48 kHz, and seven universal rows instead take /3 to 32 kHz.  That is not a new kernel: the
    # composed front end's decimation factor is a runtime field
    # (asrc_decimator_q31_init_dec_then_lpf1to1 passes d0 to dec_q31_stage_set), so a /3 row is a
    # table entry.  It IS a new stage RATE for the block-capacity arithmetic, which is why every
    # row below gets its own _Static_assert on the intermediate buffer and the ring arena.
    #
    # 96 -> 11.025 kHz USES /3 + 1:1 HERE, NOT the cheaper three-stage decimator re-spec Phase H
    # selected (/2@96k N27 -> /2@48k N25 -> /2@24k N173, 60.4 % @1.90 against 70.0 % here).  The
    # re-spec needs a decimators-only shape with three distinct coefficient sets, which no row
    # shape in the front end expresses; the two land in the SAME CPU tier at BOTH constants
    # (model-over @1.90, 12ch-model-only @1.42), so the substitution costs a tier nothing and
    # buys the whole bank one structural family.  Recorded rather than silent: the re-spec stays
    # the cheaper candidate on host, unimplemented.
    ("dec2_48k_for_16k", 48000.0, 2, 16000.0),     # 48 -> 16 k
    ("dec3_48k_for_8k", 48000.0, 3, 8000.0),       # 48 -> 8 k
    ("dec3_48k_for_11k025", 48000.0, 3, 11025.0),  # 48 -> 11.025 k
    ("dec3_48k_for_12k", 48000.0, 3, 12000.0),     # 48 -> 12 k
    ("dec2_96k_for_32k", 96000.0, 2, 32000.0),     # 96 -> 32 k
    ("dec3_96k_for_8k", 96000.0, 3, 8000.0),       # 96 -> 8 k
    ("dec3_96k_for_11k025", 96000.0, 3, 11025.0),  # 96 -> 11.025 k
    ("dec3_96k_for_12k", 96000.0, 3, 12000.0),     # 96 -> 12 k
    ("dec3_96k_for_16k", 96000.0, 3, 16000.0),     # 96 -> 16 k
    ("dec3_96k_for_22k05", 96000.0, 3, 22050.0),   # 96 -> 22.05 k
    ("dec3_96k_for_24k", 96000.0, 3, 24000.0),     # 96 -> 24 k

    # ---- 44.1 -> 12 kHz pre-stages, one per candidate shape (see the two 1:1 partners above).
    ("dec2_44k1_for_12k", 44100.0, 2, 12000.0),    # -> 22.05 kHz, partner lp1_22k05_to_12k
    ("dec3_44k1_for_12k", 44100.0, 3, 12000.0),    # -> 14.7 kHz,  partner lp1_14k7_to_12k

    # ---- 44.1 -> 8 kHz pre-stages, one per candidate shape (see the two 1:1 partners above).
    ("dec4_44k1_for_8k", 44100.0, 4, 8000.0),      # -> 11.025 kHz, partner lp1_11k025_to_8k
    ("dec3_44k1_for_8k", 44100.0, 3, 8000.0),      # -> 14.7 kHz,   partner lp1_14k7_to_8k
)


def q31_round_trip(coeff: np.ndarray) -> np.ndarray:
    """Exactly dec_q31_from_float(): round-to-nearest at absolute scale 2^31, then back."""
    v = np.asarray(coeff, dtype=np.float64) * 2147483648.0
    v = np.where(v >= 2147483647.0, 2147483647.0, v)
    v = np.where(v <= -2147483648.0, -2147483648.0, v)
    q = np.where(v >= 0.0, np.floor(v + 0.5), np.ceil(v - 0.5))
    return q / 2147483648.0


def q31_words(coeff: np.ndarray) -> np.ndarray:
    v = np.asarray(coeff, dtype=np.float64) * 2147483648.0
    v = np.where(v >= 2147483647.0, 2147483647.0, v)
    v = np.where(v <= -2147483648.0, -2147483648.0, v)
    return np.where(v >= 0.0, np.floor(v + 0.5), np.ceil(v - 0.5)).astype(np.int64)


def design_one(fs: float, fs_out: float, verbose: bool = True):
    """Smallest odd tap count whose Q31-quantized chain clears the design gate."""
    fp = PROTECT_RATIO * fs_out * 0.5
    fstop = fs_out - fp
    n0 = M.tap_law(fs, fstop - fp)
    rows = []
    chosen = None
    n = n0 if n0 % 2 else n0 + 1
    while n <= 401:
        coeff = M.dsg.design(n, fs, fp, fstop, M.KAISER_BETA)
        ch = M.Chain(f"lp1@{fs / 1000:.4g}k", "A2",
                     [M.Stage("lpf", fs, fs, n, fp, fstop, coeff, 1, 1, 1, 0.0)],
                     fs, fs_out)
        rf = M.evaluate(ch, fs, fs_out, fp)
        qc = q31_round_trip(np.asarray(coeff, dtype=np.float64))
        chq = M.Chain(ch.label, "A2",
                      [M.Stage("lpf", fs, fs, n, fp, fstop, qc, 1, 1, 1, 0.0)],
                      fs, fs_out)
        rq = M.evaluate(chq, fs, fs_out, fp)
        l1 = float(np.abs(qc).sum())
        rows.append((n, rf.worst_alias_db, rq.worst_alias_db, rq.passband_err_db, l1))
        ok = (rq.worst_alias_db <= ALIAS_DESIGN_DB) and (rq.passband_err_db <= PASSBAND_LIMIT_DB)
        if ok and chosen is None:
            chosen = (n, coeff, qc, rf, rq, l1, fp, fstop, ch)
            break
        n += 2
    return chosen, rows, n0, fp, fstop


def design_prestage(fs_in: float, d: int, fs_out: float, verbose: bool = True):
    """Smallest odd tap count whose Q31 COMPOSED CHAIN (this stage + the 1:1 + rear) passes.

    Gated on the chain, not on the stage: a first stage judged alone would be specified against
    its own Nyquist and would come out at the shipped 107 taps again.
    """
    fs_mid = fs_in / d
    fp = PROTECT_RATIO * fs_out * 0.5
    fstop_partner = fs_out - fp                 # the 1:1 stage's stopband edge
    fstop = fs_mid - fstop_partner              # all this stage has to reach
    # The partner 1:1 stage, exactly as design_one() built it and the generator emitted it.
    partner = next(x for x in STAGES if abs(x[1] - fs_mid) < 1.0 and abs(x[2] - fs_out) < 1.0)
    pchosen, _, _, pfp, pfstop = design_one(partner[1], partner[2], verbose=False)
    if pchosen is None:
        return None, [], 0, fp, fstop
    pn, pcoeff = pchosen[0], pchosen[1]
    plpf = M.Stage("lpf", fs_mid, fs_mid, pn, pfp, pfstop,
                   q31_round_trip(np.asarray(pcoeff, dtype=np.float64)), 1, 1, 1, 0.0)
    n0 = M.tap_law(fs_in, fstop - fp)
    rows = []
    n = n0 if n0 % 2 else n0 + 1
    while n <= 401:
        coeff = M.dsg.design(n, fs_in, fp, fstop, M.KAISER_BETA)
        qc = q31_round_trip(np.asarray(coeff, dtype=np.float64))
        s0f = M.Stage("dec", fs_in, fs_mid, n, fp, fstop, coeff, d, 1, 1, 0.0)
        s0q = M.Stage("dec", fs_in, fs_mid, n, fp, fstop, qc, d, 1, 1, 0.0)
        rf = M.evaluate(M.Chain("f", "A1+A2", [s0f, plpf], fs_mid, fs_out), fs_in, fs_out, fp)
        rq = M.evaluate(M.Chain("q", "A1+A2", [s0q, plpf], fs_mid, fs_out), fs_in, fs_out, fp)
        l1 = float(np.abs(qc).sum())
        rows.append((n, rf.worst_alias_db, rq.worst_alias_db, rq.passband_err_db, l1))
        if (rq.worst_alias_db <= ALIAS_DESIGN_DB) and (rq.passband_err_db <= PASSBAND_LIMIT_DB):
            return (n, coeff, qc, rf, rq, l1, fp, fstop, pn), rows, n0, fp, fstop
        n += 2
    return None, rows, n0, fp, fstop


def render(entries, pre_entries=()) -> str:
    """Emit the coefficient include, in the style of asrc_decimator_48_to_8_coeffs.inc."""
    out = [
        "/*",
        " * asrc_lpf1to1_coeffs.inc -- 1:1 source-rate anti-alias LPF coefficient sets.",
        " *",
        " * Auto-generated by tools/asrc/asrc_lpf1to1_design.py -- DO NOT EDIT BY HAND.",
        " * Regenerate with:  python tools/asrc/asrc_lpf1to1_design.py --write",
        " *",
        " * A 1:1 stage does not change the rate.  It band-limits at the SOURCE rate and lets the",
        " * existing rear asynchronous ASRC do the whole rate change, which is what keeps the rear",
        " * M30 resampler's fc = 0.464 * (its own input rate) above the protected band edge.  A",
        " * front end that decimated to Fs_out would put the rear end at step 1.0, where its",
        " * -0.1 dB point is only 0.778 * FN -- below the 0.8125 * FN this project guarantees.",
        " *",
        f" * Uniform specification (owner, 2026-09-14): protected 0 .. {PROTECT_RATIO} * FN,",
        f" * passband +/-{PASSBAND_LIMIT_DB:.2f} dB, alias <= {ALIAS_PRODUCT_DB:.0f} dB product /",
        f" * {ALIAS_DESIGN_DB:.0f} dB internal design target.  Kaiser beta = {M.KAISER_BETA}.",
        " *",
        " * Coefficients are float here and converted once at init by dec_q31_from_float().",
        " * The CRC32 below is over the little-endian IEEE-754 float bytes, sets in table order.",
        " */",
        "",
    ]
    blob = b""
    for name, fs, fs_out, n, coeff, rf, rq, l1, fp, fstop in entries:
        blob += np.asarray(coeff, dtype="<f4").tobytes()
    for name, fs, d, fs_out, n, coeff, rf, rq, l1, fp, fstop, pn in pre_entries:
        blob += np.asarray(coeff, dtype="<f4").tobytes()
    crc = zlib.crc32(blob) & 0xFFFFFFFF
    out.append(f"#define ASRC_LPF1TO1_GENERATED_CRC32  (0x{crc:08X}UL)")
    out.append("")
    for name, fs, fs_out, n, coeff, rf, rq, l1, fp, fstop in entries:
        upper = name.upper()
        out.append(f"/* {fs / 1000:.4g} kHz 1:1 stage for a {fs_out / 1000:.4g} kHz output:")
        out.append(f" * passband {fp:.1f} Hz, stopband {fstop:.1f} Hz, {n} taps.")
        out.append(f" * Host gate: worst alias into 0..{fp:.0f} Hz = {rq.worst_alias_db:.2f} dB")
        out.append(f" * (float design {rf.worst_alias_db:.2f} dB), passband error "
                   f"{rq.passband_err_db:.4f} dB, sum|h| = {l1:.4f}. */")
        out.append(f"#define ASRC_{upper}_TAPS  ({n}u)")
        out.append(f"static const float s_{name}_coeff[ASRC_{upper}_TAPS] =")
        out.append("{")
        vals = [f"{float(v):+.9e}f" for v in np.asarray(coeff, dtype="<f4")]
        for i in range(0, len(vals), 4):
            out.append("    " + ", ".join(vals[i:i + 4]) + ("," if i + 4 < len(vals) else ""))
        out.append("};")
        out.append("")
    for name, fs, d, fs_out, n, coeff, rf, rq, l1, fp, fstop, pn in pre_entries:
        upper = name.upper()
        fs_mid = fs / d
        out.append(f"/* /{d} FIRST STAGE of the composed {fs / 1000:.4g} -> {fs_out / 1000:.4g} kHz")
        out.append(f" * chain: {fs / 1000:.4g} kHz -> /{d} -> {fs_mid / 1000:.4g} kHz -> the 1:1 set")
        out.append(f" * above ({pn} taps) -> rear ASRC at step {fs_mid / fs_out:.6f}.")
        out.append(" *")
        out.append(f" * passband {fp:.1f} Hz, stopband {fstop:.1f} Hz, {n} taps.  The stopband is")
        out.append(f" * RELAXED BY THE PARTNER STAGE: alone a /{d} at this rate would have to reach")
        out.append(f" * {fs_mid / 2:.1f} Hz, but the 1:1 stage behind it removes"
                   f" {fs_out - fp:.0f}..{fs_mid / 2:.0f} Hz, so only"
                   f" {fs_mid:.0f} - {fs_out - fp:.0f} = {fstop:.0f} Hz has to be clean.")
        out.append(" * The two stages therefore share ONE specification and must be regenerated")
        out.append(" * together -- which is why this set lives here and not with the /2 sets in")
        out.append(" * asrc_decimator_48_to_8_design.py.")
        out.append(f" * Host gate over the WHOLE chain: worst alias into 0..{fp:.0f} Hz ="
                   f" {rq.worst_alias_db:.2f} dB")
        out.append(f" * (float design {rf.worst_alias_db:.2f} dB), passband error "
                   f"{rq.passband_err_db:.4f} dB, sum|h| = {l1:.4f}. */")
        out.append(f"#define ASRC_{upper}_TAPS  ({n}u)")
        out.append(f"#define ASRC_{upper}_DECIM  ({d}u)")
        out.append(f"static const float s_{name}_coeff[ASRC_{upper}_TAPS] =")
        out.append("{")
        vals = [f"{float(v):+.9e}f" for v in np.asarray(coeff, dtype="<f4")]
        for i in range(0, len(vals), 4):
            out.append("    " + ", ".join(vals[i:i + 4]) + ("," if i + 4 < len(vals) else ""))
        out.append("};")
        out.append("")
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--curve", action="store_true", help="print the tap-vs-alias search")
    a = ap.parse_args()

    print("=" * 100)
    print("1:1 SOURCE-RATE ANTI-ALIAS LPF -- host design gate")
    print("=" * 100)
    print(f"uniform spec : protected 0..{PROTECT_RATIO} * FN, passband +/-{PASSBAND_LIMIT_DB} dB,")
    print(f"               alias {ALIAS_PRODUCT_DB:.0f} dB product / {ALIAS_DESIGN_DB:.0f} dB design")
    print(f"rear end     : production Q31 M30, fc = {M.REAR.cutoff} of ITS OWN input rate "
          "(fixed for this design run)")
    print()
    entries = []
    failed = False
    for name, fs, fs_out in STAGES:
        chosen, rows, n0, fp, fstop = design_one(fs, fs_out)
        print("-" * 100)
        print(f"{name}:  {fs / 1000:.4g} kHz 1:1 -> rear ASRC -> {fs_out / 1000:.4g} kHz "
              f"(step {fs / fs_out:.5f})")
        print(f"  Fp = {fp:.1f} Hz   Fstop = {fstop:.1f} Hz   transition {fstop - fp:.1f} Hz "
              f"({(fstop - fp) / fs:.5f} of fs)")
        print(f"  tap law N = {n0}")
        if a.curve:
            print("      N   alias(float)   alias(Q31)   pb err    sum|h|")
            for n, af, aq, pb, l1 in rows:
                print(f"    {n:3d}   {af:11.2f}   {aq:10.2f}   {pb:7.4f}   {l1:.4f}")
        if chosen is None:
            print("  FAIL: no tap count <= 401 clears the design gate")
            failed = True
            continue
        n, coeff, qc, rf, rq, l1, fp, fstop, ch = chosen
        words = q31_words(np.asarray(coeff, dtype=np.float64))
        print(f"  CHOSEN N = {n}")
        print(f"    worst alias into 0..{fp:.0f} Hz : float {rf.worst_alias_db:8.2f} dB   "
              f"Q31 {rq.worst_alias_db:8.2f} dB   (design gate {ALIAS_DESIGN_DB:.0f})")
        print(f"    worst alias, whole 0..FN       : float {rf.alias_full_band_db:8.2f} dB   "
              f"Q31 {rq.alias_full_band_db:8.2f} dB   (reported, not gated)")
        print(f"    passband error over 0..Fp      : {rq.passband_err_db:.4f} dB "
              f"(limit {PASSBAND_LIMIT_DB:.2f})   at Fp {rq.passband_at_fp_db:+.4f} dB")
        print(f"    of which the rear M30 alone    : {rq.rear_only_at_fp_db:+.4f} dB at Fp")
        print(f"    Q31 degradation                : {rq.worst_alias_db - rf.worst_alias_db:+.4f} dB")
        print(f"    coefficient peak               : {float(np.abs(qc).max()):.6f} "
              f"(Q31 word {int(np.abs(words).max())}, {100.0 * float(np.abs(qc).max()):.1f} % FS)")
        print(f"    sum|h| (L1, adversarial gain)  : {l1:.4f} ({20.0 * np.log10(l1):+.2f} dB) "
              "-- shipped sets span 1.4146..2.1379")
        print(f"    DC gain (must be 1.0)          : {float(np.asarray(qc).sum()):.9f}")
        print(f"    MAC/s/ch                       : {n * fs:.0f}   cost {n * fs / 48000.0:.1f} u")
        print(f"    predicted load 16 ch / 12 ch   : "
              f"{M.load_percent(n * fs, 16):.1f} % / {M.load_percent(n * fs, 12):.1f} %")
        print(f"    ring needed (taps + batch - 1) : {n + 16 - 1} samples/ch")
        print(f"    flash                          : {4 * n} B  (float table {4 * n} B + "
              f"Q31 image {4 * n} B at init)")
        entries.append((name, fs, fs_out, n, coeff, rf, rq, l1, fp, fstop))
    print("-" * 100)
    print()

    pre_entries = []
    for name, fs_in, d, fs_out in PRESTAGES:
        chosen, rows, n0, fp, fstop = design_prestage(fs_in, d, fs_out)
        fs_mid = fs_in / d
        print("-" * 100)
        print(f"{name}:  {fs_in / 1000:.4g} kHz -> /{d} -> {fs_mid / 1000:.4g} kHz -> 1:1 -> "
              f"rear ASRC -> {fs_out / 1000:.4g} kHz (rear step {fs_mid / fs_out:.6f})")
        print(f"  Fp = {fp:.1f} Hz   Fstop = {fstop:.1f} Hz (relaxed by the 1:1 stage; a "
              f"stand-alone /{d} would need {fs_mid / 2:.1f} Hz)")
        print(f"  tap law N = {n0}   -- gated on the WHOLE chain, not on this stage alone")
        if a.curve:
            print("      N   alias(float)   alias(Q31)   pb err    sum|h|")
            for n, af, aq, pb, l1 in rows:
                print(f"    {n:3d}   {af:11.2f}   {aq:10.2f}   {pb:7.4f}   {l1:.4f}")
        if chosen is None:
            print("  FAIL: no tap count <= 401 clears the design gate")
            failed = True
            continue
        n, coeff, qc, rf, rq, l1, fp, fstop, pn = chosen
        macs = (n * fs_mid) + (pn * fs_mid)
        print(f"  CHOSEN N = {n}   (partner 1:1 stage {pn} taps)")
        print(f"    chain worst alias into 0..{fp:.0f} Hz : float {rf.worst_alias_db:8.2f} dB   "
              f"Q31 {rq.worst_alias_db:8.2f} dB   (design gate {ALIAS_DESIGN_DB:.0f})")
        print(f"    chain worst alias, whole 0..FN      : Q31 {rq.alias_full_band_db:8.2f} dB "
              "(reported, not gated)")
        print(f"    chain passband error over 0..Fp     : {rq.passband_err_db:.4f} dB "
              f"(limit {PASSBAND_LIMIT_DB:.2f})   at Fp {rq.passband_at_fp_db:+.4f} dB")
        print(f"    sum|h| (L1)                         : {l1:.4f}   DC gain "
              f"{float(np.asarray(qc).sum()):.9f}")
        print(f"    BOTH stages, MAC/s/ch               : {macs:.0f}   cost "
              f"{macs / 48000.0:.1f} u")
        print(f"    predicted load 16 ch / 12 ch        : {M.load_percent(macs, 16):.1f} % / "
              f"{M.load_percent(macs, 12):.1f} % at 1.90 cyc/MAC "
              f"({M.load_percent(macs, 16) * 1.42 / 1.90:.1f} % / "
              f"{M.load_percent(macs, 12) * 1.42 / 1.90:.1f} % at 1.42)")
        print(f"    ring needed (taps + (batch-1)*{d})   : {n + (16 - 1) * d} samples/ch")
        print(f"    flash                               : {4 * n} B")
        pre_entries.append((name, fs_in, d, fs_out, n, coeff, rf, rq, l1, fp, fstop, pn))
    print("-" * 100)
    print()

    if a.write and not failed:
        text = render(entries, pre_entries)
        OUTPUT.write_text(text, encoding="ascii", newline="\n")
        print(f"wrote {OUTPUT.relative_to(ROOT).as_posix()} ({len(text)} bytes)")
    elif not a.write:
        print("(no include written; pass --write)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
