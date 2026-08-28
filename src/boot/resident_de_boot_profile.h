/*
 * resident_de_boot_profile.h
 * --------------------------
 * Compile-time profiles private to the resident bootloader.
 *
 * boot_image.psd1 intentionally supplies SONORA_RESIDENT_BOOTLOADER to every
 * resident device configuration.  Derive the device-specific name here from
 * that common build identity and the compiler-selected device, rather than
 * adding an AK128-only compiler flag to the shared manifest.
 */
#ifndef RESIDENT_DE_BOOT_PROFILE_H
#define RESIDENT_DE_BOOT_PROFILE_H

#if defined(SONORA_RESIDENT_BOOTLOADER) && defined(__dsPIC33AK128MC106__)
#define SONORA_RESIDENT_BOOTLOADER_AK128 1
#else
#define SONORA_RESIDENT_BOOTLOADER_AK128 0
#endif

#endif /* RESIDENT_DE_BOOT_PROFILE_H */
