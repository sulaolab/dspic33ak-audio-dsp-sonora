#!/usr/bin/env python3
"""HOST gate for the 44.1 -> 16 kHz composed front end (/2 @44.1 then 1:1 @22.05).

Phase 1 proved a SINGLE 1:1 stage on hardware.  This pair is the first COMPOSITION: an
existing integer decimating stage followed by the new 1:1 band-limit, with the rear M30
resampler doing 22.05 -> 16 kHz at step 1.378125.

The question this answers is which first stage to put in front of the 1:1 set:

  reuse       the shipped /2 :22k05 set (107 taps, designed at 48 kHz for a 22.05 kHz
              output) run at 44.1 kHz input, where every edge scales by 44.1/48
  dedicated   a set designed FOR this chain: the 1:1 stage behind it removes
              9.5 .. 11.025 kHz, so the /2 stopband only has to start at
              22050 - 9500 = 12550 Hz instead of at 11025 Hz

Both are judged by the same uniform spec as every 1:1 set (owner, 2026-09-14):
protected 0 .. 0.8125*FN, passband +/-0.10 dB, alias <= -100 dB product /
-105 dB internal design target -- and at Q31, since that is what the firmware runs.
"""
import importlib.util, io, contextlib, sys, math
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

M = _load("unm", "tools/asrc/asrc_universal_nyquist_model.py")
dsg = M.dsg

FS_IN, FS_MID, FS_OUT = 44100.0, 22050.0, 16000.0
PROTECT, PB_LIMIT, ALIAS_PRODUCT, ALIAS_DESIGN = 0.8125, 0.10, -100.0, -105.0
FP = PROTECT * FS_OUT * 0.5          # 6500 Hz
FSTOP_1TO1 = FS_OUT - FP             # 9500 Hz -- the 1:1 stage's stopband edge
FSTOP_DEC = FS_MID - FSTOP_1TO1      # 12550 Hz -- all the /2 stage has to reach

def stage_at(coeff_fs, pb, sb, taps, run_fs, d):
    """A stage whose COEFFICIENTS were designed at coeff_fs but which RUNS at run_fs."""
    c = dsg.design(taps, coeff_fs, pb, sb, M.KAISER_BETA)
    return M.Stage("dec" if d > 1 else "lpf", run_fs, run_fs / d, taps,
                   pb * run_fs / coeff_fs, sb * run_fs / coeff_fs, c, d, 1, 1, 0.0)

# The 1:1 second stage is the shipped generated set: same design call the generator makes.
LP1_TAPS = 53
lp1 = stage_at(FS_MID, FP, FSTOP_1TO1, LP1_TAPS, FS_MID, 1)

CANDIDATES = {
    "reuse /2 :22k05 (107t @48k)":
        stage_at(48000.0, dsg.HALFRATE22K_PASSBAND_HZ, dsg.HALFRATE22K_STOPBAND_HZ,
                 107, FS_IN, 2),
}
# Dedicated: smallest odd tap count at 44.1 kHz that clears the design gate for the CHAIN.
n0 = M.tap_law(FS_IN, FSTOP_DEC - FP)
for n in range(n0 if n0 % 2 else n0 + 1, 202, 2):
    cand = stage_at(FS_IN, FP, FSTOP_DEC, n, FS_IN, 2)
    ch = M.Chain("t", "A1+lpf", [cand, lp1], FS_MID, FS_OUT)
    r = M.evaluate(ch, FS_IN, FS_OUT, FP, q31=True)
    if r.worst_alias_db <= ALIAS_DESIGN and r.passband_err_db <= PB_LIMIT:
        CANDIDATES[f"dedicated /2 @44.1k ({n}t, stop {FSTOP_DEC:.0f} Hz)"] = cand
        break

print(f"pair {FS_IN/1000:g}k -> {FS_OUT/1000:g}k   protected 0..{FP:.0f} Hz   "
      f"rear step {FS_MID/FS_OUT:.6f}   1:1 stage {LP1_TAPS}t @{FS_MID/1000:g}k")
print(f"tap law for a dedicated /2: N >= {n0} (transition {FSTOP_DEC-FP:.0f} Hz @44.1 kHz)\n")

for name, s0 in CANDIDATES.items():
    ch = M.Chain(name, "A1+lpf", [s0, lp1], FS_MID, FS_OUT)
    for q31 in (False, True):
        r = M.evaluate(ch, FS_IN, FS_OUT, FP, q31=q31)
        macs = ch.macs_per_sec
        verdict = ("PASS" if (r.worst_alias_db <= ALIAS_PRODUCT and r.passband_err_db <= PB_LIMIT)
                   else "FAIL")
        design = "PASS" if r.worst_alias_db <= ALIAS_DESIGN else "over design target"
        print(f"{name:44s} {'Q31 ' if q31 else 'float'}  "
              f"alias {r.worst_alias_db:8.2f} dB (src {r.worst_src_hz:7.0f} -> "
              f"{r.worst_land_hz:6.0f} Hz)  full-band {r.alias_full_band_db:7.2f}  "
              f"pb err {r.passband_err_db:5.3f} dB (at fp {r.passband_at_fp_db:+.3f}, "
              f"rear alone {r.rear_only_at_fp_db:+.3f})  {verdict}/{design}")
    # M.load_percent() still carries the 1.90 cyc/MAC calibration.  1.42 is what the
    # 2026-09-14 single-stage hardware measurement produced; BOTH are printed rather than
    # one being silently substituted -- 1.42 is a one-point calibration on a SINGLE-stage
    # chain, and this chain has two stages, which is where the composed 1.90 came from.
    print(f"{'':44s} cost   taps {ch.taps_list}  {macs/1e6:.3f} MMAC/s/ch  "
          f"{ch.cost_units():.1f} units  16 ch front end "
          f"{M.load_percent(macs, 16):.1f} % at 1.90 cyc/MAC / "
          f"{M.load_percent(macs, 16) * 1.42 / 1.90:.1f} % at 1.42  "
          f"(rear excluded; P1 front-end budget 40 %)\n")
