param()

# Host tests for the two standalone-vs-delivery build gates:
#
#   C0.1  Assert-StandaloneMapLayout   (buildtools/build.ps1)
#         checks the LINKED result -- every standalone configuration's .map
#   C0.2  buildtools/check_configurations.ps1
#         checks the INPUTS -- what each MPLAB configuration compiles and excludes
#   C0.3  buildtools/check_resident_project.ps1
#         the GENERATED boot project still matches src/boot/boot_image.psd1
#   C0.4  buildtools/check_hal_drift.ps1
#         reports src/boot/hal_* vs src/app/hal_* -- and never fails a build, which is
#         itself asserted here because it is a design decision, not an oversight
#
# Why these tests exist at all, and why they are checked in rather than run once by
# hand: a gate that passes on a good input has proved nothing. The only evidence
# that a check is alive is that it FIRES on a targeted mutation, and fires as the
# right finding. C0.2 exists precisely because a throwaway audit is not a gate --
# the same reasoning applies to the tests that prove the gates work.
#
# That is not theoretical here. Two real defects in C0.1 were found only by these
# tests: $problems.Add($fmt -f $a, $b) parses the comma as a METHOD ARGUMENT
# separator, so the assertion crashed on the way to naming defects instead of
# reporting them -- indistinguishable from a working gate unless you check WHY the
# negative cases fail. And '"a" + "b" -f $x' binds -f to the last string only.
#
# No toolchain and no board needed. C0.1's suite does need the five .map files from
# a previous build; it says so and skips rather than failing if they are absent.
#
# Same harness shape as tests/hal_noinit_ram/run_host_tests.ps1.

$ErrorActionPreference = 'Stop'

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$failures = 0

function Write-Section { param([string]$Text) Write-Host ''; Write-Host "=== $Text ===" }

# ---------------------------------------------------------------------------
# C0.1 -- Assert-StandaloneMapLayout
# ---------------------------------------------------------------------------
# The function is extracted from build.ps1 by parsing it, rather than dot-sourcing
# the script: build.ps1 is a build driver with a param block and side effects, so
# running it is not an option. This keeps the test on the SHIPPING copy of the
# function -- there is no second copy here to drift.
function Import-BuildFunction {
    param([string]$Name, [string]$Script = 'buildtools\build.ps1')
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repoRoot $Script), [ref]$null, [ref]$null)
    $found = $ast.FindAll({
        $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $args[0].Name -eq $Name }, $true)
    if ($found.Count -ne 1) {
        throw "expected exactly 1 definition of $Name in $Script, found $($found.Count)"
    }
    return [scriptblock]::Create($found[0].Extent.Text)
}

# The project has NO standalone configuration since 2026-08-15 -- dsPIC33AK128 was
# replaced by dsPIC33AK128_SERIAL_UPDATE, as the two AK512 ones had been before it.
# Assert-StandaloneMapLayout is kept anyway (standalone survives as a delivery MODE, and
# the next part to arrive arrives without a bootloader), so this suite keeps testing it
# and DERIVES the standalone base map it needs from a delivery map instead of skipping.
#
# Deriving a fixture is normally a way to test a fiction. It is safe here for one
# reason, and only that reason: the derived map is used only if the assertion ACCEPTS
# it, which Test-C01 checks before running a single mutation. A fixture that stopped
# resembling a standalone link fails that check loudly rather than quietly making every
# mutation meaningless. What this does is undo, in the text, exactly the things that
# make a link a serial-update link -- nothing is invented.
function New-StandaloneMapText {
    param([string]$DeliveryMapText)

    # The application base is read out of the map rather than named, so this stays
    # device-agnostic: it is 0x808000 on the AK512 and 0x804000 on the AK128.
    $region = [regex]::Match($DeliveryMapText,
        '"program"\s+Memory\s*\[Origin = (0x[0-9a-fA-F]+), Length = 0x[0-9a-fA-F]+\]')
    if (-not $region.Success) { throw 'the delivery map has no "program" memory region' }
    $appBase = $region.Groups[1].Value

    # 1. Drop what only a serial-update link has: the IVT relocation, the layout
    #    defsyms, and every line the download engine put there (306 of them in the
    #    AK128 map). The three .resident_* diagnostic sections go with them, which is
    #    right -- a standalone image has no bootloader to leave a trace for.
    $kept = foreach ($line in ($DeliveryMapText -split "`r?`n")) {
        if ($line -match '--ivt=0x[0-9a-fA-F]+') { continue }
        if ($line -match '--defsym=__SONORA_') { continue }
        if ($line -match '(?i)resident|nora_nvm') { continue }
        $line
    }
    $text = $kept -join "`r`n"

    # 2. The link used the device's own script, not the serial-update one.
    $text = [regex]::Replace($text, '-T(p33AK[0-9A-Za-z]+)_serial_update_app\.', '-T${1}.')

    # 3. Program memory starts at the device default. Only ORIGIN is rewritten:
    #    LENGTH is not asserted, and a fabricated one would be a constant this file
    #    has no business owning.
    $text = [regex]::Replace($text, '("program"\s+Memory\s*\[Origin = )0x[0-9a-fA-F]+', '${1}0x800004')
    $text = $text.Replace($appBase, '0x800004')

    # 4. The stack runs up to the noinit block. In a delivery image it stops 0x130
    #    lower, under the three sections removed in step 1, so leaving it alone would
    #    make the fixture fail the very check the last mutation exercises.
    $block = [regex]::Match($text, '(?m)^\.noinit_ram\s+(0x[0-9a-fA-F]+)\s')
    if (-not $block.Success) { throw 'the delivery map has no .noinit_ram block' }
    $blockStart = [Convert]::ToUInt32($block.Groups[1].Value, 16)
    $stackRe = [regex]'(?m)^(stack\s+)(0x[0-9a-fA-F]+)(\s+)0x[0-9a-fA-F]+'
    if (-not $stackRe.IsMatch($text)) { throw 'the delivery map has no stack allocation' }
    $text = $stackRe.Replace($text, {
        param($m)
        $start = [Convert]::ToUInt32($m.Groups[2].Value, 16)
        $m.Groups[1].Value + $m.Groups[2].Value + $m.Groups[3].Value +
        ('0x{0:x}' -f ($blockStart - $start))
    }, 1)

    return $text
}

function Test-C01 {
    . (Import-BuildFunction -Name 'Assert-StandaloneMapLayout')

    # All three configurations are delivery ones, so all three must be REJECTED, and
    # the standalone base is derived from one of them (see New-StandaloneMapText).
    # The AK128 map is the source because it is the part whose layout moved last;
    # any of the three would do, the derivation reads the base address from the map.
    $delivery = @('dsPIC33AK512_ASRC_SERIAL_UPDATE', 'dsPIC33AK512_CLASSIC_SERIAL_UPDATE',
                  'dsPIC33AK128_SERIAL_UPDATE')
    $fixtureSource = 'dsPIC33AK128_SERIAL_UPDATE'

    function Get-MapPath { param($Conf)
        Join-Path $repoRoot "dspic33ak_audio_dsp.X\dist\$Conf\production\dspic33ak_audio_dsp.X.production.map" }

    # Returns the assertion message, or $null when the map was accepted.
    function Invoke-Assert { param($MapPath, $Conf)
        try { Assert-StandaloneMapLayout -MapPath $MapPath -Configuration $Conf -RepoRoot $repoRoot; return $null }
        catch { return $_.Exception.Message }
    }

    $absent = @($delivery | Where-Object { -not (Test-Path -LiteralPath (Get-MapPath $_)) })
    if ($absent.Count -gt 0) {
        Write-Host "SKIP C0.1: no .map files for $($absent -join ', ')."
        Write-Host '     Build those configurations first (pwsh buildtools/build.ps1 -Configuration <name> -Full).'
        return 0
    }

    $bad = 0
    $tmp = Join-Path ([IO.Path]::GetTempPath()) 'c01_maps'
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    Write-Host '--- A. real maps: all 3 are serial-update and must be REJECTED BY THE ASSERTION ---'
    foreach ($conf in $delivery) {
        $err = Invoke-Assert (Get-MapPath $conf) $conf
        if ($null -eq $err) { Write-Host "BAD  $conf was ACCEPTED - the gate is empty"; $bad++; continue }
        if ($err -notmatch 'Standalone layout assertion failed') {
            Write-Host "BAD  $conf failed for the wrong reason (not the assertion):`n$err"; $bad++; continue
        }
        # It must NAME the real defects, not crash on the way to naming them.
        $want = @('program ORIGIN', 'relocates the IVT', 'serial-update linker script',
                  'download engine leaked', 'stack ends at')
        $missing = @($want | Where-Object { $err -notmatch [regex]::Escape($_) })
        if ($missing.Count -gt 0) {
            Write-Host "BAD  $conf assertion did not report: $($missing -join '; ')`n$err"; $bad++; continue
        }
        Write-Host "OK   $conf rejected, and all 5 expected defects named"
    }

    Write-Host ''
    Write-Host '--- B. mutated standalone map: every individual check must fire ---'
    $baseText = New-StandaloneMapText -DeliveryMapText ([IO.File]::ReadAllText((Get-MapPath $fixtureSource)))
    # THE GUARD THE WHOLE DERIVATION RESTS ON. If the fixture is not something the
    # assertion accepts, every mutation below would "be detected" for the wrong reason
    # and the suite would pass while testing nothing.
    [IO.File]::WriteAllText((Join-Path $tmp 'base.map'), $baseText)
    $fixtureErr = Invoke-Assert (Join-Path $tmp 'base.map') 'standalone(derived)'
    if ($null -ne $fixtureErr) {
        Write-Host "BAD  the derived standalone fixture is NOT accepted, so B tests nothing:`n$fixtureErr"
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        return $bad + 1
    }
    Write-Host "OK   standalone fixture derived from $fixtureSource is accepted"
    $mutations = @(
        @{ Name = 'program origin moved';   Expect = 'program ORIGIN'
           Do = { param($t) $t -replace '"program" Memory  \[Origin = 0x800004', '"program" Memory  [Origin = 0x808000' } },
        @{ Name = 'IVT relocated';          Expect = 'relocates the IVT'
           Do = { param($t) $t -replace '(?m)^  --isr \\\r?$', "  --isr \`r`n  --ivt=0x808000 \" } },
        # Device-agnostic on purpose (the base map is AK128 now, and was AK512 before):
        # renames the DEVICE DEFAULT script in the link line to a serial-update one.
        # [0-9A-Za-z]+ before the dot is what keeps it off the supplementary
        # p33AK*_noinit_ram_reserve.ld, whose name has an underscore where this needs
        # the extension -- mutating that one instead would still trip the assertion
        # and so would still "pass", for the wrong reason.
        @{ Name = 'serial-update .gld';     Expect = 'serial-update linker script'
           Do = { param($t) [regex]::Replace($t, '-T(p33AK[0-9A-Za-z]+)\.',
                                             '-T${1}_serial_update_app.', 1) } },
        @{ Name = 'Sonora defsym';          Expect = 'layout defsym'
           Do = { param($t) $t -replace '--defsym=__MPLAB_BUILD=1',
                                        "--defsym=__MPLAB_BUILD=1 \`r`n  --defsym=__SONORA_PROGRAM_ORIGIN=0x808000" } },
        @{ Name = 'engine section linked';  Expect = 'download engine leaked'
           Do = { param($t) $t -replace '(?m)^stack ', ".text.resident_de_app_console_onmsg  0x81fb74  0x480`r`nstack " } },
        # Shorten whatever length the map actually reports, rather than matching a
        # literal one. The hardcoded numbers this replaced (0xb8f8 / 0x8708) stopped
        # matching the first time the layout moved, and a mutation that does not
        # apply is caught only by the $text -eq $baseText check below -- i.e. it
        # reports a test bug forever instead of exercising the check.
        # Only the hex length is rewritten; the trailing "(decimal)" is left alone
        # because the assertion reads the two hex fields and nothing else.
        @{ Name = 'stack shortened';        Expect = 'stack ends at'
           Do = { param($t)
                  $re = [regex]'(?m)^(stack\s+0x[0-9a-fA-F]+\s+)0x([0-9a-fA-F]+)'
                  $re.Replace($t, {
                      param($m)
                      $m.Groups[1].Value +
                      ('0x{0:x}' -f ([Convert]::ToUInt32($m.Groups[2].Value, 16) - 0x200))
                  }, 1) } },
        # The two below are the standalone half of the 2026-08-12 change: the noinit block
        # is now EXPECTED in a standalone image (app_traps.c stores the trap record there in
        # every configuration), so its absence, and its presence at the wrong place, are
        # both defects. The first is what a lost linker extra-option looks like in the map.
        @{ Name = 'noinit block not reserved'; Expect = 'not in the map at all'
           Do = { param($t) [regex]::Replace($t, '(?m)^\.noinit_ram\s+\S+\s+\S+\s+\S+.*\r?\n', '', 1) } },
        @{ Name = 'noinit block at the wrong address'; Expect = 'expected 0x'
           Do = { param($t)
                  $re = [regex]'(?m)^(\.noinit_ram\s+)0x[0-9a-fA-F]+'
                  $re.Replace($t, '${1}0x13f00', 1) } }
    )
    foreach ($m in $mutations) {
        $text = & $m.Do $baseText
        # A mutation that did not apply would look exactly like a passing test.
        if ($text -eq $baseText) { Write-Host "BAD  mutation '$($m.Name)' did not change the map (test bug)"; $bad++; continue }
        $p = Join-Path $tmp ("m_" + ($m.Name -replace '\W', '_') + '.map')
        [IO.File]::WriteAllText($p, $text)
        $err = Invoke-Assert $p 'dsPIC33AK512(mutated)'
        if ($null -eq $err) { Write-Host "BAD  '$($m.Name)' was NOT detected - dead check"; $bad++; continue }
        if ($err -notmatch [regex]::Escape($m.Expect)) {
            Write-Host "BAD  '$($m.Name)' detected but not as '$($m.Expect)':`n$err"; $bad++; continue
        }
        Write-Host "OK   '$($m.Name)' detected"
    }

    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    return $bad
}

# ---------------------------------------------------------------------------
# C0.2 -- check_configurations.ps1
# ---------------------------------------------------------------------------
function Test-C02 {
    $gate = Join-Path $repoRoot 'buildtools\check_configurations.ps1'
    $src = Join-Path $repoRoot 'dspic33ak_audio_dsp.X\nbproject\configurations.xml'
    $base = [IO.File]::ReadAllText($src)
    $tmp = Join-Path ([IO.Path]::GetTempPath()) 'c02_conf'
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null

    # Run the gate as a child process: it signals by exit code, and that is the
    # part being tested.
    function Invoke-Gate { param($Xml)
        $out = & pwsh -NoProfile -File $gate -Root $repoRoot -ConfigurationsXml $Xml 2>&1 | Out-String
        return @{ Code = $LASTEXITCODE; Out = $out }
    }

    # The first ex="true" for an engine path belongs to the first configuration that
    # EXCLUDES it -- i.e. a standalone one, since a delivery configuration carries no
    # ex for the engine at all. So dropping it is the exact MPLAB X regression being
    # simulated. (Before 2026-08-15 that was dsPIC33AK512, the first conf in the file;
    # it is now dsPIC33AK128, the only standalone one left. The mutation is unchanged
    # because it was never keyed on file order, only on where the attribute exists.)
    # \s* on both sides of ex="true", and no mandatory newline: MPLAB X wraps a long
    # <item> onto several lines, but a hand-edited or freshly written entry keeps the
    # attributes on one line. A pattern that requires the wrap silently stops mutating
    # -- which surfaces as "mutation did not change the file", a dead test rather than
    # a failing one (that is exactly what the src/shared/ move produced on 2026-08-14).
    function Remove-FirstEx { param($Text, $Path)
        $pat = '(<item path="' + [regex]::Escape($Path) + '"\s*)ex="true"(\s*)'
        return [regex]::Replace($Text, $pat, '$1$2', 1)
    }

    $bad = 0
    $r = Invoke-Gate $src
    if ($r.Code -ne 0) { Write-Host "BAD  the real file must PASS:`n$($r.Out)"; return 1 }
    Write-Host 'OK   real configurations.xml passes'

    $mutations = @(
        @{ Name = 'ex="true" dropped from an engine source (the MPLAB X regression)'
           Expect = 'is NOT excluded. This standalone build'
           Requires = 'standalone'
           Do = { param($t) Remove-FirstEx $t '../src/shared/resident_de_mailbox.c' } },
        # Every configuration is a delivery one now, so what the first ex="true" on a
        # serial-update .gld excludes is the OTHER DEVICE's script -- and an unexcluded
        # foreign script is a second --script= the linker takes without complaining.
        # Same finding, other half of the same check.
        @{ Name = 'ex="true" dropped from another device''s serial-update linker script'
           Expect = 'is NOT excluded; it belongs to another device'
           Do = { param($t) Remove-FirstEx $t '../src/linker/p33AK512MPS512_serial_update_app.gld' } },
        @{ Name = 'delivery macro missing from one tool of a delivery conf'
           Expect = 'is defined in only'
           Do = { param($t) [regex]::Replace($t,
                    'SONORA_MPLAB_APP_ASRC=1;SONORA_MPLAB_SERIAL_UPDATE=1;SONORA_DELIVERY_SERIAL_UPDATE_APP=1',
                    'SONORA_MPLAB_APP_ASRC=1;SONORA_MPLAB_SERIAL_UPDATE=1', 1) } },
        @{ Name = 'delivery macro present in a standalone conf'
           Expect = 'but this is a standalone configuration'
           Requires = 'standalone'
           Do = { param($t) [regex]::Replace($t,
                    '<property key="preprocessor-macros"\s*\r?\n?\s*value=""',
                    '<property key="preprocessor-macros" value="SONORA_DELIVERY_SERIAL_UPDATE_APP=1"', 1) } },
        @{ Name = 'an unclassified configuration appears'
           Expect = "is not in this gate's expectation table"
           Do = { param($t) [regex]::Replace($t, '<conf name="dsPIC33AK128_SERIAL_UPDATE"', '<conf name="dsPIC33AK128_EXPERIMENT"', 1) } },
        @{ Name = 'a boot-image source is registered in the app project'
           Expect = 'boot-image source'
           Do = { param($t) [regex]::Replace($t,
                    '<itemPath>../src/shared/resident_de_mailbox.c</itemPath>',
                    "<itemPath>../src/boot/resident_de_bootloader.c</itemPath>`r`n        <itemPath>../src/shared/resident_de_mailbox.c</itemPath>", 1) } },
        @{ Name = 'a registered source no longer exists (rename left it dangling)'
           Expect = 'does not exist on disk'
           Do = { param($t) [regex]::Replace($t,
                    '<itemPath>../src/shared/resident_de_pipe.c</itemPath>',
                    '<itemPath>../src/shared/resident_de_pipe_renamed.c</itemPath>', 1) } },
        @{ Name = 'an engine source excluded from a DELIVERY conf'
           Expect = 'but this configuration needs it'
           Do = { param($t)
                  # The ASRC_SERIAL_UPDATE conf carries no ex for the engine; add one.
                  $i = $t.IndexOf('<conf name="dsPIC33AK512_ASRC_SERIAL_UPDATE"')
                  $j = $t.IndexOf('<item path=', $i)
                  $t.Substring(0, $j) +
                  "<item path=`"../src/app/hal_nvm/nora_nvm_dspic33ak.c`"`r`n            ex=`"true`"`r`n            overriding=`"false`">`r`n      </item>`r`n      " +
                  $t.Substring($j) } },
        # The mirror of the first mutation, for the sources that must be in EVERY
        # configuration: app_traps.c and the noinit HAL. Re-excluding either one is how the
        # 2026-08-12 change would silently unwind -- the delivery builds would keep working,
        # so nothing else would complain.
        @{ Name = 'ex="true" added to a source every configuration must compile'
           Expect = 'must be compiled by every configuration'
           Do = { param($t) [regex]::Replace($t,
                    '(<item path="' + [regex]::Escape('../src/app/diagnostics/app_traps.c') + '" )ex="false"',
                    '$1ex="true"', 1) } },
        # The other way that list unwinds: the file is not excluded, it is simply never
        # registered. traps_console.c (module 'x') is the case in point -- it carries no
        # per-configuration <item> at all, so there is no ex="true" to drop; losing its
        # <itemPath> is the whole failure, and it would take the trap-test commands with it.
        @{ Name = 'a source every configuration must compile is not registered at all'
           Expect = 'every configuration must compile it'
           Do = { param($t) [regex]::Replace($t,
                    '\s*<itemPath>' + [regex]::Escape('../src/app/uart_app/traps_console.c') + '</itemPath>',
                    '', 1) } },
        # And the option that does the reserving. It lives in a free-text field, which is
        # the kind of setting an IDE round-trip drops without saying so.
        #
        # \s* between the attributes on purpose: MPLAB X wraps a long value onto its own
        # line, so a pattern with a literal space matched when this was written and stopped
        # matching the first time the IDE reformatted the file -- which shows up as
        # "mutation did not change the file", i.e. a dead test rather than a failing one.
        @{ Name = 'the noinit reserve script lost from a standalone conf'
           Expect = 'so the noinit'
           Requires = 'standalone'
           Do = { param($t) [regex]::Replace($t,
                    '<property key="oXC16ld-extra-opts"\s*value="[^"]*_noinit_ram_reserve\.ld"',
                    '<property key="oXC16ld-extra-opts" value=""', 1) } }
    )

    # Three of the mutations above need a STANDALONE configuration to mutate, and the
    # project has had none since 2026-08-15. They are skipped rather than deleted, and
    # the skip is printed on every run, because the gate branches they exercise are
    # still in check_configurations.ps1 and are still what protects the next part that
    # arrives without a bootloader. Deleting them would make the loss of coverage
    # invisible; the notice is the point. Restore them by re-adding a standalone
    # configuration to $expectedConfigurations in the gate and to the project.
    $hasStandalone = ([IO.File]::ReadAllText(
        (Join-Path $repoRoot 'buildtools\check_configurations.ps1')) -match 'Delivery\s*=\s*\$false')

    for ($i = 0; $i -lt $mutations.Count; $i++) {
        $m = $mutations[$i]
        if ($m.Requires -eq 'standalone' -and -not $hasStandalone) {
            Write-Host "SKIP no standalone configuration exists to mutate: $($m.Name)"
            continue
        }
        $text = & $m.Do $base
        if ($text -eq $base) { Write-Host "BAD  mutation did not change the file (test bug): $($m.Name)"; $bad++; continue }
        $p = Join-Path $tmp ("m$i.xml")
        [IO.File]::WriteAllText($p, $text)
        $r = Invoke-Gate $p
        if ($r.Code -eq 0) { Write-Host "BAD  NOT DETECTED: $($m.Name)"; $bad++; continue }
        if ($r.Out -notmatch [regex]::Escape($m.Expect)) {
            Write-Host "BAD  detected but not as '$($m.Expect)': $($m.Name)`n$($r.Out)"; $bad++; continue
        }
        Write-Host "OK   detected: $($m.Name)"
    }

    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    return $bad
}

# ---------------------------------------------------------------------------
# C0.3 -- check_resident_project.ps1
# ---------------------------------------------------------------------------
# The resident project is GENERATED, so this gate's job is different from C0.2's:
# not "is the hand-maintained file still right" but "has the checked-in file drifted
# from the manifest it was generated from". The mutations below are the ways that
# happens in practice -- an IDE round-trip, an accepted "add this include directory?"
# offer, a manifest edit nobody regenerated.
function Test-C03 {
    $gate = Join-Path $repoRoot 'buildtools\check_resident_project.ps1'
    $src = Join-Path $repoRoot 'resident_bootloader.X\nbproject\configurations.xml'
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Host 'SKIP resident_bootloader.X is not generated yet (run buildtools/generate_resident_project.ps1)'
        return 0
    }
    $base = [IO.File]::ReadAllText($src)
    $tmp = Join-Path ([IO.Path]::GetTempPath()) 'c03_resident_proj'

    # The gate takes a DIRECTORY, so each mutation needs its own nbproject dir. The
    # fixture must mirror the real project's SHAPE, not just hold one file: the gate also
    # checks the Makefile one level up, so the temp dir is <m>/nbproject with the Makefile
    # beside it. Without that every mutation would "be detected" for the wrong reason -- a
    # missing Makefile -- and the suite would pass while testing nothing.
    $realMakefile = Join-Path $repoRoot 'resident_bootloader.X\Makefile'
    function Invoke-Gate { param($Text, $Index, [switch]$NoMakefile, [string]$MakefileText)
        $projDir = Join-Path $tmp "m$Index"
        $dir = Join-Path $projDir 'nbproject'
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        [IO.File]::WriteAllText((Join-Path $dir 'configurations.xml'), $Text)
        $mk = Join-Path $projDir 'Makefile'
        if ($NoMakefile) {
            if (Test-Path -LiteralPath $mk) { Remove-Item -LiteralPath $mk -Force }
        } elseif ($PSBoundParameters.ContainsKey('MakefileText')) {
            [IO.File]::WriteAllText($mk, $MakefileText)
        } else {
            Copy-Item -LiteralPath $realMakefile -Destination $mk -Force
        }
        $out = & pwsh -NoProfile -File $gate -Root $repoRoot -ProjectXmlDir $dir 2>&1 | Out-String
        return @{ Code = $LASTEXITCODE; Out = $out }
    }

    $bad = 0
    $r = Invoke-Gate $base 'real'
    if ($r.Code -ne 0) { Write-Host "BAD  the real file must PASS:`n$($r.Out)"; return 1 }
    Write-Host 'OK   generated resident_bootloader.X passes'

    # The optimization level is NOT a literal in this file. It comes from the manifest's
    # CompilerFlags -- '2' until the -Os checkpoint on 2026-08-19, 's' since -- and a
    # mutation naming a level the project no longer emits changes nothing, so the gate
    # reads as tested while testing nothing. That is not hypothetical: this entry still
    # said value="2" and self-reported as a test bug on the first run after the flag
    # moved. Read the level the generator actually emitted, and flip it to one that is
    # never the current level.
    $optLevel = [regex]::Match($base,
        '<property key="optimization-level" value="([^"]*)"/>').Groups[1].Value
    if (-not $optLevel) {
        Write-Host 'BAD  the generated project has no optimization-level property to mutate'
        return 1
    }
    $optOther = if ($optLevel -eq '1') { '0' } else { '1' }

    $mutations = @(
        @{ Name = 'a source is dropped from the project (manifest edited, never regenerated)'
           Expect = 'MISSING from the project'
           Do = { param($t) [regex]::Replace($t,
                    '\s*<itemPath>' + [regex]::Escape('../src/boot/resident_de_boot_crc32.c') + '</itemPath>', '', 1) } },
        @{ Name = 'a source the manifest does not name is added (the IDE can add files)'
           Expect = 'NOT in src/boot/boot_image.psd1'
           Do = { param($t) [regex]::Replace($t,
                    [regex]::Escape('<itemPath>../src/shared/resident_de_pipe.c</itemPath>'),
                    "<itemPath>../src/boot/hal_gpio/nora_gpio_table.c</itemPath>`r`n        <itemPath>../src/shared/resident_de_pipe.c</itemPath>", 1) } },
        # THE ONE THAT MATTERS MOST. MPLAB X offers to add an include directory when a
        # header is not found; accepting once puts ..\app back and undoes reorg step 4
        # while making the build succeed, which is the quietest possible regression.
        @{ Name = '..\app added to the include directories (the bulkhead breached)'
           Expect = 'names the application tree'
           Do = { param($t) $t.Replace('value="..\src\boot;..\src\shared;', 'value="..\src\boot;..\src\shared;..\src\app\hal_uart;') } },
        @{ Name = "an optimization level differing from the manifest (project says '$optLevel')"
           Expect = "the manifest says '$optLevel'"
           Do = { param($t) [regex]::Replace($t,
                    '<property key="optimization-level" value="' + [regex]::Escape($optLevel) + '"/>',
                    '<property key="optimization-level" value="' + $optOther + '"/>', 1) } },
        # "the size verification", not "the 32 KiB verification": the cap is per device
        # since 2026-08-15 (32 KiB on the MPS512, 16 KiB on the MC106 since 2026-08-20,
        # 28 KiB before that), and the mutation
        # disables the step for both configurations at once because the property appears
        # once per conf.
        @{ Name = 'the image-size verification disabled as a post-build step'
           Expect = 'has the post-build step disabled'
           Do = { param($t) [regex]::Replace($t,
                    '<makeCustomizationPostStepEnabled>true</makeCustomizationPostStepEnabled>',
                    '<makeCustomizationPostStepEnabled>false</makeCustomizationPostStepEnabled>', 1) } },
        # One configuration PER DEVICE is expected (two since the AK128 arrived), so the
        # mutation adds a THIRD -- the count the gate compares against is the manifest's
        # device count, not the constant 1 it was when only the AK512 existed.
        @{ Name = 'a configuration the manifest does not name appears (an image nobody checks)'
           Expect = 'A configuration the manifest does not name'
           Do = { param($t) $t.Replace('  </confs>',
                    "    <conf name=`"dsPIC33AK512_RESIDENT_BOOT_EXPERIMENT`" type=`"2`">`r`n    </conf>`r`n  </confs>") } },
        @{ Name = 'the stack guard silently reduced'
           Expect = "the manifest says '64'"
           Do = { param($t) [regex]::Replace($t,
                    '<property key="oXC16ld-stackguard" value="64"/>',
                    '<property key="oXC16ld-stackguard" value="16"/>', 1) } }
    )

    for ($i = 0; $i -lt $mutations.Count; $i++) {
        $m = $mutations[$i]
        $text = & $m.Do $base
        if ($text -eq $base) { Write-Host "BAD  mutation did not change the file (test bug): $($m.Name)"; $bad++; continue }
        $r = Invoke-Gate $text $i
        if ($r.Code -eq 0) { Write-Host "BAD  NOT DETECTED: $($m.Name)"; $bad++; continue }
        if ($r.Out -notmatch [regex]::Escape($m.Expect)) {
            Write-Host "BAD  detected but not as '$($m.Expect)': $($m.Name)`n$($r.Out)"; $bad++; continue
        }
        Write-Host "OK   detected: $($m.Name)"
    }

    # The Makefile lives outside configurations.xml, so these two are separate: MPLAB X
    # does NOT create it, and without it an IDE build dies with "make: Makefile: No such
    # file or directory" AFTER a successful clean -- which reads like a broken toolchain
    # and sent three build attempts and a tour of Project Properties in the wrong
    # direction. This is the gate that turns that into one line naming the file.
    $r = Invoke-Gate $base 'nomk' -NoMakefile
    if ($r.Code -eq 0) { Write-Host 'BAD  NOT DETECTED: the project Makefile is missing'; $bad++ }
    elseif ($r.Out -notmatch 'Makefile is missing') {
        Write-Host "BAD  detected but not as a missing Makefile:`n$($r.Out)"; $bad++
    } else { Write-Host 'OK   detected: the project Makefile is missing' }

    $r = Invoke-Gate $base 'editmk' -MakefileText "build: .build-post`r`n"
    if ($r.Code -eq 0) { Write-Host 'BAD  NOT DETECTED: the project Makefile was hand-edited'; $bad++ }
    elseif ($r.Out -notmatch 'Makefile differs') {
        Write-Host "BAD  detected but not as an edited Makefile:`n$($r.Out)"; $bad++
    } else { Write-Host 'OK   detected: the project Makefile was hand-edited' }

    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    return $bad
}

# ---------------------------------------------------------------------------
# C0.4 -- check_hal_drift.ps1 is a REPORT, and must stay one
# ---------------------------------------------------------------------------
# Unusual test: it asserts the script does NOT fail. Divergence between src/boot/hal_*
# and src/app/hal_* is a question for review, not a defect -- failing a build on it would
# re-create exactly the coupling reorg step 4 removed. So "exit 0 even when it finds
# something" is a behaviour under test, not an accident, and the detection is checked
# through the report text instead of the exit code.
function Test-C04 {
    $gate = Join-Path $repoRoot 'buildtools\check_hal_drift.ps1'
    $bad = 0

    $out = & pwsh -NoProfile -File $gate -Root $repoRoot 2>&1 | Out-String
    $code = $LASTEXITCODE
    if ($code -ne 0) { Write-Host "BAD  the drift report must exit 0 on the real tree (got $code)"; $bad++ }
    elseif ($out -notmatch 'identical:\s*\d+') { Write-Host "BAD  no drift summary in the output:`n$out"; $bad++ }
    else { Write-Host 'OK   real tree reported, exit 0' }

    # Mutate a COPY of the tree: append a byte to one boot-side file and confirm the
    # report both names it and still exits 0.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('c04_drift_' + [Guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        Copy-Item -Recurse -LiteralPath (Join-Path $repoRoot 'src\boot') -Destination (Join-Path $tmp 'src\boot')
        Copy-Item -Recurse -LiteralPath (Join-Path $repoRoot 'src\app\hal_nvm') -Destination (Join-Path $tmp 'src\app\hal_nvm') -Force
        Copy-Item -Recurse -LiteralPath (Join-Path $repoRoot 'src\app\hal_uart') -Destination (Join-Path $tmp 'src\app\hal_uart') -Force
        $victim = Join-Path $tmp 'src\boot\hal_nvm\nora_nvm_dspic33ak.c'
        [IO.File]::AppendAllText($victim, "`r`n/* drift injected by the host test */`r`n")

        $out = & pwsh -NoProfile -File $gate -Root $tmp 2>&1 | Out-String
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Write-Host "BAD  the drift report FAILED the build on a difference (exit $code). It is informational by design."
            $bad++
        } elseif ($out -notmatch 'hal_nvm/nora_nvm_dspic33ak\.c') {
            Write-Host "BAD  injected drift NOT reported:`n$out"; $bad++
        } else {
            Write-Host 'OK   injected drift named in the report, exit still 0'
        }

        # A boot module with no src/app/ counterpart at all -- the "removed or renamed on
        # the application side" case, which is how a vendored copy becomes the only one.
        Remove-Item -Recurse -Force (Join-Path $tmp 'src\app\hal_uart')
        $out = & pwsh -NoProfile -File $gate -Root $tmp 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { Write-Host 'BAD  exit non-zero on a boot-only module'; $bad++ }
        elseif ($out -notmatch 'BOOT ONLY') { Write-Host "BAD  boot-only module not reported:`n$out"; $bad++ }
        else { Write-Host 'OK   boot-only module reported, exit still 0' }
    }
    finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
    return $bad
}

Write-Section 'C0.1  Assert-StandaloneMapLayout (linked result)'
$failures += Test-C01

Write-Section 'C0.2  check_configurations.ps1 (build inputs)'
$failures += Test-C02

Write-Section 'C0.3  check_resident_project.ps1 (generated boot project)'
$failures += Test-C03

Write-Section 'C0.4  check_hal_drift.ps1 (a report, and must stay one)'
$failures += Test-C04

Write-Host ''
if ($failures -gt 0) {
    Write-Host "build_layout_gates: FAILED ($failures problem(s))"
    exit 1
}
Write-Host 'build_layout_gates: ALL TESTS PASSED'
exit 0
