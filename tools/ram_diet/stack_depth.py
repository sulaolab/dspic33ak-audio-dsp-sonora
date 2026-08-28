#!/usr/bin/env python3
"""Static worst-case stack estimate from an xc-dsc (dsPIC33A) disassembly.

Frame per function = 4 B per prologue push (mov.l wN, [w15++]) plus the largest
"add.l w15, #imm, w15" allocation in its body; a call costs 4 B for the return
address.  Depth is the longest path through the DIRECT call graph, so the result
is a LOWER bound -- calls through function pointers are invisible here, and this
tree dispatches the audio block and the console through pointers.

Usage: stack_depth.py <disassembly> [root ...]
"""
import re
import sys
from collections import defaultdict

FUNC  = re.compile(r'^([0-9a-f]{6,8}) <([^>]+)>:$')
PUSH  = re.compile(r'\bmov\.[lwb]\s+w\d+, \[w15\+\+\]')
ALLOC = re.compile(r'\badd\.l\s+w15, #0x([0-9a-f]+), w15')
CALL  = re.compile(r'\b(?:rcall|call)\s+0x[0-9a-f]+ (.*)$')
SYM   = re.compile(r'<([^>]+)>')
LOCAL = re.compile(r'^L[0-9]')

def is_label(name):
    return name.startswith('.') or LOCAL.match(name) is not None

def pick(symlist):
    """objdump prints EVERY symbol sharing the target address, and not
    function-first: '<.LFE20> <_nora_high_res_timer_is_initialized> <L0>'.
    Take the first entry that is not a compiler-local label."""
    for s in SYM.findall(symlist):
        if not is_label(s):
            return s
    return None

frames, edges = {}, defaultdict(set)
cur = None
for line in open(sys.argv[1], errors='replace'):
    m = FUNC.match(line.strip())
    if m:
        name = m.group(2)
        if is_label(name):
            continue          # a local label, not a new function
        cur = name
        frames.setdefault(cur, 0)
        continue
    if cur is None:
        continue
    if PUSH.search(line):
        frames[cur] += 4
    m = ALLOC.search(line)
    if m:
        frames[cur] += int(m.group(1), 16)
    m = CALL.search(line)
    if m:
        tgt = pick(m.group(1))
        if tgt:
            edges[cur].add(tgt)

CALL_COST = 4
memo, onstack, chain, recursive = {}, set(), {}, set()

def depth(fn):
    if fn in memo:
        return memo[fn]
    if fn in onstack:
        recursive.add(fn)
        return 0
    onstack.add(fn)
    best, worst_child = 0, None
    for callee in sorted(edges.get(fn, ())):
        d = depth(callee)
        if d > best:
            best, worst_child = d, callee
    onstack.discard(fn)
    chain[fn] = worst_child
    memo[fn] = frames.get(fn, 0) + CALL_COST + best
    return memo[fn]

roots = sys.argv[2:]
if not roots:
    for fn in frames:
        depth(fn)
    roots = sorted(memo, key=lambda f: -memo[f])[:10]
for r in roots:
    if r not in frames:
        print('%-34s (not in image)' % r)
        continue
    print('%-34s worst-case %6d B' % (r, depth(r)))
    fn, path = r, []
    while fn and len(path) < 16:
        path.append('%s(%d)' % (fn, frames.get(fn, 0)))
        fn = chain.get(fn)
    print('    ' + ' -> '.join(path))
if recursive:
    print('\nrecursion cut at: ' + ', '.join(sorted(recursive)))
print('\n== 12 largest single frames ==')
for fn, sz in sorted(frames.items(), key=lambda kv: -kv[1])[:12]:
    print('%6d  %s' % (sz, fn))
