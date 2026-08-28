#!/usr/bin/env python3
"""Minimal, dependency-free Intel HEX helpers shared by the dual-partition UCA
provisioning tools (gen/verify/test). Deliberately hand-rolled (no `intelhex`
dependency) to match the existing tools/extract_p1_image.py and tools/scan_uca.py.

Address space here is the *byte* address as it appears in the HEX (type-04 upper
16 bits + record offset). For this device the config/UCA words appear at their
literal addresses (0x7F3xxx / 0x7FBxxx / 0x7F40D0), as proven by tools/scan_uca.py.
"""
import sys


def parse_hex(path):
    """Return a dict {byte_address: value(0..255)} of all data records."""
    mem = {}
    ulba = 0
    with open(path, "r") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line[0] != ":":
                continue
            b = bytes.fromhex(line[1:])
            if sum(b) & 0xFF:
                raise ValueError(f"{path}:{lineno}: bad Intel-HEX checksum")
            count = b[0]
            offset = (b[1] << 8) | b[2]
            rectype = b[3]
            data = b[4:4 + count]
            if rectype == 0x00:            # data
                base = (ulba << 16) + offset
                for i, byte in enumerate(data):
                    mem[base + i] = byte
            elif rectype == 0x04:          # extended linear address
                ulba = (data[0] << 8) | data[1]
            elif rectype == 0x01:          # EOF
                break
            # 0x02/0x03/0x05 ignored (unused by this toolchain)
    return mem


def word32(mem, addr):
    """Little-endian 32-bit at byte address `addr`; None if fully absent,
    else present bytes with any absent byte read as 0xFF (erased)."""
    bs = [mem.get(addr + i) for i in range(4)]
    if all(x is None for x in bs):
        return None
    return sum((x if x is not None else 0xFF) << (8 * i) for i, x in enumerate(bs))


def _record(rectype, offset, data):
    n = len(data)
    body = [n, (offset >> 8) & 0xFF, offset & 0xFF, rectype] + list(data)
    chk = (-sum(body)) & 0xFF
    return ":" + bytes(body + [chk]).hex().upper()


def emit_records(byte_dict, max_len=16):
    """Deterministically emit type-04 + type-00 records for `byte_dict`
    ({addr: value}), sorted by address, splitting on 16-byte alignment and
    on upper-16 (type-04 segment) boundaries. Returns a list of ':' lines."""
    lines = []
    cur_ulba = None
    run_addr = None
    run = []

    def flush():
        nonlocal run_addr, run
        if run:
            lines.append(_record(0x00, run_addr & 0xFFFF, run))
            run_addr, run = None, []

    for a in sorted(byte_dict):
        ulba = (a >> 16) & 0xFFFF
        if ulba != cur_ulba:
            flush()
            lines.append(_record(0x04, 0, [(ulba >> 8) & 0xFF, ulba & 0xFF]))
            cur_ulba = ulba
        # break run on discontiguity, alignment, or length cap
        if (run and (a != run_addr + len(run)
                     or len(run) >= max_len
                     or (a & 0x0F) == 0)):
            flush()
        if not run:
            run_addr = a
        run.append(byte_dict[a])
    flush()
    return lines


def read_lines_drop_eof(path):
    """Return the file's ':' lines with any EOF (type-01) record removed."""
    out = []
    with open(path, "r", newline="") as f:
        for line in f:
            s = line.strip()
            if not s or s[0] != ":":
                continue
            b = bytes.fromhex(s[1:])
            if b[3] == 0x01:
                continue
            out.append(s)
    return out


EOF_RECORD = ":00000001FF"
