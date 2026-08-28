#!/usr/bin/env bash
# Silent round-trip smoke test of the *ar 96 kHz entry (RR=9), both legs.
#
# The point of RR=9 is that leaving 96 kHz is no longer one-way: before it existed,
# once a leg dropped to the 8k..48k menu it could only return via reset/reflash.
# So this test deliberately goes AWAY from 96 kHz and BACK, on each leg, and checks
# the ratio lands where it should each time.
#
# Expected ratios (ratioAB = fsA/fsB):
#   A=96k B=96k -> 1.0    A=48k B=96k -> 0.5    A=96k B=48k -> 2.0
#
# Verdict per step comes from telemetry only (no listening): measured fsA/fsB
# against the request, ratio against the expectation, plus the fault counters.
#
# Usage:  bash tools/asrc/smoke_96k_roundtrip.sh [settle_seconds]
# Requires the serial-monitor HTTP bridge (sonora profile). Never opens COM.
# API below is the sonora bind; confirm with ../serial-monitor/start-serial-monitor.ps1 -List.

set -u

API="http://127.0.0.1:8080"
SETTLE="${1:-16}"
# Resolve the monitor log at run time: it rolls over at midnight (COM12-YYYYMMDD.log),
# so a hardcoded name silently reads a stale file and every step looks like NO-DATA.
# The bridge lives in the sibling serial-monitor clone, so resolve
# it from this script's location; SERIAL_MONITOR_LOG_DIR overrides a different layout.
LOG_DIR="${SERIAL_MONITOR_LOG_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)/serial-monitor/serial_monitor/monitor_logs}"
LOG=$(ls -1t "$LOG_DIR"/sonora/COM*.log "$LOG_DIR"/COM12-*.log 2>/dev/null | head -1)
if [ -z "${LOG:-}" ]; then
    printf 'FATAL: no monitor log found under %s\n' "$LOG_DIR"; exit 1
fi

RR_48K=8
RR_96K=9

say() { printf '%s\n' "$*"; }
send() {
    curl -s --max-time 8 -X POST "$API/command" \
         -H "Content-Type: application/json" -d "{\"cmd\":\"$1\"}" >/dev/null
}
mark() { wc -c < "$LOG" 2>/dev/null || echo 0; }
since() { tail -c "+$1" "$LOG" 2>/dev/null; }
field() { printf '%s' "$1" | grep -aoE "$2" | tail -1; }

if ! curl -s --max-time 5 "$API/status" | grep -q '"connected":true'; then
    say "FATAL: serial-monitor not connected on $API"; exit 1
fi

FAILED=0
SUMMARY=()

# step <label> <CC> <RR> <expect_fsA> <expect_fsB> <expect_ratio>
step() {
    local label="$1" cc="$2" rr="$3" efa="$4" efb="$5" eratio="$6"
    local cmd; cmd=$(printf '*ar%02X%02X' "$cc" "$rr")
    local off; off=$(mark)

    send "$cmd"
    sleep "$SETTLE"
    local txt; txt=$(since "$off")

    local ccp eng sum
    ccp=$(printf '%s' "$txt" | grep -aE 'CCP  fsA=' | tail -1)
    eng=$(printf '%s' "$txt" | grep -aE '\[poly.*\]AB hr=' | tail -1)
    # Skip the post-restart peak-hold reset line (0.0%) when looking at load.
    sum=$(printf '%s' "$txt" | grep -aE 'TDMsum:' | grep -avE '\(0\.0%\)' | tail -1)

    local fsa fsb ratio rec stepv fill duty marg
    fsa=$(field "$ccp" 'fsA=[0-9]+' | sed 's/fsA=//')
    fsb=$(field "$ccp" 'fsB=[0-9]+' | sed 's/fsB=//')
    ratio=$(field "$ccp" 'ratioAB=[0-9.]+' | sed 's/ratioAB=//')
    rec=$(field "$ccp" 'recover=[0-9]+' | sed 's/recover=//')
    stepv=$(field "$eng" 'step=[0-9.]+' | sed 's/step=//')
    # hr = fmin - R (worst-pull headroom, signed); replaced fill=/fmin=/R= on 2026-08-27.
    hr=$(field "$eng" 'hr=-?[0-9]+' | sed 's/hr=//')
    duty=$(field "$sum" '\([0-9.]+%\)' | tr -d '()')
    marg=$(field "$sum" 'margin=[0-9.]+' | sed 's/margin=//')

    local faults rejected
    faults=$(printf '%s' "$txt" | grep -aoE 'miss=[1-9][0-9]*|sat=[1-9][0-9]*|udf=[1-9][0-9]*|ovf=[1-9][0-9]*' | sort -u | tr '\n' ' ')
    rejected=$(printf '%s' "$txt" | grep -aE 'not available in this build|rejected' | tail -1)

    local verdict="PASS" note=""
    if [ -z "$fsa" ] || [ -z "$fsb" ]; then
        verdict="NO-DATA"; note="no CCP line in window"
    else
        for pair in "A:$fsa:$efa" "B:$fsb:$efb"; do
            IFS=: read -r who got want <<<"$pair"
            lo=$(( want - want/200 )); hi=$(( want + want/200 ))
            if [ "$got" -lt "$lo" ] || [ "$got" -gt "$hi" ]; then
                verdict="RATE-MISS"; note="$note fs$who=$got expected ~$want;"
            fi
        done
        # ratio within 1%
        if [ -n "$ratio" ]; then
            r_x1000=$(printf '%.0f' "$(echo "$ratio 1000" | awk '{print $1*$2}')")
            e_x1000=$(printf '%.0f' "$(echo "$eratio 1000" | awk '{print $1*$2}')")
            d=$(( r_x1000 - e_x1000 )); [ "$d" -lt 0 ] && d=$(( -d ))
            lim=$(( e_x1000/100 )); [ "$lim" -lt 5 ] && lim=5
            [ "$d" -gt "$lim" ] && { verdict="RATIO-MISS"; note="$note ratio=$ratio expected ~$eratio;"; }
        fi
    fi
    [ -n "$faults" ] && { verdict="FAULT"; note="$note $faults"; }
    [ -n "$rejected" ] && note="$note | $rejected"
    [ "$verdict" != "PASS" ] && FAILED=1

    say "--- $label : $cmd -> $verdict"
    say "    fsA=${fsa:-?} fsB=${fsb:-?} ratio=${ratio:-?} (expect ~$eratio) recover=${rec:-?}"
    say "    step=${stepv:-?} hr=${hr:-?} duty=${duty:-?} margin=${marg:-?}us faults='${faults:-none}'"
    [ -n "$note" ] && say "    note :$note"
    say ""
    SUMMARY+=("$(printf '%-26s %-11s fsA=%-7s fsB=%-7s ratio=%-9s duty=%-7s %s' \
        "$label" "$verdict" "${fsa:-?}" "${fsb:-?}" "${ratio:-?}" "${duty:-?}" "${faults:-}")")
}

say "=== *ar 96 kHz (RR=9) round-trip smoke test, both legs ==="
say "settle=${SETTLE}s per step"
say ""

# Bring both legs to a known 96k/96k baseline first (idempotent if already there).
step "baseline A->96k"        0 $RR_96K 96000 96000 1.0
step "baseline B->96k"        1 $RR_96K 96000 96000 1.0

# Leg A round trip: 96k -> 48k -> 96k
step "A down 96k->48k"        0 $RR_48K 48000 96000 0.5
step "A back  48k->96k"       0 $RR_96K 96000 96000 1.0

# Leg B round trip: 96k -> 48k -> 96k
step "B down 96k->48k"        1 $RR_48K 96000 48000 2.0
step "B back  48k->96k"       1 $RR_96K 96000 96000 1.0

say "=== SUMMARY ==="
for s in "${SUMMARY[@]}"; do say "$s"; done
say ""
say "overall: $([ "$FAILED" -eq 0 ] && echo 'all steps PASS' || echo 'FAILURES PRESENT')"
exit "$FAILED"
