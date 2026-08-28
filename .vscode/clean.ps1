# Clean the build outputs for one configuration, robustly.
#
# Why not "make .clean-conf"? The MPLAB X IDE background service (mplab_backend)
# keeps file handles on build/<conf>/production/_ext/* directories even after the
# IDE window is closed. GnuWin32 "rm -rf" then aborts with "Permission denied" on
# those directories (exit 1), which also breaks the Rebuild sequence.
#
# This script deletes all output FILES (the functional clean) and only best-effort
# removes the directories, tolerating empty dirs that the backend still holds (those
# are harmless and recreated on the next build). It fails only if real output files
# stay locked.
#
# Invoked by the "MPLAB: Clean" task with cwd = dspic33ak_audio_dsp.X.
# Configuration overridable via MPLABX_CONF.
$ErrorActionPreference = 'Continue'

$conf = if ($env:MPLABX_CONF) { $env:MPLABX_CONF } else { 'dsPIC33AK512_CLASSIC_SERIAL_UPDATE' }
$targets = @("build\$conf", "dist\$conf")
$lockedFiles = 0

foreach ($t in $targets) {
    if (-not (Test-Path $t)) { Write-Host "already clean: $t"; continue }

    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Get-ChildItem -LiteralPath $t -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
            try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop } catch {}
        }
        try { Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction Stop } catch {}
        if (-not (Test-Path $t)) { break }
        Start-Sleep -Milliseconds 300
    }

    $left = @(Get-ChildItem -LiteralPath $t -Recurse -File -Force -ErrorAction SilentlyContinue)
    $lockedFiles += $left.Count
    if (-not (Test-Path $t)) {
        Write-Host "cleaned: $t"
    } elseif ($left.Count -eq 0) {
        Write-Host "cleaned (empty dirs still held by mplab_backend remain - harmless): $t"
    } else {
        Write-Host "WARNING: $($left.Count) output file(s) still locked under $t"
    }
}

if ($lockedFiles -gt 0) {
    Write-Error "Clean incomplete: $lockedFiles locked output file(s). Fully exit MPLAB X (including the mplab_backend process) and retry."
    exit 1
}
exit 0
