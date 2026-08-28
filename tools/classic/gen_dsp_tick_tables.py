#!/usr/bin/env python3
"""
gen_dsp_tick_tables.py -- generate the Classic tick-sample tables

    src/app/apps/classic/dsp/kinkon_tables.h      (KIN / KON / end-KIN)
    src/app/apps/classic/dsp/clickclack_tables.h  (tick A / B)

Why this exists
---------------
Both headers said "Auto-generated from <some>.wav" but neither the WAV nor the
generator was ever committed, so they were regeneratable in comment only: the C
file was the last surviving copy of the audio.  This tool restores the
wav -> py -> table chain that tone_data_int16.c already has, and while doing so
stores the samples as int16 instead of float32.

    table                 float32     int16      note
    g_kinkon_tickA         57,600     28,800     lossless (see below)
    g_kinkon_tickB         34,560     17,280     lossless
    g_kinkon_tickC         61,440     30,720     lossless
    g_clickclack_tickA      5,760      2,880     -96 dBFS quantization
    g_clickclack_tickB      5,760      2,880     -96 dBFS quantization
    total                 165,120     82,560     -82,560 bytes of program flash

Why the kinkon tables are lossless
----------------------------------
The kinkon masters turned out to be int16 data that had been widened to float32:
every sample sits on the k/32768 grid, with a measured deviation of only 1.6e-5
of a step -- and that residue is exactly the 9-decimal rounding of the C file's
own printed literals, not structure in the data.  So int16 storage plus a
1/32768 runtime scale recovers the values the header had *before* it was
printed.  The scale is a power of two, so (float)v * (1.0f/32768.0f) is computed
without rounding.

Against the committed float header the reconstruction therefore differs by at
most 9.3e-10 absolute (-173.6 dB below peak), and that difference is the old
header's print rounding, which the int16 form removes.  For scale: one float32
ULP at the peak sample is 3e-8, i.e. 30 dB *larger* than this deviation, so no
float32 arithmetic downstream can observe it.  93 % of the saving costs nothing
that can be measured, let alone heard.

The clickclack tables are the part that does cost something -- see below.

The clickclack tables are genuinely float (a normalized reconstruction
residual), so quantizing them to the 1/32768 grid costs at most half a step =
-96.3 dBFS relative to their own full scale.  -1.000000000 lands on -32768 and
the positive peak stays below +32767, so no sample clips and the scale can stay
a power of two here too.

Usage
-----
    # one-time: recover the masters from a git revision of the headers
    python tools/classic/gen_dsp_tick_tables.py extract --rev <rev>

    # regenerate the headers from the masters in tools/classic/tone_src/
    python tools/classic/gen_dsp_tick_tables.py gen

    # prove the generated int16 tables reconstruct a revision's float tables
    python tools/classic/gen_dsp_tick_tables.py verify --rev <rev>

The masters under tools/classic/tone_src/ are 32-bit float mono WAV and are the
authoritative source data -- float, not int16, so that re-running `gen` never
quantizes twice.  Never re-derive a master from the generated C.

Not a rate reduction
--------------------
Unlike tone_data_int16.c these tables are NOT candidates for a lower stored
rate: kinkon_synth.c / clickclack_synth.c read them with a plain integer index
at the 48 kHz internal rate (get_event_sample_48k / local_get_tick_sample_48k),
so there is no runtime SRC to convert a lower rate, and their content reaches
14 kHz (kinkon B/C, clickclack) instead of the 1.4 kHz of a button click.  A
rate cut here would mean building an interpolator first and then paying its
imaging floor -- see [internal] tone_data_sample_rate_reduction.md
for what that costs.  int16 is the part that is free.
"""

import argparse
import math
import os
import re
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC_DIR = os.path.join(HERE, "tone_src")

# The stored int16 is value * SCALE_DEN, reconstructed at runtime as
# (float)v * (1.0f / SCALE_DEN).  A power of two on purpose: the runtime
# multiply is then exact, so the kinkon tables reconstruct bit-exactly.
SCALE_DEN = 32768

FS_HZ = 48000


class Header:
    def __init__(self, path, guard, prefix, banner, defines, tables):
        self.path = path
        self.guard = guard
        self.prefix = prefix
        self.banner = banner  # provenance comment lines, without the leading //
        self.defines = defines  # [(name, value_text)] emitted before the arrays
        self.tables = tables  # [(symbol, len_define, wav_name)]


HEADERS = [
    Header(
        path="src/app/apps/classic/dsp/kinkon_tables.h",
        guard="KINKON_TABLES_H",
        prefix="KINKON",
        banner=[
            "Auto-generated from kinkon_repro_pack_v5",
            "Source WAV set:",
            "  - sample_A_kin_v5.wav",
            "  - sample_B_kon_v5.wav",
            "  - sample_C_end_kin_v5.wav",
            "Fs = 48000 Hz",
            'A = regular "KIN", B = regular "KON", C = final end-only "KIN"',
        ],
        defines=[("KINKON_FS_HZ", "(%uu)" % FS_HZ)],
        tables=[
            ("g_kinkon_tickA", "KINKON_TICKA_LEN", "kinkon_A_kin_48k.wav"),
            ("g_kinkon_tickB", "KINKON_TICKB_LEN", "kinkon_B_kon_48k.wav"),
            ("g_kinkon_tickC", "KINKON_TICKC_LEN", "kinkon_C_end_kin_48k.wav"),
        ],
    ),
    Header(
        path="src/app/apps/classic/dsp/clickclack_tables.h",
        guard="CLICKCLACK_TABLES_H",
        prefix="CLICKCLACK",
        banner=[
            "Auto-generated from: original_recon_residual_noise_period_0p40s_30s.wav",
            "Fs = 48000 Hz, tick_len = 1440 samples (~30ms)",
            'These tables reproduce the current "best sounding" tick (A/B) for initial bring-up.',
            "Later you can replace them with true real-time modal synthesis (B plan).",
        ],
        defines=[("CLICKCLACK_FS_HZ", "(%uu)" % FS_HZ)],
        tables=[
            ("g_clickclack_tickA", "CLICKCLACK_TICK_LEN", "clickclack_tickA_48k.wav"),
            ("g_clickclack_tickB", "CLICKCLACK_TICK_LEN", "clickclack_tickB_48k.wav"),
        ],
    ),
]


# ---------------------------------------------------------------- C parsing


def parse_c_float_arrays(text):
    """Return {symbol: np.float64 array} for every `const float x[N] = {...}`."""
    out = {}
    pat = r"const\s+float\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;"
    for m in re.finditer(pat, text, re.S):
        vals = [v.strip().rstrip("f") for v in m.group(2).replace("\n", " ").split(",")]
        vals = [float(v) for v in vals if v]
        out[m.group(1)] = np.asarray(vals, dtype=np.float64)
    return out


def parse_c_int16_arrays(text):
    """Return {symbol: np.int16 array} for every `const int16_t x[N] = {...}`."""
    out = {}
    pat = r"const\s+int16_t\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;"
    for m in re.finditer(pat, text, re.S):
        vals = [v.strip() for v in m.group(2).replace("\n", " ").split(",")]
        vals = [int(v) for v in vals if v]
        out[m.group(1)] = np.asarray(vals, dtype=np.int16)
    return out


# ---------------------------------------------------------------- float WAV
#
# 32-bit float mono WAV (WAVE_FORMAT_IEEE_FLOAT).  Hand-rolled because the
# stdlib `wave` module is PCM-only, and the masters have to stay float: these
# tables are normalized reconstructions, and re-quantizing a master on every
# `gen` is exactly the mistake this chain exists to prevent.


def write_wav_f32(path, data, rate):
    payload = np.asarray(data, dtype="<f4").tobytes()
    fmt = struct.pack("<HHIIHH", 3, 1, rate, rate * 4, 4, 32)  # IEEE float, mono
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    if len(payload) % 2:
        chunks += b"\x00"
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def read_wav_f32(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise SystemExit("%s: not a RIFF/WAVE file" % path)
    pos, rate, data = 12, None, None
    while pos + 8 <= len(raw):
        cid = raw[pos : pos + 4]
        size = struct.unpack("<I", raw[pos + 4 : pos + 8])[0]
        body = raw[pos + 8 : pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if (tag, ch, bits) != (3, 1, 32):
                raise SystemExit(
                    "%s: expected mono 32-bit float, got tag=%u ch=%u bits=%u"
                    % (path, tag, ch, bits)
                )
        elif cid == b"data":
            data = np.frombuffer(body, dtype="<f4").astype(np.float64)
        pos += 8 + size + (size & 1)
    if rate is None or data is None:
        raise SystemExit("%s: missing fmt or data chunk" % path)
    return data, rate


# ---------------------------------------------------------------- emitting


def to_int16(y, name):
    q = np.rint(np.asarray(y, dtype=np.float64) * SCALE_DEN)
    lo, hi = q.min(), q.max()
    if lo < -32768 or hi > 32767:
        n = int(np.count_nonzero((q < -32768) | (q > 32767)))
        print(
            "    %-20s WARNING: %u sample(s) clipped (range %.0f..%.0f)"
            % (name, n, lo, hi)
        )
    return np.clip(q, -32768, 32767).astype(np.int16)


def fmt_array(symbol, len_define, data):
    lines = ["static const int16_t %s[%s] = {" % (symbol, len_define)]
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        cells = ["%8d," % chunk[0]] + ["%7d," % v for v in chunk[1:]]
        lines.append("".join(cells))
    lines.append("};")
    return "\r\n".join(lines)


def emit_header(hdr, tables):
    """tables: list of (symbol, len_define, np.int16 data)"""
    total = sum(len(d) * 2 for _, _, d in tables)
    out = []
    out.append("#ifndef %s" % hdr.guard)
    out.append("#define %s" % hdr.guard)
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    for line in hdr.banner:
        out.append("// %s" % line)
    out.append("//")
    out.append("// GENERATED FILE -- do not edit by hand.")
    out.append("// Regenerate with: python tools/classic/gen_dsp_tick_tables.py gen")
    out.append("// Masters: 48 kHz mono 32-bit-float WAV in tools/classic/tone_src/")
    out.append("//")
    out.append(
        "// Samples are int16 scaled by 1/%u; the consumer multiplies by"
        % SCALE_DEN
    )
    out.append(
        "// %s_TICK_SCALE.  The scale is a power of two, so the reconstruction"
        % hdr.prefix
    )
    out.append("// (float)v * scale is exact.")
    out.append("//")
    for sym, _, data in tables:
        out.append(
            "// %-20s %6u samples  %6u bytes  %.3f s"
            % (sym, len(data), len(data) * 2, len(data) / float(FS_HZ))
        )
    out.append("// %-20s %20s %6u bytes" % ("total", "", total))
    out.append("")
    for name, value in hdr.defines:
        out.append("#define %-21s %s" % (name, value))
    # one length define per distinct symbol length name
    seen = []
    for _, len_define, data in tables:
        if len_define in seen:
            continue
        seen.append(len_define)
        out.append("#define %-21s (%uu)" % (len_define, len(data)))
    out.append(
        "#define %-21s (1.0f / %u.0f)" % (hdr.prefix + "_TICK_SCALE", SCALE_DEN)
    )
    out.append("")
    for sym, len_define, data in tables:
        out.append(fmt_array(sym, len_define, data))
        out.append("")
    out.append("#endif // %s" % hdr.guard)
    out.append("")
    return "\r\n".join(out)


# ---------------------------------------------------------------- commands


def show_at_rev(rev, path):
    return subprocess.check_output(
        ["git", "-C", REPO, "show", "%s:%s" % (rev, path)]
    ).decode("utf-8-sig", "replace")


def cmd_extract(args):
    os.makedirs(SRC_DIR, exist_ok=True)
    for hdr in HEADERS:
        arrays = parse_c_float_arrays(show_at_rev(args.rev, hdr.path))
        if not arrays:
            raise SystemExit(
                "%s at rev %s has no `const float` tables -- already converted? "
                "Pass the revision before the int16 change." % (hdr.path, args.rev)
            )
        for sym, _, wav_name in hdr.tables:
            if sym not in arrays:
                raise SystemExit("%s not found in %s at rev %s" % (sym, hdr.path, args.rev))
            x = arrays[sym]
            write_wav_f32(os.path.join(SRC_DIR, wav_name), x, FS_HZ)
            print(
                "  wrote %-28s %6u samples @ %u Hz  peak %.6f"
                % (wav_name, len(x), FS_HZ, float(np.abs(x).max()))
            )


def cmd_gen(args):
    grand = 0
    for hdr in HEADERS:
        print("Generating %s:" % hdr.path)
        tables = []
        for sym, len_define, wav_name in hdr.tables:
            x, rate = read_wav_f32(os.path.join(SRC_DIR, wav_name))
            if rate != FS_HZ:
                raise SystemExit("%s: master must be %u Hz" % (wav_name, FS_HZ))
            data = to_int16(x, sym)
            err = np.abs(x - data.astype(np.float64) / SCALE_DEN).max()
            peak = float(np.abs(x).max()) or 1.0
            tables.append((sym, len_define, data))
            print(
                "  %-20s %6u samples  %6u bytes  quantization %6.1f dB below peak"
                % (sym, len(data), len(data) * 2,
                   20.0 * math.log10(err / peak) if err > 0 else -999.0)
            )
        total = sum(len(d) * 2 for _, _, d in tables)
        grand += total
        path = os.path.join(REPO, hdr.path)
        with open(path, "wb") as f:
            f.write(emit_header(hdr, tables).encode("ascii"))
        print("  total %u bytes -> %s" % (total, hdr.path))
    print("Grand total %u bytes of tick tables" % grand)


def cmd_verify(args):
    """
    Compare the float tables at --rev against the current int16 headers.

    "BIT-EXACT" is decided on float32 bit patterns, because that is what the
    firmware actually holds: the old header's `const float` was the 9-decimal
    literal rounded by the compiler to the nearest float32, and the new one is
    (float)v * (1.0f/32768.0f).  If those two float32 values have identical bit
    patterns, no code downstream can tell the tables apart.  Comparing against
    the printed decimal instead would only measure the print width (5e-10).
    """
    worst = 0.0
    exact = True
    for hdr in HEADERS:
        old = parse_c_float_arrays(show_at_rev(args.rev, hdr.path))
        with open(os.path.join(REPO, hdr.path), "rb") as f:
            new = parse_c_int16_arrays(f.read().decode("ascii", "replace"))
        print("%s (vs %s):" % (hdr.path, args.rev))
        for sym, _, _ in hdr.tables:
            if sym not in old:
                raise SystemExit("%s not in %s at rev %s" % (sym, hdr.path, args.rev))
            if sym not in new:
                raise SystemExit("%s not in the current %s" % (sym, hdr.path))
            if len(old[sym]) != len(new[sym]):
                raise SystemExit(
                    "%s: length %u -> %u" % (sym, len(old[sym]), len(new[sym]))
                )
            # what the compiler stored for the old header ...
            a32 = old[sym].astype(np.float32)
            # ... and what the firmware now computes at runtime
            b32 = new[sym].astype(np.float32) * np.float32(1.0 / SCALE_DEN)
            n_diff = int(np.count_nonzero(a32.view(np.uint32) != b32.view(np.uint32)))
            bit_exact = n_diff == 0
            exact = exact and bit_exact
            err = float(np.abs(a32.astype(np.float64) - b32.astype(np.float64)).max())
            peak = float(np.abs(a32).max()) or 1.0
            worst = max(worst, err / peak)
            print(
                "  %-20s %s  max err %.3e (%7.1f dB below peak)%s"
                % (sym,
                   "BIT-EXACT" if bit_exact else "quantized",
                   err,
                   20.0 * math.log10(err / peak) if err > 0 else -999.0,
                   "" if bit_exact else "  %u/%u samples differ" % (n_diff, len(a32)))
            )
    print(
        "Worst case %.1f dB below peak%s"
        % (20.0 * math.log10(worst) if worst > 0 else -999.0,
           " -- every table bit-exact" if exact else "")
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("extract", help="recover float WAV masters from a git rev of the headers")
    p.add_argument("--rev", default="HEAD")
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("gen", help="generate the headers from the WAV masters")
    p.set_defaults(func=cmd_gen)

    p = sub.add_parser("verify", help="compare the current int16 headers against a rev's float tables")
    p.add_argument("--rev", default="HEAD")
    p.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
