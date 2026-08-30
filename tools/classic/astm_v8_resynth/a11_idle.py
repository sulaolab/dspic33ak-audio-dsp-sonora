"""Step 11: pin down the idle speed.

At idle a comb fit is hopeless: the half-order spacing is ~6 Hz, so the grid is
finer than any usable window's main lobe and every candidate scores the same
(measured: 543 / 572 / 705 / 641 rpm all within 0.007 of each other).

What *is* robust at idle is the amplitude envelope: a V8 fires 4 times per crank
revolution, and at idle each firing is a distinct thump.  So:

  1. band-pass, rectify, smooth -> envelope
  2. autocorrelate -> the firing interval and the engine-cycle interval
  3. for each candidate, angular-resample at constant f_cycle and score the
     order spectrum with the midpoint test: prominence on the half-order grid
     minus prominence at the quarter-order midpoints.  The right speed puts
     energy on the grid and nothing between the teeth; a doubled or halved guess
     cannot do both.
"""
import os
import sys

import numpy as np
from scipy import ndimage, signal

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

SRC = os.path.join(source_dir(), "astm_v8_cell_motor+idling.wav")
T0, T1 = 2.60, 4.27
M = 1024


def envelope(seg, fs, lo=150.0, hi=4000.0):
    b, a = signal.butter(4, [lo / (fs / 2), hi / (fs / 2)], btype="band")
    e = np.abs(signal.filtfilt(b, a, seg))
    return ndimage.uniform_filter1d(e, int(0.0015 * fs))


def order_score(seg, fs, rpm):
    """Angular-resample at constant rpm, then grid-minus-midpoint prominence."""
    fc = rpm / 120.0
    ncyc = int(len(seg) / fs * fc)
    if ncyc < 4:
        return -99.0, None
    idx = np.arange(ncyc * M) / (M * fc) * fs
    ang = np.interp(idx, np.arange(len(seg)), seg)
    blk = ang.reshape(ncyc, M)
    S = np.abs(np.fft.rfft(blk, axis=1)).mean(axis=0) / (M / 2)
    db = 20 * np.log10(S + 1e-12)
    fl = ndimage.median_filter(db, size=9)
    W = np.clip(db - fl, 0.0, 30.0)
    # bin k of an M-point cycle FFT is crank order k/2; half-order grid = every
    # integer k, quarter-order midpoints are not representable, so compare
    # against a shifted grid computed from a 2x-oversampled resample instead:
    # here use the simpler proxy -- odd vs even bins cannot separate, so score
    # grid occupancy against the local floor directly.
    kmax = min(len(W) - 1, 96)
    return float(W[1:kmax].mean()), db[:kmax]


def midpoint_score(seg, fs, rpm):
    """Same idea as a04's discriminator but with the phase grid twice as fine:
    resample at 2*M per cycle so quarter orders land on their own bins."""
    fc = rpm / 120.0
    M2 = 2 * M
    ncyc = int(len(seg) / fs * fc)
    if ncyc < 4:
        return -99.0
    idx = np.arange(ncyc * M2) / (M2 * fc) * fs
    ang = np.interp(idx, np.arange(len(seg)), seg)
    blk = ang.reshape(ncyc, M2)
    S = np.abs(np.fft.rfft(blk, axis=1)).mean(axis=0)
    db = 20 * np.log10(S + 1e-12)
    fl = ndimage.median_filter(db, size=11)
    W = np.clip(db - fl, 0.0, 30.0)
    # bin j of a 2M-point cycle FFT is crank order j/4:
    #   j multiple of 2 -> half orders (the real grid)
    #   j odd           -> quarter orders (must be empty)
    jmax = min(len(W) - 1, 4 * 48)
    j = np.arange(2, jmax)
    grid = W[j[j % 2 == 0]].mean()
    mid = W[j[j % 2 == 1]].mean()
    return float(grid - mid)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else SRC
    whole = len(sys.argv) > 1          # a pre-cut idle file needs no windowing
    x, fs = read_wav(src)
    m = x.mean(axis=1)
    m -= m.mean()
    seg = m if whole else m[int(T0 * fs):int(T1 * fs)]
    print("%s\nidle window %.2f s, rms %.1f dBFS"
          % (os.path.basename(src), len(seg) / fs, 20 * np.log10(seg.std())))

    e = envelope(seg, fs)
    e = e - e.mean()
    ac = np.correlate(e, e, mode="full")[len(e) - 1:]
    ac /= ac[0] + 1e-15
    print("\n--- envelope autocorrelation peaks ---")
    print("   lag[s]     r    1/lag[Hz]   as firing(order4)  as cycle(order0.5)")
    cand = []
    for i in range(int(0.008 * fs), int(0.40 * fs) - 1):
        if ac[i] > ac[i - 1] and ac[i] >= ac[i + 1] and ac[i] > 0.08:
            cand.append((ac[i], i))
    cand.sort(reverse=True)
    shown = []
    for r, i in cand[:14]:
        lag = i / fs
        if any(abs(lag - s) < 0.004 for s in shown):
            continue
        shown.append(lag)
        print("   %.4f  %5.2f   %7.2f       %7.0f rpm        %7.0f rpm"
              % (lag, r, 1 / lag, 15.0 / lag, 120.0 / lag))

    print("\n--- midpoint test over a fine RPM sweep ---")
    print("  (grid = half orders, midpoints = quarter orders; higher is better)")
    best = []
    for rpm in np.arange(520, 1500, 2.0):
        best.append((midpoint_score(seg, fs, rpm), rpm))
    best.sort(reverse=True)
    top = []
    for sc, rpm in best:
        if any(abs(rpm - r) < 25 for _, r in top):
            continue
        top.append((sc, rpm))
        if len(top) >= 8:
            break
    for sc, rpm in top:
        print("   %6.0f rpm   score %+5.2f dB   (order4 = %5.1f Hz, cycle = %.3f s)"
              % (rpm, sc, rpm * 4 / 60.0, 120.0 / rpm))
    win = top[0][1]
    print("\n  best: %.0f rpm" % win)
    # refine
    fine = [(midpoint_score(seg, fs, r), r) for r in np.arange(win - 3, win + 3, 0.25)]
    fine.sort(reverse=True)
    print("  refined: %.2f rpm (score %+.2f dB) -> order4 %.2f Hz, cycle %.4f s"
          % (fine[0][1], fine[0][0], fine[0][1] * 4 / 60.0, 120.0 / fine[0][1]))


if __name__ == "__main__":
    main()
