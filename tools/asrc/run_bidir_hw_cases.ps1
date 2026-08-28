<#
  The S7 hardware cases for the 16ch Q31 BiDir front end, driven through the
  serial-monitor HTTP API (never the COM port).

  Why a script: each case is "set both leg rates, let the servo settle, zero the
  counters, watch several report cycles, then read the worst numbers back". Done
  by hand that is a dozen round trips per case and the observation window ends up
  different every time. Here the wait is /wait -- it blocks on the board's own
  report line, so the window is counted in report cycles, not in sleeps.

  Cases (see the closing instructions):
    A   48 <-> 44.1 kHz, both directions live, no front end (fe=direct)
    B1  A=48k B=16k -- worst front end (/3) on A->B
    B2  A=16k B=48k -- the SAME front end must start on B->A
    C   48 <-> 8 kHz -- deepest history (/6, 190 taps), Y modulo wrap

  Usage: pwsh tools/asrc/run_bidir_hw_cases.ps1 [-Base http://127.0.0.1:8080]
                                                [-Cycles 6] [-Only A,B1,B2,C]
#>
param(
    [string]$Base = 'http://127.0.0.1:8080',
    [int]$Cycles = 6,
    [string[]]$Only
)

$ErrorActionPreference = 'Stop'

# *ar rate indices, from asrc_console.c.
$RATE = @{ 8000 = 0; 11025 = 1; 12000 = 2; 16000 = 3; 22050 = 4; 24000 = 5; 32000 = 6; 44100 = 7; 48000 = 8 }

$CASES = @(
    @{ Name = 'A';  A = 48000; B = 44100; Expect = 'no front end (fe=direct)' }
    @{ Name = 'B1'; A = 48000; B = 16000; Expect = 'front end /3 on A->B' }
    @{ Name = 'B2'; A = 16000; B = 48000; Expect = 'front end /3 on B->A' }
    @{ Name = 'C';  A = 48000; B =  8000; Expect = 'front end /6 on A->B, 190-tap history' }
)

function Send-Cmd([string]$cmd) {
    $body = @{ cmd = $cmd } | ConvertTo-Json -Compress
    $null = Invoke-RestMethod -Uri "$Base/command" -Method Post -ContentType 'application/json' -Body $body
    Write-Host ("    -> " + $cmd)
}

function Wait-For([string]$text, [int]$timeout) {
    # Arm-then-send is the caller's job; this only ever waits on periodic output.
    $body = @{ contains = $text; timeout = $timeout } | ConvertTo-Json -Compress
    try { return Invoke-RestMethod -Uri "$Base/wait" -Method Post -ContentType 'application/json' -Body $body }
    catch { return $null }
}

function Assert-RateSet([int]$leg, [int]$hz) {
    # Not the "$<status><name>" acknowledgement: that reply is three bytes long and
    # the monitor logs whatever arrives in one read, so it turns up split across a
    # "[partial] $80" line and a hexdump of "ar" -- the string "$80ar" then exists
    # in no single line at all.  The handler's own sentence is a whole line, and it
    # also names the rate it accepted, which is the thing worth confirming.
    $who  = if ($leg -eq 0) { 'A' } else { 'B' }
    $good = ('"\*ar" WM8904-' + $who + ' rate -> ' + $hz + ' Hz')
    $bad  = '"\*ar".*(bad args|not available|refus)'
    foreach ($try in 1..60) {
        $log = @(Get-AnyLog 60)
        if ($log -match $bad)  { throw ("the board refused the rate: " + (($log -match $bad)[-1]).Trim()) }
        if ($log -match $good) { return }
        Start-Sleep -Milliseconds 500
    }
    throw ("no answer to '*ar' for leg $who at $hz Hz -- the console did not accept it")
}

function Get-Log([int]$tail) {
    (Invoke-RestMethod -Uri "$Base/log?tail=$tail").lines |
        Where-Object { $_ -notmatch 'raw len=|\[partial' }
}

function Get-AnyLog([int]$tail) {
    # Unfiltered, for short replies.  When several lines arrive inside one read the
    # monitor logs the chunk as a hexdump ("raw len=18 ... |arting).$80ar.|") instead
    # of as lines, and the acknowledgement is then only in that dump -- filtering
    # those out is what made a successful command look unanswered.
    (Invoke-RestMethod -Uri "$Base/log?tail=$tail").lines
}

# The board must be the one we think it is before anything is sent to it.
$status = Invoke-RestMethod -Uri "$Base/status"
Write-Host ("monitor: profile={0} port={1} connected={2}" -f $status.profile, $status.port, $status.connected)
if (-not $status.connected) { throw "the monitor at $Base is not connected to its port" }

# Liveness: a console that ignores input makes every case below silently pass its
# rate change and report the PREVIOUS rate. Proven before, not assumed.
Write-Host 'console liveness check'
# "?gv" and not a servo query: its answer is a fixed banner, so a missing reply
# means the parser never saw the line -- it cannot also mean "that field was not
# printed in this build".
#
# Read back from the log instead of /wait: the banner comes back about 2 ms after
# the send, and /wait only sees bytes that arrive after the call, so arming it in
# time is a race this check would lose (and report as a dead console). The log
# already holds the reply by the time we look.
Send-Cmd '?gv'
$alive = $false
foreach ($try in 1..6) {
    if (@(Get-AnyLog 40) -match 'SONORA console-v2') { $alive = $true; break }
    Start-Sleep -Milliseconds 500
}
if (-not $alive) {
    throw ('the console did not answer "?gv": input is not reaching the parser. ' +
           'Nothing below would mean anything, so stopping here.')
}
Write-Host '  console answers'

$report = New-Object System.Collections.Generic.List[object]

foreach ($case in $CASES) {
    if ($Only -and ($Only -notcontains $case.Name)) { continue }

    Write-Host ''
    Write-Host ("=== case {0}: A={1} Hz  B={2} Hz  -- {3}" -f $case.Name, $case.A, $case.B, $case.Expect)

    foreach ($leg in @(@{ i = 0; hz = $case.A }, @{ i = 1; hz = $case.B })) {
        # The wire format is <kind><module><name><hex payload> with NO separators
        # (app_console.c: every pair of characters after the name is one payload
        # byte).  "*ar 00 08" parses as a bad hex pair and comes back as $02ar
        # with the rate untouched -- which reads exactly like a dead console.
        Send-Cmd ('*ar{0:x2}{1:x2}' -f $leg.i, $RATE[$leg.hz])
        Assert-RateSet $leg.i $leg.hz
        $null = Wait-For 'TDM_activated' 30
    }

    # Ground truth for "the rates really are what this case says": the board's own
    # measured frame clocks, not the fact that a command was accepted.
    for ($i = 0; $i -lt 2; $i++) { $null = Wait-For 'TDMsum' 30 }
    $ccp = @(Get-Log 60) | Where-Object { $_ -match 'CCP\s+fsA=' } | Select-Object -Last 1
    if (-not ($ccp -match 'fsA=([0-9.]+)\s+fsB=([0-9.]+)')) { throw "no CCP clock report to confirm the rates" }
    foreach ($want in @(@{ n = 'A'; hz = $case.A; got = [double]$matches[1] },
                        @{ n = 'B'; hz = $case.B; got = [double]$matches[2] })) {
        if ([Math]::Abs($want.got - $want.hz) / $want.hz -gt 0.03) {
            throw ("leg {0} is running at {1:N1} Hz, not the {2} Hz this case needs" -f
                   $want.n, $want.got, $want.hz)
        }
    }
    Write-Host ("    rates confirmed: " + $ccp.Trim())

    # There is no counter-reset command in this image: "*as" and the rest of the
    # servo/capture verbs live inside #if APP_ASRC_MEAS in asrc_console.c, and the
    # shipping BIDIR profile builds with APP_ASRC_MEAS=0 (the board answers $01as,
    # ERR_NOT_FOUND).  So the counters keep the pull-in transient of the rate change
    # and cannot be read as absolutes; what is measured below is GROWTH across a
    # window that starts after the servo has settled.  Two report cycles of settling
    # first, then the window itself.
    for ($i = 0; $i -lt 2; $i++) { $null = Wait-For 'TDMsum' 30 }
    for ($i = 0; $i -lt $Cycles; $i++) { $null = Wait-For 'TDMsum' 30 }

    $lines = Get-Log 400
    $sum  = @($lines | Where-Object { $_ -match 'TDMsum:' })
    # 2026-08-27: the engine line carries hr= (fmin-R) instead of fill=/fmin=/R=.
    $poly = @($lines | Where-Object { $_ -match 'poly.*\]A?B?A?.*hr=' })
    $path = @($lines | Where-Object { $_ -match 'ASRCpath\[' })
    $tdm  = @($lines | Where-Object { $_ -match 'TDM[12]:' })

    function Worst([string[]]$ls, [string]$rx) {
        $v = $ls | ForEach-Object { if ($_ -match $rx) { [double]$matches[1] } }
        if ($v) { ($v | Measure-Object -Maximum).Maximum } else { $null }
    }
    function Least([string[]]$ls, [string]$rx) {
        $v = $ls | ForEach-Object { if ($_ -match $rx) { [double]$matches[1] } }
        if ($v) { ($v | Measure-Object -Minimum).Minimum } else { $null }
    }

    $window = $sum | Select-Object -Last $Cycles
    $pwin = $poly | Select-Object -Last (2 * $Cycles)

    # drop= and starve= are cumulative and carry the pull-in transient of the rate
    # change, so the finding is growth across the settled window, not a non-zero
    # absolute value.  First and last reading of each leg, per counter.
    $bad = @()
    foreach ($name in @('drop', 'starve')) {
        foreach ($leg in @('AB', 'BA')) {
            $v = @($pwin | Where-Object { $_ -match ("\]" + $leg + " ") } |
                   ForEach-Object { if ($_ -match ($name + '=(\d+)')) { [int]$matches[1] } })
            if ($v.Count -ge 2 -and $v[-1] -ne $v[0]) {
                $bad += ('{0}[{1}] grew {2}->{3}' -f $name, $leg, $v[0], $v[-1])
            }
        }
    }
    # miss and sat are cumulative too, but they are expected to be flat at zero, so
    # a non-zero absolute value is itself the finding -- no baseline needed.
    foreach ($t in ($tdm | Select-Object -Last (2 * $Cycles))) {
        if ($t -match 'miss\)=\((\d+),(\d+),(\d+),(\d+)\)' -and [int]$matches[4] -ne 0) { $bad += 'miss=' + $matches[4] }
    }
    foreach ($s in $window) { if ($s -match 'sat=(\d+)' -and [int]$matches[1] -ne 0) { $bad += 'sat=' + $matches[1] } }
    foreach ($c in @($lines | Where-Object { $_ -match 'CCP\s+fsA=' } | Select-Object -Last $Cycles)) {
        if ($c -match 'recover=(\d+)' -and [int]$matches[1] -ne 0) { $bad += 'recover=' + $matches[1] }
    }

    $fe = @($pwin | ForEach-Object { if ($_ -match 'fe=(\S+)') { $matches[1] } } | Sort-Object -Unique) -join ','

    # "no growth" and not "all zero": drop= carries the pull-in transient, so the
    # claim being made is that it stopped moving, plus miss/sat/recover at zero.
    $counters = 'no growth (miss/sat/recover 0)'
    if ($bad.Count -ne 0) { $counters = ($bad | Sort-Object -Unique) -join ' ' }

    $row = [pscustomobject]@{
        Case          = $case.Name
        Rates         = ('{0}/{1}' -f $case.A, $case.B)
        FrontEnd      = $fe
        # TDMsum:max is a max-HOLD, reset only when the stream epoch changes, so the
        # window's last reading includes the rate-change pull-in.  Reporting whether
        # it still moved during the window is what separates "this is the steady-state
        # load" from "this was the transient of the change".
        WorstSumUs    = Worst $window 'TDMsum:.*?max=([0-9.]+)us'
        SumMaxMoved   = ((Worst $window 'TDMsum:.*?max=([0-9.]+)us') -ne (Least $window 'TDMsum:.*?max=([0-9.]+)us'))
        LeastMarginUs = Least $window 'margin=([0-9.]+)us'
        WorstCbAUs    = Worst ($path | Select-Object -Last $Cycles) 'cbA=([0-9.]+)us'
        WorstCbBUs    = Worst ($path | Select-Object -Last $Cycles) 'cbB=([0-9.]+)us'
        Counters      = $counters
    }
    $report.Add($row)
    $row | Format-List | Out-String | Write-Host
    ($pwin | Select-Object -Last 2) -join "`n" | Write-Host
}

Write-Host ''
Write-Host '=== summary ==='
$report | Format-Table -AutoSize | Out-String | Write-Host
