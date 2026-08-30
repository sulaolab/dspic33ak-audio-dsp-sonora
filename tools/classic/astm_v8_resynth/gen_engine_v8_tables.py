#!/usr/bin/env python3
"""Generate the V8 engine-model ROM tables for the target.

    tools/classic/astm_v8_resynth/out/tables_v3_low_formant_strong.npz
      -> src/app/apps/classic/dsp/engine_v8_tables.h

The sound is FIXED (analysis doc section 44: the design was frozen in section 38,
then four board rounds -- 40..43 -- corrected the level and two artefacts, and
section 44.1 tabulates every value that is now locked). This tool is the wav -> py
-> table link for it, the same chain `gen_dsp_tick_tables.py` restored for the
tick samples: the header is generated, never hand-edited, and the npz stays the
authoritative source.

What ends up in the header
--------------------------
  wave      one engine cycle per RPM bin, band-limited and decimated to the
            per-bin point count, int16 on a power-of-two scale
  noise     the shared pre-shaped residual-noise table (section 20's structure),
            4 engine cycles x 512 points
  noise_env the cycle-angular envelope of the residual noise -- **one shared
            curve**, not one per bin. Measured: per-bin curves stray 1.76 dB
            (mean |dev|) from the shared one and 1.83 dB from flat, so the
            per-bin set buys 0.07 dB for 6.4 kB. Flat is not used either: the
            shared curve keeps the cycle-locked noise modulation, which is part
            of what the owner hears as the cam clatter.
  win       the overlap-add window, as the rising half only -- see below
  rpm/gain/noise_rms   per-bin metadata, float32
  noise_norm  the constant that replaces the prototype's per-block std()

Point count per bin is a **schedule**, not one number
-----------------------------------------------------
Section 21 measured 256 points per cycle as free (0.19 dB) -- but it measured it
on an *acceleration* clip (2477-6301 rpm). A table of P points per cycle is
band-limited to crank order P/4, and that is a fixed *order*, so the Hz it
reaches scales with RPM:

    P = 256  ->  order  64  ->  7.3 kHz at 6875 rpm, but only 960 Hz at 900 rpm

The cam clatter the owner judges lives at 1.5-8 kHz, so at the settled 900 rpm
idle a 256-point table would throw all of it away, and section 21's number does
not cover that case. So the point count is per bin here, and `a34_impl_check.py`
measures section 32's imp/fold at the idle for each schedule below. Do not change
the default without re-running it.

Usage
-----
    python gen_engine_v8_tables.py                # write the header (default schedule)
    python gen_engine_v8_tables.py --schedule s1  # a different schedule
    python gen_engine_v8_tables.py --dry-run      # report the cost, write nothing
"""
import argparse
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import a20_portable_synth as A20  # noqa: E402
from a09_resynth import EngineTables  # noqa: E402

OUT = os.path.join(HERE, "out")
NPZ = os.path.join(OUT, "tables_v3_low_formant_strong.npz")
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
HDR = os.path.join(REPO, "src", "app", "apps", "classic", "dsp",
                   "engine_v8_tables.h")

# Section 45: raised 4 -> 16. Section 21 priced 4 x 512 against 8 x 2048 as "0.32 dB"
# and took the cheap one, but that metric was SPECTRAL and this table's audible
# defect is TEMPORAL: only `cycles` engine cycles of unique material exist, so at
# 4400 rpm the reader recycles the same 2048 samples every ~109 ms. From ~3700 rpm up
# the 3-6 kHz band is 98 % this noise (the wavetables are band-limited to crank
# order 64 and the recording genuinely holds nothing above it, measured -31..-56 dB),
# so that recurrence is uncovered -- the owner's "churu-churu" at 3400-5100 rpm.
# 16 cycles takes the recurrence period to ~437 ms at 4400 rpm. `noise_norm` comes
# out unchanged at 1.150479, i.e. this is de-correlation at identical level: no bin's
# spectrum, the per-cycle envelope, `noise_rms[]` and the idle are all untouched.
# Costs 12,288 B of ROM (25.1 -> 37.1 kB of tables), all of it on the AK512 --
# ENA_ENGINE_SYNTH is AK512-only, so the AK128 ROM budget does not see this.
NOISE_CYCLES = 16
NOISE_PTS = 512
WIN_PTS = 256                    # the overlap-add window, rising half
ENV_PTS = 128
# Section 20 fitted a 12 dB/oct rolloff above crank order 16 on the shared noise
# shape -- but it fitted it against the **recording**, on the 2477-6301 rpm
# acceleration clip, because both prototypes sat above the car at 2.5 kHz. Two
# reasons it is off here, both measured in a34: the frozen render the owner
# approved is the FFT prototype, which has no rolloff at all; and in the order
# domain the rolloff is 36 dB down at order 128, which at the 900 rpm idle is
# 1.9 kHz -- straight through the 1.5-8 kHz clatter band that section 34 showed
# is the thing being praised. Measured cost of leaving it in: 7.5 dB in the
# 1250 Hz band at the idle. Kept as a parameter so the A/B can be re-rendered.
ROLLOFF, CORNER = 0.0, 16.0
NOISE_SEED = 4242                # a20's own seed; pinned so this is reproducible

WAVE_SHIFT = 13                  # wave int16 scale = 1/8192 (waves are unit-std,
NOISE_SHIFT = 12                 # |max| 3.75 over all bins)
ENV_SHIFT = 14
WIN_SHIFT = 15

# Point count per bin. "flat256" is what section 21 decided on the acceleration
# clip; the others spend ROM where the clatter band needs order, i.e. at the
# bottom of the range. a34_impl_check.py picks between them by measurement.
SCHEDULES = {
    "flat256": lambda rpm: 256,
    "flat512": lambda rpm: 512,
    "low1024": lambda rpm: 1024 if rpm < 2125.0 else 256,
    "low2048": lambda rpm: 2048 if rpm < 2125.0 else 512,
}
DEFAULT_SCHEDULE = "low1024"

# Bin-to-bin phase alignment (section 42). The bins are separate measured cycles,
# so each one carries its own harmonic phases; the model crossfades two of them by
# rpm, and where their phases disagree the sum partially cancels. Measured on the
# 50/50 mix: 0.00 dB for every pair below 2125 rpm (those bins happen to be
# coherent) but -1.9 to -4.4 dB from 2125 rpm up, worst at 4250 rpm. The depth of
# that cancellation moves with the crossfade weight, so the idle drift and the POT
# wander make the timbre ripple -- heard on the board as a "churu-churu" warble,
# strongest at ~4300 rpm, exactly the 4125/4375 pair. Replacing every bin's phase
# spectrum with one measured bin's leaves each bin's MAGNITUDE spectrum untouched
# (so no bin changes timbre in isolation) and takes every pair to 0.00 dB. The
# reference is the idle bin, because with it the crest factor of bins 0-4 moves by
# 0.0 dB and of the rest by -0.2 dB, i.e. the impulsiveness the clatter is made of
# survives. Zero-phase was rejected: crest 9.9 -> 24.3 dB is a click train.
PHASE_REF_BIN = 0                # None disables the alignment

# Low-rpm clatter trim (section 42). `formant_strong` moves 7 dB out of orders
# 1-20 and puts 11-18 dB into order 80+ at the same total rms; that high-order
# content IS the clatter, and it was adopted on the sweep. On the board at ~1300
# rpm it is a few dB too prominent. This attenuates the low-rpm bins above
# CLATTER_TRIM_HZ, fading out linearly in dB so nothing changes above _RPM_HI.
CLATTER_TRIM_DB = 2.5            # 0 disables the trim
CLATTER_TRIM_HZ = 1200.0
CLATTER_TRIM_RPM_LO = 1900.0     # full trim at or below this
CLATTER_TRIM_RPM_HI = 2600.0     # no trim at or above this

# Section 43. Bins 10 and 11 (3375/3625 rpm) have the peakiest cycles in the set
# -- crest 10.04/10.32 dB against 7.3..9.3 for every neighbour -- and until the
# phase alignment above, the incoherent crossfade was flattening them by accident.
# With the bins coherent that peakiness is heard as it was recorded, which the
# owner noticed at ~3700 rpm and was unsure whether to keep. It is genuine
# recorded content, so the default is to keep it: this knob is measured, ready
# and OFF. 0.50 puts bin 11's crest at 8.71 dB, in line with bins 9 and 12, and
# takes the 1-3 kHz AM at 3700 rpm from -12.0 to -14.5 dB without touching the
# spectrum (tonality 14.4 dB at 6252 Hz either way) and without re-introducing
# any crossfade cancellation. Undoing the alignment for these two bins instead
# was measured and rejected: it recovers only -14.4 dB and puts a -3.0..-3.2 dB
# cancellation back into the 3375..3875 pairs, i.e. it trades a real pulse for a
# synthetic beat in the same rpm band the complaint is about.
CREST_SOFTEN_BINS = (10, 11)
CREST_SOFTEN_AMT = 0.50          # adopted by the owner after the c00/c50 A/B (section 43)


def phase_align(wave, ref_bin=PHASE_REF_BIN):
    """Give every bin the reference bin's harmonic phases, magnitudes untouched."""
    if ref_bin is None:
        return wave
    ref = np.angle(np.fft.rfft(np.asarray(wave[ref_bin], float)))
    out = []
    for w in wave:
        w = np.asarray(w, float)
        S = np.fft.rfft(w)
        if len(S) > len(ref):
            raise SystemExit("phase reference bin %d is shorter than bin of %d pts"
                             % (ref_bin, len(w)))
        out.append(np.fft.irfft(np.abs(S) * np.exp(1j * ref[:len(S)]), len(w)))
    return out


def crest_soften(wave, bins=CREST_SOFTEN_BINS, amt=CREST_SOFTEN_AMT):
    """Flatten a bin's own cycle envelope by `amt`, rms preserved (section 43)."""
    if amt <= 0.0:
        return wave
    from scipy import signal as _sig
    out = list(wave)
    for j in bins:
        w = np.asarray(out[j], float)
        n = len(w)
        e = np.abs(_sig.hilbert(np.tile(w, 3)))[n:2 * n]
        S = np.fft.rfft(e)
        S[6:] = 0.0                      # the slow envelope only
        e = np.fft.irfft(S, n)
        e = np.maximum(e, 0.15 * e.max())
        v = w / (e / e.mean()) ** amt
        out[j] = v * (w.std() / v.std())
    return out

def clatter_trim(wave, rpm, db=CLATTER_TRIM_DB, hz=CLATTER_TRIM_HZ,
                 rpm_lo=CLATTER_TRIM_RPM_LO, rpm_hi=CLATTER_TRIM_RPM_HI):
    """Attenuate the low-rpm bins above `hz`, by `db` at rpm_lo, 0 at rpm_hi."""
    if db <= 0.0:
        return wave
    out = []
    for w, r in zip(wave, rpm):
        w = np.asarray(w, float)
        f = 1.0 if r <= rpm_lo else max(0.0, (rpm_hi - r) / (rpm_hi - rpm_lo))
        if f <= 0.0:
            out.append(w)
            continue
        S = np.fft.rfft(w)
        order = np.arange(len(S)) * 1.0
        f_hz = order * (r / 120.0)
        g = np.where(f_hz >= hz, 10.0 ** (-f * db / 20.0), 1.0)
        out.append(np.fft.irfft(S * g, len(w)))
    return out


def band_limit_decimate(x, pts, cycles=1):
    """Store `x` (one or `cycles` engine cycles on the measured M-point grid) at
    `cycles * pts` points, exactly.

    Zeroing every order at or above the decimated Nyquist first makes the
    decimation lossless rather than aliased. a21's `band_limit` keeps the Nyquist
    bin itself and evaluates back on the original grid; that bin cannot be
    represented unambiguously once decimated, so it goes too -- one bin out of
    1025, and a34 checks the difference rather than asserting it.
    """
    L = len(x)
    dec = L // (cycles * pts)
    if dec * cycles * pts != L:
        raise SystemExit("point count %d does not divide the %d-point grid"
                         % (pts, L))
    X = np.fft.rfft(x)
    X[(pts // 2) * cycles:] = 0.0
    return np.fft.irfft(X, L)[::dec]


def shared_noise(tab, cycles=NOISE_CYCLES):
    """The full-rate shared residual-noise table, before decimation.

    Split out so a34 can hand the *identical* float table to the portable
    prototype it scores the firmware against -- otherwise the two would differ by
    a noise realisation as well as by everything under test.
    """
    return A20.build_noise_table(tab, cycles=cycles, rolloff_db_per_oct=ROLLOFF,
                                 corner_order=CORNER, quiet=True)


def hann_rising(n):
    """The rising quarter-sine (root-Hann) overlap-add weight, on the cycle grid.

    Section 45.5. This used to be the rising half of a periodic Hann, whose pair
    `(w, 1 - w)` sums to exactly 1 -- AMPLITUDE-complementary, which is the right
    criterion when the two things being crossfaded are the same signal. They are
    not: the two noise readers take INDEPENDENT random offsets into the table, so
    they are uncorrelated, and uncorrelated signals add in POWER. For the Hann pair
    `w**2 + (1-w)**2` runs from 1.0 at the cycle boundary down to 0.5 mid-cycle,
    i.e. the noise dropped 3.01 dB once per engine cycle, by construction. Measured
    on the output at a held 4400 rpm, noise only, 3-6 kHz: 4.88 dB peak-to-trough
    over the cycle, a 36.7 Hz amplitude modulation of a noise band -- and from
    ~3700 rpm up that band is 98 % noise (section 45.2), so it is fully exposed.
    That is the owner's churu-churu.

    `sin` and `cos` of the same quarter angle are power-complementary
    (`sin**2 + cos**2 == 1`) at every phase, so the noise power is now flat across
    the cycle. The falling weight is this same table read backwards -- see
    `local_read_win_pair()` in engine_v8.c -- so it costs no ROM, only a second
    interpolation per sample.
    """
    return np.sin(0.5 * np.pi * np.arange(n) / float(n))


def build(npz=NPZ, schedule=DEFAULT_SCHEDULE, noise_pts=NOISE_PTS,
          noise_cycles=NOISE_CYCLES, phase_ref=PHASE_REF_BIN,
          trim_db=CLATTER_TRIM_DB, soften_amt=CREST_SOFTEN_AMT):
    """Everything the header holds, as floats plus their int16 forms.

    a34_impl_check.py imports this so the validation render and the C build come
    from the same numbers -- if they came from two code paths, the render would
    stop being evidence about the firmware.

    `noise_pts` and `noise_cycles` are two independent axes and a34 sweeps both:
    the noise table is read on the *cycle-angular* grid, so `noise_pts` sets its
    bandwidth as a crank order (order `noise_pts/4`) and therefore in Hz only
    once the rpm is known, while `noise_cycles` sets how long it takes to repeat.
    """
    tab = EngineTables(npz)
    pts_of = SCHEDULES[schedule]
    pts = np.array([pts_of(r) for r in tab.rpm], dtype=int)

    wave = [band_limit_decimate(w, p) for w, p in zip(tab.wave, pts)]
    wave = phase_align(wave, phase_ref)
    wave = clatter_trim(wave, tab.rpm, trim_db)
    wave = crest_soften(wave, CREST_SOFTEN_BINS, soften_amt)
    off = np.concatenate([[0], np.cumsum(pts)[:-1]]).astype(int)

    A20.RNG = np.random.default_rng(NOISE_SEED)
    full = shared_noise(tab, noise_cycles)
    noise = band_limit_decimate(full, noise_pts, cycles=noise_cycles)
    noise /= noise.std()

    nenv = tab.nenv / (tab.nenv.mean(axis=1, keepdims=True) + 1e-15)
    env = nenv.mean(axis=0)
    env = env / env.mean()
    if len(env) != ENV_PTS:
        env = np.interp(np.arange(ENV_PTS) / ENV_PTS * len(env),
                        np.arange(len(env)), env, period=len(env))
    win = hann_rising(WIN_PTS)

    # The prototype scales every 2-cycle noise block by 1/blk.std() so its std
    # lands on g*nr*sqrt(0.5). That std cannot be known a sample at a time, so it
    # becomes this constant: the block is the unit-std table times the envelope
    # times the weights, and the only part that is not 1 in rms is that product.
    #
    # Derived from `win` ITSELF rather than from a second, hand-written copy of the
    # window shape. It used to re-spell the rising Hann inline as
    # `0.5 - 0.5*cos(pi*k/noise_pts)` over two cycles, so section 45.5's change of
    # `hann_rising` would have silently left the level normalisation describing the
    # old window -- and the fix would have arrived as a level change as well, moving
    # the noise ratio section 31 set by ear. Two uncorrelated readers, so the
    # variance at a given phase is env^2 * (w_new^2 + w_old^2); with the
    # power-complementary pair that bracket is 1 at every phase, which is the point.
    #
    # `1/sqrt(mean)` is the same number the old expression produced, not a new
    # convention: it spelled this as sqrt(0.5)/sqrt(mean over TWO cycles), and the
    # two-cycle mean is half the one-cycle bracket, so the sqrt(0.5) and the half
    # cancelled. Writing it this way makes the target -- total mean noise power of
    # 1.0, both readers included -- readable instead of implied.
    ph1 = np.arange(noise_pts) / float(noise_pts)
    e1 = np.interp(ph1 * ENV_PTS, np.arange(ENV_PTS), env, period=ENV_PTS)
    wxp = np.arange(WIN_PTS + 1)
    wfp = np.concatenate([win, [1.0]])          # win[WIN_PTS] is 1.0, as in the C
    w_new = np.interp(ph1 * WIN_PTS, wxp, wfp)
    w_old = np.interp((1.0 - ph1) * WIN_PTS, wxp, wfp)
    noise_norm = float(1.0
                       / np.sqrt((e1 ** 2 * (w_new ** 2 + w_old ** 2)).mean()))

    def q(x, shift):
        v = np.round(np.asarray(x) * (1 << shift))
        if np.abs(v).max() > 32767:
            raise SystemExit("scale 1/%d overflows int16 (max %.0f)"
                             % (1 << shift, np.abs(v).max()))
        return v.astype(np.int16)

    return dict(
        schedule=schedule, rpm=tab.rpm.astype(np.float32),
        gain=tab.gain.astype(np.float32), nrms=tab.nrms.astype(np.float32),
        pts=pts, off=off, fs=tab.fs, M=tab.M,
        noise_pts=noise_pts, noise_cycles=noise_cycles,
        env_pts=ENV_PTS, win_pts=WIN_PTS,
        wave=wave, noise=noise, env=env, win=win, noise_norm=noise_norm,
        # per-bin noise envelopes, mean-normalised. Not emitted -- the header
        # holds the shared curve (see the module docstring). Carried here only so
        # a34 can measure what the sharing costs instead of assuming it.
        nenv=nenv,
        q_wave=q(np.concatenate(wave), WAVE_SHIFT),
        q_noise=q(noise, NOISE_SHIFT),
        q_env=q(env, ENV_SHIFT),
        q_win=q(win, WIN_SHIFT),
    )


def snr_db(ref, quant, shift):
    err = np.asarray(quant, dtype=float) / (1 << shift) - np.asarray(ref)
    return 20.0 * np.log10(np.std(ref) / (np.std(err) + 1e-30))


def fmt_i16(symbol, count_expr, data, per_line=12):
    lines = ["static const int16_t %s[%s] = {" % (symbol, count_expr)]
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("".join(["%8d," % chunk[0]]
                             + ["%7d," % v for v in chunk[1:]]))
    lines.append("};")
    return lines


def fmt_u16(symbol, count_expr, data, per_line=12):
    lines = ["static const uint16_t %s[%s] = {" % (symbol, count_expr)]
    for i in range(0, len(data), per_line):
        lines.append("".join("%7d," % v for v in data[i:i + per_line]))
    lines.append("};")
    return lines


def c_float(v):
    """A C float literal. `%g` on a whole number gives "900", and "900f" is not a
    float constant -- XC-DSC rejects it as an integer with a bad suffix. So the
    decimal point goes in when %g left it out."""
    s = "%.9g" % v
    if ("." not in s) and ("e" not in s) and ("E" not in s) and ("n" not in s):
        s += ".0"
    return s + "f"


def fmt_f32(symbol, count_expr, data, per_line=6):
    lines = ["static const float %s[%s] = {" % (symbol, count_expr)]
    for i in range(0, len(data), per_line):
        lines.append("".join("%16s," % c_float(v) for v in data[i:i + per_line]))
    lines.append("};")
    return lines


def emit(t):
    b = t
    n_wave = len(b["q_wave"])
    rom = (n_wave + len(b["q_noise"]) + len(b["q_env"]) + len(b["q_win"])) * 2 \
        + len(b["rpm"]) * 4 * 3 + len(b["pts"]) * 2 * 2
    o = []
    o.append("#ifndef ENGINE_V8_TABLES_H")
    o.append("#define ENGINE_V8_TABLES_H")
    o.append("")
    o.append("#include <stdint.h>")
    o.append("")
    o.append("// V8 engine model: measured one-cycle wavetables plus one shared")
    o.append("// residual-noise table. Structure and every constant here come")
    o.append("// from the analysis in [internal] astm_v8_resynthesis_analysis.md")
    o.append("// (the sound is FIXED in section 44; section 44.1 tabulates")
    o.append("// every locked value and the section that decided it).")
    o.append("//")
    o.append("// GENERATED FILE -- do not edit by hand.")
    o.append("// Regenerate with:")
    o.append("//   python tools/classic/astm_v8_resynth/"
             "gen_engine_v8_tables.py")
    o.append("// Source: tools/classic/astm_v8_resynth/out/"
             "tables_v3_low_formant_strong.npz")
    o.append("//")
    o.append("// point-count schedule: %s; noise %d cycles x %d points"
             " (max crank order %d)"
             % (b["schedule"], b["noise_cycles"], b["noise_pts"],
                b["noise_pts"] // 4))
    o.append("//   %-7s %-6s %-10s %-10s"
             % ("rpm", "points", "max order", "= Hz at that rpm"))
    for r, p in zip(b["rpm"], b["pts"]):
        o.append("//   %-7.0f %-6d %-10d %-10.0f"
                 % (r, p, p // 4, p // 4 * r / 60.0))
    o.append("//")
    o.append("// %-26s %8s %10s" % ("table", "samples", "bytes"))
    o.append("// %-26s %8d %10d" % ("wave (all bins)", n_wave, n_wave * 2))
    for nm, key in (("noise", "q_noise"), ("noise_env", "q_env"),
                    ("win", "q_win")):
        o.append("// %-26s %8d %10d" % (nm, len(b[key]), len(b[key]) * 2))
    o.append("// %-26s %8s %10d" % ("rpm/gain/noise_rms (f32)", "",
                                    len(b["rpm"]) * 4 * 3))
    o.append("// %-26s %8s %10d" % ("wave_pts/wave_off (u16)", "",
                                    len(b["pts"]) * 2 * 2))
    o.append("// %-26s %8s %10d" % ("total", "", rom))
    o.append("//")
    o.append("// int16 tables are stored on power-of-two scales, so the")
    o.append("// reconstruction (float)v * scale is exact. Quantization SNR")
    o.append("// against the float source: wave %.1f dB, noise %.1f dB,"
             % (snr_db(np.concatenate(b["wave"]), b["q_wave"], WAVE_SHIFT),
                snr_db(b["noise"], b["q_noise"], NOISE_SHIFT)))
    o.append("// noise_env %.1f dB, win %.1f dB."
             % (snr_db(b["env"], b["q_env"], ENV_SHIFT),
                snr_db(b["win"], b["q_win"], WIN_SHIFT)))
    o.append("")
    o.append("#define ENGINE_V8_BINS            %d" % len(b["rpm"]))
    o.append("#define ENGINE_V8_WAVE_SAMPLES    %d" % n_wave)
    o.append("#define ENGINE_V8_NOISE_LEN       %d" % len(b["q_noise"]))
    o.append("#define ENGINE_V8_NOISE_PTS       %d"
             " // noise points per engine cycle" % b["noise_pts"])
    o.append("#define ENGINE_V8_ENV_PTS         %d" % ENV_PTS)
    o.append("#define ENGINE_V8_WIN_PTS         %d" % WIN_PTS)
    o.append("#define ENGINE_V8_WAVE_SCALE      (1.0f / %d.0f)" % (1 << WAVE_SHIFT))
    o.append("#define ENGINE_V8_NOISE_SCALE     (1.0f / %d.0f)" % (1 << NOISE_SHIFT))
    o.append("#define ENGINE_V8_ENV_SCALE       (1.0f / %d.0f)" % (1 << ENV_SHIFT))
    o.append("#define ENGINE_V8_WIN_SCALE       (1.0f / %d.0f)" % (1 << WIN_SHIFT))
    o.append("")
    o.append("// replaces the prototype's per-block 1/std() normalisation")
    o.append("#define ENGINE_V8_NOISE_NORM      %s" % c_float(b["noise_norm"]))
    o.append("")
    o.extend(fmt_u16("g_engine_v8_wave_pts", "ENGINE_V8_BINS", b["pts"]))
    o.append("")
    o.extend(fmt_u16("g_engine_v8_wave_off", "ENGINE_V8_BINS", b["off"]))
    o.append("")
    o.extend(fmt_f32("g_engine_v8_rpm", "ENGINE_V8_BINS", b["rpm"]))
    o.append("")
    o.extend(fmt_f32("g_engine_v8_gain", "ENGINE_V8_BINS", b["gain"]))
    o.append("")
    o.extend(fmt_f32("g_engine_v8_noise_rms", "ENGINE_V8_BINS", b["nrms"]))
    o.append("")
    o.extend(fmt_i16("g_engine_v8_wave", "ENGINE_V8_WAVE_SAMPLES", b["q_wave"]))
    o.append("")
    o.extend(fmt_i16("g_engine_v8_noise", "ENGINE_V8_NOISE_LEN", b["q_noise"]))
    o.append("")
    o.extend(fmt_i16("g_engine_v8_noise_env", "ENGINE_V8_ENV_PTS", b["q_env"]))
    o.append("")
    o.append("// rising half of a periodic Hann window over one engine cycle;")
    o.append("// the older noise reader uses 1 - this, so the pair sums to 1")
    o.extend(fmt_i16("g_engine_v8_win", "ENGINE_V8_WIN_PTS", b["q_win"]))
    o.append("")
    o.append("#endif // ENGINE_V8_TABLES_H")
    return "\r\n".join(o) + "\r\n", rom


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schedule", default=DEFAULT_SCHEDULE,
                    choices=sorted(SCHEDULES))
    ap.add_argument("--npz", default=NPZ)
    ap.add_argument("--out", default=HDR)
    ap.add_argument("--noise-pts", type=int, default=NOISE_PTS)
    ap.add_argument("--noise-cycles", type=int, default=NOISE_CYCLES)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    b = build(a.npz, a.schedule, a.noise_pts, a.noise_cycles)
    text, rom = emit(b)
    print("schedule %s: %d bins, %d wave samples, noise %dx%d, ROM %d B (%.1f kB)"
          % (a.schedule, len(b["rpm"]), len(b["q_wave"]), b["noise_cycles"],
             b["noise_pts"], rom, rom / 1024.0))
    print("  points per bin: "
          + " ".join("%d:%d" % (r, p) for r, p in zip(b["rpm"], b["pts"])))
    print("  noise_norm %.6f" % b["noise_norm"])
    if a.dry_run:
        print("  (dry run, nothing written)")
        return
    with open(a.out, "wb") as f:
        f.write(text.encode("ascii"))
    print("  wrote %s" % a.out)


if __name__ == "__main__":
    main()
