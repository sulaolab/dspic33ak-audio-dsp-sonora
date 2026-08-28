# Build the active selection and print the one number this task is about.
#
# The ROM diet needs a fast "how many bytes now?" loop, and build.ps1's own
# output does not carry the total. This wrapper builds (application only -- no
# .sfb / factory HEX, since nothing is being flashed), then reads the totals
# straight out of the link map, and appends one line per run to a log so the
# sequence of steps stays readable afterwards.
param(
    [string]$Label = '',
    # AK128 with a resident bootloader: 128 KiB panel - 16 KiB bootloader
    # - one 4 KiB manifest page = 0x1B000. This is RESIDENT_APP_CAPACITY_BYTES in
    # src/shared/resident_de_manifest.h, which is the authority; the number is
    # restated here rather than derived because this is a measurement wrapper, and
    # it must be updated with that header. Passed in so the same script can measure
    # the standalone budget (131068) or the AK512 one.
    [int]$Budget = 110592,
    [switch]$Full,
    [string[]]$Define,
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

$ErrorActionPreference = 'Stop'

$log = Join-Path $PSScriptRoot 'measurements.log'
$buildLog = Join-Path $env:TEMP 'ak128_diet_build.log'

$args = @('-NoDelivery', '-Root', $Root)
if ($Full) { $args += '-Full' }
foreach ($d in @($Define)) { if ($d) { $args += @('-Define', $d) } }
& pwsh -NoProfile -File (Join-Path $Root 'buildtools\build.ps1') @args *>&1 |
    Tee-Object -FilePath $buildLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED -- last lines of $buildLog :"
    Get-Content $buildLog -Tail 25
    exit 1
}

$state = Get-Content (Join-Path $Root 'buildtools\active_build.json') | ConvertFrom-Json
$map = Join-Path $Root ("dspic33ak_audio_dsp.X\dist\{0}\production\dspic33ak_audio_dsp.X.production.map" -f $state.resolved_configuration)
$text = Get-Content $map -Raw

$prog = [regex]::Match($text, 'Total "program" memory used \(bytes\):\s+0x[0-9a-f]+\s+\((\d+)\)')
$data = [regex]::Match($text, 'Total "data" memory used \(bytes\):\s+0x[0-9a-f]+\s+\((\d+)\)')
if (-not $prog.Success) { throw "no program total in $map" }

$used = [int]$prog.Groups[1].Value
$ram  = if ($data.Success) { [int]$data.Groups[1].Value } else { 0 }
$over = $used - $Budget

$line = ('{0,-46} program {1,7} B   RAM {2,6} B   vs budget {3,7} B: {4}{5} B' -f
    $Label, $used, $ram, $Budget, $(if ($over -gt 0) { 'OVER by ' } else { 'fits, spare ' }), [Math]::Abs($over))
Write-Host $line
Add-Content -Path $log -Value $line
