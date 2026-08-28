#include "nora_udid.h"

#include <stddef.h>
#include <stdint.h>

// UDID word addresses (unified address space; 4-byte aligned). See the header.
#define NORA_UDID1_ADDRESS UINT32_C(0x007F2BE0)
#define NORA_UDID2_ADDRESS UINT32_C(0x007F2BE4)
#define NORA_UDID3_ADDRESS UINT32_C(0x007F2BE8)
#define NORA_UDID4_ADDRESS UINT32_C(0x007F2BEC)

// Read one 32-bit word from an absolute address. A volatile const pointer is used
// so the compiler cannot elide or reorder the read. dsPIC33A is a unified address
// space, so no PSV / table-read setup is needed (unlike dsPIC33C/E/F).
static uint32_t nora_udid_read_word(uint32_t address)
{
    const volatile uint32_t *source;

    source = (const volatile uint32_t *)(uintptr_t)address;
    return *source;
}

bool nora_udid_read(nora_udid_t *udid)
{
    if (udid == NULL)
    {
        return false;
    }

    udid->word[0] = nora_udid_read_word(NORA_UDID1_ADDRESS);
    udid->word[1] = nora_udid_read_word(NORA_UDID2_ADDRESS);
    udid->word[2] = nora_udid_read_word(NORA_UDID3_ADDRESS);
    udid->word[3] = nora_udid_read_word(NORA_UDID4_ADDRESS);

    return nora_udid_is_plausible(udid);
}

bool nora_udid_is_plausible(const nora_udid_t *udid)
{
    bool all_zero = true;
    bool all_one = true;
    uint32_t index;

    if (udid == NULL)
    {
        return false;
    }

    for (index = 0U; index < NORA_UDID_WORD_COUNT; index++)
    {
        if (udid->word[index] != UINT32_C(0x00000000))
        {
            all_zero = false;
        }

        if (udid->word[index] != UINT32_C(0xFFFFFFFF))
        {
            all_one = false;
        }
    }

    return !(all_zero || all_one);
}
