#!/usr/bin/env python3
"""HOST-ONLY model for the universal Nyquist / anti-alias planner feasibility study.

WHAT THIS IS
============
The shipping architecture selects an anti-alias front end from a hand-written table in
`src/app/apps/asrc/asrc_audio_path.c` (`asrc_audio_path_frontend_plan()`).  That table has rows
only where one leg runs at 48 kHz or 96 kHz; every other down-conversion runs `fe=direct`, i.e.
the rear resampler alone, whose only band limit is ASRC_POLY_FC = 0.465 of its OWN input rate.

This module asks whether the per-pair table could be replaced by a PLANNER: given (Fs_in, Fs_out)
and one uniform protected-band specification, derive the anti-alias chain instead of looking it up.
It is the shared model; the driver that sweeps the 45 pairs is
`asrc_universal_nyquist_planner.py`.

NOTHING HERE TOUCHES FIRMWARE.  No hardware is used.  No production file is modified.

MODEL PROVENANCE -- every piece is reused, not re-derived
=========================================================
  * FIR design            tools/asrc/asrc_decimator_48_to_8_design.py :: design()
  * rear resampler rows   tools/asrc/asrc_headroom_filter_check.py    :: phase_rows(), CANDIDATES[1]
  * alias bookkeeping     the fold/mask logic of asrc_lowpass_rate_survey.py (integer chains) and
                          asrc_48_to_32_audio_gate.py (the L/M zero-stuffed image pair)
  * tap sizing law        the Kaiser order estimate, CALIBRATED against six shipped filters --
                          see TAP_LAW_K below.  This is the one genuinely new common function.

Only NumPy is required.  Import from the repository root.
"""
from __future__ import annotations

import contextlib
import dataclasses
import importlib.util
import io
import itertools
import math
import pathlib
import re
import sys

import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(mod)
    return mod


chk = _load("chk", "tools/asrc/asrc_headroom_filter_check.py")
dsg = _load("dsg", "tools/asrc/asrc_decimator_48_to_8_design.py")

# ------------------------------------------------------------------------------------ constants
RATES = (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 96000)

L_REAR = chk.L                       # 128 resampler phases
REAR = chk.CANDIDATES[1]             # headroom-m30-kaiser11.3: 30 taps, fc 0.464 of ITS input rate
KAISER_BETA = dsg.KAISER_BETA        # 11.0, the module-wide value every shipped set uses

NFFT = 1 << 18                       # 0.37 Hz bins at 96 kHz -- deep stopbands need the resolution
GRID_HZ = 1.0                        # alias scan step over [0, Fs_in/2]

ALIAS_GATE_DB = -100.0               # brief section 6 target
PASSBAND_TARGETS_DB = (0.10, 0.25)   # brief section 7 candidates
NYQ_RATIOS = (0.90, 0.85, 0.8125)    # brief section 5 candidates C / B / A

# THE RING, from src/app/apps/asrc/asrc_app_build_config.h for the AK512 M30 family.
FIFO_FRAMES = 128
BLOCK_FRAMES = 16
POLY_AHEAD = 15
FILL_JITTER = 4
FILL_TARGET_MAX = FIFO_FRAMES - 4 - BLOCK_FRAMES - FILL_JITTER      # 104
FILL_BLOCK_RESERVE = BLOCK_FRAMES + 1                               # 17
# Hard bound: fs_in*(BLOCK-1) < (FIFO-4-AHEAD)*fs_out  ->  step < 109/15 = 7.2667
STEP_HARD_MAX = (FIFO_FRAMES - 4 - POLY_AHEAD) / (BLOCK_FRAMES - 1)

# The A1 qualification fence (audio_app_asrc.c): DIRECT-path pairs this image is not qualified for.
QUAL_FENCE_DIRECT = frozenset({
    (22050, 8000), (32000, 11025), (44100, 12000),
    (44100, 16000), (48000, 11025), (96000, 22050),
})

# TAP SIZING LAW.  Kaiser order estimate N ~= (A-8)/(2.285*dw) with A = 108 dB (the beta=11
# asymptote this tree's design() reaches), rewritten as N = K * fs / transition_hz, K = 6.965.
# VALIDATED against every shipped set (run the driver with --anchors).  It SIZES candidates; the
# spectral model then verifies them and taps are grown until the verified alias clears the gate, so
# a mis-sized candidate costs iterations, never a wrong verdict.
TAP_LAW_K = (108.0 - 8.0) / (2.285 * 2.0 * math.pi)

# CPU CALIBRATION, from hardware.
# [internal] report_96to32_thirdband_kernel_bench_2026-09-09.md measured the SHIPPING
# composed 96 -> 32 kHz front end at 208.80 cycles per 32 kHz output per channel in one window,
# split 107.58 (41-tap /2 at 96 kHz) + 101.22 (97-tap 2/3 rows).  Converting both to MACs per
# output per channel gives 1.75 and 2.09 cycles/MAC; the pure kernel slope self-calibrated in the
# same run is 1.012 cycles/MAC, the rest being the ring push, the intermediate buffer, the phase
# schedule and the s24-left scatter that a real chain pays and a bench does not.
# INDEPENDENTLY CONFIRMED on a second, unrelated row: the shipped /4:11k front end
# (27 taps at 24 kHz + 129 at 12 kHz) models as 111 us per 48 kHz block at 16 channels, and the
# hardware telemetry field `fe` for the 48 kHz family reads 96..115 us.  Two anchors from different
# studies, different structures, same constant -- so it is not fitted to either.
#
# NOT USED, and deliberately: the "36.2 / 54.1 us Q31 vs 92.5 / 138.4 us float" figures quoted for
# the 113/169-tap wide 96->48 pre-stage in asrc_audio_path.c do NOT reconcile with either anchor at
# 16 channels (they imply ~0.5 cycles/MAC, which no scalar MAC can do), so their channel count or
# window must be different.  They are reported as an open discrepancy rather than averaged in.
# There is also no float MULTIPLIER to give: the anti-alias front end is Q31 in both arms
# (ASRC_SAMPLE_Q31 selects the REAR sample arm), and the 2026-09-12 sweep measured the front end as
# unchanged between them (dfe = -0.2..+1.7 us on fe = 96..115 us).
CYC_PER_MAC = 1.90                   # calibrated, real chain, Q31
CYC_PER_MAC_LO = 1.012               # kernel slope -- an absolute FLOOR, not an estimate
CPU_HZ = 200e6

# Front-end budget, derived from documented whole-system telemetry rather than assumed:
#   P1 16 ch, leg A 48 kHz: fe ~= 33 % of 200 MHz with max_demand 83..88 % -> ~40 % to reach 95 %
#   P2 12 ch, leg A 96 kHz: fe ~= 40 % (208.80 cyc/out/ch x 12) with max_demand ~90 % -> ~45 %
# Both are BUDGETS FOR THE FRONT END ALONE and already contain the 5 % engineering reserve; the
# report keeps spectral feasibility, hard-RT feasibility and the reserve in separate columns.
BUDGET_P1_PCT = 40.0
BUDGET_P2_PCT = 45.0


# ------------------------------------------------------------------------------------ responses
_MAG_CACHE: dict[tuple[bytes, float], tuple[np.ndarray, np.ndarray]] = {}


def _spectrum(coeff: np.ndarray, fs: float):
    key = (np.asarray(coeff, dtype=np.float64).tobytes(), fs)
    hit = _MAG_CACHE.get(key)
    if hit is None:
        mag = np.abs(np.fft.rfft(np.asarray(coeff, dtype=np.float64), NFFT))
        axis = np.linspace(0.0, fs * 0.5, mag.size)
        hit = (axis, mag)
        _MAG_CACHE[key] = hit
    return hit


def mag_at(coeff: np.ndarray, fs: float, freqs) -> np.ndarray:
    axis, mag = _spectrum(coeff, fs)
    return np.interp(np.asarray(freqs, dtype=np.float64), axis, mag)


def fold(f, rate: float) -> np.ndarray:
    r = np.asarray(f, dtype=np.float64) % rate
    return np.minimum(r, rate - r)


def db(x) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.asarray(x, dtype=np.float64), 1e-15))


_REAR_PROTO = None


def rear_prototype() -> np.ndarray:
    """The shipping M30 resampler's interleaved polyphase prototype, as its init builds it."""
    global _REAR_PROTO
    if _REAR_PROTO is None:
        rows = chk.phase_rows(REAR)
        proto = np.zeros(REAR.taps * L_REAR)
        for p in range(L_REAR):
            for t in range(REAR.taps):
                proto[t * L_REAR - p + (L_REAR - 1)] = rows[p, t]
        _REAR_PROTO = proto / proto.sum()
    return _REAR_PROTO


def tap_law(fs: float, transition_hz: float) -> int:
    """Odd tap count for a beta=11 Kaiser lowpass with this transition width at this rate."""
    if transition_hz <= 0.0:
        return 10001
    n = int(math.ceil(TAP_LAW_K * fs / transition_hz))
    return n + 1 if n % 2 == 0 else n


def q31_quantize(coeff):
    """Quantize a unit-sum prototype the way the Q31 front end stores it.

    The firmware scales a set so its largest coefficient fills Q31 and carries the scale in the
    accumulator, so the quantization step is peak-relative.  Returns (quantized, peak, sum_abs).
    """
    c = np.asarray(coeff, dtype=np.float64)
    peak = float(np.max(np.abs(c)))
    if peak == 0.0:
        return c, 0.0, 0.0
    full = 2.0 ** 31 - 1.0
    q = np.round(c / peak * full) / full * peak
    return q, peak, float(np.sum(np.abs(q)))


# ------------------------------------------------------------------------------------ chains
@dataclasses.dataclass
class Stage:
    kind: str            # 'dec' (decimate by d), 'lpf' (decimate by 1), 'rat' (L/M polyphase)
    fs_in: float
    fs_out: float
    taps: int
    pb: float
    sb: float
    coeff: np.ndarray
    d: int = 1
    L: int = 1
    M: int = 1
    proto_fs: float = 0.0        # 'rat' only: L * fs_in

    @property
    def macs_per_sec(self) -> float:
        """MACs/s per channel.  A decimating FIR computes one dot product per OUTPUT sample."""
        if self.kind == "rat":
            return (self.taps / self.L) * self.fs_out
        return self.taps * self.fs_out

    def describe(self) -> str:
        if self.kind == "rat":
            return f"{self.L}/{self.M}@{self.fs_in / 1000:.4g}k N{self.taps}"
        if self.kind == "lpf":
            return f"LPF@{self.fs_in / 1000:.4g}k N{self.taps}"
        return f"/{self.d}@{self.fs_in / 1000:.4g}k N{self.taps}"


@dataclasses.dataclass
class Chain:
    label: str
    family: str                  # 'direct' | 'A1' | 'A2' | 'A1+A2' | 'A1+rat'
    stages: list
    rear_in: float               # the rear resampler's input rate
    fs_out: float

    @property
    def step(self) -> float:
        return self.rear_in / self.fs_out

    @property
    def macs_per_sec(self) -> float:
        return sum(s.macs_per_sec for s in self.stages)

    @property
    def taps_list(self):
        return [s.taps for s in self.stages]

    def cost_units(self) -> float:
        """The repo's existing unit: taps * (stage output rate / 48 kHz)."""
        return self.macs_per_sec / 48000.0

    def structures(self):
        out = set()
        for s in self.stages:
            if s.kind == "dec":
                out.add(f"dec/{s.d}")
            elif s.kind == "lpf":
                out.add("lpf/1")
            else:
                out.add(f"rat{s.L}/{s.M}")
        return out

    def describe(self) -> str:
        return " -> ".join(s.describe() for s in self.stages) or "(no front end)"

    def history_frames(self) -> int:
        """FIR history the chain needs, per channel, in frames (RAM proxy)."""
        return sum(s.taps for s in self.stages)


def make_stage(kind: str, fs_in: float, pb: float, sb: float, d: int = 1,
               L: int = 1, M: int = 1, taps=None, scale: float = 1.0) -> Stage:
    if kind == "rat":
        proto_fs = fs_in * L
        fs_out = proto_fs / M
        n = taps if taps is not None else tap_law(proto_fs, max(sb - pb, 1.0))
        n = max(int(round(n * scale)), 9)
        if n % 2 == 0:
            n += 1
        coeff = dsg.design(n, proto_fs, pb, sb, KAISER_BETA)
        return Stage("rat", fs_in, fs_out, n, pb, sb, coeff, 1, L, M, proto_fs)
    fs_out = fs_in / d
    n = taps if taps is not None else tap_law(fs_in, max(sb - pb, 1.0))
    n = max(int(round(n * scale)), 9)
    if n % 2 == 0:
        n += 1
    coeff = dsg.design(n, fs_in, pb, sb, KAISER_BETA)
    return Stage(kind, fs_in, fs_out, n, pb, sb, coeff, d, 1, 1, 0.0)


def cascades(fs_in: float, fs_out: float, max_len: int = 3):
    """Integer decimation cascades over the factors the firmware implements ({2,3} stages)."""
    yield ()
    for n in range(1, max_len + 1):
        for combo in itertools.product((2, 3), repeat=n):
            d = 1
            for c in combo:
                d *= c
            if fs_in / d >= fs_out:
                yield combo


def build_candidates(fs_in: float, fs_out: float, fp: float, scale: float = 1.0):
    """Every candidate chain this study considers for one pair at one protected edge."""
    sb_final = fs_out - fp
    out = []

    # 0. direct -- what an unlisted pair runs today.
    out.append(Chain("direct", "direct", [], fs_in, fs_out))

    for combo in cascades(fs_in, fs_out):
        rates = [fs_in]
        for c in combo:
            rates.append(rates[-1] / c)
        R = rates[-1]

        def relaxed_cascade():
            """The cascade with every stage sized only against what IT folds in."""
            stages = []
            for i, c in enumerate(combo):
                sb = rates[i + 1] - fp
                if sb <= fp:
                    return None
                stages.append(make_stage("dec", rates[i], fp, sb, d=c, scale=scale))
            return stages

        # ---- Architecture 1: existing integer stages only, the LAST one carrying the sharp edge.
        if combo:
            stages = []
            ok = True
            for i, c in enumerate(combo):
                sb = sb_final if i == len(combo) - 1 else (rates[i + 1] - fp)
                if sb <= fp:
                    ok = False
                    break
                stages.append(make_stage("dec", rates[i], fp, sb, d=c, scale=scale))
            if ok:
                out.append(Chain(f"A1 /{int(round(fs_in / R))}", "A1", stages, R, fs_out))

        # ---- Architecture 2 at rate R: cascade (relaxed stages) + one decimate-by-1 LPF.
        #      combo == () is the pure SOURCE-RATE LPF the brief calls out as the key candidate.
        if (R > fs_out or not combo) and sb_final > fp:
            stages = relaxed_cascade()
            if stages is not None:
                stages = list(stages)
                stages.append(make_stage("lpf", R, fp, sb_final, d=1, scale=scale))
                if not combo:
                    label, fam = "A2 srcLPF", "A2"
                else:
                    label = f"A1 /{int(round(fs_in / R))} + A2 LPF@{R / 1000:.4g}k"
                    fam = "A1+A2"
                out.append(Chain(label, fam, stages, R, fs_out))

        # ---- Existing L=2/M=3 polyphase reuse, optionally behind an integer cascade.
        r23 = R * 2.0 / 3.0
        if r23 >= fs_out and (r23 - fp) > fp:
            stages = relaxed_cascade()
            if stages is not None:
                stages = list(stages)
                stages.append(make_stage("rat", R, fp, r23 - fp, L=2, M=3, scale=scale))
                label = "A1 2/3" if not combo else f"A1 /{int(round(fs_in / R))} + 2/3"
                out.append(Chain(label, "A1+rat", stages, r23, fs_out))

    return out


# ------------------------------------------------------------------------------------ evaluation
@dataclasses.dataclass
class Result:
    worst_alias_db: float            # worst alias landing in [0, fp]
    worst_src_hz: float
    worst_land_hz: float
    alias_full_band_db: float        # worst alias landing in [0, fs_out/2] -- never hidden
    passband_err_db: float           # max |deviation| over [0, fp], DC-referenced
    passband_at_fp_db: float
    rear_only_at_fp_db: float


def evaluate(chain: Chain, fs_in: float, fs_out: float, fp: float, q31: bool = False) -> Result:
    nyq_in = fs_in * 0.5
    freqs = np.arange(0.0, nyq_in + GRID_HZ, GRID_HZ)
    rear = rear_prototype()

    def coeff_of(s: Stage):
        c = np.asarray(s.coeff, dtype=np.float64)
        return q31_quantize(c)[0] if q31 else c

    # --- alias: propagate every image branch, then land it on the output grid.
    branches = [(freqs.copy(), np.ones_like(freqs), fs_in)]
    for s in chain.stages:
        nxt = []
        for pos, mag, rate in branches:
            c = coeff_of(s)
            if s.kind == "rat":
                proto_fs = rate * s.L
                new_rate = proto_fs / s.M
                seen = []
                for k in range(0, s.L + 1):
                    for sgn in (1.0, -1.0):
                        img = np.abs(k * rate + sgn * pos)
                        img = np.where(img > proto_fs * 0.5, proto_fs - img, img)
                        img = np.abs(img)
                        if any(np.allclose(img, t, atol=1e-6) for t in seen):
                            continue
                        seen.append(img)
                        nxt.append((fold(img, new_rate),
                                    mag * mag_at(c, proto_fs, img), new_rate))
            else:
                new_rate = rate / s.d
                nxt.append((fold(pos, new_rate), mag * mag_at(c, rate, pos), new_rate))
        branches = nxt

    worst = -400.0
    src = land = 0.0
    worst_full = -400.0
    for pos, mag, rate in branches:
        landing = fold(pos, fs_out)
        m = mag * mag_at(rear, float(L_REAR) * rate, pos)
        level = db(m)
        is_alias = np.abs(landing - freqs) > 0.5
        sel = is_alias & (landing <= fp)
        if sel.any():
            k = int(np.argmax(np.where(sel, level, -np.inf)))
            if level[k] > worst:
                worst, src, land = float(level[k]), float(freqs[k]), float(landing[k])
        sel = is_alias & (landing <= fs_out * 0.5)
        if sel.any():
            k = int(np.argmax(np.where(sel, level, -np.inf)))
            if level[k] > worst_full:
                worst_full = float(level[k])

    # --- passband: the THROUGH path only (fp is below every intermediate Nyquist, so no fold).
    pf = np.linspace(0.0, fp, 2001)
    resp = np.ones_like(pf)
    rate = fs_in
    for s in chain.stages:
        c = coeff_of(s)
        if s.kind == "rat":
            proto_fs = rate * s.L
            resp = resp * mag_at(c, proto_fs, pf)
            rate = proto_fs / s.M
        else:
            resp = resp * mag_at(c, rate, pf)
            rate = rate / s.d
    rear_pb = mag_at(rear, float(L_REAR) * rate, pf)
    total = db(resp * rear_pb)
    total = total - total[0]
    rear_db = db(rear_pb)
    return Result(worst, src, land, worst_full,
                  float(np.max(np.abs(total))), float(total[-1]),
                  float(rear_db[-1] - rear_db[0]))


def step_verdict(step: float) -> str:
    """Where this step sits against the ring's two bounds (asrc_fill_law + burst ratio)."""
    if step <= 1.0 + 1e-12:
        return "ok"
    if step >= STEP_HARD_MAX:
        return "RING-HARD"
    r = math.floor(step * (BLOCK_FRAMES - 1)) + POLY_AHEAD + 1
    if r + FILL_BLOCK_RESERVE > FILL_TARGET_MAX:
        return "RING-RESERVE"
    return "ok"


GROW_SCALES = (1.0, 1.25, 1.6, 2.1, 2.8)


def solve(fs_in: float, fs_out: float, fp: float, pb_target: float,
          q31: bool = False, want_all: bool = False):
    """Cheapest candidate meeting the alias gate and the passband target.

    Each STRUCTURE is grown independently -- the tap law sizes it, and if the verified alias still
    misses the gate the whole structure is scaled up and re-verified.  A structure whose PASSBAND
    fails is abandoned at once: more taps make a lowpass sharper, never flatter past its edge, and
    the passband ceiling here is usually the rear resampler's fc, which taps cannot move at all.
    """
    labels = [c.label for c in build_candidates(fs_in, fs_out, fp, scale=1.0)]
    best = None
    found = []
    for idx, _label in enumerate(labels):
        for scale in GROW_SCALES:
            cands = build_candidates(fs_in, fs_out, fp, scale=scale)
            if idx >= len(cands):
                break
            ch = cands[idx]
            if step_verdict(ch.step) != "ok":
                break
            r = evaluate(ch, fs_in, fs_out, fp, q31=q31)
            if r.passband_err_db > pb_target:
                break               # taps cannot fix flatness -- see the docstring
            if r.worst_alias_db > ALIAS_GATE_DB:
                continue            # grow
            found.append((ch, r))
            if best is None or ch.macs_per_sec < best[0].macs_per_sec:
                best = (ch, r)
            break
    return (best, found) if want_all else best


def load_percent(macs_per_sec: float, channels: int, floor: bool = False) -> float:
    """Front-end share of the 200 MHz core, which is also its share of the leg-A block window."""
    k = CYC_PER_MAC_LO if floor else CYC_PER_MAC
    return 100.0 * k * channels * macs_per_sec / CPU_HZ


# ------------------------------------------------------------------------ the shipping plan table
#
# APP_ASRC_LPF1TO1_PROTOTYPE.  The build on the board carries it, so its two rows ARE part of the
# as-built image and current_chain has to answer for them -- Phase G found this table calling the
# 44.1 -> 16 kHz row "not in the shipping table" while the hardware was running it, which would
# have labelled the image as a hypothetical in the qualification sweep.
#
# Set False to model a production image without the prototype gate.
LPF1TO1_PROTOTYPE = True

_LPF1TO1_INC = ROOT / "src/app/apps/asrc/asrc_lpf1to1_coeffs.inc"
_LPF1TO1_TAPS: dict = {}


def lpf1to1_taps(macro: str) -> int:
    """A tap count out of the GENERATED include -- the image, not a re-derivation."""
    global _LPF1TO1_TAPS
    if not _LPF1TO1_TAPS:
        txt = _LPF1TO1_INC.read_text(encoding="ascii")
        for m in re.finditer(r"#define\s+(ASRC_\w*_TAPS)\s*\((\d+)u\)", txt):
            _LPF1TO1_TAPS[m.group(1)] = int(m.group(2))
    return _LPF1TO1_TAPS[macro]


# The two prototype rows, exactly as asrc_audio_path_frontend_plan() publishes them.  Fp and
# Fstop are DERIVED from the uniform spec here for the same reason the generator derives them:
# a rate must not be able to acquire its own specification by being written down twice.
PROTOTYPE_ROWS = {
    (32000, 16000): ("1:1@32k (proto)", (), "ASRC_LP1_32K_TO_16K_TAPS"),
    (44100, 16000): ("/2 + 1:1@22.05k (proto)", (2,),
                     "ASRC_LP1_22K05_TO_16K_TAPS"),
}


def current_plan(fs_in: int, fs_out: int):
    """Reproduce asrc_audio_path_frontend_plan() for APP_BUILD_ASRC_CODEC_BIDIR.

    That preset has APP_ASRC_RUNTIME_48K_TO_8 = 1 and APP_USE_96K_RATE = 1, so every row of the
    table is live.  Returns (num, den, tag) with num/den the PUBLISHED front-end ratio; the leg
    that carries it is the one running at fs_in.
    """
    if LPF1TO1_PROTOTYPE and (fs_in, fs_out) in PROTOTYPE_ROWS:
        tag, combo, _macro = PROTOTYPE_ROWS[(fs_in, fs_out)]
        den = 1
        for c in combo:
            den *= c
        return 1, den, tag
    if fs_in == 96000:
        den = {8000: 12, 11025: 8, 12000: 8, 16000: 6, 22050: 4, 24000: 4,
               32000: 6, 44100: 2, 48000: 2}.get(fs_out, 1)
        num = 2 if fs_out == 32000 else 1
        if den == 1:
            return 1, 1, "direct"
        tag = {12: "/12", 8: "/8", 6: "/6", 4: "/4", 2: "/2"}[den]
        if fs_out == 32000:
            tag = "/2 + 2/3 (:audio)"
        elif fs_out == 44100:
            tag = "/2 :44k1 (%dt wide)" % dsg.PRESTAGE44K1_TAPS
        elif fs_out == 48000:
            tag = "/2 :48k (%dt wide)" % dsg.PRESTAGE48K_TAPS
        return num, den, tag
    if fs_in == 48000:
        den = {8000: 6, 11025: 4, 12000: 4, 16000: 3, 22050: 2, 24000: 2, 32000: 3}.get(fs_out, 1)
        num = 2 if fs_out == 32000 else 1
        if den == 1:
            return 1, 1, "direct"
        tag = {6: "/6", 4: "/4", 3: "/3", 2: "/2"}[den]
        if fs_out == 32000:
            tag = "2/3 N97 (:audio)"
        elif fs_out == 11025:
            tag = "/4 :11k"
        elif fs_out == 12000:
            tag = "/4 :12k"
        elif fs_out == 22050:
            tag = "/2 :22k05"
        return num, den, tag
    return 1, 1, "direct"


def current_chain(fs_in: int, fs_out: int, fp: float):
    """Build the SHIPPING chain for a pair, with the shipped tap counts and stopbands.

    Only the rows the table actually publishes get stages; everything else is `direct`.  Tap counts
    and stopband edges are the values in asrc_decimator_48_to_8_design.py, not re-derived.
    """
    num, den, tag = current_plan(fs_in, fs_out)

    # The prototype rows FIRST: a 1:1 stage publishes num == den == 1, so the "direct" test
    # below cannot tell it apart from having no front end at all.  That is the same blind spot
    # asrc_audio_path.c had to fix by adding a KIND field, and for the same reason.
    if LPF1TO1_PROTOTYPE and (fs_in, fs_out) in PROTOTYPE_ROWS:
        _tag, combo, lp_macro = PROTOTYPE_ROWS[(fs_in, fs_out)]
        fp_u = 0.8125 * fs_out / 2.0
        fstop_u = fs_out - fp_u                      # the 1:1 stage stopband
        stages = []
        rate = float(fs_in)
        for c in combo:
            # the partner-aware /2: stopband = Fs_mid - Fstop(1:1), the as-built rule
            mid = rate / c
            stages.append(make_stage("dec", rate, fp_u, mid - fstop_u, d=c,
                                     taps=lpf1to1_taps("ASRC_DEC2_44K1_FOR_16K_TAPS")))
            rate = mid
        stages.append(make_stage("lpf", rate, fp_u, fstop_u, d=1,
                                 taps=lpf1to1_taps(lp_macro)))
        fam = "A2" if not combo else "A1+A2"
        return Chain("shipped %s" % tag, fam, stages, rate, float(fs_out)), tag

    if (num, den) == (1, 1):
        return Chain("direct", "direct", [], float(fs_in), float(fs_out)), tag

    stages = []
    rate = float(fs_in)
    if fs_in == 96000:
        # The shipped 96 -> 48 kHz pre-stage: shared 41-tap set, or a wide variant for 44.1/48 kHz.
        #
        # CORRECTED 2026-09-15 (Phase G, G-B).  Two faults, both of the F-C kind -- a row named
        # after the image while describing something else:
        #   1  the two wide variants were SWAPPED.  asrc_decimator_48_to_8_design.py and
        #      asrc_decimator_48_to_8.h agree that FOR_44100 is 113 taps (stopband 25950 =
        #      48000 - 22050) and FOR_48000 is 169 (stopband 24000 = 48000 - 24000), and
        #      asrc_audio_path.c picks them by low_rate_hz.  This function had 169 on 44.1 kHz
        #      and 113 on 48 kHz, which overstated the 44.1 kHz row's MACs by 49.6 % and
        #      understated the 48 kHz row's by 33.1 %.
        #   2  the passband edge was RE-DERIVED from the tap law instead of read.  Both wide sets
        #      are designed at 20000 Hz; the law gave 21993 and 18083 Hz.
        # Every band edge and tap count now comes from the generator constants, so a set that is
        # re-specified moves this table with it.
        if fs_out == 48000:
            taps, pb, sb = (dsg.PRESTAGE48K_TAPS, dsg.PRESTAGE48K_PASSBAND_HZ,
                            dsg.PRESTAGE48K_STOPBAND_HZ)
        elif fs_out == 44100:
            taps, pb, sb = (dsg.PRESTAGE44K1_TAPS, dsg.PRESTAGE44K1_PASSBAND_HZ,
                            dsg.PRESTAGE44K1_STOPBAND_HZ)
        else:
            taps, pb, sb = (dsg.PRESTAGE_TAPS, dsg.PRESTAGE_PASSBAND_HZ,
                            dsg.PRESTAGE_STOPBAND_HZ)
        stages.append(make_stage("dec", 96000.0, pb, sb, d=2, taps=taps))
        rate = 48000.0
        if den == 2:
            return Chain(f"shipped {tag}", "A1", stages, rate, float(fs_out)), tag
        den = den // 2

    if fs_out == 32000:
        stages.append(make_stage("rat", rate, dsg.POLY32_PASSBAND_HZ, dsg.POLY32_STOPBAND_HZ,
                                 L=2, M=3, taps=dsg.POLY32_PROTO_TAPS))
        return Chain(f"shipped {tag}", "A1+rat", stages, rate * 2.0 / 3.0, float(fs_out)), tag

    if den == 6:
        stages.append(make_stage("dec", 48000.0, dsg.PASSBAND_HZ, dsg.STAGE1_STOPBAND_HZ,
                                 d=3, taps=dsg.STAGE1_TAPS))
        stages.append(make_stage("dec", 16000.0, dsg.PASSBAND_HZ, dsg.STAGE2_STOPBAND_HZ,
                                 d=2, taps=dsg.STAGE2_TAPS))
    elif den == 4:
        # Two coefficient sets over ONE structure: the 11.025 kHz and 12 kHz variants share the
        # 27/129 tap counts and differ only in band edges (see the QUARTER_ / QUARTER12K_ blocks).
        if fs_out == 12000:
            pb1, sb1 = dsg.QUARTER12K_PASSBAND_HZ, dsg.QUARTER12K_STAGE1_STOPBAND_HZ
            pb2, sb2 = dsg.QUARTER12K_PASSBAND_HZ, dsg.QUARTER12K_STAGE2_STOPBAND_HZ
        else:
            pb1, sb1 = dsg.QUARTER_PASSBAND_HZ, dsg.QUARTER_STAGE1_STOPBAND_HZ
            pb2, sb2 = dsg.QUARTER_PASSBAND_HZ, dsg.QUARTER_STAGE2_STOPBAND_HZ
        stages.append(make_stage("dec", 48000.0, pb1, sb1, d=2, taps=dsg.QUARTER_STAGE1_TAPS))
        stages.append(make_stage("dec", 24000.0, pb2, sb2, d=2, taps=dsg.QUARTER_STAGE2_TAPS))
    elif den == 3:
        stages.append(make_stage("dec", 48000.0, dsg.MIDRATE_PASSBAND_HZ, dsg.MIDRATE_STOPBAND_HZ,
                                 d=3, taps=dsg.MIDRATE_TAPS))
    elif den == 2:
        # Same structure, two coefficient sets: HALFRATE_ (24 kHz out) and HALFRATE22K_ share the
        # 107 taps and differ only in band edges.
        if fs_out == 22050:
            pb, sb = dsg.HALFRATE22K_PASSBAND_HZ, dsg.HALFRATE22K_STOPBAND_HZ
        else:
            pb, sb = dsg.HALFRATE_PASSBAND_HZ, dsg.HALFRATE_STOPBAND_HZ
        stages.append(make_stage("dec", 48000.0, pb, sb, d=2, taps=dsg.HALFRATE_TAPS))
    else:
        raise ValueError(f"unhandled shipped den {den}")

    return Chain(f"shipped {tag}", "A1", stages, stages[-1].fs_out, float(fs_out)), tag
