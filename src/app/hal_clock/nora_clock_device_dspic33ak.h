#ifndef NORA_CLOCK_DEVICE_DSPIC33AK_H
#define NORA_CLOCK_DEVICE_DSPIC33AK_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_clock.h"

/*
 * Internal dsPIC33AK device adaptation.
 *
 * The public Clock HAL uses logical source names such as FRC, PLL_1, and
 * PRIMARY.  This layer owns the family/device-specific NOSC encodings used by
 * PLLxCON and CLKxCON, keeping raw numeric values out of the generic core and
 * out of public headers.  If a future AK device changes encodings or source
 * availability, that mapping belongs here.
 *
 * The mapping is needed in both directions.  Encoding serves a request; decoding
 * serves an observation -- nora_clock_get_state() reads back the COSC field the
 * hardware reports, and turning that number into a logical name is the same
 * device fact read the other way, so it belongs in this file next to the table it
 * inverts rather than as a second table in the core.
 *
 * WHY THE FILE NAME CARRIES THE BACKEND TAG AND THE SYMBOLS DO NOT
 *   In this tree an untagged name reads as portable, and this content is not: the
 *   encodings are one family's.  The file was called nora_clock_device.h, which made it
 *   look like a shared interface and reserved the obvious name for a backend that has
 *   no claim on it -- another family's device adaptation is a sibling
 *   (nora_clock_device_dspic33ck.h), not an implementation of this.
 *
 *   The declared symbols stay untagged because they are internal to ONE backend's build
 *   and never linked beside another's: a build targets one part, so one device table is
 *   compiled in.  The name a caller must not get wrong is the file it includes, and that
 *   one now says which silicon it is about.
 */

#ifdef __cplusplus
extern "C" {
#endif

bool nora_clock_device_encode_pll_source(
    nora_clock_source_t source,
    uint16_t *value);

bool nora_clock_device_encode_clkgen_source(
    nora_clock_source_t source,
    uint16_t *value);

/*
 * Inverse of nora_clock_device_encode_clkgen_source().  Returns false for an
 * encoding this device table does not name, and the caller reports
 * NORA_CLOCK_SOURCE_UNKNOWN rather than inventing a name for it.
 */
bool nora_clock_device_decode_clkgen_source(
    uint16_t value,
    nora_clock_source_t *source);

/*
 * Inverse of nora_clock_device_encode_pll_source(), for the same reason: a PLL's
 * output frequency is reconstructed from its own registers, and the input select
 * field has to become a logical source name to look that input's frequency up.
 *
 * No encoding in this table names a PLL output -- a PLL is not a legal PLL input --
 * so a caller resolving "what frequency does this PLL's input carry" cannot recurse
 * back into a PLL.  That is a property of the table, and it is the reason the
 * reconstruction in the core needs no depth guard.
 */
bool nora_clock_device_decode_pll_source(
    uint16_t value,
    nora_clock_source_t *source);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CLOCK_DEVICE_DSPIC33AK_H */
