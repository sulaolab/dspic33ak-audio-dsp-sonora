#ifndef RESIDENT_DE_APP_HANDOFF_H
#define RESIDENT_DE_APP_HANDOFF_H

#include <stdbool.h>
#include <stdio.h>

/* Everything the application must do *because* it was launched by the resident
 * download engine rather than by a hardware reset.
 *
 * A resident handoff is a branch, not a reset: the CPU interrupt-controller reset
 * values are never applied, and global interrupts are masked while the engine
 * replaces IVTBASE. So the relocated image has to finish the job itself -- clear
 * every IEC/IFS and the sticky CPU trap status, then restore SR/GIE once its own
 * timer and UART state are installed.
 *
 * This header is the ONLY place the delivery mode is tested on the application
 * side. In a standalone image every entry point below is an empty inline, so
 * main.c calls them unconditionally and carries no #if of its own. */

#if defined(SONORA_DELIVERY_SERIAL_UPDATE_APP)

/* Bring-up trace: raw U1TXB writes, usable before the UART HAL exists. Kept
 * switchable because the markers are bring-up evidence, not a product feature --
 * see RESIDENT_DE_ENA_HANDOFF_TRACE in resident_de_app_handoff.c. */
void resident_de_app_handoff_mark(char marker);

/* First statement of main(): snapshot the CPU state the engine handed over, then
 * apply the interrupt-controller reset the branch skipped. Leaves GIE masked. */
void resident_de_app_handoff_entry(void);

/* Call once the image's timer and UART state are installed: restores SR and GIE. */
void resident_de_app_handoff_interrupts_resume(void);

/* Report the handed-over CPU state (and any XRAMECC hits) now that printf works. */
void resident_de_app_handoff_report(void);

/* Call from the top of the main loop. Once the loop has genuinely been running,
 * zeroes the engine's launch-attempt counter -- which is what tells it this boot
 * succeeded, and what makes a start-up crash loop trip the launch guard. */
void resident_de_app_launch_ack_tick(void);

/* Latch the reset cause the resident engine captured before it cleared RCON, so this
 * image can name its own reset. Call as the FIRST reset-related statement in main(),
 * ahead of nora_reset_snapshot_capture().
 *
 * Returns false when nothing was forwarded -- a resident image older than the pipe's
 * cause record, or a container just re-established -- and the caller must then fall back
 * to capturing RCON itself, which is what it did before this existed.
 *
 * Why it has to exist at all: the engine must clear RCON's cause bits to keep its own
 * next reset unambiguous, and the launch hand-over is a reset too, so an application in a
 * delivery image reads a clean RCON and can only ever report OTHER/unknown. That is not
 * cosmetic -- audio_transport's INITIAL_START decides COLD vs HOT codec start from the
 * reset cause and calls it "the single source of truth", so an always-warm answer makes
 * every boot pay the pre-shutdown, and boot_banner_hold_if_requested() never triggers. */
bool resident_de_app_latch_forwarded_reset_cause(void);

/* True only if a resident download engine is actually installed AND speaks this build's
 * cross-reset layout -- i.e. an update request published by *fu5A can be picked up.
 *
 * This exists because the delivery mode is a compile-time fact and the pairing is not:
 * building this configuration in MPLAB X and programming it from the IDE puts the
 * application in Flash with NO engine behind it (buildtools/README.md "Support scope"),
 * and every compile-time answer is then wrong. Ask this instead of assuming.
 *
 * Two conditions, because neither alone is sound:
 *   - a programmed boot region, which is the only evidence that survives a warm reset;
 *   - resident_boot_pipe_ready(), which rejects an engine of an incompatible generation.
 * See the implementation for why the SRAM container alone gives a false "present".
 *
 * Costs one Flash page scan; call it from console verbs and banners, not from a hot path. */
bool resident_de_app_resident_is_present(void);

/* Boot-banner lines owned by the delivery mode. */
void resident_de_app_launch_banner(void);
void resident_de_app_delivery_banner(void);

#else

static inline void resident_de_app_handoff_mark(char marker) { (void)marker; }
static inline void resident_de_app_handoff_entry(void) { }
static inline void resident_de_app_handoff_interrupts_resume(void) { }
static inline void resident_de_app_handoff_report(void) { }
static inline void resident_de_app_launch_ack_tick(void) { }
static inline void resident_de_app_launch_banner(void) { }

/* No engine ran before this image, so RCON still holds the real cause and the caller's
 * own capture is the right answer. */
static inline bool resident_de_app_latch_forwarded_reset_cause(void) { return false; }

/* A standalone image is not built to be paired with one, and its own console has no
 * update verb to guard. */
static inline bool resident_de_app_resident_is_present(void) { return false; }

static inline void resident_de_app_delivery_banner(void)
{
    printf(" Delivery: application (single panel)\n");
}

#endif

#endif
