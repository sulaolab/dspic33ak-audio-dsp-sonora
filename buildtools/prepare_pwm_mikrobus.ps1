# Build and preserve a Sonora MPQ6612A ROM image. No hardware access.
[CmdletBinding()]
param(
    [int]$FirmwareVersion = 1,
    [switch]$Stereo,
    [switch]$ScopeOnly,
    [switch]$HalAudio = $true,
    [ValidateSet(48000, 96000, 192000)][int]$CarrierHz = 48000,
    [ValidateSet('MikroB', 'Xpro1')][string]$Route = 'Xpro1',
    [switch]$CodecB
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$configuration = 'dsPIC33AK512_CLASSIC_SERIAL_UPDATE'
$device = 'dsPIC33AK512MPS512'
$enableB = $Stereo -or $Route -eq 'MikroB'
$enableXpro = $Stereo -or $Route -eq 'Xpro1'
if ($enableB -and $CodecB) { throw 'CODEC-B and MikroBUS B PWM are mutually exclusive.' }
if (-not $enableB -and -not $HalAudio) { throw 'XPRO1-only PWM requires -HalAudio.' }
$carrierFactor = [int]($CarrierHz / 48000)
$carrierK = [int]($CarrierHz / 1000)
$defines = @("MPQ6612A_CARRIER_FACTOR=$carrierFactor", 'APP_PWM_OUTPUT=1', "APP_REQ_MIKROB_WM8904=$([int][bool]$CodecB)",
    "APP_PWM_AUDIO_HAL=$([int][bool]$HalAudio)",
    "MPQ6612A_ENABLE_MIKROB=$([int][bool]$enableB)",
    "MPQ6612A_ENABLE_XPRO1=$([int][bool]$enableXpro)",
    "MPQ6612A_STEREO=$([int][bool]$Stereo)")
if ($ScopeOnly) { $defines += 'PWM_AUDIO_SCOPE_ONLY=1' }
$routeName = if ($Stereo) { 'b_xpro1_stereo' } elseif ($enableB) { 'b' } else { 'xpro1' }
$codecSuffix = if ($CodecB) { '_codec-b' } else { '' }
$mode = if ($ScopeOnly) { '_scope-only' } else { '' }
if ($HalAudio) { $mode += '_hal-audio' }

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'switch_config.ps1') -SerialUpdateSupport Yes `
    -Device $device -Profile 'Classic 1' -Root $repoRoot
if ($LASTEXITCODE -ne 0) { throw 'PWM build selection failed.' }
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Full -Root $repoRoot `
    -Define ($defines -join ',') -FirmwareVersion $FirmwareVersion
if ($LASTEXITCODE -ne 0) { throw 'PWM delivery build failed.' }

$production = Join-Path $repoRoot "dspic33ak_audio_dsp.X/dist/$configuration/production"
$factory = Join-Path $production 'dspic33ak_audio_dsp.X.factory.production.hex'
$package = Join-Path $production 'dspic33ak_audio_dsp.X.production.sfb'
$meta = Get-Content -LiteralPath ([IO.Path]::ChangeExtension($factory, '.json')) -Raw | ConvertFrom-Json
if ($meta.device -ne $device -or $meta.application_profile -ne 'APP_BUILD_STD_DEMO_1' -or
    -not $meta.serial_update_support) { throw 'Unexpected factory image selection.' }

# Keep a named copy outside dist: later Classic/legacy builds may replace dist.
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$output = Join-Path $repoRoot "artifacts/pwm_mikrobus/$stamp"
New-Item -ItemType Directory -Path $output | Out-Null
$hexName = "sonora_ak512_mpq6612a_${routeName}_${carrierK}k${mode}${codecSuffix}.factory.hex"
$packageName = "sonora_ak512_mpq6612a_${routeName}_${carrierK}k${mode}${codecSuffix}.sfb"
Copy-Item -LiteralPath $factory -Destination (Join-Path $output $hexName)
Copy-Item -LiteralPath $package -Destination (Join-Path $output $packageName)
Copy-Item -LiteralPath ([IO.Path]::ChangeExtension($factory, '.json')) `
    -Destination (Join-Path $output 'factory-build.json')
$sourceCommit = & git -C $repoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source commit.' }
$sourceStatus = @(& git -C $repoRoot status --porcelain=v1)
& git -C $repoRoot diff --binary HEAD | Set-Content -LiteralPath (Join-Path $output 'source.patch') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source delta.' }
$untrackedSources = @(& git -C $repoRoot ls-files --others --exclude-standard -- src buildtools tools)
foreach ($relative in $untrackedSources) {
    $copyTo = Join-Path $output "source-untracked/$relative"
    New-Item -ItemType Directory -Path (Split-Path -Parent $copyTo) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot $relative) -Destination $copyTo
}
$manifest = [ordered]@{
    device = $device
    application_profile = 'APP_BUILD_STD_DEMO_1'
    configuration = $configuration
    monitor_profile = 'ak-curio'
    pwm_output = 1
    pwm_audio_hal = [bool]$HalAudio
    hal_source_commit = $(if ($HalAudio) { 'f6e06819657f09d3f6cb75f1709e7c1837d25dc7' } else { $null })
    codec_b = [int][bool]$CodecB
    pwm_mikrob = [bool]$enableB
    pwm_xpro1 = [bool]$enableXpro
    carrier_hz = $CarrierHz
    carrier_factor = $carrierFactor
    pwm_gain_db = if ($ScopeOnly) { 0 } else { 12 }
    audio_channel = $(if ($Stereo) { 'L/R' } else { 'L' })
    stereo = [bool]$Stereo
    scope_only = [bool]$ScopeOnly
    nsleep_policy = $(if ($ScopeOnly) { 'held-low' } else { 'coast-wake-neutral; brake-sleep-stop' })
    extra_defines = $defines
    firmware_version = $FirmwareVersion
    source_commit = [string]$sourceCommit
    source_status = $sourceStatus
    untracked_sources = $untrackedSources
    built_at = (Get-Date).ToString('o')
    hex_file = $hexName
    hex_sha256 = (Get-FileHash -LiteralPath (Join-Path $output $hexName) -Algorithm SHA256).Hash
    package_file = $packageName
    package_sha256 = (Get-FileHash -LiteralPath (Join-Path $output $packageName) -Algorithm SHA256).Hash
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'ready.json') -Encoding utf8
# Publish the pointer only after both artifacts and their provenance are saved.
$output | Set-Content -LiteralPath (Join-Path $repoRoot 'artifacts/pwm_mikrobus/latest.txt') -Encoding utf8
$profilePointer = "latest-${routeName}${mode}${codecSuffix}.txt"
$output | Set-Content -LiteralPath (Join-Path $repoRoot "artifacts/pwm_mikrobus/$profilePointer") -Encoding utf8
Write-Host "PWM first-light ROM image ready: $output"
Write-Host 'Verify only: pwsh buildtools/flash_pwm_mikrobus.ps1'
Write-Host 'After the board is available: pwsh buildtools/flash_pwm_mikrobus.ps1 -Flash -Serial <PKOB4_SERIAL>'
