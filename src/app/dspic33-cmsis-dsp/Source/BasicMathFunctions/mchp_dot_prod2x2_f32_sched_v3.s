;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod2x2_f32_sched_v3 -- A0 schedule, 8-TAP unrolled (DTB /8).
;
; Independent experimental successor to sched_v2. The accumulator order,
; loads, MACs, registers, and result layout are unchanged; only twice as many
; consecutive taps are emitted per loop iteration. blockSize must be a
; multiple of 8 (the active ASRC configuration uses M=32).
;
;   f0=aA0 f3=aA1 f4=aB0 f5=aB1   f1=wA f6=wB f2=c0 f7=c1
;   void mchp_dot_prod2x2_f32_sched_v3(const float32_t *wA, const float32_t *wB,
;                                      const float32_t *c0, const float32_t *c1,
;                                      uint32_t blockSize, float32_t *out4);
;   w0=wA w1=wB w2=c0 w3=c1 w4=blockSize(mult of 8) w5=out4
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2x2_f32_sched_v3
_mchp_dot_prod2x2_f32_sched_v3:

    push.l  fcr
    floatsetup w6

    movc.s  #22, f0
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5

    lsr.l   w4, w4                   ; /2
    lsr.l   w4, w4                   ; /4
    lsr.l   w4, w4                   ; /8  -> 8-tap groups (M mult of 8)
    cp0.l   w4
    bra     z, _dot2x2v3_store

v_dot2x2v3_loop:
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
    ; tap n+4
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+5
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+6
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    ; tap n+7
    mov.l   [w0++], f1
    mov.l   [w2++], f2
    mov.l   [w1++], f6
    mov.l   [w3++], f7
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    DTB     w4, v_dot2x2v3_loop

_dot2x2v3_store:
    mov.l   f0, [w5]
    mov.l   f3, [w5+4]
    mov.l   f4, [w5+8]
    mov.l   f5, [w5+12]

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
