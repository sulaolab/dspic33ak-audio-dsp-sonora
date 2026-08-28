#!/usr/bin/env python3
"""Combine the resident bootloader and one committed application into factory HEX."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ihex_lite
import serial_boot_package as package


def merge(boot_hex, app_package, output):
    boot = ihex_lite.parse_hex(boot_hex)
    info = package.verify(app_package)
    layout = info["layout"]
    with open(app_package, "rb") as source:
        raw = source.read()
    header = raw[:package.HEADER_SIZE]
    payload = raw[package.HEADER_SIZE:]

    illegal = sorted(address for address in boot
                     if layout.app_base <= address < layout.flash_end)
    if illegal:
        raise ValueError(
            f"bootloader HEX writes application/manifest space at 0x{illegal[0]:06X}")
    boot_program = [address for address in boot
                    if package.BOOT_BASE <= address < layout.app_base]
    if not boot_program or package.BOOT_BASE not in boot:
        raise ValueError("bootloader HEX has no reset vector in the resident region")

    image = dict(boot)
    image.update({layout.app_base + index: value
                  for index, value in enumerate(payload)})
    image.update({layout.manifest_address + index: value
                  for index, value in enumerate(header)})
    with open(output, "w", newline="\r\n") as target:
        target.write("\n".join(ihex_lite.emit_records(image) +
                               [ihex_lite.EOF_RECORD, ""]))
    verify_factory(output)
    return info


def read_committed_manifest(memory):
    """Find the committed manifest without being told which layout this image is.

    Each layout commits at its own panel's final page, so the manifest is looked
    for at every known address and the one that decodes identifies the image. Two
    successes would mean the addresses are no longer distinguishing and is reported
    rather than resolved by preference -- the AK128 manifest page sits inside the
    AK512 application region, so "it decoded first" is not an argument.
    """
    found = []
    for layout in package.LAYOUTS:
        try:
            header = bytes(memory[layout.manifest_address + index]
                           for index in range(package.HEADER_SIZE))
        except KeyError:
            continue
        try:
            found.append((layout, package.decode_header(header)))
        except ValueError:
            continue
    if not found:
        raise ValueError("factory image has no committed manifest")
    if len(found) > 1:
        names = ", ".join(layout.name for layout, _ in found)
        raise ValueError(f"factory image carries more than one manifest ({names})")
    layout, info = found[0]
    if info["layout"] is not layout:
        raise ValueError(
            f"manifest at 0x{layout.manifest_address:06X} declares layout "
            f"{info['layout'].name}, which commits elsewhere")
    return info


def verify_factory(path):
    memory = ihex_lite.parse_hex(path)
    info = read_committed_manifest(memory)
    try:
        payload = bytes(memory[info["app_base"] + index]
                        for index in range(info["payload_length"]))
    except KeyError as exc:
        raise ValueError("factory image payload is incomplete") from exc
    if package.crc32(payload) != info["payload_crc32"]:
        raise ValueError("factory image payload CRC32 mismatch")
    if package.BOOT_BASE not in memory:
        raise ValueError("factory image is missing the resident reset vector")
    return info


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("build")
    create.add_argument("bootloader_hex")
    create.add_argument("package")
    create.add_argument("-o", "--output", required=True)
    inspect = sub.add_parser("verify")
    inspect.add_argument("factory_hex")
    args = parser.parse_args()
    try:
        if args.command == "build":
            info = merge(args.bootloader_hex, args.package, args.output)
            print(f"factory={args.output}")
        else:
            info = verify_factory(args.factory_hex)
            print(f"factory={args.factory_hex}")
        package.print_info(info, args.output if args.command == "build"
                           else args.factory_hex)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
