# Build Tools

`buildtools/` is the complete command-line build and flash toolset. It shares the
same "active selection" as MPLAB X IDE, and a bare `build.ps1` / `flashauto.ps1`
follows that selection. **No environment variables are involved.**

| Script | Role |
| --- | --- |
| `switch_config.ps1` | Choose what to build (interactive menu, or scripted arguments). |
| `build.ps1` | Build (follows the selection when given nothing). |
| `flashauto.ps1` | Flash + reset (selects the board by PKOB4 serial). |
| `resetauto.ps1` | Shorthand for reset only. |
| `build_resident_bootloader.ps1` | Build the resident boot image. Reads `src/boot/boot_image.psd1`; burns in the commit. |
| `generate_resident_project.ps1` | Generate `resident_bootloader.X` (the **debug-only** IDE project) from `src/boot/boot_image.psd1`. |
| `check_resident_project.ps1` | Gate: the generated project still matches the manifest (the resident build runs it every time). |
| `verify_resident_image.ps1` | The resident image's post-link guarantees, in one place. Silent on success, throws on failure. |
| `check_configurations.ps1` | Gate on what each MPLAB configuration builds and excludes (`build.ps1` runs it every time). |
| `check_hal_drift.ps1` | **Report, not a gate** (always exits 0): where `src/boot/hal_*` and `src/app/hal_*` have diverged. |
| `sonora_build_state.ps1` | Selection-state helper shared by the above (do not run directly). |
| `_flash_reset_tools/` | Our own PKOB4 flash / reset / UDID executables, shipped with this repository. Used by `flashauto.ps1` / `resetauto.ps1` and the `-Reset*` paths. See [`_flash_reset_tools/README.md`](./_flash_reset_tools/README.md). |

> **Note.** The flash/reset executables drive an installed MPLAB X (its bundled
> `mdb` / `ipecmdboost` and Java). MPLAB X must be present; a separate .NET
> runtime is not needed. If you would rather not use them, program via MPLAB X
> or MPLAB IPE and the on-board PKOB4 instead.

## Support scope: command line vs MPLAB X

**The command-line build (`buildtools/`) is the only officially supported build
and release route.** Deliverables can only be produced from this side.

| | Command-line build | MPLAB X IDE |
| --- | --- | --- |
| Standing | The only official build and release route | Development and debugging aid |
| Reproducible builds | yes | — |
| Producing SERIAL_UPDATE_APP | yes | — |
| Producing the distributable download image (`.sfb`) | yes | **no (unsupported)** |
| Combining RESIDENT_BOOTLOADER with the App | yes | **no (unsupported)** |
| Header / CRC / size / address checks | yes | — |
| Memory-placement and IVT-placement checks from the MAP | yes | — |
| Release artifacts / future CI | yes | — |
| Opening the project, selecting a configuration | — | yes |
| Clean and Build succeeds | yes | yes |
| Program and run from the debugger | (`flashauto.ps1`) | yes |
| Breakpoints / stepping / watching variables | — | yes |

**What the unsupported row above looks like on the bench.** Build a
`*_SERIAL_UPDATE` configuration in MPLAB X and program it from the IDE and the
board gets the **application only** — the resident bootloader is not rebuilt and
not paired into a FACTORY_IMAGE, so **the serial downloader does not run**:
`*fu5A` never enters the update wait, and the board is back in the application
about 14 ms later with no `BL` line on UART1 (observed 2026-09-02). Nothing is
broken; there is simply no engine in Flash to receive the request. **Build and
flash with buildtools** (`switch_config.ps1` → `build.ps1`, then program
`dist/<conf>/production/*.factory.production.hex` once over PKOB4/PICkit).
Since 2026-09-02 the application detects this state itself: the boot banner says
`Delivery: application ONLY -- resident bootloader ABSENT` and `*fu5A` refuses
instead of muting the transport and resetting.

### The resident boot image: `resident_bootloader.X` is GENERATED, and debug-only

The boot image has its own project, and the same line applies to it more sharply,
because the image is pinned at fixed addresses under a hard 32 KiB cap.

- **`src/boot/boot_image.psd1` is the authority** on what the image is built from: 19
  sources, 8 include directories, macros, flags, the linker script, the defsyms,
  and the cap. Both consumers read it.
- **`resident_bootloader.X` is generated** by `generate_resident_project.ps1`.
  Do not edit `resident_bootloader.X/nbproject/configurations.xml` — edit the
  manifest and regenerate. `check_resident_project.ps1` fails the build if the
  two disagree, and `build_resident_bootloader.ps1` runs it every time.
- **The IDE project exists for debugging only.** Nothing is delivered from it. An
  IDE build still runs `verify_resident_image.ps1` as a post-build step, so a
  one-off image made while debugging is checked like any other.
- **Never let MPLAB X add `..\app` to the include list.** It offers to, whenever a
  header is not found, and accepting makes the build succeed while undoing the
  isolation the boot image depends on. The gate fails on it by name. The correct
  fix is to add the required source to `src/boot/`.

Written by hand instead of generated, that project would have been a second copy
of those lists, in a file the IDE rewrites — and the two would have diverged
without either build failing. The image you debug would not be the image you ship.

A consequence of that line: **the memory layout is owned by the MPLAB
configuration.** Even a plain Clean and Build from the IDE has to link
SERIAL_UPDATE_APP at the right addresses, so linker settings are not injected
from `build.ps1`'s command line (they used to be, and IDE builds were silently
placed wrong). Ownership has two parts:

- `src/linker/p33AK512MPS512_serial_update_app.gld` — the memory range the App may
  use (`0x808000..0x87EFFF`). Registered in the project's Linker Files, and
  excluded in the Standalone configurations.
- The vtable properties of the SERIAL_UPDATE configurations — IVT generation and
  placement (effectively `--ivt=0x808000`).

If those two disagree, the App overwrites RESIDENT_BOOTLOADER. The disagreement
is silent, so `build.ps1` cross-checks the program ORIGIN in the `.gld` against
the configuration's IVT address on every build, and checks the actual placement
in the post-link MAP as well.

---

**Every script produces the same result no matter which directory you run it
from.** Each script's `$Root` defaults to *the repository the script belongs to*
(`$PSScriptRoot/..`), not to the current directory. Invoking `.\build.ps1` from
inside `buildtools/`, or from the root of some other repository, still targets
this tree. An explicit `-Root` wins (and pointing it at the `*.X` project
directory still resolves, by walking up to the parent). By convention the
examples below are written as paths from the repository root.

---

## The three things you choose

```powershell
.\buildtools\switch_config.ps1     # choose
.\buildtools\build.ps1             # build
.\buildtools\flashauto.ps1         # flash
```

There are two choices — and a third only when the selected device has a choice
to make. **The MPLAB configuration is not one of them.**

| # | Choice | Values | Default |
| --- | --- | --- | --- |
| 1 | **Target device** | `dsPIC33AK512MPS512` / `dsPIC33AK128MC106` | the project's first device |
| 2 | **Application profile** | `Classic 1`, `ASRC Codec BI`, … (display names) | that application's compile-time default |
| — | ( **Delivery mode** ) | `No` (standalone, flashed directly over PKOB4) / `Yes` (RESIDENT_BOOTLOADER + serial update) | **asked only if the selected device has configurations for both** |

**Delivery mode used to be question 1.** It stopped being a question on
2026-08-15: AK512 lost its standalone configuration earlier that day and AK128
gained `dsPIC33AK128_SERIAL_UPDATE`, so **every device is delivery-only** and the
answer is now derived from the device (see
[Internal: resolving the MPLAB configuration](#internal-resolving-the-mplab-configuration)).
It is **skipped, not deleted**: the skip is derived from `configurations.xml`, so
restoring any standalone configuration makes the question reappear for that
device by itself, with no device list to edit in the script. `-SerialUpdateSupport`
still scripts it either way, and an unsupported combination passed as a script
argument is **an error, not a silent fallback to the other mode**.

The authority is the three keys in `buildtools/active_build.json`
(**untracked**).

```json
{
  "serial_update_support": true,
  "device": "dsPIC33AK512MPS512",
  "application_profile": "APP_BUILD_STD_DEMO_1"
}
```

### Deliverables

| Serial update support | Deliverables |
| --- | --- |
| `No` | **FACTORY_IMAGE** (standalone application) |
| `Yes` | **FACTORY_IMAGE** (RESIDENT_BOOTLOADER + SERIAL_UPDATE_APP + manifest) and **SERIAL_UPDATE_PACKAGE** (`.sfb`) |

`flashauto.ps1` writes **the FACTORY_IMAGE in either mode**. The operation is
identical; there is nothing to do differently per mode. Details in
[SERIAL_UPDATE_APP deliverables](#serial_update_app-deliverables-the-seed-rom-and-the-download-file).

---

## Internal: resolving the MPLAB configuration

The configuration is **determined uniquely and internally** from the three
choices above. It is not shown to the user (`switch_config.ps1 -Internal` prints
what it resolved to).

| Serial update | Device | Application | → Configuration |
| --- | --- | --- | --- |
| `Yes` | AK512 | Classic | `dsPIC33AK512_CLASSIC_SERIAL_UPDATE` |
| `Yes` | AK512 | ASRC | `dsPIC33AK512_ASRC_SERIAL_UPDATE` |
| `Yes` | AK128 | Classic | `dsPIC33AK128_SERIAL_UPDATE` |
| `No` | AK512 | — | rejected (no standalone configuration since 2026-08-15) |
| `No` | AK128 | — | rejected (no standalone configuration since 2026-08-15) |

**Three configurations, down from five on 2026-08-15, and all three are serial
update.** `dsPIC33AK512` and `dsPIC33AK512_ASRC` — the standalone AK512 pair —
were deleted that morning: serial update is how an AK512 board is both delivered
and developed, so nobody had built them for a long time, and an unbuilt
configuration rots without saying so. `dsPIC33AK128` was replaced by
`dsPIC33AK128_SERIAL_UPDATE` the same day, once the AK128 got a resident
bootloader of its own. They are gone from the MPLAB X configuration dropdown and
from `switch_config.ps1`.

Standalone did not disappear as a *mode*, and the gates still check it: the next
part to arrive arrives without a bootloader. What it no longer has is a
configuration to check it *against*, so `Assert-StandaloneMapLayout` is exercised
by a fixture derived from a delivery map (see
`src/tests/build_layout_gates/run_host_tests.ps1`), and three of the
`check_configurations.ps1` mutations print a SKIP naming the coverage that is
waiting for a standalone configuration to come back. Adding one back means writing
the configuration into `dspic33ak_audio_dsp.X/nbproject/configurations.xml` -- a copy
of a serial-update configuration with the bootloader-specific linker settings and
`SONORA_MPLAB_SERIAL_UPDATE` removed -- plus its entry in
`check_configurations.ps1`.

**This table is not hard-coded in the scripts.** The catalogue's authority is
`dspic33ak_audio_dsp.X/nbproject/configurations.xml`; the scripts search it for
a configuration matching the three attributes below, and stop unless exactly one
matches (both zero and two-or-more are errors).

| Attribute | Read from |
| --- | --- |
| Device | `<targetDevice>` |
| Application ownership | presence of `SONORA_MPLAB_APP_ASRC=1` |
| Serial update | presence of `SONORA_MPLAB_SERIAL_UPDATE=1` |

**There is no ASRC for AK128** (no such configuration exists, so asking for an
ASRC profile on AK128 is rejected explicitly).

`switch_config.ps1` also writes the resolved configuration into the two cache
files that MPLAB X IDE and a bare `make` read (byte-wise, only the one value
concerned, keeping CRLF byte-exact — and **if a file is absent it is simply not
written**, since neither exists in a fresh clone or before MPLAB X has been
started).

| File | Role | Tracked |
| --- | --- | --- |
| `buildtools/active_build.json` | **The authority** (the three keys above). `resolved_configuration` is a record of the derived value and is never read back as authority. | untracked |
| `dspic33ak_audio_dsp.X/nbproject/private/configurations.xml` | `<defaultConf>N</defaultConf>`. Read by MPLAB X IDE. | untracked |
| `dspic33ak_audio_dsp.X/nbproject/Makefile-impl.mk` | `DEFAULTCONF=<configuration>`. Read only when you invoke **bare `make`** in the project directory. | untracked (**untracked since 2026-07-30**) |

> **Why `Makefile-impl.mk` was untracked.** It is generated, and `DEFAULTCONF` is
> a **derived value** that MPLAB rebuilds from `<defaultConf>` in
> `private/configurations.xml` every time it regenerates the makefiles. While it
> was tracked, the local act of "selecting ASRC" became a committable change, and
> it did reach `main` repeatedly, each time undone by a commit that restored the
> Classic default configuration. Untracking it costs
> nothing — the per-configuration makefiles `Makefile-dsPIC33AK*.mk` were always
> untracked, so **a fresh clone has to generate them either way** (`build.ps1`
> does it automatically). The generated file was also verified byte-identical to
> the tracked one. `Makefile-variables.mk` is generated the same way but carries
> no selection state, so it stays tracked.

---

## Application profile internal names and tiers

A profile's display name, internal name (`APP_BUILD_*`), artifact tag and tier
are all owned by the comments on each definition in
[`src/app/apps/app_build_config.h`](../src/app/apps/app_build_config.h).

`APP_BUILD` presets have a **tier**, which decides whether they appear in the
interactive menu and in `-List`. The authority for the tier is the
`/* tier: ... */` comment on each `#define APP_BUILD_*` line in
[`src/app/apps/app_build_config.h`](../src/app/apps/app_build_config.h). **The
PowerShell side holds no list of preset names and no exclusion list** — the
scripts only read those markers.

| Tier | Meaning | Listed with no arguments | Build guarantee |
| --- | --- | --- | --- |
| `normal` | Standard presets for ordinary users. | **shown** | covered by the regular smoke tests |
| `advanced` | For users, but presupposing extra hardware, a measurement setup, or a special purpose. | hidden (`-Advanced`) | kept buildable (supported feature) |
| `internal` | For development, comparison, reproducing measurements, load checks and fault isolation. | hidden (`-All`) | **outside the smoke-test scope** (may temporarily fail to build during development) |

Regardless of tier, **`-Preset <name>` can always name a preset directly**
(naming an `internal` one only prints a short warning; neither selecting nor
building is forbidden). Tiers are unrelated to which application a preset
belongs to, and unrelated to per-device availability (below).

#### `normal` (the presets listed with no arguments)

| App | Variation | Contents |
| --- | --- | --- |
| Classic | `APP_BUILD_STD_DEMO_1` | co-clocked dual codec; WM8904-A drives BCLK/FS, B is slave (the Classic default) |
| Classic | `APP_BUILD_STD_DEMO_2` | co-clocked dual codec; the dsPIC drives BCLK/FS |
| Classic | `APP_BUILD_DRC_DEMO` | co-clocked dual codec; DF2T DRC cascade |
| Classic | `APP_BUILD_DEMO_96K` | non-USB 96 kHz; co-clocked dual codec |
| ASRC | `APP_BUILD_ASRC_CODEC_BIDIR` | WM8904-B codec master; bidirectional A<->B (the ASRC default) |
| ASRC | `APP_BUILD_ASRC_DSPIC_BIDIR` | dsPIC master; bidirectional A<->B |

#### `advanced` (shown with `-Advanced`)

| App | Variation | Prerequisites / contents |
| --- | --- | --- |
| Classic | `APP_BUILD_USB_48` | USB audio input; 48 kHz (needs the USB audio DIM connector) |
| Classic | `APP_BUILD_USB_96` | USB audio input; 96 kHz (same) |
| ASRC | `APP_BUILD_ASRC_CODEC_MEAS` | codec master; one-way digital quality capture (presupposes a measurement setup) |

#### `internal` (shown with `-All`)

The list is not duplicated here, because it turns over as the research moves.
**The full list has exactly two authorities:**

```powershell
.\buildtools\switch_config.ps1 -List -All   # every preset of the active App, with tiers
```

- [`src/app/apps/app_build_config.h`](../src/app/apps/app_build_config.h) — the
  definitions and the tier markers themselves
- `switch_config.ps1 -List -All` — what the tooling actually read

`switch_config.ps1` and `build.ps1` read that header at startup to build the
list, the tiers, the defaults and the App ownership, so **adding a variation to
the header makes it appear in the tooling automatically** (only the Markdown
tables above are maintained by hand).

A preset whose tier marker was forgotten becomes `unclassified` and **appears in
no list at all** (it is never treated as `normal`). Forgetting one produces a
warning, and can be detected with:

```powershell
.\buildtools\switch_config.ps1 -CheckTiers   # lists anything unclassified and exits 1
```

**Per-device availability**: `APP_BUILD_USB_48`, `APP_BUILD_USB_96` and
`APP_BUILD_DEMO_96K` presuppose AK512 board hardware (the USB audio DIM
connector, the second WM8904/SPI2 route), and **AK128 rejects them at compile
time with `#error`** (`src/app/app_specific_config_defs.h`). AK128 Classic supports
`APP_BUILD_STD_DEMO_1`, `APP_BUILD_STD_DEMO_2` and `APP_BUILD_DRC_DEMO`.
`switch_config.ps1` does not look at this device condition (the configuration
and the header `#error` are the final judgement), so choosing a 96 kHz variation
on AK128 stops the build.

The `APP_BUILD` selection is stored per configuration under `presets` in
`buildtools/active_build.json` (**untracked**); `configuration` in the same file
is the active configuration. With nothing selected, the build passes no
`-DAPP_BUILD=` and gets the configuration's own compile-time default
(Classic → `APP_BUILD_STD_DEMO_1`, ASRC → `APP_BUILD_ASRC_CODEC_BIDIR`) — the
same as building directly from MPLAB X IDE.

---

## Choosing: `switch_config.ps1`

With no arguments you get an **interactive menu** (run it from the repository
root):

```powershell
.\buildtools\switch_config.ps1
```

```text
Target device:

  1) dsPIC33AK512MPS512   (serial update)
  2) dsPIC33AK128MC106   (serial update)

Select [1-2, Enter = keep dsPIC33AK512MPS512, q = quit]:

Delivery: resident bootloader + serial update -- the only mode
dsPIC33AK512MPS512 has a configuration for.

Application profile:

   1) Classic 1
      co-clocked dual codec; WM8904-A drives BCLK/FS
   ...
   5) ASRC Codec BI
      ASRC production M30 ...; bidirectional A<->B
      <- selected
      ... 12 more hidden: advanced, internal (show with -Advanced / -All)

Select [1-6, Enter = keep ASRC Codec BI, q = quit]: 5

Active selection:

  Serial update support: Yes
  Target device:         dsPIC33AK512MPS512
  Application profile:   ASRC Codec BI

The build will produce:
  FACTORY_IMAGE
  SERIAL_UPDATE_PACKAGE

Next:
  .\buildtools\build.ps1
  .\buildtools\flashauto.ps1
```

`Enter` keeps the current value; `q` aborts (nothing is rewritten). **The MPLAB
configuration never appears** (add `-Internal` to see the resolution alongside).
Profiles that the device cannot produce are not listed, so mixing up Classic and
ASRC, or picking ASRC on AK128, cannot happen by construction.

#### What gets listed (tiers)

With no arguments the list is `normal` only. The hidden remainder is reported at
the end with its count and tiers, so "not listed" can never be misread as "does
not exist".

```powershell
.\buildtools\switch_config.ps1            # normal only
.\buildtools\switch_config.ps1 -Advanced  # normal + advanced
.\buildtools\switch_config.ps1 -All       # normal + advanced + internal
```

The same grouping applies to `-List`.

```powershell
.\buildtools\switch_config.ps1 -List
.\buildtools\switch_config.ps1 -List -Advanced
.\buildtools\switch_config.ps1 -List -All
```

**The current selection is never changed behind your back, even when its tier is
not listed.** It is shown with its hidden status stated explicitly, and `Enter`
keeps it.

```text
Current APP_BUILD:
  APP_BUILD_ASRC_CODEC_A_B_ONLY
  [internal; hidden from this preset list]
  Enter keeps it.
```

To drive it from a script, or to settle everything in one shot, pass arguments
(no prompting). **`-Profile` is not restricted by tier** (and needs no `-All`).
It accepts either the display name or the internal name (`APP_BUILD_*`).

```powershell
# AK512 Classic, with serial update (AK512 has no standalone configuration)
.\buildtools\switch_config.ps1 -SerialUpdateSupport Yes -Device dsPIC33AK512MPS512 -Profile "Classic 1"

# AK512 ASRC, with serial update
.\buildtools\switch_config.ps1 -SerialUpdateSupport Yes -Device dsPIC33AK512MPS512 -Profile "ASRC Codec BI"

# swap only the profile (serial update / device keep their current values)
.\buildtools\switch_config.ps1 -Profile "Classic DRC"

# AK128 (serial update unsupported, so No only)
.\buildtools\switch_config.ps1 -SerialUpdateSupport No -Device dsPIC33AK128MC106 -Profile "Classic 1"

# naming an internal profile directly (warns, but selecting and building are allowed)
.\buildtools\switch_config.ps1 -Profile APP_BUILD_ASRC_DECIMATOR_MEAS

# change nothing; just print the catalogue and the current selection (-Internal adds the resolution)
.\buildtools\switch_config.ps1 -List
.\buildtools\switch_config.ps1 -List -Internal

# metadata checks (exit 1 on an unclassified tier, or a missing / duplicated artifact tag)
.\buildtools\switch_config.ps1 -CheckTiers
.\buildtools\switch_config.ps1 -CheckProfiles
```

> The old `-Configuration` / `-Preset` are still accepted during the transition
> (with a short NOTE). `-Configuration` contributes **only the device** it
> implies, and `-Preset` is treated as the profile.

```text
WARNING:
This is an internal preset and is outside the regular smoke-test scope.
```

Run with no arguments while stdin is redirected (i.e. there is no console) and it
stops with an error telling you to pass arguments, instead of hanging silently.

---

## Building: `build.ps1`

```powershell
.\buildtools\build.ps1
```

Given nothing it follows the selection, and states what it is building up front.

```text
Configuration: dsPIC33AK512_ASRC_SERIAL_UPDATE  (Asrc / dsPIC33AK512MPS512)
APP_BUILD: APP_BUILD_ASRC_CODEC_BIDIR  [active selection]
```

The `[...]` is the provenance: `[active selection]` (chosen with
`switch_config.ps1`), `[-Preset]` (a one-off override), or
`[configuration default; none selected]` (nothing selected).

| Mode | Behaviour |
| --- | --- |
| (none) | Generate the makefiles if absent, then build. |
| `-Full` | Generate makefiles → clean → build. |
| `-Clean` | Delete the outputs only. |
| `-Generate` | Generate the MPLAB X makefiles only. |
| `-Help` | Print the options only (no build). |

`-Configuration` / `-Preset` are **one-off overrides** and do not change the
stored selection.

```powershell
# build another configuration temporarily, without changing the selection
.\buildtools\build.ps1 -Configuration dsPIC33AK128_SERIAL_UPDATE
.\buildtools\build.ps1 -Configuration dsPIC33AK512_ASRC_SERIAL_UPDATE

# build another variation temporarily, without changing the selection
.\buildtools\build.ps1 -Preset APP_BUILD_USB_96

# clean verification build
.\buildtools\build.ps1 -Full
```

Passing `-Preset` a variation belonging to the other App re-resolves the
configuration, but **only within the same device** (e.g. active
`dsPIC33AK512_CLASSIC_SERIAL_UPDATE` plus `-Preset APP_BUILD_ASRC_CODEC_BIDIR` →
`dsPIC33AK512_ASRC_SERIAL_UPDATE`). It never re-resolves across devices (naming an ASRC
variation while `dsPIC33AK128_SERIAL_UPDATE` is active is an error).

`-Jobs N` sets the parallelism (default = logical core count, capped at 8).

`-Define NAME=VALUE` adds preprocessor definitions **for this build only** (not
stored). It exists so compile-time switches can be A/B compared without editing
sources.

```powershell
# e.g. build 96k ASRC through the WM8904 unified rate/role path
.\buildtools\build.ps1 -Full -Configuration dsPIC33AK512_ASRC_SERIAL_UPDATE `
    -Preset APP_BUILD_ASRC_CODEC_96K_A_TO_B -Define WM8904_USE_UNIFIED_CONFIG=1

# several at once, and value-less (NAME only), are both fine
.\buildtools\build.ps1 -Define A=1,B=2
.\buildtools\build.ps1 -Define ENABLE_SOMETHING
```

A leading `-D` is optional (`-Define -DFOO=1` works too). The definitions that
were applied are **always** echoed into the build log as `Extra define: -DFOO=1`.
That is deliberate: a definition that gets dropped silently turns into
"measuring the same image twice while believing it is an A/B comparison" — which
did happen, by passing a bare `-D` to `build.ps1` where it was discarded as an
unknown parameter.

### Build recipes for the main configurations (copy-paste)

The basic form is to choose with `switch_config.ps1` and then run a bare
`build.ps1`.

```powershell
# --- AK512 / ASRC / with serial update (FACTORY_IMAGE + SERIAL_UPDATE_PACKAGE) ---
.\buildtools\switch_config.ps1 -SerialUpdateSupport Yes -Device dsPIC33AK512MPS512 -Profile "ASRC Codec BI"
.\buildtools\build.ps1 -Full

# --- AK512 / Classic / with serial update ---
.\buildtools\switch_config.ps1 -SerialUpdateSupport Yes -Device dsPIC33AK512MPS512 -Profile "Classic 1"
.\buildtools\build.ps1 -Full

# --- AK128 / Classic (serial update unsupported) ---
.\buildtools\switch_config.ps1 -SerialUpdateSupport No -Device dsPIC33AK128MC106 -Profile "Classic 1"
.\buildtools\build.ps1 -Full

# --- RESIDENT_BOOTLOADER alone (rarely needed; a serial-update build calls it automatically) ---
.\buildtools\build.ps1 -Full -Configuration dsPIC33AK512_RESIDENT_BOOT
```

The output HEX lands under
`dspic33ak_audio_dsp.X/dist/<configuration>/production/` (`APP_BUILD` does not
appear in the path — see the stamp below). What you flash is always
`*.factory.production.hex` (the FACTORY_IMAGE).

### SERIAL_UPDATE_APP deliverables: the seed ROM and the download file

`dsPIC33AK512_ASRC_SERIAL_UPDATE` is the **one** configuration whose App HEX
cannot be flashed on its own. This layout places the App at `0x808000..`, and
**the reset vector lives inside the first 32 KiB owned by the resident
bootloader**. Flashing only the App HEX produces a board with nothing at the
reset target — and the flash still looks successful.

So a build with serial update support = `Yes` produces two more things alongside
the App's HEX/map:

| Artifact | Location | Purpose |
| --- | --- | --- |
| **FACTORY_IMAGE** | `dist/<configuration>/production/dspic33ak_audio_dsp.X.factory.production.hex` | **This is what `flashauto.ps1` writes** (RESIDENT_BOOTLOADER + SERIAL_UPDATE_APP + a committed manifest) |
| **SERIAL_UPDATE_PACKAGE** | `artifacts/serial_update_packages/<device>/sonora_<device>_<tag>_<yyyyMMddHHmm>.sfb` | **The download file.** Sent over serial to a board that already holds a FACTORY_IMAGE, to switch it to that application |

A build with serial update support = `No` produces only the FACTORY_IMAGE (whose
content is the standalone application) and no `.sfb`. **`flashauto.ps1` is
operated identically in either mode.**

RESIDENT_BOOTLOADER itself is built automatically when it is missing or older
than its sources. The FACTORY_IMAGE is always put through
`serial_boot_factory_image.py verify` after generation (generated successfully
and verified successfully are tracked as separate statuses).

```powershell
.\buildtools\switch_config.ps1 -SerialUpdateSupport Yes -Profile "ASRC Codec BI"
.\buildtools\build.ps1 -Full     # App + resident + .sfb + FACTORY_IMAGE + verify
.\buildtools\flashauto.ps1       # writes the FACTORY_IMAGE
```

- `-FirmwareVersion N` — the firmware version stamped into the `.sfb` header
  (default 1)
- `-NoDelivery` — build the App only and produce no deliverables (for checking
  that it compiles; **nothing flashable is left behind**)

#### SERIAL_UPDATE_PACKAGEs accumulate as history

The `.sfb` filename is the device, the profile's artifact tag and the build time
(local time, `yyyyMMddHHmm`), under one directory per device.

```text
artifacts/serial_update_packages/
    ak512mps512/
        sonora_ak512mps512_classic1_202608151302.sfb
        sonora_ak512mps512_asrc_bi_202608151304.sfb
    ak128mc106/
        sonora_ak128mc106_classic1_202608151237.sfb
```

**The device is in both the directory and the filename, on purpose.** The
directory is what makes the history browsable once two parts are in the fleet;
the name is what still says which board a file is for after it has been dragged
out of that directory to be sent. A package carries its layout internally as
well (`layout_id` "SAK1" / "SAK3"), and the bootloader refuses one built for the
other arrangement before it writes anything — so a mixed-up name is a nuisance
rather than a hazard. The full part number rather than just the size, because the
two parts differ in family too (MP vs MC).

The same fence separates **generations** of one part's layout, which is worth
knowing before an old file is dragged back out of this directory: AK128 packages
older than 2026-08-20 carry the retired `"SAK2"` (28 KiB boot region, application
at `0x807000`) and are refused by a current bootloader. `serial_boot_package.py`
names retired layouts when it declines them. A board still running a `SAK2`
bootloader is moved to the current layout by a PKOB4 factory flash — a package
cannot move a partition boundary.

Building the same profile several times within the same minute **does not
overwrite** (`_02`, `_03`, … are appended). This directory sits outside `dist/`
and **survives `build.ps1 -Clean` / `-Full`** — a past package you want to switch
back to must not disappear on the next build. Deletion is manual (a dedicated
command can be added if it is ever needed; it will not be folded into an ordinary
Clean). Already `.gitignore`d (`artifacts/`).

#### Guard against flashing a stale FACTORY_IMAGE

Build-time metadata (`*.factory.production.json`) is written next to the
FACTORY_IMAGE. `flashauto.ps1` compares it with the current selection and
**refuses to write on a mismatch**.

```text
FACTORY_IMAGE does not match the active selection.
  application profile   : image APP_BUILD_STD_DEMO_1, selection APP_BUILD_STD_DEMO_2
Run:
  ./buildtools/build.ps1
```

Switch the profile, forget to rebuild, and flashing loads the earlier firmware
while looking successful. This check exists to stop that accident structurally.

### Cleaning only when `APP_BUILD` changed

The variations of one App share the same object directory, so objects from a
different `APP_BUILD` must never be mixed in. `build.ps1` records "what these
objects were built as" in `build/<configuration>/.sonora_app_build` and promotes
to `-Full` **only when it changed**.

```text
APP_BUILD changed (APP_BUILD_STD_DEMO_1 -> APP_BUILD_USB_96): promoting to -Full.
```

Rebuilding the same variation stays incremental. When the stamp is absent (built
from MPLAB X IDE, say, so the provenance is unknown) it errs on the safe side and
promotes to `-Full`.

It also promotes to `-Full` when `configurations.xml` is newer than the generated
makefiles (you pulled, or changed project settings in the IDE). The generated
makefiles are only a snapshot of `configurations.xml`, and building incrementally
after the per-conf macros or source lists changed compiles new sources with old
macros — which then happens to link, or happens not to.

**Orphan objects** — `.o` files still present in `build/<conf>/production/` that
the generated makefile's `OBJECTFILES` no longer lists — also promote to `-Full`.
They arise from deleting or renaming sources, switching branches, or changing a
conf's source list, and make neither rebuilds nor removes them. **The orphan
`.o` is not itself linked** (linking uses `${OBJECTFILES_QUOTED_IF_SPACED}`, i.e.
exactly what the current makefile enumerates). But it is evidence that the build
tree and the makefile disagree about which sources exist, so rather than deduce
what else may be out of step, it falls back to a clean build.

```text
2 stale object(s) no longer listed by the makefile (old_module.o, renamed_thing.o): promoting to -Full.
```

So "accidents caused by stale artifacts" fall back to clean automatically under
four conditions: (1) `APP_BUILD` changed, (2) no stamp, (3) `configurations.xml`
updated, (4) orphan objects. You do not need to type `-Full` every time.

### `-App` is deprecated

`-App Asrc|Classic` is a leftover from the days when no native ASRC
configuration existed (2026-07-19..21). Configurations now declare their App
themselves, and an `APP_BUILD` variation belongs to only one App, so the switch
carries no information. It is still accepted but **redundant**, and contradicting
`-Preset` is an error. Do not write it into new instructions.

### Build revision banner

On every build, including a plain no-option one, `build.ps1` injects Git HEAD's
short commit ID as a preprocessor token. No dedicated option, pre-build step or
generated header is involved. The startup banner shows:

```text
 Commit: <short7>
```

This is the same mechanism as `APP_SRC_DIRNAME` (the `Source:` line):
`build.ps1` passes `-DSONORA_GIT_COMMIT=<value>` to the compiler as a bare
token, and `main.c` stringifies it for display.

- A build with tracked or untracked working-tree changes shows `<short7>_dirty`
  (`_dirty`, not `-dirty`, so it stays a valid C token).
- Where Git is unavailable — a source archive, for instance — it becomes
  `unknown`.
- Building directly from MPLAB X IDE, bypassing `build.ps1`, shows `(unknown)`
  via the `#ifndef` fallback in `main.c`. No IDE-side setup is needed.

`APP_SRC_DIRNAME` and `SONORA_GIT_COMMIT` are both defined immediately before
the banner in `main.c`, so only `main.c` — which owns the banner lines — is
recompiled when their values change.

---

## Flash / reset

`flashauto.ps1` resolves the configuration from the current selection (the three
choices) by exactly the same procedure as `build.ps1`, and **always writes the
FACTORY_IMAGE**. Serial update support makes no difference to how it is
operated. Without `-Device` it derives the device from the configuration's
`targetDevice`; without `-Serial` it uses the connected PKOB4 if there is
exactly one (with none, or with several, it stops and says why). It also reports
which profile the last build was, from the stamp.

```powershell
.\buildtools\flashauto.ps1              # flash, then reset (no options needed)
.\buildtools\flashauto.ps1 -ResetOnly   # reset only, no flash
.\buildtools\resetauto.ps1              # shorthand for the same
.\buildtools\flashauto.ps1 -DryRun      # only print what it would do
```

### Stopping the WM8904 before flashing (pop suppression)

Immediately before writing or resetting, `flashauto.ps1` sends `*ts` to the
Sonora image already on the board and confirms the HPOUT analog-mute readback on
WM8904 A/B plus TDM/DMA shutdown. The monitor to talk to is resolved from this
worktree's `.serial-monitor.json`, and `GET /status` is used to confirm
`profile: sonora` and a live UART connection.

`/wait` is armed *before* `*ts` is sent, so a short success response is not
missed. The UART COM port must never be opened directly; only the HTTP API owned
by the monitor process is used.

**The mute is best effort, not a precondition.** `*ts` is a request to software
that has to be running to hear it, so it cannot be a condition for replacing that
software — a flash is very often the *response* to an image that stopped
answering. When the mute cannot be confirmed, the script warns loudly and
programs anyway.

| Situation | Why `*ts` cannot succeed | What happens |
| --- | --- | --- |
| The board has no image, or a half-written one | Nothing is listening on the console. | warn + flash |
| The serial downloader / bootloader console does not come up | The responder is the thing being replaced; recovery has no `*ts`. | warn + flash |
| An old image predating `*ts` | The command does not exist in it. | warn + flash |
| The app is hung, or trapped | It will never reach the `*ts` handler. | warn + flash |
| The monitor serves a **different** profile | Typing into another board's console is not ours to attempt. | attempt skipped, flash proceeds (the target is chosen by PKOB4 serial) |

Two switches change that default, in opposite directions:

```powershell
.\buildtools\flashauto.ps1 -StopAudioCommand none   # skip the attempt entirely
.\buildtools\flashauto.ps1 -RequireAudioStop        # old behaviour: abort unless confirmed
```

**Which to use when the target cannot answer:** if speakers are connected and a
moment of unmuted output is unacceptable, mute or turn the output down physically
and pass `-StopAudioCommand none` — that states the intent, and skips an attempt
that was going to fail. Bare (best effort) is right the rest of the time; the
warning names the same risk. `-RequireAudioStop` is only for a bench where that
second genuinely matters, and never in a script that must work on an unresponsive
board.

Confirmed 2026-08-15: a flash onto a board holding no image failed with `WM8904
did not confirm a verified analog mute within 20s (HTTP 408)`. That refusal is
what the best-effort change removed; `-StopAudioCommand none` is the explicit way
to say so up front.

**The reset after a successful flash always happens** — it is not an option and
cannot be suppressed. `-ResetOnly` means "reset **without** flashing". The old
name `-Reset` was **removed** because it was actually misread as "reset after
flashing"; passing it now stops with the correct spelling shown.

**The reset timeout is fixed at 120 seconds and is not a parameter.** Passing
`-ResetTimeoutSec` / `-Timeout` stops. A reset that times out means the board is
not responding (power / USB, the B jumper, another tool holding the PKOB4);
shortening the wait only turns a cold start into a false failure. If you truly
need a different wait, call
`buildtools\_flash_reset_tools\reset_pkob4.exe` directly.

```text
Configuration: dsPIC33AK512_ASRC_SERIAL_UPDATE
Last build of this configuration: APP_BUILD_ASRC_CODEC_BIDIR
```

To see only what it would do:

```powershell
.\buildtools\flashauto.ps1 -DryRun -Serial <PKOB4_SERIAL>
.\buildtools\flashauto.ps1 -DryRun -ResetOnly -Serial <PKOB4_SERIAL>
```

`-Serial` can be omitted when exactly one PKOB4 is connected (with none or
several it stops rather than guessing from enumeration order). Clean-build the
intended configuration right before flashing (the HEX in `dist/` may be stale).

---

## VS Code tasks and the `MPLABX_CONF` leftover

`MPLAB: Build` and friends in `.vscode/tasks.json` call this `build.ps1`, so they
follow the selection; so does `MPLAB: Flash (PKOB4)`, which calls
`buildtools/flashauto.ps1` (it resolves configuration/device from the active
selection itself). `.vscode/clean.ps1`, on the other hand, still looks at
`MPLABX_CONF` — whose default is the now-deleted `dsPIC33AK512`, so it must be
set explicitly. `build.ps1` sets `MPLABX_CONF` to its own configuration for the
duration of the clean it invokes and restores it afterwards, so
`build.ps1 -Clean` / `-Full` always clean the right configuration.
`.vscode/flash.ps1` (flashed the bare application hex via `mdb` directly, with
no reset vector for any currently-shipped serial-update configuration) was
deleted 2026-08-16 — always use `buildtools/flashauto.ps1` to flash.

## Configuration gate

`nbproject/configurations.xml` is the single truth for "what each configuration
compiles and what it excludes". MPLAB X IDE rewrites that file whenever it
touches the project, and has silently dropped `ex="true"` in the past. A lost
exclusion still builds, and **an image that is supposed to be standalone comes
out with the download engine linked in, saying nothing** — the kind of defect you
discover on the board rather than at the desk.

So `build.ps1` runs this at the start of every build and does not begin building
if it fails (no toolchain needed, a few seconds). It also runs standalone.

```powershell
./buildtools/check_configurations.ps1
```

It checks per configuration (per `<conf>`, not by total string counts):

- that exactly the 5 configurations of the expected table exist (an
  unclassified `<conf>` is a failure — an unclassified configuration would not
  be checked at all)
- that `SONORA_DELIVERY_SERIAL_UPDATE_APP=1` is in the `preprocessor-macros` of
  every tool for the delivery configurations, and nowhere for the standalone ones
- that the serial-update linker script is excluded in standalone and not
  excluded in delivery
- that the 6 download-engine sources are registered and excluded in standalone,
  and not excluded in delivery
- that `src/boot/**` is **not registered at all** in the application
  project (the boot image links separately; absence, not exclusion, is correct)
- that registered sources exist, and that tracked sources under `src/app/`,
  `src/shared/` and `src/boot/` are either registered or listed in the script's table of
  exceptions with a reason
- that every `extra-include-directories` entry **exists on disk** (three did not
  until 2026-08-14: a non-existent `-I` path contributes nothing, so nothing
  complains, and the list stops being a statement about the tree)

The configuration expectations are design intent: changing one requires an
intentional update to the table at the top of the script.

### Boot/application HAL copies

```powershell
pwsh buildtools/check_hal_drift.ps1        # add -All to list the identical files too
```

The resident boot image compiles its **own** copy of the six HAL modules it needs
under `src/boot/hal_*`. The application and boot implementations are independent
because the boot image has its own fixed memory budget.

`check_hal_drift.ps1` compares the two copies path for path and byte for byte and
**always exits 0**. A difference is not necessarily a defect: the boot copy may
be older, smaller, or deliberately different. Use the report when a change to an
application HAL may also be needed by the boot image.

Its counterpart is `Assert-StandaloneMapLayout` /
`Assert-SerialUpdateMapLayout` in `build.ps1`, which check the **link result**
(the `.map`). The host test that detects a change killing either of them is
`src/tests/build_layout_gates/run_host_tests.ps1` (a mutation test — it confirms each
check actually fires).

```powershell
pwsh src/tests/build_layout_gates/run_host_tests.ps1
```

Only when the baseline genuinely has to be redefined wholesale is
`-ForceRebaseline` available, which accepts increases explicitly. Do not use it
for an ordinary phase transition; when a temporary dependency is unavoidable,
state that dependency and the phase that removes it in a design document, and
treat it as reviewable.

---

## Identifying the build and tracing startup (the resident boot image)

`build_resident_bootloader.ps1` compiles the shared HAL straight from the working
tree, which means **the boot image picks up the application side's HAL changes
too** (measured: merely taking in the latest `main` once grew it by 208 bytes).
That is not an accident to be avoided; it is the property that one tree yields
one matched Boot + App pair. Knowing **which tree an image came from** is
therefore enough, so the commit is burned in at build time and one line is
printed at startup.

```
BL <short7> DE ABI=1
```

- The script injects `-DSONORA_BOOT_GIT_COMMIT=<short7>` (`<short7>_dirty` if
  the working tree is dirty, `unknown` without git). Building directly from the
  IDE also gives `unknown`.
- If you need an older bootloader, check out that commit or release tag and
  build it. Release-tag practice belongs to git (and to whoever uses it); the
  repository has no dedicated tooling for it.
- This line appears **on power-on, MCLR and recovery**, because the warm-reset
  fast path jumps to the application before UART initialisation.
- **The size is printed every time** (`Resident bootloader: 0x…… / 0x8000`).
  This is where shared-HAL changes land, so the habit of reading that value is
  the only protection there is. Those 208 bytes went unnoticed because nobody
  was prompted to look.

### `RESIDENT_BOOT_ENA_BOOT_TRACE` (default 0)

The forensic startup output (entry clock / mailbox / pre-CRT / reset-source
registers, the default vector, the `BL CPU:` dump, the staged
`Manifest check:` reporting) is for bring-up only, so it is disabled by default.
Its effect against the 32 KiB ceiling is large: **measured 0x7E80 → 0x6198
(7,912 bytes saved; headroom from 400 bytes to 7.6 KiB)**.

```powershell
pwsh buildtools/build_resident_bootloader.ps1 -Full -Define @('RESIDENT_BOOT_ENA_BOOT_TRACE=1')
```

The code is wrapped in `#if` rather than deleted, so it comes straight back when
startup needs to be *explained* and not merely *observed*. What remains with it
disabled: the banner, the reset-cause word (`BL POR` and so on),
`Manifest OK; resetting into application.`, the recovery / XMODEM prompts,
`Update committed; resetting.`, and `BL REQ` / `BL LOOP` / `BL IDLE->APP`
(`serial_monitor/tests/test_traffic_observer.py` in the `serial-monitor` repo
checks for `Update committed`).

---

## Troubleshooting

| Symptom | Cause / remedy |
| --- | --- |
| The `APP_BUILD` you want is not listed | Its tier is `advanced` or `internal`. Show it with `-Advanced` / `-All`, or name it directly with `-Preset <name>` (no tier restriction). |
| A preset you added to the header is not listed | The `/* tier: ... */` marker is missing (`unclassified` appears in no list). `.\buildtools\switch_config.ps1 -CheckTiers` enumerates them. |
| An `internal` preset does not build | Within spec (`internal` is outside the smoke-test scope). `normal` / `advanced` are kept buildable. |
| The selection reverted to Classic after a pull | `Makefile-impl.mk` was untracked (2026-07-30), so pulling that commit deletes the file once in your working tree. Re-select with `switch_config.ps1` and it persists from then on (the next build regenerates it, and `active_build.json` is the authority thereafter). |
| `Makefile-impl.mk` shows up in `git status` | An older clone holding a commit from before it was untracked. If it survives `git pull`, run `git rm --cached dspic33ak_audio_dsp.X/nbproject/Makefile-impl.mk` once (the file itself stays). |
| A different App than expected gets built | Check the current selection with `.\buildtools\switch_config.ps1 -List`. `build.ps1` prints `Configuration:` and `APP_BUILD: ... [provenance]` every time. |
| A change you made does not take effect | Rather than leftover objects from another `APP_BUILD`, check the `-DAPP_BUILD` conditionals. Use `-Full` to be sure. |
| `Clean incomplete: locked ...` | MPLAB X IDE (the `mplab_backend` process) is holding the objects. Exit the IDE completely and retry. |
| It says `flashauto: reset only` and no HEX is written | You passed `-ResetOnly` (formerly `-Reset`). The reset after a flash is unconditional, so flashing needs no switch. |
| `The reset timeout is not adjustable ...` | By design. The timeout cannot be shortened (see "Flash / reset" above). If it really did time out, suspect power / USB / the B jumper / something else holding the PKOB4. |
| `No MPLAB X project directory (*.X with nbproject) found under: ...\buildtools` | Before 2026-07-30, `build.ps1` / `flashauto.ps1` defaulted `$Root` to the current directory, so running them from inside `buildtools/` produced this. Fixed (see "no matter which directory" above). |
| `Unknown MPLAB configuration '...'` | A typo. The usable names are listed in the error. |
| An ASRC variation is rejected | The active configuration is AK128, which has no ASRC. Pass `-Configuration dsPIC33AK512_ASRC_SERIAL_UPDATE`. |
| `#error "ENA_96K_RATE requires ... dsPIC33AK512."` | You selected a 96 kHz / USB variation on AK128 (see "per-device availability" above). |
| Build in the IDE, return to the CLI, and it promotes to `-Full` | By design (an IDE build leaves no stamp, so the provenance is unknown = err on the safe side). |
