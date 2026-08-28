#!/usr/bin/env python3
"""Stopband / alias analyzer for the 48 -> 32 kHz N=97 audio-mode front end.

Why not asrc_shootout.py: that tool answers "how clean is a PASSBAND tone"
(THD+N, DR) and assumes the stimulus itself is in band.  This study drives the
front end with tones ABOVE the 16 kHz output Nyquist, so the fundamental is not
in the capture at all -- what is in the capture is the FOLD, and the number
wanted is its level relative to the input tone, i.e. the stopband rejection the
host model predicts.  Reporting that needs a different question asked of the
same `*MEAS_BEGIN kernel=poly` capture, not a different capture.

Method
------
Kaiser-beta24 window (same window family as asrc_shootout.py, so the two agree
on what "no leakage" means), amplitude recovered by integrating power over the
window main lobe.  That is bin-offset tolerant: the frozen servo step leaves a
sub-ppm frequency error, so a rectangular read of a single bin would understate
the peak and its leakage would sit above the -107 dB the model predicts for the
protected band.

  amp = 2 * sqrt( sum |X_k|^2 over the lobe / (N * sum w^2) )      (see --selftest)

Levels
------
`dBFS`  : re 2^23-1 peak sine (the capture is LSB24).
`dBc`   : re the INPUT tone amplitude, i.e. dBFS - level_dbfs from the header.
          This is the same reference the host model uses for its rejection
          figures, so host and hardware columns are directly comparable.

Usage
-----
    python tools/asrc/n97_alias_analyze.py cap1.txt cap2.txt ...
    python tools/asrc/n97_alias_analyze.py --selftest
"""
from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys

import numpy as np

FULL_SCALE = float(2**23 - 1)


def db(x: float) -> float:
    return 20.0 * math.log10(max(abs(x), 1.0e-300))


def fold(f: float, fs: float) -> float:
    """Frequency f observed by a sampler running at fs (0 .. fs/2)."""
    r = math.fmod(f, fs)
    if r < 0.0:
        r += fs
    return fs - r if r > fs / 2.0 else r


def lobe_halfwidth(beta: float) -> int:
    # Kaiser main-lobe half width in bins, plus a margin for the servo's sub-ppm
    # frequency error and for the fact that the lobe skirt is not exactly zero.
    return int(math.ceil(math.sqrt(1.0 + (beta / math.pi) ** 2))) + 3


class Spectrum:
    def __init__(self, samples: np.ndarray, fs: float, beta: float) -> None:
        self.n = samples.size
        self.fs = fs
        self.beta = beta
        w = np.kaiser(self.n, beta)
        self.wpow = float(np.sum(w * w))
        self.x = np.fft.rfft(samples * w)
        self.p = np.abs(self.x) ** 2
        self.freqs = np.fft.rfftfreq(self.n, d=1.0 / fs)
        self.half = lobe_halfwidth(beta)
        self.bin_hz = fs / self.n

    def amp_at_bin(self, k: int) -> float:
        lo = max(k - self.half, 0)
        hi = min(k + self.half + 1, self.p.size)
        return 2.0 * math.sqrt(float(np.sum(self.p[lo:hi])) / (self.n * self.wpow))

    def bin_of(self, f: float) -> int:
        return int(round(f / self.bin_hz))

    def dbfs_at(self, f: float) -> float:
        return db(self.amp_at_bin(self.bin_of(f)) / FULL_SCALE)

    def peak_near(self, f: float, search_hz: float = 120.0) -> tuple[float, float]:
        """Refined (freq, dBFS) of the strongest bin within +-search_hz of f."""
        k0 = self.bin_of(f)
        span = max(int(round(search_hz / self.bin_hz)), 1)
        lo = max(k0 - span, 1)
        hi = min(k0 + span + 1, self.p.size)
        k = lo + int(np.argmax(self.p[lo:hi]))
        return float(self.freqs[k]), db(self.amp_at_bin(k) / FULL_SCALE)

    def mask(self, exclude: list[float], dc_bins: int = 6) -> np.ndarray:
        m = np.ones(self.p.size, dtype=bool)
        m[:dc_bins] = False
        for f in exclude:
            k = self.bin_of(f)
            m[max(k - self.half, 0): min(k + self.half + 1, self.p.size)] = False
        return m

    def top_spurs(self, exclude: list[float], count: int) -> list[tuple[float, float]]:
        m = self.mask(exclude)
        out: list[tuple[float, float]] = []
        p = self.p.copy()
        p[~m] = 0.0
        for _ in range(count):
            k = int(np.argmax(p))
            if p[k] <= 0.0:
                break
            out.append((float(self.freqs[k]), db(self.amp_at_bin(k) / FULL_SCALE)))
            p[max(k - self.half, 0): min(k + self.half + 1, p.size)] = 0.0
        return out

    def floor_dbfs(self, exclude: list[float]) -> tuple[float, float]:
        """(median per-bin level, integrated residual level) in dBFS."""
        m = self.mask(exclude)
        per_bin = 2.0 * np.sqrt(self.p[m] / (self.n * self.wpow)) / FULL_SCALE
        med = db(float(np.median(per_bin)))
        # Integrated: total residual power expressed as an equivalent sine amplitude.
        integ = db(2.0 * math.sqrt(float(np.sum(self.p[m])) / (self.n * self.wpow)) / FULL_SCALE)
        return med, integ


def parse(path: pathlib.Path) -> tuple[dict[str, str], np.ndarray]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    begin = next((l for l in lines if "*MEAS_BEGIN" in l), None)
    if begin is None:
        raise ValueError(f"{path}: no *MEAS_BEGIN")
    meta = dict(re.findall(r"([A-Za-z0-9_]+)=(\S+)", begin))
    vals = [int(l.strip()) for l in lines if re.fullmatch(r"-?\d+", l.strip())]
    samples = np.asarray(vals, dtype=np.float64)
    if "n" in meta and samples.size != int(meta["n"]):
        raise ValueError(f"{path}: {samples.size} samples but header says n={meta['n']}")
    return meta, samples


def selftest() -> int:
    rng = np.random.default_rng(7)
    n, fs, beta = 2048, 32000.0, 24.0
    ok = True
    for f, dbfs in ((1000.0, -6.0), (15000.0, -6.0), (8100.4, -20.0), (14750.0, -100.0)):
        a = FULL_SCALE * 10.0 ** (dbfs / 20.0)
        t = np.arange(n) / fs
        x = a * np.sin(2.0 * np.pi * f * t + rng.uniform(0, 6.28))
        got = Spectrum(x, fs, beta).dbfs_at(f)
        good = abs(got - dbfs) < 0.02
        ok &= good
        print("  amp  f=%9.1f Hz  want %8.2f dBFS  got %8.2f  %s"
              % (f, dbfs, got, "ok" if good else "FAIL"))
    for fin, want in ((17000.0, 15000.0), (18000.0, 14000.0), (23900.0, 8100.0),
                      (12000.0, 12000.0), (16000.0, 16000.0)):
        got = fold(fin, 32000.0)
        good = abs(got - want) < 1e-9
        ok &= good
        print("  fold %9.1f -> %9.1f (want %9.1f)  %s"
              % (fin, got, want, "ok" if good else "FAIL"))
    print("selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("captures", nargs="*", type=pathlib.Path)
    ap.add_argument("--beta", type=float, default=24.0)
    ap.add_argument("--fs", type=float, default=None,
                    help="override capture fs_out (Hz); default = header fs_out_hz")
    ap.add_argument("--ref-dbfs", type=float, default=None,
                    help="MEASURED output level (dBFS) of an in-passband tone at the same input "
                         "amplitude.  Use it as the 0 dB transfer reference so rel_dB cancels any "
                         "fixed path gain (this build's front-end push has a 2^-8 one -- see the "
                         "report).  Default: the header's nominal level_dbfs.")
    ap.add_argument("--ref-in-dbfs", type=float, default=None,
                    help="input amplitude (dBFS) the --ref-dbfs reference was taken at")
    ap.add_argument("--spurs", type=int, default=3)
    ap.add_argument("--csv", type=pathlib.Path, default=None)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.captures:
        ap.error("give at least one capture, or --selftest")

    rows: list[list[str]] = []
    print("%-24s %8s %8s %9s %9s %8s %8s %8s %8s %7s  %s"
          % ("file", "in_Hz", "in_dBFS", "exp_out", "meas_out", "dBFS", "rel_dB",
             "flr/bin", "flr_int", "pk_lsb", "top other spurs (Hz @ rel_dB)"))
    for path in args.captures:
        meta, samples = parse(path)
        fs = args.fs if args.fs else float(meta["fs_out_hz"])
        f_in = float(meta["tone_hz"])
        in_dbfs = float(meta["level_dbfs"])
        ref = args.ref_dbfs if args.ref_dbfs is not None else in_dbfs
        # A reference taken at one input amplitude carries that amplitude in it, so shift it to
        # this capture's input amplitude before subtracting -- otherwise an amplitude sweep would
        # read as if the rejection changed with level.
        ref = ref + (in_dbfs - (args.ref_in_dbfs if args.ref_in_dbfs is not None else in_dbfs))
        sp = Spectrum(samples, fs, args.beta)
        f_exp = fold(f_in, fs)
        f_meas, dbfs = sp.peak_near(f_exp)
        med, integ = sp.floor_dbfs([f_exp])
        spurs = sp.top_spurs([f_exp], args.spurs)
        print("%-24s %8.0f %8.1f %8.1f %9.1f %8.2f %8.2f %8.1f %8.1f %7d  %s"
              % (path.name, f_in, in_dbfs, f_exp, f_meas, dbfs, dbfs - ref, med - ref,
                 integ - ref, int(np.max(np.abs(samples))),
                 ", ".join("%.0f@%.1f" % (f, l - ref) for f, l in spurs)))
        rows.append(["%.0f" % f_in, "%.1f" % in_dbfs, "%.1f" % f_exp, "%.1f" % f_meas,
                     "%.2f" % dbfs, "%.2f" % (dbfs - ref), "%.1f" % (med - ref),
                     "%.1f" % (integ - ref), path.name])
    if args.csv:
        with args.csv.open("w", encoding="utf-8", newline="") as fh:
            fh.write("in_hz,in_dbfs,expected_out_hz,measured_out_hz,level_dbfs,rel_db,"
                     "floor_per_bin_db,floor_integrated_db,file\n")
            for r in rows:
                fh.write(",".join(r) + "\n")
        print("csv: %s" % args.csv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
