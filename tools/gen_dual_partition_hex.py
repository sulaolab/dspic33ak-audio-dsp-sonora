#!/usr/bin/env python3
"""§5.2 manifest-driven dual-partition UCA provisioning generator.

Reads the P1 production HEX, clones the P1 UCA main+backup config windows to the
P2 UCA addresses (0x7FB000 / 0x7FB800) as freshly-checksummed Intel-HEX data
records, and writes a `<name>.bundle.hex` that a fresh PKOB4 bulk-erase+program
can burn so BOTH partitions come up with WM8904-A on ALTI2C2 (ASCL2/ASDA2).

NOT a text/line copy: every P2 byte is re-emitted through ihex_lite with a fresh
checksum and correct type-04 framing (§10). Conflicts (a differing byte already
present at a P2 target address) are a hard error. The output is re-parsed and the
generated values are printed and written to a report file.

Usage:
  gen_dual_partition_hex.py <p1_production.hex> [-o out.bundle.hex] [--report r.txt]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ihex_lite
import uca_manifest as M


def clone_p2_bytes(mem):
    """Return {p2_addr: value} for every present byte in the two P1 UCA windows,
    plus a human list of the (name, p1, p2, value) words cloned. Erased bytes
    (absent in the source) are NOT emitted (they'd program to 0xFF anyway)."""
    p2 = {}
    for (src_lo, src_hi, dst_lo) in M.CLONE_WINDOWS:
        for src in range(src_lo, src_hi):
            v = mem.get(src)
            if v is None:
                continue                      # erased byte -> leave P2 erased too
            dst = dst_lo + (src - src_lo)
            p2[dst] = v
    return p2


def report_words(mem, p2mem):
    rows = []
    for w in M.WORDS:
        p1m = ihex_lite.word32(mem, w["p1_main"])
        p1b = ihex_lite.word32(mem, w["p1_backup"])
        p2m = ihex_lite.word32(p2mem, w["p2_main"])
        p2b = ihex_lite.word32(p2mem, w["p2_backup"])
        rows.append((w["name"], w["clone"], p1m, p1b, p2m, p2b))
    return rows


def fmt(v):
    return "erased(0xFFFFFFFF)" if v is None else f"0x{v:08X}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("p1_hex")
    ap.add_argument("-o", "--out")
    ap.add_argument("--report")
    args = ap.parse_args()

    mem = ihex_lite.parse_hex(args.p1_hex)

    # Confirm P1 UCA is actually present (else we'd clone nothing).
    if ihex_lite.word32(mem, M.UCA_P1_MAIN + M.OFF_FDEVOPT) is None:
        sys.exit(f"ERROR: P1 UCA FDEVOPT absent at 0x{M.UCA_P1_MAIN + M.OFF_FDEVOPT:06X}"
                 f" -- is {args.p1_hex} the ASRC dual-partition production hex?")

    # Sanity: P1 FDEVOPT.ALTI2C2 must be 0 (ON) or we'd clone a broken config.
    p1_fdevopt = ihex_lite.word32(mem, M.UCA_P1_MAIN + M.OFF_FDEVOPT)
    if p1_fdevopt & M.FDEVOPT_ALTI2C2:
        sys.exit(f"ERROR: P1 FDEVOPT=0x{p1_fdevopt:08X} has ALTI2C2 bit4=1 (OFF); "
                 f"refusing to clone a config that would leave WM8904-A unreachable.")

    p2 = clone_p2_bytes(mem)
    if not p2:
        sys.exit("ERROR: nothing to clone (P1 UCA windows are empty).")

    # Conflict check: any P2 target already carries a DIFFERENT byte in the P1 hex.
    for addr, val in p2.items():
        cur = mem.get(addr)
        if cur is not None and cur != val:
            sys.exit(f"ERROR: conflict at 0x{addr:06X}: existing 0x{cur:02X} != new 0x{val:02X}")

    # Emit: original file verbatim (EOF dropped) + fresh P2 records + EOF.
    out = args.out or (os.path.splitext(args.p1_hex)[0] + ".bundle.hex")
    orig_lines = ihex_lite.read_lines_drop_eof(args.p1_hex)
    p2_lines = ihex_lite.emit_records(p2)
    with open(out, "w", newline="\r\n") as f:
        for ln in orig_lines:
            f.write(ln + "\n")
        for ln in p2_lines:
            f.write(ln + "\n")
        f.write(ihex_lite.EOF_RECORD + "\n")

    # Re-parse the output and confirm every P2 byte round-tripped.
    check = ihex_lite.parse_hex(out)
    for addr, val in p2.items():
        if check.get(addr) != val:
            sys.exit(f"ERROR: re-parse mismatch at 0x{addr:06X}: "
                     f"wrote 0x{val:02X} got {check.get(addr)}")

    rows = report_words(mem, check)
    lines = [f"# gen_dual_partition_hex  device={M.DEVICE} dfp={M.DFP}",
             f"# in : {args.p1_hex}",
             f"# out: {out}",
             f"# cloned {len(p2)} bytes into P2 UCA (main 0x{M.UCA_P2_MAIN:06X}, "
             f"backup 0x{M.UCA_P2_BACKUP:06X})",
             ""]
    for name, clone, p1m, p1b, p2m, p2b in rows:
        lines.append(f"  {name:8s} clone={str(clone):5s} "
                     f"P1(main {fmt(p1m)} bkp {fmt(p1b)})  "
                     f"P2(main {fmt(p2m)} bkp {fmt(p2b)})")
    text = "\n".join(lines)
    print(text)

    rep = args.report or (os.path.splitext(out)[0] + ".gen_report.txt")
    with open(rep, "w", newline="\r\n") as f:
        f.write(text + "\n")
    print(f"\nbundle : {out}\nreport : {rep}")


RETIRED = """\
gen_dual_partition_hex.py is retired: the A/B Flash-Dual-Partition mechanism it
provisions has been removed from this tree (src/app/fw_update/ and
src/app/uart_app/fw_console.c are gone), and src/app/main.c sets BTMODE=SINGLE. A P1+P2
UCA bundle would program partition fuses for a boot mode the firmware cannot
enter, so this refuses to emit one.

Serial firmware update is now the resident bootloader:
  ./buildtools/build.ps1 -Full -Configuration dsPIC33AK512_ASRC_BOOTAPP
  ./buildtools/flashauto.ps1 -Serial <PKOB4_SERIAL>

The completed A/B implementation is maintained outside this repository.
This file is kept for its UCA layout reference, not as a usable generator."""


if __name__ == "__main__":
    sys.exit(RETIRED)
