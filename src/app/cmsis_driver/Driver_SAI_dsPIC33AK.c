/* SPDX-License-Identifier: MIT-0 */
/* ========================================================================== */
/* CMSIS-Driver SAI wrapper for the dsPIC33AK SPI/I2S/TDM HAL                   */
/* ========================================================================== */

/*
 * Maps the ARM CMSIS-Driver SAI API onto the nora_spi_i2s_tdm HAL. All
 * ARM_SAI_* / ARM_DRIVER_* types live here, never in the HAL.
 *
 * One instance, Driver_SAI0, = the single SPI/I2S/TDM transport. Implemented:
 *   GetVersion, GetCapabilities, Initialize, Uninitialize, PowerControl
 *   (FULL/OFF), GetStatus, a Control parse (slave + I2S/2-slot or TDM8/8-slot,
 *   32-bit; AUDIO_FREQ checked against the integrator rate hook; CONTROL_TX/RX =
 *   whole-stream start/stop; configure only while stopped), and a Send/Receive
 *   copy layer. Everything outside the validated envelope returns
 *   ARM_DRIVER_ERROR_UNSUPPORTED (or the matching ARM_SAI_ERROR_*).
 *
 * Capabilities advertise ONLY the HAL's validated envelope (mirrors
 * tdm_config_is_supported()): slave, I2S(2) / TDM8(8), 32-bit, external MCLK.
 *
 * The wrapper registers its HAL block callback explicitly in PowerControl(FULL)
 * (the HAL core has no app fallback). Board electricals and the supported-rate set
 * come from the integrator through two weak hooks declared in the header
 * (Driver_SAI_dsPIC33AK_GetDefaultConfig / _IsSampleRateSupported); a project must
 * override at least GetDefaultConfig before a stream can start. Verified live
 * (full-duplex I2S/TDM loopback) on dsPIC33AK512MPS512 in the Sonora integration
 * project.
 *
 * Copy layer: the HAL is zero-copy (block callback hands pointers into its
 * ping-pong halves). CMSIS Send/Receive caller-buffer semantics are realised by a
 * per-block copy in sai0_block_bridge(). That bridge runs in the RX-block (DMA)
 * ISR context -- keep it short; watch nora_spi_i2s_tdm_get_status().load.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "Driver_SAI_dsPIC33AK.h"

#include "nora_spi_i2s_tdm.h"
#include "nora_dma.h"   // RX-DMA IRQ guard around copy-layer state updates

/* RTE configuration header. Defaults to the bundled SAI-only example so this repo
 * builds as-is; an integrating project points it at its own RTE_Device.h with
 *   -DDRIVER_SAI_DSPIC33AK_RTE_HEADER=\"RTE_Device.h\"
 * (more portable than relying on __has_include on every toolchain). */
#ifndef DRIVER_SAI_DSPIC33AK_RTE_HEADER
#define DRIVER_SAI_DSPIC33AK_RTE_HEADER "RTE_Device_SAI_dsPIC33AK_example.h"
#endif
#include DRIVER_SAI_DSPIC33AK_RTE_HEADER

#if (RTE_SAI0 != 0) && NORA_TDM_BASE_ON_SPI34
#error "CMSIS-SAI Driver_SAI0 is bound to physical SPI1; SPI3/4 test-bank mode is unsupported."
#endif

#if (RTE_SAI0 != 0)

/* This wrapper's implementation version. The CMSIS SAI API version it conforms to
 * is ARM_SAI_API_VERSION (1.2). */
#define DRIVER_SAI_DSPIC33AK_VERSION  ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0)

/* The HAL's static DMA ping-pong geometry is fixed at compile time by
 * NORA_TDM_SLOTS_PER_FS (from conf.h): 2 = I2S, 8 = TDM8. This wrapper can
 * only realise the ONE protocol that matches the compiled geometry --
 * tdm_config_is_supported() rejects a slots_per_fs that differs from the leg's
 * built-in geometry. So advertise/accept exactly that protocol, never both. */
#if   (NORA_TDM_SLOTS_PER_FS == 2)
  #define SAI0_CAP_PROTOCOL_I2S    1u
  #define SAI0_CAP_PROTOCOL_USER   0u
#elif (NORA_TDM_SLOTS_PER_FS == 8)
  #define SAI0_CAP_PROTOCOL_I2S    0u
  #define SAI0_CAP_PROTOCOL_USER   1u
#else
  #define SAI0_CAP_PROTOCOL_I2S    0u
  #define SAI0_CAP_PROTOCOL_USER   0u
#endif

/* ========================================================================== */
/* Driver version and capabilities                                            */
/* ========================================================================== */

static const ARM_DRIVER_VERSION sai_driver_version = {
    ARM_SAI_API_VERSION,             /* api: CMSIS SAI API version (1.2) */
    DRIVER_SAI_DSPIC33AK_VERSION     /* drv: this wrapper's version (1.0) */
};

/* Validated envelope only: a single standalone full-duplex stream, external MCLK
 * input or inactive MCLK, no justified/PCM/AC97, no mono/companding, no FRAME_ERROR event (FRMERR
 * unverified in the slave-framed config). The protocol advertised (I2S vs TDM8)
 * tracks the compiled HAL geometry -- only one is realisable per build. */
static const ARM_SAI_CAPABILITIES sai_capabilities = {
    1u,                     /* asynchronous       (standalone stream)        */
    0u,                     /* synchronous                                   */
    SAI0_CAP_PROTOCOL_USER, /* protocol_user      (TDM8; iff SLOTS_PER_FS=8) */
    SAI0_CAP_PROTOCOL_I2S,  /* protocol_i2s       (iff SLOTS_PER_FS=2)       */
    0u,                     /* protocol_justified                            */
    0u,                     /* protocol_pcm                                  */
    0u,                     /* protocol_ac97                                 */
    0u,                     /* mono_mode                                     */
    0u,                     /* companding                                    */
    1u,                     /* mclk_pin           (external input/inactive)  */
    0u,                     /* event_frame_error                             */
    0u                      /* reserved                                      */
};

/* ========================================================================== */
/* Per-instance context (Driver_SAI0 = the single TDM stream)                 */
/* ========================================================================== */

typedef struct {
    ARM_SAI_SignalEvent_t cb_event;
    bool     initialized;
    bool     powered;
    bool     configured;

    /* Samples (int32) per ping/pong half = block_frames * slots_per_fs, taken
     * from the HAL default/applied config. The copy layer uses it as the per-block
     * element count. */
    uint32_t block_samples;

    /* CMSIS Send/Receive copy-layer transfers (caller-owned buffers). */
    const int32_t *tx_buf; uint32_t tx_num; volatile uint32_t tx_cnt;
    int32_t       *rx_buf; uint32_t rx_num; volatile uint32_t rx_cnt;

    /* Per-direction enable intent (Control(CONTROL_TX/RX) arg1 bit0). Whole-stream in
     * hardware, but tracked per direction so the bridge only raises tx_underflow /
     * rx_overflow for a direction the caller actually enabled. */
    volatile uint8_t tx_enabled;
    volatile uint8_t rx_enabled;

    /* Sticky, surfaced through GetStatus(); cleared at the next Send/Receive. The
     * matching ARM_SAI_EVENT_TX_UNDERFLOW / _RX_OVERFLOW is fired ONCE per episode (on
     * the 0->1 transition), not every block. */
    volatile uint8_t tx_underflow;
    volatile uint8_t rx_overflow;
} sai_ctx_t;

static sai_ctx_t sai0_ctx;

/* ========================================================================== */
/* Integration hooks -- weak defaults (the integrator overrides; see the header) */
/* ========================================================================== */

/* Weak default: no board config available. An integrator that has not provided a
 * strong override gets a clean Control(CONFIGURE_*) failure (ARM_DRIVER_ERROR)
 * rather than a stream started on uninitialised geometry. */
__attribute__((weak))
bool Driver_SAI_dsPIC33AK_GetDefaultConfig(nora_spi_i2s_tdm_config_t *cfg)
{
    (void)cfg;
    return false;
}

/* Weak default sample-rate policy: accept only the RTE-declared default rate. An
 * integrator with a wider supported-rate set (e.g. 48 k + 96 k) overrides this. */
__attribute__((weak))
bool Driver_SAI_dsPIC33AK_IsSampleRateSupported(uint32_t hz)
{
    return (hz == RTE_SAI0_DEFAULT_SAMPLE_RATE_HZ);
}

/* ========================================================================== */
/* Helpers                                                                    */
/* ========================================================================== */

/* Compute the per-block element count (int32 samples per ping/pong half) from a
 * config: block_frames frames x slots_per_fs slots. */
static uint32_t sai0_block_samples(const nora_spi_i2s_tdm_config_t *cfg)
{
    return (uint32_t)cfg->block_frames * (uint32_t)cfg->slots_per_fs;
}

/* HAL block callback (configured SPI1 RX-DMA ISR context) = the CMSIS copy layer. Copies the RX half
 * into an active Receive buffer and fills the TX half from an active Send buffer,
 * firing SEND/RECEIVE_COMPLETE when a transfer finishes. Kept short (ISR).
 *
 * Single-instance mapping: this bridge serves literal physical SPI1. The block
 * callback is per-instance (src, dst, user); all non-SPI1 legs remain outside
 * Driver_SAI0 and receive no CMSIS bridge callback. */
static void sai0_block_bridge(const int32_t *src, int32_t *dst, void *user)
{
    sai_ctx_t *ctx    = (sai_ctx_t *)user;
    uint32_t   n      = ctx->block_samples;
    uint32_t   events = 0u;   /* coalesced -- fired once after all ctx updates (below) */

    if (n == 0u) {
        return;
    }

    /* RX: copy this block into the caller's Receive buffer (bounds-checked). */
    if (ctx->rx_buf != NULL) {
        uint32_t remain = (ctx->rx_num > ctx->rx_cnt) ? (ctx->rx_num - ctx->rx_cnt) : 0u;
        uint32_t take   = (remain < n) ? remain : n;
        if (take > 0u) {
            memcpy(&ctx->rx_buf[ctx->rx_cnt], src, (size_t)take * sizeof(int32_t));
            ctx->rx_cnt += take;
        }
        if (ctx->rx_cnt >= ctx->rx_num) {
            ctx->rx_buf = NULL;
            events |= ARM_SAI_EVENT_RECEIVE_COMPLETE;
        } else if (take < n) {
            /* more HAL data than caller space: overflow (fire the event once) */
            if (ctx->rx_overflow == 0u) { events |= ARM_SAI_EVENT_RX_OVERFLOW; }
            ctx->rx_overflow = 1u;
        }
    } else if (ctx->rx_enabled != 0u) {
        /* RX enabled but no active Receive buffer: the incoming block is dropped ->
         * overflow (fire the event once per episode; cleared by the next Receive()). */
        if (ctx->rx_overflow == 0u) { events |= ARM_SAI_EVENT_RX_OVERFLOW; }
        ctx->rx_overflow = 1u;
    }

    /* TX: fill this block's half from the caller's Send buffer. Underflow if the Send
     * buffer is exhausted mid-block. */
    if (ctx->tx_buf != NULL) {
        uint32_t remain = (ctx->tx_num > ctx->tx_cnt) ? (ctx->tx_num - ctx->tx_cnt) : 0u;
        uint32_t take   = (remain < n) ? remain : n;
        if (take > 0u) {
            memcpy(dst, &ctx->tx_buf[ctx->tx_cnt], (size_t)take * sizeof(int32_t));
            ctx->tx_cnt += take;
        }
        if (take < n) {
            memset(&dst[take], 0, (size_t)(n - take) * sizeof(int32_t));
            if (ctx->tx_underflow == 0u) { events |= ARM_SAI_EVENT_TX_UNDERFLOW; }
            ctx->tx_underflow = 1u;
        }
        if (ctx->tx_cnt >= ctx->tx_num) {
            ctx->tx_buf = NULL;
            events |= ARM_SAI_EVENT_SEND_COMPLETE;
        }
    } else if (dst != NULL) {
        /* No active Send: keep the stream silent. If TX was enabled by the caller, an
         * enabled stream with no data to send is an underflow (event once per episode;
         * cleared by the next Send()). */
        memset(dst, 0, (size_t)n * sizeof(int32_t));
        if (ctx->tx_enabled != 0u) {
            if (ctx->tx_underflow == 0u) { events |= ARM_SAI_EVENT_TX_UNDERFLOW; }
            ctx->tx_underflow = 1u;
        }
    }

    /* Fire the user callback ONCE, after every ctx field above is settled. The
     * callback may re-arm Send/Receive from this (ISR) context; coalescing avoids a
     * mid-bridge re-arm racing the still-running half. */
    if ((events != 0u) && (ctx->cb_event != NULL)) {
        ctx->cb_event(events);
    }
}

/* ========================================================================== */
/* CMSIS SAI driver functions (Driver_SAI0)                                   */
/* ========================================================================== */

static ARM_DRIVER_VERSION SAI0_GetVersion(void)
{
    return sai_driver_version;
}

static ARM_SAI_CAPABILITIES SAI0_GetCapabilities(void)
{
    return sai_capabilities;
}

static int32_t SAI0_Initialize(ARM_SAI_SignalEvent_t cb_event)
{
    sai_ctx_t *ctx = &sai0_ctx;

    /* Idempotent re-Initialize: if already initialized, this is a true no-op -- keep the existing
     * callback and all transport state (powered/configured/running, transfer pointers) intact.
     * Rationale: (1) wiping the context while a stream is live would orphan a running HAL (the
     * subsequent PowerControl(OFF) would see powered=false and skip the teardown); (2) the block
     * bridge reads ctx->cb_event from the RX-DMA ISR, and a 32-bit pointer store is not atomic on
     * this 16-bit core, so replacing it here would race the ISR. To change the callback, call
     * Uninitialize() then Initialize() (the CMSIS-conventional way). */
    if (ctx->initialized) {
        return ARM_DRIVER_OK;
    }

    ctx->cb_event      = cb_event;
    ctx->initialized   = true;
    ctx->powered       = false;
    ctx->configured    = false;
    ctx->tx_buf        = NULL; ctx->tx_num = 0u; ctx->tx_cnt = 0u;
    ctx->rx_buf        = NULL; ctx->rx_num = 0u; ctx->rx_cnt = 0u;
    ctx->tx_enabled    = 0u;
    ctx->rx_enabled    = 0u;
    ctx->tx_underflow  = 0u;
    ctx->rx_overflow   = 0u;

    /* P1: no default-config seeding (the macro-derived default left the HAL core).
     * block_samples stays 0 until Control(CONFIGURE_*) sets the geometry; Send/Receive
     * already reject block_samples==0, so CONFIGURE is required before transfers. */
    ctx->block_samples = 0u;

    return ARM_DRIVER_OK;
}

static int32_t SAI0_PowerControl(ARM_POWER_STATE state);  /* fwd for Uninitialize */

static int32_t SAI0_Uninitialize(void)
{
    sai_ctx_t *ctx = &sai0_ctx;

    if (ctx->powered) {
        /* Propagate a failed power-down instead of faking a clean uninit: if OFF could not fully
         * tear down (inst_stop/close/callback-clear failed), leave initialized + cb_event intact so
         * the caller can retry Uninitialize, and surface the error. Consistent with the fail-closed
         * PowerControl(OFF) teardown above. */
        if (SAI0_PowerControl(ARM_POWER_OFF) != ARM_DRIVER_OK) {
            return ARM_DRIVER_ERROR;
        }
    }
    ctx->cb_event    = NULL;
    ctx->initialized = false;
    return ARM_DRIVER_OK;
}

static int32_t SAI0_PowerControl(ARM_POWER_STATE state)
{
    sai_ctx_t *ctx = &sai0_ctx;

    if (!ctx->initialized) {
        return ARM_DRIVER_ERROR;
    }

    switch (state) {
    case ARM_POWER_FULL:
        if (ctx->powered) {
            return ARM_DRIVER_OK;
        }
        /* The DMA HAL requires a one-time global init before any channel configuration and
         * does NOT call it from the channel APIs; it is idempotent, so this driver owns it
         * here rather than relying on the integrator to remember a startup step (forgetting
         * it would make only start() fail, silently). */
        nora_dma_global_init();
        if (!nora_dma_global_is_ready()) {
            return ARM_DRIVER_ERROR;
        }
        /* Register the wrapper's block callback on the SPI1 instance (explicit -- no app
         * fallback), FAIL-CLOSED: a NULL instance or a rejected registration (e.g. the HAL
         * refuses a callback change while running) must NOT report FULL success, or the
         * bridge would be missing while we claim powered. Does NOT start the stream;
         * streaming begins on Control(CONTROL_TX/RX enable). All non-SPI1 legs remain
         * outside Driver_SAI0 and receive no CMSIS bridge callback. */
        {
            nora_spi_i2s_tdm_inst_t *spi1 = nora_spi_i2s_tdm_spi1();
            if ((spi1 == NULL) ||
                !nora_spi_i2s_tdm_set_block_callback(spi1, sai0_block_bridge, ctx)) {
                return ARM_DRIVER_ERROR;   /* stay unpowered; surface the failure */
            }
        }
        ctx->tx_underflow = 0u;
        ctx->rx_overflow  = 0u;
        /* Start a fresh power session with no stale per-direction enable intent (defensive;
         * OFF clears these too). */
        ctx->tx_enabled   = 0u;
        ctx->rx_enabled   = 0u;
        ctx->powered      = true;
        return ARM_DRIVER_OK;

    case ARM_POWER_OFF:
        if (ctx->powered) {
            /* Fail-closed teardown: close the port and clear the callback UNCONDITIONALLY, but stop
             * the primary only when it is actually running. inst_stop() rejects a not-yet-configured
             * stream (CONFIG_MODE_NONE -> ERR_CONFIG_MODE), so calling it unconditionally would fail
             * a legitimate Initialize -> PowerControl(FULL) -> PowerControl(OFF) (FULL does not
             * configure). is_running() true implies SINGLE+configured on the wrapper route, so the
             * stop then succeeds; when not running there is nothing to stop and close() (mode-
             * agnostic, true when not running) still recovers an opened-but-stopped port. ALL steps
             * must succeed before we drop the powered state; if any fails, keep ctx->powered = true
             * and return an error so the OFF is not faked. */
            bool ok = true;
            if (nora_spi_i2s_tdm_is_running()) {
                if (!nora_spi_i2s_tdm_inst_stop(nora_spi_i2s_tdm_spi1())) { ok = false; }
            }
            if (!nora_spi_i2s_tdm_close()) { ok = false; }
            if (!nora_spi_i2s_tdm_set_block_callback(nora_spi_i2s_tdm_spi1(), NULL, NULL)) { ok = false; }
            if (!ok) {
                return ARM_DRIVER_ERROR;   /* teardown incomplete -> stay powered, surface failure */
            }
        }
        ctx->tx_buf  = NULL; ctx->rx_buf = NULL;
        /* Clear per-direction enable intent so a later FULL + single-direction enable cannot
         * inherit a stale flag (which would raise a spurious underflow/overflow for a direction
         * the new session never enabled). Only on a successful teardown (kept above on failure). */
        ctx->tx_enabled = 0u;
        ctx->rx_enabled = 0u;
        ctx->powered = false;
        return ARM_DRIVER_OK;

    case ARM_POWER_LOW:
        return ARM_DRIVER_ERROR_UNSUPPORTED;

    default:
        return ARM_DRIVER_ERROR_PARAMETER;
    }
}

/* The RX DMA channel whose ISR runs sai0_block_bridge(). The wrapper guards its
 * multi-word ctx updates (pointers + 32-bit counters) against that ISR with this
 * channel's CPU-IRQ mask: dsPIC33 is a 16-bit core, so a 32-bit pointer/count
 * store is not atomic versus the bridge. Single-instance mapping (SAI0 -> SPI1),
 * so the channel comes straight from conf.h. */
#define SAI0_RX_DMA_CH  (NORA_TDM_SPI1_RX_DMA)

static int32_t SAI0_Send(const void *data, uint32_t num)
{
    sai_ctx_t *ctx = &sai0_ctx;
    bool was;

    if (!ctx->initialized || !ctx->powered) {
        return ARM_DRIVER_ERROR;
    }
    if ((data == NULL) || (num == 0u)) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    /* Block-aligned transfers only. num must be a whole number of HAL ping-pong
     * blocks so a finite transfer cannot end mid-block (which would look like a
     * spurious tx_underflow). */
    if (ctx->block_samples == 0u) {
        return ARM_DRIVER_ERROR;
    }
    if ((num % ctx->block_samples) != 0u) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    if ((ctx->tx_buf != NULL) && (ctx->tx_cnt < ctx->tx_num)) {
        return ARM_DRIVER_ERROR_BUSY;
    }

    /* CMSIS-SAI contract: this wrapper does NOT deep-copy at Send() time -- the block
     * bridge reads `data` progressively, one ping-pong block per DMA tick. So the
     * caller buffer MUST remain valid AND UNMODIFIED until ARM_SAI_EVENT_SEND_COMPLETE
     * (here SEND_COMPLETE means "the wrapper will no longer read this buffer"). A
     * continuous stream must therefore double-buffer: only refill a buffer after its
     * SEND_COMPLETE, never the one still in flight (a single shared RX/TX buffer
     * overwritten on RECEIVE_COMPLETE caused intermittent dropouts -- the "ghost").
     * Arm under the RX-DMA IRQ mask so the bridge cannot observe a half-updated set. */
    was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
    ctx->tx_underflow = 0u;
    ctx->tx_cnt       = 0u;
    ctx->tx_num       = num;
    ctx->tx_buf       = (const int32_t *)data;
    nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
    return ARM_DRIVER_OK;
}

static int32_t SAI0_Receive(void *data, uint32_t num)
{
    sai_ctx_t *ctx = &sai0_ctx;
    bool was;

    if (!ctx->initialized || !ctx->powered) {
        return ARM_DRIVER_ERROR;
    }
    if ((data == NULL) || (num == 0u)) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    /* Block-aligned transfers only (see Send). */
    if (ctx->block_samples == 0u) {
        return ARM_DRIVER_ERROR;
    }
    if ((num % ctx->block_samples) != 0u) {
        return ARM_DRIVER_ERROR_PARAMETER;
    }
    if ((ctx->rx_buf != NULL) && (ctx->rx_cnt < ctx->rx_num)) {
        return ARM_DRIVER_ERROR_BUSY;
    }

    /* Arm under the RX-DMA IRQ mask so the bridge cannot observe a half-updated set. */
    was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
    ctx->rx_overflow = 0u;
    ctx->rx_cnt      = 0u;
    ctx->rx_num      = num;
    ctx->rx_buf      = (int32_t *)data;
    nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
    return ARM_DRIVER_OK;
}

/* The bridge updates tx_cnt/rx_cnt in RX-DMA ISR context; a 32-bit read on this
 * 16-bit core can tear, so snapshot under the IRQ mask. */
static uint32_t SAI0_GetTxCount(void)
{
    uint32_t v;
    bool was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
    v = sai0_ctx.tx_cnt;
    nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
    return v;
}

static uint32_t SAI0_GetRxCount(void)
{
    uint32_t v;
    bool was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
    v = sai0_ctx.rx_cnt;
    nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
    return v;
}

/* Parse a CONFIGURE_TX/RX control word against the validated envelope and apply it
 * (only while stopped, via the HAL configure() guard). */
static int32_t sai0_configure(sai_ctx_t *ctx, uint32_t control, uint32_t arg1, uint32_t arg2)
{
    nora_spi_i2s_tdm_config_t cfg;
    uint32_t proto;
    uint32_t dsize;
    uint32_t freq;

    if (nora_spi_i2s_tdm_is_running()) {
        return ARM_DRIVER_ERROR_BUSY;   /* configure only while stopped */
    }
    /* Base the config on the integrator's validated board defaults (via the hook --
     * the wrapper is board-independent), then override the CMSIS-expressed fields. */
    if (!Driver_SAI_dsPIC33AK_GetDefaultConfig(&cfg)) {
        return ARM_DRIVER_ERROR;
    }

    /* --- control-word checks: reject anything outside the validated envelope
     *     rather than silently ignoring it (capabilities honesty). --- */
    /* Slave only (dsPIC master clock is not advertised). */
    if ((control & ARM_SAI_MODE_Msk) != ARM_SAI_MODE_SLAVE) {
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
    /* Asynchronous (standalone) only; no SAI-to-SAI synchronisation. */
    if ((control & ARM_SAI_SYNCHRONIZATION_Msk) != ARM_SAI_ASYNCHRONOUS) {
        return ARM_SAI_ERROR_SYNCHRONIZATION;
    }
    /* 32-bit data only. */
    dsize = (((control & ARM_SAI_DATA_SIZE_Msk) >> ARM_SAI_DATA_SIZE_Pos) + 1u);
    if (dsize != 32u) {
        return ARM_SAI_ERROR_DATA_SIZE;
    }
    /* MSB-first only. */
    if ((control & ARM_SAI_BIT_ORDER_Msk) != ARM_SAI_MSB_FIRST) {
        return ARM_SAI_ERROR_BIT_ORDER;
    }
    /* No mono mode. */
    if ((control & ARM_SAI_MONO_MODE) != 0u) {
        return ARM_SAI_ERROR_MONO_MODE;
    }
    /* No companding. */
    if ((control & ARM_SAI_COMPANDING_Msk) != ARM_SAI_COMPANDING_NONE) {
        return ARM_SAI_ERROR_COMPANDING;
    }
    /* Only the default clock polarity (the HAL bakes its validated CKP/CKE). */
    if ((control & ARM_SAI_CLOCK_POLARITY_Msk) != ARM_SAI_CLOCK_POLARITY_0) {
        return ARM_SAI_ERROR_CLOCK_POLARITY;
    }
    /* External MCLK only (input), or unused; MCLK output is master-mode. */
    if (((control & ARM_SAI_MCLK_PIN_Msk) != ARM_SAI_MCLK_PIN_INPUT) &&
        ((control & ARM_SAI_MCLK_PIN_Msk) != ARM_SAI_MCLK_PIN_INACTIVE)) {
        return ARM_SAI_ERROR_MCLK_PIN;
    }

    /* Protocol -> format + slot count (validated envelope). Only the protocol that
     * matches the compiled HAL geometry (NORA_TDM_SLOTS_PER_FS) is realisable;
     * the other is not advertised in capabilities and is rejected here too, so the
     * accepted set matches the advertised set exactly (no configure() surprise). */
    proto = control & ARM_SAI_PROTOCOL_Msk;
    if (proto == ARM_SAI_PROTOCOL_I2S) {
        if (SAI0_CAP_PROTOCOL_I2S == 0u) {
            return ARM_SAI_ERROR_PROTOCOL;   /* this build's geometry is not I2S */
        }
        cfg.format       = NORA_SPI_I2S_TDM_FORMAT_I2S;
        cfg.slots_per_fs = 2u;
        /* arg1 framing/slot fields are User-Protocol-only (ignored for I2S). */
    } else if (proto == ARM_SAI_PROTOCOL_USER) {
        if (SAI0_CAP_PROTOCOL_USER == 0u) {
            return ARM_SAI_ERROR_PROTOCOL;   /* this build's geometry is not TDM8 */
        }
        uint32_t slots = (((arg1 & ARM_SAI_SLOT_COUNT_Msk) >> ARM_SAI_SLOT_COUNT_Pos) + 1u);
        uint32_t slotsz = arg1 & ARM_SAI_SLOT_SIZE_Msk;
        if (slots != 8u) {
            return ARM_SAI_ERROR_SLOT_COUNT;   /* only TDM8 validated */
        }
        /* arg1 user-protocol fields the wrapper does not honour must be left at
         * their default; reject explicit non-default values (do not silently use
         * the baked HAL framing instead). */
        if ((arg1 & ARM_SAI_FRAME_LENGTH_Msk) != 0u) {
            return ARM_SAI_ERROR_FRAME_LENGTH;
        }
        if ((arg1 & ARM_SAI_FRAME_SYNC_WIDTH_Msk) != 0u) {
            return ARM_SAI_ERROR_FRAME_SYNC_WIDTH;
        }
        if ((arg1 & ARM_SAI_FRAME_SYNC_POLARITY_Msk) != ARM_SAI_FRAME_SYNC_POLARITY_HIGH) {
            return ARM_SAI_ERROR_FRAME_SYNC_POLARITY;
        }
        if ((arg1 & ARM_SAI_FRAME_SYNC_EARLY) != 0u) {
            return ARM_SAI_ERROR_FRAME_SYNC_EARLY;
        }
        if ((slotsz != ARM_SAI_SLOT_SIZE_DEFAULT) && (slotsz != ARM_SAI_SLOT_SIZE_32)) {
            return ARM_SAI_ERROR_SLOT_SIZE;    /* slot size must be default or 32 */
        }
        if ((arg1 & ARM_SAI_SLOT_OFFSET_Msk) != 0u) {
            return ARM_SAI_ERROR_SLOT_OFFESET; /* slot offset must be 0 */
        }
        cfg.format       = NORA_SPI_I2S_TDM_FORMAT_TDM;
        cfg.slots_per_fs = 8u;
    } else {
        return ARM_SAI_ERROR_PROTOCOL;
    }

    cfg.clock_role      = NORA_SPI_I2S_TDM_CLOCK_SLAVE;
    cfg.word_bits = 32u;

    /* AUDIO_FREQ (arg2): validate against the integration sample-rate policy. The
     * transport HAL is rate-agnostic (runs at the configured BRG / external clock), so
     * the wrapper checks the requested rate here against
     * Driver_SAI_dsPIC33AK_IsSampleRateSupported() rather
     * than handing it to the core. MCLK prescaler is master-mode only -> reject any
     * non-default value. */
    if ((arg2 & ARM_SAI_MCLK_PRESCALER_Msk) != 0u) {
        return ARM_SAI_ERROR_MCLK_PRESCALER;
    }
    freq = arg2 & ARM_SAI_AUDIO_FREQ_Msk;
    if (freq != 0u) {
        if (!Driver_SAI_dsPIC33AK_IsSampleRateSupported(freq)) {   // integrator rate policy (via hook)
            return ARM_SAI_ERROR_AUDIO_FREQ;
        }
    }

    /* Apply to the SPI1 instance while stopped (HAL guards running + validates the
     * envelope). Driver_SAI0 maps only to literal physical SPI1; all other legs remain
     * outside the wrapper. */
    if (!nora_spi_i2s_tdm_inst_configure(nora_spi_i2s_tdm_spi1(), &cfg)) {
        return ARM_DRIVER_ERROR;
    }
    ctx->block_samples = sai0_block_samples(&cfg);
    ctx->configured    = true;
    return ARM_DRIVER_OK;
}

static int32_t SAI0_Control(uint32_t control, uint32_t arg1, uint32_t arg2)
{
    sai_ctx_t *ctx = &sai0_ctx;

    if (!ctx->initialized) {
        return ARM_DRIVER_ERROR;
    }

    switch (control & ARM_SAI_CONTROL_Msk) {
    case ARM_SAI_CONFIGURE_TX:
    case ARM_SAI_CONFIGURE_RX:
        /* CONFIGURE_TX and CONFIGURE_RX configure the SAME full-duplex transport
         * (no true independent per-direction config). Call either, or call both
         * with identical parameters; a second, differing CONFIGURE just re-applies
         * (last wins) -- and since only the compiled-geometry protocol is accepted,
         * a TX=I2S / RX=TDM8 mix on one build is rejected by sai0_configure(). */
        return sai0_configure(ctx, control, arg1, arg2);

    case ARM_SAI_CONTROL_TX:
    case ARM_SAI_CONTROL_RX: {
        /* Whole-stream, full-duplex start/stop (no true per-direction control).
         * arg1 bit0 = enable. Higher bits (bit1 = mute, and any undefined bits) are NOT
         * supported: reject them rather than silently succeed, matching the wrapper's
         * fail-closed "reject outside the envelope" policy (there is no codec mute in the
         * transport HAL). */
        const bool is_tx = ((control & ARM_SAI_CONTROL_Msk) == ARM_SAI_CONTROL_TX);
        if (!ctx->powered) {
            return ARM_DRIVER_ERROR;
        }
        if ((arg1 & ~1u) != 0u) {
            return ARM_DRIVER_ERROR_UNSUPPORTED;
        }
        if ((arg1 & 1u) != 0u) {
            /* Refuse to start before a successful Control(CONFIGURE_*): otherwise the
             * stream would run on stale/default geometry the caller never selected. */
            if (!ctx->configured) {
                return ARM_DRIVER_ERROR;
            }
            if (!nora_spi_i2s_tdm_is_running()) {
                /* Open the shared port (SAI0 is slave-only here), then start the SPI1
                 * instance. Either returns false (no-op) if not configured, the rate is
                 * unsupported, the clock is not ready, or a port/DMA step fails --
                 * surface that to the CMSIS caller instead of claiming success. If the
                 * port opened but the instance failed to start, close() to keep the
                 * lifecycle balanced (close() is a near-no-op today, but symmetric for a
                 * future clock-deinit hook). */
                /* open() derives the role from the committed primary leg (SPI1, configured
                 * SLAVE by sai0_configure above); no role argument. The board pin hook skips
                 * unconfigured non-SPI1 legs in this single-instance CMSIS run. */
                if (!nora_spi_i2s_tdm_open()) {
                    return ARM_DRIVER_ERROR;
                }
                if (!nora_spi_i2s_tdm_inst_start(nora_spi_i2s_tdm_spi1())) {
                    nora_spi_i2s_tdm_close();
                    return ARM_DRIVER_ERROR;
                }
            }
            /* Record this direction's enable intent (bridge uses it to decide whether a
             * missing Send/Receive buffer is a real underflow/overflow). */
            if (is_tx) { ctx->tx_enabled = 1u; } else { ctx->rx_enabled = 1u; }
        } else {
            /* Fail-closed teardown: stop the primary only when running (inst_stop() rejects a
             * not-yet-configured CONFIG_MODE_NONE/non-SINGLE stream), always close() to recover an
             * opened-but-stopped state, and PROPAGATE a failure to the CMSIS caller. The block
             * callback stays registered (a re-enable can restart without re-registering). */
            bool ok = true;
            if (nora_spi_i2s_tdm_is_running()) {
                if (!nora_spi_i2s_tdm_inst_stop(nora_spi_i2s_tdm_spi1())) { ok = false; }
            }
            if (!nora_spi_i2s_tdm_close()) { ok = false; }
            if (!ok) {
                return ARM_DRIVER_ERROR;
            }
            /* Disable stops the whole stream (full-duplex, single transport), so clear BOTH
             * directions' enable intent regardless of which control word was used. */
            ctx->tx_enabled = 0u;
            ctx->rx_enabled = 0u;
        }
        return ARM_DRIVER_OK;
    }

    case ARM_SAI_ABORT_SEND: {
        /* Clear the transfer under the RX-DMA IRQ mask so the bridge cannot read a
         * half-cleared set (and cannot fetch from a buffer being abandoned). */
        bool was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
        ctx->tx_buf = NULL; ctx->tx_num = 0u; ctx->tx_cnt = 0u;
        nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
        return ARM_DRIVER_OK;
    }

    case ARM_SAI_ABORT_RECEIVE: {
        bool was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
        ctx->rx_buf = NULL; ctx->rx_num = 0u; ctx->rx_cnt = 0u;
        nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
        return ARM_DRIVER_OK;
    }

    /* No per-slot mask mechanism in the HAL. */
    case ARM_SAI_MASK_SLOTS_TX:
    case ARM_SAI_MASK_SLOTS_RX:
    default:
        return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
}

static ARM_SAI_STATUS SAI0_GetStatus(void)
{
    sai_ctx_t *ctx = &sai0_ctx;
    ARM_SAI_STATUS status = {0};
    bool was;

    /* Snapshot the bridge-updated fields under the RX-DMA IRQ mask so busy/sticky
     * flags are read as one coherent set (no torn 32-bit reads, no mid-update view).
     * A direction is "busy" while a Send/Receive transfer is in progress (which can
     * only advance while the stream is running). */
    was = nora_dma_irq_disable_save(SAI0_RX_DMA_CH);
    status.tx_busy      = ((ctx->tx_buf != NULL) && (ctx->tx_cnt < ctx->tx_num)) ? 1u : 0u;
    status.rx_busy      = ((ctx->rx_buf != NULL) && (ctx->rx_cnt < ctx->rx_num)) ? 1u : 0u;
    status.tx_underflow = ctx->tx_underflow;
    status.rx_overflow  = ctx->rx_overflow;
    nora_dma_irq_restore(SAI0_RX_DMA_CH, was);
    status.frame_error  = 0u;   /* FRMERR unverified in the slave-framed config */
    return status;
}

/* ========================================================================== */
/* Driver access structure                                                    */
/* ========================================================================== */

ARM_DRIVER_SAI Driver_SAI0 = {
    SAI0_GetVersion,
    SAI0_GetCapabilities,
    SAI0_Initialize,
    SAI0_Uninitialize,
    SAI0_PowerControl,
    SAI0_Send,
    SAI0_Receive,
    SAI0_GetTxCount,
    SAI0_GetRxCount,
    SAI0_Control,
    SAI0_GetStatus
};

#endif /* RTE_SAI0 != 0 */
