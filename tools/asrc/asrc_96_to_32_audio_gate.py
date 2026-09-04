#!/usr/bin/env python3
"""Host audio gate for the COMPOSED 96 -> 32 kHz AUDIO MODE front end (/2 then L=2/M=3, Q31).

WHAT THIS MEASURES

The 96 kHz leg does not get its own 96->32 filter.  It reuses two shipping tables back to back:

    96 kHz --[ shared /2 pre-stage, 41 taps ]--> 48 kHz --[ L=2/M=3, N=97 ]--> 32 kHz --[ rear ]

so the routing gate publishes the COMPOSED ratio num 2 / den 6, and this gate measures the
COMPOSED chain.  Both include files are read back and CRC-checked against the design script, so
a hand edit or a stale include fails the gate instead of being measured around.

WHAT IT DOES NOT CLAIM

Same partial protection as the 48 kHz leg, inherited unchanged: 0-13 kHz is protected, and
13-16 kHz deliberately keeps residual aliasing.  Alias is therefore reported PER BAND and is
never collapsed into one "worst alias" figure.

ALIAS BOOKKEEPING, TWO FOLDS DEEP.  An input tone at f on the 96 kHz axis is shaped by the
pre-stage at f and lands at g = fold(f, 48000) on the 48 kHz axis -- so every input above
24 kHz is already an alias source before the rational stage is reached, and the pre-stage's
stopband is what holds it down.  Zero-stuffing by L=2 then makes the 48 kHz-rate spectrum
periodic with 48 kHz on the 96 kHz axis, so g appears at BOTH g and 48000-g, each shaped by the
prototype at ITS OWN frequency and folded by M=3 into fold(., 32000).  A contribution is an
alias whenever its landing frequency differs from the ORIGINAL f, which is why the plain
"f > out_nyquist" mask that serves the integer front ends is not used here.

Run from the repository root.  Only NumPy is required.
"""
from __future__ import annotations

import contextlib
import dataclasses
import importlib.util
import io
import pathlib
import re
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
INC_PRE = ROOT / "src/app/apps/asrc/asrc_decimator_96_to_48_coeffs.inc"
INC_R23 = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_32_coeffs.inc"


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


chk = _load("chk", "tools/asrc/asrc_headroom_filter_check.py")
dsg = _load("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")
gate48 = _load("gate48", "tools/asrc/asrc_48_to_32_audio_gate.py")

L_REAR = chk.L                       # 128 resampler phases
SHIPPED = chk.CANDIDATES[1]          # headroom-m30-kaiser11: 30 taps, fc 0.465 of ITS input rate
IN_HZ = 96000.0                      # the leg's input rate
MID_HZ = 48000.0                     # after the /2 pre-stage
OUT_HZ = 32000.0
OUT_NYQ = 16000.0
PROTO_HZ = 96000.0                   # the L=2 interpolated rate (same axis as the 96 kHz input)
NFFT = 1 << 16
FREQS = np.linspace(0.0, 48000.0, 48001)          # 1 Hz steps over the whole 96 kHz input band

PROTECT_HZ = 13000.0
PROTECT_GATE_DB = -105.0
ALIAS_BANDS = ((0.0, 8000.0), (0.0, 10000.0), (0.0, 12000.0),
               (0.0, 13000.0), (13000.0, 16000.0), (0.0, 16000.0))
PASSBAND_POINTS = (10000.0, 12000.0, 13000.0, 13500.0, 14000.0, 15000.0)
DROOP_POINTS = (-0.1, -0.5, -1.0, -3.0, -6.0)


def read_prestage_row(path: pathlib.Path) -> tuple[np.ndarray, int]:
    """Read the SHARED /2 row (not the two wide variants, which are never composed)."""
    text = path.read_text(encoding="ascii")
    m = re.search(r"#define\s+ASRC_DECIMATOR_96_TO_48_GENERATED_CRC32\s+\(0x([0-9A-Fa-f]+)UL\)",
                  text)
    if m is None:
        raise SystemExit(f"ERROR: shared CRC32 not found in {path}")
    crc = int(m.group(1), 16)
    a = re.search(r"static const float s_96_to_48_coeff\[(\d+)u\]\s*=\s*\{(.*?)\};", text, re.S)
    if a is None:
        raise SystemExit(f"ERROR: s_96_to_48_coeff not found in {path}")
    vals = [float(v) for v in re.findall(r"-?\d+\.\d+e[+-]\d+", a.group(2))]
    if len(vals) != int(a.group(1)):
        raise SystemExit(f"ERROR: s_96_to_48_coeff length {len(vals)} != declared {a.group(1)}")
    return np.asarray(vals, dtype="<f4"), crc


def rear_prototype() -> np.ndarray:
    cand = dataclasses.replace(SHIPPED)
    rows = chk.phase_rows(cand)
    proto = np.zeros(cand.taps * L_REAR)
    for p in range(L_REAR):
        for t in range(cand.taps):
            proto[t * L_REAR - p + (L_REAR - 1)] = rows[p, t]
    return proto / proto.sum()


mag_at = gate48.mag_at
fold = gate48.fold
db = gate48.db


def main() -> int:
    # ---------------------------------------------------------------- shipping tables, verified
    pre, pre_crc = read_prestage_row(INC_PRE)
    want_pre = dsg.design(dsg.PRESTAGE_TAPS, dsg.PRESTAGE_INPUT_HZ,
                          dsg.PRESTAGE_PASSBAND_HZ, dsg.PRESTAGE_STOPBAND_HZ)
    want_pre_crc = dsg.coefficient_crc32(want_pre)
    if pre_crc != want_pre_crc or not np.array_equal(pre, want_pre.astype("<f4")):
        print(f"ERROR: {INC_PRE.name} shared row CRC32 0x{pre_crc:08X} != design "
              f"0x{want_pre_crc:08X} -- regenerate with asrc_decimator_48_to_8_design.py --write",
              file=sys.stderr)
        return 1

    phase0, phase1, r23_crc = gate48.read_inc_rows(INC_R23)
    want_p0, want_p1, _ = dsg.design_32k_polyphase()
    if r23_crc != dsg.coefficient_crc32(want_p0, want_p1) or \
            not (np.array_equal(phase0, want_p0) and np.array_equal(phase1, want_p1)):
        print(f"ERROR: {INC_R23.name} differs from the design script output", file=sys.stderr)
        return 1

    proto = gate48.prototype_from_rows(phase0, phase1)
    rear = rear_prototype()

    print("=" * 96)
    print("96 -> 32 kHz AUDIO MODE, COMPOSED (/2 then L=2/M=3, N=97, Q31) -- host audio gate")
    print("=" * 96)
    print(f"pre-stage    : {INC_PRE.relative_to(ROOT).as_posix()}  CRC32 0x{pre_crc:08X}")
    print(f"               shared /2 row, {pre.size} taps at {IN_HZ:.0f} Hz, "
          f"pass {dsg.PRESTAGE_PASSBAND_HZ:.0f} Hz, stop {dsg.PRESTAGE_STOPBAND_HZ:.0f} Hz")
    print(f"rational     : {INC_R23.relative_to(ROOT).as_posix()}  CRC32 0x{r23_crc:08X}")
    print(f"               L={dsg.POLY32_L} M={dsg.POLY32_M} N={dsg.POLY32_PROTO_TAPS} "
          f"(phase0 {phase0.size} + phase1 {phase1.size}), prototype at {PROTO_HZ:.0f} Hz")
    print(f"rear end     : shipping Q31 resampler, fc = {SHIPPED.cutoff} of 32000 Hz "
          f"= {SHIPPED.cutoff * OUT_HZ:.0f} Hz, L={L_REAR}, M={SHIPPED.taps} (UNCHANGED)")
    print(f"published    : num {dsg.POLY32_L} / den {2 * dsg.POLY32_M}  (composed ratio)")
    print()

    # ---------------------------------------------------------------- passband
    band = FREQS[:int(OUT_NYQ) + 1]
    pre_pb = mag_at(pre, IN_HZ, band)
    fe_pb = mag_at(proto, PROTO_HZ, band)
    rear_pb = mag_at(rear, float(L_REAR) * OUT_HZ, band)
    total_pb = pre_pb * fe_pb * rear_pb

    print("-" * 96)
    print("1. PASSBAND  (the rear end's fc = 14880 Hz is still the ceiling; the pre-stage is flat "
          "here)")
    print("-" * 96)
    print("%10s %12s %12s %12s %12s" % ("f (Hz)", "pre-stage", "rational", "rear end", "total"))
    for f in PASSBAND_POINTS:
        i = int(round(f))
        print("%10.0f %9.3f dB %9.2f dB %9.2f dB %9.2f dB"
              % (f, db(pre_pb[i]), db(fe_pb[i]), db(rear_pb[i]), db(total_pb[i])))
    print()
    print("%10s %12s %12s" % ("droop", "pre-stage", "total"))
    for target in DROOP_POINTS:
        def cross(curve: np.ndarray) -> str:
            below = np.nonzero(curve <= target)[0]
            return f"{band[below[0]]:.0f} Hz" if below.size else "-"
        print("%9.1f dB %12s %12s" % (target, cross(db(pre_pb)), cross(db(total_pb))))
    print()

    # ---------------------------------------------------------------- alias, two folds deep
    print("-" * 96)
    print("2. ALIAS, PER OUTPUT BAND  (worst over every input 0-48000 Hz that folds into it)")
    print("-" * 96)
    pre_mag = mag_at(pre, IN_HZ, FREQS)          # shaped at the ORIGINAL 96 kHz-axis frequency
    mid = fold(FREQS, MID_HZ)                    # where it lands on the 48 kHz axis
    worst = {b: (-300.0, 0.0, 0.0) for b in ALIAS_BANDS}
    for image in (mid, MID_HZ - mid):
        land = fold(image, OUT_HZ)
        mag = pre_mag * mag_at(proto, PROTO_HZ, image) \
            * mag_at(rear, float(L_REAR) * OUT_HZ, land)
        is_alias = np.abs(land - FREQS) > 0.5
        level = db(mag)
        for b in ALIAS_BANDS:
            lo, hi = b
            sel = is_alias & (land >= lo) & (land <= hi)
            if not sel.any():
                continue
            k = int(np.argmax(np.where(sel, level, -np.inf)))
            if level[k] > worst[b][0]:
                worst[b] = (float(level[k]), float(FREQS[k]), float(land[k]))
    print("%18s %12s %14s %14s" % ("output band", "worst", "from input", "lands at"))
    for b in ALIAS_BANDS:
        lo, hi = b
        level, src, dst = worst[b]
        print("%13.0f-%-6.0f %9.2f dB %11.0f Hz %11.0f Hz" % (lo, hi, level, src, dst))
    print()

    protected = worst[(0.0, PROTECT_HZ)][0]
    residual = worst[(PROTECT_HZ, OUT_NYQ)][0]
    full = worst[(0.0, OUT_NYQ)][0]

    # ---------------------------------------------------------------- what the pre-stage bought
    print("-" * 96)
    print("3. THE PRE-STAGE'S OWN JOB  (24-48 kHz input, which only a 96 kHz leg can present)")
    print("-" * 96)
    for f in (24000.0, 26000.0, 32000.0, 35000.0, 40000.0, 48000.0):
        i = int(round(f))
        print("%10.0f Hz  pre-stage %9.2f dB   lands at %6.0f Hz on the 48 kHz axis"
              % (f, db(pre_mag[i]), mid[i]))
    print()

    print("-" * 96)
    print("4. VERDICT")
    print("-" * 96)
    ok = protected <= PROTECT_GATE_DB
    print(f"protected band 0-{PROTECT_HZ / 1000.0:.0f} kHz : {protected:7.2f} dB "
          f"against a {PROTECT_GATE_DB:.0f} dB gate -> {'PASS' if ok else 'FAIL'}")
    print(f"relaxed band {PROTECT_HZ / 1000.0:.0f}-{OUT_NYQ / 1000.0:.0f} kHz  : "
          f"{residual:7.2f} dB  -- RESIDUAL ALIAS BY DESIGN, no gate, a human decides "
          "whether this is acceptable product audio")
    print(f"whole band 0-{OUT_NYQ / 1000.0:.0f} kHz     : {full:7.2f} dB  "
          "-- reported so the residual is never hidden behind the protected figure")
    print()
    print("This is 96->32 AUDIO MODE / partial protection, inherited from the 48 kHz leg.")
    print("It is NOT full-band, NOT strict, NOT alias-free, and NOT '0-16 kHz fully protected'.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
