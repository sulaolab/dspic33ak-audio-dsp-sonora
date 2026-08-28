#!/usr/bin/env python3
"""Numerical guardrail for the bass-enhancer DSP-load reduction.

Re-implements the legacy and the optimised sample loops of
`src/app/apps/classic/dsp/bass_enhancer.c` in IEEE-754 single precision, operation
by operation and in source order, and answers two separate questions:

  1. With ENA_BASSENH_MONO_LOWBAND OFF the optimised loop must be **bit
     identical** to the legacy loop. Nothing about the arithmetic changed --
     only hoisting, local state and pointer walking -- so this is a bit-pattern
     comparison, and any difference is a bug.

  2. With ENA_BASSENH_MONO_LOWBAND ON it deliberately is not: the low band is
     summed to mono before being filtered instead of after, which is the same
     algebra but different rounding. Bit comparison is unavailable *by
     construction*, so this half reports error norms against limits.

Both halves also compare the final filter/envelope/delay-line state, not just
the output samples.

Run: python tools/bassenh_bitexact/bassenh_bitexact.py
"""

import math
import struct
import sys

import numpy as np

f32 = np.float32

# numpy >= 2 (NEP 50) keeps float32 op python-float in float32, which is what
# lets the models below read like the C. Refuse to run where it would silently
# widen to float64 and produce a bit-exactness "pass" that means nothing.
assert (f32(1.0) * 0.5).dtype == np.float32, "need numpy >= 2 (NEP 50) semantics"
assert (f32(1.0) + f32(2.0)).dtype == np.float32
np.seterr(over="ignore", under="ignore", invalid="ignore")

BLOOM_BUF_MAX = 2048      # APP_TARGET_AK512
LOW_SCRATCH = 32          # BASSENH_LOW_SCRATCH == APP_BLOCK_FRAMES


def bits(x):
    return struct.unpack("<I", struct.pack("<f", f32(x)))[0]


def clampf(x, lo, hi):
    # audio_fast_math.h clampf: (x < lo) ? lo : ((x > hi) ? hi : x)
    if x < lo:
        return f32(lo)
    if x > hi:
        return f32(hi)
    return f32(x)


# ---------------------------------------------------------------------------
# biquad, transposed direct form II -- mirrors biquad_process()
# ---------------------------------------------------------------------------
class Biquad:
    __slots__ = ("b0", "b1", "b2", "a1", "a2", "z1", "z2", "fused")

    def __init__(self, b0=1.0, b1=0.0, b2=0.0, a1=0.0, a2=0.0, fused=False):
        self.b0, self.b1, self.b2 = f32(b0), f32(b1), f32(b2)
        self.a1, self.a2 = f32(a1), f32(a2)
        self.z1 = f32(0.0)
        self.z2 = f32(0.0)
        self.fused = fused

    def copy(self):
        b = Biquad(self.b0, self.b1, self.b2, self.a1, self.a2, self.fused)
        b.z1, b.z2 = self.z1, self.z2
        return b

    def process(self, x):
        if self.fused:
            # -ffp-contract=fast may emit mac.s. Approximated with a double
            # accumulate rounded once; exact for the products (24x24 bits fits
            # in 53), one extra rounding on the sum versus a true fma.
            y = f32(float(self.b0) * float(x) + float(self.z1))
            z1 = f32(float(self.b1) * float(x) - float(self.a1) * float(y) + float(self.z2))
            z2 = f32(float(self.b2) * float(x) - float(self.a2) * float(y))
        else:
            y = f32(self.b0 * x + self.z1)
            z1 = f32(self.b1 * x - self.a1 * y + self.z2)
            z2 = f32(self.b2 * x - self.a2 * y)
        self.z1, self.z2 = z1, z2
        return y

    def state(self):
        return (bits(self.z1), bits(self.z2))


def make_lpf(fs, fc, q, fused=False):
    w0 = f32(2.0 * math.pi * fc / fs)
    c = f32(math.cos(w0))
    s = f32(math.sin(w0))
    alpha = f32(s / f32(2.0 * q))
    a0 = f32(1.0 + alpha)
    return Biquad(f32(f32((1.0 - c) * 0.5) / a0), f32(f32(1.0 - c) / a0),
                  f32(f32((1.0 - c) * 0.5) / a0), f32(f32(-2.0 * c) / a0),
                  f32(f32(1.0 - alpha) / a0), fused)


def make_hpf(fs, fc, q, fused=False):
    w0 = f32(2.0 * math.pi * fc / fs)
    c = f32(math.cos(w0))
    s = f32(math.sin(w0))
    alpha = f32(s / f32(2.0 * q))
    a0 = f32(1.0 + alpha)
    return Biquad(f32(f32((1.0 + c) * 0.5) / a0), f32(f32(-(1.0 + c)) / a0),
                  f32(f32((1.0 + c) * 0.5) / a0), f32(f32(-2.0 * c) / a0),
                  f32(f32(1.0 - alpha) / a0), fused)


def make_room_bpf(fs, f0, q, fused=False):
    w0 = f32(2.0 * math.pi * f0 / fs)
    c = f32(math.cos(w0))
    s = f32(math.sin(w0))
    alpha = f32(s / f32(2.0 * q))
    a0 = f32(1.0 + alpha)
    return Biquad(f32(alpha / a0), f32(0.0), f32(f32(-alpha) / a0),
                  f32(f32(-2.0 * c) / a0), f32(f32(1.0 - alpha) / a0), fused)


# ---------------------------------------------------------------------------
# module state, shared by both loop models
# ---------------------------------------------------------------------------
class State:
    def __init__(self, cfg, fused=False):
        fs = cfg["fs"]
        self.lpf1 = [make_lpf(fs, cfg["low_fc"], 0.5412, fused) for _ in range(2)]
        self.lpf2 = [make_lpf(fs, cfg["low_fc"], 1.3065, fused) for _ in range(2)]
        self.dc = [make_hpf(fs, cfg["dc_hz"], 0.707, fused) for _ in range(2)]
        self.room = make_room_bpf(fs, cfg["room_f0"], cfg["room_q"], fused)
        self.env = f32(0.0)
        self.exc_env = f32(0.0)
        self.lw_env = f32(0.0)
        self.bloom = [f32(0.0)] * BLOOM_BUF_MAX
        self.bloom_w = 0

    def fingerprint(self):
        fp = [self.lpf1[0].state(), self.lpf1[1].state(),
              self.lpf2[0].state(), self.lpf2[1].state(),
              self.dc[0].state(), self.dc[1].state(), self.room.state(),
              bits(self.env), bits(self.exc_env), bits(self.lw_env),
              self.bloom_w]
        fp += [bits(v) for v in self.bloom]
        return tuple(fp)


def derive(cfg, st):
    """Block constants, as bassenh_block_prepare() would leave them."""
    return dict(
        wet=clampf(cfg["wet"], 0.0, 1.0),
        dry=clampf(cfg["dry"], 0.0, 1.0),
        thr=clampf(cfg["thr"], 0.5, 0.99),
        env_floor_lpf=f32(cfg["env_floor_lpf"]),
        duck_lpf_coef=f32(cfg["duck_lpf_coef"]),
        exc_target=f32(cfg["exc_target"]),
        exc_aA=f32(math.exp(-1.0 / (cfg["fs"] * cfg["exc_attack_ms"] * 1e-3))),
        exc_aR=f32(math.exp(-1.0 / (cfg["fs"] * cfg["exc_release_ms"] * 1e-3))),
        gain_lpf=f32(cfg["gain_lpf"]),
        block_quiet=f32(cfg["block_quiet"]),
        env_aA=f32(math.exp(-1.0 / (2.0e-3 * cfg["fs"]))),
        env_aR=f32(math.exp(-1.0 / (80.0e-3 * cfg["fs"]))),
        loud_aA=f32(math.exp(-1.0 / (0.010 * cfg["fs"]))),
        loud_aR=f32(math.exp(-1.0 / (0.200 * cfg["fs"]))),
        loud_g=f32(cfg["loud_gain_lin"]),
        bloom_g=f32(cfg["bloom_g"]),
        bloom_D=int(cfg["bloom_D"]),
        bloom_mix=f32(cfg["bloom_mix"]),
        bloom_duck=f32(cfg["bloom_duck"]),
    )


# ---------------------------------------------------------------------------
# model A: the legacy sample loop (pre-optimisation body, verbatim order)
# ---------------------------------------------------------------------------
def loop_legacy(st, c, in_l, in_r, frames):
    out_l = [f32(0.0)] * frames
    out_r = [f32(0.0)] * frames
    N = BLOOM_BUF_MAX

    for i in range(frames):
        xl = in_l[i]
        xr = in_r[i]

        low_l = st.lpf1[0].process(xl)
        low_r = st.lpf1[1].process(xr)
        low_l = st.lpf2[0].process(low_l)
        low_r = st.lpf2[1].process(low_r)
        low_m = f32(0.5 * f32(low_l + low_r))

        wide_abs = f32(0.5 * f32(abs(xl) + abs(xr)))
        if wide_abs > st.lw_env:
            st.lw_env = f32(c["loud_aA"] * st.lw_env + f32(1.0 - c["loud_aA"]) * wide_abs)
        else:
            st.lw_env = f32(c["loud_aR"] * st.lw_env + f32(1.0 - c["loud_aR"]) * wide_abs)

        # env_update(prev, x_abs, aA, aR)
        x_abs = f32(abs(low_m))
        a = c["env_aA"] if x_abs > st.env else c["env_aR"]
        st.env = f32(f32(1.0 - a) * x_abs + a * st.env)

        floor_dyn = f32(c["env_floor_lpf"] * f32(0.2 + f32(0.8 * c["block_quiet"])))
        env_eff = floor_dyn if st.env < floor_dyn else st.env
        duck_lpf = clampf(f32(1.0 - c["duck_lpf_coef"] * st.env), 0.35, 1.0)

        sub_m = f32(f32(f32(f32(c["gain_lpf"] * env_eff) * duck_lpf) * low_m) * c["loud_g"])

        # --- bloom ---
        w = st.bloom_w
        rd = w - c["bloom_D"]
        if rd < 0:
            rd += N
        inj = f32(f32(0.75 * low_m) + f32(0.25 * sub_m))
        y = st.bloom[rd]
        st.bloom[w] = f32(inj + f32(c["bloom_g"] * y))
        w += 1
        if w >= N:
            w = 0
        st.bloom_w = w
        y = st.room.process(y)
        duck_b = f32(1.0 - min(f32(c["bloom_duck"] * st.env), c["bloom_duck"]))
        m = f32(clampf(c["bloom_mix"], 0.0, 1.0) * duck_b)
        sub_m = f32(sub_m + f32(m * y))

        # --- excursion guard ---
        s_abs = f32(abs(sub_m))
        a_exc = f32(1.0 - c["exc_aA"]) if s_abs > st.exc_env else f32(1.0 - c["exc_aR"])
        st.exc_env = f32(st.exc_env + f32(a_exc * f32(s_abs - st.exc_env)))
        if st.exc_env > c["exc_target"]:
            sub_m = f32(sub_m * f32(c["exc_target"] / f32(st.exc_env + f32(1e-12))))

        yl = f32(f32(c["dry"] * xl) + f32(c["wet"] * sub_m))
        yr = f32(f32(c["dry"] * xr) + f32(c["wet"] * sub_m))
        yl = st.dc[0].process(yl)
        yr = st.dc[1].process(yr)
        out_l[i] = soft_clip(yl, c["thr"])
        out_r[i] = soft_clip(yr, c["thr"])

    return out_l, out_r


def soft_clip(x, t):
    tt = clampf(t, 0.5, 0.99)
    cc = f32(0.4 + f32(0.4 * f32(1.0 - tt)))
    x2 = f32(x * x)
    y = f32(x * f32(1.0 - f32(cc * x2)))
    return f32(f32(y * tt) * 1.2)


# ---------------------------------------------------------------------------
# model B: the optimised loop (hoisted invariants, local state, wrap-free runs)
# ---------------------------------------------------------------------------
def loop_opt(st, c, in_l, in_r, frames, mono_lowband):
    out_l = [f32(0.0)] * frames
    out_r = [f32(0.0)] * frames
    N = BLOOM_BUF_MAX

    floor_dyn = f32(c["env_floor_lpf"] * f32(0.2 + f32(0.8 * c["block_quiet"])))
    duck_coef = c["duck_lpf_coef"]
    exc_target = c["exc_target"]
    exc_1ma = f32(1.0 - c["exc_aA"])
    exc_1mr = f32(1.0 - c["exc_aR"])
    wet, dry = c["wet"], c["dry"]
    sub_gain = c["gain_lpf"]
    sc_tt = clampf(c["thr"], 0.5, 0.99)
    sc_c = f32(0.4 + f32(0.4 * f32(1.0 - sc_tt)))
    e_aa, e_1ma = c["env_aA"], f32(1.0 - c["env_aA"])
    e_ar, e_1mr = c["env_aR"], f32(1.0 - c["env_aR"])
    l_aa, l_1ma = c["loud_aA"], f32(1.0 - c["loud_aA"])
    l_ar, l_1mr = c["loud_aR"], f32(1.0 - c["loud_aR"])
    loud_g = c["loud_g"]
    bloom_g = c["bloom_g"]
    bloom_duck = c["bloom_duck"]
    bloom_mix = clampf(c["bloom_mix"], 0.0, 1.0)
    D = c["bloom_D"]

    done = 0
    while done < frames:
        seg = min(frames - done, LOW_SCRATCH)

        # --- low-band pass, over a block-sized scratch ---
        low = [f32(0.0)] * seg
        if mono_lowband:
            for i in range(seg):
                m = f32(0.5 * f32(in_l[done + i] + in_r[done + i]))
                m = st.lpf1[0].process(m)
                m = st.lpf2[0].process(m)
                low[i] = m
        else:
            for i in range(seg):
                ll = st.lpf1[0].process(in_l[done + i])
                lr = st.lpf1[1].process(in_r[done + i])
                ll = st.lpf2[0].process(ll)
                lr = st.lpf2[1].process(lr)
                low[i] = f32(0.5 * f32(ll + lr))

        off = 0
        while off < seg:
            rd = st.bloom_w - D
            if rd < 0:
                rd += N
            run = min(seg - off, N - st.bloom_w, N - rd)

            for k in range(run):
                idx = done + off + k
                xl = in_l[idx]
                xr = in_r[idx]
                low_m = low[off + k]

                wide_abs = f32(0.5 * f32(abs(xl) + abs(xr)))
                if wide_abs > st.lw_env:
                    st.lw_env = f32(l_aa * st.lw_env + l_1ma * wide_abs)
                else:
                    st.lw_env = f32(l_ar * st.lw_env + l_1mr * wide_abs)

                low_abs = f32(abs(low_m))
                if low_abs > st.env:
                    st.env = f32(e_1ma * low_abs + e_aa * st.env)
                else:
                    st.env = f32(e_1mr * low_abs + e_ar * st.env)

                env_eff = floor_dyn if st.env < floor_dyn else st.env
                duck_lpf = clampf(f32(1.0 - duck_coef * st.env), 0.35, 1.0)

                sub_m = f32(f32(f32(f32(sub_gain * env_eff) * duck_lpf) * low_m) * loud_g)

                y_raw = st.bloom[rd + k]
                inj = f32(f32(0.75 * low_m) + f32(0.25 * sub_m))
                st.bloom[st.bloom_w + k] = f32(inj + f32(bloom_g * y_raw))

                y = st.room.process(y_raw)
                duck_b = f32(1.0 - min(f32(bloom_duck * st.env), bloom_duck))
                m = f32(bloom_mix * duck_b)
                sub_m = f32(sub_m + f32(m * y))

                s_abs = f32(abs(sub_m))
                a_exc = exc_1ma if s_abs > st.exc_env else exc_1mr
                st.exc_env = f32(st.exc_env + f32(a_exc * f32(s_abs - st.exc_env)))
                if st.exc_env > exc_target:
                    sub_m = f32(sub_m * f32(exc_target / f32(st.exc_env + f32(1e-12))))

                yl = f32(f32(dry * xl) + f32(wet * sub_m))
                yr = f32(f32(dry * xr) + f32(wet * sub_m))
                yl = st.dc[0].process(yl)
                yr = st.dc[1].process(yr)

                yl = f32(yl * f32(1.0 - f32(sc_c * f32(yl * yl))))
                yr = f32(yr * f32(1.0 - f32(sc_c * f32(yr * yr))))
                out_l[idx] = f32(f32(yl * sc_tt) * 1.2)
                out_r[idx] = f32(f32(yr * sc_tt) * 1.2)

            st.bloom_w += run
            if st.bloom_w >= N:
                st.bloom_w = 0
            off += run
        done += seg

    return out_l, out_r


# ---------------------------------------------------------------------------
# signals and configurations
# ---------------------------------------------------------------------------
SHIPPING = dict(
    fs=48000.0, low_fc=110.0, dc_hz=20.0, room_f0=55.0, room_q=2.4,
    wet=1.0, dry=1.0, thr=0.92,
    env_floor_lpf=0.28, duck_lpf_coef=0.40,
    exc_target=0.23, exc_attack_ms=2.2, exc_release_ms=240.0,
    gain_lpf=8.1283,        # db_to_lin(+18.2 dB), the measured shipping state
    block_quiet=1.0,
    loud_gain_lin=4.0741, bloom_g=0.976, bloom_D=1056,
    bloom_mix=0.78, bloom_duck=0.18,
)


def cfg_with(**kw):
    c = dict(SHIPPING)
    c.update(kw)
    return c


def signal(kind, n, seed=1):
    rng = np.random.default_rng(seed)
    t = np.arange(n)
    if kind == "uniform":
        l = rng.uniform(-0.5, 0.5, n)
        r = rng.uniform(-0.5, 0.5, n)
    elif kind == "near_mono":
        base = rng.uniform(-0.5, 0.5, n)
        l = base + 1e-6 * rng.uniform(-1, 1, n)
        r = base - 1e-6 * rng.uniform(-1, 1, n)
    elif kind == "antiphase":
        base = rng.uniform(-0.5, 0.5, n)
        l, r = base, -base
    elif kind == "square_fs":
        s = np.where((t // 17) % 2 == 0, 1.0, -1.0)
        l, r = s, -s
    elif kind == "bass_sine":
        l = 0.8 * np.sin(2 * np.pi * 60.0 * t / 48000.0)
        r = 0.8 * np.sin(2 * np.pi * 60.0 * t / 48000.0 + 0.3)
    elif kind == "silence_bursts":
        s = rng.uniform(-0.5, 0.5, n)
        s[(t // 64) % 2 == 0] = 0.0
        l, r = s, s * 0.5
    elif kind == "tiny":
        l = 1e-6 * rng.uniform(-1, 1, n)
        r = 1e-6 * rng.uniform(-1, 1, n)
    elif kind == "loud_dc":
        l = np.full(n, 0.95)
        r = np.full(n, -0.95)
    else:
        raise ValueError(kind)
    return [f32(v) for v in l], [f32(v) for v in r]


SIGNALS = ["uniform", "near_mono", "antiphase", "square_fs", "bass_sine",
           "silence_bursts", "tiny", "loud_dc"]


def run_blocks(st, c, in_l, in_r, frames, model, mono=False):
    """Feed the signal through in `frames`-sized blocks, as the app does."""
    out_l, out_r = [], []
    n = len(in_l)
    i = 0
    while i + frames <= n:
        bl = in_l[i:i + frames]
        br = in_r[i:i + frames]
        if model == "legacy":
            ol, orr = loop_legacy(st, c, bl, br, frames)
        else:
            ol, orr = loop_opt(st, c, bl, br, frames, mono)
        out_l += ol
        out_r += orr
        i += frames
    return out_l, out_r


def db(x):
    return -999.0 if x <= 0 else 20.0 * math.log10(x)


def norms(ref_l, ref_r, tst_l, tst_r):
    a = np.array([float(v) for v in ref_l] + [float(v) for v in ref_r])
    b = np.array([float(v) for v in tst_l] + [float(v) for v in tst_r])
    e = b - a
    rms_ref = math.sqrt(float(np.mean(a * a)))
    rms_err = math.sqrt(float(np.mean(e * e)))
    pk_ref = float(np.max(np.abs(a)))
    pk_err = float(np.max(np.abs(e)))
    return (db(rms_err) - db(rms_ref) if rms_ref > 0 else db(rms_err),
            db(pk_err) - db(pk_ref) if pk_ref > 0 else db(pk_err))


# ---------------------------------------------------------------------------
# suites
# ---------------------------------------------------------------------------
# Sanity bound on the raw legacy-vs-mono difference. Deliberately loose: the
# meaningful test is suite_noise_floor() below, not this number. See its docstring.
RMS_LIMIT_DB = -60.0
PEAK_LIMIT_DB = -55.0

# The mono ordering may not raise the low band's own quantisation noise floor by
# more than this, measured against a float64 reference of the same filter.
NOISE_FLOOR_MARGIN_DB = 1.0


def lp64(b, x, z):
    y = b[0] * x + z[0]
    z[0] = b[1] * x - b[3] * y + z[1]
    z[1] = b[2] * x - b[4] * y
    return y


def coeffs64(fs, fc, q):
    w0 = 2.0 * math.pi * fc / fs
    c, s = math.cos(w0), math.sin(w0)
    al = s / (2.0 * q)
    a0 = 1.0 + al
    return [(1 - c) * 0.5 / a0, (1 - c) / a0, (1 - c) * 0.5 / a0,
            (-2 * c) / a0, (1 - al) / a0]


def suite_noise_floor():
    """The honest test for the mono low-band lever.

    A raw legacy-vs-mono difference of -67 dB looks alarming and means nothing
    on its own, because the two orderings are not being compared against the
    truth -- they are being compared against each other, and BOTH carry the
    quantisation noise of a 4th-order 110 Hz Butterworth running in float32
    transposed-direct-form-II. Poles that close to z = 1 amplify each rounding
    by ~70 dB, so the module's low band already sits about 73 dB below its own
    signal, in the shipping code, today.

    So the question is not "do the two orderings differ" (they must) but "does
    the new ordering add noise". Measured against a float64 reference of the
    same filter, it does not: the difference between the two float32 orderings
    is smaller than the noise floor either one already has.

    The divergence originates entirely here -- every stage downstream of the
    low band is shared between the two loops, and continuous (the clamps, the
    envelope crossovers and the excursion-guard threshold are all continuous at
    their switching points), so it transports this error without adding a
    mechanism of its own.
    """
    n = 32 * 600
    in_l, in_r = signal("uniform", n, seed=3)
    bad = 0
    print("  low-band chain vs a float64 reference of the same filter:")
    for fc in (40.0, 110.0, 300.0):
        b1, b2 = coeffs64(48000.0, fc, 0.5412), coeffs64(48000.0, fc, 1.3065)
        z1, z2 = [0.0, 0.0], [0.0, 0.0]
        ref = np.array([lp64(b2, lp64(b1, 0.5 * (float(in_l[i]) + float(in_r[i])), z1), z2)
                        for i in range(n)])

        a1l, a1r = make_lpf(48000.0, fc, 0.5412), make_lpf(48000.0, fc, 0.5412)
        a2l, a2r = make_lpf(48000.0, fc, 1.3065), make_lpf(48000.0, fc, 1.3065)
        ster = np.array([float(f32(0.5 * f32(a2l.process(a1l.process(in_l[i]))
                                            + a2r.process(a1r.process(in_r[i])))))
                         for i in range(n)])

        m1, m2 = make_lpf(48000.0, fc, 0.5412), make_lpf(48000.0, fc, 1.3065)
        mono = np.array([float(m2.process(m1.process(f32(0.5 * f32(in_l[i] + in_r[i])))))
                         for i in range(n)])

        def rms(v):
            return math.sqrt(float(np.mean(v * v)))

        base = db(rms(ref))
        n_ster = db(rms(ster - ref)) - base
        n_mono = db(rms(mono - ref)) - base
        n_diff = db(rms(mono - ster)) - base
        worse = n_mono - n_ster
        flag = "" if worse <= NOISE_FLOOR_MARGIN_DB else "   FAIL"
        if flag:
            bad += 1
        print(f"    fc={fc:5.0f} Hz  band {base:7.2f} dBFS | "
              f"shipping {n_ster:7.1f} dB | mono {n_mono:7.1f} dB | "
              f"added {worse:+5.1f} dB | orderings differ {n_diff:7.1f} dB{flag}")
    print(f"    added noise must be <= {NOISE_FLOOR_MARGIN_DB:.1f} dB")
    print("    note fc=40 Hz: -48 dB is the module's PRE-EXISTING float32 floor,")
    print("         unchanged by this work and worth knowing about on its own.")
    return bad


def suite_bitexact(fused=False):
    """Optimised (mono lever OFF) must be bit-identical to legacy."""
    cases = []
    for sig in SIGNALS:
        cases.append(dict(sig=sig, frames=32, cfg={}))
    for frames in (1, 2, 3, 5, 16, 31, 32, 33, 64):
        cases.append(dict(sig="uniform", frames=frames, cfg={}))
    # bloom delay edge cases, including indices that alias and wraps that land
    # mid-run -- this is where the wrap-free run segmentation can go wrong
    for D in (1, 2, 31, 32, 33, 1056, BLOOM_BUF_MAX - 1):
        cases.append(dict(sig="uniform", frames=32, cfg=dict(bloom_D=D)))
        cases.append(dict(sig="uniform", frames=33, cfg=dict(bloom_D=D)))
    # feature corners: mix/duck off, clamps pinned, guard never firing / always firing
    for extra in (dict(bloom_mix=0.0), dict(bloom_duck=0.0),
                  dict(exc_target=1e9), dict(exc_target=1e-6),
                  dict(gain_lpf=1.0, loud_gain_lin=1.0), dict(thr=0.5), dict(thr=0.99),
                  dict(bloom_g=0.985), dict(bloom_g=0.0), dict(block_quiet=0.0),
                  dict(env_floor_lpf=0.0), dict(duck_lpf_coef=4.0), dict(wet=0.0),
                  dict(dry=0.0)):
        cases.append(dict(sig="uniform", frames=32, cfg=extra))
        cases.append(dict(sig="bass_sine", frames=32, cfg=extra))

    n = 32 * 40
    bad = 0
    for case in cases:
        cfg = cfg_with(**case["cfg"])
        in_l, in_r = signal(case["sig"], n)
        st_a, st_b = State(cfg, fused), State(cfg, fused)
        c = derive(cfg, st_a)
        ra = run_blocks(st_a, c, in_l, in_r, case["frames"], "legacy")
        rb = run_blocks(st_b, c, in_l, in_r, case["frames"], "opt", mono=False)
        ok_out = all(bits(x) == bits(y) for x, y in zip(ra[0], rb[0])) and \
                 all(bits(x) == bits(y) for x, y in zip(ra[1], rb[1]))
        ok_state = st_a.fingerprint() == st_b.fingerprint()
        if not (ok_out and ok_state):
            bad += 1
            print(f"  FAIL  sig={case['sig']:14s} frames={case['frames']:3d} "
                  f"cfg={case['cfg']}  out_ok={ok_out} state_ok={ok_state}")
    tag = "fused (mac.s)" if fused else "two-rounding (mul+add)"
    print(f"  bit-exact, {tag}: {len(cases) - bad}/{len(cases)} identical")
    return bad


def suite_mono_lowband():
    """Mono low band: not bit-exact by construction, so bound the error."""
    cases = []
    for sig in SIGNALS:
        cases.append(dict(sig=sig, frames=32, cfg={}))
    for extra in (dict(bloom_g=0.985), dict(room_q=5.0), dict(exc_target=1e-6),
                  dict(gain_lpf=1.0, loud_gain_lin=1.0), dict(low_fc=40.0),
                  dict(low_fc=300.0), dict(bloom_mix=0.0), dict(block_quiet=0.0)):
        cases.append(dict(sig="uniform", frames=32, cfg=extra))
        cases.append(dict(sig="bass_sine", frames=32, cfg=extra))

    n = 32 * 60
    worst_rms, worst_pk, worst_case = -999.0, -999.0, None
    bad = 0
    for case in cases:
        cfg = cfg_with(**case["cfg"])
        in_l, in_r = signal(case["sig"], n)
        st_a, st_b = State(cfg), State(cfg)
        c = derive(cfg, st_a)
        ra = run_blocks(st_a, c, in_l, in_r, case["frames"], "legacy")
        rb = run_blocks(st_b, c, in_l, in_r, case["frames"], "opt", mono=True)
        r, p = norms(ra[0], ra[1], rb[0], rb[1])
        if r > worst_rms:
            worst_rms, worst_case = r, case
        worst_pk = max(worst_pk, p)
        if r > RMS_LIMIT_DB or p > PEAK_LIMIT_DB:
            bad += 1
            print(f"  FAIL  sig={case['sig']:14s} cfg={case['cfg']}  "
                  f"rms={r:.1f} dB peak={p:.1f} dB")
    print(f"  mono low band: {len(cases) - bad}/{len(cases)} within limits "
          f"(rms <= {RMS_LIMIT_DB:.0f} dB, peak <= {PEAK_LIMIT_DB:.0f} dB)")
    print(f"    worst error RMS  {worst_rms:7.1f} dB   (limit {RMS_LIMIT_DB:.0f})"
          f"   [{worst_case['sig']} {worst_case['cfg']}]")
    print(f"    worst peak error {worst_pk:7.1f} dB   (limit {PEAK_LIMIT_DB:.0f})")
    return bad


def suite_drift():
    """Long run: the error must not accumulate through the bloom feedback."""
    n = 32 * 900
    cfg = cfg_with(bloom_g=0.985)
    in_l, in_r = signal("uniform", n, seed=7)
    st_a, st_b = State(cfg), State(cfg)
    c = derive(cfg, st_a)
    ra = run_blocks(st_a, c, in_l, in_r, 32, "legacy")
    rb = run_blocks(st_b, c, in_l, in_r, 32, "opt", mono=True)
    half = len(ra[0]) // 2
    whole = norms(ra[0], ra[1], rb[0], rb[1])
    tail = norms(ra[0][half:], ra[1][half:], rb[0][half:], rb[1][half:])
    print(f"  drift over {n} samples, bloom_g = 0.985 (worst-case feedback):")
    print(f"    whole run      rms {whole[0]:7.1f} dB   peak {whole[1]:7.1f} dB")
    print(f"    second half    rms {tail[0]:7.1f} dB   peak {tail[1]:7.1f} dB")
    bad = 1 if (tail[0] > RMS_LIMIT_DB or tail[1] > PEAK_LIMIT_DB) else 0
    if bad:
        print("  FAIL  drift exceeds limits")
    return bad


def main():
    print(__doc__.strip().splitlines()[0])
    print()
    print("1. ENA_BASSENH_MONO_LOWBAND OFF -- must be bit identical")
    bad = suite_bitexact(fused=False)
    bad += suite_bitexact(fused=True)
    print()
    print("2. ENA_BASSENH_MONO_LOWBAND ON -- error norms replace bit comparison")
    bad += suite_noise_floor()
    print()
    bad += suite_mono_lowband()
    bad += suite_drift()
    print()
    if bad:
        print(f"RESULT: {bad} failing case(s)")
        return 1
    print("RESULT: all cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
