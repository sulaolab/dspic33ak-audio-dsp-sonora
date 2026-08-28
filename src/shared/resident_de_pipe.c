//===========================================================
// resident_boot_pipe.c -- the container the bootloader and the application talk
// through. See resident_boot_pipe.h for the layout and the per-record rules.
//
// PUBLISH ORDER is the one thing to preserve if this file is edited. A reset can land
// between any two stores, so every record is written in this order:
//
//     marker = 0          the record is now explicitly absent
//     ...payload...       (only the crash record has one)
//     companion = ~M      the complement, still with no marker present
//     marker = M          the record is complete
//
// An interruption anywhere before that last store leaves the marker at zero, so the next
// boot sees "no record" -- never a plausible half-written one. That ordering is also what
// protects the crash payload: the marker being present means every payload store before
// it completed. The complement is a separate job -- it stops random power-on bits from
// looking like a marker.
//
// Measured on an earlier version of this code with the order reversed: a write interrupted
// after its check word came back VALID, carrying garbage. A check alone does not save you;
// it only mismatches by luck.
//
//-----------------------------------------------------------------------
// WHY THIS FILE IS COMPILED IN HALVES
//
// Each side compiles only its own half, selected by SONORA_RESIDENT_BOOTLOADER, which the
// resident build already defines:
//
//     bootloader side    init(), request_take()
//     application side   request_set(), crash_set/peek/clear()
//     both              ready(), launch_set(), launch_count()
//
// This started as a space measure. Both builds now use -ffunction-sections with
// --remove-unused-sections, so the linker drops uncalled functions on its own and the
// split no longer saves anything by itself. (It was written when only the resident build
// had the flag: for the application image, linking this object at all pulled in every
// function in it -- about 1.4 KB, into an image that had a few hundred bytes free.
// Enabling the flag on the application builds freed ~56 KB there, which is what removed
// that pressure.)
//
// It is kept because it now buys correctness rather than bytes: each side compiles only
// the direction it is allowed to use, so a side calling the wrong half -- the bootloader
// publishing a request, the application consuming one -- is a link error instead of a
// subtle protocol bug. Records that are opt-in features sit behind their own flag as well.
//-----------------------------------------------------------------------
//===========================================================

#include "resident_de_pipe.h"

// The container, overlaid on its region of the HAL's block. volatile: the bytes are shared
// with another image and outlive this program, so the compiler must not cache or elide them.
//
// WAS NORA_NOINIT_RAM_AS(), i.e. offset 0, until the block was extended downward to hold
// the trap record (shared/noinit_ram_layout.h). The pipe's ABSOLUTE address is unchanged at
// 0x13F80 -- that is what the layout header asserts -- so this is a change of arithmetic,
// not of the cross-image contract.
#define PIPE ( NOINIT_RAM_PIPE_AS( resident_boot_pipe_t ) )

#define BARRIER() __asm__ volatile( "" ::: "memory" )

#if defined(SONORA_RESIDENT_BOOTLOADER)
#  define PIPE_BOOT_SIDE 1
#else
#  define PIPE_APP_SIDE 1
#endif

// A record is present iff its marker reads as expected AND its companion is the exact
// complement.
static bool pair_ok( uint16_t value, uint16_t inverse, uint16_t expect )
{
    return ( value == expect ) && ( inverse == (uint16_t)~expect );
}

bool resident_boot_pipe_ready( void )
{
    return pair_ok( PIPE->version, PIPE->version_inv, RESIDENT_BOOT_PIPE_VERSION );
}

//-----------------------------------------------------------------------
// Bootloader side
//-----------------------------------------------------------------------
#if defined(PIPE_BOOT_SIDE)

void resident_boot_pipe_init( bool distrust )
{
    // Only two reasons to wipe: the cells are untrustworthy (power cycle), or the bytes
    // belong to a different layout. Anything else is left alone and each record proves
    // itself when read -- which is what makes this safe to call before the reads.
    if( ( !distrust ) && resident_boot_pipe_ready() )
    {
        return;
    }

    // Zero this container's whole REGION rather than each field: shorter code, and it
    // leaves no stale bytes anywhere in the pipe's reservation, including padding and space
    // no record uses yet. A reset partway through simply lands here again next boot.
    //
    // SCOPED TO THE REGION, not the block (2026-08-11). The block gained a second tenant
    // -- the trap record, in the low 80 bytes -- and it is the tenant that must decide
    // when its own evidence is worthless: a trap record is destroyed by a POR and by
    // nothing else, while this function also wipes on a layout-version mismatch, which
    // says nothing at all about the trap latch. Zeroing the whole block here would have
    // deleted the record every time the bootloader distrusted its own container.
    {
        uint16_t i;
        for( i = 0u; i < (uint16_t)NOINIT_RAM_PIPE_SIZE; ++i )
        {
            nora_noinit_ram[ NOINIT_RAM_PIPE_OFFSET + i ] = 0u;
        }
    }

    PIPE->version_inv = (uint16_t)~RESIDENT_BOOT_PIPE_VERSION;
    BARRIER();
    PIPE->version     = RESIDENT_BOOT_PIPE_VERSION;   // last: the container is ready
}

bool resident_boot_pipe_request_take( void )
{
    if( ( !resident_boot_pipe_ready() ) ||
        ( !pair_ok( PIPE->request, PIPE->request_inv,
                    RESIDENT_BOOT_PIPE_REQUEST_MARKER ) ) )
    {
        return false;
    }

    // Consume BEFORE reporting success. This is what makes the update wait escapable: the
    // request is already gone, so a timeout or a reset button press boots the application
    // instead of re-entering the wait for ever.
    PIPE->request     = 0u;
    BARRIER();
    PIPE->request_inv = 0u;
    return true;
}

bool resident_boot_pipe_cause_pending( void )
{
    // Same self-proving pair test the application's take() uses, and deliberately not gated
    // on ready() either -- see resident_boot_pipe_cause_take().
    return pair_ok( PIPE->cause, PIPE->cause_inv, RESIDENT_BOOT_PIPE_CAUSE_MARKER );
}

void resident_boot_pipe_cause_publish( uint32_t rcon )
{
    PIPE->cause      = 0u;                     // absent while being written
    BARRIER();
    PIPE->cause_rcon = rcon;
    PIPE->cause_inv  = (uint16_t)~RESIDENT_BOOT_PIPE_CAUSE_MARKER;
    BARRIER();
    PIPE->cause      = RESIDENT_BOOT_PIPE_CAUSE_MARKER;   // publish
}

#endif  // PIPE_BOOT_SIDE

//-----------------------------------------------------------------------
// Application side
//-----------------------------------------------------------------------
#if defined(PIPE_APP_SIDE)

void resident_boot_pipe_request_set( void )
{
    PIPE->request     = 0u;                    // absent while being written
    BARRIER();
    PIPE->request_inv = (uint16_t)~RESIDENT_BOOT_PIPE_REQUEST_MARKER;
    BARRIER();
    PIPE->request     = RESIDENT_BOOT_PIPE_REQUEST_MARKER;   // publish
}

bool resident_boot_pipe_cause_take( uint32_t *rcon )
{
    // Deliberately NOT gated on ready(): this is read before anything else in main(), and
    // the marker pair is self-proving. Gating it would also make the record unreadable in
    // exactly the case it is most wanted -- the boot after the container was established
    // for a new layout.
    if( !pair_ok( PIPE->cause, PIPE->cause_inv, RESIDENT_BOOT_PIPE_CAUSE_MARKER ) )
    {
        return false;
    }

    if( rcon != 0 ) { *rcon = PIPE->cause_rcon; }

    // Consume: the forwarded word describes THIS boot only. Left in place it would be
    // reported again after a reset the bootloader did not mediate, which is the same class
    // of quiet lie the record exists to remove.
    PIPE->cause     = 0u;
    BARRIER();
    PIPE->cause_inv = 0u;
    return true;
}

#endif  // PIPE_APP_SIDE

//-----------------------------------------------------------------------
// launch -- opt-in, and needed on BOTH sides (the bootloader counts, the application
// acknowledges), so it is not split by side.
//-----------------------------------------------------------------------
#if RESIDENT_BOOT_ENA_LAUNCH_GUARD

// One setter rather than bump/clear. Both callers already know the value they want -- the
// bootloader has just read the count, the application always writes 0 -- so a
// read-modify-write helper would re-validate what the caller already has. On this part
// that mattered: an earlier bump() compiled to 432 bytes.
void resident_boot_pipe_launch_set( uint16_t count )
{
    PIPE->launch     = 0u;                     // absent while being written
    BARRIER();
    PIPE->launch_inv = (uint16_t)~count;
    BARRIER();
    PIPE->launch     = count;                  // publish
}

// The counter has no separate marker: the count IS the value and its complement is the
// proof. Note 0 is a legitimate count and (0, 0xFFFF) is a valid pair, so a cleared
// counter reads back as a real zero rather than as "absent".
uint16_t resident_boot_pipe_launch_count( void )
{
    const uint16_t count = PIPE->launch;

    // Absent or damaged reads as zero, not as a huge number: a bad byte must not lock a
    // healthy board out of its application.
    if( ( !resident_boot_pipe_ready() ) || ( PIPE->launch_inv != (uint16_t)~count ) )
    {
        return 0u;
    }
    return count;
}

#endif  // RESIDENT_BOOT_ENA_LAUNCH_GUARD

//-----------------------------------------------------------------------
// crash -- opt-in, application side only (the trap handler writes it, and the
// application reports it; the bootloader has no room for a five-argument printf).
//-----------------------------------------------------------------------
#if RESIDENT_BOOT_ENA_CRASH_RECORD && defined(PIPE_APP_SIDE)

void resident_boot_pipe_crash_set( uint32_t vector, uint32_t detail,
                                   uint32_t sr, uint32_t splim, uint32_t w15 )
{
    // Called from a trap handler: plain stores only, marker last, nothing that could
    // itself fault. Deliberately does NOT check ready() -- a crash before the container
    // was established is still worth recording, and the marker pair written below is what
    // makes the record self-describing.
    PIPE->crash        = 0u;
    BARRIER();
    PIPE->crash_vector = vector;
    PIPE->crash_detail = detail;
    PIPE->crash_sr     = sr;
    PIPE->crash_splim  = splim;
    PIPE->crash_w15    = w15;
    PIPE->crash_inv    = (uint16_t)~RESIDENT_BOOT_PIPE_CRASH_MARKER;
    BARRIER();
    PIPE->crash        = RESIDENT_BOOT_PIPE_CRASH_MARKER;   // publish
}

bool resident_boot_pipe_crash_peek( uint32_t *vector, uint32_t *detail,
                                    uint32_t *sr, uint32_t *splim, uint32_t *w15 )
{
    if( !pair_ok( PIPE->crash, PIPE->crash_inv, RESIDENT_BOOT_PIPE_CRASH_MARKER ) )
    {
        return false;
    }

    // Peek, not take: the breadcrumb outlives being reported, and only an explicit
    // _clear() drops it. Note this does NOT require ready() -- see crash_set.
    if( vector != 0 ) { *vector = PIPE->crash_vector; }
    if( detail != 0 ) { *detail = PIPE->crash_detail; }
    if( sr     != 0 ) { *sr     = PIPE->crash_sr;     }
    if( splim  != 0 ) { *splim  = PIPE->crash_splim;  }
    if( w15    != 0 ) { *w15    = PIPE->crash_w15;    }
    return true;
}

void resident_boot_pipe_crash_clear( void )
{
    PIPE->crash     = 0u;
    BARRIER();
    PIPE->crash_inv = 0u;
}

#endif  // RESIDENT_BOOT_ENA_CRASH_RECORD && PIPE_APP_SIDE
