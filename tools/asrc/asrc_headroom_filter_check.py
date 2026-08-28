"""Host-side spectral gate for the ASRC headroom candidates.

This reproduces the firmware's phase-row construction and checks prototype
passband/image limits before a candidate is allowed onto hardware.  It is not a
replacement for end-to-end THD+N/SFDR measurements on the target.
"""

from __future__ import annotations

import dataclasses
import math

import numpy as np


L = 128
AUDIO_EDGE = 20_000.0 / 48_000.0
NFFT = 1 << 18


@dataclasses.dataclass(frozen=True)
class Candidate:
    name: str
    taps: int
    cutoff: float
    window: str
    kaiser_beta: float | None
    max_edge_loss_db: float
    max_image_db: float


CANDIDATES = (
    Candidate("baseline-m32-bh4", 32, 0.450, "bh4", None, -2.0, -105.0),
    Candidate("headroom-m30-kaiser11", 30, 0.465, "kaiser", 11.0, -1.1, -105.0),
    Candidate("headroom-m28-kaiser10.55", 28, 0.4632, "kaiser", 10.55, -1.1, -105.0),
)


@dataclasses.dataclass(frozen=True)
class DecimatorStage:
    """One FIR stage of a 48kHz front-end cascade, evaluated at its own rate."""

    taps: int
    fs: float
    passband: float
    stopband: float
    kaiser_beta: float = 11.0


def decimator_stage_response(stage: DecimatorStage) -> np.ndarray:
    """Kaiser-windowed-sinc lowpass, same construction as
    tools/asrc/asrc_decimator_48_to_8_design.py's design()."""
    center = (stage.passband + stage.stopband) * 0.5
    offset = np.arange(stage.taps, dtype=np.float64) - (stage.taps - 1) * 0.5
    coeff = (
        2.0 * center / stage.fs
        * np.sinc(2.0 * center / stage.fs * offset)
        * np.kaiser(stage.taps, stage.kaiser_beta)
    )
    return coeff / coeff.sum()


def _mag_at(coeff: np.ndarray, fs: float, freqs_hz: np.ndarray) -> np.ndarray:
    spectrum = np.abs(np.fft.rfft(coeff, NFFT))
    freq_axis = np.linspace(0.0, fs * 0.5, spectrum.size)
    return np.interp(freqs_hz, freq_axis, spectrum)


# den == 3 now serves a 16 kHz OUTPUT (2026-07-29).  It used to be the 11.025 kHz path's
# two-stage front end -- 69 taps to 16 kHz plus a 75-tap decimate-by-1 stage there -- but the
# /4 respec moved 11.025 kHz to den == 4 and those coefficients no longer exist in the firmware,
# so the row that gated them is gone with them (reviving the /3 chain means re-deriving
# those coefficients; see [internal] study_asrc_lowpass_per_rate_2026-07-29.md section 11).
#
# Single stage, forced: a 3:1 decimation to 16 kHz creates the fold itself, and the 16 kHz
# output's Nyquist IS the intermediate's, so no later stage can repair it (report section 12).
# Hence one long filter with its stopband on 8000 Hz.  161 taps rather than 157 so the stage
# clears -100 dB ON ITS OWN: at 157 it measured -92.3 dB and only the resampler's fc = 7440 Hz
# rolloff carried the cascade over the line, which is help a single-stage front end must not
# depend on.
DECIMATOR_16000_STAGE = DecimatorStage(161, 48000.0, 5900.0, 8000.0)
OUTPUT_NYQUIST_16000_HZ = 8000.0
# den == 2 serves a 24 kHz OUTPUT (2026-07-29).  Structurally the same forced single stage as
# /3 -- a 2:1 decimation to 24 kHz creates the fold itself and the output Nyquist IS the
# intermediate's, so nothing downstream can repair it -- with the stopband pinned on 12000 Hz.
# NOT a half-band: that needs the stopband above the output Nyquist with the resampler covering
# the gap, and the resampler's 30-tap prototype transitions far too gently for it (every
# candidate from +-300 to +-1800 Hz around 12000 Hz measured over 401 taps).
# 107 taps is the smallest odd count clearing -100 dB with the STAGE ALONE, the same bar the
# 16 kHz stage was held to: alone -102.37 dB, cascade -115.91 dB.  At 105 taps the cascade still
# passes (-108.32 dB) while the stage alone is only -92.41 dB, which is precisely the downstream
# dependence a single-stage front end must not take on.
DECIMATOR_24000_STAGE = DecimatorStage(107, 48000.0, 8850.0, 12000.0)
OUTPUT_NYQUIST_24000_HZ = 12000.0
# den == 2 also serves a 22.05 kHz OUTPUT (2026-07-29) -- the first rate here that is not
# 48000/den.  The stage still decimates 48 -> 24 kHz (den == 2 is the only integer choice whose
# intermediate rate reaches 22.05 kHz) and the resampler then pulls 24000 -> 22050 at step 1.0884,
# so the OUTPUT Nyquist is 11025 Hz -- 975 Hz BELOW the intermediate's, unlike every row above.
# That gap is why the 24 kHz coefficients cannot be reused: measured with the stopband left at
# 12000 Hz the cascade is -24.70 dB and the stage alone -19.74 dB, because 11025-12000 Hz walks
# through the stage at only ~-19 dB and the resampler's fc = 0.465*24000 = 11160 Hz sits ABOVE
# 11025 Hz, so it cannot remove what then folds.  Hence a second coefficient set with its
# stopband pinned on 11025 Hz.
# 107 taps again, and shared with the 24 kHz set on purpose: 7875 = 11025 - 3150 keeps the same
# 3150 Hz transition width, and at a fixed input rate taps follow transition width -- so the
# variant costs no RAM, no history and no second code path.  Held to the STAGE-ALONE bar like the
# 16 k and 24 k rows (the resampler contributes only ~5 dB here).  Priced alternatives:
# 7500 Hz / 97 taps (the fallback if DSP margin is short), 8000 Hz / 113, 8200 Hz / 119.
DECIMATOR_22050_STAGE = DecimatorStage(107, 48000.0, 7875.0, 11025.0)
OUTPUT_NYQUIST_22050_HZ = 11025.0
# /4 respec (2026-07-29, report section 11): 48 -> 24 -> 12 kHz. Stage 1's stopband edge is
# 24000 - 5512.5 = 18487.5 Hz -- it only has to suppress what would fold into the FINAL band
# after both halvings -- and stage 2 does the second halving with its stopband pinned at the
# output Nyquist. 4200 Hz passband, 200 Hz wider than the /3 chain carries.
DECIMATOR_11025_Q_STAGE1 = DecimatorStage(27, 48000.0, 4200.0, 18487.5)
DECIMATOR_11025_Q_STAGE2 = DecimatorStage(129, 24000.0, 4200.0, 5512.5)
OUTPUT_NYQUIST_11025_HZ = 5512.5
# 12 kHz got its own /4 coefficient set on 2026-07-29: the runtime routing gate had rows for
# 8 k and 11.025 k only, so 12 kHz ran fe=direct and aliased at 0 dB across the whole audio
# band.  Same structure and the same 27+129 taps as the 11.025 kHz set -- only the band edges
# move.  A 12 kHz output's Nyquist is 6000 Hz, 487.5 Hz above 11.025 kHz's, and stage 1's
# stopband edge follows it down to 24000 - 6000 = 18000 Hz; the slack buys 500 Hz of passband.
# See [internal] study_asrc_lowpass_per_rate_2026-07-29.md.
DECIMATOR_12000_Q_STAGE1 = DecimatorStage(27, 48000.0, 4700.0, 18000.0)
DECIMATOR_12000_Q_STAGE2 = DecimatorStage(129, 24000.0, 4700.0, 6000.0)
OUTPUT_NYQUIST_12000_HZ = 6000.0
MAX_CASCADE_IMAGE_DB = -100.0


def resampler_prototype() -> np.ndarray:
    """Interleaved polyphase prototype of the shipping resampler (headroom-m30-kaiser11).

    NOTE its sample rate: the interleave makes this an L-times-oversampled filter running at
    L * fs_intermediate, so every caller must evaluate it at L * fs_intermediate.
    """
    resampler = phase_rows(CANDIDATES[1])
    proto = np.zeros(CANDIDATES[1].taps * L)
    for phase in range(L):
        for tap in range(CANDIDATES[1].taps):
            proto[tap * L - phase + (L - 1)] = resampler[phase, tap]
    return proto / proto.sum()


def check_frontend_cascade(label: str,
                           stage1_spec: DecimatorStage,
                           stage2_spec: DecimatorStage | None,
                           intermediate_hz: float,
                           output_nyquist_hz: float = OUTPUT_NYQUIST_11025_HZ) -> bool:
    """Worst alias at the OUTPUT Nyquist for a 48 kHz front-end cascade.

    stage1 decimates 48 kHz down to the intermediate rate; stage2 runs AT stage1's output rate
    (which for the /3 chain equals the intermediate rate -- it is decimate-by-1 -- and for the /4
    chain is 24 kHz, twice the intermediate rate, because it does the second halving itself).

    output_nyquist_hz is the Nyquist of the rate actually being SERVED, which is not in general
    the intermediate rate's: the 11.025 kHz path decimates to 12 kHz and the resampler then
    pulls to 11.025 kHz, so its alias limit is 5512.5 Hz, not 6000 Hz.  Passing it explicitly
    is what lets one function gate both /4 variants.
    """
    proto = resampler_prototype()
    stage1 = decimator_stage_response(stage1_spec)
    # stage2_spec is None for the den == 2 / den == 3 structures, which have no stage after the
    # single rate-changing one; an all-ones response leaves the arithmetic below unchanged.
    stage2 = decimator_stage_response(stage2_spec) if stage2_spec is not None else None

    # 0.5 Hz steps: the worst-case point is the output Nyquist itself (5512.5 Hz),
    # which an integer-Hz grid steps straight over.
    freqs = np.linspace(0.0, 24000.0, 48001)  # 48kHz-domain input frequency
    # Any input frequency f whose image lands AT or ABOVE the 11.025k output Nyquist once the
    # front end has folded it to the intermediate rate is the alias this fix targets. Each stage
    # is evaluated at f folded into ITS OWN rate; the resampler at f folded into the intermediate
    # rate, which is the signal it actually sees.
    def fold(rate: float) -> np.ndarray:
        rem = freqs % rate
        return np.minimum(rem, rate - rem)

    folded = fold(intermediate_hz)
    # An input at f leaves the chain at fold(fold(f, intermediate), output_rate), which differs
    # from f -- i.e. f is an alias source -- exactly when f > the output Nyquist.  Anything at or
    # below it passes both folds unchanged.
    #
    # This replaced `folded >= output_nyquist_hz` on 2026-07-29.  That mask COLLAPSED whenever
    # the output Nyquist equalled the intermediate rate's (every exact-integer rate: 8 k via /6,
    # 12 k via /4, and any 16 k or 24 k candidate), because fold() is bounded by intermediate/2 --
    # so it selected 1-3 grid points instead of a band and reported one frequency as if it were a
    # worst case.  It also MISSED genuine alias sources when the output Nyquist was lower: at
    # 11.025 kHz an input at 6500 Hz folds to 5500 Hz in the 12 kHz-rate signal, below 5512.5, so
    # the old mask dropped it -- yet 6500 -> 5500 is exactly an alias.
    #
    # Every chain was recomputed and all stayed inside the -100 dB gate (/6 -108.38, /3 -106.20,
    # /4 11.025k -108.28, /4 12k -108.36): the reported figures had been 4-16 dB optimistic, not
    # the filters deficient.  This is the SECOND axis bug in this gate; the first was evaluating
    # the polyphase prototype at fs instead of L*fs (report section 11.1).
    mask = freqs > output_nyquist_hz

    h1 = _mag_at(stage1, stage1_spec.fs, fold(stage1_spec.fs))
    h2 = (_mag_at(stage2, stage2_spec.fs, fold(stage2_spec.fs))
          if stage2 is not None else np.ones_like(folded))
    # The interleaved polyphase prototype is an L-times-oversampled filter: its
    # sample rate is L * intermediate, NOT the intermediate rate. Evaluating it against
    # a 16 kHz axis (the bug fixed 2026-07-29) compressed its fc=0.465*16000=7440 Hz
    # cutoff down to 7440/L = 58 Hz, so h3 read -300 dB across the whole alias
    # band and this gate PASSed unconditionally -- it would have passed with
    # stage2 removed entirely. Control that validates the corrected axis: with
    # stage2 omitted the worst alias comes out at 0.00 dB, matching the -0.01 dB
    # 6 kHz fold measured on board 057 before the D2 fix. That control is the
    # "before" column below, so a regression here is visible rather than silent.
    h3 = _mag_at(proto, float(L) * intermediate_hz, folded)

    before_db = 20.0 * np.log10(np.maximum(h1[mask] * h3[mask], 1e-15))
    after_db = 20.0 * np.log10(np.maximum(h1[mask] * h2[mask] * h3[mask], 1e-15))
    passed = float(after_db.max()) <= MAX_CASCADE_IMAGE_DB

    print(
        f"{label:26s} {(stage2_spec.taps if stage2_spec is not None else stage1_spec.taps):4d} "
        f"{'--':>5} {'--':>9} "
        f"{before_db.max():8.1f}->  {after_db.max():6.1f}  "
        f"{'PASS' if passed else 'FAIL'}"
    )
    return passed


def window_value(kind: str, position: float, kaiser_beta: float | None) -> float:
    if kind == "bh4":
        return (
            0.35875
            - 0.48829 * math.cos(2.0 * math.pi * position)
            + 0.14128 * math.cos(4.0 * math.pi * position)
            - 0.01168 * math.cos(6.0 * math.pi * position)
        )
    if kind == "kaiser":
        if kaiser_beta is None:
            raise ValueError("kaiser window requires beta")
        x = 2.0 * position - 1.0
        return float(
            np.i0(kaiser_beta * math.sqrt(max(0.0, 1.0 - x * x)))
            / np.i0(kaiser_beta)
        )
    raise ValueError(f"unsupported window: {kind}")


def phase_rows(candidate: Candidate) -> np.ndarray:
    taps = candidate.taps
    midpoint = taps // 2 - 1
    rows = np.zeros((L, taps), dtype=np.float64)
    for phase in range(L):
        for tap in range(taps):
            distance = tap - midpoint - phase / L
            x = 2.0 * candidate.cutoff * distance
            sinc = 1.0 if abs(x) < 1.0e-6 else math.sin(math.pi * x) / (math.pi * x)
            position = (distance + midpoint + 1.0) / taps
            rows[phase, tap] = (
                2.0
                * candidate.cutoff
                * sinc
                * window_value(candidate.window, position, candidate.kaiser_beta)
            )
        rows[phase] /= rows[phase].sum()
    return rows


def analyze(candidate: Candidate) -> tuple[float, float, float]:
    rows = phase_rows(candidate)
    prototype = np.zeros(candidate.taps * L, dtype=np.float64)
    for phase in range(L):
        for tap in range(candidate.taps):
            prototype[tap * L - phase + (L - 1)] = rows[phase, tap]
    prototype /= prototype.sum()
    response = 20.0 * np.log10(
        np.maximum(np.abs(np.fft.rfft(prototype, NFFT)), 1.0e-15)
    )
    normalized_frequency = np.arange(len(response)) * L / NFFT
    passband = response[normalized_frequency <= AUDIO_EDGE]
    edge = response[np.argmin(np.abs(normalized_frequency - AUDIO_EDGE))]
    image = response[normalized_frequency >= (1.0 - AUDIO_EDGE)].max()
    return float(passband.max() - passband.min()), float(edge), float(image)


def main() -> int:
    failed = False
    print("candidate                  taps  MAC%    ripple    @20k    worst-image  gate")
    for candidate in CANDIDATES:
        ripple, edge, image = analyze(candidate)
        passed = edge >= candidate.max_edge_loss_db and image <= candidate.max_image_db
        failed |= not passed
        print(
            f"{candidate.name:26s} {candidate.taps:4d} "
            f"{100.0 * candidate.taps / 32.0:5.2f} "
            f"{ripple:9.3f} {edge:8.3f} {image:14.1f}  "
            f"{'PASS' if passed else 'FAIL'}"
        )
    failed |= not check_frontend_cascade(
        "11025-cascade /4 (4200Hz)",
        DECIMATOR_11025_Q_STAGE1, DECIMATOR_11025_Q_STAGE2, 12000.0)
    failed |= not check_frontend_cascade(
        "12000-cascade /4 (4700Hz)",
        DECIMATOR_12000_Q_STAGE1, DECIMATOR_12000_Q_STAGE2, 12000.0,
        OUTPUT_NYQUIST_12000_HZ)
    failed |= not check_frontend_cascade(
        "16000-single /3 (5900Hz)",
        DECIMATOR_16000_STAGE, None, 16000.0,
        OUTPUT_NYQUIST_16000_HZ)
    failed |= not check_frontend_cascade(
        "24000-single /2 (8850Hz)",
        DECIMATOR_24000_STAGE, None, 24000.0,
        OUTPUT_NYQUIST_24000_HZ)
    failed |= not check_frontend_cascade(
        "22050-single /2 (7875Hz)",
        DECIMATOR_22050_STAGE, None, 24000.0,
        OUTPUT_NYQUIST_22050_HZ)
    # The three single-stage rows above are additionally required to clear the gate WITHOUT the
    # resampler.  Their structure creates the fold itself, so passing only in cascade would mean
    # depending on a downstream filter that cannot in principle repair the alias -- the "before"
    # column of a cascade row is not that check, since it still includes the resampler.
    # For the 22.05 kHz row this is the BINDING check, not a belt-and-braces one: measured, the
    # resampler adds only ~5 dB there (cascade -24.70 vs stage alone -19.74 when the 24 kHz
    # coefficients were tried), so a cascade-only gate would have passed sets that alias audibly.
    for label, stage, nyq in (("16000-single /3", DECIMATOR_16000_STAGE, OUTPUT_NYQUIST_16000_HZ),
                              ("24000-single /2", DECIMATOR_24000_STAGE, OUTPUT_NYQUIST_24000_HZ),
                              ("22050-single /2", DECIMATOR_22050_STAGE, OUTPUT_NYQUIST_22050_HZ)):
        freqs = np.linspace(0.0, 24000.0, 48001)
        folded = np.minimum(freqs % stage.fs, stage.fs - (freqs % stage.fs))
        mag = _mag_at(decimator_stage_response(stage), stage.fs, folded)
        alone = float(20.0 * np.log10(np.maximum(mag[freqs > nyq], 1e-15)).max())
        passed = alone <= MAX_CASCADE_IMAGE_DB
        failed |= not passed
        print(f"{label + ' STAGE ALONE':26s} {stage.taps:4d} {'--':>5} {'--':>9} "
              f"{'--':>8}->  {alone:6.1f}  {'PASS' if passed else 'FAIL'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
