<#
  C0.2 -- the MPLAB project configuration gate.

  configurations.xml is the authority on what each build compiles, and MPLAB X
  rewrites it whenever the IDE touches the project. It has silently dropped
  ex="true" attributes before. An exclusion that quietly disappears does not break
  the build: it makes a standalone application link the download engine without
  saying so, which is a defect you find on a board rather than at a desk.

  So the per-configuration expectations live here, in a checked-in script that
  exits non-zero -- not in a document, and not in a throwaway audit run by hand
  when someone remembers to be suspicious.

  DELIBERATELY NOT A GENERATED BASELINE. The sibling separation ratchet uses a
  ratchet, which is right for a metric that should only ever fall. This is not
  that: the values below are design intent (three configurations, two delivery
  modes), and a regenerable baseline would happily absorb the exact MPLAB X
  regression this exists to catch. Changing an expectation is an edit to this
  table, made on purpose, and reviewable as such.

  Companion to Assert-StandaloneMapLayout in build.ps1, which checks the same
  contract on the LINKED result. This one checks the inputs, and it does so
  without building -- so it runs in seconds, on any machine, with no toolchain.
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    # Test hook: point the checks at a mutated copy of configurations.xml so the
    # self-test can prove each one actually fires.
    [string]$ConfigurationsXml
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path -LiteralPath $Root).Path
$projectDir = Join-Path $repoRoot 'dspic33ak_audio_dsp.X'
$confXmlPath = if ([string]::IsNullOrWhiteSpace($ConfigurationsXml)) {
    Join-Path $projectDir 'nbproject\configurations.xml'
} else {
    (Resolve-Path -LiteralPath $ConfigurationsXml).Path
}

# --- Expectations ----------------------------------------------------------
# The delivery macro. Set by the configuration itself, which is why build.ps1 no
# longer injects it (defining it twice was the previous arrangement's failure).
$deliveryMacro = 'SONORA_DELIVERY_SERIAL_UPDATE_APP=1'

# The serial-update linker script, PER CONFIGURATION. This was one project-level file
# while AK512 was the only delivered part; the AK128 panel is a quarter the size, so its
# script is a different file and the project now registers both.
#
# That makes the exclusion pattern two-sided, and both sides fail silently:
#   - a FOREIGN script left unexcluded is a second --script= (MPLAB X hands the linker
#     every registered script), and the image is linked to whichever it took last;
#   - a configuration's OWN script excluded falls back to the device pack's default
#     script, which links a running image at the standalone origin -- over the resident
#     bootloader.
# Neither produces a link error, so both are asserted below rather than assumed.
# Same invariant, same reasoning, as check_resident_project.ps1 section 2.
$serialUpdateGld = @{
    'dsPIC33AK512_ASRC_SERIAL_UPDATE'    = '../src/linker/p33AK512MPS512_serial_update_app.gld'
    'dsPIC33AK512_CLASSIC_SERIAL_UPDATE' = '../src/linker/p33AK512MPS512_serial_update_app.gld'
    'dsPIC33AK128_SERIAL_UPDATE'         = '../src/linker/p33AK128MC106_serial_update_app.gld'
    'dsPIC33AK128_ASRC_BI_CODEC_SERIAL_UPDATE' = '../src/linker/p33AK128MC106_serial_update_app.gld'
}
# Every script the project is expected to register, deduplicated: one AK512 file shared
# by its two application variants, one AK128 file.
$allSerialUpdateGlds = @(@($serialUpdateGld.Values) | Sort-Object -Unique)

# The download engine, as seen by the application project: the ABI pair the two
# images agree on, the two app-side halves, and the two HAL modules only the
# delivery path needs. src/boot/** is deliberately absent -- it belongs to the
# separately linked boot image and must never appear here at all (asserted below as
# a structural invariant rather than an exclusion: after reorg step 3 the boot files
# are in another tree that this project has no include path into).
$engineSources = @(
    '../src/shared/resident_de_mailbox.c',
    '../src/shared/resident_de_pipe.c',
    '../src/app/resident_de/app/resident_de_app_console.c',
    '../src/app/resident_de/app/resident_de_app_handoff.c',
    '../src/app/hal_nvm/nora_nvm_dspic33ak.c'
)

# Sources that must be in EVERY configuration -- the mirror image of $engineSources.
#
# Both entries moved here from $engineSources on 2026-08-12. app_traps.c was listed as an
# engine source because the trap record lives in the noinit block, and the block was
# reserved only by the serial-update linker script. That made hardware trap diagnostics a
# property of the delivery mode: the three standalone configurations had no trap handler at
# all, and -- since main.c calls app_traps_boot_prepare() unconditionally -- did not even
# link. The block is now reserved in every configuration (a small supplementary linker
# script per device, see src/linker/p33AK*_noinit_ram_reserve.ld), so the HAL that owns it
# belongs everywhere too.
#
# Stated as a table rather than left implicit because "not excluded" is exactly the
# attribute MPLAB X rewrites, and a silently re-excluded file here is a diagnostic that
# stops existing -- or, for the HAL, a link that fails for a reason two files away.
#
# traps_console.c joined them on 2026-08-12 for a mechanical reason: app_onmsg() routes
# module 'x' to it unconditionally, so a configuration that excluded it would fail to link
# -- the same failure app_traps.c itself used to produce, one file away from its cause.
$everyConfigurationSources = @(
    '../src/app/diagnostics/app_traps.c',
    '../src/app/hal_noinit_ram/nora_noinit_ram_dspic33ak.c',
    '../src/app/uart_app/traps_console.c'
)

# The supplementary linker script that reserves the noinit block in a NON-delivery build,
# per device -- passed as a linker extra option rather than as the project's linker file,
# because it ADDS to the device default script instead of replacing it. A delivery build
# must NOT have one: its reservation comes from the full serial-update script, and a second
# section at the same address would be a duplicate the linker has no reason to diagnose.
#
# EMPTY since 2026-08-15, and deliberately kept: AK128 was the last standalone
# configuration and it is now delivered by serial update too, so every configuration takes
# its reservation from a full serial-update linker script. The table and the assertions
# that read it stay because standalone has not been ruled out as a MODE -- a part without
# a resident bootloader would need an entry here, and the check that notices a standalone
# configuration WITHOUT one is exactly what must not have been deleted by then.
# src/linker/p33AK128MC106_noinit_ram_reserve.ld is likewise kept on disk, unreferenced.
$reserveScriptOption = @{
}

# Exactly these three configurations, each classified. A NEW configuration must be
# added here on purpose: without this the gate would simply not look at it, which
# is the quietest way for an unchecked build variant to appear.
#
# Was five until 2026-08-15. dsPIC33AK512 and dsPIC33AK512_ASRC were deleted:
# serial update is how an AK512 board is delivered AND how it is developed, so the
# standalone AK512 pair had stopped being built by anyone, and an unbuilt
# configuration is one that rots without saying so.
#
# dsPIC33AK128 was replaced by dsPIC33AK128_SERIAL_UPDATE later the same day, for the same
# reason: the AK128 board is now delivered and developed over the serial downloader too.
# So NO standalone configuration is left and every standalone assertion below currently
# has nothing to fire on. They are kept, not deleted -- standalone is still a supported
# mode, and a part that arrives without a resident bootloader must find the checks that
# describe it already written. See the note on $reserveScriptOption.
$expectedConfigurations = [ordered]@{
    'dsPIC33AK512_ASRC_SERIAL_UPDATE'    = @{ Delivery = $true  }
    'dsPIC33AK512_CLASSIC_SERIAL_UPDATE' = @{ Delivery = $true  }
    'dsPIC33AK128_SERIAL_UPDATE'         = @{ Delivery = $true  }
    'dsPIC33AK128_ASRC_BI_CODEC_SERIAL_UPDATE' = @{ Delivery = $true  }
}

# Tracked sources under src/app/, src/shared/ and src/boot/ that are intentionally NOT registered in this project.
# Listed with a reason so that "this file is never compiled" stays a decision
# somebody made, instead of something nobody noticed.
$unregisteredByDesign = [ordered]@{
    # One rule, not a file list: NOTHING under src/boot/ is ever registered here. That is
    # the bulkhead, and it is asserted positively a few lines below as well -- this entry
    # only stops the "every tracked source is registered" sweep from demanding it.
    # Note this also excuses the src/boot/hal_*/*.c that the BOOT image does not compile
    # either (3 of 20 as of reorg step 4: nora_gpio_event, nora_gpio_table,
    # nora_high_res_timer). They are there because the HAL directories were vendored
    # WHOLE rather than file-by-file (procedure section 8, decision 3), so a header that
    # a boot source starts needing tomorrow is already present. Whether the boot image
    # compiles a given src/boot/ source is the boot project's business, and reorg step 5
    # gives it its own checked list in src/boot/boot_image.psd1.
    'src/boot/'                         = 'the resident boot image is linked separately by build_resident_bootloader.ps1 and by resident_bootloader.X; no src/boot/ source may ever be registered in the application project'
    'src/app/dspic33-cmsis-dsp/Source/' = 'third-party CMSIS-DSP; only the kernels actually used are registered'
    'src/app/apps/classic/dsp/'         = 'legacy Classic DSP blocks kept in-tree but not in any current build'
}

# --- Load -------------------------------------------------------------------
[xml]$xml = Get-Content -LiteralPath $confXmlPath -Raw
$problems = [System.Collections.Generic.List[string]]::new()

function Add-Problem {
    param([string]$Configuration, [string]$Message)
    $prefix = if ([string]::IsNullOrWhiteSpace($Configuration)) { '' } else { "[$Configuration] " }
    $problems.Add($prefix + $Message)
}

$confNodes = @($xml.configurationDescriptor.confs.conf)
if ($confNodes.Count -eq 0) {
    throw "No <conf> elements found in $confXmlPath -- the file is not the MPLAB project descriptor this check understands."
}

# --- 1. The configuration set itself ---------------------------------------
$actualNames = @($confNodes | ForEach-Object { $_.name })
$unexpected = @($actualNames | Where-Object { -not $expectedConfigurations.Contains($_) })
$absent = @($expectedConfigurations.Keys | Where-Object { $actualNames -notcontains $_ })
foreach ($n in $unexpected) {
    Add-Problem '' ("configuration '$n' is not in this gate's expectation table. Add it to " +
                    '$expectedConfigurations with its delivery mode -- an unclassified ' +
                    'configuration is not checked at all.')
}
foreach ($n in $absent) {
    Add-Problem '' "expected configuration '$n' is missing from the project."
}

# --- 2. itemPath registration ----------------------------------------------
$itemPaths = @($xml.SelectNodes('//itemPath') | ForEach-Object { $_.InnerText })
$registered = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($p in $itemPaths) {
    $full = [IO.Path]::GetFullPath((Join-Path $projectDir $p))
    if (-not (Test-Path -LiteralPath $full)) {
        Add-Problem '' "registered source does not exist on disk: $p (a rename or delete left the project dangling)"
    }
    $rel = $full.Substring($repoRoot.Length).TrimStart('\', '/').Replace('\', '/')
    [void]$registered.Add($rel)
}

# The boot image's sources must not be in the application project under any
# configuration -- not registered-and-excluded, but absent. Delivery mode is a
# property of the application build; the boot image is a different link entirely.
foreach ($rel in $registered) {
    if ($rel -like 'src/boot/*') {
        Add-Problem '' ("boot-image source '$rel' is registered in the application project. " +
                        'It belongs only to the separately linked resident boot image.')
    }
}

# Every tracked source is either registered or listed as unregistered by design.
# This one check needs git. It is repo hygiene rather than image layout, so a
# missing/failing git is reported and skipped instead of failing the gate -- this
# script runs from build.ps1, and "no git here" must not be able to stop a build.
# Everything above and below is decided from configurations.xml and the disk alone.
$trackedSources = @()
$trackedKnown = $false
$gitOutput = $null
try { $gitOutput = @(& git -C $repoRoot ls-files -- src/app src/shared src/boot 2>$null) } catch { $gitOutput = $null }
if ($LASTEXITCODE -eq 0 -and $null -ne $gitOutput) {
    $trackedKnown = $true
    $trackedSources = @($gitOutput |
        ForEach-Object { $_.Replace('\', '/') } |
        Where-Object { $_ -match '\.(c|s|S)$' })
} else {
    Write-Host '  NOTE: git ls-files unavailable - skipping the "every tracked source is registered" check.'
}
foreach ($src in $trackedSources) {
    if ($registered.Contains($src)) { continue }
    $excusedBy = @($unregisteredByDesign.Keys | Where-Object { $src.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })
    if ($excusedBy.Count -eq 0) {
        Add-Problem '' ("tracked source '$src' is not registered in the project and is not covered " +
                        'by $unregisteredByDesign. It is silently never compiled -- register it, or ' +
                        'record why it is not built.')
    }
}

# Each serial-update linker script is registered exactly once, project-wide -- and no
# OTHER .gld is registered at all. The second half matters now that there is more than
# one: an extra script nobody named is one no configuration excludes, and MPLAB X passes
# it to every link.
foreach ($gld in $allSerialUpdateGlds) {
    $gldRegistrations = @($itemPaths | Where-Object { $_ -eq $gld })
    if ($gldRegistrations.Count -ne 1) {
        Add-Problem '' "the serial-update linker script should be registered exactly once as '$gld'; found $($gldRegistrations.Count)."
    }
}
foreach ($stray in @($itemPaths | Where-Object { $_ -like '*.gld' -and $allSerialUpdateGlds -notcontains $_ })) {
    Add-Problem '' ("linker script '$stray' is registered but is not named by `$serialUpdateGld. " +
                    'MPLAB X hands every registered script to the linker, so no configuration ' +
                    'excludes it and every link gets a second --script=.')
}

# --- 3. Per-configuration state --------------------------------------------
foreach ($conf in $confNodes) {
    $name = $conf.name
    if (-not $expectedConfigurations.Contains($name)) { continue }
    $isDelivery = [bool]$expectedConfigurations[$name].Delivery

    # Excluded item paths for this configuration.
    $excluded = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($item in @($conf.SelectNodes('.//item'))) {
        if ($item.GetAttribute('ex') -eq 'true') { [void]$excluded.Add($item.GetAttribute('path')) }
    }

    # 3a. The delivery macro. Checked in every non-empty preprocessor-macros
    # property, because MPLAB X keeps one per tool and setting only some of them
    # is a real way to get a half-configured build.
    $macroProps = @($conf.SelectNodes('.//property[@key="preprocessor-macros"]') |
        ForEach-Object { $_.GetAttribute('value') })
    $nonEmpty = @($macroProps | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $withMacro = @($nonEmpty | Where-Object { $_ -split ';' -contains $deliveryMacro })
    if ($isDelivery) {
        if ($nonEmpty.Count -eq 0) {
            Add-Problem $name "no preprocessor macros are set at all, but this is a delivery configuration; $deliveryMacro must be defined."
        } elseif ($withMacro.Count -ne $nonEmpty.Count) {
            Add-Problem $name ("$deliveryMacro is defined in only $($withMacro.Count) of " +
                               "$($nonEmpty.Count) macro lists. Every tool that compiles this " +
                               'configuration must see it, or the two halves of the image disagree.')
        }
    } else {
        if ($withMacro.Count -gt 0) {
            Add-Problem $name "$deliveryMacro is defined, but this is a standalone configuration."
        }
    }

    # 3a2. Every include directory exists on disk. Three of them did not until the
    # src/ -> src/app/ move (`..\src\uart`, `..\src\debug`, `..\src\adc`): a non-existent
    # -I path contributes nothing, so nothing complained, and the list stopped being
    # a statement about the tree. After a directory rename this is also the check
    # that catches a half-rewritten property.
    foreach ($prop in @($conf.SelectNodes('.//property[@key="extra-include-directories"]')) +
                      @($conf.SelectNodes('.//property[@key="extra-include-directories-for-assembler"]'))) {
        $value = $prop.GetAttribute('value')
        if ([string]::IsNullOrWhiteSpace($value)) { continue }
        foreach ($dir in ($value -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
            $full = [IO.Path]::GetFullPath((Join-Path $projectDir $dir))
            if (-not (Test-Path -LiteralPath $full -PathType Container)) {
                Add-Problem $name ("include directory '$dir' does not exist. Either the path is stale " +
                                   '(it then contributes nothing and hides a missing header) or a rename ' +
                                   'rewrote only part of the list.')
            }
        }
    }

    # 3b. The linker scripts -- this configuration's own, and every other one.
    $ownGld = $serialUpdateGld[$name]
    if ($isDelivery -and -not $ownGld) {
        Add-Problem $name 'is a delivery configuration with no entry in $serialUpdateGld. Name the serial-update linker script for its device.'
    }
    foreach ($gld in $allSerialUpdateGlds) {
        $gldExcluded = $excluded.Contains($gld)
        if ($gld -eq $ownGld) {
            if ($gldExcluded) {
                Add-Problem $name ("its own serial-update linker script '$gld' is excluded, so the " +
                                   "link falls back to the device pack's default script: the application " +
                                   'would be linked at the standalone origin and overwrite the resident bootloader.')
            }
        } elseif (-not $gldExcluded) {
            $why = if ($isDelivery) {
                "it belongs to another device and would be a second --script= alongside '$ownGld'."
            } else {
                'a standalone image must use the device default layout.'
            }
            Add-Problem $name "serial-update linker script '$gld' is NOT excluded; $why"
        }
    }

    # 3c. The download engine sources. This is the check the ex="true" loss story
    # is about: for a standalone configuration each must be present in the project
    # AND excluded from this configuration.
    foreach ($src in $engineSources) {
        $isRegistered = $itemPaths -contains $src
        if (-not $isRegistered) {
            Add-Problem $name "download-engine source '$src' is not registered in the project at all."
            continue
        }
        $isExcluded = $excluded.Contains($src)
        if (-not $isDelivery -and -not $isExcluded) {
            Add-Problem $name ("download-engine source '$src' is NOT excluded. This standalone build " +
                               'would link the download engine. If MPLAB X rewrote this file, the ' +
                               'ex="true" attribute was dropped -- restore it.')
        }
        if ($isDelivery -and $isExcluded) {
            Add-Problem $name "download-engine source '$src' is excluded, but this configuration needs it."
        }
    }

    # 3d. Sources every configuration must compile, delivery mode notwithstanding.
    foreach ($src in $everyConfigurationSources) {
        if ($itemPaths -notcontains $src) {
            Add-Problem $name "'$src' is not registered in the project at all, but every configuration must compile it."
            continue
        }
        if ($excluded.Contains($src)) {
            Add-Problem $name ("'$src' is excluded, but it must be compiled by every configuration. " +
                               'If MPLAB X rewrote this file, an ex="true" was added -- remove it.')
        }
    }

    # 3e. The noinit-block reservation for non-delivery builds. It rides in the linker's
    # extra-options field, which is a free-text box: a value lost to an IDE round-trip
    # leaves a build that links, runs, and quietly lets the automatic stack have the
    # block -- so the expected string is pinned here rather than trusted.
    $ldExtra = @($conf.SelectNodes('.//property[@key="oXC16ld-extra-opts"]') |
        ForEach-Object { $_.GetAttribute('value') })
    $expectedOption = $reserveScriptOption[$name]
    if ($isDelivery) {
        if ($expectedOption) {
            Add-Problem $name 'is a delivery configuration but has an entry in $reserveScriptOption. Its reservation comes from the full serial-update linker script.'
        }
        $withReserve = @($ldExtra | Where-Object { $_ -like '*_noinit_ram_reserve.ld*' })
        if ($withReserve.Count -gt 0) {
            Add-Problem $name 'passes a *_noinit_ram_reserve.ld script, but its linker script already reserves the block. Two sections at one address is not something the linker has to diagnose.'
        }
    } else {
        if (-not $expectedOption) {
            Add-Problem $name 'is a standalone configuration with no entry in $reserveScriptOption. Every configuration needs the noinit block reserved -- name the script for this device.'
        } elseif (-not ($ldExtra -contains $expectedOption)) {
            $shown = if ($ldExtra.Count -gt 0) { ($ldExtra | ForEach-Object { "'$_'" }) -join ', ' } else { '(none)' }
            Add-Problem $name ("the linker extra options must contain '$expectedOption' so the noinit " +
                               "block is reserved; found $shown.")
        }
    }
}

# --- Report -----------------------------------------------------------------
Write-Host "Configuration gate: $confXmlPath"
$trackedText = if ($trackedKnown) { "$($trackedSources.Count) tracked source(s) under src/app/ + src/shared/ + src/boot/" } else { 'tracked sources not checked' }
Write-Host ("  $($confNodes.Count) configuration(s), $($itemPaths.Count) registered item(s), " + $trackedText)

if ($problems.Count -gt 0) {
    Write-Host ''
    Write-Host "Configuration gate FAILED: $($problems.Count) problem(s)" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" }
    Write-Host ''
    Write-Host 'These expectations are design intent, not a generated baseline. If a change here'
    Write-Host 'is intended, edit the tables at the top of this script deliberately.'
    exit 1
}

foreach ($name in $expectedConfigurations.Keys) {
    $mode = if ($expectedConfigurations[$name].Delivery) { 'delivery  ' } else { 'standalone' }
    Write-Host "  $mode  $name"
}
Write-Host 'Configuration gate: PASS'
exit 0
