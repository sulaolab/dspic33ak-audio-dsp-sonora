//===========================================================
// Positive host test for hal_noinit_ram.
//
// The HAL is a compile-time module: it reserves RAM and offers no runtime logic. So
// what there is to test is the CONTRACT -- that a correctly configured include
// compiles clean and the two helper macros behave, and (in test_reject.c) that every
// way of getting it wrong fails the build instead of producing a silent surprise.
//
// The .c file cannot be built on the host: persistent / space / address are XC-DSC
// attributes. Its placement is verified on the target from the linker map -- see
// [internal] resident_serial_bootloader.md part 3 section 6.2.
//===========================================================

#define NORA_NOINIT_RAM_LINKER_RESERVED 1
#define NORA_NOINIT_RAM_ADDRESS         0x00013F30
#define NORA_NOINIT_RAM_SIZE            64

#include "nora_noinit_ram.h"

#include <stdio.h>
#include <string.h>

// The point of the design: the HAL knows nothing about this struct. Both images would
// share this declaration in one header; here it stands in for that shared contract.
typedef struct
{
    uint32_t magic;
    uint32_t request;
    uint32_t argument;
    uint32_t check;
} example_pipe_t;

NORA_NOINIT_RAM_FITS( example_pipe_t );

// The host has no linker-placed block, so stand in for it. On the target this symbol
// is the real reservation in nora_noinit_ram_dspic33ak.c.
volatile uint8_t nora_noinit_ram[ NORA_NOINIT_RAM_SIZE ];

// The block is exactly the size that was asked for -- a compile-time fact, so assert it
// as one rather than branching on a constant at run time.
_Static_assert( sizeof( nora_noinit_ram ) == NORA_NOINIT_RAM_SIZE,
                "the block is not NORA_NOINIT_RAM_SIZE bytes" );

int main( void )
{
    volatile example_pipe_t *pipe = NORA_NOINIT_RAM_AS( example_pipe_t );
    int failures = 0;

    // The overlay must land on the block, not on a copy of it.
    if( (const volatile void *)pipe != (const volatile void *)nora_noinit_ram )
    {
        printf( "FAIL: AS() does not point at the block\n" );
        failures++;
    }

    // A round trip through the overlay, which is all the HAL promises: bytes in, bytes
    // out, no interpretation.
    pipe->magic   = 0x50535431u;
    pipe->request = 0xA5u;
    if( ( pipe->magic != 0x50535431u ) || ( pipe->request != 0xA5u ) )
    {
        printf( "FAIL: overlay round trip\n" );
        failures++;
    }

    if( failures != 0 )
    {
        printf( "hal_noinit_ram header: %d FAILURE(S)\n", failures );
        return 1;
    }
    printf( "hal_noinit_ram header: PASS\n" );
    return 0;
}
