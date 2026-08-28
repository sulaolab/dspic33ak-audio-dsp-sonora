// GENERATED -- REGENERATE IN THIS DIRECTORY.  (tools/gen_asrc_poly_flash_table.py)
// geometry      : L=64 M=30 fc=0.465 window=kaiser11 beta=11
// entries       : 1950 = (L+1)*M
// CRC32 (LE)    : 0x5E08A074
// Computed host-side in float32, operation for operation as asrc_poly_build()
// does on the device. NOT bit-identical to a device-generated table: the device
// uses its own sinf/cosf, and the window sum cancels hard at the edges, so a
// handful of near-zero taps differ. Agreement is better than 1e-6 of full scale
// (see --selftest against the L=128 M=32 table), i.e. below -120 dBFS.
#ifndef AUDIO_APP_ASRC_POLY_L64M30_FLASH_H
#define AUDIO_APP_ASRC_POLY_L64M30_FLASH_H

#include <stdint.h>

#define ASRC_POLY_L64M30_FLASH_L       (64u)
#define ASRC_POLY_L64M30_FLASH_M       (30u)
#define ASRC_POLY_L64M30_FLASH_N       (1950u)
#define ASRC_POLY_L64M30_FLASH_CRC32   (0x5E08A074u)

extern const uint32_t asrc_poly_l64m30_flash[1950];

#endif // AUDIO_APP_ASRC_POLY_L64M30_FLASH_H
