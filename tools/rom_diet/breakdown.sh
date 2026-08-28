#!/bin/sh
# text vs const split for one object, out of the link map.
obj=${1:-wm8904}; cfg=${2:-dsPIC33AK128_SERIAL_UPDATE}
M="dspic33ak_audio_dsp.X/dist/$cfg/production/dspic33ak_audio_dsp.X.production.map"
python - "$M" "$obj" <<'PY'
import re,sys
m,obj=sys.argv[1],sys.argv[2]
lines=open(m,encoding='utf-8',errors='replace').read().splitlines()
# The map writes a section either as "<name>\n  <addr> <size> <obj>" or, when the
# name is short enough, "<name> <addr> <size>\n <name> <addr> <size> <obj>".
pat=re.compile(r'^(.*?)\s*(0x8[0-9a-f]{5})\s+(0x[0-9a-f]+) \S*/'+re.escape(obj)+r'\.o\s*$')
text=const=0; rows=[]
for i,l in enumerate(lines):
    mm=pat.match(l)
    if not mm: continue
    n=int(mm.group(3),16)
    name=mm.group(1).strip()
    if not name:
        prev=lines[i-1].split() if i else []
        name=prev[0] if prev else '?'
    rows.append((n,name))
    if name.startswith('.text'): text+=n
    else: const+=n
rows.sort(reverse=True)
print(f"{obj}: text={text}  const/str={const}  total={text+const}  ({len(rows)} sections)")
for n,name in rows[:16]: print(f"   {n:6d}  {name}")
PY
