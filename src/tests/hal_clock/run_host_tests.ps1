param()

$ErrorActionPreference = 'Stop'

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$halDir = Join-Path $repoRoot 'src\app\hal_clock'
$outDir = Join-Path $testDir 'out'
$vsDevCmd = @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1)

if ($vsDevCmd.Count -eq 0) {
    throw 'Visual Studio VsDevCmd.bat was not found.'
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$q = [char]34
$setup = 'call {0}{1}{0} -arch=x64 -host_arch=x64 >nul' -f $q, $vsDevCmd[0].FullName

# One stage, deliberately: the contract, the portable core, the device table and
# the register layer are linked together against the host model of the register
# file (fake_xc/), because what is under test is the whole path from the public
# call down to the bit that actually commits a switch. Splitting it would only
# let a layer pass in isolation while the composition was wrong.
#
# /D__dsPIC33AK512MPS512__ selects the device the backend's DFP capability tests
# expect; fake_xc/xc.h stands in for the DFP itself.
$commands = @(
    $setup,
    ('cl /nologo /W4 /WX /std:c11 /D__dsPIC33AK512MPS512__ /I{0}{1}{0} /I{0}{2}\fake_xc{0} /Fe:{0}{3}\test_nora_clock_contract.exe{0} {0}{2}\test_nora_clock_contract.c{0} {0}{1}\nora_clock_dspic33ak.c{0} {0}{1}\nora_clock_device_dspic33ak.c{0} {0}{1}\nora_clock_dspic33ak_reg.c{0} {0}{2}\fake_xc\nora_fake_clock.c{0}' -f $q, $halDir, $testDir, $outDir),
    ('{0}{1}\test_nora_clock_contract.exe{0}' -f $q, $outDir)
)

& cmd.exe /d /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) {
    throw "Host tests failed with exit code $LASTEXITCODE."
}

Write-Host 'Clock HAL host tests: PASS'
