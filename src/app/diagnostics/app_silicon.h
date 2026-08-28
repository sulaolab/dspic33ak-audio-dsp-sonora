#ifndef APP_SILICON_H
#define APP_SILICON_H

//===========================================================
// app_silicon.h -- which die is this, and is it the one this image was built for?
//
// WHY THIS EXISTS
// ---------------
// A STACK ERROR trap that no clause of the data sheet can explain was measured on this
// project's AK512 boards, and the die those boards carry is A1 (DEVREV 0x1) while the
// errata document describes A2 as current. Whether the fault is silicon-revision specific
// is not yet known -- but the moment two revisions are on the bench, every result becomes
// ambiguous unless the image says which die produced it. A run whose revision is not on
// the record is a run that has to be repeated.
//
// So this module does two separable things, and both were asked for:
//   1. IDENTIFY, always. DEVID and DEVREV go in the boot banner unconditionally, at no
//      cost, so no log can be silent about which die it came from -- and so a workaround
//      that must apply to one revision only has a runtime predicate to hang off.
//   2. REFUSE, on request. An image may declare the revision it was validated against;
//      running it on a different die then stops the audio engine before it starts.
//
// WHY IT IS APP-LEVEL AND NOT A nora_ HAL
// ---------------------------------------
// Reading a device ID is a genuinely portable idea and would sit well next to nora_udid,
// which reads its words from absolute addresses exactly the same way. It is deliberately
// NOT there yet: the addresses here are this device family's, the revision names are this
// family's, and the reason the module exists at all is one open investigation on one part.
// Publishing an API before there is a second family to shape it would fix the wrong shape.
// Promote it if a second family needs it -- the read is four lines.
//===========================================================

#include <stdbool.h>
#include <stdint.h>

//-----------------------------------------------------------------------
// WHICH REVISION THIS IMAGE EXPECTS.
//
//   0  announce only -- read it, print it, never refuse. THE DEFAULT, deliberately:
//      A1 is the silicon this project has in quantity and must keep being able to run,
//      so an image that blocked it by default would stop the work it exists to support.
//   1  require A1
//   2  require A2
//
// Set it as a build define (or override here) when a run must not be attributed to the
// wrong die -- e.g. an image carrying a workaround that is only valid on one revision, or
// an acceptance run whose result would be meaningless if the board were swapped.
//
// A MISMATCH STOPS THE AUDIO ENGINE, NOT THE CPU. The console still comes up and still
// prints why, because a board that answers "wrong die, here is what I am" is diagnosable
// and one that sits dark is not. Nothing in the ASRC signal path is started, so the fault
// under investigation cannot occur in that state either.
//-----------------------------------------------------------------------
#ifndef APP_SILICON_EXPECTED_REV
#  define APP_SILICON_EXPECTED_REV 0
#endif

#if ( APP_SILICON_EXPECTED_REV != 0 ) && ( APP_SILICON_EXPECTED_REV != 1 ) && \
    ( APP_SILICON_EXPECTED_REV != 2 )
#  error "APP_SILICON_EXPECTED_REV must be 0 (announce only), 1 (require A1) or 2 (require A2)."
#endif

typedef enum
{
    /*
     * UNKNOWN is not an error code -- it is "the identification did not answer", and it
     * NEVER refuses. A read that comes back as erased flash or as a DEVID this file does
     * not recognise says nothing about the die; refusing on it would turn a gap in this
     * module's knowledge into a board that will not run.
     */
    APP_SILICON_REV_UNKNOWN = 0,
    APP_SILICON_REV_A1      = 1,
    APP_SILICON_REV_A2      = 2
} app_silicon_rev_t;

/* Raw words, so a value this file did not think to name is still on the record. */
uint32_t          app_silicon_devid( void );
uint32_t          app_silicon_revid( void );

/* The revision as an enum, for a workaround that must apply to one die only. */
app_silicon_rev_t app_silicon_rev( void );

/* "A1" / "A2" / "unknown" -- never NULL. */
const char       *app_silicon_rev_str( app_silicon_rev_t rev );

/*
 * The part name for the DEVID that was read -- "dsPIC33AK512MPS512", or "unknown device"
 * for a DEVID this file does not name. Never NULL. The banner prints the name rather than
 * only the number because a log is read by people, and "0000A77C" is not a part.
 */
const char       *app_silicon_device_str( void );

/*
 * ONE LINE IN THE BOOT BANNER, unconditionally:
 *
 *     Silicon: dsPIC33AK512MPS512 rev A1 (DEVID=0000A77C DEVREV=00000001)
 *
 * Called from printMenu(), so the die revision sits with the rest of the identity block
 * (build, commit, preset, reset cause, UDID) instead of trailing whatever init happened to
 * print last. That grouping is the point: a log pasted into a report carries the banner,
 * and the banner has to be enough to attribute the run to a die. It prints and nothing
 * else -- no policy, no refusal -- so it is safe to call as often as the banner is printed.
 */
void              app_silicon_print_identity( void );

/*
 * Answer whether the audio engine may start. Prints ONLY on refusal (the identity itself
 * is already in the banner, above).
 *
 * true  = start it (the expected revision, or no expectation declared, or unidentifiable)
 * false = do not start it; the reason has already been printed.
 *
 * Call it once, after the banner, before sonora_app_prepare().
 */
bool              app_silicon_check( void );

#endif // APP_SILICON_H
