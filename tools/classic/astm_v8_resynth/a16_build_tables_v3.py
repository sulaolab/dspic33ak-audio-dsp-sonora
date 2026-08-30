"""Step 16: rebuild the table set from *both* trackers, all clips pooled, plus a
noise-honest idle bin.

What the first attempt at this step got wrong, and why each fix is here:

1. **Use both trackers as sources.**  The comb tracker (a04) is the trustworthy
   one in the middle of the range but cannot follow fast pulls or low RPM; the
   ridge tracker (a15) follows an 85 ms window and reaches 1936 rpm but loses the
   top end (max 6348 rpm where the comb reaches 7102).  Taking only the ridge
   *shrank* coverage to 3625-6125.  Both are offered as phase sources and the
   winner is picked per RPM bin.

2. **Pick the winner by coherence, not by cycle count.**  Selecting the segment
   with the most cycles let a poorly-phased segment win a bin that a better one
   also covered: bins measured 16-24 % coherent where the previous build had
   51-70 %.  Coherent share is exactly what the wavetable's quality is, so it is
   what the choice is made on (subject to a minimum cycle count).

3. **Chain the calibration by RPM adjacency, not only by shared bins.**  Segments
   that only cover the low range shared no bin with the reference, so all of them
   were dropped and every bin below 3375 rpm came out empty -- the exact range
   this rebuild exists to add.  Now a segment may also be aligned to a
   neighbouring bin up to MAX_CHAIN_GAP bins away.

Idle is measured from the dedicated recording, but that recording is ~90 % noise
(8-12 % cycle-locked, against 55-70 % in the pulls), so it is used only for what
it can actually support: the low orders that pass an SNR gate.  Everything above
IDLE_MEASURED_MAX_ORDER, and the noise level and shape, come from the lowest
reliable pull bin.  The printout states which part came from where.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wavio import read_wav, source_dir  # noqa: E402

WAVDIR = source_dir()
OUT = os.path.join(HERE, "out")

SOURCES = ["astm_v8_vantage_all", "astm_tmp_001", "astm_accl_001",
           "astm_stable"]
IDLE_WAV = os.path.join(source_dir(), "astm_v8_idling.wav")

M = 2048
ENV_BINS = 128
RPM_EDGES = np.arange(2000, 7251, 250)
KINDS = {"comb": ("track_%s.npz", 0.70), "ridge": ("track_%s_ridge.npz", 9.0)}
MIN_SEG_S = 0.25
MIN_CYCLES = 8
ALIGN_MAX_ORDER = 12
REFINE_LIMIT_FRAC = 0.05      # +-5 % of a cycle = +-36 crank degrees
MAX_CHAIN_GAP = 2             # bins

IDLE_RPM = 750.0              # design idle speed (the recording cannot pin it)
TREND_BINS = 6                # lowest measured bins used for the idle extrapolation
EXTRAP_CLAMP_DB = 12.0        # limit on how far the extrapolation may move an order


def lp_rows(W, max_order=ALIGN_MAX_ORDER):
    S = np.fft.rfft(W, axis=1)
    S[:, int(max_order * 2) + 1:] = 0.0
    o = np.fft.irfft(S, W.shape[1], axis=1)
    return o / (o.std(axis=1, keepdims=True) + 1e-15)


def refine(bn, m_pts):
    avg = bn.mean(axis=0)
    lim = int(m_pts * REFINE_LIMIT_FRAC)
    cand = np.concatenate([np.arange(0, lim + 1), np.arange(m_pts - lim, m_pts)])
    for _ in range(2):
        a = lp_rows(avg[None, :])[0]
        cc = np.fft.irfft(np.fft.rfft(lp_rows(bn), axis=1)
                          * np.conj(np.fft.rfft(a))[None, :], m_pts, axis=1)
        sh = cand[np.argmax(cc[:, cand], axis=1)]
        bn = np.array([np.roll(bn[i], -int(sh[i])) for i in range(len(bn))])
        avg = bn.mean(axis=0)
    return bn, avg


def best_shift(ref, cand, m_pts):
    a = lp_rows(ref[None, :])[0]
    b = lp_rows(cand[None, :])[0]
    cc = np.fft.irfft(np.fft.rfft(b) * np.conj(np.fft.rfft(a)), m_pts)
    return int(np.argmax(cc))


def runs_of(t, rpm, score, thr):
    ok = (score >= thr) & (rpm >= RPM_EDGES[0]) & (rpm < RPM_EDGES[-1])
    out, i = [], 0
    while i < len(ok):
        if not ok[i]:
            i += 1
            continue
        j = i
        while j + 1 < len(ok) and ok[j + 1]:
            j += 1
        if (t[j] - t[i]) >= MIN_SEG_S:
            out.append((t[i], t[j]))
        i = j + 1
    return out


def angular(m, fs, t_fr, fc_fr, t0, t1):
    i0, i1 = int(t0 * fs), min(int(t1 * fs), len(m))
    tt = np.arange(i0, i1) / fs
    fc_s = np.interp(tt, t_fr, fc_fr)
    c = np.cumsum(fc_s) / fs
    c -= c[0]
    ncyc = int(np.floor(c[-1]))
    if ncyc < 2:
        return None, None
    grid = np.arange(0, ncyc, 1.0 / M)
    ang = np.interp(np.interp(grid, c, tt), tt, m[i0:i1])
    rpm = np.interp(np.arange(ncyc) + 0.5, c, fc_s) * 120.0
    return ang.reshape(ncyc, M), rpm


def measure_bins(m, fs, tr, t0, t1, nbin):
    cyc, rpm = angular(m, fs, tr["t"], tr["fc"], t0, t1)
    if cyc is None:
        return {}
    bins = {}
    for bi in range(nbin):
        sel = (rpm >= RPM_EDGES[bi]) & (rpm < RPM_EDGES[bi + 1])
        if sel.sum() < 4:
            continue
        blk = cyc[sel]
        scale = blk.std(axis=1).mean() + 1e-15
        bn, avg = refine(blk / scale, M)
        res = bn - avg
        coh = avg.std() ** 2
        inc = res.std() ** 2
        bins[bi] = dict(n=int(sel.sum()), avg=avg, scale=float(scale),
                        res_rms=float(res.std()),
                        share=float(coh / (coh + inc)),
                        res_spec=np.abs(np.fft.rfft(res, axis=1)).mean(axis=0) / (M / 2),
                        res_env=np.sqrt((res ** 2).mean(axis=0)))
    return bins


def main():
    nbin = len(RPM_EDGES) - 1
    pools = []
    for stem in SOURCES:
        x, fs = read_wav(os.path.join(WAVDIR, stem + ".wav"))
        m = x.mean(axis=1)
        m -= m.mean()
        for kind, (pat, thr) in KINDS.items():
            p = os.path.join(OUT, pat % stem)
            if not os.path.exists(p):
                print("  (missing %s)" % os.path.basename(p))
                continue
            tr = np.load(p)
            segs = runs_of(tr["t"], tr["rpm"], tr["score"], thr)
            nb = 0
            for (t0, t1) in segs:
                bins = measure_bins(m, fs, tr, t0, t1, nbin)
                if bins:
                    pools.append(dict(stem=stem, kind=kind, t0=t0, t1=t1, bins=bins,
                                      span=max(bins) - min(bins)))
                    nb += len(bins)
            print("%-30s %-5s %3d segments, %3d bin-measurements"
                  % (stem, kind, len(segs), nb))
    if not pools:
        raise SystemExit("no usable segments")

    # ---- calibrate phase + level; chain by shared bins, else by adjacency ----
    pools.sort(key=lambda p: -(p["span"] * 100 + len(p["bins"])))
    ref = pools[0]
    ref["shift"], ref["gcal"] = 0, 1.0
    done, pending = [ref], pools[1:]
    print("\nreference: %s [%s] t=%.2f..%.2f, %d bins"
          % (ref["stem"], ref["kind"], ref["t0"], ref["t1"], len(ref["bins"])))
    n_adj = 0
    while pending:
        best = None                      # (priority, nshared, p, d, bi_p, bi_d)
        for p in pending:
            for d in done:
                shared = set(p["bins"]) & set(d["bins"])
                if shared:
                    bi = sorted(shared)[len(shared) // 2]
                    cand = (2, len(shared), p, d, bi, bi)
                else:
                    gap, pair = None, None
                    for bp in p["bins"]:
                        for bd in d["bins"]:
                            g = abs(bp - bd)
                            if g <= MAX_CHAIN_GAP and (gap is None or g < gap):
                                gap, pair = g, (bp, bd)
                    if pair is None:
                        continue
                    cand = (1, -gap, p, d, pair[0], pair[1])
                if best is None or cand[:2] > best[:2]:
                    best = cand
        if best is None:
            print("  dropped %d segments: no shared or adjacent bin" % len(pending))
            break
        pri, _, p, d, bi_p, bi_d = best
        p["shift"] = (best_shift(d["bins"][bi_d]["avg"], p["bins"][bi_p]["avg"], M)
                      + d["shift"]) % M
        shared = set(p["bins"]) & set(d["bins"])
        if shared:
            p["gcal"] = float(np.median([d["bins"][b]["scale"] * d["gcal"]
                                         / p["bins"][b]["scale"] for b in shared]))
        else:
            n_adj += 1
            p["gcal"] = (d["bins"][bi_d]["scale"] * d["gcal"]
                         / p["bins"][bi_p]["scale"])
        done.append(p)
        pending.remove(p)
    print("calibrated %d segments (%d of them via an adjacent bin)" % (len(done), n_adj))

    # ---- winner per bin = highest coherent share ----
    win = {}
    for p in done:
        for bi, b in p["bins"].items():
            if b["n"] < MIN_CYCLES:
                continue
            if bi not in win or b["share"] > win[bi][1]["share"]:
                win[bi] = (p, b)

    centers = 0.5 * (RPM_EDGES[:-1] + RPM_EDGES[1:])
    rows = []
    print("\n--- pull bins (winner = best coherent share, >= %d cycles) ---" % MIN_CYCLES)
    print("   rpm  cycles  coherent  noise/coh[dB]  track  source")
    for bi in range(nbin):
        if bi not in win:
            print("  %5.0f       -   -- no bin --" % centers[bi])
            continue
        p, b = win[bi]
        w = np.roll(b["avg"], p["shift"])
        r = w.std()
        print("  %5.0f     %3d    %5.1f %%    %+6.1f      %-5s  %s %.1f-%.1f s"
              % (centers[bi], b["n"], 100 * b["share"],
                 20 * np.log10(b["res_rms"] / (r + 1e-15)), p["kind"],
                 p["stem"][:16], p["t0"], p["t1"]))
        rows.append(dict(rpm=centers[bi], wave=w / r,
                         gain=b["scale"] * p["gcal"] * r,
                         nrms=b["res_rms"] / r, nspec=b["res_spec"],
                         nenv=np.roll(b["res_env"], p["shift"]).reshape(ENV_BINS, -1).mean(axis=1),
                         ncyc=b["n"],
                         src="%s[%s] %.1f-%.1f s" % (p["stem"][:16], p["kind"], p["t0"], p["t1"])))

    # ---- idle bin: extrapolated, NOT measured ----
    #
    # The idle recording cannot support a measurement.  It is 2.7 s long (13-23
    # engine cycles depending on the assumed speed) and only 5-11 % of its energy
    # is cycle-locked, so: the speed is not identifiable (every candidate from
    # 600 to 1037 rpm lands within 6 pt of the best), and the per-order SNR of
    # the cycle average swings by +-17 dB at random between neighbouring orders.
    # Order 4 -- which must dominate a V8 idle -- does not even survive an SNR
    # gate.  Building a table from that would be fitting the recording's hiss.
    #
    # So idle is produced from the *measured* trend instead: fit log-amplitude
    # against log-rpm per order over the lowest TREND_BINS measured bins and
    # evaluate it at IDLE_RPM, keeping the phases of the lowest bin.  That is a
    # model extrapolation, it is labelled as one, and it has to be judged by ear.
    print("\n--- idle bin (EXTRAPOLATED from the measured trend, not measured) ---")
    trend = rows[:TREND_BINS]
    X = np.log10([r_["rpm"] for r_ in trend])
    S = np.array([np.abs(np.fft.rfft(r_["wave"])) for r_ in trend])
    def smooth_log(sp):
        """Smooth a log spectrum over order with a proportional-width window, so
        the trend fit sees the spectral *envelope* and not the line-to-line
        scatter between bins taken from different parts of the recording.  Fitting
        per order does not work: it produced tilts of -42 dB/octave at order 8 and
        +31 dB/octave at order 16, i.e. it was fitting that scatter."""
        y = np.log10(sp + 1e-12)
        out = np.empty_like(y)
        for k in range(len(y)):
            h = max(3, int(0.25 * k))
            out[k] = y[max(0, k - h):k + h + 1].mean()
        return out

    Ys = np.array([smooth_log(s) for s in S])
    xc = X - X.mean()
    slope = (xc[:, None] * (Ys - Ys.mean(axis=0))).sum(axis=0) / (xc ** 2).sum()
    # Apply only that smooth tilt to the lowest measured bin, keeping its fine
    # structure -- the firing pattern -- and its phases exactly as measured.
    tilt_db = np.clip(20 * slope * (np.log10(IDLE_RPM) - X[0]),
                      -EXTRAP_CLAMP_DB, EXTRAP_CLAMP_DB)
    line = "  trend would suggest this tilt at %.0f rpm [dB]: " % IDLE_RPM
    for o in (2, 4, 6, 8, 12, 16, 24, 32, 48):
        line += " o%d %+.1f" % (o, tilt_db[2 * o])
    print(line)
    print("  ...but it hits the +-%.0f dB clamp at several orders, because this is a"
          " 1.5-octave extrapolation from %.0f rpm. Two variants are written and the"
          " choice is a listening one:" % (EXTRAP_CLAMP_DB, trend[0]["rpm"]))
    print("    tables_v3.npz       idle = the %.0f rpm bin unchanged, just read at"
          " %.0f rpm (invents nothing)" % (trend[0]["rpm"], IDLE_RPM))
    print("    tables_v3_tilt.npz  idle = same, with the clamped trend tilt applied")
    lim = 10 ** (EXTRAP_CLAMP_DB / 20.0)
    g = np.polyfit(X, np.log10([r_["gain"] for r_ in trend]), 1)
    n = np.polyfit(X, np.log10([r_["nrms"] for r_ in trend]), 1)
    n_idle = float(np.clip(10 ** np.polyval(n, np.log10(IDLE_RPM)),
                           trend[0]["nrms"] / lim, trend[0]["nrms"] * lim))
    print("  gain and noise level follow the same trend: noise/coherent %+.1f dB,"
          " noise shape from the %.0f rpm bin" % (20 * np.log10(n_idle), trend[0]["rpm"]))
    print("  NOTE: a usable idle *measurement* needs a longer, cleaner recording"
          " (>= 10 s, engine dominant).")

    base = rows
    for tag, weight in (("", 0.0), ("_tilt", 1.0)):
        w = np.fft.irfft(np.fft.rfft(trend[0]["wave"])
                         * 10 ** (tilt_db * weight / 20.0), M)
        r = w.std()
        idle_row = dict(rpm=IDLE_RPM, wave=w / r,
                        gain=10 ** np.polyval(g, np.log10(IDLE_RPM)) * r,
                        nrms=n_idle, nspec=trend[0]["nspec"],
                        nenv=trend[0]["nenv"], ncyc=0,
                        src="EXTRAPOLATED: %.0f rpm bin%s"
                            % (trend[0]["rpm"], " + trend tilt" if weight else ", no tilt"))
        rws = [idle_row] + base
        np.savez(os.path.join(OUT, "tables_v3%s.npz" % tag),
                 rpm=np.array([r_["rpm"] for r_ in rws]),
                 wave=np.array([r_["wave"] for r_ in rws]),
                 gain=np.array([r_["gain"] for r_ in rws]),
                 noise_rms=np.array([r_["nrms"] for r_ in rws]),
                 noise_spec=np.array([r_["nspec"] for r_ in rws]),
                 noise_env=np.array([r_["nenv"] for r_ in rws]),
                 ncyc=np.array([r_["ncyc"] for r_ in rws]),
                 src=np.array([r_["src"] for r_ in rws]),
                 M=M, env_bins=ENV_BINS, fs=fs)
        print("  wrote out/tables_v3%s.npz: %d bins, %.0f .. %.0f rpm"
              % (tag, len(rws), rws[0]["rpm"], rws[-1]["rpm"]))
    gaps = [(base[i]["rpm"], base[i + 1]["rpm"]) for i in range(len(base) - 1)
            if base[i + 1]["rpm"] - base[i]["rpm"] > 300]
    print("  gaps > 300 rpm inside the measured range: %s"
          % (", ".join("%.0f-%.0f" % g for g in gaps) if gaps else "none"))
    print("  gap from the idle bin to the lowest measured bin: %.0f -> %.0f rpm"
          " (no material exists in between)" % (IDLE_RPM, base[0]["rpm"]))


if __name__ == "__main__":
    main()
