#ifndef RESIDENT_BOOT_PLATFORM_H
#define RESIDENT_BOOT_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

bool resident_boot_platform_init(void);
bool resident_boot_platform_recovery_button_pressed(void);
uint32_t resident_boot_platform_millis(void);
bool resident_boot_platform_read(uint8_t *data, uint32_t timeout_ms);
void resident_boot_platform_write_byte(uint8_t data);
void resident_boot_platform_write(const char *text);
void resident_boot_platform_flush(void);
void resident_boot_platform_jump(uint32_t ivt_address, uint32_t entry_address)
    __attribute__((noreturn));
void resident_boot_platform_jump_early(uint32_t ivt_address, uint32_t entry_address)
    __attribute__((noreturn));
void resident_boot_platform_launch_reset(uint32_t ivt_address,
                                         uint32_t entry_address)
    __attribute__((noreturn));
void resident_boot_platform_reset(void) __attribute__((noreturn));

#endif
