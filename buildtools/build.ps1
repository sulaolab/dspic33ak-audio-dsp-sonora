param(
    [switch]$Help,
    [switch]$Full,
    [switch]$Clean,
    [switch]$Generate,
    [string]$Configuration,
    # Deprecated: the configuration already declares its application and every
    # APP_BUILD variation belongs to exactly one application, so -App carries no
    # information. Still accepted (old notes/scripts) but only cross-checked.
    [ValidateSet('Asrc', 'Classic')]
    [string]$App,
    [string]$Preset,
    # The repository this script belongs to, not the current directory: running
    # from buildtools/ (or from another repo's root) must not change what is
    # built. Same default as switch_config.ps1. Pass -Root to override.
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$ProjectDir,
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 8),
    # The firmware version stamped into the SERIAL_UPDATE_PACKAGE header.
    [int]$FirmwareVersion = 1,
    # Build the application only, and do not produce the .sfb update package or
    # the factory HEX. flashauto.ps1 then has no factory image to flash, so this
    # is for compile-only checks, not for producing something to program.
    [switch]$NoDelivery,
    # Extra preprocessor definitions for this build only, appended to
    # MP_EXTRA_CC_PRE. Accepts "NAME=VALUE" or a bare "NAME", with or without a
    # leading -D, and may be repeated:
    #   -Define APP_ASRC_RUNTIME_48K_TO_8=1
    #   -Define A=1,B=2
    # This is a one-shot override: nothing is stored, so the next unqualified
    # build.ps1 is back to the plain selection. Intended for A/B comparison of a
    # compile-time switch without editing source.
    # Declared LAST on purpose: an array parameter earlier in the list can swallow
    # a following positional argument.
    [string[]]$Define,
    # The same thing for hand-written assembly: extra assembler symbols for this
    # build only, emitted as MP_EXTRA_AS_PRE=-Wa,--defsym=NAME=VALUE. -Define does
    # NOT reach a .s file -- the generated makefile's .s rule expands
    # $(MP_EXTRA_AS_PRE), never MP_EXTRA_CC_PRE -- so an A/B switch inside a .s
    # needs this, and an assembler .ifdef silently takes the OFF branch without it.
    #   -AsDefine APP_ASRC_REPEAT_IRQ_INHIBIT_AB=1
    # A bare NAME is given the value 1, because `--defsym NAME` alone is a
    # syntax error, unlike -D. Same one-shot semantics as -Define: nothing stored.
    [string[]]$AsDefine
)

$ErrorActionPreference = 'Stop'

if ($Help) {
    @'
Usage:
  pwsh ./buildtools/build.ps1 [-Full|-Clean|-Generate] [-Jobs N]
      [-FirmwareVersion N] [-NoDelivery]

Modes:
  (none)     Build the active selection.
  -Full      Generate makefiles, clean, then build.
  -Clean     Clean outputs only.
  -Generate  Generate MPLAB X makefiles only.

Selection:
  Comes from ./buildtools/switch_config.ps1 (no environment variable is
  involved), which stores three choices in buildtools/active_build.json:
  serial update support, target device and application profile. The MPLAB
  configuration is derived from the last two, not chosen.
  A build cleans first only when the profile differs from what the existing
  objects were built with (or that is unknown, e.g. after an IDE build).

Build products:
  FACTORY_IMAGE   dist/<conf>/production/*.factory.production.hex
                  What flashauto.ps1 programs, in both delivery modes. With
                  serial update support it is the resident bootloader + the
                  application + a committed manifest; without, it is the
                  standalone application.
  SERIAL_UPDATE_PACKAGE
                  artifacts/serial_update_packages/<device>/sonora_<device>_<tag>_<time>.sfb
                  Serial update support only. Send it to a board that already
                  has a FACTORY_IMAGE to switch it to this application without
                  a programmer. Kept as history: -Clean / -Full never delete it.
                  Filed per device, and named for it as well, because a package
                  built for the other part is a valid file that only fails once
                  it is on the board.

  With serial update support the application HEX is not programmable on its own
  (its reset vector lives in the bootloader), so it is not offered as a product.
  The resident bootloader is built automatically when missing or older than its
  sources. -FirmwareVersion stamps the package header (default 1). -NoDelivery
  builds the application only and leaves nothing to flash.

Superseded parameters (still accepted):
  -Configuration  one-shot override; its device still decides whether serial
                  update support can apply.
  -Preset         now called a profile; -Preset for the other application
                  remaps the configuration accordingly.
  -App            deprecated and ignored except as a cross-check.
'@ | Write-Output
    return
}

. (Join-Path $PSScriptRoot 'sonora_build_state.ps1')

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Write-Host "==> $Description"
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Resolve-PythonExe {
    <#
      The delivery tools are Python. Prefer an explicit SONORA_PYTHON, then the
      launcher, then python on PATH -- and say what to set if none works, because
      "python is not recognized" mid-build reads like a build failure otherwise.
    #>
    foreach ($candidate in @($env:SONORA_PYTHON, 'py', 'python', 'python3')) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $resolved = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($null -ne $resolved) { return $resolved.Source }
    }
    throw @'
Python was not found, and the delivery artifacts (SERIAL_UPDATE_PACKAGE and
FACTORY_IMAGE) are produced by tools/serial_boot_package.py and
tools/serial_boot_factory_image.py. Install Python, or set SONORA_PYTHON to its
full path. To build the application alone, pass -NoDelivery (nothing to flash).
'@
}

function Get-NewestWriteTime {
    param([string[]]$Paths)

    $newest = [DateTime]::MinValue
    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) { continue }
        foreach ($item in @(Get-ChildItem -LiteralPath $path -Recurse -File -ErrorAction SilentlyContinue)) {
            if ($item.LastWriteTimeUtc -gt $newest) { $newest = $item.LastWriteTimeUtc }
        }
    }
    return $newest
}

function Confirm-ResidentBootloaderHex {
    <#
      Returns the path to a resident bootloader HEX that is at least as new as the
      sources. The resident build is small, so this errs towards rebuilding: any
      source or linker edit refreshes it rather than risking a factory image that
      pairs a current application with a stale bootloader.

      Per device: the bootloader is a different image on each part, and both its
      dist/ directory and the -Device the build script needs come from the boot image
      manifest rather than from a name written here. A hard-coded one would hand the
      other device's bootloader to serial_boot_factory_image.py, which has no way to
      tell -- both are valid HEX for the same 32 KiB boot region.
    #>
    param(
        [string]$RepoRoot,
        [string]$Device,
        [bool]$Full
    )

    $image = Get-SonoraSerialUpdateDeviceEntry -RepoRoot $RepoRoot -Device $Device

    $hex = Join-Path $RepoRoot ('dist\{0}\production\resident_bootloader.production.hex' -f $image.ConfigurationName)
    $reason = $null
    if (-not (Test-Path -LiteralPath $hex)) {
        $reason = 'not built yet'
    } elseif ($Full) {
        $reason = '-Full was requested'
    } else {
        $builtAt = (Get-Item -LiteralPath $hex).LastWriteTimeUtc
        # src\boot is where this image's OWN sources live -- its main, its XMODEM, and
        # since the 2026-08-14 reorg its private copy of the HAL. Leaving it out of this
        # list is the worst kind of miss: the build reports success, the app is fresh, and
        # the bootloader HEX that gets packaged is the one from before the boot change.
        # Measured 2026-08-19: an AK128 boot diet worth 9,672 B stayed invisible here
        # until build_resident_bootloader.ps1 was run by hand with -Full.
        $sourcesAt = Get-NewestWriteTime -Paths @(
            (Join-Path $RepoRoot 'src\boot'),
            (Join-Path $RepoRoot 'src\app'),
            (Join-Path $RepoRoot 'src\shared'),
            (Join-Path $RepoRoot 'src\linker'),
            (Join-Path $RepoRoot 'buildtools\build_resident_bootloader.ps1')
        )
        if ($sourcesAt -gt $builtAt) { $reason = 'sources are newer' }
    }

    if ($null -ne $reason) {
        Write-Host "==> Resident bootloader for $Device ($reason)"
        # A PowerShell script, so a failure arrives as a terminating error under
        # ErrorActionPreference=Stop -- do not test $LASTEXITCODE here, it still
        # holds whatever the last native tool left behind.
        #
        # Out-Host, not a bare call: that script prints the bootloader's memory map,
        # and anything left on the output stream would be returned by this function
        # alongside the path, making the caller pass a whole map dump as arguments.
        $residentBuild = Join-Path $RepoRoot 'buildtools\build_resident_bootloader.ps1'
        & $residentBuild -Root $RepoRoot -Device $image.Device -Full | Out-Host
        if (-not (Test-Path -LiteralPath $hex)) {
            throw "Resident bootloader build did not produce: $hex"
        }
    }

    return (Resolve-Path -LiteralPath $hex).Path
}

function Assert-SerialUpdateLayoutInvariants {
    <#
      The serial-update layout has two owners by design: the linker script owns the
      usable program range, and the MPLAB configuration owns where the IVT is
      generated. That only stays coherent while the IVT sits exactly at the start of
      the program range, and nothing enforces it -- the two live in different files,
      edited for different reasons. So check it, before a build rather than after a
      board misbehaves.

      Both values are read from their real sources: the .gld's program ORIGIN and
      the configuration's vtable address. Which .gld is a per-device fact, and it is
      taken from the boot image manifest -- the same entry that decides which script
      the bootloader is linked against, so the two halves of one device's layout
      cannot be read out of two different files.
    #>
    param(
        [string]$RepoRoot,
        [string]$ProjectDir,
        [string]$Configuration,
        [string]$Device
    )

    $image = Get-SonoraSerialUpdateDeviceEntry -RepoRoot $RepoRoot -Device $Device
    $gld = Join-Path $RepoRoot ($image.LinkerScript -replace '/', '\')
    if (-not (Test-Path -LiteralPath $gld)) {
        throw "Serial-update linker script not found: $gld"
    }
    $gldText = [IO.File]::ReadAllText($gld)
    $originMatch = [regex]::Match(
        $gldText,
        'program\s*\(xr\)\s*:\s*ORIGIN\s*=\s*DEFINED\(__SONORA_PROGRAM_ORIGIN\)\s*\?\s*__SONORA_PROGRAM_ORIGIN\s*:\s*(0x[0-9a-fA-F]+)')
    if (-not $originMatch.Success) {
        throw "Could not read the serial-update program ORIGIN from $gld. If its memory regions were restructured, update this check with them."
    }
    $gldOrigin = [Convert]::ToUInt32($originMatch.Groups[1].Value, 16)

    $confXml = Join-Path $ProjectDir 'nbproject\configurations.xml'
    $confText = [IO.File]::ReadAllText($confXml)
    $start = $confText.IndexOf("<conf name=`"$Configuration`"")
    if ($start -lt 0) { throw "Configuration '$Configuration' not found in $confXml" }
    $end = $confText.IndexOf('</conf>', $start)
    $confSeg = $confText.Substring($start, $end - $start)

    $addrMatch = [regex]::Match($confSeg, '<property key="vtable-address-1" value="(0x[0-9a-fA-F]+)"\s*/>')
    if (-not $addrMatch.Success) {
        throw @"
Configuration '$Configuration' does not set an IVT address (vtable-address-1), but
it is a serial-update configuration. Without it the linker places the IVT at the
device default, inside the resident bootloader's space -- the application then
overwrites the bootloader it was supposed to be launched by. Set
enable-vtable-1=true and vtable-address-1=0x{0:X} in MPLAB X (Linker > vtable).
"@ -f $gldOrigin
    }
    $ivtAddress = [Convert]::ToUInt32($addrMatch.Groups[1].Value, 16)

    if ($ivtAddress -ne $gldOrigin) {
        throw @"
Serial-update layout is inconsistent:
  linker script program ORIGIN : 0x{0:X}   ($gld)
  configuration IVT address    : 0x{1:X}   ($Configuration, vtable-address-1)
The IVT must start at the beginning of the usable program range. Change whichever
of the two is wrong so they agree.
"@ -f $gldOrigin, $ivtAddress
    }
}

function Assert-SerialUpdateMapLayout {
    <#
      After the link: the IVT really landed where the configuration asked, and no
      program section reaches into the resident bootloader or the manifest. The
      linker is not obliged to warn about either -- the packaging tool would later
      refuse the image, which is a much later and less obvious place to find out.
    #>
    param(
        [string]$MapPath,
        [string]$RepoRoot
    )

    $mapText = [IO.File]::ReadAllText($MapPath)

    $region = [regex]::Match($mapText, '"program"\s+Memory\s*\[Origin = (0x[0-9a-fA-F]+), Length = (0x[0-9a-fA-F]+)\]')
    if (-not $region.Success) {
        throw "Could not read the program memory region from $MapPath"
    }
    $origin = [Convert]::ToUInt32($region.Groups[1].Value, 16)
    $length = [Convert]::ToUInt32($region.Groups[2].Value, 16)
    $limit = $origin + $length - 1

    $ivt = [regex]::Match($mapText, '__ivt_0\s*:\s*(0x[0-9a-fA-F]+)')
    if (-not $ivt.Success) {
        throw "Could not read the IVT address from $MapPath"
    }
    $ivtAddress = [Convert]::ToUInt32($ivt.Groups[1].Value, 16)
    if ($ivtAddress -ne $origin) {
        throw ('Linked IVT is at 0x{0:X}, but the usable program range starts at 0x{1:X}. ' +
               'The application would overwrite the resident bootloader.') -f $ivtAddress, $origin
    }

    Write-Host ("    IVT 0x{0:X}, program range 0x{1:X}..0x{2:X} (bootloader and manifest untouched)" -f
                $ivtAddress, $origin, $limit)
}

function Get-MapDataAllocations {
    <#
      The linker map's allocation summary as objects -- name, address, size -- for the
      sections that land in one address window. One line per section normally; a name
      long enough to have been wrapped onto its own line is joined with the address
      line that follows it, because a section this parser silently failed to see would
      be reported below as a hole in RAM.

      Only the summary table matches: the per-input-section listing further down the
      map has no "(decimal)" column, and the symbol tables have no size column.
    #>
    param(
        [string]$MapText,
        [uint32]$Low,
        [uint32]$High
    )

    $found = New-Object System.Collections.Generic.List[object]
    $pending = $null
    foreach ($line in ($MapText -split "`r?`n")) {
        $name = $null
        if ($line -match '^(\S+)\s+(0x[0-9a-fA-F]+)\s+\S+\s+(0x[0-9a-fA-F]+)\s+\(\d+\)\s*$') {
            $name = $Matches[1]; $addr = $Matches[2]; $size = $Matches[3]
        } elseif (($null -ne $pending) -and
                  ($line -match '^\s+(0x[0-9a-fA-F]+)\s+\S+\s+(0x[0-9a-fA-F]+)\s+\(\d+\)\s*$')) {
            $name = $pending; $addr = $Matches[1]; $size = $Matches[2]
        } elseif ($line -match '^(\S+)\s*$') {
            $pending = $Matches[1]; continue
        }
        $pending = $null
        if ($null -eq $name) { continue }
        $a = [Convert]::ToUInt32($addr.Substring(2), 16)
        $z = [Convert]::ToUInt32($size.Substring(2), 16)
        if (($z -gt 0) -and ($a -ge $Low) -and ($a -lt $High)) {
            $found.Add([pscustomobject]@{ Name = $name; Address = $a; Size = $z })
        }
    }
    return ,@($found | Sort-Object Address)
}

function Assert-DataTopClaimed {
    <#
      What the old check said: "the stack ends exactly at the diagnostic block". That
      was two invariants written as one equality, and it only held while the stack was
      the last thing in RAM. Section-attributed data (space(ymemory)) is allocated
      from the top of the Y window DOWNWARD, so the stack now sits in the hole below
      it and the equality fails on a perfectly correct layout.

      The two invariants, separated, are both still checkable:

        1. The stack never reaches into the diagnostic block -- above it the stack
           would overwrite the trap record on the way to reporting it -- and it does
           not overlap allocated data either.
        2. Every byte between the top of the stack and the diagnostic block is claimed
           by an allocated section. That is what the equality really bought: no
           unaccounted hole at the top of RAM, and no orphan parked in the reservation.

      Plus a floor under the stack itself, because the stack is no longer "whatever is
      left" and can now be squeezed by a data section with nothing complaining. An
      actual overflow is still caught by SPLIM and reported by _StackError, so the
      floor is a margin check rather than the only defence.
    #>
    param(
        [string]$MapText,
        [uint32]$RamTop,
        [uint32]$DiagnosticBase,
        [uint32]$StackStart,
        [uint32]$StackLength,
        [uint32]$MinStack
    )

    $stackEnd = $StackStart + $StackLength
    $findings = New-Object System.Collections.Generic.List[string]

    if ($stackEnd -gt $DiagnosticBase) {
        $findings.Add(('the stack runs to 0x{0:X}, into the reset-diagnostic block at 0x{1:X}' -f
                       $stackEnd, $DiagnosticBase))
    }
    if ($StackLength -lt $MinStack) {
        $findings.Add((('only 0x{0:X} ({0}) bytes of stack are left, under the 0x{1:X} ({1}) ' +
                        'floor. Something new in RAM took it -- shrink that, do not lower this') -f
                       $StackLength, $MinStack))
    }

    $sections = Get-MapDataAllocations -MapText $MapText -Low 0x4000 -High $RamTop
    foreach ($section in $sections) {
        if (($section.Address -lt $stackEnd) -and
            (($section.Address + $section.Size) -gt $StackStart)) {
            $findings.Add(('{0} at 0x{1:X}..0x{2:X} overlaps the stack 0x{3:X}..0x{4:X}' -f
                           $section.Name, $section.Address, ($section.Address + $section.Size),
                           $StackStart, $stackEnd))
        }
    }

    $cursor = $stackEnd
    foreach ($section in $sections) {
        if ($cursor -ge $DiagnosticBase) { break }
        if (($section.Address + $section.Size) -le $cursor) { continue }
        if ($section.Address -gt $cursor) { break }
        $cursor = $section.Address + $section.Size
    }
    if ($cursor -lt $DiagnosticBase) {
        $findings.Add((('0x{0:X}..0x{1:X} ({2} bytes) above the stack is allocated to nothing. ' +
                        'RAM that neither the stack nor a section owns is RAM the next ' +
                        'allocation may quietly be given') -f
                       $cursor, $DiagnosticBase, ($DiagnosticBase - $cursor)))
    }

    if ($findings.Count -gt 0) {
        throw ("SERIAL_UPDATE_APP data layout assertion failed:`n  - " +
               ($findings -join "`n  - "))
    }

    Write-Host (("    stack 0x{0:X}..0x{1:X} ({2} bytes), RAM above it claimed up to the " +
                 "diagnostic block 0x{3:X}") -f $StackStart, $stackEnd, $StackLength, $DiagnosticBase)
}

function Assert-Q31ResamplerPlacement {
    <#
      The same operand-placement contract as Assert-Q31FrontEndPlacement, for the
      Q31 generic polyphase resampler (ASRC_SAMPLE_Q31). mchp_asrc_q31_row16 reads
      the sample history through one AGU and the blended coefficient row through the
      other, so the two must be in DIFFERENT spaces or every MAC costs an extra cycle
      with no other symptom (DS70005591C 4.3.17).

      The asymmetry is deliberate and is what makes this checkable: the 20 KB engine
      state stays where it lands in X, and only the ~120-byte blended row is forced to
      Y with space(ymemory). So the assertion is: s_asrc entirely below __YDATA_BASE,
      s_ceff_q31 entirely inside Y. Absent symbols are not a failure -- the float
      build has neither. One of the two missing IS a failure.
    #>
    param([string]$MapText)

    $yBase = [regex]::Match($MapText, '(?m)__YDATA_BASE\s*=\s*(0x[0-9a-fA-F]+)')
    $yEnd = [regex]::Match($MapText, '(?m)__YDATA_END\s*=\s*(0x[0-9a-fA-F]+)')
    if (-not ($yBase.Success -and $yEnd.Success)) {
        throw 'Could not read __YDATA_BASE/__YDATA_END from the final map.'
    }
    $yLow = [Convert]::ToUInt32($yBase.Groups[1].Value.Substring(2), 16)
    $yHigh = [Convert]::ToUInt32($yEnd.Groups[1].Value.Substring(2), 16)

    $all = Get-MapDataAllocations -MapText $MapText -Low 0x4000 -High $yHigh
    $ceff = @($all | Where-Object { $_.Name -match '(^|\.)s_ceff_q31$' })
    $state = @($all | Where-Object { $_.Name -match '(^|\.)s_asrc$' })

    if ($ceff.Count -eq 0) { return }   # float build: nothing to place
    if ($state.Count -ne 1) {
        throw ('Q31 resampler: s_ceff_q31 is allocated but s_asrc was not found ' +
               'once in the map ({0} matches) -- the history placement cannot be proved.' -f
               $state.Count)
    }

    $findings = New-Object System.Collections.Generic.List[string]
    if (($state[0].Address + $state[0].Size) -gt $yLow) {
        $findings.Add((('the engine state (sample history) is at 0x{0:X}..0x{1:X}, which ' +
                        'crosses into Y space at 0x{2:X}: the MAC operands would share a bus ' +
                        'and every MAC would cost an extra cycle') -f
                       $state[0].Address, ($state[0].Address + $state[0].Size), $yLow))
    }
    if (($ceff[0].Address -lt $yLow) -or (($ceff[0].Address + $ceff[0].Size) -gt $yHigh)) {
        $findings.Add((('the blended coefficient row is at 0x{0:X}..0x{1:X}, not inside Y ' +
                        'space 0x{2:X}..0x{3:X} -- space(ymemory) was not honoured') -f
                       $ceff[0].Address, ($ceff[0].Address + $ceff[0].Size), $yLow, $yHigh))
    }
    if ($findings.Count -gt 0) {
        throw ("Q31 resampler placement assertion failed:`n  - " + ($findings -join "`n  - "))
    }

    Write-Host (("    Q31 resampler: history 0x{0:X} (+{1}) in X, blended row 0x{2:X} " +
                 "(+{3}) in Y") -f $state[0].Address, $state[0].Size, $ceff[0].Address, $ceff[0].Size)
}

function Assert-Q31FrontEndPlacement {
    <#
      The Q31 decimation front end is only as fast as its operand placement: the
      dual-fetch MAC reads one operand through the X RAGU and one through the Y AGU,
      so coefficients and history must be in DIFFERENT spaces or every MAC costs one
      extra cycle (measured 1.012 -> 2.000 cycles/MAC, which is the difference between
      sixteen channels fitting and not fitting).

      space(xmemory)/space(ymemory) on the definitions is what asks for that; this is
      what proves it was granted. Nothing warns when an edit drops an attribute, and
      the cost then shows up only as a load figure nobody was watching that day. The
      X/Y boundary is read from the map's own __YDATA_BASE, so this holds on any part.

      Absent symbols are not a failure: a configuration without the Q31 front end has
      nothing to place. Half of it missing IS a failure.
    #>
    param([string]$MapText)

    $yBase = [regex]::Match($MapText, '(?m)__YDATA_BASE\s*=\s*(0x[0-9a-fA-F]+)')
    $yEnd = [regex]::Match($MapText, '(?m)__YDATA_END\s*=\s*(0x[0-9a-fA-F]+)')
    if (-not ($yBase.Success -and $yEnd.Success)) {
        throw 'Could not read __YDATA_BASE/__YDATA_END from the final map.'
    }
    $yLow = [Convert]::ToUInt32($yBase.Groups[1].Value.Substring(2), 16)
    $yHigh = [Convert]::ToUInt32($yEnd.Groups[1].Value.Substring(2), 16)

    $all = Get-MapDataAllocations -MapText $MapText -Low 0x4000 -High $yHigh
    $coeff = @($all | Where-Object { $_.Name -match '(^|\.)s_q31_coeff$' })
    $hist = @($all | Where-Object { $_.Name -match '(^|\.)s_q31_hist$' })

    if (($coeff.Count -eq 0) -and ($hist.Count -eq 0)) { return }
    if (($coeff.Count -ne 1) -or ($hist.Count -ne 1)) {
        throw (('Q31 front end is half-allocated in the final map: {0} coefficient ' +
                'workspace, {1} history. Both or neither.') -f $coeff.Count, $hist.Count)
    }

    $findings = New-Object System.Collections.Generic.List[string]
    if (($coeff[0].Address + $coeff[0].Size) -gt $yLow) {
        $findings.Add((('the coefficient workspace is at 0x{0:X}..0x{1:X}, not inside X space ' +
                        '0x4000..0x{2:X} -- space(xmemory) was not honoured') -f
                       $coeff[0].Address, ($coeff[0].Address + $coeff[0].Size), $yLow))
    }
    if (($hist[0].Address -lt $yLow) -or (($hist[0].Address + $hist[0].Size) -gt $yHigh)) {
        $findings.Add((('the history is at 0x{0:X}..0x{1:X}, not inside Y space 0x{2:X}..0x{3:X} ' +
                        '-- space(ymemory) was not honoured, and the Y AGU modulo the kernel ' +
                        'programs would then address the wrong memory') -f
                       $hist[0].Address, ($hist[0].Address + $hist[0].Size), $yLow, $yHigh))
    }
    if ($findings.Count -gt 0) {
        throw ("Q31 front-end placement assertion failed:`n  - " + ($findings -join "`n  - "))
    }

    Write-Host ("    Q31 front end: coefficients 0x{0:X} (+{1}) in X, history 0x{2:X} (+{3}) in Y" -f
                $coeff[0].Address, $coeff[0].Size, $hist[0].Address, $hist[0].Size)
}

function Assert-ModuloWindowSafety {
    <#
      The Q31 kernel programs MODCON/YMODSRT/YMODEND, and those registers are NOT
      part of the per-IPL register context (DS70005591C Table 4-2): while the
      kernel's ring window is open, it is open in every context, including an
      interrupt that preempts it. The kernel brackets its own use, which is what
      makes that safe -- but only as long as nothing else in the image addresses
      through an AGU from interrupt context (it would inherit the open window) and
      nothing else writes the modulo registers at all.

      That is a property of the code that happens to be linked in. It changes when
      an ISR does, silently, and it cannot be held by a comment -- so it is read
      back off the disassembly of the image that was just built. The gate exits
      non-zero on findings AND when it cannot run: a check that cannot run must not
      read as a pass.
    #>
    param([string]$ElfPath, [string]$RepoRoot)

    $gate = Join-Path $RepoRoot 'tools/asrc/ymod_safety_gate.py'
    if (-not (Test-Path -LiteralPath $gate)) {
        throw "Y-modulo safety gate is missing: $gate"
    }
    if (-not (Test-Path -LiteralPath $ElfPath)) {
        throw "Expected linked image was not created: $ElfPath"
    }

    $python = Resolve-PythonExe
    Invoke-CheckedCommand -Description 'Y-modulo window safety gate' -Command {
        & $python $gate $ElfPath
    }
}

function Assert-StandaloneMapLayout {
    <#
      The mirror of Assert-SerialUpdateMapLayout, for the standalone
      configurations -- of which the project currently has NONE: the two AK512 ones
      were deleted when they gained serial update, and dsPIC33AK128 was replaced by
      dsPIC33AK128_SERIAL_UPDATE on 2026-08-15. This is kept, not deleted with them,
      because standalone survives as a delivery MODE and the next part to arrive
      arrives without a bootloader. It is the more important of the two, because a standalone
      build has no packaging step to catch a mistake later. If the download engine
      leaks in, or the layout quietly shifts to the serial-update window, the build
      still succeeds and produces a programmable HEX; the symptom appears on a
      board.

      The claim being defended is the one the resident_de folder split was for: with
      delivery support off, the tree must be indistinguishable from a build that has
      no bootloader concept at all. That is asserted here rather than argued in a
      document, and asserted on the LINKED result -- the map -- not on the sources
      or the configuration, because those are the inputs, and it is the output that
      gets flashed.

      Every value is read from the map, including the link command line the linker
      echoes into its head, so nothing here duplicates a setting that could drift.
      All findings are collected and reported together: a layout regression normally
      shows several symptoms at once, and discovering them one rebuild at a time is a
      slow way to learn that.
    #>
    param(
        [string]$MapPath,
        [string]$Configuration,
        [string]$RepoRoot
    )

    # The device default program ORIGIN for this family: 0x800000 is the reset
    # instruction, so the usable range starts at 0x800004. Every AK part in the
    # fleet agrees (512 and 128 differ only in Length). A device whose default is
    # elsewhere must update this constant rather than have the check silently pass.
    $standaloneProgramOrigin = 0x800004

    $mapText = [IO.File]::ReadAllText($MapPath)
    $problems = [System.Collections.Generic.List[string]]::new()

    # --- The program region is the device default, not the serial-update window ---
    $region = [regex]::Match($mapText, '"program"\s+Memory\s*\[Origin = (0x[0-9a-fA-F]+), Length = (0x[0-9a-fA-F]+)\]')
    if (-not $region.Success) {
        throw "Could not read the program memory region from $MapPath"
    }
    $origin = [Convert]::ToUInt32($region.Groups[1].Value, 16)
    $length = [Convert]::ToUInt32($region.Groups[2].Value, 16)
    if ($origin -ne $standaloneProgramOrigin) {
        # Note on style throughout this function: every message is formatted into a
        # local and only then added. Writing $problems.Add($fmt -f $a, $b) instead
        # parses the comma as a METHOD ARGUMENT separator, so the format string gets
        # one argument and dies with "Index ... less than the size of the argument
        # list". That still counts as a thrown assertion, which is precisely how a
        # broken gate can pass for a working one -- it was caught here only because
        # the self-test checks WHY the negative cases fail, not just that they do.
        $text = 'program ORIGIN is 0x{0:X}, expected the device default 0x{1:X}. ' +
                'A standalone image owns Flash from the reset vector up; only a ' +
                'serial-update application starts above the resident bootloader.'
        $text = $text -f $origin, $standaloneProgramOrigin
        $problems.Add($text)
    }

    # --- The IVT was not relocated ---
    # The serial-update configurations pass --ivt=<address> (Linker > vtable). A
    # standalone build must not: its IVT belongs at the device default. Note the
    # bare --ivt and --isr flags are present in BOTH, so the '=' is the discriminator.
    $ivtRelocation = [regex]::Match($mapText, '--ivt=(0x[0-9a-fA-F]+)')
    if ($ivtRelocation.Success) {
        $text = 'the link relocates the IVT (--ivt={0}). That is a serial-update ' +
                'setting; enable-vtable-1 must be false in a standalone configuration.'
        $text = $text -f $ivtRelocation.Groups[1].Value
        $problems.Add($text)
    }
    $ivt = [regex]::Match($mapText, '__ivt_0\s+(0x[0-9a-fA-F]+)')
    if (-not $ivt.Success) {
        throw "Could not read the IVT address from $MapPath"
    }
    $ivtAddress = [Convert]::ToUInt32($ivt.Groups[1].Value, 16)
    if ($ivtAddress -ne $origin) {
        $text = 'linked IVT is at 0x{0:X} but the program range starts at 0x{1:X}.'
        $text = $text -f $ivtAddress, $origin
        $problems.Add($text)
    }

    # --- The serial-update linker script was not used ---
    $gldUse = [regex]::Match($mapText, '-T(\S*serial_update\S*)')
    if ($gldUse.Success) {
        $text = 'the serial-update linker script is in the link ({0}). It is registered ' +
                'as a project linker file and must stay excluded from the standalone ' +
                'configurations.'
        $text = $text -f $gldUse.Groups[1].Value
        $problems.Add($text)
    }

    # --- No serial-update defsym reached the linker ---
    $defsym = [regex]::Match($mapText, '--defsym=(\S*SONORA\S*)')
    if ($defsym.Success) {
        $text = 'a Sonora layout defsym was passed to the linker (--defsym={0}). ' +
                'The standalone layout takes no overrides.'
        $text = $text -f $defsym.Groups[1].Value
        $problems.Add($text)
    }

    # --- Nothing from the download engine was linked ---
    # Broad on purpose: object list, output sections and symbol table are all in the
    # map, so one scan covers objects that were compiled in, sections that were
    # placed, and symbols that survived --gc-sections. Measured on the merged tree,
    # a standalone map contains ZERO occurrences -- so any hit is a real leak, not
    # noise to be filtered down to a whitelist.
    #
    # The one legitimate source of the word is the clone directory itself, whose name
    # may well contain it. No current map mentions it, but it would appear if a path in
    # the link were ever absolute, so those lines are dropped -- renaming the checkout
    # can never turn this gate into a false alarm.
    $repoLeaf = Split-Path -Leaf $RepoRoot
    $engineHits = [System.Collections.Generic.List[string]]::new()
    foreach ($line in ($mapText -split "`r?`n")) {
        if ($line -match [regex]::Escape($repoLeaf)) { continue }
        # 'noinit_ram' was in this list until 2026-08-12, when the block stopped being a
        # delivery-only facility: app_traps.c stores the hardware trap record in it and is
        # now compiled by every configuration, so a standalone map is SUPPOSED to contain
        # the block. It is checked positively below instead. 'resident' and 'nora_nvm'
        # remain, and they are what the claim in this function's header is about.
        if ($line -match '(?i)resident|nora_nvm') {
            $engineHits.Add($line.Trim())
        }
    }
    if ($engineHits.Count -gt 0) {
        $shown = ($engineHits | Select-Object -First 8) -join "`n    "
        $more = if ($engineHits.Count -gt 8) { "`n    ... and $($engineHits.Count - 8) more" } else { '' }
        $text = "the download engine leaked into the standalone image " +
                "($($engineHits.Count) reference(s) to resident / NVM / noinit):`n    " +
                $shown + $more + "`n    Check the resident_de / hal_nvm / hal_noinit_ram " +
                "exclusions for this configuration in configurations.xml -- MPLAB X has " +
                "dropped ex=`"true`" attributes before."
        $problems.Add($text)
    }

    # --- The noinit block IS reserved, at the top of RAM, and the stack stops below it ---
    # These two used to be one check with the opposite expectation: "the stack must run to
    # the very top of the data region", because only a serial-update image had a tenant up
    # there. Since 2026-08-12 every configuration stores the hardware trap record in the
    # block, so the standalone claim changed shape -- the tenant is expected, and what must
    # not appear is the DOWNLOAD ENGINE (checked above). The two facts asserted here are
    # the ones that fail silently: a reservation that did not happen (the supplementary
    # linker script lost from the configuration's extra options), and a reservation the
    # stack was allowed to overlap anyway.
    #
    # Everything is derived from the map's own data region, so this is device-independent:
    # the AK512's block is at 0x13F30 and the AK128's at 0x7F30, and both are "top of RAM
    # minus the block size". Only the size is a constant, and it is the same constant
    # src/shared/noinit_ram_config.h states -- deliberately duplicated, because an assertion that
    # reads its expectation from the thing it is checking asserts nothing.
    $noinitBlockSize = 0xD0

    $dataRegion = [regex]::Match($mapText, '"data"\s+Memory\s*\[Origin = (0x[0-9a-fA-F]+), Length = (0x[0-9a-fA-F]+)\]')
    if (-not $dataRegion.Success) {
        throw "Could not read the data memory region from $MapPath"
    }
    $ramTop = [Convert]::ToUInt32($dataRegion.Groups[1].Value, 16) +
              [Convert]::ToUInt32($dataRegion.Groups[2].Value, 16)
    $expectedBlockStart = $ramTop - $noinitBlockSize

    $block = [regex]::Match($mapText, '(?m)^\.noinit_ram\s+(0x[0-9a-fA-F]+)\s+\S+\s+(0x[0-9a-fA-F]+)\b')
    if (-not $block.Success) {
        $text = 'the .noinit_ram block is not in the map at all. app_traps.c stores the ' +
                'hardware trap record there in every configuration, so a standalone build ' +
                'needs the supplementary reservation script in its linker extra options ' +
                '(-Wl,-T../src/linker/p33AK*_noinit_ram_reserve.ld). Without it the range is ' +
                'not held back and the automatic stack is free to take it.'
        $problems.Add($text)
    } else {
        $blockStart = [Convert]::ToUInt32($block.Groups[1].Value, 16)
        $blockSize = [Convert]::ToUInt32($block.Groups[2].Value, 16)
        if ($blockStart -ne $expectedBlockStart -or $blockSize -ne $noinitBlockSize) {
            $text = 'the .noinit_ram block is at 0x{0:X} size 0x{1:X}; expected 0x{2:X} ' +
                    'size 0x{3:X} (top of this device''s RAM 0x{4:X} minus the block size). ' +
                    'The linker script that reserves it and src/shared/noinit_ram_config.h must ' +
                    'agree, and one of them does not.'
            $text = $text -f $blockStart, $blockSize, $expectedBlockStart, $noinitBlockSize, $ramTop
            $problems.Add($text)
        }
    }

    $stack = [regex]::Match($mapText, '(?m)^stack\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s')
    if (-not $stack.Success) {
        throw "Could not read the stack range from $MapPath"
    }
    $stackEnd = [Convert]::ToUInt32($stack.Groups[1].Value, 16) +
                [Convert]::ToUInt32($stack.Groups[2].Value, 16)
    if ($stackEnd -ne $expectedBlockStart) {
        $text = 'the stack ends at 0x{0:X}, not at the bottom of the noinit block 0x{1:X}. ' +
                'Above 0x{1:X} the stack would overwrite the trap record on the way to ' +
                'reporting it; below, the difference is RAM nobody owns.'
        $text = $text -f $stackEnd, $expectedBlockStart
        $problems.Add($text)
    }

    if ($problems.Count -gt 0) {
        $listed = ($problems | ForEach-Object { "  - $_" }) -join "`n"
        throw @"
Standalone layout assertion failed for configuration '$Configuration'.
This build is supposed to be indistinguishable from one with no bootloader
concept, and it is not:

$listed

Map: $MapPath
"@
    }

    # Same trap in the other direction: '"a" + "b" -f $x' binds -f to the LAST string
    # only, leaving {0}..{2} unexpanded in the output. Compose, then format.
    $ok = "    Standalone layout OK: program 0x{0:X}..0x{1:X}, IVT 0x{2:X} (device default), " +
          "noinit block 0x{3:X}..0x{4:X} with the stack below it, no download-engine code linked"
    Write-Host ($ok -f $origin, ($origin + $length - 1), $ivtAddress, $expectedBlockStart, ($ramTop - 1))
}

function Get-SonoraFactoryImagePath {
    param(
        [string]$ProjectDir,
        [string]$Configuration
    )

    $projectName = Split-Path -Leaf $ProjectDir
    return (Join-Path $ProjectDir "dist\$Configuration\production\$projectName.factory.production.hex")
}

function Save-SerialUpdatePackage {
    <#
      Copies the freshly built package into the history area under a name that says
      which profile it is and when it was built, and returns that path.

      History, not a build output: switching profiles back and forth is the whole
      point of serial update, so the packages you might switch to must outlive the
      build that produced them (and -Clean; see the directory choice below).
      Collisions within the same minute get a _NN suffix rather than overwriting -
      two builds in one minute are usually a change worth keeping both of.

      SEPARATED PER DEVICE, BOTH WAYS: a subdirectory per device AND the device in the
      file name. The subdirectory is what makes the history browsable -- "which AK128
      packages do I have" is a directory listing rather than a reading of names -- and
      the device in the name is what survives the file being dragged out of it, which is
      exactly what happens on the way to an XMODEM send. Neither alone is enough: an
      AK512 package sent to an AK128 does not fail as a wrong file, it is refused later
      by the manifest's layout_id, or (before that existed) accepted and run.

      The device tag is the part number with the 'dsPIC33' shared by every part of this
      family folded away -- ak512mps512 / ak128mc106. The whole part number rather than
      just the size: the two parts differ in family too (MP vs MC, different device
      packs), and a name that said only "ak128" would stop being unambiguous the day a
      second 128 KiB part arrives.
    #>
    param(
        [string]$RepoRoot,
        [string]$Device,
        [string]$Package,
        [string]$ArtifactTag,
        [string]$Timestamp
    )

    $deviceTag = ($Device -replace '^(dsPIC)?33AK', 'ak').ToLowerInvariant()
    $historyDir = Join-Path (Join-Path $RepoRoot 'artifacts\serial_update_packages') $deviceTag
    New-Item -ItemType Directory -Force -Path $historyDir | Out-Null

    $stem = 'sonora_{0}_{1}_{2}' -f $deviceTag, $ArtifactTag, $Timestamp
    $target = Join-Path $historyDir ("$stem.sfb")
    if (Test-Path -LiteralPath $target) {
        for ($n = 2; $n -le 99; $n++) {
            $candidate = Join-Path $historyDir ("{0}_{1:D2}.sfb" -f $stem, $n)
            if (-not (Test-Path -LiteralPath $candidate)) { $target = $candidate; break }
            if ($n -eq 99) { throw "Too many packages for $stem; clean out $historyDir." }
        }
    }

    Copy-Item -LiteralPath $Package -Destination $target -Force
    return $target
}

function Save-FactoryImageMetadata {
    <#
      Records what this FACTORY_IMAGE actually is, beside it. flashauto.ps1 compares
      this against the active selection so that changing the selection and flashing
      without rebuilding is refused instead of quietly programming the old image.
    #>
    param(
        [string]$FactoryImage,
        [bool]$SerialUpdateSupport,
        [string]$Device,
        [string]$ProfileName,
        [string]$Configuration,
        [string]$RepoRoot
    )

    $gitCommit = ''
    try {
        $gitCommit = (& git -C $RepoRoot rev-parse --short HEAD 2>$null)
        if ($LASTEXITCODE -ne 0) { $gitCommit = '' }
    } catch { $gitCommit = '' }

    $payload = [ordered]@{
        serial_update_support  = $SerialUpdateSupport
        device                 = $Device
        application_profile    = $ProfileName
        resolved_configuration = $Configuration
        git_commit             = [string]$gitCommit
        built_at               = (Get-Date).ToString('o')
    }

    $json = ($payload | ConvertTo-Json -Depth 4) -replace '(?<!\r)\n', "`r`n"
    [System.IO.File]::WriteAllText(
        ([IO.Path]::ChangeExtension($FactoryImage, '.json')),
        $json + "`r`n",
        [System.Text.UTF8Encoding]::new($false))
}

function New-StandaloneFactoryImage {
    <#
      Serial update support = No. The application boots from the reset vector and is
      the whole image, so the FACTORY_IMAGE is that HEX under the one name
      flashauto.ps1 looks for. Normalised rather than left as production.hex so that
      "what gets flashed" has a single spelling in both delivery modes.
    #>
    param(
        [string]$AppHex,
        [string]$FactoryImage
    )

    if (-not (Test-Path -LiteralPath $AppHex)) {
        throw "Expected application HEX was not created: $AppHex"
    }
    Copy-Item -LiteralPath $AppHex -Destination $FactoryImage -Force
}

function New-SerialUpdateArtifacts {
    <#
      Serial update support = Yes. Produces the update package and the factory image
      (resident bootloader + application + committed manifest), verifies the factory
      image, and files the package in the history area.

      Returns @{ FactoryImage; Package; ResidentHex }.
    #>
    param(
        [string]$RepoRoot,
        [string]$Device,
        [string]$AppHex,
        [string]$AppMap,
        [string]$FactoryImage,
        [string]$ArtifactTag,
        [int]$FirmwareVersion,
        [bool]$Full
    )

    if (-not (Test-Path -LiteralPath $AppHex)) {
        throw "Expected serial-update application HEX was not created: $AppHex"
    }
    if ([string]::IsNullOrWhiteSpace($ArtifactTag)) {
        throw 'This profile has no artifact tag, so its package could not be named. See switch_config.ps1 -CheckProfiles.'
    }

    $python = Resolve-PythonExe
    $residentHex = Confirm-ResidentBootloaderHex -RepoRoot $RepoRoot -Device $Device -Full $Full

    $package = [IO.Path]::ChangeExtension($AppHex, '.sfb')
    $packageTool = Join-Path $RepoRoot 'tools\serial_boot_package.py'
    $factoryTool = Join-Path $RepoRoot 'tools\serial_boot_factory_image.py'

    Invoke-CheckedCommand -Description "SERIAL_UPDATE_PACKAGE (firmware version $FirmwareVersion)" -Command {
        & $python $packageTool build $AppHex $AppMap -o $package --firmware-version $FirmwareVersion
    }
    Invoke-CheckedCommand -Description 'FACTORY_IMAGE (resident bootloader + application + manifest)' -Command {
        & $python $factoryTool build $residentHex $package -o $FactoryImage
    }
    # Separate step on purpose: a produced factory image is not a verified one, and
    # only a verified image should ever reach a board.
    Invoke-CheckedCommand -Description 'Verify FACTORY_IMAGE' -Command {
        & $python $factoryTool verify $FactoryImage
    }

    $saved = Save-SerialUpdatePackage `
        -RepoRoot $RepoRoot `
        -Device $Device `
        -Package $package `
        -ArtifactTag $ArtifactTag `
        -Timestamp (Get-Date).ToString('yyyyMMddHHmm')

    return @{
        FactoryImage = $FactoryImage
        Package      = $saved
        ResidentHex  = $residentHex
    }
}

function Get-MplabXInstallRoots {
    $roots = @()
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not [string]::IsNullOrWhiteSpace($base)) {
            $roots += (Join-Path $base 'Microchip\MPLABX')
        }
    }
    $roots += @(
        'C:\Program Files\Microchip\MPLABX',
        'C:\Program Files (x86)\Microchip\MPLABX'
    )
    $roots = $roots | Select-Object -Unique

    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        Get-ChildItem -LiteralPath $root -Directory -Filter 'v*' -ErrorAction SilentlyContinue |
            ForEach-Object {
                $versionText = $_.Name.TrimStart('v')
                $version = $null
                if (-not [System.Version]::TryParse($versionText, [ref]$version)) {
                    return
                }

                [pscustomobject]@{
                    Path = $_.FullName
                    Version = $version
                    VersionLabel = $_.Name
                }
            }
    }
}

function Get-MplabXVersionLabelFromPath {
    param(
        [string]$Path
    )

    if ($Path -match '[\\/]MPLABX[\\/](v[^\\/]+)[\\/]') {
        return $matches[1]
    }

    return 'custom'
}

function Resolve-MplabXTool {
    param(
        [string]$ToolName,
        [string]$RelativePath,
        [string]$OverridePath,
        [string]$OverrideVariable
    )

    if (-not [string]::IsNullOrWhiteSpace($OverridePath)) {
        $resolvedOverridePath = $OverridePath
        if (Test-Path -LiteralPath $resolvedOverridePath) {
            $resolvedOverridePath = (Resolve-Path -LiteralPath $resolvedOverridePath).Path
        } else {
            throw "$ToolName not found: $resolvedOverridePath ($OverrideVariable)"
        }

        return [pscustomobject]@{
            Path = $resolvedOverridePath
            VersionLabel = Get-MplabXVersionLabelFromPath -Path $resolvedOverridePath
            Source = $OverrideVariable
        }
    }

    $toolMatches = @(Get-MplabXInstallRoots |
        ForEach-Object {
            $toolPath = Join-Path $_.Path $RelativePath
            if (Test-Path -LiteralPath $toolPath) {
                [pscustomobject]@{
                    Path = $toolPath
                    Version = $_.Version
                    VersionLabel = $_.VersionLabel
                    Source = $_.Path
                }
            }
        } |
        Sort-Object -Property Version -Descending)

    if ($toolMatches.Count -eq 0) {
        throw "$ToolName not found. Install MPLAB X or set $OverrideVariable."
    }

    return $toolMatches[0]
}

function Remove-ProjectRootIntermediates {
    param(
        [string]$ProjectDir
    )

    $patterns = @(
        '*.d',
        '*.i',
        '*.s',
        '*.o',
        '*.obj',
        '*.lst',
        '*.map',
        '*.elf',
        '*.hex',
        '*.hxl',
        '*.cof',
        'p33*MPS*.*.00'
    )
    $removedCount = 0
    $lockedFiles = @()

    foreach ($pattern in $patterns) {
        Get-ChildItem -LiteralPath $ProjectDir -File -Filter $pattern -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                $path = $_.FullName
                try {
                    Remove-Item -LiteralPath $path -Force -ErrorAction Stop
                    $removedCount++
                } catch {
                    $lockedFiles += $path
                }
            }
    }

    if ($lockedFiles.Count -gt 0) {
        Write-Host "WARNING: $($lockedFiles.Count) intermediate file(s) still locked in project root:"
        $lockedFiles | ForEach-Object { Write-Host "  $_" }
        throw "Clean incomplete: locked project-root intermediate file(s)."
    }

    Write-Host "cleaned project-root intermediate files: $removedCount"
}

function Normalize-TrackedGeneratedMakefiles {
    param(
        [string]$ProjectDir
    )

    # MPLAB's generator may emit LF and trailing whitespace here. Keep the tracked
    # file deterministic so generator runs do not leave unrelated Git noise.
    # Makefile-impl.mk is deliberately absent from this list: it is untracked
    # (it caches DEFAULTCONF = the local selection), so its EOLs are nobody's diff.
    $relativePaths = @(
        'nbproject\Makefile-variables.mk'
    )
    $encoding = [System.Text.Encoding]::GetEncoding(28591)
    $normalizedCount = 0

    foreach ($relativePath in $relativePaths) {
        $path = Join-Path $ProjectDir $relativePath
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }

        $text = $encoding.GetString([System.IO.File]::ReadAllBytes($path))
        $newText = [regex]::Replace(
            $text,
            '[ \t]+(?=\r?$)',
            '',
            [System.Text.RegularExpressions.RegexOptions]::Multiline)
        $newText = [regex]::Replace($newText, '(?<!\r)\n', "`r`n")
        if ($newText -ne $text) {
            [System.IO.File]::WriteAllBytes($path, $encoding.GetBytes($newText))
            $normalizedCount++
        }
    }

    if ($normalizedCount -gt 0) {
        Write-Host "normalized tracked generated makefile(s): $normalizedCount"
    }
}

function Invoke-ConfigurationClean {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CleanScript,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    # clean.ps1 is also used directly by VS Code and therefore reads its target
    # from MPLABX_CONF. Keep an explicit build.ps1 -Configuration override
    # authoritative for the clean phase as well as the build phase.
    $hadPrevious = Test-Path Env:MPLABX_CONF
    $previous = $env:MPLABX_CONF
    try {
        $env:MPLABX_CONF = $Configuration
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $CleanScript
    }
    finally {
        if ($hadPrevious) {
            $env:MPLABX_CONF = $previous
        }
        else {
            Remove-Item Env:MPLABX_CONF -ErrorAction SilentlyContinue
        }
    }
}

$repoRoot = Resolve-SonoraRepoRoot -RequestedRoot $Root

# The resident bootloader is not an MPLAB X configuration of this project: it has
# its own constrained standalone build, so it is dispatched before any project /
# preset state is resolved.
#
# One name per device, and the set comes from src/boot/boot_image.psd1: a name written
# here would accept the AK512's configuration and fall through to "no such MPLAB
# configuration" for the AK128's, which is a confusing way to say "this build script
# has not heard of your part".
$residentBootConfigurations = @{ }
foreach ($residentDevice in @(Get-SonoraSerialUpdateDevices -RepoRoot $repoRoot)) {
    $residentEntry = Get-SonoraSerialUpdateDeviceEntry -RepoRoot $repoRoot -Device $residentDevice
    $residentBootConfigurations[$residentEntry.ConfigurationName] = $residentEntry.Device
}
if ($residentBootConfigurations.ContainsKey($Configuration)) {
    if ($Generate) {
        throw 'The resident bootloader uses its constrained standalone build; -Generate does not apply.'
    }
    if (-not [string]::IsNullOrWhiteSpace($App) -or
        -not [string]::IsNullOrWhiteSpace($Preset)) {
        throw '-App and -Preset do not apply to the resident bootloader.'
    }
    $residentBuild = Join-Path $repoRoot 'buildtools\build_resident_bootloader.ps1'
    & $residentBuild -Root $repoRoot -Device $residentBootConfigurations[$Configuration] `
        -Full:$Full -Clean:$Clean
    return
}

$projectDir = Resolve-SonoraProjectDir -RepoRoot $repoRoot -RequestedProjectDir $ProjectDir
$configurations = Get-SonoraConfigurations -ProjectDir $projectDir
$presetCatalog = Get-SonoraPresetCatalog -RepoRoot $repoRoot
$cleanScript = Join-Path $repoRoot '.vscode\clean.ps1'

# --- What does the active selection say? -------------------------------------
# The three user choices (serial update support, device, application profile) are
# the authority; the configuration is derived from the device and the profile's
# application. -Configuration / -Preset remain as one-shot overrides for existing
# notes and scripts, and are reconciled with the selection below.
$selection = Get-SonoraSelection -RepoRoot $repoRoot -Configurations $configurations -Catalog $presetCatalog
$serialUpdateSupport = $selection.SerialUpdateSupport

if ([string]::IsNullOrWhiteSpace($Configuration)) {
    $Configuration = Get-SonoraActiveConfiguration -RepoRoot $repoRoot -ProjectDir $projectDir -Configurations $configurations
} else {
    # An explicit configuration IS a delivery mode: each configuration either
    # carries the serial-update layout or does not. So the named one decides, not
    # the stored selection -- otherwise -Configuration <standalone> while the
    # selection says Yes would try to build a serial-update image out of a
    # configuration that has no layout for it.
    $overrideEntry = Get-SonoraConfiguration -Configurations $configurations -Name $Configuration
    if ($serialUpdateSupport -ne $overrideEntry.IsSerialUpdate) {
        $mode = if ($overrideEntry.IsSerialUpdate) { 'on' } else { 'off' }
        Write-Host "NOTE: serial update support is $mode for this build - that is what configuration $Configuration builds."
        $serialUpdateSupport = $overrideEntry.IsSerialUpdate
    }
}
$configurationEntry = Get-SonoraConfiguration -Configurations $configurations -Name $Configuration

# --- What APP_BUILD variation is this build? --------------------------------
# -Preset wins; otherwise the persisted selection for this configuration; only
# if there is none does the build fall back to the header's own default (no
# -DAPP_BUILD passed at all, which is what an MPLAB X IDE build does).
$selectedAppPreset = $null
$presetSource = $null

if (-not [string]::IsNullOrWhiteSpace($Preset)) {
    $presetEntry = Get-SonoraPreset -Catalog $presetCatalog -Name $Preset
    if ($null -eq $presetEntry) {
        $allowed = ($presetCatalog.Presets | ForEach-Object { $_.Name }) -join ', '
        throw "Unknown APP_BUILD variation '$Preset'. Known variations: $allowed"
    }
    if (-not [string]::IsNullOrWhiteSpace($App) -and $App -ne $presetEntry.App) {
        throw "-App $App contradicts -Preset $Preset (a $($presetEntry.App) variation). -App is deprecated; drop it."
    }
    $selectedAppPreset = $presetEntry.Name
    $presetSource = '-Preset'
    $App = $presetEntry.App
}
elseif (-not [string]::IsNullOrWhiteSpace($App)) {
    # Deprecated form: -App without -Preset means "this application's default".
    Write-Host "NOTE: -App is deprecated (the configuration and the APP_BUILD variation already carry the application). Use -Preset, or ./buildtools/switch_config.ps1."
    $selectedAppPreset = Get-SonoraDefaultPreset -Catalog $presetCatalog -App $App
    $presetSource = "-App $App default"
}

# An explicit application (from -Preset/-App) may point at a different native
# configuration than the active one; remap to the matching configuration, keeping
# the same device where the project offers a choice.
if (-not [string]::IsNullOrWhiteSpace($App) -and $App -ne $configurationEntry.App) {
    $candidates = @($configurations | Where-Object { $_.App -eq $App })
    if ($candidates.Count -eq 0) {
        throw "No MPLAB configuration in this project builds the $App application."
    }
    # Only ever remap within the same device: switching the target device behind
    # the caller's back is never what was meant.
    $sameDevice = @($candidates | Where-Object { $_.Device -eq $configurationEntry.Device })
    if ($sameDevice.Count -eq 0) {
        $names = ($candidates | ForEach-Object { $_.Name }) -join ', '
        throw "Configuration '$Configuration' (device $($configurationEntry.Device)) cannot build the $App application. Pass -Configuration explicitly ($names)."
    }
    $target = $sameDevice[0]

    Write-Host "Mapped configuration: $Configuration + $App -> $($target.Name)"
    $configurationEntry = $target
    $Configuration = $target.Name
}

if ($null -eq $selectedAppPreset) {
    $persistedPreset = Get-SonoraSelectedPreset -RepoRoot $repoRoot -Configuration $Configuration
    if ($persistedPreset) {
        $persistedEntry = Get-SonoraPreset -Catalog $presetCatalog -Name $persistedPreset
        if ($null -eq $persistedEntry -or $persistedEntry.App -ne $configurationEntry.App) {
            Write-Host "WARNING: ignoring stored APP_BUILD '$persistedPreset' - not a $($configurationEntry.App) variation. Re-run ./buildtools/switch_config.ps1."
        } else {
            $selectedAppPreset = $persistedEntry.Name
            $presetSource = 'active selection'
        }
    }
}

# Still nothing chosen: take the active selection's profile when it belongs to
# this configuration's application. Only after that does the compiler's own
# default apply (what an MPLAB X IDE build gets).
if ($null -eq $selectedAppPreset) {
    $selectionEntry = Get-SonoraPreset -Catalog $presetCatalog -Name $selection.Profile
    if ($null -ne $selectionEntry -and $selectionEntry.App -eq $configurationEntry.App) {
        $selectedAppPreset = $selectionEntry.Name
        $presetSource = 'active selection'
    }
}

# Serial update support applies to the application being built, so it is decided
# here rather than by a configuration name. It survives the -Preset remap above
# because it is a property of the selection and the device, not of which
# application was picked.
$serialUpdateDevices = @(Get-SonoraSerialUpdateDevices -RepoRoot $repoRoot)
if ($serialUpdateSupport -and $serialUpdateDevices -notcontains $configurationEntry.Device) {
    Write-Host "NOTE: serial update support is off for this build - $($configurationEntry.Device) does not support it."
    $serialUpdateSupport = $false
}
$serialUpdateAppRequested = $serialUpdateSupport

# C0.2. configurations.xml decides what this build compiles and what it excludes,
# and MPLAB X rewrites that file whenever the IDE touches the project -- it has
# dropped ex="true" attributes before. A lost exclusion does not break the build;
# it makes a standalone application link the download engine without saying so.
# So it is checked on EVERY build, not when someone remembers to be suspicious.
# Costs a second or two and needs no toolchain. See buildtools/check_configurations.ps1.
# The gate is tracked, so every clone -- and every published archive of this tree --
# carries it. An absent gate is therefore an incomplete checkout, not a supported
# tree shape (the single-app generator that used to prune it away is gone). The
# build still proceeds so that such a checkout can be diagnosed, but it says so.
$configurationGate = Join-Path $PSScriptRoot 'check_configurations.ps1'
if (Test-Path -LiteralPath $configurationGate) {
    & pwsh -NoProfile -File $configurationGate -Root $repoRoot
    if ($LASTEXITCODE -ne 0) {
        throw ("The MPLAB project configuration gate failed (see above). This build was not " +
               "started because what it would compile is not what the project is supposed to " +
               "compile. If MPLAB X rewrote nbproject/configurations.xml, restore the dropped " +
               "attribute; if the change is intended, edit the expectation table in " +
               "buildtools/check_configurations.ps1 deliberately.")
    }
} else {
    Write-Host 'NOTE: buildtools/check_configurations.ps1 is absent - incomplete checkout, configuration gate skipped.'
}

# Before spending a build on it: the linker script's usable program range and the
# configuration's IVT address must still describe the same layout.
if ($serialUpdateSupport) {
    Assert-SerialUpdateLayoutInvariants `
        -RepoRoot $repoRoot -ProjectDir $projectDir -Configuration $Configuration `
        -Device $configurationEntry.Device
}

$configurationDefaultPreset = Get-SonoraDefaultPreset -Catalog $presetCatalog -App $configurationEntry.App

# The profile this build is actually compiling, whether it was chosen explicitly,
# taken from the selection, or left to the compiler's default. Needed here for its
# artifact tag and display name.
$effectiveProfileName = if ($null -ne $selectedAppPreset) { $selectedAppPreset } else { $configurationDefaultPreset }
$selectedProfileEntry = Get-SonoraPreset -Catalog $presetCatalog -Name $effectiveProfileName

# Serial update needs a stable package name, so refuse before spending a build on
# something that could not be filed afterwards.
if ($serialUpdateSupport -and [string]::IsNullOrWhiteSpace($selectedProfileEntry.Artifact)) {
    throw @"
Application profile '$($selectedProfileEntry.Display)' has no 'artifact:' tag in
$($presetCatalog.HeaderPath), so its SERIAL_UPDATE_PACKAGE could not be given a
stable file name. Add "artifact: <lower_case_token>" to its define comment
(see ./buildtools/switch_config.ps1 -CheckProfiles), or select serial update
support = No.
"@
}

# What this build is, in the same words switch_config.ps1 used.
Write-Host ("Serial update support: {0}" -f $(if ($serialUpdateSupport) { 'Yes' } else { 'No' }))
Write-Host ("Target device:         {0}" -f $configurationEntry.Device)
Write-Host ("Application profile:   {0}" -f $selectedProfileEntry.Display)
# Identity of this build's APP_BUILD, used as the build-directory stamp. Without
# an explicit variation the compiler picks the configuration's own default, so
# record that as the stamp instead of a variation name.
$appBuildIdentity = if ($null -ne $selectedAppPreset) { $selectedAppPreset } else { "(default:$configurationDefaultPreset)" }

$makefile = Join-Path $projectDir "nbproject\Makefile-$Configuration.mk"

if (-not (Test-Path -LiteralPath $projectDir)) {
    throw "MPLAB X project directory not found: $projectDir"
}
$modeCount = @($Full, $Clean, $Generate) | Where-Object { $_ } | Measure-Object | Select-Object -ExpandProperty Count
if ($modeCount -gt 1) {
    throw "Use only one of -Full, -Clean, or -Generate."
}

# MPLAB separates Classic and ASRC object directories by native configuration,
# but the variations within one application share that directory: objects built
# with another APP_BUILD must never be reused. The build directory carries a
# stamp of what it was built with, so only a real change forces a clean build -
# rebuilding the same variation stays incremental.
$explicitFull = $Full
$needsRegenerate = $false

$builtAppBuild = Get-SonoraBuiltPreset -ProjectDir $projectDir -Configuration $Configuration
if (-not ($Full -or $Clean -or $Generate) -and
    (Test-SonoraConfigurationHasObjects -ProjectDir $projectDir -Configuration $Configuration)) {
    $promoteReasons = @()

    # configurations.xml owns the per-configuration macros and source manifest, and
    # the generated makefile is only a snapshot of it. If the project has moved on
    # (pull, IDE edit), an incremental build would compile new sources against the
    # old macro set - which links, or fails, by luck. Regenerate and rebuild. This is
    # the ONLY promotion reason that needs the slow prjMakefilesGenerator.bat step -
    # the others below just mean the existing makefile's object list can't be trusted
    # incrementally, which a clean rebuild against the same makefile already fixes.
    $generatedMakefile = Join-Path $projectDir "nbproject\Makefile-$Configuration.mk"
    $configurationsXml = Join-Path $projectDir 'nbproject\configurations.xml'
    if ((Test-Path -LiteralPath $generatedMakefile) -and
        (Test-Path -LiteralPath $configurationsXml) -and
        ((Get-Item -LiteralPath $configurationsXml).LastWriteTimeUtc -gt (Get-Item -LiteralPath $generatedMakefile).LastWriteTimeUtc)) {
        $promoteReasons += 'configurations.xml is newer than the generated makefile'
        $needsRegenerate = $true
    }

    if ($null -eq $builtAppBuild) {
        $promoteReasons += 'APP_BUILD of the existing objects is unknown (built outside build.ps1?)'
    }
    elseif ($builtAppBuild -ne $appBuildIdentity) {
        $promoteReasons += "APP_BUILD changed ($builtAppBuild -> $appBuildIdentity)"
    }

    # Objects the makefile no longer lists: left behind by a removed/renamed source,
    # a branch switch, or a source-manifest change. They are inert for the link
    # (the recipe passes only ${OBJECTFILES}), but they prove the build tree and the
    # makefile disagree about which sources exist - rebuild clean rather than reason
    # about what else that disagreement covers.
    $orphanObjects = @(Get-SonoraOrphanObjects -ProjectDir $projectDir -Configuration $Configuration)
    if ($orphanObjects.Count -gt 0) {
        $sample = ($orphanObjects | Select-Object -First 3 | ForEach-Object { Split-Path -Leaf $_ }) -join ', '
        $suffix = if ($orphanObjects.Count -gt 3) { ", ..." } else { '' }
        $promoteReasons += "$($orphanObjects.Count) stale object(s) no longer listed by the makefile ($sample$suffix)"
    }

    if ($promoteReasons.Count -gt 0) {
        foreach ($reason in $promoteReasons) {
            Write-Host "${reason}: promoting to -Full."
        }
        $Full = $true
    }
}

Write-Host "Root: $repoRoot"
Write-Host "Project: $projectDir"
Write-Host "Configuration: $Configuration  ($($configurationEntry.App) / $($configurationEntry.Device))"
if ($null -ne $selectedAppPreset) {
    Write-Host "APP_BUILD: $selectedAppPreset  [$presetSource]"
} else {
    Write-Host "APP_BUILD: $configurationDefaultPreset  [configuration default; none selected]"
}

Push-Location $projectDir
try {
    if ($Clean) {
        if (-not (Test-Path -LiteralPath $cleanScript)) {
            throw "clean script not found: $cleanScript"
        }

        Invoke-CheckedCommand -Description "Clean $Configuration outputs" -Command {
            Invoke-ConfigurationClean -CleanScript $cleanScript -Configuration $Configuration
        }
        Remove-ProjectRootIntermediates -ProjectDir $projectDir
        return
    }

    if ($Generate) {
        $generatorTool = Resolve-MplabXTool `
            -ToolName 'prjMakefilesGenerator.bat' `
            -RelativePath 'mplab_platform\bin\prjMakefilesGenerator.bat' `
            -OverridePath $env:MPLABX_GEN `
            -OverrideVariable 'MPLABX_GEN'
        Write-Host "MPLAB X generator: $($generatorTool.VersionLabel) ($($generatorTool.Path))"

        Invoke-CheckedCommand -Description 'Generate MPLAB X makefiles' -Command {
            & $($generatorTool.Path) '.'
        }
        Normalize-TrackedGeneratedMakefiles -ProjectDir $projectDir
        Update-SonoraDefaultConfCache -ProjectDir $projectDir -Configuration $Configuration
        return
    }

    $makeTool = Resolve-MplabXTool `
        -ToolName 'make.exe' `
        -RelativePath 'gnuBins\GnuWin32\bin\make.exe' `
        -OverridePath $env:MPLABX_MAKE `
        -OverrideVariable 'MPLABX_MAKE'
    Write-Host "MPLAB X make: $($makeTool.VersionLabel) ($($makeTool.Path))"

    $needsMakefileGeneration = $explicitFull -or $needsRegenerate -or -not (Test-Path -LiteralPath $makefile)
    if ($needsMakefileGeneration) {
        $generatorTool = Resolve-MplabXTool `
            -ToolName 'prjMakefilesGenerator.bat' `
            -RelativePath 'mplab_platform\bin\prjMakefilesGenerator.bat' `
            -OverridePath $env:MPLABX_GEN `
            -OverrideVariable 'MPLABX_GEN'
        Write-Host "MPLAB X generator: $($generatorTool.VersionLabel) ($($generatorTool.Path))"

        Invoke-CheckedCommand -Description 'Generate MPLAB X makefiles' -Command {
            & $($generatorTool.Path) '.'
        }
    }
    Normalize-TrackedGeneratedMakefiles -ProjectDir $projectDir
    Update-SonoraDefaultConfCache -ProjectDir $projectDir -Configuration $Configuration

    if ($Full) {
        if (-not (Test-Path -LiteralPath $cleanScript)) {
            throw "clean script not found: $cleanScript"
        }

        Invoke-CheckedCommand -Description "Clean $Configuration outputs" -Command {
            Invoke-ConfigurationClean -CleanScript $cleanScript -Configuration $Configuration
        }
        Remove-ProjectRootIntermediates -ProjectDir $projectDir
    }

    # Inject the actual source-tree folder name (leaf of the repo root) into every compile as a
    # preprocessor token, so the boot banner can self-identify which clone this build came from
    # (guards against flashing a look-alike sibling folder by mistake). Passed as a BARE TOKEN
    # (no quotes) and stringified in C -- fleet clone folders are C-token-safe ([A-Za-z0-9_]).
    # main.c has an #ifndef fallback ("(unknown)") for IDE-direct builds that skip this script.
    $srcDirName = Split-Path -Leaf $repoRoot
    $extraDefines = @("-DAPP_SRC_DIRNAME=$srcDirName")

    # Git revision of this build, injected the same way as APP_SRC_DIRNAME: a BARE TOKEN
    # stringified in C for the boot banner ("Commit:"). No option needed -- every build stamps
    # it automatically. A dirty working tree becomes "<hash>_dirty"; when Git is unavailable the
    # token is "unknown". Sanitized to [A-Za-z0-9_] so it is always a valid single C token
    # (main.c has an #ifndef fallback of "(unknown)" for IDE-direct builds that skip this script).
    $gitRevision = 'unknown'
    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if ($null -ne $gitCmd) {
        # Check the native git exit code IMMEDIATELY after the call, BEFORE any pipe. Piping a
        # native command into Select-Object can stop the pipeline early, so that $LASTEXITCODE is
        # left unset (empty) in a fresh PowerShell 7 process even though git succeeded -- which made
        # the success test flaky and fell back to "unknown". Capture first, test exit, then slice.
        $commitRaw = & $gitCmd.Source -C $repoRoot rev-parse --short=7 HEAD 2>$null
        $commitOk  = ($LASTEXITCODE -eq 0)
        $commit    = if ($null -ne $commitRaw) {
            ([string]($commitRaw | Select-Object -First 1)).Trim()
        } else { '' }
        if ($commitOk -and -not [string]::IsNullOrWhiteSpace($commit)) {
            $gitRevision = $commit
            $statusRaw = & $gitCmd.Source -C $repoRoot status --porcelain --untracked-files=normal 2>$null
            $statusOk  = ($LASTEXITCODE -eq 0)
            if ($statusOk) {
                $dirty = @($statusRaw | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
                if ($dirty.Count -gt 0) {
                    $gitRevision += '_dirty'
                }
            }
        }
    }
    $gitRevision = ($gitRevision -replace '[^A-Za-z0-9_]', '_')
    $extraDefines += "-DSONORA_GIT_COMMIT=$gitRevision"

    if ($null -ne $selectedAppPreset) {
        $extraDefines += "-DAPP_BUILD=$selectedAppPreset"
    }
    # SONORA_DELIVERY_SERIAL_UPDATE_APP is set by the serial-update configurations
    # themselves, so it is not added here. It used to be, for a delivery built from a
    # configuration that did not carry it -- that case no longer exists, and adding
    # it again would define the macro twice.

    # One-shot -Define overrides, appended LAST so they win over anything above.
    # Normalized to -DNAME[=VALUE] and echoed, because a silently-dropped define is
    # how an A/B comparison ends up measuring the same build twice.
    # Comma-joined forms reach us as ONE string when the script is invoked through
    # `pwsh -File` (the shell does not build an array), and the NAME=VALUE pattern
    # below happily accepts the comma as part of the value. Measured 2026-08-14:
    # `-Define A=1,B=1` produced a single `-DA=1,B=1`, so B was never defined and an
    # image built to have both quietly had one. Split it here, but only when every
    # piece is itself a well-formed define -- a value that legitimately contains a
    # comma (`-DFOO=(1,2)`) must survive untouched.
    $definesExpanded = @()
    foreach ($d in @($Define)) {
        if ([string]::IsNullOrWhiteSpace($d)) { continue }
        $pieces = $d.Split(',')
        $allValid = ($pieces.Count -gt 1)
        foreach ($p in $pieces) {
            $probe = $p.Trim()
            if ($probe.StartsWith('-D')) { $probe = $probe.Substring(2) }
            if ($probe -notmatch '^[A-Za-z_][A-Za-z0-9_]*(=[^,]*)?$') { $allValid = $false }
        }
        if ($allValid) { $definesExpanded += $pieces } else { $definesExpanded += $d }
    }

    if ($definesExpanded.Count -gt 0) {
        foreach ($d in $definesExpanded) {
            if ([string]::IsNullOrWhiteSpace($d)) { continue }
            $token = $d.Trim()
            if ($token.StartsWith('-D')) { $token = $token.Substring(2) }
            if ($token -notmatch '^[A-Za-z_][A-Za-z0-9_]*(=.*)?$') {
                throw "-Define '$d' is not a valid NAME or NAME=VALUE preprocessor definition."
            }
            $extraDefines += "-D$token"
            Write-Host "Extra define: -D$token"
        }
    }

    $extraCcPre = "MP_EXTRA_CC_PRE=$($extraDefines -join ' ')"
    $extraMakeArgs = @($extraCcPre)

    # -AsDefine -> MP_EXTRA_AS_PRE. Kept separate from the -D list above because the
    # assembler takes --defsym, not -D, and because the .s rule in the generated
    # makefile expands only this variable. Same comma-splitting reasoning as -Define
    # (`pwsh -File` delivers "A=1,B=1" as one string), but here a comma can never be
    # part of a legitimate value, so the split is unconditional.
    $asDefinesExpanded = @()
    foreach ($a in @($AsDefine)) {
        if ([string]::IsNullOrWhiteSpace($a)) { continue }
        foreach ($p in $a.Split(',')) {
            $token = $p.Trim()
            if ([string]::IsNullOrWhiteSpace($token)) { continue }
            if ($token -notmatch '^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_+\-]+)?$') {
                throw "-AsDefine '$p' is not a valid NAME or NAME=VALUE assembler symbol."
            }
            if ($token -notmatch '=') { $token = "$token=1" }
            $asDefinesExpanded += $token
            Write-Host "Extra assembler symbol: --defsym $token"
        }
    }
    if ($asDefinesExpanded.Count -gt 0) {
        $extraMakeArgs += "MP_EXTRA_AS_PRE=-Wa,$(($asDefinesExpanded | ForEach-Object { "--defsym=$_" }) -join ',')"
    }

    # No linker settings are injected here. The memory layout belongs to the MPLAB
    # configuration: each serial-update configuration selects its own device's
    # src/linker/p33AK*_serial_update_app.gld (every script is registered as a project
    # linker file, and each configuration excludes the ones that are not its own) for the
    # usable program range, and carries the IVT placement in its own vtable
    # properties. check_configurations.ps1 asserts both halves of that. Passing
    # MP_LINKER_FILE_OPTION / MP_EXTRA_LD_POST from here used to be how that
    # happened, which meant a layout the IDE knew nothing about and that an
    # ordinary MPLAB X build silently got wrong.

    Invoke-CheckedCommand -Description "Build $Configuration (-j$Jobs)" -Command {
        & $($makeTool.Path) "-j$Jobs" -f "nbproject/Makefile-$Configuration.mk" SUBPROJECTS= @extraMakeArgs .build-conf
    }

    if ($serialUpdateAppRequested) {
        $map = Join-Path $projectDir `
            "dist\$Configuration\production\dspic33ak_audio_dsp.X.production.map"
        if (-not (Test-Path -LiteralPath $map)) {
            throw "Expected SERIAL_UPDATE_APP map was not created: $map"
        }
        $mapText = [IO.File]::ReadAllText($map)
        $mailbox = [regex]::Match(
            $mapText,
            '(?m)^\.resident_launch_mailbox\s+0x4050\s+0\s+0x10\s+\(16\)\s*$'
        )
        if (-not $mailbox.Success) {
            throw 'SERIAL_UPDATE_APP warm-reset mailbox is not allocated at 0x4050..0x405F in the final map.'
        }
        # The four reset-diagnostic sections sit at the TOP of RAM, and their sizes --
        # not their addresses -- are what the linker scripts and src/shared/ agree on.
        # So derive the addresses from the map's own data region, exactly as
        # Assert-StandaloneMapLayout does (see the note at its $noinitBlockSize): the
        # AK512's sentinel is at 0x13E00 and the AK128's at 0x7E00, and both are "top of
        # RAM minus 0x200". Writing the AK512 numbers here was device-specific for no
        # reason, and on the AK128 it failed as "allocation is missing" -- which reads
        # like a lost section rather than like a check that only knows one part.
        #
        # .resident_diag_guard is deliberately absent since 2026-08-11: the noinit block
        # was extended down over it, so the top 0xD0 stays fully occupied by one section
        # instead of two. Asserting the guard's absence is not possible with this shape
        # of check -- the noinit size below is what proves it.
        $dataRegion = [regex]::Match(
            $mapText, '"data"\s+Memory\s*\[Origin = (0x[0-9a-fA-F]+), Length = (0x[0-9a-fA-F]+)\]')
        if (-not $dataRegion.Success) {
            throw "Could not read the data memory region from the SERIAL_UPDATE_APP map: $map"
        }
        $ramTop = [Convert]::ToUInt32($dataRegion.Groups[1].Value, 16) +
                  [Convert]::ToUInt32($dataRegion.Groups[2].Value, 16)
        # offset below the top of RAM -> section name, size
        $diagnosticSections = @(
            @{ Offset = 0x200; Name = '.resident_far_sentinel';      Size = 0x10 }
            @{ Offset = 0x1F0; Name = '.resident_reset_source_trace'; Size = 0xE0 }
            @{ Offset = 0x110; Name = '.resident_precrt_trace';       Size = 0x40 }
            @{ Offset = 0x0D0; Name = '.noinit_ram';                  Size = 0xD0 }
        )
        foreach ($section in $diagnosticSections) {
            $address = $ramTop - $section.Offset
            $expected = '(?m)^{0}\s+0x0*{1:x}\s+\S+\s+0x{2:x}\b' -f
                        [regex]::Escape($section.Name), $address, $section.Size
            if (-not [regex]::IsMatch($mapText, $expected)) {
                throw ("SERIAL_UPDATE_APP reset diagnostic allocation is missing or moved: " +
                       "expected {0} at 0x{1:X} size 0x{2:X} (top of this device's RAM 0x{3:X} " +
                       "minus 0x{4:X}). The device's serial-update linker script places it." -f
                       $section.Name, $address, $section.Size, $ramTop, $section.Offset)
            }
        }
        $stack = [regex]::Match(
            $mapText,
            '(?m)^stack\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+'
        )
        if (-not $stack.Success) {
            throw 'Could not read SERIAL_UPDATE_APP stack range from the final map.'
        }
        $stackStart = [Convert]::ToUInt32($stack.Groups[1].Value.Substring(2), 16)
        $stackLength = [Convert]::ToUInt32($stack.Groups[2].Value.Substring(2), 16)
        # The stack must not reach the diagnostic block, the RAM above it must be claimed
        # by something, and it must not have been squeezed to nothing. Same derivation
        # as the sections above, so it holds on both parts. The floor is 0x800: the
        # deepest single frame in this image is the boot front-end selftest at about
        # 0x640, and it is called a few frames down.
        $diagnosticBase = $ramTop - $diagnosticSections[0].Offset
        Assert-DataTopClaimed -MapText $mapText -RamTop $ramTop `
            -DiagnosticBase $diagnosticBase -StackStart $stackStart `
            -StackLength $stackLength -MinStack 0x800

        # Operand placement is a performance contract, not a preference: see the
        # function's own note.
        Assert-Q31FrontEndPlacement -MapText $mapText
        Assert-Q31ResamplerPlacement -MapText $mapText

        # The linker script and the configuration each own half of this layout, so
        # confirm the halves still agree in the linked result.
        Assert-SerialUpdateMapLayout -MapPath $map -RepoRoot $repoRoot

    } else {
        # The other half of the same contract: with delivery support off, the linked
        # image must carry no trace of it. Checked here for every standalone
        # configuration, not only when someone remembers to look.
        $map = Join-Path $projectDir `
            "dist\$Configuration\production\dspic33ak_audio_dsp.X.production.map"
        if (-not (Test-Path -LiteralPath $map)) {
            throw "Expected standalone map was not created: $map"
        }
        Assert-StandaloneMapLayout -MapPath $map -Configuration $Configuration -RepoRoot $repoRoot
    }

    # Modulo addressing is machine state shared by every context, so the condition
    # that makes the Q31 kernel's window safe is about the whole image, not about
    # the kernel. Checked on the ELF beside the map, in both delivery modes.
    Assert-ModuloWindowSafety -ElfPath ([IO.Path]::ChangeExtension($map, '.elf')) `
        -RepoRoot $repoRoot

    # --- Deliverables ----------------------------------------------------------
    # One name for "the thing that gets flashed" in both delivery modes, so
    # flashauto.ps1 has nothing to decide: FACTORY_IMAGE. With serial update it is
    # the resident bootloader + application + manifest; without, it is the
    # standalone application. The application HEX of a serial-update build is NOT
    # programmable on its own (its reset vector lives in the bootloader), which is
    # why it is never presented as a deliverable.
    if (-not $NoDelivery) {
        $appHex = Join-Path $projectDir `
            "dist\$Configuration\production\dspic33ak_audio_dsp.X.production.hex"
        $appMap = [IO.Path]::ChangeExtension($appHex, '.map')
        $factoryImage = Get-SonoraFactoryImagePath -ProjectDir $projectDir -Configuration $Configuration
        $savedPackage = $null

        if ($serialUpdateSupport) {
            $artifacts = New-SerialUpdateArtifacts `
                -RepoRoot $repoRoot `
                -Device $configurationEntry.Device `
                -AppHex $appHex `
                -AppMap $appMap `
                -FactoryImage $factoryImage `
                -ArtifactTag $selectedProfileEntry.Artifact `
                -FirmwareVersion $FirmwareVersion `
                -Full:$Full
            $savedPackage = $artifacts.Package
        } else {
            New-StandaloneFactoryImage -AppHex $appHex -FactoryImage $factoryImage
        }

        Save-FactoryImageMetadata `
            -FactoryImage $factoryImage `
            -SerialUpdateSupport $serialUpdateSupport `
            -Device $configurationEntry.Device `
            -ProfileName $appBuildIdentity `
            -Configuration $Configuration `
            -RepoRoot $repoRoot

        Write-Host ''
        Write-Host 'Build products:'
        Write-Host "  FACTORY_IMAGE          $factoryImage"
        if ($null -ne $savedPackage) {
            Write-Host "  SERIAL_UPDATE_PACKAGE  $savedPackage"
        }
        Write-Host ''
        Write-Host 'Next:'
        Write-Host '  .\buildtools\flashauto.ps1'
    } else {
        Write-Host ''
        Write-Host '==> Deliverables skipped (-NoDelivery). Nothing to flash.'
    }

    # Record what these objects were built with, so the next build knows whether
    # it can be incremental (see the promotion block above) and flashauto.ps1 can
    # report which variation the HEX came from.
    Set-SonoraBuiltPreset -ProjectDir $projectDir -Configuration $Configuration -Value $appBuildIdentity
}
finally {
    Pop-Location
}
