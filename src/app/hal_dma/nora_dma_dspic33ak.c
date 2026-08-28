/* SPDX-License-Identifier: MIT-0 */
/*
 * Low-level NORA DMA backend for dsPIC33AK - implementation.
 *
 * Uses nora_dma_dspic33ak_reg.h for register bit masks/positions and generic SFR
 * helpers; no raw XC-DSC bitfield names (DMA0CHbits.*, DMA0SELbits.*, _DMA0IF,
 * ...) appear in this file except inside the small, documented per-channel IRQ
 * switch (the DMA IRQ Flag/Enable/Priority bits do not channel-index).
 *
 * This is the low-level DMA path used by the SPI-TDM driver for DMA global init,
 * channel config/enable, flag clear, stat/src read, and the ping-pong half query.
 *
 * Global init must be performed once (nora_dma_global_init(), called from
 * main) before any channel config. channel_config()/channel_enable() return
 * false (and write nothing) if the DMA controller is not ready; the caller
 * decides how to react. This module performs no printf / halt / app handling.
 *
 * No dependency on SPI, audio, PWM, DSP, printf, or application code.
 *
 * Device note: AK512 has DMA channels 0..7; AK128 has 0..5.  Channels 6/7 are
 * guarded by _DMA6IF / _DMA7IF (device-header macros present only where the
 * channel exists), so this file builds on both devices.
 */

#include <xc.h>

#include "nora_dma.h"
#include "nora_dma_dspic33ak_reg.h"
#include "nora_dma_dspic33ak_fast.h"

/* ---------------------------------------------------------------------------
 * Private: per-channel register mapping
 *
 * All DMAxCH/SRC/DST/CNT/SEL/STAT registers are uniform 32-bit SFRs, so the
 * channel number (otherwise baked into the symbol name) is collapsed into an
 * index here.  The table auto-sizes per device (6 entries on AK128, 8 on AK512).
 * ------------------------------------------------------------------------- */

typedef struct {
    volatile uint32_t *CH;
    volatile uint32_t *SRC;
    volatile uint32_t *DST;
    volatile uint32_t *CNT;
    volatile uint32_t *SEL;
    volatile uint32_t *STAT;
} nora_dma_ch_regs_t;

static const nora_dma_ch_regs_t s_dma_ch[] = {
    { &DMA0CH, &DMA0SRC, &DMA0DST, &DMA0CNT, &DMA0SEL, &DMA0STAT },
    { &DMA1CH, &DMA1SRC, &DMA1DST, &DMA1CNT, &DMA1SEL, &DMA1STAT },
    { &DMA2CH, &DMA2SRC, &DMA2DST, &DMA2CNT, &DMA2SEL, &DMA2STAT },
    { &DMA3CH, &DMA3SRC, &DMA3DST, &DMA3CNT, &DMA3SEL, &DMA3STAT },
    { &DMA4CH, &DMA4SRC, &DMA4DST, &DMA4CNT, &DMA4SEL, &DMA4STAT },
    { &DMA5CH, &DMA5SRC, &DMA5DST, &DMA5CNT, &DMA5SEL, &DMA5STAT },
#if defined(_DMA6IF)
    { &DMA6CH, &DMA6SRC, &DMA6DST, &DMA6CNT, &DMA6SEL, &DMA6STAT },
#endif
#if defined(_DMA7IF)
    { &DMA7CH, &DMA7SRC, &DMA7DST, &DMA7CNT, &DMA7SEL, &DMA7STAT },
#endif
};

#define NORA_DMA_DSPIC33AK_CHANNEL_COUNT  (sizeof(s_dma_ch) / sizeof(s_dma_ch[0]))

static const nora_dma_ch_regs_t *nora_dma_regs(nora_dma_channel_t ch)
{
    if ((unsigned)ch >= NORA_DMA_DSPIC33AK_CHANNEL_COUNT) {
        return (const nora_dma_ch_regs_t *)0;
    }
    return &s_dma_ch[ch];
}

/* Translate public DMA semantics into the dsPIC33AK register encodings. */
static bool nora_dma_dspic33ak_size_to_reg(nora_dma_size_t size, uint32_t *value)
{
    if (value == 0) {
        return false;
    }
    switch (size) {
    case NORA_DMA_SIZE_BYTE:     *value = 0u; return true;
    case NORA_DMA_SIZE_HALFWORD: *value = 1u; return true;
    case NORA_DMA_SIZE_WORD:     *value = 2u; return true;
    default: return false;
    }
}

static bool nora_dma_dspic33ak_addr_mode_to_reg(nora_dma_addr_mode_t mode,
                                                uint32_t *value)
{
    if (value == 0) {
        return false;
    }
    switch (mode) {
    case NORA_DMA_ADDR_FIXED:     *value = 0u; return true;
    case NORA_DMA_ADDR_INCREMENT: *value = 1u; return true;
    case NORA_DMA_ADDR_DECREMENT: *value = 2u; return true;
    default: return false;
    }
}

static bool nora_dma_dspic33ak_trmode_to_reg(nora_dma_trmode_t mode,
                                             uint32_t *value)
{
    if (value == 0) {
        return false;
    }
    switch (mode) {
    case NORA_DMA_TRMODE_ONESHOT:           *value = 0u; return true;
    case NORA_DMA_TRMODE_REPEAT_ONESHOT:    *value = 1u; return true;
    case NORA_DMA_TRMODE_CONTINUOUS:        *value = 2u; return true;
    case NORA_DMA_TRMODE_REPEAT_CONTINUOUS: *value = 3u; return true;
    default: return false;
    }
}

static bool nora_dma_dspic33ak_trigger_to_chsel(nora_dma_trigger_t trigger,
                                                uint32_t *chsel)
{
    if (chsel == 0) {
        return false;
    }
    switch (trigger) {
    /* The quiet code for a software-only channel.  DS70005591C Table 14-2 has no
     * "no trigger source" encoding -- every CHSEL value names a peripheral, and the
     * Reserved ranges (2Bh "tie to 0b0", 5Dh-62h, 6Fh-7Fh "do not use") are not an
     * option.  03h "NVM - NVM Write Complete" is chosen because this family cannot
     * raise it without a flash write in progress, so it is quiet by construction
     * rather than quiet on one board.  A port that self-programs flash while a
     * software-only channel is armed must choose another code -- the same caveat the
     * CK backend records for its own 07h. */
    case NORA_DMA_TRIGGER_NONE:    *chsel = 0x3u; return true;
    case NORA_DMA_TRIGGER_SPI1_RX: *chsel = 0x6u; return true;
    case NORA_DMA_TRIGGER_SPI1_TX: *chsel = 0x7u; return true;
    case NORA_DMA_TRIGGER_SPI2_RX: *chsel = 0x8u; return true;
    case NORA_DMA_TRIGGER_SPI2_TX: *chsel = 0x9u; return true;
    case NORA_DMA_TRIGGER_SPI3_RX: *chsel = 0xAu; return true;
    case NORA_DMA_TRIGGER_SPI3_TX: *chsel = 0xBu; return true;
#if defined(_DMA6IF)
    case NORA_DMA_TRIGGER_SPI4_RX: *chsel = 0xCu; return true;
    case NORA_DMA_TRIGGER_SPI4_TX: *chsel = 0xDu; return true;
#endif
    /* PWM Generator 5-8 (Classic-app audio-DAC, AK512 only). PG5/6 use DMA4/5,
     * which exist on every device this backend supports, so those two are not
     * guarded here; the caller (classic_audio_pwm.c) is itself AK512-only. */
    case NORA_DMA_TRIGGER_PWM_GEN5: *chsel = 0x27u; return true;
    case NORA_DMA_TRIGGER_PWM_GEN6: *chsel = 0x28u; return true;
#if defined(_DMA6IF)
    case NORA_DMA_TRIGGER_PWM_GEN7: *chsel = 0x29u; return true;
    case NORA_DMA_TRIGGER_PWM_GEN8: *chsel = 0x2Au; return true;
#endif
    default: return false;
    }
}

/* ---------------------------------------------------------------------------
 * Private: interrupt Flag/Enable/Priority
 *
 * Unlike the data/config registers, the DMA IRQ bits live in scattered CPU
 * registers (IFS2/IFS3, IEC2/IEC3, IPC9/IPC10/IPC14/IPC15) and do not
 * channel-index.  The per-channel mapping is isolated here as small switches
 * built on the device-header convenience macros.  Cases 6/7 are guarded for
 * AK128 (which has no DMA6/DMA7).
 * ------------------------------------------------------------------------- */

static void nora_dma_irq_clear_flag(nora_dma_channel_t ch)
{
    switch (ch) {
    case 0: _DMA0IF = 0; break;
    case 1: _DMA1IF = 0; break;
    case 2: _DMA2IF = 0; break;
    case 3: _DMA3IF = 0; break;
    case 4: _DMA4IF = 0; break;
    case 5: _DMA5IF = 0; break;
#if defined(_DMA6IF)
    case 6: _DMA6IF = 0; break;
#endif
#if defined(_DMA7IF)
    case 7: _DMA7IF = 0; break;
#endif
    default: break;
    }
}

/* Both arms write a literal, via NORA_DMA_IRQ_IE_WRITE - `_DMAxIE = v` with a
 * runtime v would read-modify-write a byte of IECx that other peripherals share.
 * The register header states the byte map and the reason. */
static void nora_dma_hw_irq_enable(nora_dma_channel_t ch, bool enable)
{
    switch (ch) {
    case 0: NORA_DMA_IRQ_IE_WRITE(_DMA0IE, enable); break;
    case 1: NORA_DMA_IRQ_IE_WRITE(_DMA1IE, enable); break;
    case 2: NORA_DMA_IRQ_IE_WRITE(_DMA2IE, enable); break;
    case 3: NORA_DMA_IRQ_IE_WRITE(_DMA3IE, enable); break;
    case 4: NORA_DMA_IRQ_IE_WRITE(_DMA4IE, enable); break;
    case 5: NORA_DMA_IRQ_IE_WRITE(_DMA5IE, enable); break;
#if defined(_DMA6IF)
    case 6: NORA_DMA_IRQ_IE_WRITE(_DMA6IE, enable); break;
#endif
#if defined(_DMA7IF)
    case 7: NORA_DMA_IRQ_IE_WRITE(_DMA7IE, enable); break;
#endif
    default: break;
    }
}

static bool nora_dma_hw_irq_is_enabled(nora_dma_channel_t ch)
{
    switch (ch) {
    case 0: return (_DMA0IE != 0u);
    case 1: return (_DMA1IE != 0u);
    case 2: return (_DMA2IE != 0u);
    case 3: return (_DMA3IE != 0u);
    case 4: return (_DMA4IE != 0u);
    case 5: return (_DMA5IE != 0u);
#if defined(_DMA6IF)
    case 6: return (_DMA6IE != 0u);
#endif
#if defined(_DMA7IF)
    case 7: return (_DMA7IE != 0u);
#endif
    default: return false;
    }
}

/* Priority is deliberately left as a runtime-value write (a 4-bit field in IPCx,
 * so a read-modify-write either way).  Unlike IFS/IEC, hardware never writes IPCx
 * and it is only programmed at init/reconfigure, so there is no concurrent writer
 * to race with - adding an atomicity dance here would complicate the API for a
 * hazard that does not exist. Callers may also re-program a priority after init
 * (see nora_spi_i2s_tdm_set_rate_monotonic_priorities), which does not change
 * that: IPCx still has exactly one writer, task level. */
void nora_dma_irq_set_priority(nora_dma_channel_t ch, uint8_t prio)
{
    switch (ch) {
    case 0: _DMA0IP = prio; break;
    case 1: _DMA1IP = prio; break;
    case 2: _DMA2IP = prio; break;
    case 3: _DMA3IP = prio; break;
    case 4: _DMA4IP = prio; break;
    case 5: _DMA5IP = prio; break;
#if defined(_DMA6IF)
    case 6: _DMA6IP = prio; break;
#endif
#if defined(_DMA7IF)
    case 7: _DMA7IP = prio; break;
#endif
    default: break;
    }
}

/* Read side of the above. A bit-alias READ is a plain field extract with no
 * write-back, so nothing here can disturb the neighbouring channel's field. */
uint8_t nora_dma_irq_get_priority(nora_dma_channel_t ch)
{
    switch (ch) {
    case 0: return (uint8_t)_DMA0IP;
    case 1: return (uint8_t)_DMA1IP;
    case 2: return (uint8_t)_DMA2IP;
    case 3: return (uint8_t)_DMA3IP;
    case 4: return (uint8_t)_DMA4IP;
    case 5: return (uint8_t)_DMA5IP;
#if defined(_DMA6IF)
    case 6: return (uint8_t)_DMA6IP;
#endif
#if defined(_DMA7IF)
    case 7: return (uint8_t)_DMA7IP;
#endif
    default: return 0u;
    }
}

/* ---------------------------------------------------------------------------
 * Global
 * ------------------------------------------------------------------------- */

void nora_dma_global_init(void)
{
    /* Configure DMA global state explicitly.
     * This turns the controller on and programs the allowed address window every
     * time (the window is written even if the controller was already on).
     * No printf / halt / application handling is performed here. */
    nora_dma_reg_set(&DMACON, NORA_DMA_DSPIC33AK_CON_ON);  /* DMACONbits.ON = 1 */

    /* The CPU X/Y data buses outrank DMA by default when both contend for SRAM.
     * Audio SPI requests arrive every 32-bit word and the channel has only one
     * pending CHREQ slot, so a delayed request becomes DMAxSTAT.OVERRUN. Give
     * DMA RAM transactions priority over the CPU; SFR arbitration is unchanged. */
    nora_dma_reg_set(&BMXINITPR, NORA_DMA_DSPIC33AK_BMX_INITPR_DMAPR);

    DMAHIGH = NORA_DMA_DSPIC33AK_ADDR_WINDOW_HIGH;
    DMALOW  = NORA_DMA_DSPIC33AK_ADDR_WINDOW_LOW;
}

bool nora_dma_global_is_ready(void)
{
    /* Side-effect-free readiness check: controller on and address window set to
     * the configured values. Returns a bool; never prints or halts. */
    if (!nora_dma_reg_is_set(&DMACON, NORA_DMA_DSPIC33AK_CON_ON)) {
        return false;
    }
    if (DMAHIGH != NORA_DMA_DSPIC33AK_ADDR_WINDOW_HIGH) {
        return false;
    }
    if (DMALOW != NORA_DMA_DSPIC33AK_ADDR_WINDOW_LOW) {
        return false;
    }
    if (!nora_dma_reg_is_set(&BMXINITPR, NORA_DMA_DSPIC33AK_BMX_INITPR_DMAPR)) {
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Per channel
 * ------------------------------------------------------------------------- */

bool nora_dma_channel_config(nora_dma_channel_t ch, const nora_dma_channel_cfg_t *cfg)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);
    uint32_t src_mode;
    uint32_t dst_mode;
    uint32_t size;
    uint32_t trmode;
    uint32_t trigger;

    /* Validate inputs and global state before touching any register. On failure
     * no DMA channel register is written. nora_dma_global_init() is never
     * called from here. */
    if ((r == 0) || (cfg == 0)) {
        return false;
    }
    if (!nora_dma_global_is_ready()) {
        return false;
    }

    /* Validate public semantic values before touching channel registers. */
    if (!nora_dma_dspic33ak_size_to_reg(cfg->size, &size) ||
        !nora_dma_dspic33ak_addr_mode_to_reg(cfg->src_mode, &src_mode) ||
        !nora_dma_dspic33ak_addr_mode_to_reg(cfg->dst_mode, &dst_mode) ||
        !nora_dma_dspic33ak_trmode_to_reg(cfg->tr_mode, &trmode) ||
        !nora_dma_dspic33ak_trigger_to_chsel(cfg->trigger, &trigger)) {
        return false;
    }
    if (cfg->irq_priority_set && (cfg->irq_priority > 7u)) {
        return false;
    }

    /* Mask this channel's CPU IRQ before reconfiguring. If the channel ran
     * before, its _DMAxIE may still be enabled and a _DMAxIF may still be
     * pending; masking first prevents a stale DMA interrupt from firing while
     * the channel is being reprogrammed (re-config / restart safety). */
    nora_dma_hw_irq_enable(ch, false);

    /* Start from a known-disabled state (matches "DMAxCH = 0; CHEN = 0;") and
     * clear stale channel-side status + pending CPU flag before programming.
     * HALF/DONE left over from a previous run must not leak into the next run's
     * ping-pong half decision. */
    *r->CH   = 0u;
    *r->STAT = 0u;
    nora_dma_irq_clear_flag(ch);

    /* Addresses and element count. Cast via uintptr_t (pointer -> integer) before
     * narrowing to the 32-bit register width. */
    *r->SRC = (uint32_t)(uintptr_t)cfg->src;
    *r->DST = (uint32_t)(uintptr_t)cfg->dst;
    *r->CNT = cfg->count;

    /* DMAxCH fields (CHEN intentionally left 0 here). */
    nora_dma_reg_write_field(r->CH, NORA_DMA_DSPIC33AK_CH_SAMODE_MASK,
                                  NORA_DMA_DSPIC33AK_CH_SAMODE_POS, src_mode);
    nora_dma_reg_write_field(r->CH, NORA_DMA_DSPIC33AK_CH_DAMODE_MASK,
                                  NORA_DMA_DSPIC33AK_CH_DAMODE_POS, dst_mode);
    nora_dma_reg_write_field(r->CH, NORA_DMA_DSPIC33AK_CH_SIZE_MASK,
                                  NORA_DMA_DSPIC33AK_CH_SIZE_POS, size);
    nora_dma_reg_write_field(r->CH, NORA_DMA_DSPIC33AK_CH_TRMODE_MASK,
                                  NORA_DMA_DSPIC33AK_CH_TRMODE_POS, trmode);

    if (cfg->reload_count) { nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_RELOADC); }
    if (cfg->reload_src)   { nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_RELOADS); }
    if (cfg->reload_dst)   { nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_RELOADD); }

    if (cfg->half_int_en)  { nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_HALFEN); }
    if (cfg->done_int_en)  { nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_DONEEN); }

    /* Trigger source (DMAxSELbits.CHSEL). */
    nora_dma_reg_write_field(r->SEL, NORA_DMA_DSPIC33AK_SEL_CHSEL_MASK,
                                  NORA_DMA_DSPIC33AK_SEL_CHSEL_POS, trigger);

    /* Clear stale channel status + pending CPU flag again after programming and
     * before (re-)enabling the IRQ, so the first post-config interrupt reflects
     * only the newly started transfer. */
    *r->STAT = 0u;
    nora_dma_irq_clear_flag(ch);

    /* CPU interrupt: priority only if requested (preserves PWM's untouched IP),
     * then enable per cfg. */
    if (cfg->irq_priority_set) {
        nora_dma_irq_set_priority(ch, cfg->irq_priority);
    }
    nora_dma_hw_irq_enable(ch, cfg->irq_enable);

    return true;
}

bool nora_dma_channel_enable(nora_dma_channel_t ch, bool enable)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return false;
    }
    if (enable) {
        /* Do not start a channel when the DMA controller is not ready. */
        if (!nora_dma_global_is_ready()) {
            return false;
        }
        nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_CHEN);
    } else {
        /* Disable is always allowed (safe direction), even if not "ready". */
        nora_dma_reg_clear(r->CH, NORA_DMA_DSPIC33AK_CH_CHEN);
    }
    return true;
}

/* Software request / request-pending / remaining count.
 *
 * These three are part of the portable NORA DMA contract even though nothing in
 * this tree calls them: the silicon has the bits (DMAxCHbits.CHREQ, DMAxCNT) and a
 * consumer written for another family -- e.g. the CK DMA self-test -- has to build
 * here unchanged. Out-of-line and off every ISR path, so they cost nothing in a
 * build with per-function sections and --gc-sections. */
void nora_dma_software_trigger(nora_dma_channel_t ch)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return;
    }
    nora_dma_reg_set(r->CH, NORA_DMA_DSPIC33AK_CH_CHREQ);
}

bool nora_dma_request_pending(nora_dma_channel_t ch)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return false;
    }
    /* Hardware clears CHREQ when the requested transfer has been serviced. */
    return nora_dma_reg_is_set(r->CH, NORA_DMA_DSPIC33AK_CH_CHREQ);
}

uint32_t nora_dma_read_count(nora_dma_channel_t ch)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return 0u;
    }
    /* DMAxCNT is 24 bits; mask so the value cannot depend on reserved bits. */
    return (*r->CNT & NORA_DMA_DSPIC33AK_CNT_MASK);
}

void nora_dma_irq_enable(nora_dma_channel_t ch, bool enable)
{
    nora_dma_hw_irq_enable(ch, enable);
}

bool nora_dma_irq_is_enabled(nora_dma_channel_t ch)
{
    return nora_dma_hw_irq_is_enabled(ch);
}

bool nora_dma_irq_disable_save(nora_dma_channel_t ch)
{
    return nora_dma_irq_disable_save_hot(ch);
}

void nora_dma_irq_restore(nora_dma_channel_t ch, bool was_enabled)
{
    nora_dma_irq_restore_hot(ch, was_enabled);
}

void nora_dma_clear_status(nora_dma_channel_t ch)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return;
    }
    *r->STAT = 0u;
}

void nora_dma_clear_irq_flag(nora_dma_channel_t ch)
{
    nora_dma_irq_clear_flag(ch);
}

nora_dma_status_t nora_dma_read_status(nora_dma_channel_t ch)
{
    const nora_dma_ch_regs_t *r = nora_dma_regs(ch);

    if (r == 0) {
        return 0u;
    }
    return *r->STAT;
}

nora_dma_status_t nora_dma_isr_snapshot(nora_dma_channel_t ch)
{
    return nora_dma_isr_snapshot_hot(ch);
}

uint32_t nora_dma_read_src(nora_dma_channel_t ch)
{
    return nora_dma_read_src_hot(ch);
}

bool nora_dma_status_has_half_done_conflict(nora_dma_status_t status)
{
    return nora_dma_status_has_half_done_conflict_hot(status);
}

bool nora_dma_status_has_overrun(nora_dma_status_t status)
{
    return nora_dma_status_has_overrun_hot(status);
}

bool nora_dma_status_has_completed_half(nora_dma_status_t status)
{
    return nora_dma_status_has_completed_half_hot(status);
}

bool nora_dma_status_has_completed(nora_dma_status_t status)
{
    return nora_dma_status_has_completed_hot(status);
}

nora_dma_half_t nora_dma_half_from_status(nora_dma_status_t status)
{
    /* DONE takes precedence over HALF, matching the current RX handler
     * (tdm_get_src_ptr: the DONE branch overwrites the HALF branch). */
    if ((status & NORA_DMA_DSPIC33AK_STAT_DONE) != 0u) {
        return NORA_DMA_HALF_SECOND;
    }
    if ((status & NORA_DMA_DSPIC33AK_STAT_HALF) != 0u) {
        return NORA_DMA_HALF_FIRST;
    }
    return NORA_DMA_HALF_NONE;
}
