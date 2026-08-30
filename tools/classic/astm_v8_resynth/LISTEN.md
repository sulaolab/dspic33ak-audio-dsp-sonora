# Listening index — astm V8 resynthesis

Everything below is in `out/` next to this file. `out/` is gitignored; to
regenerate the whole set from the recordings:

The source recordings are **not in this repository**. Set `ASTM_V8_WAVDIR` to the
directory holding them first; there is deliberately no default, because a default
is a path on one machine:

```powershell
$env:ASTM_V8_WAVDIR = "<your recordings directory>"
python a04_ordertrack.py astm_v8_vantage_all.wav --rpm-lo 1400 --rpm-hi 7600 --nperseg 8192 --hop 1024
python a04_ordertrack.py astm_tmp_001.wav  --rpm-lo 1400 --rpm-hi 7600 --nperseg 8192 --hop 1024
python a04_ordertrack.py astm_accl_001.wav --rpm-lo 1400 --rpm-hi 7600 --nperseg 8192 --hop 1024
python a04_ordertrack.py astm_stable.wav   --rpm-lo 1400 --rpm-hi 7600 --nperseg 8192 --hop 1024
python a15_ridge.py astm_v8_vantage_all.wav ; python a15_ridge.py astm_tmp_001.wav
python a15_ridge.py astm_accl_001.wav ; python a15_ridge.py astm_stable.wav
python a16_build_tables_v3.py
python a17_demos.py
python a19_starter.py
python a20_portable_synth.py
python a21_table_size.py
python a22_idle_normal_model.py
python a24_startup_sequence.py
python a25_idle_wobble.py
python a26_idle_soft.py          # needs demo_full_range.wav from a17
python a28_chord_cause.py
python a29_chord_fix.py
python a30_idle_noise_sweep.py
python a31_formant.py
python a32_clatter.py
python a33_adopt.py
python gen_engine_v8_tables.py   # writes src/app/apps/classic/dsp/engine_v8_tables.h
python a34_impl_check.py         # group J + out/a34_impl_check.txt
python a35_ladder.py             # group K
```

## Listen to this group — it is the open question

**K — where did the sound leave `demo_full_range.wav`?** The firmware failed its
listening test on the board, and not partly: 「一部がダメ、ではなくて **wav とは
別物**」, against `demo_full_range.wav` as 「完成イメージ」. Group J measures the
firmware against the render that was approved in §38 and passes it at 1.38 dB, so
group J was asking the wrong question. **`demo_full_range.wav` is older than four
decisions that were each accepted on their own render and never compared to it**
(a17 built it from `tables_v3`, jitter 0.005, no drift, no mask), and on the board
a fifth thing was wrong: an unfiltered POT read held the masking noise on
permanently (§40.2, measured gate 975/1000 where the design wants 0).

Every file is rms-matched to D0 and on D0's own trajectory (19.0 s, 750…7000 rpm),
so any pair is a fair A/B. **Listen down the list in order and stop where it stops
sounding like the reference** — that is the answer this group exists to get.

| file | rung | oct rms vs D0 | what changed |
|---|---|---|---|
| `ladder_d0.wav` | D0 | 0.00 dB | **the reference** — `tables_v3`, jitter 0.005, no drift, no mask |
| `ladder_d1.wav` | + `formant_strong` | 0.04 dB | §34/36. Spectrally invisible, but the idle's cycle-locked clatter doubles (`fold` 5.0 → 13.4) |
| `ladder_d2.wav` | + jitter 0.020 | 0.10 dB | §36 |
| `ladder_d3.wav` | + drift ±40 rpm | 0.18 dB | §25 |
| `ladder_d4.wav` | + mask ×6 | **4.26 dB** | §38. The biggest single step by far, and the reference has none of it |
| `ladder_d5.wav` | the firmware | 4.45 dB | + int16 tables / LCG / IIR / constant norm (all of group J) |
| `ladder_d6.wav` | **the firmware as the board played it** | 4.52 dB | + the §40.2 defect: mask stuck open, pitch FM'd by the ADC |

`ladder_d6.wav` is the one to check first: if that is what was heard on the board,
the defect is the whole verdict and the ladder above it is a design question, not a
regression. If D6 and D5 sound the same and both are 「別物」, the answer is in
D1/D4 and the mask or the formant tables have to be reopened.

Part C is the same ladder as the board's live `*cy` subcodes (§40.1), so the two
experiments are one: `ladder_s40` bare wave, `s47` + jitter, `s4f` + noise,
`s5f` + drift, `s7f` all (= `d5`). `*cy 41`/`43` have no file — a throttle and its
deadband only exist where there is an ADC.

## Group J — answered by measurement; the question moved to K

**J — does the firmware still sound like the render that was approved?** The
sound design is FIXED (§38) and the C is written (`src/app/apps/classic/dsp/
engine_v8.c` + `engine_v8_tables.h`). `a34_impl_check.py` renders with **only what
the C can do** — int16 tables, one shared noise shape, a constant normalisation
instead of a per-block `std()`, a 2nd-order Butterworth instead of a brick-wall
FFT filter, an LCG instead of `np.random` — and this group is the A/B that says
whether those substitutions are audible.

Read the chain the right way round. `impl_ref_*` is the approved FFT prototype;
`impl_por_*` is the portable structure, which **already passed** its own listening
test (group 7, `ab_portable_tuned.wav`). So the firmware's own question is
`impl_low1024` **vs `impl_por`**, and the gap from there to `impl_ref` is one the
owner accepted earlier.

| file | what | note |
|---|---|---|
| `impl_ref_mask1200_x6.wav` | the approved FFT render, re-rendered here | the far reference |
| `impl_por_mask1200_x6.wav` | the portable prototype (group 7, already passed) | **the reference this group is judged against** |
| `impl_low1024_mask1200_x6.wav` | **what the firmware does** | the shipping candidate |
| `impl_flat256_mask1200_x6.wav` | 256 points at every rpm instead of 1024 below 2 krpm | 7.5 kB cheaper, and the clatter collapses (imp −8.55) — the A/B that pays for the schedule |
| `impl_idle_900.wav` | the firmware at the settled idle, 8 s | パタパタ and the ±40 rpm wobble |
| `impl_roll12_mask1200_x6.wav` | firmware + §20's 12 dB/oct noise rolloff | rejected by measurement (4.16 dB); here to hear why |
| `impl_idle_900_roll12.wav` | the same at the idle | |

All rms-matched to the FFT reference, same trajectory, same seed. The numbers are
in `out/a34_impl_check.txt` and §39: firmware vs portable **1.38 dB** octave rms,
idle imp **+0.06**, idle fold **+0.26** — i.e. the clatter survived the port, and
what is left is spread thinly across the octave bands rather than concentrated in
the one band that would be a defect.

One caution while listening to the idle: 「アイドリング録音は録音が良くなくてノイズ
が高い」 — do not judge the idle by how much hiss there is. Judge it by the
パタパタ (cycle-locked, §34) and the slow wobble.

## Group I — answered; kept for the fix descriptions

**I — the chord that is left on the way down.** The table set is settled:
`formant_strong` + jitter 0.020 is adopted (group H). What is left is the chord on
the **falling** side, and §36 measures why it is there — the `noiseup` gate is
proportional to |d rpm/dt|, and this trajectory falls at 2171 rpm/s where it rose
at 8656, so the descent gets **mean gate 0.54 where the rise gets 0.91, and for
2.6× as long**. Scale 1200 closes that; the settled idle is untouched at any
scale (slew 0). `adopt_base.wav` is the adopted render with no masking at all —
the reference for this group.

| file | what | note |
|---|---|---|
| `adopt_base.wav` | adopted tables + jitter 0.020, no masking | the reference |
| `adopt_mask4000.wav` | + band-limited noiseup on **today's** gate | the descent is only half masked |
| `adopt_mask1200.wav` | the same on a gate that reaches 1.0 on the descent | **the fix the measurement points at** |
| `adopt_mask1200_full.wav` | the same, extra noise *not* band-limited | is leaving the clatter band clear worth it? |
| `adopt_mask1200_x6.wav` | the same, ×6 instead of ×4 | more masking |
| `adopt_maskfall.wav` | full masking while decelerating only, today's scale while accelerating | is the descent the only place that needs it? |
| `adopt_maskcreep.wav` | mask1200 plus a 0.5 floor while the speed still moves below 2125 rpm | covers the last 1.63 s slow settle, which no slew gate reaches; still 0 at the settled idle |
| `fall_3000_900_base.wav` | 3000 → 900 rpm overrun on its own | judge the descent without the rise in the way |
| `fall_3000_900_mask1200.wav` | the same with the re-scaled masking | |

All rms-matched to `adopt_base`, same trajectory, same seed. The masking costs
**no** clatter — `adopt_base` / `mask1200` / `mask1200_x6` all measure 16.01/9.88
(§34's imp/fold), identical because the gate is 0 in the measurement window.

Note the limitation while listening: a real overrun has no combustion, so the
descent is not the rise played backwards, and nothing measured covers a
closed-throttle descent — up and down share the same tables here. If it still
sounds wrong with the masking fixed, that needs a recording (§36).

## Group G — answered; kept for the fix descriptions

**G — the chord, second attempt.** Group E's fix was rejected: making the timbre
*move* does not remove the chord. The measurement says why — this engine's
spectral envelope is fixed in **Hz** (4.76 dB rms disagreement between measured
bins aligned by Hz, vs 7.96 dB aligned by order), so transposing one waveform
drags the exhaust/body resonances down with the pitch. §33.

All of these are the **accepted 3000 rpm flare, engine only, no starter**, same
trajectory and same seed, so any difference you hear is the fix and nothing else.
`startup_3000_jit010.wav` is the reference point (today's jitter).

| file | fix | cost on target |
|---|---|---|
| `startup_3000_jit010.wav` | today | — |
| `startup_3000_jit020.wav` | jitter ×2 | free |
| `startup_3000_jit030.wav` | jitter ×3 | free |
| `startup_3000_jit045.wav` | jitter ×4.5 | free |
| `startup_3000_formant.wav` | envelope held in Hz below 2125 rpm (drag 1.00 → 0.70; the car is 0.57) | ~2 kB ROM |
| `startup_3000_formant_strong.wav` | the same, pushed as far as it goes (drag 0.60) | ~2 kB ROM |
| `startup_3000_formant_jit020.wav` | formant + jitter ×2 | ~2 kB ROM |
| `startup_3000_detune.wav` | two wavetable readers at ±0.4 % speed, summed | one extra read |
| `startup_3000_noiseup.wav` | residual noise ×4 while the speed changes fast | a runtime gain |

Round-5 verdicts on this group: `noiseup` **least chordy**; `detune` good on the
rise but thins the パタパタ in the steady part; `formant_jit020`'s tail has the
**most authentic パタパタ** so far. Group H follows from those three.

`formant` also brightens the *already accepted* idle by about +5 dB above 40 Hz
(it is no longer rolling the top off by transposition) — and §34 shows that
brightening is **cycle-locked clatter, not hiss**, which is very likely what made
the tail sound real. `idle_900_formant.wav` / `idle_900_formant_strong.wav` vs
`idle_900_noise+0.wav` is still the A/B that decides the idle.

## Answered

| # | file | verdict |
|---|---|---|
| I | `adopt_*`, `fall_3000_900_*` | **`adopt_mask1200_x6` adopted**, and the idle stays `idle_900_noise+0` — the sound design is FIXED here (§38) and the C port follows in group J |
| H | `startup_3000_{noiseup_*,detune_*,formant_noiseup*,formant_strong_jit020}` | **`formant_strong_jit020` adopted** = the low-rpm table set (§36). `noiseup_lowband` less chordy, `noiseup_jit020` better again, `formant_noiseup` less chordy with very good パタパタ, `+jit020` better still — nothing rejected, and the remaining chord is on the **descent** → group I |
| G | `startup_3000_{jit*,formant*,detune,noiseup}` | `noiseup` least chordy; `detune` thins the パタパタ once settled; `formant_jit020`'s tail the most authentic パタパタ → group H |
| D | `chord_rise_inside` / `chord_rise_cross` / `_cross_slow` | **inside = good, both crossings = chordy** → diagnosis of §29 confirmed; slowing the crossing does not help |
| E | `chord_cross_{base,trend,borrow,jitter}`, `startup_3000_borrow` | **`borrow` rejected** (still chordy); `_jitter` the only one that maybe helped → group G |
| F | `idle_900_noise+0…+23` | **+0** — no audible difference up to +6, so the measured ratio stands |
| A | `idle_tail_lifted.wav`, `idle_extrap_overrun.wav` | good; the head noise is fixed (50 ms fade), re-rendered |
| A′ | `idle_extrap_900` vs `idle_measured_900` | **measured preferred** → superseded by group F |
| B | `idle_900_drift20/40/60`, `_drift40_rough` | **`drift40_rough` is the candidate** |
| C | `startup_2200/2600/3000/3400rpm` | **3000 rpm accepted** — chord character went to groups D/E |

`idle_normal_{800,900,1000}[_wobble].wav` (§22) are the earlier set: 900 rpm was
picked from them, and `startup_{1300,1600,1900}rpm.wav` were judged too low.
`rise_base/fluct/jitter/fluct_ease.wav` and `rise_startup3000_fluct.wav` are
`a27`'s discarded hypothesis (§29) — no need to listen unless curious.

## Already passed — kept for reference

| # | file | what it judged | notes |
|---|---|---|---|
| 1 | `demo_full_range.wav` | the whole product: 750 → 7000 → 750 rpm, 18.9 s | **PASS** ("a strict superset of what is implemented today"); the idle at both ends is the extrapolated bin, superseded by group A |
| 2 | `ab_accl_ref.wav` then `ab_accl_syn.wav` | does it accelerate like the real car | same RPM trajectory (2477 → 6301 rpm, gear changes), level-matched |
| 3 | `demo_accl_style.wav` | the same as a sound, without the reference next to it | |
| 4 | `demo_idle_notilt.wav` vs `demo_idle_tilt.wav` | ~~idle variant~~ | **FAILED** — extrapolated idle rejected; replaced by group A |
| 5 | `starter_8k_adpcm4.wav` vs `starter_orig.wav` | codec / rate for the starter | decided: **12 kHz ADPCM4 in external flash**, 0.687 s = 4.0 kB. The 8 kHz file here is the 2.7 kB variant |
| 6 | `starter_loop_demo.wav` | why looping is not recommended | decided: no loop, fixed-length sample |
| 7 | `ab_portable_tuned.wav` vs `ab_portable_fft.wav` | does the **implementable** noise generator sound the same as the prototype's | |
| 8 | `size_wave256.wav` vs `size_wave2048.wav` | wavetable point count | decided: **256 points per cycle**; measured difference 0.19 dB |
| 9 | `size_noise4x512.wav` vs `size_noise8x2048.wav` | noise table size | 4 kB instead of 32 kB; measured difference 0.32 dB |

With 256-point tables the engine model is ≈ 15 kB of ROM (§21 of the analysis
doc) and the starter another 4.0 kB in external flash.

`demo_sweep.wav` and `ab_ref.wav` / `ab_syn.wav` are the older `tables_v1`
(3375–6875 rpm) set that already passed its listening test; kept for reference.

## What is synthesised and what is not

Every `demo_*`, `ab_*_syn`, `ab_portable_*` file is **fully synthesised** — no
audio from the recordings is played back. What comes from the recordings is the
measured one-engine-cycle waveform per RPM bin, the residual noise spectrum, and
the RPM trajectory; the phase and every noise sample are generated.

The starter files are the exception and are meant to be: `starter_*` **is** the
recording, decimated and codec-round-tripped exactly as the target would play it.

## Pictures

| file | what |
|---|---|
| `ab_accl_compare.png` | reference vs resynthesis spectrograms + the RPM trace |
| `startup_timing.png` | the start-up recording's level and low/mid balance, with catch / peak / settled marked |
| `envtrack_astm_v8_cell_motor+idling.png` | why no RPM track can be extracted from the start-up recording: the strongest lines are constant through cranking *and* idle |
| `ridge_astm_accl_001.png` | the order-4 ridge track with orders 2 and 8 drawn from it |
| `track_*.png` | comb tracker output per clip, with the fitted grid overlaid |
| `orderspec_astm_stable.png` | order-domain spectrogram (the track-validity check) |
| `angular_astm_stable.png` | cycle-averaged waveforms and order spectra per RPM bin |
| `tables_v1.png` | the table set: waveforms, order spectra, noise envelope and spectrum |
| `spec_*.png` | plain spectrograms of each source recording |

## Absolute level is NOT in these files (§41)

`a32.save()` peak-normalises every render, or rms-matches it to a reference, so
that A/Bs are fair. Nothing here therefore says anything about how loud the model
is. That gap is what let the engine ship 40 dB too quiet to hear on the board
(§41): the wavs were right and the level was not. Level is a board measurement.

## Group K — the two artefacts the owner named (§42)

Three variants of the same render, all rms-matched to `x0`, so any pair is a fair
A/B. `x0` = the tables as they were flashed in §41, `x1` = bin phases aligned to
the idle bin (the 4300 rpm warble), `x2` = that plus the 2.5 dB clatter trim below
2600 rpm (the 1300 rpm clatter). `x2` is what is on the board.

| file | held rpm | what to listen for |
|---|---|---|
| `hold4300_x{0,1,2}.wav` | 4300 | the チュルチュル warble. `x0` has it; `x1`/`x2` should not — the 1–3 kHz band also rises 0.70 dB because the bins stop cancelling |
| `hold1300_x{0,1,2}.wav` | 1300 | the カタカタ clatter. `x1` = `x0` here by design; `x2` is 2.31 dB quieter in 1.5–8 kHz with everything below 1 kHz identical |
| `ladder_x{0,1,2}.wav` | sweep | that neither fix changed the sound anywhere else (octave rms 0.57 dB over the whole trajectory) |

## Group L — 3700 rpm, and how much of it to keep (§43)

All held at 3700 rpm with drift off, so only the tables differ. `hold3700_x2` is
what is on the board.

| file | tables |
|---|---|
| `hold3700_x0.wav` | before §42 — the crossfade was smearing bin 11 |
| `hold3700_x1.wav` | phases aligned (= `x2` here; the clatter trim does nothing at 3700) |
| `hold3700_x2.wav` | **the board** |
| `hold3700_c00/25/50/75.wav` | `crest_soften` on bins 10/11 at amt 0.00/0.25/0.50/0.75 — `c00` is the board, `c50` matches bin 11's crest to its neighbours |
| `hold3700_x3/x4.wav` | the rejected route: bins 10+11 / bin 11 left out of the alignment |

`c50` is the one to compare against `c00`. If `c00` is the better engine, nothing
changes — the content is the recording, and `CREST_SOFTEN_AMT` stays 0.
