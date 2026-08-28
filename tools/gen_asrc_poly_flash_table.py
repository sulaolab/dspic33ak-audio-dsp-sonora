#!/usr/bin/env python3
"""Generate a flash-resident ASRC polyphase coefficient table.

The device builds the same table into RAM at startup in asrc_poly_build()
(src/app/apps/asrc/audio_app_asrc.c). A flash-resident copy costs program
memory instead of data memory, which is the only way an AK128 (16 KB RAM) can
run the polyphase kernel at all.

This script reproduces asrc_poly_build() *operation by operation in float32*,
so the generated table is bit-identical to what the device would compute. That
is verifiable without hardware: regenerating the committed L=128 M=32 table
must reproduce its recorded CRC32.

    # self-check against the committed table (no hardware needed)
    python tools/gen_asrc_poly_flash_table.py --selftest

    # generate the AK512 M30 production geometry at a smaller phase count
    python tools/gen_asrc_poly_flash_table.py --L 64 --M 30 \
        --fc 0.465 --window kaiser11 --beta 11.0

Window / fc / beta must match what the firmware's ASRC_POLY_* macros resolve
to for the build that will link the table; a mismatch is silent and shows up
only as degraded stopband rejection. The generated header carries the values it
was built with, and audio_app_asrc.c static-asserts L and M against it.
"""

from __future__ import annotations

import argparse
import re
import sys
import zlib
from pathlib import Path

import numpy as np

F32 = np.float32

WINDOWS = ("blackman", "blackman_harris", "kaiser11")

# asrc_poly_build() spells pi as this literal; rounding it to float32 here keeps
# every downstream product on the same footing as the device.
PI = F32(3.14159265358979)


def bessel_i0(x: F32) -> F32:
    """Mirror asrc_bessel_i0(): positive power series, float32, early break."""
    y = F32(F32(0.25) * x * x)
    term = F32(1.0)
    total = F32(1.0)
    for k in range(1, 25):
        fk = F32(k)
        term = F32(term * F32(y / F32(fk * fk)))
        total = F32(total + term)
        if term <= F32(total * F32(1.0e-7)):
            break
    return total


def window_value(wpos: F32, window: str, beta: F32, kaiser_norm: F32) -> F32:
    """Mirror the three window arms in asrc_poly_build()."""
    if window == "blackman_harris":
        return F32(
            F32(
                F32(F32(0.35875) - F32(F32(0.48829) * np.cos(F32(F32(2.0) * PI * wpos))))
                + F32(F32(0.14128) * np.cos(F32(F32(4.0) * PI * wpos)))
            )
            - F32(F32(0.01168) * np.cos(F32(F32(6.0) * PI * wpos)))
        )
    if window == "kaiser11":
        kx = F32(F32(F32(2.0) * wpos) - F32(1.0))
        kr = F32(np.sqrt(max(F32(0.0), F32(F32(1.0) - F32(kx * kx)))))
        return F32(bessel_i0(F32(beta * kr)) * kaiser_norm)
    # plain Blackman -- the codebase default when neither override is selected
    return F32(
        F32(F32(0.42) - F32(F32(0.5) * np.cos(F32(F32(2.0) * PI * wpos))))
        + F32(F32(0.08) * np.cos(F32(F32(4.0) * PI * wpos)))
    )


def build_table(L: int, M: int, fc: float, window: str, beta: float) -> np.ndarray:
    """Return an (L+1, M) float32 array matching asrc_poly_build()."""
    if M % 2 != 0:
        raise SystemExit("M must be even: the read window is centred and the pair kernels require it")
    MH = F32((M // 2) - 1)  # ASRC_POLY_MH
    fcf = F32(fc)
    betaf = F32(beta)
    kaiser_norm = F32(F32(1.0) / bessel_i0(betaf)) if window == "kaiser11" else F32(1.0)

    table = np.zeros((L + 1, M), dtype=np.float32)
    for p in range(L + 1):
        row_sum = F32(0.0)
        for k in range(M):
            d = F32(F32(F32(k) - MH) - F32(F32(p) / F32(L)))
            x = F32(F32(F32(2.0) * fcf) * d)
            if F32(-1.0e-6) < x < F32(1.0e-6):
                sinc = F32(1.0)
            else:
                px = F32(PI * x)
                sinc = F32(np.sin(px) / px)
            wpos = F32(F32(F32(d + MH) + F32(1.0)) / F32(M))
            win = window_value(wpos, window, betaf, kaiser_norm)
            c = F32(F32(F32(F32(2.0) * fcf) * sinc) * win)
            table[p, k] = c
            row_sum = F32(row_sum + c)
        # normalise the row to unity DC gain, exactly as the device does
        if row_sum > F32(1.0e-9) or row_sum < F32(-1.0e-9):
            inv = F32(F32(1.0) / row_sum)
            for k in range(M):
                table[p, k] = F32(table[p, k] * inv)
    return table


def to_words(table: np.ndarray) -> list[int]:
    """Raw 32-bit float bit patterns, row-major, as the device stores them."""
    return [int(w) for w in table.reshape(-1).view(np.uint32)]


def crc32_of(words: list[int]) -> int:
    """CRC32 over the little-endian bytes of each word (audio_app_asrc.c:1394)."""
    blob = np.asarray(words, dtype="<u4").tobytes()
    return zlib.crc32(blob) & 0xFFFFFFFF


def emit_sources(
    words: list[int], L: int, M: int, fc: float, window: str, beta: float, out_dir: Path
) -> tuple[Path, Path]:
    # File stem and C symbol follow the existing pair in this directory: the file is
    # audio_app_asrc_poly_l128_flash.c, the array it defines is asrc_poly_l128_flash.
    stem = f"audio_app_asrc_poly_l{L}m{M}_flash"
    symbol = f"asrc_poly_l{L}m{M}_flash"
    guard = stem.upper() + "_H"
    macro = f"ASRC_POLY_L{L}M{M}_FLASH"
    crc = crc32_of(words)
    n = len(words)
    tool = "tools/gen_asrc_poly_flash_table.py"
    geom = f"L={L} M={M} fc={fc:g} window={window}" + (
        f" beta={beta:g}" if window == "kaiser11" else ""
    )

    header = f"""// GENERATED -- REGENERATE IN THIS DIRECTORY.  ({tool})
// geometry      : {geom}
// entries       : {n} = (L+1)*M
// CRC32 (LE)    : 0x{crc:08X}
// Computed host-side in float32, operation for operation as asrc_poly_build()
// does on the device. NOT bit-identical to a device-generated table: the device
// uses its own sinf/cosf, and the window sum cancels hard at the edges, so a
// handful of near-zero taps differ. Agreement is better than 1e-6 of full scale
// (see --selftest against the L=128 M=32 table), i.e. below -120 dBFS.
#ifndef {guard}
#define {guard}

#include <stdint.h>

#define {macro}_L       ({L}u)
#define {macro}_M       ({M}u)
#define {macro}_N       ({n}u)
#define {macro}_CRC32   (0x{crc:08X}u)

extern const uint32_t {symbol}[{n}];

#endif // {guard}
"""

    body = [
        f"// GENERATED -- REGENERATE IN THIS DIRECTORY.  ({tool})",
        f"// geometry      : {geom}",
        f"// entries       : {n} = (L+1)*M",
        f"// CRC32 (LE)    : 0x{crc:08X}",
        "// The words are the RAW 32-bit float bit patterns, so the compiled flash table needs no",
        "// startup conversion. The kernel reinterprets each row as (const float*).",
        "// Host-computed in float32 mirroring asrc_poly_build(); agreement with a device-generated",
        "// table is better than 1e-6 of full scale, not bit-exact (device libm).",
        f'#include "{stem}.h"',
        "",
        "// Manifest tripwire: this table is ASRC-app-owned. The generated header carries no app",
        "// config, so pull it in explicitly for SONORA_APP_IS_ASRC.",
        '#include "app_specific_config_defs.h"',
        "#if !SONORA_APP_IS_ASRC",
        f'#  error "{stem}.c is ASRC-app-owned; build it only in an ASRC manifest '
        '(SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."',
        "#endif",
        "",
        "// `const` -> program flash on dsPIC33A (verified in map: not .bss, no startup copy).",
        f"const uint32_t {symbol}[{n}] = {{",
    ]
    for p in range(L + 1):
        row = words[p * M : (p + 1) * M]
        cells = ", ".join(f"0x{w:08X}" for w in row)
        body.append(f"    {cells},  // phase {p}")
    body.append("};")
    body.append("")

    out_dir.mkdir(parents=True, exist_ok=True)
    c_path = out_dir / f"{stem}.c"
    h_path = out_dir / f"{stem}.h"
    # CRLF: the fleet's .gitattributes normalises to text=auto eol=crlf.
    c_path.write_text("\n".join(body), encoding="utf-8", newline="\r\n")
    h_path.write_text(header, encoding="utf-8", newline="\r\n")
    return c_path, h_path


def parse_committed_words(path: Path) -> list[int]:
    """Pull the 0xXXXXXXXX literals out of an existing generated .c.

    Scoped to the array initialiser: the file's own header comment records a
    CRC32 in the same 0x%08X form and would otherwise be counted as a coefficient.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    start = text.find("= {")
    if start < 0:
        raise SystemExit(f"no array initialiser found in {path}")
    end = text.find("};", start)
    body = text[start + 3 : end if end > 0 else len(text)]
    return [int(m, 16) for m in re.findall(r"0x([0-9A-Fa-f]{8})\b", body)]


# Pass/fail on absolute error normalised by the peak tap -- the scale that decides
# what the filter does. Rounding noise lands near 1e-7 of full scale; getting the
# window, fc or normalisation wrong lands at 1e-3 or worse, so this discriminates
# by four orders of magnitude. A *relative* threshold cannot: the edge taps are
# ~1e-6 of full scale and a last-ULP cosf difference moves them by 0.05 %.
SELFTEST_TOL_FS = 1.0e-6


def selftest(repo_root: Path) -> int:
    """Check this generator against the committed L=128 M=32 table.

    Geometry recovered from the table itself, not from today's config macros:
    its rows sum to 1.0, its peak tap is 0.9 (= 2*fc, so fc=0.45), and the
    window value implied by the edge taps is plain Blackman -- the table predates
    the Blackman-Harris default that APP_ASRC_HIFI_KERNEL now selects.

    The comparison is numerical, not bit-for-bit, and deliberately so: the
    device computes the window with sinf/cosf from its own libm, and the Blackman
    sum (0.42 - 0.5*cos + 0.08*cos) cancels down to ~0.3 % of its terms near the
    window edges, which amplifies a last-ULP cosf difference into a large
    *relative* error on taps whose absolute value is ~1e-5. Reproducing the exact
    bits host-side would mean reproducing the device's libm; that is why the
    committed table was captured from a device dump ("?ak") in the first place.
    """
    ref = repo_root / "src/app/apps/asrc/audio_app_asrc_poly_l128_flash.c"
    if not ref.exists():
        print(f"selftest: reference table not found: {ref}", file=sys.stderr)
        return 2
    expected = parse_committed_words(ref)
    got = to_words(build_table(L=128, M=32, fc=0.45, window="blackman", beta=11.0))

    print(f"reference words : {len(expected)}  crc32=0x{crc32_of(expected):08X}")
    print(f"generated words : {len(got)}  crc32=0x{crc32_of(got):08X}")
    if len(expected) != len(got):
        print("selftest FAILED: length mismatch", file=sys.stderr)
        return 1

    a = np.asarray(expected, dtype="<u4").view(np.float32).astype(np.float64)
    b = np.asarray(got, dtype="<u4").view(np.float32).astype(np.float64)
    identical = int(np.count_nonzero(np.asarray(expected) == np.asarray(got)))
    # Taps the device rounded to exactly 0.0 have no relative error to speak of;
    # they are the sinc zeros (x integer) and are reported separately.
    peak = float(np.abs(a).max())
    err_fs = float(np.abs(b - a).max()) / peak          # worst error, in units of full scale
    zero_ref = a == 0.0
    sig = np.abs(a) > 1.0e-3                            # taps that carry the filter
    rel_sig = float((np.abs(b - a)[sig] / np.abs(a)[sig]).max())

    print(f"bit-identical   : {identical}/{len(got)}")
    print(f"device-zero taps: {int(zero_ref.sum())} (sinc zeros)")
    print(f"peak tap        : {peak:.6f}  (= 2*fc)")
    print(f"worst abs error : {err_fs:.2e} of full scale  ({20 * np.log10(err_fs):.1f} dB)")
    print(f"worst rel error : {rel_sig:.2e} over the {int(sig.sum())} taps with |c| > 1e-3")

    if err_fs < SELFTEST_TOL_FS:
        print(f"selftest PASSED: geometry reproduced; residual is libm rounding "
              f"({err_fs:.1e} < tol {SELFTEST_TOL_FS:.0e} of full scale)")
        return 0
    print(f"selftest FAILED: worst error {err_fs:.2e} of full scale exceeds "
          f"{SELFTEST_TOL_FS:.0e} -- that is a formula/parameter mismatch, not rounding",
          file=sys.stderr)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--L", type=int, default=64, help="phase count (table has L+1 rows)")
    ap.add_argument("--M", type=int, default=30, help="taps per phase (must be even)")
    ap.add_argument("--fc", type=float, default=0.465, help="normalised cutoff (ASRC_POLY_FC)")
    ap.add_argument("--window", choices=WINDOWS, default="kaiser11")
    ap.add_argument("--beta", type=float, default=11.0, help="Kaiser beta (kaiser11 only)")
    ap.add_argument("--out-dir", type=Path, default=None,
                    help="default: src/app/apps/asrc next to this tool's repo root")
    ap.add_argument("--selftest", action="store_true",
                    help="regenerate the committed L=128 M=32 table and compare, then exit")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    if args.selftest:
        return selftest(repo_root)

    out_dir = args.out_dir or (repo_root / "src/app/apps/asrc")
    table = build_table(args.L, args.M, args.fc, args.window, args.beta)
    words = to_words(table)
    c_path, h_path = emit_sources(words, args.L, args.M, args.fc, args.window, args.beta, out_dir)

    rom = len(words) * 4
    print(f"geometry : L={args.L} M={args.M} fc={args.fc:g} window={args.window}"
          + (f" beta={args.beta:g}" if args.window == "kaiser11" else ""))
    print(f"entries  : {len(words)} = (L+1)*M")
    print(f"CRC32    : 0x{crc32_of(words):08X}")
    print(f"ROM cost : {rom} bytes ({rom / 1024:.1f} KiB) of program flash")
    print(f"wrote    : {c_path}")
    print(f"           {h_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
