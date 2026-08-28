param(
    [ValidateSet('Low', 'High')]
    [string]$Tone = 'Low',
    # Explicit tone-table row (MEAS_TONE_ROW_*: 0=low, 1=high@48k, 2=high@96k), sent as *at 04.
    # -1 (default) uses -Tone, i.e. lets the firmware resolve "high" from the live source rate --
    # which is what you want for a measurement.  Set this only to force a MISMATCHED row on
    # purpose, e.g. to demonstrate that the 48 kHz-nominal HF table aliases on a 96 kHz leg.
    # 3..27 = the SW_* sweep rows added 2026-08-23 for the 48 -> 32 kHz N=97 audio-mode alias
    # study, where every sweep point is a named row.  The firmware validates the row against
    # MEAS_TABLE_N_TONES, so an out-of-range row is rejected by the board, not silently mapped.
    [ValidateRange(-1, 27)]
    [int]$Row = -1,
    [ValidateRange(0, 5)]
    [int]$LevelIndex = 0,
    [string]$Out = 'asrc_capture.txt',
    [string]$BaseUrl = 'http://127.0.0.1:8080',
    [ValidateRange(0, 30)]
    [int]$SettleSec = 5,
    [ValidateRange(1, 30)]
    [int]$CaptureSec = 3,
    # Seconds to let the CLOSED loop re-converge before *as00 freezes it. The script used to send
    # *as00 with no preceding *as01, so only the FIRST capture after boot froze a converged servo;
    # every later one in the same session re-latched an already-open-loop step. Measured 2026-08-02
    # on 96 k -> 48 k: three consecutive re-freezes all latched the identical stale step (+7.7 ppm
    # off the true ratio) and read -110.0 dB, while unfreeze-first runs landed at -0.3..-1.9 ppm and
    # -123.7..-127.1 dB. At 1-2 kHz this makes no measurable difference (far-out held -138.6 dB
    # across the same step spread), so pre-2026-08-02 low-tone series are unaffected -- but any
    # BAND-EDGE series is, because HF far-out tracks the frozen-step error directly.
    # 0 disables the unfreeze (old behaviour) if you deliberately want a stale-step comparison.
    [ValidateRange(0, 60)]
    [int]$ReconvergeSec = 10
)

$ErrorActionPreference = 'Stop'

function Send-BoardCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string]$Marker,
        [Parameter(Mandatory = $true)][string]$LogPath,
        [int]$TimeoutSec = 10
    )
    $ackStartLine = @(Get-Content -LiteralPath $LogPath).Count
    $body = @{ cmd = $Command } | ConvertTo-Json -Compress
    Invoke-RestMethod "$BaseUrl/command" -Method Post -ContentType 'application/json' -Body $body | Out-Null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    do {
        $matched = Get-Content -LiteralPath $LogPath | Select-Object -Skip $ackStartLine |
            Where-Object { $_ -like "*$Marker*" } | Select-Object -First 1
        if ($null -ne $matched) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "timeout after ${TimeoutSec}s waiting for '$Marker' in monitor log"
}

try {
    $status = Invoke-RestMethod "$BaseUrl/status"
}
catch {
    throw "serial-monitor is unreachable at $BaseUrl. Start it through ../serial-monitor/start-serial-monitor.ps1 (check the bind with -List); never open the COM port directly."
}
if (-not $status.connected) {
    throw 'serial-monitor is running but connected=false; do not bypass it with direct serial access.'
}
if ([string]::IsNullOrWhiteSpace($status.log_file) -or -not (Test-Path -LiteralPath $status.log_file)) {
    throw "Monitor log_file is unavailable: $($status.log_file)"
}

$logPath = [string]$status.log_file
$startLine = @(Get-Content -LiteralPath $logPath).Count
$levelHex = '{0:X2}' -f $LevelIndex
Send-BoardCommand -Command "*at02$levelHex" -Marker '*MEAS level source=' -LogPath $logPath
if ($Row -ge 0) {
    $rowHex = '{0:X2}' -f $Row
    Send-BoardCommand -Command "*at04$rowHex" -Marker '*MEAS tone=row' -LogPath $logPath
}
elseif ($Tone -eq 'Low') {
    Send-BoardCommand -Command '*at00' -Marker '*MEAS tone=low' -LogPath $logPath
}
else {
    Send-BoardCommand -Command '*at01' -Marker '*MEAS tone=high' -LogPath $logPath
}
# Return the loop to closed control and let it re-converge, so this capture's freeze latches a
# freshly-converged step rather than the previous capture's open-loop one (see -ReconvergeSec).
if ($ReconvergeSec -gt 0) {
    Send-BoardCommand -Command '*as01' -Marker '*MEAS unfreeze' -LogPath $logPath
    Start-Sleep -Seconds $ReconvergeSec
}
# Marker is the PREFIX only: *as00 freezes every live engine, and asrc_freeze_one() prints one
# " *MEAS freeze <leg>: ..." line per leg before the summary, so the exact tail differs by preset.
Send-BoardCommand -Command '*as00' -Marker '*MEAS freeze' -LogPath $logPath
if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }

Send-BoardCommand -Command '*ac' -Marker '*MEAS arm:' -LogPath $logPath
Start-Sleep -Seconds $CaptureSec
Send-BoardCommand -Command '?ac' -Marker '*MEAS_END' -LogPath $logPath -TimeoutSec 30

$newLines = @(Get-Content -LiteralPath $logPath | Select-Object -Skip $startLine)
# The monitor gained a per-line source tag after the 2026-08 captures were taken, and it logs
# unprintable traffic as '[raw ...]' / '[partial...]' pseudo-lines.  Strip the tag and drop the
# pseudo-lines, or every sample reaches the analyzer as '[source=uart] -1234'.
$payload = foreach ($line in $newLines) {
    if ($line -match '^\d\d:\d\d:\d\d\.\d+\s+<<\s?(.*)$') {
        $body = $Matches[1]
        if ($body -notmatch '^\[(raw |partial)') {
            $body -replace '^\[source=[^\]]*\]\s?', ''
        }
    }
}
$begin = -1
$end = -1
for ($i = 0; $i -lt $payload.Count; $i++) {
    if (($begin -lt 0) -and ($payload[$i] -match '\*MEAS_BEGIN')) { $begin = $i }
    if (($begin -ge 0) -and ($payload[$i] -match '\*MEAS_END')) { $end = $i; break }
}
if (($begin -lt 0) -or ($end -lt $begin)) {
    throw "No complete MEAS block found after line $startLine in $logPath"
}

$outPath = if ([System.IO.Path]::IsPathFullyQualified($Out)) {
    [System.IO.Path]::GetFullPath($Out)
}
else {
    [System.IO.Path]::GetFullPath($Out, (Get-Location).ProviderPath)
}
$payload[$begin..$end] | Set-Content -LiteralPath $outPath -Encoding ascii
Write-Host "Capture: $outPath"
Write-Host "Monitor log: $logPath"
