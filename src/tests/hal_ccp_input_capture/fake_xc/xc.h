// SPDX-FileCopyrightText: 2026 SulaoLab
// SPDX-License-Identifier: MIT-0

#ifndef TEST_FAKE_XC_H
#define TEST_FAKE_XC_H

#include <stdint.h>

#define DECLARE_CCP(n)                     \
    extern volatile uint32_t CCP##n##CON1; \
    extern volatile uint32_t CCP##n##CON2; \
    extern volatile uint32_t CCP##n##STAT; \
    extern volatile uint32_t CCP##n##PR;   \
    extern volatile uint32_t CCP##n##BUF

DECLARE_CCP(1);
DECLARE_CCP(2);
DECLARE_CCP(3);
DECLARE_CCP(4);
DECLARE_CCP(5);
DECLARE_CCP(6);
DECLARE_CCP(7);
DECLARE_CCP(8);
DECLARE_CCP(9);

/*
 * The real DFP emits every SFR as a self-referencing macro ALONGSIDE the extern:
 *
 *     #define CCP9CON1 CCP9CON1
 *     extern volatile uint32_t CCP9CON1 __attribute__((__sfr__));
 *
 * (verified in p33AK512MPS512.h). That macro is what makes `defined(CCP9CON1)`
 * answer "this part has a CCP9", and the backend's capability test is written
 * against exactly that -- a DFP fact test rather than a compiler part-name test.
 * A part without the peripheral, e.g. p33AK128MC106.h, has neither the macro nor
 * the extern, so the test correctly selects the unsupported implementation.
 *
 * This fake had only the externs. `defined(CCP9CON1)` was therefore false here and
 * ONLY here, so the host build silently took the unsupported path and
 * nora_ccp_icap_configure() could not return OK -- test_validation.c:90. The
 * firmware was never affected; the fake was simply not imitating the DFP closely
 * enough on the one axis the code under test reads.
 *
 * A macro cannot be defined from inside another macro's body, which is why these
 * are spelled out rather than folded into DECLARE_CCP above.
 */
#define CCP1CON1 CCP1CON1
#define CCP2CON1 CCP2CON1
#define CCP3CON1 CCP3CON1
#define CCP4CON1 CCP4CON1
#define CCP5CON1 CCP5CON1
#define CCP6CON1 CCP6CON1
#define CCP7CON1 CCP7CON1
#define CCP8CON1 CCP8CON1
#define CCP9CON1 CCP9CON1

/*
 * IFSx / IECx are modelled as a word plus a single-bit overlay, because the code
 * under test reaches them BOTH ways: the priority path reads and writes whole
 * words (IPCx below stays a plain uint32_t for that reason), while the enable and
 * flag path writes one bit through a DFP bit alias -- see the _CCPnIE / _CCPnIF
 * block further down. The union is what makes those two views the same storage
 * here, which on silicon is the linker's job.
 *
 * The overlay assumes bitfields are allocated from the least significant bit up.
 * The C standard does not guarantee that, and a reversed allocation would put
 * every alias on the wrong bit while every test still passed, so
 * test_validation.c checks each alias against its documented mask before it tests
 * anything else.
 */
typedef union
{
    volatile uint32_t word;
    volatile struct
    {
        unsigned int b0 : 1;
        unsigned int b1 : 1;
        unsigned int b2 : 1;
        unsigned int b3 : 1;
        unsigned int b4 : 1;
        unsigned int b5 : 1;
        unsigned int b6 : 1;
        unsigned int b7 : 1;
        unsigned int b8 : 1;
        unsigned int rest : 23;
    } bit;
} fake_xc_irq_reg_t;

_Static_assert(sizeof(fake_xc_irq_reg_t) == sizeof(uint32_t),
               "the bit overlay must not grow the register");

extern fake_xc_irq_reg_t fake_xc_ifs1;
extern fake_xc_irq_reg_t fake_xc_ifs3;
extern fake_xc_irq_reg_t fake_xc_ifs4;
extern fake_xc_irq_reg_t fake_xc_iec1;
extern fake_xc_irq_reg_t fake_xc_iec3;
extern fake_xc_irq_reg_t fake_xc_iec4;

/* The SFR spelling the code under test uses: still a plain 32-bit lvalue. */
#define IFS1 fake_xc_ifs1.word
#define IFS3 fake_xc_ifs3.word
#define IFS4 fake_xc_ifs4.word
#define IEC1 fake_xc_iec1.word
#define IEC3 fake_xc_iec3.word
#define IEC4 fake_xc_iec4.word
extern volatile uint32_t IPC6;
extern volatile uint32_t IPC7;
extern volatile uint32_t IPC15;
extern volatile uint32_t IPC16;

#define _IFS1_CCP1IF_MASK (1UL << 0)
#define _IFS1_CCP2IF_MASK (1UL << 1)
#define _IFS1_CCP3IF_MASK (1UL << 2)
#define _IFS1_CCP4IF_MASK (1UL << 3)
#define _IFS3_CCP5IF_MASK (1UL << 4)
#define _IFS4_CCP6IF_MASK (1UL << 5)
#define _IFS4_CCP7IF_MASK (1UL << 6)
#define _IFS4_CCP8IF_MASK (1UL << 7)
#define _IFS4_CCP9IF_MASK (1UL << 8)

#define _IEC1_CCP1IE_MASK (1UL << 0)
#define _IEC1_CCP2IE_MASK (1UL << 1)
#define _IEC1_CCP3IE_MASK (1UL << 2)
#define _IEC1_CCP4IE_MASK (1UL << 3)
#define _IEC3_CCP5IE_MASK (1UL << 4)
#define _IEC4_CCP6IE_MASK (1UL << 5)
#define _IEC4_CCP7IE_MASK (1UL << 6)
#define _IEC4_CCP8IE_MASK (1UL << 7)
#define _IEC4_CCP9IE_MASK (1UL << 8)

/*
 * The DFP's CPU IRQ enable / flag BIT ALIASES.
 *
 * On silicon each of these is its own single-bit object that the linker places at
 * a bit address inside IECx / IFSx, so the backend can write `_CCP1IE = 1;` with
 * no read-modify-write and no bank knowledge -- which is why the HAL no longer
 * carries a per-device IEC/IFS pointer-and-mask table. A host compiler has
 * neither bit-addressable storage nor that linker step, so here the alias is the
 * overlay bit at the position the corresponding _IECx_CCPnIE_MASK above
 * documents, and writing it really does move that masked bit of the word.
 *
 * The real DFP pairs each alias with a self-referencing macro so that a
 * `defined(_CCP1IE)` capability guard answers correctly (the same trick as the
 * CCPnCON1 block above). Here the alias IS a macro, so such a guard already
 * answers correctly without the extra line.
 */
#define _CCP1IF fake_xc_ifs1.bit.b0
#define _CCP2IF fake_xc_ifs1.bit.b1
#define _CCP3IF fake_xc_ifs1.bit.b2
#define _CCP4IF fake_xc_ifs1.bit.b3
#define _CCP5IF fake_xc_ifs3.bit.b4
#define _CCP6IF fake_xc_ifs4.bit.b5
#define _CCP7IF fake_xc_ifs4.bit.b6
#define _CCP8IF fake_xc_ifs4.bit.b7
#define _CCP9IF fake_xc_ifs4.bit.b8

#define _CCP1IE fake_xc_iec1.bit.b0
#define _CCP2IE fake_xc_iec1.bit.b1
#define _CCP3IE fake_xc_iec1.bit.b2
#define _CCP4IE fake_xc_iec1.bit.b3
#define _CCP5IE fake_xc_iec3.bit.b4
#define _CCP6IE fake_xc_iec4.bit.b5
#define _CCP7IE fake_xc_iec4.bit.b6
#define _CCP8IE fake_xc_iec4.bit.b7
#define _CCP9IE fake_xc_iec4.bit.b8

#define _IPC6_CCP1IP_MASK      (7UL << 0)
#define _IPC6_CCP1IP_POSITION  (0u)
#define _IPC6_CCP2IP_MASK      (7UL << 4)
#define _IPC6_CCP2IP_POSITION  (4u)
#define _IPC7_CCP3IP_MASK      (7UL << 0)
#define _IPC7_CCP3IP_POSITION  (0u)
#define _IPC7_CCP4IP_MASK      (7UL << 4)
#define _IPC7_CCP4IP_POSITION  (4u)
#define _IPC15_CCP5IP_MASK     (7UL << 0)
#define _IPC15_CCP5IP_POSITION (0u)
#define _IPC16_CCP6IP_MASK     (7UL << 0)
#define _IPC16_CCP6IP_POSITION (0u)
#define _IPC16_CCP7IP_MASK     (7UL << 4)
#define _IPC16_CCP7IP_POSITION (4u)
#define _IPC16_CCP8IP_MASK     (7UL << 8)
#define _IPC16_CCP8IP_POSITION (8u)
#define _IPC16_CCP9IP_MASK     (7UL << 12)
#define _IPC16_CCP9IP_POSITION (12u)

#endif
