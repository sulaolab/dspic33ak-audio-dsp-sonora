# nora_pwm_audio -- sources

PCM-to-PWM audio output. The application hands finished Q1.31 blocks to
`nora_pwm_audio_write()`; generators, DMA, pins and the power stage stay in
one fixed board backend per build. Blocks are planar: stereo is two separate
`int32_t` arrays (`left_q31`, `right_q31`) of 32 frames each, not one
interleaved array.

## Files

| File | Role | Portable |
|---|---|---|
| `nora_pwm_audio.h` | public API: five functions | yes |
| `nora_pwm_audio.c` | core: checks, block FIFO, counters, fault latch | yes |
| `nora_pwm_audio_modulator.{h,c}` | Q1.31 -> duty, pure | yes |
| `nora_pwm_audio_backend.h` | core <-> board backend contract (internal) | yes |
| `nora_pwm_audio_conf.h_example` | project config template | yes |

None of these includes `xc.h` or names a PG, DMA channel or pin. The project
supplies `nora_pwm_audio_conf.h` (from the template) and exactly one board
backend that implements `nora_pwm_audio_backend.h`.

## Lifecycle

| State | Entered by | `write()` returns | Output |
|---|---|---|---|
| uninitialised | reset, or an `init()` the backend rejects | `NOT_READY` | not driven by this module |
| stopped | `init()` OK, `stop()`, or a failed `start()` | `NOT_READY` | STOP_SAFE |
| running | `start()` OK | `OK` or `BUSY` | audio, or neutral while not primed |
| fault | `core_fault()` from the backend | `FAULT` | EMERGENCY_SAFE |

`init()` first checks the configuration against the build. If that fails
(`INVALID_ARGUMENT`, or `UNSUPPORTED` for a rate, block length or channel
count the build does not have), nothing changes: a running output keeps
running and a latched fault stays latched. Otherwise it stops a running
output and calls the backend; if the backend rejects it, the module is
uninitialised and a fault latched before stays latched. `UNSUPPORTED` alone
does not say which check failed; call `stop()` first when a known state is
needed. A successful `init()` clears the counters and any latched fault; it
is the only way out of the fault state. `start()` from the fault state
returns `FAULT`. `stop()` is repeatable and keeps the counters.

## Core behaviour

- One FIFO slot holds every channel of one 32-frame block, so stereo is
  accepted, dropped or played as a whole. N slots buffer N-1 blocks.
- The board backend consumes complete blocks from the core FIFO and
  refills its DMA buffers. The number of blocks consumed per DMA refill
  depends on the backend implementation. When the core is not primed (until
  `NORA_PWM_AUDIO_PRIME_BLOCKS` are queued, after start and again after an
  underrun), it supplies no data and the backend outputs neutral duty.
- `underrun_blocks` counts the blocks the core could not supply after the
  first block accepted since `start()`: every `core_take()` that returned
  false from then on, including the ones while re-priming after an
  underrun. Start-up silence before the first accepted block is not counted,
  and neither is neutral output that a backend inserts on its own (for
  example while filling a local buffer); such output is the backend's to
  report. A backend that buffers blocks of its own polls
  `nora_pwm_audio_core_ready()`, which counts nothing, before it takes.
- A fault is latched per category by the backend after it has reached
  EMERGENCY_SAFE. `write()`/`start()` then return `FAULT` until `init()`.

## Safety is the backend's

This folder contains no backend. STOP_SAFE (the no-output state after a
normal stop) and EMERGENCY_SAFE (reached from a fault ISR without waits)
are defined by `nora_pwm_audio_backend.h` but implemented by the board
backend: which pins are driven, in what order the power stage is disabled,
and how long it takes. The core only calls the backend and latches the
fault it reports. Whether a given board is actually safe is a property of
that board's backend and has to be verified on that board.

## Modulator

`nora_pwm_audio_modulate_q31()` maps Q1.31 to duty as follows:
`INT32_MIN -> min_duty`, `0 -> period/2`, `INT32_MAX -> max_duty`, negative
side normalised by 2^31 and positive side by 2^31-1, round to nearest with
ties away from centre. The reference implementation uses a 32x32-to-64-bit
multiply without 64-bit division. The dsPIC33CK variant also provides a
32-bit-only fast path for smaller duty ranges (`period/2 - min_duty <=
0xffff`), preserving bit-exact results.
The host test compares it against an independent exact-rational reference.

## Host test

The host test builds the firmware `.c` files unchanged against a fake backend
(mono/stereo, prime 1/2) and checks the modulator end points, centre,
rounding, monotonicity, symmetry and clamp, and the core's
BUSY/whole-block/underrun/prime/ready/stop/fault contract. It is not part
of the module itself; a published repository of this module carries it under
`tests/`.

## Not done here

On-hardware validation is not complete: stereo whole-block accept/drop and
simultaneous neutral on L/R, the start/stop and fault-to-safe sequences on
a scope, long-run BUSY/underrun behaviour, current, temperature, ISR
worst-case time and RAM. Also open: the MIC4607 stop sequence (the legacy
path never stopped that output) and level matching against the legacy
`float -> duty` path.
