# Sonora board support

This directory owns facts and integration that are specific to the Sonora
hardware. It does not choose ASRC or Classic application behavior.

- `audio/`: Sonora audio pin routing, TDM topology and project-supplied TDM
  configuration.
- `clock/`: Sonora clock-tree policy and board clock routing.
- `devices/`: drivers and board-facing support for connected codecs, flash,
  buttons, LEDs, potentiometer and their I2C bus setup.
- `board_dbg_pins.h`: board-wide debug/scope pin assignments.

Allowed dependency direction:

```text
apps / audio runtime
        |
        v
      board
        |
        v
  HAL / CMSIS drivers
```

Board code must not select an application or depend on ASRC/Classic internals.
Board-layer audio functions use the `audio_transport_board_*` naming (renamed
2026-07-24 from the legacy `audio_app_board_*` prefix); their ownership is the
`board/audio` layer.
