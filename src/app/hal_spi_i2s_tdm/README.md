# nora_spi_i2s_tdm — SPI framed-mode I2S/TDM transport HAL

A compact, reusable SPI/I2S/TDM **transport** HAL for dsPIC33AK. It moves audio
frames over a framed SPI peripheral with DMA ping-pong and
a per-instance block callback. It is intentionally **small**: it does not try to be a
turnkey "drop-in and forget" audio stack. Board-specific, failsafe, and CMSIS-SAI
buffer-semantics concerns stay in layers above it, so a project can extend only what it
needs.

> Want to run it on hardware first?
> Start with [dspic33ak-hal-starter](https://github.com/sulaolab/dspic33ak-hal-starter),
> a ready-to-build MPLAB X project for the dsPIC33AK Curiosity board.

## 1. What this HAL does

- dsPIC33AK SPI framed mode (AUDEN=0, FRMEN=1) used as an I2S/TDM transport.
- RX/TX ping-pong DMA, double-buffered.
- One per-instance block callback per physical SPI: `cb(src, dst, user)`.
- Selectable frame-sync waveform via `config.fs_shape`: `FS_PULSE` (short ~1-BCLK sync) or
  `FS_50PCT` (50%-duty FS). I2S 50% is native; a TDM **master** gets a 50%-duty FS
  synthesized by **CLC10** on the FS pin (auto-detected by PPS reverse-lookup; no app/CLC
  code). A TDM slave receives FS as an input, so `fs_shape` is accepted but has no
  generated-waveform effect (still validated, and compared as a framing field within a sync
  domain). See `nora_spi_i2s_tdm_dspic33ak_fs_clc.{c,h}`.
- Two configure/lifecycle paths: **system** `configure_system(setups, count)` (transactional,
  all-or-nothing) + `open()` + `start_all_domains()`, or **single-instance**
  `inst_configure(inst, cfg)` + `open()` + `inst_start(inst)`. `open()` takes **no role** —
  it derives the clock role from the committed primary leg. Plus `get_status` / `get_load`
  diagnostics (block count, deadline-miss, ISR load; the arg-less ones report the primary leg).
- Optional board/clock **port** hook (`set_port()`) for pin/CLC routing and external-clock
  bring-up/readiness — the core calls only through this registered port.
- Per-instance SPI framed-transport health diagnostics (`SPIROV` / `SPITUR` / `FRMERR`, sampled
  once per completed RX block) — see "DMA and SPI transport-health diagnostics" below.
- Multi-instance: the core owns a dense logical leg table. The default bank maps rows 0/1 to
  physical SPI1/SPI2; `NORA_TDM_BASE_ON_SPI34` explicitly maps those rows to SPI3/SPI4.
  `spi1()`...`spi4()` always mean literal physical peripherals; application-semantic legs use
  dense `inst(0)`/`inst(1)`. `conf.h` supplies DMA channels, geometry, and initial `SYNC_DOMAIN`.
  Per-leg format/role come from the runtime
  config. The core defines the leg enum/buffers/table/`_DMA<rx>Interrupt` vectors in explicit C
  (no generator macro). Enumerate with `instance_count()` + `inst(i)`.

## 2. What this HAL does NOT do

- No codec init (e.g. WM8904) — that is board/app code.
- No general board pin routing in the core — board FS/BCLK/DATA/MCLK routing is supplied by
  the registered port hook. **Exception:** for `TDM master + FS_50PCT`, the HAL-owned CLC10
  helper (`nora_spi_i2s_tdm_dspic33ak_fs_clc.*`) temporarily repoints the already-routed `SSx` FS
  pin to `CLC10OUT` (and routes `SSx`→RPV8) and restores it on `release()`.
- No DSP — the callback owns any processing.
- No sample-rate policy — the transport is rate-agnostic (runs at the configured BRG or
  the incoming external clock); the supported-rate set is an app concern.
- No failsafe / board-specific teardown in the core — `close()` is a near-no-op; pin/clock
  release is left to the integrator (a future optional port deinit hook, not included).
- No CMSIS-SAI types in the core — `ARM_SAI_*` must not appear here.
- No automatic recovery from SPI framed-transport health-flag events (`SPIROV` / `SPITUR` /
  `FRMERR`) — the HAL only records per-RX-block observations (see "DMA and SPI transport-health
  diagnostics" below); reacting to them is an app-layer policy decision.

## 3. Required project config

- The project MUST provide `nora_spi_i2s_tdm_conf.h` on the include path.
- The HAL folder ships a self-contained template: `nora_spi_i2s_tdm_conf.h_example`.
- Copy/rename the example (or supply an equivalent header) and edit the geometry, logical leg
  count, optional SPI3/4 rows or explicit SPI34 bank selection, per-instance DMA channels, and
  per-leg `SYNC_DOMAIN` defaults. `*.h_example` is never compiled.
- The template is self-contained (no app-config dependency). A project MAY instead derive
  the `NORA_TDM_*` macros from its own app config (Sonora does this in its own board-level
  `nora_spi_i2s_tdm_conf.h`); that is the integrator's choice and does not make
  the HAL core app-dependent (dependency is app → conf.h → HAL, never HAL → app).

## 4. Required sibling HALs

- `NORA DMA HAL` (`nora_dma`) — DMA channel setup/arming (required). Standalone repo:
  [nora-hal-dspic33ak-dma](https://github.com/sulaolab/nora-hal-dspic33ak-dma).
- `nora_high_res_timer` — compile/link sibling dependency for the load monitor.
  Runtime use is gated by `nora_high_res_timer_is_initialized()`; if the timer is
  not initialized, `get_load()` / `inst_get_load()` returns `false` and zeroes the supplied
  load struct. Standalone repo:
  [nora-hal-dspic33ak-timer](https://github.com/sulaolab/nora-hal-dspic33ak-timer) (the
  high-resolution counter, Timer2 or an SCCP on parts without Timer2).
- `nora_cpu_load_prof` -- compile/link sibling dependency, in the same timer HAL. The RX-block
  ISR calls the profiler's enter/exit hooks unconditionally (`nora_cpu_load_prof_fast.h`), so
  the include path must reach it and `nora_cpu_load_prof_dspic33ak.c` must be in the build.
  Build with `-D NORA_CPU_LOAD_PROF=0` and the hooks compile to nothing.
- The SPI register-mask helper (`nora_spi_i2s_tdm_dspic33ak_reg.h`) ships inside this HAL folder.

## 5. Supported devices

Currently supported (silicon facts present):

- `__dsPIC33AK512MPS512__`
- `__dsPIC33AK128MC106__`

Topology availability differs by device:

- **AK512:** SPI1/2, the explicit SPI3/4 test bank, and four-leg SPI1..4 topologies are available.
- **AK128:** paired SPI3/4 and four-leg modes are unavailable because SPI4 and DMA6/7 are absent.

> Note: the `FS_50PCT`-via-**CLC10** path (TDM master) requires CLC10 + virtual pin RPV8, so
> it is **AK512-only**. On **AK128** (no CLC10) a TDM master uses `FS_PULSE`; `FS_PULSE` and
> I2S-native `FS_50PCT` work on both parts.

The HAL `#error`s on any other device. Adding a new dsPIC33AK part means adding its
silicon facts in the HW layer (`nora_spi_i2s_tdm_dspic33ak_hw.{c,h}`):

- SPI instance count
- `SPIxBUF` / `SPIxCON1` / `SPIxBRG` / `SPIxIMSK` / `SPIxSTAT` pointers
- DMA trigger values (as `nora_dma_trigger_t`)

CPU IRQ enable/flag bits are **no longer** part of that list: they go through the DFP's
`_DMAxIE` / `_DMAxIF` bit aliases, so there is no per-device `IEC`/`IFS` mask table to
extend (see "Interrupt ownership").

The vendor part macro is confined to the dsPIC33AK backend adapter in
`nora_spi_i2s_tdm_dspic33ak_hw.h`. Public headers expose neither the compiler part macro nor
the backend's opaque device tag.

## 6. DMA and SPI transport-health diagnostics

The RX ISR preserves raw `DMAxSTAT` before HALF/DONE resolution.
`rx_dma_overrun_count` records RX-DMA request-overrun snapshots, including an OVERRUN-only
interrupt that cannot deliver an audio block. `rx_dma_other_irq_count` counts snapshots with
neither HALF nor DONE, and `rx_dma_last_status` exposes the latest raw snapshot. `SPIROV` may
appear downstream when RX service stalls; `SPITUR` independently reports TX FIFO starvation,
while `FRMERR` reports frame-sync health.

The HAL deliberately hard-forces `IGNROV=1` and `IGNTUR=1`; these are no longer caller config
fields. This prevents a FIFO flag from critical-stopping the SPI leg and obscuring earlier
transport evidence. It does not make data loss benign, so the DMA and SPI diagnostics remain
publicly visible.

`nora_spi_i2s_tdm_hw_sample_ack_errflags(inst)` samples `SPIxSTAT` once per completed RX
block (`SPIROV | SPITUR | FRMERR`) and is the HAL's single ack point for the two
software-clearable bits, `SPIROV`/`FRMERR` (a W0C-safe write of only the software-clearable mask
with the observed bits zeroed — never a replay of the whole status word, so a previously
unobserved clearable bit asserted after the snapshot is preserved). `SPITUR` self-clears only in hardware
(`SPIEN=0`), so it is only ever observed. `nora_spi_i2s_tdm_diag_note_errflags()` folds the
mask into four per-instance, per-RX-block counters read via `get_status()`:
`err_rov_block_count` / `err_tur_block_count` / `err_frm_block_count` /
`frmerr_consecutive_blocks`. See the root README for the full ownership contract and counter
semantics.

## 7. Interrupt ownership

- `NORA_TDM_DEFINE_DMA_VECTORS=1` (default): the HAL defines the RX DMA interrupt
  vectors itself (turnkey). Nothing else to wire.
- `NORA_TDM_DEFINE_DMA_VECTORS=0`: the HAL defines no vectors; the integrator owns the
  IVT and calls `nora_spi_i2s_tdm_inst_rx_isr(inst)` from their own `_DMA<rx>Interrupt`
  for each instance.

TX is interrupt-less (no TX interrupt is enabled by the transport).

## 8. Tested envelope

State honestly:

- The default configuration is stable (boot, blocks advancing, `miss=0`, audio
  unchanged).
- An exhaustive format/role matrix test is **not** complete.
- Validated / currently intended envelope:
  - 32-bit word.
  - I2S 2-slot, or TDM 4/8/16/32 from the HAL envelope.
  - In practice the default test path is I2S / TDM8 depending on project config.
  - **Slave** (external BCLK/FS) is the main tested path.
  - A **master** (self-clocked) path exists; the TDM8 master with `FS_50PCT` (CLC10-generated
    50%-duty FS) was bench-verified on a dsPIC33AK Curiosity board (BCLK/FS = 256, `miss=0`).
    Other master rate/format combinations should still be confirmed on the target board.
- The system-topology model (transactional `configure_system()`, `open()` with
  no role, per-domain framing validation) has been verified with co-clocked A/B,
  94% load, deterministic phase-locked startup, CMSIS single-instance loopback,
  and a dsPIC33AK Curiosity-board TDM8-master smoke test.

## 9. CMSIS-SAI relationship

- A CMSIS-SAI wrapper is a layer **above** this HAL, not part of it.
- `ARM_SAI_*` types must not appear in this HAL core.
- The CMSIS wrapper owns Send/Receive buffer semantics, `tx_underflow` / `rx_overflow`,
  and sample-rate policy.
- This HAL's native diagnostics use `block_deadline_miss_count`, `block_count`, `load`, the
  RX-DMA diagnostics `rx_dma_overrun_count` / `rx_dma_other_irq_count` /
  `rx_dma_last_status`, and the
  framed-transport health counters `err_rov_block_count` / `err_tur_block_count` /
  `err_frm_block_count` / `frmerr_consecutive_blocks` (SPIROV/SPITUR/FRMERR, sampled once per
  RX-block; see "DMA and SPI transport-health diagnostics" above).
- **Naming note:** this HAL's `err_rov_block_count` / `err_tur_block_count` (hardware
  `SPIROV`/`SPITUR`, sampled per RX block) are not the same signal as the CMSIS wrapper's
  `rx_overflow` / `tx_underflow` (its own software buffer-semantics events, one layer up) — the
  names read alike but the two pairs detect different failure modes at different layers.

## 10. SPI/I2S/TDM API reference

`nora_spi_i2s_tdm.h` defines the transport API used by board integration,
multi-leg operation, and the CMSIS-SAI binding.

Block-callback samples use `nora_tdm_slot_t`. Portable consumers must follow
three rules: use the encode/decode/scale accessors, do not treat a slot as a
byte sequence, and fold conversion into the DSP's existing loads and stores.
The dsPIC33AK typedef is transparent, so direct stores appear to work here but
will not remain portable to a backend with a wire-word slot representation.

Source-reader note: `tdm_stream_t` in `nora_spi_i2s_tdm_dspic33ak.c` is a
file-private primary-leg/topology bookkeeping type. It is not a public stream
type and is load-bearing implementation state.

For the complete configuration, lifecycle, callback, and diagnostic reference,
see <https://github.com/sulaolab/nora-hal-dspic33ak-spi-i2s-tdm>.
