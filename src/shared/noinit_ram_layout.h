#ifndef NOINIT_RAM_LAYOUT_H
#define NOINIT_RAM_LAYOUT_H

//===========================================================
// noinit_ram_layout.h -- who owns which bytes of the hal_noinit_ram block.
//
// hal_noinit_ram deliberately offers ONE accessor, NORA_NOINIT_RAM_AS(type), which
// overlays a type on the start of the block. That is all a single-tenant block needs.
// The moment there are two tenants, the carve-up has to be stated somewhere, and it must
// be somewhere BOTH tenants and the linker script can be checked against -- not inside
// either tenant, which would make one of them the other's owner.
//
// WHY THE TRAP RECORD IS FIRST AND THE PIPE IS NOT
// -----------------------------------------------
// The block was 0x13F80..0x13FFF with the pipe at offset 0. It grew DOWNWARD to 0x13F30
// (the only direction available -- it already ended at the top of RAM), so offset 0 is
// now new ground and the pipe's old bytes are at offset 0x50.
//
// The alternative -- keeping the pipe at offset 0 and putting the trap record in the
// tail -- would have moved the pipe's ABSOLUTE address, and that address is the one thing
// the application and the resident bootloader must agree on across two separately built
// images. A mismatch there does not fail to link; it silently reads the wrong bytes, and
// the pipe's own version/complement pair is what would (correctly, uselessly) report the
// container as absent. So the layout below is chosen to leave 0x13F80 alone.
//
// COLD/WARM IS EACH TENANT'S OWN BUSINESS
// ---------------------------------------
// Both tenants validate their own region with their own magic, so neither needs the other
// to have run. resident_boot_pipe_init() no longer zeroes the whole block for the same
// reason -- see the note at its zeroing loop.
//===========================================================

#include <stdint.h>

#include "noinit_ram_config.h"
#include "hal_noinit_ram/nora_noinit_ram.h"

//-----------------------------------------------------------------------
// THE CARVE-UP. Offsets, not addresses: the HAL owns where the block starts.
// Both are 16-byte aligned so either tenant may hold a type with any alignment
// this compiler can ask for.
//-----------------------------------------------------------------------
#define NOINIT_RAM_TRAPS_OFFSET  0x00u
#define NOINIT_RAM_TRAPS_SIZE    0x50u   /* 80 bytes -- the extension */

#define NOINIT_RAM_PIPE_OFFSET   0x50u   /* absolute 0x13F80, unchanged */
#define NOINIT_RAM_PIPE_SIZE     0x80u   /* 128 bytes -- the original reservation */

_Static_assert( ( NOINIT_RAM_TRAPS_OFFSET % NORA_NOINIT_RAM_ALIGN ) == 0u,
                "the trap region must be 16-byte aligned" );
_Static_assert( ( NOINIT_RAM_PIPE_OFFSET % NORA_NOINIT_RAM_ALIGN ) == 0u,
                "the pipe region must be 16-byte aligned" );
_Static_assert( ( NOINIT_RAM_TRAPS_OFFSET + NOINIT_RAM_TRAPS_SIZE ) ==
                NOINIT_RAM_PIPE_OFFSET,
                "the two regions must be adjacent -- a hole here is RAM nobody owns" );
_Static_assert( ( NOINIT_RAM_PIPE_OFFSET + NOINIT_RAM_PIPE_SIZE ) ==
                (NORA_NOINIT_RAM_SIZE),
                "the regions must fill the block exactly; if the block grew, say who owns "
                "the new bytes here" );

/*
 * The pipe's absolute address is the cross-image contract, so it is asserted here rather
 * than left to arithmetic in two places. Each device's value is in a published document
 * ([internal] resident_serial_bootloader.md part 3), not a free parameter.
 *
 * PER DEVICE SINCE THE AK128 GAINED A RESIDENT BOOTLOADER. The address was AK512-only
 * while only that part had a second image; now both do, and the pipe is at the top of
 * whichever data RAM this part has (block + 0x50, exactly as on the AK512). The value
 * asserted is therefore per device, and the point of asserting it is unchanged: an image
 * pair that disagrees here does not fail to link. It silently reads the wrong bytes, and
 * the pipe's own version/complement pair reports the container as absent -- correctly,
 * and uselessly.
 *
 * STILL SCOPED TO THE BUILDS THAT HAVE A SECOND IMAGE. The block itself is reserved in
 * every configuration because app_traps.c needs it everywhere; a standalone image has
 * nothing to agree with, so asserting an address there would only make a build fail for
 * disagreeing with a promise nobody made. The carve-up assertions above are
 * unconditional and are what keep the two regions honest in every build.
 */
#if defined( SONORA_DELIVERY_SERIAL_UPDATE_APP )
#  if defined( __dsPIC33AK512MPS512__ )
#    define NOINIT_RAM_PIPE_PUBLISHED_ADDRESS 0x00013F80
#  elif defined( __dsPIC33AK128MC106__ )
#    define NOINIT_RAM_PIPE_PUBLISHED_ADDRESS 0x00007F80
#  else
#    error "noinit_ram_layout.h: a serial-update build must publish this device's pipe address."
#  endif
_Static_assert( ( (NORA_NOINIT_RAM_ADDRESS) + NOINIT_RAM_PIPE_OFFSET ) ==
                NOINIT_RAM_PIPE_PUBLISHED_ADDRESS,
                "the resident pipe must stay at this device's published address -- a "
                "bootloader image built before a move looks for it there" );
#endif

//-----------------------------------------------------------------------
// Typed access. Same shape as NORA_NOINIT_RAM_AS(), with the offset applied and the
// region's own size as the bound, so a type that outgrows its region is a compile error
// rather than a quiet overlap with the neighbour.
//-----------------------------------------------------------------------
#define NOINIT_RAM_REGION_AS( type, offset, size )                            \
    ( ( volatile type * )( ( volatile void * )&nora_noinit_ram[ (offset) ] ) )

#define NOINIT_RAM_REGION_FITS( type, size )                                  \
    _Static_assert( sizeof( type ) <= (size),                                 \
                    #type " does not fit its noinit_ram region" )

#define NOINIT_RAM_TRAPS_AS( type ) \
    NOINIT_RAM_REGION_AS( type, NOINIT_RAM_TRAPS_OFFSET, NOINIT_RAM_TRAPS_SIZE )
#define NOINIT_RAM_TRAPS_FITS( type ) \
    NOINIT_RAM_REGION_FITS( type, NOINIT_RAM_TRAPS_SIZE )

#define NOINIT_RAM_PIPE_AS( type ) \
    NOINIT_RAM_REGION_AS( type, NOINIT_RAM_PIPE_OFFSET, NOINIT_RAM_PIPE_SIZE )
#define NOINIT_RAM_PIPE_FITS( type ) \
    NOINIT_RAM_REGION_FITS( type, NOINIT_RAM_PIPE_SIZE )

#endif // NOINIT_RAM_LAYOUT_H
