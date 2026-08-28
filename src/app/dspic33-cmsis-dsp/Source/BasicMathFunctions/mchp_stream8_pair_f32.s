;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_stream8_pair_f32 -- two-output fused STREAM8 over channel-major rings.
;
; The two output windows must be adjacent:
;
;   y0[c] = sum(k=0..M-1) x[c][base+k]   * ce0[k]
;   y1[c] = sum(k=0..M-1) x[c][base+k+1] * ce1[k]
;
; Endpoint frames base+0 and base+M feed one output. Each of the M-1 interior
; frames is loaded once and MACed into both output accumulator sets. For M=32,
; window loads fall from 2*32*8=512 to 33*8=264 while MAC count and coefficient
; interpolation remain identical to two STREAM8 BASE calls.
;
; void mchp_stream8_pair_f32(const float32_t *wbase0,
;                            uint32_t strideBytes,
;                            const float32_t *c00,
;                            const float32_t *c01,
;                            uint32_t blockSize,
;                            float32_t *out16,
;                            const float32_t *c10,
;                            const float32_t *c11,
;                            float32_t wb0,
;                            float32_t wb1);
;
; w0=ch0, w1=stride, w2=c00, w3=c01, w4=M, w5=out16,
; w6=c10, w7=c11, f0=wb0, f1=wb1.
; Output is out16[0..7]=y0 and out16[8..15]=y1.
; F8..F21, W8..W14, and FCR are saved/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_stream8_pair_f32
_mchp_stream8_pair_f32:

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
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    mov.s   f0, f20                  ; wb0 before f0 becomes y0 accumulator 0
    mov.s   f1, f21                  ; wb1 before f1 becomes y0 accumulator 1

    add.l   w0, w1, w8               ; ch1
    add.l   w8, w1, w9               ; ch2
    add.l   w9, w1, w10              ; ch3
    add.l   w10, w1, w11             ; ch4
    add.l   w11, w1, w12             ; ch5
    add.l   w12, w1, w13             ; ch6
    add.l   w13, w1, w14             ; ch7
    floatsetup w1

    ; f0..f7 = y0 accumulators, f8..f15 = y1 accumulators
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

    ; Endpoint base+0: y0 only, ce0[0].
    mov.l   [w2++], f18
    mov.l   [w3++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f16
    mac.s   f19, f20, f16

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f19, f16, f7

    sub.l   w4, #1, w4               ; M-1 shared interior frames
    cp0.l   w4
    bra     z, _stream8pair_tail

v_stream8pair_loop:
    ; ce0[j], j=1..M-1
    mov.l   [w2++], f18
    mov.l   [w3++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f16
    mac.s   f19, f20, f16
    ; ce1[j-1], j=1..M-1
    mov.l   [w6++], f18
    mov.l   [w7++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f17
    mac.s   f19, f21, f17

    ; One shared input frame, two output MACs per channel.
    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15
    DTB     w4, v_stream8pair_loop

_stream8pair_tail:
    ; Endpoint base+M: y1 only, ce1[M-1].
    mov.l   [w6++], f18
    mov.l   [w7++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f17
    mac.s   f19, f21, f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f17, f14
    mac.s   f19, f17, f15

    mov.l   f0,  [w5]
    mov.l   f1,  [w5+4]
    mov.l   f2,  [w5+8]
    mov.l   f3,  [w5+12]
    mov.l   f4,  [w5+16]
    mov.l   f5,  [w5+20]
    mov.l   f6,  [w5+24]
    mov.l   f7,  [w5+28]
    mov.l   f8,  [w5+32]
    mov.l   f9,  [w5+36]
    mov.l   f10, [w5+40]
    mov.l   f11, [w5+44]
    mov.l   f12, [w5+48]
    mov.l   f13, [w5+52]
    mov.l   f14, [w5+56]
    mov.l   f15, [w5+60]

    pop.l   fcr
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    pop.l   f21
    pop.l   f20
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
