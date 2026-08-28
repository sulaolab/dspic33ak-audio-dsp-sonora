//===========================================================
// app_traps.c -- the AK trap vectors, the latch in surviving RAM, and the boot report.
// See app_traps.h for the division of labour and what the port changed.
//
// PUBLISH ORDER is the one thing to preserve if this file is edited. A trap can be
// followed by anything, including another trap, so the record is written in this order:
//
//     magic = 0            the record is now explicitly absent
//     ...payload...        every captured register
//     magic = MAGIC        the record is complete
//
// An interruption anywhere before that last store leaves the magic at zero, so the next
// boot sees "no record" -- never a plausible half-written one. Measured on the sibling
// resident pipe with the order reversed: a write interrupted after its check word came
// back VALID, carrying garbage. A check alone does not save you; it mismatches by luck.
//===========================================================

#include "diagnostics/app_traps.h"

#include <stdio.h>
#include <xc.h>

/*
 * The latch lives in the hal_noinit_ram block, in EVERY configuration -- see the storage
 * note in app_traps.h. Nothing here is conditional on the delivery mode: what differs per
 * configuration is only which linker script reserves the block, and that is settled in
 * src/noinit_ram_config.h and the two linker/p33AK*_noinit_ram_reserve.ld scripts.
 */
#include "noinit_ram_layout.h"

/* A record is valid only when this is written last by the trap vector. */
#define APP_TRAP_RECORD_MAGIC       (0x53545250UL) /* "STRP" */
/* 16-bit since schema 3 -- the counter pair was narrowed to fit the region; see the note at
 * the field. The pair also MOVED within the struct, so a surviving schema-2 block does not
 * present its old counter here: at worst the tag fails and the count restarts at zero, which
 * is the same outcome as a cold boot and is what the tag is for. */
#define APP_TRAP_COUNTER_MAGIC      (0x434Eu)     /* "CN" */
#define APP_TRAP_SCHEMA_VERSION     (3u)   /* 2: + sp/fp/inttreg. 3: + stacked PC/SR */

/*
 * The latch, in the hal_noinit_ram block's low region (src/noinit_ram_layout.h).
 *
 * The counter is a SEPARATE field with its OWN magic, and it is not folded into the
 * record: report_previous() consumes the record, while the counter must survive that so a
 * trap that repeats every boot can be told from one that happened once. Folding them
 * would mean the report destroyed the very evidence that the fault is deterministic.
 */
typedef struct
{
    uint32_t      magic;
    uint16_t      schema_version;
    uint16_t      id;
    uint32_t      intcon1;
    uint32_t      intcon3;
    uint32_t      intcon4;
    uint32_t      intcon5;
    uint32_t      pc;
    uint32_t      pctrap;
    uint32_t      fex;
    uint32_t      fex2;
    uint32_t      pchold;
    uint32_t      vfa;
    uint32_t      splim;
    uint32_t      rcon;
    uint32_t      sp;
    uint32_t      fp;
    uint32_t      inttreg;
    uint32_t      stacked_pc;
    uint32_t      stacked_sr;
    /*
     * THE COUNTER PAIR IS 16 BITS EACH BECAUSE THE REGION IS FULL, and this is the one pair
     * in the struct where narrowing costs nothing measurable.
     *
     * The region is 80 bytes (NOINIT_RAM_TRAPS_SIZE) and cannot grow: it is bounded above by
     * the resident pipe, whose absolute address is a contract between two separately built
     * images, and below by the top of the automatic stack. Schema 3's two new words needed 8
     * bytes and only 4 were spare, so 4 had to come from somewhere.
     *
     * Every other field is a RAW REGISTER, and this file's rule is that a raw word cannot be
     * wrong about a bit it did not think to name -- narrowing one would break exactly that
     * guarantee. These two are not registers: the magic is an internal validity tag on
     * uninitialised RAM (16 bits still rejects 65,535 of 65,536 patterns, and a wrong answer
     * misreports a diagnostic count, it does not affect behaviour), and the count saturates
     * below its own ceiling. At the observed rate -- one trap every several minutes -- 65,535
     * is not reachable in any run this counter exists to describe.
     */
    uint16_t      counter_magic;
    uint16_t      counter;
} app_trap_persistent_t;

NOINIT_RAM_TRAPS_FITS( app_trap_persistent_t );

#define TRAPS ( NOINIT_RAM_TRAPS_AS( app_trap_persistent_t ) )

#define BARRIER() __asm__ volatile( "" ::: "memory" )

static bool app_traps_record_is_valid( void );
static void app_traps_record_clear( void );
static void __attribute__((noreturn)) app_traps_record_and_reset( app_trap_id_t id );

static bool app_traps_record_is_valid( void )
{
    return ( TRAPS->magic == APP_TRAP_RECORD_MAGIC ) &&
           ( TRAPS->schema_version == APP_TRAP_SCHEMA_VERSION ) &&
           ( TRAPS->id > (uint16_t)APP_TRAP_NONE ) &&
           ( TRAPS->id < (uint16_t)APP_TRAP_COUNT );
}

static void app_traps_record_clear( void )
{
    /* Magic first, so an interrupted foreground report never replays a partially cleared
     * record on the next boot. */
    TRAPS->magic          = 0u;
    BARRIER();
    TRAPS->schema_version = 0u;
    TRAPS->id             = 0u;
    TRAPS->intcon1        = 0u;
    TRAPS->intcon3        = 0u;
    TRAPS->intcon4        = 0u;
    TRAPS->intcon5        = 0u;
    TRAPS->pc             = 0u;
    TRAPS->pctrap         = 0u;
    TRAPS->fex            = 0u;
    TRAPS->fex2           = 0u;
    TRAPS->pchold         = 0u;
    TRAPS->vfa            = 0u;
    TRAPS->splim          = 0u;
    TRAPS->rcon           = 0u;
    TRAPS->sp             = 0u;
    TRAPS->fp             = 0u;
    TRAPS->inttreg        = 0u;
    TRAPS->stacked_pc     = 0u;
    TRAPS->stacked_sr     = 0u;
}

static void __attribute__((noreturn)) app_traps_record_and_reset( app_trap_id_t id )
{
    /*
     * W15 FIRST -- before the store below, before anything that could push.
     *
     * WHY THIS IS ALLOWED TO BE C. The requirement is that nothing move the stack between
     * the fault and this read. Verified in the disassembly of this image rather than
     * assumed: each trap vector is two instructions (movs.l #id,w0 / rcall here) and pushes
     * nothing, and this function's own entry carries no prologue push -- unlike, say,
     * __DMA0Interrupt, which pushes w8..w11. So the captured value is the trapping W15 plus
     * the hardware trap push plus one return address, a fixed handful of bytes. An ASM shim
     * would buy exactness we do not need to tell "grew to SPLIM" from "jumped 16 kB", and
     * would cost a second copy of the vector table to keep correct.
     *
     * RE-VERIFY THIS AFTER ANY CHANGE HERE. Adding a local that the compiler decides to
     * spill would put a push in front of the read and quietly make the number describe this
     * handler instead of the fault. The check is one command:
     *   xc-dsc-objdump -d --mdfp=<DFP>/xc16 <elf> | grep -A6 '<_app_traps_record_and_reset>:'
     * and what must be true is that the first instruction touching w15 is this read.
     */
    uint32_t sp_at_entry;
    uint32_t fp_at_entry;

    __asm__ volatile ( "mov.l w15, %0" : "=r" (sp_at_entry) );
    __asm__ volatile ( "mov.l w14, %0" : "=r" (fp_at_entry) );

    /* Mark invalid until every raw register has been captured. */
    TRAPS->magic          = 0u;
    BARRIER();
    TRAPS->schema_version = APP_TRAP_SCHEMA_VERSION;
    TRAPS->id             = (uint16_t)id;

    /* Snapshot before touching any sticky state. FEX/FEX2 and the PC-related registers are
     * read ONLY: their clear semantics must not be guessed in an exception handler. */
    TRAPS->intcon1 = INTCON1;
    TRAPS->intcon3 = INTCON3;
    TRAPS->intcon4 = INTCON4;
    TRAPS->intcon5 = INTCON5;
    TRAPS->pc      = PC;
    TRAPS->pctrap  = PCTRAP;
    TRAPS->fex     = FEX;
    TRAPS->fex2    = FEX2;
    TRAPS->pchold  = PCHOLD;
    TRAPS->vfa     = VFA;
    TRAPS->splim   = SPLIM;
    TRAPS->rcon    = RCON;
    TRAPS->sp      = sp_at_entry;
    TRAPS->fp      = fp_at_entry;
    /* Which vector was being entered and at what interrupt priority. Under a rate-monotonic
     * priority assignment the legs can preempt each other, so "in whose ISR" stops being
     * derivable from PC alone -- and an interrupt-entry push is precisely what a STACK ERROR
     * at a non-pushing instruction has to have been. */
    TRAPS->inttreg = INTTREG;

    /*
     * THE EXCEPTION FRAME HARDWARE PUSHED -- the only trustworthy witness to where the trap
     * came from.
     *
     * PCTRAP is NOT that witness on this silicon. Errata DS80001162E #1 (A1 and A2, no
     * workaround) says PCTRAP does not capture the origin for math error traps while they
     * are disabled, and the workaround's own wording -- clear it and PCTRAP will capture
     * "any other traps" again -- only makes sense if the register is a write-once latch. It
     * was measured stuck 0x2FDEC away from the real site, in a different module, byte-
     * identical across eight records. That constancy read as localisation for several
     * rounds of this investigation and was contamination.
     *
     * Exception entry, by contrast, pushes SR and PC itself, and that copy cannot be stale.
     * The layout is measured, not assumed -- a debugger-frozen capture at this very
     * instruction showed a flash address at -8 and an SR-shaped word at -12, i.e. SR goes to
     * the LOWER address:
     *     [sp_at_entry -  4] = this handler's rcall return address (== <trap stub> + 8)
     *     [sp_at_entry -  8] = stacked PC   <- the faulting/resume address
     *     [sp_at_entry - 12] = stacked SR   <- CTX, IPL, and RA (REPEAT Loop Active)
     * Read in that order so the two words that decide the diagnosis land first.
     *
     * These are plain loads from RAM this handler cannot have disturbed, below the current
     * SP; they add no push and so do not invalidate the sp_at_entry read above (re-checked
     * in the disassembly per the note there).
     */
    TRAPS->stacked_pc = *(const volatile uint32_t *)(uintptr_t)( sp_at_entry - 8u );
    TRAPS->stacked_sr = *(const volatile uint32_t *)(uintptr_t)( sp_at_entry - 12u );

    if( TRAPS->counter_magic != APP_TRAP_COUNTER_MAGIC )
    {
        TRAPS->counter       = 0u;
        TRAPS->counter_magic = APP_TRAP_COUNTER_MAGIC;
    }
    /* Saturate rather than wrap: "65535" read as "0" would turn the most emphatic evidence
     * this field can carry into the one value that means "nothing happened". */
    if( TRAPS->counter < 0xFFFFu )
    {
        TRAPS->counter++;
    }

    /* Commit only once the complete payload is present. */
    BARRIER();
    TRAPS->magic = APP_TRAP_RECORD_MAGIC;

    /* Deliberately the same software reset the *sr console path uses, so the next boot
     * initialises a known-good UART before emitting the saved report -- and so the reset
     * cause it reports is one the banner already knows how to name. */
    __asm__ volatile ( "reset" );
    for( ;; )
    {
        /* The compiler does not know RESET is terminal. */
    }
}

void __attribute__((interrupt, context)) _BusErrorTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_BUS_ERROR );
}

void __attribute__((interrupt, context)) _IllegalInstructionTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_ILLEGAL_INSTRUCTION );
}

void __attribute__((interrupt, context)) _AddressErrorTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_ADDRESS_ERROR );
}

void __attribute__((interrupt, context)) _StackErrorTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_STACK_ERROR );
}

void __attribute__((interrupt, context)) _MathErrorTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_MATH_ERROR );
}

void __attribute__((interrupt, context)) _GeneralTrap( void )
{
    app_traps_record_and_reset( APP_TRAP_GENERAL_ERROR );
}

void app_traps_boot_prepare( bool power_on_reset )
{
    if( power_on_reset )
    {
        app_traps_record_clear();
        TRAPS->counter       = 0u;
        TRAPS->counter_magic = APP_TRAP_COUNTER_MAGIC;
        return;
    }

    /* A non-power reset may still be the first boot after arbitrary RAM contents -- the
     * resident bootloader can reach the application without a power cycle having happened
     * in this session's history. Give the counter a valid zero baseline without touching a
     * valid record. */
    if( TRAPS->counter_magic != APP_TRAP_COUNTER_MAGIC )
    {
        TRAPS->counter       = 0u;
        TRAPS->counter_magic = APP_TRAP_COUNTER_MAGIC;
    }
}

void app_traps_clear( void )
{
    app_traps_record_clear();
}

bool app_traps_previous_get( app_trap_record_t *out )
{
    if( ( out == 0 ) || !app_traps_record_is_valid() )
    {
        return false;
    }

    out->schema_version = TRAPS->schema_version;
    out->id             = (app_trap_id_t)TRAPS->id;
    out->intcon1        = TRAPS->intcon1;
    out->intcon3        = TRAPS->intcon3;
    out->intcon4        = TRAPS->intcon4;
    out->intcon5        = TRAPS->intcon5;
    out->pc             = TRAPS->pc;
    out->pctrap         = TRAPS->pctrap;
    out->fex            = TRAPS->fex;
    out->fex2           = TRAPS->fex2;
    out->pchold         = TRAPS->pchold;
    out->vfa            = TRAPS->vfa;
    out->splim          = TRAPS->splim;
    out->rcon           = TRAPS->rcon;
    out->sp             = TRAPS->sp;
    out->fp             = TRAPS->fp;
    out->inttreg        = TRAPS->inttreg;
    out->stacked_pc     = TRAPS->stacked_pc;
    out->stacked_sr     = TRAPS->stacked_sr;
    return true;
}

app_trap_id_t app_traps_last_id( void )
{
    return app_traps_record_is_valid() ? (app_trap_id_t)TRAPS->id : APP_TRAP_NONE;
}

uint32_t app_traps_count( void )
{
    return ( TRAPS->counter_magic == APP_TRAP_COUNTER_MAGIC ) ? TRAPS->counter : 0u;
}

const char *app_traps_id_str( app_trap_id_t id )
{
    switch( id )
    {
    case APP_TRAP_NONE:                return "none";
    case APP_TRAP_BUS_ERROR:           return "BUS ERROR";
    case APP_TRAP_ILLEGAL_INSTRUCTION: return "ILLEGAL INSTRUCTION";
    case APP_TRAP_ADDRESS_ERROR:       return "ADDRESS ERROR";
    case APP_TRAP_STACK_ERROR:         return "STACK ERROR";
    case APP_TRAP_MATH_ERROR:          return "MATH ERROR";
    case APP_TRAP_GENERAL_ERROR:       return "GENERAL ERROR";
    default:                           return "unknown";
    }
}

void app_traps_report_previous( void )
{
    app_trap_record_t record;
    uint32_t count;

    if( !app_traps_previous_get( &record ) )
    {
        return;
    }

    count = app_traps_count();
    printf( "\n*** AK TRAP on the previous run: %s\n",
            app_traps_id_str( record.id ) );
    printf( "    INTCON1=%08lX INTCON3=%08lX INTCON4=%08lX INTCON5=%08lX\n",
            (unsigned long)record.intcon1, (unsigned long)record.intcon3,
            (unsigned long)record.intcon4, (unsigned long)record.intcon5 );
    printf( "    PC=%06lX PCTRAP=%06lX PCHOLD=%06lX VFA=%06lX SPLIM=%06lX\n",
            (unsigned long)record.pc, (unsigned long)record.pctrap,
            (unsigned long)record.pchold, (unsigned long)record.vfa,
            (unsigned long)record.splim );
    printf( "    FEX=%08lX FEX2=%08lX RCON-at-trap=%08lX\n",
            (unsigned long)record.fex, (unsigned long)record.fex2,
            (unsigned long)record.rcon );
    /* SP-vs-SPLIM as a SIGNED distance, because the sign is the whole diagnosis: a stack
     * that overflowed honestly stops a few words past the limit, while a corrupted W15 lands
     * arbitrarily far away -- and can land BELOW it, which an unsigned print would render as
     * a plausible-looking 4 GB. VEC/ILR name the context that was being entered. */
    printf( "    SP-at-trap=%06lX (SPLIM%+ld B) FP=%06lX INTTREG=%08lX VEC=%lu ILR=%lu\n",
            (unsigned long)record.sp,
            (long)( (int32_t)record.sp - (int32_t)record.splim ),
            (unsigned long)record.fp,
            (unsigned long)record.inttreg,
            (unsigned long)( record.inttreg & 0x1FFuL ),
            (unsigned long)( ( record.inttreg >> 10 ) & 0xFuL ) );
    /* The stacked frame, decoded on the target. The raw words are printed too -- a raw word
     * cannot be wrong about a bit this decode did not think to name -- but the decode is what
     * makes the line answer the question without a host-side script: STK-PC says which
     * instruction faulted (PCTRAP does not, errata #1), and of the SR fields IPL/CTX name
     * which nesting level was being entered while RA says whether a REPEAT loop was active at
     * all. RA=0 is what killed the REPEAT-race hypothesis. */
    printf( "    STK-PC=%06lX STK-SR=%08lX (CTX=%lu IPL=%lu RA=%lu N=%lu OV=%lu Z=%lu C=%lu)\n",
            (unsigned long)record.stacked_pc,
            (unsigned long)record.stacked_sr,
            (unsigned long)( ( record.stacked_sr >> 16 ) & 0x7uL ),
            (unsigned long)( ( ( ( record.stacked_sr >> 8 ) & 0x1uL ) << 3 ) |
                             ( ( record.stacked_sr >> 5 ) & 0x7uL ) ),
            (unsigned long)( ( record.stacked_sr >> 4 ) & 0x1uL ),
            (unsigned long)( ( record.stacked_sr >> 3 ) & 0x1uL ),
            (unsigned long)( ( record.stacked_sr >> 2 ) & 0x1uL ),
            (unsigned long)( ( record.stacked_sr >> 1 ) & 0x1uL ),
            (unsigned long)( record.stacked_sr & 0x1uL ) );
    printf( "    traps since power-on: %lu%s\n",
            (unsigned long)count,
            ( count > 1u ) ? "  <- repeating" : "" );

    /* Consume only the one-shot record. The count remains diagnostic evidence for the rest
     * of this power cycle. */
    app_traps_record_clear();
}
