"""Step 13: measure an idle table and add it to the locked table set.

Driving the 3375-6875 rpm tables at idle speed does not work: every feature in
them is order-locked, so the measured 1.0-1.4 kHz noise band lands near 280 Hz
at 900 rpm.  Measured against the real idle that is -15 to -29 dB from 630 Hz up
and +5 to +16 dB below 80 Hz (see a12).  Idle needs its own measurement.

Idle is the one easy case: it is steady, so there is no tracking problem at all.
The only unknown is the speed, and the sharpest test for a *wavetable* is not a
spectral one -- it is simply "which speed makes consecutive cycles line up".
So sweep the speed and maximise the cycle-locked share after averaging; that is
the same quantity the table's quality is judged by later.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, write_wav, source_dir  # noqa: E402
from a12_idle_check import bands  # noqa: E402

IDLE = os.path.join(source_dir(), "astm_v8_idling.wav")
OUT = os.path.join(HERE, "out")
M = 2048                     # same grid as the locked tables
ENV_BINS = 128


def cycles_at(seg, fs, rpm, m_pts=M):
    fc = rpm / 120.0
    ncyc = int(len(seg) / fs * fc)
    if ncyc < 6:
        return None
    idx = np.arange(ncyc * m_pts) / (m_pts * fc) * fs
    ang = np.interp(idx, np.arange(len(seg)), seg)
    return ang.reshape(ncyc, m_pts)


def coherent_share(seg, fs, rpm, m_pts=512):
    blk = cycles_at(seg, fs, rpm, m_pts)
    if blk is None:
        return -1.0
    bn = blk / (blk.std(axis=1, keepdims=True) + 1e-15)
    avg = bn.mean(axis=0)
    coh = avg.std() ** 2
    inc = (bn - avg).std() ** 2
    return coh / (coh + inc)


def main():
    x, fs = read_wav(IDLE)
    ref = x.mean(axis=1)
    ref -= ref.mean()
    print("idle reference: %.2f s, rms %.1f dBFS" % (len(ref) / fs, 20 * np.log10(ref.std())))

    print("\n--- speed sweep by cycle-locked share (the wavetable's own criterion) ---")
    sw = [(coherent_share(ref, fs, r), r) for r in np.arange(600.0, 1400.0, 1.0)]
    sw.sort(reverse=True)
    top = []
    for s, r in sw:
        if any(abs(r - rr) < 20 for _, rr in top):
            continue
        top.append((s, r))
        if len(top) >= 6:
            break
    for s, r in top:
        print("   %6.0f rpm   coherent %5.1f %%   (order4 = %5.1f Hz)"
              % (r, 100 * s, r * 4 / 60.0))
    rpm = top[0][1]
    fine = [(coherent_share(ref, fs, r), r) for r in np.arange(rpm - 5, rpm + 5, 0.1)]
    fine.sort(reverse=True)
    rpm = fine[0][1]
    print("  -> idle = %.1f rpm, coherent %.1f %% (margin over 2nd family %.1f pt)"
          % (rpm, 100 * fine[0][0], 100 * (top[0][0] - top[1][0])))

    # ---- build the bin ----
    blk = cycles_at(ref, fs, rpm)
    scale = blk.std(axis=1).mean()
    bn = blk / scale
    avg = bn.mean(axis=0)
    # one refinement pass, same as a08
    lim = M // 20
    for _ in range(2):
        W = np.fft.rfft(bn, axis=1)
        W[:, 25:] = 0.0
        lp = np.fft.irfft(W, M, axis=1)
        lp /= lp.std(axis=1, keepdims=True) + 1e-15
        A = np.fft.rfft(avg)
        A[25:] = 0.0
        a = np.fft.irfft(A, M)
        a /= a.std() + 1e-15
        cc = np.fft.irfft(np.fft.rfft(lp, axis=1) * np.conj(np.fft.rfft(a))[None, :], M, axis=1)
        cand = np.concatenate([np.arange(0, lim + 1), np.arange(M - lim, M)])
        sh = cand[np.argmax(cc[:, cand], axis=1)]
        bn = np.array([np.roll(bn[i], -int(sh[i])) for i in range(len(bn))])
        avg = bn.mean(axis=0)
    res = bn - avg
    coh = avg.std() ** 2
    inc = res.std() ** 2
    print("  after refinement: %d cycles, coherent %.1f %%"
          % (len(bn), 100 * coh / (coh + inc)))

    nspec = np.abs(np.fft.rfft(res, axis=1)).mean(axis=0) / (M / 2)
    nenv = np.sqrt((res ** 2).mean(axis=0)).reshape(ENV_BINS, -1).mean(axis=1)
    r = avg.std()
    wave = avg / r
    gain = scale * r
    nrms = res.std() / r

    # ---- append to the locked set ----
    d = np.load(os.path.join(OUT, "tables_v1.npz"))
    order = np.argsort(np.concatenate([[rpm], d["rpm"]]))
    out = dict(
        rpm=np.concatenate([[rpm], d["rpm"]])[order],
        wave=np.concatenate([wave[None, :], d["wave"]])[order],
        gain=np.concatenate([[gain], d["gain"]])[order],
        noise_rms=np.concatenate([[nrms], d["noise_rms"]])[order],
        noise_spec=np.concatenate([nspec[None, :], d["noise_spec"]])[order],
        noise_env=np.concatenate([nenv[None, :], d["noise_env"]])[order],
        ncyc=np.concatenate([[len(bn)], d["ncyc"]])[order],
        M=d["M"], env_bins=d["env_bins"], fs=d["fs"])
    np.savez(os.path.join(OUT, "tables_v2.npz"), **out)
    print("\nwrote out/tables_v2.npz: %d bins, %.0f .. %.0f rpm"
          % (len(out["rpm"]), out["rpm"][0], out["rpm"][-1]))

    # ---- verify: resynthesise idle from the new table ----
    from a09_resynth import EngineTables, synth
    tab = EngineTables(os.path.join(OUT, "tables_v2.npz"))
    dur = len(ref) / fs
    y, _, _ = synth(np.array([rpm, rpm]), np.array([0.0, dur]), tab, fs, jitter=0.006)
    y = y[:len(ref)] if len(y) >= len(ref) else np.pad(y, (0, len(ref) - len(y)))
    y *= ref.std() / (y.std() + 1e-15)
    write_wav(os.path.join(OUT, "idle_syn_table.wav"),
              y / (np.abs(y).max() + 1e-9) * 0.9, fs, 3)

    edges = np.array([40, 80, 160, 315, 630, 1250, 2500, 5000, 10000, 20000], float)
    br, by = bands(ref, fs, edges), bands(y, fs, edges)
    print("\n  octave-band check against the real idle [dB]")
    hdr = "  %-24s" % "band [Hz]"
    for lo in edges[:-1]:
        hdr += "%7d" % lo
    print(hdr)
    for name, b in (("reference", br), ("synth from idle table", by)):
        print("  %-24s" % name + "".join("%7.1f" % v for v in b))
    print("  %-24s" % "difference" + "".join("%+7.1f" % v for v in (by - br)))
    print("\nwrote out/idle_syn_table.wav")


if __name__ == "__main__":
    main()
