#!/usr/bin/env python3
"""Attribute program-memory bytes in an XC-DSC link map to object files.

The link uses -ffunction-sections/-fdata-sections plus --gc-sections, so every
surviving function and constant object is its own output section named after the
symbol.  The map's "Linker script and memory map" region names the object file
each section came from, which is all that is needed to answer "who owns the
Flash".

Usage:
    python map_rom_report.py <map file> [--top N] [--by-symbol N]
                                       [--filter SUBSTRING] [--csv out.csv]

Only sections placed in program memory are counted (address >= 0x800000 on the
dsPIC33A parts, whose data space starts at 0x4000).
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

PROGRAM_BASE = 0x800000

DETAIL_MARKER = "Linker script and memory map"

# " .text.main       0x808d24        0x8ec build/..._ext/659850791/main.o"
# XC-DSC also emits constant sections whose name is a hash with no leading dot
# ("9d8c6a7f72fc_0"), and the IVT arrives as "__ivt_0", so the name is matched
# loosely and only the address decides whether it counts as program memory.
FULL_RE = re.compile(r"^\s(\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")
# " .text.some_really_long_name" followed by the numbers on the next line
NAME_ONLY_RE = re.compile(r"^\s([.\w$]\S*)\s*$")
CONT_RE = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*?)\s*$")


def parse(map_path: Path):
    """Yield (section, address, size, object) for program-memory sections."""
    lines = map_path.read_text(errors="replace").splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith(DETAIL_MARKER))
    except StopIteration:
        sys.exit(f"{map_path}: no '{DETAIL_MARKER}' region found")

    pending_name = None
    for line in lines[start:]:
        m = FULL_RE.match(line)
        if m:
            pending_name = None
            name, addr, size, obj = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
            if addr >= PROGRAM_BASE and size:
                yield name, addr, size, obj
            continue
        if pending_name is not None:
            m = CONT_RE.match(line)
            if m:
                addr, size, obj = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
                if addr >= PROGRAM_BASE and size:
                    yield pending_name, addr, size, obj
                pending_name = None
                continue
        m = NAME_ONLY_RE.match(line)
        pending_name = m.group(1) if m else None


HASH_SECTION_RE = re.compile(r"^[0-9a-f]{12}_\d+$")


def kind_of(section: str) -> str:
    for prefix, kind in (
        (".text", "code"),
        (".rodata", "rodata"),
        (".const", "const"),
        (".dinit", "dinit"),
        (".data", "data-init"),
    ):
        if section.startswith(prefix):
            return kind
    if section.startswith("__ivt") or section.startswith(".ivt"):
        return "ivt"
    if HASH_SECTION_RE.match(section):
        return "const"  # XC-DSC constant pool / static const in program memory
    return "other"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("map", type=Path)
    ap.add_argument("--top", type=int, default=40, help="how many object files to list")
    ap.add_argument("--by-symbol", type=int, default=0, help="also list the N largest sections")
    ap.add_argument("--filter", default="", help="only count objects whose path contains this")
    ap.add_argument("--csv", type=Path, help="write the full per-object table here")
    args = ap.parse_args()

    per_obj: dict[str, int] = defaultdict(int)
    per_obj_kind: dict[tuple[str, str], int] = defaultdict(int)
    per_kind: dict[str, int] = defaultdict(int)
    sections: list[tuple[int, str, str]] = []
    total = 0

    for name, _addr, size, obj in parse(args.map):
        if args.filter and args.filter not in obj:
            continue
        short = Path(obj).name
        if obj.endswith(")"):  # archive member: libfoo.a(member.o)
            short = obj[obj.rfind("(") + 1 : -1] + "  <" + Path(obj[: obj.rfind("(")]).name + ">"
        per_obj[short] += size
        per_obj_kind[(short, kind_of(name))] += size
        per_kind[kind_of(name)] += size
        sections.append((size, name, short))
        total += size

    print(f"map: {args.map}")
    print(f"program-memory bytes accounted for: {total} (0x{total:x})\n")

    print("by kind:")
    for kind, size in sorted(per_kind.items(), key=lambda kv: -kv[1]):
        print(f"  {kind:<10} {size:>8}  {100.0 * size / total:5.1f}%")

    print(f"\ntop {args.top} object files:")
    print(f"  {'bytes':>8} {'code':>7} {'data':>7}  object")
    for obj, size in sorted(per_obj.items(), key=lambda kv: -kv[1])[: args.top]:
        code = per_obj_kind[(obj, "code")]
        data = size - code
        print(f"  {size:>8} {code:>7} {data:>7}  {obj}")

    if args.by_symbol:
        print(f"\ntop {args.by_symbol} sections:")
        for size, name, obj in sorted(sections, reverse=True)[: args.by_symbol]:
            print(f"  {size:>8}  {name:<52} {obj}")

    if args.csv:
        with args.csv.open("w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["object", "bytes", "code", "non_code"])
            for obj, size in sorted(per_obj.items(), key=lambda kv: -kv[1]):
                code = per_obj_kind[(obj, "code")]
                w.writerow([obj, size, code, size - code])
        print(f"\nwrote {args.csv}")


if __name__ == "__main__":
    main()
