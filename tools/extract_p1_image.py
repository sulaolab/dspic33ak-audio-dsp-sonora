#!/usr/bin/env python3
"""Extract the flat program-flash slice [0x800000, end] from an Intel HEX file
into a raw binary, 0xFF-padded, for feeding the *fu XMODEM receiver into the
inactive partition. dsPIC33A program flash is flat/1:1 (no phantom bytes), so a
verbatim P1 slice is a bootable image for whichever partition is inactive (HW
remaps the active partition to 0x800000).

The output is NOT partition-specific: the *same* reflash image is sent by XMODEM
to whichever partition is currently inactive (P1->P2 or P2->P1). The conventional
output name is therefore partition-agnostic: `reflash_image.bin` (the default when
no output path is given), NOT `p2_image.bin`.

Usage:
  python tools/extract_p1_image.py <production.hex> [reflash_image.bin]
"""
import sys

PART_BASE = 0x800000
PART_END  = 0x840000  # one partition = 256 KB; never include past this

# Max bytes the firmware *fu XMODEM receiver accepts (fw_update.c): one partition
# (0x40000) MINUS the last 512-byte row, which holds BTSEQ and is write-protected
# by the receiver. An image larger than this would be rejected mid-transfer by the
# board, so we refuse to emit one here rather than fail late on the wire.
FW_MAX_IMAGE_BYTES = 0x3FE00

# Partition-agnostic default: the XMODEM payload is the reflash image for the
# inactive partition, not a "P2 image". See module docstring.
DEFAULT_BIN = "reflash_image.bin"

def parse_hex(path):
    mem = {}
    ulba = 0
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ":":
                continue
            b = bytes.fromhex(line[1:])
            count = b[0]
            offset = (b[1] << 8) | b[2]
            rectype = b[3]
            data = b[4:4 + count]
            if rectype == 0x00:      # data
                base = (ulba << 16) + offset
                for i, byte in enumerate(data):
                    mem[base + i] = byte
            elif rectype == 0x04:    # extended linear address
                ulba = (data[0] << 8) | data[1]
            elif rectype == 0x01:    # EOF
                break
            # ignore 02/03/05
    return mem

def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <production.hex> [{DEFAULT_BIN}]", file=sys.stderr)
        sys.exit(2)
    hexpath = sys.argv[1]
    binpath = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_BIN
    mem = parse_hex(hexpath)
    prog = {a: v for a, v in mem.items() if PART_BASE <= a < PART_END}
    if not prog:
        print("no program bytes in [0x800000,0x840000)", file=sys.stderr)
        sys.exit(1)
    # INVARIANT (directive §5.3 item 10 / §10): the *fu XMODEM slice carries ONLY
    # the program region [0x800000,0x840000). The per-partition UCA (0x7F3xxx /
    # 0x7FBxxx) and shared UCB FBOOT (0x7F40D0) live BELOW 0x800000 and are never
    # part of this image. A code-only serial update therefore cannot change a
    # partition's config fuses -- UCA provisioning is a flash-time concern only.
    assert all(a >= PART_BASE for a in prog), "UCA/UCB byte leaked into program slice"
    assert PART_END <= PART_BASE + 0x40000, "partition slice exceeds one 256KB partition"
    lo = PART_BASE
    hi = max(prog) + 1
    # round up to a 512-byte row boundary so rows align cleanly
    if hi % 512:
        hi += 512 - (hi % 512)
    # The firmware receiver caps the image at FW_MAX_IMAGE_BYTES (partition minus
    # the BTSEQ-protection row). If the rounded slice exceeds that, the board would
    # reject the tail mid-transfer -- fail here and write NOTHING rather than emit
    # an image that can only fail on the wire.
    if (hi - lo) > FW_MAX_IMAGE_BYTES:
        print(f"error: image size {hi-lo} (0x{hi-lo:X}) exceeds firmware limit "
              f"0x{FW_MAX_IMAGE_BYTES:X} (partition minus BTSEQ row); "
              f"top populated addr=0x{max(prog):06X}. No file written.",
              file=sys.stderr)
        sys.exit(1)
    out = bytearray(b"\xff" * (hi - lo))
    for a, v in prog.items():
        out[a - lo] = v
    with open(binpath, "wb") as f:
        f.write(out)
    used = len(prog)
    print(f"span=0x{lo:06X}..0x{hi:06X} size={hi-lo} (0x{hi-lo:X}) "
          f"bytes populated={used} padded={hi-lo-used}")

if __name__ == "__main__":
    main()
