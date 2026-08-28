#!/usr/bin/env bash
# Silent smoke test of LEG-B low-rate operation against a 96 kHz leg A.
#
# This targets the range that BLOCK=16 could not serve. The per-pull look-ahead is
#   R(step) = floor(step*(BLOCK-1)) + ASRC_POLY_AHEAD + 1
# and the ring can offer at most ASRC_FILL_TARGET_MAX = FIFO-4-BLOCK-jitter. With
# FIFO=128/BLOCK=16 that is 104, so fs_B <= 16 kHz needs 110..200 and gets CLAMPED --
# the block's tail outputs fail the window test, emit zeros and hold rd, which is the
# audible break-up. BLOCK=8 lowers R and raises the cap to 112, covering 8 k..96 k.
#
# The decisive observable is therefore the reported fill vs its setpoint: if the
# setpoint was clamped, fill sits ABOVE `set=` by a standing error instead of
# tracking it. That is checked here, alongside the fault counters.
#
# Usage:  bash tools/asrc/smoke_lowrate_b.sh [settle_seconds]
# Requires the serial-monitor HTTP bridge (sonora profile). Never opens COM.
# API below is the sonora bind; confirm with ../serial-monitor/start-serial-monitor.ps1 -List.

set -u

API="http://127.0.0.1:8080"
SETTLE="${1:-16}"
# The monitor log rolls over at midnight; resolve the newest rather than hardcoding.
# The bridge lives in the sibling serial-monitor clone, so resolve
# it from this script's location; SERIAL_MONITOR_LOG_DIR overrides a different layout.
LOG_DIR="${SERIAL_MONITOR_LOG_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/serial-monitor/serial_monitor/monitor_logs}"
LOG=$(ls -1t "$LOG_DIR"/sonora/COM*.log "$LOG_DIR"/COM12-*.log 2>/dev/null | head -1)
if [ -z "${LOG:-}" ]; then
    printf 'FATAL: no monitor log found under %s\n' "$LOG_DIR"; exit 1
fi

# RR index -> nominal Hz, low-rate end first (the previously-broken range).
IDX=(0 1 2 3 4 9)
HZ=(8000 11025 12000 16000 22050 96000)

say() { printf '%s\n' "$*"; }
send() { curl -s --max-time 8 -X POST "$API/command" \
              -H "Content-Type: application/json" -d "{\"cmd\":\"$1\"}" >/dev/null; }
mark() { wc -c < "$LOG" 2>/dev/null || echo 0; }
since() { tail -c "+$1" "$LOG" 2>/dev/null; }
field() { printf '%s' "$1" | grep -aoE "$2" | tail -1; }

if ! curl -s --max-time 5 "$API/status" | grep -q '"connected":true'; then
    say "FATAL: serial-monitor not connected on $API"; exit 1
fi

say "=== LEG-B low-rate smoke test vs 96 kHz leg A (silent) ==="
say "settle=${SETTLE}s per step"
say ""

FAILED=0
SUMMARY=()

for i in "${!IDX[@]}"; do
    rr="${IDX[$i]}"; hz="${HZ[$i]}"
    cmd=$(printf '*ar01%02X' "$rr")
    off=$(mark)
    send "$cmd"
    sleep "$SETTLE"
    txt=$(since "$off")

    ccp=$(printf '%s' "$txt" | grep -aE 'CCP  fsA=' | tail -1)
    eng=$(printf '%s' "$txt" | grep -aE '\[poly.*\]AB hr=' | tail -1)
    sum=$(printf '%s' "$txt" | grep -aE 'TDMsum:' | grep -avE '\(0\.0%\)' | tail -1)

    fsa=$(field "$ccp" 'fsA=[0-9]+' | sed 's/fsA=//')
    fsb=$(field "$ccp" 'fsB=[0-9]+' | sed 's/fsB=//')
    ratio=$(field "$ccp" 'ratioAB=[0-9.]+' | sed 's/ratioAB=//')
    stepv=$(field "$eng" 'step=[0-9.]+' | sed 's/step=//')
    hr=$(field "$eng" 'hr=-?[0-9]+' | sed 's/hr=//')
    setp=$(field "$eng"  'set=[0-9]+' | sed 's/set=//')
    capped=$(printf '%s' "$eng" | grep -aoE 'set=[0-9]+!' || true)
    fe=$(field "$eng" 'fe=[a-z0-9_>-]+' | sed 's/fe=//')
    duty=$(field "$sum" '\([0-9.]+%\)' | tr -d '()')
    marg=$(field "$sum" 'margin=[0-9.]+' | sed 's/margin=//')

    faults=$(printf '%s' "$txt" | grep -aoE 'miss=[1-9][0-9]*|sat=[1-9][0-9]*|udf=[1-9][0-9]*|ovf=[1-9][0-9]*|underrun=[1-9][0-9]*' | sort -u | tr '\n' ' ')

    verdict="PASS"; note=""
    if [ -z "$fsb" ]; then
        verdict="NO-DATA"; note="no CCP line in window"
    else
        lo=$(( hz - hz/200 )); hi=$(( hz + hz/200 ))
        if [ "$fsb" -lt "$lo" ] || [ "$fsb" -gt "$hi" ]; then
            verdict="RATE-MISS"; note="fsB=$fsb expected ~$hz"
        fi
    fi
    # The clamp used to be inferred from |fill - set| > 8. The firmware now states it directly:
    # a trailing '!' on set= means R(step) did not fit the ring. Negative hr is the other half --
    # the worst pull had less than the look-ahead it needed.
    if [ -n "${capped:-}" ]; then
        verdict="FILL-CLAMP"; note="$note set=$setp! (look-ahead does not fit the ring)"
    elif [ -n "${hr:-}" ] && [ "$hr" -lt 0 ]; then
        verdict="FILL-OFFSET"; note="$note hr=$hr (worst pull short of its look-ahead)"
    fi
    [ -n "$faults" ] && { verdict="FAULT"; note="$note $faults"; }
    [ "$verdict" != "PASS" ] && FAILED=1

    say "--- $cmd  (B = ${hz} Hz) -> $verdict"
    say "    fsA=${fsa:-?} fsB=${fsb:-?} ratio=${ratio:-?} step=${stepv:-?} fe=${fe:-?}"
    say "    hr=${hr:-?} set=${setp:-?} duty=${duty:-?} margin=${marg:-?}us faults='${faults:-none}'"
    [ -n "$note" ] && say "    note :$note"
    say ""
    SUMMARY+=("$(printf '%-10s B=%-6s %-12s fsB=%-7s ratio=%-9s step=%-8s hr=%-4s set=%-4s duty=%-7s %s' \
        "$cmd" "$hz" "$verdict" "${fsb:-?}" "${ratio:-?}" "${stepv:-?}" "${hr:-?}" "${setp:-?}" "${duty:-?}" "${faults:-}")")
done

say "=== SUMMARY (last step returns B to 96 kHz) ==="
for s in "${SUMMARY[@]}"; do say "$s"; done
say ""
say "overall: $([ "$FAILED" -eq 0 ] && echo 'all steps PASS' || echo 'FAILURES PRESENT')"
exit "$FAILED"
