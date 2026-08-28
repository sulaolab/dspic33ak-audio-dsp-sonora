#!/usr/bin/env python3
"""Answer "why is this function linked?" for an XC-DSC build.

--gc-sections keeps an input section when something it already kept refers to it,
so one unconditional call can drag in a whole feature the firmware never uses at
runtime.  The link map records what survived but never who referred to it, so
this rebuilds the reference graph from the relocation records in the object files
and intersects it with the sections that survived.

The graph nodes are *sections*, not symbols, because a section is what the linker
keeps or drops.  Relocations name their target three different ways -- a global
symbol ("_foo"), a compiler-local label inside a static's section (".L2"), or a
constant pool with a hashed name ("ebd86a860b62_3", which is how the app's
function-pointer tables show up) -- so every target is resolved back to the
section that defines it before the edge is recorded.  Resolve only two of the
three and whole dispatch paths vanish from the graph.

Usage:
    python link_refs.py <map> <build dir> [--of NAME]... [--reach]
                        [--without REGEX] [--suspects] [--min N]

--of NAME       print what refers to NAME (a symbol or a section name)
--reach         sanity-check the model: live code not reachable from the
                interrupt vectors and main
--without REGEX cut the root vectors matching REGEX and report what stops being
                reachable -- what a gate on that feature would recover
--suspects      live sections whose referrers all sit in one other object
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

PROG = 0x800000
DFP = os.environ.get(
    "AK_DFP",
    str(Path.home() / ".mchp_packs/Microchip/dsPIC33AK-MC_DFP/1.4.172/xc16"),
)
OBJDUMP = os.environ.get(
    "AK_OBJDUMP",
    r"C:/Program Files/Microchip/xc-dsc/v3.31.01/bin/xc-dsc-objdump.exe",
)

# ---- link map -------------------------------------------------------------
FULL = re.compile(r"^\s(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
NAME = re.compile(r"^\s([.\w$]\S*)\s*$")
CONT = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")


def map_sections(map_path: Path, region: str = "Linker script and memory map"):
    lines = map_path.read_text(errors="replace").splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith(region))
    if region.startswith("Discarded"):
        stop = next(i for i, l in enumerate(lines)
                    if l.startswith("Linker script and memory map"))
        lines = lines[:stop]
    pend = None
    for line in lines[start:]:
        m = FULL.match(line)
        if m:
            pend = None
            yield m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            continue
        if pend is not None:
            m = CONT.match(line)
            if m:
                yield pend, int(m.group(1), 16), int(m.group(2), 16), m.group(3)
                pend = None
                continue
        m = NAME.match(line)
        pend = m.group(1) if m else None


# ---- object files ---------------------------------------------------------
RELHDR = re.compile(r"^RELOCATION RECORDS FOR \[([^\]]+)\]:")
# "0000001c PC RELATIVE FBRANCH 24  _nora_i2c_get_device" -- the relocation TYPE
# contains spaces, so the target is the LAST field, not the third.
RELROW = re.compile(r"^[0-9a-f]{8}\s+\S.*?\s(\S+)\s*$")
# "0000001c l       .text.local_feed_locked\t00000000 .L2"
SYMROW = re.compile(r"^[0-9a-f]{8}\s+(\S+)\s*\S*\s+(\S+)\t[0-9a-f]{8}\s+(\S+)\s*$")


def dump(obj: Path) -> str:
    return subprocess.run(
        [OBJDUMP, f"-mdfp={DFP}", "-t", "-r", str(obj)],
        capture_output=True, text=True,
    ).stdout


def symbols(text: str):
    """Yield (name, section, is_global) for every defined symbol."""
    for line in text.splitlines():
        if line.startswith("RELOCATION RECORDS"):
            return
        m = SYMROW.match(line)
        if m and m.group(2) not in ("*ABS*", "*UND*"):
            yield m.group(3), m.group(2), "l" not in m.group(1)


def relocations(text: str):
    """Yield (from_section, raw_target) for every relocation."""
    sec = None
    for line in text.splitlines():
        m = RELHDR.match(line)
        if m:
            sec = m.group(1)
            continue
        if sec is None:
            continue
        m = RELROW.match(line)
        if m:
            tgt = m.group(1).split("+")[0]
            if tgt and not tgt.startswith("*"):
                yield sec, tgt


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("map", type=Path)
    ap.add_argument("builddir", type=Path)
    ap.add_argument("--of", action="append", default=[])
    ap.add_argument("--reach", action="store_true")
    ap.add_argument("--without", default="")
    ap.add_argument("--cut", default="",
                    help="regex of sections to block, as if the call were gated "
                         "out; report what stops being reachable")
    ap.add_argument("--suspects", action="store_true")
    ap.add_argument("--init-only", action="store_true", dest="init_only",
                    help="objects whose only live code is set-up: the module is "
                         "brought up and nothing can ever use it")
    ap.add_argument("--min", type=int, default=48)
    args = ap.parse_args()

    live: dict[str, int] = {}
    owner: dict[str, str] = {}
    for name, addr, size, obj in map_sections(args.map):
        if addr >= PROG and size:
            live[name] = live.get(name, 0) + size
            owner[name] = Path(obj.rstrip(")").split("(")[-1]).name

    objs = sorted(args.builddir.rglob("*.o"))
    if not objs:
        sys.exit(f"no objects under {args.builddir}")
    texts = {o: dump(o) for o in objs}

    # symbol -> section.  Globals are program-wide, locals only inside their object.
    g_sym: dict[str, str] = {}
    l_sym: dict[Path, dict[str, str]] = {}
    for o, t in texts.items():
        loc: dict[str, str] = {}
        for nm, sec, is_global in symbols(t):
            (g_sym if is_global else loc)[nm] = sec
        l_sym[o] = loc

    def resolve(obj: Path, tgt: str) -> str | None:
        """Relocation target -> the section that defines it."""
        cands = [tgt]
        if tgt.startswith("_"):
            cands.append(tgt[1:])
        for cand in cands:
            if cand in g_sym:
                return g_sym[cand]
            if cand in l_sym[obj]:
                return l_sym[obj][cand]
        return tgt if tgt in live else None

    edges: dict[str, set[str]] = defaultdict(set)
    refs: dict[str, set[str]] = defaultdict(set)
    for o, t in texts.items():
        for sec, tgt in relocations(t):
            dst = resolve(o, tgt)
            if dst is None or dst == sec:
                continue
            edges[sec].add(dst)
            refs[dst].add(f"{o.name}:{sec}")

    def pretty(sec: str) -> str:
        return sec.split(".text.", 1)[1] if ".text." in sec else sec

    print(f"objects scanned: {len(objs)}   live sections: {len(live)}")

    for want in args.of:
        hits = [s for s in live if pretty(s) == want or s == want]
        if not hits:
            hits = [s for s in refs if pretty(s) == want or s == want]
        for sec in hits or [want]:
            print()
            print(f"{pretty(sec)}  ({live.get(sec, 0)} B live, in {owner.get(sec, '-')})")
            rs = sorted(r for r in refs.get(sec, ())
                        if not r.split(":", 1)[1].startswith(".debug"))
            if sec not in live and sec not in refs:
                # Asking about a name this build does not contain must not read as
                # "nothing refers to it" -- that is how a wrong guess at a symbol
                # name turns into a confident claim that a feature is dead.
                print("   NOT PRESENT in this build (misspelt, inlined, or discarded)")
            elif not rs:
                print("   NO code referrer -- linker script KEEP, IVT or entry point")
            for r in rs:
                obj, s = r.split(":", 1)
                print(f"   <- {obj}:{pretty(s)}")

    isr_roots = [s for s in live if s.startswith(".isr.text.")]
    roots = isr_roots + [s for s in (".text.main", ".text.__reset", ".text._reset")
                         if s in live]

    def reach(rs, blocked=frozenset()):
        seen, stack = set(), [r for r in rs if r not in blocked]
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            seen.add(n)
            stack.extend(t for t in edges.get(n, ()) if t not in blocked)
        return seen

    if args.reach:
        seen = reach(roots)
        code = [s for s in live if ".text." in s]
        orphan = [(live[s], s) for s in code if s not in seen]
        print()
        print(f"roots: {len(roots)} ({len(isr_roots)} ISR/trap)")
        print(f"live code sections reachable from roots: "
              f"{len(code) - len(orphan)}/{len(code)}")
        for sz, s in sorted(orphan, reverse=True)[:20]:
            print(f"   {sz:>6}  {pretty(s)}  [{owner.get(s, '-')}]")
        print(f"   ... {len(orphan)} sections, {sum(sz for sz, _ in orphan)} B unexplained")

    if args.without:
        cut = [r for r in roots if re.search(args.without, r)]
        if not cut:
            sys.exit(f"--without matched no root")
        gone = reach(roots) - reach([r for r in roots if r not in cut])
        dies = [(live[s], s) for s in gone if s in live]
        cutb = sum(live.get(c, 0) for c in cut)
        print()
        print(f"cutting {len(cut)} roots ({cutb} B of vector body):")
        for c in sorted(cut):
            print(f"   x {pretty(c)}  {live.get(c, 0)} B")
        # The cut roots are themselves unreachable afterwards, so they are already
        # inside `dies` -- adding cutb again would double-count the vector bodies.
        print(f"no longer reachable: {len(dies)} sections, {sum(d[0] for d in dies)} B")
        for sz, s in sorted(dies, reverse=True):
            print(f"   {sz:>6}  {pretty(s)}  [{owner.get(s, '-')}]")
        print(f"TOTAL program bytes recoverable: {sum(d[0] for d in dies)}"
              f"  (of which {cutb} B is the vector bodies)")

    if args.cut:
        blocked = {s for s in live if re.search(args.cut, pretty(s))}
        if not blocked:
            sys.exit("--cut matched no live section")
        gone = reach(roots) - reach(roots, frozenset(blocked))
        dies = sorted(((live[s], s) for s in gone if s in live), reverse=True)
        print()
        print(f"blocking {len(blocked)} sections: "
              f"{', '.join(sorted(pretty(b) for b in blocked))}")
        print(f"no longer reachable: {len(dies)} sections, "
              f"{sum(d[0] for d in dies)} B of program memory")
        for sz, sec in dies:
            print(f"   {sz:>6}  {pretty(sec)}  [{owner.get(sec, '-')}]")

    if args.init_only:
        dead: dict[str, list[str]] = defaultdict(list)
        for name, _a, size, obj in map_sections(args.map, "Discarded input sections"):
            if name.startswith(".text.") and size:
                dead[Path(obj.rstrip(")").split("(")[-1]).name].append(name[6:])
        # Only names that ARE set-up count.  An earlier version also accepted
        # "set_"/"get_"/"is_"/"reset", which flagged nora_pwm_dspic33ak.o as unused
        # while nora_pwm_generator_set_duty -- the actual use -- sat in the list.
        setup = re.compile(r"(_(init|initialize|deinit|open|close|config|configure)"
                           r"(_.*)?$)|(^(init|module_init))", re.I)
        print()
        print("=== objects whose only live code is set-up (module never used) ===")
        per_obj: dict[str, list[tuple[int, str]]] = defaultdict(list)
        for sec, sz in live.items():
            if ".text." in sec:
                per_obj[owner.get(sec, "-")].append((sz, pretty(sec)))
        for obj, funcs in sorted(per_obj.items(), key=lambda kv: -sum(f[0] for f in kv[1])):
            if not dead.get(obj):
                continue
            if all(setup.search(n) for _sz, n in funcs):
                tot = sum(f[0] for f in funcs)
                print(f"{tot:>6} B live in {obj}  ({len(dead[obj])} functions discarded)")
                for sz, n in sorted(funcs, reverse=True):
                    print(f"          {sz:>5}  {n}")

    if args.suspects:
        print()
        print("=== live sections whose referrers all sit in ONE other object ===")
        rows = []
        for sec, sz in live.items():
            if sz < args.min or ".text." not in sec:
                continue
            rs = {r for r in refs.get(sec, ())
                  if not r.split(":", 1)[1].startswith(".debug")}
            srcs = {r.split(":")[0] for r in rs}
            if len(srcs) == 1 and next(iter(srcs)) != owner.get(sec):
                rows.append((sz, sec, sorted(rs)))
        for sz, sec, rs in sorted(rows, reverse=True):
            print(f"{sz:>6}  {pretty(sec)}  [{owner.get(sec, '-')}]")
            for r in rs[:4]:
                obj, s = r.split(":", 1)
                print(f"          <- {obj}:{pretty(s)}")


if __name__ == "__main__":
    main()
