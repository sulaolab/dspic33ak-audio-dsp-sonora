"""Step 19: the starter motor -- measure it, then design it as a sample.

The starter is not an engine cycle and has no RPM parameter: it is a fixed event
that starts, holds for as long as the key is held, and stops.  So the wavetable
model does not apply, and storing the recording is not a compromise -- it is the
right structure.  The only real questions are engineering ones:

  * how little bandwidth does it need?          -> decimation rate
  * can it loop, so the duration is free?       -> loop-point search
  * what does that cost in ROM?                 -> raw16 / raw12 / IMA-ADPCM4
  * does it still sound right after all that?   -> render exactly what the MCU
                                                  would output and listen

Outputs in out/:
  starter_orig.wav                  the source, for reference
  starter_<rate>k_<codec>.wav       decoded exactly as the MCU would play it,
                                    resampled to 48 kHz only for auditioning
  starter_loop_demo.wav             attack + loop x N + release, 3 crank lengths
"""
import os
import sys

import numpy as np
from scipy import signal, ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402

SRC = os.path.join(source_dir(), "astm_v8_cell_motor.wav")
FALLBACK = os.path.join(source_dir(), "astm_v8_cell_motor+idling.wav")
OUT = os.path.join(HERE, "out")
RATES = [4000, 6000, 8000, 12000]

# ---------------------------------------------------------------- IMA ADPCM
STEP_TAB = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
            41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
            190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
            724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
            2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
            6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
            16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IDX_TAB = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_roundtrip(x16):
    """Encode with IMA ADPCM and decode again -- returns the decoded signal.
    Used to audition the actual quantisation, not to estimate it."""
    pred, idx = 0, 0
    out = np.empty(len(x16), dtype=np.int32)
    for i, s in enumerate(x16):
        step = STEP_TAB[idx]
        diff = int(s) - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        # decode the same way the player will
        d = step >> 3
        if code & 4:
            d += step
        if code & 2:
            d += step >> 1
        if code & 1:
            d += step >> 2
        pred = pred - d if code & 8 else pred + d
        pred = int(np.clip(pred, -32768, 32767))
        idx = int(np.clip(idx + IDX_TAB[code & 7], 0, 88))
        out[i] = pred
    return out.astype(np.float64) / 32768.0


# ---------------------------------------------------------------- helpers
def resample_to(x, fs_in, fs_out):
    g = np.gcd(int(fs_in), int(fs_out))
    return signal.resample_poly(x, fs_out // g, fs_in // g)


def band_err(a, b, fs, edges):
    def bands(s):
        F = np.abs(np.fft.rfft(s * np.hanning(len(s)))) ** 2
        f = np.fft.rfftfreq(len(s), 1.0 / fs)
        return np.array([10 * np.log10(F[(f >= lo) & (f < hi)].sum() + 1e-20)
                         for lo, hi in zip(edges[:-1], edges[1:])])
    return bands(b) - bands(a)


def main():
    src = SRC if os.path.exists(SRC) else FALLBACK
    x, fs = read_wav(src)
    m = x.mean(axis=1)
    m -= m.mean()
    if src == FALLBACK:
        m = m[:int(0.70 * fs)]
        print("(dedicated starter cut not found; using the first 0.70 s of the"
              " combined file)")
    print("source: %s" % os.path.basename(src))
    print("  %.3f s @ %d Hz, peak %.1f dBFS, rms %.1f dBFS"
          % (len(m) / fs, fs, 20 * np.log10(np.abs(m).max()),
             20 * np.log10(m.std())))
    write_wav(os.path.join(OUT, "starter_orig.wav"),
              m / (np.abs(m).max() + 1e-9) * 0.9, fs, 3)

    # ---- bandwidth ----
    F = np.abs(np.fft.rfft(m * np.hanning(len(m)))) ** 2
    f = np.fft.rfftfreq(len(m), 1.0 / fs)
    cum = np.cumsum(F) / F.sum()
    print("\n--- bandwidth ---")
    for q in (0.90, 0.95, 0.99, 0.999):
        print("   %5.1f %% of energy below %6.0f Hz" % (100 * q, f[np.searchsorted(cum, q)]))

    # ---- periodicity: cranking rate ----
    env = ndimage.uniform_filter1d(np.abs(m), int(0.002 * fs))
    env = env - env.mean()
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    ac /= ac[0] + 1e-15
    print("\n--- envelope periodicity (cranking) ---")
    print("   lag[s]     r    rate[Hz]   as V8 compression events -> crank rpm")
    shown = []
    cand = sorted(((ac[i], i) for i in range(int(0.01 * fs), int(0.35 * fs) - 1)
                   if ac[i] > ac[i - 1] and ac[i] >= ac[i + 1] and ac[i] > 0.10),
                  reverse=True)
    for r, i in cand[:10]:
        lag = i / fs
        if any(abs(lag - s) < 0.006 for s in shown):
            continue
        shown.append(lag)
        print("   %.4f  %5.2f   %7.2f      %7.0f rpm" % (lag, r, 1 / lag, 15.0 / lag))

    # ---- loop point search ----
    print("\n--- loop-point search (seamless loop for a held key) ---")
    best = None
    n = len(m)
    for L in np.arange(0.08, 0.30, 0.002):
        Ls = int(L * fs)
        for st in np.arange(0.15, max(0.16, len(m) / fs - L - 0.10), 0.01):
            s0 = int(st * fs)
            if s0 + 2 * Ls > n:
                continue
            a = m[s0:s0 + Ls]
            b = m[s0 + Ls:s0 + 2 * Ls]
            d = np.corrcoef(a, b)[0, 1]
            if best is None or d > best[0]:
                best = (d, st, L)
    print("   best: loop start %.3f s, length %.3f s, cycle-to-cycle corr %.2f"
          % (best[1], best[2], best[0]))
    if best[0] < 0.4:
        print("   NOTE: correlation is low, so a short loop will be audible as a"
              " loop; prefer storing the whole crank or a longer loop.")

    # ---- rate / codec sweep ----
    print("\n--- decimation and codec, with ROM cost for the whole %.2f s ---"
          % (len(m) / fs))
    edges = np.array([100, 300, 700, 1500, 2500, 4000, 8000, 20000], float)
    print("   rate  codec      ROM[kB]   band error vs original [dB]"
          "  (100-300,300-700,700-1.5k,1.5-2.5k,2.5-4k)")
    for rate in RATES:
        lo = resample_to(m, fs, rate)
        # normalise before quantising, the way a real asset pipeline would, and
        # undo exactly that gain before comparing -- otherwise every band shows
        # the same +18 dB and the numbers say nothing about the codec
        g = 0.9 / (np.abs(lo).max() + 1e-12)
        lo = lo * g
        for codec in ("raw16", "adpcm4"):
            if codec == "raw16":
                dec = np.round(lo * 32767).astype(np.int16).astype(np.float64) / 32768.0
                rom = len(lo) * 2
            else:
                dec = adpcm_roundtrip(np.round(lo * 32767).astype(np.int16))
                rom = len(lo) // 2
            up = resample_to(dec / g, rate, fs)
            k = min(len(up), len(m))
            e = band_err(m[:k], up[:k], fs, edges)[:5]
            print("  %5d  %-8s  %7.1f    %s"
                  % (rate, codec, rom / 1024.0,
                     " ".join("%+6.1f" % v for v in e)))
            write_wav(os.path.join(OUT, "starter_%dk_%s.wav" % (rate // 1000, codec)),
                      up / (np.abs(up).max() + 1e-9) * 0.9, fs, 3)

    # ---- loop demo at the recommended setting ----
    rate, codec = 8000, "adpcm4"
    lo = resample_to(m, fs, rate)
    lo = lo / (np.abs(lo).max() + 1e-12) * 0.9
    dec = adpcm_roundtrip(np.round(lo * 32767).astype(np.int16))
    s0 = int(best[1] * rate)
    Ls = int(best[2] * rate)
    attack = dec[:s0]
    loop = dec[s0:s0 + Ls]
    rel = dec[s0 + Ls:]
    xf = min(len(loop) // 8, int(0.01 * rate))
    w = np.linspace(0, 1, xf)
    pieces = [attack]
    for rep in range(6):
        seg = loop.copy()
        if pieces:
            tail = pieces[-1]
            tail[-xf:] = tail[-xf:] * (1 - w) + seg[:xf] * w
            seg = seg[xf:]
        pieces.append(seg)
    pieces.append(rel)
    held = np.concatenate(pieces)
    out = []
    for reps in (1, 3, 6):
        p = [attack] + [loop] * reps + [rel]
        out.append(np.concatenate(p))
        out.append(np.zeros(int(0.4 * rate)))
    demo = np.concatenate(out + [held])
    up = resample_to(demo, rate, fs)
    write_wav(os.path.join(OUT, "starter_loop_demo.wav"),
              up / (np.abs(up).max() + 1e-9) * 0.9, fs, 3)
    print("\n  starter_loop_demo.wav: %d Hz %s, attack %.0f ms + loop %.0f ms x{1,3,6}"
          " + release %.0f ms, then a cross-faded held version"
          % (rate, codec, 1000 * len(attack) / rate, 1000 * len(loop) / rate,
             1000 * len(rel) / rate))
    print("  ROM for attack+loop+release at %d Hz %s: %.1f kB"
          % (rate, codec, (len(attack) + len(loop) + len(rel)) / 2 / 1024.0))


if __name__ == "__main__":
    main()
