#!/usr/bin/env python3
"""Price a 96 kHz -> 48 kHz decimating front end BEFORE implementing it.

Host-only. Designs nothing into the tree; it answers "what would it cost".

Method is deliberately identical to asrc_decimator_48_to_8_design.py so the numbers
are comparable to the already-measured 48 kHz stages:
  - windowed sinc, Kaiser beta 11, odd tap counts
  - the STAGE-ALONE bar of -100 dB in the stopband (the bar the /2 and /3 stages are
    held to, because a later rate change cannot undo a fold the stage itself creates)
  - cost in "units" = taps * (output_rate / 48000), the script's own normalisation, so
    a unit here means the same amount of DSP as a unit there

Only NumPy is required.
"""

from __future__ import annotations

import numpy as np

KAISER_BETA = 11.0
FFT_SIZE = 1 << 21
STAGE_ALONE_LIMIT_DB = -100.0


def design(taps: int, sample_rate: float, passband: float, stopband: float) -> np.ndarray:
    center = (passband + stopband) * 0.5
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    coeff = (
        2.0 * center / sample_rate
        * np.sinc(2.0 * center / sample_rate * offset)
        * np.kaiser(taps, KAISER_BETA)
    )
    coeff /= np.sum(coeff)
    return coeff.astype("<f4")


def worst_stopband_db(coeff: np.ndarray, sample_rate: float, stopband: float) -> float:
    spectrum = np.fft.rfft(coeff, FFT_SIZE)
    freq = np.linspace(0.0, sample_rate * 0.5, spectrum.size)
    db = 20.0 * np.log10(np.maximum(np.abs(spectrum), 1.0e-300))
    return float(np.max(db[freq >= stopband]))


def passband_edge_db(coeff: np.ndarray, sample_rate: float, passband: float) -> float:
    spectrum = np.fft.rfft(coeff, FFT_SIZE)
    freq = np.linspace(0.0, sample_rate * 0.5, spectrum.size)
    db = 20.0 * np.log10(np.maximum(np.abs(spectrum), 1.0e-300))
    band = db[freq <= passband]
    return float(band[-1])


def min_taps(sample_rate: float, passband: float, stopband: float,
             limit_db: float = STAGE_ALONE_LIMIT_DB, cap: int = 601) -> tuple[int, float, float]:
    """Smallest odd tap count whose stopband clears `limit_db`."""
    for taps in range(21, cap + 1, 2):
        coeff = design(taps, sample_rate, passband, stopband)
        stop = worst_stopband_db(coeff, sample_rate, stopband)
        if stop <= limit_db:
            return taps, stop, passband_edge_db(coeff, sample_rate, passband)
    return -1, float("nan"), float("nan")


def main() -> None:
    print("=" * 78)
    print("96 kHz -> 48 kHz /2 front-end stage: tap-count price")
    print("=" * 78)
    print()
    print("Structural situation is the SAME as the existing 48->24k /2 stage:")
    print("a 2:1 decimation whose output IS the next stage's input rate, so the")
    print("stopband must sit on the OUTPUT Nyquist (24 kHz) and the stage carries")
    print("the whole anti-alias job alone. Half-band is closed for this shape --")
    print("priced and rejected at >401 taps for 48->24k (see asrc_decimator_48_to_8.h).")
    print()

    fs_in = 96000.0
    stopband = 24000.0   # output Nyquist after /2

    # Scale the proven 48->24k choice: passband 8850/12000 = 0.7375 of Nyquist.
    # At 96 kHz in, the same relative width is 0.7375 * 24000 = 17700 Hz.
    print(f"{'passband':>10} {'rel.Nyq':>8} {'taps':>6} {'units':>7} {'stopband dB':>12} {'pb edge dB':>11}")
    rows = []
    for pb in (15000.0, 16000.0, 17000.0, 17700.0, 18000.0, 19000.0, 20000.0):
        taps, stop, edge = min_taps(fs_in, pb, stopband)
        if taps < 0:
            print(f"{pb:>10.0f} {pb/24000.0:>8.3f} {'>601':>6} {'-':>7} {'FAIL':>12} {'-':>11}")
            continue
        # units = taps * (output_rate / 48000); output of this stage is 48 kHz -> factor 1.0
        units = taps * (48000.0 / 48000.0)
        rows.append((pb, taps, units, stop, edge))
        print(f"{pb:>10.0f} {pb/24000.0:>8.3f} {taps:>6} {units:>7.1f} {stop:>12.2f} {edge:>11.3f}")

    print()
    print("Reference points from the ALREADY-MEASURED 48 kHz stages, same method:")
    print("  48->24k /2 : passband 8850 (0.7375 Nyq), 107 taps, 53.5 units")
    print("  48->16k /3 : passband 5900 (0.7375 Nyq), 161 taps, 53.7 units")
    print()

    # The scaled-equivalent choice, for the headline number.
    pick = [r for r in rows if abs(r[0] - 17700.0) < 1.0]
    if pick:
        pb, taps, units, stop, edge = pick[0]
        print("-" * 78)
        print(f"Scaled-equivalent choice: passband {pb:.0f} Hz -> {taps} taps, {units:.1f} units")
        print(f"  stage alone {stop:.2f} dB, passband edge {edge:.3f} dB")
        print()
        print("WHY THE UNITS ARE THE HEADLINE: the 48->24k stage costs 53.5 units and")
        print("produces 24 kHz output. This stage produces 48 kHz output, i.e. TWICE as")
        print("many output frames per second, so a given tap count costs twice the DSP.")
        print(f"  {taps} taps at 48 kHz output = {taps * 1.0:.1f} units")
        print(f"  the same {taps} taps at 24 kHz output would be {taps * 0.5:.1f} units")
        print()

    # Composed chains: what each low rate actually needs once /2 is in front.
    print("=" * 78)
    print("Composed chains (96 kHz source) and what the ASRC then sees")
    print("=" * 78)
    BLOCK, FIFO, AHEAD, JIT = 16, 128, 15, 4
    cap = FIFO - 4 - BLOCK - JIT
    print(f"ring cap ASRC_FILL_TARGET_MAX = {cap} frames (FIFO {FIFO}, BLOCK {BLOCK})")
    print()
    print(f"{'fs_B':>7} {'chain':>12} {'ASRC step':>10} {'R':>5} {'R+jit':>6} {'fits':>6}")
    plan = [
        (48000, "/2",        2),
        (32000, "direct",    1),
        (24000, "/2",        2),
        (22050, "/2",        2),
        (16000, "/2 +/3",    6),
        (12000, "/2 +/4",    8),
        (11025, "/2 +/4",    8),
        (8000,  "/2 +/6",   12),
    ]
    for fsb, chain, total_den in plan:
        step = (96000.0 / total_den) / fsb
        if step < 1.0:
            verdict = "n/a"   # would be an up-conversion into the ASRC
        R = int(step * (BLOCK - 1)) + AHEAD + 1
        need = R + JIT
        verdict = "yes" if need <= cap else "NO"
        print(f"{fsb:>7} {chain:>12} {step:>10.4f} {R:>5} {need:>6} {verdict:>6}")
    print()
    print("NOTE the /2 stage runs on the A-side block at 96 kHz for EVERY rate above,")
    print("including 48 kHz -- it is not only paid at the low end.")


if __name__ == "__main__":
    main()
