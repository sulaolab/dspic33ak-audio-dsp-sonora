#!/usr/bin/env python3
"""Stdlib-only tests for the resident serial-boot package format.

Every case runs once per Flash layout (see serial_boot_package.LAYOUTS), because
the layouts differ only in an address and that is exactly the kind of difference a
single-device test cannot see. The device is never passed to the tool under test:
it comes from the linker's -p flag in the .map, so the fake maps below carry one.
"""
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ihex_lite
import serial_boot_package as P
import serial_boot_factory_image as F


def write_hex(path, mem):
    with open(path, "w", newline="\r\n") as target:
        target.write("\n".join(ihex_lite.emit_records(mem) + [ihex_lite.EOF_RECORD, ""]))


def write_map(path, layout, entry=None):
    """A minimal stand-in for the linker map: the -p flag plus the two symbols."""
    entry = layout.app_base + 0x100 if entry is None else entry
    with open(path, "w") as target:
        target.write("Invocation:\n")
        target.write("  -p%s \\\n" % layout.device)
        target.write("  0x%08x                  __resetPRI\n" % entry)
        target.write("  0x%08x                  __ivt_0\n" % layout.app_base)


def expect_value_error(label, action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError(f"{label} was accepted")


def run_layout(tmp, layout):
    tag = layout.name
    hex_path = os.path.join(tmp, f"app_{tag}.hex")
    map_path = os.path.join(tmp, f"app_{tag}.map")
    package = os.path.join(tmp, f"app_{tag}.sfb")
    write_map(map_path, layout)

    mem = {layout.app_base + i: (i * 7) & 0xFF for i in range(301)}
    mem[0x800000] = 0x00  # reset/config records are deliberately not packaged
    mem[0x7F40D0] = 0xFF
    write_hex(hex_path, mem)

    info, payload = P.build(hex_path, map_path, package, 42)
    checked = P.verify(package)
    assert checked == info
    assert info["layout"] is layout, f"{tag}: built for the wrong layout"
    assert info["firmware_version"] == 42
    assert info["payload_length"] == 304
    assert payload[301:] == b"\xFF" * 3

    with open(package, "rb") as source:
        corrupt = bytearray(source.read())
    corrupt[-1] ^= 1
    bad = os.path.join(tmp, f"bad_{tag}.sfb")
    with open(bad, "wb") as target:
        target.write(corrupt)
    expect_value_error(f"{tag}: corrupt payload", lambda: P.verify(bad))

    def expect_bad_header(mutator, label):
        with open(package, "rb") as source:
            data = bytearray(source.read())
        fields = list(P.HEADER.unpack(data[:P.HEADER_SIZE]))
        mutator(fields)
        fields[-1] = 0
        rebuilt = bytearray(P.HEADER.pack(*fields))
        struct.pack_into("<I", rebuilt, P.HEADER_SIZE - 4, P.crc32(rebuilt[:-4]))
        data[:P.HEADER_SIZE] = rebuilt
        candidate = os.path.join(tmp, f"{label}_{tag}.sfb")
        with open(candidate, "wb") as target:
            target.write(data)
        expect_value_error(f"{tag}: {label} manifest",
                           lambda: P.verify(candidate))

    # Header CRC is checked independently of semantic field validation.
    with open(package, "rb") as source:
        bad_header_crc = bytearray(source.read())
    bad_header_crc[P.HEADER_SIZE - 1] ^= 1
    bad_header_crc_path = os.path.join(tmp, f"bad_header_crc_{tag}.sfb")
    with open(bad_header_crc_path, "wb") as target:
        target.write(bad_header_crc)
    expect_value_error(f"{tag}: bad header CRC",
                       lambda: P.verify(bad_header_crc_path))

    expect_bad_header(lambda fields: fields.__setitem__(3, 0xDEADBEEF),
                      "wrong_layout")
    expect_bad_header(lambda fields: fields.__setitem__(
        5, layout.manifest_address - layout.app_base + 16), "oversize")
    expect_bad_header(lambda fields: fields.__setitem__(5, 17), "unaligned")

    # An image linked for a bigger part must not be packaged as this one.
    too_big = dict(mem)
    too_big[layout.flash_end] = 0x11
    too_big_hex = os.path.join(tmp, f"too_big_{tag}.hex")
    write_hex(too_big_hex, too_big)
    expect_value_error(
        f"{tag}: HEX past the end of the panel",
        lambda: P.build(too_big_hex, map_path,
                        os.path.join(tmp, f"too_big_{tag}.sfb"), 42))

    mem[P.BOOT_BASE + 4] = 0x55
    write_hex(hex_path, mem)
    expect_value_error(f"{tag}: bootloader-space write",
                       lambda: P.build(hex_path, map_path, package, 43))

    # Factory merge commits the manifest at this layout's final page. Removing it
    # models any power loss before the last program operation.
    boot_hex = os.path.join(tmp, f"boot_{tag}.hex")
    factory_hex = os.path.join(tmp, f"factory_{tag}.hex")
    boot_mem = {P.BOOT_BASE + i: (0xA0 + i) & 0xFF for i in range(64)}
    write_hex(boot_hex, boot_mem)
    write_hex(hex_path, {layout.app_base + i: (i * 7) & 0xFF for i in range(301)})
    P.build(hex_path, map_path, package, 44)
    F.merge(boot_hex, package, factory_hex)
    verified = F.verify_factory(factory_hex)
    assert verified["firmware_version"] == 44
    assert verified["layout"] is layout, f"{tag}: factory image read as another layout"

    interrupted = ihex_lite.parse_hex(factory_hex)
    for address in range(layout.manifest_address,
                        layout.manifest_address + P.HEADER_SIZE):
        interrupted.pop(address, None)
    interrupted_hex = os.path.join(tmp, f"power_loss_before_commit_{tag}.hex")
    write_hex(interrupted_hex, interrupted)
    expect_value_error(f"{tag}: image without committed manifest",
                       lambda: F.verify_factory(interrupted_hex))

    return factory_hex


def main():
    with tempfile.TemporaryDirectory(prefix="serial_boot_") as tmp:
        assert len({l.layout_id for l in P.LAYOUTS}) == len(P.LAYOUTS), \
            "two layouts share a layout_id, so packages are indistinguishable"
        assert len({l.manifest_address for l in P.LAYOUTS}) == len(P.LAYOUTS), \
            "two layouts commit at the same address"

        # A retired layout_id must stay retired. Reissuing one for a different
        # arrangement would let a package built for either pass the fence for the other,
        # and the payload would then be programmed at the wrong base with a valid CRC.
        assert not (set(P.RETIRED_LAYOUTS) & {l.layout_id for l in P.LAYOUTS}), \
            "a retired layout_id was reissued, so an old package would be accepted"

        # An old .sfb is the package most likely to be offered by mistake -- it was real
        # and someone still has the file -- so it is refused BY NAME rather than as an
        # unrecognized number, and the refusal has to say how such a board is actually
        # migrated, because serial update cannot move a partition boundary.
        for retired_id, description in P.RETIRED_LAYOUTS.items():
            try:
                P.layout_for_id(retired_id)
            except ValueError as error:
                message = str(error)
                assert description in message, \
                    f"retired layout 0x{retired_id:08X} is not named in its rejection"
                assert "PKOB4" in message, \
                    f"retired layout 0x{retired_id:08X} does not say how to migrate"
            else:
                raise AssertionError(
                    f"retired layout 0x{retired_id:08X} was accepted as a live layout")

        for layout in P.LAYOUTS:
            run_layout(tmp, layout)

        # A map that does not name a device is refused rather than defaulted: the
        # whole point of reading -p is that nobody has to state the device twice.
        nameless = os.path.join(tmp, "nameless.map")
        with open(nameless, "w") as target:
            target.write("  0x808100                  __resetPRI\n")
            target.write("  0x808000                  __ivt_0\n")
        expect_value_error("map without -p<device>",
                           lambda: P.build(os.path.join(tmp, "app_ak512.hex"),
                                           nameless,
                                           os.path.join(tmp, "nameless.sfb"), 42))

        # The short and long spellings of a device must resolve to one layout.
        for layout in P.LAYOUTS:
            for spelling in (layout.name, layout.device, "dsPIC" + layout.device):
                assert P.layout_for_device(spelling) is layout
        expect_value_error("unknown device",
                           lambda: P.layout_for_device("33AK9999XX999"))

    print("ALL PASS: resident serial-boot package/factory/fault tests "
          f"({len(P.LAYOUTS)} layouts, {len(P.RETIRED_LAYOUTS)} retired)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
