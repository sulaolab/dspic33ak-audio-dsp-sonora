#!/usr/bin/env python3
"""Host-only Phase 4: 48 kHz full-IIR + unchanged generic ASRC feasibility.

The signal path under test is intentionally only:

    48 kHz -> IIR anti-alias LPF -> existing generic ASRC (step ~= 1.5) -> 32 kHz

No firmware coefficient include, preset, or shipping source is created or modified.
The full-chain alias/wanted convention is imported from the existing Phase-1
hybrid study; in particular, an input source above 16 kHz lands at 32 kHz - f.

The search is deliberately bounded: fixed-order 2..8 SOS references for all
four families, plus specification designs only for Chebyshev-II and elliptic
(the two stopband-efficient families).  A 10-Hz coarse pass selects a bounded
set for the existing 1-Hz full-chain metric.  It is a feasibility study, not a
global filter optimiser.
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any

import numpy as np
from scipy import signal


ROOT = pathlib.Path(__file__).resolve().parents[2]
PHASE1_PATH = ROOT / "tools" / "asrc" / "asrc_48_to_32_hybrid_filter_study.py"

FS_IN = 48_000.0
FS_OUT = 32_000.0
NYQ_OUT = FS_OUT / 2.0
SOS_COUNTS = tuple(range(2, 9))
SUMMARY_SOS = tuple(range(3, 9))
PASS_EDGES = (13_500.0, 14_000.0, 14_500.0, 15_000.0)
STOP_EDGES = (16_000.0, 16_250.0, 16_500.0, 17_000.0, 18_000.0)
RIPPLES = (0.05, 0.1, 0.25, 0.5, 1.0)
ATTENUATIONS = (40.0, 60.0, 80.0, 100.0, 120.0)
WANTED_POINTS = (1_000.0, 5_000.0, 10_000.0, 12_000.0, 13_000.0,
                 14_000.0, 14_500.0, 15_000.0, 15_250.0, 15_500.0)
PAIRS = ((13_000.0, 19_000.0), (14_000.0, 18_000.0), (15_000.0, 17_000.0))


def load_phase1() -> Any:
    spec = importlib.util.spec_from_file_location("phase4_phase1", PHASE1_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {PHASE1_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


phase = load_phase1()


@dataclass(frozen=True)
class Design:
    label: str
    family: str
    mode: str
    sos_count: int
    pass_hz: float | None
    stop_hz: float | None
    ripple_db: float | None
    attenuation_db: float | None
    sos: np.ndarray


def normalized_stable(sos: np.ndarray) -> np.ndarray | None:
    try:
        out = phase.normalize_sos_dc(np.asarray(sos, dtype=np.float64))
    except ValueError:
        return None
    if not phase.stable_sos(out):
        return None
    return out


def make_design(label: str, family: str, mode: str, pass_hz: float | None,
                stop_hz: float | None, ripple_db: float | None,
                attenuation_db: float | None, sos: np.ndarray) -> Design | None:
    out = normalized_stable(sos)
    if out is None:
        return None
    sections = int(out.shape[0])
    if sections not in SOS_COUNTS:
        return None
    return Design(label, family, mode, sections, pass_hz, stop_hz, ripple_db,
                  attenuation_db, out)


def design_bank() -> list[Design]:
    """Build a bounded, family-balanced bank; leave exact scoring to full-chain metrics."""
    out: list[Design] = []
    seen: set[tuple[Any, ...]] = set()

    def add(design: Design | None) -> None:
        if design is None:
            return
        # Equivalent coefficient sets add no spectral coverage.
        key = (design.family, tuple(np.round(design.sos.ravel(), 12)))
        if key in seen:
            return
        seen.add(key)
        out.append(design)

    for sections in SOS_COUNTS:
        order = sections * 2
        for fp in PASS_EDGES:
            add(make_design(
                f"butter/fixed/n{order}/fp{fp:g}", "butter", "fixed", fp, None,
                None, None, signal.butter(order, fp, btype="low", fs=FS_IN, output="sos")))
            for rp in RIPPLES:
                add(make_design(
                    f"cheby1/fixed/n{order}/fp{fp:g}/rp{rp:g}", "cheby1", "fixed",
                    fp, None, rp, None,
                    signal.cheby1(order, rp, fp, btype="low", fs=FS_IN, output="sos")))
            for rp in RIPPLES:
                for rs in ATTENUATIONS:
                    add(make_design(
                        f"ellip/fixed/n{order}/fp{fp:g}/rp{rp:g}/rs{rs:g}",
                        "ellip", "fixed", fp, None, rp, rs,
                        signal.ellip(order, rp, rs, fp, btype="low", fs=FS_IN,
                                     output="sos")))
        for fs in STOP_EDGES:
            for rs in ATTENUATIONS:
                add(make_design(
                    f"cheby2/fixed/n{order}/fs{fs:g}/rs{rs:g}", "cheby2", "fixed",
                    None, fs, None, rs,
                    signal.cheby2(order, rs, fs, btype="low", fs=FS_IN, output="sos")))

    # Only the two sharp families receive a pass/stop specification grid. This complements,
    # rather than replaces, the Butterworth and Chebyshev-I fixed-order references above.
    for family in ("cheby2", "ellip"):
        for fp in PASS_EDGES:
            for fs in STOP_EDGES:
                if fs <= fp:
                    continue
                for rp in RIPPLES:
                    for rs in ATTENUATIONS:
                        try:
                            sos = signal.iirdesign(fp / (FS_IN / 2.0), fs / (FS_IN / 2.0),
                                                   rp, rs, ftype=family, output="sos")
                        except ValueError:
                            continue
                        add(make_design(
                            f"{family}/spec/fp{fp:g}/fs{fs:g}/rp{rp:g}/rs{rs:g}",
                            family, "spec", fp, fs, rp, rs, sos))
    return out


def candidate(design: Design, sos: np.ndarray | None = None) -> Any:
    coeff = design.sos if sos is None else np.asarray(sos, dtype=np.float64)
    iir = phase.IirDesign(design.label, design.family, coeff, design.pass_hz or 0.0,
                          design.stop_hz or 0.0, design.ripple_db,
                          design.attenuation_db, design.mode)
    return phase.Candidate(design.label, "Full-IIR + generic ASRC", iir,
                           phase.IDENTITY_FIR, phase.IDENTITY_FIR, None, None, None)


def coarse(design: Design) -> dict[str, float]:
    """10-Hz version of the exact full-chain convention, used only for selection."""
    freqs = np.arange(0.0, FS_IN / 2.0 + 0.1, 10.0)
    h_iir = phase.sos_response(design.sos, freqs)
    rear = np.interp(freqs, phase.F_ALL, phase.GENERIC_REAR_RESPONSE)
    levels = phase.db(np.abs(h_iir) * rear)
    source = freqs > NYQ_OUT
    landing = FS_OUT - freqs
    selected = source & (landing >= 0.0) & (landing <= 15_000.0)
    alias = float(np.max(levels[selected]))
    wanted = levels[freqs <= 15_000.0]
    return {"alias15": alias, "floor15": float(np.min(wanted)), "peak15": float(np.max(wanted))}


def gate_class(alias: float, floor: float, peak: float) -> str:
    if alias <= -105.0 and floor >= -0.5 and peak <= 0.5:
        return "preferred"
    if alias <= -100.0 and floor >= -1.1 and peak <= 1.0:
        return "acceptable"
    return "fail"


def sort_key(metrics: dict[str, float]) -> tuple[float, ...]:
    alias, floor, peak = metrics["alias15"], metrics["floor15"], metrics["peak15"]
    category = {"preferred": 0.0, "acceptable": 1.0, "fail": 2.0}[gate_class(alias, floor, peak)]
    acceptable_deficit = (max(0.0, alias + 100.0) * 10.0 +
                          max(0.0, -1.1 - floor) * 25.0 +
                          max(0.0, peak - 1.0) * 10.0)
    preferred_deficit = (max(0.0, alias + 105.0) * 10.0 +
                         max(0.0, -0.5 - floor) * 25.0 +
                         max(0.0, peak - 0.5) * 10.0)
    return (category, acceptable_deficit, preferred_deficit, -alias, -floor, peak)


def select_for_fine(bank: list[Design]) -> list[Design]:
    measured = [(item, coarse(item)) for item in bank]
    picked: dict[str, Design] = {}
    for sections in SOS_COUNTS:
        rows = [(item, metric) for item, metric in measured if item.sos_count == sections]
        for family in ("butter", "cheby1", "cheby2", "ellip"):
            family_rows = [(item, metric) for item, metric in rows if item.family == family]
            for item, _ in sorted(family_rows, key=lambda row: sort_key(row[1]))[:3]:
                picked[item.label] = item
        for item, _ in sorted(rows, key=lambda row: sort_key(row[1]))[:12]:
            picked[item.label] = item
        for item, metric in rows:
            if gate_class(metric["alias15"], metric["floor15"], metric["peak15"]) != "fail":
                picked[item.label] = item
    return list(picked.values())


def first_crossing(db_values: np.ndarray, freqs: np.ndarray, threshold: float) -> float:
    at_or_below = np.flatnonzero(db_values <= threshold)
    return float(freqs[int(at_or_below[0])]) if at_or_below.size else float(freqs[-1])


def fine(design: Design) -> dict[str, Any]:
    c = candidate(design)
    result = phase.evaluate(c)
    _, total = phase.full_chain_response(c)
    total_db = phase.db(np.abs(total))
    wanted_axis = phase.F_ALL[phase.F_ALL <= NYQ_OUT]
    wanted_db = total_db[:wanted_axis.size]
    pairs = {}
    for wanted_hz, source_hz in PAIRS:
        alias = float(total_db[int(source_hz)])
        wanted = float(result.wanted_db[wanted_hz])
        pairs[f"{int(source_hz / 1000)}to{int(wanted_hz / 1000)}k"] = {
            "source_hz": source_hz, "landing_hz": wanted_hz, "alias_dbc": alias,
            "wanted_db": wanted, "alias_to_wanted_dbc": alias - wanted,
        }
    aliases = {
        str(int(band)): {"dbc": float(value[0]), "source_hz": float(value[1]),
                         "landing_hz": float(value[2])}
        for band, value in result.aliases.items()
    }
    metadata = {
        "label": design.label, "family": design.family, "mode": design.mode,
        "sos": design.sos_count, "order": design.sos_count * 2,
        "pass_hz": design.pass_hz, "stop_hz": design.stop_hz,
        "ripple_db": design.ripple_db, "attenuation_db": design.attenuation_db,
    }
    metrics = {
        # The shared Phase-1 evaluator retains only its historical wanted-point
        # set.  This study also reports 14.5 kHz, so read every requested point
        # directly from the same 1-Hz full-chain response grid rather than
        # changing the shared helper or silently dropping that required point.
        "wanted_db": {str(int(f)): float(np.interp(f, phase.F_ALL, total_db))
                      for f in WANTED_POINTS},
        "widths_hz": {
            "-0.1": first_crossing(wanted_db, wanted_axis, -0.1),
            "-0.5": first_crossing(wanted_db, wanted_axis, -0.5),
            "-1.0": first_crossing(wanted_db, wanted_axis, -1.0),
            "-1.1": first_crossing(wanted_db, wanted_axis, -1.1),
            "-3.0": first_crossing(wanted_db, wanted_axis, -3.0),
        },
        "aliases": aliases,
        "pairs": pairs,
        "worst_alias_0_to_15_dbc": float(result.aliases[15_000.0][0]),
        "worst_alias_source_hz": float(result.aliases[15_000.0][1]),
        "worst_alias_landing_hz": float(result.aliases[15_000.0][2]),
        "wanted_floor_0_to_15_db": float(result.min_0_to_band[15_000.0]),
        "wanted_peak_0_to_15_db": float(result.peak_db),
    }
    metrics["verdict"] = gate_class(metrics["worst_alias_0_to_15_dbc"],
                                    metrics["wanted_floor_0_to_15_db"],
                                    metrics["wanted_peak_0_to_15_db"])
    return {"design": metadata, "metrics": metrics, "_result": result}


def fine_key(record: dict[str, Any]) -> tuple[float, ...]:
    m = record["metrics"]
    return sort_key({"alias15": m["worst_alias_0_to_15_dbc"],
                     "floor15": m["wanted_floor_0_to_15_db"],
                     "peak15": m["wanted_peak_0_to_15_db"]})


def group_delay(sos: np.ndarray) -> dict[str, Any]:
    freqs = np.arange(0.0, 15_000.0 + 1.0, 1.0)
    response = phase.sos_response(sos, freqs)
    delay_samples = -np.gradient(np.unwrap(np.angle(response)), 2.0 * np.pi * freqs / FS_IN)
    points = {}
    for f in (1_000, 5_000, 10_000, 12_000, 13_000, 14_000, 14_500, 15_000):
        samples = float(delay_samples[f])
        points[str(f)] = {"samples": samples, "us": samples * 1_000_000.0 / FS_IN}
    bands = {}
    for band in (5_000, 10_000, 12_000, 14_000, 15_000):
        part = delay_samples[:band + 1]
        lo, hi = float(np.min(part)), float(np.max(part))
        bands[f"0-{band}"] = {
            "min_samples": lo, "max_samples": hi, "pp_samples": hi - lo,
            "pp_us": (hi - lo) * 1_000_000.0 / FS_IN,
        }
    return {"points": points, "bands": bands}


def pole_radii(sos: np.ndarray) -> list[float]:
    return [float(np.max(np.abs(np.roots(row[3:])))) for row in sos]


def df2t_detail(sos: np.ndarray, samples: np.ndarray) -> dict[str, Any]:
    state = np.zeros((sos.shape[0], 2), dtype=np.float64)
    maximum = [{"input": 0.0, "output": 0.0, "state": 0.0} for _ in range(sos.shape[0])]
    for sample in samples:
        current = float(sample)
        for index, (b0, b1, b2, a0, a1, a2) in enumerate(sos):
            if a0 != 1.0:
                b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
            z1, z2 = state[index]
            output = b0 * current + z1
            next1 = b1 * current - a1 * output + z2
            next2 = b2 * current - a2 * output
            state[index] = (next1, next2)
            maximum[index]["input"] = max(maximum[index]["input"], abs(current))
            maximum[index]["output"] = max(maximum[index]["output"], abs(output))
            maximum[index]["state"] = max(maximum[index]["state"], abs(next1), abs(next2))
            current = output
    return {
        "sections": maximum,
        "overall_peak": float(max(max(maximum[i].values()) for i in range(len(maximum)))),
        "final_output_peak": float(np.max(np.abs(signal.sosfilt(sos, samples)))),
        "finite": bool(np.isfinite(state).all()),
    }


def headroom(sos: np.ndarray) -> dict[str, Any]:
    samples = 16_384
    time = np.arange(samples) / FS_IN
    tones = {
        f"{int(freq / 1000)}k-sine": 0.999 * np.sin(2.0 * np.pi * freq * time)
        for freq in (1_000.0, 10_000.0, 15_000.0, 17_000.0, 20_000.0)
    }
    multitone = sum(np.sin(2.0 * np.pi * f * time + p)
                    for f, p in ((1_000.0, 0.1), (7_000.0, 0.7),
                                 (13_000.0, 1.1), (15_000.0, 2.0)))
    tones["multitone"] = 0.999 * multitone / np.max(np.abs(multitone))
    impulse = np.zeros(samples, dtype=np.float64)
    impulse[0] = 0.999
    tones["full-scale-impulse"] = impulse

    radii = np.asarray(pole_radii(sos))
    orders = {
        "default": np.arange(sos.shape[0]),
        "reverse": np.arange(sos.shape[0])[::-1],
        "low-Q-first": np.argsort(radii),
        "high-Q-first": np.argsort(radii)[::-1],
    }
    rows: dict[str, Any] = {}
    for name, order in orders.items():
        ordered = sos[order]
        trials = {label: df2t_detail(ordered, data) for label, data in tones.items()}
        worst_name, worst = max(trials.items(), key=lambda item: item[1]["overall_peak"])
        rows[name] = {
            "section_order": [int(x) for x in order],
            "worst_stimulus": worst_name,
            "worst_internal_peak": worst["overall_peak"],
            "worst_final_output_peak": worst["final_output_peak"],
            "finite": all(item["finite"] for item in trials.values()),
            "stimuli": trials,
        }
    best_name, best = min(rows.items(), key=lambda item: item[1]["worst_internal_peak"])
    return {
        "per_section_pole_radius": radii.tolist(),
        "orderings": rows,
        "best_ordering": best_name,
        "best": best,
    }


def float32_check(design: Design, reference: dict[str, Any], ordering: list[int]) -> dict[str, Any]:
    sos32 = design.sos.astype(np.float32).astype(np.float64)
    poles = np.concatenate([np.roots(row[3:]) for row in sos32])
    exact32 = fine(dataclasses.replace(design, sos=sos32))
    gd32 = group_delay(sos32)
    selected_headroom = headroom(sos32)["orderings"]
    order_name = next(name for name, value in selected_headroom.items()
                      if value["section_order"] == ordering)
    m64, m32 = reference["metrics"], exact32["metrics"]
    return {
        "stable": bool(np.all(np.abs(poles) < 1.0)),
        "max_pole_radius": float(np.max(np.abs(poles))),
        "max_abs_coefficient": float(np.max(np.abs(sos32))),
        "wanted15_db": m32["wanted_db"]["15000"],
        "wanted15_delta_db": m32["wanted_db"]["15000"] - m64["wanted_db"]["15000"],
        "worst_alias15_dbc": m32["worst_alias_0_to_15_dbc"],
        "worst_alias15_delta_db": m32["worst_alias_0_to_15_dbc"] - m64["worst_alias_0_to_15_dbc"],
        "gd_pp_0_to_10k_us": gd32["bands"]["0-10000"]["pp_us"],
        "gd_pp_0_to_10k_delta_us": (gd32["bands"]["0-10000"]["pp_us"] -
                                     group_delay(design.sos)["bands"]["0-10000"]["pp_us"]),
        "selected_ordering": order_name,
        "selected_ordering_peak": selected_headroom[order_name]["worst_internal_peak"],
    }


def fit_amplitude(samples: np.ndarray, hz: float, skip: int = 2_048) -> float:
    data = samples[skip:]
    time = np.arange(skip, skip + data.size) / FS_OUT
    basis = np.column_stack((np.sin(2.0 * np.pi * hz * time),
                             np.cos(2.0 * np.pi * hz * time)))
    coeff, _, _, _ = np.linalg.lstsq(basis, data, rcond=None)
    return float(np.hypot(coeff[0], coeff[1]))


def time_domain_alias(design: Design, reference: dict[str, Any]) -> dict[str, Any]:
    """Nominal 48->32 resampling: up128/filter/down192 is the L=128 generic path."""
    length = 32_768
    time = np.arange(length) / FS_IN
    rows = {}
    for wanted_hz, source_hz in PAIRS:
        def run(input_hz: float, output_hz: float) -> float:
            source = 0.8 * np.sin(2.0 * np.pi * input_hz * time)
            filtered = signal.sosfilt(design.sos, source)
            output = signal.upfirdn(phase.GENERIC_REAR, filtered, up=128, down=192)
            return fit_amplitude(output, output_hz)

        wanted_amp = run(wanted_hz, wanted_hz)
        alias_amp = run(source_hz, wanted_hz)
        time_ratio = 20.0 * np.log10(max(alias_amp, 1.0e-15) / max(wanted_amp, 1.0e-15))
        analytic = reference["metrics"]["pairs"][f"{int(source_hz / 1000)}to{int(wanted_hz / 1000)}k"]
        rows[f"{int(source_hz / 1000)}to{int(wanted_hz / 1000)}k"] = {
            "wanted_tone_output_amplitude": wanted_amp,
            "alias_source_output_amplitude": alias_amp,
            "alias_to_wanted_dbc": time_ratio,
            "analytic_alias_to_wanted_dbc": analytic["alias_to_wanted_dbc"],
            "difference_db": time_ratio - analytic["alias_to_wanted_dbc"],
        }
    return rows


def public_record(record: dict[str, Any], detailed: bool, include_phase: bool = False) -> dict[str, Any]:
    design = record["design"]
    output = {"design": design, "metrics": record["metrics"]}
    # Group delay is inexpensive and belongs in every SOS comparison row.  The
    # signal-level diagnostics below are intentionally reserved for one best
    # candidate, rather than multiplying a host-only sanity check across every
    # gate-reaching row.
    if include_phase:
        output["group_delay_iir_only"] = group_delay(record["_design"].sos)
    if detailed:
        sos = record["_design"].sos
        hd = headroom(sos)
        output["headroom_iir_only"] = hd
        output["float32"] = float32_check(record["_design"], record, hd["best"]["section_order"])
        output["time_domain_alias_sanity"] = time_domain_alias(record["_design"], record)
        output["coefficients"] = {
            "sos_float64": [[float(v) for v in row] for row in sos],
            "sos_float32": [[float(v) for v in row.astype(np.float32)] for row in sos],
            "pole_radii_float64": pole_radii(sos),
        }
    return output


def cpu_estimate_us(sections: int) -> float:
    return 25.7 * sections


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", type=pathlib.Path,
                        help="optional output path for the complete host-study record")
    args = parser.parse_args()

    print("[1/4] construct bounded full-IIR design bank", flush=True)
    bank = design_bank()
    print(f"      stable unique designs={len(bank)}", flush=True)

    print("[2/4] 10-Hz full-chain coarse selection", flush=True)
    finalists = select_for_fine(bank)
    print(f"      selected for 1-Hz evaluation={len(finalists)}", flush=True)

    print("[3/4] 1-Hz full-chain alias/wanted evaluation", flush=True)
    exact = []
    for index, item in enumerate(finalists, 1):
        row = fine(item)
        row["_design"] = item
        exact.append(row)
        if index % 50 == 0 or index == len(finalists):
            print(f"      candidate {index}/{len(finalists)}", flush=True)

    by_sos = {}
    for sections in SOS_COUNTS:
        rows = [row for row in exact if row["design"]["sos"] == sections]
        if rows:
            by_sos[str(sections)] = min(rows, key=fine_key)

    print("[4/4] detailed float32 / phase / headroom / time-domain checks", flush=True)
    nearest = min(exact, key=fine_key)
    accepted = [row for row in exact if row["metrics"]["verdict"] != "fail"]
    best = min(accepted, key=fine_key) if accepted else nearest
    detailed_label = best["design"]["label"]
    # Every SOS row retains its response and phase result.  Headroom, float32
    # DF2T, and explicit resampling are validation of the one best usable chain;
    # running those scalar simulations for several otherwise redundant rows
    # changes neither the feasibility conclusion nor the selected coefficients.
    published_sos = {
        key: public_record(value, value["design"]["label"] == detailed_label, include_phase=True)
        for key, value in by_sos.items()
    }
    best_public = published_sos[str(best["design"]["sos"])]

    accepted_min = [int(k) for k, value in by_sos.items()
                    if value["metrics"]["verdict"] in ("acceptable", "preferred")]
    preferred_min = [int(k) for k, value in by_sos.items()
                     if value["metrics"]["verdict"] == "preferred"]
    payload = {
        "meta": {
            "topology": "48k IIR only -> unchanged generic ASRC at nominal step~1.5 -> 32k",
            "metric": "existing Phase-1 full-chain generic-ASRC alias/wanted metric",
            "coarse_grid_hz": 10,
            "fine_grid_hz": 1,
            "search_scope": {
                "sos_counts": list(SOS_COUNTS), "pass_edges_hz": list(PASS_EDGES),
                "stop_edges_hz": list(STOP_EDGES), "ripples_db": list(RIPPLES),
                "attenuations_db": list(ATTENUATIONS),
                "full_spec_families": ["cheby2", "ellip"],
            },
            "generic_asrc_cpu": "hardware timing unknown; not inferred from host response",
            "iir_cpu_estimate": "25.7 us/block/SOS from prior 5-SOS target measurement; linear scaling is provisional",
        },
        "counts": {"bank": len(bank), "fine": len(finalists)},
        "best_per_sos": published_sos,
        "best": best_public,
        "minimum_sos_acceptable": min(accepted_min) if accepted_min else None,
        "minimum_sos_preferred": min(preferred_min) if preferred_min else None,
        "all_summary": [
            {
                "sos": row["design"]["sos"], "family": row["design"]["family"],
                "mode": row["design"]["mode"], "wanted15_db": row["metrics"]["wanted_db"]["15000"],
                "alias17to15_dbc": row["metrics"]["pairs"]["17to15k"]["alias_dbc"],
                "worst_alias15_dbc": row["metrics"]["worst_alias_0_to_15_dbc"],
                "cpu_estimate_us_block": cpu_estimate_us(row["design"]["sos"]),
                "verdict": row["metrics"]["verdict"],
            }
            for row in published_sos.values()
        ],
    }

    print("SOS  family   mode   wanted@15  17->15 alias  worst 0-15 alias  CPU est   verdict")
    for sections in SUMMARY_SOS:
        row = published_sos.get(str(sections))
        if row is None:
            continue
        d, m = row["design"], row["metrics"]
        print(f"{sections:3d}  {d['family']:7s} {d['mode']:5s} {m['wanted_db']['15000']:10.3f} "
              f"{m['pairs']['17to15k']['alias_dbc']:13.3f} {m['worst_alias_0_to_15_dbc']:18.3f} "
              f"{cpu_estimate_us(sections):7.1f} us  {m['verdict']}")
    print(f"minimum acceptable SOS: {payload['minimum_sos_acceptable']}")
    print(f"minimum preferred SOS: {payload['minimum_sos_preferred']}")
    print(f"best: {best['design']['label']} ({best['metrics']['verdict']})")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
