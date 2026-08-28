#!/usr/bin/env python3
"""
gen_tone_data_int16.py -- generate src/app/apps/classic/tone_data_int16.c

Why this exists
---------------
The Classic sound-effect tones used to be embedded at the processing sample
rate (48 kHz) for every tone, which cost 101,968 bytes of program flash.  The
measured content of the tones is far narrower than 24 kHz:

    tone   peak     99.9% energy   last bin > -40 dBFS(peak)
    ON     1173 Hz   1.7 kHz        1.4 kHz
    OFF     784 Hz   1.2 kHz        1.3 kHz
    Notif  1568 Hz   7.9 kHz       10.4 kHz

So each tone is stored at its own source sample rate (see TONES below) and the
runtime SRC in snd_effect_play.c converts it to the processing rate, exactly as
it already did for 44.1/96 kHz output.

Usage
-----
    # one-time: recover the 48 kHz masters from a git revision of the C file
    python tools/classic/gen_tone_data_int16.py extract --rev <rev>

    # regenerate the C table from the masters in tools/classic/tone_src/
    python tools/classic/gen_tone_data_int16.py gen

The 48 kHz WAV masters under tools/classic/tone_src/ are the authoritative
source data.  Never re-derive a master from the generated C file: that would
bake the decimation in permanently.

Resampling
----------
Rational L/M resampling (any ratio to 48 kHz), with a linear-phase
Kaiser-windowed-sinc anti-alias FIR.  The FIR group delay is compensated so the
onset transient of a button click stays where it was.  numpy only -- scipy is not
installed on the build hosts.

Choosing a stored rate
----------------------
Bandwidth is NOT the binding constraint; the runtime linear interpolator is.  It
mirrors every partial to (stored_rate - f) with only sinc^2 attenuation, so the
rule is about where the images land, not where the content ends:

    stored 12 kHz -> images from  8 kHz up, 8-16 kHz floor -28 dBFS
    stored 16 kHz -> images from 12 kHz up, 8-16 kHz floor -35 dBFS

Both were built and auditioned on hardware (2026-08-13).  Neither was
distinguishable from the 48 kHz master by ear, so the rates below are the
aggressive pair; 16 / 32 kHz remains the conservative fallback and is one edit to
the `rate` column away.

Verify any change with tools/classic/check_tone_data_src.py before believing it.
"""

import argparse
import math
import os
import re
import struct
import subprocess
import sys
import wave

import numpy as np

MASTER_RATE_HZ = 48000

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC_DIR = os.path.join(HERE, "tone_src")
C_PATH = os.path.join(REPO, "src", "app", "apps", "classic", "tone_data_int16.c")

# rate      stored sample rate, Hz.  Any rational ratio to 48 kHz is allowed.
# aa        anti-alias cutoff as a fraction of the new Nyquist.  Lower it to trade
#           stored bandwidth for a lower runtime imaging floor.
# precomp   pre-compensate the runtime linear-interpolation droop (see below).
TONES = [
    # C symbol         master wav           struct stem   rate    aa    precomp
    ("Tone_ON_i16", "tone_on_48k.wav", "Tone_ON", 12000, 0.85, True),
    ("Tone_OFF_i16", "tone_off_48k.wav", "Tone_OFF", 12000, 0.85, True),
    ("Tone_Notif_i16", "tone_notif_48k.wav", "Tone_Notif", 24000, 0.90, True),
]

# The runtime interpolator is linear, i.e. a triangular reconstruction kernel of
# width 2/stored_rate, whose magnitude response is sinc^2(f / stored_rate).  That
# droop depends only on the *stored* rate, not on the output rate, so it can be
# pre-compensated once here, offline, for free at runtime.  Without it, Notif at
# 32 kHz would lose 0.7 dB at 7 kHz and 1.2 dB at 9 kHz.
#
# The compensation boosts the top of the stored band by up to +3.9 dB, which also
# raises the runtime imaging floor by the same amount, so it is measured per tone
# (tools/classic/check_tone_data_src.py) rather than assumed.  Measured at the
# rates chosen above it wins on both counts: ON error -45.6 -> -49.1 dBpk with the
# 8-16 kHz imaging floor unchanged at -35 dBFS.
PRECOMP_TAPS = 63

# Provenance of the 48 kHz masters, carried over from the previous hand-written
# table so the generated file keeps documenting where the audio came from.
PROVENANCE = [
    "ON source : button_on_01.wav",
    "OFF source: button_off_v7_ref_onset_sine_tail.wav",
    "Notif gain: -6.0 dB (0.50x) from original Tone_Notif_i16",
]

STOPBAND_ATTEN_DB = 100.0


# ---------------------------------------------------------------- parsing


def parse_c_arrays(text):
    """Return {symbol: np.int16 array} for every `const int16_t x[] = {...}`."""
    out = {}
    for m in re.finditer(r"const\s+int16_t\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S):
        vals = [v.strip() for v in m.group(2).replace("\n", " ").split(",")]
        vals = [int(v) for v in vals if v]
        out[m.group(1)] = np.asarray(vals, dtype=np.int16)
    return out


def write_wav(path, data_i16, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(data_i16), *data_i16.tolist()))


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise SystemExit("%s: expected mono 16-bit" % path)
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    return np.frombuffer(raw, dtype="<i2").astype(np.float64), rate


# ---------------------------------------------------------------- filtering


def kaiser_beta(atten_db):
    if atten_db > 50.0:
        return 0.1102 * (atten_db - 8.7)
    if atten_db >= 21.0:
        return 0.5842 * (atten_db - 21.0) ** 0.4 + 0.07886 * (atten_db - 21.0)
    return 0.0


def design_lowpass(fs, f_pass_edge, f_stop_edge, atten_db):
    """Odd-length linear-phase FIR, -6 dB near the middle of the transition."""
    trans_w = 2.0 * math.pi * (f_stop_edge - f_pass_edge) / fs
    n = int(math.ceil((atten_db - 7.95) / (2.285 * trans_w))) + 1
    if n % 2 == 0:
        n += 1
    n = max(n, 31)
    fc = 0.5 * (f_pass_edge + f_stop_edge) / fs  # cycles/sample
    k = np.arange(n) - (n - 1) / 2.0
    h = 2.0 * fc * np.sinc(2.0 * fc * k)
    h *= np.kaiser(n, kaiser_beta(atten_db))
    h /= h.sum()  # unity DC gain
    return h


def resample(x, rate_in, rate_out, aa_of_nyquist):
    """Rational L/M resampling, group-delay compensated.  Returns (y, taps)."""
    g = math.gcd(int(rate_in), int(rate_out))
    up = int(rate_out) // g
    down = int(rate_in) // g
    if up == 1 and down == 1:
        return x.copy(), 0

    fs_work = rate_in * up
    nyq = 0.5 * min(rate_in, rate_out)
    h = design_lowpass(
        fs_work,
        aa_of_nyquist * nyq,
        nyq,
        STOPBAND_ATTEN_DB,
    )
    h = h * up  # zero-stuffing loses `up` times the amplitude

    if up > 1:
        xu = np.zeros(len(x) * up, dtype=np.float64)
        xu[::up] = x
    else:
        xu = x

    delay = (len(h) - 1) // 2
    y = np.convolve(xu, h, mode="full")[delay : delay + len(xu)]
    y = y[::down]
    # keep the length exactly proportional to the master
    want = int(math.ceil(len(x) * rate_out / float(rate_in)))
    if len(y) > want:
        y = y[:want]
    return y, len(h)


def linear_interp_precomp(rate, taps):
    """
    FIR that inverts the runtime linear-interpolation droop.

    The runtime reconstructs the stored samples with a triangular kernel of width
    2/rate, so its magnitude response is sinc^2(f/rate).  Pre-multiplying the
    stored data by 1/sinc^2 makes the reconstructed signal flat, independent of
    the output sample rate.
    """
    n_fft = 4096
    nu = np.fft.rfftfreq(n_fft)  # cycles/sample, 0..0.5
    gain = 1.0 / np.sinc(nu) ** 2
    # zero-phase impulse response, then window down to `taps`
    full = np.fft.irfft(gain, n_fft)
    half = (taps - 1) // 2
    h = np.concatenate((full[-half:], full[: half + 1]))
    h *= np.hamming(taps)
    h /= h.sum()  # unity DC gain
    _ = rate  # response is rate-independent; the argument documents intent
    return h


def apply_precomp(y, rate, taps):
    h = linear_interp_precomp(rate, taps)
    delay = (len(h) - 1) // 2
    padded = np.concatenate((np.zeros(delay), y, np.zeros(delay)))
    return np.convolve(padded, h, mode="full")[2 * delay : 2 * delay + len(y)], len(h)


def to_int16(y, name):
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    gain = 1.0
    if peak > 32767.0:
        gain = 32767.0 / peak
        print(
            "    %-16s filter overshoot: peak %.1f -> applying %.3f dB of gain"
            % (name, peak, 20.0 * math.log10(gain))
        )
    q = np.rint(y * gain)
    return np.clip(q, -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------- emitting


def fmt_array(name, data):
    lines = ["const int16_t %s[] = {" % name]
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        cells = ["%10d," % chunk[0]] + ["%7d," % v for v in chunk[1:]]
        lines.append("".join(cells))
    lines.append("};")
    return "\r\n".join(lines)  # emit_c joins with CRLF too; keep the whole file consistent


def emit_c(tables):
    """tables: list of (symbol, rate_hz, np.int16 data, field_stem)"""
    total = sum(len(d) * 2 for _, _, d, _ in tables)
    out = []
    out.append('#include "app_specific_config_defs.h"   // APP_TARGET (must precede the device #if)')
    out.append("")
    out.append("#if !SONORA_APP_IS_CLASSIC")
    out.append(
        '#  error "tone_data_int16.c is Classic-app-owned; build it only in a Classic '
        'manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."'
    )
    out.append("#endif")
    out.append("")
    out.append("#if APP_TARGET == APP_TARGET_AK512")
    out.append("")
    out.append('#include "app_runtime_overrides.h"')
    out.append("#include <xc.h>")
    out.append("#include <stdlib.h>")
    out.append("#include <string.h>")
    out.append("#include <stdio.h>")
    out.append("#include <math.h>")
    out.append("")
    out.append("")
    out.append('#include "tone_data_int16.h"')
    out.append("")
    out.append("")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Definition")
    out.append("//===========================================================")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Enum & Struct typedef")
    out.append("//===========================================================")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Function Prototype")
    out.append("//===========================================================")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Variables")
    out.append("//===========================================================")
    out.append("")
    out.append("// GENERATED FILE -- do not edit by hand.")
    out.append("// Regenerate with: python tools/classic/gen_tone_data_int16.py gen")
    out.append("// Masters: 48 kHz mono int16 WAV in tools/classic/tone_src/")
    out.append("//")
    out.append("// int16_t mono arrays, one stored sample rate per tone.")
    out.append("// The runtime SRC in snd_effect_play.c converts each tone to the")
    out.append("// processing sample rate, so the stored rate only has to cover the")
    out.append("// bandwidth the tone actually contains.")
    out.append("//")
    for tone in PROVENANCE:
        out.append("// %s" % tone)
    out.append("//")
    for sym, rate, data, _ in tables:
        out.append(
            "// %-16s %6u Hz  %6u samples  %6u bytes  %.3f s"
            % (sym, rate, len(data), len(data) * 2, len(data) / float(rate))
        )
    out.append("// %-16s %26s %6u bytes" % ("total", "", total))
    out.append("")
    out.append("")
    out.append("// ON / OFF pair")
    for i, (sym, rate, data, _) in enumerate(tables):
        if i == 2:
            out.append("")
            out.append("")
            out.append("// Notification")
        out.append(fmt_array(sym, data))
        out.append("")
    out.append("")
    out.append("const Button_Tone_i16_t Button_Tone_i16 =")
    out.append("{")
    for sym, rate, _, stem in tables:
        out.append("    .%-18s = %s," % (stem, sym))
        out.append("    .%-18s = sizeof(%s)," % (stem + "_size", sym))
        out.append("    .%-18s = ARRAY_SIZE(%s)," % (stem + "_array_s", sym))
        out.append("    .%-18s = %uu," % (stem + "_rate", rate))
        out.append("")
    out.append("};")
    out.append("")
    out.append("")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Global Function")
    out.append("//===========================================================")
    out.append("")
    out.append("")
    out.append("")
    out.append("")
    out.append("//===========================================================")
    out.append("// Local Function")
    out.append("//===========================================================")
    out.append("")
    out.append("")
    out.append("")
    out.append("")
    out.append("#endif //APP_TARGET == APP_TARGET_AK512")
    out.append("")
    return "\r\n".join(out)


# ---------------------------------------------------------------- commands


def cmd_extract(args):
    text = subprocess.check_output(
        ["git", "-C", REPO, "show", "%s:src/app/apps/classic/tone_data_int16.c" % args.rev]
    ).decode("utf-8", "replace")
    arrays = parse_c_arrays(text)
    os.makedirs(SRC_DIR, exist_ok=True)
    for sym, wav_name, _, _, _, _ in TONES:
        if sym not in arrays:
            raise SystemExit("%s not found at rev %s" % (sym, args.rev))
        data = arrays[sym]
        path = os.path.join(SRC_DIR, wav_name)
        write_wav(path, data, MASTER_RATE_HZ)
        print("  wrote %s (%u samples @ %u Hz)" % (wav_name, len(data), MASTER_RATE_HZ))


def cmd_gen(args):
    tables = []
    print("Generating tone tables:")
    for sym, wav_name, stem, rate, aa, precomp in TONES:
        x, in_rate = read_wav(os.path.join(SRC_DIR, wav_name))
        if in_rate != MASTER_RATE_HZ:
            raise SystemExit("%s: master must be %u Hz" % (wav_name, MASTER_RATE_HZ))
        y, taps = resample(x, MASTER_RATE_HZ, rate, aa)
        pre_taps = 0
        if precomp and rate != MASTER_RATE_HZ:
            y, pre_taps = apply_precomp(y, rate, PRECOMP_TAPS)
        data = to_int16(y, sym)
        tables.append((sym, rate, data, stem))
        g = math.gcd(MASTER_RATE_HZ, rate)
        print(
            "  %-16s %5u Hz  x%u/%u  %6u -> %6u samples  %6u bytes"
            "  (%u-tap AA FIR, %u-tap pre-comp)"
            % (sym, rate, rate // g, MASTER_RATE_HZ // g, len(x), len(data),
               len(data) * 2, taps, pre_taps)
        )
    total = sum(len(d) * 2 for _, _, d, _ in tables)
    print("  total %u bytes" % total)
    with open(C_PATH, "wb") as f:
        f.write(emit_c(tables).encode("ascii"))
    print("  wrote %s" % os.path.relpath(C_PATH, REPO))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="recover 48 kHz WAV masters from a git rev of the C file")
    p.add_argument("--rev", default="HEAD")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("gen", help="generate tone_data_int16.c from the WAV masters")
    p.set_defaults(func=cmd_gen)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
