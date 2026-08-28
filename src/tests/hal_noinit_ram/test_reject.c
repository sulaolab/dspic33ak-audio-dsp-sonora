//===========================================================
// Negative host tests for hal_noinit_ram: every one of these MUST fail to compile.
//
// This is the whole safety story of a placement-only HAL. It has no runtime checks to
// catch a mistake later -- if a misconfiguration slips through the build, the result is
// data quietly living in the wrong place, or in memory the linker also gave to the
// stack. So each way of getting it wrong has to be a build failure, and that is worth
// testing as deliberately as any behaviour.
//
// The runner compiles this file once per REJECT_CASE and requires a non-zero exit.
// Same pattern as tests/hal_ccp_input_capture, which requires AK128 to fail without
// its explicit opt-in.
//===========================================================

#if REJECT_CASE == 1
// No acknowledgement that the linker script reserves the range. This is the one the
// HAL cannot verify for itself, so refusing to build without the promise is all it can
// do -- see rule 3 in the header.
#  define NORA_NOINIT_RAM_ADDRESS 0x00013F30
#  define NORA_NOINIT_RAM_SIZE    64

#elif REJECT_CASE == 2
// No address. A default here would put shared data at an address neither image chose.
#  define NORA_NOINIT_RAM_LINKER_RESERVED 1
#  define NORA_NOINIT_RAM_SIZE            64

#elif REJECT_CASE == 3
// No size. A default would silently decide how much RAM the block costs.
#  define NORA_NOINIT_RAM_LINKER_RESERVED 1
#  define NORA_NOINIT_RAM_ADDRESS         0x00013F30

#elif REJECT_CASE == 4
// Misaligned address: the block is aligned(16) so anything overlaid on it is aligned,
// which the chosen address has to allow.
#  define NORA_NOINIT_RAM_LINKER_RESERVED 1
#  define NORA_NOINIT_RAM_ADDRESS         0x00013F31
#  define NORA_NOINIT_RAM_SIZE            64

#elif REJECT_CASE == 5
// Zero size.
#  define NORA_NOINIT_RAM_LINKER_RESERVED 1
#  define NORA_NOINIT_RAM_ADDRESS         0x00013F30
#  define NORA_NOINIT_RAM_SIZE            0

#elif REJECT_CASE == 6
// A struct larger than the reservation. Without FITS this would overrun into whatever
// the linker put next -- the failure the macro exists to turn into a build error.
#  define NORA_NOINIT_RAM_LINKER_RESERVED 1
#  define NORA_NOINIT_RAM_ADDRESS         0x00013F30
#  define NORA_NOINIT_RAM_SIZE            16

#else
#  error "test_reject.c: define REJECT_CASE to one of 1..6"
#endif

#include "nora_noinit_ram.h"

#if REJECT_CASE == 6
typedef struct { uint8_t too_big[ 32 ]; } oversized_t;   // 32 > SIZE (16)
NORA_NOINIT_RAM_FITS( oversized_t );
#endif

volatile uint8_t nora_noinit_ram[ NORA_NOINIT_RAM_SIZE ];

int main( void )
{
    return 0;
}
