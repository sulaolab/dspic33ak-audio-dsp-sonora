#include "nora_clock_device_dspic33ak.h"

/*
 * Device adaptation layer.
 *
 * Public Clock HAL APIs use logical source names.  This file is the internal
 * translation point for dsPIC33AK NOSC field encodings so the generic Clock HAL
 * core does not carry raw DFP register values.  PLL and CLKGEN blocks accept
 * different source subsets, so they intentionally have separate encoders.
 */

/* -------------------------------------------------------------------------- */
/* Encode PLL input source                                                    */
/* -------------------------------------------------------------------------- */
bool nora_clock_device_encode_pll_source(
    nora_clock_source_t source,
    uint16_t *value)
{
    if (value == 0) {
        return false;
    }

    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
        *value = 1u;
        return true;
    case NORA_CLOCK_SOURCE_BFRC:
        *value = 2u;
        return true;
    case NORA_CLOCK_SOURCE_PRIMARY:
        *value = 3u;
        return true;
    case NORA_CLOCK_SOURCE_REFI1:
        *value = 9u;
        return true;
    case NORA_CLOCK_SOURCE_REFI2:
        *value = 10u;
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Encode CLKGEN input source                                                 */
/* -------------------------------------------------------------------------- */
bool nora_clock_device_encode_clkgen_source(
    nora_clock_source_t source,
    uint16_t *value)
{
    if (value == 0) {
        return false;
    }

    switch (source) {
    case NORA_CLOCK_SOURCE_FRC:
        *value = 1u;
        return true;
    case NORA_CLOCK_SOURCE_BFRC:
        *value = 2u;
        return true;
    case NORA_CLOCK_SOURCE_PRIMARY:
        *value = 3u;
        return true;
    case NORA_CLOCK_SOURCE_LPRC:
        *value = 4u;
        return true;
    case NORA_CLOCK_SOURCE_PLL_1:
        *value = 5u;
        return true;
    case NORA_CLOCK_SOURCE_PLL_2:
        *value = 6u;
        return true;
    case NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV:
        *value = 7u;
        return true;
    case NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV:
        *value = 8u;
        return true;
    case NORA_CLOCK_SOURCE_REFI1:
        *value = 9u;
        return true;
    case NORA_CLOCK_SOURCE_REFI2:
        *value = 10u;
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Decode PLL input source                                                    */
/* -------------------------------------------------------------------------- */
/*
 * The inverse of the PLL encoder above, and deliberately not the CLKGEN decoder:
 * PLLxCON.NOSC and CLKxCON.NOSC share encodings for the oscillators but not the
 * rest, so decoding a PLL's input select through the CLKGEN table would name
 * sources no PLL can select.  Kept adjacent to the table it inverts.
 */
bool nora_clock_device_decode_pll_source(
    uint16_t value,
    nora_clock_source_t *source)
{
    if (source == 0) {
        return false;
    }

    switch (value) {
    case 1u:
        *source = NORA_CLOCK_SOURCE_FRC;
        return true;
    case 2u:
        *source = NORA_CLOCK_SOURCE_BFRC;
        return true;
    case 3u:
        *source = NORA_CLOCK_SOURCE_PRIMARY;
        return true;
    case 9u:
        *source = NORA_CLOCK_SOURCE_REFI1;
        return true;
    case 10u:
        *source = NORA_CLOCK_SOURCE_REFI2;
        return true;
    default:
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/* Decode CLKGEN input source                                                 */
/* -------------------------------------------------------------------------- */
bool nora_clock_device_decode_clkgen_source(
    uint16_t value,
    nora_clock_source_t *source)
{
    if (source == 0) {
        return false;
    }

    switch (value) {
    case 1u:
        *source = NORA_CLOCK_SOURCE_FRC;
        return true;
    case 2u:
        *source = NORA_CLOCK_SOURCE_BFRC;
        return true;
    case 3u:
        *source = NORA_CLOCK_SOURCE_PRIMARY;
        return true;
    case 4u:
        *source = NORA_CLOCK_SOURCE_LPRC;
        return true;
    case 5u:
        *source = NORA_CLOCK_SOURCE_PLL_1;
        return true;
    case 6u:
        *source = NORA_CLOCK_SOURCE_PLL_2;
        return true;
    case 7u:
        *source = NORA_CLOCK_SOURCE_PLL1_VCO_FRACDIV;
        return true;
    case 8u:
        *source = NORA_CLOCK_SOURCE_PLL2_VCO_FRACDIV;
        return true;
    case 9u:
        *source = NORA_CLOCK_SOURCE_REFI1;
        return true;
    case 10u:
        *source = NORA_CLOCK_SOURCE_REFI2;
        return true;
    default:
        return false;
    }
}
