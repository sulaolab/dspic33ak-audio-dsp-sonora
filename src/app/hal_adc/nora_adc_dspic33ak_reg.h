#ifndef NORA_ADC_DSPIC33AK_REG_H
#define NORA_ADC_DSPIC33AK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer for the dsPIC33AK ADC HAL.
 *
 * Like the GPIO/I2C/UART/SPI HAL register layers, this file intentionally uses
 * plain 32-bit register pointers and bit masks instead of XC-DSC bitfield
 * structures such as AD1CONbits or AD3CH0CON1bits. The goal is to let one
 * readable driver body operate on multiple ADC instances through register
 * tables, while keeping compiler/DFP-specific bitfield details out of the
 * public ADC API.
 *
 * Keep this file small. Add only the registers and bits that are actually used
 * by the ADC HAL as migration progresses.
 */

/*
 * Device identity (HAL-owned adapter). The ADC register set differs by device, so
 * this module maps the toolchain's -mcpu predefined macro to an opaque tag and
 * selects register tables on it. Self-contained: depends only on the compiler macro,
 * never on app config. Add a sibling part (e.g. another MPS5xx variant) by OR-ing it
 * into the matching arm -- the ONE place a part-number change is made here.
 *
 * Tag values are arbitrary; compare with == only (never order / arithmetic). 0 is
 * reserved as "unset" so an unknown device falls through to the NULL register table
 * (same graceful behavior as before -- this HAL does not hard-#error on device).
 */
#define DSPIC33AK_ADC_DEV_AK512   (1)
#define DSPIC33AK_ADC_DEV_AK128   (2)

#if   defined(__dsPIC33AK512MPS512__)
  #define DSPIC33AK_ADC_DEVICE    DSPIC33AK_ADC_DEV_AK512
#elif defined(__dsPIC33AK128MC106__)
  #define DSPIC33AK_ADC_DEVICE    DSPIC33AK_ADC_DEV_AK128
#else
  #define DSPIC33AK_ADC_DEVICE    (0)
#endif

typedef struct {
    volatile uint32_t *CON;
    volatile uint32_t *STAT;
    volatile uint32_t *SWTRG;
    volatile uint32_t *DATAOVR;
    volatile uint32_t *CMPSTAT;
    volatile uint32_t *RSTAT;
} dspic33ak_adc_regs_t;

typedef struct {
    volatile uint32_t *CON1;
    volatile uint32_t *CON2;
    volatile uint32_t *DATA;
    volatile uint32_t *CNT;
    volatile uint32_t *RES;
    uint32_t           positive_input_mask;
    uint8_t            positive_input_pos;
} dspic33ak_adc_channel_regs_t;

/* ADxCON bits / fields used by the current ADC code paths. */
#define DSPIC33AK_ADC_CON_ON              (1UL << 15)  /* ADxCONbits.ON */
#define DSPIC33AK_ADC_CON_ADRDY           (1UL << 31)  /* ADxCONbits.ADRDY */
#define DSPIC33AK_ADC_CON_CALREQ          (1UL << 29)  /* ADxCONbits.CALREQ */
#define DSPIC33AK_ADC_CON_CALRDY          (1UL << 30)  /* ADxCONbits.CALRDY */
#define DSPIC33AK_ADC_CON_ACALEN          (1UL << 28)  /* ADxCONbits.ACALEN */
#define DSPIC33AK_ADC_CON_MODE_POS        (24)
#define DSPIC33AK_ADC_CON_MODE_MASK       (0x3UL << DSPIC33AK_ADC_CON_MODE_POS)
#define DSPIC33AK_ADC_CON_RPTCNT_POS      (18)
#define DSPIC33AK_ADC_CON_RPTCNT_MASK     (0x3FUL << DSPIC33AK_ADC_CON_RPTCNT_POS)
#define DSPIC33AK_ADC_CON_RESET_VALUE     (0x480000UL)

/* ADxSTAT bits. */
#define DSPIC33AK_ADC_STAT_CH0RDY         (1UL << 0)   /* ADxSTATbits.CH0RDY */

/* ADxSWTRG fields. */
#define DSPIC33AK_ADC_SWTRG_CH0TRG_POS    (0)
#define DSPIC33AK_ADC_SWTRG_CH0TRG_MASK   (1UL << DSPIC33AK_ADC_SWTRG_CH0TRG_POS)

/* ADxCHnCON1 fields used by ADC1/3/4/5 CH0 paths. */
#define DSPIC33AK_ADC_CHCON1_TRG1SRC_POS  (0)
#define DSPIC33AK_ADC_CHCON1_TRG1SRC_MASK (0x3FUL << DSPIC33AK_ADC_CHCON1_TRG1SRC_POS)
#define DSPIC33AK_ADC_CHCON1_MODE_POS     (6)
#define DSPIC33AK_ADC_CHCON1_MODE_MASK    (0x3UL << DSPIC33AK_ADC_CHCON1_MODE_POS)
#define DSPIC33AK_ADC_CHCON1_TRG2SRC_POS  (8)
#define DSPIC33AK_ADC_CHCON1_TRG2SRC_MASK (0x3FUL << DSPIC33AK_ADC_CHCON1_TRG2SRC_POS)
#define DSPIC33AK_ADC_CHCON1_ACCNUM_POS   (14)
#define DSPIC33AK_ADC_CHCON1_ACCNUM_MASK  (0x3UL << DSPIC33AK_ADC_CHCON1_ACCNUM_POS)
#define DSPIC33AK_ADC_CHCON1_SAMC_POS     (16)
#define DSPIC33AK_ADC_CHCON1_SAMC_MASK    (0x1FUL << DSPIC33AK_ADC_CHCON1_SAMC_POS)
#define DSPIC33AK_ADC_CHCON1_IRQSEL       (1UL << 21)  /* ADxCHnCON1bits.IRQSEL */
#define DSPIC33AK_ADC_CHCON1_EIEN         (1UL << 22)  /* ADxCHnCON1bits.EIEN */
#define DSPIC33AK_ADC_CHCON1_TRG1POL      (1UL << 23)  /* ADxCHnCON1bits.TRG1POL */
#define DSPIC33AK_ADC_CHCON1_PINSEL_POS   (24)
#define DSPIC33AK_ADC_CHCON1_PINSEL_MASK  (0xFUL << DSPIC33AK_ADC_CHCON1_PINSEL_POS)
#define DSPIC33AK_ADC_CHCON1_NINSEL_POS   (28)
#define DSPIC33AK_ADC_CHCON1_NINSEL_MASK  (0x3UL << DSPIC33AK_ADC_CHCON1_NINSEL_POS)
#define DSPIC33AK_ADC_CHCON1_FRAC         (1UL << 30)  /* ADxCHnCON1bits.FRAC */
#define DSPIC33AK_ADC_CHCON1_DIFF         (1UL << 31)  /* ADxCHnCON1bits.DIFF */

/* ADxCHnCON2 fields used by ADC3/4 audio input setup. */
#define DSPIC33AK_ADC_CHCON2_ADCMPCNT_POS (0)
#define DSPIC33AK_ADC_CHCON2_ADCMPCNT_MASK (0x3FFUL << DSPIC33AK_ADC_CHCON2_ADCMPCNT_POS)
#define DSPIC33AK_ADC_CHCON2_CMPMOD_POS   (12)
#define DSPIC33AK_ADC_CHCON2_CMPMOD_MASK  (0x7UL << DSPIC33AK_ADC_CHCON2_CMPMOD_POS)
#define DSPIC33AK_ADC_CHCON2_CMPCNTMOD    (1UL << 28)  /* ADxCHnCON2bits.CMPCNTMOD */
#define DSPIC33AK_ADC_CHCON2_CMPVAL       (1UL << 29)  /* ADxCHnCON2bits.CMPVAL */
#define DSPIC33AK_ADC_CHCON2_ACCBRST      (1UL << 30)  /* ADxCHnCON2bits.ACCBRST */
#define DSPIC33AK_ADC_CHCON2_ACCRO        (1UL << 31)  /* ADxCHnCON2bits.ACCRO */

static inline void dspic33ak_adc_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void dspic33ak_adc_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline bool dspic33ak_adc_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

static inline void dspic33ak_adc_reg_write_field(
    volatile uint32_t *reg,
    uint32_t mask,
    uint32_t pos,
    uint32_t value)
{
    *reg = (*reg & ~mask) | ((value << pos) & mask);
}

#endif /* NORA_ADC_DSPIC33AK_REG_H */
