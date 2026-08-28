#!/usr/bin/env python3
"""Sweep leg-B rates over the serial-monitor HTTP bridge and tabulate the ASRC telemetry.

The board console is owned by serial-monitor; this script only speaks HTTP to it
(never opens the COM port).  For each requested rate it sends `*ar 1 <idx>`,
collects N whole telemetry blocks, and prints one row per rate with the numbers
that decide the low-rate click question: the A->B ring fill against its setpoint,
the servo `step` against the feed-forward `ratioAB`, the per-leg pull cost and
the summed TDM load.

Usage (from anywhere):
    python tools/asrc/lowrate_sweep.py --rates 48000,12000,11025,8000 --blocks 3
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request

import tdm_console

BASE = "http://127.0.0.1:8080"

# *ar RR indices, mirrored from src/app/apps/asrc/asrc_console.c
RATE_INDEX = {
    8000: 0, 11025: 1, 12000: 2, 16000: 3, 22050: 4,
    24000: 5, 32000: 6, 44100: 7, 48000: 8,
}

# Line shape changed 2026-07-29: the `ASRC ab[`/`ASRC ba[` prefixes became `AB[`/`BA[`,
# `fs_in=/fs_out=` became `fs:in=/out=`, and the two separate "ASRCpath <dir> front-end:"
# lines were folded into a trailing `fe=` field on these same lines (see RE_FRONTEND).
# Changed again the same day: the direction tag moved behind the kernel block and the colon
# went away, so `AB[poly-k11 x16ch]:` is now `[poly-k11 x16ch]AB` (matching the ASRCpath and
# STREAM lines, where the bracket block also comes first).
_AB = r"\[[^\]]*\]AB\s+"
_BA = r"\[[^\]]*\]BA\s+"
RE_AB = re.compile(
    _AB + r"fill=(\d+)/(\d+)\s+set=(\d+)(!?)\s+(?:intermediate_8k_frames\s+)?step=([\d.]+)"
)
RE_BA = re.compile(_BA + r"fill=(\d+)/(\d+)\s+set=(\d+)(!?)\s+step=([\d.]+)")
RE_PULL_AB = re.compile(_AB + r".*?pull=([\d.]+)us")
RE_PULL_BA = re.compile(_BA + r".*?pull=([\d.]+)us")
RE_FSOUT = re.compile(_AB + r".*?fs:in=(\d+)Hz out=(\d+)Hz")
RE_TDMSUM = re.compile(r"TDMsum:max=([\d.]+)us\(([\d.]+)%\)margin=(-?[\d.]+)us sat=(\d+)")
# Appended to the same line since 2026-07-29: which leg's block period is the denominator
# (the strictest = shortest one), and the independent collision estimate to cross-check the
# measured percentage against.  Separate pattern so RE_TDMSUM stays byte-identical.
RE_TDMWIN = re.compile(r"TDMsum:.*\bwin=TDM(\d)/([\d.]+)us bound=([\d.]+)%")
RE_PATH = re.compile(r"ASRCpath\[[^\]]*\]:\s*cbA=([\d.]+)us.*?cbB=([\d.]+)us")
# Trailing field of the AB line: "direct", or "/3 ovf=0 udf=0" naming the fixed decimator ahead
# of the resampler. The BA counterpart rides the BA line; this sweep only moves leg B, so the
# down-sampling direction is always A->B here and BA is always "direct".
RE_FRONTEND = re.compile(_AB + r".*\bfe=(.+?)\s*$")
RE_FRONTEND_BA = re.compile(_BA + r".*\bfe=(.+?)\s*$")
RE_CCP = re.compile(r"CCP\s+fsA=([\d.]+) fsB=([\d.]+) Hz\s+ratioAB=([\d.]+) recover=(\d+)")
RE_MISS = re.compile(r"TDM(\d):max=.*\(run,act,blk,miss\)=\((\d+),(\d+),(\d+),(\d+)\)")
RE_DECIM = re.compile(r"ovf=(\d+) udf=(\d+)")


def http_get(path: str) -> dict:
    with urllib.request.urlopen(BASE + path, timeout=20) as r:
        return json.load(r)


def http_post(path: str, payload: dict, timeout: int = 30) -> dict:
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        BASE + path, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:  # 408 from /wait, 503 when not connected
        return {"http_error": e.code}


def log_lines(tail: int = 400) -> list[str]:
    # Through the TDM console shim: RE_TDMSUM / RE_TDMWIN / RE_MISS are written against the
    # pre-S26 line shape and must keep parsing archived logs too (tdm_console.to_legacy).
    return tdm_console.to_legacy_all(http_get(f"/log?tail={tail}")["lines"])


def blocks_from(lines: list[str]) -> list[dict]:
    """Split the log tail into telemetry blocks (one per 10 s print) and parse each."""
    out: list[dict] = []
    cur: dict | None = None
    for ln in lines:
        if "STREAM epoch=" in ln:
            if cur:
                out.append(cur)
            cur = {"decim": None, "frontend": None}
            continue
        if cur is None:
            continue
        for rx, keys in (
            (RE_AB, ("fill_ab", "fifo", "set_ab", "capped_ab", "step_ab")),
            (RE_BA, ("fill_ba", "fifo_ba", "set_ba", "capped_ba", "step_ba")),
            (RE_PULL_AB, ("pull_ab",)),
            (RE_PULL_BA, ("pull_ba",)),
            (RE_FSOUT, ("fs_in", "fs_out")),
            (RE_TDMSUM, ("tdm_us", "tdm_pct", "tdm_margin", "sat")),
            (RE_TDMWIN, ("tdm_win_leg", "tdm_win_us", "tdm_bound")),
            (RE_PATH, ("cb_a", "cb_b")),
            (RE_CCP, ("fsA", "fsB", "ratio_ab", "recover")),
        ):
            m = rx.search(ln)
            if m:
                cur.update(dict(zip(keys, m.groups())))
        m = RE_FRONTEND.search(ln)
        if m:
            cur["frontend"] = m.group(1)
            d = RE_DECIM.search(m.group(1))
            if d:
                cur["decim"] = (int(d.group(1)), int(d.group(2)))
        m = RE_FRONTEND_BA.search(ln)
        if m:
            cur["frontend_ba"] = m.group(1)
        m = RE_MISS.search(ln)
        if m:
            cur[f"miss{m.group(1)}"] = int(m.group(5))
    if cur:
        out.append(cur)
    return [b for b in out if "fill_ab" in b and "tdm_pct" in b]


def collect(nblocks: int, timeout_s: float) -> list[dict]:
    """Wait for nblocks fresh telemetry blocks (they arrive every ~10 s)."""
    got: list[dict] = []
    t0 = time.time()
    while len(got) < nblocks and (time.time() - t0) < timeout_s:
        http_post("/wait", {"contains": "CCP  fsA=", "timeout": 20}, timeout=30)
        blk = blocks_from(log_lines(60))
        if blk:
            cand = blk[-1]
            if not got or cand != got[-1]:
                got.append(cand)
    return got


def sweep(rates: list[int], nblocks: int, settle_s: float) -> int:
    st = http_get("/status")
    if not st.get("connected"):
        print(f"monitor not connected: {st}", file=sys.stderr)
        return 2
    print(f"# monitor {st['port']} @ {st['baud']}")
    hdr = (
        f"{'fs_B':>7} {'fe':<9} {'fill ab':>10} {'set':>4} {'step_ab':>9} "
        f"{'ratio/den':>9} {'err_frm':>7} {'pull ab':>8} {'pull ba':>8} "
        f"{'cbA':>7} {'cbB':>7} {'TDMsum':>7} {'margin':>8} {'sat':>3} {'miss':>5} {'ovf/udf':>9}"
    )
    print(hdr)
    print("-" * len(hdr))
    rc = 0
    for hz in rates:
        idx = RATE_INDEX[hz]
        # payload is contiguous ASCII-HEX pairs (no spaces): CC=01 (leg B), RR=rate index
        http_post("/command", {"cmd": f"*ar01{idx:02X}"})
        http_post("/wait", {"contains": "rate ->", "timeout": 15}, timeout=20)
        time.sleep(settle_s)
        blk = collect(nblocks, timeout_s=20 + 12 * nblocks)
        if not blk:
            print(f"{hz:>7}  no telemetry captured")
            rc = 1
            continue
        fills = [int(b["fill_ab"]) for b in blk]
        last = blk[-1]
        step = float(last["step_ab"])
        ratio = float(last.get("ratio_ab", "0") or 0)
        # A front end decimates by `den` before the poly stage, so the ratio the servo tracks is
        # the measured A:B ratio divided by den.  Without this the err column is nonsense for
        # the 8 k (/6) and 11.025 k (/3) rows.
        m_den = re.search(r"^/(\d+)", last.get("frontend") or "")
        den = int(m_den.group(1)) if m_den else 1
        ratio_eff = ratio / den if ratio else 0.0
        # step/ratio_eff - 1 == ASRC_KP * (fill - setpoint): the servo's standing error in frames
        err = (step / ratio_eff - 1.0) / 1e-5 if ratio_eff else float("nan")
        miss = max(int(last.get("miss1", 0)), int(last.get("miss2", 0)))
        ovf = f"{last['decim'][0]}/{last['decim'][1]}" if last.get("decim") else "-"
        # ovf/udf already have their own column; the column here is just the divider identity.
        fe = re.sub(r"\s*ovf=.*$", "", last.get("frontend") or "?")
        setp = f"{last.get('set_ab', '?')}{last.get('capped_ab', '')}"
        print(
            f"{hz:>7} {fe:<9} {min(fills):>4}-{max(fills):<5} {setp:>4} "
            f"{step:>9.5f} {ratio_eff:>9.5f} {err:>+7.1f} {float(last['pull_ab']):>8.1f} "
            f"{float(last['pull_ba']):>8.1f} {float(last['cb_a']):>7.1f} {float(last['cb_b']):>7.1f} "
            f"{last['tdm_pct']:>6}% {float(last['tdm_margin']):>8.1f} {last['sat']:>3} {miss:>5} {ovf:>9}"
        )
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rates", default="48000,16000,12000,11025,8000")
    ap.add_argument("--blocks", type=int, default=2, help="telemetry blocks per rate")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds after the rate switch")
    a = ap.parse_args()
    return sweep([int(x) for x in a.rates.split(",")], a.blocks, a.settle)


if __name__ == "__main__":
    raise SystemExit(main())
