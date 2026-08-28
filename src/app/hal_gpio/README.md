# NORA GPIO/PPS public contract

`nora_gpio.h`, `nora_gpio_event.h`, and `nora_pps.h` define the public GPIO,
change-notification, and PPS API used by NORA applications. Public functions
and types use the `nora_` prefix; public constants and macros use `NORA_`.

This contract is for small-change application ports between NORA-supported
dsPIC33AK and dsPIC33CK targets. It is not a claim of a universal GPIO HAL for
arbitrary processors. Each target supplies its own backend, pin capabilities,
RP number encoding, and board pin map. Application feature code should use
board-level pin names rather than raw RP values or packed-pin numbers.

The current dsPIC33AK backend files are named `*_dspic33ak.*`. Backend-specific
register access and device conditionals belong there; applications include only
the public `nora_*.h` headers.
