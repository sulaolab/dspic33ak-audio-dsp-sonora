"""Single source of truth for "which DFP pack disassembles this image."

Every tool in tools/ that shells out to xc-dsc-objdump against a built ASRC/app
artifact needs `-mdfp=<pack>/xc16`, and this repo ships images for two devices
in two different packs (dsPIC33AK-MP_DFP for 33AK512MPS512, dsPIC33AK-MC_DFP
for 33AK128MC106). Handing objdump the wrong pack does not degrade -- it
refuses the disassembly outright ("can't disassemble for architecture
UNKNOWN!") -- so before 2026-08-28 a tool hardcoded to one device's pack read a
safe image of the OTHER device as a broken tool, or (worse, for a tool that
reads SFR names rather than just failing loudly) could silently report a
register that changed between pack versions.

PINS mirrors facts recorded independently in two other places, and is pinned
to the SAME versions on purpose, not by coincidence:
  - dspic33ak_audio_dsp.X/nbproject/configurations.xml (every configuration)
  - src/boot/boot_image.psd1's Devices[*].DfpPackVersion (main, commit
    5f2853d): pinned because newer packs broke real builds --
    MC 1.5.263+ drops PLL1CON.OE, MP 1.5.269+ rejects NOBTSWP=OFF.
A disassembly tool should read a pack version the image could plausibly have
been built with, not whatever happens to be newest on the machine running the
tool -- so this table, not "newest installed", is the default. Update all
three together if a pin ever moves.
"""

from __future__ import annotations

import glob
import os
import re

PINS = {
    "dsPIC33AK512MPS512": ("dsPIC33AK-MP_DFP", "1.3.185"),
    "dsPIC33AK128MC106": ("dsPIC33AK-MC_DFP", "1.4.172"),
}

# The compiler embeds its own invocation (including -mcpu=) in debug info, so
# it survives in the raw bytes of any .o or .elf XC-DSC produced -- no need to
# open the file as an object file to read it back out.
DEVICE_IN_IMAGE = re.compile(rb"-mcpu=(33AK\d+M[A-Z]+\d+)\b")


class DfpResolutionError(RuntimeError):
    """Raised instead of exiting: callers have their own exit-code contracts."""


def device_of(path):
    """-> the part `path` (an .elf or .o XC-DSC produced) was built for.

    Read out of the image rather than passed in: the caller that knows the
    answer (the build) is not the only caller, and a tool that has to be told
    which part it is looking at can be told wrong. Requiring a single distinct
    match makes this a check rather than a guess -- two matches means the
    pattern caught something else, and the answer is not trustworthy (found
    2026-08-26: a bare match beside 70 real -mcpu= matches).
    """
    with open(path, "rb") as handle:
        blob = handle.read()
    names = {m.group(1).decode() for m in DEVICE_IN_IMAGE.finditer(blob)}
    if len(names) != 1:
        raise DfpResolutionError(
            "could not read the device out of %s (found %s)"
            % (path, ", ".join(sorted(names)) if names else "nothing"))
    return "dsPIC" + names.pop()


def packs_root():
    """-> the directory holding every installed vendor/pack/version tree.

    $DSPIC33AK_DFP if set (the repo-wide override convention: an explicit pack
    ROOT, not the pack's own xc16 subdirectory), else the per-user MPLAB X
    pack cache. Never a hard-coded home path -- these tools are published.
    """
    root = os.environ.get("DSPIC33AK_DFP")
    if root:
        return root
    return os.path.join(os.path.expanduser("~"), ".mchp_packs", "Microchip")


def _version_key(path):
    """Sort pack/version directories by version number, not as strings.

    `sorted()` on the raw paths puts 1.10.x before 1.2.x, which silently picks
    a stale pack the day a minor number reaches double digits.
    """
    name = os.path.basename(path.rstrip("/\\"))
    return [int(part) if part.isdigit() else part
            for part in re.split(r"(\d+)", name)]


def _device_header(device):
    return "support/dsPIC33A/h/p%s.h" % device[len("dsPIC"):]


def _newest_installed(device):
    """-> newest installed pack (any family) whose header supports `device`.

    Fallback ONLY for a device not yet in PINS, so adding a third part works
    without a code change here. Warns to stderr rather than failing silently,
    because this is exactly the unprincipled "newest wins" resolution PINS
    exists to avoid for the two known devices.
    """
    import sys
    header = _device_header(device)
    found = glob.glob(os.path.join(packs_root(), "dsPIC33AK-*_DFP", "*", "xc16", header))
    if not found:
        raise DfpResolutionError(
            "no installed device pack supports %s, and it has no pin in "
            "tools/dfp_packs.py.PINS -- pass an explicit override" % device)
    best = sorted((path[: -(len(header) + 1)] for path in found), key=_version_key)[-1]
    sys.stderr.write(
        "dfp_packs: %s has no pin in PINS; using newest installed pack (%s)\n"
        % (device, best))
    return best


def resolve_dfp(device, override=None):
    """-> the `.../xc16` directory to pass as `-mdfp=`.

    `override` (an explicit --mdfp/--dfp/-Dfp flag or env value) wins outright.
    Otherwise PINS[device] if listed there, else the newest installed pack
    that supports the device (see _newest_installed).
    """
    if override:
        return override.replace("\\", "/")

    pinned = PINS.get(device)
    if pinned is None:
        return _newest_installed(device).replace("\\", "/")

    pack, version = pinned
    xc16 = os.path.join(packs_root(), pack, version, "xc16")
    header = os.path.join(xc16, _device_header(device))
    if not os.path.isfile(header):
        raise DfpResolutionError(
            "%s %s is pinned for %s in tools/dfp_packs.py, but %s was not "
            "found (installed pack missing, or DSPIC33AK_DFP points "
            "elsewhere)" % (pack, version, device, header))
    return xc16.replace("\\", "/")


def _main(argv):
    """CLI so PowerShell tools can share PINS without a Python import.

    Prints the resolved `.../xc16` directory for the ELF/O file's device and
    exits 0, or prints the error to stderr and exits 1 -- so a caller can do
    `$dfp = python tools/dfp_packs.py $elf; if ($LASTEXITCODE) { throw $dfp }`.
    """
    import sys as _sys
    if len(argv) != 2:
        _sys.stderr.write("usage: dfp_packs.py <elf-or-o-file>\n")
        return 1
    try:
        print(resolve_dfp(device_of(argv[1])))
    except DfpResolutionError as exc:
        _sys.stderr.write(str(exc) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(_main(sys.argv))
