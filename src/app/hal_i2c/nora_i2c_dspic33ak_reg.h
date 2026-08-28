#ifndef NORA_I2C_DSPIC33AK_REG_H
#define NORA_I2C_DSPIC33AK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer.
 *
 * This file intentionally uses 32-bit register pointers and bit masks instead
 * of XC-DSC bitfield structures such as I2C1CON1BITS.  The goal is to keep
 * compiler/DFP-specific details away from the public API.
 *
 * Bit positions were checked against:
 *   Microchip.dsPIC33AK-MP_DFP.1.3.185.atpack
 *   xc16/support/dsPIC33A/h/p33AK512MPS512.h
 *
 * Keep this file small.  Add only the bits that are actually used by the
 * readable I2C driver.
 */

/*
 * One interrupt source (event / RX / TX). The dsPIC33AK I2C peripheral has three
 * separate interrupts. The flag (IFSx) and enable (IECx) bits share the same
 * Only data/config registers are described here.  The event/RX/TX interrupt
 * flag and enable bits are NOT: they live in IFSx/IECx, which every peripheral
 * shares, so a pointer+mask read-modify-write could undo a bit another interrupt
 * changed in between.  They are reached instead through the per-instance
 * functions in the device layer, which write the DFP bit aliases (_I2CxIF,
 * _I2CxIE, ...) with literal values so the compiler emits one atomic bit
 * operation.  That also removes the per-device bank knowledge from this code -
 * the old descriptors carried hard-coded IFS2/IFS3 masks valid only on AK512.
 */
typedef struct {
    volatile uint32_t *CON1;
    volatile uint32_t *CON2;
    volatile uint32_t *STAT1;
    volatile uint32_t *STAT2;
    volatile uint32_t *BITO;
    volatile uint32_t *LBRG;
    volatile uint32_t *HBRG;
    volatile uint32_t *TRN;
    volatile uint32_t *RCV;
    volatile uint32_t *ADD;    /* slave own-address (I2CxADD) */
    volatile uint32_t *MSK;    /* slave address mask (I2CxMSK) */
    volatile uint32_t *INTC;   /* interrupt control (I2CxINTC) */
} nora_i2c_regs_t;

/* I2CxCON1 bits */
#define NORA_I2C_CON1_SEN      (1UL << 0)   /* I2CxCON1bits.SEN */
#define NORA_I2C_CON1_RSEN     (1UL << 1)   /* I2CxCON1bits.RSEN */
#define NORA_I2C_CON1_PEN      (1UL << 2)   /* I2CxCON1bits.PEN */
#define NORA_I2C_CON1_RCEN     (1UL << 3)   /* I2CxCON1bits.RCEN */
#define NORA_I2C_CON1_ACKEN    (1UL << 4)   /* I2CxCON1bits.ACKEN */
#define NORA_I2C_CON1_ACKDT    (1UL << 5)   /* I2CxCON1bits.ACKDT */
#define NORA_I2C_CON1_STREN    (1UL << 6)   /* I2CxCON1bits.STREN  (slave clock stretch) */
#define NORA_I2C_CON1_GCEN     (1UL << 7)   /* I2CxCON1bits.GCEN   (general-call enable) */
#define NORA_I2C_CON1_A10M     (1UL << 10)  /* I2CxCON1bits.A10M   (10-bit own address) */
#define NORA_I2C_CON1_STRICT   (1UL << 11)  /* I2CxCON1bits.STRICT */
#define NORA_I2C_CON1_SCLREL   (1UL << 12)  /* I2CxCON1bits.SCLREL (release clock) */
#define NORA_I2C_CON1_ON       (1UL << 15)  /* I2CxCON1bits.ON */
#define NORA_I2C_CON1_DHEN     (1UL << 16)  /* I2CxCON1bits.DHEN   (data hold) */
#define NORA_I2C_CON1_AHEN     (1UL << 17)  /* I2CxCON1bits.AHEN   (address hold) */
#define NORA_I2C_CON1_BOEN     (1UL << 20)  /* I2CxCON1bits.BOEN   (buffer overwrite) */
#define NORA_I2C_CON1_SCIE     (1UL << 21)  /* I2CxCON1bits.SCIE   (START int, client) */
#define NORA_I2C_CON1_PCIE     (1UL << 22)  /* I2CxCON1bits.PCIE   (STOP int, client)  */

/* I2CxCON2 bits */
#define NORA_I2C_CON2_PSZ_1_BYTE (1UL)       /* I2CxCON2bits.PSZ = 1 */
#define NORA_I2C_CON2_BITE     (1UL << 16)  /* I2CxCON2bits.BITE */

/* I2CxSTAT1 bits */
#define NORA_I2C_STAT1_TBF     (1UL << 0)   /* I2CxSTAT1bits.TBF */
#define NORA_I2C_STAT1_RBF     (1UL << 1)   /* I2CxSTAT1bits.RBF */
#define NORA_I2C_STAT1_R_W     (1UL << 2)   /* I2CxSTAT1bits.R_W  (slave: last addr was read) */
#define NORA_I2C_STAT1_S       (1UL << 3)   /* I2CxSTAT1bits.S    (START detected) */
#define NORA_I2C_STAT1_P       (1UL << 4)   /* I2CxSTAT1bits.P    (STOP detected) */
#define NORA_I2C_STAT1_D_A     (1UL << 5)   /* I2CxSTAT1bits.D_A  (0=address, 1=data) */
#define NORA_I2C_STAT1_ADD10   (1UL << 8)   /* I2CxSTAT1bits.ADD10 */
#define NORA_I2C_STAT1_GCSTAT  (1UL << 9)   /* I2CxSTAT1bits.GCSTAT (general call) */
#define NORA_I2C_STAT1_I2COV   (1UL << 6)   /* I2CxSTAT1bits.I2COV */
#define NORA_I2C_STAT1_IWCOL   (1UL << 7)   /* I2CxSTAT1bits.IWCOL */
#define NORA_I2C_STAT1_BCL     (1UL << 10)  /* I2CxSTAT1bits.BCL */
#define NORA_I2C_STAT1_TRSTAT  (1UL << 14)  /* I2CxSTAT1bits.TRSTAT */
#define NORA_I2C_STAT1_ACKSTAT (1UL << 15)  /* I2CxSTAT1bits.ACKSTAT */

/* I2CxINTC bits (interrupt routing). In Client mode the address/data/stop
 * conditions are aggregated into CLIIF and reach the event interrupt I2CxIF
 * only when CLTIE is set; CADDRIE/CDRXIE pull address-match / RBF into CLIIF.
 * RXIE/TXIE instead route RBF/TBF to the separate I2CxRXIF/I2CxTXIF vectors. */
#define NORA_I2C_INTC_CDTXIE   (1UL << 3)   /* client data TX -> int */
#define NORA_I2C_INTC_CDRXIE   (1UL << 4)   /* RBF=1 contributes to CLIIF */
#define NORA_I2C_INTC_TXIE     (1UL << 6)   /* TBF=0 -> I2CxTXIF */
#define NORA_I2C_INTC_RXIE     (1UL << 7)   /* RBF=1 -> I2CxRXIF */
#define NORA_I2C_INTC_CADDRIE  (1UL << 10)  /* address detect -> CLIIF */
#define NORA_I2C_INTC_CLTIE    (1UL << 12)  /* CLIIF -> I2CxIF (client gate) */

/* I2CxSTAT2 bits */
#define NORA_I2C_STAT2_ERR     (1UL << 11)  /* I2CxSTAT2bits.ERR */
#define NORA_I2C_STAT2_CLTIF   (1UL << 12)  /* I2CxSTAT2bits.CLTIF */
#define NORA_I2C_STAT2_HSTIF   (1UL << 13)  /* I2CxSTAT2bits.HSTIF */
#define NORA_I2C_STAT2_STARTE  (1UL << 14)  /* I2CxSTAT2bits.STARTE */
#define NORA_I2C_STAT2_STOPE   (1UL << 15)  /* I2CxSTAT2bits.STOPE */
#define NORA_I2C_STAT2_NACKE   (1UL << 18)  /* I2CxSTAT2bits.NACKE */
#define NORA_I2C_STAT2_BITO    (1UL << 20)  /* I2CxSTAT2bits.BITO */
#define NORA_I2C_STAT2_HSTACT  (1UL << 29)  /* I2CxSTAT2bits.HSTACT */
#define NORA_I2C_STAT2_CLTACT  (1UL << 30)  /* I2CxSTAT2bits.CLTACT */
#define NORA_I2C_STAT2_SSPND   (1UL << 31)  /* I2CxSTAT2bits.SSPND */

/* I2CxBITO field */
#define NORA_I2C_BITO_BITOTMR_MASK  (0x00FFFFFFUL) /* I2CxBITObits.BITOTMR */

static inline void nora_i2c_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void nora_i2c_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline void nora_i2c_reg_write(volatile uint32_t *reg, uint32_t value)
{
    *reg = value;
}

static inline bool nora_i2c_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

static inline void nora_i2c_reg_write_field(
    volatile uint32_t *reg,
    uint32_t mask,
    uint32_t value)
{
    *reg = (*reg & ~mask) | (value & mask);
}

#endif /* NORA_I2C_DSPIC33AK_REG_H */
