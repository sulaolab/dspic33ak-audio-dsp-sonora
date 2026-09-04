# CMSIS-Driver wrappers for dsPIC33AK HALs

This directory holds the CMSIS-Driver wrappers that map ARM `ARM_DRIVER_*` APIs
onto the dsPIC33AK HALs (I2C, USART, and SAI). The I2C section
follows first; see **SAI integration** at the end for the SPI/I2S/TDM SAI
driver.

## USART wrapper note

`Driver_USART_dsPIC33AK.{c,h}` maps the CMSIS-Driver USART API onto the UART HAL
async transfer model. In this Sonora project, `RTE_Device_USART_dsPIC33AK.h` is
the application configuration consumed by the wrapper; keep
`RTE_Device_USART_dsPIC33AK_example.h` as the template/reference copy.

`Receive()` uses `nora_uart_rx_start_clean()` so each CMSIS receive starts
from a clean async RX arm instead of opening a separate flush/start window.

# CMSIS-Driver I2C Wrapper for dsPIC33AK I2C HAL

## Overview

This directory provides a CMSIS-Driver I2C wrapper for the dsPIC33AK blocking I2C HAL.

The current implementation is a blocking master-only driver intended to make the dsPIC33AK I2C HAL usable through the CMSIS-Driver I2C API.

## File layout

```text
cmsis_driver/
  Driver_I2C_dsPIC33AK.c
  Driver_I2C_dsPIC33AK.h
  RTE_Device_I2C_dsPIC33AK_example.h
```

In this sonora validation project, these files are located under `src/cmsis_driver/`. When moved to the public HAL repository, this directory is intended to become the top-level `cmsis_driver/` folder.

## Instance mapping

| CMSIS driver object | dsPIC33AK HAL instance |
|---|---|
| `Driver_I2C0` | `NORA_I2C_INST_1` |
| `Driver_I2C1` | `NORA_I2C_INST_2` |
| `Driver_I2C2` | `NORA_I2C_INST_3` |
| `Driver_I2C3` | `NORA_I2C_INST_4` |

## Supported features

- Blocking master transmit
- Blocking master receive
- 7-bit addressing
- `MasterTransmit(..., xfer_pending = false)`
- `MasterReceive(..., xfer_pending = false)`
- `MasterTransmit(..., xfer_pending = true)` followed by `MasterReceive(..., xfer_pending = false)`
- Bus speed selection via `Control(ARM_I2C_BUS_SPEED, ...)`, callable before or
  after `PowerControl(ARM_POWER_FULL)` when the bus is idle:
  - Standard mode: 100 kHz
  - Fast mode: 400 kHz
  - Fast-mode Plus: 1 MHz
  - High-speed mode (`ARM_I2C_BUS_SPEED_HIGH`): unsupported
  - Returns `ARM_DRIVER_ERROR_BUSY` during an active transfer or a pending
    no-STOP transaction

## Unsupported features and limitations

The initial wrapper intentionally does not support:

- `MasterReceive(..., xfer_pending = true)`
- Slave/client mode:
  - `SlaveTransmit()`
  - `SlaveReceive()`
- 10-bit addressing
- General Call
- High-speed mode
- Bus clear
- Abort transfer
- Interrupt-driven asynchronous transfer

Unsupported functions return `ARM_DRIVER_ERROR_UNSUPPORTED`.

## Configuration through RTE_Device_I2C_dsPIC33AK_example.h

`RTE_Device_I2C_dsPIC33AK_example.h` is an example RTE configuration for this I2C
wrapper (not a shared application-level `RTE_Device.h`). It selects which CMSIS
I2C driver objects are enabled and provides default timing parameters. In an
integrated application, copy the required I2C definitions into that application's
own `RTE_Device.h` or equivalent configuration header.

Example:

```c
#define RTE_I2C1 1

#define RTE_I2C1_FCY_HZ             100000000u
#define RTE_I2C1_BUS_HZ             400000u
#define RTE_I2C1_TIMEOUT_MS         10u
#define RTE_I2C1_PENDING_TIMEOUT_MS 0u
```

`RTE_I2C1` enables `Driver_I2C1`, which maps to `NORA_I2C_INST_2`.

## Tick provider

The HAL timeout mechanism requires a millisecond tick provider.

The wrapper provides a weak default implementation:

```c
uint32_t Driver_I2C_dsPIC33AK_GetMs(void)
{
    return 0u;
}
```

Applications should override this function when timeout support is required.

Example:

```c
uint32_t Driver_I2C_dsPIC33AK_GetMs(void)
{
    return GetTicks();
}
```

## Include path

The wrapper intentionally uses header names without hard-coded relative paths.

The build system should provide include paths for:

```text
src
cmsis_driver
path/to/CMSIS/Driver/Include
```

If the HAL headers are placed in a separate folder, add that folder as an include path as well.

Example for a project layout where HAL files are in `src/hal_i2c` and CMSIS wrapper files are in `src/cmsis_driver`:

```text
src/hal_i2c
src/cmsis_driver
path/to/CMSIS/Driver/Include
```

## Basic usage

```c
#include "Driver_I2C_dsPIC33AK.h"

extern ARM_DRIVER_I2C Driver_I2C1;

uint8_t reg = 0x00;
uint8_t rx[2];

Driver_I2C1.Initialize(NULL);
Driver_I2C1.PowerControl(ARM_POWER_FULL);

Driver_I2C1.MasterTransmit(0x1A, &reg, 1, true);
Driver_I2C1.MasterReceive(0x1A, rx, 2, false);
```

This sequence performs a typical register read:

```text
START + address write + register address
Repeated START + address read + data read + STOP
```

## Verified behavior

The wrapper has been verified on the dspic33ak-audio-dsp-sonora project using the WM8904 codec register read path.

Verified sequence:

```c
Driver_I2Cx.MasterTransmit(addr7, &reg, 1, true);
Driver_I2Cx.MasterReceive(addr7, rx, 2, false);
```

The WM8904 device ID register was read successfully as `0x8904`.

Runtime bus speed change was also verified on the same project:

```c
Driver_I2Cx.Initialize(NULL);
Driver_I2Cx.PowerControl(ARM_POWER_FULL);
Driver_I2Cx.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_STANDARD);
```

`Control(ARM_I2C_BUS_SPEED, ...)` after `PowerControl(ARM_POWER_FULL)` returned
`ARM_DRIVER_OK` and the WM8904 device ID read / init succeeded at the new speed.

Verified at 100 kHz, 150 kHz, 200 kHz, 300 kHz and 400 kHz. The 100 kHz case in
particular depends on the HAL STOP-completion behaviour: STOP waits for
`CON1.PEN` to clear, not just `STAT2.STOPE`.

---

# CMSIS-Driver SAI integration for dsPIC33AK SPI/I2S/TDM HAL

## Overview

`Driver_SAI_dsPIC33AK.{c,h}` maps the ARM CMSIS-Driver SAI API onto the
`nora_spi_i2s_tdm` HAL and is integrated into the opt-in Sonora validation
routes. Its public API reference is
[dspic33ak-sai-cmsis-driver](https://github.com/sulaolab/dspic33ak-sai-cmsis-driver).
The official ARM `Driver_SAI.h` (Apache-2.0, API v1.2) is vendored under
`src/third_party/arm_cmsis_driver/Include/`.

## Sonora enablement

- `ENA_SAI_WRAPPER_DRYTEST` enables the boot-time API dry test without starting a live stream.
- `ENA_SAI_WRAPPER_LIVE` selects the live full-duplex wrapper loopback route instead of the
  direct demo callback path.
- `RTE_SAI0` is derived from the resolved `APP_USE_SAI_WRAPPER_LIVE` and
  `APP_USE_SAI_WRAPPER_DRYTEST` flags, so an unused wrapper is compiled out.

`Driver_SAI0` is fixed to literal physical SPI1. Its RX DMA follows
`NORA_TDM_SPI1_RX_DMA` (DMA0 by default); non-SPI1 legs remain outside the wrapper.
An enabled wrapper combined with the explicit SPI3/4 test bank is rejected at compile time.

## Sonora integration hooks

`board/audio/audio.c` provides `Driver_SAI_dsPIC33AK_GetDefaultConfig()` and
`Driver_SAI_dsPIC33AK_IsSampleRateSupported()`. These supply board-electrical fields,
block geometry, and the product rate policy. The wrapper derives protocol/slot count
from `Control(CONFIGURE_*)` and forces its validated slave/32-bit transport envelope.
MCLK may be an external input or inactive.

## Status

Build-verified on dsPIC33AK512 in the dspic33ak-audio-dsp-sonora project and
exercised through the opt-in `ENA_SAI_WRAPPER_LIVE` loopback harness. The full RX-to-TX copy path on codec A
(`Driver_SAI0` = SPI1) completed 5/5 clean starts in the recorded validation run.

Historical note: an older run observed an intermittent first-block cold-start crackle
(approximately 1/10). That observation has not been revalidated against the current
DMA/TDM reliability changes; treat it as a hardware-gate retest item, not a current
measured failure rate.
