# Sonora Timer HAL Integration Notes

This file records Sonora-specific timer wiring that intentionally lives outside
`src/app/hal_timer/`. The HAL directory is kept project-neutral so it can be copied
as a whole to other repositories.

## Timer1 Tick

- Timer1 is the 1 ms system tick source.
- The application compatibility layer in `src/app/timer_app/` owns `_T1Interrupt()`
  and forwards to `nora_tick_timer_irq_handler()`.
- `Timer1_Init()` initializes the public-style Timer1 HAL with
  `timer_clk_hz = (uint32_t)FCY`.
- Legacy Sonora entry points are kept in `src/app/timer_app/timer_app.c`:
  - `Timer1_Init()`
  - `GetTicks()`
  - `delay_ms()`
  - `delay_us()`
- `GetTicks()` returns `nora_tick_timer_get_ms()`.

## Timer2 High-Resolution Counter

- Timer2 is used as a free-running profiling counter.
- The Sonora validation app initializes it with `timer_clk_hz = (uint32_t)FCY`.
- The high-resolution timer feeds the SPI/I2S/TDM DMA ISR load monitor and
  related status prints.

## Compatibility Detail

The legacy `delay_us()` wrapper truncates to 10 us units internally:

```text
delay_us(1)  ... delay_us(9)  : effectively 0 us
delay_us(15)                  : about 10 us
delay_us(99)                  : about 90 us
```

This behavior is preserved in `src/app/timer_app/` for compatibility with the
current Sonora application.
