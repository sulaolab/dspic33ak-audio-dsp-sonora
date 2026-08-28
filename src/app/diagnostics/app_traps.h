#ifndef APP_TRAPS_H
#define APP_TRAPS_H

/*
 * app_traps.h -- the dsPIC33AK hardware trap vectors, once, plus the boot-time report of
 * a trap that happened BEFORE this boot.
 *
 * PORTED FROM CK, AND FROM THIS REPO'S OWN EARLIER ATTEMPT
 * -------------------------------------------------------
 * Two ancestors, and the names come from the first one:
 *
 *   the dsPIC33CK lab tree's own app_traps.c -- the same design, matured on CK: eight
 *     vectors, a persistent latch, a boot report, a policy switch, and console commands
 *     that fire traps on demand. Its file names and function names are used here so the
 *     two families read as one module.
 *   an earlier AK attempt in this project (2026-08-02) -- which established the AK
 *     register set worth capturing and the write-order that makes a record trustworthy.
 *     It was picked up long after it was written, by which time the NORA rename had moved
 *     the reset API it called.
 *
 * WHAT CHANGED IN THE PORT, AND WHY
 * ---------------------------------
 *   - The latch lives in the hal_noinit_ram block (src/noinit_ram_layout.h) instead of
 *     raw __attribute__((persistent)) locals. On this repo's AK that HAL already exists and
 *     already owns the surviving-RAM question: it is the only thing that knows the range is
 *     KEEP + NOLOAD in the linker script and out of the stack's reach. The attribute alone
 *     does NOT protect a range -- it is a section request, and a request the best-fit
 *     allocator is free to place the automatic stack on top of. CK still uses the raw
 *     attribute because CK has no such HAL, not because the attribute is sufficient.
 *   - No policy switch. CK needs one because two boards want opposite behaviour
 *     (Curiosity Nano with no reset button wants latch-and-reset, DM330030 with a debugger
 *     attached wants spin). This repo builds one board family and the AK original already
 *     chose latch-and-reset, for the same reason the Nano does: the console is the only
 *     observer. A switch with one value is a switch nobody sets correctly the day a second
 *     value appears -- it can be added then, with a board that wants it.
 *
 * TWO HALVES, BECAUSE A TRAP CANNOT PRINT
 * ---------------------------------------
 * 1. The handlers run in trap context, where the machine state is by definition suspect.
 *    They do the least they can: latch which trap fired and the raw AK fault registers,
 *    then RESET. They do NOT print -- printing means calling the UART with an unknown
 *    stack, and a printf that faults inside a trap handler loses the evidence entirely.
 *    This repo's main.c had exactly that until this change: an _AddressErrorTrap that
 *    printf'd and then spun forever.
 * 2. app_traps_report_previous() runs from ordinary code after the console is up, reads the
 *    latch, prints it, and consumes it.
 *
 * It composes with the reset-cause report the boot banner already prints: a trap-induced
 * reset appears as SWR in the reset cause AND as a record here, which together say "the
 * firmware trapped and restarted" instead of a bare SWR that looks like someone typed *sr.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Trap-test console commands (*xa / *xm / *xs), implemented in uart_app/traps_console.c.
 *
 * On by default BECAUSE a trap handler is code that only runs when something has already
 * gone wrong -- the kind that rots unnoticed unless it can be fired on demand. Without
 * them, "the latch works" is an assumption; with them it is a thirty-second console check.
 * Set to 0 for a shipping image; ?xl stays available either way.
 *
 * Declared here, next to the handlers, rather than in traps_console.c, so anything that
 * describes the command set (the console module's own help line included) agrees with what
 * is actually compiled in.
 */
#ifndef APP_TRAP_TEST_CMDS
#define APP_TRAP_TEST_CMDS 1
#endif

/*
 * Stack high-water measurement console commands (*xw arm / ?xw read), implemented in
 * uart_app/traps_console.c.
 *
 * Measurement-only, so it follows the trap tests rather than standing on its own: an image
 * built for shipping (APP_TRAP_TEST_CMDS = 0) carries neither. It costs a few hundred bytes
 * of program memory and two words of RAM, and NOTHING in the audio path calls it -- the
 * region is only painted when a human asks.
 *
 * It exists because a priority change that lets two ISRs nest changes the stack requirement,
 * and the linker cannot tell you the new number: the stack is whatever hole is left after the
 * static allocation, so the only honest answer is a measured one.
 */
/*
 * RCOUNT hardware-banking probe console command (*xr), implemented in
 * uart_app/traps_console.c.
 *
 * Diagnostic-only, and it STOPS THE AUDIO TRANSPORT when it runs -- so it follows the trap
 * tests for the same reason the watermark does: an image built for shipping
 * (APP_TRAP_TEST_CMDS = 0) carries neither. Nothing calls it; it runs when a human types *xr.
 *
 * It exists because DS70005591C contradicts itself about whether RCOUNT is one of the
 * per-IPL hardware-context registers, and the whole mechanism proposed for this repo
 * STACK ERROR turns on the answer. See the block comment on the probe itself.
 */
#ifndef APP_RCOUNT_PROBE
#define APP_RCOUNT_PROBE  APP_TRAP_TEST_CMDS
#endif

#ifndef APP_STACK_WATERMARK
#define APP_STACK_WATERMARK  APP_TRAP_TEST_CMDS
#endif

/*
 * Which trap fired.
 *
 * Six, and that is this device's COMPLETE trap table, not an inherited guess: the IVT
 * disassembly shows all six installed in words 2..7, and the EDC device file names the
 * remaining two of the eight slots ReservedTrap0 and ReservedTrap7. CK's enum has eight
 * because the CK vector table has eight -- the two families are NOT expected to match here,
 * and the difference is not a parity defect. Measured 2026-08-12; see
 * [internal] ak_persistent_traps_port.md.
 */
typedef enum
{
    APP_TRAP_NONE = 0,
    APP_TRAP_BUS_ERROR,
    APP_TRAP_ILLEGAL_INSTRUCTION,
    APP_TRAP_ADDRESS_ERROR,
    APP_TRAP_STACK_ERROR,
    APP_TRAP_MATH_ERROR,
    APP_TRAP_GENERAL_ERROR,
    APP_TRAP_COUNT
} app_trap_id_t;

/*
 * Raw AK fault evidence saved by a vector before it resets the CPU.
 *
 * Kept as raw values on purpose. The DFP exposes FEX/FEX2 and the PC-related registers,
 * but their detailed decoding belongs in a later, separately validated pass -- and a raw
 * word cannot be wrong about a bit this file did not think to name. Saving them now is what
 * makes that decoding possible for faults already seen in the field.
 */
typedef struct
{
    uint16_t         schema_version;
    app_trap_id_t    id;
    uint32_t         intcon1;
    uint32_t         intcon3;
    uint32_t         intcon4;
    uint32_t         intcon5;
    uint32_t         pc;
    uint32_t         pctrap;
    uint32_t         fex;
    uint32_t         fex2;
    uint32_t         pchold;
    uint32_t         vfa;
    uint32_t         splim;
    uint32_t         rcon;
    /*
     * The stack pointer AS THE TRAP WAS TAKEN (schema 2 and later; 0 in a schema-1 record).
     *
     * PC/PCTRAP say WHERE the machine was; only W15 says whether a STACK ERROR was a stack
     * that grew honestly to SPLIM or a stack pointer that was already wrong. Those two want
     * opposite fixes -- more stack vs. find the corruption -- and no other field in this
     * record can tell them apart, which is why a measured demand far below the available
     * region could not settle the question.
     *
     * The value is the trapping W15 plus the hardware trap push plus this handler's own
     * rcall return address: a constant offset of a few words, verified in the disassembly
     * to contain no compiler prologue push. So it is exact to within that offset -- which
     * is orders of magnitude below the distinction it exists to make.
     */
    uint32_t         sp;
    uint32_t         fp;        /* W14 alongside it: a garbage frame pointer names the culprit */
    uint32_t         inttreg;   /* VECNUM (bits 0..8) + ILR (10..13): which vector, at what IPL */
    /*
     * The PC and SR that exception entry itself pushed (schema 3 and later; 0 before that).
     *
     * PCTRAP is not usable as the trap origin on this silicon: errata DS80001162E #1 (A1 and
     * A2, no workaround) exempts disabled math error traps from capturing it, and the
     * register behaves as a write-once latch, so one disabled arithmetic condition anywhere
     * pins it forever and every later trap reports that stale address. Measured: stuck
     * 0x2FDEC from the real site, byte-identical across eight records.
     *
     * The stacked copy cannot go stale that way -- hardware writes it at each entry. Read
     * from the SP captured above at -8 (PC) and -12 (SR); SR occupies the LOWER address, a
     * layout measured on a debugger-frozen capture rather than taken from the data sheet.
     *
     * stacked_sr is the field that decides between whole classes of cause: bit 4 RA says
     * whether a REPEAT loop was even active (RA=0 retired the REPEAT-race hypothesis),
     * bits 18:16 CTX and IPL (bit 8 as IPL3, bits 7:5 as IPL[2:0]) say which nesting level
     * was being entered, and CTX==IPL is what distinguishes a genuine frame from stale stack
     * garbage when walking outwards.
     */
    uint32_t         stacked_pc;
    uint32_t         stacked_sr;
} app_trap_record_t;

/*
 * Call exactly once, after nora_reset_latch_cause() and before anything can trap.
 *
 * A power-on or brown-out makes the surviving RAM untrustworthy, so both record and counter
 * are reinitialised in that case; every other reset class retains the evidence, which is
 * the whole point -- a trap resets the part, and the next boot is what reports it.
 *
 * The argument is nora_reset_snapshot_is_power_on_class(). It is passed in rather than
 * called here so this module needs no reset HAL of its own, and so a caller with a
 * different definition of "cold" cannot be silently overruled.
 */
void app_traps_boot_prepare( bool power_on_reset );

/*
 * Print and consume one valid previous-trap record. Prints nothing when the latch is empty,
 * so a clean boot stays quiet.
 *
 * Safe only in ordinary foreground code after stdio/UART initialisation. It is never called
 * from a trap vector, and it must not be: it prints.
 */
void app_traps_report_previous( void );

/* Clear the record without printing it, for a caller that reports it some other way.
 * The COUNTER deliberately survives this -- see app_traps_count(). */
void app_traps_clear( void );

/* Raw accessors, for a debugger or a console command. */
bool         app_traps_previous_get( app_trap_record_t *out );
app_trap_id_t app_traps_last_id( void );
const char  *app_traps_id_str( app_trap_id_t id );

/*
 * Traps since power-on. Survives the per-report clear (unlike the record), so this is what
 * distinguishes a deterministic fault from a one-off glitch. 0 = none since power-on.
 *
 * Readable on demand because the boot report is not always observable: a power cycle makes
 * the host re-enumerate the CDC, and the board prints its banner before the port is back.
 */
uint32_t app_traps_count( void );

#ifdef __cplusplus
}
#endif

#endif /* APP_TRAPS_H */
