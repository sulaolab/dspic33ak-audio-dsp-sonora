#!/usr/bin/env python3
"""One-rate soak measurement for the 48 -> 32 kHz audio-mode front-end study.

Why a dedicated driver instead of rate_regression_sweep.py: that script answers
"did any rate regress" and reports TDMsum plus anomalies.  This study needs the
full per-window record -- TDM1, TDM2, TDMsum, both legs' pull, the ASRCpath
callback costs, and the CUMULATIVE counters read as `last - first` over the
window rather than as absolutes (an absolute `starve=49236` is the count since
boot, not something that happened in this window).

It talks only to the serial-monitor HTTP API, never to the COM port, and it
refuses to run against the wrong board: pass --profile and the /status profile
must match.  `--rate` is optional; without it the current rate is soaked as-is.

Usage
-----
    python tools/asrc/n97_measure.py --label "baseline 48<->32 direct" \
        --leg b --rate 32k --settle 20 --soak 180
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.request

import tdm_console

STAMP_LEN = 12

LEG_INDEX = {"a": "00", "b": "01"}
RATE_INDEX = {
    "8k": "00", "11.025k": "01", "12k": "02", "16k": "03", "22.05k": "04",
    "24k": "05", "32k": "06", "44.1k": "07", "48k": "08",
}

TDM_RE = re.compile(
    # `resp=` is the console spelling; tdm_console.to_legacy_all() rewrites it to the
    # pre-S26 `max=`, so this driver saw ZERO TDM lines while every other pattern matched
    # (found 2026-09-05). Accept both rather than depending on which side is translated.
    r"TDM(\d):(?:resp|max)=([\d.]+)us margin=([\d.]+)us(?: fs=(\d+)Hz)? "
    r"\(run,act,blk,miss\)=\((\d+),(\d+),(\d+),(\d+)\)")
TDMSUM_RE = re.compile(
    r"TDMsum:max=([\d.]+)us\(([\d.]+)%\)margin=([\d.]+)us sat=(\d+)")
# 2026-08-27 telemetry: fill=/fmin=/R= collapsed into hr=fmin-R, and the rates plus R/set/J
# moved to the once-per-ratio-lock line (LOCK_RE). hr is signed; starve may carry a !J flag.
POLY_RE = re.compile(
    r"\](A B|AB|BA) hr=(-?\d+) set=(\d+)(!?) "
    r"step=([\d.]+) pull=([\d.]+)us "
    r"drop=(\d+) starve=(\d+)(!J)? fe=(\S+)")
LOCK_RE = re.compile(
    r"ASRC (AB|BA) lock: step=([\d.]+) R=(\d+) set=(\d+)(!?) J=(\d+)/Jmax=(\d+)(!J)?")
PATH_RE = re.compile(
    r"cbA=([\d.]+)us pushAB=([\d.]+)us ledA=([\d.]+)us\s+"
    r"cbB=([\d.]+)us pushBA=([\d.]+)us ledB=([\d.]+)us")
CCP_RE = re.compile(r"CCP\s+fsA=([\d.]+) fsB=([\d.]+) Hz\s+ratioAB=([\d.]+) recover=(\d+)")
MISC_RE = re.compile(r"ovf=(\d+) udf=(\d+)")
CCPQ_RE = re.compile(r"CCP  period queue overrun A/B=(\d+)/(\d+)")
# DSPload is the AUTHORITATIVE load metric in this build, and TDMsum is not emitted at all
# any more (it saturated -- see the two-metric note in the 16ch mixed-rate work).  This driver
# used to REQUIRE a TDMsum line and printed "NO TELEMETRY" on a perfectly healthy board
# (found 2026-09-05).  `bad=` is the four per-leg anomaly counters.
DSPLOAD_RE = re.compile(
    r"DSPload:A=([\d.]+)% B=([\d.]+)% max=([\d.]+)%"
    r"(?: stolen=([\d.]+)% max_demand=([\d.]+)%)? bad=(\d+)/(\d+)/(\d+)/(\d+)")
# The trial Full-IIR 48 -> 32 stage's own line, when that stage is armed.
FULLIIR_RE = re.compile(
    r"\[full-iir x(\d+)ch\]AB (\d+) SOS gain=(-?\d+)\.(\d+)m?dB "
    r"out=([\d.]+) state=([\d.]+) FS (?:clip|over_fs)=(\d+) nan=(\d+) blk=(\d+)")


class Monitor:
    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")

    def _get(self, path: str, timeout: float):
        with urllib.request.urlopen(self.base_url + path, timeout=timeout) as r:
            return json.load(r)

    def status(self):
        return self._get("/status", timeout=10)

    def command(self, cmd: str) -> None:
        req = urllib.request.Request(
            self.base_url + "/command",
            data=json.dumps({"cmd": cmd}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=15) as r:
            json.load(r)

    def log(self, tail: int):
        # Pre-S26 line shape for the module patterns; see tdm_console.to_legacy.
        return tdm_console.to_legacy_all(
            self._get("/log?tail=%d" % tail, timeout=30)["lines"] )


def stat(values, how):
    if not values:
        return float("nan")
    return max(values) if how == "max" else min(values)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8080")
    ap.add_argument("--profile", default="sonora")
    ap.add_argument("--label", default="")
    ap.add_argument("--leg", choices=sorted(LEG_INDEX))
    ap.add_argument("--rate", choices=sorted(RATE_INDEX))
    ap.add_argument("--settle", type=float, default=20.0)
    ap.add_argument("--soak", type=float, default=180.0)
    ap.add_argument("--tail", type=int, default=1200)
    args = ap.parse_args()

    mon = Monitor(args.base_url)
    st = mon.status()
    if st.get("profile") != args.profile or not st.get("connected"):
        print("ERROR: /status is profile=%r connected=%r, expected %r connected"
              % (st.get("profile"), st.get("connected"), args.profile), file=sys.stderr)
        return 2
    print("board  : profile=%s port=%s baud=%s" % (st["profile"], st["port"], st["baud"]))

    if args.rate:
        if not args.leg:
            print("ERROR: --rate needs --leg", file=sys.stderr)
            return 2
        cmd = "*ar%s%s" % (LEG_INDEX[args.leg], RATE_INDEX[args.rate])
        print("command: %s   (leg %s -> %s)" % (cmd, args.leg.upper(), args.rate))
        mon.command(cmd)

    time.sleep(args.settle)
    mark = mon.log(2)[-1][:STAMP_LEN]      # window starts AFTER the transition settles
    time.sleep(args.soak)
    lines = [l for l in mon.log(args.tail) if l[:STAMP_LEN] >= mark]

    tdm = {1: [], 2: []}
    tdm_margin = {1: [], 2: []}
    miss = {1: [], 2: []}
    tdmsum, margin, sat = [], [], []
    poly = {"AB": [], "BA": []}
    lock = {}
    cb = {"A": [], "B": []}
    push = {"AB": [], "BA": []}
    ccp = []
    dsp = []
    ccpq = []
    bad = []
    fiir = []
    for line in lines:
        m = TDM_RE.search(line)
        if m:
            tdm[int(m.group(1))].append(float(m.group(2)))
            tdm_margin[int(m.group(1))].append(float(m.group(3)))
            miss[int(m.group(1))].append(int(m.group(8)))
        m = TDMSUM_RE.search(line)
        if m:
            tdmsum.append(float(m.group(1)))
            margin.append(float(m.group(3)))
            sat.append(int(m.group(4)))
        m = POLY_RE.search(line)
        if m:
            leg = m.group(1).replace(" ", "")
            poly[leg].append({
                "hr": int(m.group(2)), "set": int(m.group(3)),
                "capped": m.group(4) == "!", "step": float(m.group(5)),
                "pull": float(m.group(6)),
                "drop": int(m.group(7)), "starve": int(m.group(8)),
                "starve_floor": m.group(9) is not None, "fe": m.group(10),
            })
        m = LOCK_RE.search(line)
        if m:
            lock[m.group(1)] = {
                "step": float(m.group(2)), "R": int(m.group(3)), "set": int(m.group(4)),
                "capped": m.group(5) == "!", "J": int(m.group(6)),
                "Jmax": int(m.group(7)), "starve_floor": m.group(8) is not None,
            }
        m = PATH_RE.search(line)
        if m:
            cb["A"].append(float(m.group(1)))
            push["AB"].append(float(m.group(2)))
            cb["B"].append(float(m.group(4)))
            push["BA"].append(float(m.group(5)))
        m = CCP_RE.search(line)
        if m:
            ccp.append((float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4))))
        m = CCPQ_RE.search(line)
        if m:
            ccpq.append((int(m.group(1)), int(m.group(2))))
        m = DSPLOAD_RE.search(line)
        if m:
            demand = m.group(5)
            dsp.append({"a": float(m.group(1)), "b": float(m.group(2)),
                        "max_self": float(m.group(3)),
                        "stolen": float(m.group(4)) if m.group(4) else float("nan"),
                        "max_demand": float(demand) if demand else float("nan")})
            bad.append(tuple(int(m.group(i)) for i in (6, 7, 8, 9)))
        m = FULLIIR_RE.search(line)
        if m:
            fiir.append({"ch": int(m.group(1)), "sos": int(m.group(2)),
                         "gain_db": float("%s.%s" % (m.group(3), m.group(4))),
                         "out_fs": float(m.group(5)), "state_fs": float(m.group(6)),
                         "clip": int(m.group(7)), "nan": int(m.group(8)),
                         "blk": int(m.group(9))})

    print("=" * 88)
    print("%s   report windows=%d  (settle %.0fs, soak %.0fs)"
          % (args.label or "measurement", len(tdm[1]) or len(poly["AB"]),
             args.settle, args.soak))
    print("=" * 88)
    if not ( tdm[1] or poly["AB"] ):
        print("NO TELEMETRY in the window -- is the ASRC app running?")
        return 1

    print("TDM1     worst peak = %7.1f us   TDM2 worst peak = %7.1f us  (n=%d/%d)"
          % (stat(tdm[1], "max"), stat(tdm[2], "max"), len(tdm[1]), len(tdm[2])))
    if tdm[1]:
        print("TDM1     min margin  = %7.1f us   TDM2 min margin = %7.1f us"
              % (stat(tdm_margin[1], "min"), stat(tdm_margin[2], "min")))
    if dsp:
        print("DSPload  max_demand = %7.1f %%    max_self        = %7.1f %%   "
              "(A %.1f / B %.1f, stolen %.1f)"
              % (max(d["max_demand"] for d in dsp), max(d["max_self"] for d in dsp),
                 max(d["a"] for d in dsp), max(d["b"] for d in dsp),
                 max(d["stolen"] for d in dsp)))
    if tdmsum:
        print("TDMsum   worst peak = %7.1f us   min margin      = %7.1f us   (SATURATES -- "
              "not a verdict metric)" % (max(tdmsum), min(margin)))
    for leg in ("AB", "BA"):
        s = poly[leg]
        if not s:
            print("%s       no telemetry" % leg)
            continue
        lk = lock.get(leg)
        print("%s  pull worst = %6.1f us  step=%.5f  fe=%s  "
              "hr %d..%d (worst headroom)  set=%d%s  %s"
              % (leg, max(x["pull"] for x in s), s[-1]["step"], s[-1]["fe"],
                 min(x["hr"] for x in s), max(x["hr"] for x in s),
                 s[-1]["set"], "!" if s[-1]["capped"] else "",
                 ("R=%d J=%d/Jmax=%d%s" % (lk["R"], lk["J"], lk["Jmax"],
                  " STARVE-FLOOR" if lk["starve_floor"] else ""))
                 if lk else "no lock line in window"))
    print("cbA worst = %6.1f us   cbB worst = %6.1f us   pushAB = %5.1f  pushBA = %5.1f"
          % (stat(cb["A"], "max"), stat(cb["B"], "max"),
             stat(push["AB"], "max"), stat(push["BA"], "max")))
    if fiir:
        print("full-IIR stage: x%dch %dSOS gain=%+.3f dB   peak out=%.3f FS   peak state=%.3f FS"
              % (fiir[-1]["ch"], fiir[-1]["sos"], fiir[-1]["gain_db"],
                 max(f["out_fs"] for f in fiir), max(f["state_fs"] for f in fiir)))
    print("-" * 88)
    print("cumulative counters, read as last - first over the window (NOT absolutes)")
    for name, series in (("AB drop", [x["drop"] for x in poly["AB"]]),
                         ("AB starve", [x["starve"] for x in poly["AB"]]),
                         ("BA drop", [x["drop"] for x in poly["BA"]]),
                         ("BA starve", [x["starve"] for x in poly["BA"]]),
                         ("TDM1 miss", miss[1]),
                         ("TDM2 miss", miss[2]),
                         ("CCP recover", [c[3] for c in ccp]),
                         ("bad[0]", [b[0] for b in bad]),
                         ("bad[1]", [b[1] for b in bad]),
                         ("bad[2]", [b[2] for b in bad]),
                         ("bad[3]", [b[3] for b in bad]),
                         ("IIR clip", [f["clip"] for f in fiir]),
                         ("IIR nan", [f["nan"] for f in fiir]),
                         ("IIR blocks", [f["blk"] for f in fiir]),
                         ("CCPq ovr A", [c[0] for c in ccpq]),
                         ("CCPq ovr B", [c[1] for c in ccpq])):
        if not series:
            print("  %-12s no telemetry" % name)
            continue
        print("  %-12s first=%-10d last=%-10d delta=%d"
              % (name, series[0], series[-1], series[-1] - series[0]))
    if sat:   # TDMsum is not emitted at all in the current build; do not crash on its absence
        print("  %-12s max per window=%d  sum over windows=%d"
              % ("TDMsum sat", max(sat), sum(sat)))
    if ccp:
        print("CCP last: fsA=%.2f fsB=%.2f ratioAB=%.6f" % ccp[-1][:3])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
