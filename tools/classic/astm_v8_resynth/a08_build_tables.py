"""Step 8: build the measured cycle-wavetable set (architecture A).

For every RPM bin, measure from the recording:
  wave[bin]      one engine cycle, M points, unit RMS   -> the deterministic part
                 (all orders and their phases, half orders included, by
                 construction; nothing is assumed about the harmonic series)
  gain[bin]      overall level of that bin, source-relative
  noise_rms[bin] level of the part that does NOT repeat cycle to cycle
  noise_spec     residual spectrum vs crank order
  noise_env      residual RMS vs crank angle -> is the noise gated per firing?

Segments come from different places in the compilation, so each has its own
arbitrary phase origin and its own recording level.  Both are calibrated out by
chaining segments through their overlapping RPM bins:
  * phase: circular cross-correlation of the low-order (<= 12) part of the
    cycle average -- i.e. align the firing pattern, not the hiss
  * level: median RMS ratio over the shared bins
The widest segment is the reference; every other segment is pulled onto it.
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402
from a07_segments import segments_of  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")

STEM = "astm_v8_vantage_all"     # superset of every other cut
M = 2048                                  # samples per engine cycle
RPM_EDGES = np.arange(3250, 7501, 250)
MIN_CYCLES = 8
ALIGN_MAX_ORDER = 12                      # align on the firing pattern only
ENV_BINS = 128                            # crank-angle resolution of noise env


def angular_cycles(m, fs, t_frames, fc_frames, t0, t1):
    """Resample [t0,t1] onto a whole number of engine cycles, M points each."""
    i0, i1 = int(t0 * fs), min(int(t1 * fs), len(m))
    tt = np.arange(i0, i1) / fs
    fc_s = np.interp(tt, t_frames, fc_frames)
    c = np.cumsum(fc_s) / fs
    c -= c[0]
    ncyc = int(np.floor(c[-1]))
    if ncyc < 2:
        return None, None
    grid = np.arange(0, ncyc, 1.0 / M)
    ts = np.interp(grid, c, tt)
    ang = np.interp(ts, tt, m[i0:i1])
    rpm = np.interp(np.arange(ncyc) + 0.5, c, fc_s) * 120.0
    return ang.reshape(ncyc, M), rpm


def lp_order(w, max_order=ALIGN_MAX_ORDER):
    W = np.fft.rfft(w)
    W[int(max_order * 2) + 1:] = 0.0
    return np.fft.irfft(W, len(w))


def lp_order_rows(W, max_order=ALIGN_MAX_ORDER):
    S = np.fft.rfft(W, axis=1)
    S[:, int(max_order * 2) + 1:] = 0.0
    out = np.fft.irfft(S, W.shape[1], axis=1)
    return out / (out.std(axis=1, keepdims=True) + 1e-15)


def best_shift(ref, cand):
    """Circular shift (samples) that best aligns cand onto ref."""
    a, b = lp_order(ref), lp_order(cand)
    a = a / (a.std() + 1e-15)
    b = b / (b.std() + 1e-15)
    cc = np.fft.irfft(np.fft.rfft(a) * np.conj(np.fft.rfft(b)), len(a))
    return int(np.argmax(cc))


def main():
    tr, segs = segments_of(STEM)
    if tr is None:
        raise SystemExit("run a04 on %s first (--rpm-lo 1400 --rpm-hi 7600)" % STEM)
    x, fs = read_wav(os.path.join(WAVDIR, STEM + ".wav"))
    m = x.mean(axis=1)
    m -= m.mean()

    # ---- per segment: cycles + per-bin raw averages ----
    segs = [s for s in segs if s["rpm_hi"] - s["rpm_lo"] > 40]
    print("using %d locked segments of %s" % (len(segs), STEM))
    nbin = len(RPM_EDGES) - 1
    per_seg = []
    for si, s in enumerate(segs):
        cyc, rpm = angular_cycles(m, fs, tr["t"], tr["fc"], s["t0"], s["t1"])
        if cyc is None:
            continue
        bins = {}
        for bi in range(nbin):
            sel = (rpm >= RPM_EDGES[bi]) & (rpm < RPM_EDGES[bi + 1])
            if sel.sum() < 3:
                continue
            blk = cyc[sel]
            scale = blk.std(axis=1).mean() + 1e-15
            bn = blk / scale
            avg = bn.mean(axis=0)
            # Phase refinement: the RPM track is good but not exact, so the
            # cycles inside a bin drift a little against each other and the
            # average partly cancels itself (one bin measured 34 % coherent
            # where its neighbours were 67 %).  Re-align each cycle to the
            # average on the firing pattern only, within +-36 crank degrees,
            # then average again.  The shift limit is what stops this from
            # simply aligning noise into a fake "coherent" waveform.
            lim = M // 20
            for _ in range(2):
                ref_lp = lp_order(avg)
                ref_lp = ref_lp / (ref_lp.std() + 1e-15)
                R = np.conj(np.fft.rfft(ref_lp))
                B = np.fft.rfft(lp_order_rows(bn), axis=1)
                cc = np.fft.irfft(B * R[None, :], M, axis=1)
                cand = np.concatenate([np.arange(0, lim + 1),
                                       np.arange(M - lim, M)])
                sh = cand[np.argmax(cc[:, cand], axis=1)]
                bn = np.array([np.roll(bn[i], -int(sh[i])) for i in range(len(bn))])
                avg = bn.mean(axis=0)
            res = bn - avg
            bins[bi] = dict(n=int(sel.sum()), avg=avg, scale=float(scale),
                            res_rms=float(res.std()),
                            res_spec=np.abs(np.fft.rfft(res, axis=1)).mean(axis=0) / (M / 2),
                            res_env=np.sqrt((res ** 2).mean(axis=0)))
        if bins:
            per_seg.append(dict(seg=s, bins=bins, span=s["rpm_hi"] - s["rpm_lo"]))

    # ---- calibrate phase + level by chaining through shared bins ----
    per_seg.sort(key=lambda p: -p["span"])
    ref = per_seg[0]
    ref["shift"], ref["gcal"] = 0, 1.0
    done = [ref]
    print("\nreference segment: t=%.2f..%.2f  %.0f-%.0f rpm  (%d bins)"
          % (ref["seg"]["t0"], ref["seg"]["t1"], ref["seg"]["rpm_lo"],
             ref["seg"]["rpm_hi"], len(ref["bins"])))
    pending = per_seg[1:]
    print("\n  calibration (phase shift in crank deg, level in dB):")
    while pending:
        best = None
        for p in pending:
            for d in done:
                shared = set(p["bins"]) & set(d["bins"])
                if shared and (best is None or len(shared) > best[0]):
                    best = (len(shared), p, d, sorted(shared))
        if best is None:
            for p in pending:
                print("   t=%.2f..%.2f : no shared RPM bin, dropped"
                      % (p["seg"]["t0"], p["seg"]["t1"]))
            break
        _, p, d, shared = best
        bi = shared[len(shared) // 2]
        sh = best_shift(d["bins"][bi]["avg"], p["bins"][bi]["avg"])
        sh = (sh + d["shift"]) % M
        ratios = [d["bins"][b]["scale"] * d["gcal"] / p["bins"][b]["scale"]
                  for b in shared]
        g = float(np.median(ratios))
        p["shift"], p["gcal"] = sh, g
        print("   t=%6.2f..%6.2f  %4.0f-%4.0f rpm : shift %6.1f deg, level %+5.1f dB"
              " (%d shared bin%s)"
              % (p["seg"]["t0"], p["seg"]["t1"], p["seg"]["rpm_lo"],
                 p["seg"]["rpm_hi"], 720.0 * sh / M, 20 * np.log10(g),
                 len(shared), "" if len(shared) == 1 else "s"))
        done.append(p)
        pending.remove(p)

    # ---- merge into the table ----
    wave = np.zeros((nbin, M))
    gain = np.zeros(nbin)
    nrms = np.zeros(nbin)
    nspec = np.zeros((nbin, M // 2 + 1))
    nenv = np.zeros((nbin, ENV_BINS))
    ncyc_bin = np.zeros(nbin, dtype=int)
    src = ["-"] * nbin
    # Winner-take-all per RPM bin, not a blend across segments.  Blending
    # relies on the inter-segment phase alignment being exact; where it is a
    # little off, averaging *cancels* the cycle-locked part and the bin comes
    # out as noise (measured: one bin dropped to 33 % coherent, its neighbours
    # were 60-68 %).  One segment per bin cannot do that.
    winner = {}
    for p in done:
        for bi, b in p["bins"].items():
            if bi not in winner or b["n"] > winner[bi][1]["n"]:
                winner[bi] = (p, b)
    for bi, (p, b) in winner.items():
        wave[bi] = np.roll(b["avg"], p["shift"])
        gain[bi] = b["scale"] * p["gcal"]
        nrms[bi] = b["res_rms"]
        nspec[bi] = b["res_spec"]
        nenv[bi] = np.roll(b["res_env"], p["shift"]).reshape(ENV_BINS, -1).mean(axis=1)
        ncyc_bin[bi] = b["n"]
        src[bi] = "%.1f-%.1f s" % (p["seg"]["t0"], p["seg"]["t1"])
    ok = ncyc_bin >= MIN_CYCLES

    centers = 0.5 * (RPM_EDGES[:-1] + RPM_EDGES[1:])
    print("\n--- table ---")
    print("   rpm   cycles  coherent  gain[dBFS]  noise/coh[dB]  noise peak    source")
    print("                  share                              order    Hz")
    for bi in range(nbin):
        if not ok[bi]:
            print("  %5.0f      %3d   -- not enough cycles --" % (centers[bi], ncyc_bin[bi]))
            continue
        w = wave[bi]
        coh = w.std() ** 2
        share = coh / (coh + nrms[bi] ** 2)
        sp = nspec[bi].copy()
        sp[:4] = 0.0
        kpk = int(np.argmax(sp))
        opk = kpk / 2.0
        fpk = opk * centers[bi] / 60.0
        print("  %5.0f      %3d    %5.1f %%    %6.1f      %+6.1f       %5.1f  %6.0f   %s"
              % (centers[bi], ncyc_bin[bi], 100 * share,
                 20 * np.log10(gain[bi] + 1e-15),
                 20 * np.log10(nrms[bi] / (w.std() + 1e-15)), opk, fpk, src[bi]))
    # normalise wave to unit RMS, fold the level into gain
    for bi in np.where(ok)[0]:
        r = wave[bi].std()
        wave[bi] /= r
        gain[bi] *= r
        nrms[bi] /= r

    np.savez(os.path.join(OUT, "tables_v1.npz"),
             rpm=centers[ok], wave=wave[ok], gain=gain[ok], noise_rms=nrms[ok],
             noise_spec=nspec[ok], noise_env=nenv[ok], ncyc=ncyc_bin[ok],
             M=M, env_bins=ENV_BINS, fs=fs)
    print("\nwrote out/tables_v1.npz  (%d usable RPM bins)" % ok.sum())

    # ---- plots ----
    sel = np.where(ok)[0]
    fig, axes = plt.subplots(2, 2, figsize=(15, 9), constrained_layout=True)
    ph = np.arange(M) / M * 720.0
    for bi in sel[::max(1, len(sel) // 6)]:
        axes[0, 0].plot(ph, wave[bi], lw=0.7, label="%.0f rpm" % centers[bi])
        o = np.arange(M // 2 + 1) / 2.0
        axes[0, 1].plot(o, 20 * np.log10(np.abs(np.fft.rfft(wave[bi])) / (M / 2) + 1e-12),
                        lw=0.8, label="%.0f rpm" % centers[bi])
        axes[1, 0].plot(np.arange(ENV_BINS) / ENV_BINS * 720.0,
                        nenv[bi] / (nenv[bi].mean() + 1e-15), lw=0.8)
        axes[1, 1].plot(o, 20 * np.log10(nspec[bi] + 1e-12), lw=0.8)
    for d in range(0, 721, 90):
        axes[0, 0].axvline(d, color="k", lw=0.4, alpha=0.3)
        axes[1, 0].axvline(d, color="k", lw=0.4, alpha=0.3)
    axes[0, 0].set_title("cycle-locked waveform (unit RMS)")
    axes[0, 0].set_xlabel("crank angle [deg]")
    axes[0, 0].legend(fontsize=7)
    axes[0, 1].set_title("its order spectrum")
    axes[0, 1].set_xlim(0, 40)
    axes[0, 1].set_ylim(-60, 5)
    axes[0, 1].set_xlabel("crank order")
    axes[0, 1].grid(alpha=0.3)
    axes[1, 0].set_title("residual-noise envelope vs crank angle (1.0 = flat)")
    axes[1, 0].set_xlabel("crank angle [deg]")
    axes[1, 1].set_title("residual-noise order spectrum")
    axes[1, 1].set_xlim(0, 60)
    axes[1, 1].set_xlabel("crank order")
    axes[1, 1].grid(alpha=0.3)
    out = os.path.join(OUT, "tables_v1.png")
    fig.savefig(out, dpi=85)
    plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    main()
