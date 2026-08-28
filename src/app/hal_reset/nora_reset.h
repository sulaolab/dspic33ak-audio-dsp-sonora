#ifndef NORA_RESET_H
#define NORA_RESET_H

//===========================================================
// nora_reset.h -- capture and decode the core reset cause (RCON).
//
// WHY THIS EXISTS
//   The RCON register records WHY the core last reset (power-on, brown-out, the
//   MCLR pin / reset button, a software reset, watchdog time-out, ...). Each event
//   only SETS its flag; the flags accumulate until software clears them. So the
//   correct pattern is: read RCON ONCE at the very start of main(), latch the
//   decoded cause, then clear the flags -- the next boot then reflects only the
//   newest reset.
//
//   Knowing the reset cause lets startup logic be driven by PROVENANCE instead of
//   by probing peripheral state over a bus. In particular the audio path uses it to
//   decide whether the WM8904 codecs need a pre-shutdown/quiesce before re-init:
//     - POWER-ON / BROWN-OUT : the whole board (dsPIC AND codecs) lost power, so the
//       codecs are at their own reset defaults (HPOUT dead). A pre-shutdown is
//       redundant  -> COLD START.
//     - EXTERNAL (MCLR) / SOFTWARE / WATCHDOG : only the dsPIC reset while the board
//       rails -- and thus a codec, notably WM8904-B on its own XTAL -- kept power and
//       a possibly-live/charged HPOUT. Re-init must quiesce it first -> HOT START
//       (assume the codec may be half-alive).
//   Reset provenance is therefore the deterministic, codec-independent input to
//   the boot-time pre-shutdown decision.
//
// SCOPE
//   RCON only reflects genuine CORE resets. A firmware-initiated audio restart
//   (e.g. an in-place *tr / rate change) is NOT a chip reset and does not touch
//   RCON -- those paths already force a quiesce on their own. The reset cause only
//   informs the FIRST (boot / INITIAL_START) bring-up.
//
// WHY THE LATCH POLICY IS AN ARGUMENT AND NOT A BUILD OPTION
//   Boards genuinely differ. A board that latches and clears at the top of its
//   bring-up has exactly one boot's worth of causes present, and may name one. A
//   board that deliberately leaves RCON alone -- so a debugger still sees what the
//   last reset set -- has bits that ACCUMULATED across resets, and naming "the"
//   cause from an accumulation is a plausible-looking wrong answer. Hence
//   nora_reset_snapshot_capture() takes the policy, and a PRESERVE capture keeps
//   the raw word but reports UNKNOWN rather than guessing.
//
// PORTABILITY (this is the AK/CK common face; see hal_reset/README of each target)
//   - The classification enum, the policy enum, and every function below are
//     identical on dsPIC33A (AK) and dsPIC33C/CK. Application code branches on
//     nora_reset_snapshot_is_power_on_class() / _is_warm() and never sees RCON.
//   - `latched` is uint32_t on the portable face even though CK's RCON is 16 bits:
//     the face is widened once, at the seam, so one prototype serves both.
//   - The CAUSE TABLE behind nora_reset_cause_str() is a family fact and is NOT
//     the same on both: dsPIC33A RCON has no TRAPR and no IOPUWR bit (it has CM,
//     the configuration-mismatch reset, instead), while CK has TRAPR -- which on CK
//     is the only evidence a stack overflow ever happened. Each backend therefore
//     owns its own table; only the semantics below are shared.
//
// THE TWO PRECEDENCES DIFFER, AND BOTH ARE RIGHT
//   nora_reset_snapshot_cause() is power-event-first (POR, BOR, EXTR, SWR, WDTO).
//   nora_reset_cause_str() is most-specific-first (CM, WDTO, SWR, EXTR, POR, BOR).
//   They disagree in practice: a cold start with MCLR held while the supply comes
//   up sets POR and BOR and EXTR together, and the first names it POWER_ON while
//   the second names it EXTR(MCLR). That is intended, because they answer different
//   questions -- one is a cold/warm CLASSIFICATION an application branches on (a
//   supply that has just come up is cold no matter what else was asserted during
//   it; calling that boot warm would skip a codec pre-shutdown on a genuinely cold
//   start), the other is a DIAGNOSTIC naming the most specific thing RCON can say.
//   Do not "fix" either order to match the other, and do not assume they agree.
//===========================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What capture() is allowed to do to RCON. PRESERVE keeps a raw diagnostic word but
// never names a cause (sticky bits may predate this boot). AND_CLEAR captures,
// classifies, and clears the cause bits, so its cause is authoritative for this boot.
typedef enum
{
    NORA_RESET_LATCH_PRESERVE_RCON  = 0,
    NORA_RESET_LATCH_AND_CLEAR_RCON = 1,
} nora_reset_latch_policy_t;

typedef enum
{
    // Nothing captured yet, captured under PRESERVE (see above), or no known flag
    // was set. Treated as WARM (conservative: quiesce) by the helpers below.
    NORA_RESET_CAUSE_UNKNOWN = 0,
    // POR: genuine power-on. Board rails (and codecs) were off -> COLD.
    NORA_RESET_CAUSE_POWER_ON,
    // BOR without POR: brown-out dip below the BOR threshold then recovery. Rails
    // moved, so codecs likely browned out too -> treated as COLD (power-on class).
    NORA_RESET_CAUSE_BROWNOUT,
    // EXTR: the MCLR pin (reset button / debugger). dsPIC-only reset; rails kept -> WARM.
    NORA_RESET_CAUSE_EXTERNAL,
    // SWR: a software reset (__builtin_software_reset / RESET instruction). WARM.
    NORA_RESET_CAUSE_SOFTWARE,
    // WDTO: watchdog time-out reset. dsPIC-only; rails kept -> WARM.
    NORA_RESET_CAUSE_WATCHDOG,
    // A configuration-mismatch / other core reset. Treated as WARM (conservative).
    NORA_RESET_CAUSE_OTHER,
} nora_reset_cause_t;

// Read RCON ONCE and snapshot it, applying `policy`. Call this as the FIRST thing in
// main(), before any clock/port/peripheral init (RCON is readable at any clock).
// Returns true only for the first valid capture after reset; a second call, or an
// out-of-range policy, returns false WITHOUT reading or clearing RCON -- so a stray
// second call can no longer destroy the latched cause (the old void latch_cause()
// silently re-read an already-cleared RCON and downgraded the cause to UNKNOWN).
bool nora_reset_snapshot_capture(nora_reset_latch_policy_t policy);

// Latch a raw RCON word captured by an EARLIER BOOT STAGE instead of reading RCON here.
//
// For images that a bootloader launches. A bootloader has to clear RCON's cause bits so
// its own next reset is unambiguous, and on this part the hand-over is itself a reset, so
// by the time this image runs there is nothing left in RCON to classify: every boot would
// name OTHER/unknown. The stage that did see the real word passes it forward and this
// latches it, classified exactly as an AND_CLEAR capture would have classified it.
//
// It does NOT touch RCON -- there is nothing there worth clearing, and the earlier stage
// already did it. Same one-shot rule as capture(): returns false if a capture already
// happened, so ordinary boots can call this first and fall back to capture() when it
// returns false without risking a double latch.
//
// Where the word comes from is not this HAL's business: the caller supplies it.
bool nora_reset_snapshot_capture_forwarded(uint32_t raw);

// True once a capture has succeeded. Distinguishes "cause is UNKNOWN because nobody
// captured" from "captured, and the cause genuinely is not nameable".
bool nora_reset_snapshot_is_captured(void);

// The cause classified at capture time. UNKNOWN until an AND_CLEAR capture runs.
nora_reset_cause_t nora_reset_snapshot_cause(void);

// The raw RCON word read at capture time (for logging / diagnostics). 0 until captured.
uint32_t nora_reset_snapshot_raw(void);

// Short human-readable label for the classified cause (for the boot banner).
const char *nora_reset_snapshot_cause_str(void);

// True for a power-event reset (POWER_ON or BROWNOUT): the codecs power-cycled with
// the board, so a boot-time codec pre-shutdown/quiesce is redundant (COLD START).
bool nora_reset_snapshot_is_power_on_class(void);

// True for a warm reset (EXTERNAL / SOFTWARE / WATCHDOG / OTHER / UNKNOWN): only the
// dsPIC reset while the board rails -- and a codec -- may have kept power. Boot must
// quiesce the codec first (HOT START). UNKNOWN maps here so an un-captured or novel
// cause errs on the safe side (quiescing is never harmful, just a short delay).
bool nora_reset_snapshot_is_warm(void);

// Name the most specific cause in `latched` (see the two-precedence note above).
//
// `latched` must be an RCON word, either captured before anything could clear it or
// read live. `is_latched` says which: pass false when the bits may have accumulated
// over more than one reset, and the return value refuses to name a single cause when
// more than one candidate is present rather than reporting the highest-priority one
// as though it were the whole story.
//
// Returns a pointer to a string literal -- never NULL, never a buffer, so it is safe
// to hand straight to a console writer from a trap handler.
const char *nora_reset_cause_str(uint32_t latched, bool is_latched);

// Clear every cause bit this family defines. Call immediately after capturing the
// word, so the NEXT boot reports its own cause instead of an accumulation.
// AND_CLEAR captures already do this; boards that do not latch must not call it,
// because leaving RCON alone is what makes a live read meaningful.
void nora_reset_cause_clear(void);

#ifdef __cplusplus
}
#endif

#endif // NORA_RESET_H
