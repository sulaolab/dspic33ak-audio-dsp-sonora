#!/usr/bin/env python3
"""UART1 Biquad CSV bulk-upload smoke test.

Sends a known-good coefficient CSV file over UART1 ("USB Serial Port") N
times and checks for the "BIQUAD COEFF CSV APPLY OK" marker. A per-transfer
ring_ovf > 0 is a known, occasional UART1 RX ring-buffer overflow (graceful
CSV abort, not a crash/hang) and is reported but does not count as a smoke
failure by itself -- the smoke criterion is "at least one success in N
tries" (see [internal] manual_pkob4_monitor_console.md, CSV bulk-transfer
smoke test section): a build that never succeeds even once is the actual
regression signature, not the per-run flake rate.
"""
import argparse
import pathlib
import sys
import time

import serial

DEFAULT_FILE = pathlib.Path(__file__).parent / "fixtures" / "Sparkly__output_ALL_TREBLE_+12dB.txt"


def drain(ser, duration):
    end = time.time() + duration
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    return buf


def run_once(ser, data, drain_secs):
    ser.reset_input_buffer()
    ser.write(data)
    ser.flush()
    resp = drain(ser, drain_secs).decode(errors="replace")
    ok = "BIQUAD COEFF CSV APPLY OK" in resp
    started = "BIQUAD COEFF CSV RX START" in resp
    ring_ovf = None
    for line in resp.splitlines():
        if "ring_ovf=" in line:
            try:
                ring_ovf = int(line.split("ring_ovf=")[1].split()[0])
            except (IndexError, ValueError):
                pass
    return ok, started, ring_ovf, resp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12", help='UART1 "USB Serial Port" COM port')
    parser.add_argument("--baud", type=int, default=230400)
    parser.add_argument("--file", type=pathlib.Path, default=DEFAULT_FILE)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--drain-secs", type=float, default=3.0)
    parser.add_argument("--pause-secs", type=float, default=1.0, help="delay between repeated sends")
    args = parser.parse_args()

    data = args.file.read_bytes()

    ser = serial.Serial(args.port, args.baud, timeout=0.2, rtscts=False, dsrdtr=False)
    ser.dtr = True
    ser.rts = True
    time.sleep(0.3)

    results = []
    for run in range(1, args.runs + 1):
        ok, started, ring_ovf, resp = run_once(ser, data, args.drain_secs)
        results.append(ok)
        print(f"=== run {run}/{args.runs}: started={started} apply_ok={ok} ring_ovf={ring_ovf} ===")
        if not ok:
            for line in resp.splitlines():
                if "BIQUAD" in line:
                    print("  " + line)
        time.sleep(args.pause_secs)

    ser.close()

    succeeded = sum(results)
    print(f"\nSummary: {results} -> {succeeded}/{args.runs} succeeded")

    if succeeded == 0:
        print("SMOKE FAIL: zero successes out of", args.runs, "tries")
        sys.exit(1)

    print("SMOKE PASS: at least one successful transfer")
    sys.exit(0)


if __name__ == "__main__":
    main()
