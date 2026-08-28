param(
    [switch]$Full,
    [switch]$Clean,
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    # Extra preprocessor symbols, e.g. -Define @('RESIDENT_BOOT_ENA_BOOT_PAYLOAD_CRC=1').
    # Pass the array form explicitly: a bare multi-element list can be swallowed by the
    # next positional parameter.
    [string[]]$Define = @(),
    # Which part to build the image for, as src/boot/boot_image.psd1 spells it
    # ('33AK512MPS512', '33AK128MC106'). Empty = that file's DefaultDevice. Each device
    # has its own ConfigurationName, so build/ and dist/ never collide and -Clean only
    # removes the device you asked for.
    [string]$Device = ''
)

$ErrorActionPreference = 'Stop'

function Resolve-NewestVersionedFile {
    param([string]$Base, [string]$RelativeFile)

    $candidates = @(Get-ChildItem -LiteralPath $Base -Directory -Filter 'v*' -ErrorAction SilentlyContinue |
        ForEach-Object {
            $version = $null
            if ([Version]::TryParse($_.Name.TrimStart('v'), [ref]$version)) {
                $file = Join-Path $_.FullName $RelativeFile
                if (Test-Path -LiteralPath $file) {
                    [pscustomobject]@{ Version = $version; File = $file }
                }
            }
        } | Sort-Object Version -Descending)
    if ($candidates.Count -eq 0) {
        return $null
    }
    return $candidates[0].File
}

# This image compiles its OWN copy of the HAL, in src/boot/hal_*, and nothing from src/app/.
# Until reorg step 4 (2026-08-14) it shared the application's copy, so an
# application-side HAL change reached this image the next time it was built -- which
# is how the image once quietly grew 208 bytes. For an image pinned at fixed
# addresses under a hard 32 KiB cap, isolation is the right default and propagation
# is the exception: a fix travels src/app/hal_X/f -> src/boot/hal_X/f deliberately, and
# check_hal_drift.ps1 reports the divergence rather than forbidding it.
# The size and free space are still printed at the end of every build -- read them.
# Which sources an image was built from is answered by its commit: the boot banner
# prints it, so an older boot can always be reproduced by building that commit.

function Assert-ChildPath {
    param([string]$Parent, [string]$Child)

    $parentPath = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    $childPath = [IO.Path]::GetFullPath($Child)
    if (-not $childPath.StartsWith($parentPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $childPath"
    }
}

$repoRoot = (Resolve-Path -LiteralPath $Root).Path

# What this image is built from -- sources, include directories, macros, flags, the
# linker script and the 32 KiB cap -- comes from src/boot/boot_image.psd1, which
# generate_resident_project.ps1 also reads. One authority for both consumers, so the
# image you debug in the IDE is the image this script delivers. Do not re-inline any
# of these lists here: the second copy is the failure.
#
# $image below is that manifest AS ONE DEVICE SEES IT -- the shared lists plus this
# device's configuration name, linker script, defsyms and cap, merged and validated by
# boot_image.ps1. Everything after this point builds one image and has no idea the
# manifest describes more than one part.
. (Join-Path $PSScriptRoot 'boot_image.ps1')
$manifest = Get-BootImageManifest -RepoRoot $repoRoot
if ([string]::IsNullOrWhiteSpace($Device)) { $Device = $manifest.DefaultDevice }
$image = Get-BootImageForDevice -Image $manifest -Device $Device

$buildDir = Join-Path $repoRoot ('build\{0}\production' -f $image.ConfigurationName)
$distDir = Join-Path $repoRoot ('dist\{0}\production' -f $image.ConfigurationName)
Assert-ChildPath -Parent $repoRoot -Child $buildDir
Assert-ChildPath -Parent $repoRoot -Child $distDir

if ($Clean -or $Full) {
    foreach ($target in @($buildDir, $distDir)) {
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force
        }
    }
    if ($Clean) {
        Write-Host 'Resident bootloader outputs cleaned.'
        return
    }
}

# The generated IDE project must still say what the manifest says. Run here, before
# spending a toolchain on anything: this build does not depend on the project, but it
# is the moment somebody is looking, and a project that drifted is a debugging session
# spent on an image that is not this one. Absent script = incomplete checkout, which
# must not be able to stop a build (same rule as build.ps1's configuration gate).
$projectGate = Join-Path $PSScriptRoot 'check_resident_project.ps1'
if (Test-Path -LiteralPath $projectGate) {
    & $projectGate -Root $repoRoot
    if ($LASTEXITCODE -ne 0) {
        throw ('The generated resident project no longer matches src/boot/boot_image.psd1 (see above). ' +
               'Regenerate with buildtools/generate_resident_project.ps1, or fix the manifest.')
    }
} else {
    Write-Host 'NOTE: buildtools/check_resident_project.ps1 is absent - project gate skipped.'
}

$compiler = $env:XC_DSC_CC
if ([string]::IsNullOrWhiteSpace($compiler)) {
    $compiler = Resolve-NewestVersionedFile `
        -Base 'C:\Program Files\Microchip\xc-dsc' `
        -RelativeFile 'bin\xc-dsc-gcc.exe'
}
if ([string]::IsNullOrWhiteSpace($compiler) -or
    -not (Test-Path -LiteralPath $compiler)) {
    throw 'xc-dsc-gcc.exe was not found. Set XC_DSC_CC to its full path.'
}
$toolBin = Split-Path -Parent $compiler
$bin2hex = Join-Path $toolBin 'xc-dsc-bin2hex.exe'
$objdump = Join-Path $toolBin 'xc-dsc-objdump.exe'
if (-not (Test-Path -LiteralPath $bin2hex)) {
    throw "xc-dsc-bin2hex.exe was not found beside the compiler: $toolBin"
}
if (-not (Test-Path -LiteralPath $objdump)) {
    throw "xc-dsc-objdump.exe was not found beside the compiler: $toolBin"
}

# The device pack, per device: the MPS512 is in dsPIC33AK-MP_DFP and the MC106 in
# dsPIC33AK-MC_DFP, so this cannot be a constant (it was one while there was one part).
$dfpPack = $image.DfpPack
# Not $profile: that is a PowerShell automatic variable, and assigning to it is
# flagged by the analyser even where the scope makes it harmless.
$userProfile = [Environment]::GetFolderPath('UserProfile')
$packBase = Join-Path $userProfile (Join-Path '.mchp_packs\Microchip' $dfpPack)

# Whether a pack root actually supports THIS part, decided by the one file that says
# so. Every branch below asks, because a wrong pack does not announce itself: -mdfp
# simply supplies another part's headers and linker support.
function Test-SonoraDfpSupportsDevice {
    param([string]$Root, [string]$Device)
    if ([string]::IsNullOrWhiteSpace($Root)) { return $false }
    $header = Join-Path $Root ('xc16\support\dsPIC33A\h\p{0}.h' -f $Device)
    return (Test-Path -LiteralPath $header -PathType Leaf)
}

# Resolution order, and why it is this order.
#
# Until 2026-08-28 this was "DSPIC33AK_DFP if set, otherwise the newest installed
# version", and both halves were traps.
#
# DSPIC33AK_DFP is ONE variable while the two parts live in two DIFFERENT packs, so a
# value that is correct for one part is necessarily wrong for the other. That made a
# two-part build impossible to run: `check_publication*.ps1 -Build` builds all three
# configurations in a single pass, and no value of the variable let all three through.
#
# Newest-installed is worse than arbitrary, because the newest pack of each family
# breaks this image for its own unrelated reason (MC dropped PLL1CON.OE; MP stopped
# accepting NOBTSWP = OFF). Both read as defects in this repository, and both are
# invisible until someone builds in a fresh clone.
#
# So the version is pinned per device in src/boot/boot_image.psd1 (with the reason
# next to it), the override is still honoured but only when it can actually serve the
# part being built, and newest-installed is the last resort rather than the default.
$dfpRoot   = $null
$dfpOrigin = $null
$dfpPinned = $image.DfpPackVersion

if (-not [string]::IsNullOrWhiteSpace($env:DSPIC33AK_DFP)) {
    if (Test-SonoraDfpSupportsDevice -Root $env:DSPIC33AK_DFP -Device $image.Device) {
        $dfpRoot   = $env:DSPIC33AK_DFP
        $dfpOrigin = 'DSPIC33AK_DFP'
    }
    else {
        # Deliberately not fatal. The realistic cause is a variable set for the other
        # part, and throwing here would restore exactly the two-part deadlock above.
        Write-Host "  note: DSPIC33AK_DFP does not support $($image.Device); using the pinned pack for this device instead"
    }
}
if (-not $dfpRoot -and -not [string]::IsNullOrWhiteSpace($dfpPinned)) {
    $candidate = Join-Path $packBase $dfpPinned
    if (Test-SonoraDfpSupportsDevice -Root $candidate -Device $image.Device) {
        $dfpRoot   = $candidate
        $dfpOrigin = "boot_image.psd1 pin $dfpPinned"
    }
}
if (-not $dfpRoot) {
    $newest = @(Get-ChildItem -LiteralPath $packBase -Directory -ErrorAction SilentlyContinue |
        ForEach-Object {
            $version = $null
            if ([Version]::TryParse($_.Name, [ref]$version) -and
                (Test-SonoraDfpSupportsDevice -Root $_.FullName -Device $image.Device)) {
                [pscustomobject]@{ Version = $version; Path = $_.FullName }
            }
        } | Sort-Object Version -Descending | Select-Object -First 1)
    if ($newest.Count -ne 0) {
        $dfpRoot   = $newest[0].Path
        $dfpOrigin = "newest installed, $($newest[0].Version)"
        if (-not [string]::IsNullOrWhiteSpace($dfpPinned)) {
            Write-Host "  warning: pinned $dfpPack $dfpPinned is not installed; falling back to the $dfpOrigin -- see src/boot/boot_image.psd1 for why a pin exists"
        }
    }
}
if (-not $dfpRoot) {
    # Built separately: an `if` in argument position parses and then fails at runtime.
    $pinNote = ''
    if (-not [string]::IsNullOrWhiteSpace($dfpPinned)) { $pinNote = " (pinned version $dfpPinned)" }
    throw ("No installed $dfpPack supports $($image.Device). Looked under $packBase$pinNote. " +
           'Install that pack version, or point DSPIC33AK_DFP at a version whose ' +
           "xc16\support\dsPIC33A\h\p$($image.Device).h exists.")
}
$dfp = Join-Path $dfpRoot 'xc16'
Write-Host "  device pack: $dfpPack $(Split-Path $dfpRoot -Leaf) (from: $dfpOrigin)"

$sources = @($image.Sources)
if ($sources.Count -eq 0) { throw 'src/boot/boot_image.psd1 lists no sources.' }
foreach ($source in $sources) {
    if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $source) -PathType Leaf)) {
        throw "src/boot/boot_image.psd1 names a source that is not on disk: $source"
    }
}

# The contract in src/shared/ is reached by bare name through -Ishared. The boot
# internals are bare-name too, but ONLY because -Isrc/boot exists here and nowhere
# else: the application project has no include path into src/boot/, so a bare-name
# engine header there is a compile error rather than a working shortcut.
#
# NOT ONE ENTRY MAY NAME src/app/ -- the bulkhead (feasibility 8.1 rule 1), asserted
# here rather than trusted, because adding one back would make a broken build work
# again and that is the quietest kind of regression there is. Each directory is also
# required to exist: a non-existent -I path contributes nothing, so a typo or a
# half-finished rename would hide a missing header instead of reporting one.
$includes = @()
foreach ($dir in @($image.Includes)) {
    # The pattern names the APPLICATION TREE, and since the src/ reorg that is
    # src/app -- a bare "src" is no longer a synonym for it (src/ holds boot/ and
    # shared/ too), so matching "src" alone would fire on every legitimate include.
    # The optional prefix keeps a pre-reorg bare "app" caught as well.
    if ($dir -match '(?<![A-Za-z0-9_-])(?:src[/\\])?app(?=[/\\]|$)') {
        throw ("src/boot/boot_image.psd1 include directory '$dir' names the application tree. " +
               'The boot image compiles nothing from src/app/; vendor the file into src/boot/ instead.')
    }
    if (-not (Test-Path -LiteralPath (Join-Path $repoRoot $dir) -PathType Container)) {
        throw "src/boot/boot_image.psd1 include directory '$dir' does not exist."
    }
    $includes += "-I$dir"
}
# build/ and dist/ are shared across variants, so a -Define build must not be mistaken
# for the default one later. Report what was used and require -Full to switch.
$defineFlags = @($Define | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    ForEach-Object { "-D$_" })
if ($defineFlags.Count -ne 0) {
    Write-Host ('Extra defines: {0}' -f ($defineFlags -join ' '))
    if (-not $Full) {
        throw ('-Define changes the image; rerun with -Full so no object from a ' +
               'previous variant survives in build/.')
    }
}

# Which revision this boot image was built from, stamped in as a BARE TOKEN and
# stringified in C for the boot banner ("BL <commit>"). Same mechanism as the
# application's -DSONORA_GIT_COMMIT in build.ps1, and the same two traps: read
# $LASTEXITCODE immediately after the native call (piping into Select-Object can stop
# the pipeline early and leave it unset), and sanitize to [A-Za-z0-9_] so the result is
# always one valid C token. A dirty tree becomes "<hash>_dirty"; no Git means "unknown".
#
# This is how an older boot image is identified and reproduced: build that commit.
$bootRevision = 'unknown'
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($null -ne $gitCmd) {
    $commitRaw = & $gitCmd.Source -C $repoRoot rev-parse --short=7 HEAD 2>$null
    $commitOk  = ($LASTEXITCODE -eq 0)
    $commit    = if ($null -ne $commitRaw) {
        ([string]($commitRaw | Select-Object -First 1)).Trim()
    } else { '' }
    if ($commitOk -and -not [string]::IsNullOrWhiteSpace($commit)) {
        $bootRevision = $commit
        $statusRaw = & $gitCmd.Source -C $repoRoot status --porcelain --untracked-files=normal 2>$null
        $statusOk  = ($LASTEXITCODE -eq 0)
        if ($statusOk) {
            $dirty = @($statusRaw | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            if ($dirty.Count -gt 0) { $bootRevision += '_dirty' }
        }
    }
}
$bootRevision = ($bootRevision -replace '[^A-Za-z0-9_]', '_')

# Manifest lists -> command-line flags. Kept next to the build so the mapping from
# the data file to what the compiler actually sees is one short read.
$compilerFlags = @($image.CompilerFlags)
$macroFlags = @(@($image.Macros) | ForEach-Object { "-D$_" })

New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null
Push-Location $repoRoot
try {
    $objects = @()
    foreach ($source in $sources) {
        $objectName = (($source -replace '[\\/]', '_') -replace '\.(c|S)$', '.o')
        $object = Join-Path $buildDir $objectName
        & $compiler "-mcpu=$($image.Device)" @compilerFlags @macroFlags `
            "-DSONORA_BOOT_GIT_COMMIT=$bootRevision" `
            @defineFlags @includes -c $source -o $object "-mdfp=$dfp"
        if ($LASTEXITCODE -ne 0) {
            throw "Resident bootloader compile failed: $source"
        }
        $objects += $object
    }

    $elf = Join-Path $distDir 'resident_bootloader.production.elf'
    $map = Join-Path $distDir 'resident_bootloader.production.map'
    # Assembled from the manifest in a fixed order -- script, options, -Map, defsyms --
    # so that the same order can be asserted against the generated project's linker
    # options and the two are comparable by reading rather than by inference.
    $defsymFlags = @(@($image.Defsym.Keys) | Sort-Object |
        ForEach-Object { "--defsym=$_=$($image.Defsym[$_])" })
    $linkerOptions = (@("--script=$($image.LinkerScript)") +
        @($image.LinkerOptions) + @("-Map=$map") + $defsymFlags) -join ','
    & $compiler -o $elf @objects "-mcpu=$($image.Device)" `
        "-Wl,$linkerOptions" "-mdfp=$dfp"
    if ($LASTEXITCODE -ne 0) {
        throw 'Resident bootloader link failed.'
    }

    & $bin2hex $elf -a -omf=elf "-mdfp=$dfp"
    if ($LASTEXITCODE -ne 0) {
        throw 'Resident bootloader HEX conversion failed.'
    }

    $hex = [IO.Path]::ChangeExtension($elf, '.hex')
    # Every post-link guarantee lives in one script so the MPLAB X project can run
    # the identical set from a post-build step. Do not re-inline checks here.
    # -MaxBytes from the manifest rather than from the verifier's own default, so the
    # cap is stated in exactly one place for both consumers.
    & (Join-Path $PSScriptRoot 'verify_resident_image.ps1') `
        -Elf $elf -Map $map -Hex $hex -Objdump $objdump -Dfp $dfp `
        -MaxBytes $image.SizeCapBytes
    Write-Host "ELF: $elf"
    Write-Host "HEX: $hex"
    Write-Host "MAP: $map"
}
finally {
    Pop-Location
}
