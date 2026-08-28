#!/usr/bin/env python3
"""Polyphase PHASE-WORST host oracle candidate for the 48 -> 32 kHz N=97 study.

Why: the shipping gate (asrc_48_to_32_audio_gate.py) evaluates the rear resampler as its
NOMINAL interpolating prototype -- one static |H| sampled at L*fs_out.  Hardware at a frozen
ratio near 1.0 does something different: it latches ONE polyphase phase for the whole capture
and convolves with that subfilter alone.  Near the kernel's cutoff the subfilters differ by
several dB, which is what the measured 9.11 dB (pre-fix) / 3.17 dB (post-fix) repeat scatter
and the +3.46 dB host-vs-HW gap at 16.25 kHz look like.

This does NOT replace the gate.  It asks one question: does the measured worst fall inside the
per-phase envelope?  If yes, the gap is the old oracle averaging over a dimension the hardware
does not average over -- not a filter-design error.

Each subfilter is proto[p::L] scaled by L so its DC gain is 1 (the prototype is normalised to
sum 1), evaluated at the OUTPUT rate, because that is the rate a single latched phase runs at.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import numpy as np
import asrc_48_to_32_audio_gate as g

IN_HZ, OUT_HZ = 48000.0, 32000.0
POINTS = [16250, 16500, 16750, 17000, 17500, 18000, 18500, 19000]

p0, p1, crc = g.read_inc_rows(g.INC)
proto = g.prototype_from_rows(p0, p1)
rear = g.rear_prototype()
L = int(g.L_REAR)


def mag(coeff, fs, f):
    return float(g.mag_at(coeff, fs, np.asarray([float(f)]))[0])


print("host phase-worst oracle candidate")
print("front end: %s CRC32 0x%08X (N=97 L=2/M=3)   rear: L=%d phases, fc=0.465" %
      (g.INC.name, crc, L))
print()
print("%7s %8s %10s %10s %10s %10s %8s  %s"
      % ("in Hz", "land Hz", "fe dB", "nominal", "best", "worst", "spread", "worst phase"))

rows = []
for f in POINTS:
    land = float(np.minimum(f % OUT_HZ, OUT_HZ - f % OUT_HZ))
    fe = mag(proto, g.PROTO_HZ, f)
    nominal = mag(rear, L * OUT_HZ, land)

    per_phase = []
    for p in range(L):
        sub = rear[p::L] * float(L)          # DC gain 1
        per_phase.append(mag(sub, OUT_HZ, land))
    per_phase = np.asarray(per_phase)

    tot_nom = g.db(fe * nominal)
    tot_best = g.db(fe * per_phase.min())    # best rejection = smallest rear gain
    tot_worst = g.db(fe * per_phase.max())
    wp = int(np.argmax(per_phase))
    print("%7d %8.0f %9.2f %9.2f %9.2f %9.2f %7.2f  p=%d"
          % (f, land, g.db(fe), tot_nom, tot_best, tot_worst, tot_worst - tot_best, wp))
    rows.append((f, tot_nom, tot_best, tot_worst))

print()
print("HW worst measured, against the envelope above:")
HW = {16250: -22.17, 16500: -24.48, 17000: -28.85, 18000: -47.14, 18500: -64.85, 19000: -107.03}
for f, nom, best, worst in rows:
    if f not in HW:
        continue
    hw = HW[f]
    inside = (hw <= worst + 0.01) and (hw >= best - 0.01)
    print("  %5d Hz  HW %8.2f  envelope [%8.2f .. %8.2f]  nominal %8.2f  -> %s"
          % (f, hw, best, worst, nom, "INSIDE" if inside else "OUTSIDE"))
