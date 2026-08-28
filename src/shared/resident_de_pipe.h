#ifndef RESIDENT_BOOT_PIPE_H
#define RESIDENT_BOOT_PIPE_H

//===========================================================
// resident_boot_pipe.{c,h} -- the container the resident bootloader and the
// application pass messages through, built on hal_noinit_ram.
//
// THIS FILE IS THE SHARED CONTRACT. Both images compile it, so both agree on the
// address, the layout, and the version. Change anything here and both sides must be
// rebuilt -- which is what RESIDENT_BOOT_PIPE_VERSION is for.
//
// The HAL underneath reserves bytes and nothing more: it does not know or care what is
// in them. Markers, validity, one-shot consumption and publish ordering are this
// container's job.
//
// WHAT TRAVELS THROUGH IT, and why each record is separate
//
//   request   APP -> bootloader.  "enter update mode." ONE-SHOT: the bootloader consumes
//             it as it reads it, so a timeout in the update wait, or a reset button
//             press, leaves nothing behind and the application boots normally. Nothing
//             is erased, unlike invalidating the Flash manifest.
//
//   launch    bootloader -> APP.  Launch attempts since the last acknowledged success.
//             The bootloader increments it before handing over; the application zeroes it
//             once it has run long enough to call the boot a success. Past
//             RESIDENT_BOOT_PIPE_LAUNCH_LIMIT the bootloader stops handing over.
//
//   crash     trap handler -> next boot.  How the last run died. NOT one-shot: it
//             outlives being reported and only an explicit clear drops it.
//
//   cause     bootloader -> APP.  RCON as the bootloader found it, before it cleared
//             the cause bits. ONE-SHOT: it describes exactly one boot of the
//             application, so consuming it stops a later read reporting a stale cause.
//
//             The application CANNOT get this any other way in a delivery image. The
//             bootloader must clear RCON's cause bits so its own next reset is
//             unambiguous, and the hand-over to the application is itself a reset --
//             so by the time the application reads RCON, both the original cause and
//             the bootloader's clear are gone and every boot reads OTHER/unknown.
//             That is what this record repairs. Measured 2026-08-12: `?sr` reported
//             `OTHER (warm)` immediately after a `*sr` software reset.
//
// Three records, three independent markers -- deliberately not one checksum over the
// whole container. Their lifetimes are opposites: the request is consumed immediately
// while the crash record must survive. One shared checksum would mean clearing the
// request re-stamps the crash record, so a reset during that clear would destroy the
// crash evidence at exactly the moment it matters. Independent markers also let each
// record keep its own validity rule, which they need: a *request* is only meaningful
// right after a software reset, whereas a *crash* record is meaningful precisely after a
// trap-induced reset, which is not one.
//
// WHY 16-BIT FIELDS AND value/~value PAIRS
//   This is a 16-bit machine and the resident bootloader has about 1.5 KB of Flash left.
//   A first version of this container used 32-bit fields with an XOR check word, and the
//   launch counter alone compiled to 976 bytes -- every field being a 32-bit volatile
//   access. Halving the width and validating with a complement pair (the same shape as
//   resident_boot_probe_set's token/~token) does the same job in a fraction of the space.
//
//   A record is present iff its marker equals the expected value AND its companion is
//   the exact complement. The complement is what stops random power-on bits from looking
//   like a record; the PUBLISH ORDER is what protects the payload (see the .c).
//
// WHAT THIS CANNOT DO -- the cells are volatile
//   Everything here is lost on a power cycle, by physics. So the launch guard is
//   inherently warm-reset-scoped: an application that crash-loops is caught, but
//   power-cycling clears the count and grants a fresh start. Loop protection that
//   survives a power cycle would need Flash, not this.
//===========================================================

#include <stdbool.h>
#include <stdint.h>

//-----------------------------------------------------------------------
// The block this container lives in is described by shared/noinit_ram_config.h (address,
// size, and the linker-reservation promise), which hal_noinit_ram pulls in. Both images
// compile that one file, which is what makes them agree on the address. The device check
// lives there too, next to the address it constrains.
//-----------------------------------------------------------------------
// noinit_ram_layout.h, not nora_noinit_ram.h directly: the block has two tenants since
// 2026-08-11, so which bytes are this container's is stated there (and it includes the HAL).
#include "noinit_ram_layout.h"

// Bump whenever a record changes meaning, size, or POSITION: the bootloader and the
// application are updated independently, so an old reader can meet a new writer.
//
// APPENDING a self-proving record at the end does NOT need a bump, and the `cause`
// record added 2026-08-12 deliberately did not take one. The reasoning, because "the
// size changed and the version did not" is exactly the sort of thing that looks like an
// oversight later:
//
//   - Compatibility is already covered. Every record proves itself by marker plus
//     complement, so a bootloader that never wrote `cause` leaves the marker at zero and
//     the reader reports "absent" and falls back. That is the same mechanism the version
//     exists to provide, applied per record instead of per container.
//
//   - A bump would make one thing WORSE. Only the bootloader establishes the container
//     (init() is boot-side only), so a serial update -- which replaces the application
//     alone and leaves the resident image in place -- would leave a new application whose
//     ready() disagrees with the version the old bootloader established. Existing record
//     offsets are unchanged, so arming still works, but launch_count() would start
//     returning 0 on that pair and the launch guard's acknowledgement would silently stop
//     mattering. Nothing is gained in exchange.
//
// So: append freely, bump for anything that moves or redefines an existing field.
#define RESIDENT_BOOT_PIPE_VERSION ((uint16_t)0x0001)

// Launch attempts tolerated before the bootloader stops handing over.
#define RESIDENT_BOOT_PIPE_LAUNCH_LIMIT 10u

//-----------------------------------------------------------------------
// The launch guard is ON by default.
//
// It was default-OFF while Flash was the constraint: it needs code on BOTH sides and the
// CLASSIC serial-update application had 280 bytes free. Enabling
// -ffunction-sections/-fdata-sections on the application builds (they already had
// --remove-unused-sections, and the resident build already had the flags) freed about
// 56 KB there, so the application side now fits easily.
//
// The BOOTLOADER side is the tight one: it already had both flags, so it won nothing back,
// and this guard's counting code leaves it with 376 bytes free (0x7E88 of 0x8000). If you
// add to the bootloader and it no longer links, this guard is one of the few things in it
// that can be switched off to buy space back.
//
// ONE SWITCH CONTROLS BOTH SIDES, and that is not tidiness. Enabling only one half is
// worse than enabling neither: if the bootloader counts and the application never
// acknowledges, the count climbs on every boot and the board stops launching the
// application once it passes the limit. Never define this differently for the two builds.
//
// Set it to 0 to compile the guard out of both images.
//-----------------------------------------------------------------------
#if !defined(RESIDENT_BOOT_ENA_LAUNCH_GUARD)
#define RESIDENT_BOOT_ENA_LAUNCH_GUARD 1
#endif

// The crash record is still opt-in, but no longer for space reasons: writing one means
// touching the trap handlers, which is a separate piece of work. Its layout and accessors
// are defined here and ready, and NOTHING WRITES ONE YET -- so enabling this flag on its
// own only adds dead code. Enable it in the change that wires the handlers.
#if !defined(RESIDENT_BOOT_ENA_CRASH_RECORD)
#define RESIDENT_BOOT_ENA_CRASH_RECORD 0
#endif

// Distinct markers, so a record cannot be mistaken for its neighbour if an offset slips.
#define RESIDENT_BOOT_PIPE_REQUEST_MARKER ((uint16_t)0x5251) /* "RQ" */
#define RESIDENT_BOOT_PIPE_CRASH_MARKER   ((uint16_t)0x4353) /* "CS" */
#define RESIDENT_BOOT_PIPE_CAUSE_MARKER   ((uint16_t)0x4358) /* "CX" */

typedef struct
{
    uint16_t version;       // RESIDENT_BOOT_PIPE_VERSION
    uint16_t version_inv;

    uint16_t request;       // REQUEST_MARKER while an update request is pending
    uint16_t request_inv;

    uint16_t launch;        // launch attempts since the last acknowledged success
    uint16_t launch_inv;

    uint16_t crash;         // CRASH_MARKER while a breadcrumb is present
    uint16_t crash_inv;
    uint32_t crash_vector;  // which trap / interrupt vector
    uint32_t crash_detail;  // vector-specific detail
    uint32_t crash_sr;      // CPU status at the crash
    uint32_t crash_splim;   // stack limit
    uint32_t crash_w15;     // stack pointer

    uint16_t cause;         // CAUSE_MARKER while a forwarded reset cause is present
    uint16_t cause_inv;
    uint32_t cause_rcon;    // RCON as the bootloader found it, cause bits still set
} resident_boot_pipe_t;

// Outgrowing the reservation is a build error, not an overrun into whatever follows.
// Bounded by the pipe's REGION, not the whole block: the neighbour's 80 bytes are not
// this container's to grow into.
NOINIT_RAM_PIPE_FITS( resident_boot_pipe_t );

//-----------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------

// Establish the container if it cannot be trusted, and do nothing otherwise. Call once,
// early, BEFORE reading any record. It does not clear valid records, which is what makes
// that ordering safe -- anything that wipes the container before the reader looks makes
// an incoming message vanish with no trace.
//
// `distrust` should be the reset HAL's power-on-class answer (POR or BOR): after a power
// cycle the cells hold whatever they hold, so the container is re-established regardless
// of what appears to have survived. Establishment is otherwise LAZY -- a wrong version
// re-establishes -- so one mechanism covers power-on, the first boot after programming,
// corruption, and a firmware whose layout changed.
void resident_boot_pipe_init( bool distrust );

// True if the container currently holds this build's layout version.
bool resident_boot_pipe_ready( void );

//-----------------------------------------------------------------------
// request -- APP writes, bootloader consumes
//-----------------------------------------------------------------------

// Publish an update request. The caller resets afterwards; the bootloader picks it up.
void resident_boot_pipe_request_set( void );

// Consume a pending request. True once and only once per request: the record is cleared
// before this returns.
bool resident_boot_pipe_request_take( void );

//-----------------------------------------------------------------------
// launch -- bootloader counts, APP acknowledges
//-----------------------------------------------------------------------

// Attempts since the last acknowledged success. 0 if the record is absent or damaged --
// a bad byte must not lock a healthy board out of its application.
uint16_t resident_boot_pipe_launch_count( void );

// Set the counter. One setter rather than bump/clear, because both callers already know
// the value: the bootloader has just read the count and writes count+1 before handing
// over, and the application writes 0 to declare success.
//
// WHEN the application writes 0 is the whole design of the mechanism, and the tempting
// answer is wrong. Doing it at the end of initialization defeats the guard: an
// application that initializes cleanly and then dies in its main loop would clear the
// count on every boot, so the count never climbs and the loop is never caught. Write 0
// only once the application has run long enough that a boot loop is ruled out -- seconds
// of main-loop time, not the end of setup.
void resident_boot_pipe_launch_set( uint16_t count );

//-----------------------------------------------------------------------
// crash -- trap handler writes, next boot reports
//-----------------------------------------------------------------------

// Record how this run is dying. Called from a trap handler, so it does the minimum:
// plain stores, marker last, no library calls.
//
// Known limit: a STACK OVERFLOW cannot be recorded this way, because the handler's own
// prologue overflows too. RCON's latch is the only evidence that survives that one.
void resident_boot_pipe_crash_set( uint32_t vector, uint32_t detail,
                                   uint32_t sr, uint32_t splim, uint32_t w15 );

// Read the breadcrumb without consuming it. Returns false if none is present.
bool resident_boot_pipe_crash_peek( uint32_t *vector, uint32_t *detail,
                                    uint32_t *sr, uint32_t *splim, uint32_t *w15 );

// Drop the breadcrumb once it has been reported.
void resident_boot_pipe_crash_clear( void );

//-----------------------------------------------------------------------
// cause -- bootloader publishes, APP consumes
//-----------------------------------------------------------------------

// Publish RCON as this image found it, BEFORE clearing the cause bits. Call once, early,
// after pipe_init() -- init() zeroes the container, so publishing first would lose it.
void resident_boot_pipe_cause_publish( uint32_t rcon );

// Is a published cause still waiting to be consumed? Non-destructive.
//
// It exists because the bootloader runs TWICE per launch: the first pass sees the real
// reset (POR, EXTR, ...) and hands the application over with a software reset, and the
// second pass therefore sees SWR. Publishing on both passes overwrote the real cause with
// the hand-over's own SWR, so a genuine power cycle reached the application as "warm" --
// which is what silently disabled every power-on-only feature downstream (the boot-banner
// hold, the cold codec start). The second pass must keep the record the first one left.
bool resident_boot_pipe_cause_pending( void );

// Consume the forwarded RCON. True once and only once: the record is cleared before this
// returns, so a later read cannot report a cause that belonged to an earlier boot.
//
// False means "not forwarded" -- a resident image that predates this record, a standalone
// image with no resident at all, or a container that was just re-established. The caller
// must fall back to reading RCON itself, which is the behaviour that existed before.
bool resident_boot_pipe_cause_take( uint32_t *rcon );

#endif // RESIDENT_BOOT_PIPE_H
