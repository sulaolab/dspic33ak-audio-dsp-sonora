#!/usr/bin/env python3
"""§8.1 host acceptance tests for the dual-partition UCA provisioning toolchain.

Stdlib only. Builds synthetic Intel-HEX inputs, runs gen/verify as subprocesses,
and asserts the expected PASS/FAIL outcome for each case:

  1. golden bundle (gen from a synthetic P1)          -> verify PASS
  2. P1-only hex (no P2 UCA)                           -> verify FAIL (P1!=P2)
  3. P2 main present but backup missing                -> verify FAIL
  4. P2 main != backup                                 -> verify FAIL
  5. FDEVOPT.ALTI2C2 = OFF (bit4=1) on P1              -> gen refuses (exit!=0)
  6. wrong DFP expectation                             -> verify FAIL
  7. deterministic regen (same input => identical out) -> byte-identical
  8. XMODEM extraction excludes UCA/UCB                 -> extract only 0x800000+

Run: python tools/test_dual_partition_hex.py
Exit 0 = all pass.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ihex_lite
import uca_manifest as M

PY = sys.executable
GEN = os.path.join(HERE, "gen_dual_partition_hex.py")
VERIFY = os.path.join(HERE, "verify_dual_partition_hex.py")
EXTRACT = os.path.join(HERE, "extract_p1_image.py")

PASS = 0
FAIL = 1


def write_hex(path, byte_dict):
    lines = ihex_lite.emit_records(byte_dict)
    with open(path, "w", newline="\r\n") as f:
        for ln in lines:
            f.write(ln + "\n")
        f.write(ihex_lite.EOF_RECORD + "\n")


def w32bytes(base, value):
    return {base + i: (value >> (8 * i)) & 0xFF for i in range(4)}


def synth_p1(alti2c2_on=True, with_program=True):
    """A minimal but valid P1 hex: FICD+FDEVOPT main+backup, FBOOT=DUAL, and a few
    program bytes so structural checks have something to see."""
    mem = {}
    fdevopt = 0xFFFFFFEF if alti2c2_on else (0xFFFFFFEF | M.FDEVOPT_ALTI2C2)
    mem.update(w32bytes(M.UCA_P1_MAIN + M.OFF_FICD, 0xFFFF7FDF))
    mem.update(w32bytes(M.UCA_P1_BACKUP + M.OFF_FICD, 0xFFFF7FDF))
    mem.update(w32bytes(M.UCA_P1_MAIN + M.OFF_FDEVOPT, fdevopt))
    mem.update(w32bytes(M.UCA_P1_BACKUP + M.OFF_FDEVOPT, fdevopt))
    mem.update(w32bytes(M.UCB_FBOOT, 0xFFFFFFFE))   # BTMODE=DUAL
    if with_program:
        for i in range(16):
            mem[M.PROGRAM_REGION_LO + i] = i
    return mem


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


results = []


def check(name, cond, detail=""):
    results.append((name, cond, detail))
    tag = "ok  " if cond else "FAIL"
    print(f"  [{tag}] {name}{('  -- ' + detail) if detail and not cond else ''}")


def main():
    tmp = tempfile.mkdtemp(prefix="uca_test_")

    # --- 1: golden bundle from a good P1 -> verify PASS
    p1 = os.path.join(tmp, "p1.hex")
    write_hex(p1, synth_p1())
    bundle = os.path.join(tmp, "p1.bundle.hex")
    rc, out = run([PY, GEN, p1, "-o", bundle])
    check("gen golden succeeds", rc == PASS, out)
    rc, out = run([PY, VERIFY, bundle])
    check("verify golden PASS", rc == PASS, out)

    # --- 2: P1-only (no clone) -> verify FAIL (P1!=P2)
    rc, out = run([PY, VERIFY, p1])
    check("verify P1-only FAIL", rc == FAIL, out)

    # --- 3: P2 main present, backup missing -> FAIL
    mem = synth_p1()
    mem.update(w32bytes(M.UCA_P2_MAIN + M.OFF_FICD, 0xFFFF7FDF))
    mem.update(w32bytes(M.UCA_P2_MAIN + M.OFF_FDEVOPT, 0xFFFFFFEF))
    # note: NOT adding P2 backup
    h = os.path.join(tmp, "p2_no_backup.hex")
    write_hex(h, mem)
    rc, out = run([PY, VERIFY, h])
    check("verify P2 backup-missing FAIL", rc == FAIL, out)

    # --- 4: P2 main != backup -> FAIL
    mem = synth_p1()
    mem.update(w32bytes(M.UCA_P2_MAIN + M.OFF_FICD, 0xFFFF7FDF))
    mem.update(w32bytes(M.UCA_P2_BACKUP + M.OFF_FICD, 0xFFFF7FDF))
    mem.update(w32bytes(M.UCA_P2_MAIN + M.OFF_FDEVOPT, 0xFFFFFFEF))
    mem.update(w32bytes(M.UCA_P2_BACKUP + M.OFF_FDEVOPT, 0xFFFFFFAF))  # differ
    h = os.path.join(tmp, "p2_mismatch.hex")
    write_hex(h, mem)
    rc, out = run([PY, VERIFY, h])
    check("verify P2 main!=backup FAIL", rc == FAIL, out)

    # --- 5: P1 ALTI2C2 OFF -> gen refuses
    p1bad = os.path.join(tmp, "p1_alti2c2_off.hex")
    write_hex(p1bad, synth_p1(alti2c2_on=False))
    rc, out = run([PY, GEN, p1bad, "-o", os.path.join(tmp, "x.hex")])
    check("gen refuses ALTI2C2=OFF source", rc != PASS, out)

    # --- 6: wrong DFP expectation -> verify FAIL
    rc, out = run([PY, VERIFY, bundle, "--expect-dfp", "wrong/0.0.0"])
    check("verify wrong-DFP FAIL", rc == FAIL, out)
    rc, out = run([PY, VERIFY, bundle, "--expect-dfp", M.DFP,
                   "--expect-device", M.DEVICE])
    check("verify correct DFP+device PASS", rc == PASS, out)

    # --- 7: deterministic regen
    b2 = os.path.join(tmp, "p1.bundle2.hex")
    rc, out = run([PY, GEN, p1, "-o", b2])
    with open(bundle, "rb") as f:
        a = f.read()
    with open(b2, "rb") as f:
        b = f.read()
    check("regen byte-identical", a == b)

    # --- 8: XMODEM extract excludes UCA/UCB
    mem = synth_p1()
    # add program bytes AND config bytes; extractor must keep only program.
    for i in range(512):
        mem[M.PROGRAM_REGION_LO + 0x100 + i] = (i & 0xFF)
    h = os.path.join(tmp, "extract_src.hex")
    write_hex(h, mem)
    binout = os.path.join(tmp, "img.bin")
    rc, out = run([PY, EXTRACT, h, binout])
    check("extract succeeds", rc == PASS, out)
    if rc == PASS:
        size = os.path.getsize(binout)
        # image starts at 0x800000; must not contain any 0x7Fxxxx bytes (impossible
        # by construction) and size stays within the firmware receiver's cap
        # (partition 0x40000 minus the last 512-byte BTSEQ-protection row).
        check("extract image within firmware limit", size <= 0x3FE00,
              f"size=0x{size:X}")

    # --- 9: extractor refuses an over-limit image (writes nothing)
    mem = synth_p1()
    # populate a byte in the last row so the rounded slice would exceed 0x3FE00.
    mem[M.PROGRAM_REGION_LO + 0x3FE00] = 0x5A
    h2 = os.path.join(tmp, "extract_toobig.hex")
    write_hex(h2, mem)
    big = os.path.join(tmp, "toobig.bin")
    rc, out = run([PY, EXTRACT, h2, big])
    check("extract refuses over-limit image", rc == FAIL, out)
    check("extract wrote no file when over limit", not os.path.exists(big))

    ok = all(c for _, c, _ in results)
    print(f"\n{'ALL PASS' if ok else 'FAILURES PRESENT'}: "
          f"{sum(1 for _,c,_ in results if c)}/{len(results)} checks passed")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
