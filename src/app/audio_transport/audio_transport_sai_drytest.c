//===========================================================
// audio_transport_sai_drytest.c
//
// Opt-in CMSIS-SAI wrapper API dry test.
// This file owns the Driver_SAI0 return-code/state-transition checks so main.c
// can stay at the transport-route selection level.
//===========================================================

#include "resolved_sai_test_config.h"
#include "app_runtime_overrides.h"

#if RESOLVED_SAI_TEST_DRY_RUN_ENABLED

#include <stdint.h>
#include <stdio.h>

#include "Driver_SAI_dsPIC33AK.h"
#include "board/audio/audio.h"
#include "audio_transport_sai_drytest.h"
#include "nora_spi_i2s_tdm.h"

/*
 * CMSIS-SAI wrapper API dry test (boot, no live audio path).
 *
 * Exercises Driver_SAI0 return codes / state transitions only -- it never enables
 * the stream (no CONTROL_TX/RX enable), so the block bridge never runs. Must be
 * called while the HAL is stopped and BEFORE the demo registers its callback /
 * starts the stream. Restores the default config and clears the block callback at
 * the end so the demo proceeds normally. Prints one [SAI-DRYTEST] summary line.
 */
/* The wrapper realises exactly ONE protocol per build, fixed by the HAL's
 * compile-time DMA geometry NORA_TDM_SLOTS_PER_FS (2 = I2S, 8 = TDM8 via
 * ARM_SAI_PROTOCOL_USER) -- see Driver_SAI_dsPIC33AK.c. So the dry test asks for
 * the protocol this build can actually realise instead of a fixed pair: a
 * hard-coded TX=USER / RX=I2S mix is rejected on every build, which is a defect
 * in the test, not in the driver. arg1's framing/slot fields are User-Protocol
 * only (ignored for I2S), hence the paired slot argument. */
#if   (NORA_TDM_SLOTS_PER_FS == 2)
  #define SAI_DT_PROTOCOL   ARM_SAI_PROTOCOL_I2S
  #define SAI_DT_SLOT_ARG   0u
#elif (NORA_TDM_SLOTS_PER_FS == 8)
  #define SAI_DT_PROTOCOL   ARM_SAI_PROTOCOL_USER
  #define SAI_DT_SLOT_ARG   ARM_SAI_SLOT_COUNT(NORA_TDM_SLOTS_PER_FS)
#else
  #error "SAI dry test: NORA_TDM_SLOTS_PER_FS is neither 2 (I2S) nor 8 (TDM8)"
#endif

#define SAI_DT_CHECK(expr, want)                                                   \
    do {                                                                           \
        int32_t rc_ = (int32_t)(expr);                                             \
        if (rc_ == (int32_t)(want)) { pass++; }                                    \
        else { fail++; printf(" [SAI-DRYTEST] FAIL %s: got %ld want %ld\n",        \
                              #expr, (long)rc_, (long)(want)); }                   \
    } while (0)

void audio_transport_sai_drytest_run( void )
{
    static int32_t dt_buf[1];                 /* dummy (Send/Receive only store the ptr) */
    uint32_t pass = 0u, fail = 0u;
    nora_spi_i2s_tdm_config_t dcfg;
    uint32_t n;
    ARM_DRIVER_VERSION ver;
    ARM_SAI_CAPABILITIES cap;
    ARM_SAI_STATUS st;

    /* per-block element count: frames * slots (from HAL compile-time geometry) */
    n = Driver_SAI_dsPIC33AK_GetDefaultConfig(&dcfg)
            ? ((uint32_t)dcfg.block_frames * (uint32_t)dcfg.slots_per_fs) : 0u;

    ver = Driver_SAI0.GetVersion();
    if (ver.api == ARM_SAI_API_VERSION) { pass++; } else { fail++; }

    /* Exactly one of the two protocols must be advertised -- the contract is
     * "the compiled geometry's protocol, never both". `!=` on the two 0/1 fields
     * checks that exclusivity; `&&` (the previous form) demanded both and so could
     * never pass, and `||` would not catch a driver advertising both. */
    cap = Driver_SAI0.GetCapabilities();
    if (cap.protocol_i2s != cap.protocol_user) { pass++; } else { fail++; }

    SAI_DT_CHECK(Driver_SAI0.Initialize(NULL),               ARM_DRIVER_OK);
    SAI_DT_CHECK(Driver_SAI0.PowerControl(ARM_POWER_FULL),   ARM_DRIVER_OK);

    /* Valid configs: 32-bit/48k on the protocol this build realises, for TX and RX
     * alike. CONFIGURE_TX/RX drive the SAME full-duplex transport, so a mixed pair
     * is not a thing the driver can honour. */
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 48000u),                            ARM_DRIVER_OK);
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_RX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 48000u),                            ARM_DRIVER_OK);

    /* Invalid configs must be rejected (not silently accepted). MODE, DATA_SIZE and
     * MONO_MODE are validated ahead of the protocol in sai0_configure(), so those
     * three would reject on any protocol; they still carry the realisable one so a
     * reordering there cannot turn them into ERROR_PROTOCOL passes for the wrong
     * reason. AUDIO_FREQ is validated after the protocol and needs it outright. */
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(16) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 48000u),                            ARM_SAI_ERROR_DATA_SIZE);
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_MASTER | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 48000u),                        ARM_DRIVER_ERROR_UNSUPPORTED);
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 44100u),                            ARM_SAI_ERROR_AUDIO_FREQ);
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MONO_MODE | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG, 48000u),                            ARM_SAI_ERROR_MONO_MODE);
#if (NORA_TDM_SLOTS_PER_FS == 8)
    /* SLOT_OFFSET lives in arg1, which is User-Protocol only -- for I2S the driver
     * ignores arg1 by contract, so there is no offset to reject and this check has
     * no I2S counterpart. It is therefore compiled only for the TDM8 geometry (an
     * I2S build reports one check fewer, not a failure). */
    SAI_DT_CHECK(Driver_SAI0.Control(
        ARM_SAI_CONFIGURE_TX | ARM_SAI_MODE_SLAVE | SAI_DT_PROTOCOL |
        ARM_SAI_DATA_SIZE(32) | ARM_SAI_MCLK_PIN_INPUT,
        SAI_DT_SLOT_ARG | ARM_SAI_SLOT_OFFSET(1), 48000u),
        ARM_SAI_ERROR_SLOT_OFFESET);
#endif

    /* Send/Receive: block-aligned accepted, mis-aligned rejected. ABORT between. */
    SAI_DT_CHECK(Driver_SAI0.Send(dt_buf, n),                ARM_DRIVER_OK);
    (void)Driver_SAI0.Control(ARM_SAI_ABORT_SEND, 0u, 0u);
    SAI_DT_CHECK(Driver_SAI0.Send(dt_buf, n + 1u),           ARM_DRIVER_ERROR_PARAMETER);
    SAI_DT_CHECK(Driver_SAI0.Receive(dt_buf, n),             ARM_DRIVER_OK);
    (void)Driver_SAI0.Control(ARM_SAI_ABORT_RECEIVE, 0u, 0u);
    SAI_DT_CHECK(Driver_SAI0.Receive(dt_buf, n + 1u),        ARM_DRIVER_ERROR_PARAMETER);

    /* GetStatus: idle after aborts (stream never started). */
    st = Driver_SAI0.GetStatus();
    if (!st.tx_busy && !st.rx_busy && !st.frame_error) { pass++; } else { fail++; }

    SAI_DT_CHECK(Driver_SAI0.PowerControl(ARM_POWER_OFF),    ARM_DRIVER_OK);
    SAI_DT_CHECK(Driver_SAI0.Uninitialize(),                 ARM_DRIVER_OK);

    /* Restore the primary leg's default config so the downstream route starts from a clean
     * SINGLE-mode state. The dry-test drove the CMSIS single-instance path (inst_configure on
     * the primary -> SINGLE mode); the per-leg API now addresses ONLY the primary leg, so the
     * former SPI2 restore is removed. It is not needed: whichever route follows re-commits its
     * own config anyway -- the demo via configure_system() (recommits BOTH legs, allowed from
     * SINGLE), a CMSIS-live route via inst_configure() on the primary (allowed from SINGLE).
     * Leaving SINGLE keeps both routes legal (a SYSTEM recommit here would reject the CMSIS one). */
    if (Driver_SAI_dsPIC33AK_GetDefaultConfig(&dcfg)) {
        (void)nora_spi_i2s_tdm_inst_configure(nora_spi_i2s_tdm_spi1(), &dcfg);
    }

    /* The geometry is in the line because it selects both the protocol under test and
     * the number of checks; a bare pass/fail count is not comparable across builds. */
    printf(" [SAI-DRYTEST] pass=%lu fail=%lu (block_samples=%lu, slots/fs=%u -> %s)\n",
           (unsigned long)pass, (unsigned long)fail, (unsigned long)n,
           (unsigned)NORA_TDM_SLOTS_PER_FS,
           (NORA_TDM_SLOTS_PER_FS == 2) ? "I2S" : "TDM8/PROTOCOL_USER");
}

#endif // RESOLVED_SAI_TEST_DRY_RUN_ENABLED
