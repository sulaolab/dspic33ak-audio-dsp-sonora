/*
 * traps_console.c -- common console module 'x': exceptions (traps).
 *
 *   ?xl  last trap and the count since power-on
 *   *xa  force an address error    *xm  force a math error
 *   *xs  force a stack overflow
 *
 * PORTED FROM the dsPIC33CK lab tree's own traps_console.c -- file name, module letter
 * and command names included, so the two families read as one module. What the AK side
 * has to answer differently is only WHERE the two "genuinely wrong" values come from
 * -- see THE TWO TARGETS below.
 *
 * WHY 'x' AND NOT 't'
 * -------------------
 * 't' is this repo's transport module (*tr restart, *tf frame-slip). CK made the same
 * choice for the same reason. One letter meaning two things across two projects is how muscle
 * memory turns into a wrong command on hardware.
 *
 * WHY THE FORCED TRAPS EXIST AT ALL
 * ---------------------------------
 * Trap handlers only run when something has already gone wrong, which makes them exactly
 * the kind of code that rots unnoticed: without a way to fire one, "the trap latch works"
 * is an assumption. Each of these is genuinely undefined behaviour by design -- that is the
 * point -- so they are shaped to defeat the optimiser (volatile operands it cannot fold,
 * values it cannot prove) rather than relying on it being naive. Guarded by
 * APP_TRAP_TEST_CMDS so a shipping image need not carry them.
 *
 * THE TWO TARGETS, AND WHY THERE IS NO BOARD SEAM HERE
 * ---------------------------------------------------
 * CK asks the linked board for these two values (board_trap_bad_addr(),
 * board_trap_stack_beyond_limit()) because it builds two boards from one tree. This repo
 * builds one board family, and neither value is a board fact in it:
 *
 *   the bad address is a DEVICE fact, and it is already stated once, in
 *   src/noinit_ram_config.h -- that block sits at the top of data RAM by construction, so
 *   the first address with nothing behind it is its end. Deriving it from there instead of
 *   writing 0x14000 again means the two cannot drift; the _Static_assert below still pins
 *   the expected value per device, so moving the block away from the top of RAM fails the
 *   build here rather than silently pointing this test at ordinary RAM.
 *
 *   the stack limit is a CONFIGURATION fact, not even a device one -- SPLIM differs per
 *   configuration (measured: 0x13DC0 in the AK512 serial-update build, 0x7F20 on AK128,
 *   each being that link's stack top minus its --stackguard). So it is read from SPLIM at
 *   run time, which is the register the hardware itself compares against. A constant here
 *   would have to be re-measured after every link that moves the stack.
 *
 * A board seam can be introduced the day a second board wants different answers, exactly
 * as CK's was; there is nothing above this line to change when it does.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <xc.h>

#include "app_console.h"
#include "traps_console.h"
#include "diagnostics/app_traps.h"   /* the latch, and APP_TRAP_TEST_CMDS */
#include "noinit_ram_config.h"       /* where the surviving block is -- i.e. where RAM ends */
#include "timer_app.h"               /* delay_ms -- drain the console before the fault */
#include "audio_transport.h"        /* *xr stops the transport before probing */
#include "nora_high_res_timer.h"    /* *xr times the outer REPEAT loop */

/*
 * ---- Stack high-water measurement:  *xw  arm,  ?xw  read -----------------------------
 *
 * WHY HERE, in module 'x', and not in a file of its own: it answers the same question as the
 * trap latch above -- did the stack survive -- and this module already owns SPLIM for the
 * same reason (it is a per-configuration fact, read from the register rather than written
 * down). A new translation unit would also mean editing every configuration in
 * nbproject/configurations.xml, which is a worse trade for a measurement.
 *
 * HOW IT WORKS. The stack grows UP on this core and SPLIM is the top the hardware enforces,
 * so everything between the console's own stack pointer and SPLIM is memory that no live
 * frame owns. *xw paints that region with a pattern; ?xw scans DOWN from SPLIM for the first
 * word that is no longer the pattern. That word is the deepest point ANY context reached
 * since the arm -- interrupt frames, and nested interrupt frames, included. Nesting is
 * exactly what a rate-monotonic priority assignment introduces, so this is the instrument
 * for it.
 *
 * Painting while interrupts are live is safe on this core: an ISR that preempts the fill has
 * returned before the fill resumes, so the only frames the loop ever writes over are dead
 * ones. There is no second context to race with.
 *
 * WHAT IT CANNOT TELL YOU. If the stack really overflows, the report says the peak sits at
 * SPLIM -- the demand exceeded the region -- and never by how much, because the STACK ERROR
 * trap stops the machine at the limit. Measuring the DEMAND needs a region big enough to hold
 * it, which is why the flash-coefficient build is where that number comes from.
 * [internal] report_ak512_16ch_mixed_rate_margin_cause_2026-08-24.md section 17.
 */
#if APP_STACK_WATERMARK

#define STACK_WM_PATTERN   0xA5A5A5A5uL

/* Left unpainted above the arming frame, so the report describes contexts that came later
 * rather than the arm itself. */
#define STACK_WM_SKIP      32u

static uint32_t s_wm_base;   /* first painted address; 0 = never armed */
static uint32_t s_wm_top;    /* one past the last painted address (== SPLIM) */

static inline uint32_t stack_wm_sp( void )
{
    uint32_t sp;

    /* W15 IS the stack pointer on this core; the write direction of this same instruction is
     * what *xs uses to move it past the limit. */
    __asm__ volatile ( "mov.l w15, %0" : "=r" (sp) );
    return sp;
}

static void stack_wm_arm( void )
{
    const uint32_t sp   = stack_wm_sp();
    const uint32_t base = ( sp + STACK_WM_SKIP + 3u ) & ~3u;
    const uint32_t top  = (uint32_t)SPLIM & ~3u;
    volatile uint32_t* p;

    if( top <= base )
    {
        printf( " \"*xw\" cannot arm: SP=0x%06lX is already at SPLIM=0x%06lX\n",
                (unsigned long)sp, (unsigned long)top );
        return;
    }

    for( p = (volatile uint32_t*)base; (uint32_t)p < top; p++ )
    {
        *p = STACK_WM_PATTERN;
    }

    s_wm_base = base;
    s_wm_top  = top;

    printf( " \"*xw\" stack watermark armed: painted 0x%06lX..0x%06lX (%lu B),"
            " arming SP=0x%06lX\n",
            (unsigned long)base, (unsigned long)( top - 4u ),
            (unsigned long)( top - base ), (unsigned long)sp );
}

static void stack_wm_report( void )
{
    volatile const uint32_t* p;
    uint32_t peak;

    if( s_wm_base == 0u )
    {
        printf( " \"?xw\" not armed -- run *xw first (SPLIM=0x%06lX, SP now 0x%06lX)\n",
                (unsigned long)SPLIM, (unsigned long)stack_wm_sp() );
        return;
    }

    /* Scan down: the first word that is not the pattern is the top of what was used. */
    peak = s_wm_base;
    for( p = (volatile const uint32_t*)( s_wm_top - 4u );
         (uint32_t)p >= s_wm_base;
         p-- )
    {
        if( *p != STACK_WM_PATTERN )
        {
            peak = (uint32_t)p + 4u;
            break;
        }
    }

    printf( " \"?xw\" stack peak=0x%06lX SPLIM=0x%06lX free=%lu B used_above_arm=%lu B"
            " painted=%lu B (arm base 0x%06lX)\n",
            (unsigned long)peak, (unsigned long)s_wm_top,
            (unsigned long)( s_wm_top - peak ),
            (unsigned long)( peak - s_wm_base ),
            (unsigned long)( s_wm_top - s_wm_base ),
            (unsigned long)s_wm_base );
}

#endif /* APP_STACK_WATERMARK */

#if APP_TRAP_TEST_CMDS

/*
 * The first data address with no memory behind it. The surviving block occupies the top of
 * this device's data RAM (src/noinit_ram_config.h states the address per device and says
 * so), so the byte after it is the first that does not exist.
 */
#define TRAPS_CONSOLE_BAD_ADDR  ( (uint32_t)NORA_NOINIT_RAM_ADDRESS + (uint32_t)NORA_NOINIT_RAM_SIZE )

#if defined(__dsPIC33AK512MPS512__)
_Static_assert( TRAPS_CONSOLE_BAD_ADDR == 0x00014000UL,
                "*xa reads the first byte past data RAM. On AK512MPS512 data is "
                "0x4000..0x13FFF (DFP p33AK512MPS512.gld), so this must be 0x14000. It is "
                "derived from the noinit block, so if that block no longer ends at the top "
                "of RAM this test would read ordinary RAM and prove nothing." );
#elif defined(__dsPIC33AK128MC106__)
_Static_assert( TRAPS_CONSOLE_BAD_ADDR == 0x00008000UL,
                "*xa reads the first byte past data RAM. On AK128MC106 data is "
                "0x4000..0x7FFF (DFP p33AK128MC106.gld), so this must be 0x8000." );
#else
#error "traps_console.c: unknown device -- confirm where THIS device's data RAM ends before letting *xa read past it, the same decision src/noinit_ram_config.h documents."
#endif

/*
 * Stack error: push W15 past SPLIM.
 *
 * SPLIM is what the hardware compares the stack pointer against, so writing a stack
 * pointer beyond it is the DEFINITION of the fault rather than an attempt to provoke one.
 * (CK's first attempt recursed instead and did not trap -- either the optimiser folded the
 * recursion or the frames never reached the limit. Rather than find out which, drive W15.)
 *
 * noreturn + noinline so the compiler neither inlines this into the caller's frame nor
 * assumes execution continues afterwards.
 *
 * The margin is small on purpose. Everything above SPLIM is the linker's stack guard and
 * then the surviving block, so the trap's own context save has real memory to land in --
 * and since app_traps_record_and_reset() publishes the record AFTER that save, a save that
 * reaches into the block cannot destroy the evidence.
 *
 * WHAT THIS PROVES IS NOT SETTLED YET. On CK this path does not produce a software
 * _StackError report at all: W15 is already past SPLIM, so the handler's own push
 * overflows too, the core takes a trap within a trap and resets with RCON.TRAPR set. AK has
 * no TRAPR bit and a General trap this family may take instead, so which of "a STACK ERROR
 * record" and "a bare reset" appears here is a hardware question -- and it is one of the
 * two things the first hardware run of this feature is for. Either outcome is informative:
 * the reset alone still says the limit was enforced.
 */
static void __attribute__((noinline, noreturn)) traps_console_blow_stack( void )
{
    uint32_t beyond = (uint32_t)SPLIM + 8u;

    /* Move the stack pointer past the limit. The operand is read into a W register before
     * W15 changes, so the value survives the move that invalidates the frame. */
    __asm__ volatile ( "mov.l %0, w15" : : "r" (beyond) );

    /* A push, to force the fault here rather than at some later call. This is the plain
     * W-register push on this core -- the compiler's own prologues use the same
     * instruction (verified in the disassembly of an interrupt entry). */
    for( ;; )
    {
        __asm__ volatile ( "mov.l w0, [w15++]" );
    }
}

static void traps_console_force( uint8_t which )
{
    printf( " \"*x%c\" forcing a trap on purpose -- expect a reset, then a report on the"
            " next boot\n", (char)which );
    delay_ms( 150u );   /* let the line above leave the UART before the machine stops making sense */

    switch( which )
    {
    case 'a':
    {
        /*
         * Address error: read an UNIMPLEMENTED data address. NOT a misaligned one -- CK
         * measured that a misaligned word read inside valid RAM carries straight on.
         *
         * This may latch BUS ERROR rather than ADDRESS ERROR: both vectors exist on AK and
         * which one an absent-memory read raises is a hardware fact this has not been run
         * against yet. The report names whichever fired, so either answer is a pass for
         * the latch -- it is the trap id that then has to be believed over this comment.
         */
        volatile uint32_t *nowhere = (volatile uint32_t *)TRAPS_CONSOLE_BAD_ADDR;

        (void)*nowhere;
        break;
    }
    case 'm':
    {
        /* Math error: integer divide by zero. The divisor is volatile so the division is
         * emitted rather than folded into a compile-time diagnostic. */
        static volatile int denom;    /* zero-initialised */
        static volatile int result;

        result = 1000 / denom;
        (void)result;
        break;
    }
    case 's':
        traps_console_blow_stack();   /* does not return */
        break;

    default:
        break;
    }
}
#endif /* APP_TRAP_TEST_CMDS */

/* ============================================================================
 * *xr -- does the dsPIC33A really bank RCOUNT per IPL?   (investigation #3.25)
 * ============================================================================
 * A SILICON QUESTION, ASKED BECAUSE THE DOCUMENTS DISAGREE
 * -------------------------------------------------------
 * DS70005591C 4.3.9 lists RCOUNT among the registers each of the seven per-IPL
 * hardware contexts owns, and XC-DSC agrees with it in practice: the dsPIC33A
 * interrupt attribute was "made hardware register context aware" (XCDSC-127,
 * v3.30) and the v3.31.01 ELF of this very image contains ZERO RCOUNT saves in
 * ISRs that run REPEAT loops. DS 4.3.15.1.2 says the opposite in prose: an ISR
 * that executes its own REPEAT must stack RCOUNT and unstack it before RETFIE,
 * and an interrupted REPEAT can be terminated early by clearing RCOUNT inside
 * the ISR -- both of which only mean anything if RCOUNT is SHARED.
 *
 * Two documents, one of them wrong, and the answer decides whether the
 * STACK ERROR at _fir_ring_q31_ymod_yonly_block+0x54 has a mechanism at all.
 * No amount of further reading settles it. The silicon can.
 *
 * WHY IT LIVES IN THE ASRC IMAGE AND NOT IN A BENCH BUILD
 * ------------------------------------------------------
 * A separate bench would be a different translation-unit set, possibly with
 * different compiler options and different interrupt attributes -- i.e. it
 * could answer the question for a build that is not the one that traps. This is
 * compiled into the image under test, behind APP_RCOUNT_PROBE, and it runs only
 * when a human types *xr.
 *
 * TWO SEPARATE INTERRUPT SOURCES, NOT ONE RE-PRIORITISED
 * -----------------------------------------------------
 * QEI2 is the outer (lower priority), QEI1 the inner (higher). Raising one
 * source priority mid-test to make it interrupt itself would drag the live
 * IPC-update question -- the thing under investigation elsewhere in this report
 * -- into the measurement. Two sources keep them apart.
 *
 * QEI1/QEI2 because the peripheral exists on this device (ATDF vectors 136 and
 * 137) and NOTHING in this firmware uses it: no QEI clock, no ISR, no PPS
 * route. So the vectors can be driven purely by writing IFS, which needs no
 * peripheral, no pin and no clock, and is instantaneous -- unlike the spare
 * timer this probe was first designed around, which needed a peripheral this
 * image does not have free (CCP1 and CCP9 are the high-res timer, and the
 * input-capture HAL claims CCP1-CCP9).
 *
 * THE BYTE WRITE, AND WHY IT IS A BYTE
 * ------------------------------------
 * IFS4<8..15> is QEI1-4IF, BISS1EIF, BISS1IF, CRCIF, ICDIF -- eight flags, not
 * one of them used by this firmware. So byte 1 of IFS4 can be written whole, as
 * a single instruction, without a read-modify-write and without disturbing
 * anything live. That matters twice: the trigger inside the outer REPEAT loop
 * MUST be one instruction, and a whole-WORD store to IFS4 would have rewritten
 * CCP9IF -- the high-res timer, which this probe uses to measure. IEC4 byte 1
 * is the same eight bits, which is what lets the enables be set and cleared by
 * constant byte writes (a bitfield write would be a read-modify-write of a word
 * shared with CCP9IE -- see the fleet rule in memory
 * dfp-bit-alias-atomic-only-if-constant).
 *
 * PROBE-EXCLUSIVE CONTEXTS -- IPL 5 AND 6, NOT 3 AND 4
 * ---------------------------------------------------
 * The sentinel below is only evidence if NOTHING ELSE can write the context it
 * is stored in. The fault lives at IPL3/IPL4, so the obvious choice would be to
 * probe there -- but a single ordinary IPL4 DMA ISR running one REPEAT during
 * the test overwrites the sentinel, and "the sentinel was gone" then means
 * either "RCOUNT is shared" or "a DMA ISR stepped on it". Those are opposite
 * conclusions, and the wrong one is the one we would act on.
 *
 * So the probe owns two contexts NOTHING in this firmware uses: IPL5 and IPL6.
 * The question -- is RCOUNT banked per IPL -- is the same for any two distinct
 * IPLs, and DS 4.3.9 describes one mechanism for all seven contexts. The
 * transport is stopped first as well, and IEC2/IEC3/IFS2/IFS3 are printed
 * before and after so the isolation is MEASURED and not assumed. That second
 * belt matters here specifically: 19.8.1 records an unconfirmed suspicion that
 * audio_transport_stop() may leave leg A TDM DMA armed, and IPL5/6 is immune to
 * it either way.
 *
 * WHAT EACH STAGE PROVES
 * ----------------------
 *   A  poison + separation. The inner ISR writes 0xA5A5 into RCOUNT and reads
 *      it straight back (does the write take at all?). The base context has
 *      0x5A5A in it, written before the trigger; after the ISR returns, base
 *      still reading 0x5A5A means the two contexts are distinct. Then the inner
 *      is fired a second time and reads RCOUNT FIRST: 0xA5A5 means its own
 *      context survived a return to base and a re-entry -- banked. 0x5A5A means
 *      it is looking at the base register -- shared.
 *   B  mid-REPEAT capture. The outer starts a 20,000-iteration REPEAT whose
 *      repeated instruction fires the inner. The inner captures RCOUNT as its
 *      first instruction: 0xA5A5 = the outer live loop counter is invisible to
 *      it (banked); a large value near 20,000 = it is reading the outer
 *      countdown (shared), which would be the mechanism the report is hunting.
 *   C  early termination, which adjudicates the DS prose directly. Same loop,
 *      but the inner writes RCOUNT = 0. If the outer loop still runs to
 *      completion (elapsed ~= stage B) the DS Early-Termination clause cannot
 *      be describing this core. If it ends immediately (elapsed << B) then
 *      RCOUNT is shared or aliased and the clause is literal.
 *
 * NO CLEAN-UP AMBITION. State is left as the probe leaves it and the report
 * says to power-cycle or *sr afterwards: a diagnostic that also tries to
 * restore a half-stopped audio transport is a diagnostic with a second thing
 * that can go wrong.
 */
#if APP_RCOUNT_PROBE

#define RCP_SENTINEL    0x0000A5A5uL   /* what the INNER context is poisoned with */
#define RCP_BASE_MARK   0x00005A5AuL   /* what the BASE context holds meanwhile */
#define RCP_LOOP_N      20000uL        /* outer REPEAT iterations (~200 us at IPL5) */
#define RCP_IPL_OUTER   5u
#define RCP_IPL_INNER   6u
#define RCP_SPIN_LIMIT  4000000uL      /* bounded wait: a vector that never fires must
                                        * report, not hang the console for ever */

/* IFS4<8..15> / IEC4<8..15>: QEI1-4, BISS1E, BISS1, CRC, ICD -- none of them used here.
 * Byte 1 keeps CCP9 (the high-res timer, IFS4<7>) out of every write below. */
#define RCP_IFS4_HI     (*((volatile uint8_t *)&IFS4 + 1))
#define RCP_IEC4_HI     (*((volatile uint8_t *)&IEC4 + 1))
#define RCP_QEI1_HI     0x01u          /* IFS4<8>  = byte1<0> : the INNER source */
#define RCP_QEI2_HI     0x02u          /* IFS4<9>  = byte1<1> : the OUTER source */

enum { RCP_MODE_POISON = 0u, RCP_MODE_PEEK = 1u, RCP_MODE_LOOP = 2u };

static volatile uint8_t  rcp_mode;
static volatile uint8_t  rcp_zero_rcount;      /* stage C: inner clears RCOUNT */
static volatile uint32_t rcp_entries;          /* inner ISR entries */
static volatile uint32_t rcp_capture;          /* RCOUNT as the inner saw it, FIRST thing */
static volatile uint32_t rcp_readback;         /* stage A: 0xA5A5 read back in-context */
static volatile uint32_t rcp_outer_done;       /* outer ISR completions */
static volatile uint32_t rcp_outer_elapsed;    /* high-res counts across the REPEAT loop */
static volatile uint32_t rcp_outer_rcount_end; /* RCOUNT after the loop, outer context */

/*
 * THE INNER ISR. The RCOUNT read is the FIRST statement, and the generated code
 * must have nothing above it -- no prologue, no loop, and above all no REPEAT of
 * the compiler own. Verified in the disassembly (see the objdump gate recorded
 * with this change): the body begins `mov.l _RCOUNT,w0`.
 *
 * `context` matches how every audio ISR in this image is declared, which is the
 * situation being modelled -- and on this compiler it also means no prologue.
 */
void __attribute__((interrupt, context)) _QEI1Interrupt( void )
{
    uint32_t r = RCOUNT;               /* FIRST. Nothing may be inserted above this. */
    rcp_capture  = r;
    rcp_entries += 1u;

    if( rcp_mode == RCP_MODE_POISON )
    {
        RCOUNT       = RCP_SENTINEL;
        rcp_readback = RCOUNT;         /* did the write land in THIS context at all? */
    }
    else if( rcp_mode == RCP_MODE_LOOP )
    {
        if( rcp_zero_rcount != 0u ) { RCOUNT = 0u; }   /* stage C: try to end the outer loop */
        RCP_IEC4_HI = 0u;              /* one-shot: the loop re-arms IF every iteration */
    }

    RCP_IFS4_HI = 0u;
}

/*
 * THE OUTER ISR: one REPEAT loop whose repeated instruction sets the inner IF.
 * The loop body is a single-instruction byte store, so the REPEAT is a true
 * hardware repeat of one instruction -- the same shape as the FIR kernel
 * `repeat.w w8 / mac.l ...` where the trap lands.
 */
void __attribute__((interrupt, context)) _QEI2Interrupt( void )
{
    volatile uint8_t *trig = (volatile uint8_t *)&IFS4 + 1;
    uint32_t          n    = RCP_LOOP_N;
    uint8_t           bit  = RCP_QEI1_HI;
    uint32_t          t0;

    RCP_IFS4_HI = 0u;                  /* our own flag (and the inner -- both ours) */

    t0 = nora_high_res_timer_get_count();
    __asm__ volatile ( "repeat %0        \n"
                       "mov.b  %1, [%2]  \n"
                       : : "r" (n), "r" (bit), "r" (trig) : "memory" );
    rcp_outer_elapsed    = nora_high_res_timer_elapsed_count( t0 );
    rcp_outer_rcount_end = RCOUNT;

    RCP_IFS4_HI     = 0u;
    rcp_outer_done += 1u;
}

/* Bounded wait on a volatile counter. Returns false on timeout, which is a RESULT
 * (the vector never fired) and not a reason to stop the probe. */
static bool rcp_wait( volatile uint32_t *counter )
{
    uint32_t spin = RCP_SPIN_LIMIT;
    while( ( *counter == 0u ) && ( spin != 0u ) ) { spin--; }
    return ( *counter != 0u );
}

static void rcp_run( void )
{
    uint32_t iec2_before, iec3_before, ifs2_before, ifs3_before;
    uint32_t iec2_after,  iec3_after,  ifs2_after,  ifs3_after;
    uint32_t a_readback, a_base_after, a_peek;
    uint32_t b_capture, b_elapsed, b_rc_end;
    uint32_t c_capture, c_elapsed, c_rc_end;
    bool     a1_ok, a2_ok, b_ok, c_ok;
    bool     hrt = nora_high_res_timer_is_initialized();

    printf( " \"*xr\" RCOUNT hardware-banking probe (#3.25)\n" );
    printf( "       IPL %u outer (QEI2) / IPL %u inner (QEI1); sentinel %04lX, base mark %04lX\n",
            (unsigned)RCP_IPL_OUTER, (unsigned)RCP_IPL_INNER,
            (unsigned long)RCP_SENTINEL, (unsigned long)RCP_BASE_MARK );
    printf( "       stopping the audio transport first; power-cycle or *sr afterwards\n" );

    audio_transport_stop();
    delay_ms( 100u );

    iec2_before = IEC2; iec3_before = IEC3;
    ifs2_before = IFS2; ifs3_before = IFS3;

    RCP_IEC4_HI = 0u;
    RCP_IFS4_HI = 0u;
    /* Byte 0 of IPC17 holds QEI1IP<0:2> and QEI2IP<4:6> and nothing else. */
    *(volatile uint8_t *)&IPC17 = (uint8_t)( ( RCP_IPL_OUTER << 4 ) | RCP_IPL_INNER );

    /* ---- stage A ------------------------------------------------------------
     * No printf between the base mark and the read-back: the printf path may use
     * a REPEAT of its own, which would overwrite the base context RCOUNT and make
     * the separation test meaningless. */
    rcp_mode        = RCP_MODE_POISON;
    rcp_zero_rcount = 0u;
    rcp_entries     = 0u;
    rcp_capture     = 0u;
    rcp_readback    = 0u;

    RCOUNT      = RCP_BASE_MARK;
    RCP_IEC4_HI = (uint8_t)( RCP_QEI1_HI | RCP_QEI2_HI );
    RCP_IFS4_HI = RCP_QEI1_HI;                  /* fire the inner */
    a1_ok        = rcp_wait( &rcp_entries );
    a_readback   = rcp_readback;
    a_base_after = RCOUNT;                      /* still the base context here */

    rcp_mode    = RCP_MODE_PEEK;
    rcp_entries = 0u;
    RCP_IFS4_HI = RCP_QEI1_HI;                  /* fire the inner again */
    a2_ok       = rcp_wait( &rcp_entries );
    a_peek      = rcp_capture;

    printf( "   A  inner write read-back  = %08lX  (expect %04lX if the write lands)  %s\n",
            (unsigned long)a_readback, (unsigned long)RCP_SENTINEL, a1_ok ? "" : "[NO ENTRY]" );
    printf( "   A  base RCOUNT after ISR  = %08lX  -> %s\n", (unsigned long)a_base_after,
            ( a_base_after == RCP_BASE_MARK ) ? "base untouched: contexts are DISTINCT"
                                              : "base overwritten: SHARED" );
    printf( "   A  inner re-entry capture = %08lX  -> %s  %s\n", (unsigned long)a_peek,
            ( a_peek == RCP_SENTINEL ) ? "own context survived: BANKED"
                                       : "sees the other context: SHARED",
            a2_ok ? "" : "[NO ENTRY]" );

    /* ---- stage B: capture mid-REPEAT, inner does NOT touch RCOUNT ---------- */
    rcp_mode          = RCP_MODE_LOOP;
    rcp_zero_rcount   = 0u;
    rcp_entries       = 0u;
    rcp_capture       = 0u;
    rcp_outer_done    = 0u;
    rcp_outer_elapsed = 0u;
    RCP_IEC4_HI = (uint8_t)( RCP_QEI1_HI | RCP_QEI2_HI );
    RCP_IFS4_HI = RCP_QEI2_HI;                  /* fire the OUTER */
    b_ok      = rcp_wait( &rcp_outer_done );
    b_capture = rcp_capture;
    b_elapsed = rcp_outer_elapsed;
    b_rc_end  = rcp_outer_rcount_end;

    printf( "   B  inner capture in-loop  = %08lX  -> %s  %s\n", (unsigned long)b_capture,
            ( b_capture == RCP_SENTINEL ) ? "outer live count invisible: BANKED"
                                          : "reads the outer countdown: SHARED",
            b_ok ? "" : "[OUTER DID NOT COMPLETE]" );
    printf( "       outer entries=%lu inner entries=%lu RCOUNT after loop=%08lX elapsed=%lu%s\n",
            (unsigned long)rcp_outer_done, (unsigned long)rcp_entries,
            (unsigned long)b_rc_end, (unsigned long)b_elapsed,
            hrt ? "" : "  [high-res timer not initialised]" );

    /* ---- stage C: same loop, inner clears RCOUNT (DS Early Termination) ---- */
    rcp_mode          = RCP_MODE_LOOP;
    rcp_zero_rcount   = 1u;
    rcp_entries       = 0u;
    rcp_capture       = 0u;
    rcp_outer_done    = 0u;
    rcp_outer_elapsed = 0u;
    RCP_IEC4_HI = (uint8_t)( RCP_QEI1_HI | RCP_QEI2_HI );
    RCP_IFS4_HI = RCP_QEI2_HI;
    c_ok      = rcp_wait( &rcp_outer_done );
    c_capture = rcp_capture;
    c_elapsed = rcp_outer_elapsed;
    c_rc_end  = rcp_outer_rcount_end;

    printf( "   C  inner capture in-loop  = %08lX   RCOUNT after loop=%08lX  %s\n",
            (unsigned long)c_capture, (unsigned long)c_rc_end,
            c_ok ? "" : "[OUTER DID NOT COMPLETE]" );
    printf( "   C  elapsed=%lu vs B=%lu  -> %s\n",
            (unsigned long)c_elapsed, (unsigned long)b_elapsed,
            ( !hrt ) ? "no timer: undecided"
                     : ( ( c_elapsed * 4u ) < b_elapsed )
                       ? "loop ENDED EARLY: RCOUNT shared/aliased, DS Early Termination is literal"
                       : "loop ran to completion: clearing RCOUNT from the ISR did NOT end it" );

    /* leave the probe vectors off; do not try to put the transport back */
    RCP_IEC4_HI = 0u;
    RCP_IFS4_HI = 0u;
    RCOUNT      = 0u;

    iec2_after = IEC2; iec3_after = IEC3;
    ifs2_after = IFS2; ifs3_after = IFS3;
    printf( "   iso IEC2 %08lX->%08lX  IEC3 %08lX->%08lX\n",
            (unsigned long)iec2_before, (unsigned long)iec2_after,
            (unsigned long)iec3_before, (unsigned long)iec3_after );
    printf( "   iso IFS2 %08lX->%08lX  IFS3 %08lX->%08lX  (non-zero = something else was live)\n",
            (unsigned long)ifs2_before, (unsigned long)ifs2_after,
            (unsigned long)ifs3_before, (unsigned long)ifs3_after );
    printf( "       transport is stopped and the probe left state as-is: *sr or power-cycle now\n" );
}

#endif /* APP_RCOUNT_PROBE */

void traps_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    // ---- Write actions (kind '*') -------------------------------------------------
    if( msg->kind == '*' )
    {
#if APP_STACK_WATERMARK
        if( msg->name == 'w' )
        {
            stack_wm_arm();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            return;
        }
#endif
#if APP_RCOUNT_PROBE
        if( msg->name == 'r' )
        {
            rcp_run();
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            return;
        }
#endif
#if APP_TRAP_TEST_CMDS
        switch( msg->name )
        {
        case 'a':
        case 'm':
        case 's':
            traps_console_force( msg->name );   /* normally does not return */
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_OK;
            return;

        default:
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
            return;
        }
#else
        /* Deliberately UNSUPPORTED rather than NOT_FOUND: the command exists, this image
         * was built without it. */
        printf( " \"*x\" trap tests are not compiled in (APP_TRAP_TEST_CMDS = 0)\n" );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
#endif
    }

    // ---- Read queries (kind '?') --------------------------------------------------
    if( msg->kind != '?' )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }

    switch( msg->name )
    {
#if APP_STACK_WATERMARK
    case 'w':   // ?xw : how close the deepest context came to SPLIM since *xw
        stack_wm_report();
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;
#endif

    case 'l':   // ?xl : the latch, on demand
        /*
         * Normally "none" plus a non-zero count, and that is not a contradiction: the boot
         * report consumes the RECORD, while the COUNTER survives it on purpose so a fault
         * that repeats every boot can be told from one that happened once. The full raw
         * register dump belongs to that boot report -- see app_traps_report_previous().
         */
        printf( " \"?xl\" last trap: %s  (the record is consumed by the boot report)\n",
                app_traps_id_str( app_traps_last_id() ) );
        printf( "       traps since power-on: %lu\n",
                (unsigned long)app_traps_count() );
#if APP_TRAP_TEST_CMDS
        printf( "       fire one: *xa address  *xm math  *xs stack\n" );
#endif
#if APP_STACK_WATERMARK
        printf( "       stack watermark: *xw arm, then ?xw read\n" );
#endif
#if APP_RCOUNT_PROBE
        printf( "       RCOUNT banking probe: *xr  (stops the transport; *sr afterwards)\n" );
#endif
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_OK;
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}
