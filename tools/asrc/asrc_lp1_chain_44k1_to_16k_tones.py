#!/usr/bin/env python3
"""HOST prediction at the EXACT tone frequencies the 44.1 -> 16 kHz R2 capture injects.

The gate (asrc_lp1_chain_44k1_to_16k_gate.py) answers "does the chain meet the spec" by
sweeping a grid and reporting the single worst point.  A hardware capture cannot inject a
grid: it injects one table row at a time, and each row is a sample-domain-exact tone at a
frequency the table's (len, cycles) pin.  So the HOST number a measurement is compared
against has to be evaluated AT THAT FREQUENCY, not at the grid's worst point.

This prints that table.  It reuses the model's own branch propagation and the gate's own
chain construction -- nothing is re-derived here, so a change to either shows up in both.

Rows come from tools/asrc/gen_asrc_meas_tones.py (the 44.1 kHz block) and are read from the
generated header rather than retyped, so the two cannot drift.
"""
import importlib.util, io, contextlib, re, sys
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[2]

def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod

G = _load("gate44", "tools/asrc/asrc_lp1_chain_44k1_to_16k_gate.py")
M = G.M

FS_IN, FS_MID, FS_OUT, FP = G.FS_IN, G.FS_MID, G.FS_OUT, G.FP

# The ADOPTED design is the dedicated one -- the second entry the gate built.
name, s0 = [kv for kv in G.CANDIDATES.items() if kv[0].startswith("dedicated")][0]
chain = M.Chain(name, "A1+lpf", [s0, G.lp1], FS_MID, FS_OUT)

HDR = ROOT / "src/app/apps/asrc/audio_app_meas_tones.h"
ROW_RE = re.compile(r"#define MEAS_TONE_ROW_((?:PB44|AL44)_\d+)\s+\(\s*(\d+)u\)\s*//\s*(\d+) Hz")
TONES = [(m.group(1), int(m.group(2)), float(m.group(3)))
         for m in ROW_RE.finditer(HDR.read_text(encoding="utf-8", errors="replace"))]
if not TONES:
    sys.exit("no 44.1 kHz tone rows found in audio_app_meas_tones.h -- regenerate first")


def components(f_src: float, q31: bool):
    """Every spectral component the chain puts on the 16 kHz output for ONE source tone.

    Same propagation as M.evaluate(), with the frequency grid replaced by a single tone.
    """
    rear = M.rear_prototype()
    def coeff_of(s):
        c = np.asarray(s.coeff, dtype=np.float64)
        return M.q31_quantize(c)[0] if q31 else c
    branches = [(np.array([f_src]), np.ones(1), FS_IN)]
    for s in chain.stages:
        nxt = []
        for pos, mag, rate in branches:
            c = coeff_of(s)
            new_rate = rate / s.d
            nxt.append((M.fold(pos, new_rate), mag * M.mag_at(c, rate, pos), new_rate))
        branches = nxt
    out = []
    for pos, mag, rate in branches:
        landing = float(M.fold(pos, FS_OUT)[0])
        level = float(M.db(mag * M.mag_at(rear, float(M.L_REAR) * rate, pos))[0])
        out.append((landing, level))
    return out


# DC reference for the passband rows: the same through path at f -> 0.
def through_db(f_src: float, q31: bool) -> float:
    comps = components(f_src, q31)
    ref = components(1.0, q31)
    return max(l for _, l in comps) - max(l for _, l in ref)


print(f"chain: {name} + 1:1 {G.LP1_TAPS}t @{FS_MID/1000:g}k, rear M30 step "
      f"{FS_MID/FS_OUT:.6f}, protected 0..{FP:.0f} Hz")
print(f"gate worst point for reference: see asrc_lp1_chain_44k1_to_16k_gate.py\n")
print(f"{'row':13s} {'f_src @44.1k':>13s} {'lands':>9s} {'kind':22s} "
      f"{'float':>9s} {'Q31':>9s}   verdict")
for row, idx, f_src in TONES:
    vals = {}
    for q31 in (False, True):
        comps = components(f_src, q31)
        if f_src <= FP:
            vals[q31] = ("passband", f_src, through_db(f_src, q31))
        else:
            # strongest component that lands in the protected band
            inb = [(lv, ld) for ld, lv in comps if ld <= FP]
            lv, ld = max(inb) if inb else (-400.0, 0.0)
            vals[q31] = ("alias -> protected", ld, lv)
    kind, land, _ = vals[True]
    fl = vals[False][2]
    q3 = vals[True][2]
    if kind == "passband":
        ok = "PASS" if abs(q3) <= G.PB_LIMIT else "FAIL"
        note = f"(+/-{G.PB_LIMIT:.2f} dB)"
    else:
        ok = "PASS" if q3 <= G.ALIAS_PRODUCT else "FAIL"
        note = f"(<= {G.ALIAS_PRODUCT:.0f} dB)"
    print(f"{row:13s} {f_src:10.0f} Hz {land:7.0f} Hz {kind:22s} "
          f"{fl:+8.2f}  {q3:+8.2f}   {ok} {note}   *at 04 {idx:02X}")
