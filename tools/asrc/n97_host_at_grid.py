#!/usr/bin/env python3
"""Host prediction at the HARDWARE sweep grid, for the 48 -> 32 kHz N=97 audio-mode study.

Not a new model: this imports `asrc_48_to_32_audio_gate` and evaluates *that* model
(front end |H| at the image frequency  x  rear resampler |H| at the landing frequency)
at exactly the tone rows the board can generate, so the report's host column and
hardware column compare the same input frequencies.  The gate script itself only
prints per-band worsts, which cannot be lined up against a measured sweep.

The "front end only" column is what N=97 contributes on top of the shipping rear
resampler -- the honest host answer to "how many dB better than N=97 OFF", since the
OFF case cannot be measured on hardware without changing the rate planner (forbidden).
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import numpy as np
import asrc_48_to_32_audio_gate as g

IN_HZ, OUT_HZ = 48000.0, 32000.0

p0, p1, crc = g.read_inc_rows(g.INC)
proto = g.prototype_from_rows(p0, p1)
rear = g.rear_prototype()
print("host model: %s CRC32 0x%08X  (front end L=2/M=3 N=97, rear fc=0.465x32k)"
      % (g.INC.name, crc))

PASSBAND = [1000, 5000, 10000, 12000, 13000, 13500, 14000, 15000, 15500, 16000]
SWEEP = [16250, 16500, 16750, 17000, 17250, 17500, 17750, 18000, 18250, 18500,
         18750, 19000, 20000, 21000, 22000, 23000, 23900]


def mag(coeff, fs, f):
    return float(g.mag_at(coeff, fs, np.asarray([float(f)]))[0])


print()
print("PASSBAND (host)")
print("%9s %11s %11s %11s" % ("f Hz", "front end", "rear", "total"))
for f in PASSBAND:
    fe, re_ = mag(proto, g.PROTO_HZ, f), mag(rear, g.L_REAR * OUT_HZ, f)
    print("%9d %8.2f dB %8.2f dB %8.2f dB" % (f, g.db(fe), g.db(re_), g.db(fe * re_)))

print()
print("ALIAS (host) -- image at f is shaped by the front end, then folded to `land`")
print("%9s %9s %11s %11s %11s %11s  %s"
      % ("in Hz", "land Hz", "front end", "rear", "total", "N97 OFF", "2nd image (48k-f) land/total"))
for f in SWEEP:
    land = float(np.minimum(f % OUT_HZ, OUT_HZ - f % OUT_HZ))
    fe, re_ = mag(proto, g.PROTO_HZ, f), mag(rear, g.L_REAR * OUT_HZ, land)
    f2 = IN_HZ - f
    land2 = float(np.minimum(f2 % OUT_HZ, OUT_HZ - f2 % OUT_HZ))
    fe2, re2 = mag(proto, g.PROTO_HZ, f2), mag(rear, g.L_REAR * OUT_HZ, land2)
    # N=97 OFF: no front end, and the rear kernel's cutoff is ASRC_POLY_FC of its INPUT rate,
    # which in direct mode is 48 kHz (22320 Hz) rather than 32 kHz -- see asrc_audio_path.c:721
    # ("worst alias 0.00 dB, folding 16-22.3 kHz down into 9.7-16 kHz").  So the OFF column is
    # the rear kernel alone, referred to 48 kHz, at the INPUT frequency.
    off = mag(rear, g.L_REAR * IN_HZ, f)
    print("%9d %9.0f %8.2f dB %8.2f dB %8.2f dB %8.2f dB  %7.0f Hz %8.2f dB"
          % (f, land, g.db(fe), g.db(re_), g.db(fe * re_), g.db(off), land2, g.db(fe2 * re2)))
