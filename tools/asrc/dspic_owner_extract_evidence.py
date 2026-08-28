#!/usr/bin/env python3
"""One-shot evidence extractor for the dsPIC-owner 2026-07-31 run.

Reads the sonora_monitor capture (a file on disk -- never the COM port) and
emits the compact artifacts the measurement commit needs: the fill histogram
that backs section 10.6, representative telemetry lines, and SHA-256 of every
raw log so the bulk captures can stay out of Git.
"""
import hashlib
import os
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b\(B|\x0f")
RE_POLY = re.compile(r"\[poly x16ch\](AB|BA) fill=(\d+)/(\d+) set=(\d+)(!?) step=([\d.]+) pull=([\d.]+)us")
RE_CCP = re.compile(r"CCP\s+fsA=([\d.]+)\s+fsB=([\d.]+)\s+Hz\s+ratioAB=([\d.]+)\s+recover=(\d+)")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CAPTURE = os.path.join(ROOT, "tools", "sonora_monitor", "monitor_logs", "COM7-20260731.log")
OUTDIR = os.path.join(ROOT, "docs_internal", "asrc", "meas", "dspic_owner_2026-07-31")

RAW_LOGS = [
    "_build_dspic_owner.log",
    "_flash_dspic_owner.log",
    "_monitor.log",
    "tools/asrc/_probe.log",
    "tools/sonora_monitor/monitor_logs/COM7-20260731.log",
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest(), os.path.getsize(path)


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    with open(CAPTURE, "r", encoding="utf-8", errors="replace") as f:
        lines = [ANSI.sub("", l.rstrip("\n")) for l in f]

    # Split acquisition from steady state.  The servo is still acquiring for the
    # first minutes after boot, and those samples sit outside the block-quantisation
    # band -- lumping them in would overstate the steady-state excursion.
    STEADY_FROM = "14:20:00"
    hist = {("all", "AB"): {}, ("all", "BA"): {}, ("steady", "AB"): {}, ("steady", "BA"): {}}
    selfcal, ccp = [], []
    for l in lines:
        m = RE_POLY.search(l)
        if m:
            eng, v, ts = m.group(1), int(m.group(2)), l[:8]
            hist[("all", eng)][v] = hist[("all", eng)].get(v, 0) + 1
            if ts >= STEADY_FROM:
                hist[("steady", eng)][v] = hist[("steady", eng)].get(v, 0) + 1
        if "display self-cal" in l:
            selfcal.append(l.strip())
        m = RE_CCP.search(l)
        if m:
            ccp.append(l.strip())

    with open(os.path.join(OUTDIR, "fill_histogram.txt"), "w", encoding="utf-8") as f:
        f.write("# fill snapshot histogram, dsPIC-owner 2026-07-31, board <PKOB4_SERIAL>\n")
        f.write("# source: COM7-20260731.log ; one snapshot per 2 s telemetry line\n")
        f.write("# backs report section 10.6 (bound = APP_BLOCK_FRAMES = 16)\n")
        f.write("# 'steady' = from %s, i.e. after boot/servo acquisition.\n" % STEADY_FROM)
        f.write("# The only samples below 56 are the first telemetry lines at 14:13:33-35,\n")
        f.write("# emitted before the CCP self-cal latch -- acquisition, not steady state.\n\n")
        for phase in ("all", "steady"):
            for eng in ("AB", "BA"):
                h = hist[(phase, eng)]
                n = sum(h.values())
                if not n:
                    f.write("%-7s %s n=0\n" % (phase, eng))
                    continue
                ks = sorted(h)
                mean = sum(k * v for k, v in h.items()) / n
                f.write("%-7s %s n=%d min=%d max=%d span=%d mean=%.1f\n"
                        % (phase, eng, n, ks[0], ks[-1], ks[-1] - ks[0], mean))
                f.write("  " + " ".join("%d:%d" % (k, h[k]) for k in ks) + "\n\n")

    with open(os.path.join(OUTDIR, "telemetry_excerpt.txt"), "w", encoding="utf-8") as f:
        f.write("# representative lines, dsPIC-owner 2026-07-31\n\n")
        f.write("## CCP display self-cal (one-shot latch)\n")
        if selfcal:
            for l in selfcal:
                f.write(l + "\n")
        else:
            # Never leave a bare heading: an empty section reads as "the evidence is
            # here" when it is not.  Say so in the artifact itself.
            f.write("NOT PRESENT IN THIS CAPTURE -- do not cite a self-cal line as\n")
            f.write("evidence from this file.  The capture may have started after the\n")
            f.write("one-shot latch, or the matcher needs updating.\n")
        f.write("\n## CCP rate lines (first 5, last 5 of %d)\n" % len(ccp))
        f.write("# NOTE the first line, emitted BEFORE the self-cal latch: fsB reads exactly\n")
        f.write("# the compile-time design value 43402.78 Hz.  The CCP time base and leg B's\n")
        f.write("# BCLK both descend from PLL1 by integer dividers, so leg B counted in that\n")
        f.write("# time base cannot read anything else.  After the latch it is that same value\n")
        f.write("# multiplied by the scale (43402.78 * 1.0066 = 43690.30).  This is why the\n")
        f.write("# leg B figure is a DERIVED quantity, not an independent measurement.\n")
        for l in ccp[:5] + ["..."] + ccp[-5:]:
            f.write(l + "\n")

    with open(os.path.join(OUTDIR, "RAW_LOG_SHA256.txt"), "w", encoding="utf-8") as f:
        f.write("# Source log hashes.  NOT a backup -- a hash proves identity only while\n")
        f.write("# you still hold the file.\n")
        f.write("#   omitted from Git (bulk): COM7-20260731.log, _build_dspic_owner.log\n")
        f.write("#   copied into this directory: _flash_dspic_owner.log, _monitor.log,\n")
        f.write("#                               _probe.log\n")
        f.write("# Precedent: meas/pll1_frc_2026-07-30/MANIFEST.md\n\n")
        for rel in RAW_LOGS:
            p = os.path.join(ROOT, rel.replace("/", os.sep))
            if not os.path.exists(p):
                f.write("MISSING  %s\n" % rel)
                continue
            digest, size = sha256(p)
            f.write("%s  %9d bytes  %s\n" % (digest, size, rel))

    print("wrote evidence to", OUTDIR)
    for eng in ("AB", "BA"):
        h = hist[("steady", eng)]
        ks = sorted(h)
        print(" steady %s n=%d span=%d..%d" % (eng, sum(h.values()), ks[0], ks[-1]))
    print(" self-cal lines: %d ; CCP lines: %d" % (len(selfcal), len(ccp)))
    if not selfcal:
        # The report cites the self-cal line as the proof that leg B's figure is
        # derived.  If it is absent the artifact must not silently pretend otherwise.
        print("ERROR: no self-cal line captured -- the report must not cite one",
              file=sys.stderr)
        return 1
    if not ccp:
        print("ERROR: no CCP rate lines captured", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
