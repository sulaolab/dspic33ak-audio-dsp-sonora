#ifndef NORA_HIGH_RES_TIMER_H
#define NORA_HIGH_RES_TIMER_H

/*
 * nora_high_res_timer.h
 * --------------------------
 * Minimal Timer2-based free-running high-resolution counter for profiling and
 * short interval measurement.
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NORA_HIGH_RES_TIMER_OK = 0,
    NORA_HIGH_RES_TIMER_ERR_INVALID_ARG,
    NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT,
    NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED
} nora_high_res_timer_status_t;

typedef struct {
    /* Actual input clock supplied to Timer2, in Hz. */
    uint32_t timer_clk_hz;
    bool run_in_idle;
} nora_high_res_timer_config_t;

/* Configure and start Timer2 as a free-running counter. On success, this HAL
 * owns Timer2 until nora_high_res_timer_deinit() is called. */
nora_high_res_timer_status_t nora_high_res_timer_init(
    const nora_high_res_timer_config_t *config);

nora_high_res_timer_status_t nora_high_res_timer_deinit(void);

bool nora_high_res_timer_is_present(void);

bool nora_high_res_timer_is_initialized(void);

/* Return the raw Timer2 counter value. One count is one Timer2 input-clock
 * period. Returns 0 when Timer2 is absent or the HAL is not initialized. */
uint32_t nora_high_res_timer_get_count(void);

/* Wraparound-safe when the measured interval is shorter than one full 32-bit
 * Timer2 counter cycle. At 100 MHz, one cycle is about 42.95 seconds. */
uint32_t nora_high_res_timer_elapsed_count(uint32_t start_count);

/* Convert raw counts to integer microseconds. The result is truncated and
 * saturates to UINT32_MAX if the converted value does not fit in uint32_t. */
uint32_t nora_high_res_timer_count_to_us(uint32_t count);

/* Convert raw counts to 0.1 us units. For example, 1234 means 123.4 us.
 * The result is truncated and saturates to UINT32_MAX if the converted value
 * does not fit in uint32_t. This uses 64-bit division; prefer storing raw
 * counts in ISRs and converting them later in foreground/status code. */
uint32_t nora_high_res_timer_count_to_us_x10(uint32_t count);

/* Return elapsed time from a saved count in integer microseconds. */
uint32_t nora_high_res_timer_elapsed_us(uint32_t start_count);

/* Return elapsed time from a saved count in 0.1 us units. */
uint32_t nora_high_res_timer_elapsed_us_x10(uint32_t start_count);

#ifdef __cplusplus
}
#endif

#endif /* NORA_HIGH_RES_TIMER_H */
