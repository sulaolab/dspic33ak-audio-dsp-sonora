# dsPIC33AK Timer HAL

## Overview

This directory contains reusable dsPIC33AK timer services:

- `nora_tick_timer_dspic33ak.c / nora_tick_timer.h`
  - Timer1-based 1 ms monotonic tick source.
  - Provides `nora_tick_timer_*()` APIs.
- `nora_high_res_timer_dspic33ak.c / nora_high_res_timer.h`
  - Timer2-based free-running high-resolution counter.
  - Provides `nora_high_res_timer_*()` APIs.

The HAL does not own clock-tree setup and does not define interrupt vectors.
Applications provide the timer input clocks through configuration structures and
own any interrupt vector wrappers.

## Timer1 Tick

Timer1 is configured as a 1 kHz periodic interrupt source. The HAL selects the
smallest available prescaler that can generate an **exact** 1 ms period within
the 32-bit `PR1` range.

Exact means exact: an input clock that no prescaler divides to 1.000 ms is
refused with `NORA_TICK_TIMER_ERR_INEXACT_PERIOD` rather than rounded to the
nearest count. Rounding is the worse outcome, because every cadence built on
this HAL is expressed in milliseconds of this tick, so an approximate tick makes
all of them wrong by one unstated factor with no attributable symptom.
`NORA_TICK_TIMER_ERR_OUT_OF_RANGE` is the other wall: the period is exact, but
no available prescaler brings the count inside `PR1`.

`clock_source` selects which clock family feeds the tick. This part supports
`NORA_TICK_TIMER_CLOCK_INTERNAL` only — its Timer1 has no FRC input, so
`NORA_TICK_TIMER_CLOCK_FRC` returns `NORA_TICK_TIMER_ERR_NOT_SUPPORTED`. The
data-sheet evidence and the two routes to an FRC-clocked tick (re-clock the
system to FRC; or feed FRC back in through `T1CK`) are documented at that
enumerator in `nora_tick_timer.h`.

Basic setup:

```c
#include "nora_tick_timer.h"

const nora_tick_timer_config_t tick_timer_config = {
    .timer_clk_hz = timer1_input_hz,
    .clock_source = NORA_TICK_TIMER_CLOCK_INTERNAL,
    .irq_priority = NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY,
    .run_in_idle = false,
};

if (nora_tick_timer_init(&tick_timer_config) != NORA_TICK_TIMER_OK) {
    while (1) {
        ;
    }
}
```

The application-owned Timer1 vector should forward to the HAL handler:

```c
void __attribute__((interrupt, context)) _T1Interrupt(void)
{
    nora_tick_timer_irq_handler();
}
```

`nora_tick_timer_get_ms()` returns 0 before successful initialization and
after `nora_tick_timer_deinit()`.

`NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY` is a recommended default, not a
hardware requirement. Applications can pass another valid non-zero priority when
their interrupt ordering requires it.

## Timer2 High-Resolution Counter

Timer2 is configured as an interrupt-free 32-bit free-running counter with a
1:1 prescaler and `PR2 = 0xFFFFFFFF`.

Basic setup:

```c
#include "nora_high_res_timer.h"

const nora_high_res_timer_config_t high_res_timer_config = {
    .timer_clk_hz = timer2_input_hz,
    .run_in_idle = false,
};

if (nora_high_res_timer_init(&high_res_timer_config) !=
    NORA_HIGH_RES_TIMER_OK) {
    while (1) {
        ;
    }
}
```

The high-resolution counter provides:

- Raw 32-bit Timer2 count reads.
- Wraparound-safe elapsed count calculation for intervals shorter than one full
  32-bit Timer2 cycle.
- Integer microsecond conversion.
- 0.1 us conversion through `*_us_x10()` helpers.

Conversion helpers use 64-bit arithmetic. Results are truncated to the requested
unit and saturate to `UINT32_MAX` if the converted value does not fit in
`uint32_t`. Interrupt code should normally store raw counts and convert them
later in foreground or status code.

## Ownership

- The HAL owns Timer1 after successful `nora_tick_timer_init()`.
- The HAL owns Timer2 after successful `nora_high_res_timer_init()`.
- Clock-tree setup remains application responsibility.
- Timer1 interrupt vector ownership remains application responsibility.
- Timer2 interrupts are disabled by the high-resolution timer path; no
  `_T2Interrupt()` vector is needed.
- Other modules should not reconfigure Timer1 or Timer2 while these HAL services
  are active.

## Scope

In scope:

- Timer1-based 1 ms monotonic tick.
- Timer2-based high-resolution free-running counter.
- Configurable Timer1 and Timer2 input clocks.
- Configurable Timer1 interrupt priority.
- Init, deinit, presence, and initialized-state queries.
- Application-owned Timer1 interrupt forwarding.

Out of scope:

- RTOS software timers.
- Busy-wait delay functions.
- Capture, compare, or PWM.
- External pulse counting.
- Gated timer mode.
- Callback scheduler.

## Integration Notes

Project-specific wiring, default clock choices, boot checks, and compatibility
wrappers should live outside this reusable HAL directory. Keeping this directory
project-neutral makes it safe to copy as a whole between integration projects and
standalone HAL repositories.
