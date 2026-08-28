/**
 * @file    nora_canfd_dspic33ak_reg.h
 * @brief   dsPIC33AK CAN FD HAL - register pointer table, bit definitions and
 *          message-object layout. Register-level only; never included by the
 *          public header. Only this file and nora_canfd_device_dspic33ak.c know the
 *          raw MCU register names.
 *
 * Bit positions verified against DFP p33AK512MPS512.h (dsPIC33AK-MP_DFP 1.2.135)
 * and datasheet DS70005591 (CAN FD chapter 14).
 */
#ifndef NORA_CANFD_REG_H
#define NORA_CANFD_REG_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* Register pointer table (one per instance, filled by the device map).  */
/* Only the registers the HAL touches in phase 1 are mapped; interrupt    */
/* descriptors are added with the ISR phase.                              */
/* ===================================================================== */
typedef struct {
    volatile uint32_t *CON;       /* CxCON     - mode/control            */
    volatile uint32_t *NBTCFG;    /* CxNBTCFG  - nominal bit timing      */
    volatile uint32_t *DBTCFG;    /* CxDBTCFG  - data bit timing         */
    volatile uint32_t *TDC;       /* CxTDC     - transmitter delay comp  */
    volatile uint32_t *FIFOBA;    /* CxFIFOBA  - message RAM base addr   */
    volatile uint32_t *TXQCON;    /* CxTXQCON  - TX queue control        */
    volatile uint32_t *TXQSTA;    /* CxTXQSTA  - TX queue status         */
    volatile uint32_t *TXQUA;     /* CxTXQUA   - TX queue user address   */
    volatile uint32_t *FIFOCON1;  /* CxFIFOCON1- FIFO1 control (RX)      */
    volatile uint32_t *FIFOSTA1;  /* CxFIFOSTA1- FIFO1 status            */
    volatile uint32_t *FIFOUA1;   /* CxFIFOUA1 - FIFO1 user address      */
    volatile uint32_t *FLTCON0;   /* CxFLTCON0 - filter 0..3 control     */
    volatile uint32_t *FLTOBJ0;   /* CxFLTOBJ0 - filter 0 object         */
    volatile uint32_t *MASK0;     /* CxMASK0   - filter 0 mask           */
    volatile uint32_t *INT;       /* CxINT     - interrupt flags/enables */
    volatile uint32_t *TREC;      /* CxTREC    - TX/RX error counters    */
} nora_canfd_regs_t;

/* ===================================================================== */
/* CxCON                                                                 */
/* ===================================================================== */
#define NORA_CANFD_CON_ISOCRCEN  (1UL << 5)
#define NORA_CANFD_CON_PXEDIS    (1UL << 6)
#define NORA_CANFD_CON_CLKSEL    (1UL << 7)   /* 0=CAN clk-gen(CLKGEN10), 1=sysclk */
#define NORA_CANFD_CON_CANBUSY   (1UL << 11)
#define NORA_CANFD_CON_BRSDIS    (1UL << 12)  /* 1=disable bit-rate switching */
#define NORA_CANFD_CON_ON        (1UL << 15)
#define NORA_CANFD_CON_STEF      (1UL << 19)  /* 1=store TX events in TEF */
#define NORA_CANFD_CON_TXQEN     (1UL << 20)  /* 1=enable TX queue */
#define NORA_CANFD_CON_OPMOD_POS  21u         /* OPMOD[23:21] read-only mode */
#define NORA_CANFD_CON_OPMOD_MASK (0x7UL << 21)
#define NORA_CANFD_CON_REQOP_POS  24u         /* REQOP[26:24] requested mode */
#define NORA_CANFD_CON_REQOP_MASK (0x7UL << 24)
#define NORA_CANFD_CON_ABAT      (1UL << 27)  /* abort all transmissions */

/* OPMOD/REQOP 3-bit mode codes */
#define NORA_CANFD_OPMODE_NORMAL_FD       0x0u
#define NORA_CANFD_OPMODE_DISABLE         0x1u
#define NORA_CANFD_OPMODE_INTERNAL_LOOP   0x2u
#define NORA_CANFD_OPMODE_LISTEN_ONLY     0x3u
#define NORA_CANFD_OPMODE_CONFIG          0x4u
#define NORA_CANFD_OPMODE_EXTERNAL_LOOP   0x5u
#define NORA_CANFD_OPMODE_NORMAL_CLASSIC  0x6u
#define NORA_CANFD_OPMODE_RESTRICTED      0x7u

/* ===================================================================== */
/* CxNBTCFG (nominal) / CxDBTCFG (data) bit-timing field positions.       */
/* Field values are (actual - 1).                                         */
/* ===================================================================== */
#define NORA_CANFD_NBTCFG_SJW_POS    0u    /* [6:0]   */
#define NORA_CANFD_NBTCFG_TSEG2_POS  8u    /* [14:8]  */
#define NORA_CANFD_NBTCFG_TSEG1_POS  16u   /* [23:16] */
#define NORA_CANFD_NBTCFG_BRP_POS    24u   /* [31:24] */

#define NORA_CANFD_DBTCFG_SJW_POS    0u    /* [3:0]   */
#define NORA_CANFD_DBTCFG_TSEG2_POS  8u    /* [11:8]  */
#define NORA_CANFD_DBTCFG_TSEG1_POS  16u   /* [20:16] */
#define NORA_CANFD_DBTCFG_BRP_POS    24u   /* [31:24] */

/* CxTDC */
#define NORA_CANFD_TDC_TDCO_POS      8u    /* [14:8] transmitter delay comp offset */
#define NORA_CANFD_TDC_TDCMOD_POS    16u   /* [17:16] 0=off,1=manual,2=auto */
#define NORA_CANFD_TDC_TDCMOD_AUTO   0x2u

/* ===================================================================== */
/* CxTXQCON / CxFIFOCONn (shared layout) and status flags                 */
/* ===================================================================== */
#define NORA_CANFD_FIFOCON_RXTSEN   (1UL << 5)  /* RX timestamp enable */
#define NORA_CANFD_FIFOCON_TXEN     (1UL << 7)  /* 1=TX FIFO, 0=RX FIFO */
#define NORA_CANFD_FIFOCON_UINC     (1UL << 8)  /* increment head/tail */
#define NORA_CANFD_FIFOCON_TXREQ    (1UL << 9)  /* request transmit */
#define NORA_CANFD_FIFOCON_FRESET   (1UL << 10) /* reset FIFO */
#define NORA_CANFD_FIFOCON_FSIZE_POS  24u       /* [28:24] objects-1 */
#define NORA_CANFD_FIFOCON_PLSIZE_POS 29u       /* [31:29] payload code */

/* CxTXQSTA / CxFIFOSTAn flags */
#define NORA_CANFD_FIFOSTA_TFNRFNIF (1UL << 0)  /* TX:not-full / RX:not-empty */
#define NORA_CANFD_FIFOSTA_RXOVIF   (1UL << 3)  /* RX FIFO overflow */

/* Interrupt-enable bits used only by the optional ISR layer (nora_canfd_isr_dspic33ak.c). */
#define NORA_CANFD_FIFOCON_TFNRFNIE (1UL << 0)  /* RX FIFO not-empty IRQ enable */
#define NORA_CANFD_FIFOCON_RXOVIE   (1UL << 3)  /* RX FIFO overflow IRQ enable */
#define NORA_CANFD_TXQCON_TXQEIE    (1UL << 2)  /* TXQ-empty IRQ enable */
#define NORA_CANFD_TXQSTA_TXQEIF    (1UL << 2)  /* TXQ-empty flag (all sent) */

/* ===================================================================== */
/* CxFLTCON0 (filters 0..3) and CxFLTOBJ0 / CxMASK0                        */
/* ===================================================================== */
#define NORA_CANFD_FLTCON0_F0BP_POS   0u    /* filter 0 dest FIFO [4:0] */
#define NORA_CANFD_FLTCON0_FLTEN0     (1UL << 7)

#define NORA_CANFD_FLTOBJ_EXIDE      (1UL << 30)  /* match extended id */
#define NORA_CANFD_MASK_MIDE         (1UL << 30)  /* match IDE bit */

/* CxINT flags (low 16) + enables (high 16). Used by the optional ISR layer. */
#define NORA_CANFD_INT_TXIF          (1UL << 0)   /* TX FIFO/TXQ event roll-up */
#define NORA_CANFD_INT_RXIF          (1UL << 1)   /* RX FIFO event roll-up */
#define NORA_CANFD_INT_RXOVIF        (1UL << 11)  /* RX overflow roll-up */
#define NORA_CANFD_INT_CERRIF        (1UL << 13)  /* CAN bus error */
#define NORA_CANFD_INT_IVMIF         (1UL << 15)  /* invalid message */
#define NORA_CANFD_INT_TXIE          (1UL << 16)
#define NORA_CANFD_INT_RXIE          (1UL << 17)
#define NORA_CANFD_INT_RXOVIE        (1UL << 27)
#define NORA_CANFD_INT_CERRIE        (1UL << 29)
#define NORA_CANFD_INT_IVMIE         (1UL << 31)
/* Writable flag bits the ISR clears (TXIF/RXIF/RXOVIF are read-only roll-ups). */
#define NORA_CANFD_INT_CLR_MASK      (NORA_CANFD_INT_CERRIF | NORA_CANFD_INT_IVMIF)

/* CxTREC error counters / fault state (read-only). */
#define NORA_CANFD_TREC_RERRCNT_POS   0u    /* [7:0]  receive error count  */
#define NORA_CANFD_TREC_RERRCNT_MASK  0xFFUL
#define NORA_CANFD_TREC_TERRCNT_POS   8u    /* [15:8] transmit error count */
#define NORA_CANFD_TREC_TERRCNT_MASK  0xFF00UL
#define NORA_CANFD_TREC_EWARN        (1UL << 16)  /* error warning */
#define NORA_CANFD_TREC_RXWARN       (1UL << 17)
#define NORA_CANFD_TREC_TXWARN       (1UL << 18)
#define NORA_CANFD_TREC_RXBP         (1UL << 19)  /* rx error-passive */
#define NORA_CANFD_TREC_TXBP         (1UL << 20)  /* tx error-passive */
#define NORA_CANFD_TREC_TXBO         (1UL << 21)  /* bus-off */

/* PLSIZE payload code -> bytes */
#define NORA_CANFD_PLSIZE_8    0x0u
#define NORA_CANFD_PLSIZE_12   0x1u
#define NORA_CANFD_PLSIZE_16   0x2u
#define NORA_CANFD_PLSIZE_20   0x3u
#define NORA_CANFD_PLSIZE_24   0x4u
#define NORA_CANFD_PLSIZE_32   0x5u
#define NORA_CANFD_PLSIZE_48   0x6u
#define NORA_CANFD_PLSIZE_64   0x7u

/* ===================================================================== */
/* Message object layout in CAN message RAM (see DS70005591 14.x).        */
/* word0 = ID (T0/R0), word1 = CTRL (T1/R1), data starts at byte 8        */
/* (timestamp disabled). Helpers below build/parse the header words.      */
/* ===================================================================== */
#define NORA_CANFD_OBJ_HEADER_BYTES  8u   /* T0 + T1, no timestamp */

/* ID word (T0/R0) */
#define NORA_CANFD_OBJID_SID_POS    0u    /* [10:0]  standard id */
#define NORA_CANFD_OBJID_SID_MASK   0x7FFUL
#define NORA_CANFD_OBJID_EID_POS    11u   /* [28:11] extended id low part */
#define NORA_CANFD_OBJID_EID_MASK   0x3FFFFUL
#define NORA_CANFD_OBJID_SID11_POS  29u

/* CTRL word (T1/R1) */
#define NORA_CANFD_OBJCTRL_DLC_POS  0u    /* [3:0] */
#define NORA_CANFD_OBJCTRL_DLC_MASK 0xFUL
#define NORA_CANFD_OBJCTRL_IDE      (1UL << 4)  /* extended id */
#define NORA_CANFD_OBJCTRL_RTR      (1UL << 5)
#define NORA_CANFD_OBJCTRL_BRS      (1UL << 6)  /* bit-rate switch */
#define NORA_CANFD_OBJCTRL_FDF      (1UL << 7)  /* CAN FD frame */
#define NORA_CANFD_OBJCTRL_ESI      (1UL << 8)

/* ===================================================================== */
/* Inline register helpers (mirror the I2C HAL helper set).               */
/* ===================================================================== */
static inline void nora_canfd_reg_set(volatile uint32_t *reg, uint32_t mask)
{
    *reg |= mask;
}

static inline void nora_canfd_reg_clear(volatile uint32_t *reg, uint32_t mask)
{
    *reg &= ~mask;
}

static inline bool nora_canfd_reg_is_set(volatile uint32_t *reg, uint32_t mask)
{
    return (*reg & mask) != 0UL;
}

static inline void nora_canfd_reg_write_field(volatile uint32_t *reg,
                                                   uint32_t mask, uint32_t value)
{
    *reg = (*reg & ~mask) | (value & mask);
}

/** Decode a 4-bit DLC code into a byte count (CAN FD encoding). */
static inline uint8_t nora_canfd_dlc_to_len(uint8_t dlc)
{
    static const uint8_t k[16] = {0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64};
    return k[dlc & 0x0Fu];
}

/** Encode a byte count into the smallest DLC code that holds it. */
static inline uint8_t nora_canfd_len_to_dlc(uint8_t len)
{
    if (len <= 8u)  return len;
    if (len <= 12u) return 9u;
    if (len <= 16u) return 10u;
    if (len <= 20u) return 11u;
    if (len <= 24u) return 12u;
    if (len <= 32u) return 13u;
    if (len <= 48u) return 14u;
    return 15u; /* 64 */
}

#endif /* NORA_CANFD_REG_H */
