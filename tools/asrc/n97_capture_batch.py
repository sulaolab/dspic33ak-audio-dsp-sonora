#!/usr/bin/env python3
"""Batch tone-capture driver for the 48 -> 32 kHz N=97 study (many captures per call).

Why a batch driver: each capture is arm -> settle -> dump -> slice, and a sweep needs
dozens.  Doing that one console command per tool call costs one round trip each; the
network here makes that the dominant cost.  This runs the whole list in one process.

It talks ONLY to the serial-monitor HTTP API -- never the COM port -- and refuses to
run if /status reports a different profile than --profile.  The 2048-line dump is read
back from the monitor's own log_file rather than from /log, because /log's tail window
is smaller than one dump.

Every `?ac` dump is framed by `*MEAS_BEGIN ... *MEAS_END`; the slice taken is the LAST
complete frame that appeared after the command was sent, and the output filename is
derived from that frame's own header (tone_hz / level_dbfs), never from what we asked
for -- so a row/level that did not take shows up as a mislabelled-looking file instead
of silently corrupting the sweep table.

Usage
-----
    python tools/asrc/n97_capture_batch.py --out <capture output dir> \
        --rate 32k --leg b --freeze --jobs 0:5 12:5 12:5 13:0 13:2
        (job = <tone row>:<level index>, level 0=-1 2=-20 5=-6 dBFS)
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import threading
import time
import urllib.request

LEG_INDEX = {"a": "00", "b": "01"}
RATE_INDEX = {
    "8k": "00", "11.025k": "01", "12k": "02", "16k": "03", "22.05k": "04",
    "24k": "05", "32k": "06", "44.1k": "07", "48k": "08",
}
LEVEL_DBFS = {0: -1.0, 1: -60.0, 2: -20.0, 3: -40.0, 4: -80.0, 5: -6.0}


class Monitor:
    def __init__(self, base_url):
        self.base_url = base_url.rstrip("/")

    def _get(self, path, timeout):
        with urllib.request.urlopen(self.base_url + path, timeout=timeout) as r:
            return json.load(r)

    def status(self):
        return self._get("/status", timeout=10)

    def command(self, cmd):
        req = urllib.request.Request(
            self.base_url + "/command",
            data=json.dumps({"cmd": cmd}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=20) as r:
            return json.load(r)

    def wait(self, contains, timeout):
        req = urllib.request.Request(
            self.base_url + "/wait",
            data=json.dumps({"contains": contains, "timeout": timeout}).encode(),
            headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout + 15) as r:
            return json.load(r)


def armed_command(mon, cmd, marker, timeout):
    """Arm /wait BEFORE sending: /wait only sees bytes observed after the call, and a
    console reply that comes back in ~10 ms otherwise loses the race."""
    box = {}

    def run():
        try:
            box["res"] = mon.wait(marker, timeout)
        except Exception as exc:            # noqa: BLE001 -- reported, not swallowed
            box["err"] = exc

    th = threading.Thread(target=run, daemon=True)
    th.start()
    time.sleep(0.35)                        # let the wait register before we transmit
    mon.command(cmd)
    th.join(timeout + 20)
    return box


def log_lines(path):
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def last_frame(lines, from_index):
    """Last complete *MEAS_BEGIN..*MEAS_END frame at or after from_index."""
    begin = end = None
    for i in range(len(lines) - 1, from_index - 1, -1):
        if end is None and "*MEAS_END" in lines[i]:
            end = i
        elif end is not None and "*MEAS_BEGIN" in lines[i]:
            begin = i
            break
    if begin is None or end is None:
        return None
    return lines[begin:end + 1]


def strip_prefix(lines):
    """Monitor log lines carry 'HH:MM:SS.mmm << [source=uart] '."""
    out = []
    for ln in lines:
        m = re.match(r"^\d\d:\d\d:\d\d\.\d+ *<< *\[[^\]]*\] ?(.*)$", ln)
        out.append(m.group(1) if m else ln)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url", default="http://127.0.0.1:8080")
    ap.add_argument("--profile", default="sonora")
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--jobs", nargs="+", required=True,
                    help="<row>:<level index> pairs, e.g. 0:5 12:5 12:5")
    ap.add_argument("--leg", choices=sorted(LEG_INDEX), default=None)
    ap.add_argument("--rate", choices=sorted(RATE_INDEX), default=None)
    ap.add_argument("--freeze", action="store_true", help="send *as00 before capturing")
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    mon = Monitor(args.base_url)
    st = mon.status()
    if st.get("profile") != args.profile:
        print("REFUSING: /status profile=%r, expected %r" % (st.get("profile"), args.profile))
        return 2
    if not st.get("connected"):
        print("REFUSING: monitor not connected")
        return 2
    logp = pathlib.Path(st["log_file"])
    print("board: profile=%s port=%s log=%s" % (st["profile"], st["port"], logp.name))

    if args.rate is not None:
        leg = LEG_INDEX[args.leg or "b"]
        cmd = "*ar%s%s" % (leg, RATE_INDEX[args.rate])
        mon.command(cmd)
        print("rate: %s -> leg %s %s" % (cmd, args.leg or "b", args.rate))
        time.sleep(args.settle)

    mon.command("*at01")                   # select high tone row (row is overridden below)
    if args.freeze:
        mon.command("*as00")
        print("servo: frozen (*as00)")
    time.sleep(1.0)

    args.out.mkdir(parents=True, exist_ok=True)
    written = []
    for job in args.jobs:
        row_s, _, lvl_s = job.partition(":")
        row, lvl = int(row_s), int(lvl_s)
        mon.command("*at02%02X" % lvl)   # payload is HEX
        mon.command("*at04%02X" % row)   # payload is HEX: row 18 must be sent as 12
        time.sleep(1.2)

        before = len(log_lines(logp))
        res = armed_command(mon, "*ac", "MEAS", timeout=25)
        if "err" in res:
            print("job %s: arm wait error: %s" % (job, res["err"]))
        time.sleep(1.0)
        before_dump = len(log_lines(logp))
        armed_command(mon, "?ac", "*MEAS_END", timeout=60)

        lines = log_lines(logp)
        frame = last_frame(lines, before_dump)
        if frame is None:
            frame = last_frame(lines, before)
        if frame is None:
            print("job %s: NO FRAME captured" % job)
            continue
        body = strip_prefix(frame)
        hdr = body[0]
        meta = dict(re.findall(r"([A-Za-z0-9_]+)=(\S+)", hdr))
        tone = meta.get("tone_hz", "unknown")
        lv = meta.get("level_dbfs", str(LEVEL_DBFS.get(lvl, lvl)))
        lvtag = "m" + str(abs(float(lv))).rstrip("0").rstrip(".")
        name = "r%02u_%sHz_%s%s.txt" % (row, tone, lvtag, ("_" + args.tag) if args.tag else "")
        dst = args.out / name
        n = 2
        while dst.exists():
            dst = args.out / name.replace(".txt", "_rep%u.txt" % n)
            n += 1
        dst.write_text("\n".join(body) + "\n", encoding="utf-8")
        nsamp = sum(1 for b in body if re.fullmatch(r"-?\d+", b.strip()))
        print("job %-8s -> %-34s tone=%s level=%s samples=%d" % (job, dst.name, tone, lv, nsamp))
        written.append(dst)

    print("captures written: %d" % len(written))
    return 0


if __name__ == "__main__":
    sys.exit(main())
