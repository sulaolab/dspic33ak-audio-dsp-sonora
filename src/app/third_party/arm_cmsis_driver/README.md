# ARM CMSIS-Driver Headers

This directory contains a minimal third-party copy of ARM CMSIS-Driver headers
required to build the dsPIC33AK CMSIS-Driver I2C and USART wrappers used by this
project.

## Imported files

- Include/Driver_Common.h
- Include/Driver_I2C.h
- Include/Driver_USART.h

## Source

- Upstream repository: ARM-software/CMSIS_5
- Upstream path: CMSIS/Driver/Include/
- License source file: LICENSE.txt from the upstream repository root

## License

The imported ARM CMSIS-Driver header files are provided by ARM under the
Apache License 2.0.

See LICENSE.txt in this directory.

The original copyright and SPDX license headers in the imported files must be
kept intact.

## Local policy

These files are copied without modification.

Do not edit the imported header files locally unless absolutely necessary. If an
update is needed, re-import the files from the upstream CMSIS source and keep
the original copyright and SPDX license headers intact.

This directory intentionally contains only the minimal CMSIS-Driver API headers
needed by the local I2C and USART CMSIS-Driver wrappers. It is not a full CMSIS,
CMSIS-Core, CMSIS-DSP, or Microchip Harmony distribution.
