#!/usr/bin/env python3
"""Attribute a link map's data-memory bytes to object files and symbols.

The ROM-diet report (tools/rom_diet/map_rom_report.py) does this for program
memory.  RAM needs the same view: which object owns which .nbss/.ybss/.data
bytes, because the totals line only says how much is gone, not where.
"""
import re
import sys
from collections import defaultdict

INPUT = re.compile(
    r'^\s+(\.[\w.$-]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$')
# A section whose name is too long wraps: name on one line, numbers on the next.
WRAPPED = re.compile(r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$')
NAME_ONLY = re.compile(r'^\s+(\.[\w.$-]+)\s*$')

def parse(path):
    rows = []
    with open(path, 'r', errors='replace') as fh:
        lines = fh.read().splitlines()
    try:
        start = next(i for i, l in enumerate(lines)
                     if l.startswith('Linker script and memory map'))
    except StopIteration:
        start = 0
    pending = None
    for line in lines[start:]:
        m = INPUT.match(line)
        if m:
            pending = None
            name, addr, size, obj = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            rows.append((name, addr, size, obj))
            continue
        m = NAME_ONLY.match(line)
        if m:
            pending = m.group(1)
            continue
        m = WRAPPED.match(line)
        if m and pending:
            addr, size, obj = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
            rows.append((pending, addr, size, obj))
            pending = None
            continue
        pending = None
    return rows

def short(obj):
    return obj.rsplit('/', 1)[-1]

def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    rows = [r for r in parse(path)
            if 0x4000 <= r[1] < 0x800000 and r[2] > 0 and r[3].endswith('.o')
            and not r[0].startswith(('.debug', '.comment', '.dinit'))]
    by_obj = defaultdict(int)
    for name, addr, size, obj in rows:
        by_obj[short(obj)] += size
    total = sum(by_obj.values())
    print('== data memory by object (attributed %d B) ==' % total)
    for obj, size in sorted(by_obj.items(), key=lambda kv: -kv[1])[:limit]:
        print('%8d  %s' % (size, obj))
    print()
    print('== largest individual data objects ==')
    for name, addr, size, obj in sorted(rows, key=lambda r: -r[2])[:limit]:
        print('%8d  %-42s %s' % (size, name, short(obj)))

main()
