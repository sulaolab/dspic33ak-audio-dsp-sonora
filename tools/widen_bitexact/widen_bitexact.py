#!/usr/bin/env python3
"""
Bit-exactness check for the widen_ctrl.c load reduction (branch perf/widen-ctrl-load).

WHAT THIS PROVES, AND WHAT IT DOES NOT

The rewrite claims the output bit pattern is unchanged.  The claim rests on one
algebraic fact -- the internal Side/Mid signals are carried at twice the
textbook M/S amplitude, and the factor 2 is taken back out at the re-mix -- plus
the observation that scaling by an exact power of two commutes with
round-to-nearest.  This script is the numerical audit of that claim: it
re-implements BOTH the old and the new sample loops in IEEE-754 single
precision, operation by operation and in source order, and compares the raw
32-bit patterns of every output sample.

It also exercises the parts that are pure index arithmetic and therefore have no
algebra to argue about but plenty of room for an off-by-one: the non-wrapping
run segmentation of the circular delay buffer, and the 2x unrolled core.

Not covered here:
  * The target FPU's mac.s.  With -ffp-contract=fast the compiler may fuse
    a*b+c into a single-rounding multiply-accumulate.  Both the old and the new
    code use the SAME all-pass expression, textually, so both fuse identically;
    and the factor-2 argument holds for a fused mac as well
    (fma(-a, 2x, 2x1) == 2*fma(-a, x, x1) exactly).  A --fma pass is included
    that emulates single-rounding accumulation via float64 to spot-check this.
  * Overflow and subnormal corners.  Doubling costs one bit of exponent range,
    so sample magnitudes near 1e38 / 1e-38 can differ.  The --extreme pass
    reports how far into those ranges the two agree.

Run:  python widen_bitexact.py
"""

import numpy as np

f32 = np.float32


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def bits(a):
    """Raw 32-bit pattern of a float32 array, for exact comparison (NaN-safe)."""
    return np.asarray(a, dtype=np.float32).view(np.uint32)


def fma(a, b, c, fused):
    """a*b + c, either as two roundings (fused=False) or one (fused=True)."""
    if not fused:
        return f32(f32(a) * f32(b)) + f32(c)
    # float32*float32 is exact in float64; the sum is then rounded once to f32.
    return f32(np.float64(a) * np.float64(b) + np.float64(c))


def db_to_gain(db):
    return f32(10.0) ** f32(f32(db) * f32(0.05))


class Params:
    """One widen_t worth of coefficients, in the units the C code stores."""

    def __init__(self, out_gain_db, side_gain, side_hpf_hz, use_delay, delay_ms,
                 use_allpass, ap_a, sample_rate=48000, delay_len=480):
        self.out_gain = db_to_gain(out_gain_db)
        self.out_gain_half = f32(0.5) * self.out_gain
        self.side_gain = f32(side_gain)
        self.side_hpf_hz = f32(side_hpf_hz)
        self.use_delay = use_delay
        self.use_allpass = use_allpass
        self.ap_a = f32(ap_a)
        self.delay_len = delay_len
        self.delay_samp = (int(round((delay_ms * 1e-3) * sample_rate))
                           if use_delay else 0)
        if self.delay_samp >= delay_len:
            self.delay_samp = delay_len - 1
        if self.delay_samp < 0:
            self.delay_samp = 0
        if side_hpf_hz > 0.0:
            alpha = f32(np.exp(-2.0 * np.pi * side_hpf_hz / sample_rate))
            self.hpf_a = f32(min(max(float(alpha), 0.0), 0.9999))
        else:
            self.hpf_a = f32(0.0)


# ---------------------------------------------------------------------------
# OLD implementations (git main, commit 2d02359)
# ---------------------------------------------------------------------------

def old_fast(L, R, p, fused):
    """widen_process_fast_nohpf_delay_ap() as it was: 1x loop, index recompute."""
    half = f32(0.5)
    buf = np.zeros(p.delay_len, dtype=np.float32)
    w = 0
    ap_x1 = f32(0.0)
    ap_y1 = f32(0.0)
    oL = np.empty(len(L), dtype=np.float32)
    oR = np.empty(len(L), dtype=np.float32)

    for n in range(len(L)):
        Mid = half * (L[n] + R[n])
        Side = half * (L[n] - R[n])
        Side = Side * p.side_gain

        rd = w - p.delay_samp
        if rd < 0:
            rd += p.delay_len
        Side_d = buf[rd]

        buf[w] = Side
        w += 1
        if w >= p.delay_len:
            w = 0

        y = fma(-p.ap_a, Side_d, ap_x1, fused)
        y = fma(p.ap_a, ap_y1, y, fused)
        ap_x1 = Side_d
        ap_y1 = y

        oL[n] = (Mid + y) * p.out_gain
        oR[n] = (Mid - y) * p.out_gain

    return oL, oR


def old_generic(L, R, p, fused):
    """The generic widen_process() loop as it was."""
    half = f32(0.5)
    buf = np.zeros(p.delay_len, dtype=np.float32)
    w = 0
    hpf_z = f32(0.0)
    ap_x1 = f32(0.0)
    ap_y1 = f32(0.0)
    oL = np.empty(len(L), dtype=np.float32)
    oR = np.empty(len(L), dtype=np.float32)

    for n in range(len(L)):
        Mid = half * (L[n] + R[n])
        Side = half * (L[n] - R[n])

        if p.side_hpf_hz > f32(0.0):
            y_h = Side - hpf_z
            hpf_z = fma(p.hpf_a, y_h, hpf_z, fused)
            Side = y_h

        Side = Side * p.side_gain

        if p.use_delay:
            rd = w - p.delay_samp
            if rd < 0:
                rd += p.delay_len
            Sd = buf[rd]
            buf[w] = Side
            w += 1
            if w >= p.delay_len:
                w = 0
            Side = Sd

        if p.use_allpass:
            y = fma(-p.ap_a, Side, ap_x1, fused)
            y = fma(p.ap_a, ap_y1, y, fused)
            ap_x1 = Side
            ap_y1 = y
            Side = y

        oL[n] = (Mid + Side) * p.out_gain
        oR[n] = (Mid - Side) * p.out_gain

    return oL, oR


# ---------------------------------------------------------------------------
# NEW implementations (this branch)
# ---------------------------------------------------------------------------

def new_fast(L, R, p, fused, block):
    """
    widen_process_fast_nohpf_delay_ap() as rewritten: doubled M/S domain,
    advancing pointers, delay walked in non-wrapping runs, 2x unrolled core.
    Called per block of `block` samples, exactly as the audio path does.
    """
    buf = np.zeros(p.delay_len, dtype=np.float32)
    wr_state = 0
    ap_x1 = f32(0.0)
    ap_y1 = f32(0.0)
    oL = np.empty(len(L), dtype=np.float32)
    oR = np.empty(len(L), dtype=np.float32)
    can_unroll = p.delay_samp >= 2
    og = p.out_gain_half
    sg = p.side_gain
    a = p.ap_a

    for base in range(0, len(L), block):
        samples = min(block, len(L) - base)

        wr = wr_state
        rd = wr - p.delay_samp
        if rd < 0:
            rd += p.delay_len

        i = base            # index into the in/out planes
        left = samples

        while left > 0:
            run = left
            run = min(run, p.delay_len - wr)
            run = min(run, p.delay_len - rd)

            pr = rd         # p_rd
            pw = wr         # p_wr
            n = run

            if can_unroll:
                while n >= 2:
                    L0 = L[i]
                    R0 = R[i]
                    L1 = L[i + 1]
                    R1 = R[i + 1]

                    M0 = L0 + R0
                    M1 = L1 + R1
                    S0 = (L0 - R0) * sg
                    S1 = (L1 - R1) * sg

                    d0 = buf[pr]
                    d1 = buf[pr + 1]

                    buf[pw] = S0
                    buf[pw + 1] = S1

                    ya = fma(-a, d0, ap_x1, fused)
                    ya = fma(a, ap_y1, ya, fused)
                    yb = fma(-a, d1, d0, fused)
                    yb = fma(a, ya, yb, fused)

                    ap_x1 = d1
                    ap_y1 = yb

                    oL[i] = (M0 + ya) * og
                    oR[i] = (M0 - ya) * og
                    oL[i + 1] = (M1 + yb) * og
                    oR[i + 1] = (M1 - yb) * og

                    i += 2
                    pr += 2
                    pw += 2
                    n -= 2

            while n > 0:
                Lv = L[i]
                Rv = R[i]

                Mid = Lv + Rv
                Side = (Lv - Rv) * sg

                Side_d = buf[pr]
                buf[pw] = Side
                pr += 1
                pw += 1

                y = fma(-a, Side_d, ap_x1, fused)
                y = fma(a, ap_y1, y, fused)
                ap_x1 = Side_d
                ap_y1 = y

                oL[i] = (Mid + y) * og
                oR[i] = (Mid - y) * og

                i += 1
                n -= 1

            wr += run
            if wr >= p.delay_len:
                wr -= p.delay_len
            rd += run
            if rd >= p.delay_len:
                rd -= p.delay_len
            left -= run

        wr_state = wr

    return oL, oR


def new_generic(L, R, p, fused):
    """The generic widen_process() loop as rewritten (doubled M/S, og folded)."""
    buf = np.zeros(p.delay_len, dtype=np.float32)
    w = 0
    hpf_z = f32(0.0)
    ap_x1 = f32(0.0)
    ap_y1 = f32(0.0)
    oL = np.empty(len(L), dtype=np.float32)
    oR = np.empty(len(L), dtype=np.float32)
    og = p.out_gain_half

    for n in range(len(L)):
        Mid = L[n] + R[n]
        Side = L[n] - R[n]

        if p.side_hpf_hz > f32(0.0):
            y_h = Side - hpf_z
            hpf_z = fma(p.hpf_a, y_h, hpf_z, fused)
            Side = y_h

        Side = Side * p.side_gain

        if p.use_delay:
            rd = w - p.delay_samp
            if rd < 0:
                rd += p.delay_len
            Sd = buf[rd]
            buf[w] = Side
            w += 1
            if w >= p.delay_len:
                w = 0
            Side = Sd

        if p.use_allpass:
            y = fma(-p.ap_a, Side, ap_x1, fused)
            y = fma(p.ap_a, ap_y1, y, fused)
            ap_x1 = Side
            ap_y1 = y
            Side = y

        oL[n] = (Mid + Side) * og
        oR[n] = (Mid - Side) * og

    return oL, oR


# ---------------------------------------------------------------------------
# FUSED two-stage path (F6) -- NOT bit-exact, validated by error norm
# ---------------------------------------------------------------------------

def seq_2stage(L, R, p1, p2, fused, block):
    """
    The sequential reference: what app_widen_process() does today, i.e.
    widen_process(&g_widen1, in, out) followed by widen_process(&g_widen2, out,
    out).  Stage 2 sees stage 1's rounded float32 L/R.
    """
    l1, r1 = new_fast(L, R, p1, fused, block)
    return new_fast(l1, r1, p2, fused, block)


def fused_2stage(L, R, p1, p2, fused, block):
    """
    widen_process_fused_2stage(): Mid and Side pass from stage 1 to stage 2
    directly, so the re-mix to L/R and the re-split never happen.

        k_mid  = g1 * gh2      k_out = gh2      k_side = g1 * sg2

    Two roundings disappear versus seq_2stage() -- L1 and R1 are never formed,
    so their sum and difference are never rounded either.  Everything else,
    including the run segmentation and the 2x unrolled core, matches the C.
    """
    buf1 = np.zeros(p1.delay_len, dtype=np.float32)
    buf2 = np.zeros(p2.delay_len, dtype=np.float32)
    wr1_state = 0
    wr2_state = 0
    ap1_x1 = f32(0.0)
    ap1_y1 = f32(0.0)
    ap2_x1 = f32(0.0)
    ap2_y1 = f32(0.0)
    oL = np.empty(len(L), dtype=np.float32)
    oR = np.empty(len(L), dtype=np.float32)

    can_unroll = (p1.delay_samp >= 2) and (p2.delay_samp >= 2)

    sg1 = p1.side_gain
    a1 = p1.ap_a
    a2 = p2.ap_a
    k_mid = f32(p1.out_gain * p2.out_gain_half)
    k_out = p2.out_gain_half
    k_side = f32(p1.out_gain * p2.side_gain)

    for base in range(0, len(L), block):
        samples = min(block, len(L) - base)

        wr1 = wr1_state
        rd1 = wr1 - p1.delay_samp
        if rd1 < 0:
            rd1 += p1.delay_len
        wr2 = wr2_state
        rd2 = wr2 - p2.delay_samp
        if rd2 < 0:
            rd2 += p2.delay_len

        i = base
        left = samples

        while left > 0:
            run = left
            run = min(run, p1.delay_len - wr1)
            run = min(run, p1.delay_len - rd1)
            run = min(run, p2.delay_len - wr2)
            run = min(run, p2.delay_len - rd2)

            pr1, pw1 = rd1, wr1
            pr2, pw2 = rd2, wr2
            n = run

            if can_unroll:
                while n >= 2:
                    L0 = L[i]
                    R0 = R[i]
                    L1 = L[i + 1]
                    R1 = R[i + 1]

                    M0 = L0 + R0
                    M1 = L1 + R1

                    S0 = (L0 - R0) * sg1
                    S1 = (L1 - R1) * sg1

                    e0 = buf1[pr1]
                    e1 = buf1[pr1 + 1]
                    buf1[pw1] = S0
                    buf1[pw1 + 1] = S1

                    y1a = fma(-a1, e0, ap1_x1, fused)
                    y1a = fma(a1, ap1_y1, y1a, fused)
                    y1b = fma(-a1, e1, e0, fused)
                    y1b = fma(a1, y1a, y1b, fused)

                    ap1_x1 = e1
                    ap1_y1 = y1b

                    T0 = y1a * k_side
                    T1 = y1b * k_side

                    f0 = buf2[pr2]
                    f1 = buf2[pr2 + 1]
                    buf2[pw2] = T0
                    buf2[pw2 + 1] = T1

                    y2a = fma(-a2, f0, ap2_x1, fused)
                    y2a = fma(a2, ap2_y1, y2a, fused)
                    y2b = fma(-a2, f1, f0, fused)
                    y2b = fma(a2, y2a, y2b, fused)

                    ap2_x1 = f1
                    ap2_y1 = y2b

                    m0 = M0 * k_mid
                    m1 = M1 * k_mid
                    o0 = y2a * k_out
                    o1 = y2b * k_out

                    oL[i] = m0 + o0
                    oR[i] = m0 - o0
                    oL[i + 1] = m1 + o1
                    oR[i + 1] = m1 - o1

                    i += 2
                    pr1 += 2
                    pw1 += 2
                    pr2 += 2
                    pw2 += 2
                    n -= 2

            while n > 0:
                Lv = L[i]
                Rv = R[i]

                Mid = Lv + Rv
                S1_in = (Lv - Rv) * sg1

                e = buf1[pr1]
                buf1[pw1] = S1_in
                pr1 += 1
                pw1 += 1

                y1 = fma(-a1, e, ap1_x1, fused)
                y1 = fma(a1, ap1_y1, y1, fused)
                ap1_x1 = e
                ap1_y1 = y1

                S2_in = y1 * k_side

                fv = buf2[pr2]
                buf2[pw2] = S2_in
                pr2 += 1
                pw2 += 1

                y2 = fma(-a2, fv, ap2_x1, fused)
                y2 = fma(a2, ap2_y1, y2, fused)
                ap2_x1 = fv
                ap2_y1 = y2

                m = Mid * k_mid
                o = y2 * k_out

                oL[i] = m + o
                oR[i] = m - o

                i += 1
                n -= 1

            wr1 += run
            if wr1 >= p1.delay_len:
                wr1 -= p1.delay_len
            rd1 += run
            if rd1 >= p1.delay_len:
                rd1 -= p1.delay_len
            wr2 += run
            if wr2 >= p2.delay_len:
                wr2 -= p2.delay_len
            rd2 += run
            if rd2 >= p2.delay_len:
                rd2 -= p2.delay_len
            left -= run

        wr1_state = wr1
        wr2_state = wr2

    return oL, oR


# ---------------------------------------------------------------------------
# test driver
# ---------------------------------------------------------------------------

def compare(name, a_old, a_new):
    (oL, oR) = a_old
    (nL, nR) = a_new
    dl = bits(oL) != bits(nL)
    dr = bits(oR) != bits(nR)
    nbad = int(dl.sum() + dr.sum())
    total = len(oL) * 2
    if nbad == 0:
        print(f"  PASS  {name:52s}  {total} samples bit-identical")
        return True

    i = int(np.argmax(dl | dr))
    print(f"  FAIL  {name:52s}  {nbad}/{total} differ, first at n={i}")
    print(f"        old L {oL[i]!r:>16} {bits(oL)[i]:#010x}   "
          f"new L {nL[i]!r:>16} {bits(nL)[i]:#010x}")
    print(f"        old R {oR[i]!r:>16} {bits(oR)[i]:#010x}   "
          f"new R {nR[i]!r:>16} {bits(nR)[i]:#010x}")
    return False


def compare_norm(name, a_ref, a_got, limit_rms_db, limit_peak_db):
    """
    Error-norm comparison, for the fused two-stage path where bit equality is
    not available.  Both metrics are relative to the SIGNAL SCALE, not to the
    individual sample:

      rms   20*log10(err_rms / ref_rms)
      peak  20*log10(max|err| / max|ref|)

    Deliberately NOT "ULP of the reference sample".  The output is a cancelling
    difference (m - o with m ~= o), so wherever cancellation is deepest the
    reference sample is near zero and its ULP is vanishing -- a perfectly
    ordinary last-bit error there reads as thousands of ULP and says nothing
    about audibility.  Peak-relative error is the honest absolute bound.
    """
    ref = np.concatenate([np.asarray(a_ref[0], dtype=np.float64),
                          np.asarray(a_ref[1], dtype=np.float64)])
    got = np.concatenate([np.asarray(a_got[0], dtype=np.float64),
                          np.asarray(a_got[1], dtype=np.float64)])
    err = got - ref

    def rel_db(num, den):
        return 20.0 * np.log10(num / den) if (num > 0.0 and den > 0.0) else -np.inf

    ref_rms = float(np.sqrt(np.mean(ref * ref)))
    err_rms = float(np.sqrt(np.mean(err * err)))
    ref_peak = float(np.max(np.abs(ref)))
    err_peak = float(np.max(np.abs(err)))

    d_rms = rel_db(err_rms, ref_rms)
    d_peak = rel_db(err_peak, ref_peak)

    good = (d_rms <= limit_rms_db) and (d_peak <= limit_peak_db)
    tag = "PASS" if good else "FAIL"
    print(f"  {tag}  {name:52s}  rms {d_rms:7.1f} dB   peak {d_peak:7.1f} dB")
    if not good:
        i = int(np.argmax(np.abs(err)))
        print(f"        limits rms {limit_rms_db:.1f} / peak {limit_peak_db:.1f} dB; "
              f"worst at n={i}: ref {ref[i]!r} got {got[i]!r}")
    return good


def make_signal(n, kind, rng):
    if kind == "uniform":
        return (rng.uniform(-1.0, 1.0, n).astype(np.float32),
                rng.uniform(-1.0, 1.0, n).astype(np.float32))
    if kind == "correlated":       # near-mono: Side is tiny, worst case for cancellation
        m = rng.uniform(-1.0, 1.0, n).astype(np.float32)
        d = (rng.uniform(-1.0, 1.0, n) * 1e-4).astype(np.float32)
        return (m + d).astype(np.float32), (m - d).astype(np.float32)
    if kind == "antiphase":        # pure Side: Mid is tiny
        s = rng.uniform(-1.0, 1.0, n).astype(np.float32)
        return s, (-s).astype(np.float32)
    if kind == "fullscale":
        return (rng.choice([-1.0, 1.0], n).astype(np.float32),
                rng.choice([-1.0, 1.0], n).astype(np.float32))
    if kind == "silence_bursts":
        a, b = make_signal(n, "uniform", rng)
        mask = rng.random(n) < 0.5
        a[mask] = 0.0
        b[mask] = 0.0
        return a, b
    if kind == "tiny":             # 1e-30: one doubling away from subnormal land
        a, b = make_signal(n, "uniform", rng)
        return (a * f32(1e-30)).astype(np.float32), (b * f32(1e-30)).astype(np.float32)
    if kind == "huge":             # 1e30: one doubling away from overflow
        a, b = make_signal(n, "uniform", rng)
        return (a * f32(1e30)).astype(np.float32), (b * f32(1e30)).astype(np.float32)
    raise ValueError(kind)


# The two stages the demo actually ships (app_widen_enable), plus configurations
# that force the paths the demo does not take.
STAGES = {
    "stage1 demo (-1.8dB sg1.8 d2.3ms ap0.70)":
        Params(-1.8, 1.8, 0.0, True, 2.3, True, 0.70),
    "stage2 demo (-3.0dB sg3.5 d7.7ms ap0.85)":
        Params(-3.0, 3.5, 0.0, True, 7.7, True, 0.85),
    "unity (0dB sg1.0 d1.0ms ap0.0)":
        Params(0.0, 1.0, 0.0, True, 1.0, True, 0.0),
    "ap at clamp (0dB sg1.0 d0.5ms ap0.98)":
        Params(0.0, 1.0, 0.0, True, 0.5, True, 0.98),
}

# Index-arithmetic edge cases: tiny buffers force several wraps per block, and
# delay_samp 0/1 force the 1x tail loop (no unrolling) and the read/write-alias
# boundary.
EDGE = {
    "len=5  d=0  (rd==wr, aliased, 1x path)":
        Params(-1.8, 1.8, 0.0, True, 0.0, True, 0.70, delay_len=5),
    "len=5  d=1  (1x path, adjacent rd/wr)":
        Params(-1.8, 1.8, 0.0, True, 1.0 / 48.0, True, 0.70, delay_len=5),
    "len=5  d=2  (unrolled, minimum legal distance)":
        Params(-1.8, 1.8, 0.0, True, 2.0 / 48.0, True, 0.70, delay_len=5),
    "len=7  d=3  (odd len, wrap mid-block)":
        Params(-3.0, 3.5, 0.0, True, 3.0 / 48.0, True, 0.85, delay_len=7),
    "len=33 d=32 (len just over block, d=len-1)":
        Params(-3.0, 3.5, 0.0, True, 32.0 / 48.0, True, 0.85, delay_len=33),
    "len=480 d=479 (max legal delay)":
        Params(-1.8, 1.8, 0.0, True, 479.0 / 48.0, True, 0.70, delay_len=480),
}

# Generic-path configurations (the fast path declines these).
GENERIC = {
    "hpf 200Hz + delay + allpass":
        Params(-1.8, 1.8, 200.0, True, 2.3, True, 0.70),
    "hpf 2kHz, no delay, allpass":
        Params(-3.0, 3.5, 2000.0, False, 0.0, True, 0.85),
    "no hpf, delay only":
        Params(-1.8, 1.8, 0.0, True, 2.3, False, 0.0),
    "no hpf, allpass only":
        Params(-1.8, 1.8, 0.0, False, 0.0, True, 0.70),
    "nothing on (old app_widen_disable state)":
        Params(0.0, 1.0, 0.0, False, 0.0, False, 0.0),
}

KINDS = ["uniform", "correlated", "antiphase", "fullscale", "silence_bursts"]
EXTREME_KINDS = ["tiny", "huge"]

BLOCK = 32          # APP_BLOCK_FRAMES for the classic profile
NSAMP = 32 * 64     # 64 blocks: every delay_len here wraps at least once

# --- fused two-stage (F6) ------------------------------------------------
#
# The pair the demo ships, plus pairs that force the index paths the demo does
# not take: a 1x-only stage (delay_samp < 2) on either side, unequal buffer
# lengths, and the aliased rd==wr case.
DEMO_PAIR = (STAGES["stage1 demo (-1.8dB sg1.8 d2.3ms ap0.70)"],
             STAGES["stage2 demo (-3.0dB sg3.5 d7.7ms ap0.85)"])

FUSION_PAIRS = {
    "demo pair (shipping)": DEMO_PAIR,
    "unity x unity": (STAGES["unity (0dB sg1.0 d1.0ms ap0.0)"],
                      STAGES["unity (0dB sg1.0 d1.0ms ap0.0)"]),
    "both at ap clamp 0.98": (STAGES["ap at clamp (0dB sg1.0 d0.5ms ap0.98)"],
                              STAGES["ap at clamp (0dB sg1.0 d0.5ms ap0.98)"]),
    "stage1 d=1 (1x path forced by stage 1)":
        (EDGE["len=5  d=1  (1x path, adjacent rd/wr)"], DEMO_PAIR[1]),
    "stage2 d=0 (rd==wr aliased on stage 2)":
        (DEMO_PAIR[0], EDGE["len=5  d=0  (rd==wr, aliased, 1x path)"]),
    "unequal lengths 7 x 33 (wraps at different points)":
        (EDGE["len=7  d=3  (odd len, wrap mid-block)"],
         EDGE["len=33 d=32 (len just over block, d=len-1)"]),
    "d=2 x d=2 (minimum legal unroll distance, both)":
        (EDGE["len=5  d=2  (unrolled, minimum legal distance)"],
         EDGE["len=5  d=2  (unrolled, minimum legal distance)"]),
}

# Locked at the measured worst case with margin (see the run recorded in
# [internal] widen_ctrl_load_reduction.md).  The fusion removes two
# roundings, so the residual is float32 last-bit noise; anything materially
# above these numbers means an algebra or index error, not float behaviour.
FUSION_LIMIT_RMS_DB = -125.0
FUSION_LIMIT_PEAK_DB = -95.0

# Long-run drift check: both all-passes have a <= 0.98 and the delay lines are
# FIR, so the error must stay bounded rather than accumulate.  Comparing the
# OUTPUT over a long run also covers the internal state, since every delay-line
# and all-pass state element reaches the output within delay_len samples.
FUSION_LONGRUN = 32 * 8192


def main():
    rng = np.random.default_rng(20260809)
    ok = True

    for fused in (False, True):
        tag = "fused mac (-ffp-contract=fast)" if fused else "two-rounding mul+add"
        print(f"\n=== all-pass / HPF accumulation: {tag} ===")

        print("\n-- fast path, shipping stage coefficients --")
        for pname, p in STAGES.items():
            for kind in KINDS:
                L, R = make_signal(NSAMP, kind, rng)
                ok &= compare(f"{pname[:28]} / {kind}",
                              old_fast(L, R, p, fused),
                              new_fast(L, R, p, fused, BLOCK))

        print("\n-- fast path, delay-index edge cases --")
        for pname, p in EDGE.items():
            L, R = make_signal(NSAMP, "uniform", rng)
            ok &= compare(f"{pname} / uniform",
                          old_fast(L, R, p, fused),
                          new_fast(L, R, p, fused, BLOCK))

        print("\n-- fast path, ragged block sizes (tail handling) --")
        p = STAGES["stage1 demo (-1.8dB sg1.8 d2.3ms ap0.70)"]
        for blk in (1, 2, 3, 5, 16, 31, 32, 33, 64):
            L, R = make_signal(BLOCK * 20, "uniform", rng)
            ok &= compare(f"block={blk} / uniform",
                          old_fast(L, R, p, fused),
                          new_fast(L, R, p, fused, blk))

        print("\n-- generic path --")
        for pname, p in GENERIC.items():
            for kind in ("uniform", "correlated"):
                L, R = make_signal(NSAMP, kind, rng)
                ok &= compare(f"{pname[:38]} / {kind}",
                              old_generic(L, R, p, fused),
                              new_generic(L, R, p, fused))

        print("\n-- exponent extremes (where doubling may legitimately differ) --")
        for kind in EXTREME_KINDS:
            for pname, p in list(STAGES.items())[:2]:
                L, R = make_signal(NSAMP, kind, rng)
                compare(f"{pname[:28]} / {kind}   [informational]",
                        old_fast(L, R, p, fused),
                        new_fast(L, R, p, fused, BLOCK))

        # ---- fused two-stage: error norm, not bit equality ----
        print(f"\n-- FUSED two-stage vs sequential  "
              f"(limits: rms {FUSION_LIMIT_RMS_DB:.0f} dB, peak {FUSION_LIMIT_PEAK_DB:.0f} dB) --")
        for pname, (p1, p2) in FUSION_PAIRS.items():
            for kind in KINDS:
                L, R = make_signal(NSAMP, kind, rng)
                ok &= compare_norm(f"{pname[:36]} / {kind}",
                                   seq_2stage(L, R, p1, p2, fused, BLOCK),
                                   fused_2stage(L, R, p1, p2, fused, BLOCK),
                                   FUSION_LIMIT_RMS_DB, FUSION_LIMIT_PEAK_DB)

        print("\n-- FUSED two-stage, ragged block sizes (tail handling) --")
        p1, p2 = DEMO_PAIR
        for blk in (1, 2, 3, 5, 16, 31, 32, 33, 64):
            L, R = make_signal(BLOCK * 20, "uniform", rng)
            ok &= compare_norm(f"block={blk} / uniform",
                               seq_2stage(L, R, p1, p2, fused, blk),
                               fused_2stage(L, R, p1, p2, fused, blk),
                               FUSION_LIMIT_RMS_DB, FUSION_LIMIT_PEAK_DB)

        print(f"\n-- FUSED two-stage, {FUSION_LONGRUN} samples (drift) --")
        L, R = make_signal(FUSION_LONGRUN, "uniform", rng)
        ok &= compare_norm("demo pair / uniform, long run",
                           seq_2stage(L, R, p1, p2, fused, BLOCK),
                           fused_2stage(L, R, p1, p2, fused, BLOCK),
                           FUSION_LIMIT_RMS_DB, FUSION_LIMIT_PEAK_DB)
        # Second half alone must be no worse than the whole: no accumulation.
        half = FUSION_LONGRUN // 2
        sref = seq_2stage(L, R, p1, p2, fused, BLOCK)
        sgot = fused_2stage(L, R, p1, p2, fused, BLOCK)
        ok &= compare_norm("demo pair / uniform, long run 2nd half only",
                           (sref[0][half:], sref[1][half:]),
                           (sgot[0][half:], sgot[1][half:]),
                           FUSION_LIMIT_RMS_DB, FUSION_LIMIT_PEAK_DB)

    print("\n" + ("RESULT: bit-identical on every non-extreme case, "
                  "fusion error within limits"
                  if ok else "RESULT: MISMATCH -- see FAIL lines above"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
