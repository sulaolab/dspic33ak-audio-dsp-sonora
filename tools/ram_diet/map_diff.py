#!/usr/bin/env python3
"""Section-level diff between two xc-dsc link maps (data and program tables).

Usage: map_diff.py <before.map> <after.map> [data|prog]

Program-memory sections carry a per-build hash in their names, so a name-based
diff of the program table shows churn that means nothing; use
map_rom_report.py --csv and diff by object for that side.
"""
import re
import sys

DATA_HDR = re.compile(r'^section\s+address\s+alignment gaps\s+total length')
PROG_HDR = re.compile(r'^section\s+address\s+length \(PC units\)')
ROW = re.compile(r'^(\S+)\s+0x([0-9a-f]+)\s+.*\((\d+)\)\s*$')
TOTAL = re.compile(r'Total "(program|data)" memory used \(bytes\):\s+0x[0-9a-f]+\s+\((\d+)\)')


def parse(path, which):
    hdr = DATA_HDR if which == 'data' else PROG_HDR
    out, inside, totals = {}, False, {}
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        m = TOTAL.search(line)
        if m:
            totals[m.group(1)] = int(m.group(2))
        if hdr.match(line.strip()):
            inside = True
            continue
        if inside:
            if line.strip().startswith('---'):
                continue
            m = ROW.match(line.strip())
            if not m:
                if line.strip() == '' or 'Total' in line or 'Dynamic' in line:
                    inside = False
                continue
            name, addr, ln = m.group(1), int(m.group(2), 16), int(m.group(3))
            # generated X/Y section names carry a per-build hash prefix; normalise it
            name = re.sub(r'^_[0-9A-F]{16}[0-9a-f]{8}\.', 'XY.', name)
            key = name
            n = 2
            while key in out:          # .data appears several times
                key = f'{name}#{n}'
                n += 1
            out[key] = (addr, ln)
    return out, totals


def main():
    before, after = sys.argv[1], sys.argv[2]
    which = sys.argv[3] if len(sys.argv) > 3 else 'data'
    b, tb = parse(before, which)
    a, ta = parse(after, which)
    keys = sorted(set(b) | set(a))
    gone, new, changed = [], [], []
    for k in keys:
        lb = b.get(k, (0, 0))[1]
        la = a.get(k, (0, 0))[1]
        if k not in a:
            gone.append((lb, k))
        elif k not in b:
            new.append((la, k))
        elif lb != la:
            changed.append((la - lb, k, lb, la))
    print(f'== {which}: removed (present before, gone after) ==')
    for ln, k in sorted(gone, reverse=True):
        print(f'  -{ln:7d}  {k}')
    print(f'  subtotal removed: -{sum(l for l, _ in gone)}')
    print(f'== {which}: added ==')
    for ln, k in sorted(new, reverse=True):
        print(f'  +{ln:7d}  {k}')
    print(f'  subtotal added: +{sum(l for l, _ in new)}')
    print(f'== {which}: resized ==')
    for d, k, lb, la in sorted(changed, key=lambda t: -abs(t[0])):
        print(f'  {d:+8d}  {k}  ({lb} -> {la})')
    print(f'  subtotal resized: {sum(d for d, _, _, _ in changed):+d}')
    net = sum(l for l, _ in new) - sum(l for l, _ in gone) + sum(d for d, _, _, _ in changed)
    print(f'== section-sum net: {net:+d}')
    for kind in ('program', 'data'):
        if kind in tb and kind in ta:
            print(f'== map total "{kind}": {tb[kind]} -> {ta[kind]}  ({ta[kind]-tb[kind]:+d})')


main()
