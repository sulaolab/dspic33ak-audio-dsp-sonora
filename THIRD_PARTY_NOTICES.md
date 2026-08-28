# Third-party notices

The repository-level MIT-0 license applies to original SulaoLab contributions.
It does not replace licenses or use restrictions attached to vendored or
adapted third-party components.

The following groups require separate attention when redistributing the
repository:

| Component | Location | License or restriction |
| --- | --- | --- |
| Arm CMSIS-Driver headers | `src/app/third_party/arm_cmsis_driver/` | Apache License 2.0; the full license is included as `LICENSE.txt`. |
| CMSIS-DSP material and dsPIC33 ports | `src/app/dspic33-cmsis-dsp/` | Individual files retain Arm and/or Microchip notices. Microchip-supplied assembly and libraries include a Microchip-products-only use condition. |
| Other Microchip example-derived files | Various source directories | Retain the license blocks present in each file and comply with their Microchip-products-only terms. |

The precompiled `.a` files are intentionally tracked because a clean build
depends on them. Their presence in this repository does not grant rights beyond
the terms supplied by their copyright owners.

The WM8904 driver in `src/app/board/devices/` no longer carries an ASF notice. It
began as the Atmel ASF WM8904 driver for SAM, but nothing of that
implementation remains: the register map, the rate/role table, the
configuration path and the declick handling are all SulaoLab code written for
this project. The notice was removed on the project owner's determination that
the file is no longer a derivative work.

The ASF-derived `wm8731.[ch]` driver was deleted rather than kept: it was never
rewritten, no application used it, and its Atmel-microcontroller-product
condition is not one a dsPIC33 project satisfies on its face. **No Atmel ASF
material remains in this repository.**

## QTouch: removed

The vendor QTouch library (`src/app/touch/` and its `.a` files) was deleted
from the project; its redistribution terms are therefore no longer this
repository's concern. Touch is now the project's own open ITC implementation
(`src/app/hal_touch/`), which carries no third-party terms.

This file is an engineering inventory, not legal advice.
