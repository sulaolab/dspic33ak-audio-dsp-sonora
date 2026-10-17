#!/usr/bin/env python3
"""HOST model of the REAR resampler's phase-interpolation spur (the quantity that
turned out to be the limiter of 44.1 -> 16 kHz -- study 2026-09-14 SS 24.5 / SS 26).

WHAT THIS IS FOR.  The 45-pair spectral matrix so far ranked pairs by FRONT-END
ALIAS.  The 44.1 -> 16 kHz measurement showed the alias is not always the worst
in-band unwanted component: alias -114.46 dBc, but a spur at 3,125 Hz sat at
-105.7 dBc, and the adopted spectral performance of a pair is now the WORST
unwanted component inside 0 .. 0.8125 x FN (study SS 26.1).  So a HOST model has
to produce that spur, not just the filter response.

THE MECHANISM, AND WHY IT IS FALSIFIABLE.  The rear resampler stores L=128
sub-filters and LINEARLY interpolates between rows p and p+1 (audio_app_asrc.c
asrc_poly_phase, asrc_poly_q31.inc asrc_q31_phase_of).  The interpolation error is a
periodic function of the inter-row weight wb with period 1 in wb, i.e. period 1/L in
the read phase `frac`.  frac advances by `step` per output sample, so the error acts
as a carrier at

    d_hz = ((L * step) mod 1) * fs_out

multiplying a signal-dependent envelope -> sidebands at |f_tone +/- k * d_hz|, folded
into [0, fs_out/2].  That is a prediction with a MOVING target: d_hz depends on the
exact frozen float32 step, so two captures of the same tone at slightly different
frozen steps must place the spur at DIFFERENT frequencies.  The anchor captures do,
which is what separates this mechanism from a fixed artefact of the instrument.

WHAT IS ISOLATED FROM WHAT.  Three arms over one and the same phase bookkeeping:

  q31   bit-exact shipping path (Q31 table, Q31 blend, sacr rounding, slot mask)
  lin64 float64 rows, same LINEAR inter-row blend  -> the spur without arithmetic noise
  cont  float64 prototype evaluated at the EXACT frac, no row grid at all
        -> the filter's own imaging only, i.e. the spur-free reference

So   spur    = lin64 against cont     (phase interpolation)
     numeric = q31   against lin64    (Q31 blend + sacr + slot mask)
and the reported "overall worst in-band unwanted component" is read off the q31 arm,
because that is what the target actually emits.

The FRONT END is deliberately not modelled here: an in-band tone passes its passband,
and the front end's own alias is a separate number that
tools/asrc/asrc_universal_nyquist_model.py already produces.  This tool supplies the
term that was missing -- what the REAR adds inside the protected band -- so that a
pair's spectral performance can be taken as the worse of the two.
"""
from __future__ import annotations

import argparse
import fractions
import math
import pathlib
import struct
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import asrc_q31_resampler_model as Q  # noqa: E402  (exact mirror of the target arithmetic)

f32 = np.float32
FULL_SCALE = float(2 ** 23 - 1)


# --------------------------------------------------------------- phase walk ---
def phase_walk(step, frac0, nout):
    """The target's per-output (p, wb, window base), float32 bookkeeping exactly.

    Sequential on purpose: `frac += step; while (frac >= 1.0f) { frac -= 1.0f; rd++; }`
    in float32 is not the same sequence as frac(n*step) evaluated in float64, and the
    spur's exact frequency is set by this sequence.
    """
    rd = Q.MH
    frac = f32(frac0)
    ps = np.empty(nout, dtype=np.int64)
    wbs = np.empty(nout, dtype=np.float32)
    ws = np.empty(nout, dtype=np.int64)
    st = f32(step)
    for n in range(nout):
        p, wb = Q.phase_of(frac)
        ps[n], wbs[n], ws[n] = p, wb, rd - Q.MH
        frac = f32(frac + st)
        while float(frac) >= 1.0:
            frac = f32(frac - f32(1.0))
            rd += 1
    return ps, wbs, ws


def carrier_hz(step, fs_out, k=1):
    """d_hz: the inter-row interpolation error's carrier (see the module docstring)."""
    return ((float(f32(step)) * Q.L * k) % 1.0) * fs_out


def spur_freqs(tone_hz, step, fs_out, kmax=2):
    """Where the sidebands land after folding into [0, fs_out/2]."""
    out = []
    for k in range(1, kmax + 1):
        d = carrier_hz(step, fs_out, k)
        for s in (-1.0, +1.0):
            f = abs(tone_hz + s * d) % fs_out
            if f > fs_out / 2.0:
                f = fs_out - f
            out.append((k, int(s), f))
    return out


# ------------------------------------------------------------- coefficients ---
def proto_row_f64(frac):
    """The prototype at an ARBITRARY read phase, float64, per-row unity DC.

    Same formula as asrc_poly_build() and poly_build_f64(), with p/L replaced by the
    continuous `frac`.  This is the row the target would use if it stored infinitely
    many phases, so `cont` differs from `lin64` only by the interpolation error.
    """
    k = np.arange(Q.M, dtype=np.float64)
    d = k - float(Q.MH) - float(frac)
    x = 2.0 * Q.FC * d
    sinc = np.where(np.abs(x) < 1.0e-12, 1.0, np.sinc(x))
    wpos = (d + float(Q.MH) + 1.0) / float(Q.M)
    kx = 2.0 * wpos - 1.0
    kr = np.sqrt(np.maximum(0.0, 1.0 - kx * kx))
    win = np.i0(Q.BETA * kr) / np.i0(Q.BETA)
    c = 2.0 * Q.FC * sinc * win
    s = c.sum()
    return c / s if abs(s) > 1.0e-12 else c


# ------------------------------------------------------------------- arms -----
def run_q31(x_slots, ps, wbs, ws, rows_q31):
    """Bit-exact shipping path, vectorised.

    The accumulation is kept exact in int64 by factoring the slot's eight dead LSBs
    out first (x = v << 8, |v| <= 2^23), so no partial sum can approach 2^63; the 256
    goes back before sacr_l, which is the same integer.  `selfcheck` asserts this arm
    is bit-identical to the reference loop.
    """
    nout = ps.size
    c0 = rows_q31[ps]
    c1 = rows_q31[ps + 1]
    wbq = np.array([Q.wb_to_q31(w) for w in wbs], dtype=np.int64)[:, None]
    ceff = c0 + ((wbq * (c1 - c0) + (1 << 30)) >> 31)
    idx = ws[:, None] + np.arange(Q.M, dtype=np.int64)[None, :]
    v = (np.asarray(x_slots, dtype=np.int64) >> 8)[idx]
    s = (ceff * v).sum(axis=1)
    out = np.empty(nout, dtype=np.int64)
    for n in range(nout):
        out[n] = Q.q31_to_slot(Q.sacr_l(int(s[n]) * 256))
    return out.astype(np.float64)


def run_lin64(x_slots, ps, wbs, ws, rows_f64):
    idx = ws[:, None] + np.arange(Q.M, dtype=np.int64)[None, :]
    xw = np.asarray(x_slots, dtype=np.float64)[idx]
    w = np.asarray(wbs, dtype=np.float64)[:, None]
    ceff = rows_f64[ps] * (1.0 - w) + rows_f64[ps + 1] * w
    return (ceff * xw).sum(axis=1)


def run_cont(x_slots, ps, wbs, ws):
    """`cont`: the row at the exact frac = (p + wb)/L, no row grid.  Rows are cached
    per distinct frac because a near-periodic step revisits only a few hundred."""
    idx = ws[:, None] + np.arange(Q.M, dtype=np.int64)[None, :]
    xw = np.asarray(x_slots, dtype=np.float64)[idx]
    cache = {}
    out = np.empty(ps.size, dtype=np.float64)
    for n in range(ps.size):
        key = (int(ps[n]), float(wbs[n]))
        row = cache.get(key)
        if row is None:
            row = proto_row_f64((float(ps[n]) + float(wbs[n])) / float(Q.L))
            cache[key] = row
        out[n] = float(row @ xw[n])
    return out


# ----------------------------------------------------------------- closed form ---
def rear_spur_dbc(f_tone_hz, rear_in_hz, phases=None):
    """Closed form for the k=1 phase-interpolation sideband, in dBc.

        spur = 40 * log10( f_tone / (L * rear_fs_in) )

    DERIVED, NOT FITTED.  Linear interpolation between rows p and p+1 has error
    -(1/2) * w(1-w) * (1/L)^2 * d2y/dphi^2 with phi in INPUT samples; for a tone
    d2y/dphi^2 = -(2*pi*f/fs_in)^2 * y, and the periodic weight w(1-w) has first
    Fourier coefficient 1/(2*pi^2).  The 2*pi and the pi^2 cancel exactly:

        |spur| / |tone| = (1/(2*pi^2)) * (1/2) * (2*pi*f/(L*fs_in))^2
                        = (f / (L * fs_in))^2

    So there is no free constant to tune, and three levers only: the protected edge,
    L, and THE REAR RESAMPLER'S INPUT RATE.  Consequences that matter for design:

      * the FRONT-END FILTER cannot improve it at all -- only how much the front end
        DECIMATES can, and every extra /2 costs exactly 40*log10(2) = 12.04 dB;
      * halving L costs 12.04 dB, doubling L buys 12.04 dB (until the filter's own
        imaging becomes the limit -- at L=256 the 44.1 -> 16 row is already there).

    TWO CAVEATS, BOTH MEASURED (see `law`).  The derivation treats w as sweeping the
    whole [0,1) ramp, which holds when the NOMINAL ratio fs_in/fs_out = a/q has a large
    denominator q (441 for 16 -> 11.025, 320 for 22.05 -> 16).  When q is small the wb
    sequence has period q and the error's energy collapses onto a few lines instead of
    spreading, so the PEAK can sit ABOVE the law -- measured +4.6 dB at 32 -> 24 kHz
    (q = 3).  At the other extreme, when (L*a) mod q == 0 the weight is CONSTANT and
    there is no modulation and no spur at all: that is 48 -> 32 (step 3/2) and
    96 -> 48 (step 2), and it is what asrc_app_config.h means by "an inter-phase blend
    weight of exactly zero".  So treat this as a SIZING law with a +/-1.5 dB band for
    q >= 32, and VERIFY any small-q pair with the simulator -- exactly as
    asrc_universal_nyquist_model.TAP_LAW_K sizes taps and the spectral model verifies.

    THRESHOLDS THIS LAW IMPLIES at L=128 with the protected edge at 0.8125 x FN
    (so fp = 0.40625 * fs_out), as a bound on the REAR STEP:

        step >= 1.0037   for the -100 dBc product guarantee
        step >= 1.3384   for the -105 dBc engineering target

    i.e. it is the DENSE pairs, not the wide ones, that the rear spur threatens, and
    every extra /2 of front-end decimation moves a pair 12.04 dB toward the limit.
    """
    lp = float(Q.L if phases is None else phases)
    return 40.0 * math.log10(max(float(f_tone_hz), 1.0e-12) / (lp * float(rear_in_hz)))


# ------------------------------------------------------------- measurement ----
# GUARD_BINS: half width of the exclusion around the tone, in FFT bins, when a
# caller does not name one in Hz.  MEASURED, not guessed: on the 2026-09-15
# 48 -> 44.1 kHz captures (Kaiser beta=24, n=2048) the reading is the tone own
# skirt at 4 and 6 bins (-34 / -86 dBc) and then sits on a dead flat plateau from
# 8 bins out to 30 (-113.59 dBc, same bin every time).  12 is 1.5x the plateau
# entry, which is what guard_plateau() below checks.  A fixed 120 Hz -- the value
# this file shipped with -- is 5.6 bins at that geometry, and read 48 dB high.
GUARD_BINS = 12.0


def guard_of(freqs, guard_hz=None):
    """The tone guard in Hz: what the caller asked for, or GUARD_BINS bins."""
    if guard_hz is not None:
        return float(guard_hz)
    return GUARD_BINS * float(freqs[1] - freqs[0])


def spectrum(y_slots, beta=24.0, slot_shift=8):
    """Same convention as tools/asrc/analyze_meas_capture_alias.py: Kaiser window,
    normalised so that a full-scale sine reads 0 dBFS.

    slot_shift IS NOT COSMETIC.  This model arms emit s24-LEFT slots (a 24-bit
    sample << 8, the form the TDM slot carries), so the default 8 puts them back
    on 24-bit counts.  A HARDWARE *MEAS capture is ALREADY plain 24-bit counts, so
    a reader must pass slot_shift=0 -- and analyze_meas_capture_spur.py did not,
    which scaled every hardware spectrum by 1/256 = -48.16 dB.  That did not
    cancel out of the dBc figure because the dBc was taken against the header
    NOMINAL level instead of the tone measured in the same spectrum, so the
    48 -> 44.1 kHz capture was reported at -161.92 dBc when the truth is -113.6
    (Phase F F-A, 2026-09-15).  Two lessons are wired into the code below: the
    shift is explicit, and dBc is referred to the MEASURED tone so that any scale
    error cancels."""
    x = np.asarray(y_slots, dtype=np.float64) / float(1 << int(slot_shift))
    w = np.kaiser(x.size, beta)
    X = np.fft.rfft(x / FULL_SCALE * w)
    return np.abs(X) * 2.0 / w.sum()


def db(v):
    return 20.0 * math.log10(max(abs(float(v)), 1.0e-300))


def worst_in_band(mag, freqs, protected_hz, tone_hz, guard_hz=None, first_bin=3):
    """Worst unwanted component in 0..protected_hz, excluding DC leakage and the
    tone's own main lobe.  This is the SS 26.1 adoption rule, applied on HOST.

    guard_hz=None means GUARD_BINS bins of THIS spectrum, which is the only form
    that is safe across rates: a guard in Hz is a different number of bins at
    every (fs_out, n), and too few bins reports the tone's skirt as the spur."""
    guard_hz = guard_of(freqs, guard_hz)
    keep = (freqs <= protected_hz) & (np.arange(freqs.size) >= first_bin)
    keep &= np.abs(freqs - tone_hz) > guard_hz
    if not keep.any():
        return None, None
    k = int(np.argmax(np.where(keep, mag, 0.0)))
    return db(mag[k]), float(freqs[k])


def level_at(mag, freqs, f_hz, window_hz=40.0):
    sel = np.abs(freqs - f_hz) <= window_hz
    if not sel.any():
        return None, None
    k = int(np.argmax(np.where(sel, mag, 0.0)))
    return db(mag[k]), float(freqs[k])


def tone_slots(f_hz, fs_hz, n, level_dbfs, bits=24):
    """The tone as it reaches the rear FIFO: s24-left slots.  `bits=31` keeps full
    Q31 resolution instead, to show the result does not rest on the dead LSBs."""
    amp = FULL_SCALE * (10.0 ** (level_dbfs / 20.0))
    ph = 2.0 * math.pi * f_hz / fs_hz * np.arange(n, dtype=np.float64)
    v = np.sin(ph) * amp
    if bits == 24:
        return np.rint(v).astype(np.int64) << 8
    return np.rint(v * 256.0).astype(np.int64)


# ------------------------------------------------------------------ report ----
def evaluate(step, fs_out, tone_hz, level_dbfs, nout, frac0, protected_hz,
             arms=('q31', 'lin64', 'cont'), input_bits=24, beta=24.0,
             guard_hz=None, tone_snap=False):
    fs_in = float(step) * fs_out
    if tone_snap:
        # F-A option: put the tone on the OUTPUT bin grid so that it leaks
        # nothing at all.  MEASURED CONCLUSION (Phase F F-A): not needed, and not
        # the default.  With a correct guard the non-coherent tone already reads
        # the spur to 0.10 dB of hardware, while snapping moves the tone by up to
        # half a bin (-411 ppm at 17,750 Hz / 44.1 kHz) -- and the SPUR is still
        # not on the grid, so the rectangular window that snapping makes tempting
        # under-reads it by up to 2.9 dB of scallop loss.  Kept as the cross-check
        # that proves the guard is doing its job, not as a measurement mode.
        tone_hz = round(tone_hz * nout / fs_out) * fs_out / nout
    n_in = Q.MH + int(math.ceil(float(step) * (nout + 2))) + Q.M + 8
    x = tone_slots(tone_hz, fs_in, n_in, level_dbfs, bits=input_bits)
    ps, wbs, ws = phase_walk(step, frac0, nout)

    rows_f64 = Q.poly_build_f64()
    res = {}
    if 'q31' in arms:
        res['q31'] = run_q31(x, ps, wbs, ws, Q.poly_build_q31_shipped())
    if 'lin64' in arms:
        res['lin64'] = run_lin64(x, ps, wbs, ws, rows_f64)
    if 'cont' in arms:
        res['cont'] = run_cont(x, ps, wbs, ws)

    freqs = np.fft.rfftfreq(nout, 1.0 / fs_out)
    g = guard_of(freqs, guard_hz)
    out = {'fs_in': fs_in, 'freqs': freqs, 'y': res, 'mag': {}, 'tone_dbfs': {},
           'tone_hz': tone_hz, 'guard_hz': g}
    for k, y in res.items():
        mag = spectrum(y, beta)
        out['mag'][k] = mag
        t_lvl, _t_f = level_at(mag, freqs, tone_hz, window_hz=g)
        out['tone_dbfs'][k] = t_lvl
        lvl, f = worst_in_band(mag, freqs, protected_hz, tone_hz, guard_hz=g)
        # dBc against the tone MEASURED IN THIS SPECTRUM, not against the nominal
        # injected level.  Scale invariant on purpose: a wrong slot scale, a codec
        # gain, or the 0.17 dB window bias on the tone can then no longer
        # masquerade as a spectral result (Phase F F-A).
        ref = t_lvl if t_lvl is not None else level_dbfs
        out[k] = (lvl - ref if lvl is not None else None, f)
    return out


def guard_plateau(mag, freqs, protected_hz, tone_hz, guard_hz=None,
                  factor=1.5, tol_db=0.5):
    """Is the reading on the guard plateau, or still the tone's own skirt?

    Widening a correct guard by 50 % must not move the answer.  If it does, the
    argmax is inside the tone's main lobe and the number is not a spur level.
    This is the check that would have caught the 120 Hz default (-65.83 dBc)
    the moment it was printed."""
    g = guard_of(freqs, guard_hz)
    a, fa = worst_in_band(mag, freqs, protected_hz, tone_hz, guard_hz=g)
    b, fb = worst_in_band(mag, freqs, protected_hz, tone_hz, guard_hz=g * factor)
    ok = (a is not None) and (b is not None) and (abs(a - b) <= tol_db)
    return ok, (a, fa), (b, fb)


def decode_step(step, bits):
    if bits:
        return float(struct.unpack('<f', struct.pack('<I', int(bits, 16)))[0])
    if step is None:
        sys.exit("give --step or --step-bits")
    return float(step)


def cmd_anchor(args):
    step = decode_step(args.step, args.step_bits)
    fs_out = args.fs_out
    tone = args.tone
    prot = args.protected if args.protected else 0.8125 * fs_out / 2.0
    bits = struct.unpack('<I', struct.pack('<f', np.float32(step)))[0]

    print("REAR-SPUR HOST MODEL -- anchor check")
    print("  geometry     L=%d M=%d fc=%.4f beta=%.2f" % (Q.L, Q.M, Q.FC, Q.BETA))
    print("  step         %.10f  (float32 0x%08X)" % (step, bits))
    print("  rear fs_in   %.3f Hz -> fs_out %.1f Hz   protected band 0 .. %.1f Hz"
          % (step * fs_out, fs_out, prot))
    print("  tone         %.1f Hz = %.4f x FN_out    level %+g dBFS   n=%d"
          % (tone, tone / (fs_out / 2.0), args.level, args.nout))
    print()
    print("  predicted sideband positions, d_hz = ((L*step) mod 1) * fs_out, folded:")
    for k, s, f in spur_freqs(tone, step, fs_out, kmax=2):
        print("    k=%d %s  d=%9.2f Hz   ->  %9.2f Hz"
              % (k, '-' if s < 0 else '+', carrier_hz(step, fs_out, k), f))
    print()

    r = evaluate(step, fs_out, tone, args.level, args.nout, args.frac0, prot,
                 input_bits=args.input_bits, beta=args.fft_beta,
                 guard_hz=args.guard_hz, tone_snap=args.tone_snap)
    freqs = r['freqs']
    print("  analysis     n=%d  bin %.3f Hz  guard +/-%.1f Hz (%.1f bins)"
          "  tone measured %+.2f dBFS"
          % (args.nout, fs_out / args.nout, r['guard_hz'],
             r['guard_hz'] / (fs_out / args.nout), r['tone_dbfs']['q31']))
    ok, lo, hi = guard_plateau(r['mag']['q31'], freqs, prot, r['tone_hz'],
                               guard_hz=r['guard_hz'])
    print("  guard        %s  (%.2f dBFS at the guard, %.2f at 1.5x)"
          % ('ON PLATEAU' if ok else 'NOT ON PLATEAU -- this is the tone lobe',
             lo[0], hi[0]))
    print("  worst unwanted component inside the protected band (tone lobe excluded):")
    for k, label in (('cont', 'cont  (no row grid)      = filter imaging only'),
                     ('lin64', 'lin64 (linear inter-row) = + phase-interp spur'),
                     ('q31', 'q31   (shipping path)    = + Q31/slot arithmetic')):
        lvl, f = r[k]
        print("    %-46s %+8.2f dBc at %9.1f Hz" % (label, lvl, f))
    print()
    for k, s, f in spur_freqs(tone, step, fs_out, kmax=1):
        l_q, f_q = level_at(r['mag']['q31'], freqs, f, args.window)
        l_c, _ = level_at(r['mag']['cont'], freqs, f, args.window)
        print("    k=1 %s sideband, predicted %8.1f Hz : q31 %+8.2f dBc (peak at %8.1f Hz)"
              "   cont %+8.2f dBc"
              % ('-' if s < 0 else '+', f, l_q - args.level, f_q, l_c - args.level))
    return 0


def cmd_scan(args):
    step = decode_step(args.step, args.step_bits)
    fs_out = args.fs_out
    fn = fs_out / 2.0
    prot = args.protected if args.protected else 0.8125 * fn
    tones = np.linspace(args.from_fn * fn, args.to_fn * fn, args.points)
    print("REAR-SPUR HOST MODEL -- tone scan  (L=%d M=%d fc=%.4f beta=%.2f)"
          % (Q.L, Q.M, Q.FC, Q.BETA))
    print("  step %.10f   rear fs_in %.1f -> fs_out %.1f   protected 0 .. %.1f Hz   level %+g dBFS"
          % (step, step * fs_out, fs_out, prot, args.level))
    print("  d_hz(k=1) = %.2f Hz" % carrier_hz(step, fs_out))
    print()
    print("  %9s %8s | %10s %10s | %10s %10s | %8s"
          % ("tone Hz", "x FN", "cont dBc", "at Hz", "q31 dBc", "at Hz", "spur dB"))
    worst = (-999.0, None, None)
    for t in tones:
        r = evaluate(step, fs_out, float(t), args.level, args.nout, args.frac0, prot,
                     input_bits=args.input_bits, beta=args.fft_beta,
                     guard_hz=args.guard_hz, tone_snap=args.tone_snap)
        c_l, c_f = r['cont']
        q_l, q_f = r['q31']
        print("  %9.1f %8.4f | %10.2f %10.1f | %10.2f %10.1f | %8.2f"
              % (t, t / fn, c_l, c_f, q_l, q_f, q_l - c_l))
        if q_l > worst[0]:
            worst = (q_l, float(t), q_f)
    print()
    print("  WORST in-band unwanted component over the scan: %+.2f dBc"
          " at tone %.1f Hz (component at %.1f Hz)" % worst)
    return 0


def cmd_selfcheck(args):
    """Negative test: the vectorised q31 arm must be BIT-IDENTICAL to the reference
    loop in asrc_q31_resampler_model.resample(mode='q31'), the one the target's boot
    vectors already validate.  Without this the fast path could be quietly wrong in
    exactly the digits the spur lives in."""
    rf = Q.poly_build_f32()
    rq = Q.poly_build_q31_shipped()
    fails = 0
    for step in (1.378129482269287, 1.5, 2.0, 32000.0 / 11025.0, 1.0):
        nout = 96
        n_in = Q.need_input(step, nout)
        for sig in ('sine:0.29333:0.89', 'lcg', 'dc_small'):
            x = Q.gen_signal(sig, n_in)
            ref = Q.resample(x, step, 0.0, nout, rf, rq, 'q31')
            ps, wbs, ws = phase_walk(step, 0.0, nout)
            got = run_q31(x, ps, wbs, ws, rq)
            bad = int(np.sum(ref.astype(np.float64) != got))
            fails += bad
            print("  step %-20.12g %-18s mismatches %d" % (step, sig, bad))
    print("selfcheck: %s" % ("pass" if fails == 0 else "FAIL (%d)" % fails))
    return 0 if fails == 0 else 1


def cmd_law(args):
    """Closed form against the bit-exact simulator, over rates / tones / L."""
    print("CLOSED FORM  spur_dBc = 40*log10( f_tone / (L * rear_fs_in) )   vs the simulator")
    print("  %8s %9s %6s | %10s %10s %8s" % ("rear_in", "tone", "L", "law dBc", "sim dBc", "delta"))
    worst = 0.0
    scored = 0
    skipped = 0
    cases = []
    for rear_in, fs_out in ((22050.072, 16000.0), (16000.0, 11025.0), (32000.0, 11025.0),
                            (48000.0, 32000.0), (24000.0, 16000.0), (32000.0, 24000.0),
                            (48000.0, 44100.0)):
        fn = fs_out / 2.0
        for frac in (0.4, 0.8125):
            cases.append((rear_in, fs_out, frac * fn))
    for phases in (64, 128, 256):
        Q.set_geometry(argparse.Namespace(phases=phases, taps=args.taps, fc=args.fc, beta=args.beta))
        for rear_in, fs_out, tone in cases:
            step = rear_in / fs_out
            r = evaluate(step, fs_out, tone, args.level, args.nout, 0.0,
                         0.8125 * fs_out / 2.0, arms=('lin64', 'cont'),
                         input_bits=31, beta=args.fft_beta)
            # read the k=1 sideband explicitly: at low tones the WORST in-band
            # component is the filter's own imaging, which this law does not model.
            pred = rear_spur_dbc(tone, rear_in, phases)
            # DEGENERATE STEPS ARE NOT A MISS, THEY ARE THE OTHER HALF OF THE MECHANISM.
            # d_hz = 0 when L*step is an integer: the inter-row weight is then CONSTANT, so
            # there is no modulation and no spur at all -- which is what
            # asrc_app_config.h says about 48->48 / 48->32 / 48->24 / 44.1->44.1 ("blend
            # weight of exactly zero").  And when a predicted sideband lands ON the tone
            # (or on DC) the spur is unobservable by construction, not absent.  Both are
            # skipped rather than scored, and counted, so a silently empty check is visible.
            d_hz = carrier_hz(step, fs_out)
            k1 = spur_freqs(tone, step, fs_out, kmax=1)
            guard = 4.0 * fs_out / args.nout
            if d_hz < guard or any(min(abs(f - tone), f) < guard for _, _, f in k1):
                skipped += 1
                why = 'd_hz=0 (no modulation)' if d_hz < guard else 'sideband on tone/DC'
                print("  %8.0f %9.1f %6d | %10.2f %>10s %8s   skip: %s"
                      .replace('%>10s', '%10s') % (rear_in, tone, phases, pred, '-', '-', why))
                continue
            sim = max(level_at(r['mag']['lin64'], r['freqs'], f, 40.0)[0] for _, _, f in k1)
            sim -= args.level
            d = sim - pred
            # q = denominator of the NOMINAL ratio in lowest terms.  Small q concentrates
            # the error onto a few lines, so the law is not a bound there -- reported, not
            # scored (see rear_spur_dbc's caveats).
            q = fractions.Fraction(rear_in / fs_out).limit_denominator(4096).denominator
            if q < 32:
                print("  %8.0f %9.1f %6d | %10.2f %10.2f %+8.2f   q=%d line-concentrated,"
                      " not scored" % (rear_in, tone, phases, pred, sim, d, q))
                skipped += 1
                continue
            worst = max(worst, abs(d))
            scored += 1
            print("  %8.0f %9.1f %6d | %10.2f %10.2f %+8.2f   q=%d" % (rear_in, tone, phases, pred, sim, d, q))
    print()
    print("  scored %d cases, not scored %d (degenerate step, unobservable sideband, or small q)"
          % (scored, skipped))
    print("  worst |delta| : %.2f dB" % worst)
    ok = (worst <= 1.5) and (scored >= 12)
    print("  verdict: %s" % ("law usable for SIZING (band +/-1.5 dB)" if ok
                             else "LAW OUT OF BAND or too few scored cases -- do not size with it"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p):
        Q.add_geometry_args(p)
        p.add_argument('--step', type=float, default=None, help='rear step = fs_in/fs_out')
        p.add_argument('--step-bits', type=str, default=None,
                       help='the frozen float32 step as it appears in *MEAS_BEGIN '
                            'freeze_applied_bits (e.g. 0x3FB0668C) -- use this to '
                            'reproduce a specific capture exactly')
        p.add_argument('--fs-out', type=float, default=16000.0)
        p.add_argument('--level', type=float, default=-1.0, help='injected level in dBFS')
        p.add_argument('--nout', type=int, default=2048)
        p.add_argument('--frac0', type=float, default=0.0)
        p.add_argument('--protected', type=float, default=None,
                       help='protected band edge in Hz (default 0.8125 x FN_out)')
        p.add_argument('--input-bits', type=int, choices=(24, 31), default=24)
        p.add_argument('--fft-beta', dest='fft_beta', type=float, default=24.0,
                       help='Kaiser window beta for the analysis FFT (matches the analyser)')
        p.add_argument('--guard-hz', dest='guard_hz', type=float, default=None,
                       help='tone guard half width in Hz (default: %g bins of '
                            'the analysis FFT -- see GUARD_BINS)' % GUARD_BINS)
        p.add_argument('--tone-snap', dest='tone_snap', action='store_true',
                       help='snap the tone onto the output bin grid (coherent '
                            'cross-check; NOT a measurement mode -- see evaluate)')

    p = sub.add_parser('anchor', help='reproduce one capture and name the sidebands')
    common(p)
    p.add_argument('--tone', type=float, default=6468.0)
    p.add_argument('--window', type=float, default=40.0)
    p.set_defaults(fn=cmd_anchor)

    p = sub.add_parser('scan', help='sweep the tone across the protected edge')
    common(p)
    p.add_argument('--from-fn', type=float, default=0.50)
    p.add_argument('--to-fn', type=float, default=0.8125)
    p.add_argument('--points', type=int, default=12)
    p.set_defaults(fn=cmd_scan)

    p = sub.add_parser('law', help='closed form vs the simulator, over rates / tones / L')
    common(p)
    p.set_defaults(fn=cmd_law)

    p = sub.add_parser('selfcheck', help='bit-identity of the fast q31 arm vs the reference loop')
    Q.add_geometry_args(p)
    p.set_defaults(fn=cmd_selfcheck)

    args = ap.parse_args()
    Q.set_geometry(args)
    return args.fn(args)


if __name__ == '__main__':
    raise SystemExit(main())
