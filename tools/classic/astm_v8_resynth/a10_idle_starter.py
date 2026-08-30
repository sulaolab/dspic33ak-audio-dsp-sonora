"""Step 10: measure the starter-motor + idle recording.

Two different problems in one 4.3 s file:

  * idle -- steady, so a long window is finally the right tool.  At ~700 rpm the
    half-order spacing is only ~6 Hz, which is why every earlier window length
    was useless here.  Search 500..1800 rpm with a 2 s window.
  * starter motor -- not an engine cycle at all: a DC motor whine plus cranking
    compression pulses at a few hundred rpm.  Measured separately: envelope
    period (cranking rate), spectral peaks (motor whine), and how much bandwidth
    the thing actually occupies -- the last one decides whether a decimated
    sample is cheap enough to just store.
"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

SRC = os.path.join(source_dir(), "astm_v8_cell_motor+idling.wav")


def spectrum(seg, fs, nfft):
    w = np.hanning(len(seg))
    X = np.abs(np.fft.rfft(seg * w, nfft)) / (w.sum() / 2)
    f = np.fft.rfftfreq(nfft, 1.0 / fs)
    return f, X


def peaks(f, X, flo, fhi, med_hz, min_prom):
    db = 20 * np.log10(X + 1e-12)
    sel = (f >= flo) & (f <= fhi)
    fb, dbb = f[sel], db[sel]
    fl = ndimage.median_filter(dbb, size=int(med_hz / (f[1] - f[0])) | 1)
    out = []
    for i in range(2, len(fb) - 2):
        if dbb[i] == max(dbb[i - 2:i + 3]) and dbb[i] - fl[i] > min_prom:
            if out and fb[i] - out[-1][0] < 2.0:
                if dbb[i] > out[-1][1]:
                    out[-1] = (fb[i], dbb[i], dbb[i] - fl[i])
                continue
            out.append((fb[i], dbb[i], dbb[i] - fl[i]))
    return out


def comb_rank(f, X, rpm_lo, rpm_hi, fmax, top=6):
    """Rank candidate engine speeds by explained x density (same criterion as a04)."""
    db = 20 * np.log10(X + 1e-12)
    half = int(round(8.0 / (f[1] - f[0])))
    floor = ndimage.median_filter(db, size=2 * half + 1)
    W = np.clip(db - floor, 0.0, 18.0)
    band = f <= fmax
    total = W[band].sum()
    df = f[1] - f[0]
    res = []
    for rpm in np.arange(rpm_lo, rpm_hi, 1.0):
        fc = rpm / 120.0
        ks = np.arange(1, int(fmax / fc) + 1)
        idx = np.round(ks * fc / df).astype(int)
        idx = idx[idx < len(W)]
        acc = np.maximum.reduce([W[np.clip(idx + d, 0, len(W) - 1)] for d in (-1, 0, 1)])
        s = acc.sum()
        res.append((s * s / (total * len(idx)), rpm))
    res.sort(reverse=True)
    # prune neighbours within 20 rpm
    out = []
    for sc, rpm in res:
        if any(abs(rpm - r) < 20 for _, r in out):
            continue
        out.append((sc, rpm))
        if len(out) >= top:
            break
    return out


def main():
    x, fs = read_wav(SRC)
    m = x.mean(axis=1)
    m -= m.mean()
    print("%s\n  %.3f s @ %d Hz, %d ch, peak %.1f dBFS"
          % (os.path.basename(SRC), len(m) / fs, fs, x.shape[1],
             20 * np.log10(np.abs(x).max())))

    # ---- coarse map: where is the starter, where is the idle ----
    print("\n--- level / centroid map (0.2 s steps) ---")
    print("   t[s]   rms[dBFS]  centroid[Hz]  hf(>2k)/lf(<300) [dB]")
    step = int(0.2 * fs)
    for i in range(0, len(m) - step, step):
        seg = m[i:i + step]
        f, X = spectrum(seg, fs, 1 << 14)
        c = float((f * X).sum() / (X.sum() + 1e-15))
        hf = X[(f > 2000)].sum()
        lf = X[(f > 20) & (f < 300)].sum()
        print("  %5.2f    %6.1f      %7.0f        %+6.1f"
              % (i / fs, 20 * np.log10(seg.std() + 1e-15), c,
                 20 * np.log10(hf / (lf + 1e-15))))

    # ---- idle: steady, long window ----
    for (t0, t1) in [(2.20, 4.25), (3.00, 4.25)]:
        seg = m[int(t0 * fs):int(t1 * fs)]
        f, X = spectrum(seg, fs, 1 << 20)
        print("\n=== idle candidate window t=%.2f..%.2f s (%.2f s) ===" % (t0, t1, t1 - t0))
        print("  top engine-speed candidates (comb over 20..900 Hz):")
        for sc, rpm in comb_rank(f, X, 500, 1800, 900.0):
            print("     %6.0f rpm   score %.3f   (order4 = %5.1f Hz, order0.5 = %4.1f Hz)"
                  % (rpm, sc, rpm * 4 / 60.0, rpm / 120.0))
        pk = peaks(f, X, 20, 320, 25.0, 6.0)
        print("  prominent peaks 20..320 Hz (freq, dB, prominence):")
        for fr, d, p in pk[:22]:
            print("     %7.2f  %6.1f  %5.1f" % (fr, d, p))

    # ---- starter motor ----
    t0, t1 = 0.05, 0.65
    seg = m[int(t0 * fs):int(t1 * fs)]
    print("\n=== starter motor t=%.2f..%.2f s ===" % (t0, t1))
    f, X = spectrum(seg, fs, 1 << 18)
    print("  prominent peaks 40..3000 Hz:")
    for fr, d, p in peaks(f, X, 40, 3000, 60.0, 8.0)[:20]:
        print("     %7.1f  %6.1f  %5.1f" % (fr, d, p))
    # envelope period -> cranking rate
    env = np.abs(seg)
    env = ndimage.uniform_filter1d(env, int(0.002 * fs))
    env -= env.mean()
    ac = np.correlate(env, env, mode="full")[len(env) - 1:]
    ac /= ac[0] + 1e-15
    lo, hi = int(0.02 * fs), int(0.40 * fs)
    j = lo + int(np.argmax(ac[lo:hi]))
    print("  envelope autocorrelation peak: %.4f s (r=%.2f)" % (j / fs, ac[j]))
    print("     -> if that is one firing interval (2 per crank rev on a V8 at"
          " 4 per cycle): %.0f rpm cranking" % (60.0 / (j / fs) / 4.0))
    # bandwidth: cumulative energy vs frequency
    f2, X2 = spectrum(seg, fs, 1 << 16)
    p = X2 ** 2
    cum = np.cumsum(p) / p.sum()
    print("  energy bandwidth of the starter section:")
    for q in (0.90, 0.95, 0.99, 0.999):
        print("     %5.1f %% of energy below %6.0f Hz" % (100 * q, f2[np.searchsorted(cum, q)]))


if __name__ == "__main__":
    main()
