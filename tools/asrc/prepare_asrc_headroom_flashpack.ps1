[CmdletBinding()]
param(
    [string]$OutDir = 'notes_private/asrc_headroom_flashpack'
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $repo 'buildtools\build.ps1'
$configuration = 'dsPIC33AK512_ASRC'
$project = Join-Path $repo 'dspic33ak_audio_dsp.X'
$production = Join-Path $project "dist\$configuration\production"
$sourceHex = Join-Path $production 'dspic33ak_audio_dsp.X.production.hex'
$sourceMap = Join-Path $production 'dspic33ak_audio_dsp.X.production.map'
$packDir = if ([System.IO.Path]::IsPathRooted($OutDir)) {
    [System.IO.Path]::GetFullPath($OutDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $OutDir))
}

# C is deliberately last so the ordinary MPLAB production output is the formal
# M30 default image after this comparison-pack command finishes.
$stages = @(
    [pscustomobject]@{ Stage = 'A'; Preset = 'APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M32' },
    [pscustomobject]@{ Stage = 'B'; Preset = 'APP_BUILD_ASRC_CODEC_BIDIR_HEADROOM_M30' },
    [pscustomobject]@{ Stage = 'Q'; Preset = 'APP_BUILD_ASRC_CODEC_MEAS' },
    [pscustomobject]@{ Stage = 'C'; Preset = 'APP_BUILD_ASRC_CODEC_BIDIR' }
)

New-Item -ItemType Directory -Path $packDir -Force | Out-Null
$images = @()

Push-Location $repo
try {
    foreach ($item in $stages) {
        Write-Host "==> clean build stage $($item.Stage): $($item.Preset)" -ForegroundColor Cyan
        & $build -Full -Configuration dsPIC33AK512 -App Asrc -Preset $item.Preset
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for stage $($item.Stage) with exit code $LASTEXITCODE"
        }
        if (-not (Test-Path -LiteralPath $sourceHex) -or -not (Test-Path -LiteralPath $sourceMap)) {
            throw "Expected production HEX/map missing after stage $($item.Stage) build."
        }

        $fileName = "stage-$($item.Stage)-$($item.Preset).hex"
        $destHex = Join-Path $packDir $fileName
        Copy-Item -LiteralPath $sourceHex -Destination $destHex -Force

        $mapText = [System.IO.File]::ReadAllText($sourceMap)
        $programMatch = [regex]::Match($mapText, 'Total "program" memory used \(bytes\):.*?\((\d+)\)', 'Singleline')
        $dataMatch = [regex]::Match($mapText, 'Total "data" memory used \(bytes\):.*?\((\d+)\)', 'Singleline')
        if (-not $programMatch.Success -or -not $dataMatch.Success) {
            throw "Could not parse memory totals for stage $($item.Stage)."
        }

        $images += [pscustomobject]@{
            stage = $item.Stage
            preset = $item.Preset
            hex = $fileName
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $destHex).Hash
            program_bytes = [int]$programMatch.Groups[1].Value
            data_bytes = [int]$dataMatch.Groups[1].Value
        }
    }

    $head = (git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'git rev-parse HEAD failed.' }
    $branch = (git branch --show-current).Trim()
    $diffFingerprint = (git diff --binary HEAD | git hash-object --stdin).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint the tracked source diff.' }

    $manifest = [ordered]@{
        schema = 1
        generated_at = (Get-Date).ToString('o')
        repository = $repo
        branch = $branch
        head = $head
        tracked_diff_sha1 = $diffFingerprint
        configuration = $configuration
        final_production_stage = 'C'
        images = $images
    }
    $manifestPath = Join-Path $packDir 'manifest.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding utf8

    Write-Host "Flash pack: $packDir" -ForegroundColor Green
    $images | Format-Table stage, preset, program_bytes, data_bytes, sha256 -AutoSize
    Write-Host 'Production output left at stage C.' -ForegroundColor Green
}
finally {
    Pop-Location
}
