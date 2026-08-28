# WM8904 codec-master and variable-rate ASRC

## Status and scope

This document records the implemented WM8904 codec-master topology and its verified runtime-rate
behavior. It replaces the former Phase-A design and pre-implementation research notes.

The feature is available on the dsPIC33AK512 build with the second WM8904 enabled. The WM8904 is a
stereo device: each codec consumes at most two audio slots even when the dsPIC transport frame is
configured as TDM8. The current board therefore has four physical codec channels; larger ASRC channel
counts are compute/transport workloads unless additional external endpoints are attached.

## Clock-domain topology

The two independent-domain modes are deliberately separated by clock ownership:

| Mode | WM8904-B | dsPIC B leg | B MCLK | Purpose |
|---|---|---|---|---|
| Co-clocked demo | slave | slave, same domain as A | routed from the established board clock | classic demo |
| dsPIC-master ASRC | slave | master, independent domain | derived by board routing | ASRC with dsPIC-owned B clock |
| codec-master ASRC | master | slave, independent domain | B board XTAL via jumper | ASRC with WM8904-B-owned clock |

`APP_B_INDEP_DOMAIN` is the ASRC/domain fact. Clock ownership remains a separate fact so code that
means “asynchronous B domain” does not accidentally mean “dsPIC is the B clock master.” Named
`APP_BUILD_*` presets derive the required profile, owner and route settings.

### Codec-master hardware rule

Codec-master mode requires both actions together:

1. Set the WM8904-B board jumper to connect B-XTAL to B-MCLK.
2. Ensure firmware does not drive the B-MCLK/RP97 net.

Doing only the first risks two drivers on the MCLK net. Doing only the second leaves B without an
MCLK. A/B frame-clock passthrough must also be disabled because WM8904-B drives its own BCLK/FS.

WM8904-B must be initialized as master before the dsPIC slave leg is armed. The HAL readiness gate is
not a per-domain external-clock detector, so initialization order is part of the board contract.

When moving from a codec-master image back to a classic image, cold-power-cycle the codec boards.
Neither a dsPIC reset nor a reflash reaches the codec's own reset, so whatever interface state a
previous image wrote into WM8904-B -- including clock-direction bits that make it a master --
persists indefinitely. A classic image then finds a leg that is driving its own BCLK/FS while the
image expects to drive them, which is a clock conflict and not something firmware infers or
corrects: the board topology and each leg's clock ownership are declared at build time, and there
is no runtime codec-identity or alias probe anywhere in the tree. A full power cycle returns the
codec to its POR defaults, which is the only thing that clears it.

## WM8904 rate model

WM8904 SYSCLK is selected from direct MCLK or the codec FLL. `CLK_SYS_RATE` defines SYSCLK/fs;
`BCLK_DIV` and `LRCLK_RATE` derive the serial clocks. Codec-master mode supports the standard rate
menu through the runtime rate table.

The 48 kHz family divides directly from the 12.288 MHz crystal and remains FLL-less:

| fs | SYSCLK/fs | FLL |
|---:|---:|---|
| 48 kHz | 256 | off |
| 32 kHz | 384 | off |
| 24 kHz | 512 | off |
| 16 kHz | 768 | off |
| 12 kHz | 1024 | off |
| 8 kHz | 1536 | off |

The 44.1 kHz family uses one shared FLL setup to synthesize 11.2896 MHz from 12.288 MHz:

| fs | SYSCLK/fs | FLL |
|---:|---:|---|
| 44.1 kHz | 256 | on |
| 22.05 kHz | 512 | on |
| 11.025 kHz | 1024 | on |

The verified FLL parameters are FVCO=90.3168 MHz, OUTDIV=8, N=7, K=`0x599A`, fractional mode on,
FRATIO=/1 and MCLK reference /1. Register programming occurs with the FLL disabled, followed by FLL
enable, lock delay and SYSCLK source selection. Direct-MCLK rates leave the FLL disabled.

96 kHz extends the 48 kHz family the same way — direct MCLK, FLL-less — but the serial-clock divider
only exists for a 2-slot I2S frame (BCLK = fs × 64: 12.288 MHz / 2 at 96 kHz, the same pattern as the
48 kHz family's own /4). It is deliberately absent for TDM8, which is what "Runtime control" below
refuses at the console rather than at the register level. **88.2 kHz is not implemented**: the
44.1 kHz family stops at 44.1 kHz, and no 88.2 kHz row exists in the rate table.

## Runtime control

The WM8904 driver keeps a rate per codec instance and exposes rate set/get operations. Application
control accepts (write only):

```text
*ar CC RR
```

- `CC=0`: codec A, `CC=1`: codec B
- `RR=0..9`: 8, 11.025, 12, 16, 22.05, 24, 32, 44.1, 48 and 96 kHz

`*ar` is write-only — there is no query verb for the currently active rate. It only works on a
codec-master build (leg B clocked from its own endpoint); on any other build it refuses with "runtime
rate change needs an endpoint-clocked leg B".

### 96 kHz: the hardware boundary versus the implemented menu

Two things are easy to conflate here, so they are separated:

- **The hardware restriction** is the WM8904's own: at **fs ≥ 88.2 kHz** the part cannot run its ADC
  and DAC simultaneously, and `ADC_OSR128`/`DAC_OSR128` must be clear. That is where the datasheet
  draws the line, which is why the firmware's checks are written as a `≥ 88200` threshold.
- **What this project implements** is the ten rates listed above. **There is no 88.2 kHz entry** — no
  table row, no `RR` code, no command mapping. 96 kHz is the only implemented rate on the far side of
  the hardware boundary, so it is the only rate those checks can ever refuse.

**96 kHz therefore has two extra conditions, checked before anything is touched — a request that
cannot succeed is refused with its reason and the running stream is left completely untouched:**

- **The build's I2S frame must be 2 slots, not TDM8.** A TDM8 build refuses with "96 kHz needs
  a 2-slot I2S frame; this build is TDM8" (8 × 32 × 96k = 24.576 MHz BCLK is beyond this SYSCLK).
- **The target leg must be ADC-only or DAC-only, not full-duplex.** Each leg's role is fixed at build
  time by that leg's *initial* configured rate (an initial rate at or above the hardware boundary
  locks it to one-way; below that, it is full-duplex ADC+DAC). A leg built full-duplex refuses with
  "96 kHz cannot run ADC and DAC together; this leg is full-duplex" — `*ar` does not change a leg's
  role, only its rate. In practice this means the default bidirectional builds (both legs
  full-duplex, 8/11.025-through-48 kHz family) cannot reach 96 kHz from the console; only a build
  already configured one-way over a 2-slot frame can.

96 kHz needs no dedicated register sequence: it is a rate-table row like the others, differing from
48 kHz in two fields (`CLK_SYS_RATE` 128 fs against 256 fs, `BCLK_DIV` ÷2 against ÷4) and reusing the
48 kHz `SAMPLE_RATE` code. Its TDM8 columns are marked unsupported rather than filled in, which is
what makes a TDM8 request a refusal instead of a mis-programmed codec.

A rate change that passes those checks performs the normal mute-bounded restart, reinitializes the
selected codec and lets the ASRC ratio controller reacquire. Changing codec A also retunes the live
A-domain DSP modules through an fs-aware initialization guard. Changing only codec B does not
unnecessarily retune A-domain DSP.

## Verified results

Hardware qualification used the AK512 Curiosity platform with two WM8904 boards and the B-XTAL to
B-MCLK jumper. This qualification predates the 96 kHz rate table entry and its one-way/2-slot build
requirement (see "Runtime control" above) — it verified the nine rates that existed at the time.

- WM8904-B codec-master with dsPIC slave/independent domain: PASS.
- A-to-B ASRC audio at approximately 48 kHz on both codec clocks: PASS, sustained transport misses 0.
- All nine 8–48 kHz-family runtime rates on codec B: PASS, audio present and sustained misses 0.
- 44.1 kHz-family FLL operation and return from FLL to direct MCLK: PASS.
- Codec A runtime-rate changes with A-domain DSP retuning: PASS.
- Bidirectional A↔B ASRC cross-connect: PASS.

Representative ratios were 48/44.1=`1.088435`, 48/11.025=`4.353741` and 48/48=`1.0`. Absolute CCP
frequency readout was slightly low when measured against the FRC-derived timebase, while the A:B ratio
was correct. That display offset was not a codec-rate error.

The tested board reported A and B rates almost identical at 48 kHz. This proves the topology but does
not prove large asynchronous crystal offset; the two boards may be effectively coherent on the test
fixture. ASRC servo behavior at near-unity ratio was stable.

## Bidirectional route and resource decisions

Bidirectional mode is a cross-connect:

- A output receives B input resampled to fsA.
- B output receives A input resampled to fsB.

Both ADC inputs must be live. The two ASRC directions share the coefficient table but keep independent
FIFO/state objects. The adopted memory reductions are FIFO 128 frames with center target 64 and a
16-frame audio block on AK512 ASRC builds.

The L128/M32 coefficient table stays in RAM for speed. Flash residency was measured as slower and is
not the active configuration. Sound effects are not part of the ASRC route and should remain gated
with their SST26 dependency when that feature is disabled.

## Remaining constraints

- WM8904 provides only two physical audio channels per codec; extra channels require external TDM
  endpoints.
- The fixed FLL lock delay worked in qualification; polling the codec lock indication remains a
  possible robustness improvement.
- The live rate menu now reaches 96 kHz, but only on a build whose target leg is already one-way
  (ADC-only or DAC-only) over a 2-slot I2S frame — see "Runtime control" above. 88.2 kHz is not
  offered at all; the hardware would allow it under the same one-way/2-slot conditions, but no rate
  table row or command code exists for it. Several
  inactive/legacy DSP modules still assume a compile-time 48 kHz reference and would need review
  before enabling them at arbitrary rates.
- Per-domain external-clock readiness is still primarily guaranteed by codec initialization order.
- A fixture with genuinely independent oscillators is needed for large-offset asynchronous stress.
