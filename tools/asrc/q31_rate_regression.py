#!/usr/bin/env python3
"""Phase 7 all-rate hardware regression driver for the Q31/float ASRC arms.

Walks every supported rate pair on the AK512 BIDIR image, dwells on each one,
and reports the WORST OBSERVED PEAK of every load counter -- not the last
telemetry block and not an average.  One process, one summary table, so the
whole sweep costs a single tool round-trip instead of one per rate.

It never touches the COM port: everything goes through the serial-monitor
localhost HTTP bridge (the port is owned exclusively by the monitor).  The base
URL is resolved from the caller, defaulting to the sonora alias this repo's
.serial-monitor.json declares -- several monitors share port 8080 on different
loopback aliases, so /status is checked for the expected profile before a single
command is sent.

Usage:
    python tools/asrc/q31_rate_regression.py --arm q31 --out sweep_q31.json
    python tools/asrc/q31_rate_regression.py --arm float --dwell 60
"""

import argparse
import json
import re
import sys
import time
import urllib.request

import tdm_console

RATES = {              # *ar RR index -> nominal Hz
    0: 8000, 1: 11025, 2: 12000, 3: 16000, 4: 22050,
    5: 24000, 6: 32000, 7: 44100, 8: 48000, 9: 96000,
}

# Leg A is pinned at 48 kHz throughout: every shipping case is "48 <-> x", and
# the pair gate rejects two low-rate legs anyway.
SWEEP = [8, 7, 6, 5, 4, 3, 2, 1, 0, 9]

# 2026-08-27: the per-leg line is `resp=` (was `max=`) and no longer carries a percentage --
# resp includes preemption, so it was never a load. Load is the DSPload line.
RE_TDM = re.compile(r"TDM(1|2):resp=([\d.]+)us margin=([\d.]+)us(?: fs=(\d+)Hz)?"
                    r".*?\(run,act,blk,miss\)=\((\d+),(\d+),(\d+),(\d+)\)")
RE_SUM = re.compile(r"TDMsum:max=([\d.]+)us\(([\d.]+)%\)margin=([-\d.]+)us sat=(\d+)")
# 2026-08-27 telemetry: hr=fmin-R replaces fill=/fmin=/R=; rates moved to the ratio-lock line.
RE_POLY = re.compile(r"\[poly-k11 x16ch\](AB|BA) hr=(-?\d+) set=(\d+)(!?) "
                     r"step=([\d.]+) pull=([\d.]+)us drop=(\d+) starve=(\d+)(!J)? fe=(\S+)")
RE_PATH = re.compile(r"cbA=([\d.]+)us pushAB=([\d.]+)us .*?cbB=([\d.]+)us pushBA=([\d.]+)us")
# The measured rates left the per-engine line in the 2026-08-27 diet (they are constant between
# rate changes). The CCP line is the authority for them and always has been.
RE_CCP = re.compile(r"CCP\s+fsA=([\d.]+) fsB=([\d.]+) Hz")


class Bridge:
    def __init__(self, base, profile):
        self.base = base.rstrip("/")
        st = self.get("/status")
        if profile and st.get("profile") != profile:
            sys.exit("wrong board: %s answers for profile %r, expected %r"
                     % (self.base, st.get("profile"), profile))
        if not st.get("connected"):
            sys.exit("monitor at %s is not connected to %s" % (self.base, st.get("port")))
        print("bridge %s  profile=%s port=%s" % (self.base, st.get("profile"), st.get("port")))

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=30) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    def post(self, path, obj, timeout=30):
        req = urllib.request.Request(
            self.base + path, data=json.dumps(obj).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))

    def log(self, tail=1200):
        # Pre-S26 line shape for RE_TDM / RE_SUM; see tdm_console.to_legacy.
        return tdm_console.to_legacy_all(self.get("/log?tail=%d" % tail)["lines"])


def sdelta(d):
    """Steady-state starve count: what accrued inside the window, not since reset."""
    return (d["starve_last"] or 0) - (d["starve_first"] or 0)


def worst(lines):
    """Fold every telemetry block in `lines` into one worst-case record."""
    w = {"tdm1_us": 0.0, "tdm1_margin_us": 0.0, "tdm2_us": 0.0, "tdm2_margin_us": 0.0,
         "fs_1": None, "fs_2": None,
         "tdmsum_us": 0.0, "tdmsum_pct": 0.0, "margin_us": None, "sat": 0,
         "miss": 0, "blocks": 0,
         "cbA_us": 0.0, "cbB_us": 0.0, "pushAB_us": 0.0, "pushBA_us": 0.0,
         "samples": 0}
    for leg in ("AB", "BA"):
        w[leg] = {"pull_us": 0.0, "starve_first": None, "starve_last": None,
                  "drop_first": None, "drop_last": None,
                  "hr_min": None, "set": None, "capped": False,
                  "starve_floor": False,
                  "step": None, "fs_in": None, "fs_out": None, "fe": None}
    for ln in lines:
        m = RE_SUM.search(ln)
        if m:
            us, pct, margin, sat = float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4))
            if us > w["tdmsum_us"]:
                w["tdmsum_us"], w["tdmsum_pct"], w["margin_us"] = us, pct, margin
            w["sat"] = max(w["sat"], sat)
            w["samples"] += 1
        m = RE_TDM.search(ln)
        if m:
            k = "tdm%s" % m.group(1)
            if float(m.group(2)) > w[k + "_us"]:
                # group(3) is now margin_us, not a percentage: the per-leg line no longer
                # reports one (resp/deadline was never a load -- see the regex comment).
                w[k + "_us"], w[k + "_margin_us"] = float(m.group(2)), float(m.group(3))
            # groups: 1 leg, 2 resp, 3 margin, 4 fs (optional), 5 run, 6 act, 7 blk, 8 miss
            w["fs_%s" % m.group(1)] = int(m.group(4)) if m.group(4) else None
            w["miss"] = max(w["miss"], int(m.group(8)))
            w["blocks"] = max(w["blocks"], int(m.group(7)))
        m = RE_POLY.search(ln)
        if m:
            d = w[m.group(1)]
            d["pull_us"] = max(d["pull_us"], float(m.group(8)))
            # starve and drop are counters that run from the leg's last restart, so
            # the max over the window is whatever warm-up already left behind.  Only
            # the first-to-last delta says anything about steady state.
            starve = int(m.group(8))
            if d["starve_first"] is None:
                d["starve_first"] = starve
            d["starve_last"] = starve
            # hr = fmin - R, the worst pull's headroom in frames; signed, and the only form
            # that compares across ring sizes. The old fill/fmin/R triple is gone from this line
            # (R and set arrive once per ratio-lock on the "ASRC <leg> lock:" line).
            hr = int(m.group(2))
            d["hr_min"] = hr if d["hr_min"] is None else min(d["hr_min"], hr)
            d["set"], d["step"] = int(m.group(3)), float(m.group(5))
            d["capped"], d["starve_floor"] = (m.group(4) == "!"), (m.group(9) is not None)
            d["fe"] = m.group(10)
            drop = int(m.group(7))
            if d["drop_first"] is None:
                d["drop_first"] = drop
            d["drop_last"] = drop
        m = RE_CCP.search(ln)
        if m:
            fsa, fsb = int(float(m.group(1))), int(float(m.group(2)))
            w["AB"]["fs_in"], w["AB"]["fs_out"] = fsa, fsb
            w["BA"]["fs_in"], w["BA"]["fs_out"] = fsb, fsa
        m = RE_PATH.search(ln)
        if m:
            for i, k in enumerate(("cbA_us", "pushAB_us", "cbB_us", "pushBA_us")):
                w[k] = max(w[k], float(m.group(i + 1)))
    return w


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", required=True, choices=("float", "q31"))
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    ap.add_argument("--profile", default="sonora")
    ap.add_argument("--dwell", type=float, default=45.0,
                    help="seconds to observe each rate after the restart settles")
    ap.add_argument("--rates", default="", help="comma-separated RR indices (default: the full sweep)")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    sweep = [int(x) for x in args.rates.split(",")] if args.rates else SWEEP
    br = Bridge(args.base, args.profile)
    results = []

    # Leg A is the fixed 48 kHz side of every case; pin it once so a leftover
    # setting from an earlier session cannot silently change what "48 <-> x" means.
    br.post("/command", {"cmd": "*ar0008"})
    time.sleep(3.0)

    for rr in sweep:
        name = "48<->%s" % ("%.3g" % (RATES[rr] / 1000.0))
        # Arm /wait BEFORE the command: it only sees bytes that arrive after the
        # call, and the console acknowledges in ~10 ms.
        print("\n=== %s (RR=%02d) ===" % (name, rr), flush=True)
        # No pre-armed /wait on the console ack: this is a single-threaded client,
        # so a blocking /wait would have to return before the command could be
        # sent.  The telemetry block below is a 10 s heartbeat, which cannot lose
        # the race, and the accepted rate is verified from the poly line's
        # fs:in/out rather than from the ack text.  The frame carries its two
        # arguments as a bare hex payload -- "*ar0106", not "*ar 01 06":
        # a space is not a hex digit, so the spaced form is dropped by the
        # shared parser before the handler runs and prints NOTHING at all.
        ack = br.post("/command", {"cmd": "*ar01%02X" % rr})
        if ack.get("status") not in (None, "ok", 0):
            print("  command status: %r" % ack.get("status"))
        # Let the servo settle before the measurement window opens: a rate change
        # restarts the leg, and the first telemetry window after a restart shows
        # warm-up starves that say nothing about steady-state load.  Two whole
        # 10 s windows are discarded, which covers the 3-4 s warm-up with margin.
        for _ in range(2):
            try:
                br.post("/wait", {"contains": "TDMsum", "timeout": 40}, timeout=60)
            except Exception:
                print("  no TDMsum within 40 s after the rate change")
        time.sleep(1.0)
        mark = br.log(tail=40)
        anchor = mark[-1] if mark else ""
        time.sleep(args.dwell)
        lines = br.log(tail=1500)
        if anchor in lines:
            lines = lines[lines.index(anchor) + 1:]
        w = worst(lines)
        w["name"], w["rr"], w["hz"] = name, rr, RATES[rr]
        results.append(w)
        print("  TDM1=%.1f TDM2=%.1f TDMsum=%.1f us (%.1f%%) sat=%d miss=%d  "
              "pull AB=%.1f BA=%.1f  starve AB=%d BA=%d  wins=%d"
              % (w["tdm1_us"], w["tdm2_us"], w["tdmsum_us"], w["tdmsum_pct"],
                 w["sat"], w["miss"], w["AB"]["pull_us"], w["BA"]["pull_us"],
                 sdelta(w["AB"]), sdelta(w["BA"]), w["samples"]), flush=True)
        print("  AB fs %s->%s step=%s fe=%s | BA fs %s->%s step=%s fe=%s"
              % (w["AB"]["fs_in"], w["AB"]["fs_out"], w["AB"]["step"], w["AB"]["fe"],
                 w["BA"]["fs_in"], w["BA"]["fs_out"], w["BA"]["step"], w["BA"]["fe"]), flush=True)
        if w["AB"]["fs_out"] is not None and abs(w["AB"]["fs_out"] - RATES[rr]) > RATES[rr] * 0.01:
            print("  !! AB output rate is not the requested %d Hz" % RATES[rr], flush=True)

    print("\n%-12s %7s %7s %8s %7s %7s %7s %6s %6s %6s %6s %6s %5s" %
          ("case", "TDM1", "TDM2", "TDMsum", "dl%", "ABpull", "BApull",
           "cbA", "cbB", "starv", "drop", "miss", "sat"))
    for w in results:
        dab = (w["AB"]["drop_last"] or 0) - (w["AB"]["drop_first"] or 0)
        dba = (w["BA"]["drop_last"] or 0) - (w["BA"]["drop_first"] or 0)
        print("%-12s %7.1f %7.1f %8.1f %7.1f %7.1f %7.1f %6.1f %6.1f %6d %6d %6d %5d" %
              (w["name"], w["tdm1_us"], w["tdm2_us"], w["tdmsum_us"], w["tdmsum_pct"],
               w["AB"]["pull_us"], w["BA"]["pull_us"], w["cbA_us"], w["cbB_us"],
               sdelta(w["AB"]) + sdelta(w["BA"]), dab + dba, w["miss"], w["sat"]))

    if args.out:
        with open(args.out, "w", newline="\n") as fh:
            json.dump({"arm": args.arm, "dwell": args.dwell, "cases": results}, fh, indent=1)
        print("\nwrote %s" % args.out)


if __name__ == "__main__":
    main()
