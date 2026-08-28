# Shared application audio utilities

This directory contains application-plane audio utilities used by both Classic
and ASRC. It does not own transport framing, timing, buffering, recovery or
hardware configuration.

- `float_conversion.*` owns PCM/float conversion and the existing application
  gain setup.
- `LED_level_meter.*` owns audio-level analysis and the current Sonora LED view.

These modules may depend on resolved application geometry and board presentation
APIs. Shared transport and board infrastructure must not depend on them; the
separation ratchet enforces that direction.
