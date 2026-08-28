# Type_TY AVAS: the L1 line model, run as cluster carriers

The Type_TY AVAS voice in `src/app/apps/classic/dsp/avas_synth_type_ty.c` is a
**line model**: a fixed coefficient set of sinusoidal components, evaluated with a
clustered-carrier implementation sized for the dsPIC33AK per-sample budget.

This document is the public specification of that engine -- what it computes, how the
implementation differs from the naive form and why, what it costs, and which knobs are
real. The coefficient set itself is a generated artifact (see
[§7](#7-coefficient-tables)); how a coefficient set is produced is outside this
repository.

---

## 1. The model

```
y(t) = SUM_{j=1..185} AMP[j] * cos(2*pi*FRQ[j]*t + PHA[j])
```

That is the whole signal model: no FM, no AM, no per-partial envelope. The only
time-varying element is the on/off gate.

The line set is deliberately **not** a harmonic grid, and that is a structural
property rather than a tuning preference:

- most lines do not sit on an integer multiple of the lowest component;
- what is perceived as a single partial is a group of 6-13 separate lines within about
  +-10 Hz of each other and within 10 dB of each other. Their mutual beating is the
  character of the sound, and one oscillator per grid slot cannot produce it --
  neither AM nor FM is a substitute;
- some grid positions carry no component at all.

A grid model is therefore not a cheaper approximation of this one; it is a different
sound. All 185 lines stay alive in the shipped engine.

## 2. How it is computed: cluster carriers with a decimated envelope

185 oscillators at 48 kHz do not fit. The compiled inner loop is about 25 instructions
plus 3-4 conditional branches per line, so 185 lines is over 4,600 instructions --
roughly 5,700 cycles against the 4,166-cycle per-sample budget at 200 MHz, i.e. about
137 % of the budget for the AVAS voice alone.

The implementation keeps every line and changes the arithmetic. Lines are grouped into
contiguous clusters no wider than 200 Hz (11 clusters for the shipped table). Within
one cluster, **exactly**:

```
SUM_{j in cluster} A_j cos(2 pi f_j t + p_j) = Re{ e^{i 2 pi fc t} * Z(t) },
Z(t) = SUM_{j in cluster} A_j e^{i (2 pi (f_j - fc) t + p_j)}
```

`Z(t)` is band-limited to the cluster half-span (<= 100 Hz), so it does not need to be
evaluated at 48 kHz. It is rebuilt every `AVAS_TYPE_TY_DEC` samples (default 32) and
linearly interpolated in between. Only the 11 carriers run at the full rate.

Two details are load-bearing. Both were established by measurement, and both are easy
to get wrong in a way that band-level metrics do not reveal:

- **The rebuild must evaluate the envelope one block AHEAD** and interpolate towards
  it. Interpolating towards a value already reached delays the envelope by a whole
  block, which by itself drops accuracy from 48.0 dB to 15.3 dB below signal -- while
  band levels stay within 0.04 dB, so only the time waveform shows it. See
  `avas_type_ty_rebuild_envelope()`.
- **The carrier frequency is the cluster's amplitude-weighted centroid**, not its
  geometric middle. That gives the strongest lines the smallest baseband offset and
  therefore the smallest interpolation error. It is set by the table generator.

## 3. Firmware approximations

The firmware cannot evaluate `cos(2*pi*f*t)` exactly. A float32 phase accumulator feeds
`audio_fast_sinf_0_to_2pi()`, a parabolic approximation. Reproducing both
approximations in reference arithmetic and comparing against the ideal model gives, for
the direct 185-oscillator form (which isolates the approximation from the clustering):

| Quantity | Ideal (float64, exact cos) | Firmware equivalent (float32, approximate sine) |
|---|---|---|
| peak | 0.851403 | 0.852968 |
| error rms | -- | **49.2 dB below signal** |
| waveform correlation | -- | **0.999994** |
| level error, the 13 occupied bands | -- | mean **0.003 dB**, max 0.00 dB |
| the 5 line-free bands | empty | **-72.4 dBFS** (the approximate sine's distortion floor) |

So the parabolic sine and float32 phase are sufficient: 185 components
intermodulating still keep the distortion floor at -72 dBFS.

## 4. Cost

Instruction counts from the generated assembly
(`-mcpu=33AK512MPS512 -O3 -ffast-math -ffp-contract=fast`):

| Loop | Instructions per iteration | Iterations | Per sample |
|---|---|---|---|
| carriers (`avas_type_ty_process_carriers`) | 47 | 11 per sample | 517 |
| envelope (`avas_type_ty_eval_cluster` inner) | 40 | 185 / `AVAS_TYPE_TY_DEC` = 32 | 231 |
| cluster post-processing, gate, etc. | -- | -- | about 45 |
| **total** | | | **about 790 instructions** |

A carrier costs 47 instructions because it uses one fast cosine and one fast sine. A
per-carrier rotator -- a complex multiply -- would remove both; see
[§4.1](#41-measured-on-hardware).

Size (`dsPIC33AK512_CLASSIC_SERIAL_UPDATE`):

| Item | Bytes |
|---|---|
| `s_type_ty_l1_line` (flash const) | 2220 |
| `s_type_ty_l1_cluster` (flash const) | 88 |
| `g_avas_type_ty` (RAM) | 1776 |
| `avas_type_ty_synth_process_sample` | 1096 |

RAM is two floats per line (baseband phase and step) plus six floats per cluster.
Amplitudes are read straight from the flash const table; normalisation and gain are
uniform, so they are applied once to the summed output. Baseband frequency offsets are
computed at init rather than stored in flash.

### 4.1 Measured on hardware

B-XTAL, `TDMsum` telemetry, 48 kHz, 32-sample block = a 666.6 us window:

| | `TDMsum` max | share of the window | margin | miss / sat |
|---|---|---|---|---|
| AVAS off | 181.3 us | 27.1 % | 485.3 us | 0 / 0 |
| **Type_TY on** | **487.6 us** | **73.1 %** | **179.0 us** | **0 / 0** |
| difference = Type_TY | **306.3 us** | **45.9 %** | | |
| Type_LB on, for comparison | 392.7 us | 58.9 % | 273.9 us | 0 / 0 |
| difference = Type_LB | 211.4 us | **31.7 %** | | |

Those last two rows are why the two voices are **mutually exclusive at runtime**
([§6](#runtime-exclusivity-hard-refusal)): together they would be 45.9 + 31.7 = 77.6 %,
and with the rest of the chain at 27.1 % that is about 105 % -- a guaranteed overflow.
Refusing one fixes the peak at `max(45.9, 31.7)`.

The overflow of the naive form is gone (margin positive, miss 0, sat 0, no clock
errors), but 45.9 % is well above the ~18 % that was aimed for. The reason is that §4
counts **instructions, not cycles**: 306.3 us / 32 samples = 9.57 us per sample is
about 1,900 cycles at 200 MHz, 2.4x the ~790 instructions, because float multiply,
float add and calls are not one cycle each.

The internal split is unchanged (carriers 517 : envelope 231), so **the carriers
dominate** -- about 1,300 cycles (~31 %) against the envelope's ~600 (~14 %).
Consequences:

- raising `AVAS_TYPE_TY_DEC` from 32 to 64 saves only about 7 % and costs
  48.0 -> 36.5 dB of accuracy. It is no longer the main term.
- the remaining real reduction is **removing the fast sine/cosine from the carrier
  loop**: replacing 22 approximate sine/cosine evaluations per sample with a
  per-carrier rotator (complex multiply: 4 multiplies, 2 adds) would cut the carrier
  term to roughly a third or a quarter. Not implemented.

### 4.2 Accuracy versus decimation

2 s at 48 kHz, float32, the same approximate sine the firmware uses:

| Form | Full-rate oscillators | Error below signal | Level error, 13 occupied bands | Line-free bands |
|---|---|---|---|---|
| direct 185, the form this replaced | 185 | 49.2 dB | mean 0.003 / max 0.00 dB | -72.4 dBFS |
| clustered, D=8 | 11 | 54.0 dB | mean 0.004 / max 0.01 dB | -72.5 dBFS |
| clustered, D=16 | 11 | 53.1 dB | mean 0.008 / max 0.04 dB | -71.7 dBFS |
| **clustered, D=32 (default)** | **11** | **48.0 dB** | **mean 0.041 / max 0.18 dB** | **-71.5 dBFS** |
| clustered, D=64 | 11 | 36.5 dB | mean 0.184 / max 0.74 dB | -71.3 dBFS |
| D=16, no lookahead (the bug) | 11 | 21.3 dB | mean 0.008 dB | -71.7 dBFS |
| D=32, no lookahead (the bug) | 11 | 15.3 dB | mean 0.042 dB | -71.5 dBFS |

D=32 is parity with the direct form at about one seventh of the cost. The two
no-lookahead rows are the cost of the delayed envelope described in §2 -- invisible in
the band-level columns, which is exactly why it is called out.

## 5. The load knob is `AVAS_TYPE_TY_DEC`, not the line count

Truncating the line count was removed as a build option. It does not buy load -- the
carriers dominate, not the lines -- and it changes the sound:

| Reduction | N=32 | N=48 | Bands left silent |
|---|---|---|---|
| keep the strongest N lines | mean **64.4 dB** error | mean 48.9 dB | 7 of 13 -> 5 of 13 |
| optimal per-band quota | mean **5.2 dB**, max 8.8 dB | mean 4.6 dB | 0 of 13 |

The line count corresponding to an 18 % budget is around 25 lines, and at that point
the intra-cluster beating that §1 identifies as the character of the sound is gone.
Clustering reaches the same load at 0.04 dB. Lower the load with `AVAS_TYPE_TY_DEC`;
`AVAS_TYPE_TY_L1_LINES` no longer exists.

## 6. Using it

```
# Build. Both Type_TY and Type_LB are compiled in; no -Define is needed.
pwsh ./buildtools/build.ps1 -Full
```

**Both engines are in the same image and the choice is made at runtime** (the AVAS
block in `src/app/app_specific_config_defs.h` defines both). The build-time
"exactly one" check became "at least one". To build only one, comment out the other
`#define` there -- `-Define` cannot remove a definition, and passing
`-Define ENA_AVAS_TYPE_TY_SYNTH=0` **enables** it: these switches are tested with
`defined()`, so the value is never read, and the preprocessor cannot tell an empty
expansion from `0`, which is why this misuse cannot be caught in code. Pass the bare
name.

### Starting each voice

| Route | Plays |
|---|---|
| hotkey `a` (lower case) | **Type_TY** |
| hotkey `A` (upper case) | **Type_LB** |
| console command `*cy00` | **Type_TY** |
| button 1 (short press = mute), **long press** | **alternates on each start** (Type_TY -> Type_LB -> Type_TY ...) |

A long press starts, another long press stops, and stopping stops whichever voice is
sounding. A start that is refused (the other voice is still fading) **does not consume
the alternation**: the next long press retries the same engine rather than silently
skipping it.

### Pitch trim (POT)

The L1 table is a fixed-pitch coefficient set, so the engine has no pitch of its own to
track. The POT is a **trim, not a transposition**, and it is the only control (there are
no keys):

| Action | Effect |
|---|---|
| POT fully counter-clockwise | **default pitch (0 cent)**; the bottom 2 % snaps to exactly 0 |
| clockwise | pitch rises; full scale = **+`AVAS_TYPE_TY_POT_TOP_CENT`** (default +200 cent) |
| `?cs` | reports the current value (`AVAS(TYPE_TY) pitch = +15.0 cent (x1.00871)`) |

The mapping is one-sided so that the reference pitch sits at a mechanical end stop and
can be found without looking -- centring a detent-less knob on 0 is hard -- and so that
the whole travel is useful. That end is also below `ENG_SYNTH_POT_ACTIVE_VAL`, the
engine-synth off zone, which is consistent: the knob's rest position adds nothing.
Design: [`avas_pitch_pot_design.md`](avas_pitch_pot_design.md).

The ratio `r` is common to all components -- both the 11 carriers and the 185 baseband
oscillators -- so it is an exact pitch shift of the whole model. **Runtime cost does not
change**: the hot loop is untouched, and `avas_type_ty_set_steps()` rewrites the step
tables only when the knob actually moves. Rewrites happen **only at an envelope-rebuild
boundary** -- mid-rebuild the baseband phases are partway along their old slopes, so
swapping there would leave some of the 185 lines at the old pitch. Worst-case latency is
`AVAS_TYPE_TY_DEC` samples = **0.67 ms**.

Accuracy cost of the range: `r` widens each cluster's baseband bandwidth, and the
envelope's linear-interpolation error grows roughly as `r^2`. At +200 cent
(`r = 1.1225`) that is +26 %, i.e. about **1 dB** against the 48.0 dB of §4.2 --
negligible. **An octave (`r = 2`) is 4x the error, 6 dB**, which is the point at which
`AVAS_TYPE_TY_DEC` should be lowered and traded against load.

**Stopping resets the pitch to 0 cent**; a session does not inherit the previous one's
detune. The reset is applied **after the release fade has finished**, so the decaying
tail does not slide -- only the next start changes. The console prints
`AVAS(TYPE_TY) pitch reset: -30.0 -> +0.0 cent (next start)`, and only when the value
was non-zero. Ownership is also released at startup, so **a knob left at full scale
still starts at 0 cent**: the trim becomes writable only once the knob moves.

Type_LB has the same control and the same 0...+200 cent range. The POT is shared with
the engine synth, and the engine synth and AVAS are mutually exclusive in
`fx_domain_48k`, which is why the owner of the knob has to be decided at runtime.

### Runtime exclusivity (hard refusal)

**While one voice is sounding the other is refused**, release fade included, with the
reason printed:

```
 AVAS(Type_LB): rejected -- TYPE_TY is still sounding (wait for it to fade).
```

The reason is load: 45.9 % + 31.7 % ([§4.1](#41-measured-on-hardware)) is 77.6 %, about
105 % with the rest of the chain. Refusing fixes the peak at
`max(Type_TY, Type_LB)`. Off is always accepted -- there is nothing to protect.

**The handover wait after switching off is about 3 s** (`AVAS_*_GATE_EPS` = -50 dB).
The gate falls as a one-pole with tau = 0.5 s, so its tail is exponential and the
"finished" threshold *is* the wait:

| "finished" threshold | time to reach | Note |
|---|---|---|
| 1e-6 | 13.8 tau = **6.9 s** | a request 6.2 s after off was still refused, load still 73 % |
| **0.0031623 = -50 dB (current)** | 5.8 tau = **2.9 s** | the ramp shape is unchanged; only the -50 dB tail is cut |

Measured on hardware (`a` off, then `A` twice): a request **2.23 s** after off is
refused, **3.47 s** is accepted -- consistent with the computed 2.9 s once telemetry
granularity and the main-loop period are included.

The audible ramp-down is unchanged. What changed is where rendering stops: the part cut
off is 50 dB below the voice's own level, about -56 dBFS overall. The gate is snapped to
0 at that point so that the stored value and the output agree.

**This wait is also a period during which the load stays high** -- the early-out only
applies below the threshold -- so raising the threshold shortens the wait *and* frees
the load sooner.

The decision uses each engine's `app_avas_*_is_active()` (the gate has not fully
decayed). The on/off toggle stays a **per-engine static latch** and is not derived from
`is_active()`: re-enabling during a release is a normal supported path -- it skips the
phase reset to avoid a click -- and deriving from the gate would block it.

`fx_domain_48k` calls both voices every sample. The idle one returns `0.0f` immediately
when fully gated off, so it costs essentially nothing.

Enabling resets the phases to the table's values, so every start produces the same
waveform. A re-enable during release does not reset, to avoid a click.

Tuning points:

| Macro | Location | Default | Meaning |
|---|---|---|---|
| `AVAS_TYPE_TY_DEC` | `avas_synth_type_ty.h` | 32 | **the load knob.** Envelope rebuild interval in samples; see §4.2. Outside 2..64 is an `#error` |
| `AVAS_TYPE_TY_TONE_GAIN_DB` | `avas_synth_type_ty.c` | 0.0 dB | 0 dB is the coefficient set's own normalised level |
| `AVAS_TYPE_TY_GATE_ATTACK_S` | `avas_synth_type_ty.c` | 4.000 s | gate attack |
| `AVAS_TYPE_TY_GATE_EPS` | `avas_synth_type_ty.c` | 0.0031623 (-50 dB) | **the release-finished threshold, i.e. the handover wait.** The ramp itself is unaffected. Keep equal to Type_LB's `AVAS_TYPE_LB_GATE_EPS` |
| cluster max span | table generator argument | 200.0 Hz | narrower means more full-rate carriers; wider widens the envelope bandwidth and increases interpolation error |

`AVAS_TYPE_TY_L1_NORM` normalises the **60-second** peak of the full sum to 0.9, not the
peak over a shorter window: the sum is quasi-periodic, so its peak keeps growing outside
any short window, and normalising on a short-window peak clips once the engine is left
running.

## 7. Coefficient tables

`avas_synth_type_ty_tables.h` is **generated -- never hand-edit it.** The header records
the cluster inventory that goes with the line list, and the table is stored
frequency-ascending so that each cluster is a contiguous run and the firmware needs no
per-line cluster index. `avas_synth_type_lb_tables.h` and
`avas_synth_type_lb_noise_tables.h` are the same kind of artifact for the Type_LB voice.

**The coefficient generators are not part of this repository.** The tables are the
shipped artifact: everything the firmware needs to run the engine, and everything this
document describes, is in the headers. Deriving a new coefficient set is a separate
activity from building or using this firmware.

## 8. Known limitations

- **The load target is not met**: 45.9 % measured against the ~18 % aimed for
  ([§4.1](#41-measured-on-hardware)). The overflow is gone and margin is positive with
  miss 0, but reaching the target needs the carrier rotator described there.
- **The button routes are not hardware-verified.** Alternating playback on a mute
  (button 1) long press, and button 3's long press *not* starting AVAS, are code-only
  changes; they need a physical press. Hotkeys `a`/`A`, `*cy00`, refusal in both
  directions and the load figures are verified on hardware.
- **There is no pitch sweep.** The table is a fixed-pitch coefficient set and loops
  stationary. Speed-linked operation -- the actual point of an AVAS -- requires making
  each line time-varying.
- **Type_TY has no noise layer**, by choice. The Type_LB voice adds a shaped noise bank
  (`avas_synth_type_lb_noise_tables.h`).
