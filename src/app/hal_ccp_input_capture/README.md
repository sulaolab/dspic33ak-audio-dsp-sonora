# NORA CCP Input Capture HAL

## Overview

Lightweight Input Capture HAL for SCCP/MCCP. It configures a CCP
instance and returns raw edge timestamps through polling or an ISR callback.
The application owns all period, frequency, duty-ratio, and sample-rate-ratio
calculations.

This project is an independent open-source HAL and is not an official
Microchip software product.

## Scope

Supported:

- Input Capture
- 16-bit and 32-bit free-running timestamps (`CCPxCON1.SYNC = 11111`)
- rising, falling, both-edge, every-fourth-rising, and
  every-sixteenth-rising capture
- capture-event interrupt postscaling
- polling
- ISR callback
- overflow detection

Not supported:

- Timer mode API
- Output Compare
- PWM
- duty or frequency calculation
- PPS configuration
- CLKGEN configuration or frequency verification
- DMA

Polling and callback delivery must not be used concurrently on the same
instance. The ISR always drains the capture FIFO. Interrupts may be enabled
without a callback, but captured values will then be discarded by the ISR.

## Backend availability

The dsPIC33AK backend selects its complete CCP1-through-CCP9 register table from
the DFP capability macros. On a device without that inventory, every API reports
`NORA_CCP_ERR_INSTANCE`; the public API remains available without a
device-specific preprocessor setting.

## Files

| File | Purpose |
| --- | --- |
| `nora_ccp_input_capture.h` | Public API and configuration types |
| `nora_ccp_input_capture_dspic33ak.c` | Device facts, capture logic, and optional vectors |
| `nora_ccp_input_capture_dspic33ak_fast.h` | Backend-private ISR hot-path helpers (flag clear, raw timestamp read) |
| `nora_ccp_input_capture_dspic33ak_reg.h` | Self-contained register masks and field helpers |
| `nora_ccp_input_capture_conf_dspic33ak.h_example` | Optional compile-time configuration example |

## Integration

1. Route the signal to the selected ICx input using PPS.
2. Prepare the selected peripheral clock source.
3. Call `nora_ccp_icap_configure()`.
4. Choose one delivery model: register a callback or poll the FIFO.
5. Call `nora_ccp_icap_start()`.
6. Calculate timestamp differences in the application.

The HAL does not read or verify the actual CLKGEN frequency.
`nora_ccp_icap_timebase_hz()` returns the caller-supplied
`timebase_src_hz` divided by the configured prescaler.

`nora_ccp_icap_configure()` sets `CCPxCON1.SYNC = 11111` and keeps
`TRIGEN = 0` to select the free-running Input Capture time base. The family
data sheet's Input Capture prose refers to CLKGEN12 for `CLKSEL = 001`, but
the clock-peripheral allocation table and XC-DSC DFP 1.3.185 both define that
encoding as CLKGEN13; therefore the public API uses `CLKGEN13`.

## Minimal callback example

```c
#include <stddef.h>

#include "nora_ccp_input_capture.h"

static volatile uint32_t s_prev;
static volatile uint32_t s_period;

static void capture_cb(nora_ccp_inst_t inst,
                       uint32_t timestamp,
                       void *user)
{
    (void)inst;
    (void)user;

    s_period = timestamp - s_prev;
    s_prev = timestamp;
}

void capture_init(void)
{
    const nora_ccp_icap_config_t cfg =
    {
        .source          = NORA_CCP_SRC_PIN,
        .edge            = NORA_CCP_EDGE_EVERY_RISING,
        .clock           = NORA_CCP_CLK_PERIPHERAL,
        .prescaler       = NORA_CCP_PS_1,
        .use_32bit       = true,
        .irq_ops         = NORA_CCP_IRQ_EVERY_EVENT,
        .irq_enable      = true,
        .irq_priority    = 4,
        .timebase_src_hz = 100000000u
    };

    (void)nora_ccp_icap_set_callback(NORA_CCP1,
                                          capture_cb,
                                          NULL);
    (void)nora_ccp_icap_configure(NORA_CCP1, &cfg);
    (void)nora_ccp_icap_start(NORA_CCP1);
}
```

Frequency calculation remains in application code:

```c
float frequency_hz =
    (float)nora_ccp_icap_timebase_hz(NORA_CCP1) /
    (float)s_period;
```

Unsigned 32-bit subtraction produces the correct difference across one timer
wrap, provided no more than one wrap occurs between the two captures.

For polling, set `irq_enable = false` and repeatedly call
`nora_ccp_icap_read()`. Do not register or use a callback for that
instance.

## Interrupt vector ownership

The default is application-owned: the HAL defines no interrupt vectors.

Application-owned example:

```c
void __attribute__((interrupt, no_auto_psv)) _CCP1Interrupt(void)
{
    nora_ccp_icap_isr(NORA_CCP1);
}
```

To let the HAL own every supported CCP Input Capture vector, define:

```c
#define NORA_CCP_DSPIC33AK_DEFINE_VECTORS 1
```

Prefer selecting only the instances an application uses:

```c
#define NORA_CCP_DSPIC33AK_DEFINE_CCP1_VECTOR 1
#define NORA_CCP_DSPIC33AK_DEFINE_CCP2_VECTOR 1
```

These macros are normally supplied as project-wide compiler definitions so
they also apply while compiling the HAL source file.

## API usage notes

- Configure before starting.
- Stop the instance, or use an application-owned critical section, before
  changing a callback that an ISR may read.
- Callbacks run in ISR context and must remain short and non-blocking.
- Overflow means at least one captured timestamp was lost.
- PPS and peripheral clock routing must be complete before capture starts.

## Verification

The host tests compile each public header by itself and exercise configuration
validation without target hardware. A release candidate must additionally be
verified on dsPIC33AK512MPS512 for callback and polling operation, FIFO drain,
overflow, stop/start, 32-bit wrap subtraction, multiple instances, and both
vector-ownership modes.

Record the hardware result without estimating missing values:

| Field | Result |
| --- | --- |
| Device | dsPIC33AK512MPS512, revision 1 |
| Compiler version | XC-DSC 3.31.01 |
| DFP version | dsPIC33AK-MP_DFP 1.3.185 |
| Clock source | Standard peripheral clock; caller-supplied `FCY` |
| CCP instance | CCP1 and CCP2 |
| Input frequency | Codec A: approximately 48 kHz; Codec B: approximately 48 kHz and 44.1 kHz |
| Measured period | 48 kHz: approximately 20.8333 us; 44.1 kHz: approximately 22.6757 us |
| Free-running synchronization | PASS: `SYNC = 11111`, `TRIGEN = 0`; CCP1 and CCP2 continued to capture at approximately 48 kHz |
| Callback / polling | PASS: callback with HAL-owned and application-owned vectors; polling also captured both instances, but overflowed under the full 16-channel ASRC load |
| Overflow | PASS: polling test detected FIFO overflow after timestamp loss |
| Stop/start | PASS: two mute-bounded stop/start cycles recovered with `recover=0` |
| Multiple instances | PASS: CCP1 and CCP2 operated concurrently |

## License

MIT No Attribution (MIT-0). See `LICENSE`.

Microchip, dsPIC, and MPLAB are trademarks of Microchip Technology
Incorporated in the U.S.A. and other countries.
