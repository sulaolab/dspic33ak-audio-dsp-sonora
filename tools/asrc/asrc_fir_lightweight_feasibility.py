#!/usr/bin/env python3
"""Host-only study: can an FIR-led structure be LIGHTER than the shipping N97 front end?

Two questions, deliberately kept apart because they have different answers:

  Q(48->32)  is there an FIR structure that reaches Full-IIR quality at less DSP load?
  Q(96->32)  is there an FIR structure at least 15 us / 96-kHz-block LIGHTER than N97,
             which is what 12 ch would need in order to become feasible?

TWO PATH CONVENTIONS, both taken unchanged from the existing studies:

  poly23  a dedicated L=2 / M=3 polyphase front end (this is what N97 is), followed by the
          unchanged generic ASRC at step 1.0.  The rear therefore sees a 32 kHz input and
          its own prototype (M=30, fc = 0.465 of ITS input) lands at 14.88 kHz.  An input
          at f appears at BOTH f and 48000-f on the 96 kHz axis, each image folds into
          fold(., 32000), and the rear shapes the LANDING frequency.

  pre48   a prefilter running at 48 kHz on every input sample, followed by the unchanged
          generic ASRC at step ~1.5 (this is what Full-IIR is).  The rear sees a 48 kHz
          input, so its fc lands at 22.3 kHz and it is flat across the whole wanted band.
          A source above 16 kHz lands at 32000-f and the rear shapes the SOURCE.

That difference is not cosmetic and it decides this study, so it is measured, not argued.
The alias conventions are the ones already in asrc_48_to_32_hybrid_filter_study.py
(polyphase_reference for poly23, evaluate for pre48); the generic ASRC is that module's
existing host model and no second ASRC model is introduced here.

TWO QUALITY GATES, both applied to every candidate:

  gate15  the brief of 2026-09-06: worst alias into 0-15 kHz <= -105 dBc (preferred) or
          -100 dBc (acceptable), wanted floor over 0-15 kHz >= -0.5 dB / -1.1 dB.
  gate13  the same thresholds over 0-13 kHz, which is what the SHIPPING N97 chain holds.
          N97 itself FAILS gate15 (its wanted floor is -10.1 dB at 15 kHz), so gate13 is
          the only honest yardstick for the "same quality, less CPU" question, and every
          row also reports the wanted response at 13 / 14 / 15 kHz so that a candidate
          which buys its taps back by narrowing the passband cannot hide that trade.

CPU.  Host Python time is never used for anything.  Every cost is an arithmetic count
times a MEASURED cycles-per-MAC, and every verdict is a DELTA against the measured N97
load rather than an absolute prediction.  Anchors and provenance are in ANCHORS below.
Every configured channel is processed; no channel is skipped, hidden or duplicated.
Taps that are mathematically zero (the stretched IFIR model) are not counted as MAC.

Writes no coefficient include, preset or shipping source.  Run from the repository root.
"""
from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import json
import pathlib
import sys
from dataclasses import dataclass, field
from typing import Any, Callable

import numpy as np
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]

FS_IN = 48_000.0
FS_OUT = 32_000.0
FS_PROTO = 96_000.0          # the L=2 interpolated rate of the poly23 structure
NYQ_OUT = FS_OUT / 2.0
BLOCK_FRAMES = 16

ANCHORS: dict[str, Any] = {
    # dsPIC33A issues at most one instruction per CLOCK, so the instruction rate is the
    # 200 MHz PLL1 output -- NOT the repo macro FCY (= PLL1/2 = 100 MHz), which is
    # mis-named and is really the Timer2 tick rate.  1 tick = 2 instruction cycles.
    # Sources: `[internal] report_ak512_fir_kernel_measured_2026-08-21.md` section 4.3 (a 190-tap
    # mirrored kernel is 199 static instructions yet completes in 113 ticks, which is
    # only possible if a tick is longer than one instruction cycle), and the measured
    # 1.012 cycles/MAC is quoted in those 200 MHz instruction cycles.
    # Using 100 MHz here overstates every MAC-derived microsecond by exactly 2x.
    "instr_hz": 200.0e6,
    "q31_cycles_per_mac": 1.012,
    "float_cycles_per_mac": 2.256,
    "cycles_per_mac_source": "[internal] report_ak512_fir_kernel_measured_2026-08-21.md (Q31 mirrored X/Y 1.012 cy/MAC; float wide8 2.256 cy/MAC, 8 ch)",
    "rear_taps": 30,
    "rear_phases": 128,
    "prestage_96_to_48_taps": 41,
    "n97_proto_taps": 97,
    "meas_48_32_8ch_n97_demand_pct": 88.1,
    "meas_48_32_8ch_full_iir_demand_pct": 95.2,
    "meas_96_32_12ch_n97_q31_demand_pct": 99.8,
    "meas_96_96_12ch_q31_demand_pct": 90.0,
    "measurement_source": "[internal] study_ak512_96k32k_12ch_n97_vs_full_iir_2026-09-06.md",
}
US_PER_CYCLE = 1.0e6 / ANCHORS["instr_hz"]
WINDOW_48_US = BLOCK_FRAMES / FS_IN * 1.0e6            # 333.33 us
WINDOW_96_US = BLOCK_FRAMES / FS_PROTO * 1.0e6         # 166.67 us
RESCUE_BAR_US = 15.0                                   # brief section 6
BANDS = (13_000.0, 15_000.0)
WANTED_POINTS = (13_000.0, 14_000.0, 15_000.0)
GATE_ORDER = {"preferred": 0, "acceptable": 1, "fail": 2}


def _load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {relative}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(module)
    return module


phase = _load("lw_phase1", "tools/asrc/asrc_48_to_32_hybrid_filter_study.py")
dsg = _load("lw_design", "tools/asrc/asrc_decimator_48_to_8_design.py")

F_FINE = phase.F_ALL                        # 0..24000 Hz, 1 Hz
F_COARSE = np.arange(0.0, 24_001.0, 10.0)   # search grid
REAR_32_FINE = phase.REAR_32_RESPONSE       # rear with a 32 kHz input (poly23)
REAR_48_FINE = phase.GENERIC_REAR_RESPONSE  # rear with a 48 kHz input (pre48)


def db(value) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.asarray(value, dtype=np.float64), 1.0e-15))


def fold(freq, rate: float) -> np.ndarray:
    rem = np.asarray(freq, dtype=np.float64) % rate
    return np.minimum(rem, rate - rem)


def fir_mag(coeff: np.ndarray, sample_rate: float, freqs: np.ndarray) -> np.ndarray:
    omega = 2.0 * np.pi * np.asarray(freqs, dtype=np.float64) / sample_rate
    _, response = signal.freqz(np.asarray(coeff, dtype=np.float64), worN=omega)
    return np.abs(response)


@dataclass
class Quality:
    alias: dict[float, float]
    floor: dict[float, float]
    peak: dict[float, float]
    worst_source_hz: dict[float, float]
    wanted: dict[float, float]

    def gate(self, band: float) -> str:
        alias, floor, peak = self.alias[band], self.floor[band], self.peak[band]
        if alias <= -105.0 and floor >= -0.5 and peak <= 0.5:
            return "preferred"
        if alias <= -100.0 and floor >= -1.1 and peak <= 1.0:
            return "acceptable"
        return "fail"


def _summarise(wanted_db: np.ndarray, grid: np.ndarray,
               alias_levels: list[tuple[np.ndarray, np.ndarray]]) -> Quality:
    alias: dict[float, float] = {}
    floor: dict[float, float] = {}
    peak: dict[float, float] = {}
    where: dict[float, float] = {}
    for band in BANDS:
        best = (-400.0, 0.0)
        for level, mask in alias_levels:
            selected = mask & (level >= -np.inf)
            selected = mask
            band_mask = selected & (LANDING_CACHE[id(level)] <= band) \
                if id(level) in LANDING_CACHE else selected
            if not band_mask.any():
                continue
            index = int(np.argmax(np.where(band_mask, level, -np.inf)))
            if level[index] > best[0]:
                best = (float(level[index]), float(grid[index]))
        inside = grid <= band
        alias[band] = best[0]
        where[band] = best[1]
        floor[band] = float(np.min(wanted_db[inside]))
        peak[band] = float(np.max(wanted_db[inside]))
    wanted = {f: float(np.interp(f, grid, wanted_db)) for f in WANTED_POINTS}
    return Quality(alias, floor, peak, where, wanted)


LANDING_CACHE: dict[int, np.ndarray] = {}


def quality_poly23(proto96: np.ndarray, grid: np.ndarray) -> Quality:
    rear = np.interp(grid, F_FINE, REAR_32_FINE)
    wanted_db = db(fir_mag(proto96, FS_PROTO, grid) * rear)
    alias: dict[float, float] = {}
    floor: dict[float, float] = {}
    peak: dict[float, float] = {}
    where: dict[float, float] = {}
    images: list[tuple[np.ndarray, np.ndarray]] = []
    for image in (grid, FS_IN - grid):
        landing = fold(image, FS_OUT)
        level = db(fir_mag(proto96, FS_PROTO, image) *
                   np.interp(landing, F_FINE, REAR_32_FINE))
        images.append((level, landing))
    for band in BANDS:
        best = (-400.0, 0.0)
        for level, landing in images:
            mask = (np.abs(landing - grid) > 0.5) & (landing <= band)
            if not mask.any():
                continue
            index = int(np.argmax(np.where(mask, level, -np.inf)))
            if level[index] > best[0]:
                best = (float(level[index]), float(grid[index]))
        inside = grid <= band
        alias[band] = best[0]
        where[band] = best[1]
        floor[band] = float(np.min(wanted_db[inside]))
        peak[band] = float(np.max(wanted_db[inside]))
    wanted = {f: float(np.interp(f, grid, wanted_db)) for f in WANTED_POINTS}
    return Quality(alias, floor, peak, where, wanted)


def quality_pre48(mag48: np.ndarray, grid: np.ndarray) -> Quality:
    total_db = db(mag48 * np.interp(grid, F_FINE, REAR_48_FINE))
    alias: dict[float, float] = {}
    floor: dict[float, float] = {}
    peak: dict[float, float] = {}
    where: dict[float, float] = {}
    landing = FS_OUT - grid
    source = grid > NYQ_OUT
    for band in BANDS:
        mask = source & (landing >= 0.0) & (landing <= band)
        inside = grid <= band
        if mask.any():
            index = int(np.argmax(np.where(mask, total_db, -np.inf)))
            alias[band] = float(total_db[index])
            where[band] = float(grid[index])
        else:
            alias[band] = -400.0
            where[band] = 0.0
        floor[band] = float(np.min(total_db[inside]))
        peak[band] = float(np.max(total_db[inside]))
    wanted = {f: float(np.interp(f, grid, total_db)) for f in WANTED_POINTS}
    return Quality(alias, floor, peak, where, wanted)


def quality_of(coeff: np.ndarray, structure: str, grid: np.ndarray) -> Quality:
    if structure == "poly23":
        return quality_poly23(coeff, grid)
    return quality_pre48(fir_mag(coeff, FS_IN, grid), grid)


# ------------------------------------------------------------------------- cost model
@dataclass
class Candidate:
    label: str
    family: str
    structure: str
    taps: int
    nonzero: int
    quality: Quality
    group_delay_ms: float
    linear_phase: bool
    detail: dict[str, Any] = field(default_factory=dict)

    def nonzero_mac_per_output(self) -> float:
        # poly23 evaluates one polyphase row (half the prototype) per 32 kHz output.
        if self.structure == "pre96hb":
            return float(self.nonzero)
        return self.nonzero / 2.0 if self.structure == "poly23" else float(self.nonzero)

    def front_mac(self, block_input_frames: float) -> float:
        if self.structure == "poly23":
            outputs = block_input_frames * FS_OUT / FS_IN
            return outputs * self.nonzero_mac_per_output()
        return block_input_frames * float(self.nonzero)

    def chain_48_32(self) -> float:
        if self.structure == "pre96hb":
            # a 48 kHz leg input never runs the /2 pre-stage, so this row cannot change it
            outputs = BLOCK_FRAMES * FS_OUT / FS_IN
            return (outputs * ANCHORS["n97_proto_taps"] / 2.0 +
                    outputs * ANCHORS["rear_taps"])
        return (self.front_mac(BLOCK_FRAMES) +
                BLOCK_FRAMES * FS_OUT / FS_IN * ANCHORS["rear_taps"])

    def chain_96_32(self) -> float:
        pre_frames = BLOCK_FRAMES * FS_IN / FS_PROTO
        if self.structure == "pre96hb":
            # sparse /2 pre-stage, N97 front end unchanged, generic rear unchanged
            return (pre_frames * self.nonzero +
                    pre_frames * FS_OUT / FS_IN * ANCHORS["n97_proto_taps"] / 2.0 +
                    BLOCK_FRAMES * FS_OUT / FS_PROTO * ANCHORS["rear_taps"])
        return (pre_frames * ANCHORS["prestage_96_to_48_taps"] +
                self.front_mac(pre_frames) +
                BLOCK_FRAMES * FS_OUT / FS_PROTO * ANCHORS["rear_taps"])


def us_per_block(mac_per_ch: float, channels: int, cycles_per_mac: float) -> float:
    return mac_per_ch * channels * cycles_per_mac * US_PER_CYCLE


# ------------------------------------------------------------------------- designers
def group_delay_ms(coeff: np.ndarray, sample_rate: float, linear: bool) -> float:
    if linear:
        return (coeff.size - 1) / 2.0 / sample_rate * 1000.0
    grid = np.arange(0.0, 15_001.0, 25.0)
    _, gd = signal.group_delay((coeff, [1.0]), w=2.0 * np.pi * grid / sample_rate)
    return float(np.max(gd)) / sample_rate * 1000.0


def equiripple(taps: int, sample_rate: float, passband: float, stopband: float,
               stop_weight: float) -> np.ndarray | None:
    """Remez.  Returns None when it will not converge (it stops working near 400 taps)."""
    if stopband >= sample_rate / 2.0 or passband >= stopband or taps < 9:
        return None
    try:
        with np.errstate(all="ignore"):
            return signal.remez(taps, [0.0, passband, stopband, sample_rate / 2.0],
                                [1.0, 0.0], weight=[1.0, stop_weight], fs=sample_rate)
    except (ValueError, RuntimeError):
        return None


def kaiser_design(taps: int, sample_rate: float, passband: float, stopband: float,
                 beta: float) -> np.ndarray | None:
    """The shipping designer's family, with beta exposed as a search axis.

    dsg.design() is this with KAISER_BETA fixed at 11; a candidate in this family is a
    drop-in regeneration of the existing coefficient include, no new kernel.
    """
    if stopband >= sample_rate / 2.0 or passband >= stopband or taps < 9:
        return None
    center = (passband + stopband) * 0.5
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    coeff = (2.0 * center / sample_rate *
             np.sinc(2.0 * center / sample_rate * offset) * np.kaiser(taps, beta))
    total = float(np.sum(coeff))
    if not np.isfinite(total) or abs(total) < 1.0e-12:
        return None
    return coeff / total


def rear_compensated(taps: int, passband: float, stopband: float,
                     boost_cap_db: float) -> np.ndarray | None:
    """poly23 prototype whose passband pre-compensates the rear's 32 kHz-input droop."""
    if stopband >= FS_PROTO / 2.0 or passband >= stopband or taps < 9:
        return None
    nodes = np.arange(0.0, FS_PROTO / 2.0 + 1.0, 100.0)
    rear = np.interp(np.minimum(nodes, F_FINE[-1]), F_FINE, REAR_32_FINE)
    gain = np.where(nodes <= passband, 1.0 / np.maximum(rear, 1.0e-3), 0.0)
    gain = np.minimum(gain, 10.0 ** (boost_cap_db / 20.0))
    gain[(nodes > passband) & (nodes < stopband)] = 0.0
    try:
        with np.errstate(all="ignore"):
            coeff = signal.firls(taps, nodes, gain, fs=FS_PROTO)
    except (ValueError, RuntimeError):
        return None
    return np.asarray(coeff, dtype=np.float64)


def ifir(model_taps: int, stretch: int, mask_taps: int, sample_rate: float,
         passband: float, stopband: float) -> tuple[np.ndarray, int] | None:
    """Interpolated FIR: a stretched shaping filter convolved with a masking filter.

    The stretched model's zero coefficients are mathematically zero and are not counted.
    Returns (overall impulse response, non-zero MAC of the pair).
    """
    shaped_stop = stopband * stretch
    if shaped_stop >= sample_rate / 2.0 or passband * stretch >= shaped_stop:
        return None
    model = equiripple(model_taps, sample_rate, passband * stretch, shaped_stop, 1.0)
    if model is None:
        return None
    stretched = np.zeros((model_taps - 1) * stretch + 1, dtype=np.float64)
    stretched[::stretch] = model
    mask = equiripple(mask_taps, sample_rate, passband, stopband, 1.0)
    if mask is None:
        return None
    return np.convolve(stretched, mask), model_taps + mask_taps


def minimum_phase_of(coeff: np.ndarray) -> np.ndarray | None:
    try:
        with np.errstate(all="ignore"):
            return signal.minimum_phase(np.asarray(coeff, dtype=np.float64),
                                        method="homomorphic")
    except (ValueError, RuntimeError):
        return None


# ------------------------------------------------ E: sparse (half-band) /2 pre-stage
_G96: Any = None
_G96_PROTO: Any = None
_G96_REAR: Any = None


def _g96():
    """The existing composed 96->32 host gate; nothing about it is re-modelled here."""
    global _G96, _G96_PROTO, _G96_REAR
    if _G96 is None:
        _G96 = _load("lw_g96", "tools/asrc/asrc_96_to_32_audio_gate.py")
        row0, row1, _ = dsg.design_32k_polyphase()
        _G96_PROTO = _G96.gate48.prototype_from_rows(row0, row1)
        _G96_REAR = _G96.rear_prototype()
    return _G96


def halfband(taps: int, beta: float) -> np.ndarray:
    """Windowed-sinc half-band /2 decimator: cutoff is exactly fs/4.

    Every even offset from the centre is then mathematically zero, which is the only
    omission section 7 of the brief allows.  A half-band's passband edge is the mirror of
    its stopband edge, so stop = 32 kHz (the fold edge of a 16 kHz output band) forces
    pass = 16 kHz -- WIDER than the 15 kHz the shipping 41-tap pre-stage was designed for.
    """
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    coeff = 0.5 * np.sinc(0.5 * offset) * np.kaiser(taps, beta)
    coeff[(np.abs(offset % 2.0) < 1.0e-12) & (np.abs(offset) > 1.0e-12)] = 0.0
    return coeff / coeff.sum()


def composed_96_32(prestage: np.ndarray) -> Quality:
    """Quality of prestage -> N97 -> generic rear, two folds deep, shipping model."""
    g = _g96()
    pre_mag = g.mag_at(prestage, g.IN_HZ, g.FREQS)
    mid = g.fold(g.FREQS, g.MID_HZ)
    band = g.FREQS[:16_001]
    wanted_db = db(g.mag_at(prestage, g.IN_HZ, band) *
                   g.mag_at(_G96_PROTO, g.PROTO_HZ, band) *
                   g.mag_at(_G96_REAR, float(g.L_REAR) * g.OUT_HZ, band))
    alias: dict[float, float] = {}
    floor: dict[float, float] = {}
    peak: dict[float, float] = {}
    where: dict[float, float] = {}
    for target in BANDS:
        best, source = -400.0, 0.0
        for image in (mid, g.MID_HZ - mid):
            land = g.fold(image, g.OUT_HZ)
            level = db(pre_mag * g.mag_at(_G96_PROTO, g.PROTO_HZ, image) *
                       g.mag_at(_G96_REAR, float(g.L_REAR) * g.OUT_HZ, land))
            mask = (np.abs(land - g.FREQS) > 0.5) & (land <= target)
            if not mask.any():
                continue
            index = int(np.argmax(np.where(mask, level, -np.inf)))
            if level[index] > best:
                best, source = float(level[index]), float(g.FREQS[index])
        alias[target] = best
        where[target] = source
        inside = int(target)
        floor[target] = float(np.min(wanted_db[:inside + 1]))
        peak[target] = float(np.max(wanted_db[:inside + 1]))
    wanted = {f: float(wanted_db[int(f)]) for f in WANTED_POINTS}
    return Quality(alias, floor, peak, where, wanted)


def sparse_prestage_bank(quick: bool) -> list[Candidate]:
    """Half-band /2 pre-stages that HOLD the shipping composed 96->32 numbers.

    The test is not "passes a threshold" but "is no worse than what ships", so a candidate
    that merely scrapes -105 dBc where the shipping chain reaches -107.3 is rejected.
    """
    g = _g96()
    ship = np.asarray(dsg.design(dsg.PRESTAGE_TAPS, dsg.PRESTAGE_INPUT_HZ,
                                 dsg.PRESTAGE_PASSBAND_HZ, dsg.PRESTAGE_STOPBAND_HZ),
                      dtype=np.float64)
    reference = composed_96_32(ship)
    out: list[Candidate] = []
    for taps in range(27, 68, 4):
        for beta in ((10.0, 11.0) if quick else (9.0, 10.0, 11.0, 12.0)):
            coeff = halfband(taps, beta)
            nonzero = int(np.sum(np.abs(coeff) > 1.0e-12))
            quality = composed_96_32(coeff)
            if quality.alias[13_000.0] > reference.alias[13_000.0] + 0.5:
                continue
            if quality.wanted[13_000.0] < reference.wanted[13_000.0] - 0.02:
                continue
            out.append(Candidate(
                f"E pre96 half-band {taps}t b{beta:g} ({nonzero} nz)",
                "sparse half-band /2 pre-stage, N97 front end unchanged", "pre96hb",
                taps, nonzero, quality, (taps - 1) / 2.0 / FS_PROTO * 1000.0, True,
                {"kaiser_beta": beta, "passband_hz": 16_000.0, "stopband_hz": 32_000.0,
                 "zero_taps": taps - nonzero, "method": "half-band windowed sinc",
                 "replaces": "asrc_decimator_96_to_48_coeffs.inc shared /2 row, 41 taps",
                 "shipping_alias_0_13k_dbc": reference.alias[13_000.0],
                 "shipping_wanted_13k_db": reference.wanted[13_000.0],
                 "quality_model": "asrc_96_to_32_audio_gate.py composed, two folds deep",
                 "kernel_risk": "stride-2 data addressing; the 1.012 cycles/MAC anchor is "
                                "measured on a contiguous mirrored X/Y loop and is NOT "
                                "verified for a half-band loop -- microbench required"}))
    ranked = sorted(out, key=lambda c: c.nonzero)
    return ranked[:6] if quick else ranked


Builder = Callable[[int], "tuple[np.ndarray, int] | None"]


def smallest_passing(make: Builder, structure: str, band: float, wanted: str,
                     lo: int, hi: int, step: int
                     ) -> tuple[int, np.ndarray, int, Quality] | None:
    """Smallest tap count on the search lattice whose gate class reaches `wanted`.

    A design method that refuses to converge counts as "does not pass", so the answer is
    the smallest PASSING AND BUILDABLE point on the lattice, never an extrapolation.
    """
    lattice = list(range(lo, hi + 1, step))
    if not lattice:
        return None
    cache: dict[int, tuple[bool, Any]] = {}

    def ok(index: int) -> tuple[bool, Any]:
        if index not in cache:
            built = make(lattice[index])
            if built is None:
                cache[index] = (False, None)
            else:
                coeff, nonzero = built
                quality = quality_of(coeff, structure, F_COARSE)
                cache[index] = (GATE_ORDER[quality.gate(band)] <= GATE_ORDER[wanted],
                                (coeff, nonzero))
        return cache[index]

    stride = max(1, len(lattice) // 12)
    probes = list(range(0, len(lattice), stride))
    if probes[-1] != len(lattice) - 1:
        probes.append(len(lattice) - 1)
    first = None
    previous = -1
    for index in probes:
        if ok(index)[0]:
            first = index
            break
        previous = index
    if first is None:
        return None
    low, high = previous + 1, first
    while low < high:
        mid = (low + high) // 2
        if ok(mid)[0]:
            high = mid
        else:
            low = mid + 1
    good, payload = ok(low)
    if not good or payload is None:
        return None
    coeff, nonzero = payload
    return lattice[low], coeff, nonzero, quality_of(coeff, structure, F_FINE)


# ------------------------------------------------------------------------ references
def n97_reference() -> Candidate:
    _, _, proto = dsg.design_32k_polyphase()
    proto = np.asarray(proto, dtype=np.float64)
    return Candidate("Current N97 (shipping, measured)", "windowed sinc (Kaiser 11)",
                     "poly23", proto.size, proto.size,
                     quality_poly23(proto, F_FINE),
                     group_delay_ms(proto, FS_PROTO, True), True,
                     {"passband_hz": 15_000.0, "stopband_hz": 16_000.0,
                      "note": "L=2/M=3, phase rows 49/48"})


def full_iir_reference() -> tuple[Candidate, int] | None:
    record = ROOT / "tools/asrc/asrc_48_to_32_full_iir_feasibility_2026-09-05.json"
    if not record.exists():
        return None
    best = json.loads(record.read_text(encoding="utf-8"))["best"]
    sos = np.asarray(best["coefficients"]["sos_float64"], dtype=np.float64)
    mag = np.abs(phase.sos_response(sos, F_FINE))
    grid = np.arange(0.0, 15_001.0, 25.0)
    _, gd = signal.group_delay(signal.sos2tf(sos), w=2.0 * np.pi * grid / FS_IN)
    return (Candidate("Current Full-IIR 6 SOS (measured)", "elliptic IIR DF2T float",
                      "pre48", 0, 0, quality_pre48(mag, F_FINE),
                      float(np.max(gd)) / FS_IN * 1000.0, False,
                      {"sos": int(sos.shape[0]), "design": best.get("label")}),
            int(sos.shape[0]))


def iir_cycles_per_sos_sample(n97: Candidate) -> float:
    """Calibrate the float DF2T biquad from the 2026-09-06 hardware method delta.

    Switching N97 -> Full-IIR at 8 ch cost a measured +23.7 us/block while REMOVING the
    whole 2/3 front end, so the biquad bank must account for the delta plus what was
    removed.  This keeps the Full-IIR row anchored to hardware, not to a model.
    """
    channels = 8
    delta_us = (ANCHORS["meas_48_32_8ch_full_iir_demand_pct"] -
                ANCHORS["meas_48_32_8ch_n97_demand_pct"]) / 100.0 * WINDOW_48_US
    removed_us = us_per_block(n97.front_mac(BLOCK_FRAMES), channels,
                              ANCHORS["float_cycles_per_mac"])
    return (delta_us + removed_us) / US_PER_CYCLE / (6 * BLOCK_FRAMES * channels)


# ------------------------------------------------------------------------------ bank
def build_bank(quick: bool) -> list[Candidate]:
    out: list[Candidate] = []
    step = 4 if quick else 2
    hi_poly = 401                      # remez stops converging above this at 96 kHz
    hi_pre = 401 if quick else 601
    edges = (15_000.0, 14_000.0) if quick else (15_000.0, 14_000.0, 13_500.0, 13_000.0)
    weights = (1.0, 100.0) if quick else (1.0, 10.0, 100.0, 300.0, 1000.0)

    for band in BANDS:
        stop = FS_OUT - band                  # first source frequency that lands in band
        for wanted in ("preferred", "acceptable"):
            for edge in edges:
                if edge >= stop:
                    continue
                for weight in weights:
                    # --- A: weighted equiripple linear phase, dedicated 2/3 -----------
                    def make_a(t: int, s=stop, w=weight, e=edge):
                        c = equiripple(t, FS_PROTO, e, s, w)
                        return None if c is None else (c, t)

                    found = smallest_passing(make_a, "poly23", band, wanted,
                                             33, hi_poly, 2 * step)
                    if found is not None:
                        taps, coeff, nonzero, quality = found
                        out.append(Candidate(
                            f"A poly23 equirip {band/1000:.0f}k/{wanted[:4]}"
                            f"/pass{edge/1000:g}k/w{weight:g}",
                            "equiripple FIR, dedicated 2/3", "poly23", taps, nonzero,
                            quality, group_delay_ms(coeff, FS_PROTO, True), True,
                            {"passband_hz": edge, "stopband_hz": stop,
                             "stop_weight": weight, "gate_target": wanted,
                             "gate_band_hz": band, "method": "remez"}))

                    # --- D: short FIR at 48 kHz + the existing generic ASRC -----------
                    def make_d(t: int, s=stop, w=weight, e=edge):
                        c = equiripple(t, FS_IN, e, s, w)
                        return None if c is None else (c, t)

                    found = smallest_passing(make_d, "pre48", band, wanted,
                                             21, hi_pre, step)
                    if found is not None:
                        taps, coeff, nonzero, quality = found
                        out.append(Candidate(
                            f"D pre48 equirip {band/1000:.0f}k/{wanted[:4]}"
                            f"/pass{edge/1000:g}k/w{weight:g}",
                            "equiripple FIR at 48 kHz + generic ASRC", "pre48", taps,
                            nonzero, quality, group_delay_ms(coeff, FS_IN, True), True,
                            {"passband_hz": edge, "stopband_hz": stop,
                             "stop_weight": weight, "gate_target": wanted,
                             "gate_band_hz": band, "method": "remez"}))

                # --- A-K: the SHIPPING designer family with beta as a search axis -----
                for beta in ((9.0, 11.0) if quick else (8.0, 9.0, 10.0, 11.0)):
                    def make_ak(t: int, s=stop, b=beta, e=edge):
                        c = kaiser_design(t, FS_PROTO, e, s, b)
                        return None if c is None else (c, t)

                    found = smallest_passing(make_ak, "poly23", band, wanted,
                                             33, 601, 2 * step)
                    if found is not None:
                        taps, coeff, nonzero, quality = found
                        out.append(Candidate(
                            f"A-K poly23 kaiser{beta:g} {band/1000:.0f}k/{wanted[:4]}"
                            f"/pass{edge/1000:g}k",
                            "windowed sinc, drop-in .inc regeneration", "poly23", taps,
                            nonzero, quality, group_delay_ms(coeff, FS_PROTO, True), True,
                            {"passband_hz": edge, "stopband_hz": stop, "kaiser_beta": beta,
                             "gate_target": wanted, "gate_band_hz": band,
                             "method": "kaiser"}))

            # --- A': poly23 with the rear droop pre-compensated in the prototype ------
            for boost in (7.0, 12.0):
                def make_comp(t: int, s=stop, b=boost):
                    c = rear_compensated(t, 15_000.0, s, b)
                    return None if c is None else (c, t)

                found = smallest_passing(make_comp, "poly23", band, wanted,
                                         33, 801, 2 * step)
                if found is not None:
                    taps, coeff, nonzero, quality = found
                    out.append(Candidate(
                        f"A' poly23 rear-comp {band/1000:.0f}k/{wanted[:4]}"
                        f"/boost{boost:g}dB",
                        "least-squares FIR, rear pre-compensated", "poly23", taps,
                        nonzero, quality, group_delay_ms(coeff, FS_PROTO, True), True,
                        {"passband_hz": 15_000.0, "stopband_hz": stop,
                         "boost_cap_db": boost, "gate_target": wanted,
                         "gate_band_hz": band, "method": "firls"}))

            # --- B: minimum phase for the same magnitude requirement ------------------
            for structure, fs, lo in (("poly23", FS_PROTO, 33), ("pre48", FS_IN, 21)):
                def make_min(t: int, s=stop, f=fs):
                    linear = equiripple(2 * t - 1, f, 15_000.0, s, 1.0)
                    if linear is None:
                        return None
                    c = minimum_phase_of(linear)
                    return None if c is None else (c, c.size)

                found = smallest_passing(make_min, structure, band, wanted,
                                         lo, 201, step)
                if found is not None:
                    taps, coeff, nonzero, quality = found
                    out.append(Candidate(
                        f"B {structure} min-phase {band/1000:.0f}k/{wanted[:4]}",
                        "minimum-phase FIR", structure, coeff.size, coeff.size, quality,
                        group_delay_ms(coeff, fs, False), False,
                        {"linear_source_taps": 2 * taps - 1, "stopband_hz": stop,
                         "gate_target": wanted, "gate_band_hz": band,
                         "method": "remez + homomorphic"}))

        # --- C: IFIR, ranked by NON-ZERO MAC rather than by total taps ---------------
        stretches = (2, 3) if quick else (2, 3, 4, 5, 6)
        models = range(21, 201, 20 if quick else 10)
        for structure, fs in (("poly23", FS_PROTO), ("pre48", FS_IN)):
            best: tuple[float, Candidate] | None = None
            for stretch in stretches:
                for model_taps in models:
                    found = smallest_passing(
                        lambda t, st=stretch, m=model_taps, f=fs, s=stop:
                            ifir(m, st, t, f, 15_000.0, s),
                        structure, band, "acceptable", 21, 301, 2 * step)
                    if found is None:
                        continue
                    mask_taps, coeff, nonzero, quality = found
                    cand = Candidate(
                        f"C {structure} IFIR x{stretch} {band/1000:.0f}k",
                        "IFIR (stretched model + masking)", structure, coeff.size,
                        nonzero, quality, group_delay_ms(coeff, fs, True), True,
                        {"stretch": stretch, "model_taps": model_taps,
                         "mask_taps": mask_taps, "stopband_hz": stop,
                         "gate_band_hz": band, "gate_class": quality.gate(band),
                         "method": "remez x2"})
                    key = cand.nonzero_mac_per_output()
                    if best is None or key < best[0]:
                        best = (key, cand)
            if best is not None:
                out.append(best[1])
    return out


# ---------------------------------------------------------------------------- report
def rescue_band(delta_us: float) -> str:
    saving = -delta_us
    if saving >= RESCUE_BAR_US:
        return ">=15us microbench candidate"
    if saving >= 12.0:
        return "12-15us borderline"
    if saving >= 8.0:
        return "8-12us reserve only"
    if saving > 0.0:
        return "<8us insufficient"
    return "heavier than N97"


def rows_for(candidates: list[Candidate], n97: Candidate, full_iir: Candidate | None,
             sos_count: int, iir_cost: float) -> list[dict[str, Any]]:
    base48 = n97.chain_48_32()
    base96 = n97.chain_96_32()
    rows: list[dict[str, Any]] = []

    def row(cand: Candidate, mac48: float, mac96: float, note: str = "") -> dict[str, Any]:
        d48 = us_per_block(mac48 - base48, 8, ANCHORS["float_cycles_per_mac"])
        d96 = us_per_block(mac96 - base96, 12, ANCHORS["q31_cycles_per_mac"])
        return {
            "label": cand.label, "family": cand.family, "structure": cand.structure,
            "taps": cand.taps, "nonzero_taps": cand.nonzero,
            "nonzero_mac_per_output": cand.nonzero_mac_per_output(),
            "gate_0_13k": cand.quality.gate(13_000.0),
            "gate_0_15k": cand.quality.gate(15_000.0),
            "alias_0_13k_dbc": cand.quality.alias[13_000.0],
            "floor_0_13k_db": cand.quality.floor[13_000.0],
            "alias_0_15k_dbc": cand.quality.alias[15_000.0],
            "floor_0_15k_db": cand.quality.floor[15_000.0],
            "peak_0_15k_db": cand.quality.peak[15_000.0],
            "wanted_13k_db": cand.quality.wanted[13_000.0],
            "wanted_14k_db": cand.quality.wanted[14_000.0],
            "wanted_15k_db": cand.quality.wanted[15_000.0],
            "worst_alias_source_hz_15k": cand.quality.worst_source_hz[15_000.0],
            "group_delay_ms": cand.group_delay_ms,
            "linear_phase": cand.linear_phase,
            "mac_per_ch_block_48_32": mac48,
            "mac_per_ch_block_96_32": mac96,
            "delta_us_block_48_32_8ch_float": d48,
            "delta_us_block_96_32_12ch_q31": d96,
            "est_demand_48_32_8ch_pct":
                ANCHORS["meas_48_32_8ch_n97_demand_pct"] + d48 / WINDOW_48_US * 100.0,
            "est_demand_96_32_12ch_pct":
                ANCHORS["meas_96_32_12ch_n97_q31_demand_pct"] + d96 / WINDOW_96_US * 100.0,
            "rescue_band": rescue_band(d96),
            "detail": cand.detail | ({"note": note} if note else {}),
        }

    rows.append(row(n97, base48, base96,
                    "measured baseline: 88.1 % at 8 ch, 99.8 % at 96/32 12 ch"))
    if full_iir is not None:
        eq_mac = sos_count * iir_cost / ANCHORS["float_cycles_per_mac"]
        mac48 = BLOCK_FRAMES * eq_mac + BLOCK_FRAMES * FS_OUT / FS_IN * ANCHORS["rear_taps"]
        pre_frames = BLOCK_FRAMES * FS_IN / FS_PROTO
        mac96 = (pre_frames * ANCHORS["prestage_96_to_48_taps"] + pre_frames * eq_mac +
                 BLOCK_FRAMES * FS_OUT / FS_PROTO * ANCHORS["rear_taps"])
        rows.append(row(full_iir, mac48, mac96,
                        f"measured 95.2 % at 8 ch; {iir_cost:.1f} float cycles per "
                        f"SOS-sample back-solved from that measurement"))
    for cand in candidates:
        note = ("the 48->32 leg has no /2 pre-stage, so this row changes only the 96 kHz leg"
                if cand.structure == "pre96hb" else "")
        rows.append(row(cand, cand.chain_48_32(), cand.chain_96_32(), note))
    return rows


def print_report(rows: list[dict[str, Any]], iir_cost: float) -> None:
    print("=" * 128)
    print("FIR-led lightweight feasibility, 48->32 and 96->32 kHz  (host study, no MCU)")
    print("=" * 128)
    print(f"block windows: 48 kHz {WINDOW_48_US:.2f} us / 96 kHz {WINDOW_96_US:.2f} us")
    print(f"float DF2T biquad calibrated from hardware: {iir_cost:.2f} cycles/SOS-sample")
    print(f"12 ch rescue bar: at least {RESCUE_BAR_US:.0f} us LESS per 96 kHz block than N97")
    print()
    head = (f"{'candidate':<44}{'gate13':>10}{'gate15':>10}{'taps':>6}{'nzMAC/o':>8}"
            f"{'d48us':>8}{'d96us':>8}{'w13':>7}{'w14':>7}{'w15':>7}{'GDms':>6}")
    print(head)
    print("-" * len(head))
    for item in rows:
        print(f"{item['label'][:43]:<44}{item['gate_0_13k']:>10}{item['gate_0_15k']:>10}"
              f"{item['taps']:>6}{item['nonzero_mac_per_output']:>8.1f}"
              f"{item['delta_us_block_48_32_8ch_float']:>+8.1f}"
              f"{item['delta_us_block_96_32_12ch_q31']:>+8.1f}"
              f"{item['wanted_13k_db']:>7.2f}{item['wanted_14k_db']:>7.2f}"
              f"{item['wanted_15k_db']:>7.2f}{item['group_delay_ms']:>6.2f}")
    print()
    print("--- lighter than N97 while holding N97 quality (gate13) ---------------------")
    kept = [r for r in rows
            if r["delta_us_block_96_32_12ch_q31"] < 0.0
            and r["gate_0_13k"] in ("preferred", "acceptable")
            and not r["label"].startswith("Current")]
    if not kept:
        print("  none")
    for item in sorted(kept, key=lambda r: r["delta_us_block_96_32_12ch_q31"]):
        print(f"  {item['label'][:50]:<52}{item['delta_us_block_96_32_12ch_q31']:+8.1f} us"
              f"  w15={item['wanted_15k_db']:+6.2f} dB  {item['rescue_band']}")
    print()
    print("--- gate15 (0-15 kHz) passers, any cost ------------------------------------")
    strict = [r for r in rows if r["gate_0_15k"] in ("preferred", "acceptable")]
    if not strict:
        print("  none")
    for item in sorted(strict, key=lambda r: r["delta_us_block_48_32_8ch_float"]):
        print(f"  {item['label'][:50]:<52}{item['gate_0_15k']:>11}"
              f"  48/32 8ch {item['delta_us_block_48_32_8ch_float']:+8.1f} us"
              f"  est {item['est_demand_48_32_8ch_pct']:.1f} %")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()

    n97 = n97_reference()
    reference = full_iir_reference()
    full_iir, sos_count = reference if reference is not None else (None, 0)
    iir_cost = iir_cycles_per_sos_sample(n97)
    bank = build_bank(args.quick) + sparse_prestage_bank(args.quick)
    rows = rows_for(bank, n97, full_iir, sos_count, iir_cost)
    print_report(rows, iir_cost)
    if args.json:
        args.json.write_text(json.dumps(
            {"anchors": ANCHORS, "iir_cycles_per_sos_sample": iir_cost,
             "window_48_us": WINDOW_48_US, "window_96_us": WINDOW_96_US,
             "rescue_bar_us_per_96k_block": RESCUE_BAR_US, "rows": rows},
            ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
