<#
.SYNOPSIS
Post-link assertions for the resident boot image.

.DESCRIPTION
Every guarantee that makes a 32 KiB image with hand-placed sections safe to flash,
in one place: the launch mailbox address, the four reset-diagnostic allocations,
the stack end, the primary IVT (vector by vector), the physical reset vector, the
diagnostic sections' ELF flags, and the 32 KiB cap.

This file was extracted verbatim from build_resident_bootloader.ps1 (2026-08-14)
so that the same assertions can run on an image MPLAB X linked. MPLAB gives back
the compile and the link; it does not give back any of this unless a post-build
step calls it. Keep the checks here and call them from both routes -- an assertion
that exists on only one build route is worse than no assertion, because it makes
the other route look verified.

.PARAMETER Elf
The linked ELF. Map and HEX default to the same path with the extension changed,
which is how both the standalone script and MPLAB name them.

.PARAMETER Objdump
xc-dsc-objdump.exe. From an MPLAB post-build step pass ${MP_CC_DIR}/xc-dsc-objdump.exe;
falls back to $env:XC_DSC_CC's directory.

.PARAMETER Dfp
The DFP's xc16 directory. From an MPLAB post-build step pass "${DFP_DIR}/xc16";
falls back to $env:DSPIC33AK_DFP.

.EXAMPLE
# MPLAB X post-build step for resident_bootloader.X
pwsh -NoProfile -File ../buildtools/verify_resident_image.ps1 `
    -Elf "${ImagePath}" -Objdump ${MP_CC_DIR}/xc-dsc-objdump.exe -Dfp "${DFP_DIR}/xc16" `
    -MaxBytes 0x8000

.NOTES
Deliberately not folded in yet: build.ps1:1300-1338 repeats four of these checks
for the application image. Collapsing both onto this script is a real cleanup, but
it touches the application build and this extraction must not.
#>
param(
    [Parameter(Mandatory = $true)][string]$Elf,
    [string]$Map,
    [string]$Hex,
    [string]$Objdump,
    [string]$Dfp,
    <#
      'debug' or 'production' -- MPLAB X's ${IMAGE_TYPE}, passed by the generated
      project's post-build step. It decides ONE thing: whether a missing HEX is a
      failure. A debug build links an ELF and a map and no HEX at all, because the
      debugger loads the ELF; requiring one broke "Debug Main Project" outright,
      which is the only reason resident_bootloader.X exists. Every other assertion
      below runs on both image types and is not weakened by this.
    #>
    [ValidateSet('', 'debug', 'production')]
    [string]$ImageType = '',
    # No default: the region is device-specific (0x8000 on the MPS512, 0x4000 on
    # the MC106 since its 2026-08-20 16 KiB migration), and a default silently
    # matching only one of them is exactly what let a wrong cap through unnoticed
    # before. Every real caller passes the manifest's SizeCapBytes explicitly.
    [Parameter(Mandatory = $true)]
    [uint32]$MaxBytes
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Elf)) {
    throw "Resident ELF was not found: $Elf"
}
$elf = (Resolve-Path -LiteralPath $Elf).Path
if ([string]::IsNullOrWhiteSpace($Map)) {
    $Map = [IO.Path]::ChangeExtension($elf, '.map')
}
if ([string]::IsNullOrWhiteSpace($Hex)) {
    $Hex = [IO.Path]::ChangeExtension($elf, '.hex')
}
if (-not (Test-Path -LiteralPath $Map)) {
    throw "Resident map was not found: $Map"
}
$map = (Resolve-Path -LiteralPath $Map).Path
$hex = $Hex

# Both real callers pass these: the standalone script has them resolved already, and
# an MPLAB post-build step gets them from ${MP_CC_DIR} / ${DFP_DIR}. The env-var
# fallback is for running this by hand on an existing ELF. There is deliberately no
# newest-version scan here -- one copy of that logic, in the build script.
if ([string]::IsNullOrWhiteSpace($Objdump)) {
    if (-not [string]::IsNullOrWhiteSpace($env:XC_DSC_CC)) {
        $Objdump = Join-Path (Split-Path -Parent $env:XC_DSC_CC) 'xc-dsc-objdump.exe'
    }
}
if ([string]::IsNullOrWhiteSpace($Objdump) -or -not (Test-Path -LiteralPath $Objdump)) {
    throw ('xc-dsc-objdump.exe was not found. Pass -Objdump, or set XC_DSC_CC to ' +
           'the compiler beside it.')
}
$objdump = (Resolve-Path -LiteralPath $Objdump).Path
if ([string]::IsNullOrWhiteSpace($Dfp)) {
    if (-not [string]::IsNullOrWhiteSpace($env:DSPIC33AK_DFP)) {
        $Dfp = Join-Path $env:DSPIC33AK_DFP 'xc16'
    }
}
if ([string]::IsNullOrWhiteSpace($Dfp) -or -not (Test-Path -LiteralPath $Dfp)) {
    throw 'The DFP xc16 directory was not found. Pass -Dfp, or set DSPIC33AK_DFP.'
}
$dfp = (Resolve-Path -LiteralPath $Dfp).Path

#=======================================================================
# Below this line: moved from build_resident_bootloader.ps1 without
# behaviour change. Edit here, not there.
#=======================================================================

if (-not (Test-Path -LiteralPath $hex)) {
    if ($ImageType -eq 'debug') {
        # Expected: a debug link produces .elf + .map only. Reported rather than
        # silent, so "no HEX" cannot be mistaken for "HEX verified".
        Write-Host "  (debug image: no HEX is produced; HEX presence not checked)"
    } else {
        throw ("Expected HEX output was not created: $hex" + [Environment]::NewLine +
               'If this is an IDE DEBUG build, the post-build step is missing ' +
               '-ImageType ${IMAGE_TYPE}: regenerate the project with ' +
               'pwsh buildtools/generate_resident_project.ps1')
    }
}
$mapText = [IO.File]::ReadAllText($map)

# WHICH PART THIS IMAGE IS FOR comes from the map's own -p flag, not from a parameter.
# The fixed RAM records below sit at the top of data RAM, so their addresses differ
# per device; taking the device from the artefact means an image can never be checked
# against another part's table, and there is no flag an IDE post-build step could get
# wrong. Same mechanism as tools/serial_boot_package.py, which reads -p for the same
# reason.
$deviceMatch = [regex]::Match($mapText, '(?m)^\s*-p(33AK\S+?)\s*\\?\s*$')
if (-not $deviceMatch.Success) {
    throw ("Could not read the target device from the map's -p flag: $map. " +
           'Every address checked below is per device, so there is nothing to check without it.')
}
$mapDevice = $deviceMatch.Groups[1].Value

# The four fixed RAM records, per device, in the 512-byte reservation at the top of
# data RAM. This is deliberately a SECOND, independent statement of the addresses in
# src/shared/resident_de_mailbox.h, src/shared/noinit_ram_config.h and the device
# linker script: a verifier that derived its expectations from the artefact it checks
# would agree with anything. The SIZES are device-independent (the records are the
# same structures), and so is the launch mailbox at 0x4050 -- that one is at the
# BOTTOM of RAM, which is 0x4000 on every part in the family.
$diagnosticAddresses = @{
    '33AK512MPS512' = @{ Sentinel = 0x13E00; Trace = 0x13E10; Precrt = 0x13EF0; Noinit = 0x13F30 }
    '33AK128MC106'  = @{ Sentinel = 0x07E00; Trace = 0x07E10; Precrt = 0x07EF0; Noinit = 0x07F30 }
}
if (-not $diagnosticAddresses.ContainsKey($mapDevice)) {
    throw ("This image was linked for $mapDevice, which this verifier has no diagnostic " +
           'address table for. Add it here -- from the device linker script and ' +
           'src/shared/resident_de_mailbox.h -- rather than skipping the check.')
}
$diag = $diagnosticAddresses[$mapDevice]

$mailbox = [regex]::Match(
    $mapText,
    '(?m)^\.resident_launch_mailbox\s+0x4050\s+0\s+0x10\s+\(16\)\s*$'
)
if (-not $mailbox.Success) {
    throw 'Resident launch mailbox is not allocated at 0x4050..0x405F in the final map.'
}
foreach ($expected in @(
    ('(?m)^\.resident_far_sentinel\s+0x0*{0:x}\s+\S+\s+0x10\b' -f $diag.Sentinel),
    ('(?m)^\.resident_reset_source_trace\s+0x0*{0:x}\s+\S+\s+0xe0\b' -f $diag.Trace),
    ('(?m)^\.resident_precrt_trace\s+0x0*{0:x}\s+\S+\s+0x40\b' -f $diag.Precrt),
    # .resident_diag_guard is deliberately absent since 2026-08-11 -- the block below
    # was extended down over it. The 0xd0 size is what proves the range stays occupied.
    ('(?m)^\.noinit_ram\s+0x0*{0:x}\s+\S+\s+0xd0\b' -f $diag.Noinit)
)) {
    if (-not [regex]::IsMatch($mapText, $expected)) {
        throw "Resident reset diagnostic allocation is missing ($mapDevice): $expected"
    }
}
$stack = [regex]::Match(
    $mapText,
    '(?m)^stack\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+'
)
if (-not $stack.Success) {
    throw 'Could not read resident stack range from the final map.'
}
$stackStart = [Convert]::ToUInt32($stack.Groups[1].Value.Substring(2), 16)
$stackLength = [Convert]::ToUInt32($stack.Groups[2].Value.Substring(2), 16)
if (($stackStart + $stackLength) -ne $diag.Sentinel) {
    throw ('Resident stack does not end at this device''s diagnostic boundary ' +
           ('0x{0:X} ({1}): ' -f $diag.Sentinel, $mapDevice) +
           ('start=0x{0:X} length=0x{1:X}' -f $stackStart, $stackLength))
}
$sectionHeaders = (& $objdump "-mdfp=$dfp" -h $elf 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw 'Could not inspect resident ELF section flags.'
}
$ivt = [regex]::Match(
    $sectionHeaders,
    '(?m)^\s*\d+\s+__ivt_0\s+([0-9a-fA-F]+)\s+00800004\b'
)
if ((-not $ivt.Success) -or
    ([Convert]::ToUInt32($ivt.Groups[1].Value, 16) -eq 0)) {
    throw 'Resident primary IVT is not allocated at 0x800004.'
}
$ivtAddress = [uint32]0x800004
$ivtLength = [Convert]::ToUInt32($ivt.Groups[1].Value, 16)
$symbols = (& $objdump "-mdfp=$dfp" -t $elf 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw 'Could not inspect resident ELF symbols.'
}
$precrtSymbol = [regex]::Match(
    $symbols,
    '(?m)^([0-9a-fA-F]{8})\s+.*\s_resident_precrt_entry\s*$'
)
if (-not $precrtSymbol.Success) {
    throw 'Could not locate the resident pre-CRT entry symbol.'
}
$defaultInterruptSymbol = [regex]::Match(
    $symbols,
    '(?m)^([0-9a-fA-F]{8})\s+.*\s__DefaultInterrupt\s*$'
)
$timer1InterruptSymbol = [regex]::Match(
    $symbols,
    '(?m)^([0-9a-fA-F]{8})\s+.*\s__T1Interrupt\s*$'
)
if ((-not $defaultInterruptSymbol.Success) -or
    (-not $timer1InterruptSymbol.Success)) {
    throw 'Could not locate the resident default/Timer1 interrupt symbols.'
}
$defaultInterruptAddress =
    [Convert]::ToUInt32($defaultInterruptSymbol.Groups[1].Value, 16)
$timer1InterruptAddress =
    [Convert]::ToUInt32($timer1InterruptSymbol.Groups[1].Value, 16)
$expectedDefaultWord = -join ([BitConverter]::GetBytes($defaultInterruptAddress) |
    ForEach-Object { $_.ToString('x2') })
$expectedTimer1Word = -join ([BitConverter]::GetBytes($timer1InterruptAddress) |
    ForEach-Object { $_.ToString('x2') })
$ivtDump = (& $objdump "-mdfp=$dfp" -s -j __ivt_0 $elf 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw 'Could not inspect the resident primary IVT.'
}
$ivtWords = @()
foreach ($line in ($ivtDump -split '\r?\n')) {
    $data = [regex]::Match(
        $line,
        '^\s*[0-9a-fA-F]{6,8}\s+((?:[0-9a-fA-F]{8}\s+){1,4})'
    )
    if ($data.Success) {
        foreach ($word in [regex]::Matches($data.Groups[1].Value,
                                            '[0-9a-fA-F]{8}')) {
            $ivtWords += $word.Value.ToLowerInvariant()
        }
    }
}
$expectedVectorCount = [int]($ivtLength / 4)
$timer1VectorIndex = [int](([uint32]0x8000E4 - $ivtAddress) / 4)
if ($ivtWords.Count -ne $expectedVectorCount) {
    throw ('Resident primary IVT dump has the wrong number of vectors: ' +
           'expected={0} actual={1}' -f $expectedVectorCount, $ivtWords.Count)
}
for ($index = 0; $index -lt $ivtWords.Count; $index++) {
    $expectedWord = if ($index -eq $timer1VectorIndex) {
        $expectedTimer1Word
    } else {
        $expectedDefaultWord
    }
    if ($ivtWords[$index] -ine $expectedWord) {
        $vectorAddress = $ivtAddress + [uint32](4 * $index)
        throw ('Resident vector 0x{0:X6} does not point to the expected ' +
               'handler.' -f $vectorAddress)
    }
}
$precrtAddress = [Convert]::ToUInt32($precrtSymbol.Groups[1].Value, 16)
if ($precrtAddress -lt ($ivtAddress + $ivtLength)) {
    throw ('Resident pre-CRT entry overlaps the primary IVT: ' +
           'entry=0x{0:X6} ivt_end=0x{1:X6}' -f
           $precrtAddress, ($ivtAddress + $ivtLength))
}
$resetDump = (& $objdump "-mdfp=$dfp" -s -j .reset $elf 2>&1 | Out-String)
$resetWord = [regex]::Match(
    $resetDump,
    '(?m)^\s*800000\s+([0-9a-fA-F]{8})\b'
)
$expectedResetWord = -join ([BitConverter]::GetBytes($precrtAddress) |
    ForEach-Object { $_.ToString('x2') })
if (($LASTEXITCODE -ne 0) -or (-not $resetWord.Success) -or
    ($resetWord.Groups[1].Value -ine $expectedResetWord)) {
    throw ('Resident physical reset vector does not point to pre-CRT entry ' +
           ('0x{0:X6}.' -f $precrtAddress))
}
foreach ($section in @('noinit_ram',
                        'resident_far_sentinel',
                        'resident_reset_source_trace',
                        'resident_precrt_trace')) {
    $flags = [regex]::Match(
        $sectionHeaders,
        "(?ms)^\s*\d+\s+\.$section\s+.*?\r?\n\s*([^\r\n]+)"
    )
    if ((-not $flags.Success) -or
        ($flags.Groups[1].Value -notmatch '\bNEVER_LOAD\b') -or
        ($flags.Groups[1].Value -notmatch '\bPERSIST\b') -or
        ($flags.Groups[1].Value -notmatch '\bYMEMORY\b') -or
        ($flags.Groups[1].Value -match '\bCODE\b')) {
        throw "Resident diagnostic section has unsafe ELF flags: .$section"
    }
}
$usage = [regex]::Match($mapText, 'Total "program" memory used \(bytes\):\s+(0x[0-9a-fA-F]+)')
if (-not $usage.Success) {
    throw 'Could not read resident bootloader program usage from the map.'
}
$used = [Convert]::ToUInt32($usage.Groups[1].Value.Substring(2), 16)
if ($used -gt $MaxBytes) {
    throw ('Resident bootloader exceeds its 0x{1:X}-byte window: 0x{0:X} bytes' -f
           $used, $MaxBytes)
}
Write-Host ('Resident bootloader: 0x{0:X} / 0x{1:X} bytes ({2:N1}%)' -f `
    $used, $MaxBytes, (100.0 * $used / $MaxBytes))
