#!/usr/bin/env python3
"""Crystal-referenced rate + margin/health probe for the dsPIC-owner ASRC preset.

Talks to the board only through the serial-monitor HTTP API (never the COM port).

WHAT THIS DOES AND DOES NOT MEASURE
-----------------------------------
In APP_BUILD_ASRC_DSPIC_BIDIR leg A is clocked by codec-A's crystal while leg B is
derived from the dsPIC's FRC-based PLL1.  The CCP counts both legs in the *same*
time base, so `fsA_raw` (the crystal leg counted against the FRC) is a genuine
measurement, and it yields this board's FRC error.

Leg B is NOT independently measured.  The CCP time base and leg B's BCLK both
descend from PLL1 through integer dividers, so leg B counted in that time base is
pinned to its compile-time design value and cannot read anything else.  Leg B's
crystal-referenced rate is therefore *derived*: design x (1 + FRC error), which is
algebraically the same thing as the firmware's `self-cal scale`.  Do not report the
two as independent corroboration.

"Crystal-referenced", not "absolute": codec-A's crystal tolerance (tens of ppm)
remains, so this method is no more accurate than that crystal.

Analog THD+N / phase noise is out of reach here -- the MEAS harness has no ADC
capture source.

This preset has no runtime rate switch: audio_transport_reconfigure_leg_rate_hz()
is compiled out when leg B's clock source is CONTROLLER, so one image = one
operating point.  The build-time facts are therefore inputs, not assumptions --
pass them explicitly (or accept the documented defaults and have them recorded).

Exit codes: 0 = all gates passed, 1 = usage/transport problem, 2 = the capture is
not evidence (lines lost, or any metric below its minimum sample count, or a TDM
leg missing entirely), 3 = the capture is sound but a health gate failed.
A capture failure takes precedence over a health failure, because health verdicts
read from an incomplete capture are not trustworthy in the first place.

Health gates cover liveness, not just error counters: a soak that sat unqualified,
safe-muted, or with a TDM leg stopped would otherwise report sat=miss=failed=0 and
pass.  --flashed-commit is required and is never inferred from the checkout.

    python dspic_owner_rate_probe.py --soak 150 --board <PKOB4_SERIAL> \
        --jumper B-ExtMCLK --preset APP_BUILD_ASRC_DSPIC_BIDIR \
        --flashed-commit f6f6e47
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.request

import tdm_console

BASE = "http://127.0.0.1:8080"

RE_CCP = re.compile(r"CCP\s+fsA=([\d.]+)\s+fsB=([\d.]+)\s+Hz\s+ratioAB=([\d.]+)\s+recover=(\d+)")
RE_SUM = re.compile(r"TDMsum:max=([\d.]+)us\(([\d.]+)%\)margin=([\d.]+)us\s+sat=(\d+)")
RE_TDM = re.compile(r"TDM([12]):max=([\d.]+)us.*\(run,act,blk,miss\)=\((\d+),(\d+),(\d+),(\d+)\)")
RE_POLY = re.compile(r"\[poly x16ch\](AB|BA) fill=(\d+)/(\d+) set=(\d+)(!?) step=([\d.]+) pull=([\d.]+)us")
RE_STREAM = re.compile(r"STREAM epoch=(\d+) qualified=(\d+).*safe_mute=(\d+) failed=(\d+) error=(\S+)")
RE_MUTE_HELD = re.compile(r"mute_held=(\d+)")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]|\x1b\(B|\x0f")

# Minimum sample counts below which the run is not evidence.  A 150 s soak emits a
# telemetry burst every ~2 s, so ~70 of each is expected; require a clear majority so
# a partially-captured log fails loudly instead of quietly reporting min()/max() over
# three lines.
# TDM1 and TDM2 are counted separately on purpose: a combined count lets one leg's
# telemetry vanish entirely while the other leg alone clears the threshold.
MIN_COUNTS = {"ccp": 20, "tdmsum": 20, "tdm1": 20, "tdm2": 20, "stream": 5,
              "poly_AB": 20, "poly_BA": 20}


def http_json(path, timeout=30):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return json.loads(r.read().decode())


def log_tail(n):
    # ANSI-stripped, then normalised to the pre-S26 line shape (tdm_console.to_legacy)
    # so RE_SUM / RE_TDM keep matching the renamed load line.
    return tdm_console.to_legacy_all(
        [ANSI.sub("", l) for l in http_json("/log?tail=%d" % n)["lines"]] )


def git_head(repo_root):
    try:
        out = subprocess.run(["git", "-C", repo_root, "rev-parse", "HEAD"],
                             capture_output=True, text=True, timeout=20)
        if out.returncode == 0:
            return out.stdout.strip()
    except Exception:
        pass
    return None


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--soak", type=float, default=150.0, help="soak seconds (default 150)")
    p.add_argument("--tail", type=int, default=8000,
                   help="log lines to fetch after the soak (default 8000)")
    # Provenance -- recorded in the JSON so a result file is self-describing.
    #
    # --flashed-commit is REQUIRED and is never defaulted to the worktree HEAD.  The
    # image on the board is whatever was last built and flashed; the checkout moves on
    # (docs commits, amends, rebases) while the board does not.  Defaulting to HEAD
    # silently mislabels every result taken after the next commit.
    p.add_argument("--flashed-commit", default=None,
                   help="REQUIRED: commit whose build is actually on the board")
    p.add_argument("--board", default=None, help="PKOB4/board serial")
    p.add_argument("--jumper", default=None, help="B-jumper setting, e.g. B-ExtMCLK")
    p.add_argument("--preset", default=None, help="APP_BUILD_* preset flashed")
    # Build-time facts.  Defaults match asrc_app_config.h APP_SPI2_MASTER_BRG=8 and
    # resolved_transport_config.h, but they are recorded and gated, not assumed.
    p.add_argument("--pll1-hz", type=float, default=200e6)
    p.add_argument("--brg", type=int, default=8)
    p.add_argument("--slots", type=int, default=8)
    p.add_argument("--word-bits", type=int, default=32)
    p.add_argument("--fs-a-nominal", type=float, default=48000.0,
                   help="codec-A crystal nominal rate, the reference (default 48000)")
    p.add_argument("--fs-b-intent", type=float, default=44100.0,
                   help="rate this preset leans toward, for the quantization term")
    p.add_argument("--out", default=None, help="output JSON path (default: timestamped)")
    return p.parse_args()


def collect(lines):
    """Parse telemetry, keeping per-metric counts so thin captures are detectable."""
    d = {
        "fsa": [], "fsb": [], "ratio": [], "recover": 0,
        "margins": [], "sums": [], "sat": 0, "miss": 0,
        "tdm_max": {}, "epochs": set(), "failed": 0, "errors": set(),
        "capped": set(), "steps": {}, "pulls": {}, "fills": {},
        # Liveness, as opposed to error counters.  A stream can be fault-free and still
        # not be passing audio: unqualified, safe-muted, or a TDM leg not running.
        "qualified": set(), "safe_mute": set(), "mute_held": set(),
        "tdm_run": {}, "tdm_act": {},
        "counts": {"ccp": 0, "tdmsum": 0, "tdm1": 0, "tdm2": 0, "stream": 0,
                   "poly_AB": 0, "poly_BA": 0},
    }
    for l in lines:
        m = RE_CCP.search(l)
        if m:
            d["counts"]["ccp"] += 1
            d["fsa"].append(float(m.group(1)))
            d["fsb"].append(float(m.group(2)))
            d["ratio"].append(float(m.group(3)))
            d["recover"] = max(d["recover"], int(m.group(4)))
        m = RE_SUM.search(l)
        if m:
            d["counts"]["tdmsum"] += 1
            d["sums"].append(float(m.group(2)))
            d["margins"].append(float(m.group(3)))
            d["sat"] = max(d["sat"], int(m.group(4)))
        m = RE_TDM.search(l)
        if m:
            d["counts"]["tdm" + m.group(1)] += 1
            k = "TDM" + m.group(1)
            d["tdm_max"][k] = max(d["tdm_max"].get(k, 0.0), float(m.group(2)))
            d["tdm_run"].setdefault(k, set()).add(int(m.group(3)))
            d["tdm_act"].setdefault(k, set()).add(int(m.group(4)))
            d["miss"] = max(d["miss"], int(m.group(6)))
        m = RE_POLY.search(l)
        if m:
            eng = m.group(1)
            d["counts"]["poly_" + eng] += 1
            if m.group(5) == "!":
                d["capped"].add(eng)
            d["steps"].setdefault(eng, []).append(float(m.group(6)))
            d["pulls"][eng] = max(d["pulls"].get(eng, 0.0), float(m.group(7)))
            d["fills"].setdefault(eng, []).append(int(m.group(2)))
        m = RE_STREAM.search(l)
        if m:
            d["counts"]["stream"] += 1
            d["epochs"].add(int(m.group(1)))
            d["qualified"].add(int(m.group(2)))
            d["safe_mute"].add(int(m.group(3)))
            d["failed"] = max(d["failed"], int(m.group(4)))
            d["errors"].add(m.group(5))
            mh = RE_MUTE_HELD.search(l)
            if mh:
                d["mute_held"].add(int(mh.group(1)))
    return d


def main():
    a = parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(os.path.dirname(here))

    fs_b_design = a.pll1_hz / (2.0 * (a.brg + 1)) / (a.slots * a.word_bits)

    # Refuse to produce a result file that cannot say which image it describes.
    # Handled here rather than via argparse required=True so the documented
    # "1 = usage problem" exit code holds (argparse would exit 2, i.e. "not evidence").
    if not a.flashed_commit:
        print("--flashed-commit is required: pass the commit whose build is on the "
              "board.\nIt is deliberately NOT defaulted to the worktree HEAD, which "
              "drifts away from\nthe flashed image as soon as you make another commit.",
              file=sys.stderr)
        return 1

    try:
        st = http_json("/status", timeout=10)
    except Exception as e:
        print("monitor unreachable at %s: %s" % (BASE, e), file=sys.stderr)
        return 1
    if not st.get("connected"):
        print("monitor is running but not connected to the board", file=sys.stderr)
        return 1
    print("monitor on %s @ %d ; soaking %.0f s" % (st["port"], st["baud"], a.soak), flush=True)

    # Anchor on the last line present before the soak.  If it is missing from the
    # post-soak fetch, the log advanced past --tail and lines were LOST -- report that
    # instead of silently analysing a truncated window.  This also removes the old
    # HH:MM:SS string comparison, which broke across midnight.
    pre = log_tail(50)
    anchor = pre[-1] if pre else None

    time.sleep(a.soak)
    lines = log_tail(a.tail)

    truncated = False
    if anchor is not None:
        idx = None
        for i in range(len(lines) - 1, -1, -1):
            if lines[i] == anchor:
                idx = i
                break
        if idx is None:
            truncated = True
        else:
            lines = lines[idx + 1:]

    d = collect(lines)
    thin = {k: (d["counts"][k], v) for k, v in MIN_COUNTS.items() if d["counts"][k] < v}

    now = time.localtime()
    stamp = time.strftime("%Y%m%dT%H%M%S", now)
    res = {
        "schema": 2,
        "run": {
            "timestamp_local": time.strftime("%Y-%m-%d %H:%M:%S", now),
            # "unverified" rather than null: a reader must be able to tell "nobody
            # stated this" apart from "this was checked".
            "board": a.board or "unverified",
            "jumper": a.jumper or "unverified",
            "preset": a.preset or "unverified",
            "flashed_commit": a.flashed_commit,
            # The checkout the tool ran from -- NOT necessarily what is on the board.
            "tool_git_head": git_head(repo_root),
            "provenance_note":
                "flashed_commit is operator-asserted; tool_git_head is the checkout the "
                "probe ran from. They differ whenever commits landed after the flash.",
            "monitor_port": st.get("port"),
            "monitor_baud": st.get("baud"),
            "soak_s": a.soak,
            "log_truncated": truncated,
        },
        "build_facts": {
            "pll1_hz": a.pll1_hz, "brg": a.brg, "slots": a.slots,
            "word_bits": a.word_bits,
            "fs_a_nominal_hz": a.fs_a_nominal, "fs_b_intent_hz": a.fs_b_intent,
            "fs_b_design_hz": round(fs_b_design, 3),
        },
        "sample_counts": d["counts"],
        "insufficient": {k: {"got": g, "need": n} for k, (g, n) in thin.items()},
    }

    if d["counts"]["ccp"]:
        fsa_mean = sum(d["fsa"]) / len(d["fsa"])
        fsb_mean = sum(d["fsb"]) / len(d["fsb"])
        # fsA is displayed post-self-cal, so it sits at the nominal; the FRC error is
        # what the scale removed.  Reporting leg B relative to design IS that same
        # quantity -- labelled derived, not an independent measurement.
        ppm_design = (fsb_mean - fs_b_design) / fs_b_design * 1e6
        res["rate"] = {
            "fsA_displayed_mean": round(fsa_mean, 3),
            "fsA_displayed_span": [min(d["fsa"]), max(d["fsa"])],
            "fsB_displayed_mean_hz": round(fsb_mean, 3),
            "fsB_displayed_span": [min(d["fsb"]), max(d["fsb"])],
            "fsB_crystal_referenced_hz__DERIVED": round(fsb_mean, 3),
            "ratioAB_mean": round(sum(d["ratio"]) / len(d["ratio"]), 6),
            # Basis matters: this is derived from the POST-self-cal leg B display, so it
            # is the +6624.5-style number.  Deriving it from fsA_raw instead gives a
            # figure ~2 ppm larger (+6626.5 on board ...1164) -- same quantity, different
            # rounding path.  This tool cannot produce the fsA_raw basis, because after
            # the latch fsA is displayed at its nominal; that needs the pre-latch CCP
            # line or the self-cal line, neither of which this tool parses.
            "frc_error_ppm_from_fsB_display": round(ppm_design, 1),
            "frc_error_ppm_from_fsB_display__note":
                "equals self-cal scale-1; NOT an independent path. The fsA_raw basis is "
                "~2 ppm larger and is not computed here (pre-latch line not parsed).",
            "ppm_brg_quantization": round((fs_b_design - a.fs_b_intent) / a.fs_b_intent * 1e6, 1),
            "ppm_total_vs_intent": round((fsb_mean - a.fs_b_intent) / a.fs_b_intent * 1e6, 1),
            "cents_total_vs_intent": round(1200.0 * math.log2(fsb_mean / a.fs_b_intent), 2),
            # Named for what it actually tests: the displayed values are all equal.
            # It does NOT check agreement with fs_b_design -- after the self-cal latch
            # the display is design x scale, so it deliberately will not equal design.
            "fsB_display_zero_spread": (min(d["fsb"]) == max(d["fsb"])),
            "fsB_display_zero_spread_note":
                "zero spread is structural (leg B and the CCP time base share PLL1), "
                "not a stability result; this field does not compare against fs_b_design",
        }
    if d["counts"]["tdmsum"]:
        res["load"] = {
            "margin_min_us": min(d["margins"]),
            "margin_max_us": max(d["margins"]),
            "tdmsum_max_pct": max(d["sums"]),
        }
    res["health"] = {
        "sat": d["sat"], "miss": d["miss"], "recover": d["recover"], "failed": d["failed"],
        "stream_epochs": sorted(d["epochs"]),
        "stream_errors": sorted(d["errors"]),
        "stream_qualified_values": sorted(d["qualified"]),
        "stream_safe_mute_values": sorted(d["safe_mute"]),
        "stream_mute_held_values": sorted(d["mute_held"]),
        "tdm_run_values": {k: sorted(v) for k, v in d["tdm_run"].items()},
        "tdm_act_values": {k: sorted(v) for k, v in d["tdm_act"].items()},
        "fill_target_capped": sorted(d["capped"]),
        "tdm_leg_max_us": d["tdm_max"],
        "pull_max_us": d["pulls"],
        "step_mean": {k: round(sum(v) / len(v), 6) for k, v in d["steps"].items()},
        "fill_span": {k: [min(v), max(v)] for k, v in d["fills"].items()},
        "fill_n": {k: len(v) for k, v in d["fills"].items()},
    }

    # --- gates ---------------------------------------------------------------
    # Two distinct kinds of failure, matching the documented exit codes:
    #   capture_fails -> exit 2 (the run is not evidence: lines lost or too few)
    #   health_fails  -> exit 3 (the capture is sound and the board misbehaved)
    capture_fails, health_fails = [], []
    if truncated:
        capture_fails.append(
            "log truncated: pre-soak anchor missing from tail=%d, lines were lost" % a.tail)
    for k, (got, need) in sorted(thin.items()):
        capture_fails.append("insufficient %s samples: %d < %d" % (k, got, need))
    for leg in ("TDM1", "TDM2"):
        if leg not in d["tdm_max"]:
            capture_fails.append("no %s telemetry at all" % leg)
    # A liveness gate that silently no-ops when its field disappears is worse than no
    # gate, because the PASS still looks meaningful.  qualified/safe_mute come from
    # RE_STREAM itself (absent -> the stream count is already 0 -> caught above), but
    # mute_held is matched separately and would just leave an empty set.
    if d["counts"]["stream"] and not d["mute_held"]:
        capture_fails.append(
            "mute_held field missing from STREAM telemetry: the mute_held gate would "
            "pass vacuously (telemetry format changed?)")
    for k in ("sat", "miss", "recover", "failed"):
        if d[k]:
            health_fails.append("health counter %s = %d (expected 0)" % (k, d[k]))
    if d["capped"]:
        health_fails.append("fill_target_capped set for %s" % ",".join(sorted(d["capped"])))
    bad_err = sorted(e for e in d["errors"] if e != "none")
    if bad_err:
        health_fails.append("stream error(s): %s" % ",".join(bad_err))
    if len(d["epochs"]) > 1:
        health_fails.append("stream restarted mid-soak: epochs %s" % sorted(d["epochs"]))
    # Liveness: fault-free is not the same as passing audio.  Without these, a soak that
    # sat unqualified or safe-muted the whole time, or with a TDM leg stopped, reports
    # sat=miss=failed=0 and would otherwise PASS.
    if d["qualified"] and d["qualified"] != {1}:
        health_fails.append("stream not continuously qualified: values %s"
                            % sorted(d["qualified"]))
    if d["safe_mute"] and d["safe_mute"] != {0}:
        health_fails.append("safe_mute asserted during the soak: values %s"
                            % sorted(d["safe_mute"]))
    if d["mute_held"] and d["mute_held"] != {0}:
        health_fails.append("mute_held asserted during the soak: values %s"
                            % sorted(d["mute_held"]))
    for k in sorted(set(d["tdm_run"]) | set(d["tdm_act"])):
        for field in ("run", "act"):
            seen = d["tdm_" + field].get(k)
            if seen and seen != {1}:
                health_fails.append("%s %s != 1 during the soak: %s"
                                    % (k, field, sorted(seen)))

    fails = capture_fails + health_fails
    res["gates"] = {
        "passed": not fails,
        "capture_failures": capture_fails,
        "health_failures": health_fails,
    }

    out = a.out or os.path.join(here, "dspic_owner_rate_probe_%s.json" % stamp)
    with open(out, "w") as f:
        json.dump(res, f, indent=2)
    print(json.dumps(res, indent=2))
    print("\nwrote %s" % out)

    if capture_fails:
        print("FAIL (exit 2): capture is not evidence:", file=sys.stderr)
        for m in capture_fails:
            print("  - " + m, file=sys.stderr)
        for m in health_fails:
            print("  (also, health: %s)" % m, file=sys.stderr)
        return 2
    if health_fails:
        print("FAIL (exit 3): %d health gate(s) failed:" % len(health_fails), file=sys.stderr)
        for m in health_fails:
            print("  - " + m, file=sys.stderr)
        return 3
    print("PASS: all gates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
