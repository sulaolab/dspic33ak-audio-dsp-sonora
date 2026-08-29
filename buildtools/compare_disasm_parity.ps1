<#
.SYNOPSIS
  C2.2 -- compare two ELFs function by function, so every surviving difference can
  be attributed to a known change (or is a regression).

.DESCRIPTION
  "Same as main" cannot be checked by hashing: xc-dsc builds are not reproducible
  (measured -- section names and local label numbering vary run to run), so equal
  bytes are not expected even from identical sources and unequal bytes prove nothing.

  A whole-file `objdump -d` diff is no better. Adding or removing one function
  shifts every address after it, so a two-line change produces a diff the length of
  the image and nothing can be attributed to anything.

  So this compares PER FUNCTION, keyed by symbol name, with the parts that move for
  free normalised away:

    - addresses in the instruction column and in symbol headers
    - the byte-encoding columns (they restate the addresses)
    - compiler local labels (L11, L2^B1 ...) -- numbered image-wide, so they shift
      when unrelated code is added
    - branch targets, kept as <symbol+offset> (which is stable) rather than the
      absolute address objdump prints beside it

  What survives is a per-function verdict: identical, differing, only in the
  reference, or only in the new build. That list is the thing a reviewer can check
  against a list of intended changes; the raw hunks for the differing functions are
  written out too, for the ones that need reading.

  NOT A GATE. It reports and returns 0 even when functions differ, because
  "different" is the expected answer here -- the branch is supposed to contain
  changes. The judgement is whether each difference is an intended one, and that
  belongs to a person.

.PARAMETER RefElf
  Reference ELF (e.g. built from origin/main).

.PARAMETER NewElf
  ELF under test (e.g. built from the branch).

.PARAMETER OutDir
  Where to write the report and the per-function hunks. Default: a temp directory,
  printed at the end.

.PARAMETER Dfp
  Device family pack directory passed to objdump as -mdfp. Auto-detected (highest
  version under ~/.mchp_packs) when omitted -- objdump needs it to decode
  dsPIC33A instructions at all.

.EXAMPLE
  pwsh buildtools/compare_disasm_parity.ps1 `
      -RefElf ../_parity_main/dspic33ak_audio_dsp.X/dist/dsPIC33AK512/production/dspic33ak_audio_dsp.X.production.elf `
      -NewElf dspic33ak_audio_dsp.X/dist/dsPIC33AK512/production/dspic33ak_audio_dsp.X.production.elf
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RefElf,
    [Parameter(Mandatory = $true)][string]$NewElf,
    [string]$OutDir,
    [string]$Dfp,
    [string]$Objdump
)

$ErrorActionPreference = 'Stop'

# --- Toolchain ---------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($Objdump)) {
    $candidates = @(Get-ChildItem 'C:\Program Files\Microchip\xc-dsc' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\xc-dsc-objdump.exe' } |
        Where-Object { Test-Path -LiteralPath $_ })
    if ($candidates.Count -eq 0) { throw 'xc-dsc-objdump.exe not found; pass -Objdump.' }
    $Objdump = $candidates[0]
}
if ([string]::IsNullOrWhiteSpace($Dfp)) {
    # Read out of tools/dfp_packs.py's PINS (via its CLI, since PowerShell
    # cannot import a Python module) rather than "newest installed" -- that
    # silently picked a pack version the build never used. RefElf and NewElf
    # are expected to be the same device, so RefElf decides the pin.
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $Dfp = & python (Join-Path $repoRoot 'tools/dfp_packs.py') $RefElf
    if ($LASTEXITCODE -ne 0) { throw "dfp_packs.py could not resolve a DFP for $RefElf : $Dfp" }
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path ([IO.Path]::GetTempPath()) ("parity_" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# --- Disassemble and split into functions -----------------------------------
# Compiler local and debug labels. Note the CONTROL characters: xc-dsc emits them as
# 'L0<0x01>', 'L2<0x02>1' and so on. A terminal renders those as a bare 'L0', so a
# pattern of [0-9] alone looks right and silently fails to match -- which is exactly
# how a pure relocation reached the 'structurally differing' list once already.
$script:LocalLabelPattern = '^(L\d+([\x01-\x1f]\d*)*|\.L[A-Za-z]*\d+)$'

function Add-Body {
    param($Table, [string]$Name, [string]$Body)
    if (-not $Table.Contains($Name)) { $Table[$Name] = [System.Collections.Generic.List[string]]::new() }
    $Table[$Name].Add($Body)
}

function Get-Functions {
    param([string]$Elf, [string]$Label)

    $elfFull = (Resolve-Path -LiteralPath $Elf).Path
    $raw = & $Objdump -d "-mdfp=$Dfp" $elfFull 2>&1
    if ($LASTEXITCODE -ne 0) { throw "objdump failed on $elfFull" }
    [IO.File]::WriteAllLines((Join-Path $OutDir "$Label.disasm.txt"), [string[]]$raw)

    # Function boundaries come from the SYMBOL TABLE (address + size), not from the
    # '<name>:' headers in the disassembly. objdump only prints a header where a
    # symbol happens to sit, so a function whose successor has no symbol of its own
    # silently absorbs it -- measured, __DefaultInterrupt captured 96 instructions
    # instead of 42 and ran on into unrelated code, which reads as a large source
    # change and is not one. Sizes cannot run on.
    $symRaw = & $Objdump -t "-mdfp=$Dfp" $elfFull 2>&1
    if ($LASTEXITCODE -ne 0) { throw "objdump -t failed on $elfFull" }
    $ranges = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $symRaw) {
        if ($s -notmatch '^(?<addr>[0-9a-f]{8})\s+\S+\s+F\s+\S+\s+(?<size>[0-9a-f]+)\s+(?<n>.+)$') { continue }
        $size = [Convert]::ToUInt32($Matches['size'], 16)
        if ($size -eq 0) { continue }
        $start = [Convert]::ToUInt32($Matches['addr'], 16)
        # Same image-wide numeric suffix problem as before: '_x.28124' vs '_x.28241'.
        $n = [regex]::Replace($Matches['n'].Trim(), '\.\d+$', '.N')
        $ranges.Add([pscustomobject]@{ Start = $start; End = $start + $size; Name = $n })
    }
    $ranges = @($ranges | Sort-Object Start)
    $starts = [uint32[]]@($ranges | ForEach-Object { $_.Start })

    # Hand-written assembly and library code often has no F entry, or an F entry with
    # size 0, so an address-range lookup alone drops it all into one nameless bucket.
    # These are the same symbols without the size requirement, used as a
    # nearest-preceding fallback so that code still gets attributed to something a
    # reviewer can look up.
    $anchors = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $symRaw) {
        if ($s -notmatch '^(?<addr>[0-9a-f]{8})\s+\S+\s+\S*\s*(?<sect>\S+)\s+[0-9a-f]+\s+(?<n>.+)$') { continue }
        if ($Matches['sect'] -eq '*ABS*') { continue }
        $n = $Matches['n'].Trim()
        if ($n -match $script:LocalLabelPattern) { continue }
        # objdump lists a section-symbol per section, and xc-dsc RANDOMISES section
        # names ('17386a70bc0c_2'). Anchoring on one names the same code differently in
        # the two builds, which reports it as both removed and added -- measured, 56
        # removals and 61 additions that are the same bytes. A randomised name is not an
        # identity; leave that code in the '<no symbol>' bucket instead of mis-naming it.
        if ($n -match '^[0-9a-f]{8,}_\d+$') { continue }
        $n = [regex]::Replace($n, '\.\d+$', '.N')
        # With -ffunction-sections the SAME code is anchored on the section symbol
        # '.text.foo' in one build and on the function symbol '_foo' in the other,
        # depending on which happens to sit lowest at that address. Left alone that
        # reports 22 removals and 27 additions which are one-to-one the same functions.
        # Both spellings collapse to a single bare name.
        $n = [regex]::Replace($n, '^\.(text|data|rodata|bss|dinit)\.', '')
        $n = $n -replace '^_', ''
        $anchors.Add([pscustomobject]@{
            Start = [Convert]::ToUInt32($Matches['addr'], 16)
            Name  = $n
        })
    }
    $anchors = @($anchors | Sort-Object Start)
    $anchorStarts = [uint32[]]@($anchors | ForEach-Object { $_.Start })

    function Find-Owner {
        param($Starts, $Ranges, [uint32]$Addr)
        $i = [Array]::BinarySearch($Starts, $Addr)
        if ($i -lt 0) { $i = (-$i) - 2 }
        if ($i -ge 0 -and $Addr -lt $Ranges[$i].End) { return $Ranges[$i].Name }
        return $null
    }

    # Nearest-preceding non-local symbol, used ONLY when there is no sized F entry AND
    # no '<name>:' header to inherit -- i.e. the '<no symbol>' catch-all. It must rank
    # BELOW the header name: with -ffunction-sections the header a reviewer sees is
    # '.text.foo' in one build and the anchor is '_foo' in the other, so preferring the
    # anchor renames half the functions and manufactures both a removal and an addition
    # (measured: structural 10 -> 26, only-in-ref 0 -> 65).
    function Find-Anchor {
        param($AnchorStarts, $Anchors, [uint32]$Addr)
        $j = [Array]::BinarySearch($AnchorStarts, $Addr)
        if ($j -lt 0) { $j = (-$j) - 2 }
        if ($j -ge 0) { return '~' + $Anchors[$j].Name }
        return $null
    }

    $functions = [ordered]@{}
    $name = $null
    $body = $null
    $hdrName = $null

    $localLabel = $script:LocalLabelPattern

    foreach ($line in $raw) {
        # Section names are RANDOMISED by xc-dsc (measured: '527c6a70b50c_1'), so they
        # cannot be part of the key. The symbol name is the stable identity; the
        # section is not recorded at all.
        # A section boundary ends the current block and clears the fallback name, so
        # data in a section with no leading symbol cannot inherit the previous one.
        if ($line -match '^Disassembly of section ') {
            if ($null -ne $name) { Add-Body -Table $functions -Name $name -Body ($body -join "`n") }
            $name = $null
            $body = $null
            $hdrName = $null
            continue
        }
        if ($line -match '^[0-9a-f]{8} <(?<n>.+)>:$') {
            $sym = $Matches['n']
            # Local labels name nothing; the symbol table decides ownership now, so
            # they are simply ignored here -- with one exception. A '.LCn' label heads a
            # STRING LITERAL sitting in program memory, which objdump happily decodes as
            # instructions ('====' reads as 'cp.l w3, w13'). It has no F entry, so it fell
            # through to the nearest preceding function and contaminated that function's
            # body; and its number is allocated image-wide, so the same string is .LC0
            # here and .LC7 there. Strings are data: give the whole pool one bucket, whose
            # growth is then a single reviewable line instead of noise on ten functions.
            # Labels inside a function ('L11', '.LVLn') still lose to the F range above.
            if ($sym -match $localLabel) {
                if ($sym -match '^\.LC\d+$') {
                    if ($null -ne $name) { Add-Body -Table $functions -Name $name -Body ($body -join "`n") }
                    $name = $null
                    $body = $null
                    $hdrName = '<string pool>'
                }
                continue
            }
            if ($null -ne $name) { Add-Body -Table $functions -Name $name -Body ($body -join "`n") }
            # File-scope statics and compiler-synthesised tables carry a numeric
            # disambiguation suffix that is allocated image-wide, so the SAME object
            # is '_dbg_cfg.28124' in one build and '_dbg_cfg.28241' in the other
            # (measured). Keeping the number would report every static as both
            # removed and added. '.CSnnn'/'_CSWTCH.nn' are switch tables and get the
            # same treatment.
            $sym = [regex]::Replace($sym, '\.\d+$', '.N')
            $sym = [regex]::Replace($sym, '^(\.CS|_CSWTCH\.)\d+$', '$1N')
            $hdrName = $sym
            $name = $null
            $body = $null
            continue
        }
        # Instruction line: "  8005b4:\t41 b0 0c 80 \tmov.l  w15, ..." -- and
        # continuation lines that carry only more encoding bytes for the previous
        # instruction. The encoding restates addresses, so it is dropped entirely
        # and only the mnemonic column is compared.
        if ($line -match '^\s+(?<addr>[0-9a-f]+):\t[0-9a-f ]+\t(?<insn>.+)$') {
            # Ownership by address. The header name is only a fallback, for data that
            # the symbol table does not type as F (the .dinit table, switch tables,
            # const config structs) -- those are still worth comparing.
            $addr = [Convert]::ToUInt32($Matches['addr'], 16)
            $owner = Find-Owner -Starts $starts -Ranges $ranges -Addr $addr
            $key =
                if ($null -ne $owner) { $owner }
                elseif ($null -ne $hdrName) { $hdrName }
                else {
                    $a = Find-Anchor -AnchorStarts $anchorStarts -Anchors $anchors -Addr $addr
                    if ($null -ne $a) { $a } else { '<no symbol>' }
                }
            if ($key -ne $name) {
                if ($null -ne $name) { Add-Body -Table $functions -Name $name -Body ($body -join "`n") }
                $name = $key
                $body = [System.Collections.Generic.List[string]]::new()
            }
            $insn = $Matches['insn'].Trim()
            # Branch/call targets: keep the SYMBOL, drop the absolute address that
            # objdump prints beside it -- the symbol is stable under code motion.
            # Case-insensitive: objdump prints some operands with UPPERCASE hex
            # digits ('0x0040F4'), so a [0-9a-f] class silently misses them and the
            # relocation shows up as a fake code change.
            $insn = [regex]::Replace($insn, '(?i)0x[0-9a-f]+ (<[^>]+>)', '$1')
            # Compiler local and debug labels are numbered image-wide -- L11, L0<0x01>,
            # L2<0x02>1, and the DWARF .LVLnnn / .LFBnnn / .LFEnnn families alike. The
            # control characters are part of the name; see the note on $localLabel.
            $insn = [regex]::Replace($insn, '(?i)<(L\d+([\x01-\x1f]\d*)*|\.L[A-Za-z]*\d+)([+-]0x[0-9a-f]+)?>', '<L#>')
            # objdump prints EVERY symbol that shares the target address, and how many
            # local labels land there -- and where in the list -- varies between builds
            # ('<L#> <L#> <_f>' vs '<_f> <L#> <L#> <L#> <L#>' for the same call). Once
            # the numbering is gone those tokens carry nothing, so drop them and keep
            # the real symbols; a purely local target keeps a single placeholder.
            if ($insn -match '<') {
                $keep = @([regex]::Matches($insn, '<[^>]+>') |
                    ForEach-Object { $_.Value } | Where-Object { $_ -ne '<L#>' })
                $head = ($insn -replace '\s*<[^>]+>.*$', '')
                $insn = if ($keep.Count -gt 0) { "$head " + ($keep -join ' ') } else { "$head <L#>" }
            }
            $body.Add($insn)
        }
    }
    if ($null -ne $name) { Add-Body -Table $functions -Name $name -Body ($body -join "`n") }

    # Collapse the duplicate-symbol lists. Same-named file-scope statics in two
    # translation units land under one key; their bodies are SORTED before joining so
    # the merged text does not depend on which one the linker placed first.
    $result = [ordered]@{}
    foreach ($k in $functions.Keys) {
        $bodies = $functions[$k]
        $result[$k] = if ($bodies.Count -eq 1) { $bodies[0] } else { (($bodies | Sort-Object) -join "`n---`n") }
    }
    return $result
}

Write-Host "objdump : $Objdump"
Write-Host "dfp     : $Dfp"
Write-Host "ref     : $RefElf"
Write-Host "new     : $NewElf"
Write-Host "out     : $OutDir"
Write-Host ''

$refFns = Get-Functions -Elf $RefElf -Label 'ref'
$newFns = Get-Functions -Elf $NewElf -Label 'new'

$onlyRef = @($refFns.Keys | Where-Object { -not $newFns.Contains($_) })
$onlyNew = @($newFns.Keys | Where-Object { -not $refFns.Contains($_) })
$common = @($refFns.Keys | Where-Object { $newFns.Contains($_) })
$differ = @($common | Where-Object { $refFns[$_] -ne $newFns[$_] })
$same = $common.Count - $differ.Count

# Second pass over the differing ones, because a bare address difference is not a
# code difference. Enabling -ffunction-sections/-fdata-sections with
# --remove-unused-sections (commit 4838e5a) moves every function and every global,
# so any instruction carrying an absolute immediate differs in EVERY function --
# which would bury the handful of real changes. Masking all hex literals separates:
#
#   address-only : same instruction sequence, operands relocated  -> relayout
#   structural   : the instruction sequence itself changed        -> needs attribution
#
# Alignment padding goes the same way: the assembler inserts a different number of
# 'neop' fillers once code has moved, which is relayout, not a code change.
function Get-Masked {
    param([string]$Body)
    $m = [regex]::Replace($Body, '(?i)0x[0-9a-f]+', '0xH')
    return ([regex]::Replace($m, '(?m)^\s*(neop|nop)\s*\r?\n?', ''))
}
$structural = @($differ | Where-Object { (Get-Masked $refFns[$_]) -ne (Get-Masked $newFns[$_]) })
$addressOnly = @($differ | Where-Object { (Get-Masked $refFns[$_]) -eq (Get-Masked $newFns[$_]) })

# --- Report -----------------------------------------------------------------
$report = [System.Collections.Generic.List[string]]::new()
$report.Add('# C2.2 normalised disassembly parity')
$report.Add('')
$report.Add("ref: $((Resolve-Path -LiteralPath $RefElf).Path)")
$report.Add("new: $((Resolve-Path -LiteralPath $NewElf).Path)")
$report.Add('')
$report.Add("identical            : $same")
$report.Add("differing            : $($differ.Count)")
$report.Add("  structural         : $($structural.Count)   <- attribute each of these")
$report.Add("  address-only       : $($addressOnly.Count)   <- same instructions, operands relocated")
$report.Add("only in ref          : $($onlyRef.Count)")
$report.Add("only in new          : $($onlyNew.Count)")
$report.Add('')
if ($onlyRef.Count -gt 0) {
    $report.Add('## Only in ref (removed by the branch)')
    foreach ($f in $onlyRef) { $report.Add("- $f") }
    $report.Add('')
}
if ($onlyNew.Count -gt 0) {
    $report.Add('## Only in new (added by the branch)')
    foreach ($f in $onlyNew) { $report.Add("- $f") }
    $report.Add('')
}
if ($structural.Count -gt 0) {
    $report.Add('## Structurally differing (instruction sequence changed -- ATTRIBUTE EACH)')
    foreach ($f in $structural) {
        $refLines = ($refFns[$f] -split "`n").Count
        $newLines = ($newFns[$f] -split "`n").Count
        $report.Add("- $f  ($refLines -> $newLines instructions)")
    }
    $report.Add('')
}
if ($addressOnly.Count -gt 0) {
    $report.Add('## Address-only differences (same instructions, operands relocated)')
    foreach ($f in $addressOnly) { $report.Add("- $f") }
    $report.Add('')
}
if ($differ.Count -gt 0) {
    $report.Add('## All differing bodies')
    foreach ($f in $differ) {
        $refLines = ($refFns[$f] -split "`n").Count
        $newLines = ($newFns[$f] -split "`n").Count
        $report.Add("- $f  ($refLines -> $newLines instructions)")
    }
    $report.Add('')
    $report.Add('Per-function hunks are in hunks/.')
}

$reportPath = Join-Path $OutDir 'parity_report.md'
[IO.File]::WriteAllLines($reportPath, [string[]]$report)

# Per-function hunks for the differing ones, so a reviewer reads only what changed.
if ($differ.Count -gt 0) {
    $hunkDir = Join-Path $OutDir 'hunks'
    New-Item -ItemType Directory -Path $hunkDir -Force | Out-Null
    foreach ($f in $differ) {
        $safe = ($f -replace '[^A-Za-z0-9_.-]', '_')
        if ($safe.Length -gt 120) { $safe = $safe.Substring(0, 120) }
        $a = Join-Path $hunkDir "$safe.ref.txt"
        $b = Join-Path $hunkDir "$safe.new.txt"
        [IO.File]::WriteAllText($a, $refFns[$f])
        [IO.File]::WriteAllText($b, $newFns[$f])
        $d = Compare-Object -ReferenceObject ($refFns[$f] -split "`n") -DifferenceObject ($newFns[$f] -split "`n") |
            ForEach-Object { ('{0} {1}' -f $_.SideIndicator, $_.InputObject) }
        [IO.File]::WriteAllLines((Join-Path $hunkDir "$safe.diff.txt"), [string[]]@($d))
    }
}

Write-Host "identical   : $same"
Write-Host "differing   : $($differ.Count)"
Write-Host "only in ref : $($onlyRef.Count)"
Write-Host "only in new : $($onlyNew.Count)"
Write-Host ''
Write-Host "report: $reportPath"
exit 0
