"""Step 1: overview, engine-cycle period detection, half-order spectrum.

astm v8 (4.7 L, 90-deg cross-plane, 4-stroke, 8 cyl):
  one engine cycle = 2 crank revolutions = 8 firings
  order 1   = crank rotation           = rpm/60          [Hz]
  order 0.5 = engine cycle             = rpm/120         [Hz]
  order 4   = firing (main "engine" pitch)
Half-orders (0.5, 1.5, 2.5, 3.5) are what the ear hears as "rumble" on a
cross-plane V8, so the analysis grid must be 0.5-order, not 1.0-order.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

SRC = os.path.join(source_dir(), "astm_stable.wav")
OUT = os.path.join(HERE, "out")


def main():
    x, fs = read_wav(SRC)
    n, ch = x.shape
    m = x.mean(axis=1)
    print("file      : %s" % os.path.basename(SRC))
    print("fs / ch   : %d Hz / %d" % (fs, ch))
    print("length    : %d frames = %.3f s" % (n, n / fs))
    print("peak      : %.4f (%.1f dBFS)" % (np.abs(x).max(), 20 * np.log10(np.abs(x).max())))
    print("rms(mono) : %.4f (%.1f dBFS)" % (m.std(), 20 * np.log10(m.std())))
    print("L/R corr  : %.3f" % np.corrcoef(x[:, 0], x[:, 1])[0, 1])
    print("DC        : %.5f" % m.mean())

    # ---- engine cycle period via autocorrelation of the broadband signal ----
    # high-pass a little to kill DC/rumble bias, then normalised autocorrelation
    y = m - m.mean()
    N = 1 << int(np.ceil(np.log2(2 * len(y))))
    Y = np.fft.rfft(y, N)
    ac = np.fft.irfft(np.abs(Y) ** 2, N)[: len(y)]
    ac /= ac[0]
    lo = int(fs * 0.03)   # 0.03 s -> cycle at 4000 rpm
    hi = int(fs * 0.50)   # 0.50 s -> cycle at  240 rpm
    seg = ac[lo:hi]
    # collect the strongest local maxima
    cand = []
    for i in range(1, len(seg) - 1):
        if seg[i] > seg[i - 1] and seg[i] >= seg[i + 1] and seg[i] > 0.15:
            cand.append((seg[i], lo + i))
    cand.sort(reverse=True)
    print("\n--- autocorrelation peaks (candidate cycle / rev periods) ---")
    print("   lag[s]    r      => f0[Hz]   rpm if order1   rpm if order0.5")
    for r, lag in cand[:12]:
        T = lag / fs
        f = 1.0 / T
        print("   %.5f  %.3f  => %8.3f  %10.1f  %13.1f" % (T, r, f, f * 60.0, f * 120.0))

    # ---- refine: comb-fit the half-order grid over a plausible f_cycle range ----
    NF = 1 << 20
    w = np.hanning(len(m))
    X = np.abs(np.fft.rfft(m * w, NF)) / (np.sum(w) / 2.0)
    f = np.fft.rfftfreq(NF, 1.0 / fs)

    def comb_energy(fc, nord=40):
        """Sum of spectrum magnitude at multiples of fc (= order 0.5 grid)."""
        tot = 0.0
        for k in range(1, nord + 1):
            ft = fc * k
            if ft > 3000:
                break
            i = int(round(ft / (fs / NF)))
            tot += X[max(i - 2, 0):i + 3].max()
        return tot

    best = None
    for fc in np.arange(3.0, 40.0, 0.01):
        e = comb_energy(fc)
        if best is None or e > best[1]:
            best = (fc, e)
    fc0 = best[0]
    # fine search
    for fc in np.arange(fc0 - 0.05, fc0 + 0.05, 0.001):
        e = comb_energy(fc)
        if e > best[1]:
            best = (fc, e)
    fcyc = best[0]
    rpm = fcyc * 120.0
    print("\n--- comb fit ---")
    print("f_cycle (order 0.5) = %.3f Hz  ->  RPM = %.1f  (firing order4 = %.2f Hz)"
          % (fcyc, rpm, fcyc * 8))

    # ---- half-order amplitude table ----
    print("\n--- half-order content (order, freq Hz, dB rel. peak order) ---")
    rows = []
    for k in range(1, 81):           # order 0.5 .. 40
        order = 0.5 * k
        ft = fcyc * k
        if ft > 4000:
            break
        i = int(round(ft / (fs / NF)))
        amp = X[max(i - 3, 0):i + 4].max()
        rows.append((order, ft, amp))
    ref = max(r[2] for r in rows)
    for order, ft, amp in rows:
        db = 20 * np.log10(amp / ref + 1e-12)
        half = "" if abs(order - round(order)) < 1e-6 else "  <half>"
        bar = "#" * max(0, int(40 + db * 0.8))
        print("  %5.1f  %8.1f  %6.1f  %s%s" % (order, ft, db, bar, half))

    os.makedirs(OUT, exist_ok=True)
    np.save(os.path.join(OUT, "stable_mono.npy"), m)
    with open(os.path.join(OUT, "stable_fcyc.txt"), "w") as fp:
        fp.write("%.6f\n%.6f\n" % (fcyc, fs))


if __name__ == "__main__":
    main()
