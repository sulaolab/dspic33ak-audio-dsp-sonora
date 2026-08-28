# SPDX-FileCopyrightText: 2026 SulaoLab
# SPDX-License-Identifier: MIT-0

param()

$ErrorActionPreference = 'Stop'

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir '..\..\..')).Path
$halDir = Join-Path $repoRoot 'src\app\hal_ccp_input_capture'
$outDir = Join-Path $testDir 'out'
$vsDevCmd = @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter VsDevCmd.bat -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1)

if ($vsDevCmd.Count -eq 0) {
    throw 'Visual Studio VsDevCmd.bat was not found.'
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$q = [char]34
$setup = 'call {0}{1}{0} -arch=x64 -host_arch=x64 >nul' -f $q, $vsDevCmd[0].FullName
$commands = @(
    $setup,
    ('cl /nologo /W4 /WX /std:c11 /D__dsPIC33AK512MPS512__ /I{0}{1}{0} /Fe:{0}{2}\test_header_api.exe{0} {0}{3}\test_header_api.c{0}' -f $q, $halDir, $outDir, $testDir),
    ('{0}{1}\test_header_api.exe{0}' -f $q, $outDir),
    ('cl /nologo /W4 /WX /std:c11 /I{0}{1}{0} /Fe:{0}{2}\test_header_reg.exe{0} {0}{3}\test_header_reg.c{0}' -f $q, $halDir, $outDir, $testDir),
    ('{0}{1}\test_header_reg.exe{0}' -f $q, $outDir),
    ('cl /nologo /W4 /WX /std:c11 /D__dsPIC33AK512MPS512__ /I{0}{1}{0} /I{0}{2}\fake_xc{0} /Fe:{0}{3}\test_validation.exe{0} {0}{2}\test_validation.c{0} {0}{1}\nora_ccp_input_capture_dspic33ak.c{0}' -f $q, $halDir, $testDir, $outDir),
    ('{0}{1}\test_validation.exe{0}' -f $q, $outDir),
    # The public header is device-neutral by design (docs_public/nora_hal_public_api.md):
    # it must compile for a part with no CCP inventory, with no opt-in macro and no
    # #error. Device selection lives in the backend's DFP capability test, which the
    # unsupported-backend stage below is what actually guards.
    ('cl /nologo /W4 /WX /std:c11 /D__dsPIC33AK128MC106__ /I{0}{1}{0} /Fe:{0}{2}\test_header_ak128.exe{0} {0}{3}\test_header_ak128.c{0}' -f $q, $halDir, $outDir, $testDir),
    ('{0}{1}\test_header_ak128.exe{0}' -f $q, $outDir),
    # Backend compiled against a CCP9-less xc.h: every entry point must report
    # unavailable, and must EXIST (a call added to the full-map branch only shows up
    # here as a link error).
    ('cl /nologo /W4 /WX /std:c11 /D__dsPIC33AK128MC106__ /I{0}{1}{0} /I{0}{2}\fake_xc_no_ccp9{0} /Fe:{0}{3}\test_unsupported.exe{0} {0}{2}\test_unsupported.c{0} {0}{1}\nora_ccp_input_capture_dspic33ak.c{0}' -f $q, $halDir, $testDir, $outDir),
    ('{0}{1}\test_unsupported.exe{0}' -f $q, $outDir)
)

& cmd.exe /d /c ($commands -join ' && ')
if ($LASTEXITCODE -ne 0) {
    throw "Host tests failed with exit code $LASTEXITCODE."
}

Write-Host 'CCP Input Capture host tests: PASS'
