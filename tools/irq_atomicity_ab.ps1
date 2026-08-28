# A/B build harness for the IRQ-register-atomicity port to the NORA HALs.
#
# Builds the five MPLAB configurations through the sanctioned path
# (switch_config.ps1 -> bare build.ps1 -Full) and records, per configuration,
# the build log, the program/data size line, and a copy of the ELF/MAP so a
# before/after disassembly comparison can be done without rebuilding.
#
# Usage:  pwsh -File tools/irq_atomicity_ab.ps1 -Tag before
#         pwsh -File tools/irq_atomicity_ab.ps1 -Tag after
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string[]]$Only
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $root ("_ab_" + $Tag)
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$matrix = @(
    @{ Name = 'dsPIC33AK512';                       Serial = 'No';  Device = 'dsPIC33AK512MPS512'; Profile = 'Classic 1' }
    @{ Name = 'dsPIC33AK512_ASRC';                  Serial = 'No';  Device = 'dsPIC33AK512MPS512'; Profile = 'ASRC Codec BI' }
    @{ Name = 'dsPIC33AK512_CLASSIC_SERIAL_UPDATE'; Serial = 'Yes'; Device = 'dsPIC33AK512MPS512'; Profile = 'Classic 1' }
    @{ Name = 'dsPIC33AK512_ASRC_SERIAL_UPDATE';    Serial = 'Yes'; Device = 'dsPIC33AK512MPS512'; Profile = 'ASRC Codec BI' }
    @{ Name = 'dsPIC33AK128';                       Serial = 'No';  Device = 'dsPIC33AK128MC106';  Profile = 'Classic 1' }
)

foreach ($m in $matrix) {
    if ($Only -and ($Only -notcontains $m.Name)) { continue }
    $log = Join-Path $outDir ($m.Name + '.log')
    Write-Host ("=== " + $m.Name + " ===")

    & pwsh -NoProfile -File (Join-Path $root 'buildtools/switch_config.ps1') `
        -SerialUpdateSupport $m.Serial -Device $m.Device -Profile $m.Profile *>&1 |
        Tee-Object -FilePath $log | Out-Null

    & pwsh -NoProfile -File (Join-Path $root 'buildtools/build.ps1') -Full *>&1 |
        Tee-Object -FilePath $log -Append | Out-Null

    $dist = Join-Path $root ("dspic33ak_audio_dsp.X/dist/" + $m.Name + "/production")
    foreach ($ext in @('elf', 'map', 'hex')) {
        $f = Join-Path $dist ("dspic33ak_audio_dsp.X.production." + $ext)
        if (Test-Path $f) { Copy-Item $f (Join-Path $outDir ($m.Name + '.' + $ext)) -Force }
    }

    $size = Select-String -Path $log -Pattern 'Program\s+space|Data\s+space|program memory|data memory|used.*bytes' |
        Select-Object -First 12
    if ($size) { $size.Line | Set-Content (Join-Path $outDir ($m.Name + '.size.txt')) }
    $ok = Select-String -Path $log -Pattern 'BUILD SUCCESSFUL|BUILD FAILED' | Select-Object -Last 1
    Write-Host ("  " + $(if ($ok) { $ok.Line.Trim() } else { '(no BUILD line)' }))
}
Write-Host ("artifacts: " + $outDir)
