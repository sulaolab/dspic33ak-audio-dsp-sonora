# flashauto.ps1 - build-adjacent flash (+reset) helper, board selected by PKOB4 serial.
#
# Output: by default the flash/reset tools now print the compact [flash]/[reset]
# progress log with a 5s heartbeat (proof-of-life during long waits). Pass -Verbose
# to instead stream the raw mdb / IPECMDBoost output (the previous behaviour), or
# -Quiet to suppress this wrapper's own status lines. See resetauto.ps1 for a
# reset-only shortcut.
#
# A flash ALWAYS resets the board afterwards; there is no switch for that and it
# cannot be turned off. -ResetOnly means "skip the flash, reset only". The reset
# timeout is fixed and not a parameter - a reset that times out is a power or
# connection problem, and shortening the wait only hides it.
#
# What gets programmed is always the FACTORY_IMAGE for the active selection
# (switch_config.ps1): with serial update support that is the resident bootloader +
# application + committed manifest, without it the standalone application. There is
# no per-mode difference in how this is run.
#
# The image also carries metadata of what it was built from, and this refuses to
# program one that no longer matches the selection -- changing profile and flashing
# without rebuilding would otherwise put the previous firmware on the board and
# report success.

param(
    # Reset the board WITHOUT flashing. Not "reset after flashing" - that is
    # unconditional. Named -ResetOnly because the old -Reset was read as the
    # latter and silently skipped the flash.
    [switch]$ResetOnly,
    [switch]$List,
    [switch]$DryRun,
    [switch]$Verbose,
    [switch]$Quiet,
    [Alias('clean-java')]
    [switch]$CleanJava,
    [string]$Serial = $env:PKOB4_SERIAL,
    [string]$Device,
    [string]$ResetDevice,
    [string]$Hex,
    [string]$Configuration,
    # The repository this script belongs to, not the current directory: running
    # from buildtools/ (or from another repo's root) must not change which board
    # image is flashed. Same default as switch_config.ps1. Pass -Root to override.
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$ProjectDir,
    [string]$ToolsDir = $env:FLASH_RESET_TOOLS,
    <#
      Before a programmer flash or reset, ask the currently running Sonora
      image to analog-mute its WM8904 codec(s) and stop TDM/DMA.

      BEST EFFORT, NOT A PRECONDITION. A flash is frequently the response to an
      image that has stopped answering -- a hung console, a trap loop, a board
      still in bootloader recovery, an image that predates *ts. Making a console
      reply a condition for programming meant the tool refused hardest in exactly
      the situation it exists for. If the mute cannot be confirmed, this script
      now says so loudly and programs anyway; the risk it was guarding against is
      loud output for a second, not a bad image.

      Use -RequireAudioStop to get the old behaviour (abort unless the mute is
      confirmed) when speakers are connected and that second matters. Use
      'none' to skip the attempt entirely.
    #>
    [string]$StopAudioCommand = '*ts',
    # Turn the best-effort pre-flash mute back into a precondition. Only for a
    # setup where unmuted output is genuinely unacceptable -- never as a default,
    # and never in a script that must work on an unresponsive board.
    [switch]$RequireAudioStop,
    [string]$MonitorUrl,
    [ValidateRange(1, 60)]
    [int]$UartTimeoutSec = 20,

    # --- retired parameters: accepted only to explain themselves ---------------
    # Declared so a caller that passes them gets the reason rather than a bare
    # "parameter cannot be found" (or, worse for -Reset, silently different work).
    # Typed loosely on purpose: the point is to reach the message below, not to
    # bind a value.
    [Alias('Timeout', 'TimeoutSec')]
    [string]$ResetTimeoutSec,
    [switch]$Reset,
    [switch]$Bundle
)

$ErrorActionPreference = 'Stop'

# The reset wait is deliberately not adjustable. Every observed "it timed out"
# incident was a short timeout passed by a caller, and the reaction to a real
# timeout is to check power/USB and the B-jumper, never to wait less. Kept as an
# explicit constant rather than relying on reset_pkob4's own default so the
# effective wait stays pinned here. Not named $ResetTimeoutSec: that name belongs
# to the retired parameter below.
$script:SonoraResetTimeoutSec = 120

if ($PSBoundParameters.ContainsKey('ResetTimeoutSec')) {
    throw @"
The reset timeout is not adjustable (fixed at $($script:SonoraResetTimeoutSec)s); drop -ResetTimeoutSec / -Timeout.
A reset that times out means the board is not answering - check power/USB, the
B-jumper, and that no other tool holds the PKOB4. Shortening the wait only turns
a slow cold start into a false failure. If you genuinely need a different wait,
call buildtools\_flash_reset_tools\reset_pkob4.exe directly.
"@
}

if ($Reset) {
    throw @'
-Reset is retired because it was read as "reset after flashing". Use:
  -ResetOnly   reset the board and do NOT flash it
  (no switch)  flash, then reset - the reset after a flash is unconditional
'@
}

if ($Bundle) {
    throw @'
-Bundle is retired: it flashed the A/B Flash-Dual-Partition provisioning bundle
(P1+P2 UCA cloning), and that mechanism has been removed from this tree. src/app/main.c
now sets BTMODE=SINGLE, so a dual-partition bundle would provision partition fuses
for a boot mode the firmware no longer uses.

Serial firmware update is now the resident bootloader. Flash the seed ROM with:
  ./buildtools/switch_config.ps1     # serial update support = Yes
  ./buildtools/build.ps1
  ./buildtools/flashauto.ps1

The completed A/B implementation is maintained outside this repository.
'@
}

if ($Verbose -and $Quiet) {
    throw "Use either -Verbose or -Quiet, not both."
}

. (Join-Path $PSScriptRoot 'sonora_build_state.ps1')

function Write-Status {
    param(
        [string]$Message
    )

    if (-not $Quiet) {
        Write-Host $Message
    }
}

function Resolve-FlashResetToolsDir {
    param(
        [string]$RequestedToolsDir,
        [string]$Root
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedToolsDir)) {
        return (Resolve-Path -LiteralPath $RequestedToolsDir).Path
    }

    $scriptToolsDir = Join-Path $PSScriptRoot '_flash_reset_tools'
    if (Test-Path -LiteralPath $scriptToolsDir) {
        return (Resolve-Path -LiteralPath $scriptToolsDir).Path
    }

    $repoToolsDir = Join-Path $Root '_flash_reset_tools'
    if (Test-Path -LiteralPath $repoToolsDir) {
        return (Resolve-Path -LiteralPath $repoToolsDir).Path
    }

    $siblingToolsDir = Join-Path (Split-Path -Parent $Root) '_flash_reset_tools'
    if (Test-Path -LiteralPath $siblingToolsDir) {
        return (Resolve-Path -LiteralPath $siblingToolsDir).Path
    }

    throw "Flash/reset tools directory not found. Expected .\buildtools\_flash_reset_tools, or set FLASH_RESET_TOOLS / pass -ToolsDir."
}

function Get-ConnectedPkob4Serials {
    param(
        [string]$FlashTool
    )

    Write-Status "Checking connected PKOB4 serials..."
    $output = & $FlashTool --list 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | Write-Host
        throw "PKOB4 list failed with exit code $LASTEXITCODE"
    }

    return @($output | ForEach-Object {
        if ($_ -match '^\s*([0-9A-Z]{10,})\s*$') {
            $matches[1]
        }
    })
}

function Resolve-Pkob4Serial {
    param(
        [string]$RequestedSerial,
        [string]$FlashTool
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedSerial)) {
        return $RequestedSerial
    }

    $serials = @(Get-ConnectedPkob4Serials -FlashTool $FlashTool)
    if ($serials.Count -eq 1) {
        Write-Status "Serial: $($serials[0]) (auto-detected)"
        return $serials[0]
    }
    if ($serials.Count -eq 0) {
        Write-Host "No connected PKOB4 serial found."
        Write-Host "Connect one target, or pass -Serial if the tool list is not available."
        exit 2
    }

    Write-Host "Multiple PKOB4 serials found. Refusing to choose a target automatically."
    Write-Host "Connected serials:"
    foreach ($serial in $serials) {
        Write-Host "  $serial"
    }
    Write-Host ""
    Write-Host "Run again with an explicit serial, for example:"
    Write-Host "  .\buildtools\flashauto.ps1 -Serial $($serials[0])"
    Write-Host "  .\buildtools\flashauto.ps1 -ResetOnly -Serial $($serials[0])"
    exit 2
}

function Resolve-ProductionHex {
    param(
        [string]$RequestedHex,
        [string]$ProjectDir,
        [string]$Configuration
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedHex)) {
        return (Resolve-Path -LiteralPath $RequestedHex).Path
    }

    $projectName = Split-Path -Leaf $ProjectDir
    $prodDir = Join-Path $ProjectDir "dist\$Configuration\production"

    # Always the FACTORY_IMAGE, in both delivery modes -- with serial update support
    # it is the resident bootloader + application + committed manifest, without it the
    # standalone application. One name means this has nothing to decide, and there is
    # no path here that programs a serial-update application HEX, which has no reset
    # vector and would leave a board with nothing at reset while the flash reported
    # success.
    $factoryPath = Join-Path $prodDir "$projectName.factory.production.hex"
    if (-not (Test-Path -LiteralPath $factoryPath)) {
        Write-Status "Expected FACTORY_IMAGE: $factoryPath"
        throw @"
FACTORY_IMAGE not found: $factoryPath

Nothing has been built for the active selection yet, or it was built with
-NoDelivery. Build it with:
  ./buildtools/build.ps1
"@
    }
    return (Resolve-Path -LiteralPath $factoryPath).Path
}

function Assert-FactoryImageMatchesSelection {
    <#
      The FACTORY_IMAGE records what it was built from. If the selection has moved on
      since -- a different profile, device or delivery mode -- then programming this
      image would put something other than what switch_config.ps1 currently says on
      the board, and the flash would report success either way. So compare, and stop.
    #>
    param(
        [string]$FactoryImage,
        [hashtable]$Selection,
        [string]$Configuration,
        [bool]$Quiet
    )

    $metaPath = [IO.Path]::ChangeExtension($FactoryImage, '.json')
    if (-not (Test-Path -LiteralPath $metaPath)) {
        # Built before this metadata existed, or by hand. Say so rather than either
        # refusing a good image or pretending it was checked.
        if (-not $Quiet) {
            Write-Host "NOTE: this FACTORY_IMAGE carries no build metadata, so it could not be"
            Write-Host "      checked against the active selection. Rebuild to get that check."
        }
        return
    }

    try {
        $meta = Get-Content -LiteralPath $metaPath -Raw | ConvertFrom-Json
    } catch {
        throw "Could not read FACTORY_IMAGE metadata $metaPath ($($_.Exception.Message)). Rebuild with ./buildtools/build.ps1."
    }

    $differences = @()
    if ([bool]$meta.serial_update_support -ne $Selection.SerialUpdateSupport) {
        $differences += "  serial update support : image {0}, selection {1}" -f
            $(if ([bool]$meta.serial_update_support) { 'Yes' } else { 'No' }),
            $(if ($Selection.SerialUpdateSupport) { 'Yes' } else { 'No' })
    }
    if ([string]$meta.device -ne $Selection.Device) {
        $differences += "  target device         : image $($meta.device), selection $($Selection.Device)"
    }
    if ([string]$meta.application_profile -ne $Selection.Profile) {
        $differences += "  application profile   : image $($meta.application_profile), selection $($Selection.Profile)"
    }

    if ($differences.Count -gt 0) {
        throw @"
FACTORY_IMAGE does not match the active selection.
$($differences -join "`n")
Run:
  ./buildtools/build.ps1
"@
    }
}


function Convert-ToResetDevice {
    param(
        [string]$Device
    )

    if ($Device.StartsWith('dsPIC', [System.StringComparison]::OrdinalIgnoreCase)) {
        return $Device.Substring(5)
    }

    return $Device
}

function Invoke-CheckedExe {
    param(
        [string]$Exe,
        [string[]]$Arguments
    )

    Write-Status "==> $Exe $($Arguments -join ' ')"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE"
    }
}

function Invoke-ResetJavaCleanup {
    param(
        [string]$ResetTool,
        [bool]$DryRun
    )

    if ($DryRun) {
        Write-Status "Dry-run: would clean PKOB4 Boost Java state before reset."
        return
    }

    $cleanupArgs = @('--clean-java')
    if (-not $Quiet) { $cleanupArgs += '--verbose' }

    Write-Status "Cleaning PKOB4 Boost Java state before reset..."
    Invoke-CheckedExe -Exe $ResetTool -Arguments $cleanupArgs
}

function Get-ExpectedMonitorProfile {
    param(
        [string]$RepoRoot
    )

    $configPath = Join-Path $RepoRoot '.serial-monitor.json'
    if (-not (Test-Path -LiteralPath $configPath)) {
        throw "Serial-monitor profile config not found: $configPath"
    }
    try {
        $profile = (Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json).profile
    } catch {
        throw "Could not read serial-monitor profile config $configPath ($($_.Exception.Message))."
    }
    if ([string]::IsNullOrWhiteSpace($profile)) {
        throw "Serial-monitor profile config has no profile: $configPath"
    }
    return [string]$profile
}

function Resolve-MonitorUrl {
    param(
        [string]$RequestedUrl,
        [string]$RepoRoot
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedUrl)) {
        return $RequestedUrl.TrimEnd('/')
    }

    # Do not bake 127.0.0.1 into this flash tool. Multiple monitors use the
    # same port on different loopback aliases; the launcher resolves the alias
    # from this worktree's .serial-monitor.json without opening a COM port.
    $launcher = Join-Path (Split-Path -Parent $RepoRoot) 'serial-monitor\start-serial-monitor.ps1'
    if (-not (Test-Path -LiteralPath $launcher)) {
        throw "Cannot resolve serial-monitor URL: launcher not found at $launcher. Pass -MonitorUrl explicitly."
    }

    Push-Location -LiteralPath $RepoRoot
    try {
        # `*>&1`, not `2>&1`: the launcher prints the resolved bind with
        # Write-Host, which goes to the information stream. Redirecting only
        # stderr leaves $listing empty while the line is plainly on screen.
        $listing = @(& $launcher -List *>&1 | ForEach-Object { [string]$_ })
        $launcherExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    # A PowerShell launcher that returns without `exit` leaves $LASTEXITCODE
    # unset; only a non-zero value is a failure.
    if (($null -ne $launcherExitCode) -and ($launcherExitCode -ne 0)) {
        throw "serial-monitor URL resolution failed (exit $launcherExitCode). Pass -MonitorUrl explicitly."
    }

    foreach ($line in $listing) {
        if ([string]$line -match "^Profile '[^']+'\s+->\s+bind\s+([^\s]+)") {
            return "http://$($matches[1]):8080"
        }
    }
    throw "Could not find the resolved serial-monitor bind in launcher output. Pass -MonitorUrl explicitly."
}

# Why every failure below is a warning and not a throw:
#
# The pre-flash mute is a courtesy to whoever is listening, and it depends on the
# TARGET being alive enough to parse a console command. Programming does not: the
# PKOB4 reaches the device over the debug interface and does not care what the
# current image is doing, or whether it is doing anything at all. So a design in
# which "the target did not answer" blocks the flash inverts the priority -- it
# withholds the repair because the patient is unconscious. Reported by a flash of
# dsPIC33AK512_CLASSIC_SERIAL_UPDATE onto a board that was sitting in resident
# bootloader recovery, where *ts does not exist and no amount of waiting would
# have produced the marker.
#
# What is NOT downgraded: anything about WHICH BOARD or WHICH IMAGE. A wrong
# profile still cancels the console attempt (we must not type into someone else's
# board) but no longer cancels the flash, because the flash target is chosen by
# PKOB4 serial and is unaffected by whatever the monitor is attached to.
function Invoke-PreFlashAudioStop {
    param(
        [string]$Command,
        [string]$RequestedMonitorUrl,
        [string]$RepoRoot,
        [int]$TimeoutSec,
        [bool]$DryRun,
        [bool]$Required
    )

    # One place decides whether an unconfirmed mute is fatal, so the two callers
    # (flash and -ResetOnly) cannot drift apart.
    function Stop-OrWarn {
        param([string]$Reason)

        if ($Required) {
            throw ("$Reason -- and -RequireAudioStop was given, so this is fatal. " +
                   'Drop -RequireAudioStop to program anyway (the flash itself does ' +
                   'not need the console).')
        }
        Write-Warning ("Pre-flash audio stop NOT confirmed: $Reason")
        Write-Warning ('Programming anyway. The output is possibly UNMUTED for a moment -- ' +
                       'turn the volume down or unplug the speakers if that matters.')
    }

    if ([string]::Equals($Command, 'none', [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Status 'Pre-flash audio stop: skipped by -StopAudioCommand none.'
        return
    }
    if ([string]::IsNullOrWhiteSpace($Command)) {
        throw 'StopAudioCommand must be a console command or ''none''.'
    }

    # Get-ExpectedMonitorProfile/Resolve-MonitorUrl throw when the profile config
    # or the sibling serial-monitor repo is simply absent (e.g. a fresh clone
    # with no ../serial-monitor checked out yet). That is exactly the kind of
    # "could not confirm" failure the rest of this function treats as a warning,
    # not a reason to withhold the flash -- so it goes through Stop-OrWarn too.
    $expectedProfile = $null
    $url = $null
    try {
        $expectedProfile = Get-ExpectedMonitorProfile -RepoRoot $RepoRoot
        $url = Resolve-MonitorUrl -RequestedUrl $RequestedMonitorUrl -RepoRoot $RepoRoot
    } catch {
        Stop-OrWarn "could not resolve the serial-monitor for a pre-flash mute ($($_.Exception.Message))"
        return
    }
    if ($DryRun) {
        Write-Status "Dry-run: would confirm serial-monitor profile '$expectedProfile' at $url, then send '$Command' and wait for 'analog mute verified'."
        return
    }

    $status = $null
    try {
        $status = Invoke-RestMethod -Uri "$url/status" -TimeoutSec 5
    } catch {
        Stop-OrWarn "serial-monitor is unavailable at $url ($($_.Exception.Message))"
        return
    }
    if ([string]$status.profile -ne $expectedProfile) {
        # Not Stop-OrWarn's "we could not confirm" case but a "we must not try" one:
        # sending *ts here would type into a different board's console. Skipping the
        # attempt is mandatory; skipping the flash is not.
        Write-Warning ("Pre-flash audio stop SKIPPED: monitor at $url serves profile " +
                       "'$($status.profile)', but this worktree is '$expectedProfile'. " +
                       'Not sending anything -- that console belongs to another board.')
        if ($Required) {
            throw ("Refusing to drive monitor profile '$($status.profile)' at $url; this " +
                   "Sonora worktree requires '$expectedProfile'.")
        }
        return
    }
    if (-not [bool]$status.connected) {
        Stop-OrWarn "serial-monitor '$expectedProfile' at $url is not connected to its UART"
        return
    }
    if ($status.tx_gate.held_by) {
        Stop-OrWarn ("the monitor's transmit gate is held by '$($status.tx_gate.held_by)', " +
                     "so '$Command' cannot be sent")
        return
    }

    # /wait is deliberately armed FIRST. The command reply is often faster
    # than a process round-trip, so command-then-wait loses the only evidence.
    $client = [System.Net.Http.HttpClient]::new()
    try {
        $waitBody = @{ contains = 'analog mute verified'; timeout = $TimeoutSec } | ConvertTo-Json -Compress
        $waitContent = [System.Net.Http.StringContent]::new($waitBody, [Text.Encoding]::UTF8, 'application/json')
        $waitTask = $client.PostAsync("$url/wait", $waitContent)
        Start-Sleep -Milliseconds 100

        $commandBody = @{ cmd = $Command } | ConvertTo-Json -Compress
        $commandContent = [System.Net.Http.StringContent]::new($commandBody, [Text.Encoding]::UTF8, 'application/json')
        $commandResponse = $client.PostAsync("$url/command", $commandContent).GetAwaiter().GetResult()
        $commandText = $commandResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $commandResponse.IsSuccessStatusCode) {
            Stop-OrWarn ("the monitor rejected '$Command' (HTTP " +
                         "$([int]$commandResponse.StatusCode)): $commandText")
            return
        }

        $waitResponse = $waitTask.GetAwaiter().GetResult()
        $waitText = $waitResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if (-not $waitResponse.IsSuccessStatusCode) {
            Stop-OrWarn ("the target did not confirm a verified analog mute within " +
                         "${TimeoutSec}s (HTTP $([int]$waitResponse.StatusCode)): $waitText. " +
                         'A hung, trapped or bootloader-resident image cannot answer *ts')
            return
        }
        $waitResult = $waitText | ConvertFrom-Json
        if (-not [bool]$waitResult.matched) {
            Stop-OrWarn ("the target did not confirm a verified analog mute within ${TimeoutSec}s. " +
                         'A hung, trapped or bootloader-resident image cannot answer *ts')
            return
        }
        Write-Status "Pre-flash audio stop: '$Command' confirmed by '$($waitResult.line)'."
    } finally {
        $client.Dispose()
    }
}

$repoRoot = Resolve-SonoraRepoRoot -RequestedRoot $Root
$projectDir = Resolve-SonoraProjectDir -RepoRoot $repoRoot -RequestedProjectDir $ProjectDir
$configurations = Get-SonoraConfigurations -ProjectDir $projectDir
$presetCatalog = Get-SonoraPresetCatalog -RepoRoot $repoRoot

# The selection is the authority; the configuration is derived from it, exactly as
# build.ps1 derives it, so both act on the same three choices.
$selection = Get-SonoraSelection -RepoRoot $repoRoot -Configurations $configurations -Catalog $presetCatalog
if ([string]::IsNullOrWhiteSpace($Configuration)) {
    $selectionProfile = Get-SonoraPreset -Catalog $presetCatalog -Name $selection.Profile
    $Configuration = (Resolve-SonoraConfiguration `
        -Configurations $configurations `
        -Device $selection.Device `
        -App $selectionProfile.App `
        -SerialUpdate $selection.SerialUpdateSupport).Name
}
if ([string]::IsNullOrWhiteSpace($Device)) {
    # Device token comes from the configuration's own targetDevice.
    $Device = (Get-SonoraConfiguration -Configurations $configurations -Name $Configuration).Device
}
$toolsDir = Resolve-FlashResetToolsDir -RequestedToolsDir $ToolsDir -Root $repoRoot
$flashTool = Join-Path $toolsDir 'flash_pkob4.exe'
$resetTool = Join-Path $toolsDir 'reset_pkob4.exe'

if (-not (Test-Path -LiteralPath $flashTool)) {
    throw "flash_pkob4.exe not found: $flashTool"
}
if (-not (Test-Path -LiteralPath $resetTool)) {
    throw "reset_pkob4.exe not found: $resetTool"
}

if ($List) {
    Write-Status "flashauto: list connected PKOB4 targets"
    Invoke-CheckedExe -Exe $flashTool -Arguments @('--list')
    return
}

if ($ResetOnly) {
    Write-Status "flashauto: reset only (no flash)"
} else {
    Write-Status "flashauto: flash + reset"
}

$serialNumber = Resolve-Pkob4Serial -RequestedSerial $Serial -FlashTool $flashTool
$resetDeviceToken = if ([string]::IsNullOrWhiteSpace($ResetDevice)) {
    Convert-ToResetDevice -Device $Device
} else {
    $ResetDevice
}

Write-Status "Root: $repoRoot"
Write-Status "Project: $projectDir"
Write-Status "Tools: $toolsDir"
Write-Status "Serial: $serialNumber"

if ($ResetOnly) {
    $resetArgs = @('--serial', $serialNumber, '--device', $resetDeviceToken, '--timeout', $script:SonoraResetTimeoutSec)
    if ($Verbose) { $resetArgs += '--verbose' }
    if ($DryRun) { $resetArgs += '--dry-run' }
    Write-Status "Reset device token: $resetDeviceToken"
    Write-Status "Reset timeout: $($script:SonoraResetTimeoutSec)s (fixed)"
    if ($CleanJava) {
        Invoke-ResetJavaCleanup -ResetTool $resetTool -DryRun $DryRun
    }
    Invoke-PreFlashAudioStop -Command $StopAudioCommand -RequestedMonitorUrl $MonitorUrl `
        -RepoRoot $repoRoot -TimeoutSec $UartTimeoutSec -DryRun:$DryRun `
        -Required:$RequireAudioStop
    Invoke-CheckedExe -Exe $resetTool -Arguments $resetArgs
    Write-Status "flashauto: reset completed"
    return
}

$hexPath = Resolve-ProductionHex -RequestedHex $Hex -ProjectDir $projectDir -Configuration $Configuration

# An explicitly passed -Hex is the caller's business; anything resolved from the
# selection gets checked against it.
if ([string]::IsNullOrWhiteSpace($Hex)) {
    Assert-FactoryImageMatchesSelection `
        -FactoryImage $hexPath -Selection $selection -Configuration $Configuration -Quiet:$Quiet
}
Write-Status "Configuration: $Configuration"
# APP_BUILD is not part of the HEX path, so say which variation the last build of
# this configuration produced (stamped by build.ps1; unknown after an IDE build).
$builtAppBuild = Get-SonoraBuiltPreset -ProjectDir $projectDir -Configuration $Configuration
if ($builtAppBuild) {
    Write-Status "Last build of this configuration: $builtAppBuild"
} else {
    Write-Status "Last build of this configuration: unknown (not built by build.ps1)"
}
Write-Status "Flash device token: $Device"
Write-Status "Reset device token: $resetDeviceToken"
Write-Status "HEX: $hexPath"

$flashArgs = @(
    '--serial', $serialNumber,
    '--device', $Device,
    '--hex', $hexPath
)
if ($Verbose) { $flashArgs += '--verbose' }
if ($DryRun) { $flashArgs += '--dry-run' }

Invoke-PreFlashAudioStop -Command $StopAudioCommand -RequestedMonitorUrl $MonitorUrl `
    -RepoRoot $repoRoot -TimeoutSec $UartTimeoutSec -DryRun:$DryRun `
    -Required:$RequireAudioStop
Invoke-CheckedExe -Exe $flashTool -Arguments $flashArgs

$resetArgs = @('--serial', $serialNumber, '--device', $resetDeviceToken, '--timeout', $script:SonoraResetTimeoutSec)
if ($Verbose) { $resetArgs += '--verbose' }
if ($DryRun) { $resetArgs += '--dry-run' }

Write-Status "Running reset after successful flash (always, not optional)..."
Write-Status "Reset timeout: $($script:SonoraResetTimeoutSec)s (fixed)"
if ($CleanJava) {
    Invoke-ResetJavaCleanup -ResetTool $resetTool -DryRun $DryRun
}
$resetStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
Invoke-CheckedExe -Exe $resetTool -Arguments $resetArgs
$resetStopwatch.Stop()
Write-Status ("Reset elapsed: {0:N1}s" -f $resetStopwatch.Elapsed.TotalSeconds)
Write-Status "flashauto: flash + reset completed"
