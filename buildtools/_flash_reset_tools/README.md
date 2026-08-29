# PKOB4 Flash / Reset Tools

A set of tools for quickly flashing and resetting a dsPIC33AK512MPS512 Curiosity
board from the **command line**, without opening MPLAB X IDE. It **selects the
board by PKOB4 serial number**, so you can operate on exactly one target even
with several boards connected at once. Well suited to driving a
build→flash→reset→observe loop from a script or an AI agent.

The three exe's in this folder are used for different purposes (flash and
reset as a pair, or flash triggering reset automatically):

| Tool | Role |
| --- | --- |
| `flash_pkob4.exe` | **Flash** (programs a HEX). `--reset-after-flash` can auto-reset after a successful flash. |
| `reset_pkob4.exe` | **Reset only** (release-from-reset). Does not flash. |
| `read_udid_pkob4.exe` | **UDID read only** (displays the die's 128-bit Unique Device ID). Does not flash or reset, but the CDC console drops briefly on connect. |

All three are self-contained exe's with no .NET dependency (Windows x64, about
35 MB each).
The binaries ship with this repository, so a fresh clone can use them as-is.
MPLAB X must be installed: the tools auto-detect the newest installation and use
its bundled `mdb`, `ipecmdboost` and Java runtime.

The source is maintained separately, in
<https://github.com/sulaolab/pkob4-flash-reset> (MIT-0). It is not needed to
build the firmware. To rebuild from source, run `dotnet publish -c Release` in
that repository under `flash_pkob4/`, `reset_pkob4/` or `read_udid_pkob4/`, then
copy the published executables into this directory.

---

## Board serial numbers

| Board | Example PKOB4 serial |
| --- | --- |
| Board A | `<PKOB4_SERIAL_A>` |
| Board B | `<PKOB4_SERIAL_B>` |

> Operate only on your own connected board. Do not run this against a board
> someone else is using.
> (Don't use an index like `hwtool ... -p <index>` -- it depends on connection
> order. **Always specify by serial**.)

### When you don't know the serial: `--list`

Either exe can list the connected PKOB4 serials with `--list` (USB enumeration
only -- **instant and harmless**):

```powershell
& "$dir\reset_pkob4.exe" --list      # flash_pkob4.exe --list also works
# Connected PKOB4 serial(s): 1
#   <PKOB4_SERIAL>
```

`reset_pkob4 --list --probe` additionally shows each board's **device name +
Device Id** (it connects to each board to do so, so **that board is reset**
and its CDC console drops briefly).
The default probe timeout is 60 s (allowing for a cold connect; it returns
immediately on success, so the ceiling is effectively free).

```powershell
& "$dir\reset_pkob4.exe" --list --probe
# Connected PKOB4: 1  (probing with device token '33AK512MPS512' -- this resets each board)
#   <PKOB4_SERIAL>   dsPIC33AK512MPS512   Device Id 0xa77c
```

(As before, you can also confirm this from the `INFO: <serial> successfully
reserved` line in a flash log, or from MPLAB X's Tool list.)

---

## Usage

From PowerShell. When running from the repository root, set it up like this:

```powershell
$dir = Join-Path (Get-Location) 'buildtools\_flash_reset_tools'
```

### Flash

```powershell
$hex = 'C:\path\to\your.production.hex'

# Flash Board C
& "$dir\flash_pkob4.exe" --serial <PKOB4_SERIAL> --hex $hex --verbose

# Flash, then auto-reset on success
& "$dir\flash_pkob4.exe" --serial <PKOB4_SERIAL> --hex $hex --reset-after-flash --verbose
```

Main options:

```text
--list            List the connected PKOB4 serials and exit (instant, harmless)
--serial  <sn>    PKOB4 serial (required)
--hex     <path>  HEX file to program (required)
--device  <dev>   Device name (default dsPIC33AK512MPS512 -- mdb wants the "long form")
--reset-after-flash
                  After a successful flash, run reset_pkob4.exe with the same serial
--reset-device <dev>
                  Short device name for reset_pkob4 (normally derived automatically from --device)
--timeout <sec>   Timeout per attempt, in seconds (default 120)
--retry   <n>     Retry count on failure (default 0)
--verbose         Show the detected path, mdb script, output and exit code
--dry-run         Show what would run, without running it
-h, --help        Usage
```

Internally this passes the following script to mdb (the `<sn>` prefix selects
the serial):

```text
device dsPIC33AK512MPS512
hwtool pkob4 -p <sn><PKOB4_SERIAL>
program "C:\path\to\your.production.hex"
quit
```

`--reset-after-flash` only runs after a successful flash. If the flash fails,
it deliberately does not reset, for safety. The device name for reset is
auto-converted, e.g. `dsPIC33AK512MPS512` → `33AK512MPS512`.

### Reset

```powershell
& "$dir\reset_pkob4.exe" --serial <PKOB4_SERIAL> --verbose
```

Main options:

```text
--list            List the connected PKOB4 serials and exit (instant, harmless)
--probe           Combined with --list: also connect to each board and show its
                  device name + Device Id (this resets each board; use it to check --device)
--check-java      Instantly report IPECMDBoost's warm/cold/stale state (harmless, does not
                  touch the target). Shows port 2012's owning PID and lock/ini.
                  exit 0 = usable, exit 8 = stale/problem
--shutdown-boost  Ask IPECMDBoost to exit cleanly via /OQ /OY2012, then exit
--clean-java      Emergency recovery: kill the boost java (including port 2012's owner)
                  and delete its lock/ini
--serial  <sn>    PKOB4 serial (required)
--device  <dev>   Device name (default 33AK512MPS512 -- boost wants the "short form")
--warm-timeout <sec>
                  Timeout if a boost java is already waiting on port 2012 (default 5)
--cold-timeout <sec>
                  Timeout for a boost cold start (default 60; 20+ seconds with no output
                  on the first run is normal)
--timeout <sec>   Compatibility option: use the same timeout for both warm and cold
--retry   <n>     Recovery retry count after a warm failure (default 1)
--verbose         Show the detected path, command and exit code
--dry-run         Show what would run, without running it
```

> **The policy is to keep Boost alive.** A normal `reset_pkob4` leaves
> IPECMDBoost's resident java running, so the next reset takes the fast warm
> path. Recovery -- `/OQ` → kill port 2012's owner java → delete
> `2012.lock`/`.ini` → cold retry, in that order -- only kicks in on a timeout,
> a clear failure, or a non-zero exit.
> `2012.ini`/`2012.lock` can exist even while a warm boost is alive, so they
> are not deleted on that basis alone. They ARE deleted, before a cold start,
> if they are still present while port 2012 is free -- that combination means
> stale.
> Check state by hand with `--check-java`, ask for a clean shutdown with
> `--shutdown-boost`, or force emergency cleanup with `--clean-java`.

> **Mind the device-name form**: boost (reset) wants the **short form
> `33AK512MPS512`**, mdb (flash) wants the **long form
> `dsPIC33AK512MPS512`** -- the two tools disagree, on purpose. Passing reset
> the long form makes `reset_pkob4` suggest the short form and **stop with
> exit 1** (heading off a confusing `PICDSPIC...` failure before it happens).
> Omitting `--device` (the default) is normally fine.

`reset_pkob4` uses IPECMDBoost's release-from-reset, which pulses MCLR.
Because USB re-enumerates, **the CDC console (PRINTF / TeraTerm) drops
briefly and needs reconnecting** -- this is expected.

### UDID read (read_udid_pkob4)

Reads the target dsPIC33AK's **UDID** (a die-unique 128-bit ID). The UDID is
distinct from the **PKOB4 serial** (the debugger's ID) and from **DEVID**
(the part-number ID), and can be used to identify an individual board.

```powershell
& "$dir\read_udid_pkob4.exe" --serial <PKOB4_SERIAL_A>
# UDID1=<UDID1>
# UDID2=<UDID2>
# UDID3=<UDID3>
# UDID4=FFFFFFFF
# UDID128=<UDID128>
# Serial: <PKOB4_SERIAL_A>
```

Main options:

```text
--list            List the connected PKOB4 serials and exit (instant, harmless)
--serial  <sn>    PKOB4 serial (required)
--device  <dev>   Device name (default dsPIC33AK512MPS512 -- mdb wants the "long form")
--timeout <sec>   Timeout per attempt, in seconds (default 60)
--retry   <n>     Retry count on failure (default 1)
--verbose         Show the detected path, mdb script, output and exit code
--dry-run         Show what would run, without running it
```

`UDID128` is UDID4..UDID1 concatenated from the high word down (the same form
the firmware's boot banner prints). Two boards from the same lot often share
UDID1/UDID2 (lot/wafer) and differ only in UDID3 (the die's X/Y position), so
distinguish boards by the full 128 bits, not a prefix.

> **On the read method**: `mdb` **prints `UDIDn = 0x...` automatically on
> connect**, and this tool parses that output. The `x /U4xw 0x7F2BE0`
> approach some documentation describes does not work with the DFP in use
> here (`dsPIC33AK-MP_DFP` 1.3.185) -- it returns FF/00 garbage, because the
> `U` memory space is not mapped for `x` -- so the connect-time output is the
> reliable path. Confirmed on real hardware that the values match the
> firmware's own on-chip read (`0x007F2BE0..EC`) exactly.
>
> Connecting resets the target, so an open CDC console drops briefly and
> needs reconnecting. This fails if MPLAB X IDE / IPE already has the same
> PKOB4 open (close it first).

### Typical flow (flash a built HEX and run it)

```powershell
$dir = Join-Path (Get-Location) 'buildtools\_flash_reset_tools'
$hex = 'C:\path\to\your.production.hex'
& "$dir\flash_pkob4.exe" --serial <PKOB4_SERIAL> --hex $hex --verbose
& "$dir\reset_pkob4.exe"  --serial <PKOB4_SERIAL> --verbose

# Or: auto-reset after a successful flash
& "$dir\flash_pkob4.exe" --serial <PKOB4_SERIAL> --hex $hex --reset-after-flash --verbose
```

---

## Recognizing success

A successful **flash** prints the following in mdb's output:

```text
Target device dsPIC33AK512MPS512 found.
Programming/Verify complete
Program succeeded.
```

> Java may print a `ChronicleHashClosedException` when mdb exits; if
> `Program succeeded.` appeared before that, the flash completed (harmless).

A successful **reset** prints the following. If a warm/cold decision or
recovery was needed, the details appear in `--verbose` output:

```text
PKOB4 Boost reset succeeded.
```

Exit codes:

```text
0  Success
1  Argument error (e.g. wrong device-name form; a suggestion is printed)
2  MPLAB X / Java / IPECMDBoost not found
3  Reset failed
4  Still timed out after retries
5  Unexpected exception
6  PKOB4 is wedged (needs a USB unplug/replug; not recoverable by retrying)
```

`flash_pkob4 --reset-after-flash` returns exit code 6 when the flash itself
succeeded but the following reset failed.

---

## Troubleshooting

- **Fails while MPLAB X IDE / IPE is open**: the IDE holds the PKOB4. Close
  it before running this.
- **Reset times out, or "Wait for current operation to complete"**: usually
  a leftover boost server (the java bundled with MPLAB) or stale
  `2012.ini`/`2012.lock` files.
  **`reset_pkob4` cleans this up automatically** (deleting stale files
  before a cold start, `/OQ` on failure, killing the port's owner java,
  deleting `2012.ini`/`2012.lock`, then a cold retry).
  If it still gets stuck, clean up by hand:
  ```powershell
  Get-CimInstance Win32_Process -Filter "Name='java.exe'" |
    Where-Object { $_.ExecutablePath -like '*MPLABX*' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
  Remove-Item "$env:USERPROFILE\.mchp_ipe\2012.ini" -Force -ErrorAction SilentlyContinue
  ```
- **exit 6 / `unloaded while still busy` / `unplug and reconnect`**: the
  PKOB4's own firmware is wedged; no amount of host-side cleanup recovers
  it. **Unplug and replug the USB cable**, then rerun.
  (`reset_pkob4` detects this and reports exit 6 rather than getting stuck
  retrying.)
- **Operating on the wrong board with more than one connected**: you forgot
  `--serial`. Always pass it; use `--list` to check if you're unsure.

---

## Notes

- The exe's are deployment build output generated from a separate project
  (Windows x64); this firmware repository tracks the binaries as usable
  as-is.
- Where you can rebuild the tools, run `dotnet publish -c Release -r win-x64
  --self-contained true` against the corresponding .NET 8 source project,
  then copy the resulting exe's into this folder.
- A normal firmware build does not need these exe's.
