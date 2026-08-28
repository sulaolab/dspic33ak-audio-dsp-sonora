/* SPDX-License-Identifier: MIT-0 */

/*
 * Low-level NORA PWM (PG) backend for dsPIC33AK.
 *
 * Single file, unlike hal_uart's device.c split: that split earns its keep
 * with a real per-instance register-pointer table (multiple identically-typed
 * instances). PG1..PG8 have no such uniform table -- PGxEVT/PGxIOCON vs
 * PGxEVT1/PGxIOCON1 differ by device generation, so each instance's sequence
 * is its own macro expansion (NORA_PWM_DEFINE_GEN_H/_HL below) -- so keeping
 * the per-instance code and the public-API validation in one file matches
 * hal_dma's actual shape (nora_dma_dspic33ak.c has no separate device.c
 * either) and lets a single-TU build (this project does not use LTO) inline
 * across what used to be a device.c/dspic33ak.c call boundary. Measured on
 * AK128 (LED-only consumer): merging recovered was not enough by itself to
 * offset the multi-call-site cfg-struct indirection cost documented below --
 * see the ROM-budget note in nora_pwm.h.
 *
 * Validates public nora_pwm_generator_cfg_t semantics, resolves them to
 * register-neutral socs/pmod values, then runs the PGxCON..PGxIOCON1
 * sequence. Only this file names PG1CON..PG8IOCON1 and the
 * NORA_PPS_OUTPUT_PWM1H..PWM8L enum members.
 *
 * No DMA, ISR, float, or application-config (APP_USE_PWM_AUDIO etc.)
 * knowledge here -- see nora_pwm.h for the full scope statement.
 */

#include <xc.h>
#include <stddef.h>

#include "nora_pwm.h"
#include "nora_pps.h"   /* nora_pinmux_route_output(), NORA_PPS_OUTPUT_PWMxH/L */

#if defined(PG1EVT1)
#define NORA_PWM_EVTBITS(n)    PG##n##EVT1bits
#define NORA_PWM_IOCONREG(n)   PG##n##IOCON1
#define NORA_PWM_IOCONBITS(n)  PG##n##IOCON1bits
#else
#define NORA_PWM_EVTBITS(n)    PG##n##EVTbits
#define NORA_PWM_IOCONREG(n)   PG##n##IOCON
#define NORA_PWM_IOCONBITS(n)  PG##n##IOCONbits
#endif

/* Single output (H only). PENL may still be set (matches shipped pwm.c: the
 * LED generators enable PENL without ever routing PWMxL to a pin). */
#define NORA_PWM_DEFINE_GEN_H(n)                                             \
static bool nora_pwm_pg##n##_init(const nora_pwm_generator_cfg_t *cfg,       \
                                   uint32_t socs, uint32_t pmod)             \
{                                                                            \
    if ((cfg->rp_h != 0) &&                                                 \
        !nora_pinmux_route_output(NORA_PPS_OUTPUT_PWM##n##H, cfg->rp_h,     \
                                   false)) {                                 \
        return false;                                                       \
    }                                                                       \
    PG##n##CON = 0;                                                         \
    PG##n##CONbits.CLKSEL = 1;                                              \
    PG##n##CONbits.MDCSEL = 0;                                              \
    PG##n##CONbits.MPERSEL = 0;                                             \
    PG##n##CONbits.SOCS = socs;                                             \
    PG##n##PERbits.PER = cfg->period - 1u;                                  \
    PG##n##DCbits.DC = cfg->duty_init;                                      \
    PG##n##DTbits.DTH = cfg->dead_time_high;                                \
    PG##n##DTbits.DTL = cfg->dead_time_low;                                 \
    NORA_PWM_EVTBITS(n).UPDTRG = 0x1;                                       \
    NORA_PWM_EVTBITS(n).PGTRGSEL = 0x0;                                     \
    NORA_PWM_IOCONREG(n) = 0;                                               \
    NORA_PWM_IOCONBITS(n).PPSEN = 1;                                        \
    NORA_PWM_IOCONBITS(n).PMOD = pmod;                                      \
    NORA_PWM_IOCONBITS(n).PENH = cfg->pen_h ? 1u : 0u;                      \
    NORA_PWM_IOCONBITS(n).PENL = cfg->pen_l ? 1u : 0u;                      \
    PG##n##CONbits.MSTEN = cfg->is_module_master ? 1u : 0u;                 \
    PG##n##CONbits.ON = 1;                                                  \
    return true;                                                            \
}                                                                            \
static void nora_pwm_pg##n##_set_duty(uint32_t duty)                        \
{                                                                            \
    PG##n##DCbits.DC = duty;                                                \
}

/* Complementary H+L pair (audio-DAC use). AK512-only in practice (AK128 has
 * no PG5-8), but guarded on PGxCON presence like every other instance here,
 * not on APP_TARGET. */
#define NORA_PWM_DEFINE_GEN_HL(n)                                            \
static bool nora_pwm_pg##n##_init(const nora_pwm_generator_cfg_t *cfg,       \
                                   uint32_t socs, uint32_t pmod)             \
{                                                                            \
    if ((cfg->rp_h != 0) &&                                                 \
        !nora_pinmux_route_output(NORA_PPS_OUTPUT_PWM##n##H, cfg->rp_h,     \
                                   false)) {                                 \
        return false;                                                       \
    }                                                                       \
    if ((cfg->rp_l != 0) &&                                                 \
        !nora_pinmux_route_output(NORA_PPS_OUTPUT_PWM##n##L, cfg->rp_l,     \
                                   false)) {                                 \
        return false;                                                       \
    }                                                                       \
    PG##n##CON = 0;                                                         \
    PG##n##CONbits.CLKSEL = 1;                                              \
    PG##n##CONbits.MDCSEL = 0;                                              \
    PG##n##CONbits.MPERSEL = 0;                                             \
    PG##n##CONbits.SOCS = socs;                                             \
    PG##n##PERbits.PER = cfg->period - 1u;                                  \
    PG##n##DCbits.DC = cfg->duty_init;                                      \
    PG##n##DTbits.DTH = cfg->dead_time_high;                                \
    PG##n##DTbits.DTL = cfg->dead_time_low;                                 \
    NORA_PWM_EVTBITS(n).UPDTRG = 0x1;                                       \
    NORA_PWM_EVTBITS(n).PGTRGSEL = 0x0;                                     \
    NORA_PWM_IOCONREG(n) = 0;                                               \
    NORA_PWM_IOCONBITS(n).PPSEN = 1;                                        \
    NORA_PWM_IOCONBITS(n).PMOD = pmod;                                      \
    NORA_PWM_IOCONBITS(n).PENH = cfg->pen_h ? 1u : 0u;                      \
    NORA_PWM_IOCONBITS(n).PENL = cfg->pen_l ? 1u : 0u;                      \
    PG##n##CONbits.MSTEN = cfg->is_module_master ? 1u : 0u;                 \
    PG##n##CONbits.ON = 1;                                                  \
    return true;                                                            \
}                                                                            \
static void nora_pwm_pg##n##_set_duty(uint32_t duty)                        \
{                                                                            \
    PG##n##DCbits.DC = duty;                                                \
}

#if defined(PG1CON)
NORA_PWM_DEFINE_GEN_H(1)
#endif
#if defined(PG2CON)
NORA_PWM_DEFINE_GEN_H(2)
#endif
#if defined(PG3CON)
NORA_PWM_DEFINE_GEN_H(3)
#endif
#if defined(PG5CON)
NORA_PWM_DEFINE_GEN_HL(5)
#endif
#if defined(PG6CON)
NORA_PWM_DEFINE_GEN_HL(6)
#endif
#if defined(PG7CON)
NORA_PWM_DEFINE_GEN_HL(7)
#endif
#if defined(PG8CON)
NORA_PWM_DEFINE_GEN_HL(8)
#endif

static bool nora_pwm_device_is_present(nora_pwm_generator_id_t id)
{
    switch (id) {
#if defined(PG1CON)
    case NORA_PWM_GEN_1: return true;
#endif
#if defined(PG2CON)
    case NORA_PWM_GEN_2: return true;
#endif
#if defined(PG3CON)
    case NORA_PWM_GEN_3: return true;
#endif
#if defined(PG5CON)
    case NORA_PWM_GEN_5: return true;
#endif
#if defined(PG6CON)
    case NORA_PWM_GEN_6: return true;
#endif
#if defined(PG7CON)
    case NORA_PWM_GEN_7: return true;
#endif
#if defined(PG8CON)
    case NORA_PWM_GEN_8: return true;
#endif
    default: return false;
    }
}

static bool nora_pwm_device_init(nora_pwm_generator_id_t id,
                                  const nora_pwm_generator_cfg_t *cfg,
                                  uint32_t socs,
                                  uint32_t pmod)
{
    switch (id) {
#if defined(PG1CON)
    case NORA_PWM_GEN_1: return nora_pwm_pg1_init(cfg, socs, pmod);
#endif
#if defined(PG2CON)
    case NORA_PWM_GEN_2: return nora_pwm_pg2_init(cfg, socs, pmod);
#endif
#if defined(PG3CON)
    case NORA_PWM_GEN_3: return nora_pwm_pg3_init(cfg, socs, pmod);
#endif
#if defined(PG5CON)
    case NORA_PWM_GEN_5: return nora_pwm_pg5_init(cfg, socs, pmod);
#endif
#if defined(PG6CON)
    case NORA_PWM_GEN_6: return nora_pwm_pg6_init(cfg, socs, pmod);
#endif
#if defined(PG7CON)
    case NORA_PWM_GEN_7: return nora_pwm_pg7_init(cfg, socs, pmod);
#endif
#if defined(PG8CON)
    case NORA_PWM_GEN_8: return nora_pwm_pg8_init(cfg, socs, pmod);
#endif
    default: return false;
    }
}

static void nora_pwm_device_set_duty(nora_pwm_generator_id_t id, uint32_t duty)
{
    switch (id) {
#if defined(PG1CON)
    case NORA_PWM_GEN_1: nora_pwm_pg1_set_duty(duty); return;
#endif
#if defined(PG2CON)
    case NORA_PWM_GEN_2: nora_pwm_pg2_set_duty(duty); return;
#endif
#if defined(PG3CON)
    case NORA_PWM_GEN_3: nora_pwm_pg3_set_duty(duty); return;
#endif
#if defined(PG5CON)
    case NORA_PWM_GEN_5: nora_pwm_pg5_set_duty(duty); return;
#endif
#if defined(PG6CON)
    case NORA_PWM_GEN_6: nora_pwm_pg6_set_duty(duty); return;
#endif
#if defined(PG7CON)
    case NORA_PWM_GEN_7: nora_pwm_pg7_set_duty(duty); return;
#endif
#if defined(PG8CON)
    case NORA_PWM_GEN_8: nora_pwm_pg8_set_duty(duty); return;
#endif
    default: return;
    }
}

static bool s_module_initialized = false;
static nora_pwm_mclk_source_t s_module_source;
static bool s_generator_ready[NORA_PWM_GEN_COUNT];

bool nora_pwm_module_init(nora_pwm_mclk_source_t source)
{
    if (s_module_initialized) {
        return (s_module_source == source);
    }

    if (source == NORA_PWM_MCLK_HIGH_FREQ) {
        PCLKCONbits.MCLKSEL = 1;
    } else {
        PCLKCONbits.MCLKSEL = 0;
        PCLKCONbits.DIVSEL = 0;
    }
    MPER = 0;  /* Master PER/DC are not used; every generator uses its own PGxPER/PGxDC. */
    MDC = 0;

    s_module_initialized = true;
    s_module_source = source;
    return true;
}

/* PGxIOCON(1).PMOD encoding: 00 Complementary, 01 Independent, 10 Push-Pull. */
static bool nora_pwm_resolve_pmod(nora_pwm_output_mode_t mode, uint32_t *pmod)
{
    switch (mode) {
    case NORA_PWM_MODE_COMPLEMENTARY: *pmod = 0x0u; return true;
    case NORA_PWM_MODE_INDEPENDENT:   *pmod = 0x1u; return true;
    case NORA_PWM_MODE_PUSH_PULL:     *pmod = 0x2u; return true;
    default: return false;
    }
}

/* PGxCON.SOCS: 0000 = local EOC (self-run). The two non-zero encodings below
 * are the only (follower, master) pairings a datasheet-verified sequence
 * exists for in this tree today (classic_audio_pwm.c's PG6-follows-PG5 /
 * PG8-follows-PG7 L/R pairing) -- any other pairing is refused rather than
 * guessed. */
static bool nora_pwm_resolve_socs(nora_pwm_generator_id_t id,
                                   const nora_pwm_generator_cfg_t *cfg,
                                   uint32_t *socs)
{
    if (!cfg->trigger_follows) {
        *socs = 0x0u;
        return true;
    }
    if ((id == NORA_PWM_GEN_6) && (cfg->trigger_master == NORA_PWM_GEN_5)) {
        *socs = 0x5u;   /* "PWM5 PG Trigger output" */
        return true;
    }
    if ((id == NORA_PWM_GEN_8) && (cfg->trigger_master == NORA_PWM_GEN_7)) {
        *socs = 0x7u;   /* "PWM7 PG Trigger output" */
        return true;
    }
    return false;
}

bool nora_pwm_generator_init(nora_pwm_generator_id_t id,
                              const nora_pwm_generator_cfg_t *cfg)
{
    uint32_t socs;
    uint32_t pmod;

    if ((cfg == NULL) || ((unsigned)id >= (unsigned)NORA_PWM_GEN_COUNT)) {
        return false;
    }
    if (!s_module_initialized) {
        /* Loud, checkable failure instead of silently running off whatever
         * PCLKCON happened to already hold. */
        return false;
    }
    if (!nora_pwm_device_is_present(id)) {
        return false;
    }
    if (!nora_pwm_resolve_pmod(cfg->output_mode, &pmod)) {
        return false;
    }
    if (!nora_pwm_resolve_socs(id, cfg, &socs)) {
        return false;
    }

    if (!nora_pwm_device_init(id, cfg, socs, pmod)) {
        return false;
    }

    s_generator_ready[id] = true;
    return true;
}

void nora_pwm_generator_set_duty(nora_pwm_generator_id_t id, uint32_t duty)
{
    if ((unsigned)id >= (unsigned)NORA_PWM_GEN_COUNT) {
        return;
    }
    if (!s_generator_ready[id]) {
        return;
    }
    nora_pwm_device_set_duty(id, duty);
}

bool nora_pwm_generator_is_present(nora_pwm_generator_id_t id)
{
    if ((unsigned)id >= (unsigned)NORA_PWM_GEN_COUNT) {
        return false;
    }
    return nora_pwm_device_is_present(id);
}
