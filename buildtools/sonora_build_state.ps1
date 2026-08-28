# sonora_build_state.ps1 - shared build-selection state for the buildtools scripts.
#
# Dot-sourced by switch_config.ps1 / build.ps1 / flashauto.ps1. It owns the two
# "what am I building?" questions and reads their answers from authoritative
# sources instead of hardcoded lists, so this file cannot drift from the project:
#
#   1. WHICH MPLAB CONFIGURATION (= device + application)
#      Catalog : dspic33ak_audio_dsp.X/nbproject/configurations.xml
#                (conf order = the <defaultConf> index, targetDevice = the flash
#                 device token, SONORA_MPLAB_APP_ASRC=1 marks the ASRC app)
#      Active  : buildtools/active_build.json "configuration" (untracked)
#      Caches  : nbproject/private/configurations.xml <defaultConf>N (MPLAB X IDE)
#                nbproject/Makefile-impl.mk  DEFAULTCONF=<name>     (bare `make`)
#      The two cache files are generated, untracked, and byte-exact CRLF: only
#      the one value is rewritten, and a missing file is skipped, not an error.
#      They are NOT the authority. DEFAULTCONF used to be, and being tracked it
#      turned "I selected the ASRC configuration" into a commitable change that
#      repeatedly reached main (see f064cf6 "restore the Classic default
#      configuration"); the makefile generator rebuilds it from <defaultConf>
#      anyway, so it was only ever a derived value.
#
#   2. WHICH APP_BUILD VARIATION (preset) inside that application
#      Catalog : src/app/apps/app_build_config.h (names, app grouping, one-line
#                detail text, the tier marker, and the per-app compile-time
#                default)
#      Active  : buildtools/active_build.json (untracked, per configuration)
#      Stamp   : dspic33ak_audio_dsp.X/build/<conf>/.sonora_app_build records the
#                APP_BUILD the existing objects were compiled with, so a build
#                only has to clean when the variation actually changed.
#
# There is deliberately NO environment variable in this path: MPLABX_CONF as the
# source of truth was removed because it leaked across a shell session (one
# explicit -Configuration made every later unqualified build follow it).

$ErrorActionPreference = 'Stop'

$script:SonoraStateFileRelative = 'buildtools\active_build.json'
$script:SonoraBuildStampName = '.sonora_app_build'
$script:SonoraAppBuildHeaderRelative = 'src\app\apps\app_build_config.h'

# Preset tiers, in increasing "how hidden is it" order. The tier of each preset
# is owned by src/app/apps/app_build_config.h ("tier: <name>" trailing comment); this
# is only the vocabulary, so no preset name is ever listed on the PowerShell side.
$script:SonoraPresetTiers = @('normal', 'advanced', 'internal')
# A preset whose define carries no recognised marker. Deliberately NOT 'normal':
# it stays out of every list until someone classifies it in the header.
$script:SonoraPresetTierUnclassified = 'unclassified'

function Resolve-SonoraRepoRoot {
    param(
        [string]$RequestedRoot
    )

    $resolvedRoot = (Resolve-Path -LiteralPath $RequestedRoot).Path

    # Tolerate being pointed at the MPLAB project dir itself.
    if ((Split-Path -Leaf $resolvedRoot) -like '*.X' -and
        (Test-Path -LiteralPath (Join-Path $resolvedRoot 'nbproject'))) {
        return (Split-Path -Parent $resolvedRoot)
    }

    return $resolvedRoot
}

function Get-SonoraBootImageManifest {
    <#
      src/boot/boot_image.psd1 -- the single authority on what the resident boot image is
      built from, which device it targets and what its project is called. Read here
      rather than duplicated, so this file cannot drift from the image it describes.
    #>
    param(
        [string]$RepoRoot
    )

    $manifest = Join-Path $RepoRoot 'src\boot\boot_image.psd1'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "The boot image manifest is missing: $manifest"
    }
    return Import-PowerShellDataFile -LiteralPath $manifest
}

function Resolve-SonoraProjectDir {
    param(
        [string]$RepoRoot,
        [string]$RequestedProjectDir
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedProjectDir)) {
        if ([System.IO.Path]::IsPathRooted($RequestedProjectDir)) {
            return (Resolve-Path -LiteralPath $RequestedProjectDir).Path
        }
        return (Resolve-Path -LiteralPath (Join-Path $RepoRoot $RequestedProjectDir)).Path
    }

    # The repository holds two MPLAB X projects now: the application and the generated
    # resident bootloader. The bootloader is not a candidate -- it has its own build
    # script (build_resident_bootloader.ps1) and its project exists only for debugging.
    # Named from src/boot/boot_image.psd1, not spelled out here, so renaming it is one edit.
    $bootProject = (Get-SonoraBootImageManifest -RepoRoot $RepoRoot).ProjectName + '.X'
    $projects = @(Get-ChildItem -LiteralPath $RepoRoot -Directory -Filter '*.X' |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'nbproject') } |
        Where-Object { $_.Name -ne $bootProject })

    if ($projects.Count -eq 0) {
        throw "No MPLAB X project directory (*.X with nbproject) found under: $RepoRoot"
    }
    if ($projects.Count -gt 1) {
        $names = ($projects | ForEach-Object { $_.Name }) -join ', '
        throw "Multiple MPLAB X project directories found: $names. Specify -ProjectDir."
    }

    return $projects[0].FullName
}

function Get-SonoraConfigurations {
    <#
      Reads the project's own configuration catalog. Index is the position in
      <confs>, which is exactly what private/configurations.xml <defaultConf>
      stores. App is derived from the SONORA_MPLAB_APP_ASRC macro that the ASRC
      configuration sets (see src/app/apps/app_build_config.h).
    #>
    param(
        [string]$ProjectDir
    )

    $configurationsXml = Join-Path $ProjectDir 'nbproject\configurations.xml'
    if (-not (Test-Path -LiteralPath $configurationsXml)) {
        throw "MPLAB configuration catalog not found: $configurationsXml"
    }

    [xml]$xml = Get-Content -LiteralPath $configurationsXml -Raw
    $confNodes = @($xml.configurationDescriptor.confs.conf)
    if ($confNodes.Count -eq 0) {
        throw "No <conf> entries found in $configurationsXml"
    }

    $result = @()
    for ($i = 0; $i -lt $confNodes.Count; $i++) {
        $node = $confNodes[$i]
        $macros = @($node.SelectNodes(".//property[@key='preprocessor-macros']") |
            ForEach-Object { $_.GetAttribute('value') })
        $isAsrc = @($macros | Where-Object { $_ -match 'SONORA_MPLAB_APP_ASRC' }).Count -gt 0

        # Does this configuration build the serial-update layout? The MPLAB
        # configuration owns the memory layout (its own linker script and IVT
        # placement), so the delivery mode is a property OF the configuration, and
        # a (device, application) pair alone does not identify one -- the serial
        # update attribute is the third axis. Read from the macro the configuration
        # sets, never from its name, so renaming one changes nothing here.
        $isSerialUpdate = @($macros | Where-Object { $_ -match 'SONORA_MPLAB_SERIAL_UPDATE' }).Count -gt 0

        $result += [pscustomobject]@{
            Name           = $node.name
            Index          = $i
            Device         = $node.toolsSet.targetDevice
            App            = if ($isAsrc) { 'Asrc' } else { 'Classic' }
            IsSerialUpdate = $isSerialUpdate
        }
    }

    return $result
}

function Get-SonoraConfiguration {
    param(
        [object[]]$Configurations,
        [string]$Name
    )

    $match = @($Configurations | Where-Object { $_.Name -eq $Name })
    if ($match.Count -ne 1) {
        $available = ($Configurations | ForEach-Object { $_.Name }) -join ', '
        throw "Unknown MPLAB configuration '$Name'. Available: $available"
    }

    return $match[0]
}

function Get-SonoraActiveConfiguration {
    <#
      Resolution order, most authoritative first:

        1. buildtools/active_build.json "configuration"  - the untracked selection
        2. nbproject/Makefile-impl.mk  DEFAULTCONF=      - generated cache; also
           what a clone written by an older switch_config.ps1 still carries, so
           reading it keeps that selection working without a migration step
        3. the project's first configuration               - fresh clone, or any
           tree that carries no recorded selection

      A name that is not in the project's catalog is skipped rather than trusted,
      so a stale state file or a dropped configuration falls through to (3)
      instead of failing.
    #>
    param(
        [string]$RepoRoot,
        [string]$ProjectDir,
        [object[]]$Configurations
    )

    if (-not [string]::IsNullOrWhiteSpace($RepoRoot)) {
        $selected = (Get-SonoraLocalState -RepoRoot $RepoRoot).Configuration
        if (-not [string]::IsNullOrWhiteSpace($selected) -and
            @($Configurations | Where-Object { $_.Name -eq $selected }).Count -eq 1) {
            return $selected
        }
    }

    $implMakefile = Join-Path $ProjectDir 'nbproject\Makefile-impl.mk'
    if (Test-Path -LiteralPath $implMakefile) {
        $text = [System.IO.File]::ReadAllText($implMakefile, [System.Text.Encoding]::ASCII)
        $match = [regex]::Match($text, '(?m)^DEFAULTCONF=([^\s\r\n]+)')
        if ($match.Success) {
            $name = $match.Groups[1].Value
            if (@($Configurations | Where-Object { $_.Name -eq $name }).Count -eq 1) {
                return $name
            }
        }
    }

    return $Configurations[0].Name
}

function Update-SonoraAsciiFileExact {
    <#
      Byte-level single-value rewrite. Makefile-impl.mk and
      private/configurations.xml are CRLF byte-exact (.gitattributes -text), so
      Get-Content/Set-Content must not be used: they would renormalize every
      line and turn a one-value switch into a whole-file EOL diff.
    #>
    param(
        [string]$Path,
        [string]$Pattern,
        [scriptblock]$Replacement
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "File not found: $Path"
    }

    $encoding = [System.Text.Encoding]::GetEncoding(28591)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = $encoding.GetString($bytes)
    $regex = [System.Text.RegularExpressions.Regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Multiline)
    $found = $regex.Matches($text)
    if ($found.Count -ne 1) {
        throw "Expected exactly one match in $Path, found $($found.Count)"
    }

    $newText = $regex.Replace(
        $text,
        {
            param($match)
            & $Replacement $match
        },
        1)

    if ($newText -ne $text) {
        [System.IO.File]::WriteAllBytes($Path, $encoding.GetBytes($newText))
    }
}

function Update-SonoraDefaultConfCache {
    <#
      Point Makefile-impl.mk's DEFAULTCONF at the given configuration.

      The makefile generator rebuilds that value from private/configurations.xml,
      which may not exist yet (fresh clone, MPLAB X never opened) - in which case
      the generator picks the project's first configuration and the cache would
      disagree with the selection until the IDE runs once. The buildtools scripts
      read the selection itself, so only a bare `make` in the project directory
      would notice, but a cache that silently means something else is a trap.
      Callers re-apply this after generating makefiles. Absent file = nothing to
      cache, which is not an error.
    #>
    param(
        [string]$ProjectDir,
        [string]$Configuration
    )

    $implMakefile = Join-Path $ProjectDir 'nbproject\Makefile-impl.mk'
    if (-not (Test-Path -LiteralPath $implMakefile)) {
        return
    }

    Update-SonoraAsciiFileExact `
        -Path $implMakefile `
        -Pattern '^(DEFAULTCONF=)[^\r\n]*' `
        -Replacement { param($m) $m.Groups[1].Value + $Configuration }
}

function Set-SonoraActiveConfiguration {
    <#
      Writes the selection to the untracked state file, then updates the two
      generated files that cache it so anything reading them agrees:

        private/configurations.xml  <defaultConf>N  - what MPLAB X IDE reads
        Makefile-impl.mk            DEFAULTCONF=    - what a bare `make` in the
                                                      project directory reads

      Both are generated and untracked, so writing them cannot reach a commit,
      and both may legitimately be absent (fresh clone, IDE never opened): each
      write is skipped rather than failing. They are caches, not the authority -
      the makefile generator itself rebuilds DEFAULTCONF from <defaultConf>.
    #>
    param(
        [string]$RepoRoot,
        [string]$ProjectDir,
        [object]$ConfigurationEntry
    )

    $name = $ConfigurationEntry.Name
    $index = [string]$ConfigurationEntry.Index

    $state = Get-SonoraLocalState -RepoRoot $RepoRoot
    $state.Configuration = $name
    Set-SonoraLocalState -RepoRoot $RepoRoot -State $state

    $implMakefile = Join-Path $ProjectDir 'nbproject\Makefile-impl.mk'
    $privateConfig = Join-Path $ProjectDir 'nbproject\private\configurations.xml'

    # Absent until the makefile generator has run at least once in this clone.
    Update-SonoraDefaultConfCache -ProjectDir $ProjectDir -Configuration $name

    # Untracked IDE state: absent until MPLAB X has opened the project once.
    if (Test-Path -LiteralPath $privateConfig) {
        Update-SonoraAsciiFileExact `
            -Path $privateConfig `
            -Pattern '(<defaultConf>)\d+(</defaultConf>)' `
            -Replacement { param($m) $m.Groups[1].Value + $index + $m.Groups[2].Value }
    }
}

function Get-SonoraPresetCatalog {
    <#
      Parses src/app/apps/app_build_config.h - the single place that owns the
      APP_BUILD variation set - and returns:
        Presets  : ordered list of @{ Name; Value; App; Tier; Detail; Artifact; Display }
        Defaults : @{ Classic = <name>; Asrc = <name> }  (the header's #ifndef APP_BUILD)
      Adding a variation to the header is therefore enough; no list here to update.

      All per-profile metadata lives in the trailing comment on the numeric
      define, ';'-separated:  /* tier: normal; artifact: classic1; display: Classic 1 */

      Tier comes from "tier: normal|advanced|internal". A define without one gets
      Tier = 'unclassified', which no visibility set includes, so a forgotten
      marker shows up as a preset that has gone missing from the menus rather
      than as a new normal preset.

      Artifact is the stable filename token used for serial update packages, and
      Display is the name the menus show. Both are declared in the header rather
      than derived here: a script that abbreviated APP_BUILD names itself would
      rename files whenever a display name changed.
    #>
    param(
        [string]$RepoRoot
    )

    $headerPath = Join-Path $RepoRoot $script:SonoraAppBuildHeaderRelative
    if (-not (Test-Path -LiteralPath $headerPath)) {
        throw "APP_BUILD catalog header not found: $headerPath"
    }

    $text = [System.IO.File]::ReadAllText($headerPath)

    # 1) numeric variation defines, with their metadata comment:
    #      #define APP_BUILD_FOO (7)  /* tier: normal; artifact: foo; display: Foo */
    #    Fields are ';'-separated so a value may contain spaces (display names do).
    $values = [ordered]@{}
    $tiers = @{}
    $artifacts = @{}
    $displays = @{}
    foreach ($m in [regex]::Matches($text, '(?m)^\s*#define\s+(APP_BUILD_[A-Z0-9_]+)\s+\((\d+)\)[^\S\r\n]*(?:/\*(?<comment>[^\r\n]*?)\*/)?')) {
        $name = $m.Groups[1].Value
        $values[$name] = [int]$m.Groups[2].Value
        $comment = $m.Groups['comment'].Value

        $tier = $script:SonoraPresetTierUnclassified
        $tierMatch = [regex]::Match($comment, 'tier\s*:\s*([A-Za-z]+)')
        if ($tierMatch.Success) {
            $declared = $tierMatch.Groups[1].Value.ToLowerInvariant()
            if ($script:SonoraPresetTiers -contains $declared) {
                $tier = $declared
            } else {
                throw "Unknown APP_BUILD tier '$($tierMatch.Groups[1].Value)' on $name in $headerPath. Expected one of: $($script:SonoraPresetTiers -join ', ')."
            }
        }
        $tiers[$name] = $tier

        # artifact: the stable filename token for a serial update package. Kept
        # here, not derived in PowerShell, so renaming the display name never
        # renames files and no script has to guess an abbreviation.
        $artifactMatch = [regex]::Match($comment, 'artifact\s*:\s*([^;]+)')
        $artifact = ''
        if ($artifactMatch.Success) {
            $artifact = $artifactMatch.Groups[1].Value.Trim()
            if ($artifact -notmatch '^[a-z0-9_]+$') {
                throw "Invalid artifact tag '$artifact' on $name in $headerPath. Use lower-case letters, digits and underscore only."
            }
        }
        $artifacts[$name] = $artifact

        # display: what the menus call this profile. Falls back to the define name
        # so an unlabelled profile is still selectable, just less friendly.
        $displayMatch = [regex]::Match($comment, 'display\s*:\s*([^;]+)')
        $displays[$name] = if ($displayMatch.Success) { $displayMatch.Groups[1].Value.Trim() } else { $name }
    }
    if ($values.Count -eq 0) {
        throw "No '#define APP_BUILD_* (n)' variations found in $headerPath"
    }

    # 2) app grouping: the #if/#elif ranges that set SONORA_APP
    $appOfName = @{}
    $rangeMatches = [regex]::Matches(
        $text,
        '\(APP_BUILD\s*>=\s*(APP_BUILD_\w+)\)\s*&&\s*\(APP_BUILD\s*<=\s*(APP_BUILD_\w+)\)[\s\S]*?#define\s+SONORA_APP\s+(SONORA_APP_\w+)')
    if ($rangeMatches.Count -eq 0) {
        throw "Could not determine the APP_BUILD -> application ranges in $headerPath"
    }
    foreach ($m in $rangeMatches) {
        $lowName = $m.Groups[1].Value
        $highName = $m.Groups[2].Value
        $appToken = $m.Groups[3].Value
        if (-not $values.Contains($lowName) -or -not $values.Contains($highName)) {
            throw "Range bound not defined in $headerPath ($lowName .. $highName)"
        }
        $app = switch ($appToken) {
            'SONORA_APP_ASRC' { 'Asrc' }
            'SONORA_APP_CLASSIC_AUDIO_DEMO' { 'Classic' }
            default { throw "Unknown application token '$appToken' in $headerPath" }
        }
        $low = $values[$lowName]
        $high = $values[$highName]
        foreach ($name in $values.Keys) {
            $value = $values[$name]
            if ($value -ge $low -and $value -le $high) {
                $appOfName[$name] = $app
            }
        }
    }

    # 3) one-line description, taken from the boot-banner APP_BUILD_NAME /
    #    APP_BUILD_DETAIL pairs. A variation may define APP_BUILD_DETAIL inside an
    #    #if/#else (e.g. an experimental kernel switch), so scan the whole segment
    #    up to the next APP_BUILD_NAME and keep the LAST detail = the #else, which
    #    is the branch a normal build compiles.
    $details = @{}
    $nameMatches = @([regex]::Matches($text, '#define\s+APP_BUILD_NAME\s+"(APP_BUILD_\w+)"'))
    for ($n = 0; $n -lt $nameMatches.Count; $n++) {
        $start = $nameMatches[$n].Index + $nameMatches[$n].Length
        $end = if ($n + 1 -lt $nameMatches.Count) { $nameMatches[$n + 1].Index } else { $text.Length }
        $segment = $text.Substring($start, $end - $start)
        $detailMatches = @([regex]::Matches($segment, '#define\s+APP_BUILD_DETAIL\s+"([^"]*)"'))
        if ($detailMatches.Count -gt 0) {
            $details[$nameMatches[$n].Groups[1].Value] = $detailMatches[$detailMatches.Count - 1].Groups[1].Value
        }
    }

    # 4) per-application compile-time default (#ifndef APP_BUILD block)
    $defaultMatch = [regex]::Match(
        $text,
        '#ifndef\s+APP_BUILD[\s\S]*?defined\(SONORA_MPLAB_APP_ASRC\)\s*\r?\n\s*#define\s+APP_BUILD\s+\((APP_BUILD_\w+)\)\s*\r?\n\s*#else\s*\r?\n\s*#define\s+APP_BUILD\s+\((APP_BUILD_\w+)\)')
    if (-not $defaultMatch.Success) {
        throw "Could not determine the per-application default APP_BUILD in $headerPath"
    }

    $presets = @()
    foreach ($name in $values.Keys) {
        if (-not $appOfName.ContainsKey($name)) {
            continue   # not inside any application range (should not happen)
        }
        $presets += [pscustomobject]@{
            Name     = $name
            Value    = $values[$name]
            App      = $appOfName[$name]
            Tier     = $tiers[$name]
            Detail   = if ($details.ContainsKey($name)) { $details[$name] } else { '' }
            Artifact = $artifacts[$name]
            Display  = $displays[$name]
        }
    }

    return [pscustomobject]@{
        Presets  = @($presets | Sort-Object -Property Value)
        Defaults = @{
            Asrc    = $defaultMatch.Groups[1].Value
            Classic = $defaultMatch.Groups[2].Value
        }
        HeaderPath = $headerPath
    }
}

function Get-SonoraPresetsForApp {
    <#
      Presets of one application, optionally narrowed to a set of tiers. Callers
      pass the set from Get-SonoraVisibleTiers; omitting -Tiers means "every
      tier", including unclassified ones (used for diagnostics, not for menus).
    #>
    param(
        [object]$Catalog,
        [string]$App,
        [string[]]$Tiers
    )

    $presets = @($Catalog.Presets | Where-Object { $_.App -eq $App })
    if ($null -eq $Tiers) {
        return $presets
    }

    return @($presets | Where-Object { $Tiers -contains $_.Tier })
}

function Get-SonoraVisibleTiers {
    <#
      The tiers a preset list shows: normal only, +advanced, or everything
      classified. Never includes 'unclassified' - a preset with a missing or
      unreadable tier marker is reported separately (Get-SonoraUnclassifiedPresets)
      instead of quietly joining a list.
    #>
    param(
        [switch]$Advanced,
        [switch]$All
    )

    if ($All) { return @($script:SonoraPresetTiers) }
    if ($Advanced) { return @('normal', 'advanced') }

    return @('normal')
}

function Get-SonoraUnclassifiedPresets {
    param(
        [object]$Catalog
    )

    return @($Catalog.Presets | Where-Object { $_.Tier -eq $script:SonoraPresetTierUnclassified })
}

function Get-SonoraPreset {
    param(
        [object]$Catalog,
        [string]$Name
    )

    $match = @($Catalog.Presets | Where-Object { $_.Name -eq $Name })
    if ($match.Count -ne 1) {
        return $null
    }

    return $match[0]
}

function Get-SonoraDefaultPreset {
    param(
        [object]$Catalog,
        [string]$App
    )

    return $Catalog.Defaults[$App]
}

function Get-SonoraStateFilePath {
    param(
        [string]$RepoRoot
    )

    return (Join-Path $RepoRoot $script:SonoraStateFileRelative)
}

function Get-SonoraLocalState {
    <#
      The whole untracked selection file, as
        @{ Configuration = <name or $null>; Presets = @{ <conf> = <preset> } }
      An absent or unreadable file yields empty values, which every caller already
      treats as "nothing selected yet".
    #>
    param(
        [string]$RepoRoot
    )

    $empty = @{
        Configuration = $null
        Presets = @{}
        SerialUpdateSupport = $null
        Device = $null
        Profile = $null
    }

    $statePath = Get-SonoraStateFilePath -RepoRoot $RepoRoot
    if (-not (Test-Path -LiteralPath $statePath)) {
        return $empty
    }

    try {
        $json = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    } catch {
        Write-Host "WARNING: ignoring unreadable $statePath ($($_.Exception.Message))"
        return $empty
    }

    $map = @{}
    if ($null -ne $json -and $null -ne $json.presets) {
        foreach ($property in $json.presets.PSObject.Properties) {
            $map[$property.Name] = [string]$property.Value
        }
    }

    $configuration = $null
    if ($null -ne $json -and -not [string]::IsNullOrWhiteSpace($json.configuration)) {
        $configuration = [string]$json.configuration
    }

    # New-model keys. Absent in a file written before this model existed, which
    # Get-SonoraSelection migrates from `configuration` + `presets`.
    $serialUpdate = $null
    if ($null -ne $json -and $null -ne $json.serial_update_support) {
        $serialUpdate = [bool]$json.serial_update_support
    }
    $device = $null
    if ($null -ne $json -and -not [string]::IsNullOrWhiteSpace($json.device)) {
        $device = [string]$json.device
    }
    $profileName = $null
    if ($null -ne $json -and -not [string]::IsNullOrWhiteSpace($json.application_profile)) {
        $profileName = [string]$json.application_profile
    }

    return @{
        Configuration = $configuration
        Presets = $map
        SerialUpdateSupport = $serialUpdate
        Device = $device
        Profile = $profileName
    }
}

function Set-SonoraLocalState {
    <#
      Rewrites the untracked selection file from a full state object. Both values
      are optional: a state file may name a configuration before any preset has
      been chosen for it, and vice versa.
    #>
    param(
        [string]$RepoRoot,
        [hashtable]$State
    )

    $presets = [ordered]@{}
    foreach ($key in (@($State.Presets.Keys) | Sort-Object)) {
        $presets[$key] = $State.Presets[$key]
    }

    $payload = [ordered]@{
        comment = 'Local build selection written by buildtools/switch_config.ps1. The authority is serial_update_support + device + application_profile; the MPLAB configuration is derived from the device and the profile''s application. Untracked per-developer state - delete it to fall back to serial update off, the project''s first device, and that application''s compile-time default profile.'
    }

    # The three authoritative values.
    if ($null -ne $State.SerialUpdateSupport) {
        $payload.serial_update_support = [bool]$State.SerialUpdateSupport
    }
    if (-not [string]::IsNullOrWhiteSpace($State.Device)) {
        $payload.device = [string]$State.Device
    }
    if (-not [string]::IsNullOrWhiteSpace($State.Profile)) {
        $payload.application_profile = [string]$State.Profile
    }

    # Derived, kept only so a tool still on the older path (and anyone reading this
    # file) can see which configuration the selection resolved to. Never read back
    # as authority: Get-SonoraSelection re-resolves from device + profile.
    if (-not [string]::IsNullOrWhiteSpace($State.Configuration)) {
        $payload.resolved_configuration = [string]$State.Configuration
        $payload.configuration = [string]$State.Configuration
    }
    $payload.presets = $presets

    $statePath = Get-SonoraStateFilePath -RepoRoot $RepoRoot
    $json = ($payload | ConvertTo-Json -Depth 4)
    $json = $json -replace '(?<!\r)\n', "`r`n"
    [System.IO.File]::WriteAllText($statePath, $json + "`r`n", [System.Text.UTF8Encoding]::new($false))
}

# ---------------------------------------------------------------------------
# The user-facing selection model.
#
# A user chooses three things -- serial update support, target device and
# application profile -- and never an MPLAB configuration: that is derived from
# the device plus the profile's application. See Resolve-SonoraConfiguration.
# ---------------------------------------------------------------------------

function Get-SonoraSerialUpdateDevices {
    <#
      EVERY device the resident bootloader is built for, read out of the boot image
      manifest instead of being listed here, so the two cannot disagree. Returned in
      the same 'dsPIC33AK...' spelling the project uses for devices.

      Plural since 2026-08-15, when the manifest grew a second device. It was singular
      (Get-SonoraSerialUpdateDevice) and read the manifest's top-level Device field --
      which the two-device manifest does not have, so this would have thrown "does not
      name a Device" rather than quietly answering with one of them. The callers compare
      with -contains now; a caller left on -eq would have turned serial update off for
      whichever device was not first.

      This used to scrape -mcpu= out of build_resident_bootloader.ps1. Reorg step 5
      moved that flag's value into src/boot/boot_image.psd1 and the scrape stopped
      matching -- which failed the DELIVERY build only, with "Could not read the
      resident bootloader's target device", long after the bootloader itself had built
      fine on its own. Reading the authority directly cannot go stale that way.
    #>
    param(
        [string]$RepoRoot
    )

    $manifest = Get-SonoraBootImageManifest -RepoRoot $RepoRoot
    $devices = @(@($manifest.Devices.Keys) | Sort-Object)
    if ($devices.Count -eq 0) {
        throw "src/boot/boot_image.psd1 lists no Devices for the resident bootloader."
    }

    return @($devices | ForEach-Object { 'dsPIC' + $_ })
}

function Get-SonoraSerialUpdateDeviceEntry {
    <#
      What the boot image manifest says about ONE device, looked up in the
      'dsPIC33AK...' spelling the project uses. Returns that device's sub-hash with a
      'Device' key added holding the manifest's own (prefix-less) key, so a caller that
      has to hand the device to build_resident_bootloader.ps1 does not re-derive it.

      Why here: several delivery-side facts are per device and all of them are already
      written down in the manifest -- the resident configuration name that names
      dist/<conf>/, and the serial-update linker script the application links with.
      A caller that hard-codes either builds one device's bootloader into the other
      device's factory image, or checks the wrong .gld's program ORIGIN, and neither
      failure looks like a failure.
    #>
    param(
        [string]$RepoRoot,
        [string]$Device
    )

    $manifest = Get-SonoraBootImageManifest -RepoRoot $RepoRoot
    $key = $Device -replace '^dsPIC', ''
    if (-not $manifest.Devices.ContainsKey($key)) {
        $known = @(@($manifest.Devices.Keys) | Sort-Object | ForEach-Object { 'dsPIC' + $_ }) -join ', '
        throw ("src/boot/boot_image.psd1 has no resident boot image for '$Device'. " +
               "It knows: $known")
    }

    $entry = @{ Device = $key }
    foreach ($k in @($manifest.Devices[$key].Keys)) { $entry[$k] = $manifest.Devices[$key][$k] }
    return $entry
}

function Get-SonoraDevices {
    <#
      Every device this project can build for, in configuration order, derived
      from the configuration catalog so adding a device needs no change here.
    #>
    param(
        [object[]]$Configurations
    )

    return @($Configurations |
        ForEach-Object { $_.Device } |
        Select-Object -Unique)
}

function Get-SonoraDevicesForMode {
    <#
      The devices that can actually be built in one mode -- standalone or with serial
      update support -- derived from the configuration catalog.

      Needed since 2026-08-15, when the two AK512 standalone configurations were
      deleted: before that every device could be built standalone, so "which devices
      offer this mode" had one answer and nobody had to ask. Both devices are
      delivery-only since later the same day, so this returns an EMPTY set for
      standalone -- the mode and the device are no longer independent choices, and a
      menu that pretends otherwise walks the user
      into Resolve-SonoraConfiguration's "No MPLAB configuration builds ..." throw --
      correct, but arriving three questions too late to be useful.

      Derived, not tabulated, so restoring a standalone AK512 configuration is enough
      to make it selectable again.
    #>
    param(
        [object[]]$Configurations,
        [bool]$SerialUpdate
    )

    return @($Configurations |
        Where-Object { $_.IsSerialUpdate -eq $SerialUpdate } |
        ForEach-Object { $_.Device } |
        Select-Object -Unique)
}

function Resolve-SonoraConfiguration {
    <#
      The MPLAB configuration for a (device, application) pair, found by searching
      the catalog rather than from a name table: nothing here needs updating when
      a configuration is added or renamed. Ambiguity is an error, not a first-match
      guess, because silently picking one of two would be unexplainable later.
    #>
    param(
        [object[]]$Configurations,
        [string]$Device,
        [string]$App,
        [bool]$SerialUpdate
    )

    $found = @($Configurations | Where-Object {
        $_.Device -eq $Device -and $_.App -eq $App -and $_.IsSerialUpdate -eq $SerialUpdate })
    $mode = if ($SerialUpdate) { 'with serial update support' } else { 'standalone' }
    if ($found.Count -eq 0) {
        throw "No MPLAB configuration builds the $App application $mode for $Device."
    }
    if ($found.Count -gt 1) {
        $names = ($found | ForEach-Object { $_.Name }) -join ', '
        throw "Ambiguous: $($found.Count) configurations build the $App application $mode for $Device ($names). The project needs exactly one."
    }

    return $found[0]
}

function Get-SonoraSelection {
    <#
      The active selection, resolved from buildtools/active_build.json:
        @{ SerialUpdateSupport = [bool]; Device = <name>; Profile = <APP_BUILD_*> }

      Missing or unusable values fall back to a working default rather than
      failing, so a fresh clone can build without running switch_config first:
      serial update off (an existing user's board keeps behaving as before), the
      project's first device, and that application's compile-time default profile.

      Also migrates the older shape, which stored an MPLAB configuration plus a
      per-configuration preset map. The configuration only contributes its device
      -- it is a derived value in this model and is never read back as authority.
    #>
    param(
        [string]$RepoRoot,
        [object[]]$Configurations,
        [object]$Catalog
    )

    $state = Get-SonoraLocalState -RepoRoot $RepoRoot
    $devices = Get-SonoraDevices -Configurations $Configurations

    # --- device ---
    $device = $null
    if (-not [string]::IsNullOrWhiteSpace($state.Device) -and $devices -contains $state.Device) {
        $device = $state.Device
    } elseif (-not [string]::IsNullOrWhiteSpace($state.Configuration)) {
        # Migration: carry the old selection across by the device it implied.
        $old = @($Configurations | Where-Object { $_.Name -eq $state.Configuration })
        if ($old.Count -eq 1) { $device = $old[0].Device }
    }
    if ($null -eq $device) { $device = $devices[0] }

    # --- profile ---
    $profileName = $null
    $candidate = $state.Profile
    if ([string]::IsNullOrWhiteSpace($candidate) -and
        -not [string]::IsNullOrWhiteSpace($state.Configuration) -and
        $state.Presets.ContainsKey($state.Configuration)) {
        # Migration: the preset chosen for the old active configuration.
        $candidate = $state.Presets[$state.Configuration]
    }
    if (-not [string]::IsNullOrWhiteSpace($candidate)) {
        $entry = Get-SonoraPreset -Catalog $Catalog -Name $candidate
        if ($null -ne $entry -and
            (Test-SonoraProfileAvailable -Configurations $Configurations -Device $device -ProfileEntry $entry)) {
            $profileName = $entry.Name
        }
    }
    if ($null -eq $profileName) {
        # This device's first application, at its compile-time default profile.
        $app = @($Configurations |
            Where-Object { $_.Device -eq $device })[0].App
        $profileName = Get-SonoraDefaultPreset -Catalog $Catalog -App $app
    }

    # --- serial update support ---
    # Only ever true where it is actually implemented; a stored true for another
    # device is dropped rather than carried into an impossible build.
    $serial = ($state.SerialUpdateSupport -eq $true) -and
              ((Get-SonoraSerialUpdateDevices -RepoRoot $RepoRoot) -contains $device)
    # ... and only ever FALSE where standalone is implemented. Added 2026-08-15 with the
    # deletion of the two AK512 standalone configurations: before that every device could
    # build standalone, so falling back to false was always safe. It stopped being safe
    # the moment AK512 became delivery-only -- a fresh clone has no active_build.json, so
    # it took the first device (AK512) with serial=false and produced a combination the
    # project has no configuration for, i.e. build.ps1 failed on a clean checkout with
    # nothing wrong. Note this is not the mirror of the clause above and must not be
    # written as one: the question is what this DEVICE has a configuration for, which
    # only the catalog knows, not which single device carries the bootloader.
    if (-not $serial -and
        (Get-SonoraDevicesForMode -Configurations $Configurations -SerialUpdate $false) -notcontains $device) {
        $serial = $true
    }

    return @{
        SerialUpdateSupport = $serial
        Device              = $device
        Profile             = $profileName
    }
}

function Test-SonoraProfileAvailable {
    <#
      Can this device build this profile? True when the project has a
      configuration for the profile's application on that device.
    #>
    param(
        [object[]]$Configurations,
        [string]$Device,
        [object]$ProfileEntry
    )

    return @($Configurations |
        Where-Object { $_.Device -eq $Device -and $_.App -eq $ProfileEntry.App }).Count -ge 1
}

function Set-SonoraSelection {
    param(
        [string]$RepoRoot,
        [bool]$SerialUpdateSupport,
        [string]$Device,
        [string]$ProfileName
    )

    $state = Get-SonoraLocalState -RepoRoot $RepoRoot
    $state.SerialUpdateSupport = $SerialUpdateSupport
    $state.Device = $Device
    $state.Profile = $ProfileName
    Set-SonoraLocalState -RepoRoot $RepoRoot -State $state
}

function Get-SonoraSelectedPresets {
    param(
        [string]$RepoRoot
    )

    return (Get-SonoraLocalState -RepoRoot $RepoRoot).Presets
}

function Get-SonoraSelectedPreset {
    param(
        [string]$RepoRoot,
        [string]$Configuration
    )

    $map = Get-SonoraSelectedPresets -RepoRoot $RepoRoot
    if ($map.ContainsKey($Configuration)) {
        return $map[$Configuration]
    }

    return $null
}

function Set-SonoraSelectedPreset {
    param(
        [string]$RepoRoot,
        [string]$Configuration,
        [string]$Preset
    )

    $state = Get-SonoraLocalState -RepoRoot $RepoRoot
    $state.Presets[$Configuration] = $Preset
    Set-SonoraLocalState -RepoRoot $RepoRoot -State $state
}

function Get-SonoraConfigurationBuildDir {
    param(
        [string]$ProjectDir,
        [string]$Configuration
    )

    return (Join-Path $ProjectDir "build\$Configuration")
}

function Get-SonoraBuiltPreset {
    <#
      What the objects currently in build/<conf>/ were compiled with:
        <name>     an explicit -DAPP_BUILD=<name> build
        (default)  built without -DAPP_BUILD (configuration's own default)
        $null      nothing built, or built by something that leaves no stamp
                   (MPLAB X IDE), which callers must treat as "unknown".
    #>
    param(
        [string]$ProjectDir,
        [string]$Configuration
    )

    $stampPath = Join-Path (Get-SonoraConfigurationBuildDir -ProjectDir $ProjectDir -Configuration $Configuration) $script:SonoraBuildStampName
    if (-not (Test-Path -LiteralPath $stampPath)) {
        return $null
    }

    $value = ([System.IO.File]::ReadAllText($stampPath)).Trim()
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $null
    }

    return $value
}

function Set-SonoraBuiltPreset {
    param(
        [string]$ProjectDir,
        [string]$Configuration,
        [string]$Value
    )

    $buildDir = Get-SonoraConfigurationBuildDir -ProjectDir $ProjectDir -Configuration $Configuration
    if (-not (Test-Path -LiteralPath $buildDir)) {
        # No build tree (e.g. a build that produced nothing): nothing to stamp.
        return
    }

    $stampPath = Join-Path $buildDir $script:SonoraBuildStampName
    [System.IO.File]::WriteAllText($stampPath, $Value + "`r`n", [System.Text.UTF8Encoding]::new($false))
}

function Get-SonoraObjectFiles {
    param(
        [string]$Directory
    )

    if (-not (Test-Path -LiteralPath $Directory)) {
        return @()
    }

    # Extension test rather than -Filter '*.o': Windows wildcard matching also
    # accepts the sibling '<name>.o.d' dependency files.
    return @(Get-ChildItem -LiteralPath $Directory -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -eq '.o' })
}

function Test-SonoraConfigurationHasObjects {
    param(
        [string]$ProjectDir,
        [string]$Configuration
    )

    $buildDir = Get-SonoraConfigurationBuildDir -ProjectDir $ProjectDir -Configuration $Configuration
    $firstObject = Get-SonoraObjectFiles -Directory $buildDir | Select-Object -First 1

    return ($null -ne $firstObject)
}

function Get-SonoraOrphanObjects {
    <#
      Objects sitting in the build directory that the generated makefile does not
      list any more: what a file removal/rename, a branch switch, or a change of
      the configuration's source manifest leaves behind. Make neither rebuilds nor
      deletes them.

      They do NOT reach the linker - the link recipe passes
      ${OBJECTFILES_QUOTED_IF_SPACED}, i.e. only what the current makefile lists.
      So this is a divergence signal, not a correctness bug: the build tree and
      the makefile disagree about which sources exist. Callers use it to fall back
      to a clean build on the conservative side; the objects themselves are inert.

      Returns the leftover object paths (empty when the makefile is missing, i.e.
      nothing to compare against).
    #>
    param(
        [string]$ProjectDir,
        [string]$Configuration,
        [string]$ImageType = 'production'
    )

    $makefile = Join-Path $ProjectDir "nbproject\Makefile-$Configuration.mk"
    if (-not (Test-Path -LiteralPath $makefile)) {
        return @()
    }

    $text = [System.IO.File]::ReadAllText($makefile, [System.Text.Encoding]::ASCII)
    $match = [regex]::Match($text, '(?m)^OBJECTFILES=([^\r\n]*)')
    if (-not $match.Success) {
        return @()
    }

    $expected = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in ($match.Groups[1].Value -split '\s+')) {
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        $entryMatch = [regex]::Match($entry, '^\$\{OBJECTDIR\}[\\/](.+)$')
        if (-not $entryMatch.Success) { continue }
        [void]$expected.Add(($entryMatch.Groups[1].Value -replace '/', '\'))
    }
    if ($expected.Count -eq 0) {
        return @()
    }

    # OBJECTDIR is build/<conf>/<image type>; only that tree is linked.
    $objectDir = Join-Path (Get-SonoraConfigurationBuildDir -ProjectDir $ProjectDir -Configuration $Configuration) $ImageType
    $objectDirFull = if (Test-Path -LiteralPath $objectDir) { (Resolve-Path -LiteralPath $objectDir).Path } else { $null }
    if ($null -eq $objectDirFull) {
        return @()
    }

    $orphans = @()
    foreach ($object in (Get-SonoraObjectFiles -Directory $objectDirFull)) {
        $relative = $object.FullName.Substring($objectDirFull.Length).TrimStart('\', '/')
        if (-not $expected.Contains($relative)) {
            $orphans += $object.FullName
        }
    }

    return $orphans
}
