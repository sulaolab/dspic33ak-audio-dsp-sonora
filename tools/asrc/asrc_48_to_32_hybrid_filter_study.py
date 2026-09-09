#!/usr/bin/env python3
"""Host-only feasibility study for 48 kHz -> 32 kHz hybrid anti-alias filters.

This deliberately does *not* generate firmware coefficients.  It compares a
48 kHz FIR/IIR/FIR prefilter followed by the unchanged generic ASRC (step
about 1.5) with the shipping N=97 2/3 front end.  The two signal paths have
different alias bookkeeping, so the shipping audio-gate helper is reused for
the N=97 reference and the generic-ASRC path is measured separately.

Run from the repository root:

    python tools/asrc/asrc_48_to_32_hybrid_filter_study.py --json C:/temp/study.json

SciPy is a host-design dependency only.  This script never writes coefficient
includes, presets, or shipping C sources.
"""
from __future__ import annotations

import argparse
import contextlib
import dataclasses
import importlib.util
import io
import json
import math
import pathlib
import sys
from dataclasses import dataclass
from typing import Any, Iterable

import numpy as np
from scipy import signal


ROOT = pathlib.Path(__file__).resolve().parents[2]
FS_IN = 48_000.0
FS_OUT = 32_000.0
NYQ_OUT = FS_OUT / 2.0
CHANNELS = 16
BLOCK_FRAMES = 16
F_ALL = np.arange(0.0, FS_IN / 2.0 + 1.0, 1.0)
F_WANTED = F_ALL[F_ALL <= NYQ_OUT]
PROTECTED_BANDS = (13_000.0, 14_000.0, 15_000.0, 15_250.0, 15_500.0, 15_750.0, 16_000.0)
WANTED_POINTS = (1_000.0, 5_000.0, 10_000.0, 12_000.0, 13_000.0, 14_000.0,
                 15_000.0, 15_250.0, 15_500.0, 15_750.0)
GD_POINTS = (1_000.0, 5_000.0, 10_000.0, 12_000.0, 13_000.0, 14_000.0,
             15_000.0, 15_500.0)


def _load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {relative}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    with contextlib.redirect_stdout(io.StringIO()):
        spec.loader.exec_module(module)
    return module


gate = _load("hybrid_audio_gate", "tools/asrc/asrc_48_to_32_audio_gate.py")
check = _load("hybrid_headroom", "tools/asrc/asrc_headroom_filter_check.py")
design = _load("hybrid_design", "tools/asrc/asrc_decimator_48_to_8_design.py")


def db(value: np.ndarray | float) -> np.ndarray:
    return 20.0 * np.log10(np.maximum(np.asarray(value, dtype=np.float64), 1.0e-15))


def fold(freq: np.ndarray, rate: float) -> np.ndarray:
    rem = np.asarray(freq, dtype=np.float64) % rate
    return np.minimum(rem, rate - rem)


def fir_response(coeff: np.ndarray, sample_rate: float, freqs: np.ndarray) -> np.ndarray:
    omega = 2.0 * np.pi * np.asarray(freqs, dtype=np.float64) / sample_rate
    _, response = signal.freqz(np.asarray(coeff, dtype=np.float64), worN=omega)
    return response


def sos_response(sos: np.ndarray, freqs: np.ndarray) -> np.ndarray:
    omega = 2.0 * np.pi * np.asarray(freqs, dtype=np.float64) / FS_IN
    _, response = signal.sosfreqz(np.asarray(sos, dtype=np.float64), worN=omega)
    return response


def normalize_sos_dc(sos: np.ndarray) -> np.ndarray:
    """Apply the free static gain needed to make the IIR unity at DC."""
    out = np.asarray(sos, dtype=np.float64).copy()
    dc = np.prod((out[:, 0] + out[:, 1] + out[:, 2]) / (out[:, 3] + out[:, 4] + out[:, 5]))
    if not np.isfinite(dc) or abs(dc) < 1.0e-12:
        raise ValueError("non-finite IIR DC gain")
    out[0, :3] /= dc
    return out


def stable_sos(sos: np.ndarray) -> bool:
    if not np.isfinite(sos).all():
        return False
    _, poles, _ = signal.sos2zpk(sos)
    return bool(np.all(np.abs(poles) < 1.0 - 1.0e-10))


def generic_rear_prototype() -> np.ndarray:
    # Exactly the unchanged generic resampler prototype constructed in the host audio gate.
    return gate.rear_prototype()


GENERIC_REAR = generic_rear_prototype()
GENERIC_REAR_RESPONSE = np.abs(fir_response(GENERIC_REAR, check.L * FS_IN, F_ALL))
REAR_32_RESPONSE = np.abs(fir_response(GENERIC_REAR, check.L * FS_OUT, F_ALL))


@dataclass(frozen=True)
class IirDesign:
    label: str
    family: str
    sos: np.ndarray
    pass_edge: float
    stop_edge: float
    ripple: float | None
    attenuation: float | None
    design_mode: str

    @property
    def sections(self) -> int:
        return int(self.sos.shape[0])


@dataclass(frozen=True)
class Candidate:
    label: str
    topology: str
    iir: IirDesign
    fir1: np.ndarray
    fir2: np.ndarray
    fir1_cutoff: float | None
    eq_target: float | None
    eq_boost_db: float | None

    @property
    def fir_taps(self) -> int:
        return int((0 if self.fir1.size == 1 else self.fir1.size) +
                   (0 if self.fir2.size == 1 else self.fir2.size))


@dataclass
class Result:
    candidate: Candidate
    aliases: dict[float, tuple[float, float, float]]
    wanted_db: dict[float, float]
    widths: dict[float, float]
    min_0_to_band: dict[float, float]
    recovered_100: float
    recovered_105: float
    peak_db: float


def sane(result: Result) -> bool:
    """Hard exploration gate: no numerical failure and no gross boosted passband peak."""
    numbers = [result.peak_db, *result.wanted_db.values(),
               *(entry[0] for entry in result.aliases.values())]
    return bool(np.isfinite(numbers).all() and result.peak_db <= 0.5)


IDENTITY_FIR = np.array([1.0], dtype=np.float64)


def make_gentle_fir(taps: int, cutoff: float) -> np.ndarray:
    if taps == 0:
        return IDENTITY_FIR
    # beta=4 is intentionally gentle; this stage is never tasked with the stopband gate.
    return signal.firwin(taps, cutoff, window=("kaiser", 4.0), fs=FS_IN).astype(np.float64)


def make_equalizer(pre_mag: np.ndarray, target_hz: float, boost_db: float, taps: int) -> np.ndarray:
    """Magnitude-only inverse limited to the wanted side of the alias boundary."""
    source_edge = FS_OUT - target_hz
    if not (0.0 < target_hz < source_edge < FS_IN / 2.0):
        raise ValueError("equalizer target has no wanted/alias separation")
    nodes = np.array([0.0, target_hz * 0.50, target_hz * 0.82, target_hz,
                      source_edge, FS_IN / 2.0])
    samples = np.interp(nodes, F_ALL, pre_mag)
    maximum = 10.0 ** (boost_db / 20.0)
    wanted_gain = np.clip(1.0 / np.maximum(samples[:4], 1.0e-12), 1.0, maximum)
    # The two high-frequency nodes stay at unity.  Thus FIR2 cannot intentionally restore an
    # alias source that would fold into the recovered output band.
    gain = np.r_[wanted_gain, 1.0, 1.0]
    coeff = signal.firwin2(taps, nodes / (FS_IN / 2.0), gain, fs=2.0)
    coeff /= np.sum(coeff)
    return coeff.astype(np.float64)


def full_chain_response(candidate: Candidate) -> tuple[np.ndarray, np.ndarray]:
    h1 = fir_response(candidate.fir1, FS_IN, F_ALL)
    hi = sos_response(candidate.iir.sos, F_ALL)
    h2 = fir_response(candidate.fir2, FS_IN, F_ALL)
    hybrid = h1 * hi * h2
    # The generic ASRC runs at a 48 kHz input for every hybrid candidate (step approximately 1.5).
    return hybrid, hybrid * GENERIC_REAR_RESPONSE


def first_crossing(curve_db: np.ndarray, threshold_db: float) -> float:
    below = np.flatnonzero(curve_db <= threshold_db)
    if not below.size:
        return float(NYQ_OUT)
    return float(F_WANTED[int(below[0])])


def evaluate(candidate: Candidate) -> Result:
    hybrid, total = full_chain_response(candidate)
    total_db = db(np.abs(total))
    # Generic 48 -> 32 resampling maps a source above 16 kHz to 32k - source.  Unlike the
    # dedicated 2/3 front end it does not create the 48k-f image that the audio gate handles.
    source = F_ALL > NYQ_OUT
    landing = FS_OUT - F_ALL
    aliases: dict[float, tuple[float, float, float]] = {}
    for band in PROTECTED_BANDS:
        selected = source & (landing >= 0.0) & (landing <= band)
        index = int(np.argmax(np.where(selected, total_db, -np.inf)))
        aliases[band] = (float(total_db[index]), float(F_ALL[index]), float(landing[index]))

    wanted_db = {f: float(np.interp(f, F_ALL, total_db)) for f in WANTED_POINTS}
    widths = {threshold: first_crossing(total_db[:F_WANTED.size], threshold)
              for threshold in (-0.1, -0.5, -1.0, -3.0)}
    min_0_to_band = {band: float(np.min(total_db[:int(band) + 1])) for band in PROTECTED_BANDS}
    recovered_100 = max((band for band in PROTECTED_BANDS
                         if aliases[band][0] <= -100.0 and min_0_to_band[band] >= -1.1), default=0.0)
    recovered_105 = max((band for band in PROTECTED_BANDS
                         if aliases[band][0] <= -105.0 and min_0_to_band[band] >= -1.1), default=0.0)
    return Result(candidate, aliases, wanted_db, widths, min_0_to_band, recovered_100,
                  recovered_105, float(np.max(total_db[:F_WANTED.size])))


def iir_bank() -> list[IirDesign]:
    families = ("butter", "cheby1", "cheby2", "ellip")
    pass_edges = (12_000.0, 13_000.0, 14_000.0, 15_000.0)
    stop_edges = (16_000.0, 16_500.0, 17_000.0, 18_000.0)
    ripples = (0.1, 0.5)
    attenuations = (60.0, 80.0, 100.0)
    out: list[IirDesign] = []

    def append(label: str, family: str, sos: np.ndarray, fp: float, fs: float,
               rp: float | None, rs: float | None, mode: str) -> None:
        try:
            sos = normalize_sos_dc(sos)
        except ValueError:
            return
        if stable_sos(sos) and sos.shape[0] <= 5:
            out.append(IirDesign(label, family, sos, fp, fs, rp, rs, mode))

    # Fixed orders make the 1..5-SOS implementation envelope explicit.
    for family in families:
        for order in (2, 4, 6, 8, 10):
            for fp in pass_edges:
                for rs in attenuations if family in ("cheby2", "ellip") else (None,):
                    for rp in ripples if family in ("cheby1", "ellip") else (None,):
                        try:
                            if family == "butter":
                                sos = signal.butter(order, fp, btype="low", fs=FS_IN, output="sos")
                            elif family == "cheby1":
                                sos = signal.cheby1(order, rp, fp, btype="low", fs=FS_IN, output="sos")
                            elif family == "cheby2":
                                # Type II's Wn is its first stopband edge.
                                sos = signal.cheby2(order, rs, min(stop_edges), btype="low",
                                                    fs=FS_IN, output="sos")
                            else:
                                sos = signal.ellip(order, rp, rs, fp, btype="low", fs=FS_IN,
                                                   output="sos")
                        except ValueError:
                            continue
                        append(f"{family}-fixed-n{order}-fp{fp/1000:g}-rp{rp}-rs{rs}", family,
                               sos, fp, min(stop_edges), rp, rs, "fixed-order")

    # Specification designs sweep both edges.  Only orders representable as <=5 SOS survive.
    for family in families:
        for fp in pass_edges:
            for fs in stop_edges:
                if fs <= fp:
                    continue
                for rp in ripples:
                    for rs in attenuations:
                        try:
                            sos = signal.iirdesign(fp / (FS_IN / 2.0), fs / (FS_IN / 2.0),
                                                   rp, rs, ftype=family, output="sos")
                        except ValueError:
                            continue
                        append(f"{family}-spec-fp{fp/1000:g}-fs{fs/1000:g}-rp{rp}-rs{rs}", family,
                               sos, fp, fs, rp, rs, "spec")
    return out


def score_for_band(result: Result, band: float) -> tuple[float, float, int, int]:
    # Lower is better: clear the alias gate, retain wanted response, then minimize implementation.
    alias = result.aliases[band][0]
    wanted = result.min_0_to_band[band]
    penalty = max(0.0, alias + 100.0) * 10.0 + max(0.0, -1.1 - wanted) * 25.0
    return (penalty, -alias, result.candidate.iir.sections, result.candidate.fir_taps)


def topology_shortlist(results: Iterable[Result], wanted: int = 1) -> list[Result]:
    picked: list[Result] = []
    seen: set[str] = set()
    for family in ("butter", "cheby1", "cheby2", "ellip"):
        subset = [r for r in results if r.candidate.iir.family == family]
        for band in PROTECTED_BANDS[:-1]:
            for result in sorted(subset, key=lambda r, b=band: score_for_band(r, b))[:wanted]:
                if result.candidate.label not in seen:
                    picked.append(result)
                    seen.add(result.candidate.label)
    return picked


def topology_best(results: Iterable[Result]) -> Result:
    all_results = list(results)
    eligible = [r for r in all_results if sane(r)]
    if not eligible:
        raise RuntimeError("no candidate survived the finite/no-gross-peak sanity gate")
    return min(eligible, key=lambda r: (-r.recovered_100, -r.recovered_105,
                                        score_for_band(r, 15_000.0), r.candidate.fir_taps,
                                        r.peak_db))


def best_for_band(results: Iterable[Result], band: float) -> Result:
    eligible = [r for r in results if sane(r)]
    if not eligible:
        raise RuntimeError("no candidate survived the finite/no-gross-peak sanity gate")
    return min(eligible, key=lambda r: score_for_band(r, band))


def h0_candidates(bank: Iterable[IirDesign]) -> list[Candidate]:
    return [Candidate(f"H0/{item.label}", "H0 IIR only", item, IDENTITY_FIR, IDENTITY_FIR,
                      None, None, None) for item in bank]


def h1_candidates(seeds: Iterable[Result], quick: bool) -> list[Candidate]:
    taps = (17, 33, 65) if quick else (9, 17, 25, 33, 49, 65)
    boosts = (6.0, 12.0) if quick else (3.0, 6.0, 9.0, 12.0)
    targets = (13_000.0, 14_000.0, 15_000.0, 15_500.0, 15_750.0)
    out: list[Candidate] = []
    for seed in seeds:
        iir = seed.candidate.iir
        pre = np.abs(sos_response(iir.sos, F_ALL))
        for target in targets:
            for boost in boosts:
                for n in taps:
                    eq = make_equalizer(pre, target, boost, n)
                    out.append(Candidate(f"H1/{iir.label}/eq{n}-b{boost:g}-t{target/1000:g}",
                                         "H1 IIR + FIR equalizer", iir, IDENTITY_FIR, eq,
                                         None, target, boost))
    return out


def h2_candidates(seeds: Iterable[Result], quick: bool) -> list[Candidate]:
    taps = (9, 17, 33) if quick else (9, 17, 25, 33, 49)
    cutoffs = (14_000.0, 15_000.0) if quick else (14_000.0, 15_000.0, 16_000.0)
    out: list[Candidate] = []
    for seed in seeds:
        iir = seed.candidate.iir
        for n in taps:
            for cutoff in cutoffs:
                fir1 = make_gentle_fir(n, cutoff)
                out.append(Candidate(f"H2/{iir.label}/pre{n}-fc{cutoff/1000:g}",
                                     "H2 gentle FIR + IIR", iir, fir1, IDENTITY_FIR,
                                     cutoff, None, None))
    return out


def h3_candidates(seeds: Iterable[Result], quick: bool) -> list[Candidate]:
    taps = (17, 33, 65) if quick else (9, 17, 25, 33, 49, 65)
    boosts = (6.0, 12.0) if quick else (3.0, 6.0, 9.0, 12.0)
    targets = (13_000.0, 14_000.0, 15_000.0, 15_500.0, 15_750.0)
    out: list[Candidate] = []
    for seed in seeds:
        base = seed.candidate
        hpre = np.abs(fir_response(base.fir1, FS_IN, F_ALL) * sos_response(base.iir.sos, F_ALL))
        for target in targets:
            for boost in boosts:
                for n in taps:
                    eq = make_equalizer(hpre, target, boost, n)
                    out.append(Candidate(f"H3/{base.iir.label}/pre{base.fir1.size}-fc{base.fir1_cutoff/1000:g}"
                                         f"/eq{n}-b{boost:g}-t{target/1000:g}",
                                         "H3 gentle FIR + IIR + FIR equalizer", base.iir,
                                         base.fir1, eq, base.fir1_cutoff, target, boost))
    return out


def collapse(candidate: Candidate) -> Candidate:
    if candidate.fir1.size == 1 or candidate.fir2.size == 1:
        raise ValueError("collapse requires two physical FIR stages")
    combined = np.convolve(candidate.fir1, candidate.fir2)
    return Candidate(candidate.label.replace("H3/", "collapsed/"), "IIR + combined FIR",
                     candidate.iir, combined, IDENTITY_FIR, candidate.fir1_cutoff,
                     candidate.eq_target, candidate.eq_boost_db)


def polyphase_reference(coeff96: np.ndarray, label: str) -> dict[str, Any]:
    """Metric used by the shipping audio gate: both zero-stuffing images are aliases."""
    wanted = np.abs(fir_response(coeff96, 96_000.0, F_ALL) * REAR_32_RESPONSE)
    wanted_db = db(wanted)
    aliases: dict[str, dict[str, float]] = {}
    for band in PROTECTED_BANDS:
        best = (-300.0, 0.0, 0.0)
        for image in (F_ALL, FS_IN - F_ALL):
            landing = fold(image, FS_OUT)
            level = db(np.abs(fir_response(coeff96, 96_000.0, image)) *
                       np.interp(landing, F_ALL, REAR_32_RESPONSE))
            is_alias = np.abs(landing - F_ALL) > 0.5
            mask = is_alias & (landing <= band)
            index = int(np.argmax(np.where(mask, level, -np.inf)))
            if level[index] > best[0]:
                best = (float(level[index]), float(F_ALL[index]), float(landing[index]))
        aliases[f"{band:.0f}"] = {"db": best[0], "source_hz": best[1], "landing_hz": best[2]}
    return {"label": label, "aliases": aliases,
            "wanted_db": {f"{f:.0f}": float(np.interp(f, F_ALL, wanted_db)) for f in WANTED_POINTS},
            "widths_hz": {str(t): first_crossing(wanted_db[:F_WANTED.size], t)
                          for t in (-0.1, -0.5, -1.0, -3.0)}}


def section_peak_gains(sos: np.ndarray) -> list[float]:
    grid = np.linspace(0.0, math.pi, 16_385)
    out = []
    for section in sos:
        _, response = signal.sosfreqz(section[None, :], worN=grid)
        out.append(float(np.max(np.abs(response))))
    return out


def df2t_peaks(sos: np.ndarray, stimulus: np.ndarray) -> list[float]:
    """Maximum of DF2T state/input/output values per section, in full-scale input units."""
    state = np.zeros((sos.shape[0], 2), dtype=np.float64)
    peaks = np.zeros(sos.shape[0], dtype=np.float64)
    for sample in stimulus:
        current = float(sample)
        for index, (b0, b1, b2, a0, a1, a2) in enumerate(sos):
            if a0 != 1.0:
                b0, b1, b2, a1, a2 = b0/a0, b1/a0, b2/a0, a1/a0, a2/a0
            z1, z2 = state[index]
            output = b0 * current + z1
            state[index, 0] = b1 * current - a1 * output + z2
            state[index, 1] = b2 * current - a2 * output
            peaks[index] = max(peaks[index], abs(current), abs(output),
                               abs(state[index, 0]), abs(state[index, 1]))
            current = output
    return peaks.tolist()


def headroom(candidate: Candidate) -> dict[str, Any]:
    rng = np.random.default_rng(48_032)
    samples = 8192
    time = np.arange(samples) / FS_IN
    tones = np.concatenate([0.999 * np.sin(2.0 * np.pi * f * time)
                            for f in (1000, 5000, 10_000, 12_000, 14_000, 15_000, 16_000, 18_000)])
    multitone = sum(np.sin(2.0 * np.pi * f * time + phase)
                    for f, phase in ((1000, 0.1), (7000, 0.7), (13_000, 1.1), (15_000, 2.0)))
    multitone = 0.999 * multitone / np.max(np.abs(multitone))
    noise = rng.normal(size=samples)
    noise = 0.999 * noise / np.max(np.abs(noise))
    stimuli = {"full-scale-sine": tones, "multitone": multitone, "white-noise": noise}
    naive: dict[str, float] = {}
    minimized: dict[str, float] = {}
    gains = section_peak_gains(candidate.iir.sos)
    # Feeding lower-peak sections first is a simple documented ordering heuristic, not an MCU claim.
    order = np.argsort(gains)
    ordered = candidate.iir.sos[order]
    for name, data in stimuli.items():
        pre = signal.lfilter(candidate.fir1, [1.0], data)
        naive[name] = float(max(df2t_peaks(candidate.iir.sos, pre)))
        minimized[name] = float(max(df2t_peaks(ordered, pre)))
    return {"section_peak_gain": gains, "naive_peak": naive,
            "peak_minimizing_order": [int(i) for i in order], "ordered_peak": minimized}


def robustness(candidate: Candidate) -> dict[str, Any]:
    sos64 = candidate.iir.sos
    sos32 = sos64.astype(np.float32).astype(np.float64)
    _, poles64, _ = signal.sos2zpk(sos64)
    _, poles32, _ = signal.sos2zpk(sos32)
    max_coeff = float(np.max(np.abs(sos64[:, [0, 1, 2, 4, 5]])))
    # A raw Q1.31 biquad coefficient representation cannot hold coefficients outside [-1, 1).
    # The repository contains no reusable Q31 biquad/kernel or coefficient format, so claiming a
    # scaled representation would invent an implementation contract.
    q31_raw_representable = max_coeff < 1.0
    return {"float64_max_pole_radius": float(np.max(np.abs(poles64))),
            "float32_max_pole_radius": float(np.max(np.abs(poles32))),
            "float32_stable": bool(np.all(np.abs(poles32) < 1.0)),
            "raw_q31_representable": q31_raw_representable,
            "max_abs_non_a0_coefficient": max_coeff,
            "q31_note": ("raw Q1.31 possible for coefficients only; no repo Q31 biquad kernel exists"
                         if q31_raw_representable else
                         "raw Q1.31 cannot represent all SOS coefficients; a scaled/Q-format kernel is required")}


def phase_and_time(candidate: Candidate) -> dict[str, Any]:
    grid = np.linspace(0.0, NYQ_OUT, 16_001)
    response = (fir_response(candidate.fir1, FS_IN, grid) * sos_response(candidate.iir.sos, grid) *
                fir_response(candidate.fir2, FS_IN, grid))
    phase = np.unwrap(np.angle(response))
    gd = -np.gradient(phase, 2.0 * np.pi * grid / FS_IN)
    points = {f"{f:.0f}": {"samples": float(np.interp(f, grid, gd)),
                             "us": float(np.interp(f, grid, gd) / FS_IN * 1.0e6)} for f in GD_POINTS}
    bands = {}
    for edge in (10_000.0, 12_000.0, 14_000.0):
        values = gd[grid <= edge]
        bands[f"0-{edge/1000:g}k"] = {"min_samples": float(np.min(values)),
                                        "max_samples": float(np.max(values)),
                                        "pp_samples": float(np.ptp(values))}
    impulse = signal.lfilter(candidate.fir2, [1.0], signal.sosfilt(candidate.iir.sos,
                         signal.lfilter(candidate.fir1, [1.0], np.r_[1.0, np.zeros(8191)])))
    step = np.cumsum(impulse)
    final = float(step[-1])
    tolerance = max(0.01 * abs(final), 1.0e-12)
    bad = np.flatnonzero(np.abs(step - final) > tolerance)
    settling = int(bad[-1] + 1) if bad.size else 0
    tail = np.flatnonzero(np.abs(impulse) > max(np.max(np.abs(impulse)) * 1.0e-3, 1.0e-15))
    return {"group_delay": {"points": points, "bands": bands},
            "time_domain": {"impulse_peak": float(np.max(np.abs(impulse))),
                            "step_final": final,
                            "step_overshoot": float(np.max(step) - final),
                            "settling_samples_1pct": settling,
                            "impulse_tail_samples_60db": int(tail[-1]) if tail.size else 0}}


def cost_and_memory(result: Result) -> dict[str, Any]:
    candidate = result.candidate
    fir_taps = candidate.fir_taps
    sos = candidate.iir.sections
    fir_macs = BLOCK_FRAMES * CHANNELS * fir_taps
    iir_multiplies = BLOCK_FRAMES * CHANNELS * sos * 5
    iir_adds = BLOCK_FRAMES * CHANNELS * sos * 4
    coeff = 4 * (fir_taps + sos * 5)
    state_per_channel = 4 * (fir_taps + sos * 2)
    return {"fir_macs_per_block": fir_macs, "iir_multiplies_per_block": iir_multiplies,
            "iir_adds_per_block": iir_adds, "iir_sections": sos,
            "generic_asrc_step": 1.5,
            "cpu_note": "estimated arithmetic count only / hardware us unknown; generic ASRC remains at step about 1.5",
            "coeff_bytes_float32": coeff, "coeff_bytes_q31": coeff,
            "state_bytes_per_channel_float32": state_per_channel,
            "state_bytes_16ch_float32": state_per_channel * CHANNELS,
            "state_bytes_per_channel_q31": state_per_channel,
            "state_bytes_16ch_q31": state_per_channel * CHANNELS}


def result_dict(result: Result) -> dict[str, Any]:
    candidate = result.candidate
    return {
        "label": candidate.label, "topology": candidate.topology,
        "iir": {"label": candidate.iir.label, "family": candidate.iir.family,
                "sections": candidate.iir.sections, "pass_edge_hz": candidate.iir.pass_edge,
                "stop_edge_hz": candidate.iir.stop_edge, "ripple_db": candidate.iir.ripple,
                "attenuation_db": candidate.iir.attenuation, "mode": candidate.iir.design_mode},
        "fir1_taps": int(candidate.fir1.size if candidate.fir1.size > 1 else 0),
        "fir2_taps": int(candidate.fir2.size if candidate.fir2.size > 1 else 0),
        "fir_taps_total": candidate.fir_taps, "fir1_cutoff_hz": candidate.fir1_cutoff,
        "eq_target_hz": candidate.eq_target, "eq_max_boost_db": candidate.eq_boost_db,
        "aliases": {f"{key:.0f}": {"db": value[0], "source_hz": value[1], "landing_hz": value[2]}
                    for key, value in result.aliases.items()},
        "wanted_db": {f"{key:.0f}": value for key, value in result.wanted_db.items()},
        "widths_hz": {str(key): value for key, value in result.widths.items()},
        "minimum_wanted_0_to_band_db": {f"{key:.0f}": value for key, value in result.min_0_to_band.items()},
        "recovered_100_hz": result.recovered_100, "recovered_105_hz": result.recovered_105,
        "peak_wanted_db": result.peak_db, "sanity_pass": sane(result),
        "cost_memory": cost_and_memory(result)}


def equalizer_alias_lift(result: Result) -> dict[str, float] | None:
    """Positive values mean FIR2 restored alias energy relative to its no-EQ base."""
    candidate = result.candidate
    if candidate.fir2.size == 1:
        return None
    without_eq = Candidate(candidate.label + "/no-eq", candidate.topology + " (no EQ)",
                           candidate.iir, candidate.fir1, IDENTITY_FIR,
                           candidate.fir1_cutoff, None, None)
    base = evaluate(without_eq)
    return {f"{band:.0f}": result.aliases[band][0] - base.aliases[band][0]
            for band in PROTECTED_BANDS}


def print_summary(results: dict[str, Result]) -> None:
    print("topology   selected candidate (abbreviated)                 recovered(-100)  alias@15k  wanted@15k  FIR  SOS")
    for name, result in results.items():
        label = result.candidate.label[:42]
        print(f"{name:10s} {label:42s} {result.recovered_100/1000:8.2f} kHz "
              f"{result.aliases[15000.0][0]:10.2f} dBc {result.wanted_db[15000.0]:10.2f} dB "
              f"{result.candidate.fir_taps:8d} {result.candidate.iir.sections:4d}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quick", action="store_true", help="reduced FIR/EQ grid for an exploratory run")
    parser.add_argument("--json", type=pathlib.Path, help="write complete host-study results to this path")
    args = parser.parse_args()

    print("[1/6] Build and sanity-check IIR bank", flush=True)
    bank = iir_bank()
    if not bank:
        raise SystemExit("no stable IIR candidates")
    print(f"      stable candidates (<=5 SOS): {len(bank)}", flush=True)

    print("[2/6] H0 IIR-only full-chain sweep", flush=True)
    h0 = [evaluate(candidate) for candidate in h0_candidates(bank)]
    seeds = topology_shortlist(h0)
    print(f"      H0={len(h0)}; family/band seeds={len(seeds)}", flush=True)

    print("[3/6] H1 IIR + FIR equalizer sweep", flush=True)
    h1 = [evaluate(candidate) for candidate in h1_candidates(seeds, args.quick)]
    print(f"      H1={len(h1)}", flush=True)

    print("[4/6] H2 gentle FIR + IIR sweep", flush=True)
    h2 = [evaluate(candidate) for candidate in h2_candidates(seeds, args.quick)]
    h2_seeds = topology_shortlist(h2)
    print(f"      H2={len(h2)}; H2 seeds={len(h2_seeds)}", flush=True)

    print("[5/6] H3 gentle FIR + IIR + FIR equalizer sweep", flush=True)
    h3 = [evaluate(candidate) for candidate in h3_candidates(h2_seeds, args.quick)]
    print(f"      H3={len(h3)}", flush=True)

    best = {"H0": topology_best(h0), "H1": topology_best(h1),
            "H2": topology_best(h2), "H3": topology_best(h3)}
    collapsed_candidate = collapse(best["H3"].candidate)
    collapsed = evaluate(collapsed_candidate)
    hybrid, collapsed_hybrid = full_chain_response(best["H3"].candidate)[0], full_chain_response(collapsed_candidate)[0]
    collapse_error = float(np.max(np.abs(hybrid - collapsed_hybrid)))
    best["collapsed H3"] = collapsed

    print("[6/6] References and deep float/headroom/time checks", flush=True)
    p0, p1, _ = gate.read_inc_rows(gate.INC)
    n97 = gate.prototype_from_rows(p0, p1)
    n161 = design.design(161, 96_000.0, 15_000.0, 16_000.0)
    plain = Candidate("plain-48k-fir-n335", "plain full-rate FIR reference",
                      IirDesign("identity", "identity", np.array([[1., 0., 0., 1., 0., 0.]]),
                                0.0, 0.0, None, None, "identity"),
                      design.design(335, FS_IN, 15_000.0, 16_000.0), IDENTITY_FIR,
                      15_000.0, None, None)
    plain_result = evaluate(plain)
    deep = {name: {"result": result_dict(result), "robustness": robustness(result.candidate),
                   "headroom": headroom(result.candidate), "phase_time": phase_and_time(result.candidate)}
            for name, result in best.items()}
    for name, result in best.items():
        deep[name]["equalizer_alias_lift_db"] = equalizer_alias_lift(result)
    frontiers = {name: {f"{band:.0f}": result_dict(best_for_band(results, band))
                        for band in PROTECTED_BANDS}
                 for name, results in (("H0", h0), ("H1", h1), ("H2", h2), ("H3", h3))}
    payload = {
        "meta": {"repository_root": str(ROOT), "python": sys.version,
                 "numpy": np.__version__, "scipy": __import__("scipy").__version__,
                 "frequency_grid_hz": 1, "source_range_hz": [0, 24_000],
                 "generic_asrc": "unchanged shipping 30-tap resampler at 48 kHz input; step about 1.5",
                 "search": {"quick": args.quick, "iir_bank": len(bank), "h0": len(h0),
                            "h1": len(h1), "h2": len(h2), "h3": len(h3)}},
        "references": {"n97_shipping": polyphase_reference(n97, "N=97 shipping 2/3"),
                       "n161": polyphase_reference(n161, "N=161 2/3 reference"),
                       "plain_full_rate_fir_n335": result_dict(plain_result),
                       "n97_cost_note": "prior host model: 71.7 us/block; 2026-08-23 hardware: +88.2 us front-end effect, with generic ASRC step 1.0",
                       "n161_cost_note": "prior host model: 112.2 us/block at 16 channels, 11 output frames worst-case"},
        "best": {name: result_dict(result) for name, result in best.items()},
        "deep": deep,
        "best_for_each_protected_band": frontiers,
        "collapse": {"complex_response_max_abs_error": collapse_error,
                     "physical_fir_taps": [int(best["H3"].candidate.fir1.size),
                                           int(best["H3"].candidate.fir2.size)],
                     "combined_fir_taps": int(collapsed_candidate.fir1.size),
                     "finding": "frequency response is identical to numerical precision; physical staging needs a headroom/implementation reason"},
        "all_best_by_topology": {name: result_dict(result) for name, result in best.items()}}
    print_summary(best)
    print(f"collapse complex-response max error: {collapse_error:.3e}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
