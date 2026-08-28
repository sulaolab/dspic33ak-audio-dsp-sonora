/* SPDX-License-Identifier: MIT-0 */
#ifndef NORA_PWM_H
#define NORA_PWM_H

#include <stdint.h>
#include <stdbool.h>

#include "nora_gpio.h"   /* nora_gpio_rp_t */

/*
 * nora_pwm.h
 * NORA PWM (PG - PWM Generator) HAL public interface.
 *
 * Portability scope:
 *   Register-level PG generator primitives only: module-wide clock-source
 *   select, per-generator period/duty/dead-time/output-enable/trigger setup,
 *   and a direct duty write. This is not a universal PWM HAL (no space-vector,
 *   no fault/PCI handling, no current-limit inputs) -- those have zero
 *   callers anywhere in this project today, and this is the first PWM HAL in
 *   the fleet, so there is no sibling scope to match either.
 *
 * Design boundaries (intentional)
 * --------------------------------
 *  - No DMA / duty-streaming policy. A generator whose duty is fed by DMA
 *    (audio-DAC use) is configured here the same as one whose duty is written
 *    by the CPU (LED use); which one and how often is entirely the caller's
 *    business.
 *  - No callback framework: PG interrupt vectors stay in the consumer.  This
 *    HAL never enables a PG's own EOC/fault interrupt.
 *  - No float conversion, no channel/slot layout policy, no board pin *names*
 *    (callers pass RP numbers from their own board map).
 *  - App -> HAL one-way dependency: this module knows nothing about
 *    app_specific_config_defs.h, APP_USE_PWM_AUDIO, or any app-level macro.
 *
 * The module-wide clock-select bit (PCLKCON.MCLKSEL/DIVSEL, MPER, MDC) is
 * shared by every PG instance on this silicon -- it is not per-generator.
 * nora_pwm_module_init() owns it explicitly, once, and every subsequent
 * nora_pwm_generator_init() call checks that it already ran rather than
 * silently assuming a caller ordering.
 */

typedef enum {
    NORA_PWM_GEN_1 = 0,
    NORA_PWM_GEN_2,
    NORA_PWM_GEN_3,
    NORA_PWM_GEN_5,
    NORA_PWM_GEN_6,
    NORA_PWM_GEN_7,
    NORA_PWM_GEN_8,
    NORA_PWM_GEN_COUNT,
} nora_pwm_generator_id_t;

/* PGxIOCON(1).PMOD. */
typedef enum {
    NORA_PWM_MODE_COMPLEMENTARY,  /* 00 */
    NORA_PWM_MODE_INDEPENDENT,    /* 01 */
    NORA_PWM_MODE_PUSH_PULL,      /* 10 */
} nora_pwm_output_mode_t;

/* PCLKCON.MCLKSEL/DIVSEL -- module-wide, not per-generator. */
typedef enum {
    NORA_PWM_MCLK_STANDARD,    /* standard peripheral clock (MCLKSEL=0)      */
    NORA_PWM_MCLK_HIGH_FREQ,   /* board's high-freq PWM clock (MCLKSEL=1) --
                                * caller must have that clock already running
                                * (e.g. sonora_clock_pwm_prepare()) before
                                * calling nora_pwm_module_init() with this. */
} nora_pwm_mclk_source_t;

/*
 * One generator's configuration.
 *
 * period/duty_init are generator-clock-domain counts in the same Q4-ish units
 * PGxPER/PGxDC already take (this HAL applies the "-1" PGxPER convention
 * internally; pass the full period count, not period-1).
 *
 * rp_h/rp_l: 0 means "do not route this output" (leave PPS unrouted). Every
 * board in this project numbers real RPs from 1 up, so 0 is a safe sentinel.
 *
 * trigger_follows/trigger_master: only two (follower, master) pairings are
 * datasheet-verified and implemented today -- GEN_6 following GEN_5, and
 * GEN_8 following GEN_7 (the audio L/R pairing). Any other pairing returns
 * false from nora_pwm_generator_init() rather than guessing an SOCS encoding
 * nothing here has confirmed.
 */
typedef struct {
    uint32_t period;
    uint32_t duty_init;
    uint16_t dead_time_high;
    uint16_t dead_time_low;

    nora_gpio_rp_t rp_h;
    nora_gpio_rp_t rp_l;
    bool pen_h;
    bool pen_l;

    nora_pwm_output_mode_t output_mode;

    bool trigger_follows;
    nora_pwm_generator_id_t trigger_master;   /* used iff trigger_follows */

    bool is_module_master;   /* PGxCON.MSTEN */
} nora_pwm_generator_cfg_t;

/*
 * Set the module-wide PWM clock source (PCLKCON.MCLKSEL/DIVSEL) and clear the
 * unused master PER/DC. Must be called exactly once, before any
 * nora_pwm_generator_init() call. Calling it again with the SAME source is a
 * harmless no-op; calling it again with a DIFFERENT source is refused (every
 * generator already configured is running off the clock the first call
 * chose) -- returns false in that case.
 */
bool nora_pwm_module_init(nora_pwm_mclk_source_t source);

/*
 * Configure and enable one PG generator: dead time, PPS pin routing (via
 * nora_pinmux_route_output(), skipped for a 0 rp), output mode/enables,
 * trigger source, MSTEN, then ON. Returns false (writes nothing further) if:
 *   - nora_pwm_module_init() has not run yet,
 *   - id names a generator this device does not have (AK128 has no PG5-8),
 *   - a requested pin route fails,
 *   - trigger_follows names an (id, master) pair not listed above.
 */
bool nora_pwm_generator_init(nora_pwm_generator_id_t id,
                              const nora_pwm_generator_cfg_t *cfg);

/* Direct PGxDC write. No-op for an id this device does not have or one never
 * initialized. Not meant for a generator whose duty is DMA-fed. */
void nora_pwm_generator_set_duty(nora_pwm_generator_id_t id, uint32_t duty);

/* True if this device defines the generator (independent of whether it has
 * been initialized yet). */
bool nora_pwm_generator_is_present(nora_pwm_generator_id_t id);

#endif /* NORA_PWM_H */
