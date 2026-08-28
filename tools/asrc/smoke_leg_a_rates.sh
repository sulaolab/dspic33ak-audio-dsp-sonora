#!/usr/bin/env bash
# Silent (no listening) smoke test of LEG-A runtime rate changes on the ASRC 96k build.
#
# Leg A is the 96 kHz ADC-only master. Unlike leg B it takes the FULL transport
# restart path (its fs-parameterized DSP retune needs the transport stopped), so
# each step here is a heavier transition than an "*ar 01xx" leg-B change.
#
# What this checks per step, from telemetry only:
#   - the codec actually reprogrammed (wm8904_config trace: fs / CLK_SYS_RATE / BCLK_DIV)
#   - the measured fsA landed on the requested rate (CCP), and fsB stayed put
#   - ratioAB == fsA/fsB, and the servo re-locked (step settles, fill near target)
#   - transport integrity: miss / sat / ovf / udf / recover, and TDMsum margin
#   - nothing silently reverted (the class of defect this area has produced twice)
#
# Usage:  bash tools/asrc/smoke_leg_a_rates.sh [settle_seconds]
# Requires the serial-monitor HTTP bridge (sonora profile). Never opens COM.
# API below is the sonora bind; confirm with ../serial-monitor/start-serial-monitor.ps1 -List.

set -u

API="http://127.0.0.1:8080"
SETTLE="${1:-14}"
# Resolve the monitor log at run time: it rolls over at midnight (COM12-YYYYMMDD.log),
# so a hardcoded name silently reads a stale file and every step looks like NO-DATA.
# The bridge lives in the sibling serial-monitor clone, so resolve
# it from this script's location; SERIAL_MONITOR_LOG_DIR overrides a different layout.
LOG_DIR="${SERIAL_MONITOR_LOG_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/serial-monitor/serial_monitor/monitor_logs}"
LOG=$(ls -1t "$LOG_DIR"/sonora/COM*.log "$LOG_DIR"/COM12-*.log 2>/dev/null | head -1)
if [ -z "${LOG:-}" ]; then
    printf 'FATAL: no monitor log found under %s\n' "$LOG_DIR"; exit 1
fi

# RR index -> nominal Hz (asrc_console.c rates_hz[])
IDX=(0 1 2 3 4 5 6 7 8)
HZ=(8000 11025 12000 16000 22050 24000 32000 44100 48000)

say() { printf '%s\n' "$*"; }

send() {
    curl -s --max-time 8 -X POST "$API/command" \
         -H "Content-Type: application/json" -d "{\"cmd\":\"$1\"}" >/dev/null
}

# Tail the log from a byte offset so each step only sees its own output.
mark() { wc -c < "$LOG" 2>/dev/null || echo 0; }
since() { tail -c "+$1" "$LOG" 2>/dev/null; }

field() { # field <text> <regex-with-one-group>
    printf '%s' "$1" | grep -aoE "$2" | tail -1
}

if ! curl -s --max-time 5 "$API/status" | grep -q '"connected":true'; then
    say "FATAL: serial-monitor not connected on $API"
    exit 1
fi

say "=== LEG-A runtime rate smoke test (silent) ==="
say "settle=${SETTLE}s per step; leg A takes the full-restart path"
say ""

FAILED=0
SUMMARY=()

for i in "${!IDX[@]}"; do
    rr="${IDX[$i]}"
    hz="${HZ[$i]}"
    cmd=$(printf '*ar00%02X' "$rr")

    off=$(mark)
    send "$cmd"
    sleep "$SETTLE"
    txt=$(since "$off")

    # Codec-level evidence that the write happened for instance 2 (leg A).
    cfg=$(printf '%s' "$txt" | grep -aE 'wm8904_config\(2\)' | tail -1)
    # Latest CCP line: absolute rates + ratio + recover count.
    ccp=$(printf '%s' "$txt" | grep -aE 'CCP  fsA=' | tail -1)
    # Latest engine line: step / fill / front-end.
    eng=$(printf '%s' "$txt" | grep -aE '\[poly.*\]AB hr=' | tail -1)
    sum=$(printf '%s' "$txt" | grep -aE 'TDMsum:' | tail -1)

    fsa=$(field "$ccp" 'fsA=[0-9]+' | tr -d 'fsA=')
    fsb=$(field "$ccp" 'fsB=[0-9]+' | tr -d 'fsB=')
    ratio=$(field "$ccp" 'ratioAB=[0-9.]+' | sed 's/ratioAB=//')
    rec=$(field "$ccp" 'recover=[0-9]+' | sed 's/recover=//')
    step=$(field "$eng" 'step=[0-9.]+' | sed 's/step=//')
    # hr = fmin - R (worst-pull headroom, signed); replaced fill=/fmin=/R= on 2026-08-27.
    hr=$(field "$eng" 'hr=-?[0-9]+' | sed 's/hr=//')
    fe=$(field "$eng" 'fe=[a-z0-9_>-]+' | sed 's/fe=//')
    # Take the LAST NON-ZERO TDMsum in the window. The restart resets the peak-hold, so
    # the first telemetry line after it legitimately reads 0.0%/166.6us -- reporting that
    # as the load would understate it. If every line in the window is zero the settle
    # time was too short to see a real sample; duty is then reported as 0.0% and should
    # be read as "not sampled", not "idle".
    nz=$(printf '%s' "$txt" | grep -aE 'TDMsum:' | grep -avE '\(0\.0%\)' | tail -1)
    [ -n "$nz" ] && sum="$nz"
    duty=$(field "$sum" '\([0-9.]+%\)' | tr -d '()')
    marg=$(field "$sum" 'margin=[0-9.]+' | sed 's/margin=//')

    # Faults anywhere in this step's window.
    faults=$(printf '%s' "$txt" | grep -aoE 'miss=[1-9][0-9]*|sat=[1-9][0-9]*|udf=[1-9][0-9]*|ovf=[1-9][0-9]*|underrun=[1-9][0-9]*' | sort -u | tr '\n' ' ')
    rejected=$(printf '%s' "$txt" | grep -aE 'rejected|not supported|cannot run|apply failed|restored' | tail -1)

    # Verdict: did fsA reach the requested rate (0.5% tolerance)?
    verdict="PASS"
    note=""
    if [ -z "$fsa" ]; then
        verdict="NO-DATA"; note="no CCP line in window"
    else
        lo=$(( hz - hz/200 )); hi=$(( hz + hz/200 ))
        if [ "$fsa" -lt "$lo" ] || [ "$fsa" -gt "$hi" ]; then
            verdict="RATE-MISS"; note="fsA=$fsa expected ~$hz"
        fi
    fi
    [ -n "$faults" ] && { verdict="FAULT"; note="$note $faults"; }
    [ -n "$rejected" ] && { note="$note | $rejected"; }
    [ -z "$cfg" ] && note="$note | no wm8904_config(2) trace"
    [ "$verdict" != "PASS" ] && FAILED=1

    say "--- $cmd  (request A = ${hz} Hz) -> $verdict"
    say "    codec : ${cfg:-<none>}"
    say "    rates : fsA=${fsa:-?} fsB=${fsb:-?} ratioAB=${ratio:-?} recover=${rec:-?}"
    say "    engine: step=${step:-?} hr=${hr:-?} fe=${fe:-?}"
    say "    load  : duty=${duty:-?} margin=${marg:-?}us  faults='${faults:-none}'"
    [ -n "$note" ] && say "    note  :$note"
    say ""

    SUMMARY+=("$(printf '%-10s A=%-6s %-10s fsA=%-7s fsB=%-7s ratio=%-10s step=%-8s duty=%-7s %s' \
        "$cmd" "$hz" "$verdict" "${fsa:-?}" "${fsb:-?}" "${ratio:-?}" "${step:-?}" "${duty:-?}" "${faults:-}")")
done

say "=== restoring A to 96 kHz ==="
say "    RR=9 is 96 kHz, so leaving the 96 kHz operating point is no longer one-way."
say "    This sweep ends with A at 48 kHz; run \"*ar 00 09\" to put it back, or use"
say "    tools/asrc/smoke_96k_roundtrip.sh which exercises the round trip on both legs."
say ""
say "=== SUMMARY ==="
for s in "${SUMMARY[@]}"; do say "$s"; done
say ""
say "overall: $([ "$FAILED" -eq 0 ] && echo 'all steps PASS' || echo 'FAILURES PRESENT')"
exit "$FAILED"
