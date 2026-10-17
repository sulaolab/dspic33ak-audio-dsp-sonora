# Default: verify saved artifacts only. -DryRun is offline; -Flash programs hardware.
[CmdletBinding()]
param(
    [switch]$Flash,
    [switch]$DryRun,
    [string]$Serial,
    [string]$ImageDir
)

$ErrorActionPreference = 'Stop'
if ($Flash -and $DryRun) { throw 'Choose -Flash or -DryRun.' }
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($ImageDir)) {
    $pointer = Join-Path $repoRoot 'artifacts/pwm_mikrobus/latest.txt'
    if (-not (Test-Path -LiteralPath $pointer)) { throw 'Run prepare_pwm_mikrobus.ps1 first.' }
    $ImageDir = (Get-Content -LiteralPath $pointer -Raw).Trim()
}
$ImageDir = (Resolve-Path -LiteralPath $ImageDir).Path
$ready = Get-Content -LiteralPath (Join-Path $ImageDir 'ready.json') -Raw | ConvertFrom-Json
# Keep saved pre-migration Sonora manifests verifiable; flashauto resolves
# the current monitor profile from this repository's .serial-monitor.json.
if ($ready.device -ne 'dsPIC33AK512MPS512' -or $ready.monitor_profile -notin @('ak-curio', 'sonora') -or
    $ready.application_profile -ne 'APP_BUILD_STD_DEMO_1' -or $ready.pwm_output -ne 1 -or
    $ready.carrier_hz -notin @(48000, 96000, 192000)) { throw 'Not a Sonora MPQ6612A Classic 1 image.' }
# Old manifests describe the original 48 kHz build; new rates must name their define.
if ($ready.carrier_hz -ne 48000 -and
    ($ready.carrier_factor -ne ($ready.carrier_hz / 48000) -or
     $ready.extra_defines -notcontains "MPQ6612A_CARRIER_FACTOR=$($ready.carrier_factor)")) {
    throw 'Saved carrier setting is missing or inconsistent.'
}
$enableB = if ($null -ne $ready.pwm_mikrob) { [bool]$ready.pwm_mikrob } else { $true }
$enableXpro = if ($null -ne $ready.pwm_xpro1) { [bool]$ready.pwm_xpro1 } else { [bool]$ready.stereo }
if ($enableB -and $ready.codec_b -ne 0) { throw 'Saved image conflicts: CODEC-B and MikroBUS B PWM.' }
if (-not $enableB -and (-not $enableXpro -or -not $ready.pwm_audio_hal)) {
    throw 'Saved XPRO1-only image must use hal_pwm_audio.'
}
foreach ($kind in @('hex', 'package')) {
    $name = [string]$ready."${kind}_file"
    if ([IO.Path]::GetFileName($name) -ne $name) { throw 'Artifact filename must be a basename.' }
    $path = Join-Path $ImageDir $name
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $ready."${kind}_sha256") {
        throw "Saved $kind hash differs from ready.json. Rebuild or select the original artifact."
    }
}
$hex = Join-Path $ImageDir $ready.hex_file
Write-Host "Verified: $hex"
Write-Host "Carrier: $($ready.carrier_hz) Hz; PWM gain: $($ready.pwm_gain_db) dB"
Write-Host "HAL audio: $($ready.pwm_audio_hal)"
Write-Host "PWM routes: MikroBUS B=$enableB XPRO1=$enableXpro; CODEC-B=$($ready.codec_b)"
Write-Host "Audio: $($ready.audio_channel); stereo: $($ready.stereo); scope-only: $($ready.scope_only)"
if ($ready.scope_only) { Write-Host 'Scope-only: both bridges remain asleep; logic-pin observation only.' }
Write-Host "Source: $($ready.source_commit); saved source status: $($ready.source_status -join '; ')"
if (-not $Flash -and -not $DryRun) { return }
if ($Flash -and [string]::IsNullOrWhiteSpace($Serial)) { throw '-Flash requires the intended Sonora PKOB4 -Serial.' }
if ($DryRun -and [string]::IsNullOrWhiteSpace($Serial)) { $Serial = 'OFFLINE-PWM-PREFLIGHT' }

# Explicit saved HEX avoids selecting a later non-PWM dist build. Route all
# programming/reset through the sanctioned helper, with the non-Nano device.
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'flashauto.ps1') -Root $repoRoot -Hex $hex `
    -Configuration 'dsPIC33AK512_CLASSIC_SERIAL_UPDATE' -Device 'dsPIC33AK512MPS512' `
    -Serial $Serial -DryRun:$DryRun
if ($LASTEXITCODE -ne 0) { throw 'PWM flash/preflight failed.' }
