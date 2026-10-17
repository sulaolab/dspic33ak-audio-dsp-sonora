# Read-only endpoint discovery. The serial-monitor owns the COM port; this helper
# only asks its machine-readable process/status inventory which URL is live.
function Resolve-RunningSerialMonitorUrl {
    param(
        [string]$RequestedUrl,
        [Parameter(Mandatory)][string]$ExpectedProfile,
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$MonitorRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedUrl)) {
        return $RequestedUrl.TrimEnd('/')
    }

    if ([string]::IsNullOrWhiteSpace($MonitorRoot)) {
        $cursor = [IO.DirectoryInfo]::new((Resolve-Path -LiteralPath $RepoRoot).Path)
        while ($cursor) {
            $candidate = Join-Path $cursor.FullName 'serial-monitor\resolve-serial-monitor.ps1'
            if (Test-Path -LiteralPath $candidate) {
                $MonitorRoot = Join-Path $cursor.FullName 'serial-monitor'
                break
            }
            $cursor = $cursor.Parent
        }
    }
    if ([string]::IsNullOrWhiteSpace($MonitorRoot)) {
        throw 'Cannot locate serial-monitor. Pass -MonitorUrl explicitly.'
    }

    $resolver = Join-Path $MonitorRoot 'resolve-serial-monitor.ps1'
    if (-not (Test-Path -LiteralPath $resolver)) {
        throw "serial-monitor resolver not found: $resolver"
    }

    $json = @(& $resolver -Profile $ExpectedProfile -Json)
    try {
        $resolved = ($json -join "`n") | ConvertFrom-Json
    } catch {
        throw "Could not parse serial-monitor resolver output ($($_.Exception.Message))."
    }
    if ([int]$resolved.schema_version -ne 1 -or
        [string]$resolved.profile -ine $ExpectedProfile -or
        [string]::IsNullOrWhiteSpace([string]$resolved.http_url)) {
        throw "serial-monitor resolver returned an unexpected result for '$ExpectedProfile'."
    }
    return ([string]$resolved.http_url).TrimEnd('/')
}
