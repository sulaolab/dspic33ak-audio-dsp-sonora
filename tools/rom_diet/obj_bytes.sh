#!/bin/sh
# Per-object program-memory bytes for one object, straight out of the link map.
# usage: obj_bytes.sh <object-basename> [config]
obj=${1:-wm8904}
cfg=${2:-dsPIC33AK128_SERIAL_UPDATE}
M="dspic33ak_audio_dsp.X/dist/$cfg/production/dspic33ak_audio_dsp.X.production.map"
grep -oP "\s0x8[0-9a-f]{5}\s+0x[0-9a-f]+ \S*/$obj\.o" "$M" |
  awk '{s+=strtonum($2); n++} END{printf "%-12s sections=%-3d bytes=%d\n","'"$obj"'",n,s}'
grep -oP "^\.text\.\S+\s+0x8[0-9a-f]{5}\s+0x[0-9a-f]+" "$M" >/dev/null
