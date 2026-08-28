/* Provenance: written from DS70005591 ch.18 (ITC), ch.30 (ADC) and the DFP SFR
 * header p33AK512MPS512.h only. No vendor touch-library source, header or
 * binary was consulted; the data sheet's Example 18-4 was read for facts about
 * the silicon and not transcribed.
 */

/* Integrated Touch Controller register layer -- dsPIC33AK backend.
 *
 * Backend-private. This is the only file in the touch HAL that spells out
 * register array bases, command-word bit positions and field shifts, so a
 * reader comparing the driver against DS70005591 ch.18 has one place to look
 * and the detection layer above it (nora_touch.c) has none. No caller includes
 * this header.
 *
 * ITC is the data sheet's own name for the block (DS70005591 ch.18), not an
 * abbreviation invented here; the SFRs carry the same prefix.
 */

#ifndef NORA_ITC_DSPIC33AK_REG_H
#define NORA_ITC_DSPIC33AK_REG_H

#include <stdint.h>

#include <xc.h>

#include "nora_itc_internal.h"   /* NORA_ITC_LIST_COUNT, used by the per-list types */

/* ---------------------------------------------------------------------------
 * Register access
 *
 * The ITC's per-record, per-result and per-command registers are contiguous
 * 32-bit words in the SFR map (ITCREC0..15 at 4-byte stride, ITCRES0..31,
 * SDATACMD0..15 at 0x7C3000 + 4n, SMATHCMD0..15 at 0x7C3040 + 4n), so they are
 * addressed as arrays from the first element rather than through a 32-entry
 * pointer table. The bitfield unions from the DFP header are still used for
 * every *named* field, so no field position is spelled out here twice.
 * ------------------------------------------------------------------------- */
#define ITC_REC_ARRAY      ((volatile uint32_t *)&ITCREC0)
#define ITC_RES_ARRAY      ((volatile uint32_t *)&ITCRES0)
#define ITC_DATACMD_ARRAY  ((volatile uint32_t *)&SDATACMD0)
#define ITC_MATHCMD_ARRAY  ((volatile uint32_t *)&SMATHCMD0)

/* Two records share one ITCRECx word: record 2n in the low half, 2n+1 in the
 * high half. The halves are field-identical, so one shift decides which. */
#define ITC_REC_WORD(rec)        ((rec) >> 1u)
#define ITC_REC_SHIFT(rec)       (((rec) & 1u) ? 16u : 0u)
#define ITC_REC_PIN_MASK         (0x7Fu)      /* PINn[6:0]                  */
#define ITC_REC_ACCDONE_BIT      (8u)         /* ACCDONEn                   */
#define ITC_REC_GRDA_SHIFT       (11u)        /* GRDAn[1:0]                 */
#define ITC_REC_GRDB_SHIFT       (13u)        /* GRDBn[1:0]                 */
#define ITC_REC_HALF_MASK        (0x0000FFFFu)

/* ---------------------------------------------------------------------------
 * Command-word encodings (DS70005591 §18.3 register tables).
 * ------------------------------------------------------------------------- */

/* SDATACMDx */
#define DCMD_PC0_SHIFT      (0u)    /* the record's electrode pin           */
#define DCMD_PCA_SHIFT      (2u)    /* guard A pin                          */
#define DCMD_PCB_SHIFT      (4u)    /* guard B pin                          */
#define DCMD_PCC_SHIFT      (6u)    /* the CVD capacitor node               */
/* All four PC* fields share one encoding. Note that "leave it alone" is 3
 * (tri-state) and 0 hands the pin back to TRIS/LAT — so break-before-make is
 * written as 3, and writing 0 by mistake gives the port's idle level, not a
 * float. This is the single easiest field in the peripheral to get backwards. */
#define DCMD_PC_PORT        (0u)    /* release to TRIS/LAT                  */
#define DCMD_PC_LOW         (1u)
#define DCMD_PC_HIGH        (2u)
#define DCMD_PC_TRISTATE    (3u)

#define DCMD_BAL            (1uL << 12u)   /* connect for charge balance    */
#define DCMD_CONV           (1uL << 13u)   /* start an ADC 5 conversion     */
#define DCMD_CHRG           (1uL << 14u)   /* charge the CVD capacitor      */
#define DCMD_DISCHRG        (1uL << 15u)   /* discharge the CVD capacitor   */
#define DCMD_SECOND         (1uL << 18u)   /* this command is in sample B   */
#define DCMD_MSTART         (1uL << 22u)   /* run the math sequence now     */
#define DCMD_END            (1uL << 31u)   /* last command of the sequence  */

#define DCMD_LOOP_SHIFT     (27u)          /* LOOP[3:0]: what to wait for   */
#define DCMD_LOOP_NONE      (0u)
#define DCMD_LOOP_TMRA      (1u)
#define DCMD_LOOP_TMRB      (2u)
#define DCMD_LOOP_SAMC      (3u)
#define DCMD_LOOP_ADC_EOC   (4u)
#define DCMD_LOOP_TMRC      (9u)
#define DCMD_LOOP_TMRD      (10u)

/* SMATHCMDx */
#define MCMD_AIN_SHIFT      (0u)
#define MCMD_AIN_ZERO       (0u)
#define MCMD_AIN_ACCA       (1u)
#define MCMD_AIN_ADC5       (3u)
#define MCMD_BIN_SHIFT      (2u)
#define MCMD_BIN_RESULT     (0u)    /* the record's own ITCRESx             */
#define MCMD_BIN_ACCB       (1u)
#define MCMD_BIN_ADC5       (3u)
#define MCMD_F_SHIFT        (4u)
#define MCMD_F_A            (0u)
#define MCMD_F_B            (1u)
#define MCMD_F_B_PLUS_A     (2u)
#define MCMD_F_B_MINUS_A    (3u)
#define MCMD_WM_SHIFT       (6u)
#define MCMD_WM_ALWAYS      (0u)
#define MCMD_WM_NEVER       (2u)
#define MCMD_ACCA           (1uL << 11u)
#define MCMD_ACCB           (1uL << 12u)
#define MCMD_END            (1uL << 23u)

/* SDATAMAP / SMATHMAP: four groups, one per block of four command registers. */
#define MAP_GROUP_STRIDE    (8u)    /* bits per group in either MAP register */
#define DATAMAP_SPLIT_SHIFT (0u)
#define DATAMAP_SEQ_SHIFT   (3u)
#define MATHMAP_SPLIT_SHIFT (0u)
#define MATHMAP_CMP_BIT     (2u)
#define MATHMAP_ACC_BIT     (3u)
#define MATHMAP_SECOND_BIT  (4u)
#define MATHMAP_SEQ_SHIFT   (5u)

/* SPLITn selects how wide the sequence starting at this group is. The reset
 * value of every group in both MAP registers is 2, which is the 8-command
 * granularity the data sheet's own configuration uses: groups 0+1 form one
 * sequence and groups 2+3 form another. Writing 0 makes all sixteen commands a
 * single sequence, which is how the first math attempt lost the ability to
 * select a second sequence at all (see itc_program_math_sequence). It is
 * written explicitly rather than left at reset so the value is visible here. */
#define MAP_SPLIT_8CMD      (2u)

/* Sequence numbers are 4..7 (0..3 name the four command blocks themselves).
 * This HAL uses one acquisition sequence and one math sequence, both 4. */
#define ITC_SEQ_ACQUISITION (4u)
#define ITC_SEQ_MATH        (4u)

/* ITCLSxCON */
#define ITC_MODE_LIST_NO_IRQ    (4u)
#define ITC_MODE_LIST_IRQ_END   (5u)
#define ITC_SSRC_SOFTWARE       (0u)
#define ITC_SSRC_INTERNAL_TIMER (7u)

/* ADC 5 */
#define AD5_TRG1SRC_ITC     (26u)   /* trigger 26 = ITC (DS70005591 §18.4)  */
#define AD5_PINSEL_CVD      (5u)    /* channel 5 takes the CVD mux          */
#define AD5_NINSEL_VSS      (0u)
#define AD5_READY_TIMEOUT   (1000000UL)
#define ITC_READY_TIMEOUT   (1000000UL)

/* TMRA..TMRD are 8-bit; TMRPR is 16-bit. */
#define ITC_TIMER_MAX       (255u)
#define ITC_TMRPR_MAX       (65535u)

/* ---------------------------------------------------------------------------
 * Per-list register set
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t *CON;
    volatile uint32_t *SEQ;
    volatile uint32_t *TMR;
    volatile uint32_t *STAT;
    volatile uint32_t *MUL;
} itc_list_regs_t;

/* ITCLSxSTAT field positions (the DFP header gives one struct per list, so the
 * positions are named once here and used through the pointer table above). */
#define LSSTAT_NEXT_MASK    (0x3Fu)
#define LSSTAT_INT_BIT      (21u)
#define LSSTAT_BUSY_BIT     (24u)
#define LSSTAT_TACT_BIT     (25u)

/* ITCLSxCON field positions, same reason. */
#define LSCON_RECCNT_SHIFT  (0u)
#define LSCON_RECCNT_MASK   (0x3Fu)
#define LSCON_SSRC_SHIFT    (8u)
#define LSCON_SSRC_MASK     (0x1Fu)
#define LSCON_TRGCLR_BIT    (13u)
#define LSCON_SAMP_BIT      (14u)
#define LSCON_TRGEN_BIT     (15u)
#define LSCON_MODE_SHIFT    (29u)
#define LSCON_MODE_MASK     (0x7u)

/* ITCLSxSEQ field positions. */
#define LSSEQ_ACCCNT_SHIFT  (0u)
#define LSSEQ_ACCEN_BIT     (7u)
#define LSSEQ_MATHSEQ_SHIFT (17u)
#define LSSEQ_DATASEQ_SHIFT (21u)
#define LSSEQ_CVDCAP_SHIFT  (28u)

#endif /* NORA_ITC_DSPIC33AK_REG_H */
