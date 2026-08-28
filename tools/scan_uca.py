#!/usr/bin/env python3
# Read-only scan of an Intel HEX for the dual-partition UCA config words.
# Reports the value present at each address, or "NO RECORD (=erased 0xFFFFFFFF)".
import sys

if len(sys.argv) < 2:
    # There is no sensible default: which build's HEX to scan is the whole question,
    # and the path it used to default to was one clone on one machine.
    sys.exit("usage: scan_uca.py <path to dspic33ak_audio_dsp.X.production.hex>")
HEX = sys.argv[1]

# byte-address -> value map (each record byte)
mem = {}
ext = 0  # upper 16 bits from type-04 records
with open(HEX) as f:
    for line in f:
        line = line.strip()
        if not line.startswith(":"):
            continue
        b = bytes.fromhex(line[1:])
        n = b[0]
        addr = (b[1] << 8) | b[2]
        typ = b[3]
        data = b[4:4 + n]
        if typ == 0x04:
            ext = (data[0] << 8) | data[1]
        elif typ == 0x00:
            base = (ext << 16) | addr
            for i, v in enumerate(data):
                mem[base + i] = v

def word32(a):
    # little-endian 32-bit at byte address a; None if fully absent
    bs = [mem.get(a + i) for i in range(4)]
    if all(x is None for x in bs):
        return None
    return sum((x if x is not None else 0xFF) << (8 * i) for i, x in enumerate(bs))

targets = [
    ("UCA1/P1 FCP     main", 0x7F3000), ("UCA1/P1 FCP     backup", 0x7F3800),
    ("UCA1/P1 FICD    main", 0x7F3010), ("UCA1/P1 FICD    backup", 0x7F3810),
    ("UCA1/P1 FDEVOPT main", 0x7F3020), ("UCA1/P1 FDEVOPT backup", 0x7F3820),
    ("UCA1/P1 FWDT    main", 0x7F3030), ("UCA1/P1 FWDT    backup", 0x7F3830),
    ("UCA2/P2 FCP     main", 0x7FB000), ("UCA2/P2 FCP     backup", 0x7FB800),
    ("UCA2/P2 FICD    main", 0x7FB010), ("UCA2/P2 FICD    backup", 0x7FB810),
    ("UCA2/P2 FDEVOPT main", 0x7FB020), ("UCA2/P2 FDEVOPT backup", 0x7FB820),
    ("UCA2/P2 FWDT    main", 0x7FB030), ("UCA2/P2 FWDT    backup", 0x7FB830),
    ("UCB FBOOT (shared)   ", 0x7F40D0),
]
print(f"# {HEX}")
for name, a in targets:
    w = word32(a)
    if w is None:
        print(f"  {name} @0x{a:06X}  NO RECORD (=erased 0xFFFFFFFF)")
    else:
        note = ""
        if a in (0x7F3020, 0x7F3820, 0x7FB020, 0x7FB820):
            note = f"  ALTI2C2(bit4)={'0->ON(ASCL2/ASDA2)' if not (w>>4)&1 else '1->OFF(SCL2/SDA2)'}"
        print(f"  {name} @0x{a:06X}  0x{w:08X}{note}")

# also report any records anywhere in 0x7FB000..0x7FB840
present = [a for a in mem if 0x7FB000 <= a <= 0x7FB83F]
print(f"\n# bytes present in P2-UCA region 0x7FB000..0x7FB83F: {len(present)}")
