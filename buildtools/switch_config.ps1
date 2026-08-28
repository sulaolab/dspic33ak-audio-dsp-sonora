# switch_config.ps1 - select what the next build targets.
#
# Two choices, and a third only when the device has a choice to make:
#
#   1. Target device          - which silicon
#   2. Application profile    - which application feature set
#   ( Delivery mode           - resident bootloader + serial update, or a standalone
#                               application flashed directly. ASKED ONLY IF THE
#                               SELECTED DEVICE HAS BOTH. )
#
# Delivery mode used to be question 1. It stopped being a question on 2026-08-15,
# when the last standalone configuration disappeared: the two AK512 standalone
# configurations were deleted that morning and the AK128 gained a resident
# bootloader of its own the same day, so serial update is now how every device in
# this project is both delivered and developed. Asking a question with one possible
# answer -- and asking it FIRST, before the device it depends on -- is how someone
# picked a mode and then found their device missing from the next question.
#
# It is skipped, not deleted, and the skip is derived from configurations.xml, never
# from a device list in this script: restore a standalone configuration for a device
# and the question comes back for that device on its own, in its correct place,
# after the device is known. -SerialUpdateSupport still scripts it either way.
#
# The MPLAB configuration is NOT one of them. It is derived from the device plus
# the profile's application (see Resolve-SonoraConfiguration), because those two
# already determine it -- asking again only invites a combination the project
# cannot build. It is written to the caches MPLAB X and bare `make` read, but it
# is not shown in the normal output; pass -Internal to see what was resolved.
#
# The selection persists in buildtools/active_build.json (untracked), so a bare
# build.ps1 / flashauto.ps1 follows it. Run with no arguments for the interactive
# menu, or pass -SerialUpdateSupport / -Device / -Profile to script it.
#
# The profile list shows normal profiles only. -Advanced adds the advanced ones
# (specialized hardware / measurement setups), -All adds the internal ones too
# (development, comparison, load checks). Tier, display name and artifact tag all
# come from src/app/apps/app_build_config.h; there is no profile list in this script.
# -Profile <name> always works regardless of tier.

param(
    # Resident bootloader + serial firmware update, or a standalone application.
    [ValidateSet('No', 'Yes')]
    [string]$SerialUpdateSupport,
    # Target device, e.g. dsPIC33AK512MPS512.
    [string]$Device,
    # Application profile: an APP_BUILD_* name, or a display name ("Classic 1").
    # Deliberately shadows the automatic $PROFILE inside this script: -Profile is
    # the name this tool is specified to take, and nothing here reads the
    # PowerShell profile path. Helpers take -ProfileEntry / -ProfileName instead,
    # so the collision stops at this parameter.
    [string]$Profile,
    # Print the catalog and the current selection, change nothing.
    [switch]$List,
    # Also show advanced profiles in the list / interactive menu.
    [switch]$Advanced,
    # Also show internal profiles (implies -Advanced).
    [switch]$All,
    # Report profiles with no tier marker and exit non-zero if any exist.
    [switch]$CheckTiers,
    # Report profiles with a missing or duplicated artifact tag, exit non-zero.
    [switch]$CheckProfiles,
    # Also print the derived internals (MPLAB configuration, APP_BUILD name).
    [switch]$Internal,
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$ProjectDir,

    # --- superseded parameters: accepted during the transition ------------------
    # The configuration is derived now, and "preset" is called a profile. Both are
    # still honoured so existing notes and scripts keep working, with a warning.
    [string]$Configuration,
    [string]$Preset
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'sonora_build_state.ps1')

function Write-ProfileCatalog {
    param(
        [object[]]$Profiles,
        [string]$SelectedName
    )

    for ($i = 0; $i -lt $Profiles.Count; $i++) {
        $entry = $Profiles[$i]
        Write-Host ("  {0,2}) {1}" -f ($i + 1), $entry.Display)
        if ($entry.Detail) {
            Write-Host ("      {0}" -f $entry.Detail)
        }
        $notes = @()
        if ($entry.Tier -ne 'normal') { $notes += "[$($entry.Tier)]" }
        if ($entry.Name -eq $SelectedName) { $notes += '<- selected' }
        if ($notes.Count -gt 0) { Write-Host ("      {0}" -f ($notes -join '  ')) }
    }
}

function Write-ProfileVisibilityHint {
    param(
        [object[]]$Shown,
        [object[]]$Available
    )

    $shownNames = @($Shown | Where-Object { $null -ne $_ } | ForEach-Object { $_.Name })
    $all = @($Available | Where-Object { $null -ne $_ })

    $hiddenTiers = @($all |
        Where-Object { $shownNames -notcontains $_.Name } |
        ForEach-Object { $_.Tier } |
        Select-Object -Unique |
        Sort-Object { $script:SonoraPresetTiers.IndexOf($_) })
    if ($hiddenTiers.Count -eq 0) { return }

    $switches = @()
    if ($hiddenTiers -contains 'advanced') { $switches += '-Advanced' }
    if (($hiddenTiers -contains 'internal') -or ($hiddenTiers -contains 'unclassified')) { $switches += '-All' }
    $hint = if ($switches.Count -gt 0) { " (show with $($switches -join ' / '))" } else { '' }

    Write-Host ("      ... {0} more hidden: {1}{2}" -f ($all.Count - $shownNames.Count), ($hiddenTiers -join ', '), $hint)
}

function Write-UnclassifiedProfileWarning {
    param([object[]]$Unclassified)

    $entries = @($Unclassified | Where-Object { $null -ne $_ })
    if ($entries.Count -eq 0) { return }

    Write-Host ''
    Write-Host 'WARNING:'
    Write-Host 'These profiles have no "tier:" marker in src/app/apps/app_build_config.h'
    Write-Host 'and are therefore hidden from every list (they are NOT treated as normal):'
    foreach ($entry in $entries) {
        Write-Host ("  {0} ({1})" -f $entry.Name, $entry.App)
    }
    Write-Host 'Add "tier: normal|advanced|internal" to each define comment.'
}

function Read-MenuChoice {
    <#
      Returns the 1-based index chosen, 0 for "keep current" (Enter), or $null for
      quit. Loops until the answer is valid.
    #>
    param(
        [string]$Prompt,
        [int]$Count
    )

    while ($true) {
        $answer = Read-Host $Prompt
        if ($null -eq $answer) { return $null }
        $answer = $answer.Trim()

        if ($answer -eq '') { return 0 }
        if ($answer -in @('q', 'Q', 'quit', 'exit')) { return $null }

        $index = 0
        if ([int]::TryParse($answer, [ref]$index) -and $index -ge 1 -and $index -le $Count) {
            return $index
        }

        Write-Host "  Enter 1-$Count, Enter to keep, or q to quit."
    }
}

function Resolve-ProfileArgument {
    <#
      -Profile accepts either the APP_BUILD_* name or the display name, because the
      menus show the display name and that is what a user has just been reading.
    #>
    param(
        [object]$Catalog,
        [string]$Value
    )

    $entry = Get-SonoraPreset -Catalog $Catalog -Name $Value
    if ($null -ne $entry) { return $entry }

    $byDisplay = @($Catalog.Presets | Where-Object { $_.Display -eq $Value })
    if ($byDisplay.Count -eq 1) { return $byDisplay[0] }
    if ($byDisplay.Count -gt 1) {
        throw "Ambiguous profile '$Value' - more than one profile uses that display name."
    }

    $known = ($Catalog.Presets | ForEach-Object { $_.Display }) -join ', '
    throw "Unknown application profile '$Value'. Available: $known"
}

function Get-AvailableProfiles {
    <#
      The profiles a device can actually build: those whose application has a
      configuration for that device.
    #>
    param(
        [object]$Catalog,
        [object[]]$Configurations,
        [string]$Device,
        [string[]]$Tiers
    )

    $profiles = @($Catalog.Presets |
        Where-Object { Test-SonoraProfileAvailable -Configurations $Configurations -Device $Device -ProfileEntry $_ })
    if ($null -ne $Tiers) {
        $profiles = @($profiles | Where-Object { $Tiers -contains $_.Tier })
    }
    return $profiles
}

function Write-ActiveSelection {
    param(
        [hashtable]$Selection,
        [object]$Catalog,
        [object[]]$Configurations,
        [bool]$ShowInternal
    )

    $entry = Get-SonoraPreset -Catalog $Catalog -Name $Selection.Profile
    $display = if ($null -ne $entry) { $entry.Display } else { $Selection.Profile }
    $serial = if ($Selection.SerialUpdateSupport) { 'Yes' } else { 'No' }

    Write-Host ''
    Write-Host 'Active selection:'
    Write-Host ''
    Write-Host ("  Serial update support: {0}" -f $serial)
    Write-Host ("  Target device:         {0}" -f $Selection.Device)
    Write-Host ("  Application profile:   {0}" -f $display)

    if ($Selection.SerialUpdateSupport) {
        Write-Host ''
        Write-Host 'The build will produce:'
        Write-Host '  FACTORY_IMAGE'
        Write-Host '  SERIAL_UPDATE_PACKAGE'
    }

    if ($ShowInternal) {
        $conf = Resolve-SonoraConfiguration `
            -Configurations $Configurations -Device $Selection.Device -App $entry.App `
            -SerialUpdate $Selection.SerialUpdateSupport
        Write-Host ''
        Write-Host ("  Resolved MPLAB configuration: {0}" -f $conf.Name)
        Write-Host ("  Resolved APP_BUILD:           {0}" -f $Selection.Profile)
        if ($entry.Artifact) {
            Write-Host ("  Artifact tag:                 {0}" -f $entry.Artifact)
        }
    }

    Write-Host ''
    Write-Host 'Next:'
    Write-Host '  .\buildtools\build.ps1'
    Write-Host '  .\buildtools\flashauto.ps1'
}

$repoRoot = Resolve-SonoraRepoRoot -RequestedRoot $Root
$projectDir = Resolve-SonoraProjectDir -RepoRoot $repoRoot -RequestedProjectDir $ProjectDir
$configurations = Get-SonoraConfigurations -ProjectDir $projectDir
$catalog = Get-SonoraPresetCatalog -RepoRoot $repoRoot

$serialUpdateDevices = @(Get-SonoraSerialUpdateDevices -RepoRoot $repoRoot)
# @() on every list that shrinks when a configuration is deleted: a function or
# an if statement that yields one item assigns a scalar, and the menus index into
# it. One device / one profile is now the normal case, not an edge case.
$devices = @(Get-SonoraDevices -Configurations $configurations)
# Which devices each mode has a configuration for. Two sets rather than one flag
# because the modes stopped being independent of the device on 2026-08-15: both
# devices are delivery-only now, so $devicesStandalone is legitimately EMPTY -- which
# is why it is @()-wrapped rather than assumed to hold something. Both are read from
# the catalog, so a restored configuration reappears in the menus with no edit here.
$devicesWithSerialUpdate = Get-SonoraDevicesForMode -Configurations $configurations -SerialUpdate $true
$devicesStandalone       = Get-SonoraDevicesForMode -Configurations $configurations -SerialUpdate $false
$visibleTiers = Get-SonoraVisibleTiers -Advanced:$Advanced -All:$All
$unclassified = Get-SonoraUnclassifiedPresets -Catalog $catalog
$current = Get-SonoraSelection -RepoRoot $repoRoot -Configurations $configurations -Catalog $catalog

# ----------------------------------------------------------------- checks -----
if ($CheckTiers) {
    if ($unclassified.Count -eq 0) {
        Write-Host "All $($catalog.Presets.Count) profile(s) in $($catalog.HeaderPath) carry a tier marker."
        return
    }
    Write-UnclassifiedProfileWarning -Unclassified $unclassified
    exit 1
}

if ($CheckProfiles) {
    $problems = @()
    foreach ($entry in $catalog.Presets) {
        if ([string]::IsNullOrWhiteSpace($entry.Artifact)) {
            $problems += "  $($entry.Name): no 'artifact:' tag"
        }
    }
    $duplicates = @($catalog.Presets |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_.Artifact) } |
        Group-Object -Property Artifact |
        Where-Object { $_.Count -gt 1 })
    foreach ($group in $duplicates) {
        $names = ($group.Group | ForEach-Object { $_.Name }) -join ', '
        $problems += "  artifact '$($group.Name)' used by more than one profile: $names"
    }

    if ($problems.Count -eq 0) {
        Write-Host "All $($catalog.Presets.Count) profile(s) in $($catalog.HeaderPath) carry a unique artifact tag."
        return
    }

    Write-Host 'Profile metadata problems in ' + $catalog.HeaderPath + ':'
    $problems | ForEach-Object { Write-Host $_ }
    Write-Host ''
    Write-Host 'An artifact tag names this profile''s serial update package file, so a'
    Write-Host 'missing or shared tag would make packages unidentifiable. Add or correct'
    Write-Host '"artifact: <lower_case_token>" in the define comment.'
    exit 1
}

# -------------------------------------------------------------- list only -----
if ($List) {
    Write-Host ("Project: {0}" -f $projectDir)
    Write-Host ''
    Write-Host 'Target devices:'
    foreach ($d in $devices) {
        $marker = if ($d -eq $current.Device) { '  <- selected' } else { '' }
        # Say which of the two modes this device does NOT have a configuration for.
        # Read from the catalog rather than compared against the bootloader's device:
        # since 2026-08-15 the interesting case is the other direction (AK512 has no
        # standalone configuration), and the old test could not express it.
        $note = if ($devicesWithSerialUpdate -notcontains $d) { '   (no serial update support)' }
                elseif ($devicesStandalone -notcontains $d)   { '   (serial update only)' }
                else { '' }
        Write-Host ("  {0}{1}{2}" -f $d, $note, $marker)
    }
    Write-Host ''
    $available = Get-AvailableProfiles -Catalog $catalog -Configurations $configurations -Device $current.Device
    $shown = @(Get-AvailableProfiles -Catalog $catalog -Configurations $configurations -Device $current.Device -Tiers $visibleTiers)
    Write-Host ("Application profiles for {0} (tier {1}):" -f $current.Device, ($visibleTiers -join '+'))
    Write-ProfileCatalog -Profiles $shown -SelectedName $current.Profile
    Write-ProfileVisibilityHint -Shown $shown -Available $available

    Write-ActiveSelection -Selection $current -Catalog $catalog -Configurations $configurations `
        -ShowInternal ($Internal -or $All)
    Write-UnclassifiedProfileWarning -Unclassified $unclassified
    return
}

# ------------------------------------------------- superseded parameters -------
if (-not [string]::IsNullOrWhiteSpace($Configuration)) {
    Write-Host 'NOTE: -Configuration is superseded. The MPLAB configuration is derived from'
    Write-Host '      the target device and the application profile; only its device is used.'
    $entry = Get-SonoraConfiguration -Configurations $configurations -Name $Configuration
    if ([string]::IsNullOrWhiteSpace($Device)) { $Device = $entry.Device }
}
if (-not [string]::IsNullOrWhiteSpace($Preset)) {
    Write-Host 'NOTE: -Preset is superseded by -Profile (same values, plus display names).'
    if ([string]::IsNullOrWhiteSpace($Profile)) { $Profile = $Preset }
}

$interactive = [string]::IsNullOrWhiteSpace($SerialUpdateSupport) -and
               [string]::IsNullOrWhiteSpace($Device) -and
               [string]::IsNullOrWhiteSpace($Profile)

# ------------------------------------------------------------- interactive ----
if ($interactive) {
    if ([Console]::IsInputRedirected) {
        throw @'
switch_config.ps1 needs a console for its interactive menu (stdin is redirected).
Pass the selection explicitly instead, for example:
  ./buildtools/switch_config.ps1 -SerialUpdateSupport Yes -Device dsPIC33AK512MPS512 -Profile "Classic 1"
  ./buildtools/switch_config.ps1 -List
'@
    }

    Write-Host ("Project: {0}" -f $projectDir)

    # --- 1. target device ---
    # Every device that has ANY configuration, in either delivery mode. The device is
    # the first question because the mode is a property of the device, not the other
    # way round: filtering devices by a mode chosen before the device is what used to
    # hide someone's board behind a "not shown because ..." line.
    # The outer @() is load-bearing: assigning the result of an if statement unrolls
    # a one-element array to a scalar, and the inner @() cannot prevent that. With
    # one device left after filtering $deviceChoices became the string itself, so
    # $deviceChoices[$i] indexed into the characters and the menu printed "1) d".
    $deviceChoices = @($devices | Where-Object {
        ($devicesWithSerialUpdate -contains $_) -or ($devicesStandalone -contains $_)
    })
    if ($deviceChoices.Count -eq 0) {
        throw ("No device in $($projectDir) has any configuration to build. " +
               'Check dspic33ak_audio_dsp.X/nbproject/configurations.xml.')
    }

    Write-Host ''
    Write-Host 'Target device:'
    Write-Host ''
    # Say each device's delivery mode on its line. It is not a question for a device
    # that has one mode, but it is still a fact worth seeing before choosing.
    for ($i = 0; $i -lt $deviceChoices.Count; $i++) {
        $d = $deviceChoices[$i]
        $hasSerial     = $devicesWithSerialUpdate -contains $d
        $hasStandalone = $devicesStandalone -contains $d
        $modeNote = if ($hasSerial -and $hasStandalone) { 'serial update or standalone' }
                    elseif ($hasSerial)                 { 'serial update' }
                    else                                { 'standalone' }
        Write-Host ("  {0}) {1}   ({2})" -f ($i + 1), $d, $modeNote)
    }
    $omitted = @($devices | Where-Object { $deviceChoices -notcontains $_ })
    foreach ($d in $omitted) {
        Write-Host ("  {0} is not shown because it has no configuration at all." -f $d)
    }
    $keepDevice = if ($deviceChoices -contains $current.Device) { $current.Device } else { $deviceChoices[0] }
    Write-Host ''
    $choice = Read-MenuChoice -Prompt "Select [1-$($deviceChoices.Count), Enter = keep $keepDevice, q = quit]" -Count $deviceChoices.Count
    if ($null -eq $choice) { Write-Host 'Cancelled; nothing changed.'; return }
    $Device = if ($choice -eq 0) { $keepDevice } else { $deviceChoices[$choice - 1] }

    # --- delivery mode: only if THIS device has both ---
    # Asked here, after the device, or not at all. Currently no device has both, so
    # nobody sees this question; restoring a standalone configuration brings it back
    # for that device without touching this script.
    $deviceHasSerial     = $devicesWithSerialUpdate -contains $Device
    $deviceHasStandalone = $devicesStandalone -contains $Device
    if ($deviceHasSerial -and $deviceHasStandalone) {
        $currentSerial = if ($current.SerialUpdateSupport) { 'Yes' } else { 'No' }
        Write-Host ''
        Write-Host "Delivery mode for ${Device}:"
        Write-Host ''
        Write-Host '  1) No   Standalone application; direct PKOB4 flash'
        Write-Host '  2) Yes  Resident bootloader and serial firmware update'
        Write-Host ''
        $choice = Read-MenuChoice -Prompt "Select [1-2, Enter = keep $currentSerial, q = quit]" -Count 2
        if ($null -eq $choice) { Write-Host 'Cancelled; nothing changed.'; return }
        $SerialUpdateSupport = if ($choice -eq 0) { $currentSerial } elseif ($choice -eq 1) { 'No' } else { 'Yes' }
    } else {
        $SerialUpdateSupport = if ($deviceHasSerial) { 'Yes' } else { 'No' }
        $modeText = if ($deviceHasSerial) {
            'resident bootloader + serial firmware update'
        } else {
            'standalone application, direct PKOB4 flash'
        }
        Write-Host ''
        Write-Host ("Delivery: $modeText -- the only mode $Device has a configuration for.")
    }

    # --- 2. application profile ---
    $available = Get-AvailableProfiles -Catalog $catalog -Configurations $configurations -Device $Device
    $shown = @(Get-AvailableProfiles -Catalog $catalog -Configurations $configurations -Device $Device -Tiers $visibleTiers)
    if ($shown.Count -eq 0) {
        throw "No application profile of tier $($visibleTiers -join '/') is available for $Device. Re-run with -All, or name one with -Profile."
    }

    $keepProfileEntry = $null
    if ($available.Name -contains $current.Profile) {
        $keepProfileEntry = Get-SonoraPreset -Catalog $catalog -Name $current.Profile
    }
    if ($null -eq $keepProfileEntry) { $keepProfileEntry = $shown[0] }

    Write-Host ''
    Write-Host 'Application profile:'
    Write-Host ''
    Write-ProfileCatalog -Profiles $shown -SelectedName $keepProfileEntry.Name
    Write-ProfileVisibilityHint -Shown $shown -Available $available
    Write-Host ''
    $choice = Read-MenuChoice `
        -Prompt "Select [1-$($shown.Count), Enter = keep $($keepProfileEntry.Display), q = quit]" `
        -Count $shown.Count
    if ($null -eq $choice) { Write-Host 'Cancelled; nothing changed.'; return }
    $Profile = if ($choice -eq 0) { $keepProfileEntry.Name } else { $shown[$choice - 1].Name }
}

# --------------------------------------------------- resolve the selection ----
if ([string]::IsNullOrWhiteSpace($SerialUpdateSupport)) {
    $SerialUpdateSupport = if ($current.SerialUpdateSupport) { 'Yes' } else { 'No' }
}
if ([string]::IsNullOrWhiteSpace($Device)) { $Device = $current.Device }
if ($devices -notcontains $Device) {
    throw "Unknown target device '$Device'. Available: $($devices -join ', ')"
}

$serialWanted = ($SerialUpdateSupport -eq 'Yes')

# An unsupported combination is an error, never a quiet downgrade: someone who
# asked for serial update and got a standalone image would find out on the board.
if ($serialWanted -and $serialUpdateDevices -notcontains $Device) {
    throw @"
Serial update support is not available for $Device.
The resident bootloader is built for: $($serialUpdateDevices -join ', ').
Choose one of those devices, or select serial update support = No.
"@
}
# The mirror, added 2026-08-15 with the deletion of the two AK512 standalone
# configurations. Without it, -SerialUpdateSupport No -Device dsPIC33AK512MPS512 fell
# through to Resolve-SonoraConfiguration and came back as a bare "No MPLAB
# configuration builds the Classic application standalone for ..." -- true, but it
# reads like a missing profile rather than a mode this device no longer has.
if (-not $serialWanted -and $devicesStandalone -notcontains $Device) {
    throw @"
There is no standalone configuration for $Device.
Serial update is how this device is both delivered and developed, so its standalone
configurations were deleted on 2026-08-15 (dsPIC33AK512 and dsPIC33AK512_ASRC on the
AK512, dsPIC33AK128 on the AK128 once that part got a resident bootloader too).
Select serial update support = Yes.
Devices that do have a standalone configuration: $(if ($devicesStandalone) { $devicesStandalone -join ', ' } else { 'none, currently' }).
"@
}

$profileEntry = if ([string]::IsNullOrWhiteSpace($Profile)) {
    Get-SonoraPreset -Catalog $catalog -Name $current.Profile
} else {
    Resolve-ProfileArgument -Catalog $catalog -Value $Profile
}

if (-not (Test-SonoraProfileAvailable -Configurations $configurations -Device $Device -ProfileEntry $profileEntry)) {
    $available = (Get-AvailableProfiles -Catalog $catalog -Configurations $configurations -Device $Device |
        ForEach-Object { $_.Display }) -join ', '
    throw "Application profile '$($profileEntry.Display)' is not available for $Device. Available: $available"
}

if ($serialWanted -and [string]::IsNullOrWhiteSpace($profileEntry.Artifact)) {
    throw @"
Application profile '$($profileEntry.Display)' has no 'artifact:' tag in
$($catalog.HeaderPath), so its serial update package could not be given a stable
file name. Add "artifact: <lower_case_token>" to its define comment, or select
serial update support = No.
"@
}

# The configuration is derived, and still written to the caches MPLAB X and bare
# `make` read so the IDE follows this selection.
$confEntry = Resolve-SonoraConfiguration `
    -Configurations $configurations -Device $Device -App $profileEntry.App `
    -SerialUpdate $serialWanted

# ------------------------------------------------------------------- apply ----
Set-SonoraSelection `
    -RepoRoot $repoRoot `
    -SerialUpdateSupport $serialWanted `
    -Device $Device `
    -ProfileName $profileEntry.Name
Set-SonoraActiveConfiguration -RepoRoot $repoRoot -ProjectDir $projectDir -ConfigurationEntry $confEntry
Set-SonoraSelectedPreset -RepoRoot $repoRoot -Configuration $confEntry.Name -Preset $profileEntry.Name

$selection = @{
    SerialUpdateSupport = $serialWanted
    Device              = $Device
    Profile             = $profileEntry.Name
}
Write-ActiveSelection -Selection $selection -Catalog $catalog -Configurations $configurations `
    -ShowInternal ([bool]$Internal)

if ($profileEntry.Tier -eq 'internal' -or $profileEntry.Tier -eq $script:SonoraPresetTierUnclassified) {
    Write-Host ''
    Write-Host 'WARNING:'
    if ($profileEntry.Tier -eq 'internal') {
        Write-Host 'This is an internal profile and is outside the regular smoke-test scope.'
    } else {
        Write-Host "This profile has no tier marker; treat it as internal until it is classified."
    }
}
Write-UnclassifiedProfileWarning -Unclassified $unclassified
