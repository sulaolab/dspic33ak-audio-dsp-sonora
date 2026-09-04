# Full Test — definition, procedure and acceptance criteria

This procedure defines the checks that establish whether a Sonora build works on
the supported hardware. It separates host checks, builds, board bring-up,
serial update, application function, measurements, and listening.

## 1. When a full run is required

| trigger | scope |
|---|---|
| End-to-end verification of a build or board setup | Full run, T0 – T6. |
| A focused source or configuration change | Run the applicable tiers in addition to the normal build checks. |

A run that skips a tier is not a full run. Report it as the subset it was.

## 2. Test tiers

| tier | what it establishes | hardware | measured cost |
|---|---|---|---|
| **T0** Host gates | The tree is internally consistent and the host-side models still hold | none | ~15 s |
| **T1** Build matrix | Every shipped selection still builds, links and fits | none | 17 min 30 s (7 selections, measured) |
| **T2** Platform bring-up | The board boots, clocks, talks, and keeps time | board | minutes |
| **T3** Resident downloader | An application can be replaced over the serial link, in both directions, by both a script and a human terminal | board | ~15 min |
| **T4** Application function | Every console verb behaves as documented, in both applications | board | ~1 h |
| **T5** Quality and measurement | Audio quality, rate coverage, CPU load and lock behaviour are no worse than the recorded baseline | board + audio analyser | hours |
| **T6** Listening | A human accepts the sound | board + listener | human |

The ordering is a dependency order, not a preference: T1 needs T0's answer to be
worth having, T2 needs an image from T1, and T5 measures the image T4 proved
functional.

## 3. Fixtures — pin these before starting

A result without these is not reproducible and must not be recorded as a pass.

| fixture | how to capture it |
|---|---|
| Commit | `git rev-parse HEAD`, plus `git status --short` (must be empty) |
| Selection triple | `buildtools/switch_config.ps1 -List` — serial-update support, device, application profile. The MPLAB X configuration is *derived* from the last two; do not record it as an independent choice |
| Toolchain | XC-DSC version, MPLAB X version, device family pack (DFP) version. All three appear in the build log |
| Board topology | Which codec drives BCLK/FS, sample rate, and the clock-source jumper position. The profile description printed by `-List` states the intended topology; the board must match it |
| Console channel | Which port carries the debug console and which carries UART1. Do not record numbers as facts — USB serial numbering is assigned by the host and moves between sessions |
| Running image | `?gv` on the device (protocol tag + selected-app build role + git commit). This is the only acceptable evidence of what is executing; "I just flashed it" is not |

If a step changes the clock configuration, the board needs a **power cycle**, not
a reset, before its result counts.

## 4. T0 — host gates

No board. Run from the repository root. Every one of these is a gate: a failure
stops the run.

| # | gate | command | pass rule | measured |
|---|---|---|---|---|
| T0.1 | Configuration integrity | `pwsh ./buildtools/check_configurations.ps1` | prints `Configuration gate: PASS` and lists every configuration with its delivery mode | PASS, 1.7 s |
| T0.2 | Serial-boot package model | `python tools/test_serial_boot_package.py` | exit 0; ends `ALL PASS: resident serial-boot package/factory/fault tests` | PASS, 0.7 s |
| T0.3 | Fixed 48→8 kHz decimator model | `python tools/asrc/test_asrc_decimator_48_to_8.py` | exit 0; ends `PASS: fixed-decimator host acceptance` | PASS, 1.7 s |
| T0.4 | ASRC rate-plan equation | `python tools/asrc/test_asrc_rate_plan.py` | exit 0; ends `PASS: general ASRC rate-plan equation` | PASS, 0.7 s |
| T0.5 | Hot-path invariance | `python tools/asrc/hotpath_invariance.py --elf <elf> --map <map> --compare tools/asrc/hotpath_baseline_<...>.json` | every watched function's instruction sequence, and program and data size, unchanged against the selected baseline | runs after T1 (needs an ELF) when the selected profile has a baseline |

**T0.2 – T0.4 are `__main__` scripts, not pytest tests.** They define no `test_`
functions, so `python -m pytest tools/...` collects nothing and reports
`no tests ran` (exit code 5, measured). Nothing was executed, and a runner that
treats "not 1" as success, or a human reading a quiet `-q` line, records a pass
for a suite that never ran. Invoke each file with `python` and read *its* exit
code.

Two scripts under `tools/` are not T0 host gates:

- `tools/test_dual_partition_hex.py` is not a verifier and is expected to fail
  when run. Do not include it in a host-gate result.
- `tools/classic/test_csv_biquad_smoke.py` needs a running Classic image and
  hardware; treat it as a T4 check rather than a host test.

## 5. T0 evidence

One line per gate: command, exit code, the summary line it printed, elapsed
time. Nothing else — T0 logs are short by construction, and a full transcript
hides the one line that matters.

## 6. T1 — build matrix

The authority for what exists is `buildtools/switch_config.ps1 -List`; the
authority for how to build is `buildtools/build.ps1`. There is no
`-Configuration` switch to drive a matrix with: **selection first, then build.**

```
pwsh ./buildtools/switch_config.ps1 -SerialUpdateSupport Yes \
     -Device <device> -Profile "<profile>"
pwsh ./buildtools/build.ps1 -Full
```

`switch_config.ps1` with no arguments opens an interactive menu and throws when
stdin is redirected, so a scripted matrix must pass the three values
explicitly. `-List -All` (not `-All` alone) shows the hidden tiers.

**Serial update support is no longer one of the three questions.** As of
2026-08-15 every shipped configuration on both devices is serial-update-only —
`-SerialUpdateSupport No` is rejected on both — so `switch_config.ps1` derives
it instead of asking; `-SerialUpdateSupport Yes` still scripts it explicitly.
See `buildtools/README.md` for the history.

**Weekly scope — the normal tier**, which is what `-List` shows without
`-Advanced`/`-All`. The last four columns are a measured reference run (all
seven PASS, `-Full` each, 17 min 30 s total on an 8-job build host); they are
what a later run compares against, not a limit written into the build:

| # | device | profile | resolved configuration | s | ROM | RAM |
|---|---|---|---|---|---|---|
| 1 | dsPIC33AK512MPS512 | Classic 1 | `dsPIC33AK512_CLASSIC_SERIAL_UPDATE` | 169 | 337,520 (69 %) | 37,788 (57 %) |
| 2 | dsPIC33AK512MPS512 | Classic 2 | `dsPIC33AK512_CLASSIC_SERIAL_UPDATE` | 149 | 337,464 (69 %) | 37,788 (57 %) |
| 3 | dsPIC33AK512MPS512 | Classic DRC | `dsPIC33AK512_CLASSIC_SERIAL_UPDATE` | 125 | 173,292 (35 %) | **65,430 (99 %)** |
| 4 | dsPIC33AK512MPS512 | Classic 96k | `dsPIC33AK512_CLASSIC_SERIAL_UPDATE` | 138 | 327,972 (67 %) | 34,630 (52 %) |
| 5 | dsPIC33AK512MPS512 | ASRC Codec BI | `dsPIC33AK512_ASRC_SERIAL_UPDATE` | 164 | 186,932 (38 %) | 50,024 (76 %) |
| 6 | dsPIC33AK512MPS512 | ASRC dsPIC BI | `dsPIC33AK512_ASRC_SERIAL_UPDATE` | 149 | 153,228 (31 %) | 48,110 (73 %) |
| 7 | dsPIC33AK128MC106 | Classic 1 | `dsPIC33AK128_SERIAL_UPDATE` | 156 | **98,016 (99 %)** | 12,162 (74 %) |

(Measured 2026-08-16, superseding the pre-2026-08-15 nine-selection run this
table used to show — that run included two standalone configurations per
device and a duplicate serial-update pair that no longer exist. Row 7 predates
the AK128 boot region's 2026-08-20 move from 28 KiB to 16 KiB, which raised the
AK128 application's own capacity from 98,304 B to 110,592 B — the ROM figure
and percentage on that row are stale and are due for a fresh measurement on
the next full run.)

One row carries a standing risk worth watching between runs rather than only
at failure: **Classic DRC sits at 99 % of data memory** on the AK512, which is
unaffected by the AK128 boot region change above. AK128's own headroom is
inherently tighter than the AK512's — its flash is a fraction of the size —
but its current margin is not stated here pending that fresh measurement. A
change that adds a buffer or a table can turn a tight row into a link failure
with no other warning.

Each AK512 row's resident bootloader built to `0x62E0 / 0x8000` (25,312 B,
77.2 %). Verify the AK128 resident size against its 16 KiB region for the
selected build. Every row produced a verified factory image plus one `.sfb`
package.

**Reading a build's log:** it contains **two** size blocks, and the *last* one
is the resident bootloader (25,312 bytes at 77 % of its own 0x8000 region on
the AK512; 0x4000 on the AK128), not the application. Take the application's
figures from the first block. A script that greps for the last match silently
reports the bootloader for every configuration — which looks like a dramatic
size improvement.

**Release scope** additionally covers the advanced and internal profiles — 21
application profiles exist for dsPIC33AK512MPS512 alone (USB variants,
per-direction ASRC legs, measurement and headroom profiles), plus AK128's own
smaller set. A representative run that covers 6 of the AK512's 21 (plus one
AK128 profile as a device sanity check) must say so; silence reads as "all of
them".

Pass rules, per selection:

1. `build.ps1 -Full` exits 0 with no compiler warning in project sources.
2. The resident bootloader fits its region (the build prints
   `Resident bootloader: 0x…/0x<region> bytes` — 0x8000 on the MPS512, 0x4000 on
   the MC106), and program and data memory fit the device.
3. A `FACTORY_IMAGE` is produced and verified (`Verify FACTORY_IMAGE` in the
   log), plus a `.sfb` under `artifacts/serial_update_packages/`.
4. When a baseline JSON exists for the selected profile, run T0.5 against that
   profile's ELF and require no change.

Record per selection: status, elapsed seconds, resolved configuration, program
and data bytes, and the `.sfb` name. Build products are timestamped and kept;
`-Clean` and `-Full` never delete the packages.

## 7. T2 — platform bring-up

Board required. Talk to the console only through the serial-monitor bridge
(never open the port directly); confirm the bridge reports the profile and port
you intend before sending anything.

| # | check | pass rule |
|---|---|---|
| T2.1 | Power-cycle boot | The board really did cold-start (`?sr` reports `POR(power-on)` as the most specific bit, and the transport's block counter restarts from zero), the boot it performed is healthy, and **each banner printed is complete and identical, with no truncation**. Catching a banner at all needs **board button 1 held at power-on** — see below |
| T2.2 | Running image | `?gv` matches the commit and application profile under test |
| T2.3 | Clock | Reported system/instruction clock equals the profile's intent; no clock-failure diagnostic |
| T2.4 | Board identity | Board-ID / UDID readback answers and is stable across resets |
| T2.5 | Traps | No trap report after boot, and none during 5 minutes idle |
| T2.6 | Console liveness | Liveness (`?gh` hello) answers; an unknown verb returns the documented not-found status rather than silence |
| T2.7 | 1 ms tick | The telemetry print cadence, commanded with `*tq0002<period>`, matches that period within 1 % over ≥ 10 intervals |
| T2.8 | Reset path | Software reset verb (`*sr`) returns the board to T2.1 state |

T2.7 exists because the tick is the unit every cadence in the tree is expressed
in: the HAL refuses an input clock it cannot divide to exactly 1.000 ms
(`NORA_TICK_TIMER_ERR_INEXACT_PERIOD`), so a wrong tick shows up as a refused
`nora_tick_timer_init()` at bring-up rather than as drift — but only if someone
looks at the boot status.

**There is no console verb that reads the millisecond counter** (checked across the
general, system, diagnostics, transport and Classic console modules).
The measurable substitutes, and what each one actually proves:

- **the ms tick** — the periodic-monitor cadence is driven by `GetTicks()`, so
  commanding a period with `*tq0002<period-hex>` and measuring the arrival
  interval of the telemetry lines measures the tick. This is T2.7's rule above.
- **the audio clock** (T2.3) — the TDM telemetry's `blk` counter advances once per
  audio block, so its rate is `sample-rate / block-size` (1500 blk/s for the
  nominal 48 kHz, 32-sample profile). This is a different clock from the tick and
  a pass on one is not a pass on the other.

Both must be timed from the **bridge's own timestamps on the telemetry lines**,
not from host wall-clock reads around a sleep: a line can be up to one period old
when the log is read, which on a 3 s period is a ~20 % error over a 15 s window —
measured, and it read as a 6.6 % clock error on hardware that was in fact exact
to 0.003 %. Cut the window at the bridge's echo of the period command as well, or
the log tail reaches back into the previous cadence and the mean is meaningless.

**A power cycle takes the console away with the board, so the banner needs the
boot-button hold.** The USB CDC device is part of what loses power, and the host
needs several seconds to re-enumerate it — measured at ~7 s of port absence, by
which time the early banner has already been printed; the first line the bridge
captured on a plain power cycle was mid-boot. The firmware already answers this:
holding **board button 1 at power-on** puts it in banner-hold mode and re-prints
the full banner once a second for 5 s (`APP_BOOT_BANNER_HOLD_*` in
`app_specific_config_defs.h`, enabled by default; LED 0 blinks while it is in
that mode). The hold is armed only on a power-on-class reset, so a warm reset is
never delayed, and the button is sampled once before the runtime button controls
start.

**So T2.1's procedure is: hold button 1 while switching the board on.** Without
the hold there is nothing to see — that is the fast-boot path, not a fault. Note
that "the banner appears once" describes that fast-boot path; in banner-hold mode
the repetition is the mechanism, and what is judged is that each printed banner is
complete and identical to the others.

Measured with the hold: the bridge caught repeats **4/5 and 5/5**, each a complete
15-line banner, byte-identical. What is evidenced on a cold start either way:

- **that it was a cold start**: `?sr` reports the latched cause, and the most
  specific `RCON` bit separates the two paths — `POR(power-on)` for a real power
  cycle (`RCON=0x00740003` measured) against `EXTR(MCLR)` for the programmer's
  reset (`RCON=0x00740081`). The **banner's own `Reset:` line says "POR" for
  both**, so cite `?sr`, not the banner.
- **that the board restarted at all**: the transport's `blk` counter restarts
  from zero (2,140,413 → 1,217 across the event, measured). A port that dropped
  and returned is not by itself proof the target rebooted.
- **that the boot was healthy**: `wm8904_init_role(...): apply=verified`, and
  `STREAM ... qualified=1 ... mute_held=0` with `miss=0`.

`buildtools/run_t2.ps1` implements this tier, including both measurements.

Read the boot log from the bridge's **log file on disk**, not `GET /log?tail=N`,
when the question is what happened at a boot: the tail is a bounded live window
and was measured returning an older span than the newest traffic, which reads as
"no banner, board never rebooted" when the opposite is true.

## 8. T3 — resident downloader (serial update)

Every T1 row is a delivery-mode image now, so any of them qualifies. The point of this tier
is that the *application can be exchanged without a programmer*, so it must be
exercised in both directions and through both client kinds.

| # | case | pass rule |
|---|---|---|
| T3.1 | Classic → ASRC, scripted | Transfer completes; after power cycle `?gv` reports the ASRC build; ASRC console answers |
| T3.2 | ASRC → Classic, scripted | Mirror of T3.1 |
| T3.3 | Same-application update | A package of the same application installs and the version stamp advances |
| T3.4 | **Tera Term (human terminal)** | Same transfer as T3.1 succeeds from the terminal. Transfer type **XMODEM 1K**, and the terminal's *Service* must be set to **Other**: a bare CR arrives as `CR NUL` and corrupts an XMODEM stream. A failure here is a terminal-configuration failure, not a board fault — do not open a firmware investigation before re-checking this setting |
| T3.5 | Aborted transfer | Cancel mid-transfer; the board stays on its current application and the console recovers |
| T3.6 | Corrupt package | A truncated or bit-flipped `.sfb` is rejected with a status, not accepted |
| T3.7 | Writer exclusion | While a transfer holds the transmit gate, another writer is refused (the bridge answers HTTP 409) and nothing is injected into the stream |

Evidence: the `.sfb` name, transfer duration, and the `?gv` line **after a power
cycle** for each direction. A `?gv` taken without the power cycle proves nothing
about what will run next time.

## 9. T4 — application function tests

Board required. **One pass per PROFILE, not per application** — see the warning
below; "Classic" and "ASRC" are too coarse a unit for this tier.

**Derive the checklist from the image under test, not from memory.** A list
pasted into a document silently goes stale as verbs are added, so this document
does not carry one.

**A profile decides the verb surface, so a T4 pass covers one profile only.**
The module letters are common, but the verbs and the hotkeys inside them are
`#if`-gated on the `ENA_*` set that `src/app/app_specific_config_defs.h` defines for
that profile, and Classic's two big profiles do not intersect the way the names
suggest:

- `ENA_BIQUAD_IIR_CASCADE` and `ENA_SAMPLE_DELAY` are defined **only** in the
  `ENA_DRC_DF2T_CASCADE` branch — i.e. **Classic DRC**. On **Classic 1** there is
  no `*cf`, no biquad coefficient CSV upload, and no `*nd50`. Confirmed on
  hardware 2026-08-12: `*cf00` answered `$01` (NOT_FOUND) on a Classic 1 image.
- The reverse holds for the "Regular mode" branch: `ENA_ENGINE_SYNTH`,
  `ENA_BASS_ENHANCER`, `ENA_KINKON`, `ENA_CLICK_CLACK`, both AVAS engines and
  `ENA_WIDEN_CTRL` are Classic-1-side and absent under DRC.
- **The hotkey map changes with it, silently.** `case 'b'` appears twice in
  `sonora_app_handle_hotkey()`, once under `ENA_BIQUAD_IIR_CASCADE` and once
  under `ENA_ENGINE_SYNTH`. Those branches are mutually exclusive so it compiles,
  but `b` means "activate IIR block process" on Classic DRC and "engine blip" on
  Classic 1. Re-derive the hotkeys per profile; do not carry a table across.

So the T4.x rows below are **not all reachable from one image**, and a run record
must name the profile it was taken on. The rows are tagged accordingly.

**`?gh` is not a verb listing** — it prints ` SONORA console hello` and nothing
else (`general_console.c`, case `'h'`), and no other verb enumerates the console
either. So the intended "ask the device" step is not available today, and the
checklist has to be derived from the source of the image under test: the module
switch in `app_debug.c` (`dbcapp_*_onmsg`) gives the module letters, and the
`case` labels in each module's console file give that module's verbs. Deriving it
from source is still image-specific — the modules are conditionally compiled — but
it is not self-checking the way a device capture would be. Adding a listing verb
would close this (§14.7).

**The console modules are not all under `src/app/uart_app/`** — half of them are not,
and looking only there yields an incomplete checklist. The full map, per module
letter, as `app_onmsg()` in `app_debug.c` dispatches them:

| module | file | notes |
|---|---|---|
| `g` | `src/app/uart_app/general_console.c` | `?gv` `?gh` |
| `s` | `src/app/uart_app/system_console.c` | `*sr` `?si` `?sr` |
| `d` | `src/app/uart_app/diag_console.c` | `?dr<II>`, `II` = codec instance (2 and 3 on this board) |
| `n` | `src/app/uart_app/app_debug.c` | legacy `*nt` / `*nd`; most subcodes have been moved out and the survivors are gated |
| `t` | `src/app/audio_transport/audio_transport_console.c` | `*tr` `*td<NN>` **`?td`** `*tq…` `*tf`. `?td` exists and is easy to miss — it is a `kind == '?'` branch inside `case 'd'`, not its own case label |
| `f` | `src/app/resident_de/app/resident_de_app_console.c` | `?fu` `*fu5A` `?fm` `*fmA4` `*fmA5` `*feaa55`. Serial-update images only |
| `c` | `src/app/apps/classic/classic_console.c` | Classic's own module, reached through `app_onmsg()`'s `default:` → `sonora_app_console_onmsg()`. **Grepping `app_debug.c` for `case 'c'` finds nothing** |
| `a` | `src/app/apps/asrc/asrc_console.c` | ASRC's own module, same `default:` route |

`src/app/uart_app/app_biquad_coeff_csv.c` is the CSV marker-line handler rather than a
verb module, and `app_console.c` is the parser and status encoder (`$` + hex status
+ module + name) — read it to know what a status byte means before calling a verb
failed.

The single-key hotkeys are a **second surface** that no module letter covers:
`sonora_app_handle_hotkey()` in the application's console file. They must be swept
too, and they are `#if`-gated the same way the verbs are.

Per verb, the pass rule is the same three-part rule:

1. the documented response appears, in the documented format;
2. no trap and no error counter increments;
3. the millisecond tick is still advancing afterwards (a verb that stops the
   tick has broken every cadence in the tree, and reports nothing itself).

Beyond per-verb coverage, these cases must appear explicitly:

| # | profile | case | pass rule |
|---|---|---|---|
| T4.1 | any Classic | Classic audio path | Signal passes A→B with the profile's topology; no mute, no clipping at nominal level |
| T4.2 | **Classic DRC** | Classic DRC profile | DRC cascade active; changing a parameter changes the measured response in the expected direction |
| T4.3 | **Classic DRC** | **Biquad coefficient CSV bulk upload** | Needs `ENA_BIQUAD_IIR_CASCADE`, so it is a DRC-profile row — on Classic 1 the verb does not exist and a "failure" there means the wrong image, not a regression. Send `tools/classic/fixtures/` CSV over **UART1** with `python tools/classic/test_csv_biquad_smoke.py --port <uart1-port>`; the marker `BIQUAD COEFF CSV APPLY OK` appears. The criterion is **at least one success in N tries**: an occasional `ring_ovf > 0` is a known UART1 RX ring overflow that aborts the CSV gracefully, so per-run flake is reported but is not the regression signature — never succeeding is |
| T4.4 | **Classic DRC** | Coefficient readback | Coefficients read back after upload match what was sent. The Classic-1 analogue, if a write+readback check is wanted on that profile, is `*cb VV` → `?cs` (bass-enhancer LPF cap) |
| T4.5 | ASRC | ASRC direction and rate switching | Each supported direction and rate pair engages, locks, and reports its rate; no click at the switch |
| T4.6 | any | Telemetry and level meter | `*tq` OFF / ON / explicit period all take effect, and the meter tracks an applied input |
| T4.7 | any | Mute / restart | Mute and application restart are click-free |
| T4.8 | any | Negative cases | Out-of-range and malformed arguments are refused with a status; nothing hangs |

**T4.3 owns UART1, so the monitor must move off it first.** The CSV transport is
UART1-only (`app_debug.c` rejects a CSV BEGIN arriving on UART2 by policy), and
UART1 is the port the serial-monitor normally holds. Point the monitor at the
PKOB4 "USB Serial Device" for this row and hand UART1 to the python script. The
inverse is also true and is the trap that cost two days: **the resident
bootloader only ever talks on UART1**, so T3 needs the monitor moved back. The
two cannot be run from one monitor placement.

## 10. T5 — quality and measurement

Board plus an audio analyser. This is the expensive tier; it is also the one
whose numbers are meaningless without §3's fixtures, because a rate, a jumper
and a codec role each move the result.

| # | measurement | method | pass rule |
|---|---|---|---|
| T5.1 | THD+N and dynamic range, ASRC | Procedure A of the ASRC measurement guide: tone in, capture, analyse | Not worse than the recorded baseline for that rate pair by more than the baseline's stated tolerance |
| T5.2 | 48/8 endpoint test | Procedure B, with B at 8 kHz | Baseline conformance; low-rate aliasing and click behaviour unchanged |
| T5.3 | Rate coverage sweep | `tools/asrc/rate_regression_sweep.py`, `smoke_leg_a_rates.sh`, `smoke_lowrate_b.sh`, `smoke_96k_roundtrip.sh` | Every rate pair locks and reports the expected rate |
| T5.4 | CPU load | Hot-path/load telemetry against the recorded baseline for the same profile | Within the baseline's margin; a load regression is a failure even when audio sounds fine |
| T5.5 | Servo re-lock | Force a re-acquisition | Lock time within the recorded envelope, no audible artefact |
| T5.6 | Click-free operation | Rate change, direction change, mute, restart | No click at any transition |

Two traps that have produced wrong conclusions before, both documented in the
internal measurement guide and repeated here because they invalidate results
silently:

- **The reported sample rate can itself be biased** by which clock the capture
  time base is derived from. A rate that reads correct is not proof the time
  base is correct.
- **Direct low-rate captures are capture-sensitive**: the measurement setup, not
  the firmware, can dominate the result. Compare like with like, and prefer the
  procedure the baseline was taken with.

Keep lab baselines and raw captures with the test record. A consumer running T5
supplies and states their own baseline.

## 11. T6 — listening test

The acceptance criterion is a **full listen-through**, not a residual or a
difference measurement. A signal-domain result that looks clean has repeatedly
coexisted with audible defects, so T5 passing does not stand in for T6.

- Both applications, and for AVAS content both voices, played through
  completely.
- The listener records: material, path, level, and a plain verdict.
- A named human signs off. This tier cannot be automated and must not be
  reported as passed by anyone who did not listen.

## 12. Recording a run

Record the selected configuration, toolchain versions, board topology, commands,
and each tier's PASS, FAIL, or not-run result. Keep full logs and raw captures
with the test record rather than in the source tree. Preserve each tool's own
verdict line so the result can be checked later.

## 13. Subsets by change class

Not every change needs a full run. These are practical minimum subsets; report a
subset as such rather than calling it a full run.

| change | minimum |
|---|---|
| Documentation only | T0 |
| Host tool / model | T0 (its own gate must be among them) |
| Application logic, one application | T0, T1 rows for that application, T2, T4 for that application |
| DSP algorithm, gain or filter | the above plus T5.1/T5.4/T5.6 and T6 |
| HAL, clock, or transport | T0, full T1, T2, T3, T4 both applications, T5.3–T5.5. Wide blast radius: this is the class that most often looks local and is not |
| Bootloader, serial update, memory layout | T0, full T1 (every row is a delivery-mode image now), T2, full T3 |

## 14. Known limitations

- T3 – T6 require hardware and are performed by hand.
- T2.1 requires a real power cycle; a host reset is not equivalent.
- The console `?` command prints a module and grammar legend, not an exhaustive
  list of individual commands.
- T2.3 and T2.7 are inferred from telemetry counters and cadence rather than a
  dedicated readout.
