# provision.ps1 - dual-partition UCA provisioning for the ASRC (Dual Boot) build.
#
# WHY: each physical partition owns its own UCA (config fuses) at FIXED addresses
# that P2ACTIV does NOT remap. A plain PKOB4 bulk-erase leaves the P2 UCA blank,
# so FDEVOPT.ALTI2C2 reads OFF and WM8904-A lands on the wrong I2C-2 pins -> codec-A
# init fails only on P2. The fix is to clone the P1 UCA words into the P2 UCA in the
# HEX before flashing, producing a "bundle" hex that provisions BOTH partitions.
#
# This wrapper runs, for a dual-partition configuration:
#   1) tools/gen_dual_partition_hex.py   -> <config>.production.bundle.hex (+ gen report)
#   2) tools/verify_dual_partition_hex.py (read-only) on the bundle
# It FAILS LOUDLY (non-zero exit) if verify != PASS. Only a verified bundle is a
# publishable provisioning artifact (design doc section 5.4). Build-success and
# bundle-verify-success are deliberately separate: a green build does NOT imply a
# provisioned bundle.
#
# RETIRED -- see the throw below; nothing in this file runs. The configuration
# names it mentions (dsPIC33AK512_ASRC and friends) are the ones that existed when
# it was written; the standalone AK512 pair was deleted on 2026-08-15, so they name
# history, not anything buildable. They are left as written because rewriting a
# retired script's examples to name configurations it was never used with would
# make the historical record wrong instead of merely stale.
#
# Usage when it was live:
#   pwsh -File buildtools/provision.ps1                         # default dsPIC33AK512_ASRC
#   pwsh -File buildtools/provision.ps1 -Configuration dsPIC33AK512_ASRC
#   pwsh -File buildtools/provision.ps1 -Hex path\to\some.production.hex
# Then flash the bundle:
#   pwsh -File buildtools/flashauto.ps1 -Bundle -Configuration dsPIC33AK512_ASRC

param(
    [string]$Configuration = 'dsPIC33AK512_ASRC',
    [string]$Root = (Get-Location).Path,
    [string]$ProjectDir,
    [string]$Hex,
    [string]$Python,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# RETIRED. Kept for the UCA/BTSEQ reference below, not as a usable path: the A/B
# Flash-Dual-Partition mechanism this provisions has been removed from this tree
# (src/app/fw_update/ and src/app/uart_app/fw_console.c are gone) and src/app/main.c sets
# BTMODE=SINGLE. Producing a P1+P2 UCA bundle now would provision partition fuses
# for a boot mode the firmware cannot enter, so it stops here rather than handing
# back a plausible-looking bundle.
throw @'
provision.ps1 is retired: it provisions the A/B Flash-Dual-Partition layout
(P1+P2 UCA cloning), which has been removed from this tree. src/app/main.c now sets
BTMODE=SINGLE, so this bundle would program partition fuses for a boot mode the
firmware no longer uses.

Serial firmware update is now the resident bootloader:
  ./buildtools/build.ps1 -Full -Configuration dsPIC33AK512_ASRC_SERIAL_UPDATE
  ./buildtools/flashauto.ps1 -Serial <PKOB4_SERIAL>

(or dsPIC33AK512_CLASSIC_SERIAL_UPDATE for the Classic application.)

The completed A/B implementation is maintained outside this repository.
'@

function Write-Status {
    param([string]$Message)
    if (-not $Quiet) { Write-Host $Message }
}

# Only these configurations are Dual Boot and thus need a P2 UCA. Guard rather than
# silently produce a meaningless single-partition "bundle".
$DualPartitionConfigs = @('dsPIC33AK512_ASRC')

function Resolve-BuildRoot {
    param([string]$RequestedRoot)
    $resolvedRoot = (Resolve-Path -LiteralPath $RequestedRoot).Path
    if ((Split-Path -Leaf $resolvedRoot) -like '*.X' -and
        (Test-Path -LiteralPath (Join-Path $resolvedRoot 'nbproject'))) {
        return (Split-Path -Parent $resolvedRoot)
    }
    return $resolvedRoot
}

function Resolve-MplabProjectDir {
    param([string]$Root, [string]$RequestedProjectDir)
    if (-not [string]::IsNullOrWhiteSpace($RequestedProjectDir)) {
        if ([System.IO.Path]::IsPathRooted($RequestedProjectDir)) {
            return (Resolve-Path -LiteralPath $RequestedProjectDir).Path
        }
        return (Resolve-Path -LiteralPath (Join-Path $Root $RequestedProjectDir)).Path
    }
    $projects = @(Get-ChildItem -LiteralPath $Root -Directory -Filter '*.X' |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'nbproject') })
    if ($projects.Count -eq 0) {
        throw "No MPLAB X project directory (*.X with nbproject) found under: $Root"
    }
    if ($projects.Count -gt 1) {
        $names = ($projects | ForEach-Object { $_.Name }) -join ', '
        throw "Multiple MPLAB X project directories found: $names. Specify -ProjectDir."
    }
    return $projects[0].FullName
}

function Resolve-Python {
    param([string]$Requested)
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { return $Requested }
    foreach ($cand in @('python', 'py')) {
        $cmd = Get-Command $cand -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw "Python not found on PATH. Install Python 3 or pass -Python."
}

function Invoke-CheckedPython {
    param([string]$Python, [string[]]$Arguments)
    Write-Status "==> $Python $($Arguments -join ' ')"
    # Stream python's stdout straight to the host (Out-Host keeps it OFF the function's
    # return pipeline) so we return ONLY the exit code, not the tool's chatter.
    & $Python @Arguments | Out-Host
    return $LASTEXITCODE
}

$repoRoot   = Resolve-BuildRoot -RequestedRoot $Root
$projectDir = Resolve-MplabProjectDir -Root $repoRoot -RequestedProjectDir $ProjectDir
$toolsDir   = Join-Path $repoRoot 'tools'
$python     = Resolve-Python -Requested $Python

if ($DualPartitionConfigs -notcontains $Configuration) {
    throw "Configuration '$Configuration' is not a dual-partition config ($($DualPartitionConfigs -join ', ')). Provisioning a P2 UCA is only meaningful for Dual Boot builds."
}

# P1 production hex (the freshly built single-partition image).
if ([string]::IsNullOrWhiteSpace($Hex)) {
    $projectName = Split-Path -Leaf $projectDir
    $Hex = Join-Path $projectDir "dist\$Configuration\production\$projectName.production.hex"
}
if (-not (Test-Path -LiteralPath $Hex)) {
    throw "P1 production HEX not found: $Hex. Build the $Configuration configuration first."
}
$Hex = (Resolve-Path -LiteralPath $Hex).Path

$genScript    = Join-Path $toolsDir 'gen_dual_partition_hex.py'
$verifyScript = Join-Path $toolsDir 'verify_dual_partition_hex.py'
foreach ($s in @($genScript, $verifyScript)) {
    if (-not (Test-Path -LiteralPath $s)) { throw "Provisioning tool not found: $s" }
}

# Device / DFP expectations pinned from the manifest (single source of truth).
$expectDevice = 'dsPIC33AK512MPS512'
$expectDfp    = 'dsPIC33AK-MP_DFP/1.3.185'

# Bundle / report paths mirror gen_dual_partition_hex.py's own default naming.
$bundleHex = ($Hex -replace '\.hex$', '') + '.bundle.hex'
$genReport = ($bundleHex -replace '\.hex$', '') + '.gen_report.txt'
$verReport = ($bundleHex -replace '\.hex$', '') + '.verify_report.txt'

Write-Status "provision: dual-partition UCA"
Write-Status "  config : $Configuration"
Write-Status "  P1 hex : $Hex"
Write-Status "  bundle : $bundleHex"

# 1) Generate the bundle (clones P1 UCA -> P2 UCA). Refuses on missing/broken P1 UCA.
$genArgs = @($genScript, $Hex, '-o', $bundleHex, '--report', $genReport)
$rc = Invoke-CheckedPython -Python $python -Arguments $genArgs
if ($rc -ne 0) {
    throw "gen_dual_partition_hex.py FAILED (exit $rc). No bundle produced."
}

# 2) Verify the bundle (read-only). FAIL LOUDLY on anything but PASS.
$verArgs = @($verifyScript, $bundleHex, '--report', $verReport,
             '--expect-device', $expectDevice, '--expect-dfp', $expectDfp)
$rc = Invoke-CheckedPython -Python $python -Arguments $verArgs
if ($rc -ne 0) {
    throw "verify_dual_partition_hex.py did NOT PASS (exit $rc). The bundle is NOT a publishable provisioning artifact. See: $verReport"
}

Write-Status ""
Write-Status "provision: PASS"
Write-Status "  bundle : $bundleHex"
Write-Status "  gen rep: $genReport"
Write-Status "  ver rep: $verReport"
Write-Status ""
Write-Status "Flash it with:"
Write-Status "  pwsh -File buildtools/flashauto.ps1 -Bundle -Configuration $Configuration"
