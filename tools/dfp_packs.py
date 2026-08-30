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

A disassembly tool should read a pack version the image could plausibly have
been built with, not whatever happens to be newest on the machine running the
tool. So the default is the version THIS PROJECT PINS, read out of the project
at call time rather than copied into a table here:

    dspic33ak_audio_dsp.X/nbproject/configurations.xml
      <conf>/toolsSet/targetDevice  +  <conf>/packs/pack[@name,@version]

Until 2026-08-29 that was a PINS constant in this file, which made a third copy
of a version already written down twice (the project above, and
src/boot/boot_image.psd1's Devices[*].DfpPackVersion for the resident boot
image). Those two are compared on every build by
buildtools/check_configurations.ps1 and a divergence is a hard error -- but the
copy here was in neither half of that comparison, so it could drift for as long
as nobody happened to disassemble something. Reading the project removes the
copy rather than adding a fourth thing to remember to update.

Why the application project and not boot_image.psd1: the gate makes the two
agree, so either is the same answer, and this file is Python -- the .psd1 is
PowerShell data and would need a parser of its own here.

An explicit override still wins outright (--dfp / --mdfp / DSPIC33AK_DFP), which
is how you point a tool at some other pack to read an older image.
"""

from __future__ import annotations

import glob
import os
import re
from xml.etree import ElementTree

# The application project, relative to this file: tools/ sits at the repo root,
# so one level up. Not a search and not a CWD-relative path -- these tools are
# run from wherever the caller happens to be.
PROJECT_XML = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "dspic33ak_audio_dsp.X", "nbproject", "configurations.xml")

_PINS_CACHE = None


def pins(project_xml=None):
    """-> {device: (pack, version)} as the application project pins them.

    Parsed once and cached: several call sites ask, and the answer cannot change
    inside one run of a tool.

    Two configurations naming the same device with DIFFERENT packs is an error
    rather than a first-one-wins pick. It means the project itself disagrees
    about the part, and guessing here would hand objdump one of two answers with
    nothing said about it. (buildtools/check_configurations.ps1 also catches
    this, but a tool may be run against a tree that was never built.)
    """
    global _PINS_CACHE
    if project_xml is None and _PINS_CACHE is not None:
        return _PINS_CACHE

    path = project_xml or PROJECT_XML
    if not os.path.isfile(path):
        raise DfpResolutionError(
            "the application project is not where this tool expects it "
            "(%s), so the pinned device packs cannot be read" % path)
    try:
        root = ElementTree.parse(path).getroot()
    except ElementTree.ParseError as exc:
        raise DfpResolutionError("%s is not readable as XML: %s" % (path, exc))

    found = {}
    for conf in root.iter("conf"):
        device = conf.findtext("./toolsSet/targetDevice")
        packs = conf.findall("./packs/pack")
        if not device or len(packs) != 1:
            # A configuration with no device, or with no <packs> block, states
            # nothing about a pack -- it is not a disagreement, just silent.
            continue
        entry = (packs[0].get("name"), packs[0].get("version"))
        if None in entry:
            continue
        previous = found.get(device)
        if previous is not None and previous != entry:
            raise DfpResolutionError(
                "%s pins %s to both %s %s and %s %s -- the project disagrees "
                "with itself about which pack serves this part"
                % ((os.path.basename(path), device) + previous + entry))
        found[device] = entry

    if not found:
        raise DfpResolutionError(
            "%s names no device pack in any configuration" % path)

    if project_xml is None:
        _PINS_CACHE = found
    return found

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


# A link map has no -mcpu= record (it is not something the compiler wrote), but it
# does name the part (as "dsPIC33AK128MC106" -- so no leading word boundary: the
# character before "33" is a word character too). Kept as its own pattern rather
# than reusing the one above,
# because a bare part number can appear in ordinary text and this one is only
# trusted where a map is what was handed over.
DEVICE_IN_MAP = re.compile(r"33AK\d+M[A-Z]+\d+\b")


def device_of_map(path):
    """-> the part a LINK MAP was produced for.

    Same single-distinct-match discipline as device_of: two different parts named
    in one map means the pattern caught something that is not the target, and the
    answer is not trustworthy.

    Separate from device_of because the resident boot image is built without the
    debug info that carries -mcpu=, so its .o and .elf files cannot answer at all
    while its map can.
    """
    with io_open_text(path) as handle:
        blob = handle.read()
    names = set(DEVICE_IN_MAP.findall(blob))
    if len(names) != 1:
        raise DfpResolutionError(
            "could not read the device out of the link map %s (found %s)"
            % (path, ", ".join(sorted(names)) if names else "nothing"))
    return "dsPIC" + names.pop()


def io_open_text(path):
    """A map is ASCII in practice; do not let one odd byte end the tool."""
    return open(path, "r", encoding="utf-8", errors="replace")


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

    Fallback ONLY for a device the application project does not pin, so a part
    that only exists in some other project's tree still works. Warns to stderr
    rather than failing silently, because this is exactly the unprincipled
    "newest wins" resolution that reading the project's pin exists to avoid.
    """
    import sys
    header = _device_header(device)
    found = glob.glob(os.path.join(packs_root(), "dsPIC33AK-*_DFP", "*", "xc16", header))
    if not found:
        raise DfpResolutionError(
            "no installed device pack supports %s, and it has no pin in "
            "the application project -- pass an explicit override" % device)
    best = sorted((path[: -(len(header) + 1)] for path in found), key=_version_key)[-1]
    sys.stderr.write(
        "dfp_packs: %s is not pinned by the application project; using the newest "
        "installed pack (%s)\n"
        % (device, best))
    return best


def resolve_dfp(device, override=None):
    """-> the `.../xc16` directory to pass as `-mdfp=`.

    `override` (an explicit --mdfp/--dfp/-Dfp flag or env value) wins outright.
    Otherwise the pack the application project pins for `device`, else the
    newest installed pack that supports it (see _newest_installed).
    """
    if override:
        return override.replace("\\", "/")

    pinned = pins().get(device)
    if pinned is None:
        return _newest_installed(device).replace("\\", "/")

    pack, version = pinned
    xc16 = os.path.join(packs_root(), pack, version, "xc16")
    header = os.path.join(xc16, _device_header(device))
    if not os.path.isfile(header):
        raise DfpResolutionError(
            "%s %s is pinned for %s by the application project, but %s was not "
            "found (installed pack missing, or DSPIC33AK_DFP points "
            "elsewhere)" % (pack, version, device, header))
    return xc16.replace("\\", "/")


def _main(argv):
    """CLI so PowerShell tools share the resolution without importing Python.

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
