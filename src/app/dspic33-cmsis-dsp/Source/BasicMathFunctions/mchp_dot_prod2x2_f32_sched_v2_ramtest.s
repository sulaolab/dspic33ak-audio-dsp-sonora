;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; RAM-test duplicate of mchp_dot_prod2x2_f32_sched_v2.
; The executable instructions are intentionally identical; only the symbol,
; branch labels, and XC-DSC standard ramfunc section placement differ.
;
;   void mchp_dot_prod2x2_f32_sched_v2_ramtest(
;       const float32_t *wA, const float32_t *wB,
;       const float32_t *c0, const float32_t *c1,
;       uint32_t blockSize, float32_t *out4);
;   w0=wA w1=wB w2=c0 w3=c1 w4=blockSize(mult of 4) w5=out4
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .ramfunc,group,ramfunc
    .align 4

    .global    _mchp_dot_prod2x2_f32_sched_v2_ramtest
_mchp_dot_prod2x2_f32_sched_v2_ramtest:

    push.l  fcr
    floatsetup w6

    movc.s  #22, f0
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5

    lsr.l   w4, w4                   ; /2
    lsr.l   w4, w4                   ; /4  -> 4-tap groups (M mult of 4)
    cp0.l   w4
    bra     z, _dot2x2v2_ramtest_store

v_dot2x2v2_ramtest_loop:
    ; tap n
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+1
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+2
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+3
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    DTB     w4, v_dot2x2v2_ramtest_loop

_dot2x2v2_ramtest_store:
    mov.l   f0, [w5]
    mov.l   f3, [w5+4]
    mov.l   f4, [w5+8]
    mov.l   f5, [w5+12]

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
