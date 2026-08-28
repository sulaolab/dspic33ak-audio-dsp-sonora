#include "resident_de_bootloader.h"

#include <stddef.h>

#include "nora_nvm.h"
#include "resident_de_boot_crc32.h"
#include "resident_de_boot_led.h"
#include "resident_de_boot_platform.h"
#include "resident_de_boot_xmodem.h"

typedef struct {
    resident_boot_manifest_t manifest;
    uint32_t wire_offset;
    uint32_t payload_written;
    uint32_t payload_crc_state;
    uint32_t next_page;
    uint32_t row_address;
    uint32_t row_buffer[NORA_NVM_U32_PER_ROW] __attribute__((aligned(4)));
    uint16_t row_fill;
    resident_boot_install_status_t error;
    bool header_valid;
    bool manifest_invalidated;
    bool payload_complete;
} receive_context_t;

static bool bytes_equal(const uint8_t *left, const uint8_t *right, uint32_t length)
{
    uint32_t index;
    for (index = 0u; index < length; index++) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void copy_bytes(uint8_t *target, const uint8_t *source, uint32_t length)
{
    uint32_t index;
    for (index = 0u; index < length; index++) {
        target[index] = source[index];
    }
}

static __attribute__((always_inline)) inline uint8_t
read_flash_byte(const volatile uint8_t *source)
{
    uint32_t value;

    /* Force a single-register EA for a program-Flash data read. XC-DSC 3.31
     * otherwise folds the surrounding copy loop into an indexed EA of the form
     * -target+(Flash+target); that final instruction traps on dsPIC33AK even
     * though the algebraic result is a valid Flash address. */
    __asm__ volatile ("mov.bz [%1], %0"
                      : "=r"(value)
                      : "r"(source)
                      : "memory");
    return (uint8_t)value;
}

static void read_flash(uint32_t address, uint8_t *target, uint32_t length)
{
    const volatile uint8_t *source = (const volatile uint8_t *)(uintptr_t)address;
    uint32_t index;
    for (index = 0u; index < length; index++) {
        target[index] = read_flash_byte(source + index);
    }
}

static uint32_t flash_crc32(uint32_t address, uint32_t length)
{
    uint8_t block[128];
    uint32_t state = RESIDENT_BOOT_CRC32_INITIAL;

    while (length != 0u) {
        const uint32_t count = (length > sizeof(block)) ? sizeof(block) : length;
        read_flash(address, block, count);
        state = resident_boot_crc32_update(state, block, count);
        address += count;
        length -= count;
    }
    return resident_boot_crc32_finish(state);
}

bool resident_boot_manifest_validate(const resident_boot_manifest_t *manifest)
{
    static const uint8_t magic[8] = RESIDENT_MANIFEST_MAGIC_BYTES;
    uint32_t index;

    if ((manifest == NULL) || !bytes_equal(manifest->magic, magic, sizeof(magic)) ||
        (manifest->format_version != RESIDENT_MANIFEST_FORMAT_VERSION) ||
        (manifest->header_size != sizeof(*manifest)) ||
        (manifest->layout_id != RESIDENT_LAYOUT_ID) ||
        (manifest->app_base != RESIDENT_APP_BASE_ADDRESS) ||
        (manifest->payload_length == 0u) ||
        (manifest->payload_length > RESIDENT_APP_CAPACITY_BYTES) ||
        ((manifest->payload_length & (NORA_NVM_WORD_BYTES - 1u)) != 0u) ||
        (manifest->ivt_address != RESIDENT_APP_BASE_ADDRESS) ||
        (manifest->entry_address < RESIDENT_APP_BASE_ADDRESS) ||
        (manifest->entry_address >=
            (RESIDENT_APP_BASE_ADDRESS + manifest->payload_length)) ||
        (manifest->flags != 0u)) {
        return false;
    }
    for (index = 0u; index < sizeof(manifest->reserved); index++) {
        if (manifest->reserved[index] != 0u) {
            return false;
        }
    }
    return manifest->header_crc32 ==
           resident_boot_crc32(manifest, sizeof(*manifest) - sizeof(uint32_t));
}

bool resident_boot_installed_manifest(resident_boot_manifest_t *manifest)
{
    if (manifest == NULL) {
        return false;
    }
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("Manifest check: preflight page\r\n");
#endif
    /* A cold reset can leave an interrupted manifest erase/program with invalid
     * ECC. Preflight through the NVM CRC engine so the CPU does not take a DED
     * trap while merely deciding to enter recovery. */
    if (nora_nvm_crc_preflight(RESIDENT_MANIFEST_ADDRESS,
                               NORA_NVM_PAGE_BYTES) !=
        NORA_NVM_OK) {
        return false;
    }
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("Manifest check: read header\r\n");
#endif
    read_flash(RESIDENT_MANIFEST_ADDRESS,
               (uint8_t *)manifest,
               sizeof(*manifest));
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("Manifest check: validate header\r\n");
#endif
    if (!resident_boot_manifest_validate(manifest)) {
        return false;
    }
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("Manifest check: preflight payload\r\n");
#endif
    /* A valid manifest already attests a complete, readback-verified install, so this
     * is not re-checking the install: it is the cheap detector for uncorrectable ECC
     * damage in the image about to be launched, and it keeps the CPU from taking a DED
     * trap here rather than in the App. Costs 17 ms for 167 KB, 49 ms for 430 KB. */
    if (nora_nvm_crc_preflight(manifest->app_base,
                               manifest->payload_length) !=
        NORA_NVM_OK) {
        return false;
    }
#if RESIDENT_BOOT_ENA_BOOT_PAYLOAD_CRC
    /* Seconds, not milliseconds -- see the switch comment in resident_bootloader.h. */
#if RESIDENT_BOOT_ENA_BOOT_TRACE
    resident_boot_platform_write("Manifest check: payload CRC\r\n");
#endif
    return flash_crc32(manifest->app_base, manifest->payload_length) ==
           manifest->payload_crc32;
#else
    return true;
#endif
}

static bool erase_allowed_page(uint32_t page_address)
{
    if (!nora_nvm_is_page_aligned(page_address)) {
        return false;
    }
    return (page_address == RESIDENT_MANIFEST_ADDRESS) ||
           ((page_address >= RESIDENT_APP_BASE_ADDRESS) &&
            (page_address < RESIDENT_MANIFEST_ADDRESS));
}

static bool program_allowed_word(uint32_t word_address,
                                 uint32_t allowed_start,
                                 uint32_t allowed_end)
{
    return (allowed_end > allowed_start) &&
           nora_nvm_is_word_aligned(word_address) &&
           (word_address >= allowed_start) &&
           (word_address <= (allowed_end - NORA_NVM_WORD_BYTES));
}

static resident_boot_install_status_t erase_page(uint32_t address)
{
    if (!erase_allowed_page(address)) {
        return RESIDENT_BOOT_INSTALL_RANGE;
    }
    return (nora_nvm_page_erase(address) == NORA_NVM_OK)
        ? RESIDENT_BOOT_INSTALL_OK
        : RESIDENT_BOOT_INSTALL_ERASE;
}

static resident_boot_install_status_t program_word(uint32_t address,
                                                    const uint8_t *source,
                                                    uint32_t allowed_start,
                                                    uint32_t allowed_end)
{
    uint32_t words[NORA_NVM_U32_PER_WORD] __attribute__((aligned(4)));

    if (!program_allowed_word(address, allowed_start, allowed_end)) {
        return RESIDENT_BOOT_INSTALL_RANGE;
    }
    copy_bytes((uint8_t *)words, source, NORA_NVM_WORD_BYTES);
    if (nora_nvm_word_program(address, words) != NORA_NVM_OK) {
        return RESIDENT_BOOT_INSTALL_PROGRAM;
    }
    if (nora_nvm_verify(address, words, NORA_NVM_WORD_BYTES) !=
        NORA_NVM_OK) {
        return RESIDENT_BOOT_INSTALL_VERIFY;
    }
    return RESIDENT_BOOT_INSTALL_OK;
}

static resident_boot_install_status_t prepare_page(receive_context_t *context,
                                                   uint32_t address)
{
    if (address == context->next_page) {
        const resident_boot_install_status_t status = erase_page(address);
        if (status != RESIDENT_BOOT_INSTALL_OK) {
            return status;
        }
        context->next_page += NORA_NVM_PAGE_BYTES;
    } else if (address > context->next_page) {
        return RESIDENT_BOOT_INSTALL_RANGE;
    }
    return RESIDENT_BOOT_INSTALL_OK;
}

static resident_boot_install_status_t program_full_row(receive_context_t *context)
{
    resident_boot_install_status_t status;
    const uint32_t payload_end = context->manifest.app_base +
                                 context->manifest.payload_length;

    if ((context->row_fill != NORA_NVM_ROW_BYTES) ||
        !nora_nvm_is_row_aligned(context->row_address) ||
        (context->row_address < context->manifest.app_base) ||
        (context->row_address > (payload_end - NORA_NVM_ROW_BYTES))) {
        return RESIDENT_BOOT_INSTALL_RANGE;
    }
    status = prepare_page(context, context->row_address);
    if (status != RESIDENT_BOOT_INSTALL_OK) {
        return status;
    }
    if (nora_nvm_row_program(context->row_address,
                             context->row_buffer) != NORA_NVM_OK) {
        return RESIDENT_BOOT_INSTALL_PROGRAM;
    }
    if (nora_nvm_verify(context->row_address,
                        context->row_buffer,
                        NORA_NVM_ROW_BYTES) != NORA_NVM_OK) {
        return RESIDENT_BOOT_INSTALL_VERIFY;
    }
    context->row_address += NORA_NVM_ROW_BYTES;
    context->row_fill = 0u;
    return RESIDENT_BOOT_INSTALL_OK;
}

static resident_boot_install_status_t program_partial_row(receive_context_t *context)
{
    uint32_t offset;
    resident_boot_install_status_t status;
    const uint32_t payload_end = context->manifest.app_base +
                                 context->manifest.payload_length;

    if (context->row_fill == 0u) {
        return RESIDENT_BOOT_INSTALL_OK;
    }
    if ((context->row_fill & (NORA_NVM_WORD_BYTES - 1u)) != 0u) {
        return RESIDENT_BOOT_INSTALL_LENGTH;
    }
    status = prepare_page(context, context->row_address);
    if (status != RESIDENT_BOOT_INSTALL_OK) {
        return status;
    }
    for (offset = 0u; offset < context->row_fill;
         offset += NORA_NVM_WORD_BYTES) {
        status = program_word(context->row_address + offset,
                              ((const uint8_t *)context->row_buffer) + offset,
                              context->manifest.app_base,
                              payload_end);
        if (status != RESIDENT_BOOT_INSTALL_OK) {
            return status;
        }
    }
    context->row_address += context->row_fill;
    context->row_fill = 0u;
    return RESIDENT_BOOT_INSTALL_OK;
}

static int receive_sink(uint32_t offset,
                        const uint8_t *data,
                        uint16_t length,
                        void *opaque)
{
    receive_context_t *context = (receive_context_t *)opaque;
    uint32_t cursor = 0u;

    if ((context == NULL) || (data == NULL) ||
        (offset != context->wire_offset) || context->payload_complete) {
        return -1;
    }

    if (!context->header_valid) {
        resident_boot_install_status_t status;

        if (length < sizeof(context->manifest)) {
            context->error = RESIDENT_BOOT_INSTALL_HEADER;
            return -1;
        }
        copy_bytes((uint8_t *)&context->manifest,
                   data,
                   sizeof(context->manifest));
        if (!resident_boot_manifest_validate(&context->manifest)) {
            context->error = RESIDENT_BOOT_INSTALL_HEADER;
            return -1;
        }
        /* Keep a previously installed App bootable while forced recovery is
         * merely waiting. The first valid package header is the irreversible
         * update intent: invalidate the old manifest before accepting or
         * programming any new payload bytes. */
        status = erase_page(RESIDENT_MANIFEST_ADDRESS);
        if (status != RESIDENT_BOOT_INSTALL_OK) {
            context->error = status;
            return -1;
        }
        context->manifest_invalidated = true;
        context->header_valid = true;
        /* A real transfer just started (a fresh, validated header): clear any
         * bar left over from an idle handshake round or a prior failed
         * attempt. Not done unconditionally per call -- see resident_de_boot_led.h. */
        resident_boot_led_progress_reset();
        cursor = sizeof(context->manifest);
    }

    while ((cursor < length) &&
           (context->payload_written < context->manifest.payload_length)) {
        const uint32_t block_remaining = (uint32_t)length - cursor;
        const uint32_t payload_remaining = context->manifest.payload_length -
                                           context->payload_written;
        const uint32_t row_remaining = NORA_NVM_ROW_BYTES -
                                       context->row_fill;
        uint32_t count = (block_remaining < payload_remaining)
            ? block_remaining
            : payload_remaining;
        resident_boot_install_status_t status;

        if (count > row_remaining) {
            count = row_remaining;
        }
        copy_bytes(((uint8_t *)context->row_buffer) + context->row_fill,
                   &data[cursor],
                   count);
        context->payload_crc_state = resident_boot_crc32_update(
            context->payload_crc_state,
            &data[cursor],
            count);
        context->row_fill = (uint16_t)(context->row_fill + count);
        context->payload_written += count;
        cursor += count;

        if (context->row_fill == NORA_NVM_ROW_BYTES) {
            status = program_full_row(context);
            if (status != RESIDENT_BOOT_INSTALL_OK) {
                context->error = status;
                return -1;
            }
        }
    }

    context->payload_complete =
        (context->payload_written == context->manifest.payload_length);
    context->wire_offset += length;
    /* One notification per block, after its bytes are already in Flash --
     * the display reflects what was programmed, not just what arrived. */
    resident_boot_led_progress(context->payload_written, context->manifest.payload_length);
    return 0;
}

static resident_boot_install_status_t commit_manifest(
    const resident_boot_manifest_t *manifest)
{
    const uint8_t *source = (const uint8_t *)manifest;
    uint32_t offset;

    for (offset = 0u; offset < sizeof(*manifest); offset += NORA_NVM_WORD_BYTES) {
        const resident_boot_install_status_t status = program_word(
            RESIDENT_MANIFEST_ADDRESS + offset,
            source + offset,
            RESIDENT_MANIFEST_ADDRESS,
            RESIDENT_MANIFEST_ADDRESS + sizeof(*manifest));
        if (status != RESIDENT_BOOT_INSTALL_OK) {
            return status;
        }
    }
    return RESIDENT_BOOT_INSTALL_OK;
}

resident_boot_install_status_t resident_boot_receive_and_install(void)
{
    static receive_context_t context;
    const resident_xmodem_io_t io = {
        .read = resident_boot_platform_read,
        .write = resident_boot_platform_write_byte,
        .flush = resident_boot_platform_flush,
    };
    resident_xmodem_status_t transfer_status;
    resident_boot_install_status_t status;
    uint32_t received = 0u;

    context.wire_offset = 0u;
    context.payload_written = 0u;
    context.payload_crc_state = RESIDENT_BOOT_CRC32_INITIAL;
    context.next_page = RESIDENT_APP_BASE_ADDRESS;
    context.row_address = RESIDENT_APP_BASE_ADDRESS;
    context.row_fill = 0u;
    context.error = RESIDENT_BOOT_INSTALL_OK;
    context.header_valid = false;
    context.manifest_invalidated = false;
    context.payload_complete = false;

    transfer_status = resident_xmodem_receive(&io,
                                               receive_sink,
                                               &context,
                                               &received);
    (void)received;
    if (transfer_status != RESIDENT_XMODEM_OK) {
        return (context.error != RESIDENT_BOOT_INSTALL_OK)
            ? context.error
            : RESIDENT_BOOT_INSTALL_TRANSFER;
    }
    if (!context.header_valid || !context.manifest_invalidated ||
        !context.payload_complete) {
        return RESIDENT_BOOT_INSTALL_LENGTH;
    }
    if (resident_boot_crc32_finish(context.payload_crc_state) !=
        context.manifest.payload_crc32) {
        return RESIDENT_BOOT_INSTALL_CRC;
    }
    status = program_partial_row(&context);
    if (status != RESIDENT_BOOT_INSTALL_OK) {
        return status;
    }
    if (flash_crc32(context.manifest.app_base,
                    context.manifest.payload_length) !=
        context.manifest.payload_crc32) {
        return RESIDENT_BOOT_INSTALL_VERIFY;
    }

    /* This is the commit point: write the manifest only after full verification. */
    status = commit_manifest(&context.manifest);
    if (status != RESIDENT_BOOT_INSTALL_OK) {
        return status;
    }
    return RESIDENT_BOOT_INSTALL_OK;
}
