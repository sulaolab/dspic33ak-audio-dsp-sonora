#ifndef RESIDENT_BOOTLOADER_H
#define RESIDENT_BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "resident_de_manifest.h"

/* Re-verify the whole installed application with the software CRC32 on every boot.
 *
 * OFF by design, not as an optimisation. Guaranteeing the App section is the App's
 * own responsibility; this image exists to offer a download path when the App
 * cannot -- plus the intentional update path. Checking the App on every boot is work
 * that does not belong here, and it is not free: measured 1.29 s for a 167 KB image
 * and 3.34 s for a 430 KB one, purely length-proportional, because this image runs
 * at FCY 4 MHz with a bitwise CRC over byte-at-a-time Flash reads.
 *
 * It is also nearly redundant. receive_and_install() erases the manifest page before
 * accepting a single payload byte and commits it only after reading the programmed
 * Flash back and matching its CRC, so a valid manifest already proves a complete,
 * verified install and an interrupted one leaves an erased manifest that the cheap
 * header check rejects. What is left -- Flash damage after a good install -- is
 * covered by the payload ECC preflight that always runs, and by the launch guard,
 * which is the mechanism actually designed for "the App will not run".
 *
 * Turn it on (-DRESIDENT_BOOT_ENA_BOOT_PAYLOAD_CRC=1) only to trade that boot time
 * for detection of silent, ECC-correctable payload rot. */
#if !defined(RESIDENT_BOOT_ENA_BOOT_PAYLOAD_CRC)
#define RESIDENT_BOOT_ENA_BOOT_PAYLOAD_CRC 0
#endif

/* Print the bring-up forensics: entry clock/mailbox/pre-CRT/reset-source registers,
 * the default-vector record, the CPU dump and the per-step manifest check.
 *
 * OFF by default because this image has a hard 32 KiB ceiling and the static strings
 * alone were spending a few hundred bytes of it. Normal operation needs only the
 * boot banner, the reset-cause word and the outcome lines, all of which stay
 * unconditional. Turn it on (-DRESIDENT_BOOT_ENA_BOOT_TRACE=1) when a boot has to be
 * explained rather than merely observed; the code is gated, not deleted. */
#if !defined(RESIDENT_BOOT_ENA_BOOT_TRACE)
#define RESIDENT_BOOT_ENA_BOOT_TRACE 0
#endif

typedef enum {
    RESIDENT_BOOT_INSTALL_OK = 0,
    RESIDENT_BOOT_INSTALL_TRANSFER,
    RESIDENT_BOOT_INSTALL_HEADER,
    RESIDENT_BOOT_INSTALL_RANGE,
    RESIDENT_BOOT_INSTALL_ERASE,
    RESIDENT_BOOT_INSTALL_PROGRAM,
    RESIDENT_BOOT_INSTALL_VERIFY,
    RESIDENT_BOOT_INSTALL_CRC,
    RESIDENT_BOOT_INSTALL_LENGTH
} resident_boot_install_status_t;

bool resident_boot_manifest_validate(const resident_boot_manifest_t *manifest);
bool resident_boot_installed_manifest(resident_boot_manifest_t *manifest);
resident_boot_install_status_t resident_boot_receive_and_install(void);

#endif
