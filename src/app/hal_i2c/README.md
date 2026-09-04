# NORA I2C HAL

## Scope

The NORA I2C HAL exposes a common master/slave API to NORA applications. It
does not choose an I2C controller for an application, define a PCB bus map, or
require AK and CK boards to use matching physical controller numbers.

Application or board code selects a `nora_i2c_instance_t` and passes it to the
HAL. The selected backend resolves that logical instance to its controller
registers. `nora_i2c_is_present()` reports whether the selected backend provides
the requested instance.

## Public API

Include only the role headers required by the caller:

- `nora_i2c.h` — shared instance, status, lifecycle, and interrupt-priority API
- `nora_i2c_master.h` — blocking master transactions, controlled no-STOP
  sequences, low-level primitives, and interrupt helpers
- `nora_i2c_slave.h` — 7-bit callback-driven slave API and its ISR delegate

These headers use only NORA types. They do not include compiler device headers,
SFR names, or dsPIC33AK register definitions.

## Backend boundary

The current implementation is the dsPIC33AK backend:

- `nora_i2c_dspic33ak_master.c`
- `nora_i2c_dspic33ak_slave.c`
- `nora_i2c_dspic33ak_common.c`
- `nora_i2c_dspic33ak_device.c`
- `nora_i2c_dspic33ak_reg.h`
- `nora_i2c_dspic33ak_internal.h`
- `nora_i2c_dspic33ak_device.h`

The last two headers and the register header are backend-internal. They are not
application include files. A CK backend may implement the same public API while
using a different controller inventory, register map, and interrupt binding.

### Interrupt vectors belong to the backend

`nora_i2c_dspic33ak_device.c` defines the `_I2CxInterrupt` vectors (and the
RX/TX pair) for the instances the target has, and each one calls the slave
engine. An application does not write I2C vector functions; if it defines one
itself the link fails on the duplicate symbol, which is the intended signal.

This is what makes a slave application source-portable: the number of interrupt
sources per controller is silicon count (four on AK, a single `SI2Cx` on CK), so
a portable caller must not be the one binding them. Only
`nora_i2c_slave_event_irq()` is public, so a custom vector or a host-side test
can still drive the service routine.

## Board and application policy

The application or board owns policy such as which controller reaches Codec A,
Codec B, or an expansion connector. For example, an AK Sonora board may choose
one pair of instances while a CK board chooses another. That selection belongs
outside this HAL and is supplied as a `nora_i2c_instance_t` argument.

The optional CMSIS I2C adapter is separate from this HAL and is
dsPIC33AK-specific.
