#ifndef NOINIT_RAM_CONFIG_H
#define NOINIT_RAM_CONFIG_H

//===========================================================
// noinit_ram_config.h -- THIS PROJECT's answer to hal_noinit_ram's three questions.
//
// The HAL supplies no defaults, because where a shared block lives and how big it is are
// memory-map decisions, not library decisions. This file is where this project states
// them, and it is the only place they are stated: the linker script
// (linker/p33AK512MPS512_serial_update_app.gld) reserves the same range, and
// buildtools/build.ps1 asserts the two agree against the map after every link.
//
// Both images -- the resident bootloader and the application -- compile this file, which
// is what makes them agree on the address.
//===========================================================

//-----------------------------------------------------------------------
// WHERE. 0x13F30..0x13FFF is the tail of the 512-byte reset-diagnostic reservation at
// the top of data RAM, which used to be pure guard padding (.resident_diag_guard existed
// only so the best-fit allocator could not hand the range to the automatic stack).
//
// Taking it costs NO additional RAM, which is the reason it was chosen: the data region
// is 0x4000..0x13FFF and the stack already fills everything below the reservation
// (measured at 0x1027C..0x13DFF in the ASRC serial-update build, butting straight up
// against it). Anywhere else would have come out of stack headroom.
//
// EXTENDED DOWNWARD 2026-08-11, from 0x13F80/128 to 0x13F30/208, to hold the AK trap
// record as well. The direction matters and was not free to choose: the block ends at
// the top of RAM, so the only room is below it. The RESIDENT PIPE STAYS AT 0x13F80 --
// it is now at OFFSET 0x50 within the block instead of offset 0, and the extension is
// the new low 80 bytes (see noinit_ram_layout.h). That keeps the one address the two
// images have to agree on exactly where it was, so an application image built from this
// branch and a resident bootloader built before it still find the pipe in the same place.
//
// .resident_diag_guard is GONE, not shrunk: the block now occupies the whole
// 0x13F30..0x13FFF range, which was the guard's only job.
//
// PER DEVICE, 2026-08-12. The block is no longer a serial-update-only facility: every
// configuration compiles app_traps.c, so every configuration needs the block, including
// the AK128 one. The address is therefore chosen per device, and it is the SAME decision
// on both -- top of data RAM minus 0xD0, which on both devices is Y memory:
//
//   AK512MPS512   data 0x4000..0x13FFF   Y 0xC000..0x13FFF   block 0x13F30..0x13FFF
//   AK128MC106    data 0x4000..0x7FFF    Y 0x6000..0x7FFF    block 0x7F30..0x7FFF
//
// A non-resident image gets the range reserved by a small supplementary linker script
// (linker/p33AK*_noinit_ram_reserve.ld) added to the device default, NOT by replacing that
// default -- see the long comment in the AK512 one. A serial-update image gets it from
// linker/p33AK512MPS512_serial_update_app.gld as before.
//-----------------------------------------------------------------------
#if defined(__dsPIC33AK512MPS512__)
#  define NORA_NOINIT_RAM_ADDRESS 0x00013F30
#elif defined(__dsPIC33AK128MC106__)
#  define NORA_NOINIT_RAM_ADDRESS 0x00007F30
#else
#  error "noinit_ram_config.h: unknown device. Choose an address at the top of THIS device's data RAM, inside Y memory, and reserve it in the linker script this device's configurations use -- an out-of-region address() links silently and faults at run time (measured)."
#endif

//-----------------------------------------------------------------------
// HOW BIG. 208 bytes -- the documented maximum, i.e. all of the former guard. 128 of them
// are the resident reservation as before (pipe at offset 0x50, with room to grow into the
// tail); the low 80 are the trap record.
//
// This is now the ceiling. Growing further means taking stack headroom or moving the
// block, and BOTH the linker script and the two build assertions
// (buildtools/build.ps1, buildtools/build_resident_bootloader.ps1) pin this address and
// size against the map after every link -- they must move with it.
//-----------------------------------------------------------------------
#define NORA_NOINIT_RAM_SIZE 208u

//-----------------------------------------------------------------------
// WHICH DATA SPACE. The high data addresses on this part are Y memory -- the resident
// diagnostic sections next door (0x13E00+) are declared space(ymemory), and
// build_resident_bootloader.ps1 asserts the YMEMORY flag on each of them. The block must
// match its neighbours or the same check would reject it.
//-----------------------------------------------------------------------
#define NORA_NOINIT_RAM_SPACE ymemory

//-----------------------------------------------------------------------
// THE PROMISE. The HAL cannot read the linker script, so it refuses to build until this
// says the range really is reserved there. Three things only the linker script can do,
// each of which fails silently:
//   KEEP        or --gc-sections discards the block in an image that declares but never
//               references it (measured, not theoretical)
//   (NOLOAD)    keeps it out of .dinit; without it crt0 re-initializes the block every
//               boot and the module quietly degrades to "always cold"
//   guard fill  stops the stack being placed there -- neither compiler nor linker warns
//-----------------------------------------------------------------------
#define NORA_NOINIT_RAM_LINKER_RESERVED 1

//-----------------------------------------------------------------------
// WHY THE DEVICE IS CHECKED AT ALL, above and not somewhere later: an out-of-region
// address() does NOT fail the link. Measured -- the resident_boot_request.c regions at
// 0x13E00+ are reported as placed there in an AK128 build even though that RAM does not
// exist. Nothing downstream catches it, so an unknown device has to stop the build here,
// where the address is chosen, rather than produce an image that faults on a board.
//
// This used to be a blanket "#if !defined(__dsPIC33AK512MPS512__) #error" placed after a
// single hard-coded address. It is now the #else of the per-device selection above, which
// is the same guarantee for any number of devices instead of exactly one.
//-----------------------------------------------------------------------

#endif // NOINIT_RAM_CONFIG_H
