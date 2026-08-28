#!/usr/bin/env python3
"""Price a /4 (48 -> 12 kHz) anti-alias front end for the 11.025 kHz ASRC path.

Reproduces the trade table in
[internal] report_asrc_d2_alias_route_2026-07-28.md section 11.2: for each candidate
passband edge, the minimum stage-2 tap count that keeps the worst alias at the 11.025 kHz output
Nyquist (5512.5 Hz) at or below -100 dB, and what that costs in leg-A callback microseconds.

Cost model, calibrated against hardware (report section 10.7): a stage's cost in "units" is
taps * (stage output rate / 48 kHz), and the measured /3 front end is 48.0 units = 51.5 us of cbA.

Cascade metric: the interleaved polyphase resampler prototype is L-times oversampled, so it MUST
be evaluated at L * fs_intermediate. Evaluating it at fs_intermediate is the bug that made
check_11025_cascade() pass unconditionally until 2026-07-29 (report section 11.1). The control
that validates the corrected axis is printed by this script: with the anti-alias stage omitted the
worst alias comes out at 0.00 dB, matching the -0.01 dB 6 kHz fold measured on board 057 before
the D2 fix.

Only NumPy is required. Run from the repository root.
"""
import contextlib, importlib.util, io, sys
import numpy as np

spec = importlib.util.spec_from_file_location("chk", "tools/asrc/asrc_headroom_filter_check.py")
chk = importlib.util.module_from_spec(spec); sys.modules["chk"] = chk
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(chk)
dspec = importlib.util.spec_from_file_location("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")
dsg = importlib.util.module_from_spec(dspec); sys.modules["dsg"] = dsg
with contextlib.redirect_stdout(io.StringIO()):
    dspec.loader.exec_module(dsg)

L = chk.L
C = chk.CANDIDATES[1]
NYQ = 5512.5
# The real gate limit is -100 dB, but searching for the FIRST tap count that clears it lands on
# knife-edge ripple alignments: at 4200 Hz, 127 taps scrapes -101.5 dB while 131 taps -- 1.1 us
# more -- reaches -114.3 dB. Search to -105 dB so the table recommends points that are actually
# inside the limit rather than points that happen to touch it.
LIMIT = -105.0
GATE_LIMIT = -100.0
US_PER_UNIT = 51.5 / 48.0        # measured: /3 = 48.0 cost units = 51.5 us of cbA

rows = chk.phase_rows(C)
PROTO = np.zeros(C.taps * L)
for p in range(L):
    for t in range(C.taps):
        PROTO[t * L - p + (L - 1)] = rows[p, t]
PROTO /= PROTO.sum()

FREQS = np.linspace(0.0, 24000.0, 240001)   # 0.1 Hz steps


def fold(f, rate):
    r = f % rate
    return np.minimum(r, rate - r)


def cascade(stages, inter_rate):
    """stages = [(coeff, its own input rate)]. Returns (worst alias dB, worst without last stage)."""
    f12 = fold(FREQS, inter_rate)
    mask = f12 >= NYQ
    h = [chk._mag_at(c, r, fold(FREQS, r)) for c, r in stages]
    hp = chk._mag_at(PROTO, L * inter_rate, f12)
    full = h[0] * hp
    for x in h[1:]:
        full = full * x
    partial = h[0] * hp                      # stage1 + resampler only (no anti-alias stage)
    d = lambda v: float(20.0 * np.log10(np.maximum(v[mask], 1e-15)).max())
    return d(full), d(partial)


def pass_edge(stages, inter_rate, passband):
    f = np.linspace(0.0, passband, 2000)
    mag = np.ones(f.size)
    for c, r in stages:
        mag = mag * chk._mag_at(c, r, f)
    mag = mag * chk._mag_at(PROTO, L * inter_rate, f)
    return float((20.0 * np.log10(np.maximum(mag, 1e-15))).min())


print("=== control: shipping /3 (69@48k -> 16k, then 75@16k decimate-by-1) ===")
s3a = dsg.design(69, 48000.0, 5500.0, 10500.0)
s3b = dsg.design(75, 16000.0, 4000.0, NYQ)
w3, p3 = cascade([(s3a, 48000.0), (s3b, 16000.0)], 16000.0)
u3 = 69 * (16000.0 / 48000.0) + 75 * (16000.0 / 48000.0)
print(f"  units {u3:5.1f}  ({u3*US_PER_UNIT:5.1f} us)  worst alias {w3:8.2f} dB   "
      f"without D2 stage {p3:6.2f} dB   pass-edge@4000 {pass_edge([(s3a,48000.0),(s3b,16000.0)],16000.0,4000.0):6.2f} dB")

print()
print("=== /4 candidates: stage1 48->24 (/2), stage2 24->12 (/2), 12 kHz intermediate ===")
print("    (resampler fc = 0.465*12000 = 5580 Hz, only 67 Hz above the 5512.5 Hz output Nyquist)")
print(f"{'P (Hz)':>7} {'N1':>4} {'N2':>4} {'units':>6} {'us':>6} {'d-us':>6} {'worst':>8} {'no-S2':>8} {'edge':>8}")
for P in (4000.0, 4200.0, 4400.0, 4600.0, 4800.0, 5000.0):
    N1 = 27
    a = dsg.design(N1, 48000.0, P, 18487.5)
    hit = None
    for N2 in range(31, 261, 2):
        b = dsg.design(N2, 24000.0, P, NYQ)
        w, p = cascade([(a, 48000.0), (b, 24000.0)], 12000.0)
        if w <= LIMIT:
            hit = (N2, w, p, pass_edge([(a, 48000.0), (b, 24000.0)], 12000.0, P))
            break
    if hit is None:
        print(f"{P:7.0f} {N1:4d}    -      -      -      -   no solution <= 260 taps")
        continue
    N2, w, p, edge = hit
    units = N1 * 0.5 + N2 * 0.25
    us = units * US_PER_UNIT
    print(f"{P:7.0f} {N1:4d} {N2:4d} {units:6.1f} {us:6.1f} {us-u3*US_PER_UNIT:+6.1f} "
          f"{w:8.2f} {p:8.2f} {edge:8.2f}")
