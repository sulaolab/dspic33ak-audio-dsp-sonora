<#
  HAL drift report -- src/boot/hal_*/ against src/app/hal_*/.

  Reorg step 4 gave the resident boot image its own copy of the six HAL modules it
  needs. Before that both images compiled src/app/hal_*/ out of one working tree, which
  is how the boot image once quietly grew 208 bytes: an application-side HAL change
  reached a 32 KiB image pinned at fixed addresses, with nothing in between.

  The copy removes that coupling and creates the opposite risk -- a fix landing on
  one side only. This script makes that visible.

  INFORMATIONAL BY DESIGN. It exits 0 no matter what it finds, and nothing gates on
  it. Divergence is not a defect: the boot copy is allowed to be older, or smaller,
  or deliberately different (it has no console, no DMA, no runtime reconfiguration).
  Failing the build on a difference would mean the two copies must move together,
  which is exactly the coupling step 4 removed -- the report would then be pressure
  to re-merge them. What it is for is the review question "does this app-side HAL fix
  need to travel to src/boot/ too?", answered by a list instead of by memory.

  Read it after touching any src/app/hal_* file that src/boot/ also has, and when the boot
  image's size moves for no reason you can name.

  Comparison is path-for-path and byte-exact (CRLF included -- the fleet is
  eol=crlf, and a copy whose EOLs flipped is a real difference worth seeing).
#>
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    # Print every identical file too, not just the differences.
    [switch]$All
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path -LiteralPath $Root).Path
$bootDir = Join-Path $repoRoot 'src\boot'
$appDir = Join-Path $repoRoot 'src\app'

if (-not (Test-Path -LiteralPath $bootDir -PathType Container)) {
    Write-Host "No src/boot/ directory at $bootDir -- nothing to compare."
    exit 0
}

# Which modules the boot image vendored. Discovered rather than listed, so adding a
# seventh module to src/boot/ does not also require editing this script to be seen.
$modules = @(Get-ChildItem -LiteralPath $bootDir -Directory -Filter 'hal_*' |
    Sort-Object Name)
if ($modules.Count -eq 0) {
    Write-Host 'No src/boot/hal_* modules -- nothing to compare.'
    exit 0
}

function Get-FileBytes {
    param([string]$Path)
    return [IO.File]::ReadAllBytes($Path)
}

function Test-SameContent {
    param([string]$A, [string]$B)
    $x = Get-FileBytes $A
    $y = Get-FileBytes $B
    if ($x.Length -ne $y.Length) { return $false }
    for ($i = 0; $i -lt $x.Length; $i++) {
        if ($x[$i] -ne $y[$i]) { return $false }
    }
    return $true
}

$same = 0
$differ = [System.Collections.Generic.List[string]]::new()
$bootOnly = [System.Collections.Generic.List[string]]::new()
$appOnly = [System.Collections.Generic.List[string]]::new()

foreach ($module in $modules) {
    $appModule = Join-Path $appDir $module.Name
    if (-not (Test-Path -LiteralPath $appModule -PathType Container)) {
        # The boot copy has no counterpart at all. Worth saying plainly: it means the
        # module was removed or renamed on the application side and src/boot/ is now the
        # only copy in the tree.
        $bootOnly.Add("$($module.Name)/  (whole module -- src/app/$($module.Name) does not exist)")
        continue
    }

    $bootFiles = @(Get-ChildItem -LiteralPath $module.FullName -File -Recurse |
        ForEach-Object { $_.FullName.Substring($module.FullName.Length).TrimStart('\', '/').Replace('\', '/') })
    $appFiles = @(Get-ChildItem -LiteralPath $appModule -File -Recurse |
        ForEach-Object { $_.FullName.Substring($appModule.Length).TrimStart('\', '/').Replace('\', '/') })

    foreach ($rel in ($bootFiles | Sort-Object)) {
        $appFile = Join-Path $appModule $rel
        if (-not (Test-Path -LiteralPath $appFile -PathType Leaf)) {
            $bootOnly.Add("$($module.Name)/$rel")
            continue
        }
        if (Test-SameContent -A (Join-Path $module.FullName $rel) -B $appFile) {
            $same++
            if ($All) { Write-Host "  same    $($module.Name)/$rel" }
        } else {
            $differ.Add("$($module.Name)/$rel")
        }
    }

    foreach ($rel in ($appFiles | Sort-Object)) {
        if ($bootFiles -notcontains $rel) {
            $appOnly.Add("$($module.Name)/$rel")
        }
    }
}

Write-Host "HAL drift report: src/boot/hal_* vs src/app/hal_*  ($($modules.Count) module(s))"
Write-Host "  identical: $same   differing: $($differ.Count)   boot-only: $($bootOnly.Count)   app-only: $($appOnly.Count)"

if ($differ.Count -gt 0) {
    Write-Host ''
    Write-Host '  DIFFERS (the two copies have diverged):'
    foreach ($f in $differ) { Write-Host "    $f" }
}
if ($bootOnly.Count -gt 0) {
    Write-Host ''
    Write-Host '  BOOT ONLY (no counterpart under src/app/ -- removed or renamed there?):'
    foreach ($f in $bootOnly) { Write-Host "    $f" }
}
if ($appOnly.Count -gt 0) {
    Write-Host ''
    Write-Host '  APP ONLY (added under src/app/ after the copy; add it to src/boot/ only if boot needs it):'
    foreach ($f in $appOnly) { Write-Host "    $f" }
}

if ($differ.Count -eq 0 -and $bootOnly.Count -eq 0 -and $appOnly.Count -eq 0) {
    Write-Host '  The two copies are identical, path for path.'
}

Write-Host ''
Write-Host 'This report never fails. A difference is a question for review, not a defect:'
Write-Host 'the boot copy is allowed to differ (see this script header).'
exit 0
