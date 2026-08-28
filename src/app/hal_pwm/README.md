# NORA PWM (PG) public contract

`nora_pwm.h` defines the PWM Generator (PG) register API used by NORA
applications. Public functions and types use the `nora_pwm_` prefix; public
constants and enum values use `NORA_PWM_`.

This is the fleet's first PWM HAL -- no sibling project has a PWM port yet, on
either device family -- so its scope is set by this project's own two consumers,
rather than by matching an existing sibling:

- The RGB LED (PG1/2/3): single output, no dead time in practice, CPU writes
  duty directly.
- The Classic app's optional PWM audio-DAC output (PG5/6, optionally 7/8):
  complementary H+L pair with dead time, duty streamed by DMA.

Both are the same PG hardware with different parameters, and they already
share one module-wide resource: `PCLKCON.MCLKSEL`/`DIVSEL` and `MPER`/`MDC`
select the clock for every PG instance on the device, not just one.
`nora_pwm_module_init()` owns that explicitly and `nora_pwm_generator_init()`
refuses to run before it, rather than relying on caller ordering.

Not in scope: space-vector modes, fault/PCI/current-limit inputs, PWM-event
outputs -- nothing in this project calls any of that today, and there is no
sibling PWM HAL to match either. DMA duty-streaming policy, PG/DMA ISRs, and
float-to-duty conversion stay with the consumer (Classic app); see
`hal_dma/README.md` for the matching DMA-side boundary.

The current dsPIC33AK backend is one file, `nora_pwm_dspic33ak.c` -- the only
place that names PG1CON..PG8IOCON1 and the PWM1H..PWM8L PPS output enum
members. It picks the PGxEVT/PGxIOCON vs PGxEVT1/PGxIOCON1 naming (a
device-header fact, checked via `#if defined(PG1EVT1)`) without depending on
any app-level target macro. Unlike hal_uart's device.c, there is no separate
device-mapping file here: PG1..PG8 have no uniform per-instance register
table to justify one (each instance's sequence is its own macro expansion),
and keeping validation + per-instance code in one translation unit lets the
compiler inline across what would otherwise be a needless call boundary --
this project has no LTO, so a cross-file call cannot be folded even at -Os.
