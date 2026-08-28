# tone_src/attic — archived audio, not generator input

Nothing here is read by a generator. `gen_tone_data_int16.py` and
`gen_dsp_tick_tables.py` only read the WAVs one directory up. These files exist
so that audio which was about to be deleted stays listenable instead of only
recoverable by git archaeology.

| file | samples @ 48 kHz | where it came from |
|---|---|---|
| `notif_action_original_48k.wav` | 31,752 | `src/app/apps/classic/notif_action_int16.h`, second variant |
| `notif_action_short_48k.wav` | 15,444 | same header, first variant |

`notif_action_int16.h` was 389,831 B of tone data entirely inside `#if 0`,
`#include`d by `snd_effect_play.c` and costing zero ROM. It was deleted
2026-08-13; these two WAVs are what it contained.

* `notif_action_original_48k.wav` is the **pre-gain original** of the tracked
  master `../tone_notif_48k.wav`: that master is this file at −6.0 dB
  (`master = round(original × 0.5)`, verified — they differ only by the 1 LSB of
  that rounding). So it is 1 LSB better provenance than the master, and it is
  kept for that reason, not because anything needs it.
* `notif_action_short_48k.wav` is a shorter earlier variant with **no other copy
  anywhere in the repo**. That is why it is archived rather than dropped.

Neither is played by the firmware, and neither was when the header still
existed.
