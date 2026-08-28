// GENERATED -- REGENERATE IN THIS DIRECTORY.  (tools/gen_asrc_poly_flash_table.py)
// geometry      : L=128 M=30 fc=0.465 window=kaiser11 beta=11
// entries       : 3870 = (L+1)*M
// CRC32 (LE)    : 0x230DC7CA
// Computed host-side in float32, operation for operation as asrc_poly_build()
// does on the device. NOT bit-identical to a device-generated table: the device
// uses its own sinf/cosf, and the window sum cancels hard at the edges, so a
// handful of near-zero taps differ. Agreement is better than 1e-6 of full scale
// (see --selftest against the L=128 M=32 table), i.e. below -120 dBFS.
#ifndef AUDIO_APP_ASRC_POLY_L128M30_FLASH_H
#define AUDIO_APP_ASRC_POLY_L128M30_FLASH_H

#include <stdint.h>

#define ASRC_POLY_L128M30_FLASH_L       (128u)
#define ASRC_POLY_L128M30_FLASH_M       (30u)
#define ASRC_POLY_L128M30_FLASH_N       (3870u)
#define ASRC_POLY_L128M30_FLASH_CRC32   (0x230DC7CAu)

extern const uint32_t asrc_poly_l128m30_flash[3870];

#endif // AUDIO_APP_ASRC_POLY_L128M30_FLASH_H
