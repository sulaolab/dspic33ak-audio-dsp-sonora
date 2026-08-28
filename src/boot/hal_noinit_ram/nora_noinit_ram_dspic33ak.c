//===========================================================
// nora_noinit_ram_dspic33ak.c -- the one no-init RAM block.
//
// There is no logic here, and that is the design: this HAL reserves RAM and knows
// nothing about what goes in it. See nora_noinit_ram.h for the contract and
// [internal] resident_serial_bootloader.md part 3 for the linker-script side.
//===========================================================

#include "nora_noinit_ram.h"

//-----------------------------------------------------------------------
// The block. Four attributes, each load-bearing:
//
//   persistent  crt0 neither zero-fills nor reloads this section, so the bytes carry
//               across a warm reset. This is the whole reason the module exists.
//   address     pins the block, so a separately-linked image can reach the same bytes
//               by agreeing on the number rather than by sharing a symbol. Without
//               this, the linker would place it wherever it liked and only one image
//               could ever use it.
//   space       which data space the address lives in; must match the address.
//   aligned     so any struct overlaid on the block is properly aligned.
//
// NOT here, because a C attribute cannot express them -- they are linker-script work,
// and skipping them fails silently:
//   KEEP        or --remove-unused-sections discards the block in an image that
//               declares it but never references it (measured, not theoretical).
//   (NOLOAD)    keeps it out of .dinit.
//   guard fill  stops the best-fit allocator handing the range to the stack. Neither
//               the compiler nor the linker warns about that one.
//-----------------------------------------------------------------------
volatile uint8_t nora_noinit_ram[ NORA_NOINIT_RAM_SIZE ]
    __attribute__(( persistent,
                    space( NORA_NOINIT_RAM_SPACE ),
                    section( ".noinit_ram" ),
                    address( NORA_NOINIT_RAM_ADDRESS ),
                    aligned( NORA_NOINIT_RAM_ALIGN ) ));
