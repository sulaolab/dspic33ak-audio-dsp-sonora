# ADC HAL Restart Notes

This note records the safe stopping point for the ADC HAL cleanup branch.

## Current Scope

- `src/hal_adc/` owns the small polling ADC HAL used by the potentiometer path.
- The current HAL covers the validated CH0 polling use case:
  - dsPIC33AK512: ADC5 CH0, positive input AN0.
  - dsPIC33AK128: ADC1 CH0, positive input AN6.
- The board's POT driver is the current consumer of `hal_adc`.
- Legacy MCC-style polling files for ADC1/ADC5 and the unused common ADC headers were removed.
- The former `src/adc/` ADC3/ADC4 audio-in DMA path was removed (2026-07-25): it was
  compile-dead everywhere (its `APP_USE_ADC_INOUT` gate was a hard `0`) and its only
  live residue was a misleading boot-time button prompt. `hal_adc` is unaffected.

## Validation Status

- dsPIC33AK512 clean build: passed.
- dsPIC33AK128 clean build: passed.
- dsPIC33AK512 hardware: potentiometer value changes LED color through the `hal_adc`-backed path.
- dsPIC33AK128 hardware: not yet tested; current acceptance is by clean build plus register/API symmetry with the legacy ADC1 CH0 path.

## Recommended Next Work

1. Build both `dsPIC33AK512` and `dsPIC33AK128` after any ADC-related source change.
2. Keep `hal_adc` focused on reusable ADC register/API behavior; keep board wiring,
   DMA stream policy, and audio processing outside the core HAL until deliberately migrated.
3. If an ADC audio-in path is needed again in the future, reintroduce it as a
   dedicated `src/adc_audio_dma/` layer with a real (default-off but overridable)
   feature gate, rather than reviving the compile-dead `APP_USE_ADC_INOUT` shape.
