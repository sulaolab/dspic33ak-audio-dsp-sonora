#!/usr/bin/env python3
"""Source-side lint for the IRQ-register-atomicity rule.

IFSx / IECx are shared by every peripheral on the device, so they must only ever
be touched one bit at a time through a DFP bit alias whose register, bit and
written value are all compile-time constant:

    if (enable) { _DMA0IE = 1; } else { _DMA0IE = 0; }

Any form that reaches the register through a pointer or a runtime mask compiles
to a byte read-modify-write and can silently undo another peripheral's enable.
The objdump census (tools/irq_atomicity_hist.py) only sees *direct-address*
writes -- `mov.b w1, 0xc8` is visible there, `mov.b w1, [w2]` is not. This lint
closes that gap from the other end by banning the constructs that produce the
pointer form in the first place.

Banned (outside comments and string literals):

  &IFS3, & IEC2          taking the address of a shared interrupt register
  *ifs, *iec             dereferencing such a pointer
  ->ifs, ->iec, .ifs     a descriptor field holding one
  if_mask, ie_mask,      the runtime mask that went with it
  ifs_mask, iec_mask
  _DMA0IE = enable       a *runtime* value written to a bit alias -- the defect
  _DMA0IE = e ? 1 : 0    this whole port exists to remove. Only the literal
  _DMA0IE |= 1           forms `= 0` / `= 1` (with an optional u suffix) fold to
                         a single bset.b/bclr.b; anything else is a byte RMW.
  IEC2 |= m              a read-modify-write of the whole shared register
  IFS3 = 0               a whole-register store: not an RMW, but it clears every
                         other peripheral's bit in the same register
  IEC2bits, IFS3bits     the bitfield struct, which also compiles to a byte RMW
  _IFS3_CNAIF_MASK       per-device bank knowledge -- allowed *only* inside
  _IEC4_CCP9IE_MASK      defined(...) as a capability probe

Reads are never flagged: `if (IEC2 & mask)` and `x = IFS0` cannot race.

Usage:
    python tools/irq_atomicity_source_lint.py [--root src] [--verbose]

Exit status 0 = clean, 1 = violations found.
"""

import argparse
import os
import re
import sys

EXTS = (".c", ".h")

# Rules are (name, compiled pattern, human explanation).
RULES = [
    ("address-of-IFS/IEC",
     re.compile(r"&\s*(?:IFS|IEC)\d"),
     "take no address of a shared interrupt register; write the _XxxIE/_XxxIF alias"),
    ("IFS/IEC pointer deref",
     re.compile(r"\*\s*(?:ifs|iec)\b"),
     "dereferencing an IFS/IEC pointer is a read-modify-write"),
    # A descriptor field holding a pointer to a shared interrupt register.
    # `x.iec[0] = IEC0` is *not* this: an indexed value array cannot hold a
    # register pointer, because taking &IECn is banned by the rule above.
    # The optional leading identifier is captured so the finding text is the
    # whole qualified expression (`source_trace.iec`), which is what ALLOW keys on.
    ("IFS/IEC descriptor field",
     re.compile(r"(?:[A-Za-z_]\w*\s*)?(?:->|\.)\s*(?:ifs|iec)\b(?!\s*\[)"),
     "no descriptor may carry an IFS/IEC pointer"),
    ("IFS/IEC runtime mask",
     re.compile(r"\b(?:if_mask|ie_mask|ifs_mask|iec_mask)\b"),
     "a runtime mask forces the byte read-modify-write path"),
    # Direct writes to the whole shared register. `IEC2 |= mask` is the same
    # byte/word read-modify-write as the pointer form and would be invisible to
    # the objdump census in a translation unit that --gc-sections drops.
    # Reads (`if (IEC2 & mask)`) are not a hazard and are not matched.
    # The right-hand side up to the statement end is part of the finding text, so
    # an ALLOW entry pins the exact assignment rather than the register name.
    ("IFS/IEC whole-register write",
     re.compile(r"\b(?:IFS|IEC)\d+\s*(?:\|=|&=|\^=|\+=|-=|<<=|>>=|=(?!=))[^;]*"),
     "write the _XxxIE/_XxxIF bit alias; a whole-register write is a read-modify-write"),
    ("IFS/IECbits write",
     re.compile(r"\b(?:IFS|IEC)\d+bits\b"),
     "IFSxbits/IECxbits compiles to a byte RMW; use the _XxxIE/_XxxIF alias"),
]

# Documented exceptions, keyed by repo-relative posix path. The value is a set
# of (rule name, exact finding text) pairs: an entry excuses *that expression*
# under *that rule* only, never the whole file or the whole rule. Keep this
# small; each pair is a promise that the site cannot race.
# `IECn = 0u; IFSn = 0u;` for every bank: the wholesale interrupt-controller
# teardown both resident-download-engine entry points do immediately after
# __builtin_disable_interrupts(), to start a relocated image from a known state.
# It is a plain constant word store, not a read-modify-write, and there is no
# concurrent writer because global interrupts are off. Pinned to the literal
# `= 0u` form: `IECn = <anything else>` is still a violation.
_INT_CTRL_CLEAR_ALL = frozenset(
    ("IFS/IEC whole-register write", "{}{}=0u".format(reg, bank))
    for reg in ("IFS", "IEC") for bank in range(12)
)

ALLOW = {
    # uint32_t[12] snapshots of the whole IEC0..11 / IFS0..11 registers, taken
    # once at reset for the boot trace and only ever printed. No register
    # pointer, no write path. (The capturing sites in resident_de_mailbox.c use
    # `.iec[0]`-style indexing and are not findings at all.)
    "src/boot/resident_de_boot_main.c": frozenset({
        ("IFS/IEC descriptor field", "source_trace.iec"),
        ("IFS/IEC descriptor field", "source_trace.ifs"),
    }),
    "src/app/resident_de/app/resident_de_app_handoff.c": _INT_CTRL_CLEAR_ALL,
    "src/boot/resident_de_boot_platform.c": _INT_CTRL_CLEAR_ALL,
}

# A DFP interrupt bit alias -- `_DMA0IE`, `_U1RXIF`, `_SPI3TXIE`. Leading
# underscore, then uppercase/digits with no further underscore (so the bank
# macros `_IFS3_CNAIF_MASK` are not caught here), ending in IE or IF. `IP` is
# deliberately out of scope: priority bits live in IPCx, not IFSx/IECx.
BIT_ALIAS_ASSIGN = re.compile(
    r"\b(_[A-Z][A-Z0-9]*I[EF])\s*(\|=|&=|\^=|<<=|>>=|=(?!=))([^;]*)")
# The only right-hand sides the compiler can fold into one bset.b / bclr.b.
BIT_ALIAS_LITERAL = re.compile(r"^[01][uU]?$")

# _IFSn_..._MASK / _IECn_..._MASK: bank knowledge. Legal only as defined(...).
BANK_MACRO = re.compile(r"\b_(?:IFS|IEC)\d+_[A-Z0-9_]+_MASK\b")
BANK_MACRO_PROBED = re.compile(r"\bdefined\s*\(\s*_(?:IFS|IEC)\d+_[A-Z0-9_]+_MASK\s*\)")


def strip_comments_and_strings(text):
    """Blank out /*...*/, //... and "..." / '...' while keeping line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            quote, j = c, i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def normalize(text):
    """Collapse internal whitespace so ALLOW keys stay readable."""
    return re.sub(r"\s+", "", text.strip())


def check_file(path):
    allowed = ALLOW.get(path.replace("\\", "/"), frozenset())
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        raw = fh.read()
    return scan_text(raw, allowed)


def scan_text(text, allowed=frozenset()):
    """Rule engine over a string. Shared by check_file() and the self-test."""
    code = strip_comments_and_strings(text.replace("\r\n", "\n"))
    findings = []
    for lineno, line in enumerate(code.split("\n"), start=1):
        for name, pattern, why in RULES:
            for m in pattern.finditer(line):
                findings.append((lineno, name, normalize(m.group(0)), why))
        # The original defect: a bit alias assigned a runtime value. Plain
        # `= 0` / `= 1` is the required form and is not a finding; a compound
        # assignment never is, because it reads the register back.
        for m in BIT_ALIAS_ASSIGN.finditer(line):
            if m.group(2) == "=" and BIT_ALIAS_LITERAL.match(normalize(m.group(3))):
                continue
            findings.append((lineno, "IFS/IEC bit alias runtime write",
                             normalize(m.group(0)),
                             "write the literal 0 or 1 (`if (e) { A = 1; } else { A = 0; }`); "
                             "a runtime value makes it a byte read-modify-write"))
        for m in BANK_MACRO.finditer(line):
            span = m.span()
            probed = any(p.span()[0] <= span[0] and span[1] <= p.span()[1]
                         for p in BANK_MACRO_PROBED.finditer(line))
            if not probed:
                findings.append((lineno, "IFS/IEC bank macro", m.group(0),
                                 "bank numbers are per-device; use the bit alias, "
                                 "or keep the macro inside defined(...)"))
    # Exclude only exact (rule, expression) pairs listed in ALLOW.
    return [f for f in findings if (f[1], f[2]) not in allowed]


# (source line, allow-set, expected finding texts). Keeps the rules honest, and
# in particular pins that an ALLOW entry excuses one expression, not a file.
SELF_TEST = [
    ("/* &IFS3 and IEC2 |= m in a comment */", frozenset(), []),
    ('write("IEC2 |= m");', frozenset(), []),
    ("static volatile uint32_t *iec = &IEC2;", frozenset(), ["&IEC2", "*iec"]),
    ("*iec |= mask;", frozenset(), ["*iec"]),
    # `*d->iec` is caught by the descriptor-field rule, not the deref rule --
    # the `*` is not adjacent to the field name.
    ("*d->iec |= d->ie_mask;", frozenset(), ["d->iec", "ie_mask"]),
    ("IEC2 |= mask;", frozenset(), ["IEC2|=mask"]),
    ("IFS3 &= ~mask;", frozenset(), ["IFS3&=~mask"]),
    ("IEC3bits.DMA6IE = 1;", frozenset(), ["IEC3bits"]),
    ("if (IEC2 & mask) { x = IFS0; }", frozenset(), []),
    ("#if defined(_IFS4_CCP9IF_MASK)", frozenset(), []),
    ("k = _IFS3_CNAIF_MASK;", frozenset(), ["_IFS3_CNAIF_MASK"]),
    # The defect this whole port removes, pinned so it can never come back
    # unnoticed: the required shape passes, every runtime form fails.
    ("if (enable) { _DMA0IE = 1; } else { _DMA0IE = 0; }", frozenset(), []),
    ("_DMA0IF = 0;", frozenset(), []),
    ("_U1TXIF = 1u;", frozenset(), []),
    ("_DMA0IE = enable;", frozenset(), ["_DMA0IE=enable"]),
    ("_U1RXIE = rx_on;", frozenset(), ["_U1RXIE=rx_on"]),
    ("_DMA0IF = flag;", frozenset(), ["_DMA0IF=flag"]),
    ("_DMA0IE = enable ? 1 : 0;", frozenset(), ["_DMA0IE=enable?1:0"]),
    ("_DMA0IE |= 1;", frozenset(), ["_DMA0IE|=1"]),
    ("_DMA0IE = 2;", frozenset(), ["_DMA0IE=2"]),
    # Neither a read of an alias nor a capability probe is a write.
    ("if (_DMA0IF) { n++; }", frozenset(), []),
    ("#if defined(_CNAIE) && defined(_CNAIF)", frozenset(), []),
    # A bank macro on the right of an assignment stays with the bank-macro rule
    # and must not also trip the alias rule.
    ("k = _IFS3_CNAIF_MASK | m;", frozenset(), ["_IFS3_CNAIF_MASK"]),
    # ALLOW is per (rule, expression): the pinned clear-all passes, anything
    # else written to the same register in the same file still fails.
    ("IEC0 = 0u;", _INT_CTRL_CLEAR_ALL, []),
    ("IEC0 = saved_iec0;", _INT_CTRL_CLEAR_ALL, ["IEC0=saved_iec0"]),
    ("IEC0 |= 0u;", _INT_CTRL_CLEAR_ALL, ["IEC0|=0u"]),
    ("dev->iec = irq_reg;", _INT_CTRL_CLEAR_ALL, ["dev->iec"]),
]


def self_test():
    failures = 0
    for line, allowed, expected in SELF_TEST:
        got = [f[2] for f in scan_text(line, allowed)]
        if got != expected:
            failures += 1
            print("SELF-TEST FAIL: {!r}\n  expected {}\n  got      {}".format(
                line, expected, got))
    print("irq_atomicity_source_lint self-test: {} cases, {} failure(s)".format(
        len(SELF_TEST), failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="src/app", help="directory to scan (default: src/app)")
    ap.add_argument("--verbose", action="store_true", help="list every scanned file")
    ap.add_argument("--self-test", action="store_true",
                    help="check the rules against known good/bad lines and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    files = []
    for dirpath, _dirs, names in os.walk(args.root):
        for name in names:
            if name.endswith(EXTS):
                files.append(os.path.join(dirpath, name))
    files.sort()

    total = 0
    for path in files:
        findings = check_file(path)
        if args.verbose and not findings:
            print("ok   {}".format(path))
        for lineno, name, text, why in findings:
            total += 1
            print("{}:{}: {}: '{}' -- {}".format(
                path.replace("\\", "/"), lineno, name, text, why))

    print("irq_atomicity_source_lint: scanned {} files, {} violation(s)".format(
        len(files), total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
