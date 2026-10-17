#!/usr/bin/env python3
"""Analyse a *MEAS poly capture for PROTECTED-BAND ALIAS level (44.1 -> 16 kHz R2 and friends).

Reads the serial-monitor LOG FILE, not the COM port: the port belongs to the monitor bridge and
use the bridge documented in the separate serial-monitor repository.  Every `*MEAS_BEGIN .. *MEAS_END`
block in the log is addressable, so a whole session's captures are analysed from one file after
the fact.

WHAT IT REPORTS, AND WHY THE ALIAS IS NOT A "THD" NUMBER.  For an alias row the injected tone is
OUTSIDE the output band, so the output ideally contains nothing at all: the thing being measured
is the residual that folded into the protected band, referred to the injected level.  So the
figure of merit is (alias peak in dBFS) - (injected level in dBFS) = dBc, which is directly
comparable to the HOST gate's alias number.  For a passband row the tone IS in band and the
figure is its own level relative to the injected level.

Windowing: Kaiser.  The output tone is NOT sample-domain exact even when the input is -- the rear
resampler's step is irrational (1.378125 here) -- so a rectangular window would smear the
fundamental across the very bins the alias lives in.  beta=24 puts the window's own sidelobes near
-200 dB, far below the 24-bit capture floor, which is what makes a -114 dB alias readable at all.
"""
from __future__ import annotations
import argparse, math, pathlib, re, sys
import numpy as np

FULL_SCALE = float(2**23 - 1)


def db(x) -> float:
    return 20.0 * math.log10(max(abs(float(x)), 1.0e-300))


_MON_RX = re.compile(r"<< \[(partial(?: idle)?|raw len=\d+ |)(?:\] )?\[?source=uart\] ?(.*)$")
_RAW_HEX = re.compile(r"^((?:[0-9A-F]{2} )*[0-9A-F]{2}) \|")


def _reassemble(lines):
    """Rebuild the board's UART text from serial-monitor log lines, so a dump reads as it was sent.

    The monitor writes one log line per RX event, not per UART line.  Under load it records a line in
    pieces -- `[partial]` / `[partial idle]` fragments with no newline, and `[raw len=N]` hex chunks
    that can carry the newline -- so one sample or one `*MEAS_END` can be spread over several log lines.
    Parsing log lines directly then (a) misses a split end marker, so the block merges with the next
    one, and (b) counts each digit fragment of a sample as a sample of its own.  Measured 2026-09-24
    (full81, A11k025_B32 image_probe): a dump in 5,220 fragments with `*MEAS_END` spread over six
    `[partial idle]` lines read as 7,023 samples; reassembled it is exactly 2,048.  Lines that are not
    monitor RX lines (an extracted capture file, TX echoes, monitor notices) pass through unchanged.
    """
    out, pending = [], ""
    for ln in lines:
        m = _MON_RX.search(ln)
        if not m:
            out.append(ln)
            continue
        kind, body = m.group(1), m.group(2)
        if kind.startswith("partial"):
            pending += body
        elif kind.startswith("raw"):
            r = _RAW_HEX.match(body)
            if not r:
                continue
            parts = bytes.fromhex(r.group(1).replace(" ", "")).decode("latin-1").split("\n")
            for part in parts[:-1]:
                out.append(pending + part.rstrip("\r"))
                pending = ""
            pending += parts[-1]
        else:
            out.append(pending + body)
            pending = ""
    if pending:
        out.append(pending)
    return out


def blocks(log: pathlib.Path):
    """Every complete MEAS block in the log, as (header_meta, samples)."""
    return blocks_from_text(log.read_text(encoding="utf-8", errors="replace"))


def blocks_from_text(text: str):
    """blocks() on text already read, e.g. only the part of a log written after a known offset."""
    lines = _reassemble(text.splitlines())
    out = []
    i = 0
    while i < len(lines):
        if "MEAS_BEGIN" in lines[i]:
            meta = dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", lines[i]))
            vals = []
            j = i + 1
            while j < len(lines) and "MEAS_END" not in lines[j]:
                m = re.search(r"\]\s*(-?\d+)\s*$", lines[j]) or re.fullmatch(r"\s*(-?\d+)\s*", lines[j])
                if m:
                    vals.append(int(m.group(1)))
                j += 1
            if j < len(lines):
                out.append((meta, np.asarray(vals, dtype=np.float64)))
            i = j
        i += 1
    return out


def mainlobe_amplitude_dbfs(x: np.ndarray, beta: float, k_peak: int, half: int = 8) -> float:
    """Amplitude of the tone at bin k_peak, by COHERENT POWER SUM over its main lobe.

    Peak-bin magnitude is wrong for a tone that does not sit on a bin centre, and the tone
    generally does NOT: the input table is sample-domain exact at the SOURCE rate, but the rear
    resampler's step is irrational, so the output frequency is arbitrary with respect to the
    capture length.  980 Hz over 2048 samples at 16 kHz is 125.44 cycles -- 0.44 of a bin off,
    which costs 0.3 dB of scalloping with a Kaiser-24 main lobe and would be misread as passband
    droop.  6,468 Hz lands at 828.0 bins and shows none of it, which is how the artefact was
    identified rather than believed.

    Parseval for a windowed sine gives sum|X_k|^2 over the +f lobe = N * A^2 / 4 * sum(w^2),
    so A follows from the lobe's power regardless of where inside a bin the tone sits.
    """
    w = np.kaiser(x.size, beta)
    X = np.fft.rfft(x / FULL_SCALE * w)
    lo, hi = max(0, k_peak - half), min(X.size, k_peak + half + 1)
    power = float(np.sum(np.abs(X[lo:hi]) ** 2))
    amp = math.sqrt(4.0 * power / (x.size * float(np.sum(w * w))))
    return db(amp)


def spectrum(x: np.ndarray, beta: float):
    w = np.kaiser(x.size, beta)
    # Normalise so a full-scale sine reads 0 dBFS regardless of window or length.
    X = np.fft.rfft(x / FULL_SCALE * w)
    return np.abs(X) * 2.0 / w.sum()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=pathlib.Path, help="serial-monitor log file")
    ap.add_argument("--block", type=int, default=-1, help="which MEAS block (default: last)")
    ap.add_argument("--all", action="store_true", help="report every block in the log")
    ap.add_argument("--at", type=float, default=None,
                    help="expected alias/tone frequency in Hz (searched +/- --window)")
    ap.add_argument("--window", type=float, default=40.0, help="search half-width in Hz")
    ap.add_argument("--protected", type=float, default=6500.0, help="protected band edge in Hz")
    ap.add_argument("--beta", type=float, default=24.0)
    args = ap.parse_args()

    found = blocks(args.log)
    if not found:
        print("no complete *MEAS_BEGIN..*MEAS_END block in that log", file=sys.stderr)
        return 2
    sel = found if args.all else [found[args.block]]

    for meta, x in sel:
        n = int(meta.get("n", x.size))
        fs = float(meta.get("fs_out_hz", 0.0))
        tone = float(meta.get("tone_hz", 0.0))
        level = float(meta.get("level_dbfs", 0.0))
        if x.size != n:
            print(f"  ! sample count {x.size} != n={n} -- block truncated in the log", file=sys.stderr)
        mag = spectrum(x, args.beta)
        freqs = np.fft.rfftfreq(x.size, 1.0 / fs)
        # DC and the first couple of bins carry window leakage of any offset; exclude from peaks.
        first = 3
        prot = (freqs <= args.protected) & (np.arange(freqs.size) >= first)
        rms_dbfs = db(float(np.sqrt(np.mean((x / FULL_SCALE) ** 2))) * math.sqrt(2.0))
        kp = int(np.argmax(np.where(prot, mag, 0.0)))
        print(f"block: row={meta.get('table_row')} tone={tone:.0f} Hz @ fs_in={meta.get('fs_in_hz')} "
              f"level={level:+.0f} dBFS  fs_out={fs:.0f} Hz  n={x.size}  bin={fs/x.size:.3f} Hz")
        print(f"  full-capture peak-equivalent RMS      {rms_dbfs:+8.2f} dBFS")
        print(f"  strongest component <= {args.protected:.0f} Hz    "
              f"{db(mag[kp]):+8.2f} dBFS at {freqs[kp]:8.1f} Hz   "
              f"({db(mag[kp]) - level:+8.2f} dBc)")
        if args.at is not None:
            sel_at = np.abs(freqs - args.at) <= args.window
            if sel_at.any():
                k = int(np.argmax(np.where(sel_at, mag, 0.0)))
                lobe = mainlobe_amplitude_dbfs(x, args.beta, k)
                print(f"  at {args.at:.0f} +/- {args.window:.0f} Hz                "
                      f"{db(mag[k]):+8.2f} dBFS at {freqs[k]:8.1f} Hz   "
                      f"({db(mag[k]) - level:+8.2f} dBc)")
                print(f"  same component, main-lobe power sum   "
                      f"{lobe:+8.2f} dBFS                     "
                      f"({lobe - level:+8.2f} dBc)")
            # A floor estimate that excludes the expected component and the band edges.
            keep = prot & (np.abs(freqs - args.at) > 3.0 * args.window)
            if keep.any():
                med = float(np.median(mag[keep]))
                mx = float(np.max(mag[keep]))
                print(f"  rest of the protected band: median {db(med):+8.2f} dBFS   "
                      f"max {db(mx):+8.2f} dBFS")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
