#!/usr/bin/env python3
"""Host acceptance tests for the fixed 48 kHz low-rate front ends."""

from __future__ import annotations

import math
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import asrc_decimator_48_to_8_design as design  # noqa: E402


FS_IN = 48000
FS_OUT = 8000
AMPLITUDE = 10.0 ** (-1.0 / 20.0)
TONES = (1000, 3000, 3500, 3900, 4100, 5000, 8000, 12000, 18000)
MIDRATE_TONES = (1000, 5000, 5500, 10500, 12000, 18000)
FS_OUT_24 = 24000
# 8800 Hz sits just inside the 8850 Hz passband edge; 13000/18000/23000 are alias sources
# (they fold to 11000/6000/1000 Hz), which is what the /2 stage exists to suppress.
HALFRATE_TONES = (1000, 8000, 8800, 13000, 18000, 23000)
# The 22.05 kHz variant of the same /2 structure.  7800 Hz sits just inside its 7875 Hz passband
# edge.  11500 Hz is the tone that separates the two variants: it is ABOVE the 22.05 kHz output
# Nyquist (11025 Hz) but BELOW the intermediate's (12000 Hz), so it does not fold in the stage at
# all -- it survives to the resampler, which cannot remove it (its fc = 0.465*24000 = 11160 Hz
# sits above 11025 Hz).  The 24 kHz coefficients leave it ~-19 dB down; this variant must bury it.
# 13000/18000/23000 are the same fold sources as above, kept so the two variants are compared on
# identical ground.
HALFRATE22K_TONES = (1000, 7000, 7800, 11500, 13000, 18000, 23000)
FS_IN_96 = 96000
# The 96 -> 48 kHz pre-stage, measured at ITS input rate.  6300 Hz sits just inside the 6400 Hz
# passband edge (the 16 kHz chain needs it flat).  The rest are fold sources against the tightest
# final rate, 16 kHz: 40000/44000/48000 Hz all land in or below the 8000 Hz final band after the
# remaining /3, and 41000 Hz is the designed stopband edge plus a margin.  1000 Hz pins the gain.
PRESTAGE_TONES = (1000, 6300, 41000, 44000, 48000)


class StreamingReference:
    def __init__(self, channels: int, h1: np.ndarray, h2: np.ndarray) -> None:
        self.channels = channels
        self.h1 = h1.astype(np.float32)
        self.h2 = h2.astype(np.float32)
        self.s1 = np.zeros((channels, h1.size), dtype=np.float32)
        self.s2 = np.zeros((channels, h2.size), dtype=np.float32)
        self.w1 = self.w2 = self.p1 = self.p2 = 0
        self.input_frames = self.output_frames = 0

    def expected(self, frames: int) -> int:
        stage1 = (self.p1 + frames) // 3
        return (self.p2 + stage1) // 2

    @staticmethod
    def dot(history: np.ndarray, write: int, coeff: np.ndarray) -> np.float32:
        order = (write - 1 - np.arange(coeff.size)) % coeff.size
        return np.sum(history[order] * coeff, dtype=np.float32)

    def process(self, block: np.ndarray) -> np.ndarray:
        output: list[np.ndarray] = []
        for sample in np.asarray(block, dtype=np.float32):
            self.s1[:, self.w1] = sample
            self.w1 = (self.w1 + 1) % self.h1.size
            self.p1 += 1
            if self.p1 != 3:
                continue
            self.p1 = 0
            for channel in range(self.channels):
                self.s2[channel, self.w2] = self.dot(
                    self.s1[channel], self.w1, self.h1
                )
            self.w2 = (self.w2 + 1) % self.h2.size
            self.p2 += 1
            if self.p2 != 2:
                continue
            self.p2 = 0
            output.append(
                np.asarray(
                    [self.dot(self.s2[c], self.w2, self.h2) for c in range(self.channels)],
                    dtype=np.float32,
                )
            )
        self.input_frames += block.shape[0]
        self.output_frames += len(output)
        if not output:
            return np.empty((0, self.channels), dtype=np.float32)
        return np.vstack(output)


class StreamingMidrateReference:
    def __init__(self, channels: int, coeff: np.ndarray) -> None:
        self.channels = channels
        self.coeff = coeff.astype(np.float32)
        self.history = np.zeros((channels, coeff.size), dtype=np.float32)
        self.write = self.phase = 0

    def expected(self, frames: int) -> int:
        return (self.phase + frames) // 3

    def process(self, block: np.ndarray) -> np.ndarray:
        output: list[np.ndarray] = []
        for sample in np.asarray(block, dtype=np.float32):
            self.history[:, self.write] = sample
            self.write = (self.write + 1) % self.coeff.size
            self.phase += 1
            if self.phase != 3:
                continue
            self.phase = 0
            output.append(
                np.asarray(
                    [StreamingReference.dot(self.history[c], self.write, self.coeff)
                     for c in range(self.channels)],
                    dtype=np.float32,
                )
            )
        if not output:
            return np.empty((0, self.channels), dtype=np.float32)
        return np.vstack(output)


class StreamingHalfrateReference:
    """Same single-stage shape as StreamingMidrateReference, decimating by 2 instead of 3."""

    def __init__(self, channels: int, coeff: np.ndarray) -> None:
        self.channels = channels
        self.coeff = coeff.astype(np.float32)
        self.history = np.zeros((channels, coeff.size), dtype=np.float32)
        self.write = self.phase = 0

    def expected(self, frames: int) -> int:
        return (self.phase + frames) // 2

    def process(self, block: np.ndarray) -> np.ndarray:
        output: list[np.ndarray] = []
        for sample in np.asarray(block, dtype=np.float32):
            self.history[:, self.write] = sample
            self.write = (self.write + 1) % self.coeff.size
            self.phase += 1
            if self.phase != 2:
                continue
            self.phase = 0
            output.append(
                np.asarray(
                    [StreamingReference.dot(self.history[c], self.write, self.coeff)
                     for c in range(self.channels)],
                    dtype=np.float32,
                )
            )
        if not output:
            return np.empty((0, self.channels), dtype=np.float32)
        return np.vstack(output)


def cascade(signal: np.ndarray, h1: np.ndarray, h2: np.ndarray) -> np.ndarray:
    # The C phase counters emit at input indices 2 and then 1 in the two stages.
    stage1 = np.convolve(signal, h1.astype(np.float64), mode="full")[: signal.size][2::3]
    return np.convolve(stage1, h2.astype(np.float64), mode="full")[: stage1.size][1::2]


def decimate3(signal: np.ndarray, coeff: np.ndarray) -> np.ndarray:
    return np.convolve(signal, coeff.astype(np.float64), mode="full")[: signal.size][2::3]


def decimate2(signal: np.ndarray, coeff: np.ndarray) -> np.ndarray:
    # The C phase counter emits at input index 1, then every second sample.
    return np.convolve(signal, coeff.astype(np.float64), mode="full")[: signal.size][1::2]


def db(value: float) -> float:
    return 20.0 * math.log10(max(abs(value), 1.0e-300))


def tone_metrics(freq: int, h1: np.ndarray, h2: np.ndarray) -> tuple[float, float]:
    duration_s = 3
    n = np.arange(FS_IN * duration_s, dtype=np.float64)
    source = AMPLITUDE * np.sin(2.0 * np.pi * freq * n / FS_IN)
    source = np.trunc(source * 8388607.0) / 8388607.0
    output = cascade(source, h1, h2)
    output = output[4096:]
    output = output[: (output.size // FS_OUT) * FS_OUT]
    rms_dbfs = db(float(np.sqrt(np.mean(output * output))) * math.sqrt(2.0))

    alias = freq % FS_OUT
    if alias > FS_OUT // 2:
        alias = FS_OUT - alias
    t = np.arange(output.size, dtype=np.float64) / FS_OUT
    basis = np.column_stack(
        (np.sin(2.0 * np.pi * alias * t), np.cos(2.0 * np.pi * alias * t))
    )
    if alias == 0:
        basis = np.ones((output.size, 1), dtype=np.float64)
    fit = basis @ np.linalg.lstsq(basis, output, rcond=None)[0]
    fundamental = float(np.sqrt(np.mean(fit * fit)))
    residual = float(np.sqrt(np.mean((output - fit) ** 2)))
    thdn_db = db(residual / max(fundamental, 1.0e-300))
    return rms_dbfs, thdn_db


def midrate_tone_level(freq: int, coeff: np.ndarray) -> float:
    duration_s = 3
    n = np.arange(FS_IN * duration_s, dtype=np.float64)
    source = AMPLITUDE * np.sin(2.0 * np.pi * freq * n / FS_IN)
    output = decimate3(source, coeff)[4096:]
    return db(float(np.sqrt(np.mean(output * output))) * math.sqrt(2.0))


def halfrate_tone_level(freq: int, coeff: np.ndarray) -> float:
    duration_s = 3
    n = np.arange(FS_IN * duration_s, dtype=np.float64)
    source = AMPLITUDE * np.sin(2.0 * np.pi * freq * n / FS_IN)
    output = decimate2(source, coeff)[4096:]
    return db(float(np.sqrt(np.mean(output * output))) * math.sqrt(2.0))


def prestage_tone_level(freq: int, coeff: np.ndarray) -> float:
    """Level after the 96 -> 48 kHz pre-stage alone, measured at its own 96 kHz input rate."""
    duration_s = 3
    n = np.arange(FS_IN_96 * duration_s, dtype=np.float64)
    source = AMPLITUDE * np.sin(2.0 * np.pi * freq * n / FS_IN_96)
    output = decimate2(source, coeff)[4096:]
    return db(float(np.sqrt(np.mean(output * output))) * math.sqrt(2.0))


def chain_gain_db(stages: tuple[tuple[np.ndarray, float], ...],
                  top_hz: float) -> np.ndarray:
    """Cascaded gain in dB over 0..top_hz.

    `stages` is (coefficients, that stage's own input rate) in signal order.  Evaluated as a
    product of DTFTs rather than by convolution so the decimation phase cannot smear the answer.
    """
    freq = np.linspace(0.0, top_hz, 801)
    gain_db = np.zeros_like(freq)
    for coeff, rate_hz in stages:
        offset = np.arange(coeff.size) - (coeff.size - 1) * 0.5
        response_mag = np.abs(
            np.exp(-2j * np.pi * np.outer(freq / rate_hz, offset)) @ coeff.astype(np.float64)
        )
        gain_db += 20.0 * np.log10(np.maximum(response_mag, 1.0e-300))
    return gain_db


def combined_passband(h1: np.ndarray, h2: np.ndarray) -> tuple[float, float]:
    freq = np.linspace(0.0, 3200.0, 3201)
    n1 = np.arange(h1.size) - (h1.size - 1) * 0.5
    n2 = np.arange(h2.size) - (h2.size - 1) * 0.5
    a1 = np.abs(np.exp(-2j * np.pi * np.outer(freq / 48000.0, n1)) @ h1)
    a2 = np.abs(np.exp(-2j * np.pi * np.outer(freq / 16000.0, n2)) @ h2)
    gain = 20.0 * np.log10(np.maximum(a1 * a2, 1.0e-300))
    return float(np.ptp(gain)), float(gain[-1])


def main() -> int:
    h1 = design.design(
        design.STAGE1_TAPS,
        design.STAGE1_INPUT_HZ,
        design.PASSBAND_HZ,
        design.STAGE1_STOPBAND_HZ,
    )
    h2 = design.design(
        design.STAGE2_TAPS,
        design.STAGE2_INPUT_HZ,
        design.PASSBAND_HZ,
        design.STAGE2_STOPBAND_HZ,
    )
    h16 = design.design(
        design.MIDRATE_TAPS,
        design.STAGE1_INPUT_HZ,
        design.MIDRATE_PASSBAND_HZ,
        design.MIDRATE_STOPBAND_HZ,
    )

    h24 = design.design(
        design.HALFRATE_TAPS,
        design.STAGE1_INPUT_HZ,
        design.HALFRATE_PASSBAND_HZ,
        design.HALFRATE_STOPBAND_HZ,
    )
    h22 = design.design(
        design.HALFRATE_TAPS,
        design.STAGE1_INPUT_HZ,
        design.HALFRATE22K_PASSBAND_HZ,
        design.HALFRATE22K_STOPBAND_HZ,
    )

    # The two /4 chains, designed here only so the composed 96 kHz cascade can be checked through
    # them; their own stand-alone acceptance is the design script's survey.
    hq_1 = design.design(
        design.QUARTER_STAGE1_TAPS,
        design.QUARTER_STAGE1_INPUT_HZ,
        design.QUARTER_PASSBAND_HZ,
        design.QUARTER_STAGE1_STOPBAND_HZ,
    )
    hq_2 = design.design(
        design.QUARTER_STAGE2_TAPS,
        design.QUARTER_STAGE2_INPUT_HZ,
        design.QUARTER_PASSBAND_HZ,
        design.QUARTER_STAGE2_STOPBAND_HZ,
    )
    hq12_1 = design.design(
        design.QUARTER_STAGE1_TAPS,
        design.QUARTER_STAGE1_INPUT_HZ,
        design.QUARTER12K_PASSBAND_HZ,
        design.QUARTER12K_STAGE1_STOPBAND_HZ,
    )
    hq12_2 = design.design(
        design.QUARTER_STAGE2_TAPS,
        design.QUARTER_STAGE2_INPUT_HZ,
        design.QUARTER12K_PASSBAND_HZ,
        design.QUARTER12K_STAGE2_STOPBAND_HZ,
    )
    hpre = design.design(
        design.PRESTAGE_TAPS,
        design.PRESTAGE_INPUT_HZ,
        design.PRESTAGE_PASSBAND_HZ,
        design.PRESTAGE_STOPBAND_HZ,
    )

    failures: list[str] = []
    if not np.array_equal(hpre, hpre[::-1]):
        failures.append("96-to-48 pre-stage coefficients are not exactly float32 symmetric")
    if not np.array_equal(h16, h16[::-1]):
        failures.append("48-to-16 coefficients are not exactly float32 symmetric")
    if not np.array_equal(h24, h24[::-1]):
        failures.append("48-to-24 coefficients are not exactly float32 symmetric")
    if not np.array_equal(h22, h22[::-1]):
        failures.append("48-to-24 22k coefficients are not exactly float32 symmetric")
    # The two /2 variants share a tap count on purpose, which makes a copy-paste that renders the
    # same array twice invisible to every size and CRC-metadata check on the target.  Pin it here.
    if h22.size != h24.size:
        failures.append(f"the two /2 variants must share a tap count ({h22.size} vs {h24.size})")
    elif np.array_equal(h22, h24):
        failures.append("the two /2 variants are byte-identical -- one of them is the wrong set")

    rng = np.random.default_rng(0x488)
    samples = rng.uniform(-0.9, 0.9, size=(997, 2)).astype(np.float32)
    one = StreamingReference(2, h1, h2)
    whole = one.process(samples)
    split = StreamingReference(2, h1, h2)
    parts = []
    offset = 0
    sizes = (1, 16, 7, 31, 2, 19, 5, 64)
    index = 0
    while offset < samples.shape[0]:
        count = min(sizes[index % len(sizes)], samples.shape[0] - offset)
        expected = split.expected(count)
        result = split.process(samples[offset : offset + count])
        if result.shape[0] != expected:
            failures.append("predicted output count differs from produced count")
        parts.append(result)
        offset += count
        index += 1
    fragmented = np.vstack(parts)
    if not np.array_equal(whole, fragmented):
        failures.append("output changes across block boundaries")

    block_ref = StreamingReference(1, h1, h2)
    block_counts = []
    zeros = np.zeros((16, 1), dtype=np.float32)
    for _ in range(6):
        block_counts.append(block_ref.process(zeros).shape[0])
    if block_counts != [2, 3, 3, 2, 3, 3]:
        failures.append(f"16-frame block count sequence is {block_counts}")

    mid_ref = StreamingMidrateReference(1, h16)
    mid_block_counts = [mid_ref.process(zeros).shape[0] for _ in range(6)]
    if mid_block_counts != [5, 5, 6, 5, 5, 6]:
        failures.append(f"48-to-16 block count sequence is {mid_block_counts}")

    # 16 frames / 2 divides exactly, so unlike /6 and /3 this sequence is flat -- which is
    # also why DECIMATED_BLOCK_CAPACITY has to be 8 rather than 6 for this rate.
    #
    # Deliberately not repeated for the 22.05 kHz variant: block counts and the long-run count
    # below are a function of the decimation factor and the tap count only, and the two /2
    # variants share both (asserted above).  Re-running them would exercise nothing new.
    half_ref = StreamingHalfrateReference(1, h24)
    half_block_counts = [half_ref.process(zeros).shape[0] for _ in range(6)]
    if half_block_counts != [8, 8, 8, 8, 8, 8]:
        failures.append(f"48-to-24 block count sequence is {half_block_counts}")

    # The pre-stage shares the /2 structure asserted above, so only its own counts are re-checked:
    # it is fed APP_BLOCK_FRAMES at 96 kHz and must emit exactly half, which is what sizes the
    # intermediate scratch buffer in asrc_audio_path.c.
    pre_ref = StreamingHalfrateReference(1, hpre)
    pre_block_counts = [pre_ref.process(zeros).shape[0] for _ in range(6)]
    if pre_block_counts != [8, 8, 8, 8, 8, 8]:
        failures.append(f"96-to-48 block count sequence is {pre_block_counts}")

    long_input = FS_IN * 60 + 5
    expected_long = long_input // 6
    count_ref = StreamingReference(1, h1, h2)
    predicted = count_ref.expected(long_input)
    if predicted != expected_long:
        failures.append(f"long-run count {predicted} != {expected_long}")

    mid_long = StreamingMidrateReference(1, h16).expected(long_input)
    if mid_long != long_input // 3:
        failures.append(f"48-to-16 long-run count {mid_long} != {long_input // 3}")

    half_long = StreamingHalfrateReference(1, h24).expected(long_input)
    if half_long != long_input // 2:
        failures.append(f"48-to-24 long-run count {half_long} != {long_input // 2}")

    pre_long = StreamingHalfrateReference(1, hpre).expected(long_input)
    if pre_long != long_input // 2:
        failures.append(f"96-to-48 long-run count {pre_long} != {long_input // 2}")

    ripple, edge_gain = combined_passband(h1, h2)
    if ripple > 0.1:
        failures.append(f"passband ripple {ripple:.6f} dB exceeds 0.1 dB")

    metrics: dict[int, tuple[float, float]] = {}
    for freq in TONES:
        metrics[freq] = tone_metrics(freq, h1, h2)
    gain_error = metrics[1000][0] - (-1.0)
    if abs(gain_error) > 0.05:
        failures.append(f"1 kHz gain error {gain_error:.6f} dB exceeds +/-0.05 dB")
    for freq in (5000, 8000, 12000, 18000):
        if metrics[freq][0] > -90.0:
            failures.append(f"{freq} Hz alias {metrics[freq][0]:.2f} dBFS exceeds -90 dBFS")

    mid_ripple, mid_stop = design.response(
        h16,
        design.STAGE1_INPUT_HZ,
        design.MIDRATE_PASSBAND_HZ,
        design.MIDRATE_STOPBAND_HZ,
    )
    mid_levels = {freq: midrate_tone_level(freq, h16) for freq in MIDRATE_TONES}
    if mid_ripple > 0.1 or mid_stop > -100.0:
        failures.append(
            f"48-to-16 response ripple={mid_ripple:.6f} stop={mid_stop:.2f} dB"
        )
    if abs(mid_levels[1000] - (-1.0)) > 0.05:
        failures.append(f"48-to-16 1 kHz gain is {mid_levels[1000]:.3f} dBFS")
    for freq in (10500, 12000, 18000):
        if mid_levels[freq] > -90.0:
            failures.append(
                f"48-to-16 {freq} Hz alias {mid_levels[freq]:.2f} dBFS exceeds -90 dBFS"
            )

    half_ripple, half_stop = design.response(
        h24,
        design.STAGE1_INPUT_HZ,
        design.HALFRATE_PASSBAND_HZ,
        design.HALFRATE_STOPBAND_HZ,
    )
    half_levels = {freq: halfrate_tone_level(freq, h24) for freq in HALFRATE_TONES}
    if half_ripple > 0.1 or half_stop > -100.0:
        failures.append(
            f"48-to-24 response ripple={half_ripple:.6f} stop={half_stop:.2f} dB"
        )
    if abs(half_levels[1000] - (-1.0)) > 0.05:
        failures.append(f"48-to-24 1 kHz gain is {half_levels[1000]:.3f} dBFS")
    for freq in (13000, 18000, 23000):
        if half_levels[freq] > -90.0:
            failures.append(
                f"48-to-24 {freq} Hz alias {half_levels[freq]:.2f} dBFS exceeds -90 dBFS"
            )

    half22_ripple, half22_stop = design.response(
        h22,
        design.STAGE1_INPUT_HZ,
        design.HALFRATE22K_PASSBAND_HZ,
        design.HALFRATE22K_STOPBAND_HZ,
    )
    half22_levels = {freq: halfrate_tone_level(freq, h22) for freq in HALFRATE22K_TONES}
    if half22_ripple > 0.1 or half22_stop > -100.0:
        failures.append(
            f"48-to-24 22k response ripple={half22_ripple:.6f} stop={half22_stop:.2f} dB"
        )
    if abs(half22_levels[1000] - (-1.0)) > 0.05:
        failures.append(f"48-to-24 22k 1 kHz gain is {half22_levels[1000]:.3f} dBFS")
    # 11500 Hz is included here and NOT in the 24 kHz list above: it is in-band for that
    # variant (below its 12000 Hz stopband) and must be buried by this one.
    for freq in (11500, 13000, 18000, 23000):
        if half22_levels[freq] > -90.0:
            failures.append(
                f"48-to-24 22k {freq} Hz alias {half22_levels[freq]:.2f} dBFS exceeds -90 dBFS"
            )
    # The measurement that refutes reusing the 24 kHz set: it must leave 11500 Hz well ABOVE
    # the -90 dBFS bar, or the whole second variant is unnecessary.
    reuse_11500 = halfrate_tone_level(11500, h24)
    if reuse_11500 <= -90.0:
        failures.append(
            f"the 24 kHz set already buries 11500 Hz at {reuse_11500:.2f} dBFS -- "
            "the 22.05 kHz variant would be redundant"
        )

    # The 96 -> 48 kHz pre-stage.  ONE shared set serves all four final rates, so it is held to
    # the tightest of them (16 kHz: passband 6400 Hz, stopband from 48000 - 8000 = 40000 Hz).
    pre_ripple, pre_stop = design.response(
        hpre,
        design.PRESTAGE_INPUT_HZ,
        design.PRESTAGE_PASSBAND_HZ,
        design.PRESTAGE_STOPBAND_HZ,
    )
    pre_levels = {freq: prestage_tone_level(freq, hpre) for freq in PRESTAGE_TONES}
    if pre_ripple > 0.1 or pre_stop > -100.0:
        failures.append(
            f"96-to-48 response ripple={pre_ripple:.6f} stop={pre_stop:.2f} dB"
        )
    if abs(pre_levels[1000] - (-1.0)) > 0.05:
        failures.append(f"96-to-48 1 kHz gain is {pre_levels[1000]:.3f} dBFS")
    # 6300 Hz is just inside the passband edge and must survive: a pre-stage that ate the top of
    # the 16 kHz chain's band would be a regression the alias checks below cannot see.
    if abs(pre_levels[6300] - (-1.0)) > 0.1:
        failures.append(f"96-to-48 6300 Hz passband edge is {pre_levels[6300]:.3f} dBFS")
    for freq in (41000, 44000, 48000):
        if pre_levels[freq] > -90.0:
            failures.append(
                f"96-to-48 {freq} Hz alias {pre_levels[freq]:.2f} dBFS exceeds -90 dBFS"
            )

    # Composed-chain transparency.  The quantity that matters is whether the relaxed pre-stage
    # DEGRADES the existing 48 kHz chain, so each chain is evaluated twice -- with and without the
    # pre-stage -- and the bar is on the DIFFERENCE.  Absolute ripple is the wrong bar and would
    # trip on the second stage's own transition band: sweeping the /3 chain to 0.8 of its final
    # Nyquist (6400 Hz) reads 0.33 dB of ripple WITH THE PRE-STAGE ABSENT, because its designed
    # passband ends at 5900 Hz.  That is the same band-edge trap that produced the spurious
    # -99.78 dB reading in study section 6.7.  Hence: sweep each chain's OWN designed passband,
    # and assert on added droop.
    #
    # Stage rates are explicit because they differ per chain -- /3 and the /6's and /4's first
    # stages run at 48 kHz, the /6's second stage at 16 kHz and the /4's at 24 kHz.
    prestage_chains = (
        (16000.0, "/2 +/3", design.MIDRATE_PASSBAND_HZ, ((h16, 48000.0),)),
        (12000.0, "/2 +/4", design.QUARTER12K_PASSBAND_HZ, ((hq12_1, 48000.0), (hq12_2, 24000.0))),
        (11025.0, "/2 +/4", design.QUARTER_PASSBAND_HZ, ((hq_1, 48000.0), (hq_2, 24000.0))),
        (8000.0, "/2 +/6", design.PASSBAND_HZ, ((h1, 48000.0), (h2, 16000.0))),
    )
    cascade_report: list[str] = []
    for final_hz, label, passband_hz, chain in prestage_chains:
        baseline = chain_gain_db(chain, passband_hz)
        composed = chain_gain_db(((hpre, 96000.0),) + chain, passband_hz)
        added = composed - baseline
        added_droop = float(np.min(added))
        c_ripple = float(np.ptp(composed))
        cascade_report.append(
            f"{final_hz:.0f}Hz({label},pb{passband_hz:.0f}) ripple={c_ripple:.4f} "
            f"added={added_droop:+.6f}"
        )
        # The pre-stage is designed flat to 6400 Hz, above every chain's passband, so any real
        # in-band droop it introduced would show up here as a negative number.
        if added_droop < -0.01 or float(np.max(added)) > 0.01:
            failures.append(
                f"96k pre-stage alters the {final_hz:.0f} Hz chain in band by "
                f"{added_droop:+.6f} dB over 0..{passband_hz:.0f} Hz"
            )
        if c_ripple > 0.1:
            failures.append(
                f"96k cascade to {final_hz:.0f} Hz has {c_ripple:.4f} dB ripple over its own "
                f"0..{passband_hz:.0f} Hz passband"
            )

    dc = cascade(np.ones(FS_IN, dtype=np.float64), h1, h2)[4096:]
    dc_error = float(abs(np.mean(dc) - 1.0))
    if dc_error > 1.0e-6:
        failures.append(f"DC error {dc_error:.3e} exceeds 1e-6")

    impulse = np.zeros(4096, dtype=np.float64)
    impulse[0] = 1.0
    impulse_output = cascade(impulse, h1, h2)
    if not np.all(np.isfinite(impulse_output)):
        failures.append("impulse response contains non-finite samples")

    print(f"coeff_crc32=0x{design.coefficient_crc32(h1, h2):08X}")
    print(f"block_counts_16={block_counts} long_count={predicted}/{expected_long}")
    print(
        f"midrate_crc32=0x{design.coefficient_crc32(h16):08X} "
        f"block_counts_16={mid_block_counts} long_count={mid_long}/{long_input // 3}"
    )
    print(f"passband_ripple_db={ripple:.6f} gain_at_3k2_db={edge_gain:.6f}")
    print(f"dc_error={dc_error:.3e} impulse_peak={float(np.max(np.abs(impulse_output))):.9f}")
    print("tone_hz  level_dbfs  thdn_db")
    for freq, (level, thdn) in metrics.items():
        print(f"{freq:7d}  {level:10.3f}  {thdn:7.2f}")
    print(
        f"midrate_ripple_db={mid_ripple:.6f} midrate_stop_db={mid_stop:.2f} "
        + " ".join(f"{freq}Hz={level:.2f}dBFS" for freq, level in mid_levels.items())
    )
    print(
        f"halfrate_crc32=0x{design.coefficient_crc32(h24):08X} "
        f"block_counts_16={half_block_counts} long_count={half_long}/{long_input // 2}"
    )
    print(
        f"halfrate_ripple_db={half_ripple:.6f} halfrate_stop_db={half_stop:.2f} "
        + " ".join(f"{freq}Hz={level:.2f}dBFS" for freq, level in half_levels.items())
    )
    print(f"halfrate22k_crc32=0x{design.coefficient_crc32(h22):08X}")
    print(
        f"halfrate22k_ripple_db={half22_ripple:.6f} halfrate22k_stop_db={half22_stop:.2f} "
        + " ".join(f"{freq}Hz={level:.2f}dBFS" for freq, level in half22_levels.items())
    )
    print(f"reuse_check_24k_set_at_11500Hz={reuse_11500:.2f}dBFS")
    print(
        f"prestage_crc32=0x{design.coefficient_crc32(hpre):08X} "
        f"block_counts_16={pre_block_counts} long_count={pre_long}/{long_input // 2}"
    )
    print(
        f"prestage_ripple_db={pre_ripple:.6f} prestage_stop_db={pre_stop:.2f} "
        + " ".join(f"{freq}Hz={level:.2f}dBFS" for freq, level in pre_levels.items())
    )
    print("prestage_cascade: " + "  ".join(cascade_report))

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: fixed-decimator host acceptance")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
