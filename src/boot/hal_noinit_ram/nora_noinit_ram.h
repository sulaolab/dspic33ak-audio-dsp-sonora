#ifndef NORA_NOINIT_RAM_H
#define NORA_NOINIT_RAM_H

//===========================================================
// nora_noinit_ram.{c,h} -- reserve ONE block of RAM that
//   (1) sits at an address YOU choose, so two separately-linked images
//       (the resident bootloader and the application) can both reach the same
//       bytes, and
//   (2) the C runtime start-up does NOT initialize, so those bytes keep their
//       values across a reset.
//
// That is the whole job. This HAL hands you a lump of RAM and gets out of the way.
//
// WHAT THIS HAL DOES NOT DO -- on purpose
//   It does not know, and does not want to know, what you put in there. No magic
//   number, no version field, no length, no checksum, no "is it valid" question.
//   Those belong to whatever you build INSIDE the block: share a struct between the
//   bootloader and the application, and let that struct carry whatever framing your
//   protocol needs. The HAL reserves the pipe; you decide what flows through it.
//
//   Keeping the payload out of the HAL is what lets the same reservation serve a
//   command mailbox, a duplicate sentinel, and a register trace that a stackless
//   pre-CRT assembly stub writes by hard-coded field offset. A HAL that imposed its
//   own header would fight every one of those.
//
// NAME vs ATTRIBUTE -- read this before grepping.
//   The generic embedded term for "RAM the C runtime deliberately does not
//   initialize" is a .noinit section, and that is what this module is named after.
//   On XC-DSC the equivalent spelling is __attribute__((persistent)); there is no
//   section literally called .noinit here. So: grep for "persistent", not ".noinit".
//   The name says WHAT the memory is; the attribute is this toolchain's way of asking.
//
// WHY "_ram" IS IN THE NAME
//   Because losing it on power-off is correct behaviour, not a bug. This is
//   volatile-cell persistence, NOT non-volatile storage. The cells keep their bits
//   across a warm reset (MCLR / software / watchdog) because they never lost power;
//   after a real power cycle their contents are INDETERMINATE. Names like "retained"
//   or "backup" are used elsewhere for battery-backed domains that DO survive a power
//   cycle (e.g. STM32 backup SRAM). This is not that.
//
//   So: if your container must be able to tell "this is genuine data" from "these are
//   whatever bits the cells came up with", your container needs its own validity check
//   (a magic word and a checksum is the usual answer, and the reset cause from
//   hal_reset tells you when to distrust the block entirely). The HAL will not do it
//   for you, because only you know what the data means.
//
// PORTABILITY
//   __attribute__((persistent)) and the crt0 behaviour are identical on the
//   dsPIC33AK512MPS512 (AK512) and dsPIC33AK128MC106 (AK128).
//===========================================================
//
// HOW TO USE IT
//
//   1. Reserve the range in the linker script (see 2 below and the doc -- this is the
//      part the HAL cannot do for you), then say so:
//
//        #define NORA_NOINIT_RAM_LINKER_RESERVED 1
//
//   2. Say where the block goes and how big it is (both required):
//
//        #define NORA_NOINIT_RAM_ADDRESS  0x00004050
//        #define NORA_NOINIT_RAM_SIZE     64
//
//   3. Describe the contents in a header BOTH images include, and overlay it:
//
//        typedef struct { uint32_t magic; uint32_t request; ... } my_pipe_t;
//        NORA_NOINIT_RAM_FITS( my_pipe_t );          // compile-time size check
//        volatile my_pipe_t *p = NORA_NOINIT_RAM_AS( my_pipe_t );
//
//      Both images must agree on the address, the size, and the struct. Nothing here
//      can check that the OTHER image agrees -- that is what sharing one header for
//      the struct and one for the two #defines is for.
//
//===========================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------
// Where the answers come from.
//
// The three required values are supplied by a project-owned header, so that both images
// sharing the block get them from one place and neither the HAL nor the build command
// line has to carry the memory map. A test (or a project that prefers to define them
// itself) can set them before including this header and the include is skipped.
//-----------------------------------------------------------------------
#if !defined( NORA_NOINIT_RAM_ADDRESS ) && !defined( NORA_NOINIT_RAM_SIZE )
#  if defined( __has_include )
#    if __has_include( "noinit_ram_config.h" )
#      include "noinit_ram_config.h"
#    endif
#  else
#    include "noinit_ram_config.h"
#  endif
#endif

//-----------------------------------------------------------------------
// The three things you must state. These are #errors rather than defaults because
// every one of them is a decision about the memory map, and a silent default would
// put your data somewhere you did not choose -- or, worse, somewhere the linker later
// hands to the stack.
//-----------------------------------------------------------------------

#if !defined( NORA_NOINIT_RAM_LINKER_RESERVED )
#  error "hal_noinit_ram: reserve the block in the linker script first (KEEP + NOLOAD + a guard filler so the stack cannot be placed there), then #define NORA_NOINIT_RAM_LINKER_RESERVED 1. The attribute alone does NOT protect the range -- see [internal] resident_serial_bootloader.md part 3."
#endif

#if !defined( NORA_NOINIT_RAM_ADDRESS )
#  error "hal_noinit_ram: #define NORA_NOINIT_RAM_ADDRESS to the block's start address. Both images that share the block must use the same value."
#endif

#if !defined( NORA_NOINIT_RAM_SIZE )
#  error "hal_noinit_ram: #define NORA_NOINIT_RAM_SIZE to the block's size in bytes. Both images that share the block must use the same value."
#endif

// Which data space the address lives in. On this family the low data addresses are X
// memory and the high ones are Y; the value here must match the address you chose, so
// override it if your block is in Y memory.
#if !defined( NORA_NOINIT_RAM_SPACE )
#  define NORA_NOINIT_RAM_SPACE xmemory
#endif

// The block is aligned so any struct you overlay is correctly aligned, which means the
// address you pick has to be aligned too. Caught here rather than as a puzzling link
// error or a misaligned access at run time.
#define NORA_NOINIT_RAM_ALIGN 16u
_Static_assert( ( (NORA_NOINIT_RAM_ADDRESS) % NORA_NOINIT_RAM_ALIGN ) == 0u,
                "NORA_NOINIT_RAM_ADDRESS must be 16-byte aligned" );
_Static_assert( (NORA_NOINIT_RAM_SIZE) > 0u,
                "NORA_NOINIT_RAM_SIZE must not be zero" );

//-----------------------------------------------------------------------
// The block itself.
//
// `volatile` because the bytes are shared with another image and survive a reset: the
// compiler must not assume it knows their history, cache them in a register across a
// reset, or drop a store it thinks nobody reads.
//-----------------------------------------------------------------------
extern volatile uint8_t nora_noinit_ram[ NORA_NOINIT_RAM_SIZE ];

// Overlay your shared struct on the block. Use with FITS below.
#define NORA_NOINIT_RAM_AS( type ) \
    ( (volatile type *)(void *)nora_noinit_ram )

// Compile-time guarantee that your struct fits the block you reserved. Put this next
// to the struct in the shared header, so growing the struct past the reservation is a
// build failure instead of a silent overrun into whatever follows.
#define NORA_NOINIT_RAM_FITS( type )                                  \
    _Static_assert( sizeof( type ) <= (NORA_NOINIT_RAM_SIZE),          \
                    #type " does not fit NORA_NOINIT_RAM_SIZE" )

#ifdef __cplusplus
}
#endif

#endif // NORA_NOINIT_RAM_H
