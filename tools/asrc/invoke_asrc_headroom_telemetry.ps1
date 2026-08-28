[CmdletBinding()]
param(
    [ValidateRange(1000, 60000)]
    [int]$PeriodMs = 10000,
    [ValidateRange(1, 50)]
    [int]$Windows = 6,
    [string]$Out,
    [string]$BaseUrl = 'http://127.0.0.1:8080',
    [ValidateRange(0, 60000)]
    [int]$RestorePeriodMs = 10000
)

$ErrorActionPreference = 'Stop'

function Invoke-MonitorWait {
    param(
        [Parameter(Mandatory = $true)][string]$Contains,
        [Parameter(Mandatory = $true)][int]$TimeoutSec
    )

    $body = @{ contains = $Contains; timeout = $TimeoutSec } | ConvertTo-Json -Compress
    Invoke-RestMethod "$BaseUrl/wait" -Method Post -ContentType 'application/json' -Body $body
}

function Send-TelemetryPeriod {
    param(
        [Parameter(Mandatory = $true)][int]$Milliseconds,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    if ($Milliseconds -eq 0) {
        $command = '*tq0000'
        $marker = 'telemetry OFF'
    }
    else {
        $command = '*tq0002{0:X4}' -f $Milliseconds
        $marker = "telemetry ON period=${Milliseconds}ms"
    }
    # /wait only observes lines received after the wait call.  The console ACK
    # can arrive between /command returning and a subsequent /wait request, so
    # use a log cursor established before sending to avoid that race.
    $ackStartLine = @(Get-Content -LiteralPath $LogPath).Count
    $body = @{ cmd = $command } | ConvertTo-Json -Compress
    Invoke-RestMethod "$BaseUrl/command" -Method Post -ContentType 'application/json' -Body $body | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    do {
        $matched = Get-Content -LiteralPath $LogPath | Select-Object -Skip $ackStartLine |
            Where-Object { $_ -like "*$marker*" } | Select-Object -First 1
        if ($null -ne $matched) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "timeout after 10s waiting for '$marker' in monitor log"
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
if ([string]::IsNullOrWhiteSpace($status.log_file) -or
    -not (Test-Path -LiteralPath $status.log_file)) {
    throw "Monitor log_file is unavailable: $($status.log_file)"
}

$logPath = [string]$status.log_file
$startLine = @(Get-Content -LiteralPath $logPath).Count
$timeoutSec = [Math]::Max(15, [int][Math]::Ceiling($PeriodMs / 1000.0) + 10)
$captureSucceeded = $false

try {
    Send-TelemetryPeriod -Milliseconds $PeriodMs -LogPath $logPath
    for ($i = 1; $i -le $Windows; $i++) {
        # CCP is the final line of each normal BIDIR report, so matching it means
        # STREAM/TDM/ASRC/ASRCpath from that window are already in the log.
        Invoke-MonitorWait -Contains 'CCP  fsA=' -TimeoutSec $timeoutSec | Out-Null
        Write-Host "Captured telemetry window $i/$Windows"
    }
    $captureSucceeded = $true
}
finally {
    if (($RestorePeriodMs -ne $PeriodMs) -and $status.connected) {
        try {
            Send-TelemetryPeriod -Milliseconds $RestorePeriodMs -LogPath $logPath
        }
        catch {
            Write-Warning "Could not restore telemetry period to ${RestorePeriodMs}ms: $($_.Exception.Message)"
        }
    }
}

if (-not $captureSucceeded) {
    throw 'Telemetry capture did not complete.'
}

$newLines = @(Get-Content -LiteralPath $logPath | Select-Object -Skip $startLine)
$payload = foreach ($line in $newLines) {
    if ($line -match '^\d\d:\d\d:\d\d\.\d+\s+<<\s?(.*)$') {
        $text = $Matches[1]
        # The console may prepend ANSI reset sequences or a stray prompt byte
        # to STREAM after terminal reconnects.  Keep the canonical telemetry
        # suffix rather than requiring the marker at byte zero.
        # The per-engine lines are '[<kernel> x<n>ch]AB ...' since 2026-07-29; they were
        # 'AB[<kernel> x<n>ch]: ...' earlier the same day and 'ASRC ab[...' before that. All
        # three spellings are accepted so this script can still summarize an older log.
        if ($text -match '(STREAM .*|TDM[12]:.*|TDMsum:.*|\[[^\]]*\](AB|BA) .*|(ASRC )?AB\[.*|(ASRC )?BA\[.*|ASRC ab\[.*|ASRC ba\[.*|ASRCpath\[.*|CCP  fsA=.*)$') {
            $Matches[1]
        }
    }
}

$streamCount = @($payload | Where-Object { $_ -like 'STREAM *' }).Count
$sumCount = @($payload | Where-Object { $_ -like 'TDMsum:*' }).Count
$ccpCount = @($payload | Where-Object { $_ -like 'CCP  fsA=*' }).Count
if (($streamCount -lt $Windows) -or ($sumCount -lt $Windows) -or ($ccpCount -lt $Windows)) {
    throw "Incomplete telemetry groups: STREAM=$streamCount TDMsum=$sumCount CCP=$ccpCount expected=$Windows. Full monitor log: $logPath"
}

if ([string]::IsNullOrWhiteSpace($Out)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $Out = "notes_private/asrc-headroom-telemetry-$stamp.txt"
}
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) {
    [System.IO.Path]::GetFullPath($Out)
}
else {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    [System.IO.Path]::GetFullPath((Join-Path $repo $Out))
}
$outParent = Split-Path -Parent $outPath
if (-not (Test-Path -LiteralPath $outParent)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}
$payload | Set-Content -LiteralPath $outPath -Encoding ascii

Write-Host "Telemetry: $outPath" -ForegroundColor Green
Write-Host "Windows: STREAM=$streamCount TDMsum=$sumCount CCP=$ccpCount"
Write-Host "Monitor log: $logPath"
