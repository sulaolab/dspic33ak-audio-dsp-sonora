# NORA DMA public contract

`nora_dma.h` defines the DMA API used by NORA applications and transport
drivers. Public functions and types use the `nora_dma_` prefix; public
constants and enum values use `NORA_DMA_`.

The contract is intended to keep application changes small between
NORA-supported dsPIC33AK and dsPIC33CK ports. It is not a universal DMA HAL:
each port provides the backend-specific channel inventory, trigger mapping,
addressing rules, and register access implementation.

The current NORA DMA consumers are the SPI/I2S/TDM transport and the Classic
app's PWM audio-DAC output (`apps/classic/classic_audio_pwm.c`, DMA4-7 via the
`NORA_DMA_TRIGGER_PWM_GEN5..8` triggers). Both configure through
`nora_dma_channel_config()`; ping-pong half-tracking and the DMA ISR vectors
stay in each consumer (see the design-boundaries note in `nora_dma.h`).

The current dsPIC33AK backend files are named `nora_dma_dspic33ak.*`. Code that
uses DMA includes only `nora_dma.h`; it contains portable types and function
declarations only, with no XC header or SFR spelling. Backend register
definitions, device conditionals, and the dsPIC33AK ISR fast-path inlines stay
in `nora_dma_dspic33ak_reg.h` / `nora_dma_dspic33ak_fast.h` and their backend
users.

`nora_dma_channel_t` and `nora_dma_trigger_t` are logical NORA identifiers.
The dsPIC33AK backend maps them to its channel inventory and CHSEL encodings;
the SPI/I2S/TDM transport table therefore does not contain raw trigger-register
values. A CK port supplies its own mapping and fast-path implementation while
application-level DMA calls stay unchanged.
