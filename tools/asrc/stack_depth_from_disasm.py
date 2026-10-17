#!/usr/bin/env python3
"""Worst-case stack depth from an xc-dsc-objdump -d listing (dsPIC33A).

Host-only, static. The stack grows UPWARD on this part (w15 post-increment), so a
frame allocation is `add.l #imm, w15` (2- or 3-operand) and a save is
`mov.l wN, [w15++]` / `push[.l] X`.

METHOD, and what it can and cannot say
  * Per function: walk the instructions in ADDRESS order, keep a running w15
    delta, and take the maximum reached. That is exact for the straight-line
    prologue/epilogue shape xc-dsc emits and an over-estimate wherever a
    conditional branch skips an allocation -- the safe direction.
  * Per call site: the delta at that point, plus 4 bytes for the pushed return
    address, is charged to the callee's own requirement.
  * Deepest path: DFS with memoisation over the resolved call graph.
  * Indirect calls (`call w0`, `call [w2]`, and a `call 0x...` whose target is
    not a function start in this listing) have no target here. They are handled
    in TWO steps, because step 1 alone is not a bound:
      1. RESOLVED-ONLY, any function as entry. This finds the deepest chain in
         the image even when no resolved edge reaches it (the transport client
         vtable is exactly that case), so it bounds the indirect call's TARGET.
         It charges NOTHING for the stack the caller has already pushed at the
         call site, so on its own it is NOT a worst case.
      2. COMPOSED. Every indirect call site is given an edge to a synthetic ANY
         node whose depth is the deepest chain in the image, and the whole thing
         is iterated to a fixed point. This does add the caller-side prefix
         (`sp_at + 4` at the site, plus everything above the caller).
         The candidate target set cannot be narrowed from this input: the
         function pointers live in const data, and `mov #<code addr>, wN` never
         appears for them, so an address-taken test needs `objdump -s` over
         .rodata, not `-d`. ANY = every non-ISR function is therefore the
         assumption. If a function that itself holds an indirect call site sits
         on the deepest chain, that assumption makes the model genuinely
         unbounded, and the iteration reports that instead of a number.
  * Interrupts are separate roots. A handler can preempt the main line, and a
    higher-priority handler can preempt a lower one, so the reported worst case
    is main-line depth plus one chain per distinct priority above it.
"""
from __future__ import annotations
import re
import sys

FUNC_RE = re.compile(r'^([0-9a-f]+) <([^>]+)>:')
INSN_RE = re.compile(r'^\s+([0-9a-f]+):\t[0-9a-f ]+\t(.*)$')
CALL_RE = re.compile(r'^(rcall|call)\b[a-z.]*\s+0x([0-9a-f]+)')
# register-indirect: `call w0`, `call.l w1`, `call [w2]`, `call.l [w3++]`
CALL_IND_RE = re.compile(
    r'^(rcall|call)\b[a-z.]*\s+(?:w\d+|\[\s*-{0,2}w\d+[^\]]*\])\s*$')

ALLOC_RE = [
    re.compile(r'^add\.l\s+#(0x[0-9a-f]+|\d+),\s*w15$'),
    re.compile(r'^add\.l\s+w15,\s*#(0x[0-9a-f]+|\d+),\s*w15$'),
]
FREE_RE = [
    re.compile(r'^sub\.l\s+#(0x[0-9a-f]+|\d+),\s*w15$'),
    re.compile(r'^sub\.l\s+w15,\s*#(0x[0-9a-f]+|\d+),\s*w15$'),
]
PUSH_RE = re.compile(r'^(?:mov\.l\s+\S+,\s*\[w15\+\+\]|push(\.l)?\s+\S+)$')
PUSHD_RE = re.compile(r'^push\.d\s+\S+$')
POP_RE = re.compile(r'^(?:mov\.l\s+\[--w15\],\s*\S+|pop(\.l)?\s+\S+)$')
POPD_RE = re.compile(r'^pop\.d\s+\S+$')

RET_ADDR_BYTES = 4
MAX_INDIRECT_LEVELS = 8


def imm(txt: str) -> int:
    return int(txt, 16) if txt.startswith('0x') else int(txt)


def parse(path: str):
    """-> (funcs, start2name) ; funcs[name] = dict(own, calls, ind, addr)

    `calls` is [(callee, sp_at)]. `ind` is [(kind, sp_at)] for every call site
    whose target this listing cannot name: kind 'reg' for register-indirect,
    'abs' for an absolute target that is not a known function start.
    """
    start2name: dict[int, str] = {}
    lines = []
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            line = line.rstrip('\n')
            m = FUNC_RE.match(line)
            if m:
                addr, name = int(m.group(1), 16), m.group(2)
                lines.append(('LBL', addr, name))
                if not name.startswith('.'):
                    start2name.setdefault(addr, name)
                continue
            m = INSN_RE.match(line)
            if m:
                lines.append(('INS', int(m.group(1), 16), m.group(2).strip()))

    funcs: dict[str, dict] = {}
    cur = None
    sp = 0
    peak = 0
    for kind, addr, text in lines:
        if kind == 'LBL':
            if not text.startswith('.'):
                if cur is not None:
                    funcs[cur]['own'] = peak
                cur = text
                funcs.setdefault(cur, {'own': 0, 'calls': [], 'ind': [],
                                       'addr': addr})
                sp = 0
                peak = 0
            continue
        if cur is None:
            continue
        t = re.sub(r'\s+', ' ', text).strip()
        hit = False
        for rx in ALLOC_RE:
            m = rx.match(t)
            if m:
                sp += imm(m.group(1)); hit = True; break
        if not hit:
            for rx in FREE_RE:
                m = rx.match(t)
                if m:
                    sp -= imm(m.group(1)); hit = True; break
        if not hit:
            if PUSHD_RE.match(t):
                sp += 8; hit = True
            elif PUSH_RE.match(t):
                sp += 4; hit = True
            elif POPD_RE.match(t):
                sp -= 8; hit = True
            elif POP_RE.match(t):
                sp -= 4; hit = True
        if sp > peak:
            peak = sp
        m = CALL_RE.match(t)
        if m:
            tgt = int(m.group(2), 16)
            if tgt in start2name:
                funcs[cur]['calls'].append((start2name[tgt], sp))
            else:
                funcs[cur]['ind'].append(('abs', sp))
        elif CALL_IND_RE.match(t):
            funcs[cur]['ind'].append(('reg', sp))
    if cur is not None:
        funcs[cur]['own'] = peak
    return funcs, start2name


def deepest(funcs: dict, root: str, any_depth: int = 0):
    """-> (bytes, path, cycles) ; recursion is cut and flagged.

    `any_depth` is the depth charged to an indirect call site (the ANY node).
    0 reproduces the resolved-edges-only walk.
    """
    memo: dict[str, tuple[int, list[str]]] = {}
    onstack: set[str] = set()
    cycles: set[str] = set()

    def go(name: str):
        if name in memo:
            return memo[name]
        if name in onstack:
            cycles.add(name)
            return (0, ['<recursion>'])
        f = funcs.get(name)
        if f is None:
            return (0, [])
        onstack.add(name)
        best = (0, [])
        for callee, sp_at in f['calls']:
            sub, path = go(callee)
            cost = sp_at + RET_ADDR_BYTES + sub
            if cost > best[0]:
                best = (cost, [callee] + path)
        if any_depth:
            for kind, sp_at in f['ind']:
                cost = sp_at + RET_ADDR_BYTES + any_depth
                if cost > best[0]:
                    best = (cost, ['<indirect:' + kind + '>'])
        onstack.discard(name)
        # the function's own peak may exceed any call-point depth
        total = max(f['own'], best[0])
        res = (total, best[1] if best[0] >= f['own'] else [])
        memo[name] = res
        return res

    total, path = go(root)
    return total, path, cycles


def global_max(funcs: dict, any_depth: int = 0):
    """-> sorted [(depth, name, path)] over every non-ISR function as entry."""
    out = []
    for n in funcs:
        if n.endswith('Interrupt'):
            continue
        d, p, _ = deepest(funcs, n, any_depth)
        out.append((d, n, p))
    out.sort(reverse=True)
    return out


def main(path: str, label: str):
    funcs, start2name = parse(path)
    roots_isr = sorted(n for n in funcs if n.endswith('Interrupt'))
    n_reg = sum(1 for f in funcs.values() for k, _ in f['ind'] if k == 'reg')
    n_abs = sum(1 for f in funcs.values() for k, _ in f['ind'] if k == 'abs')
    n_fn = sum(1 for f in funcs.values() if f['ind'])
    print('==== ' + label + ' : ' + str(len(funcs)) + ' functions, '
          + str(n_reg) + ' register-indirect + ' + str(n_abs)
          + ' unnamed-absolute call sites in ' + str(n_fn) + ' functions')

    main_root = '_main' if '_main' in funcs else None
    reset = [n for n in funcs if n in ('__reset', '__resetPRI', '_start')]
    results = []
    for r in ([main_root] if main_root else []) + reset:
        d, p, cy = deepest(funcs, r)
        results.append((d, r, p, cy))
    for d, r, p, cy in sorted(results, reverse=True):
        print(f'  MAINLINE {r:28s} {d:6d} B  (resolved edges only)')
        print(f'           path: {" -> ".join(p[:12])}')
        if cy:
            print(f'           recursion cut at: {sorted(cy)}')

    isr = []
    for r in roots_isr:
        d, p, cy = deepest(funcs, r)
        isr.append((d, r, p))
    isr.sort(reverse=True)
    print(f'  ISR roots: {len(isr)}')
    for d, r, p in isr[:8]:
        print(f'    {r:34s} {d:6d} B   {" -> ".join(p[:6])}')
    tot_isr = sum(d for d, _, _ in isr)
    print(f'    sum of ALL ISR roots (absolute upper bound) = {tot_isr} B')
    print(f'    sum of top 4 ISR roots                      = '
          f'{sum(d for d, _, _ in isr[:4])} B')

    # STEP 1 -- resolved edges only, any function as entry. Bounds an indirect
    # call's TARGET (the deepest chain is found even with no resolved edge into
    # it) but charges nothing for the caller's stack at the site.
    allroots = global_max(funcs)
    d0 = allroots[0][0] if allroots else 0
    isr1 = sorted((deepest(funcs, r, d0)[0], r) for r in roots_isr)
    print(f'    sum of ALL ISR roots, one indirect level each = '
          f'{sum(d for d, _ in isr1)} B')
    print('  GLOBAL deepest chains, RESOLVED edges only (any function as entry;')
    print('  bounds an indirect TARGET, not the caller-side prefix):')
    for d, n, p in allroots[:5]:
        print(f'    {d:6d} B  {n}')
        print(f'            {" -> ".join(p[:8])}')

    # STEP 2 -- compose: indirect site -> ANY, iterated to a fixed point, so the
    # caller-side prefix is included.
    d_any = allroots[0][0] if allroots else 0
    seq = [d_any]
    level1 = None
    converged = False
    for _ in range(MAX_INDIRECT_LEVELS):
        nxt = global_max(funcs, d_any)
        d_new = nxt[0][0] if nxt else 0
        if level1 is None:
            level1 = nxt
        if d_new == d_any:
            converged = True
            break
        seq.append(d_new)
        d_any = d_new
    print('  COMPOSED bound (indirect call site -> ANY non-ISR function;')
    print('  includes the caller-side prefix, which step 1 does not):')
    print('    per added level of indirection: '
          + ' -> '.join(str(v) + ' B' for v in seq) + '   '
          + ('CONVERGED' if converged else 'STILL GROWING = UNBOUNDED'))
    if len(seq) > 1:
        deltas = sorted({seq[i + 1] - seq[i] for i in range(len(seq) - 1)})
        print('    cost of ONE level = '
              + ', '.join(str(d) + ' B' for d in deltas)
              + '  (deepest indirect call site + return address)')
    if not converged:
        print('    ^ a function holding an indirect call site is reachable from')
        print('      ANY, so no finite worst case exists under the any-target')
        print('      assumption -- the levels above are what one, two, ... nested')
        print('      indirect calls cost, not a fixed point. Narrow the target set')
        print('      with an address-taken test over const data (that needs')
        print('      objdump -s on .rodata, which -d does not give).')
    print('    ONE level of indirection (the transport client vtable is one):')
    for d, n, p in (level1 or [])[:5]:
        print(f'    {d:6d} B  {n}')
        print(f'            {" -> ".join(p[:8])}')

    # the indirect call sites themselves, with the stack already pushed there
    sites = [(sp, k, n) for n, f in funcs.items() for k, sp in f['ind']]
    sites.sort(reverse=True)
    if sites:
        print(f'  indirect call sites ({len(sites)} total), deepest sp_at first:')
        for sp, k, n in sites[:8]:
            print(f'    sp_at={sp:5d} B  +ret 4 B  {k:3s}  {n}')

    # the named boot selftests, for the report table
    print('  named frames:')
    for n in ('_asrc_q31_check_16ch', '_asrc_q31_opt_selftest',
              '_asrc_poly_q31_selftest', '_asrc_decimator_q31_isolation_selftest',
              '_asrc_decimator_q31_selftest', '_asrc_q31_check_vectors'):
        if n in funcs:
            d, p, _ = deepest(funcs, n)
            print(f'    {n:42s} own={funcs[n]["own"]:5d} B  deepest={d:5d} B')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
