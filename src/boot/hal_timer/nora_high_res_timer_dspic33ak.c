/*
 * nora_high_res_timer_dspic33ak.c
 * --------------------------
 * Timer2 free-running high-resolution counter. See
 * nora_high_res_timer.h.
 */

#include "nora_high_res_timer.h"

#include <xc.h>

#if defined(T2CON) && defined(TMR2) && defined(PR2) && defined(_T2IF) && defined(_T2IE)
#define DSPIC33AK_HIGH_RES_TIMER_PRESENT 1
#else
#define DSPIC33AK_HIGH_RES_TIMER_PRESENT 0
#endif

static uint32_t high_res_timer_clk_hz = 0u;
static volatile bool high_res_timer_initialized = false;

static uint32_t count_to_units(uint32_t count, uint64_t units_per_second);

nora_high_res_timer_status_t nora_high_res_timer_init(
    const nora_high_res_timer_config_t *config)
{
    if ((config == 0) || (config->timer_clk_hz == 0u)) {
        return NORA_HIGH_RES_TIMER_ERR_INVALID_ARG;
    }

    if (!nora_high_res_timer_is_present()) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
    }

#if DSPIC33AK_HIGH_RES_TIMER_PRESENT
    T2CONbits.ON = 0;
    _T2IE = 0;
    _T2IF = 0;
    high_res_timer_initialized = false;

    T2CON = 0u;
    T2CONbits.TCS = 0;
    T2CONbits.TCKPS = 0b00;
    T2CONbits.TGATE = 0;
    T2CONbits.SIDL = config->run_in_idle ? 0u : 1u;

    TMR2 = 0x00000000UL;
    PR2 = 0xFFFFFFFFUL;

    high_res_timer_clk_hz = config->timer_clk_hz;
    high_res_timer_initialized = true;

    _T2IF = 0;
    _T2IE = 0;
    T2CONbits.ON = 1;

    return NORA_HIGH_RES_TIMER_OK;
#else
    return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
#endif
}

nora_high_res_timer_status_t nora_high_res_timer_deinit(void)
{
    if (!nora_high_res_timer_is_present()) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
    }

#if DSPIC33AK_HIGH_RES_TIMER_PRESENT
    if (!high_res_timer_initialized) {
        return NORA_HIGH_RES_TIMER_ERR_NOT_INITIALIZED;
    }

    _T2IE = 0;
    T2CONbits.ON = 0;
    _T2IF = 0;
    TMR2 = 0;
    PR2 = 0;
    T2CON = 0u;
    high_res_timer_clk_hz = 0u;
    high_res_timer_initialized = false;

    return NORA_HIGH_RES_TIMER_OK;
#else
    return NORA_HIGH_RES_TIMER_ERR_NOT_PRESENT;
#endif
}

bool nora_high_res_timer_is_present(void)
{
#if DSPIC33AK_HIGH_RES_TIMER_PRESENT
    return true;
#else
    return false;
#endif
}

bool nora_high_res_timer_is_initialized(void)
{
    return high_res_timer_initialized;
}

uint32_t nora_high_res_timer_get_count(void)
{
#if DSPIC33AK_HIGH_RES_TIMER_PRESENT
    if (!high_res_timer_initialized) {
        return 0u;
    }

    return TMR2;
#else
    return 0u;
#endif
}

uint32_t nora_high_res_timer_elapsed_count(uint32_t start_count)
{
#if DSPIC33AK_HIGH_RES_TIMER_PRESENT
    if (!high_res_timer_initialized) {
        return 0u;
    }

    return TMR2 - start_count;
#else
    (void)start_count;
    return 0u;
#endif
}

uint32_t nora_high_res_timer_count_to_us(uint32_t count)
{
    return count_to_units(count, 1000000ULL);
}

uint32_t nora_high_res_timer_count_to_us_x10(uint32_t count)
{
    return count_to_units(count, 10000000ULL);
}

uint32_t nora_high_res_timer_elapsed_us(uint32_t start_count)
{
    return nora_high_res_timer_count_to_us(
        nora_high_res_timer_elapsed_count(start_count));
}

uint32_t nora_high_res_timer_elapsed_us_x10(uint32_t start_count)
{
    return nora_high_res_timer_count_to_us_x10(
        nora_high_res_timer_elapsed_count(start_count));
}

static uint32_t count_to_units(uint32_t count, uint64_t units_per_second)
{
    uint64_t converted;

    if (high_res_timer_clk_hz == 0u) {
        return 0u;
    }

    converted = ((uint64_t)count * units_per_second) / high_res_timer_clk_hz;
    if (converted > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)converted;
}
