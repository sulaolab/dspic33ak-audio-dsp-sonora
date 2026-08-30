#!/usr/bin/env python3
"""Single source of truth for the dual-partition UCA provisioning toolchain.

Encodes device / DFP / compiler identity plus every per-word config record with
its P1 and P2 (main + backup) physical addresses, compare mask, must_match flag,
and the semantic bit checks (§4.2, §5.2, §10 of the implementation directive).

Bit definitions VERIFIED against the DFP atdf
  <packs>/Microchip/dsPIC33AK-MP_DFP/1.3.185/atdf/dsPIC33AK512MPS512.atdf
  (<packs> = $DSPIC33AK_DFP's pack root, or ~/.mchp_packs)
  FDEVOPT (offset 0x20, initval 0xFFFFFFFF): ALTI2C1=0x8 ALTI2C2=0x10 ALTI2C3=0x20
                                             BISTDIS=0x40 SPI2PIN=0x2000
  FICD    (offset 0x10, initval 0xFFFFFFDF): JTAGEN=0x20 NOBTSWP=0x8000

These records must stay coupled to src/app/main.c `#pragma config`. If one
changes, change the other. (fw_uca.{c,h} was retired with the dual-partition
split, which now lives outside this repo.)
"""

DEVICE = "dsPIC33AK512MPS512"
# Deliberately a CONSTANT, unlike everywhere else that now reads the pack the
# application project pins. This is not "which pack to build with" -- it is a
# record of which atdf the bit offsets above were read out of BY HAND. If the
# project moves to another pack, the right answer is for a human to re-read the
# atdf and update this line, not for the line to follow along quietly and keep
# claiming a verification that was never redone.
#
# provision.ps1 passes its own copy of this string as --expect-dfp and
# verify_dual_partition_hex.py fails unless it matches: an intentional
# double entry, so a bundle records which pack its config words were derived
# from. Do not collapse the two into one source.
DFP = "dsPIC33AK-MP_DFP/1.3.185"
XCDSC = "3.31.01"


def _check_dfp_against_project():
    """Complain if the project has moved off the pack these bits were read from.

    A mismatch means the offsets/masks above are a claim about a pack this repo
    no longer builds with -- silent until a config word is written to the wrong
    place. Hard error, because there is nothing safe to do with a stale record.

    Unreadable project (this file copied out of the tree, a partial checkout) is
    NOT an error: it makes the check impossible, not failed.
    """
    import os
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import dfp_packs
        pinned = dfp_packs.pins().get(DEVICE)
    except Exception:
        return
    if pinned is None:
        return
    project = "%s/%s" % pinned
    if project != DFP:
        raise RuntimeError(
            "tools/uca_manifest.py records its bit definitions as verified "
            "against %s, but the application project now pins %s for %s.\n"
            "The UCA offsets and masks in this file were read out of that "
            "pack's atdf by hand, so they are a claim about %s and nothing "
            "here can tell whether they still hold.\n"
            "Re-read <packs>/Microchip/%s/atdf/%s.atdf, confirm the FDEVOPT / "
            "FICD / FWDT offsets and bit masks, then update DFP in this file "
            "and $expectDfp in buildtools/provision.ps1 together."
            % (DFP, project, DEVICE, DFP, project, DEVICE))


_check_dfp_against_project()

# UCA (per physical partition) region bases; each word at base+offset below.
# UCA is NOT remapped by NVMCON.P2ACTIV (unlike program flash) — fixed addresses.
UCA_P1_MAIN = 0x7F3000
UCA_P1_BACKUP = 0x7F3800
UCA_P2_MAIN = 0x7FB000
UCA_P2_BACKUP = 0x7FB800

# Word offsets within a UCA region.
OFF_FCP = 0x000
OFF_FICD = 0x010
OFF_FDEVOPT = 0x020
OFF_FWDT = 0x030

# Shared UCB (P1/P2 common) — FBOOT. MUST NOT be duplicated per-partition.
UCB_FBOOT = 0x7F40D0

ERASED = 0xFFFFFFFF

# Semantic bit masks (verified vs atdf).
FDEVOPT_ALTI2C2 = 0x10      # bit4: 0 => ALTI2C2 ON (ASCL2/ASDA2, WM8904-A). MUST be 0.
FDEVOPT_ALTI2C1 = 0x08
FDEVOPT_ALTI2C3 = 0x20
FICD_NOBTSWP = 0x8000       # bit15: 0 => NOBTSWP ON. Expected 0 for this build.
FICD_JTAGEN = 0x20
FBOOT_BTMODE_MASK = 0x3
FBOOT_BTMODE_DUAL = 0x2

# Program (application) region carried by the XMODEM *fu path. UCA/UCB are OUTSIDE
# this range — the extractor and verifier both assert that.
PROGRAM_REGION_LO = 0x800000
PROGRAM_REGION_HI = 0x840000   # exclusive

# Per-word records. `compare_mask` excludes reserved/unimplemented bits so that
# main==backup and P1==P2 comparisons ignore bits that may float. Words whose P1
# value is erased today (FCP/FWDT) are marked expected="erased": recorded, not
# ignored, and NOT cloned to P2 (cloning erased words is a no-op that only adds
# checksum surface). Only non-erased words (FICD/FDEVOPT) are cloned.
#
# compare_mask = union of documented bitfields for that word (0xFFFFFFFF if we
# intend an exact full-word compare because the word is either fully erased or
# fully specified). We keep exact-equality for identity checks (clone is byte-
# exact) and use the semantic masks below for the value assertions.
WORDS = [
    {
        "name": "FCP",
        "offset": OFF_FCP,
        "p1_main": UCA_P1_MAIN + OFF_FCP,
        "p1_backup": UCA_P1_BACKUP + OFF_FCP,
        "p2_main": UCA_P2_MAIN + OFF_FCP,
        "p2_backup": UCA_P2_BACKUP + OFF_FCP,
        "compare_mask": 0xFFFFFFFF,
        "must_match_p1_p2": True,
        "expected": "erased",
        "clone": False,
    },
    {
        "name": "FICD",
        "offset": OFF_FICD,
        "p1_main": UCA_P1_MAIN + OFF_FICD,
        "p1_backup": UCA_P1_BACKUP + OFF_FICD,
        "p2_main": UCA_P2_MAIN + OFF_FICD,
        "p2_backup": UCA_P2_BACKUP + OFF_FICD,
        "compare_mask": 0xFFFFFFFF,
        "must_match_p1_p2": True,
        "expected": None,           # cloned from P1; semantic check: NOBTSWP bit=0
        "clone": True,
        "checks": [("NOBTSWP", FICD_NOBTSWP, 0)],
    },
    {
        "name": "FDEVOPT",
        "offset": OFF_FDEVOPT,
        "p1_main": UCA_P1_MAIN + OFF_FDEVOPT,
        "p1_backup": UCA_P1_BACKUP + OFF_FDEVOPT,
        "p2_main": UCA_P2_MAIN + OFF_FDEVOPT,
        "p2_backup": UCA_P2_BACKUP + OFF_FDEVOPT,
        "compare_mask": 0xFFFFFFFF,
        "must_match_p1_p2": True,
        "expected": None,           # cloned from P1; semantic check: ALTI2C2 bit=0
        "clone": True,
        "checks": [("ALTI2C2", FDEVOPT_ALTI2C2, 0)],
    },
    {
        "name": "FWDT",
        "offset": OFF_FWDT,
        "p1_main": UCA_P1_MAIN + OFF_FWDT,
        "p1_backup": UCA_P1_BACKUP + OFF_FWDT,
        "p2_main": UCA_P2_MAIN + OFF_FWDT,
        "p2_backup": UCA_P2_BACKUP + OFF_FWDT,
        "compare_mask": 0xFFFFFFFF,
        "must_match_p1_p2": True,
        "expected": "erased",
        "clone": False,
    },
]

# The two contiguous windows cloned P1 -> P2 (byte-for-byte). Deliberately tight:
# [base, base+SPAN) covers the four config words (0x00..0x30) and nothing else, so
# the shared UCB FBOOT at 0x7F40D0 can never be swept in.
UCA_WINDOW_SPAN = 0x40
P1_TO_P2_DELTA = UCA_P2_MAIN - UCA_P1_MAIN   # 0x8000, applied per-window explicitly

CLONE_WINDOWS = [
    # (src_lo, src_hi_exclusive, dst_lo)
    (UCA_P1_MAIN, UCA_P1_MAIN + UCA_WINDOW_SPAN, UCA_P2_MAIN),
    (UCA_P1_BACKUP, UCA_P1_BACKUP + UCA_WINDOW_SPAN, UCA_P2_BACKUP),
]


def dump():
    print(f"device={DEVICE} dfp={DFP} xcdsc={XCDSC}")
    print(f"UCA P1 main=0x{UCA_P1_MAIN:06X} backup=0x{UCA_P1_BACKUP:06X}")
    print(f"UCA P2 main=0x{UCA_P2_MAIN:06X} backup=0x{UCA_P2_BACKUP:06X}")
    print(f"UCB FBOOT (shared)=0x{UCB_FBOOT:06X}  expected BTMODE=DUAL")
    for w in WORDS:
        print(f"  {w['name']:8s} p1_main=0x{w['p1_main']:06X} p1_bkp=0x{w['p1_backup']:06X}"
              f" p2_main=0x{w['p2_main']:06X} p2_bkp=0x{w['p2_backup']:06X}"
              f" clone={w['clone']} expected={w['expected']}")


if __name__ == "__main__":
    dump()
