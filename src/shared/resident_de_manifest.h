#ifndef RESIDENT_BOOT_MANIFEST_H
#define RESIDENT_BOOT_MANIFEST_H

#include <stdint.h>

/* The single-panel Flash layout, per device.
 *
 * Stated per device rather than as one set of constants because the panel is a
 * quarter of the size on the MC106 -- so the manifest page and the end of Flash
 * differ, and since 2026-08-15 so do the bootloader region and the application base.
 *
 * WHY THE BOOT REGION IS 16 KiB ON THE MC106 AND 32 KiB ON THE MPS512. Both numbers
 * are measurements of an image, not budgets. The MC106 region was 32 KiB, then 28 KiB
 * on 2026-08-15 when the application turned out to need the seventh erase page, and is
 * 16 KiB since 2026-08-20: a ROM-diet campaign took this part's resident image from
 * 25,896 B to 15,156 B (thirteen individually measured checkpoints -- AK128 profile
 * gates over the generic clock/PPS/UART/GPIO/timer platform, plus -Os for both parts),
 * and board-gate run 4 passed every gate in scope on that image in this 16 KiB
 * region (run 3 had passed the byte-identical image in the old 28 KiB one). 16 KiB is
 * the smallest page-aligned region it fits in with the 1 KiB of growth margin its
 * feasibility report requires (0x3C00 = 15,360 B release criterion, 204 B of which is
 * still unspent); 12 KiB would not hold the image at all. The 12 KiB this released
 * went to the application, which is why its base moved with the region. The MPS512
 * keeps 32 KiB: its resident links to 25,308 B and it has 471 KiB of application
 * space, so it has nothing to gain by trimming the region.
 *
 * The whole reasoning, the measurements and the gate results are in
 * [internal] report_resident_bootloader_16k_feasibility_2026-08-19.md.
 *
 * A device without an arm here is a compile
 * error rather than a wrong address, for the same reason noinit_ram_config.h
 * refuses to guess: an out-of-panel address links silently and only fails when a
 * board is in front of you.
 *
 * RESIDENT_LAYOUT_ID differs as well, and must: it is what a bootloader compares
 * against an incoming manifest, so the two layouts have to be distinguishable
 * before the payload is written, not after. IT ALSO CHANGES WHEN A LAYOUT MOVES, which
 * is why the MC106 is "SAK3" and not "SAK2": "SAK2" IS RETIRED and denotes the 28 KiB
 * arrangement with the application at 0x807000. Reusing it would have let a package
 * built for either arrangement pass the fence for the other, and the payload would
 * then be programmed 12 KiB from where its IVT expects to be. A retired ID is never
 * reissued -- tools/serial_boot_package.py names the retired ones when it refuses
 * them. Moving a layout therefore means changing every contract point in one commit:
 * this header, src/linker/p33AK128MC106_serial_update_app.gld, src/boot/boot_image.psd1,
 * the application's MPLAB IVT address, and that package tool. */
#if defined(__dsPIC33AK512MPS512__)
  #define RESIDENT_BOOT_BASE_ADDRESS       UINT32_C(0x800000)
  #define RESIDENT_BOOT_SIZE_BYTES         UINT32_C(0x008000)
  #define RESIDENT_APP_BASE_ADDRESS        UINT32_C(0x808000)
  #define RESIDENT_MANIFEST_ADDRESS        UINT32_C(0x87F000)
  #define RESIDENT_FLASH_END_ADDRESS       UINT32_C(0x880000)
  #define RESIDENT_LAYOUT_ID               UINT32_C(0x53414B31) /* "SAK1" */
#elif defined(__dsPIC33AK128MC106__)
  #define RESIDENT_BOOT_BASE_ADDRESS       UINT32_C(0x800000)
  #define RESIDENT_BOOT_SIZE_BYTES         UINT32_C(0x004000)
  #define RESIDENT_APP_BASE_ADDRESS        UINT32_C(0x804000)
  #define RESIDENT_MANIFEST_ADDRESS        UINT32_C(0x81F000)
  #define RESIDENT_FLASH_END_ADDRESS       UINT32_C(0x820000)
  #define RESIDENT_LAYOUT_ID               UINT32_C(0x53414B33) /* "SAK3"; "SAK2" = the retired 28 KiB layout */
#else
  #error "resident_de_manifest.h: unsupported device -- expects __dsPIC33AK512MPS512__ or __dsPIC33AK128MC106__."
#endif

#define RESIDENT_APP_CAPACITY_BYTES      (RESIDENT_MANIFEST_ADDRESS - RESIDENT_APP_BASE_ADDRESS)
#define RESIDENT_MANIFEST_FORMAT_VERSION UINT16_C(1)
#define RESIDENT_MANIFEST_MAGIC_BYTES    { 'S', 'O', 'N', 'O', 'R', 'A', '1', '\0' }
#define RESIDENT_MANIFEST_MAGIC_WORD0    UINT32_C(0x4F4E4F53) /* "SONO" */
#define RESIDENT_MANIFEST_MAGIC_WORD1    UINT32_C(0x00314152) /* "RA1\0" */

typedef struct __attribute__((packed))
{
    uint8_t  magic[8];
    uint16_t format_version;
    uint16_t header_size;
    uint32_t layout_id;
    uint32_t app_base;
    uint32_t payload_length;
    uint32_t payload_crc32;
    uint32_t entry_address;
    uint32_t ivt_address;
    uint32_t firmware_version;
    uint32_t flags;
    uint8_t  reserved[16];
    uint32_t header_crc32;
} resident_boot_manifest_t;

_Static_assert(sizeof(resident_boot_manifest_t) == 64u,
               "resident boot manifest wire size must remain 64 bytes");

_Static_assert((RESIDENT_BOOT_SIZE_BYTES % UINT32_C(4096)) == 0u,
               "bootloader region must end on an erase-page boundary");
_Static_assert((RESIDENT_APP_CAPACITY_BYTES % UINT32_C(16)) == 0u,
               "application region must contain complete Flash words");
_Static_assert((RESIDENT_FLASH_END_ADDRESS - RESIDENT_MANIFEST_ADDRESS) == UINT32_C(4096),
               "the manifest must occupy exactly the final erase page of the panel");

#endif
