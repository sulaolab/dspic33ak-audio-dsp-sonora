#!/usr/bin/env python3
"""Which ASRC output rates need an anti-alias low-pass, what structure, and what it costs.

Answers the question the 12 kHz listening test raised: the D2/D4 work fixed 8 kHz and
11.025 kHz, but the routing gate in src/app/apps/asrc/asrc_audio_path.c has rows for those two
rates ONLY, so every other rate runs `fe=direct` -- the resampler alone, whose cutoff is a
fixed ASRC_POLY_FC = 0.465 of its INPUT rate (22.32 kHz for a 48 kHz input) regardless of how
low the output Nyquist is.  This script prices the whole set on one axis.

Three questions per rate:

  1. How bad is it now?  Worst alias with no front end, plus WHERE the folded energy lands
     (that is what decides audibility, not the dB figure alone).
  2. What structure can fix it?  A front end must decimate 48 kHz by an INTEGER factor to a
     rate at or above the target; the resampler then pulls the small remainder.  Only
     den in {2,3,4,6} are reachable with the 2:1 / 3:1 stages the decimator implements, so
     32 kHz and 44.1 kHz (48/32 = 1.5, 48/44.1 = 1.088) have no integer front end at all and
     need a different lever -- scaling ASRC_POLY_FC, which this script also measures.
  3. What does it cost?  Minimum stage-2 tap count that puts the worst alias at or below
     -105 dB, converted to the `taps * (stage output rate / 48 kHz)` cost model.

     CAUTION on that model: report section 11.7 measured it to be structurally wrong -- the
     /4 chain (45.75 units) costs LESS on hardware than /6 (38.8 units): 155.6 us vs 157.6 us
     of cbA.  Treat the unit column as an ORDERING within one structure, and as pessimistic
     across structures.  The three measured calibration points are printed at the end.

Only NumPy is required.  Run from the repository root.
"""
import contextlib
import dataclasses
import importlib.util
import io
import sys

import numpy as np


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


chk = _load("chk", "tools/asrc/asrc_headroom_filter_check.py")
dsg = _load("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")

L = chk.L
SHIPPED = chk.CANDIDATES[1]          # headroom-m30-kaiser11, fc = 0.465, the shipping resampler
IN_HZ = 48000.0
LIMIT = -105.0                       # scan target; the release gate itself is -100 dB
US_PER_UNIT = 51.5 / 48.0            # measured: /3 = 48.0 cost units = 51.5 us of cbA
FREQS = np.linspace(0.0, 24000.0, 240001)   # 0.1 Hz steps over the 48 kHz input band


def prototype(cutoff):
    """Interleaved polyphase prototype of the resampler at an arbitrary normalized cutoff.

    cutoff is in the same units as Candidate.cutoff (fraction of the INPUT sample rate).
    The firmware builds these rows at init from ASRC_POLY_FC, so changing the cutoff costs
    no taps, no RAM and no cycles -- it is a different number in the same loop.
    """
    cand = dataclasses.replace(SHIPPED, cutoff=cutoff)
    rows = chk.phase_rows(cand)
    proto = np.zeros(cand.taps * L)
    for p in range(L):
        for t in range(cand.taps):
            proto[t * L - p + (L - 1)] = rows[p, t]
    return proto / proto.sum()


PROTO_SHIPPED = prototype(SHIPPED.cutoff)


def fold(f, rate):
    r = f % rate
    return np.minimum(r, rate - r)


def worst_alias(stages, inter_hz, out_nyq_hz, proto):
    """Worst alias in the output band, referenced to the 48 kHz input band.

    stages = [(coeff, that stage's own input rate)].  Each stage is evaluated at f folded into
    ITS OWN rate; the resampler prototype at f folded into the intermediate rate, on the
    L * inter_hz axis (it is an L-times-oversampled filter).

    THE MASK.  An input at f leaves the chain at fold(fold(f, inter), out_rate).  That differs
    from f -- i.e. f is an alias source -- exactly when **f > out_nyq**, because anything at or
    below the output Nyquist passes through both folds unchanged.  So the mask is simply
    `FREQS > out_nyq_hz`.

    The earlier mask, `fold(f, inter) >= out_nyq`, was wrong in BOTH directions and was fixed
    on 2026-07-29 (second axis bug in this gate; the first was the L*fs prototype axis, report
    section 11.1):

      - It COLLAPSED to a measure-zero set whenever out_nyq == inter/2, which is every
        exact-integer rate.  fold(.., inter) is bounded by inter/2, so `>= inter/2` selected
        literally 1-3 grid points instead of a band: 3 points for the /6 8 kHz chain, 2 for
        the /4 12 kHz chain, 1 for a /2 24 kHz candidate.  Any "measurement" on those rows was
        one frequency wearing a band's clothing, and it made a nonsense repair stage look like
        it worked (relaxing a den=3 stage 1 and "fixing" it downstream read -19.6 -> -128.7 dB,
        which is impossible -- the alias is created by the decimation and nothing after it can
        undo it).
      - It also MISSED real alias sources when out_nyq < inter/2: at the 11.025 kHz rate an
        input at 6500 Hz folds to 5500 Hz in the 12 kHz-rate signal, which is below 5512.5, so
        the old mask excluded it -- yet 6500 -> 5500 is precisely an alias.

    Recomputing every chain with the correct mask left all of them inside the -100 dB gate
    (/6 -108.38, /3 -106.20, /4 11.025k -108.28, /4 12k -108.36), so no shipped filter was
    deficient -- the reported figures had simply been 4-16 dB optimistic.  They now cluster
    where theory says they should: Kaiser beta=11 buys about -108 dB and the window sets the
    depth, so a correct metric SHOULD read roughly the same for all four.
    """
    mask = FREQS > out_nyq_hz
    mag = chk._mag_at(proto, float(L) * inter_hz, fold(FREQS, inter_hz))
    for coeff, fs in stages:
        mag = mag * chk._mag_at(coeff, fs, fold(FREQS, fs))
    return float(20.0 * np.log10(np.maximum(mag[mask], 1e-15)).max())


def pass_edge(stages, inter_hz, passband_hz, proto):
    f = np.linspace(0.0, passband_hz, 4000)
    mag = chk._mag_at(proto, float(L) * inter_hz, f)
    for coeff, fs in stages:
        mag = mag * chk._mag_at(coeff, fs, f)
    return float((20.0 * np.log10(np.maximum(mag, 1e-15))).min())


# ---------------------------------------------------------------------------------------------
# Per-rate structure. `den` is the integer decimation of the 48 kHz input; the intermediate rate
# 48000/den must be >= the target, and the resampler pulls the rest (step = intermediate/target).
#
#   den 2 -> one 2:1 stage  (48 -> 24)                cost = taps * 0.50
#   den 3 -> one 3:1 stage  (48 -> 16)                cost = taps * 0.3333
#   den 4 -> two 2:1 stages (48 -> 24 -> 12)          cost = N1 * 0.50 + N2 * 0.25
#   den 6 -> 3:1 then 2:1   (48 -> 16 -> 8)           cost = N1 * 0.3333 + N2 * 0.1667
#
# For den 4 and 6 the FIRST stage gets a large transition band for free: it only has to suppress
# what would fold into the FINAL band after the remaining halving(s), so its stopband edge sits at
# (its output rate) - (output Nyquist), far above the final passband.  For den 2 and 3 there is no
# remaining rate change, so the single stage's stopband must sit ON the output Nyquist and it pays
# the full transition -- which is why the exact-integer rates 16 k and 24 k are EXPENSIVE, not
# cheap, despite their step = 1.0000.
# ---------------------------------------------------------------------------------------------
RATES = (
    ("8000",   8000.0,  6),
    ("11025", 11025.0,  4),
    ("12000", 12000.0,  4),
    ("16000", 16000.0,  3),
    ("22050", 22050.0,  2),
    ("24000", 24000.0,  2),
    ("32000", 32000.0,  0),   # 48/32 = 1.5 -- no integer front end exists
    ("44100", 44100.0,  0),   # 48/44.1 = 1.088 -- likewise
)


def build(den, passband, out_nyq, n1, n2):
    """Return (stages, inter_hz, units) for one candidate front end."""
    if den == 2:
        s = dsg.design(n2, 48000.0, passband, out_nyq)
        return [(s, 48000.0)], 24000.0, n2 * 0.50
    if den == 3:
        s = dsg.design(n2, 48000.0, passband, out_nyq)
        return [(s, 48000.0)], 16000.0, n2 / 3.0
    if den == 4:
        a = dsg.design(n1, 48000.0, passband, 24000.0 - out_nyq)
        b = dsg.design(n2, 24000.0, passband, out_nyq)
        return [(a, 48000.0), (b, 24000.0)], 12000.0, n1 * 0.50 + n2 * 0.25
    if den == 6:
        a = dsg.design(n1, 48000.0, passband, 16000.0 - out_nyq)
        b = dsg.design(n2, 16000.0, passband, out_nyq)
        return [(a, 48000.0), (b, 16000.0)], 8000.0, n1 / 3.0 + n2 / 6.0
    raise ValueError(den)


STAGE1 = {2: 0, 3: 0, 4: 27, 6: 43}      # stage-1 taps held at the shipped/short values


def scan(den, passband, out_nyq):
    """Smallest odd stage-2 tap count reaching LIMIT, or None."""
    n1 = STAGE1[den]
    lo, hi = (31, 401) if den in (2, 3) else (31, 301)
    for n2 in range(lo, hi, 2):
        stages, inter, units = build(den, passband, out_nyq, n1, n2)
        w = worst_alias(stages, inter, out_nyq, PROTO_SHIPPED)
        if w <= LIMIT:
            return n2, units, w, pass_edge(stages, inter, passband, PROTO_SHIPPED)
    return None


print("=" * 108)
print("1. WHAT HAPPENS NOW  (fe=direct: resampler only, its cutoff fixed at 0.465*48000 = 22320 Hz)")
print("=" * 108)
print(f"{'rate':>7} {'Nyquist':>9} {'fold source band':>19} {'lands at':>14} {'worst alias':>12}  audibility")
for name, fs, _den in RATES:
    nyq = fs * 0.5
    w = worst_alias([], 48000.0, nyq, PROTO_SHIPPED)
    top = 0.465 * 48000.0
    lands_lo = max(0.0, 2.0 * nyq - top) if top < 2.0 * nyq else 0.0
    band = f"{nyq:.0f}-{min(top, 24000.0):.0f} Hz"
    lands = f"{lands_lo:.0f}-{nyq:.0f} Hz"
    frac = (min(top, 24000.0) - nyq) / nyq
    verdict = ("negligible" if frac < 0.05 else
               "narrow, HF only" if frac < 0.5 else
               "severe (folds across the whole band)")
    print(f"{name:>7} {nyq:9.1f} {band:>19} {lands:>14} {w:11.2f} dB  {verdict}")

print()
print("=" * 108)
print("2. INTEGER FRONT END: minimum stage-2 taps for <= -105 dB, at three passband targets")
print("=" * 108)
print(f"{'rate':>7} {'den':>4} {'step':>8} {'inter':>7} "
      f"{'0.70*Nyq':>22} {'0.76*Nyq':>22} {'0.85*Nyq':>22}")
print(f"{'':>7} {'':>4} {'':>8} {'':>7} "
      + "".join(f"{'Hz  N2  units    us':>22}" for _ in range(3)))
for name, fs, den in RATES:
    if den == 0:
        print(f"{name:>7} {'--':>4} {'--':>8} {'--':>7}   no integer decimation of 48 kHz reaches "
              f"this rate -- see section 3")
        continue
    nyq = fs * 0.5
    inter = 48000.0 / den
    cells = ""
    for frac in (0.70, 0.76, 0.85):
        P = round(nyq * frac / 50.0) * 50.0
        hit = scan(den, P, nyq)
        if hit is None:
            cells += f"{P:7.0f}{'   -':>5}{'      -':>7}{'      -':>7} "
        else:
            n2, units, _w, _e = hit
            cells += f"{P:7.0f}{n2:5d}{units:7.1f}{units * US_PER_UNIT:7.1f} "
    print(f"{name:>7} {den:4d} {inter / fs:8.5f} {inter / 1000:6.0f}k {cells}")

print()
print("=" * 108)
print("3. 32 kHz / 44.1 kHz: no integer front end.  Two other levers, priced.")
print("=" * 108)
print("(a) Scale ASRC_POLY_FC with the ratio -- fc = 0.465 * (target / 48000).  Costs NOTHING:")
print("    the phase rows are generated at init, so this is a different constant in the same")
print("    30-tap loop.  It was rejected for 8 kHz because a 30-tap prototype's transition width")
print("    is fixed in NORMALIZED terms, so at a deep ratio it lands far inside the band.  At a")
print("    shallow ratio it may be enough -- measured here rather than assumed.")
print(f"{'rate':>7} {'scaled fc':>11} {'worst alias':>12} {'pass edge':>11}  (edge measured at 0.85*Nyquist)")
for name, fs, den in RATES:
    if den != 0:
        continue
    nyq = fs * 0.5
    cutoff = SHIPPED.cutoff * fs / 48000.0
    proto = prototype(cutoff)
    w = worst_alias([], 48000.0, nyq, proto)
    e = pass_edge([], 48000.0, nyq * 0.85, proto)
    print(f"{name:>7} {cutoff * 48000.0:10.0f}Hz {w:11.2f} dB {e:10.2f} dB")
print()
print("    Trade curve -- the scaling above is the endpoint, not the only choice.  Less scaling")
print("    keeps the passband flatter and rejects less; the 30-tap prototype's transition width")
print("    is what is being spent.  'edge' is the worst passband loss up to 0.85*Nyquist.")
print(f"{'rate':>7} {'fc (norm)':>10} {'fc (Hz)':>9} {'worst alias':>12} {'edge':>9}")
for name, fs, den in RATES:
    if den != 0:
        continue
    nyq = fs * 0.5
    for cutoff in (0.465, 0.44, 0.42, 0.40, 0.38, SHIPPED.cutoff * fs / 48000.0):
        proto = prototype(cutoff)
        w = worst_alias([], 48000.0, nyq, proto)
        e = pass_edge([], 48000.0, nyq * 0.85, proto)
        tag = "  <- ratio-scaled" if abs(cutoff - SHIPPED.cutoff * fs / 48000.0) < 1e-9 else ""
        print(f"{name:>7} {cutoff:10.4f} {cutoff * 48000.0:8.0f}Hz {w:11.2f} dB {e:8.2f} dB{tag}")
print()
print("(b) A decimate-by-1 FIR at 48 kHz ahead of the resampler (stopband on the output")
print("    Nyquist).  Every tap runs at the full 48 kHz input rate, so cost = taps * 1.00 --")
print("    the most expensive unit rate in the whole table.")
print(f"{'rate':>7} {'passband':>9} {'taps':>6} {'units':>7} {'us':>7} {'worst':>9}")
for name, fs, den in RATES:
    if den != 0:
        continue
    nyq = fs * 0.5
    P = round(nyq * 0.85 / 50.0) * 50.0
    for n in range(31, 501, 2):
        s = dsg.design(n, 48000.0, P, nyq)
        w = worst_alias([(s, 48000.0)], 48000.0, nyq, PROTO_SHIPPED)
        if w <= LIMIT:
            print(f"{name:>7} {P:9.0f} {n:6d} {float(n):7.1f} {n * US_PER_UNIT:7.1f} {w:8.2f} dB")
            break
    else:
        print(f"{name:>7} {P:9.0f}      -       -       -   no solution <= 500 taps")

print()
print("=" * 108)
print("4. CALIBRATION -- the three front ends measured on board <PKOB4_SERIAL>")
print("=" * 108)
print("    structure   model units   measured cbA   note")
print("    /6 (8 k)          38.8      157.6 us     43@48k + 147@16k")
print("    /4 (11.025 k)     45.75     155.6 us     27@48k + 129@24k  <- MORE units, LESS time")
print("    /3 (11.025 k)     48.0      169.1 us     69@48k + 75@16k   (superseded)")
print("    direct (12 k)      0.0      117.2 us     no front end")
print("  The unit model orders candidates correctly WITHIN a structure but over-charges the")
print("  deeper cascades; see report section 11.7.  Read the us columns above as upper bounds.")
