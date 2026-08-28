#!/usr/bin/env python3
"""Build or inspect a Sonora resident-bootloader application package.

The wire file is a 64-byte manifest followed by the dense application bytes.
The bootloader erases the fixed manifest page first, programs/verifies the
payload, then writes this manifest at the panel's final page as the last commit
operation.

Two Flash layouts exist, one per device, and they differ only in where the panel
ends. Which one a package is for is never passed in as an option: `build` reads
the linker's own -p<device> flag out of the .map it is already given, and every
later step reads the layout_id out of the manifest itself. An option could be
wrong; neither of those can be.

The C side of this table is src/shared/resident_de_manifest.h -- keep the two in
step, and note that the bootloader compares layout_id before it writes anything,
so a package sent to the other board is refused rather than half-programmed.
"""
import argparse
import binascii
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ihex_lite

BOOT_BASE = 0x800000
MAGIC = b"SONORA1\0"
FORMAT_VERSION = 1
HEADER = struct.Struct("<8sHHIIIIIIII16sI")
HEADER_SIZE = HEADER.size

Layout = collections.namedtuple(
    "Layout", "layout_id name device app_base manifest_address flash_end")

LAYOUTS = (
    Layout(0x53414B31, "ak512", "33AK512MPS512",  # "SAK1"
           app_base=0x808000, manifest_address=0x87F000, flash_end=0x880000),
    # app_base is 0x804000 here and 0x808000 above because the MC106's boot region is
    # 16 KiB rather than 32 KiB -- its resident bootloader was measured down to 15,156 B
    # and the application takes what that released. src/shared/resident_de_manifest.h is
    # the authority and states the measurement; these must match it, and the layout_id is
    # what stops a package built for one arrangement from being written with the other.
    Layout(0x53414B33, "ak128", "33AK128MC106",   # "SAK3"
           app_base=0x804000, manifest_address=0x81F000, flash_end=0x820000),
)

# Layout IDs that were once issued and are not any more. Kept so an old package is
# refused BY NAME rather than as an unrecognized number: these are the ones most likely
# to be offered by mistake, because they were real and someone still has the .sfb.
#
# They cannot stay in LAYOUTS. A retired layout shares its device and its manifest page
# with the layout that replaced it, and both "which layout is this device" and "who
# commits at this address" have to stay single-valued -- test_serial_boot_package.py
# asserts exactly that. So they are named here and nowhere else, and an ID is never
# reissued for a different arrangement.
RETIRED_LAYOUTS = {
    0x53414B32: "SAK2, the AK128MC106 28 KiB boot region with the application at "
                "0x807000, retired 2026-08-20 when that region became 16 KiB",
}


def layout_for_id(layout_id):
    for layout in LAYOUTS:
        if layout.layout_id == layout_id:
            return layout
    known = ", ".join(f"0x{l.layout_id:08X} ({l.name})" for l in LAYOUTS)
    retired = RETIRED_LAYOUTS.get(layout_id)
    if retired is not None:
        raise ValueError(
            f"package layout 0x{layout_id:08X} is {retired}. It cannot be installed by "
            f"a bootloader built for the current layouts ({known}); rebuild the package "
            f"from a current image. A board still carrying the retired bootloader is "
            f"moved to the current layout by a PKOB4 factory flash, not by serial update")
    raise ValueError(
        f"package layout 0x{layout_id:08X} is not one this tool knows ({known})")


def layout_for_device(device):
    """Accept '33AK128MC106', 'dsPIC33AK128MC106' or the short 'ak128'."""
    wanted = device.lower().removeprefix("dspic")
    for layout in LAYOUTS:
        if wanted in (layout.device.lower(), layout.name):
            return layout
    known = ", ".join(l.device for l in LAYOUTS)
    raise ValueError(f"device {device} has no resident-boot layout ({known})")


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def parse_symbol(map_path, name):
    pattern = re.compile(r"^\s*(0x[0-9A-Fa-f]+)\s+" + re.escape(name) + r"\s*$")
    with open(map_path, "r", errors="replace") as source:
        for line in source:
            match = pattern.match(line)
            if match:
                return int(match.group(1), 16)
    raise ValueError(f"symbol {name} not found in {map_path}")


def parse_layout(map_path):
    """The layout of the image this .map describes, from the linker's own -p flag.

    The device is stated once per link, by the linker, in the command line the map
    reproduces at the top. Deriving it here means the package cannot be labelled
    for a device it was not linked for.
    """
    pattern = re.compile(r"^\s*-p(33AK\S+)\s*\\?\s*$")
    with open(map_path, "r", errors="replace") as source:
        for line in source:
            match = pattern.match(line)
            if match:
                return layout_for_device(match.group(1))
    raise ValueError(
        f"{map_path} does not contain the linker's -p<device> flag, so the "
        "target device could not be determined")


def make_header(payload, entry, ivt, firmware_version, layout):
    values = (MAGIC, FORMAT_VERSION, HEADER_SIZE, layout.layout_id,
              layout.app_base,
              len(payload), crc32(payload), entry, ivt, firmware_version,
              0, bytes(16), 0)
    raw = bytearray(HEADER.pack(*values))
    struct.pack_into("<I", raw, HEADER_SIZE - 4, crc32(raw[:-4]))
    return bytes(raw)


def decode_header(raw):
    if len(raw) < HEADER_SIZE:
        raise ValueError("package is shorter than the 64-byte manifest")
    fields = HEADER.unpack(raw[:HEADER_SIZE])
    names = ("magic", "format_version", "header_size", "layout_id", "app_base",
             "payload_length", "payload_crc32", "entry_address", "ivt_address",
             "firmware_version", "flags", "reserved", "header_crc32")
    result = dict(zip(names, fields))
    if result["magic"] != MAGIC:
        raise ValueError("bad package magic")
    if result["format_version"] != FORMAT_VERSION or result["header_size"] != HEADER_SIZE:
        raise ValueError("unsupported manifest format")
    layout = layout_for_id(result["layout_id"])
    if result["app_base"] != layout.app_base:
        raise ValueError("package targets a different Flash layout")
    result["layout"] = layout
    if result["header_crc32"] != crc32(raw[:HEADER_SIZE - 4]):
        raise ValueError("manifest CRC32 mismatch")
    if (result["payload_length"] == 0 or
            result["payload_length"] > layout.manifest_address - layout.app_base or
            result["payload_length"] & 15):
        raise ValueError("payload length is outside the application region")
    if result["ivt_address"] != layout.app_base:
        raise ValueError("manifest IVT does not match the application base")
    if not (layout.app_base <= result["entry_address"] <
            layout.app_base + result["payload_length"]):
        raise ValueError("manifest entry is outside the payload")
    if result["flags"] != 0 or result["reserved"] != bytes(16):
        raise ValueError("unsupported manifest flags or reserved data")
    return result


def build(hex_path, map_path, output, firmware_version):
    layout = parse_layout(map_path)
    mem = ihex_lite.parse_hex(hex_path)
    protected = sorted(a for a in mem if BOOT_BASE + 4 <= a < layout.app_base)
    if protected:
        raise ValueError(f"application HEX writes bootloader space at 0x{protected[0]:06X}")
    reserved = sorted(a for a in mem
                      if layout.manifest_address <= a < layout.flash_end)
    if reserved:
        raise ValueError(f"application HEX writes manifest page at 0x{reserved[0]:06X}")
    # Beyond the panel is not "outside the application region" -- it is a HEX built
    # for a bigger part, and saying so is more useful than a manifest-page complaint.
    overflow = sorted(a for a in mem if a >= layout.flash_end)
    if overflow:
        raise ValueError(
            f"application HEX writes 0x{overflow[0]:06X}, past the end of the "
            f"{layout.device} panel (0x{layout.flash_end:06X})")
    app_bytes = [a for a in mem if layout.app_base <= a < layout.manifest_address]
    if not app_bytes:
        raise ValueError("application HEX contains no application bytes")
    end = max(app_bytes) + 1
    length = (end - layout.app_base + 15) & ~15
    payload = bytes(mem.get(a, 0xFF)
                    for a in range(layout.app_base, layout.app_base + length))
    entry = parse_symbol(map_path, "__resetPRI")
    ivt = parse_symbol(map_path, "__ivt_0")
    if not (layout.app_base <= entry < layout.manifest_address) or ivt != layout.app_base:
        raise ValueError(f"unexpected entry/IVT: entry=0x{entry:06X} ivt=0x{ivt:06X}")
    header = make_header(payload, entry, ivt, firmware_version, layout)
    with open(output, "wb") as target:
        target.write(header)
        target.write(payload)
    return decode_header(header), payload


def verify(path):
    with open(path, "rb") as source:
        raw = source.read()
    info = decode_header(raw)
    payload = raw[HEADER_SIZE:]
    if len(payload) != info["payload_length"]:
        raise ValueError("package size does not match manifest payload length")
    if crc32(payload) != info["payload_crc32"]:
        raise ValueError("payload CRC32 mismatch")
    return info


def print_info(info, path):
    layout = info["layout"]
    print(f"package={path}")
    print(f"layout=0x{info['layout_id']:08X} ({layout.name}, {layout.device}) "
          f"version={info['format_version']}")
    print(f"payload=0x{info['app_base']:06X}+0x{info['payload_length']:X} "
          f"crc32=0x{info['payload_crc32']:08X}")
    print(f"entry=0x{info['entry_address']:06X} ivt=0x{info['ivt_address']:06X} "
          f"firmware={info['firmware_version']}")
    print(f"commit-manifest=0x{layout.manifest_address:06X}")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("build")
    create.add_argument("hex")
    create.add_argument("map")
    create.add_argument("-o", "--output", required=True)
    create.add_argument("--firmware-version", type=lambda x: int(x, 0), required=True)
    inspect = sub.add_parser("verify")
    inspect.add_argument("package")
    args = parser.parse_args()
    try:
        if args.command == "build":
            info, _ = build(args.hex, args.map, args.output, args.firmware_version)
            print_info(info, args.output)
        else:
            print_info(verify(args.package), args.package)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
