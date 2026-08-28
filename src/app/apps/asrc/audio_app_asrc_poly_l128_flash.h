// GENERATED -- REGENERATE IN THIS DIRECTORY.  (tools/gen_asrc_poly_flash_table.py)
// source        : runtime-generated dsPIC L=128 M=32 polyphase table, exact float32 bits
//                 (console *nt2B dump; storage=ram)
// entries       : 4128 = (L+1)*M
// CRC32 (LE)    : 0x1EA0941C   (device == host verified)
// The words are the RAW 32-bit float bit patterns, stored verbatim so the compiled flash table is
// byte-identical to the RAM-generated table. The kernel reinterprets each row as (const float*).
#ifndef AUDIO_APP_ASRC_POLY_L128_FLASH_H
#define AUDIO_APP_ASRC_POLY_L128_FLASH_H

#include <stdint.h>

#define ASRC_POLY_L128_FLASH_N        (4128u)
#define ASRC_POLY_L128_FLASH_CRC32    (0x1EA0941Cu)

extern const uint32_t asrc_poly_l128_flash[4128];

#endif // AUDIO_APP_ASRC_POLY_L128_FLASH_H
