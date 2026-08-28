/*
 * nora_high_res_timer_dspic33ak.c
 * --------------------------
 * Free-running 32-bit high-resolution counter. See nora_high_res_timer.h.
 *
 * Two backends, chosen by what the part actually has:
 *
 *   Timer2   dsPIC33AK512MPS512 and the other MPS parts.  TMR2/PR2 in 32-bit
 *            mode, TCS=0 (peripheral clock), 1:1 prescale.
 *   SCCP     dsPIC33AK128MC106 and the rest of the MC family, which have no
 *            Timer2 at all -- only TMR1/PR1 plus SCCP CCP1..CCP4.  One SCCP
 *            instance runs as a 32-bit free-running timer.  This is the same
 *            substitute dsPIC33CK uses for the same reason.
 *
 * Both count once per peripheral-clock period, so the caller's timer_clk_hz
 * contract -- and every microsecond figure derived from it downstream -- is
 * identical either way.  A part with neither reports NOT_PRESENT, and callers
 * then read a zero count as "no instrument" rather than as zero load.
 */

#include "nora_high_res_timer.h"

#include <xc.h>

/* Which SCCP instance the SCCP backend drives.  1..4 exist on every MC part.
 *
 * CCP1 is the default because it is what dsPIC33CK uses, and because it is
 * free on the MC parts: the CCP input-capture HAL that claims CCP1/CCP2 for
 * rate detection on the MPS parts gates itself on the full CCP1..CCP9 map
 * (it tests CCP9CON1) and so compiles out entirely on a 4-instance part.  A
 * build that does need CCP1 for something else moves this timer instead of
 * giving up the measurement. */
#ifndef NORA_HIGH_RES_TIMER_SCCP_INSTANCE
#define NORA_HIGH_RES_TIMER_SCCP_INSTANCE 1
#endif

#if defined(T2CON) && defined(TMR2) && defined(PR2) && defined(_T2IF) && defined(_T2IE)
#define DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2 1
#else
#define DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2 0
#endif

#if !DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2 && \
    defined(CCP1CON1) && defined(CCP1TMR) && defined(CCP1PR)
#define DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP 1
#else
#define DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP 0
#endif

#define DSPIC33AK_HIGH_RES_TIMER_PRESENT        \
    (DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2 || \
     DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP)

#if DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP

/* Build the SFR and interrupt-alias names for the selected instance.  Two
 * levels so NORA_HIGH_RES_TIMER_SCCP_INSTANCE expands before it is pasted. */
#define HRT_SCCP_CAT_(a, b, c) a##b##c
#define HRT_SCCP_CAT(a, b, c) HRT_SCCP_CAT_(a, b, c)
#define HRT_SCCP_REG(suffix) \
    HRT_SCCP_CAT(CCP, NORA_HIGH_RES_TIMER_SCCP_INSTANCE, suffix)
#define HRT_SCCP_IRQ(suffix) \
    HRT_SCCP_CAT(_CCP, NORA_HIGH_RES_TIMER_SCCP_INSTANCE, suffix)

/* CCPxCON1 fields.  dsPIC33AK family reference manual DS70005591C section 27.3
 * ("SCCP/MCCP Control Register 1") -- the same layout the CCP input-capture HAL
 * encodes in nora_ccp_input_capture_dspic33ak_reg.h.  Written as one word so
 * there is no read-modify-write on a half-configured timer. */
#define HRT_SCCP_CON1_MOD_TIMER     (0x0UL << 0)   /* 16/32-bit timer mode     */
#define HRT_SCCP_CON1_CCSEL_TIMER   (0x0UL << 4)   /* timer, not input capture */
#define HRT_SCCP_CON1_T32           (0x1UL << 5)   /* 32-bit counter           */
#define HRT_SCCP_CON1_TMRPS_1_1     (0x0UL << 6)   /* 1:1 prescale             */
#define HRT_SCCP_CON1_CLKSEL_PERIPH (0x0UL << 8)   /* peripheral clock         */
#define HRT_SCCP_CON1_SIDL          (0x1UL << 13)  /* stop in idle             */
#define HRT_SCCP_CON1_ON            (0x1UL << 15)
#define HRT_SCCP_CON1_SYNC_FREE_RUN (0x1FUL << 16) /* no sync source           */

#define HRT_SCCP_CON1_BASE                                 \
    (HRT_SCCP_CON1_MOD_TIMER | HRT_SCCP_CON1_CCSEL_TIMER | \
     HRT_SCCP_CON1_T32 | HRT_SCCP_CON1_TMRPS_1_1 |         \
     HRT_SCCP_CON1_CLKSEL_PERIPH | HRT_SCCP_CON1_SYNC_FREE_RUN)

#endif /* DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP */

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

#if DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2
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
#elif DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP
    HRT_SCCP_REG(CON1) = 0u; /* clears ON before anything else is touched   */
    HRT_SCCP_IRQ(IE) = 0;    /* constant, so a bit-clear rather than an RMW */
    HRT_SCCP_IRQ(IF) = 0;
    high_res_timer_initialized = false;

    HRT_SCCP_REG(CON2) = 0u;
    HRT_SCCP_REG(CON3) = 0u;

    /* Free-running to the top: the counter is read as an absolute time base and
     * differences are taken modulo 2^32, exactly as the Timer2 backend does.
     * At 100 MHz that is 10 ns per count and a 42.9 s wrap. */
    HRT_SCCP_REG(PR) = 0xFFFFFFFFUL;
    HRT_SCCP_REG(TMR) = 0x00000000UL;

    HRT_SCCP_REG(CON1) =
        HRT_SCCP_CON1_BASE | (config->run_in_idle ? 0UL : HRT_SCCP_CON1_SIDL);

    high_res_timer_clk_hz = config->timer_clk_hz;
    high_res_timer_initialized = true;

    HRT_SCCP_IRQ(IF) = 0;
    HRT_SCCP_IRQ(IE) = 0;
    HRT_SCCP_REG(CON1) |= HRT_SCCP_CON1_ON;

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
#endif

#if DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2
    _T2IE = 0;
    T2CONbits.ON = 0;
    _T2IF = 0;
    TMR2 = 0;
    PR2 = 0;
    T2CON = 0u;
    high_res_timer_clk_hz = 0u;
    high_res_timer_initialized = false;

    return NORA_HIGH_RES_TIMER_OK;
#elif DSPIC33AK_HIGH_RES_TIMER_BACKEND_SCCP
    HRT_SCCP_IRQ(IE) = 0;
    HRT_SCCP_REG(CON1) = 0u; /* ON=0, and every field back to its reset value */
    HRT_SCCP_IRQ(IF) = 0;
    HRT_SCCP_REG(TMR) = 0u;
    HRT_SCCP_REG(PR) = 0u;
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

    /* One load of one 32-bit SFR in either backend.  CCPxTMR is a single 32-bit
     * register on this family, so the high/low/high re-read dsPIC33CK needs for
     * its 16-bit timer pair does not apply -- which matters, because this is
     * called from inside the TDM ISR. */
#if DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2
    return TMR2;
#else
    return HRT_SCCP_REG(TMR);
#endif
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

#if DSPIC33AK_HIGH_RES_TIMER_BACKEND_TIMER2
    return TMR2 - start_count;
#else
    return HRT_SCCP_REG(TMR) - start_count;
#endif
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
