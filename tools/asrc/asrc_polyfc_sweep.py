#!/usr/bin/env python3
"""How much room is there above ASRC_POLY_FC = 0.465, and what does it buy at a 32 kHz output?

Follow-up to tools/asrc/asrc_48_to_32_feasibility.py, which found that a 48 -> 32 kHz
L=2/M=3 front end drops the resampler's input rate to 32 kHz, so the resampler's own cutoff
(a fixed fraction of ITS INPUT rate) lands at 0.465 * 32000 = 14880 Hz and costs -6.78 dB at
15 kHz.  That ceiling belongs to the resampler, not to any front end.

Nothing here changes firmware.  Every number is host-side, computed from the same phase-row
construction the firmware runs at init (chk.phase_rows), so a value adopted from this sweep is
adopted by editing one constant.

Sections:
  1. The two gates the codebase actually pins (edge at 20 kHz >= -1.1 dB, worst prototype image
     <= -105 dB), swept over fc.  This is what decides whether 0.465 is near a limit.
  2. The 32 kHz-input passband per fc, plus the -0.1 / -0.5 / -1 / -3 / -6 dB points.
  3. Per-rate degradation: every supported pair, both legs, referenced to fc = 0.465.
  4. The five front-end cascades chk gates, recomputed per fc (the resampler is a factor in them).
  5. Candidate re-evaluation for 48 -> 32 with the improved resampler.

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
SHIPPED = chk.CANDIDATES[1]           # headroom-m30-kaiser11, fc 0.465, the shipping resampler
NFFT = 1 << 16
FREQS = np.linspace(0.0, 24000.0, 24001)
GATE = -100.0
EDGE_GATE = -1.1                      # Candidate.max_edge_loss_db
IMAGE_GATE = -105.0                   # Candidate.max_image_db
CASCADE_GATE = chk.MAX_CASCADE_IMAGE_DB
BLOCK_US = 1000000.0 * 16.0 / 48000.0
CH = 16
OUT_HZ = 32000.0
OUT_NYQ = 16000.0

FCS = (0.4600, 0.4650, 0.4660, 0.4665, 0.4670, 0.4680, 0.4700, 0.4750)
FC_REF = 0.4650
FCS_STRICT = (0.4650, 0.4660, 0.4670)


def cost_us(macs, calls):
    return 0.00719 * macs + 0.343 * calls


def mag_at(coeff, fs, freqs_hz):
    spectrum = np.abs(np.fft.rfft(coeff, NFFT))
    axis = np.linspace(0.0, fs * 0.5, spectrum.size)
    return np.interp(freqs_hz, axis, spectrum)


def fold(f, rate):
    r = f % rate
    return np.minimum(r, rate - r)


def prototype(cutoff):
    cand = dataclasses.replace(SHIPPED, cutoff=cutoff)
    rows = chk.phase_rows(cand)
    proto = np.zeros(SHIPPED.taps * L)
    for p in range(L):
        for t in range(SHIPPED.taps):
            proto[t * L - p + (L - 1)] = rows[p, t]
    return proto / proto.sum()


PROTO = {fc: prototype(fc) for fc in FCS}


def coarse_fine(lo, hi, coarse, step, ok):
    hit = None
    n = lo
    while n < hi:
        if ok(n):
            hit = n
            break
        n += coarse
    if hit is None:
        return None
    n = hit
    while n - step >= lo and ok(n - step):
        n -= step
    return n


def say(*a):
    print(*a)
    sys.stdout.flush()


def rule(title):
    say("=" * 104)
    say(title)
    say("=" * 104)


# =============================================================================================
rule("1. THE PINNED GATES, SWEPT OVER fc   (chk.analyze: ripple / edge at 20 kHz / worst image)")
say("   These are the two limits the codebase itself enforces on the prototype, and they are")
say("   RATE-INDEPENDENT (normalised to the resampler's own input rate).  Gates: edge >= -1.1 dB,")
say("   worst image <= -105.0 dB.  M = 30, L = 128, Kaiser beta = 11 throughout.")
say(f"{'fc':>8} {'ripple':>9} {'edge@20k':>10} {'edge slack':>11} "
    f"{'worst image':>12} {'image slack':>12}  gate")
gate1 = {}
for fc in FCS:
    ripple, edge, image = chk.analyze(dataclasses.replace(SHIPPED, cutoff=fc))
    ok = (edge >= EDGE_GATE) and (image <= IMAGE_GATE)
    gate1[fc] = (ripple, edge, image, ok)
    tag = "  <- shipping" if fc == FC_REF else ""
    say(f"{fc:8.4f} {ripple:9.3f} {edge:10.3f} {edge - EDGE_GATE:+11.3f} "
        f"{image:12.2f} {IMAGE_GATE - image:+12.2f}  {'PASS' if ok else 'FAIL'}{tag}")
say("   edge slack = margin still held against the -1.1 dB gate; image slack = margin against -105.")
say()
say("   Fine crossing search -- the largest fc that still clears each gate (step 0.0002):")
for limit, label in ((IMAGE_GATE, "image <= -105.0 (Candidate gate)"),
                     (CASCADE_GATE, "image <= -100.0 (cascade gate)")):
    best = None
    fc = 0.4600
    while fc <= 0.4760001:
        _r, _e, img = chk.analyze(dataclasses.replace(SHIPPED, cutoff=round(fc, 4)))
        if img <= limit:
            best = (round(fc, 4), img)
        else:
            break
        fc += 0.0002
    if best is None:
        say(f"      {label:38} : nothing in 0.4600-0.4760 clears it")
    else:
        say(f"      {label:38} : fc <= {best[0]:.4f}  (image {best[1]:.2f} dB)")
say("   The prototype has M = 30 taps and a FIXED Kaiser beta = 11, so raising fc narrows the")
say("   transition band without buying back any stopband: the floor falls roughly 3 dB per")
say("   0.001 of fc.  That is why this is a cliff and not a gradient.")

# =============================================================================================
say()
rule("2. PASSBAND AT A 32 kHz RESAMPLER INPUT   (what a 48 -> 32 front end leaves the resampler)")
PB_FREQS = (10000.0, 12000.0, 12800.0, 13000.0, 13600.0, 14000.0, 14500.0, 15000.0, 15500.0)
head = f"{'fc':>8} " + "".join(f"{int(f/100)/10.0:>8.1f}k" for f in PB_FREQS)
say(head)
fine = np.linspace(0.0, 16000.0, 16001)
points = {}
for fc in FCS:
    mag = 20.0 * np.log10(np.maximum(mag_at(PROTO[fc], float(L) * OUT_HZ,
                                            np.array(PB_FREQS)), 1e-15))
    tag = "  <- shipping" if fc == FC_REF else ""
    say(f"{fc:8.4f} " + "".join(f"{v:9.2f}" for v in mag) + tag)
say()
say("   The frequency at which the chain reaches each loss level, 32 kHz input:")
say(f"{'fc':>8} {'-0.1 dB':>9} {'-0.5 dB':>9} {'-1 dB':>9} {'-3 dB':>9} {'-6 dB':>9}"
    f"   {'gain @15k':>10}")
for fc in FCS:
    curve = 20.0 * np.log10(np.maximum(mag_at(PROTO[fc], float(L) * OUT_HZ, fine), 1e-15))
    row = []
    for lvl in (-0.1, -0.5, -1.0, -3.0, -6.0):
        idx = np.nonzero(curve <= lvl)[0]
        row.append(fine[idx[0]] if idx.size else float("nan"))
    points[fc] = row
    g15 = float(np.interp(15000.0, fine, curve))
    tag = "  <- shipping" if fc == FC_REF else ""
    say(f"{fc:8.4f} " + "".join(f"{v:9.0f}" for v in row) + f"   {g15:10.2f}" + tag)

# =============================================================================================
say()
rule("3. PER-RATE DEGRADATION, BOTH LEGS, REFERENCED TO fc = 0.4650")
say("   Source content is assumed band-limited to 20/48 of the leg's INPUT rate -- exactly the")
say("   assumption the -105 dB Candidate gate makes.  Under that convention a contribution")
say("   counts only if it LANDS inside the protected band, which is why several legs report")
say("   none at all.  The metric depends on the direction, because only one can happen:")
say("     out < in  -> ALIAS: worst 20log10 H over the source frequencies that fold back into")
say("                  the protected band")
say("     out > in  -> IMAGE: worst 20log10 H from (1-20/48)*in_hz up, which is where the")
say("                  lowest replica of protected content lands")
say("     out = in  -> neither is in band; only the passband edge moves")
say("   'resampler only' rows carry no front-end stage, either because none exists for that")
say("   rate or because the leg is an upsample.  Where a shipped front end exists its stages")
say("   are included -- see section 4, which is the gated form of those rows.")
say()

# (name, B rate, integer den of the AB front end or 1 for direct)
PAIRS = (
    ("8000", 8000.0, 6),
    ("11025", 11025.0, 4),
    ("12000", 12000.0, 4),
    ("16000", 16000.0, 3),
    ("22050", 22050.0, 2),
    ("24000", 24000.0, 2),
    ("32000", 32000.0, 1),
    ("44100", 44100.0, 1),
    ("48000", 48000.0, 1),
    ("96000", 96000.0, 1),
)


PROT = chk.AUDIO_EDGE       # 20000/48000 -- the codebase's "content is band-limited to this"


def alias_resampler(proto, in_hz, out_hz, freqs):
    """Worst alias landing inside the protected band of a step > 1 leg.

    Source content is assumed band-limited to PROT * in_hz, the same assumption the -105 dB
    Candidate gate makes; a contribution counts only if it lands at or below PROT * out_hz.
    """
    land = fold(freqs, out_hz)
    sel = ((freqs <= PROT * in_hz) & (land <= PROT * out_hz)
           & (np.abs(land - freqs) > 0.5))
    if not sel.any():
        return None
    mag = mag_at(proto, float(L) * in_hz, freqs)
    return float(20.0 * np.log10(np.maximum(mag[sel], 1e-15)).max())


def image_resampler(proto, in_hz, out_nyq):
    """Worst interpolator image landing inside the protected band of a step < 1 leg.

    Content up to PROT * in_hz has its lowest replica at (1 - PROT) * in_hz, so that is where
    the region starts -- not at the input Nyquist, which no real source reaches.
    """
    lo = (1.0 - PROT) * in_hz
    hi = min(out_nyq, 20000.0)
    if hi <= lo:
        return None
    g = np.linspace(lo, hi, 8001)
    mag = mag_at(proto, float(L) * in_hz, g)
    return float(20.0 * np.log10(np.maximum(mag, 1e-15)).max())


def edge_resampler(proto, in_hz, out_nyq):
    """Passband loss at 0.9 * the narrower Nyquist of the pair -- comparable across rates."""
    e = 0.9 * min(in_hz * 0.5, out_nyq)
    return float(20.0 * np.log10(max(float(mag_at(proto, float(L) * in_hz,
                                                  np.array([e]))[0]), 1e-15))), e


say(f"{'leg':>14} {'resampler in':>13} {'metric':>7} "
    + "".join(f"{fc:>9.4f}" for fc in FCS) + f"  {'worst degr':>10}")
rows = []
for name, b_hz, den in PAIRS:
    # ---- A -> B leg: 48 kHz in, b_hz out
    inter = 48000.0 / den
    if b_hz < 48000.0:
        kind, in_hz, arg = "alias", inter, b_hz
    elif b_hz > 48000.0:
        kind, in_hz, arg = "image", 48000.0, b_hz * 0.5
    else:
        kind, in_hz, arg = "edge", 48000.0, b_hz * 0.5
    rows.append((f"48->{name}", kind, in_hz, arg, den))
    # ---- B -> A leg: b_hz in, 48 kHz out
    if b_hz < 48000.0:
        rows.append((f"{name}->48", "image", b_hz, 24000.0, 1))
    elif b_hz > 48000.0:
        rows.append((f"{name}->48", "alias", b_hz, 48000.0, 1))

for label, kind, in_hz, arg, den in rows:
    vals = []
    for fc in FCS:
        p = PROTO[fc]
        if kind == "alias":
            f = np.linspace(0.0, in_hz * 0.5, 24001) if in_hz > 48000.0 else FREQS
            v = alias_resampler(p, in_hz, arg, f)
        elif kind == "image":
            v = image_resampler(p, in_hz, arg)
        else:
            v, _e = edge_resampler(p, in_hz, arg)
        vals.append(v)
    if vals[0] is None:
        say(f"{label:>14} {in_hz/1000.0:12.3f}k {kind:>7}   "
            f"(no in-band {kind} for this pair: step 1.0)")
        continue
    ref = vals[FCS.index(FC_REF)]
    degr = max(v - ref for v in vals)
    fe = "" if den == 1 else f"  (/{den} front end excluded here; see section 4)"
    say(f"{label:>14} {in_hz/1000.0:12.3f}k {kind:>7} "
        + "".join(f"{v:9.2f}" for v in vals) + f"  {degr:+10.2f}{fe}")
say()
say("   'worst degr' is the largest rise above the fc = 0.4650 column across the whole sweep,")
say("   i.e. the price paid at fc = 0.4750 (the end of the grid).  Read the individual columns for a smaller step.")
say("   NOTE the 8000 row's front end (43@48k + 147@16k) is not among the host-gated stage sets,")
say("   so its cascade is not recomputed in section 4; its resampler factor is the row above.")

# =============================================================================================
say()
rule("4. THE FIVE GATED FRONT-END CASCADES, RECOMPUTED PER fc   (gate: <= -100.0 dB)")
say("   The resampler is a factor in each of these, so raising fc degrades them.  The three")
say("   single-stage rows (/3 16k, /2 24k, /2 22.05k) are ADDITIONALLY required to clear the")
say("   gate with the stage alone, which is fc-independent -- so for them the cascade figure is")
say("   informational.  The two /4 rows have no stage-alone requirement, which makes them the")
say("   fc-CRITICAL rows: their margin is the resampler's to spend.")
say()

CASCADES = (
    ("11025 /4 (27+129)", chk.DECIMATOR_11025_Q_STAGE1, chk.DECIMATOR_11025_Q_STAGE2,
     12000.0, chk.OUTPUT_NYQUIST_11025_HZ, True),
    ("12000 /4 (27+129)", chk.DECIMATOR_12000_Q_STAGE1, chk.DECIMATOR_12000_Q_STAGE2,
     12000.0, chk.OUTPUT_NYQUIST_12000_HZ, True),
    ("16000 /3 (161)", chk.DECIMATOR_16000_STAGE, None,
     16000.0, chk.OUTPUT_NYQUIST_16000_HZ, False),
    ("24000 /2 (107)", chk.DECIMATOR_24000_STAGE, None,
     24000.0, chk.OUTPUT_NYQUIST_24000_HZ, False),
    ("22050 /2 (107)", chk.DECIMATOR_22050_STAGE, None,
     24000.0, chk.OUTPUT_NYQUIST_22050_HZ, False),
)

cfreqs = np.linspace(0.0, 24000.0, 48001)


def cascade_worst(proto, s1, s2, inter, out_nyq):
    mask = cfreqs > out_nyq
    h1 = mag_at(chk.decimator_stage_response(s1), s1.fs, fold(cfreqs, s1.fs))
    h2 = (mag_at(chk.decimator_stage_response(s2), s2.fs, fold(cfreqs, s2.fs))
          if s2 is not None else np.ones_like(cfreqs))
    h3 = mag_at(proto, float(L) * inter, fold(cfreqs, inter))
    return float(20.0 * np.log10(np.maximum((h1 * h2 * h3)[mask], 1e-15)).max())


say(f"{'cascade':>20} {'critical':>9} " + "".join(f"{fc:>9.4f}" for fc in FCS))
for label, s1, s2, inter, nyq, critical in CASCADES:
    vals = [cascade_worst(PROTO[fc], s1, s2, inter, nyq) for fc in FCS]
    say(f"{label:>20} {('YES' if critical else 'no'):>9} "
        + "".join(f"{v:9.2f}" for v in vals))
say()
say(f"{'cascade':>20} {'first fc that FAILS the -100 dB gate':>44}")
for label, s1, s2, inter, nyq, critical in CASCADES:
    bad = [fc for fc in FCS if cascade_worst(PROTO[fc], s1, s2, inter, nyq) > CASCADE_GATE]
    say(f"{label:>20} {(f'{bad[0]:.4f}' if bad else 'none in 0.4600-0.4750'):>44}")

# =============================================================================================
say()
rule("5. 48 -> 32 kHz CANDIDATES RE-EVALUATED AGAINST fc   (L=2/M=3 front end, 16 channels)")
say("   Front end designed at the 96 kHz interpolated rate, L=2 phases, MAC/output = N/2,")
say("   11 outputs per block worst case, 32 kernel calls (2 per channel, both input stride 3).")
say("   Passband and alias are reported SEPARATELY, and the alias figure is split into the")
say("   protected region and the whole 0-16 kHz output band.")
say()


def chain_alias(coeff96, proto, protect_hz=None):
    """Worst alias of the up2 / LPF / down3 chain including the resampler.

    Zero-stuffing by 2 makes the 48 kHz-rate spectrum periodic with 48 kHz on the 96 kHz axis,
    so an input at f appears at BOTH f and 48000-f; each is shaped by H at its own frequency,
    then folded by the 3:1 decimation.  A contribution is an alias whenever it lands away from
    f, which includes f below the output Nyquist -- so the plain `f > out_nyq` mask used for
    integer front ends is invalid here.  protect_hz, when given, counts only the aliases that
    LAND at or below protect_hz.
    """
    worst = -300.0
    for g in (FREQS, 48000.0 - FREQS):
        land = fold(g, OUT_HZ)
        mag = mag_at(coeff96, 96000.0, g) * mag_at(proto, float(L) * OUT_HZ, land)
        sel = np.abs(land - FREQS) > 0.5
        if protect_hz is not None:
            sel = sel & (land <= protect_hz)
        if sel.any():
            worst = max(worst, float(20.0 * np.log10(np.maximum(mag[sel], 1e-15)).max()))
    return worst


def chain_passband(coeff96, proto, level_db):
    """Highest frequency where the whole chain is still within level_db of 0."""
    f = np.linspace(0.0, 16000.0, 16001)
    curve = 20.0 * np.log10(np.maximum(
        mag_at(coeff96, 96000.0, f) * mag_at(proto, float(L) * OUT_HZ, f), 1e-15))
    idx = np.nonzero(curve <= level_db)[0]
    return f[idx[0]] if idx.size else 16000.0


def us_for(n):
    return cost_us(CH * 11 * (n // 2), 32)


say("(a) Candidate P (partial protection) -- N held at 97, designed passband 15000 Hz,")
say("    stopband on the output Nyquist.  Only fc moves.")
say(f"{'fc':>8} {'-0.5dB pb':>10} {'-1dB pb':>9} {'gain@13k':>9} {'alias<13k':>10} "
    f"{'alias 0-16k':>12} {'us/blk':>8} {'%/333':>7}")
c97 = dsg.design(97, 96000.0, 15000.0, OUT_NYQ)
for fc in FCS:
    p = PROTO[fc]
    pb05 = chain_passband(c97, p, -0.5)
    pb10 = chain_passband(c97, p, -1.0)
    g13 = 20.0 * np.log10(max(float(mag_at(c97, 96000.0, np.array([13000.0]))[0]
                                    * mag_at(p, float(L) * OUT_HZ, np.array([13000.0]))[0]),
                              1e-15))
    a13 = chain_alias(c97, p, 13000.0)
    aall = chain_alias(c97, p)
    u = us_for(97)
    tag = "  <- shipping" if fc == FC_REF else ""
    say(f"{fc:8.4f} {pb05:10.0f} {pb10:9.0f} {g13:9.2f} {a13:10.2f} {aall:12.2f} "
        f"{u:8.1f} {100.0*u/BLOCK_US:7.1f}%" + tag)

say()
say("(b) Strict candidates -- smallest N whose WHOLE 0-16 kHz output band clears -100 dB,")
say("    rescanned for each fc because the resampler is part of the chain.")
say(f"{'fc':>8} {'passband':>9} {'N':>5} {'N/2':>5} {'worst alias':>12} {'-0.5dB pb':>10} "
    f"{'-1dB pb':>9} {'us/blk':>8} {'%/333':>7} {'hist/ch':>8} {'coeffX':>7}")
for fc in FCS_STRICT:
    p = PROTO[fc]
    for pb in (12000.0, 12800.0, 13600.0):
        n = coarse_fine(33, 1025, 32, 4,
                        lambda n: chain_alias(dsg.design(n, 96000.0, pb, OUT_NYQ), p) <= GATE)
        if n is None:
            say(f"{fc:8.4f} {pb:9.0f} {'-':>5}   no solution <= 1024 taps")
            continue
        c = dsg.design(n, 96000.0, pb, OUT_NYQ)
        u = us_for(n)
        half = n // 2
        say(f"{fc:8.4f} {pb:9.0f} {n:5d} {half:5d} {chain_alias(c, p):12.2f} "
            f"{chain_passband(c, p, -0.5):10.0f} {chain_passband(c, p, -1.0):9.0f} "
            f"{u:8.1f} {100.0*u/BLOCK_US:7.1f}% {half + 10:8d} {2*half*4:6d}B")
say()
say("   hist/ch = half + (outputs-1)*decim rounded up = half + 10 samples, against")
say("   DEC_Q31_HIST_PER_CH = 190.  coeffX = 2 phases * half * 4 B of X-space coefficients,")
say("   against s_q31_coeff = 844 B and 2794 B of free data RAM.")
