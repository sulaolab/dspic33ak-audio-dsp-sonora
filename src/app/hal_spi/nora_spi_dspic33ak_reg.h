#ifndef NORA_SPI_DSPIC33AK_REG_H
#define NORA_SPI_DSPIC33AK_REG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Internal register helper layer for the blocking 8-bit SPI HAL.
 *
 * Like nora_dma_dspic33ak_reg.h / nora_i2c_dspic33ak_reg.h / nora_spi_i2s_tdm_dspic33ak_reg.h, this
 * intentionally uses plain 32-bit register pointers and bit masks instead of the
 * XC-DSC bitfield structures (SPIxCON1bits / SPIxSTATbits).  The goal is to let
 * one driver body drive any of SPI1..SPI4 through a table of register pointers,
 * while keeping compiler/DFP-specific bitfield details out of the readable code.
 *
 * Bit positions were checked against:
 *   Microchip dsPIC33AK-MP_DFP 1.3.185  p33AK512MPS512.h
 *     (tagSPI1CON1BITS / tagSPI1STATBITS)
 * SPIxCON1 / SPIxSTAT / SPIxBRG / SPIxBUF are uniform 32-bit SFRs across all
 * SPI instances on this device, which is what makes the table-driven HAL valid.
 *
 * Keep this file small.  Add only the bits actually used by the HAL.
 */

/* ---- SPIxCON1 single-bit fields (subset used by the blocking master HAL) ---- */
#define DSPIC33AK_SPI_CON1_MCLKEN   (1UL << 2)    /* SPIxCON1bits.MCLKEN  */
#define DSPIC33AK_SPI_CON1_DISSCK   (1UL << 3)    /* SPIxCON1bits.DISSCK  */
#define DSPIC33AK_SPI_CON1_DISSDI   (1UL << 4)    /* SPIxCON1bits.DISSDI  */
#define DSPIC33AK_SPI_CON1_MSTEN    (1UL << 5)    /* SPIxCON1bits.MSTEN   */
#define DSPIC33AK_SPI_CON1_CKP      (1UL << 6)    /* SPIxCON1bits.CKP  (= CPOL)        */
#define DSPIC33AK_SPI_CON1_CKE      (1UL << 8)    /* SPIxCON1bits.CKE  (= inverse CPHA) */
#define DSPIC33AK_SPI_CON1_MODE16   (1UL << 10)   /* SPIxCON1bits.MODE16  */
#define DSPIC33AK_SPI_CON1_MODE32   (1UL << 11)   /* SPIxCON1bits.MODE32  */
#define DSPIC33AK_SPI_CON1_DISSDO   (1UL << 12)   /* SPIxCON1bits.DISSDO  */
#define DSPIC33AK_SPI_CON1_ON       (1UL << 15)   /* SPIxCON1bits.ON      */
#define DSPIC33AK_SPI_CON1_FRMEN    (1UL << 23)   /* SPIxCON1bits.FRMEN   */
#define DSPIC33AK_SPI_CON1_IGNTUR   (1UL << 28)   /* SPIxCON1bits.IGNTUR  */
#define DSPIC33AK_SPI_CON1_IGNROV   (1UL << 29)   /* SPIxCON1bits.IGNROV  */
#define DSPIC33AK_SPI_CON1_AUDEN    (1UL << 31)   /* SPIxCON1bits.AUDEN   */

/* ---- SPIxSTAT single-bit fields ---- */
#define DSPIC33AK_SPI_STAT_SPIRBF   (1UL << 0)    /* SPIxSTATbits.SPIRBF  (RX buffer full)   */
#define DSPIC33AK_SPI_STAT_SPITBF   (1UL << 1)    /* SPIxSTATbits.SPITBF  (TX buffer full)   */
#define DSPIC33AK_SPI_STAT_SPIROV   (1UL << 6)    /* SPIxSTATbits.SPIROV  (RX overflow)      */
#define DSPIC33AK_SPI_STAT_SPIBUSY  (1UL << 11)   /* SPIxSTATbits.SPIBUSY (shifter busy)     */

/* ---- SPIxBRG ---- */
#define DSPIC33AK_SPI_BRG_MAX       (0x1FFFUL)    /* SPIxBRG is a 13-bit field */

/* ---- Minimal generic 32-bit SFR access helpers ---- */
static inline void dspic33ak_spi_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void dspic33ak_spi_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline bool dspic33ak_spi_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return ((*reg & mask) != 0u);
}

#endif /* NORA_SPI_DSPIC33AK_REG_H */
