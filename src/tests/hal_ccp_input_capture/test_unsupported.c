// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

/*
 * The backend on a part WITHOUT the full CCP register inventory must REPORT that,
 * not silently pretend to work.
 *
 * This replaces an older gate that compiled the PUBLIC header for AK128 and
 * required it to fail unless an opt-in macro was defined. That gate stopped
 * meaning anything once device selection moved out of the public header and into
 * the backend's DFP capability test: the public header is now deliberately
 * device-neutral (docs_public/nora_hal_public_api.md), so of course it compiles for any
 * part -- the old test could only have passed by contradicting the design.
 *
 * What still needs guarding is the rule the design does assert: "a capability that
 * only one target can implement must report that fact; it must not be silently
 * ignored." So this compiles the BACKEND against a CCP9-less fake xc.h and checks
 * every entry point answers "not available". Two failure modes it catches that a
 * header-compile test cannot:
 *
 *   - an entry point added to the full-map branch only, so a part without the
 *     inventory fails to LINK (this happened to nora_ccp_icap_irq_clear when it
 *     was introduced);
 *   - a stub that returns OK / true, which would let a caller believe capture is
 *     running on hardware that has none.
 *
 * No SFR definitions here on purpose: with the table compiled out, the backend must
 * not reference a single CCP register. If this file ever needs one to link, that is
 * itself the bug.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <xc.h>

#include "nora_ccp_input_capture.h"

int main(void)
{
    const nora_ccp_icap_config_t config =
    {
        .source          = NORA_CCP_SRC_PIN,
        .edge            = NORA_CCP_EDGE_EVERY_RISING,
        .clock           = NORA_CCP_CLK_PERIPHERAL,
        .prescaler       = NORA_CCP_PS_1,
        .use_32bit       = true,
        .irq_ops         = NORA_CCP_IRQ_EVERY_EVENT,
        .irq_enable      = true,
        .irq_priority    = 4u,
        .timebase_src_hz = 25000000u,
    };
    uint32_t timestamp = 0x5A5A5A5Au;

    /* Every status-returning entry point reports unavailable ... */
    assert(nora_ccp_icap_configure(NORA_CCP1, &config) == NORA_CCP_ERR_INSTANCE);
    assert(nora_ccp_icap_set_callback(NORA_CCP1, NULL, NULL) ==
           NORA_CCP_ERR_INSTANCE);
    assert(nora_ccp_icap_start(NORA_CCP1) == NORA_CCP_ERR_INSTANCE);
    assert(nora_ccp_icap_stop(NORA_CCP1) == NORA_CCP_ERR_INSTANCE);

    /* ... the queries answer "nothing here" rather than a plausible value ... */
    assert(nora_ccp_icap_read(NORA_CCP1, &timestamp) == false);
    assert(timestamp == 0x5A5A5A5Au);   /* untouched on a failed read */
    assert(nora_ccp_icap_overflow(NORA_CCP1, true) == false);
    assert(nora_ccp_icap_timebase_hz(NORA_CCP1) == 0u);

    /* ... and the void entry points exist and are safe no-ops. Reaching this line
     * at all is the link-side half of the check. */
    nora_ccp_icap_isr(NORA_CCP1);
    nora_ccp_icap_irq_clear(NORA_CCP1);

    /* An out-of-range instance behaves the same way, not worse. */
    assert(nora_ccp_icap_configure(NORA_CCP_INST_COUNT, &config) ==
           NORA_CCP_ERR_INSTANCE);
    nora_ccp_icap_irq_clear(NORA_CCP_INST_COUNT);

    return 0;
}
