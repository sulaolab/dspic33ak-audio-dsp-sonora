param()

# Host tests for hal_noinit_ram. The HAL is a compile-time module -- it reserves RAM and
# has no runtime logic -- so these check the contract:
#   test_header_api.c  a correct configuration compiles clean, and AS()/FITS behave
#   test_reject.c      all 6 ways of misconfiguring it FAIL the build
#
# The 6 rejects are the safety story: a placement-only HAL has no runtime check to catch
# a mistake later, so every mistake has to be a build failure.
#
# Not covered here (needs the toolchain / a board -- see [internal] resident_serial_bootloader.md part 3):
#   the .c itself, whose persistent/space/address attributes are XC-DSC only
#   6.2 that the block lands at the requested address and stays out of .dinit
#   6.3 that the bytes actually survive a warm reset and are lost on a cold one
#
# Same harness shape as tests/hal_ccp_input_capture/run_host_tests.ps1.

$ErrorActionPreference = 'Stop'

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$halDir = Join-Path $repoRoot 'src\app\hal_noinit_ram'
$outDir = Join-Path $testDir 'out'
$vsDevCmd = @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1)

if ($vsDevCmd.Count -eq 0) {
    throw 'Visual Studio VsDevCmd.bat was not found.'
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$q = [char]34
$setup = 'call {0}{1}{0} -arch=x64 -host_arch=x64 >nul' -f $q, $vsDevCmd[0].FullName

# --- positive: a correct configuration builds and runs ---
$commands = @(
    $setup,
    ('cd /d {0}{1}{0}' -f $q, $outDir),
    ('cl /nologo /W4 /WX /std:c11 /I{0}{1}{0} /Fe:{0}{2}\test_header_api.exe{0} {0}{3}\test_header_api.c{0}' -f $q, $halDir, $outDir, $testDir),
    ('{0}{1}\test_header_api.exe{0}' -f $q, $outDir)
)

& cmd.exe /d /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) {
    throw "hal_noinit_ram positive host test failed with exit code $LASTEXITCODE."
}

# --- negative: each misconfiguration must be refused ---
$rejectNames = @(
    'no NORA_NOINIT_RAM_LINKER_RESERVED',
    'no NORA_NOINIT_RAM_ADDRESS',
    'no NORA_NOINIT_RAM_SIZE',
    'misaligned address',
    'zero size',
    'struct larger than the reservation'
)

for ($case = 1; $case -le 6; $case++) {
    $rejectCommand = $setup + ' && ' + ('cl /nologo /std:c11 /DREJECT_CASE={0} /I{1}{2}{1} /c {1}{3}\test_reject.c{1} /Fo:{1}{4}\reject{0}.obj{1} >nul 2>&1' -f $case, $q, $halDir, $testDir, $outDir)
    & cmd.exe /d /c $rejectCommand
    if ($LASTEXITCODE -eq 0) {
        throw ("hal_noinit_ram reject case {0} ({1}) COMPILED -- it must be refused." -f $case, $rejectNames[$case - 1])
    }
}

Write-Host ('No-init RAM HAL host tests: PASS (1 positive, {0} rejects)' -f $rejectNames.Count)

# The loop above ends on a compile that is SUPPOSED to fail, so $LASTEXITCODE is still
# cl's error code (2) at this point. Without this the script reports PASS and then exits
# nonzero -- which any CI gate reads as a failed suite. The reject cases are asserted by
# the throws above; the process exit code is ours to set.
exit 0
