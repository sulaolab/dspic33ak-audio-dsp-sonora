#ifndef DMA_SELFTEST_H
#define DMA_SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "nora_dma.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dma_selftest.h -- standalone proof that the DMA controller itself moves data.
 *
 * WHERE THIS CAME FROM, AND WHY IT IS HERE UNCHANGED
 * --------------------------------------------------
 * This file and its .c are the dsPIC33CK application src/app/dma_selftest.{c,h},
 * moved to dsPIC33A. They are not a rewrite: the NORA DMA contract review named
 * exactly this move as the conformance gate for hal_dma --
 *
 *   "Conformance is an application moving, not a header matching. DMA: CK's
 *    src/app/dma_selftest.c builds and runs on AK with no change other than the
 *    channel and trigger enumerators it names."
 *   -- [internal] nora_dma_tdm_contract_draft_2026-08-10.md, section 4
 *
 * so the value of the file is in how little of it had to change. The complete diff
 * against the CK original, and the two contract gaps the move exposed, are in
 * [internal] nora_dma_conformance_gate_2026-08-10.md. Keep the two
 * copies convergent: an edit here that is not a dsPIC33A necessity is an edit that
 * makes the gate meaningless.
 *
 * The comments below are the CK author's, retained verbatim, including the
 * references to that side's history and file layout.
 *
 * WHY THIS EXISTS AS ITS OWN STEP
 * -------------------------------
 * When a DMA-fed peripheral transport does not stream, the symptom ("no transfers ever
 * happened") is identical whether the DMA controller is misconfigured, the trigger
 * source is wrong, or the peripheral never raises its event. One flash cycle cannot
 * distinguish them by staring at the transport. This test removes the peripheral from
 * the loop entirely: RAM -> RAM, triggered by software (DMACHn.CHREQ). A PASS narrows a
 * remaining transport failure to the trigger/peripheral side; a FAIL says stop looking
 * at the SPI. That ordering is what found two of the three DMA defects in
 * [internal] ck_silicon_findings.md.
 *
 * WAS boards/ev88g73a/ev88g73a_dma_selftest.{c,h}
 * ----------------------------------------------
 * Measured before moving it: zero pins, zero ports, zero board registers. Its only
 * board-shaped content was the CHANNEL NUMBER -- and a channel number is not a board
 * fact either, it is an allocation the application makes (the SPI/TDM transport owns 0
 * and 1, see nora_spi_i2s_tdm_conf.h). So the channel became the argument below,
 * and the caller states its own allocation.
 *
 * Output goes through app/console_out.h, so it works on any board that implements the
 * seam rather than on the one whose UART function it used to call by name.
 */

/*
 * Run the test on `channel`. Prints its own one-line result and returns true on PASS.
 *
 * The channel must be one nothing else is using: it is configured, enabled, triggered,
 * then left disabled with its status and IRQ flag cleared, so it is safe to run before
 * a transport starts -- but not safe to point at a channel a running transport owns.
 */
bool dma_selftest_run(nora_dma_channel_t channel);

#ifdef __cplusplus
}
#endif

#endif /* DMA_SELFTEST_H */
