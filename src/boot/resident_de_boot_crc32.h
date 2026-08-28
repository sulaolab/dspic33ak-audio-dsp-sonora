#ifndef RESIDENT_BOOT_CRC32_H
#define RESIDENT_BOOT_CRC32_H

#include <stddef.h>
#include <stdint.h>

#define RESIDENT_BOOT_CRC32_INITIAL UINT32_C(0xFFFFFFFF)

uint32_t resident_boot_crc32_update(uint32_t state, const void *data, size_t length);
uint32_t resident_boot_crc32_finish(uint32_t state);
uint32_t resident_boot_crc32(const void *data, size_t length);

#endif
