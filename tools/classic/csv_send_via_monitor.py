"""Send a Biquad coefficient CSV to the board through the serial-monitor bridge.

Why this exists next to test_csv_biquad_smoke.py: that one opens the COM port
directly, which the monitor owns, so it cannot be used while a monitor is running
(and a human may be watching the same port in Tera Term). This one talks only to
the monitor's localhost HTTP API.

The whole file goes out in ONE POST /command (or the atomic /command_wait when a
result marker is requested). That matters: the bridge turns one request into one
write(), so the board sees a continuous 230400-baud stream, which is the
condition under which the receive path is actually interesting. Sending line by
line over HTTP throttles to milliseconds per line and reproduces nothing.

For a result marker this script uses `/command_wait`, whose monitor-side matcher
is armed before the continuous write. `/command` followed by `/wait` is a race:
a fast console reply can arrive between the two HTTP requests and is then lost.
The bridge appends its own line terminator after the payload, which is harmless
(the END line is already framed) but does mean a file with no trailing newline
still arrives with one -- so this cannot exercise the END-at-EOF framing path.

Resolve the base URL, do not assume it: several monitors run at once, one per
board, all on port 8080 but on different loopback aliases, so the wrong board
answers just as healthily as the right one. Pass the URL resolved for Sonora
and check GET /status first, or run
`pwsh ../serial-monitor/start-serial-monitor.ps1 -List`.

Examples:
  python -u csv_send_via_monitor.py fixtures/eq.txt --base http://127.0.0.1:8080
  python -u csv_send_via_monitor.py fixtures/eq.txt --base http://127.0.0.1:8080 --wait "APPLY OK" --timeout 30
  python -u csv_send_via_monitor.py fixtures/eq.txt --base http://127.0.0.1:8080 --truncate 600  # no-END abort test
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request

KEEP = ("BIQUAD", "console hello", "inactive", "drain complete", "tblsum",
        "APPLY", "ABORT", "DIAG", "ring overflow", "raw buffer")


def post(base, path, obj, timeout):
    req = urllib.request.Request(
        base + path,
        data=json.dumps(obj).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def get(base, path, timeout):
    with urllib.request.urlopen(base + path, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--base", required=True,
                    help="resolved monitor base URL; verify GET /status before use")
    ap.add_argument("--expect-profile", default="sonora",
                    help="required monitor profile (default: sonora; use '' to disable)")
    ap.add_argument("--wait", default=None,
                    help='marker to wait for after the send, e.g. "APPLY OK"')
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--tail", type=int, default=40,
                    help="log lines to show afterwards (0 = none)")
    ap.add_argument("--all-lines", action="store_true",
                    help="show every log line, not only CSV-related ones")
    ap.add_argument("--truncate", type=int, default=0,
                    help="send only the first N bytes (deadline / abort tests)")
    args = ap.parse_args()

    status = get(args.base, "/status", 10)
    if not status.get("connected"):
        print("monitor at %s is not connected to a UART" % args.base)
        return 2
    print("monitor: profile=%s port=%s baud=%s"
          % (status.get("profile"), status.get("port"), status.get("baud")))
    if args.expect_profile and status.get("profile") != args.expect_profile:
        print("refusing wrong monitor profile: expected=%s got=%s"
              % (args.expect_profile, status.get("profile")))
        return 2

    with open(args.file, "rb") as f:
        raw = f.read()
    if args.truncate:
        raw = raw[: args.truncate]
    print("file=%s bytes=%d lines=%d" % (args.file, len(raw), raw.count(b"\n")))

    payload = {"cmd": raw.decode("utf-8", "replace")}
    endpoint = "/command"
    if args.wait:
        # The monitor arms this matcher before it writes payload["cmd"].  Keeping
        # that ordering inside one endpoint removes the HTTP round-trip race
        # without fragmenting the CSV's one-write continuous stream.
        endpoint = "/command_wait"
        payload.update({"contains": args.wait, "timeout": args.timeout})

    t0 = time.time()
    code, body = post(args.base, endpoint, payload, args.timeout + 5)
    print("POST %s -> %s  (%.3fs)" % (endpoint, code, time.time() - t0))
    if code == 409:
        print("a transfer holds the transmit gate; nothing was sent")
        return 3
    if code != 200:
        print("body:", body)
        return 2

    if args.wait:
        if isinstance(body, dict) and body.get("line"):
            print("   " + body["line"])

    rc = 0
    if args.tail:
        time.sleep(1.0)
        lines = get(args.base, "/log?tail=%d" % args.tail, 15)["lines"]
        if not args.all_lines:
            lines = [l for l in lines if any(k in l for k in KEEP)]
        print("--- log ---")
        for line in lines:
            print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main())
