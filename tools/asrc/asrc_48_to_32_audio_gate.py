#!/usr/bin/env python3
"""Host audio gate for the 48 -> 32 kHz AUDIO MODE front end (L=2/M=3, N=97, Q31).

WHAT THIS MEASURES, AND WHAT IT DOES NOT CLAIM

This is NOT a strict 48->32 front end.  It band-limits enough to protect 0-13 kHz and
deliberately leaves residual aliasing in 13-16 kHz.  The gate therefore reports alias PER BAND
and never collapses the answer into one "worst alias" number: a single figure taken over
0-13 kHz would read about -105 dB and would hide the 13-16 kHz residual entirely.

The measured object is the SHIPPING coefficient table.  The two phase rows are read back out of
src/app/apps/asrc/asrc_decimator_48_to_32_coeffs.inc, their CRC32 is checked against the design
script, and the L=2 prototype is reconstructed from them -- so a hand edit of the include, or a
stale include, fails the gate instead of being measured around.

The chain under test is front end THEN the shipping Q31 rear resampler, unchanged:
ASRC_POLY_FC = 0.465 of ITS OWN input rate (which is 32 kHz once the front end is in front of
it), L = 128 phases, M = 30 taps.  The rear end is what sets the passband ceiling here, not the
front end -- 0.465 * 32000 = 14880 Hz.

ALIAS BOOKKEEPING.  Zero-stuffing by L=2 makes the 48 kHz-rate spectrum periodic with 48 kHz on
the 96 kHz axis, so one input tone at f appears there at BOTH f and 48000-f.  Each image is
shaped by the prototype at ITS OWN frequency and then folded by the M=3 decimation into
fold(., 32000).  A contribution is an alias whenever its landing frequency differs from f, which
makes even an input BELOW the output Nyquist an alias source through its 48000-f image.  The
plain "f > out_nyquist" mask that serves the integer front ends is therefore WRONG here and is
not used.

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
INC = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_32_coeffs.inc"


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


chk = _load("chk", "tools/asrc/asrc_headroom_filter_check.py")
dsg = _load("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")

L_REAR = chk.L                       # 128 resampler phases
SHIPPED = chk.CANDIDATES[1]          # headroom-m30-kaiser11: 30 taps, fc 0.465 of ITS input rate
IN_HZ = 48000.0
OUT_HZ = 32000.0
OUT_NYQ = 16000.0
PROTO_HZ = 96000.0                   # the L=2 interpolated rate
NFFT = 1 << 16                       # 96 kHz / 65536 = 1.46 Hz bins
FREQS = np.linspace(0.0, 24000.0, 24001)          # 1 Hz steps over the 48 kHz input band

PROTECT_HZ = 13000.0                 # the band this design DOES protect
PROTECT_GATE_DB = -105.0             # and the level it is expected to hold there
ALIAS_BANDS = ((0.0, 8000.0), (0.0, 10000.0), (0.0, 12000.0),
               (0.0, 13000.0), (13000.0, 16000.0), (0.0, 16000.0))
PASSBAND_POINTS = (10000.0, 12000.0, 13000.0, 13500.0, 14000.0, 15000.0)
DROOP_POINTS = (-0.1, -0.5, -1.0, -3.0, -6.0)


# ------------------------------------------------------------------ shipping table, read back
def read_inc_rows(path: pathlib.Path) -> tuple[np.ndarray, np.ndarray, int]:
    text = path.read_text(encoding="ascii")

    def define(name: str) -> int:
        m = re.search(rf"#define\s+{name}\s+\(0x([0-9A-Fa-f]+)UL\)", text)
        if m is not None:
            return int(m.group(1), 16)
        m = re.search(rf"#define\s+{name}\s+\((\d+)u\)", text)
        if m is None:
            raise SystemExit(f"ERROR: {name} not found in {path}")
        return int(m.group(1))

    def array(name: str) -> np.ndarray:
        m = re.search(rf"static const float {name}\[(\d+)u\]\s*=\s*\{{(.*?)\}};",
                      text, re.S)
        if m is None:
            raise SystemExit(f"ERROR: {name} not found in {path}")
        vals = [float(v) for v in re.findall(r"-?\d+\.\d+e[+-]\d+", m.group(2))]
        if len(vals) != int(m.group(1)):
            raise SystemExit(f"ERROR: {name} length {len(vals)} != declared {m.group(1)}")
        return np.asarray(vals, dtype="<f4")

    return array("s_48_to_32_phase0_coeff"), array("s_48_to_32_phase1_coeff"), \
        define("ASRC_DECIMATOR_48_TO_32_GENERATED_CRC32")


def prototype_from_rows(phase0: np.ndarray, phase1: np.ndarray) -> np.ndarray:
    """Undo the L=2 decomposition and the L gain, giving the unit-sum 96 kHz prototype."""
    proto = np.zeros(phase0.size + phase1.size, dtype=np.float64)
    proto[0::2] = np.asarray(phase0, dtype=np.float64) / float(dsg.POLY32_L)
    proto[1::2] = np.asarray(phase1, dtype=np.float64) / float(dsg.POLY32_L)
    return proto


# ------------------------------------------------------------------ responses
def mag_at(coeff: np.ndarray, fs: float, freqs_hz: np.ndarray) -> np.ndarray:
    spectrum = np.abs(np.fft.rfft(np.asarray(coeff, dtype=np.float64), NFFT))
    axis = np.linspace(0.0, fs * 0.5, spectrum.size)
    return np.interp(freqs_hz, axis, spectrum)


def fold(f: np.ndarray, rate: float) -> np.ndarray:
    r = np.asarray(f) % rate
    return np.minimum(r, rate - r)


def rear_prototype() -> np.ndarray:
    """The shipping resampler's interleaved polyphase prototype, as its init() builds it."""
    cand = dataclasses.replace(SHIPPED)
    rows = chk.phase_rows(cand)
    proto = np.zeros(cand.taps * L_REAR)
    for p in range(L_REAR):
        for t in range(cand.taps):
            proto[t * L_REAR - p + (L_REAR - 1)] = rows[p, t]
    return proto / proto.sum()


def db(x) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.asarray(x, dtype=np.float64), 1e-15))


def main() -> int:
    phase0, phase1, inc_crc = read_inc_rows(INC)
    want_p0, want_p1, _ = dsg.design_32k_polyphase()
    want_crc = dsg.coefficient_crc32(want_p0, want_p1)
    if inc_crc != want_crc:
        print(f"ERROR: {INC.name} CRC32 0x{inc_crc:08X} != design 0x{want_crc:08X} "
              "-- regenerate with asrc_decimator_48_to_8_design.py --write",
              file=sys.stderr)
        return 1
    if not (np.array_equal(phase0, want_p0) and np.array_equal(phase1, want_p1)):
        print(f"ERROR: {INC.name} coefficients differ from the design script output",
              file=sys.stderr)
        return 1

    proto = prototype_from_rows(phase0, phase1)
    rear = rear_prototype()

    print("=" * 96)
    print("48 -> 32 kHz AUDIO MODE (L=2/M=3, N=97, Q31) -- host audio gate")
    print("=" * 96)
    print(f"coefficients : {INC.relative_to(ROOT).as_posix()}  CRC32 0x{inc_crc:08X}")
    print(f"front end    : L={dsg.POLY32_L} M={dsg.POLY32_M} N={dsg.POLY32_PROTO_TAPS} "
          f"(phase0 {phase0.size} taps + phase1 {phase1.size} taps), "
          f"prototype at {PROTO_HZ:.0f} Hz, passband {dsg.POLY32_PASSBAND_HZ:.0f} Hz")
    print(f"rear end     : shipping Q31 resampler, fc = {SHIPPED.cutoff} of 32000 Hz "
          f"= {SHIPPED.cutoff * OUT_HZ:.0f} Hz, L={L_REAR}, M={SHIPPED.taps} (UNCHANGED)")
    print()

    # ---------------------------------------------------------------- passband
    fe_pb = mag_at(proto, PROTO_HZ, FREQS)
    rear_pb = mag_at(rear, float(L_REAR) * OUT_HZ, FREQS)
    total_pb = fe_pb * rear_pb

    print("-" * 96)
    print("1. PASSBAND  (the rear end's fc = 14880 Hz is the ceiling here, not the front end)")
    print("-" * 96)
    print("%10s %14s %14s %14s" % ("f (Hz)", "front end", "rear end", "total"))
    for f in PASSBAND_POINTS:
        i = int(round(f))
        print("%10.0f %11.2f dB %11.2f dB %11.2f dB"
              % (f, db(fe_pb[i]), db(rear_pb[i]), db(total_pb[i])))
    print()
    tot_db = db(total_pb)
    print("%10s %14s %14s %14s" % ("droop", "front end", "rear end", "total"))
    for target in DROOP_POINTS:
        def cross(curve: np.ndarray) -> str:
            below = np.nonzero(curve <= target)[0]
            below = below[below <= int(OUT_NYQ)]
            return f"{FREQS[below[0]]:.0f} Hz" if below.size else "-"
        print("%9.1f dB %14s %14s %14s"
              % (target, cross(db(fe_pb)), cross(db(rear_pb)), cross(tot_db)))
    print()

    # ---------------------------------------------------------------- alias, per band
    # For every input f, both images (f and 48000-f) are shaped and then folded.  Collect the
    # ones that land somewhere other than where they started.
    print("-" * 96)
    print("2. ALIAS, PER OUTPUT BAND  (worst over every input 0-24000 Hz that folds into it)")
    print("-" * 96)
    worst = {band: (-300.0, 0.0, 0.0) for band in ALIAS_BANDS}
    for image in (FREQS, IN_HZ - FREQS):
        land = fold(image, OUT_HZ)
        mag = mag_at(proto, PROTO_HZ, image) * mag_at(rear, float(L_REAR) * OUT_HZ, land)
        is_alias = np.abs(land - FREQS) > 0.5
        level = db(mag)
        for band in ALIAS_BANDS:
            lo, hi = band
            sel = is_alias & (land >= lo) & (land <= hi)
            if not sel.any():
                continue
            k = int(np.argmax(np.where(sel, level, -np.inf)))
            if level[k] > worst[band][0]:
                worst[band] = (float(level[k]), float(FREQS[k]), float(land[k]))
    print("%18s %12s %14s %14s" % ("output band", "worst", "from input", "lands at"))
    for band in ALIAS_BANDS:
        lo, hi = band
        level, src, dst = worst[band]
        print("%13.0f-%-6.0f %9.2f dB %11.0f Hz %11.0f Hz"
              % (lo, hi, level, src, dst))
    print()

    protected = worst[(0.0, PROTECT_HZ)][0]
    residual = worst[(PROTECT_HZ, OUT_NYQ)][0]
    full = worst[(0.0, OUT_NYQ)][0]

    print("-" * 96)
    print("3. VERDICT")
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
    print("This is 48->32 AUDIO MODE / partial protection.  It is NOT full-band, NOT strict,")
    print("NOT alias-free, and NOT '0-16 kHz fully protected'.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
