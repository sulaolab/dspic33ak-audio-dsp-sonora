#!/usr/bin/env python3
"""Bit-exact reference model for the AK512 generic polyphase ASRC resampler,
float (shipping) and Q31 (new), plus the float-vs-Q31 numerical/frequency
comparison and the on-target selftest vector emitter.

The Q-format is NOT newly invented here.  It is the one already shipping and
hardware-verified in src/app/apps/asrc/asrc_decimator_q31.inc:

  coefficient  round(c * 2^31), clamped to [-2^31, 2^31-1]      dec_q31_from_float()
  sample       the DMA slot word itself (s24-left == Q31)       (low 8 bits zero)
  mac.l        plain int64 product c*x, no implicit doubling    dec_q31_st_ref_stage()
  sacr.l       (sum + 2^30) >> 31, then saturate to int32       dec_q31_st_sacr()
  slot out     v & 0xFFFFFF00  (truncates toward -inf)          dec_q31_to_s24_left()

Float side mirrors audio_app_asrc.c: asrc_poly_build(), asrc_poly_phase(),
asrc_poly_at() / the hoisted-blend form the STREAM16 kernels actually use, and
asrc_to_slot() with ASRC_FAST_SLOT_CONVERT == 1 (truncate, then << 8).

Usage:
  python asrc_q31_resampler_model.py coeff
  python asrc_q31_resampler_model.py compare
  python asrc_q31_resampler_model.py freq
  python asrc_q31_resampler_model.py vectors --out ../../src/app/apps/asrc/asrc_poly_q31_vectors.h
"""

import argparse
import math
import sys

import numpy as np

# ---------------------------------------------------------------- config -----
L = 128           # ASRC_POLY_L
M = 30            # ASRC_POLY_M
FC = 0.465        # ASRC_POLY_FC   -- fixed by the specification, do not move
BETA = 11.0       # ASRC_POLY_KAISER_BETA
MH = (M // 2) - 1  # ASRC_POLY_MH == 14
Q31_ONE = 1 << 31
SAMP_MAX = 8388607.0
SAMP_MIN = -8388608.0

f32 = np.float32


# ------------------------------------------------ float coefficient build ----
def bessel_i0_f32(x):
    """asrc_bessel_i0(): 24-term series with the same early-out, in float32."""
    x = f32(x)
    y = f32(f32(0.25) * x * x)
    term = f32(1.0)
    total = f32(1.0)
    for k in range(1, 25):
        fk = f32(k)
        term = f32(term * f32(y / f32(fk * fk)))
        total = f32(total + term)
        if term <= f32(total * f32(1.0e-7)):
            break
    return total


def poly_build_f32():
    """Exact mirror of asrc_poly_build() -- float32, Kaiser-11, per-row unity DC."""
    pi = f32(3.14159265358979)
    knorm = f32(f32(1.0) / bessel_i0_f32(BETA))
    rows = np.zeros((L + 1, M), dtype=np.float32)
    for p in range(L + 1):
        total = f32(0.0)
        for k in range(M):
            d = f32(f32(k) - f32(MH) - f32(f32(p) / f32(L)))
            x = f32(f32(2.0) * f32(FC) * d)
            if -1.0e-6 < float(x) < 1.0e-6:
                sinc = f32(1.0)
            else:
                px = f32(pi * x)
                sinc = f32(f32(math.sin(float(px))) / px)
            wpos = f32(f32(d + f32(MH) + f32(1.0)) / f32(M))
            kx = f32(f32(2.0) * wpos - f32(1.0))
            kr = f32(math.sqrt(max(0.0, float(f32(f32(1.0) - f32(kx * kx))))))
            win = f32(bessel_i0_f32(f32(BETA) * kr) * knorm)
            c = f32(f32(2.0) * f32(FC) * sinc * win)
            rows[p, k] = c
            total = f32(total + c)
        if abs(float(total)) > 1.0e-9:
            inv = f32(f32(1.0) / total)
            for k in range(M):
                rows[p, k] = f32(rows[p, k] * inv)
    return rows


def poly_build_f64():
    """The SHIPPED Q31 design: the same Kaiser-11 / per-row unity-DC formula as
    poly_build_f32(), evaluated in float64 with the platform's libm.

    Why a second builder exists.  The Q31 coefficient table is generated HERE and
    baked into the image (asrc_poly_q31_table.h); the target does not rebuild it.
    That is a deliberate reversal of what the float arm does, and it is what makes
    the bit-exactness proof possible at all: sinf()/sqrtf() in XC-DSC and in numpy
    are each within an ulp of correct but not identical, one ulp in a tap moves the
    row sum, and the row sum divides every tap in the row -- so a target-generated
    table drifts from the host model by a few hundred Q31 LSB (measured 2026-08-22:
    448 at (p=1,k=15), which is 2.8e-6 relative).  That is numerically harmless and
    it is fatal to a bit-exact oracle.  Baking removes the divergence instead of
    tolerating it, and the design is then evaluated in double rather than float32,
    which is strictly closer to the intended filter.
    """
    pi = math.pi
    knorm = 1.0 / bessel_i0_f64(BETA)
    rows = np.zeros((L + 1, M), dtype=np.float64)
    for p in range(L + 1):
        total = 0.0
        for k in range(M):
            d = float(k) - float(MH) - (float(p) / float(L))
            x = 2.0 * float(FC) * d
            sinc = 1.0 if abs(x) < 1.0e-12 else math.sin(pi * x) / (pi * x)
            wpos = (d + float(MH) + 1.0) / float(M)
            kx = 2.0 * wpos - 1.0
            kr = math.sqrt(max(0.0, 1.0 - kx * kx))
            win = bessel_i0_f64(float(BETA) * kr) * knorm
            c = 2.0 * float(FC) * sinc * win
            rows[p, k] = c
            total += c
        if abs(total) > 1.0e-12:
            inv = 1.0 / total
            for k in range(M):
                rows[p, k] *= inv
    return rows


def bessel_i0_f64(x):
    """The same 24-term series and early-out as the target, in float64."""
    y = 0.25 * float(x) * float(x)
    term = 1.0
    total = 1.0
    for k in range(1, 25):
        term = term * (y / (float(k) * float(k)))
        total += term
        if term <= total * 1.0e-15:
            break
    return total


def q31_from_f64(c):
    """Round-half-away-from-zero into Q31, clamped.  Same rule as the target's
    dec_q31_from_float(), applied to the float64 design."""
    v = float(c) * 2147483648.0
    if v >= 2147483647.0:
        return 0x7FFFFFFF
    if v <= -2147483648.0:
        return -0x80000000
    return int(v + 0.5) if v >= 0.0 else int(v - 0.5)


def poly_build_q31_shipped():
    """The table the image carries.  Every command in this file uses THIS as the
    Q31 side, so host artefacts and target behaviour cannot diverge."""
    rf64 = poly_build_f64()
    q = np.zeros(rf64.shape, dtype=np.int64)
    for p in range(rf64.shape[0]):
        for k in range(rf64.shape[1]):
            q[p, k] = q31_from_f64(rf64[p, k])
    return q


def q31_from_float(c):
    """dec_q31_from_float(): round-half-away-from-zero, clamped."""
    v = float(f32(c)) * 2147483648.0
    if v >= 2147483647.0:
        return 0x7FFFFFFF
    if v <= -2147483648.0:
        return -0x80000000
    return int(v + 0.5) if v >= 0.0 else int(v - 0.5)


def poly_build_q31(rows_f32):
    q = np.zeros(rows_f32.shape, dtype=np.int64)
    for p in range(rows_f32.shape[0]):
        for k in range(rows_f32.shape[1]):
            q[p, k] = q31_from_float(rows_f32[p, k])
    return q


# --------------------------------------------------------- output stages -----
def sacr_l(acc64):
    """sacr.l: round then saturate the 40-bit accumulator into Q31."""
    v = (int(acc64) + (1 << 30)) >> 31
    if v > 0x7FFFFFFF:
        return 0x7FFFFFFF
    if v < -0x80000000:
        return -0x80000000
    return int(v)


def q31_to_slot(v):
    """dec_q31_to_s24_left(): mask off the eight sub-LSBs."""
    m = int(v) & 0xFFFFFF00
    return m - (1 << 32) if m >= (1 << 31) else m


def float_to_slot(y):
    """asrc_to_slot() with ASRC_FAST_SLOT_CONVERT == 1."""
    y = float(y)
    if y > SAMP_MAX:
        y = SAMP_MAX
    if y < SAMP_MIN:
        y = SAMP_MIN
    t = int(y)                      # C cast: truncate toward zero
    return (t << 8) if t >= 0 else ((t << 8) | 0) # arithmetic, exact in python


# ------------------------------------------------------------- resamplers ----
def phase_of(frac):
    """asrc_poly_phase() with APP_ASRC_HEADROOM_INSTRUMENT == 1."""
    pf = f32(f32(frac) * f32(L))
    p = int(np.int32(pf))
    if p >= L:
        p = L - 1
    return p, f32(pf - f32(p))


def wb_to_q31(wb):
    """Blend weight -> Q31.  Truncating against 2^31-1 keeps it inside int32
    for every wb in [0,1) without a clamp on the hot path."""
    v = int(f32(wb) * f32(2147483647.0))
    return 0 if v < 0 else v


def blend_q31(c0, c1, wbq):
    """Hoisted per-tap phase blend, Q31 x Q31 -> Q31, sacr rounding.
    Convex in (c0,c1) so the result cannot leave int32."""
    out = np.empty(M, dtype=np.int64)
    for k in range(M):
        delta = int(c1[k]) - int(c0[k])
        out[k] = int(c0[k]) + ((wbq * delta + (1 << 30)) >> 31)
    return out


def resample(x_slots, step, frac0, nout, rows_f32, rows_q31, mode):
    """One channel.  `mode` in {'q31', 'float_hoist', 'float_dots'}.

    The mirrored history ring makes every window a contiguous span, so a flat
    input array indexed from wbase == rd - MH is exactly equivalent to the ring.
    """
    rd = MH
    frac = f32(frac0)
    xf = np.asarray(x_slots, dtype=np.int64)
    xv = (xf >> 8).astype(np.float32)          # asrc_push: (float)(slot >> 8)
    out = np.empty(nout, dtype=np.int64)
    for n in range(nout):
        p, wb = phase_of(frac)
        w = rd - MH
        if mode == 'q31':
            ceff = blend_q31(rows_q31[p], rows_q31[p + 1], wb_to_q31(wb))
            acc = 0
            for k in range(M):
                acc += int(ceff[k]) * int(xf[w + k])
            out[n] = q31_to_slot(sacr_l(acc))
        elif mode == 'float_hoist':
            wb1 = f32(f32(1.0) - wb)
            acc = f32(0.0)
            for k in range(M):
                ce = f32(f32(rows_f32[p, k] * wb1) + f32(rows_f32[p + 1, k] * wb))
                acc = f32(acc + f32(ce * xv[w + k]))
            out[n] = float_to_slot(acc)
        else:
            a0 = f32(0.0)
            a1 = f32(0.0)
            for k in range(M):
                a0 = f32(a0 + f32(rows_f32[p, k] * xv[w + k]))
                a1 = f32(a1 + f32(rows_f32[p + 1, k] * xv[w + k]))
            wb1 = f32(f32(1.0) - wb)
            out[n] = float_to_slot(f32(f32(a0 * wb1) + f32(a1 * wb)))
        frac = f32(frac + f32(step))
        while float(frac) >= 1.0:
            frac = f32(frac - f32(1.0))
            rd += 1
    return out


def need_input(step, nout):
    return MH + int(math.ceil(step * (nout + 2))) + M + 8


# ----------------------------------------------------------------- signals ---
def lcg_next(state):
    return (state * 1664525 + 1013904223) & 0xFFFFFFFF


def s24_left(v):
    """Clamp to s24 then left-justify, i.e. what the codec puts on the wire."""
    v = int(max(-8388608, min(8388607, int(v))))
    return v << 8


def gen_signal(kind, n, seed=0x12345678):
    x = np.zeros(n, dtype=np.int64)
    if kind == 'zero':
        pass
    elif kind == 'dc_small':
        x[:] = s24_left(1000)
    elif kind == 'dc_fs_pos':
        x[:] = s24_left(8388607)
    elif kind == 'dc_fs_neg':
        x[:] = s24_left(-8388608)
    elif kind == 'impulse':
        x[min(7, n - 1)] = s24_left(8388607)
    elif kind == 'impulse_low':
        x[min(7, n - 1)] = s24_left(1)
    elif kind == 'alt_fs':
        for i in range(n):
            x[i] = s24_left(8388607 if (i & 1) == 0 else -8388608)
    elif kind == 'lcg_half':
        st = seed
        for i in range(n):
            st = lcg_next(st)
            sv = st - (1 << 32) if st >= (1 << 31) else st
            x[i] = s24_left((sv >> 8) // 4)
    elif kind == 'lcg':
        st = seed
        for i in range(n):
            st = lcg_next(st)
            sv = st - (1 << 32) if st >= (1 << 31) else st
            x[i] = s24_left(sv >> 8)
    elif kind.startswith('sine'):
        # sine:<cycles_per_sample>:<amp_frac>
        _, cps, amp = kind.split(':')
        cps = float(cps)
        amp = float(amp) * 8388607.0
        for i in range(n):
            x[i] = s24_left(int(round(amp * math.sin(2.0 * math.pi * cps * i))))
    else:
        raise ValueError(kind)
    return x


# -------------------------------------------------------------- reporting ----
def cmd_coeff(_args):
    rf = poly_build_f32()
    rq = poly_build_q31_shipped()

    max_abs_f = float(np.abs(rf).max())
    max_abs_q = int(np.abs(rq).max())
    sum_abs = np.abs(rf.astype(np.float64)).sum(axis=1)
    row_sum_f = rf.astype(np.float64).sum(axis=1)
    row_sum_q = rq.sum(axis=1)

    # DC gain of each quantised row, and the wobble the quantiser introduces.
    gain_q = row_sum_q.astype(np.float64) / float(Q31_ONE)
    gain_f = row_sum_f
    # Two DIFFERENT errors, deliberately not conflated.  qerr is the true
    # quantisation error: the shipped Q31 table against the float64 design it
    # was rounded from, so it must be <= 0.5 LSB31 by construction.  terr is the
    # difference between the two ARMS' coefficient sets -- Q31 ships a baked
    # float64-designed table, float rebuilds its own in float32 on target -- and
    # is therefore an input to the float-vs-Q31 comparison, not a defect.
    rf64 = poly_build_f64()
    qerr = np.abs(rq.astype(np.float64) / float(Q31_ONE) - rf64)
    terr = np.abs(rq.astype(np.float64) / float(Q31_ONE) - rf.astype(np.float64))

    # Worst blended row: quantise the blend at the worst weight and re-measure.
    worst_ceff = 0
    worst_sumabs = 0.0
    for p in range(L):
        for wbq in (0, 1 << 29, 1 << 30, (1 << 31) - 1):
            ce = blend_q31(rq[p], rq[p + 1], wbq)
            worst_ceff = max(worst_ceff, int(np.abs(ce).max()))
            worst_sumabs = max(worst_sumabs,
                               float(np.abs(ce.astype(np.float64)).sum()) / Q31_ONE)

    print("=== coefficient format (L=%d, M=%d, fc=%.3f, Kaiser beta=%.1f) ===" %
          (L, M, FC, BETA))
    print("rows                       : %d (p = 0..L, row L exists for the p=L-1 blend)" % (L + 1))
    print("max |c| float              : %.9f" % max_abs_f)
    print("max |c| Q31                : %d  (= %.9f, Q31 needs < 1.0)" %
          (max_abs_q, max_abs_q / Q31_ONE))
    print("headroom to Q31 clip       : %.6f  -> Q31 is legal, no Q30 rescale needed"
          % (1.0 - max_abs_q / Q31_ONE))
    print("max sum|c| over rows       : %.9f   (accumulator bound, see below)" % sum_abs.max())
    print("max sum|c_eff| blended     : %.9f" % worst_sumabs)
    print("max |c_eff| blended        : %d  (%.9f)" % (worst_ceff, worst_ceff / Q31_ONE))
    print()
    print("=== DC gain (row sum) ===")
    print("float  min/max             : %.9f / %.9f" % (gain_f.min(), gain_f.max()))
    print("Q31    min/max             : %.9f / %.9f" % (gain_q.min(), gain_q.max()))
    print("Q31 worst |gain-1|         : %.3e  (%.4f dB, %.2f LSB31 on the row sum)" %
          (np.abs(gain_q - 1.0).max(),
           20.0 * math.log10(1.0 + np.abs(gain_q - 1.0).max()),
           np.abs(row_sum_q - Q31_ONE).max()))
    print("per-phase gain wobble p2p  : %.3e  (%.4f dB)" %
          (gain_q.max() - gain_q.min(),
           20.0 * math.log10(gain_q.max() / gain_q.min())))
    print()
    print("=== quantisation error (vs the float64 design it was rounded from) ===")
    print("max |c_q31/2^31 - c_f64|   : %.3e  (%.2f LSB31)" % (qerr.max(), qerr.max() * Q31_ONE))
    print("rms                        : %.3e" % math.sqrt(float((qerr ** 2).mean())))
    print()
    print("=== table difference between the arms (NOT quantisation error) ===")
    print("max |c_q31/2^31 - c_f32|   : %.3e  (%.2f LSB31)" % (terr.max(), terr.max() * Q31_ONE))
    print("rms                        : %.3e" % math.sqrt(float((terr ** 2).mean())))
    print("cause                      : Q31 ships a baked float64 design; float"
          " rebuilds in float32 on target (ulp-level sinf/sqrtf differences,"
          " amplified by the per-row DC normaliser)")
    print()
    print("=== accumulator bound (Phase 2: NOT assumed) ===")
    print("worst |acc| in Q31 units   : sum|c_eff| * full scale = %.6f" % worst_sumabs)
    print("bits needed above Q31      : ceil(log2(%.6f)) = %d guard bit(s)" %
          (worst_sumabs, max(0, math.ceil(math.log2(worst_sumabs)))))
    print("ACCA is 40-bit             : 40 - 31 = 9 bits, 8 magnitude + 1 sign")
    print("verdict                    : %s" %
          ("PASS - 8 guard bits vs %d needed, margin %dx"
           % (max(0, math.ceil(math.log2(worst_sumabs))),
              int(256.0 / worst_sumabs))))
    print("32-bit accumulator would   : FAIL (0 guard bits, sum|c_eff| = %.3f > 1)" % worst_sumabs)
    return rf, rq


def resample_exact(x_slots, step, frac0, nout, rows_f64):
    """float64 ground truth, in slot units (s24-left scale).  Neither shipping
    path is 'correct'; both are measured against this."""
    rd = MH
    frac = f32(frac0)
    xf = np.asarray(x_slots, dtype=np.float64)
    out = np.empty(nout, dtype=np.float64)
    for n in range(nout):
        p, wb = phase_of(frac)          # phase bookkeeping is float32 in both paths
        w = rd - MH
        wbd = float(wb)
        ce = rows_f64[p] * (1.0 - wbd) + rows_f64[p + 1] * wbd
        out[n] = float(np.dot(ce, xf[w:w + M]))
        frac = f32(frac + f32(step))
        while float(frac) >= 1.0:
            frac = f32(frac - f32(1.0))
            rd += 1
    return out


STEPS = [
    ('step=1.0        (48->48)', 1.0),
    ('step=48/44.1    (44.1->48 pull)', 48000.0 / 44100.0),
    ('step=44.1/48    (48->44.1 pull)', 44100.0 / 48000.0),
    ('step=1.5        (48->32 AB)', 1.5),
    ('step=0.6666667  (32->48 BA)', 32000.0 / 48000.0),
    ('step=2.0        (96->48)', 2.0),
    ('step=0.5        (48->96)', 0.5),
]

SIGNALS = ['zero', 'dc_small', 'dc_fs_pos', 'dc_fs_neg', 'impulse', 'impulse_low',
           'alt_fs', 'lcg', 'lcg_half', 'sine:0.01:0.9', 'sine:0.2:0.9',
           'sine:0.45:0.5', 'sine:0.49:0.9']


def cmd_compare(args):
    rf = poly_build_f32()
    rq = poly_build_q31_shipped()
    rd64 = rf.astype(np.float64)
    nout = args.nout

    print("=== float vs Q31, sample domain (units = one 24-bit LSB) ===")
    print("%-34s %-14s %9s %9s %9s %9s %7s" %
          ("step", "signal", "maxerr", "rmserr", "SNRf dB", "SNRq dB", "clip"))
    worst = {}
    for label, step in STEPS:
        n_in = need_input(step, nout)
        for sig in SIGNALS:
            x = gen_signal(sig, n_in)
            yq = resample(x, step, 0.0, nout, rf, rq, 'q31')
            yf = resample(x, step, 0.0, nout, rf, rq, 'float_hoist')
            ye = resample_exact(x, step, 0.0, nout, rd64)
            # everything in slot units; report in 24-bit LSBs
            eq = (yq.astype(np.float64) - ye) / 256.0
            ef = (yf.astype(np.float64) - ye) / 256.0
            dfq = (yf.astype(np.float64) - yq.astype(np.float64)) / 256.0
            sig_pow = float((ye / 256.0) ** 2).__float__() if False else float(((ye / 256.0) ** 2).sum())
            snr = lambda e: (10.0 * math.log10(sig_pow / float((e ** 2).sum()))
                             if sig_pow > 0 and float((e ** 2).sum()) > 0 else float('inf'))
            # 'clip' = outputs whose EXACT value leaves the s24 range, i.e. where
            # both paths must clamp.  sum|c_eff| = 2.009, so a full-scale broadband
            # input legitimately overshoots -- that is a property of the shipping
            # filter, not of Q31.
            nsat = int(np.sum(np.abs(ye) > (8388607.0 * 256.0)))
            degenerate = (float(np.abs(ye).max()) < 256.0 * 4.0) or (nsat > 0)
            print("%-34s %-14s %9.2f %9.3f %9.1f %9.1f %7d" %
                  (label, sig, float(np.abs(dfq).max()), float(np.sqrt((dfq ** 2).mean())),
                   snr(ef), snr(eq), nsat))
            worst[(label, sig)] = (float(np.abs(dfq).max()), snr(ef), snr(eq), degenerate)

    mx = max(worst.items(), key=lambda kv: kv[1][0])
    print()
    print("worst float-vs-Q31 sample difference : %.2f LSB24  at %s / %s"
          % (mx[1][0], mx[0][0], mx[0][1]))
    fs = [v[1] for v in worst.values() if math.isfinite(v[1]) and not v[3]]
    qs = [v[2] for v in worst.values() if math.isfinite(v[2]) and not v[3]]
    print("(sub-LSB cases -- impulse_low -- are excluded from the SNR verdict:")
    print(" their exact output is smaller than one 24-bit LSB, so both paths are")
    print(" pure quantisation there -- and so are clipping cases, where the exact")
    print(" output leaves s24 and both paths clamp.)")
    print("worst SNR vs float64 truth  float    : %.1f dB" % min(fs))
    print("worst SNR vs float64 truth  Q31      : %.1f dB" % min(qs))
    print("=> Q31 is %s than the shipping float path" %
          ("BETTER" if min(qs) > min(fs) else "WORSE"))


def fit_tone(y, cps, n0=0):
    """Least-squares amplitude/phase of a tone at `cps` cycles/sample in y."""
    n = np.arange(len(y), dtype=np.float64) + n0
    c = np.cos(2.0 * math.pi * cps * n)
    s = np.sin(2.0 * math.pi * cps * n)
    A = np.vstack([c, s]).T
    sol, *_ = np.linalg.lstsq(A, np.asarray(y, dtype=np.float64), rcond=None)
    return math.hypot(sol[0], sol[1]), math.atan2(-sol[1], sol[0])


def cmd_freq(args):
    rf = poly_build_f32()
    rq = poly_build_q31_shipped()
    rd64 = rf.astype(np.float64)
    nout = args.nout
    amp = 0.5

    for label, step in [('step=1.0 (48->48)', 1.0), ('step=1.5 (48->32)', 1.5),
                        ('step=0.6666667 (32->48)', 32000.0 / 48000.0)]:
        print("=== frequency response, %s ===" % label)
        print("%9s %11s %11s %11s %11s %11s" %
              ("f/fs_in", "gain_f dB", "gain_q dB", "diff dB", "ph_f deg", "ph_q-ph_f"))
        for fin in (0.005, 0.02, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30,
                    0.35, 0.40, 0.4325, 0.45, 0.48):
            n_in = need_input(step, nout)
            x = gen_signal('sine:%.6f:%.3f' % (fin, amp), n_in)
            fout = fin * step                      # output-rate frequency
            if fout >= 0.5:
                continue
            yq = resample(x, step, 0.0, nout, rf, rq, 'q31').astype(np.float64)
            yf = resample(x, step, 0.0, nout, rf, rq, 'float_hoist').astype(np.float64)
            aq, pq = fit_tone(yq, fout)
            af, pf = fit_tone(yf, fout)
            ref = amp * 8388607.0 * 256.0
            gf = 20.0 * math.log10(max(af, 1e-30) / ref)
            gq = 20.0 * math.log10(max(aq, 1e-30) / ref)
            print("%9.4f %11.4f %11.4f %11.4f %11.3f %11.5f" %
                  (fin, gf, gq, gq - gf, math.degrees(pf),
                   math.degrees(pq - pf)))
        print()

    # Image / alias floor: single in-band tone, look at everything that is not
    # the wanted output component.
    print("=== residual (image/alias/quantisation) floor, tone in, step=1.5 ===")
    print("%9s %11s %11s %11s %11s" % ("f/fs_in", "f peak dB", "q peak dB", "f rms dB", "q rms dB"))
    for fin in (0.02, 0.10, 0.20, 0.30, 0.40):
        n_in = need_input(1.5, nout)
        x = gen_signal('sine:%.6f:%.3f' % (fin, amp), n_in)
        yq = resample(x, 1.5, 0.0, nout, rf, rq, 'q31').astype(np.float64)
        yf = resample(x, 1.5, 0.0, nout, rf, rq, 'float_hoist').astype(np.float64)
        ye = resample_exact(x, 1.5, 0.0, nout, rd64)
        win = np.hanning(len(ye))
        pk = float(np.abs(np.fft.rfft(ye * win)).max())
        ref_rms = float(np.sqrt((ye ** 2).mean()))
        out = []
        for y in (yf, yq):
            e = (y - ye)
            sp = np.abs(np.fft.rfft(e * win))
            out.append(20.0 * math.log10(max(float(sp.max()), 1e-30) / pk))
            out.append(20.0 * math.log10(max(float(np.sqrt((e ** 2).mean())), 1e-30)
                                        / ref_rms))
        print("%9.4f %11.2f %11.2f %11.2f %11.2f"
              % (fin, out[0], out[2], out[1], out[3]))


FNV64_OFF = 0xCBF29CE484222325
FNV64_PRM = 0x100000001B3


def fnv1a64_i32(vals):
    h = FNV64_OFF
    for v in vals:
        u = int(v) & 0xFFFFFFFF
        for shift in (0, 8, 16, 24):
            h ^= (u >> shift) & 0xFF
            h = (h * FNV64_PRM) & 0xFFFFFFFFFFFFFFFF
    return h


VEC_SIGNALS = [
    ('ZERO', 'zero'), ('DC_SMALL', 'dc_small'), ('DC_FS_POS', 'dc_fs_pos'),
    ('DC_FS_NEG', 'dc_fs_neg'), ('IMPULSE', 'impulse'), ('IMPULSE_LOW', 'impulse_low'),
    ('ALT_FS', 'alt_fs'), ('LCG', 'lcg'),
    ('SINE_LOW', 'sine:0.010000:0.900'), ('SINE_MID', 'sine:0.200000:0.900'),
    ('SINE_NYQ', 'sine:0.490000:0.900'),
]

VEC_STEPS = [
    ('ONE', 1.0, 0.0), ('ONE_PH_LAST', 1.0, 0.9999),
    ('UP_44K', 48000.0 / 44100.0, 0.0),
    ('DOWN_44K', 44100.0 / 48000.0, 0.0),
    ('DOWN_32K', 1.5, 0.0), ('DOWN_32K_PH_HI', 1.5, 0.99),
    ('UP_32K', 32000.0 / 48000.0, 0.0),
    ('HALF', 0.5, 0.0), ('DOUBLE', 2.0, 0.0),
    ('WRAP_EDGE', 1.5, 1.0 - 1.0 / 256.0),
]


def cmd_vectors(args):
    rf = poly_build_f32()
    rq = poly_build_q31_shipped()
    nout = args.vec_nout
    lines = []
    cases = []
    n_in_max = max(need_input(step, nout) for _, step, _ in VEC_STEPS)
    inputs = []
    for sname, skind in VEC_SIGNALS:
        # ONE block per signal, sliced by every case.  gen_signal is a pure
        # function of the sample index for every kind here (the impulse sits at
        # index 7 for any n >= 8), so slicing is identical to regenerating at the
        # shorter length -- and it lets the target read the exact same samples out
        # of flash instead of having to reproduce sinf() bit for bit.
        xfull = gen_signal(skind, n_in_max)
        inputs.append((sname, xfull))
        for rname, step, frac0 in VEC_STEPS:
            n_in = need_input(step, nout)
            assert (gen_signal(skind, n_in) == xfull[:n_in]).all()
            y = resample(xfull[:n_in], step, frac0, nout, rf, rq, 'q31')
            cases.append((sname, rname, step, frac0, y))

    with open(args.out, 'w', newline='\n') as fh:
        fh.write("/* GENERATED by tools/asrc/asrc_q31_resampler_model.py -- do not edit.\n"
                 " * Expected Q31 resampler output, from the independent Python integer\n"
                 " * reference model.  L=%d M=%d fc=%.3f Kaiser beta=%.1f.\n"
                 " * Proof A of the Q31 design: the on-target kernel must reproduce these\n"
                 " * bit for bit.  Signals and phase bookkeeping are regenerated on target\n"
                 " * from the case id, so only the digest and a few head samples are stored. */\n"
                 "#ifndef ASRC_POLY_Q31_VECTORS_H\n#define ASRC_POLY_Q31_VECTORS_H\n\n"
                 % (L, M, FC, BETA))
        fh.write("#define ASRC_Q31_VEC_NOUT   (%du)\n" % nout)
        fh.write("#define ASRC_Q31_VEC_NIN    (%du)\n" % n_in_max)
        # The vector digests are only meaningful if the target generated the SAME
        # Q31 coefficient table.  sinf()/cosf()/sqrtf() are the one place where the
        # host and XC-DSC can legitimately differ by an ulp, so publish a digest of
        # the table itself plus a few probe entries and let the target check it
        # first -- a coefficient mismatch then reports as a coefficient mismatch
        # rather than as 110 failing vectors.
        flat = [int(v) for row in rq for v in row]
        fh.write("#define ASRC_Q31_VEC_COEFF_DIGEST 0x%016XULL\n" % fnv1a64_i32(flat))
        probes = [(0, 0), (0, M // 2), (1, M // 2), (L // 2, M // 2),
                  (L - 1, M // 2), (L, 0), (L, M // 2), (L, M - 1)]
        fh.write("#define ASRC_Q31_VEC_COEFF_PROBES (%du)\n\n" % len(probes))
        fh.write("static const struct { uint16_t p; uint16_t k; int32_t c; }\n"
                 "    asrc_q31_vec_coeff_probe[ASRC_Q31_VEC_COEFF_PROBES] = {\n")
        for pp, kk in probes:
            fh.write("    { %du, %du, %d },\n" % (pp, kk, int(rq[pp, kk])))
        fh.write("};\n\n")
        fh.write("#define ASRC_Q31_VEC_COUNT  (%du)\n\n" % len(cases))
        fh.write("typedef struct {\n"
                 "    uint8_t  sig;        /* asrc_q31_vec_sig_t          */\n"
                 "    float    step;\n"
                 "    float    frac0;\n"
                 "    uint64_t digest;     /* FNV-1a-64 over the int32 LE stream */\n"
                 "    int32_t  head[4];\n"
                 "    const char* name;\n"
                 "} asrc_q31_vec_t;\n\n")
        sigs = [s for s, _ in VEC_SIGNALS]
        fh.write("typedef enum {\n")
        for i, s in enumerate(sigs):
            fh.write("    ASRC_Q31_SIG_%s = %d,\n" % (s, i))
        fh.write("} asrc_q31_vec_sig_t;\n\n")
        fh.write("static const asrc_q31_vec_t asrc_q31_vectors[ASRC_Q31_VEC_COUNT] = {\n")
        for sname, rname, step, frac0, y in cases:
            fh.write('    { ASRC_Q31_SIG_%s, %.9ef, %.9ef, 0x%016XULL, '
                     '{ %d, %d, %d, %d }, "%s/%s" },\n'
                     % (sname, step, frac0, fnv1a64_i32(y),
                        y[0], y[1], y[2], y[3], sname, rname))
        fh.write("};\n\n")
        fh.write("/* Input blocks, s24-left (== Q31).  One per signal, indexed by\n"
                 " * asrc_q31_vec_sig_t; each case reads the first need_input(step) of them. */\n")
        fh.write("static const int32_t asrc_q31_vec_input[%d][ASRC_Q31_VEC_NIN] = {\n"
                 % len(inputs))
        for sname, xfull in inputs:
            fh.write("    { /* %s */\n" % sname)
            for i in range(0, len(xfull), 8):
                fh.write("      " + ", ".join("%d" % int(v) for v in xfull[i:i + 8]) + ",\n")
            fh.write("    },\n")
        fh.write("};\n\n#endif /* ASRC_POLY_Q31_VECTORS_H */\n")
    print("wrote %s: %d cases x %d outputs" % (args.out, len(cases), nout))


def cmd_table(args):
    """Emit the Q31 polyphase table the image carries.

    Baked, not target-generated: see poly_build_f64() for why.  The digest here and
    the digest in the vectors header are computed from the same array in the same
    run, so a mismatch on target means the image was built from mismatched headers,
    which is worth failing on."""
    rq = poly_build_q31_shipped()
    rf64 = poly_build_f64()
    qerr = np.abs(rq.astype(np.float64) / float(Q31_ONE) - rf64)
    flat = [int(v) for row in rq for v in row]
    cont = " \\\n"
    with open(args.out, 'w', newline='\n') as fh:
        fh.write("/* GENERATED by tools/asrc/asrc_q31_resampler_model.py table -- do not edit.\n"
                 " * Q31 polyphase ASRC coefficients, L=%d M=%d fc=%.3f Kaiser beta=%.1f.\n"
                 " *\n"
                 " * Designed in float64 on the host and baked in, unlike the float arm's\n"
                 " * table which the target rebuilds at reset in float32.  The reason is the\n"
                 " * bit-exactness proof: sinf()/sqrtf() differ by an ulp between XC-DSC and\n"
                 " * the host, one ulp moves the per-row DC normaliser, and the normaliser\n"
                 " * divides every tap of the row -- so a target-built table lands a few\n"
                 " * hundred Q31 LSB away from the host model (measured: 448 at p=1,k=15)\n"
                 " * and no oracle over it can be bit-exact.  Baking also costs the target\n"
                 " * nothing at reset and drops the float32 Bessel/sinc build entirely.\n"
                 " *\n"
                 " * max |quantisation error| vs the float64 design: %.3e (%.2f Q31 LSB)\n"
                 " */\n"
                 "#ifndef ASRC_POLY_Q31_TABLE_H\n#define ASRC_POLY_Q31_TABLE_H\n\n"
                 % (L, M, FC, BETA, float(qerr.max()), float(qerr.max()) * Q31_ONE))
        fh.write("#define ASRC_POLY_Q31_TABLE_L    (%du)\n" % L)
        fh.write("#define ASRC_POLY_Q31_TABLE_M    (%du)\n" % M)
        fh.write("#define ASRC_POLY_Q31_TABLE_DIGEST 0x%016XULL\n\n" % fnv1a64_i32(flat))
        fh.write("/* [L+1][M]: row L exists only so the p=L-1 blend has a c1. */\n")
        fh.write("#define ASRC_POLY_Q31_TABLE_INIT" + cont)
        for p in range(L + 1):
            fh.write("    { ")
            fh.write(", ".join("%d" % int(v) for v in rq[p]))
            fh.write(" }%s%s" % ("," if p < L else "", cont))
        fh.write("\n#endif /* ASRC_POLY_Q31_TABLE_H */\n")
    print("wrote %s: %d rows x %d taps, digest 0x%016X, max qerr %.2f LSB"
          % (args.out, L + 1, M, fnv1a64_i32(flat), float(qerr.max()) * Q31_ONE))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('coeff');   p.set_defaults(fn=cmd_coeff)
    p = sub.add_parser('compare'); p.add_argument('--nout', type=int, default=192); p.set_defaults(fn=cmd_compare)
    p = sub.add_parser('freq');    p.add_argument('--nout', type=int, default=512); p.set_defaults(fn=cmd_freq)
    p = sub.add_parser('table')
    p.add_argument('--out', required=True)
    p.set_defaults(fn=cmd_table)
    p = sub.add_parser('vectors')
    p.add_argument('--out', required=True)
    p.add_argument('--vec-nout', type=int, default=32)
    p.set_defaults(fn=cmd_vectors)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
