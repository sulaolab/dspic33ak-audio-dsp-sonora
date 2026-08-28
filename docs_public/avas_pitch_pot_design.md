# AVAS pitch trim on the POT -- design (Type_TY and Type_LB)

Scope: `dspic33ak_audio_dsp`, Classic profiles on the AK512 board. **The POT is the only
control**; there are no key bindings. The engine-side view of the trim, including its
accuracy cost, is in [`avas_type_ty_l1_line_model.md`](avas_type_ty_l1_line_model.md).

---

## 1. The constraint this design exists for

The POT is already read from three places, and one of them is mutually exclusive with
AVAS:

| Reader | Period | Purpose | Conditioning |
|---|---|---|---|
| `main.c`, main loop | main loop | RGB LED ramp | integer EMA (shift 4) plus 80-on / 40-off hysteresis |
| `classic_demo_app.c` | **400 ms** | **engine-synth on/off decision** | `> ENG_SYNTH_POT_ACTIVE_VAL` (50) for 3 consecutive counts calls `app_engine_synth_enable(true)` |
| `engine_synth.c` | per render block | rpm command | `rpm = adc * 1.5` |

And in `fx_domain_48k.c` the engine synth and AVAS are mutually exclusive:

```c
if( app_engine_synth_is_enable() ) { y += engine; }
else                              { y += avas_type_ty; y += avas_type_lb; }
```

So **turning the POT past 50 enables the engine synth about 1.2 s later (3 counts) and
AVAS stops being called at all.** Nearly the whole useful travel of the knob is above
50, so a naive "POT drives pitch" addition would mean *the AVAS silently turns into
engine noise the moment the knob is touched.*

That exclusivity is not new work; it is the existing load-driven arrangement. Type_TY
alone is about 46 % of the per-sample budget, which leaves no room to run the engine
synth at the same time.

The design question is therefore **who owns the knob at runtime**, not how to scale it.

## 2. Options considered

### A (adopted): whichever voice is sounding owns the POT

| AVAS (Type_TY / Type_LB) state | Meaning of the POT |
|---|---|
| either voice **on**, release fade included | **pitch trim of the sounding voice**, and the engine synth's POT start is suppressed |
| off | unchanged: **engine-synth throttle** |

- It extends the runtime-exclusivity model already in place and **does not change the
  build configuration** -- one image still does both, so no reflash is needed to
  compare them.
- Two changes only:
  1. the 400 ms engine-start decision in `classic_demo_app.c` gains a "neither AVAS is
     active" condition (`app_avas_*_is_active()` already exists);
  2. the POT-to-pitch sampler sits in `classic_controls_process()` on a **100 ms**
     period -- not in the 400 ms block, where 2.5 updates per second feels heavy.
- Side effect: the engine synth cannot be started from the POT while AVAS is sounding.
  That was **already** the case, since the engine's audio cannot be heard during AVAS,
  so nothing is actually lost.

### B: drop `ENA_ENGINE_SYNTH` in an AVAS-specific configuration

Simplest possible -- no arbitration code at all -- but the engine-synth demo disappears
from that image, and `-Define` cannot remove a definition, so it means editing
`app_specific_config_defs.h` and building a second image, i.e. a reflash for every
comparison. On the 96 kHz profile `ENA_ENGINE_SYNTH` is undefined to begin with, so
there A and B are equivalent (the POT is free from the start).

### C (rejected): treat the POT as an offset added to a key-set value

Two input sources writing one value makes `?cs` disagree with what is heard.

## 3. Mapping (identical for both voices)

| Item | Value | Reason |
|---|---|---|
| mapping | **one-sided.** Fully counter-clockwise = 0 cent (the engine's own pitch), rising clockwise, full scale **+`AVAS_*_POT_TOP_CENT`** (default +200 cent) | the reference pitch is at a **mechanical end stop**, so it can be found without looking; centring a detent-less knob on 0 is hard. The whole travel is useful. The low end is also below `ENG_SYNTH_POT_ACTIVE_VAL`, i.e. in the engine-synth off zone, which agrees with "rest position adds nothing" |
| bottom snap | the bottom **82 counts (2 %) round to 0 cent** | makes "knob at rest = exactly no trim" true. It is below the deadband anyway, so no resolution is lost |
| smoothing | integer EMA, shift 4 | the same idiom (and constants) as the LED ramp in `main.c` |
| deadband | **24 counts** (about 1.2 cent over a 200 cent travel) | ADC jitter must not rewrite the pitch or print to the console |
| sample period | **100 ms** | verified to feel right on hardware |
| console output | one line, **300 ms after the sweep stops** | printing during a sweep floods even at 230400 baud. `?cs` reads the value at any time |
| clamp | +-`AVAS_*_PITCH_LIMIT_CENT` (default +-200 cent) | the knob only produces 0...+200; the clamp protects against other callers |

The bottom snap is **discontinuous** and deliberately left so: `pot <= 82` is 0 while
`pot = 83` is `83 x 200 / 4095` = +4.1 cent, a 4.1 cent step at the boundary (about 3.5x
the deadband). It is not audible in use. To make it continuous instead:
`cent = (pot - 82) * top / (4095 - 82)`.

## 4. How the trim is applied

Neither engine gains a new signal path:

| | Application | Latency | Extra load |
|---|---|---|---|
| Type_TY | rebuild the step tables (11 carriers + 185 lines) from the const table. Request flag, applied **at an envelope-rebuild boundary** -- swapping mid-rebuild would leave only some lines at the new pitch | <= `AVAS_TYPE_TY_DEC` = **0.67 ms** | hot loop unchanged; the rewrite happens only when the knob moves |
| Type_LB | **the same** (7 carriers + 264 lines = 271 steps, `avas_type_lb_set_steps()`) | <= `AVAS_TYPE_LB_DEC` = **0.67 ms** | the same: 271 multiplies at the POT's 10 Hz |

Both voices are line models, so a pitch ratio `r` means rewriting every step; there is
no single base-frequency register to scale. That is why both use the same
request-flag-plus-rebuild-boundary mechanism rather than a per-sample multiply.

**The noise bank does not follow the pitch.** The Type_LB noise bank's band centre
frequencies are fixed band edges, not harmonics of the tone, so the wind does not
transpose with the voice. That is deliberate.

Ownership carries **no held flag**. Releasing it re-seeds the EMA and re-seats the
reference position at the knob's current reading, which makes "the knob must actually
move before it can write again" fall out naturally rather than being enforced by a
flag that could disagree with the reference position.

The engine-synth guard tests `app_avas_*_is_active()` -- the gate, including the release
fade, not the UI on/off flag. Returning the POT to the engine mid-fade would cut the
tail.

## 5. Where each piece lives

| Change | Location |
|---|---|
| `AVAS_TYPE_TY_POT_TOP_CENT` / `AVAS_TYPE_LB_POT_TOP_CENT` (200.0f), clamp +-200 cent | `dsp/avas_synth_type_ty.h` / `dsp/avas_synth_type_lb.h` |
| POT sampler (100 ms, EMA, deadband, bottom snap, one line once settled, routed to the sounding voice) | `classic_controls.c`, `local_avas_pot_process()` |
| ownership release on AVAS start/stop | `classic_controls.c`, `local_avas_pot_release()` |
| engine synth not started from the POT while either AVAS is active | `classic_demo_app.c`, the 400 ms block |
| pitch reset to 0 cent on stop, applied once silent | each engine's `app_avas_*_set_enable(false)` |
| Type_LB step rewrite | `dsp/avas_synth_type_lb.c`, `avas_type_lb_set_steps()` |

## 6. Why there are no keys

`[` / `]` were implemented once, complete with a 100 ms throttle and soft takeover (the
POT takes ownership past the deadband, a key press takes it back), and they worked. They
were **removed**:

- the POT is an **absolute** input, so coexisting with keys requires "whoever moved last
  owns it" arbitration. Without it, a key-set value is overwritten by the next POT
  sample and the keys look dead;
- that complexity would be carried for a *second* path writing the *same* value;
- `?cs` still reports the value, so nothing is lost in observability.

Consequence: **there is no way to set the pitch from an automated test**, since the POT
is a physical input. If that is needed, a console verb (`*cp <cent>`) is the
straightforward addition. It is not implemented.
