#include "resident_de_boot_crc32.h"

uint32_t resident_boot_crc32_update(uint32_t state, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index;

    for (index = 0u; index < length; index++) {
        uint8_t bit;
        state ^= bytes[index];
        for (bit = 0u; bit < 8u; bit++) {
            const uint32_t mask = (uint32_t)-(int32_t)(state & 1u);
            state = (state >> 1) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return state;
}

uint32_t resident_boot_crc32_finish(uint32_t state)
{
    return state ^ UINT32_C(0xFFFFFFFF);
}

uint32_t resident_boot_crc32(const void *data, size_t length)
{
    return resident_boot_crc32_finish(
        resident_boot_crc32_update(RESIDENT_BOOT_CRC32_INITIAL, data, length));
}
