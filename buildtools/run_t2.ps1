# T2 -- platform bring-up runner (full_test.md section 7).
# Talks to the board only through the serial-monitor HTTP bridge. Never opens the COM port.
#
#   pwsh -NoProfile -File run_t2.ps1 -ExpectCommit 3c1222d [-Base http://127.0.0.1:8080]
#        [-BoardProfile sonora] [-TickSeconds 12] [-IdleMinutes 0] [-Transcript <path>]
#
# -IdleMinutes > 0 runs the T2.5 idle soak (5 = the documented value).
# T2.8 (*sr software reset) is only run with -Reset, because it interrupts audio.
# The runner leaves telemetry OFF; re-enable with *tq0001 when done.

param(
  [string]$Base         = 'http://127.0.0.1:8080',
  [string]$BoardProfile = 'sonora',
  [string]$ExpectCommit = '',
  [int]   $TickSeconds  = 30,
  [int]   $TelemetryMs  = 1000,
  [double]$NominalBlkRate = 1500.0,   # 48 kHz / 32-sample block
  [int]   $IdleMinutes  = 0,
  [switch]$Reset,
  [string]$Transcript   = ''
)

$ErrorActionPreference = 'Stop'
$script:results = @()
$script:log     = @()

function Note([string]$s) { Write-Host $s; $script:log += $s }

function Add-Result([string]$id, [string]$verdict, [string]$note) {
  $script:results += [pscustomobject]@{ id = $id; verdict = $verdict; note = $note }
  Note ("[{0,-6}] {1,-5} {2}" -f $verdict, $id, $note)
}

function Get-Status { Invoke-RestMethod -Uri "$Base/status" -TimeoutSec 10 }

# Send a console command and return the log lines that arrived after the bridge echoed it.
# The bridge's own '>> [source=http-command] <cmd>' echo is the cursor, so there is no race
# with /wait (which only observes bytes seen after the call is made).
function Send-Verb([string]$cmd, [int]$settleMs = 900, [int]$tail = 120) {
  Invoke-RestMethod -Uri "$Base/command" -Method Post -ContentType 'application/json' `
                    -Body (@{ cmd = $cmd } | ConvertTo-Json -Compress) -TimeoutSec 10 | Out-Null
  Start-Sleep -Milliseconds $settleMs
  $lines  = (Invoke-RestMethod -Uri "$Base/log?tail=$tail" -TimeoutSec 10).lines
  $marker = "[source=http-command] $cmd"
  $idx    = -1
  # plain substring search: -like would read '[' in the marker as a wildcard character class
  for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i].Contains($marker)) { $idx = $i; break } }
  if ($idx -lt 0 -or $idx -ge $lines.Count - 1) { return @() }
  $out = @()
  foreach ($l in $lines[($idx + 1)..($lines.Count - 1)]) {
    $t = $l -replace '^\d\d:\d\d:\d\d\.\d\d\d\s+<<\s+\[[^\]]*\]\s*', ''
    $t = $t -replace '\x1b\[[0-9;]*m', '' -replace '\x1b\(B', '' -replace '\x0f', ''
    if ($t -match '^\s*$') { continue }
    if ($t.Trim() -eq $cmd) { continue }
    $out += $t.TrimEnd()
  }
  return $out
}

function Get-LogTail([int]$tail) { (Invoke-RestMethod -Uri "$Base/log?tail=$tail" -TimeoutSec 15).lines }

# ---- fixtures ---------------------------------------------------------------------------
$st = Get-Status
Note ("bridge : profile={0} port={1} baud={2} connected={3} tcp_clients=[{4}]" -f `
      $st.profile, $st.port, $st.baud, $st.connected, ($st.tcp.clients -join ','))
Note ("logfile: {0}" -f $st.log_file)
if ($st.profile -ne $BoardProfile) { throw "wrong board: bridge profile '$($st.profile)' != '$BoardProfile'" }
if (-not $st.connected)            { throw "bridge is not connected to $($st.port)" }

# ---- quiet the console so every capture below is readable --------------------------------
$q = Send-Verb '*tq0000'
Note ("telemetry gate: " + (($q -join ' | ')))

# ---- T2.2 running image -----------------------------------------------------------------
$gv     = Send-Verb '?gv'
$gvLine = ($gv | Where-Object { $_ -match 'SONORA' } | Select-Object -First 1)
if (-not $gvLine) {
  Add-Result 'T2.2' 'FAIL' ('?gv produced no version line: ' + ($gv -join ' | '))
} elseif ($ExpectCommit -and $gvLine -notmatch [regex]::Escape($ExpectCommit)) {
  Add-Result 'T2.2' 'FAIL' ("image is not the commit under test (want $ExpectCommit): " + $gvLine.Trim())
} elseif ($gvLine -match '_dirty') {
  Add-Result 'T2.2' 'FAIL' ('image was built from a dirty tree: ' + $gvLine.Trim())
} else {
  Add-Result 'T2.2' 'PASS' $gvLine.Trim()
}

# ---- T2.4 board identity (?si), read twice for stability --------------------------------
$si1 = (Send-Verb '?si') -join ' / '
$si2 = (Send-Verb '?si') -join ' / '
if ($si1 -and $si1 -eq $si2) { Add-Result 'T2.4' 'PASS' $si1 }
elseif (-not $si1)           { Add-Result 'T2.4' 'FAIL' 'no answer to ?si' }
else                         { Add-Result 'T2.4' 'FAIL' "unstable: [$si1] vs [$si2]" }

# ---- T2.5 (part 1) reset cause / trap latch at this boot -------------------------------
$sr = (Send-Verb '?sr') -join ' / '
Add-Result 'T2.5a' $(if ($sr) { 'INFO' } else { 'FAIL' }) ("?sr: " + $sr)

# ---- T2.6 console liveness: hello answers, unknown verb is refused ---------------------
$gh = (Send-Verb '?gh') -join ' / '
$zz = (Send-Verb '?zz') -join ' / '
if ($gh -match 'hello') { Add-Result 'T2.6a' 'PASS' ("?gh -> " + $gh) }
else                    { Add-Result 'T2.6a' 'FAIL' ("?gh -> " + $gh) }
if ($zz) { Add-Result 'T2.6b' 'PASS' ("unknown verb ?zz -> " + $zz) }
else     { Add-Result 'T2.6b' 'FAIL' 'unknown verb ?zz answered with silence' }

# ---- Classic status (context for T4, not a T2 gate) ------------------------------------
$cs = (Send-Verb '?cs' 1200) -join ' / '
Note ("?cs   : " + $cs)

# ---- T2.3 / T2.7 clock and 1 ms tick ---------------------------------------------------
# There is no console verb that reads the millisecond counter (checked: general/system/diag/
# classic/transport modules print no counter). What can be measured instead:
#   * the telemetry print cadence is driven by GetTicks(), so its wall-clock interval
#     measures the ms tick;
#   * the TDM 'blk' counter advances one per audio block, so its rate measures the audio
#     clock (48 kHz / 32 samples = 1500 blk/s at the nominal profile).
# Both numbers are taken from the SAME telemetry lines, timestamped by the bridge: the host's
# own Get-Date around a sleep is not usable, because a telemetry line can be up to one period
# old when the tail is read, which on a 3 s period is a 20 % error over a 15 s window. The
# window is also cut at the bridge's echo of the period command, or the tail reaches back into
# the previous (or switched-off) cadence and the mean is meaningless.
$period  = $TelemetryMs
$tqCmd   = "*tq0002{0:X4}" -f $period
$null    = Send-Verb $tqCmd 600
Start-Sleep -Seconds $TickSeconds
$tail    = Get-LogTail 400
$cut     = -1
for ($i = $tail.Count - 1; $i -ge 0; $i--) { if ($tail[$i].Contains("[source=http-command] $tqCmd")) { $cut = $i; break } }
$window  = if ($cut -ge 0 -and $cut -lt $tail.Count - 1) { $tail[($cut + 1)..($tail.Count - 1)] } else { @() }

$samples = @()
foreach ($l in $window) {
  if ($l -match '^(\d\d):(\d\d):(\d\d\.\d\d\d)\b' ) {
    $ts = [double]$Matches[1] * 3600.0 + [double]$Matches[2] * 60.0 + [double]$Matches[3]
    if ($l -match 'TDM1:(?:max|resp).*\(run,act,blk,miss\)=\((\d+),(\d+),(\d+),(\d+)\)') {
      $samples += [pscustomobject]@{ t = $ts; blk = [double]$Matches[3]; miss = [int]$Matches[4] }
    }
  }
}

if ($samples.Count -ge 2) {
  $f = $samples[0]; $l = $samples[-1]
  $dt = $l.t - $f.t
  if ($dt -gt 0) {
    $rate = ($l.blk - $f.blk) / $dt
    $err  = [math]::Abs($rate - $NominalBlkRate) / $NominalBlkRate * 100.0
    $note = ("blk rate = {0:N2}/s over {1:N3}s ({2} samples; nominal {3:N1}/s, err {4:N3}%), miss={5}" -f `
             $rate, $dt, $samples.Count, $NominalBlkRate, $err, $l.miss)
    if ($err -le 1.0 -and $l.miss -eq 0) { Add-Result 'T2.3' 'PASS' $note } else { Add-Result 'T2.3' 'FAIL' $note }
  } else { Add-Result 'T2.3' 'NOTRUN' 'telemetry samples share one timestamp' }
} else {
  Add-Result 'T2.3' 'NOTRUN' ("only {0} telemetry samples inside the window" -f $samples.Count)
}

# telemetry cadence -> ms tick (the print cadence is driven by GetTicks())
if ($samples.Count -ge 4) {
  $d = @(); for ($i = 1; $i -lt $samples.Count; $i++) { $d += ($samples[$i].t - $samples[$i-1].t) }
  $mean = ($d | Measure-Object -Average).Average
  $err  = [math]::Abs($mean * 1000.0 - $period) / $period * 100.0
  $note = ("telemetry cadence mean = {0:N3}s over {1} intervals (commanded {2}ms, err {3:N2}%)" -f `
           $mean, $d.Count, $period, $err)
  if ($err -le 1.0) { Add-Result 'T2.7' 'PASS' $note } else { Add-Result 'T2.7' 'FAIL' $note }
} else {
  Add-Result 'T2.7' 'NOTRUN' ("only {0} telemetry samples inside the window; raise -TickSeconds" -f $samples.Count)
}

# ---- T2.5 (part 2) idle soak -----------------------------------------------------------
if ($IdleMinutes -gt 0) {
  $null = Send-Verb '*tq0000' 600
  Note ("idle soak: {0} min with telemetry off ..." -f $IdleMinutes)
  Start-Sleep -Seconds ($IdleMinutes * 60)
  $soak = Get-LogTail 400
  $bad  = $soak | Where-Object { $_ -match '(?i)trap|address error|stack error|math error|oscillator fail|hard fault' }
  if ($bad) { Add-Result 'T2.5b' 'FAIL' (($bad | Select-Object -First 5) -join ' | ') }
  else      { Add-Result 'T2.5b' 'PASS' ("no trap/fault text in {0} min idle" -f $IdleMinutes) }
} else {
  Add-Result 'T2.5b' 'NOTRUN' 'idle soak skipped (-IdleMinutes 0)'
}

# ---- T2.8 software reset ---------------------------------------------------------------
if ($Reset) {
  $null  = Send-Verb '*tq0000' 600
  $rs    = (Send-Verb '*sr' 2000 40) -join ' / '
  Start-Sleep -Seconds 10
  $after = Get-LogTail 500
  $gv2 = (Send-Verb '?gv') -join ' / '
  if ($gv2 -match 'SONORA') { Add-Result 'T2.8a' 'PASS' ("*sr -> " + $rs + " ; console back") }
  else                      { Add-Result 'T2.8a' 'FAIL' ("*sr -> " + $rs + " ; no ?gv after reset") }

  # The console answering is not the whole rule: the reboot must also bring the audio path
  # up. A codec that fails to apply tears the transport down mute-held, and ?gv answers
  # perfectly well while it does -- so the boot log itself has to be read.
  $sick = $after | Where-Object {
    $_ -match 'apply=FAILED' -or $_ -match 'apply failed' -or $_ -match 'unmatch!!' -or
    $_ -match 'mute-held teardown' -or $_ -match '(?i)\btrap\b' }
  if ($sick) { Add-Result 'T2.8b' 'FAIL' (($sick | ForEach-Object { ($_ -replace '^\d\d:\d\d:\d\d\.\d\d\d\s+<<\s+\[[^\]]*\]\s*','').Trim() }) -join ' ; ') }
  else       { Add-Result 'T2.8b' 'PASS' 'no codec-apply failure, teardown or trap in the post-reset boot log' }

  # Final state after any internal retry: is the transport actually streaming?
  $st1 = $after | Where-Object { $_ -match 'STREAM epoch' } | Select-Object -Last 1
  $tdm = $after | Where-Object { $_ -match 'TDM1:(?:max|resp)' }     | Select-Object -Last 1
  if ($st1 -match 'qualified=1' -and $st1 -match 'mute_held=0' -and $tdm -match 'miss,?\)?=\(\d+,\d+,\d+,0\)|,0\)$') {
    Add-Result 'T2.8c' 'PASS' (($st1 -replace '^.*STREAM','STREAM') + ' | ' + ($tdm -replace '^.*TDM1','TDM1'))
  } else {
    Add-Result 'T2.8c' 'FAIL' ("final transport state: " + ($st1 -replace '^.*STREAM','STREAM') + ' | ' + ($tdm -replace '^.*TDM1','TDM1'))
  }
  Note '--- post-reset boot log (for T2.1/T2.8 banner completeness) ---'
  foreach ($l in $after) { Note $l }
  Note '--- end post-reset boot log ---'
} else {
  Add-Result 'T2.8' 'NOTRUN' 'software reset skipped (-Reset not given)'
}

Add-Result 'T2.1' 'NOTRUN' 'power-cycle boot needs a human at the board'

# ---- summary ---------------------------------------------------------------------------
Note ''
Note '=== T2 summary ==='
foreach ($r in $script:results) { Note ("{0,-6} {1,-6} {2}" -f $r.id, $r.verdict, $r.note) }
$fails = ($script:results | Where-Object { $_.verdict -eq 'FAIL' }).Count
Note ("FAIL={0}  NOTRUN={1}" -f $fails, ($script:results | Where-Object { $_.verdict -eq 'NOTRUN' }).Count)
Note 'telemetry left OFF -- re-enable with *tq0001'

if ($Transcript) { $script:log | Set-Content -Path $Transcript -Encoding utf8 }
exit ([int]($fails -gt 0))
