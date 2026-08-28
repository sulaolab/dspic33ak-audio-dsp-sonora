/*
 * dma_selftest.c -- see the header for what this proves and why it is a step of its
 * own. No board-ownership guard: nothing in here is board-specific, which is why it
 * left boards/ev88g73a/.
 *
 * This is the dsPIC33CK original moved to dsPIC33A as the hal_dma conformance gate
 * (see the header). Every line that differs from the CK copy is listed in
 * [internal] nora_dma_conformance_gate_2026-08-10.md; do not edit this
 * file for style, only for a dsPIC33A necessity.
 */

#include "dma_selftest.h"

#include <stdint.h>
#include <stdbool.h>

#include "console_out.h"
#include "nora_dma.h"

/* The channel is the caller's to choose -- it is the one who knows which channels its
 * transport already owns. See the header. */

#define DMA_SELFTEST_ELEMS    (4u)

/* Bounded spin. Four 16-bit RAM->RAM elements complete in well under a
 * microsecond at Fcy = 100 MHz; this is a hang guard, not a timing figure. */
#define DMA_SELFTEST_SPINS    (10000u)

/*
 * Both buffers must live in RAM: DMASRCn/DMADSTn are data-space addresses, so a
 * `const` source in program memory is not reachable by the DMA and would fail the
 * test for the wrong reason. Filled at run time for the same reason.
 */
static uint16_t s_src[DMA_SELFTEST_ELEMS];
static uint16_t s_dst[DMA_SELFTEST_ELEMS];

bool dma_selftest_run(nora_dma_channel_t channel)
{
    nora_dma_channel_cfg_t cfg;
    uint16_t spins;
    nora_dma_status_t status;
    uint32_t remaining;
    uint8_t  i;
    bool     data_ok = true;
    bool     completed;

    for (i = 0u; i < (uint8_t)DMA_SELFTEST_ELEMS; i++) {
        /* A distinct, non-zero, non-repeating pattern: zeros would also "pass"
         * against a destination the DMA never touched, and a constant would hide a
         * stuck address pointer. */
        s_src[i] = (uint16_t)(0xA500u + i);
        s_dst[i] = 0u;
    }

    cfg.src      = (volatile void *)s_src;
    cfg.dst      = (volatile void *)s_dst;
    cfg.count    = DMA_SELFTEST_ELEMS;
    cfg.src_mode = NORA_DMA_ADDR_INCREMENT;
    cfg.dst_mode = NORA_DMA_ADDR_INCREMENT;
    cfg.size     = NORA_DMA_SIZE_HALFWORD;
    /* Continuous: one trigger moves the whole block (DS70005399D, DMACHn Note 3
     * -- how many transfers a set CHREQ performs is decided by TRMODE). */
    cfg.tr_mode  = NORA_DMA_TRMODE_CONTINUOUS;

    cfg.reload_count = false;
    cfg.reload_src   = false;
    cfg.reload_dst   = false;

    cfg.half_int_en  = false;
    cfg.done_int_en  = false;

    /* Software-triggered only, so no peripheral event drives this transfer -- but the
     * select field still names something, and it has to name a source that cannot fire
     * during the test window, or a stray event would trigger a second transfer and
     * corrupt the result. That requirement is what NORA_DMA_TRIGGER_NONE states; which
     * code is quiet is the backend's knowledge, not this test's. */
    cfg.trigger = NORA_DMA_TRIGGER_NONE;

    cfg.irq_priority_set = false;
    cfg.irq_priority     = 0u;
    cfg.irq_enable       = false;   /* polled; no ISR for this channel */

    /* The channel is printed rather than baked into the text: with the channel a
     * parameter, a hard-coded "ch3" would eventually be a lie in a log. */
    console_out_str("DMA selftest (ch");
    console_out_u32((uint32_t)channel);
    console_out_str(", RAM->RAM, software CHREQ): ");

    if (!nora_dma_global_is_ready()) {
        console_out_str("FAIL (controller not ready: DMACON.DMAEN / MSTRPR.DMAPR)\n");
        return false;
    }
    if (!nora_dma_channel_config(channel, &cfg)) {
        console_out_str("FAIL (channel config rejected)\n");
        return false;
    }
    if (!nora_dma_channel_enable(channel, true)) {
        console_out_str("FAIL (channel enable rejected)\n");
        return false;
    }

    nora_dma_software_trigger(channel);

    /* has_completed(), not has_completed_half(): this is a single-shot transfer, and
     * the half predicate is already true at the midpoint -- spinning on it would
     * release this loop while the second half of the block was still being written,
     * and the comparison below would then fail on data the DMA was about to write
     * correctly. */
    for (spins = 0u; spins < (uint16_t)DMA_SELFTEST_SPINS; spins++) {
        if (nora_dma_status_has_completed(nora_dma_read_status(channel))) {
            break;
        }
    }

    status    = nora_dma_read_status(channel);
    remaining = nora_dma_read_count(channel);
    completed = nora_dma_status_has_completed(status);

    for (i = 0u; i < (uint8_t)DMA_SELFTEST_ELEMS; i++) {
        if (s_dst[i] != s_src[i]) {
            data_ok = false;
        }
    }

    /* Leave nothing armed behind us: the transport starts right after this. */
    (void)nora_dma_channel_enable(channel, false);
    nora_dma_clear_status(channel);
    nora_dma_clear_irq_flag(channel);

    if (completed && data_ok) {
        console_out_str("PASS\n");
        return true;
    }

    /* Distinguish the two failure shapes rather than reporting a bare FAIL: no
     * DONE means the controller never serviced the request at all, while DONE with
     * bad data means it ran but moved the wrong bytes (address/size/mode). */
    console_out_str(completed ? "FAIL (DONE but data mismatch)"
                                         : "FAIL (no DONE: request never serviced)");
    console_out_str("  DMAINT=0x");
    console_out_hex16((uint16_t)status);
    console_out_str(" DMACNT=0x");
    console_out_hex16((uint16_t)remaining);
    console_out_str(" dst=");
    for (i = 0u; i < (uint8_t)DMA_SELFTEST_ELEMS; i++) {
        console_out_str(" 0x");
        console_out_hex16(s_dst[i]);
    }
    console_out_str("\n");
    return false;
}
