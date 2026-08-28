[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('A', 'B', 'C', 'Q')]
    [string]$Stage,
    [string]$PackDir = 'notes_private/asrc_headroom_flashpack',
    [string]$Serial,
    [switch]$CheckOnly,
    [switch]$AllowSourceMismatch
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resolvedPack = if ([System.IO.Path]::IsPathRooted($PackDir)) {
    [System.IO.Path]::GetFullPath($PackDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $PackDir))
}
$manifestPath = Join-Path $resolvedPack 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Flash-pack manifest not found: $manifestPath. Run prepare_asrc_headroom_flashpack.ps1 first."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$image = @($manifest.images | Where-Object { $_.stage -eq $Stage })
if ($image.Count -ne 1) {
    throw "Stage $Stage is missing or duplicated in $manifestPath."
}
$image = $image[0]
$hexPath = Join-Path $resolvedPack $image.hex
if (-not (Test-Path -LiteralPath $hexPath)) {
    throw "Cached HEX not found: $hexPath"
}
$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $hexPath).Hash
if ($actualHash -ne $image.sha256) {
    throw "Cached HEX hash mismatch for stage $Stage. Expected $($image.sha256), got $actualHash."
}

Push-Location $repo
try {
    $head = (git rev-parse HEAD).Trim()
    $diffFingerprint = (git diff --binary HEAD | git hash-object --stdin).Trim()
    $sourceMatches = ($head -eq $manifest.head) -and
                     ($diffFingerprint -eq $manifest.tracked_diff_sha1)
    if (-not $sourceMatches -and -not $AllowSourceMismatch) {
        throw 'Current tracked source differs from the flash pack. Re-run prepare_asrc_headroom_flashpack.ps1, or explicitly pass -AllowSourceMismatch.'
    }

    Write-Host "Stage: $Stage" -ForegroundColor Cyan
    Write-Host "Preset: $($image.preset)"
    Write-Host "HEX: $hexPath"
    Write-Host "SHA-256: $actualHash"
    Write-Host "Memory: program=$($image.program_bytes) B, data=$($image.data_bytes) B"
    if (-not $sourceMatches) {
        Write-Warning 'Source mismatch override is active; flashing the cached, hash-verified image.'
    }
    if ($CheckOnly) {
        Write-Host 'Check only: no PKOB4 enumeration, flash, reset, monitor access, or COM access performed.' -ForegroundColor Green
        return
    }

    $flash = Join-Path $repo 'buildtools\flashauto.ps1'
    $flashParams = @{
        Configuration = [string]$manifest.configuration
        Hex = $hexPath
    }
    if (-not [string]::IsNullOrWhiteSpace($Serial)) {
        $flashParams.Serial = $Serial
    }

    # No switch is needed for the post-flash reset: flashauto always resets after a
    # successful flash. (-ResetOnly, formerly the misread -Reset, would skip the flash.)
    & $flash @flashParams
    if ($LASTEXITCODE -ne 0) {
        throw "flashauto failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
