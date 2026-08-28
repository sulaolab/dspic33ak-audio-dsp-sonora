"""Histogram of instructions whose data operand lands in the IFSx/IECx window.

IFS0..IFS11 = 0x90..0xbf, IEC0..IEC11 = 0xc0..0xef (measured from the ELF symbol
table).  Counts per mnemonic, plus the owning function for the byte write-backs
(mov.b), which are the read-modify-write halves this work is removing.
"""
import re
import sys
from collections import Counter, defaultdict

FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
INSN = re.compile(r"^\s*[0-9a-f]+:\t(?:[0-9a-f]{2} )+\t(\S+)\s*(.*)$")
ADDR = re.compile(r"(?<![#\w.])0x([0-9a-f]{1,4})\b")


def scan(path):
    hist = Counter()
    movb_by_func = Counter()
    func = "?"
    for line in open(path, encoding="utf-8", errors="replace"):
        m = FUNC.match(line)
        if m:
            name = m.group(1)
            if not name.startswith(".L") and not re.fullmatch(r"L\d+", name):
                func = name
            continue
        m = INSN.match(line)
        if not m:
            continue
        mnem, ops = m.group(1), m.group(2)
        for a in ADDR.findall(ops):
            v = int(a, 16)
            if 0x90 <= v <= 0xEF:
                hist[mnem] += 1
                if mnem == "mov.b":
                    movb_by_func[func] += 1
                break
    return hist, movb_by_func


for p in sys.argv[1:]:
    hist, movb = scan(p)
    print("=== %s" % p)
    for k in sorted(hist):
        print("    %-10s %d" % (k, hist[k]))
    if movb:
        print("    mov.b write-backs by function:")
        for f, n in movb.most_common():
            print("        %-50s %d" % (f, n))
