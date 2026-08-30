"""Step 12: pin the idle speed with a sharp discriminator, then test the
"just run the locked model at ~1000 rpm" idea against the real idle.

Why a new discriminator: at idle the mean-over-all-orders scores used earlier
have no discrimination left (measured spread 0.24-0.43 dB across 554..1482 rpm).
But *order 4 is the firing order of a V8* and at idle it is the loudest thing in
the recording, so instead of scoring the whole comb, score only the integer
firing multiples -- orders 4, 8, 12, 16.  Those are strong, unambiguous, and a
wrong speed puts them nowhere.

Then the practical question: the 3375-6875 rpm model is design-locked. Does
simply advancing its phase at idle speed give a usable idle?  The tables are
RPM-locked in *order*, so every feature -- including the measured 1.0-1.4 kHz
noise band -- scales down with the speed.  Whether that is acceptable is a
listening question; this step produces the files and the numbers for it.
"""
import os
import sys

import numpy as np
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a09_resynth import EngineTables, synth  # noqa: E402

IDLE = os.path.join(source_dir(), "astm_v8_idling.wav")
OUT = os.path.join(HERE, "out")
M = 1024
FIRING_ORDERS = (4, 8, 12, 16)


def firing_score(seg, fs, rpm):
    """Prominence of the firing orders after angular resampling at `rpm`."""
    fc = rpm / 120.0
    ncyc = int(len(seg) / fs * fc)
    if ncyc < 4:
        return -99.0
    idx = np.arange(ncyc * M) / (M * fc) * fs
    ang = np.interp(idx, np.arange(len(seg)), seg)
    S = np.abs(np.fft.rfft(ang.reshape(ncyc, M), axis=1)).mean(axis=0)
    db = 20 * np.log10(S + 1e-12)
    fl = ndimage.median_filter(db, size=15)
    W = db - fl
    # bin k of an M-point cycle FFT is crank order k/2
    return float(np.mean([W[2 * o - 1:2 * o + 2].max() for o in FIRING_ORDERS]))


def bands(sig, fs, edges):
    F = np.fft.rfft(sig * np.hanning(len(sig)))
    f = np.fft.rfftfreq(len(sig), 1.0 / fs)
    p = np.abs(F) ** 2
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        s = p[(f >= lo) & (f < hi)].sum()
        out.append(10 * np.log10(s + 1e-20))
    return np.array(out)


def main():
    x, fs = read_wav(IDLE)
    ref = x.mean(axis=1)
    ref -= ref.mean()
    print("idle reference: %.2f s, rms %.1f dBFS" % (len(ref) / fs, 20 * np.log10(ref.std())))

    print("\n--- firing-order test (orders %s), 1 rpm steps ---"
          % ", ".join(str(o) for o in FIRING_ORDERS))
    sc = [(firing_score(ref, fs, r), r) for r in np.arange(560.0, 1500.0, 1.0)]
    sc.sort(reverse=True)
    top = []
    for s, r in sc:
        if any(abs(r - rr) < 15 for _, rr in top):
            continue
        top.append((s, r))
        if len(top) >= 8:
            break
    for s, r in top:
        print("   %6.0f rpm   %+5.2f dB   (order4 = %5.1f Hz, cycle = %.4f s)"
              % (r, s, r * 4 / 60.0, 120.0 / r))
    idle_rpm = top[0][1]
    fine = [(firing_score(ref, fs, r), r) for r in np.arange(idle_rpm - 4, idle_rpm + 4, 0.1)]
    fine.sort(reverse=True)
    idle_rpm = fine[0][1]
    print("  -> idle = %.1f rpm  (order4 = %.2f Hz, firing interval %.2f ms,"
          " cycle %.1f ms)"
          % (idle_rpm, idle_rpm * 4 / 60.0, 15000.0 / idle_rpm, 120000.0 / idle_rpm))
    print("     second-best is %.0f rpm at %+.2f dB -- margin %.2f dB"
          % (top[1][1], top[1][0], top[0][0] - top[1][0]))

    # ---- run the locked model at idle speeds ----
    tab = EngineTables()
    print("\n--- locked model (%.0f-%.0f rpm tables) driven at idle speed ---"
          % (tab.rpm[0], tab.rpm[-1]))
    dur = len(ref) / fs
    tg = np.array([0.0, dur])
    edges = np.array([40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000], float)
    rows = [("reference (real idle)", ref)]
    for rpm in (idle_rpm, 1000.0):
        y, _, _ = synth(np.array([rpm, rpm]), tg, tab, fs, jitter=0.006)
        y = y[:len(ref)] if len(y) >= len(ref) else np.pad(y, (0, len(ref) - len(y)))
        y *= ref.std() / (y.std() + 1e-15)
        rows.append(("synth @ %.0f rpm" % rpm, y))
        write_wav(os.path.join(OUT, "idle_syn_%.0frpm.wav" % rpm),
                  y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)
    write_wav(os.path.join(OUT, "idle_ref.wav"),
              ref / (np.abs(ref).max() + 1e-9) * 0.9, fs, 3)

    print("\n  octave-band levels, all level-matched to the reference [dB]")
    hdr = "  %-22s" % "band [Hz]"
    for lo, hi in zip(edges[:-1], edges[1:]):
        hdr += "%7s" % ("%d" % lo)
    print(hdr)
    base = None
    for name, sig in rows:
        b = bands(sig, fs, edges)
        if base is None:
            base = b
        line = "  %-22s" % name
        for v in b:
            line += "%7.1f" % v
        print(line)
    print("\n  difference from the reference [dB]")
    for name, sig in rows[1:]:
        b = bands(sig, fs, edges) - base
        line = "  %-22s" % name
        for v in b:
            line += "%+7.1f" % v
        print(line)
    print("\nwrote out/idle_ref.wav, out/idle_syn_%.0frpm.wav, out/idle_syn_1000rpm.wav"
          % idle_rpm)


if __name__ == "__main__":
    main()
