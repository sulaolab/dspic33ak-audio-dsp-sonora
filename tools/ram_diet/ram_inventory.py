#!/usr/bin/env python3
"""Complete data-memory inventory from an xc-dsc link map.

Unlike tools/ram_diet/map_ram_report.py this attributes EVERY placed data
section -- including the hash-named space(xmemory)/space(ymemory) sections --
to the object that contributed it, by cross-referencing the placement table
with the input-section listing further down the map.  Prints one row per
section, sorted, plus per-object and per-space totals, and reconciles the
sum against the map's own "Total data memory used".

Usage: ram_inventory.py <map> [--by-object] [--min N]
"""
import re
import sys

MAP = sys.argv[1]
BY_OBJECT = '--by-object' in sys.argv
MIN = 0
if '--min' in sys.argv:
    MIN = int(sys.argv[sys.argv.index('--min') + 1])

text = open(MAP, errors='replace').read()
lines = text.splitlines()

# --- 1. placement table (authoritative addresses and lengths) ---
place = []
inside = False
for ln in lines:
    s = ln.strip()
    if re.match(r'^section\s+address\s+alignment gaps\s+total length', s):
        inside = True
        continue
    if not inside:
        continue
    if s.startswith('---'):
        continue
    m = re.match(r'^(\S+)\s+0x([0-9a-f]+)\s+\d+\s+0x[0-9a-f]+\s+\((\d+)\)$', s)
    if not m:
        if 'Total "data"' in s:
            break
        continue
    place.append((m.group(1), int(m.group(2), 16), int(m.group(3))))

# --- 2. section -> object, from the input-section listing ---
owner = {}
cur = None
for ln in lines:
    m = re.match(r'^ ?(\.\S+|_[0-9A-F]{16}[0-9a-f]{8}\.\S+)\s*$', ln)
    if m:
        cur = m.group(1).strip()
        continue
    m = re.search(r'0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+\.o)\)?$', ln)
    if m and cur:
        obj = m.group(1).split('/')[-1]
        owner.setdefault(cur, obj)

total_map = int(re.search(r'Total "data" memory used \(bytes\):\s+0x[0-9a-f]+\s+\((\d+)\)', text).group(1))
stack = re.search(r'^stack\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+\((\d+)\)', text, re.M)

XY_SPLIT = 0xC000   # AK512MPS512: X = 0x4000..0xBFFF, Y = 0xC000..0x13FFF

rows = []
for name, addr, ln in place:
    obj = owner.get(name, '?')
    space = 'X' if addr < XY_SPLIT else 'Y'
    rows.append((ln, name, addr, obj, space))

print(f'map: {MAP}')
print(f'sections placed: {len(rows)}   sum: {sum(r[0] for r in rows)}   map total: {total_map}')
if stack:
    print(f'stack: {stack.group(3)} B at 0x{stack.group(1)}')

# gaps
prev = 0x4000
gaps = []
for ln, name, addr, obj, space in sorted(rows, key=lambda r: r[2]):
    if addr > prev:
        gaps.append((addr - prev, prev))
    prev = max(prev, addr + ln)
print(f'placement gaps (>=16 B): ' + ', '.join(f'{g} B at 0x{a:x}' for g, a in gaps if g >= 16))

byobj, byspace = {}, {'X': 0, 'Y': 0}
for ln, name, addr, obj, space in rows:
    byobj[obj] = byobj.get(obj, 0) + ln
    byspace[space] += ln
print(f'X space used {byspace["X"]} / 32768   Y space used {byspace["Y"]} / 32768')

if BY_OBJECT:
    print('\n== by object ==')
    for v, k in sorted(((v, k) for k, v in byobj.items()), reverse=True):
        print(f'{v:8d}  {k}')
else:
    print('\n== every section, largest first ==')
    for ln, name, addr, obj, space in sorted(rows, reverse=True):
        if ln >= MIN:
            print(f'{ln:8d}  {space}  0x{addr:05x}  {name:52s} {obj}')
