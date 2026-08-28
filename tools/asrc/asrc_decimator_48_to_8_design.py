#!/usr/bin/env python3
"""Generate and verify the fixed 48 kHz to 8 kHz decimator coefficients.

Only NumPy is required.  The generated include stores float32 coefficients;
the reported CRC32 is over their little-endian IEEE-754 bytes, stage 1 first.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_8_coeffs.inc"
MIDRATE_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_16_coeffs.inc"
HALFRATE_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_24_coeffs.inc"
QUARTER_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_12_coeffs.inc"
PRESTAGE_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_96_to_48_coeffs.inc"
POLY32_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_32_coeffs.inc"
TONE_OUTPUT = ROOT / "src/app/apps/asrc/asrc_decimator_48_to_8_meas_tone.inc"

PASSBAND_HZ = 3200.0
FINAL_STOPBAND_HZ = 4000.0
KAISER_BETA = 11.0
STAGE1_INPUT_HZ = 48000.0
STAGE1_STOPBAND_HZ = 12800.0
STAGE1_TAPS = 43
STAGE2_INPUT_HZ = 16000.0
STAGE2_STOPBAND_HZ = FINAL_STOPBAND_HZ
STAGE2_TAPS = 147
# den == 3 -- the 48 -> 16 kHz decimator.  REPURPOSED 2026-07-29.
#
# It used to be the two-stage front end for the 11.025 kHz path (69 taps at 48 kHz to reach
# 16 kHz, then a 75-tap decimate-by-1 stage at 16 kHz to pull the band down to the 5512.5 Hz
# output Nyquist).  The /4 respec moved 11.025 kHz to den == 4, which left den == 3 selected by
# no rate at all -- 6286 bytes of flash and a boot selftest chain for code the audio path could
# never reach.  den == 3 now serves a **16 kHz output**, which is what the name always implied.
#
# SINGLE STAGE, and that is forced, not a simplification.  A 3:1 decimation to 16 kHz folds
# everything at multiples of 16 kHz, and the output Nyquist IS 8000 Hz -- the intermediate's own
# Nyquist -- so every input above 8000 Hz lands somewhere inside the final band during the
# decimation itself.  No stage placed after it can help: once two frequencies share an output
# bin they cannot be separated.  Measured control (report section 12.1): relaxing stage 1 and
# adding a 75-tap "repair" stage at 16 kHz moves the worst alias from -14.8 dB only to
# -21.2 dB.  So the single stage carries the whole anti-alias job with its stopband pinned on
# 8000 Hz, and it needs to be long: 161 taps against the /4 chain's 27-tap first stage, because
# a den == 4 or 6 first stage gets a huge transition band for free (it only has to suppress what
# would fold into the FINAL band after the remaining halvings) while den == 2 and 3 have no
# remaining rate change and pay the full transition.
#
# Passband 5900 Hz (0.74 of the 8000 Hz Nyquist) was chosen from this trade -- taps against DSP
# budget, all RAM-neutral (the union ceiling is 190 taps' worth of history):
#   5600 Hz -> 137 taps -> 45.7 units -> ~37 us -> predicted margin ~35 us (12 kHz class)
#   5900 Hz -> 161 taps -> 53.7 units -> ~44 us -> predicted margin ~29 us   <- chosen
#   6100 Hz -> 173 taps -> 57.7 units -> ~47 us -> predicted margin ~25 us (thinnest)
#   6300 Hz -> 193 taps -> RAM grows past the union ceiling
# Cascade alias -116.1 dB out of band, passband edge -0.01 dB (gate -100 dB).
#
# 161 taps rather than the 157 that the transition width alone would suggest.  At 157 the stage's
# OWN stopband is only -92.3 dB: a Kaiser beta=11 design reaches its -108 dB asymptote only once
# the tap count covers the transition, and 5900 -> 8000 Hz at 48 kHz needs about 159.  The
# cascade at 157 taps still measured -108.3 dB, because the resampler's fc = 0.465*16000 =
# 7440 Hz happens to cover exactly the stage's weak spot just above 8000 Hz (content at 8100 Hz
# folds to 7900 Hz, where the resampler is already rolling off).  That is a real and stable
# effect -- but the whole point of a single-stage front end here is that nothing downstream can
# be relied on, so leaning on it to clear the gate would contradict the structure.  161 taps
# costs 1.1 us and takes the stage alone to -107.5 dB and the cascade to -116.1 dB, which is
# cheaper than giving up passband (157 taps needs 5850 Hz to clear the same bar).
#
# The resampler's contribution is therefore a margin bonus, not a dependency: stage alone
# -107.5 dB, cascade -116.1 dB.
#
# See [internal] study_asrc_lowpass_per_rate_2026-07-29.md for the per-rate survey and
# report section 12 for why an alias cannot be repaired downstream of the fold that created it.
MIDRATE_PASSBAND_HZ = 5900.0
MIDRATE_STOPBAND_HZ = 8000.0
MIDRATE_TAPS = 161

# /4 respec of the 11.025 kHz path (2026-07-29): 48 -> 24 -> 12 kHz instead of the
# /3 chain's 48 -> 16 kHz. Priced in section 11 of
# [internal] report_asrc_d2_alias_route_2026-07-28.md; option B of the
# menu there was signed off, trading part of the saving for passband instead of
# taking all of it as DSP margin:
#   front end 49.1 us vs the /3 chain's measured 51.5 us   (-2.4 us)
#   pull      113.2 us vs 116.9 us, since step falls 1.45125 -> 1.08843  (-3.7 us)
#   passband  4200 Hz vs 4000 Hz                            (+200 Hz)
#   alias     -108.3 dB cascade at the 5512.5 Hz output Nyquist (limit -100)
#             (was reported as -112.1 dB until the gate's alias mask was corrected on
#              2026-07-29 -- see the note above QUARTER12K below; the choice stands)
# The 12 kHz intermediate does NOT let the resampler absorb the anti-alias job --
# with stage 2 removed the cascade measures -0.2 dB, i.e. the resampler contributes
# essentially NOTHING. The reason is where aliases arrive: the worst ones land at
# multiples of the intermediate rate (12.1 kHz folds to 100 Hz), and at those fold
# centres the resampler sees a LOW frequency and passes it at full level. Its
# fc = 0.465*12000 = 5580 Hz never gets to act on them. So stage 2 is
# still mandatory, and because its input rate doubles (16 -> 24 kHz) its relative
# transition halves and it needs MORE taps than the /3 chain's stage 2, not fewer.
# All of the saving comes from stage 1: 27 taps at 24 kHz replaces 69 at 16 kHz.
#
# Stage 1's stopband edge is 24000 - 5512.5 = 18487.5 Hz: it only has to suppress
# what would fold into the FINAL band after both halvings, which is why such a
# short filter suffices. Stage 2's stopband edge is pinned at the output Nyquist.
QUARTER_PASSBAND_HZ = 4200.0
QUARTER_STAGE1_INPUT_HZ = 48000.0
QUARTER_STAGE1_STOPBAND_HZ = 18487.5
QUARTER_STAGE1_TAPS = 27
QUARTER_STAGE2_INPUT_HZ = 24000.0
QUARTER_STAGE2_STOPBAND_HZ = 5512.5
# 129, not the 127 that first scrapes the -100 dB limit: 127 is worth only -101.6 dB
# while 129 reaches -108.3 dB for 0.6 us more.  (Figures corrected 2026-07-29 with the
# fixed alias mask -- previously read -101.5 and -112.1 dB.  The ordering, and therefore
# the choice of 129, is unchanged; only the depths moved.)
QUARTER_STAGE2_TAPS = 129

# Second /4 coefficient set, for a 12 kHz OUTPUT (2026-07-29).  Same structure and -- the
# point of the exercise -- the same TAP COUNTS as the 11.025 kHz set above, so both variants
# share the union member, the mirrored-history layout and the tap-loop constants.  Selecting
# one then costs a coefficient pointer rather than a second copy of the ISR code, and RAM is
# unchanged.  Only the three band edges move:
#
#   output Nyquist   6000.0 Hz  (12 kHz), 487.5 Hz above the 11.025 kHz path's 5512.5 Hz
#   stage 1 stopband 18000.0 Hz = 24000 - 6000, the fold-into-final-band edge
#   passband          4700 Hz vs 4200 Hz -- the whole benefit: 500 Hz more audio for free,
#                     because the same 129 taps face a 487.5 Hz looser stopband edge
#
# Measured cascade (tools/asrc/asrc_lowpass_rate_survey.py, gate limit -100 dB).  These are the
# CORRECTED figures -- the gate's alias mask was wrong until 2026-07-29 and had been reporting
# these rows 7-25 dB optimistic; see the mask note in worst_alias() there.  The passband choice
# is unaffected: 4800 Hz still fails, so 4700 Hz is still the widest the 129-tap frame carries.
#   passband 4200 Hz -> -106.94 dB   (the 11.025 kHz set's passband, for reference)
#   passband 4700 Hz -> -108.36 dB worst alias, -0.13 dB cascade passband edge   <- chosen
#   passband 4800 Hz ->  -90.70 dB  (fails; the 129-tap frame is the binding constraint)
#   passband 5100 Hz ->  -58.00 dB  (fails badly)
# Control, stage 2 omitted: -0.20 dB -- the resampler contributes essentially nothing, so
# stage 2 is doing the entire job.  See [internal] study_asrc_lowpass_per_rate_2026-07-29.md.
#
# Do NOT read the per-stage "quarter12k_2 ... stop=-99.77 dB" line printed below as a failure:
# that is stage 2 EVALUATED ALONE against its own stopband edge, which is not the release
# metric.  The gate is the worst alias of the whole cascade (both stages plus the resampler)
# over the entire out-of-band region, and that measures -108.36 dB against a -100 dB limit.
QUARTER12K_PASSBAND_HZ = 4700.0
QUARTER12K_STAGE1_STOPBAND_HZ = 18000.0
QUARTER12K_STAGE2_STOPBAND_HZ = 6000.0

# den == 2 -- the 48 -> 24 kHz decimator, for a 24 kHz OUTPUT (2026-07-29).
#
# SINGLE STAGE, forced by the same structure that forces it for den == 3: a 2:1 decimation to
# 24 kHz folds at multiples of 24 kHz and the 24 kHz output's Nyquist IS the intermediate's
# (12000 Hz), so every input above 12000 Hz lands inside the final band during the decimation
# itself.  Nothing placed after it can separate frequencies already summed into one bin (report
# section 12).  So the stopband is pinned ON 12000 Hz and this filter carries the whole job.
#
# HALF-BAND SPECIALISATION WAS PRICED AND REJECTED.  The obvious saving for a 2:1 stage is a
# half-band filter (transition centred on fs/4, every other tap zero, ~half the multiplies), but
# that requires the stopband to start ABOVE the output Nyquist and leans on the resampler to
# cover the gap.  Measured: every candidate from +-300 Hz to +-1800 Hz around 12000 Hz needs
# more than 401 taps, because the resampler's 30-tap prototype has far too gentle a transition
# to cover a half-band transition band.  The saving is negative, so the stopband stays pinned.
#
# 107 taps is the smallest ODD count that clears -100 dB with the stage evaluated ALONE, which
# is the bar the 16 kHz /3 stage was held to and for the same reason -- a single-stage front end
# must not depend on downstream help.  The cascade figure is a bonus, not the gate:
#   103 taps -> stage alone  -85.81 dB, cascade -101.71 dB   (cascade passes, stage does not)
#   105 taps -> stage alone  -92.41 dB, cascade -108.32 dB   (same)
#   107 taps -> stage alone -102.37 dB, cascade -115.91 dB   <- chosen, 53.5 units
#   109 taps -> stage alone -109.14 dB, cascade -116.06 dB
# Passband edge loss -0.010 dB.  RAM cost is zero: the union that holds every front end is sized
# by the /6 member (190 taps' worth of history), and 107 is far below that ceiling.
#
# Passband 8850 Hz = 0.7375 of the 12000 Hz Nyquist -- the SAME relative width the 16 kHz stage
# carries (5900 / 8000).  Two independent cross-checks that this is the consistent choice rather
# than a coincidence: the relative passband matches, and scaling the 16 kHz stage's tap count by
# the transition-width ratio gives 161 * (2100 / 3150) = 107.3.
#
# If the measured DSP margin turns out too thin, the ONLY remaining lever is a narrower
# passband (half-band is closed, above).  Priced under the same two bars:
#   8400 Hz ->  95 taps -> 47.5 units  (-6.0 units, stage alone -108.66 dB)
#   8000 Hz ->  85 taps -> 42.5 units  (-11.0 units, stage alone -106.71 dB)
# See [internal] study_asrc_lowpass_per_rate_2026-07-29.md section 6.9.
HALFRATE_PASSBAND_HZ = 8850.0
HALFRATE_STOPBAND_HZ = 12000.0
HALFRATE_TAPS = 107

# Second /2 coefficient set, for a 22.05 kHz OUTPUT (2026-07-29).  Same relationship to the
# 24 kHz set above as QUARTER12K_ has to QUARTER_: two coefficient sets over ONE structure,
# sharing the tap count -- and therefore the union member, the mirrored-history layout and the
# tap-loop literals.  Selecting one costs a coefficient pointer, not a second ISR code path.
#
# 22.05 kHz is NOT 48000/2, so the structure differs from the 24 kHz case in an important way:
# the stage still decimates 48 -> 24 kHz (den == 2 is the only integer choice whose intermediate
# rate is >= 22.05 kHz), and the RESAMPLER then pulls 24000 -> 22050 at step 1.0884.  So the
# intermediate Nyquist is 12000 Hz but the OUTPUT Nyquist is 11025 Hz, 975 Hz lower.
#
# REUSING THE 24 kHz SET WAS PRICED FIRST AND FAILS BADLY.  It is the obvious move -- same
# structure, same taps, zero new coefficients -- but the 24 kHz filter is only 18.91 dB down at
# 11025 Hz, so input between 11025 and 12000 Hz walks through the stage and then folds into the
# 22.05 kHz band in the resampler.  Measured with the stopband left at 12000 Hz:
#   cascade -24.70 dB, stage alone -19.74 dB  (limit -100 dB) -- FAIL by ~80 dB.
# Hence the stopband is re-pinned ON the OUTPUT Nyquist, 11025 Hz.
#
# The resampler contributes almost nothing here (-24.70 cascade vs -19.74 alone is ~5 dB), so
# this set is held to the STAGE-ALONE -100 dB bar, exactly like the 24 kHz and 16 kHz stages.
# Tap search at that bar (units = taps * 24000/48000, i.e. taps * 0.5):
#   passband 6800 Hz ->  81 taps -> 40.5 units, stage alone -109.02 dB
#   passband 7500 Hz ->  97 taps -> 48.5 units, stage alone -109.08 dB
#   passband 7875 Hz -> 107 taps -> 53.5 units, stage alone -102.22 dB   <- chosen
#   passband 8000 Hz -> 113 taps -> 56.5 units, stage alone -108.84 dB
#   passband 8200 Hz -> 119 taps -> 59.5 units, stage alone -100.97 dB
# 7875 Hz = 11025 - 3150, i.e. the SAME 3150 Hz transition width the 24 kHz set carries -- which
# is why the tap count comes out identical at 107 (taps follow transition width in Hz at a fixed
# input rate).  That identity is the whole reason the variant is free: shared tap count means
# shared structure, so RAM and flash both stay put.
#
# Margin risk, stated up front: the stage costs the same 53.5 units as the 24 kHz set AND the
# resampler runs non-unity (step 1.0884) instead of at 1.0, so 22.05 kHz may displace 24 kHz as
# the thinnest rate -- and 24 kHz already measures only 18.9-21.1 us of TDMsum margin.  If it
# does not fit, the lever is a narrower passband (7500 Hz / 97 taps, -5.0 units, priced above);
# half-band specialisation is closed for den == 2 for the reason given in the block above.
HALFRATE22K_PASSBAND_HZ = 7875.0
HALFRATE22K_STOPBAND_HZ = 11025.0

# The 96 -> 48 kHz PRE-STAGE (2026-08-02).  Sits in FRONT of every block above, and it is the
# first filter in this file whose input is not 48 kHz -- hence its own PRESTAGE_INPUT_HZ.
#
# WHY IT EXISTS.  Against a 96 kHz leg A, the look-ahead one ASRC pull needs,
#   R(step) = floor(step * (APP_BLOCK_FRAMES - 1)) + ASRC_POLY_AHEAD + 1
# is proportional to the rate ratio, while the ring offers at most ASRC_FILL_TARGET_MAX = 104
# frames (FIFO 128, BLOCK 16).  96 k -> 16 k needs 110 and 96 k -> 8 k needs 200, so the setpoint
# CLAMPS: the tail outputs of each block fail the window test, emit zeros and hold rd -- audible
# break-up.  A /2 in front halves the step the ASRC sees, taking R+jitter to 35..36.  (Halving
# APP_BLOCK_FRAMES instead was measured and rejected: duty 62.3 -> 82.2 %, margin down to 14.8 us,
# and 11.025 kHz still broken.  See [internal] asrc_96k.md part 3 section 3.)
#
# 21 TAPS, NOT 107, AND THE DIFFERENCE IS THE WHOLE POINT.  Scaling the proven 48 -> 24 kHz /2
# stage to a 96 kHz input (passband 0.7375 of Nyquist -> 17700 Hz) also gives 107 taps, but its
# output is 48 kHz rather than 24 kHz, so under the units = taps * (output_rate / 48000) cost model
# each tap costs TWICE as much: 107 units ~ 87.7 us of a 166.6 us window, which prices the feature
# out at every rate and nearly produced a false no-go (study section 6.1).
#
# That 107-tap figure is the price of a /2 that must stand ALONE -- correct only where the stage's
# output IS the final rate.  Here it is the FIRST OF TWO stages, exactly like the /6 chain's 27-tap
# first stage, whose own header states the principle: it is short because "a later rate change is
# still pending, so it only has to suppress what would fold into the FINAL band after that
# change."  So the passband only has to protect the final band, and the stopband only has to start
# where energy would fold into it after the remaining decimation, at 48000 - final_Nyquist.  That
# is an enormous transition width, and 21 taps clear the same stage-alone -100 dB bar (17.2 us).
#
# ONE SHARED SET COVERS ALL FOUR RATES -- no variant enum, no coefficient pointer, no `fe=` tag,
# which is a genuine simplification over the /4 and /2 stages it sits in front of.  Designed
# against the TIGHTEST case of the four: passband 6400 Hz (16 kHz needs it flat) and stopband from
# 48000 - 8000 = 40000 Hz (16 kHz's edge, the lowest of the four).  Measured stage-alone at each
# rate's own stopband edge, with the resulting passband-edge loss:
#    8000 -> edge 44000 -> -107.50 dB, pb edge -0.0001 dB
#   11025 -> edge 42488 -> -107.50 dB, pb edge -0.0000 dB
#   12000 -> edge 42000 -> -107.50 dB, pb edge -0.0000 dB
#   16000 -> edge 40000 -> -104.28 dB, pb edge -0.0000 dB   <- the binding case
# Full-cascade check over each chain's own designed passband: ripple 0.0001 dB, min -0.0001 dB,
# i.e. the pre-stage is transparent in band and adds no droop.  RAM cost +336 B (21 taps of
# mirrored, channel-interleaved history); it does NOT share the union with the stages behind it,
# because a composed chain runs both at once.
#
# DO NOT re-litigate the -99.78 dB reading at the 12 kHz cascade's 6000 Hz band edge: it is a
# sampling artifact, not a defect.  It sits exactly ON the existing /4 chain's own stopband edge,
# reads -100.70 dB one Hz above and -108.35 dB fifty Hz above, and does not move with pre-stage tap
# count (21/23/25 all give -99.78 dB) -- the tell that the pre-stage is not its source.  The
# shipping gate evaluates an alias MASK, not bare |H| at the band edge.  From a 96 kHz source the
# /4 chain alone would pass 12 kHz aliases at 0.00 dB, so the correct verdict is a ~100 dB
# improvement (study section 6.7).
PRESTAGE_INPUT_HZ = 96000.0
PRESTAGE_PASSBAND_HZ = 6400.0
PRESTAGE_STOPBAND_HZ = 40000.0
PRESTAGE_TAPS = 21

# 48 kHz -> 32 kHz "Audio mode": the L=2/M=3 rational polyphase front end.
#
# WHAT THIS IS NOT.  It is not a strict 48->32 front end and must not be described as one: it
# does NOT protect the whole 0-16 kHz output band.  The prototype's transition band runs from
# 15 kHz to the 16 kHz output Nyquist, which leaves residual aliasing in 13-16 kHz (worst
# -25.5 dB) while 0-13 kHz is protected below about -105 dB.  The full accounting is in
# [internal] study_ak512_48_to_32_bandlimit_16ch_2026-08-22.md; N = 97 is fixed by that
# study's option B and is not a knob to be turned here.
#
# STRUCTURE.  48/32 = 3/2 has no integer decimation, so the chain is conceptually
# "up L=2 / lowpass at the 96 kHz interpolated rate / down M=3".  The prototype therefore lives
# at 96 kHz, not at 48 kHz, and is decomposed into the L=2 phase rows so that the firmware never
# zero-stuffs and never computes a tap against a known-zero sample:
#
#     y[2q]   = sum_i h[2i]   * x[3q   - i]      i = 0 .. 48   (49 taps, ends at input 3q)
#     y[2q+1] = sum_i h[2i+1] * x[3q+1 - i]      i = 0 .. 47   (48 taps, ends at input 3q+1)
#
# Both rows read CONSECUTIVE input samples and both hop by M=3 input samples between successive
# outputs of the same row, which is exactly the (taps, decim) shape the existing Q31 block kernel
# already serves -- see asrc_decimator_q31.inc.
#
# GAIN.  design() normalises the prototype to unit sum, but a zero-stuffed input only ever
# presents half of the taps with a non-zero sample, so each phase row would carry a gain of 1/L.
# The rows are therefore scaled by L, which makes each row sum to ~1 and the chain unity-gain at
# DC.  Scaling by an exact power of two is exact in float32, so the emitted rows are bit-exactly
# 2 x the prototype taps.
POLY32_L = 2
POLY32_M = 3
POLY32_PROTO_TAPS = 97
POLY32_PROTO_HZ = 96000.0            # the L=2 interpolated rate the prototype is designed at
POLY32_PASSBAND_HZ = 15000.0
POLY32_STOPBAND_HZ = 16000.0         # the 32 kHz output Nyquist
POLY32_PHASE0_TAPS = (POLY32_PROTO_TAPS + 1) // 2      # 49
POLY32_PHASE1_TAPS = POLY32_PROTO_TAPS // 2            # 48

FFT_SIZE = 1 << 21
TONE_TABLE_LENGTH = 480
TONE_DBFS = -1.0


def design(taps: int, sample_rate: float, passband: float, stopband: float) -> np.ndarray:
    center = (passband + stopband) * 0.5
    offset = np.arange(taps, dtype=np.float64) - (taps - 1) * 0.5
    coeff = (
        2.0
        * center
        / sample_rate
        * np.sinc(2.0 * center / sample_rate * offset)
        * np.kaiser(taps, KAISER_BETA)
    )
    coeff /= np.sum(coeff)
    return coeff.astype("<f4")


def design_32k_polyphase() -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """The L=2/M=3 Audio-mode front end: (phase0 row, phase1 row, unit-sum prototype).

    The prototype is the ordinary windowed sinc this module designs everywhere else, only at the
    96 kHz interpolated rate.  It is returned unscaled (unit sum) because that is the right
    normalisation for a frequency-response check -- 0 dB at DC -- while the two phase rows are
    scaled by L so the firmware chain is unity gain.

    Odd tap count makes the prototype exactly symmetric, and taking every second tap of a
    symmetric odd-length set leaves each row symmetric in its own right.  That is not cosmetic:
    the kernel walks a window forward from its oldest sample, so a row would otherwise have to be
    emitted reversed.  It is asserted rather than assumed.
    """
    proto = design(POLY32_PROTO_TAPS, POLY32_PROTO_HZ,
                   POLY32_PASSBAND_HZ, POLY32_STOPBAND_HZ)
    if not np.array_equal(proto, proto[::-1]):
        raise AssertionError("the L=2/M=3 prototype is not bit-exactly symmetric")
    phase0 = (proto[0::2] * np.float32(POLY32_L)).astype("<f4")
    phase1 = (proto[1::2] * np.float32(POLY32_L)).astype("<f4")
    if phase0.size != POLY32_PHASE0_TAPS or phase1.size != POLY32_PHASE1_TAPS:
        raise AssertionError("L=2/M=3 phase row length mismatch")
    for name, row in (("phase0", phase0), ("phase1", phase1)):
        if not np.array_equal(row, row[::-1]):
            raise AssertionError(f"L=2/M=3 {name} row is not bit-exactly symmetric")
    return phase0, phase1, proto


def render_poly32(phase0: np.ndarray, phase1: np.ndarray, crc: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "/*",
        " * 48 kHz -> 32 kHz AUDIO MODE, L=2/M=3 rational polyphase.  NOT a strict 48->32 front",
        " * end: 0-13 kHz is protected, 13-16 kHz keeps residual alias (see the study).",
        " *",
        " * The two rows are the L=2 phases of a 97-tap prototype designed at the 96 kHz",
        " * interpolated rate, scaled by L so the chain is unity gain at DC:",
        " *",
        " *     y[2q]   = sum(phase0[k] * x[3q   - 48 + k]), k = 0..48",
        " *     y[2q+1] = sum(phase1[k] * x[3q+1 - 47 + k]), k = 0..47",
        " *",
        " * Each row is symmetric in its own right, so ascending order IS the kernel's",
        " * oldest-sample-first order and no row has to be stored reversed.",
        " */",
        "#ifndef ASRC_DECIMATOR_48_TO_32_COEFFS_INC",
        "#define ASRC_DECIMATOR_48_TO_32_COEFFS_INC",
        "",
        f"#define ASRC_DECIMATOR_48_TO_32_L ({POLY32_L}u)",
        f"#define ASRC_DECIMATOR_48_TO_32_M ({POLY32_M}u)",
        f"#define ASRC_DECIMATOR_48_TO_32_PROTO_COEFF_TAPS ({POLY32_PROTO_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_32_PHASE0_COEFF_TAPS ({POLY32_PHASE0_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_32_PHASE1_COEFF_TAPS ({POLY32_PHASE1_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_32_GENERATED_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_32_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
    ]
    lines.extend(format_array("s_48_to_32_phase0_coeff", phase0))
    lines.append("")
    lines.extend(format_array("s_48_to_32_phase1_coeff", phase1))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_48_TO_32_COEFFS_INC */", ""])
    return "\n".join(lines)


def response(coeff: np.ndarray, sample_rate: float, passband: float, stopband: float) -> tuple[float, float]:
    spectrum = np.fft.rfft(coeff, FFT_SIZE)
    freq = np.linspace(0.0, sample_rate * 0.5, spectrum.size)
    db = 20.0 * np.log10(np.maximum(np.abs(spectrum), 1.0e-300))
    ripple = float(np.ptp(db[freq <= passband]))
    worst_stop = float(np.max(db[freq >= stopband]))
    return ripple, worst_stop


def coefficient_crc32(*sets: np.ndarray) -> int:
    payload = b"".join(np.asarray(value, dtype="<f4").tobytes() for value in sets)
    return zlib.crc32(payload) & 0xFFFFFFFF


def format_array(name: str, values: np.ndarray) -> list[str]:
    lines = [f"static const float {name}[{values.size}u] =", "{"]
    for base in range(0, values.size, 4):
        chunk = values[base : base + 4]
        text = ", ".join(f"{float(item):.9e}f" for item in chunk)
        suffix = "," if base + 4 < values.size else ""
        lines.append(f"    {text}{suffix}")
    lines.append("};")
    return lines


def render(stage1: np.ndarray, stage2: np.ndarray, crc: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_48_TO_8_COEFFS_INC",
        "#define ASRC_DECIMATOR_48_TO_8_COEFFS_INC",
        "",
        f"#define ASRC_DECIMATOR_STAGE1_TAPS ({STAGE1_TAPS}u)",
        f"#define ASRC_DECIMATOR_STAGE2_TAPS ({STAGE2_TAPS}u)",
        f"#define ASRC_DECIMATOR_COEFF_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
    ]
    lines.extend(format_array("s_stage1_coeff", stage1))
    lines.append("")
    lines.extend(format_array("s_stage2_coeff", stage2))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_48_TO_8_COEFFS_INC */", ""])
    return "\n".join(lines)


def render_midrate(coeff: np.ndarray, crc: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_48_TO_16_COEFFS_INC",
        "#define ASRC_DECIMATOR_48_TO_16_COEFFS_INC",
        "",
        f"#define ASRC_DECIMATOR_48_TO_16_COEFF_TAPS ({MIDRATE_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_16_GENERATED_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_16_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
    ]
    lines.extend(format_array("s_48_to_16_coeff", coeff))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_48_TO_16_COEFFS_INC */", ""])
    return "\n".join(lines)


def render_prestage(coeff: np.ndarray, crc: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_96_TO_48_COEFFS_INC",
        "#define ASRC_DECIMATOR_96_TO_48_COEFFS_INC",
        "",
        f"#define ASRC_DECIMATOR_96_TO_48_COEFF_TAPS ({PRESTAGE_TAPS}u)",
        f"#define ASRC_DECIMATOR_96_TO_48_GENERATED_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_96_TO_48_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
    ]
    lines.extend(format_array("s_96_to_48_coeff", coeff))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_96_TO_48_COEFFS_INC */", ""])
    return "\n".join(lines)


def render_halfrate(coeff: np.ndarray, crc: int,
                    coeff_22k: np.ndarray, crc_22k: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_48_TO_24_COEFFS_INC",
        "#define ASRC_DECIMATOR_48_TO_24_COEFFS_INC",
        "",
        "/* Two coefficient sets over ONE structure, the same arrangement the /4 chain uses for",
        " * 11.025 vs 12 kHz.  The tap count below is shared by both variants on purpose: that is",
        " * what lets the 22.05 kHz path reuse the 24 kHz path's union member, mirrored-history",
        " * layout and tap-loop constants instead of duplicating the ISR code.  It comes out shared",
        " * for free because both sets carry the same 3150 Hz transition width at the same 48 kHz",
        " * input rate -- only the band edges move.  See the HALFRATE_ and HALFRATE22K_ blocks in",
        " * the generator, including why the 24 kHz set cannot simply be reused at 22.05 kHz. */",
        f"#define ASRC_DECIMATOR_48_TO_24_COEFF_TAPS ({HALFRATE_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_24_GENERATED_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_24_OUT22K_GENERATED_CRC32 (0x{crc_22k:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_24_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
        "/* Variant FOR_24000: passband "
        f"{HALFRATE_PASSBAND_HZ:.0f} Hz, stopband {HALFRATE_STOPBAND_HZ:.1f} Hz */",
    ]
    lines.extend(format_array("s_48_to_24_coeff", coeff))
    lines.append("")
    lines.append("/* Variant FOR_22050: passband "
                 f"{HALFRATE22K_PASSBAND_HZ:.0f} Hz, stopband "
                 f"{HALFRATE22K_STOPBAND_HZ:.1f} Hz */")
    lines.extend(format_array("s_48_to_24_out22k_coeff", coeff_22k))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_48_TO_24_COEFFS_INC */", ""])
    return "\n".join(lines)


def render_quarter(stage1: np.ndarray, stage2: np.ndarray, crc: int,
                   stage1_12k: np.ndarray, stage2_12k: np.ndarray, crc_12k: int) -> str:
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_48_TO_12_COEFFS_INC",
        "#define ASRC_DECIMATOR_48_TO_12_COEFFS_INC",
        "",
        "/* Two coefficient sets over ONE structure.  The tap counts below are shared by both",
        " * variants on purpose: that is what lets the 12 kHz path reuse the 11.025 kHz path's",
        " * union member, history layout and tap-loop constants instead of duplicating the ISR",
        " * code.  Band edges differ -- see the QUARTER_ and QUARTER12K_ blocks in the",
        " * generator. */",
        f"#define ASRC_DECIMATOR_48_TO_12_STAGE1_COEFF_TAPS ({QUARTER_STAGE1_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_12_STAGE2_COEFF_TAPS ({QUARTER_STAGE2_TAPS}u)",
        f"#define ASRC_DECIMATOR_48_TO_12_GENERATED_CRC32 (0x{crc:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_12_OUT12K_GENERATED_CRC32 (0x{crc_12k:08X}UL)",
        f"#define ASRC_DECIMATOR_48_TO_12_KAISER_BETA ({KAISER_BETA:.1f}f)",
        "",
        "/* Variant FOR_11025: passband "
        f"{QUARTER_PASSBAND_HZ:.0f} Hz, stopband {QUARTER_STAGE2_STOPBAND_HZ:.1f} Hz */",
    ]
    lines.extend(format_array("s_48_to_12_stage1_coeff", stage1))
    lines.append("")
    lines.extend(format_array("s_48_to_12_stage2_coeff", stage2))
    lines.append("")
    lines.append("/* Variant FOR_12000: passband "
                 f"{QUARTER12K_PASSBAND_HZ:.0f} Hz, stopband "
                 f"{QUARTER12K_STAGE2_STOPBAND_HZ:.1f} Hz */")
    lines.extend(format_array("s_48_to_12_out12k_stage1_coeff", stage1_12k))
    lines.append("")
    lines.extend(format_array("s_48_to_12_out12k_stage2_coeff", stage2_12k))
    lines.extend(["", "#endif /* ASRC_DECIMATOR_48_TO_12_COEFFS_INC */", ""])
    return "\n".join(lines)


def render_tone() -> str:
    amplitude = (2**23 - 1) * 10.0 ** (TONE_DBFS / 20.0)
    phase = 2.0 * np.pi * np.arange(TONE_TABLE_LENGTH) / TONE_TABLE_LENGTH
    values = np.rint(amplitude * np.sin(phase)).astype(np.int32)
    lines = [
        "/* Generated by tools/asrc_decimator_48_to_8_design.py.  Do not edit. */",
        "#ifndef ASRC_DECIMATOR_48_TO_8_MEAS_TONE_INC",
        "#define ASRC_DECIMATOR_48_TO_8_MEAS_TONE_INC",
        "",
        f"#define ASRC_DECIMATOR_MEAS_TONE_TABLE_LENGTH ({TONE_TABLE_LENGTH}u)",
        f"#define ASRC_DECIMATOR_MEAS_TONE_DBFS ({TONE_DBFS:.1f}f)",
        f"static const int32_t s_decimator_meas_tone[{TONE_TABLE_LENGTH}u] =",
        "{",
    ]
    for base in range(0, values.size, 8):
        chunk = values[base : base + 8]
        text = ", ".join(str(int(item)) for item in chunk)
        suffix = "," if base + 8 < values.size else ""
        lines.append(f"    {text}{suffix}")
    lines.extend(["};", "", "#endif /* ASRC_DECIMATOR_48_TO_8_MEAS_TONE_INC */", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="write the generated C include")
    mode.add_argument("--check", action="store_true", help="fail if the generated include is stale")
    args = parser.parse_args()

    stage1 = design(STAGE1_TAPS, STAGE1_INPUT_HZ, PASSBAND_HZ, STAGE1_STOPBAND_HZ)
    stage2 = design(STAGE2_TAPS, STAGE2_INPUT_HZ, PASSBAND_HZ, STAGE2_STOPBAND_HZ)
    midrate = design(
        MIDRATE_TAPS,
        STAGE1_INPUT_HZ,
        MIDRATE_PASSBAND_HZ,
        MIDRATE_STOPBAND_HZ,
    )
    halfrate = design(
        HALFRATE_TAPS,
        STAGE1_INPUT_HZ,
        HALFRATE_PASSBAND_HZ,
        HALFRATE_STOPBAND_HZ,
    )
    halfrate22k = design(
        HALFRATE_TAPS,
        STAGE1_INPUT_HZ,
        HALFRATE22K_PASSBAND_HZ,
        HALFRATE22K_STOPBAND_HZ,
    )
    quarter1 = design(
        QUARTER_STAGE1_TAPS,
        QUARTER_STAGE1_INPUT_HZ,
        QUARTER_PASSBAND_HZ,
        QUARTER_STAGE1_STOPBAND_HZ,
    )
    quarter2 = design(
        QUARTER_STAGE2_TAPS,
        QUARTER_STAGE2_INPUT_HZ,
        QUARTER_PASSBAND_HZ,
        QUARTER_STAGE2_STOPBAND_HZ,
    )
    quarter12k_1 = design(
        QUARTER_STAGE1_TAPS,
        QUARTER_STAGE1_INPUT_HZ,
        QUARTER12K_PASSBAND_HZ,
        QUARTER12K_STAGE1_STOPBAND_HZ,
    )
    quarter12k_2 = design(
        QUARTER_STAGE2_TAPS,
        QUARTER_STAGE2_INPUT_HZ,
        QUARTER12K_PASSBAND_HZ,
        QUARTER12K_STAGE2_STOPBAND_HZ,
    )
    prestage = design(
        PRESTAGE_TAPS,
        PRESTAGE_INPUT_HZ,
        PRESTAGE_PASSBAND_HZ,
        PRESTAGE_STOPBAND_HZ,
    )
    poly32_phase0, poly32_phase1, poly32_proto = design_32k_polyphase()
    crc = coefficient_crc32(stage1, stage2)
    midrate_crc = coefficient_crc32(midrate)
    halfrate_crc = coefficient_crc32(halfrate)
    halfrate22k_crc = coefficient_crc32(halfrate22k)
    quarter_crc = coefficient_crc32(quarter1, quarter2)
    quarter12k_crc = coefficient_crc32(quarter12k_1, quarter12k_2)
    prestage_crc = coefficient_crc32(prestage)
    poly32_crc = coefficient_crc32(poly32_phase0, poly32_phase1)
    expected_poly32 = render_poly32(poly32_phase0, poly32_phase1, poly32_crc)
    expected = render(stage1, stage2, crc)
    expected_midrate = render_midrate(midrate, midrate_crc)
    expected_halfrate = render_halfrate(halfrate, halfrate_crc,
                                        halfrate22k, halfrate22k_crc)
    expected_quarter = render_quarter(quarter1, quarter2, quarter_crc,
                                      quarter12k_1, quarter12k_2, quarter12k_crc)
    expected_prestage = render_prestage(prestage, prestage_crc)
    expected_tone = render_tone()
    r1, s1 = response(stage1, STAGE1_INPUT_HZ, PASSBAND_HZ, STAGE1_STOPBAND_HZ)
    r2, s2 = response(stage2, STAGE2_INPUT_HZ, PASSBAND_HZ, STAGE2_STOPBAND_HZ)
    rm, sm = response(
        midrate,
        STAGE1_INPUT_HZ,
        MIDRATE_PASSBAND_HZ,
        MIDRATE_STOPBAND_HZ,
    )

    print(
        f"stage1: taps={STAGE1_TAPS} fs=48000 pass=3200 stop=12800 "
        f"ripple={r1:.6f} dB stop={s1:.2f} dB"
    )
    print(
        f"stage2: taps={STAGE2_TAPS} fs=16000 pass=3200 stop=4000 "
        f"ripple={r2:.6f} dB stop={s2:.2f} dB"
    )
    print(
        f"midrate: taps={MIDRATE_TAPS} fs=48000 pass={MIDRATE_PASSBAND_HZ:.0f} stop={MIDRATE_STOPBAND_HZ:.0f} "
        f"ripple={rm:.6f} dB stop={sm:.2f} dB"
    )
    rh, sh = response(
        halfrate,
        STAGE1_INPUT_HZ,
        HALFRATE_PASSBAND_HZ,
        HALFRATE_STOPBAND_HZ,
    )
    print(
        f"halfrate: taps={HALFRATE_TAPS} fs=48000 pass={HALFRATE_PASSBAND_HZ:.0f} "
        f"stop={HALFRATE_STOPBAND_HZ:.0f} ripple={rh:.6f} dB stop={sh:.2f} dB"
    )
    print(f"coeff_crc32=0x{crc:08X} dc1={float(np.sum(stage1)):.9f} dc2={float(np.sum(stage2)):.9f}")
    print(f"midrate_crc32=0x{midrate_crc:08X} dc={float(np.sum(midrate)):.9f}")
    print(f"halfrate_crc32=0x{halfrate_crc:08X} dc={float(np.sum(halfrate)):.9f}")
    rh22, sh22 = response(
        halfrate22k,
        STAGE1_INPUT_HZ,
        HALFRATE22K_PASSBAND_HZ,
        HALFRATE22K_STOPBAND_HZ,
    )
    print(
        f"halfrate22k: taps={HALFRATE_TAPS} fs=48000 pass={HALFRATE22K_PASSBAND_HZ:.0f} "
        f"stop={HALFRATE22K_STOPBAND_HZ:.1f} ripple={rh22:.6f} dB stop={sh22:.2f} dB"
    )
    print(f"halfrate22k_crc32=0x{halfrate22k_crc:08X} dc={float(np.sum(halfrate22k)):.9f}")

    rq1, sq1 = response(
        quarter1,
        QUARTER_STAGE1_INPUT_HZ,
        QUARTER_PASSBAND_HZ,
        QUARTER_STAGE1_STOPBAND_HZ,
    )
    rq2, sq2 = response(
        quarter2,
        QUARTER_STAGE2_INPUT_HZ,
        QUARTER_PASSBAND_HZ,
        QUARTER_STAGE2_STOPBAND_HZ,
    )
    print(
        f"quarter1: taps={QUARTER_STAGE1_TAPS} fs={QUARTER_STAGE1_INPUT_HZ:.0f} "
        f"pass={QUARTER_PASSBAND_HZ:.0f} stop={QUARTER_STAGE1_STOPBAND_HZ:.1f} "
        f"ripple={rq1:.6f} dB stop={sq1:.2f} dB"
    )
    print(
        f"quarter2: taps={QUARTER_STAGE2_TAPS} fs={QUARTER_STAGE2_INPUT_HZ:.0f} "
        f"pass={QUARTER_PASSBAND_HZ:.0f} stop={QUARTER_STAGE2_STOPBAND_HZ:.1f} "
        f"ripple={rq2:.6f} dB stop={sq2:.2f} dB"
    )
    print(f"quarter_crc32=0x{quarter_crc:08X} dcq1={float(np.sum(quarter1)):.9f} "
          f"dcq2={float(np.sum(quarter2)):.9f}")

    rq1b, sq1b = response(
        quarter12k_1,
        QUARTER_STAGE1_INPUT_HZ,
        QUARTER12K_PASSBAND_HZ,
        QUARTER12K_STAGE1_STOPBAND_HZ,
    )
    rq2b, sq2b = response(
        quarter12k_2,
        QUARTER_STAGE2_INPUT_HZ,
        QUARTER12K_PASSBAND_HZ,
        QUARTER12K_STAGE2_STOPBAND_HZ,
    )
    print(
        f"quarter12k_1: taps={QUARTER_STAGE1_TAPS} fs={QUARTER_STAGE1_INPUT_HZ:.0f} "
        f"pass={QUARTER12K_PASSBAND_HZ:.0f} stop={QUARTER12K_STAGE1_STOPBAND_HZ:.1f} "
        f"ripple={rq1b:.6f} dB stop={sq1b:.2f} dB"
    )
    print(
        f"quarter12k_2: taps={QUARTER_STAGE2_TAPS} fs={QUARTER_STAGE2_INPUT_HZ:.0f} "
        f"pass={QUARTER12K_PASSBAND_HZ:.0f} stop={QUARTER12K_STAGE2_STOPBAND_HZ:.1f} "
        f"ripple={rq2b:.6f} dB stop={sq2b:.2f} dB"
    )
    print(f"quarter12k_crc32=0x{quarter12k_crc:08X} dcq1={float(np.sum(quarter12k_1)):.9f} "
          f"dcq2={float(np.sum(quarter12k_2)):.9f}")

    rp, sp = response(
        prestage,
        PRESTAGE_INPUT_HZ,
        PRESTAGE_PASSBAND_HZ,
        PRESTAGE_STOPBAND_HZ,
    )
    print(
        f"prestage: taps={PRESTAGE_TAPS} fs={PRESTAGE_INPUT_HZ:.0f} "
        f"pass={PRESTAGE_PASSBAND_HZ:.0f} stop={PRESTAGE_STOPBAND_HZ:.0f} "
        f"ripple={rp:.6f} dB stop={sp:.2f} dB"
    )
    # One shared set serves four final rates, so the binding number is the worst attenuation in
    # EACH rate's own fold band (48000 - its Nyquist), not just at the designed 40000 Hz edge.
    for final_hz in (8000.0, 11025.0, 12000.0, 16000.0):
        edge_hz = 48000.0 - (final_hz * 0.5)
        _, worst = response(prestage, PRESTAGE_INPUT_HZ, PRESTAGE_PASSBAND_HZ, edge_hz)
        print(
            f"  prestage @ final {final_hz:7.1f}: fold edge {edge_hz:8.1f} Hz "
            f"worst={worst:.2f} dB"
        )
    print(f"prestage_crc32=0x{prestage_crc:08X} dc={float(np.sum(prestage)):.9f}")

    # 48 -> 32 kHz Audio mode.  The stopband figure below is the PROTOTYPE's own attenuation
    # above the 16 kHz output Nyquist on the 96 kHz axis; it is NOT the chain's worst alias,
    # because zero-stuffing by L=2 also folds each input f in from 48000 - f.  The alias
    # accounting that accounts for that lives in tools/asrc/asrc_48_to_32_audio_gate.py.
    rp32, sp32 = response(poly32_proto, POLY32_PROTO_HZ,
                          POLY32_PASSBAND_HZ, POLY32_STOPBAND_HZ)
    print(
        f"poly32 proto: taps={POLY32_PROTO_TAPS} fs={POLY32_PROTO_HZ:.0f} "
        f"pass={POLY32_PASSBAND_HZ:.0f} stop={POLY32_STOPBAND_HZ:.0f} "
        f"edge_droop={rp32:.6f} dB at_nyq={sp32:.2f} dB "
        f"(NOT gates -- 15-16 kHz is this design's transition band; see the audio gate)"
    )
    print(
        f"poly32 rows: L={POLY32_L} M={POLY32_M} "
        f"phase0={POLY32_PHASE0_TAPS} phase1={POLY32_PHASE1_TAPS} "
        f"dc0={float(np.sum(poly32_phase0)):.9f} dc1={float(np.sum(poly32_phase1)):.9f}"
    )
    print(f"poly32_crc32=0x{poly32_crc:08X}")

    if args.write:
        # These generated files are declared eol=crlf in .gitattributes.
        # Match the checkout format so --write does not leave content-clean
        # files reported as modified solely because of their worktree EOLs.
        OUTPUT.write_text(expected, encoding="ascii", newline="\r\n")
        MIDRATE_OUTPUT.write_text(expected_midrate, encoding="ascii", newline="\r\n")
        HALFRATE_OUTPUT.write_text(expected_halfrate, encoding="ascii", newline="\r\n")
        QUARTER_OUTPUT.write_text(expected_quarter, encoding="ascii", newline="\r\n")
        PRESTAGE_OUTPUT.write_text(expected_prestage, encoding="ascii", newline="\r\n")
        POLY32_OUTPUT.write_text(expected_poly32, encoding="ascii", newline="\r\n")
        TONE_OUTPUT.write_text(expected_tone, encoding="ascii", newline="\r\n")
        print(f"wrote {OUTPUT.relative_to(ROOT)}")
        print(f"wrote {MIDRATE_OUTPUT.relative_to(ROOT)}")
        print(f"wrote {HALFRATE_OUTPUT.relative_to(ROOT)}")
        print(f"wrote {QUARTER_OUTPUT.relative_to(ROOT)}")
        print(f"wrote {PRESTAGE_OUTPUT.relative_to(ROOT)}")
        print(f"wrote {POLY32_OUTPUT.relative_to(ROOT)}")
        print(f"wrote {TONE_OUTPUT.relative_to(ROOT)}")
    elif args.check:
        actual = OUTPUT.read_text(encoding="ascii") if OUTPUT.exists() else ""
        actual_midrate = MIDRATE_OUTPUT.read_text(encoding="ascii") if MIDRATE_OUTPUT.exists() else ""
        actual_halfrate = HALFRATE_OUTPUT.read_text(encoding="ascii") if HALFRATE_OUTPUT.exists() else ""
        actual_quarter = QUARTER_OUTPUT.read_text(encoding="ascii") if QUARTER_OUTPUT.exists() else ""
        actual_prestage = PRESTAGE_OUTPUT.read_text(encoding="ascii") if PRESTAGE_OUTPUT.exists() else ""
        actual_poly32 = POLY32_OUTPUT.read_text(encoding="ascii") if POLY32_OUTPUT.exists() else ""
        actual_tone = TONE_OUTPUT.read_text(encoding="ascii") if TONE_OUTPUT.exists() else ""
        if (actual != expected or actual_midrate != expected_midrate or
                actual_halfrate != expected_halfrate or
                actual_quarter != expected_quarter or
                actual_prestage != expected_prestage or
                actual_poly32 != expected_poly32 or actual_tone != expected_tone):
            print(f"ERROR: stale generated file: {OUTPUT.relative_to(ROOT)}", file=sys.stderr)
            return 1
        print("generated coefficient and measurement-tone includes: current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
