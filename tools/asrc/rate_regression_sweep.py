#!/usr/bin/env python3
"""Per-rate DSP-load regression sweep for the ASRC app.

Drives `*ar<leg><rate>` over the `serial-monitor` HTTP API, soaks each rate, and
prints one summary line per rate: TDM occupancy, the **minimum** per-window
`margin`, saturation count, the selected anti-alias front-end tag, and any
anomaly lines.

Why a dedicated driver: `TDMsum ... margin=` is a **per-window** value, not a
cumulative peak-hold, so the figure of merit is the minimum over many windows
(n >= 25 by convention, see `[internal] asrc_study` section 6.12).  A
single `GET /log?tail=140` spans two to three rate configurations, so each rate
needs its own log window -- that bookkeeping is what this script exists for.

It only ever talks to the monitor's HTTP API; it never opens the COM port.
See `../serial-monitor/AI_UART_ACCESS.md` (the monitor lives in its own repo).
Pass `--base-url` when this board's monitor is not on the default bind.

Examples
--------
    # leg B down through every supported rate, 60 s soak each
    python tools/asrc/rate_regression_sweep.py --leg b --soak 60 --rates all

    # the mandatory thin-margin gate, leg A (i.e. the B->A direction)
    python tools/asrc/rate_regression_sweep.py --leg a --soak 90 --rates 22.05k

Exit status is 0 if every rate reported telemetry with `sat == 0` and no anomaly
lines, 1 otherwise -- so it can be used as a gate in a larger script.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.request

import tdm_console

DEFAULT_BASE_URL = "http://127.0.0.1:8080"

# `*ar<CC><RR>`: CC = leg (00 = A, 01 = B), RR = rate index.
LEG_INDEX = {"a": "00", "b": "01"}

RATE_INDEX = {
    "8k": "00",
    "11.025k": "01",
    "12k": "02",
    "16k": "03",
    "22.05k": "04",
    "24k": "05",
    "32k": "06",
    "44.1k": "07",
    "48k": "08",
}

# Canonical expected anti-alias front-end tag per rate, for eyeballing the
# summary against `[internal] asrc` records.  Informational only.
#
# A value may be a dict keyed by leg when the two directions legitimately differ.  32 kHz is the
# one such rate: the L=2/M=3 audio-mode front end is fitted to the A->B direction only, and that
# direction is selected by the *B* rate, so it appears when leg **b** is driven to 32 kHz.  Leg a
# at 32 kHz makes B->A the down-sampling direction, which deliberately has no front end (see the
# AB-LEG-ONLY note in asrc_audio_path.c), and stays `direct`.
EXPECTED_FRONTEND = {
    "8k": "/6",
    "11.025k": "/4:11k",
    "12k": "/4:12k",
    "16k": "/3",
    "22.05k": "/2:22k",
    "24k": "/2:24k",
    "32k": {"a": "direct", "b": "2/3:audio"},
    "44.1k": "direct",
    "48k": "direct",
}

TDMSUM_RE = re.compile(
    r"TDMsum:max=([\d.]+)us\(([\d.]+)%\)margin=([\d.]+)us sat=(\d+)")
FRONTEND_RE = re.compile(r"fe=(\S+)")
CCP_RE = re.compile(r"CCP  fsA=([\d.]+) fsB=([\d.]+)")
ANOMALY_RE = re.compile(r"miss=[1-9]|recover=[1-9]|sat=[1-9]|ovf=[1-9]|udf=[1-9]")

# The monitor timestamps every line as HH:MM:SS.mmm; the first 12 characters are
# a lexically sortable key, which is all the windowing below needs.
STAMP_LEN = 12


class Monitor:
    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")

    def _get(self, path: str, timeout: float):
        with urllib.request.urlopen(self.base_url + path, timeout=timeout) as r:
            return json.load(r)

    def require_connected(self) -> None:
        try:
            status = self._get("/status", timeout=10)
        except Exception as exc:  # noqa: BLE001 - message matters, type does not
            raise SystemExit(
                "serial-monitor is unreachable at %s. Start it through "
                "../serial-monitor/start-serial-monitor.ps1 (and check the bind "
                "with -List); never open the COM port directly."
                % self.base_url) from exc
        if not status.get("connected"):
            raise SystemExit(
                "serial-monitor is running but connected=false; do not bypass "
                "it with direct serial access.")

    def command(self, cmd: str) -> None:
        req = urllib.request.Request(
            self.base_url + "/command",
            data=json.dumps({"cmd": cmd}).encode(),
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=10) as r:
            json.load(r)

    def log(self, tail: int) -> list[str]:
        # Pre-S26 line shape for TDMSUM_RE; see tdm_console.to_legacy.
        return tdm_console.to_legacy_all(
            self._get("/log?tail=%d" % tail, timeout=20)["lines"] )


def summarize(lines: list[str], since: str) -> dict:
    windowed = [l for l in lines if l[:STAMP_LEN] >= since]
    joined = " ".join(windowed)
    samples = [
        (float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)))
        for m in (TDMSUM_RE.search(l) for l in windowed) if m
    ]
    return {
        "samples": samples,
        "frontends": sorted(set(FRONTEND_RE.findall(joined))),
        "anomalies": [l for l in windowed if ANOMALY_RE.search(l)],
        "ccp": CCP_RE.findall(joined),
    }


def sweep(mon: Monitor, leg: str, rates: list[str], soak_s: float,
          settle_s: float, tail: int) -> bool:
    ok = True
    leg_code = LEG_INDEX[leg]
    for rate in rates:
        mon.command("*ar%s%s" % (leg_code, RATE_INDEX[rate]))
        time.sleep(settle_s)
        # Mark the log position *after* settling so the transition's own
        # telemetry does not pollute the steady-state window.
        mark = mon.log(2)[-1][:STAMP_LEN]
        time.sleep(soak_s)
        s = summarize(mon.log(tail), mark)

        if not s["samples"]:
            print("%-9s NO TELEMETRY" % rate)
            ok = False
            continue

        sat = sum(v[3] for v in s["samples"])
        fe = ",".join(s["frontends"])
        expected = EXPECTED_FRONTEND.get(rate)
        if isinstance(expected, dict):
            expected = expected.get(leg)
        fe_note = "" if (expected is None or expected in fe) else \
            "  (expected fe=%s)" % expected
        print("%-9s n=%-3d occ_max=%.1f%%  sum_max=%.1fus  margin_min=%.1fus  "
              "margin_max=%.1fus  sat=%d  fe=%s  anomalies=%d%s"
              % (rate, len(s["samples"]),
                 max(v[1] for v in s["samples"]),
                 max(v[0] for v in s["samples"]),
                 min(v[2] for v in s["samples"]),
                 max(v[2] for v in s["samples"]),
                 sat, fe, len(s["anomalies"]), fe_note))
        if s["ccp"]:
            print("           fsA=%s fsB=%s (last)" % s["ccp"][-1])
        for line in s["anomalies"][:4]:
            print("           ANOM " + line)
        if sat or s["anomalies"] or fe_note:
            ok = False
        sys.stdout.flush()
    return ok


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--leg", required=True, choices=sorted(LEG_INDEX),
                   help="which leg to retune; the *other* leg stays at 48 kHz, "
                        "so --leg b measures A->B and --leg a measures B->A")
    p.add_argument("--rates", nargs="+", required=True,
                   help="rate names (%s), or 'all'" % ", ".join(RATE_INDEX))
    p.add_argument("--soak", type=float, default=60.0,
                   help="steady-state soak seconds per rate (default 60; "
                        "aim for n >= 25 telemetry windows)")
    p.add_argument("--settle", type=float, default=12.0,
                   help="seconds to wait after *ar before the window opens "
                        "(default 12)")
    p.add_argument("--tail", type=int, default=300,
                   help="log lines to fetch per rate (default 300)")
    p.add_argument("--base-url", default=DEFAULT_BASE_URL)
    args = p.parse_args(argv)

    rates = list(RATE_INDEX) if args.rates == ["all"] else args.rates
    unknown = [r for r in rates if r not in RATE_INDEX]
    if unknown:
        p.error("unknown rate(s): %s" % ", ".join(unknown))

    mon = Monitor(args.base_url)
    mon.require_connected()
    return 0 if sweep(mon, args.leg, rates, args.soak, args.settle,
                      args.tail) else 1


if __name__ == "__main__":
    sys.exit(main())
