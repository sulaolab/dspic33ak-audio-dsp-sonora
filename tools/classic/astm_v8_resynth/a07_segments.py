"""Step 7: list the usable (locked) segments across all clips.

The tracker parks at the bottom of its search range when it has no lock, so a
score threshold plus a minimum duration is enough to cut the material into
segments that can actually be measured.  Everything else is discarded on
purpose -- a wrong phase track poisons the cycle average silently.

Prints one row per segment: time span, RPM span, mean score, mean |RPM slope|,
and how many engine cycles it contains.  A wide RPM span in a *single* segment
is worth much more than the same span stitched from several, because one
continuous phase integration keeps every RPM bin phase-consistent.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")

SCORE_MIN = 0.70
MIN_DUR = 0.50           # s
RPM_FLOOR_MARGIN = 60.0  # ignore frames parked at the search floor


def segments_of(stem, score_min=SCORE_MIN):
    p = os.path.join(OUT, "track_%s.npz" % stem)
    if not os.path.exists(p):
        return None, []
    tr = np.load(p)
    t, rpm, sc = tr["t"], tr["rpm"], tr["score"]
    floor = rpm.min() + RPM_FLOOR_MARGIN
    ok = (sc >= score_min) & (rpm > floor)
    dt = t[1] - t[0]
    segs = []
    i = 0
    while i < len(ok):
        if not ok[i]:
            i += 1
            continue
        j = i
        while j + 1 < len(ok) and ok[j + 1]:
            j += 1
        if (t[j] - t[i]) >= MIN_DUR:
            r = rpm[i:j + 1]
            slope = np.abs(np.diff(r)) / dt if j > i else np.array([0.0])
            ncyc = float(np.sum(r / 120.0) * dt)
            segs.append(dict(t0=t[i], t1=t[j], rpm_lo=r.min(), rpm_hi=r.max(),
                             score=sc[i:j + 1].mean(), slope=slope.mean(),
                             ncyc=ncyc))
        i = j + 1
    return tr, segs


def main():
    stems = sys.argv[1:] or ["astm_stable", "astm_accl_001", "astm_tmp_001",
                             "astm_v8_vantage_all"]
    allsegs = []
    for stem in stems:
        tr, segs = segments_of(stem)
        if tr is None:
            print("%s: no track (run a04 first)" % stem)
            continue
        print("\n=== %s ===  (score >= %.2f, >= %.1f s)" % (stem, SCORE_MIN, MIN_DUR))
        if not segs:
            print("   no locked segment")
        print("   t0      t1     RPM span      span   score  slope[rpm/s]  cycles")
        for s in sorted(segs, key=lambda s: -(s["rpm_hi"] - s["rpm_lo"])):
            print("  %6.2f  %6.2f   %5.0f-%5.0f   %5.0f   %5.2f   %8.0f     %5.0f"
                  % (s["t0"], s["t1"], s["rpm_lo"], s["rpm_hi"],
                     s["rpm_hi"] - s["rpm_lo"], s["score"], s["slope"], s["ncyc"]))
            s["stem"] = stem
            allsegs.append(s)

    # ---- pooled RPM coverage ----
    print("\n=== pooled coverage (cycles per 250 rpm bin, all clips) ===")
    edges = np.arange(2000, 7501, 250)
    hist = np.zeros(len(edges) - 1)
    for s in allsegs:
        lo, hi = s["rpm_lo"], s["rpm_hi"]
        if hi <= lo:
            continue
        for bi in range(len(edges) - 1):
            a, b = max(lo, edges[bi]), min(hi, edges[bi + 1])
            if b > a:
                hist[bi] += s["ncyc"] * (b - a) / (hi - lo)
    for bi in range(len(edges) - 1):
        bar = "#" * int(min(60, hist[bi] / 2))
        print("  %4d-%4d  %6.0f  %s" % (edges[bi], edges[bi + 1], hist[bi], bar))
    gaps = [(edges[bi], edges[bi + 1]) for bi in range(len(edges) - 1) if hist[bi] < 8]
    print("\n  bins with < 8 cycles (unusable): %s"
          % (", ".join("%d-%d" % g for g in gaps) if gaps else "none"))
    best = sorted(allsegs, key=lambda s: -(s["rpm_hi"] - s["rpm_lo"]))[:5]
    print("\n  widest single segments (best primary source for the table):")
    for s in best:
        print("   %-30s t=%6.2f..%6.2f  %5.0f-%5.0f rpm  score %.2f  %.0f cycles"
              % (s["stem"], s["t0"], s["t1"], s["rpm_lo"], s["rpm_hi"],
                 s["score"], s["ncyc"]))


if __name__ == "__main__":
    main()
