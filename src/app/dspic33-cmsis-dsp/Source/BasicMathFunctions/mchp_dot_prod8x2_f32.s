;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod8x2_f32 -- coefficient-shared EIGHT-channel dual dot.
;
; Experimental bit-equivalent wide8 successor to mchp_dot_prod4x2_f32. One
; call dots eight channel windows against c0 and c1, loading each coefficient
; once per tap. Per tap: 10 loads and 16 MACs, versus 12 loads and 16 MACs for
; two 4x2 calls. Each output accumulator preserves sequential tap add order.
;
; This is an exact-math alternative to STREAM8. STREAM8 blends c0/c1 per tap
; and uses one accumulator per channel; this kernel retains two accumulators
; per channel and leaves the final phase blend to the caller.
; HW O2 8ch: pull=132.5/130.5 us, combined TDM=92.7%, miss=0. It improves
; DUAL4X but is slower than STREAM8 BASE (117.1/115.0 us), so remains opt-in.
;
; f0/f1=ch0 c0/c1 ... f14/f15=ch7 c0/c1
; f16=c0, f17=c1, f18/f19=window temporaries
;
; void mchp_dot_prod8x2_f32(const float32_t *w0, uint32_t strideBytes,
;                           const float32_t *c0, const float32_t *c1,
;                           uint32_t blockSize, float32_t *out16);
;
; w0=ch0, w6..w12=ch1..ch7, w2=c0, w3=c1, w4=blockSize, w5=out16
; blockSize must be even. F8..F19 and W8..W12 are saved/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod8x2_f32
_mchp_dot_prod8x2_f32:

    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  fcr

    ; Derive the remaining channel-row pointers once.
    add.l   w0, w1, w6
    add.l   w6, w1, w7
    add.l   w7, w1, w8
    add.l   w8, w1, w9
    add.l   w9, w1, w10
    add.l   w10, w1, w11
    add.l   w11, w1, w12
    floatsetup w1

    ; Zero 16 accumulators: two sub-filter results per channel.
    movc.s  #22, f0
    movc.s  #22, f1
    movc.s  #22, f2
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5
    movc.s  #22, f6
    movc.s  #22, f7
    movc.s  #22, f8
    movc.s  #22, f9
    movc.s  #22, f10
    movc.s  #22, f11
    movc.s  #22, f12
    movc.s  #22, f13
    movc.s  #22, f14
    movc.s  #22, f15

    lsr.l   w4, w4                   ; two taps per iteration
    cp0.l   w4
    bra     z, _dot8x2_store

v_dot8x2_loop:
    ; tap n
    mov.l   [w0++], f18
    mov.l   [w2++], f16
    mov.l   [w6++], f19
    mov.l   [w3++], f17
    mac.s   f18, f16, f0
    mac.s   f19, f16, f2
    mac.s   f18, f17, f1
    mov.l   [w7++], f18
    mac.s   f19, f17, f3
    mov.l   [w8++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f5
    mov.l   [w9++], f18
    mac.s   f19, f16, f6
    mac.s   f19, f17, f7
    mov.l   [w10++], f19
    mac.s   f18, f16, f8
    mac.s   f18, f17, f9
    mov.l   [w11++], f18
    mac.s   f19, f16, f10
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f12
    mac.s   f18, f17, f13
    mac.s   f19, f16, f14
    mac.s   f19, f17, f15

    ; tap n+1
    mov.l   [w0++], f18
    mov.l   [w2++], f16
    mov.l   [w6++], f19
    mov.l   [w3++], f17
    mac.s   f18, f16, f0
    mac.s   f19, f16, f2
    mac.s   f18, f17, f1
    mov.l   [w7++], f18
    mac.s   f19, f17, f3
    mov.l   [w8++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f5
    mov.l   [w9++], f18
    mac.s   f19, f16, f6
    mac.s   f19, f17, f7
    mov.l   [w10++], f19
    mac.s   f18, f16, f8
    mac.s   f18, f17, f9
    mov.l   [w11++], f18
    mac.s   f19, f16, f10
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f12
    mac.s   f18, f17, f13
    mac.s   f19, f16, f14
    mac.s   f19, f17, f15
    DTB     w4, v_dot8x2_loop

_dot8x2_store:
    mov.l   f0, [w5++]
    mov.l   f1, [w5++]
    mov.l   f2, [w5++]
    mov.l   f3, [w5++]
    mov.l   f4, [w5++]
    mov.l   f5, [w5++]
    mov.l   f6, [w5++]
    mov.l   f7, [w5++]
    mov.l   f8, [w5++]
    mov.l   f9, [w5++]
    mov.l   f10, [w5++]
    mov.l   f11, [w5++]
    mov.l   f12, [w5++]
    mov.l   f13, [w5++]
    mov.l   f14, [w5++]
    mov.l   f15, [w5++]

    pop.l   fcr
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    pop.l   f19
    pop.l   f18
    pop.l   f17
    pop.l   f16
    pop.l   f15
    pop.l   f14
    pop.l   f13
    pop.l   f12
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
