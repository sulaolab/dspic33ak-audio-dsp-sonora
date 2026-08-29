#!/usr/bin/env python3
"""Prove, on the linked image, the one condition the Y-only modulo kernel needs.

WHAT CONDITION, AND WHY IT IS NOT A CONVENTION

fir_ring_q31_ymod_yonly_dspic33ak.s enables Y AGU modulo addressing for the
duration of one block, and leaves X modulo off.  MODCON, XMODSRT/XMODEND and
YMODSRT/YMODEND are NOT part of the per-IPL register context (DS70005591C
Table 4-2), so while that block runs, the modulo setting is live in every
context -- including an interrupt that preempts it.

The datasheet bounds the damage: the Y AGU serves "the DSP MAC-class of
instructions only" (4.3.16), and on this core `mac.s` and friends take their
operands from registers, so ordinary compiled code cannot reach the Y AGU at
all.  Only an instruction that prefetches through two pointers can.  That makes
the residual condition narrow enough to check on the image rather than promise
in a comment:

    every instruction that can address through the Y AGU sits inside a leaf
    kernel that saves and restores the modulo registers around its own use,
    and nothing else in the image writes MODCON / YMODSRT / YMODEND.

Given that, no context can fold an address into another context's ring.  A
kernel that brackets its own window may be preempted by a kernel that brackets
its own window: the inner one points modulo at its ring, restores what it found,
and the outer one resumes with its window intact.  What breaks the scheme is a
Y-AGU instruction that does NOT bracket -- it inherits whatever window happens
to be open -- or a stray MODCON write that leaves modulo enabled after return.
Those are what this script fails on.

An unbracketed Y-AGU user is permitted at task level only, where it cannot
preempt anything: an interrupt that arrives while a task holds a window runs to
completion before the task resumes, so the task never executes inside someone
else's window.  The reverse is not true, which is why the same instruction in an
interrupt handler is a finding.

The check was done by hand on 2026-08-21; this makes it repeatable, which is the
point -- the condition is about which code exists, and that changes.

    python tools/asrc/ymod_safety_gate.py <image.elf>

Exit 0 = clean, 1 = findings, 2 = the check could not be run (also a failure:
a gate that cannot run must not read as a pass -- but kept distinct from 1, so a
build log can tell a real finding from a broken tool).

The device pack is resolved from the image itself rather than fixed here.  The
parts this project builds for are in different packs, and objdump given the wrong
one refuses the disassembly instead of degrading, which read as a finding on
every AK128 image until 2026-08-22.
"""

from __future__ import annotations

import argparse
import bisect
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import dfp_packs

# Modulo control registers, from the DFP linker script (p33AK512MPS512.gld):
# MODCON 0x14, XMODSRT 0x18, XMODEND 0x1C, YMODSRT 0x20, YMODEND 0x24.
MODULO_SFR = {0x14: "MODCON", 0x18: "XMODSRT", 0x1C: "XMODEND",
              0x20: "YMODSRT", 0x24: "YMODEND"}

# The DSP MAC class.  Matching the mnemonic alone would flag every float multiply
# the compiler emits (`mac.s` appears a couple of hundred times in ordinary code,
# taking its operands from registers), so an AGU offence needs a prefetch operand
# as well -- see agu_use().
MAC_MNEMONIC = re.compile(r"^(mac|msc|mpy|mpyn|ed|edac|movsac)(\.[a-z]+)?$")
PREFETCH = re.compile(r"\[w\d+\]\s*[+-]?=")

SYM_ADDR_MIN = 0x800000          # program space; anything below it is data
INSN = re.compile(r"^\s+([0-9a-f]+):\t[0-9a-f ]+\t(\S+)\s*(.*?)\s*$")
HEXNUM = re.compile(r"\b0x([0-9a-f]+)\b")
IMMEDIATE = re.compile(r"#0x([0-9a-f]+)\b")
UNNAMED = "<code with no symbol>"

# objdump prints one of these before each section it disassembles.  They matter
# because almost none of the hand-written kernels in this tree carry .size, so
# without a boundary here an unsized label owns every later instruction that
# belongs to no sized symbol.  That is not merely untidy: it can HIDE a finding
# (an unbracketed AGU user absorbed into a bracketed kernel's group reads as
# "brackets its own use"), which is a false negative in a safety gate.  Observed
# on 2026-08-22: _fir_ring_q31 absorbed another function's retfie and was
# reported as an interrupt handler, without the kernel having changed at all.
SECTION = re.compile(r"^Disassembly of section (\S+):")

# The part name as it appears inside the image, anchored to the compiler's own
# "-mcpu=<part>" build-flag comment (embedded once per compile unit, so dozens of
# hits but always the same part for a single-target link). NOT anchored to bare
# "33AK<flash>M<family><pins>" text anywhere in the image: app_silicon.c's runtime
# device-identification code (src/app/diagnostics/app_silicon.c) returns the
# literal name of a device it can recognise but is not necessarily running on,
# e.g. "dsPIC33AK512MPS512" as a rodata string linked into every configuration --
# found 2026-08-26 on an AK128 build, one bare match beside 70 -mcpu= matches for
# the real target, which is exactly the "answer would not be trustworthy" case
# dfp_packs.device_of() guards against; it just was not looking at the
# discriminating text. device_of()/pack resolution moved to tools/dfp_packs.py
# on 2026-08-28 so the other objdump-consuming tools share the same logic.


class Insn:
    __slots__ = ("addr", "mnemonic", "operands")

    def __init__(self, addr, mnemonic, operands):
        self.addr, self.mnemonic, self.operands = addr, mnemonic, operands

    def __str__(self):
        return "%06x: %-9s %s" % (self.addr, self.mnemonic, self.operands)


class Func:
    __slots__ = ("name", "addr", "insns")

    def __init__(self, name, addr):
        self.name, self.addr, self.insns = name, addr, []

    def any(self, pred):
        return any(pred(i) for i in self.insns)

    def all_matching(self, pred):
        return [i for i in self.insns if pred(i)]


CANNOT_RUN = 2


def bail(message):
    """Exit 2, never 1: "the check could not be run" is not "the check found something".

    Every caller treats a non-zero exit as a failure, which is right -- but a build
    log that says FAIL wants to tell a real finding from a broken tool, and 1 is
    spoken for by findings.
    """
    print(message, file=sys.stderr)
    sys.exit(CANNOT_RUN)


def version_key(path):
    """Sort pack / compiler directories by version number, not as strings.

    `sorted()` on the raw paths puts 1.10.x before 1.2.x, which silently picks a
    stale pack the day a minor number reaches double digits.
    """
    name = os.path.basename(path.rstrip("/\\"))
    return [int(part) if part.isdigit() else part
            for part in re.split(r"(\d+)", name)]


def newest(pattern, what, flag):
    found = sorted(glob.glob(pattern), key=version_key)
    if not found:
        bail("%s not found -- pass %s" % (what, flag))
    return found[-1]




def run(argv):
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode != 0:
        bail("%s failed (%d):\n%s" % (os.path.basename(argv[0]),
                                      result.returncode, result.stderr.strip()))
    return result.stdout


def read_symbols(symtab):
    """-> ([(addr, size, name)] sized functions, {addr: [(name, local)]} the rest).

    Sizes matter: a function's extent has to stop where the symbol says it does.
    Extending each symbol to the next one instead puts the tail of one function
    inside another -- and when the neighbour is an interrupt handler, that quietly
    credits the wrong function with a `retfie`.

    Binding matters for the same reason in the other direction: the assembly
    kernels carry no size, and their internal branch targets (`_yonly_out`) are
    symbols too.  Splitting a kernel there would separate its `push MODCON` from
    its `pop MODCON` and make a correctly bracketed kernel look unbracketed, so
    local symbols are kept as labels and only exported ones start a function.
    """
    sized, labels = [], {}
    for line in symtab.splitlines():
        if "\t" not in line:
            continue
        left, right = line.split("\t", 1)
        fields, rest = left.split(), right.split()
        if len(fields) < 2 or len(rest) < 2:
            continue
        try:
            addr, size = int(fields[0], 16), int(rest[0], 16)
        except ValueError:
            continue
        name, flags = rest[1], line[9:16]
        if addr < SYM_ADDR_MIN or name.startswith("."):
            continue
        if size and "F" in flags:
            sized.append((addr, size, name))
        else:
            labels.setdefault(addr, []).append((name, flags[:1] == "l"))
    sized.sort()
    return sized, labels


def parse(disasm, sized, labels):
    """Split the disassembly into functions. Code with no symbol is kept, not dropped."""
    starts = [a for a, _, _ in sized]

    def sized_at(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return sized[i] if i >= 0 and addr < sized[i][0] + sized[i][1] else None

    instructions, called, section_starts = [], set(), []
    pending_section = False
    for line in disasm.splitlines():
        if SECTION.match(line):
            pending_section = True
            continue
        match = INSN.match(line)
        if not match:
            continue
        insn = Insn(int(match.group(1), 16), match.group(2), match.group(3))
        if pending_section:
            section_starts.append(insn.addr)
            pending_section = False
        instructions.append(insn)
        is_call, target = call_target(insn)
        if is_call and target is not None:
            called.add(target)

    # A symbol outside every sized interval starts a function when it is exported
    # or called; otherwise it is an internal label of the kernel it sits in.
    plain = sorted(set(
        [addr for addr, names in labels.items()
         if sized_at(addr) is None
         and (addr in called or any(not local for _, local in names))]
        # A section boundary ends a group even with no label on it, so an unsized
        # kernel cannot annex the next section's code.  See SECTION above.
        + [addr for addr in section_starts if sized_at(addr) is None]))

    def owner(addr):
        found = sized_at(addr)
        if found:
            return found[0], found[2]
        i = bisect.bisect_right(plain, addr) - 1
        if i < 0:
            return None, UNNAMED
        base = plain[i]
        here = labels.get(base, [])
        named = [n for n, _ in here if n.startswith("_")]
        if named:
            return base, named[0]
        return base, (here[0][0] if here else UNNAMED)

    funcs = {}
    for insn in instructions:
        key, name = owner(insn.addr)
        funcs.setdefault(key, Func(name, key)).insns.append(insn)
    return funcs


def agu_use(insn):
    """Can this instruction address through an AGU, with modulo applied to it?

    Two prefetch operands means the second goes through the Y AGU, which is the
    case that matters.  A MAC-class instruction with one prefetch is counted too:
    that one goes through the X AGU, and one of these kernels enables X modulo as
    well, so counting it costs nothing and settles the case without having to
    argue which AGU a given form uses.
    """
    prefetches = len(PREFETCH.findall(insn.operands))
    if prefetches >= 2:
        return "two-operand prefetch (Y AGU)"
    if prefetches and MAC_MNEMONIC.match(insn.mnemonic):
        return "MAC-class prefetch"
    return None


def sfr_at(operand):
    match = re.fullmatch(r"0x0*([0-9a-f]+)", operand.strip())
    return MODULO_SFR.get(int(match.group(1), 16)) if match else None


def written_sfr(insn):
    """The modulo register this instruction writes, if any.

    A store names its destination last (`mov.l w8, 0x000014`), a load names it
    first, so the last operand alone separates writes from reads without a table
    of every mnemonic.  `push 0x14` reads it; `pop 0x14` writes it.
    """
    if insn.mnemonic.startswith("push"):
        return None
    return sfr_at(insn.operands.rsplit(",", 1)[-1])


def saved_sfr(insn):
    return sfr_at(insn.operands) if insn.mnemonic.startswith("push") else None


def restored_sfr(insn):
    return sfr_at(insn.operands) if insn.mnemonic.startswith("pop") else None


def call_target(insn):
    """-> (is a call, target address or None if it goes through a register)."""
    if not insn.mnemonic.startswith(("call", "rcall")):
        return False, None
    match = HEXNUM.search(insn.operands)
    return True, (int(match.group(1), 16) if match else None)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", help="the linked image to check")
    parser.add_argument("--objdump", help="xc-dsc-objdump.exe (default: newest installed)")
    parser.add_argument("--mdfp", help="device family pack (default: the pin in "
                        "tools/dfp_packs.py, else newest installed)")
    args = parser.parse_args()

    if not os.path.isfile(args.elf):
        bail("no such image: " + args.elf)
    objdump = args.objdump or newest(
        "C:/Program Files/Microchip/xc-dsc/*/bin/xc-dsc-objdump.exe",
        "xc-dsc-objdump.exe", "--objdump")
    try:
        device = None if args.mdfp else dfp_packs.device_of(args.elf)
        mdfp = dfp_packs.resolve_dfp(device, override=args.mdfp)
    except dfp_packs.DfpResolutionError as exc:
        bail(str(exc) + " -- pass --mdfp")

    sized, labels = read_symbols(run([objdump, "-t", "--mdfp=" + mdfp, args.elf]))
    funcs = parse(run([objdump, "-d", "--mdfp=" + mdfp, args.elf]), sized, labels)
    if not funcs:
        bail("no code found in " + args.elf)

    handlers = {k: f for k, f in funcs.items()
                if f.any(lambda i: i.mnemonic.startswith("retfie"))}
    agu = {k: f for k, f in funcs.items() if f.any(agu_use)}
    writers = {k: f for k, f in funcs.items() if f.any(written_sfr)}

    # Reachability is needed only to place an unbracketed AGU user, so direct
    # edges plus one closure are enough: a function whose address is never taken
    # cannot be reached by an indirect call either.  Only immediates count as
    # taking an address -- a branch target is not a function pointer.
    edges, blind, taken = {}, {}, set()
    for key, func in funcs.items():
        out, unresolved = set(), []
        for insn in func.insns:
            is_call, target = call_target(insn)
            if is_call and target is not None:
                out.add(target)
            elif is_call:
                unresolved.append(insn)
            for immediate in IMMEDIATE.finditer(insn.operands):
                value = int(immediate.group(1), 16)
                if value in funcs:
                    taken.add(value)
        edges[key], blind[key] = out, unresolved

    def reachable(root):
        seen, stack = {root}, [root]
        while stack:
            for callee in edges.get(stack.pop(), ()):
                if callee not in seen:
                    seen.add(callee)
                    stack.append(callee)
        return seen

    from_isr = set()
    for key in handlers:
        from_isr |= reachable(key)

    findings, notes = [], []

    for key, func in sorted(agu.items(), key=lambda kv: kv[1].name):
        uses = len(func.all_matching(agu_use))
        writes = {written_sfr(i) for i in func.insns if written_sfr(i)}
        saves = {saved_sfr(i) for i in func.insns if saved_sfr(i)}
        restores = {restored_sfr(i) for i in func.insns if restored_sfr(i)}
        calls = func.all_matching(lambda i: call_target(i)[0])

        if calls:
            findings.append(
                "%s addresses through the AGU and also calls out:\n      %s\n"
                "    Its window, while open, would span code this check has not looked at."
                % (func.name, calls[0]))
        if writes:
            missing = sorted((writes - saves) | (writes - restores))
            if missing:
                findings.append(
                    "%s writes %s without saving and restoring %s: the setting\n"
                    "    survives the return and is then live in every other context."
                    % (func.name, "+".join(sorted(writes)), "+".join(missing)))
            else:
                notes.append("%s brackets %s around its own use (%d AGU instruction%s)"
                             % (func.name, "+".join(sorted(writes)), uses,
                                "" if uses == 1 else "s"))
        else:
            # No window of its own, so it runs in whatever window it inherits.
            why = []
            if key in from_isr:
                why.append("reachable from an interrupt handler")
            if key in taken:
                why.append("has its address taken, so an indirect call may reach it")
            if why:
                findings.append(
                    "%s addresses through the AGU without setting modulo itself, and %s.\n"
                    "    In interrupt context it inherits the window of whatever it preempted."
                    % (func.name, " and ".join(why)))
            else:
                notes.append("%s addresses through the AGU with no window of its own,"
                             " task level only (%d AGU instruction%s)"
                             % (func.name, uses, "" if uses == 1 else "s"))

    for key, func in sorted(writers.items(), key=lambda kv: kv[1].name):
        if key not in agu:
            insn = next(i for i in func.insns if written_sfr(i))
            findings.append(
                "%s writes %s but has no AGU instruction of its own: nothing here needs\n"
                "    modulo, and the write is live in every other context.\n      %s"
                % (func.name, written_sfr(insn), insn))

    unnamed = funcs.get(None)
    print("image:     %s" % args.elf)
    print("device:    %s, pack %s"
          % (device or "(not read -- --mdfp was given)", mdfp))
    print("code:      %d functions, %d interrupt handlers%s"
          % (len(funcs), len(handlers),
             ", %d instructions with no symbol" % len(unnamed.insns) if unnamed else ""))

    if not agu:
        print("\nNo AGU-addressing instruction in this image: modulo addressing is unused,"
              "\nso there is no window for another context to inherit.")
        return 0

    for note in notes:
        print("  - " + note)
    unresolved = sum(len(blind[key]) for key in from_isr if key in blind)
    if unresolved:
        # Stated rather than followed: an indirect call can only reach a function
        # whose address is taken, and every AGU user was tested for that, so these
        # edges cannot be hiding one.
        print("  - %d indirect call site(s) in interrupt-reachable code, not followed"
              " (closed by the address-taken test)" % unresolved)

    if findings:
        print("\nFAIL: %d finding%s." % (len(findings), "" if len(findings) == 1 else "s"))
        for finding in findings:
            print("  - " + finding)
        return 1

    print("\nPASS: every AGU-addressing instruction is bracketed by the kernel that uses it"
          "\nor confined to task level, and nothing else writes MODCON/YMODSRT/YMODEND.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
