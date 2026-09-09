#!/usr/bin/env python3
"""Host precheck for the 48 -> 32 kHz Full-IIR candidate: real float32 arithmetic
and off-nominal rate behaviour.

Phase 4 qualified the six-SOS elliptic candidate with a float64 evaluation, and
its `float32_check` quantises the COEFFICIENTS to float32 and then evaluates in
float64 (`sos.astype(np.float32).astype(np.float64)`).  Its time-domain sanity
check resamples with `upfirdn(up=128, down=192)`, a fixed nominal ratio.

Two things therefore remained unverified, and this script is only about those:

  B  Sequential float32 arithmetic.  The device runs `asrc_samp_t = float`, so the
     IIR state, the resampler's phase accumulator and the 30-tap dot are all
     float32.  Coefficient quantisation and arithmetic rounding are different
     questions, and the second one matters here: a float32 chain's own arithmetic
     floor sits around -120..-130 dBc, i.e. the SAME order as the -119.751 dBc
     alias being claimed.  So this script measures the arithmetic floor
     separately (wanted tone only, no alias source) and reports every alias
     against it.  An alias reading that sits on the floor is a floor reading, not
     a filter reading, and is labelled as such.

  C  Off-nominal ratio.  The device's generic ASRC is an L=128 / M=30 polyphase
     with a LINEAR BLEND between adjacent phases: `p = (int)(frac*128)`,
     `wb = frac*128 - p`, `ceff = c0 + wb*(c1-c0)`.  When step is exactly 1.5,
     `frac*128` is always an integer, so `wb` is always zero and the blend never
     fires -- which is exactly why `upfirdn(up=128, down=192)` reproduces the
     nominal case and cannot see the blend error at all.  Any clock offset makes
     the blend active on nearly every output.

The polyphase coefficients are NOT regenerated here.  They are read from the
shipping table `src/app/apps/asrc/audio_app_asrc_poly_l128m30_flash.c`, whose
words are raw float32 bit patterns, so the model uses the coefficients the ROM
image actually contains.

Fidelity limits, stated rather than assumed:

  * The 30-tap accumulation is float32 but numpy-pairwise, where the device
    accumulates sequentially and, with `-ffp-contract=fast`, may fuse.  That is a
    difference of order sqrt(30)*eps ~ 7e-7 (about -123 dBc), which is NOT
    negligible at this gate -- it is one of the reasons the arithmetic floor is
    measured instead of assumed.
  * The IIR recurrence order is the device's: y = b0*x + d1; d1 = b1*x + d2 +
    (-a1)*y; d2 = b2*x + (-a2)*y, with float32 state.
  * Only the A -> B direction and one channel's arithmetic are modelled.  Nothing
    here is a statement about the full 16-channel firmware.

Nothing in this script builds, flashes, or touches hardware.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import re
import sys

import numpy as np
from scipy import signal

ROOT = pathlib.Path(__file__).resolve().parents[2]
PHASE1 = ROOT / "tools" / "asrc" / "asrc_48_to_32_hybrid_filter_study.py"
# The Phase-4 machine-readable record, kept in the repo so reproduction does not
# depend on a scratch directory outside it.
PHASE4_RECORD = ROOT / "tools" / "asrc" / "asrc_48_to_32_full_iir_feasibility_2026-09-05.json"
POLY_TABLE = ROOT / "src" / "app" / "apps" / "asrc" / "audio_app_asrc_poly_l128m30_flash.c"

FS_IN = 48_000.0
NOMINAL_STEP = 1.5
POLY_L = 128
POLY_M = 30

# The Phase-4 candidate, by specification.  Re-derived, then required to match the
# Phase-4 machine-readable record, so this script cannot drift from the filter that
# was qualified.
ORDER, RIPPLE_DB, STOP_DB, PASS_HZ = 12, 0.25, 120.0, 15_000.0

WANTED_HZ = (1_000.0, 5_000.0, 10_000.0, 12_000.0, 13_000.0,
             14_000.0, 14_500.0, 15_000.0)
# source -> landing at the NOMINAL rate; the landing is recomputed per step in C.
ALIAS_SOURCES_HZ = (17_000.0, 17_081.0, 18_000.0, 19_000.0)
AMPLITUDE = 0.8            # below full scale, so the +1.0965 internal peak cannot clip
FIT_SKIP = 4_096


# ---------------------------------------------------------------- the candidate

def load_phase1():
    spec = importlib.util.spec_from_file_location("full_iir_precheck_phase1", PHASE1)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {PHASE1}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def design_sos(verify_json: pathlib.Path | None) -> np.ndarray:
    raw = signal.ellip(ORDER, RIPPLE_DB, STOP_DB, PASS_HZ / (FS_IN / 2.0),
                       btype="low", output="sos")
    sos = load_phase1().normalize_sos_dc(np.asarray(raw, dtype=np.float64))
    if verify_json is not None:
        record = json.loads(verify_json.read_text())
        reference = np.asarray(record["best"]["coefficients"]["sos_float64"], dtype=np.float64)
        delta = float(np.max(np.abs(sos - reference)))
        if delta != 0.0:
            raise SystemExit(f"re-derived SOS differs from the Phase-4 record by {delta:.3e}")
    return sos


def df2t_rows_f32(sos: np.ndarray) -> np.ndarray:
    """[b0, b1, b2, -a1, -a2] per section, quantised to float32 exactly as the
    generated firmware header does.  Section order is the file order, which the
    Phase-4 headroom sweep selected (low-Q-first); do not reorder."""
    rows = []
    for b0, b1, b2, a0, a1, a2 in sos:
        if a0 != 1.0:
            raise SystemExit("a0 must be 1.0 after DC normalisation")
        rows.append([b0, b1, b2, -a1, -a2])
    return np.asarray(rows, dtype=np.float32)


# ------------------------------------------------------- the shipping poly table

def load_poly_table() -> np.ndarray:
    text = POLY_TABLE.read_text(encoding="utf-8", errors="replace")
    start = text.index("= {")
    body = text[start:]
    words = re.findall(r"0x([0-9A-Fa-f]{8})", body)
    if len(words) != (POLY_L + 1) * POLY_M:
        raise SystemExit(f"poly table has {len(words)} words, expected "
                         f"{(POLY_L + 1) * POLY_M}")
    raw = np.array([int(word, 16) for word in words], dtype=np.uint32)
    table = raw.view(np.float32).reshape(POLY_L + 1, POLY_M)
    if not np.isfinite(table).all():
        raise SystemExit("poly table contains a non-finite float32 word")
    return table


# --------------------------------------------------------------- float32 kernels

def df2t_f32(rows: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Sequential float32 transposed direct-form II, in the device's operation
    order, vectorised over the leading axis only (independent signals).  Every
    intermediate is a float32 array op, so the rounding is float32 per operation."""
    sections = rows.shape[0]
    d1 = np.zeros((sections, x.shape[0]), dtype=np.float32)
    d2 = np.zeros_like(d1)
    y = np.empty_like(x)
    for n in range(x.shape[1]):
        v = x[:, n]
        for s in range(sections):
            b0, b1, b2, na1, na2 = rows[s]
            out = b0 * v + d1[s]
            d1[s] = b1 * v + d2[s] + na1 * out
            d2[s] = b2 * v + na2 * out
            v = out
        y[:, n] = v
    return y


def resample_f32(x: np.ndarray, step: float, table: np.ndarray) -> np.ndarray:
    """The device's generic ASRC pull, in float32.

    Mirrors asrc_q31_ref_stream(): the read cursor starts so the first window
    begins at sample 0, `frac` advances AFTER the output is emitted, and the
    coefficient row is the linear blend of phases p and p+1.  `frac` is a float on
    the device, so it accumulates in float32 here too -- that accumulation is part
    of what an off-nominal step exercises."""
    step32 = np.float32(step)
    length = x.shape[1]
    base = 0
    frac = np.float32(0.0)
    columns = []
    while base + POLY_M <= length:
        pf = frac * np.float32(POLY_L)
        p = int(pf)
        if p >= POLY_L:
            p = POLY_L - 1
        wb = np.float32(pf - np.float32(p))
        row = table[p] + wb * (table[p + 1] - table[p])
        columns.append(np.sum(x[:, base:base + POLY_M] * row.astype(np.float32),
                              axis=1, dtype=np.float32))
        frac = np.float32(frac + step32)
        while frac >= np.float32(1.0):
            frac = np.float32(frac - np.float32(1.0))
            base += 1
    return np.stack(columns, axis=1)


def chain_f32(tones_hz: np.ndarray, step: float, rows: np.ndarray,
              table: np.ndarray, length: int,
              scale: float = 1.0) -> tuple[np.ndarray, dict]:
    """One float32 pass: sine bank -> six-SOS IIR -> generic ASRC at `step`."""
    time = (np.arange(length, dtype=np.float64) / FS_IN)
    x = ((AMPLITUDE * scale) * np.sin(2.0 * np.pi * tones_hz[:, None] * time[None, :])
         ).astype(np.float32)
    filtered = df2t_f32(rows, x)
    resampled = resample_f32(filtered, step, table)
    detail = {
        "iir_peak": float(np.max(np.abs(filtered))),
        "iir_finite": bool(np.isfinite(filtered).all()),
        "out_peak": float(np.max(np.abs(resampled))),
        "out_finite": bool(np.isfinite(resampled).all()),
    }
    return resampled, detail


# ------------------------------------------------------------------ measurement

def fit_amplitude(samples: np.ndarray, hz: float, fs_out: float) -> np.ndarray:
    """Least-squares amplitude of one frequency, per row.  Done in float64 on the
    float32 output: this measures the signal, it is not part of the chain."""
    data = np.asarray(samples[:, FIT_SKIP:], dtype=np.float64)
    time = np.arange(FIT_SKIP, FIT_SKIP + data.shape[1]) / fs_out
    basis = np.column_stack((np.sin(2.0 * np.pi * hz * time),
                            np.cos(2.0 * np.pi * hz * time)))
    coeff, _, _, _ = np.linalg.lstsq(basis, data.T, rcond=None)
    return np.hypot(coeff[0], coeff[1])


def landing_hz(source_hz: float, fs_out: float) -> float:
    """Where a source above the output Nyquist lands after decimation."""
    folded = source_hz % fs_out
    return folded if folded <= (fs_out / 2.0) else (fs_out - folded)


def db(ratio: float) -> float:
    return 20.0 * np.log10(max(ratio, 1.0e-30))


def worst_spur(samples: np.ndarray, fs_out: float, exclude_hz: float,
               guard_hz: float = 120.0, limit_hz: float = 15_000.0) -> dict:
    """Largest component in 0..limit_hz that is neither DC nor the wanted tone.
    Reported separately from the alias metric: a blend artefact need not land on
    the folding image."""
    data = np.asarray(samples[FIT_SKIP:], dtype=np.float64)
    window = np.hanning(data.size)
    spectrum = np.abs(np.fft.rfft(data * window)) / (window.sum() / 2.0)
    freqs = np.fft.rfftfreq(data.size, 1.0 / fs_out)
    mask = (freqs > guard_hz) & (freqs <= limit_hz)
    mask &= np.abs(freqs - exclude_hz) > guard_hz
    if not mask.any():
        return {"hz": 0.0, "amplitude": 0.0}
    index = int(np.argmax(spectrum[mask]))
    return {"hz": float(freqs[mask][index]), "amplitude": float(spectrum[mask][index])}


# ------------------------------------------------------------------------ item B

def item_b(rows: np.ndarray, table: np.ndarray, length: int, sweep_step_hz: float) -> dict:
    fs_out = FS_IN / NOMINAL_STEP
    result: dict = {"fs_out_hz": fs_out, "length_in": length}

    # 1. Wanted magnitudes, and the arithmetic floor read at each alias landing
    #    with NO alias source present.  Same pass: the wanted tones ARE the
    #    floor-only stimulus.
    wanted = np.asarray(WANTED_HZ, dtype=np.float64)
    out, detail = chain_f32(wanted, NOMINAL_STEP, rows, table, length)
    result["headroom"] = detail
    amplitudes = {}
    for index, hz in enumerate(WANTED_HZ):
        amp = float(fit_amplitude(out[index:index + 1], hz, fs_out)[0])
        amplitudes[f"{hz:.0f}"] = {"amplitude": amp, "db": db(amp / AMPLITUDE)}
    result["wanted"] = amplitudes

    # NOTE.  An arithmetic floor cannot be read at the alias landing frequency
    # while the wanted tone is present: 17.081 kHz lands at 14.919 kHz, only 81 Hz
    # from the 15 kHz wanted tone, and a single-frequency least-squares fit over a
    # finite window picks up that tone's leakage (about -21 dB, measured) instead
    # of the floor.  The question the floor was meant to answer -- "is this alias
    # reading the filter or the float32 noise?" -- is answered instead by LEVEL
    # LINEARITY, which needs no separation: a filter-limited alias tracks the
    # source amplitude exactly, a floor-limited one does not.
    linearity = {}
    for source_hz in ALIAS_SOURCES_HZ:
        row = {}
        for label, scale in (("full", 1.0), ("minus20dB", 0.1)):
            tone = np.asarray([source_hz], dtype=np.float64)
            scaled, _ = chain_f32(tone, NOMINAL_STEP, rows, table, length, scale)
            land = landing_hz(source_hz, fs_out)
            amp = float(fit_amplitude(scaled, land, fs_out)[0])
            row[label] = {"amplitude": amp, "dbc_vs_source": db(amp / (AMPLITUDE * scale))}
        delta = row["full"]["dbc_vs_source"] - row["minus20dB"]["dbc_vs_source"]
        row["relative_dbc_delta"] = delta
        # A linear (filter-limited) result gives the same dBc at both levels.  A
        # floor-limited one gives a WORSE dBc at the lower level, because the noise
        # did not scale down with the signal.
        row["filter_limited"] = bool(abs(delta) < 1.0)
        linearity[f"{source_hz:.0f}"] = row
    result["level_linearity"] = linearity
    # 2. The alias sources themselves.
    sources = np.asarray(ALIAS_SOURCES_HZ, dtype=np.float64)
    alias_out, alias_detail = chain_f32(sources, NOMINAL_STEP, rows, table, length)
    result["alias_headroom"] = alias_detail
    aliases = {}
    for index, source_hz in enumerate(ALIAS_SOURCES_HZ):
        land = landing_hz(source_hz, fs_out)
        alias_amp = float(fit_amplitude(alias_out[index:index + 1], land, fs_out)[0])
        # The wanted amplitude AT THE LANDING FREQUENCY, so the ratio is
        # alias-to-wanted and any common interpolation gain cancels.
        wanted_at_land, _ = chain_f32(np.asarray([land]), NOMINAL_STEP, rows, table, length)
        wanted_amp = float(fit_amplitude(wanted_at_land, land, fs_out)[0])
        aliases[f"{source_hz:.0f}"] = {
            "landing_hz": land,
            "alias_amplitude": alias_amp,
            "wanted_at_landing_amplitude": wanted_amp,
            "alias_to_wanted_dbc": db(alias_amp / wanted_amp),
            "worst_spur": worst_spur(alias_out[index], fs_out, land),
        }
    result["aliases"] = aliases

    # 3. Sweeps.  Coarse by design: this is a float32 confirmation of an already
    #    swept float64 result, not a second global search.
    passband = np.arange(100.0, PASS_HZ + 1.0, sweep_step_hz)
    sweep_out, _ = chain_f32(passband, NOMINAL_STEP, rows, table, length)
    passband_db = np.asarray([
        db(float(fit_amplitude(sweep_out[i:i + 1], hz, fs_out)[0]) / AMPLITUDE)
        for i, hz in enumerate(passband)])
    result["passband_sweep"] = {
        "step_hz": sweep_step_hz,
        "points": int(passband.size),
        "floor_db": float(passband_db.min()),
        "floor_hz": float(passband[int(np.argmin(passband_db))]),
        "peak_db": float(passband_db.max()),
        "peak_hz": float(passband[int(np.argmax(passband_db))]),
    }

    stopband = np.arange(fs_out / 2.0 + 100.0, 24_000.0 + 1.0, sweep_step_hz)
    alias_sweep_out, _ = chain_f32(stopband, NOMINAL_STEP, rows, table, length)
    rows_out = []
    for i, hz in enumerate(stopband):
        land = landing_hz(hz, fs_out)
        if land < 100.0:
            continue
        amp = float(fit_amplitude(alias_sweep_out[i:i + 1], land, fs_out)[0])
        rows_out.append((hz, land, db(amp / AMPLITUDE)))
    worst = max(rows_out, key=lambda item: item[2]) if rows_out else None
    result["alias_sweep"] = {
        "step_hz": sweep_step_hz,
        "points": len(rows_out),
        "worst_source_hz": None if worst is None else worst[0],
        "worst_landing_hz": None if worst is None else worst[1],
        "worst_dbc": None if worst is None else worst[2],
    }
    return result


# ------------------------------------------------------------------------ item C

def item_c(rows: np.ndarray, table: np.ndarray, length: int,
           offsets_ppm: tuple[float, ...]) -> dict:
    result: dict = {"length_in": length, "offsets_ppm": list(offsets_ppm),
                    "note": "+-100 ppm is a screening range, NOT a product "
                            "specification: no clock tolerance figure was found "
                            "in the repo."}
    per_offset = {}
    for ppm in offsets_ppm:
        step = NOMINAL_STEP * (1.0 + ppm * 1.0e-6)
        fs_out = FS_IN / step
        tones = np.asarray(list(WANTED_HZ) + list(ALIAS_SOURCES_HZ), dtype=np.float64)
        out, detail = chain_f32(tones, step, rows, table, length)
        row: dict = {"step": step, "fs_out_hz": fs_out, "headroom": detail,
                     "wanted": {}, "aliases": {}}
        wanted_amp = {}
        for index, hz in enumerate(WANTED_HZ):
            amp = float(fit_amplitude(out[index:index + 1], hz, fs_out)[0])
            wanted_amp[hz] = amp
            row["wanted"][f"{hz:.0f}"] = {
                "amplitude": amp,
                "db": db(amp / AMPLITUDE),
                "worst_spur": worst_spur(out[index], fs_out, hz),
            }
        for offset, source_hz in enumerate(ALIAS_SOURCES_HZ):
            index = len(WANTED_HZ) + offset
            land = landing_hz(source_hz, fs_out)
            amp = float(fit_amplitude(out[index:index + 1], land, fs_out)[0])
            reference = wanted_amp.get(15_000.0, AMPLITUDE)
            row["aliases"][f"{source_hz:.0f}"] = {
                "landing_hz": land,
                "amplitude": amp,
                "dbc_vs_full_scale": db(amp / AMPLITUDE),
                "dbc_vs_wanted15": db(amp / reference),
                "worst_spur": worst_spur(out[index], fs_out, land),
            }
        per_offset[f"{ppm:+.0f}"] = row
    result["per_offset"] = per_offset
    return result


# ------------------------------------------------------------------------ driver

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--item", choices=("b", "c", "all"), default="all")
    parser.add_argument("--json", type=pathlib.Path, default=None)
    parser.add_argument("--verify-json", type=pathlib.Path, default=PHASE4_RECORD,
                        help="Phase-4 record to check the re-derived SOS against")
    parser.add_argument("--length-b", type=int, default=49_152)
    parser.add_argument("--length-c", type=int, default=147_456,
                        help="long enough that the +-ppm phase pattern repeats "
                             "several times; 1/1e-4 ~ 1e4 outputs per cycle")
    parser.add_argument("--sweep-step-hz", type=float, default=125.0)
    parser.add_argument("--offsets-ppm", type=float, nargs="*",
                        default=[0.0, 25.0, -25.0, 100.0, -100.0])
    args = parser.parse_args()

    sos = design_sos(args.verify_json)
    rows = df2t_rows_f32(sos)
    table = load_poly_table()
    print(f"candidate: elliptic n{ORDER} fp{PASS_HZ:.0f} rp{RIPPLE_DB} rs{STOP_DB:.0f}, "
          f"{rows.shape[0]} SOS, float32 rows, low-Q-first order")
    print(f"poly table: {POLY_TABLE.name}, {table.shape[0]} phases x {table.shape[1]} taps, "
          f"float32 from the shipping bit patterns")

    record: dict = {
        "candidate": {"order": ORDER, "sos": int(rows.shape[0]),
                      "ripple_db": RIPPLE_DB, "stop_db": STOP_DB, "pass_hz": PASS_HZ,
                      "section_order": "file order == low-Q-first [0..5]"},
        "chain": "float32 sine -> float32 six-SOS DF2T -> float32 L=128/M=30 "
                 "generic ASRC with inter-phase linear blend",
        "amplitude_full_scale_fraction": AMPLITUDE,
    }
    if args.item in ("b", "all"):
        print("item B: sequential float32 arithmetic ...")
        record["item_b"] = item_b(rows, table, args.length_b, args.sweep_step_hz)
    if args.item in ("c", "all"):
        print("item C: off-nominal ratio ...")
        record["item_c"] = item_c(rows, table, args.length_c, tuple(args.offsets_ppm))

    text = json.dumps(record, indent=2, sort_keys=True)
    if args.json is not None:
        args.json.write_text(text, encoding="utf-8")
        print(f"wrote {args.json}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
