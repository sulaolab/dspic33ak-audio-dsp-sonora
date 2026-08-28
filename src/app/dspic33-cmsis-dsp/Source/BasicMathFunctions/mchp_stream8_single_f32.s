;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_stream8_single_f32 -- EIGHT-channel single-coefficient dot.
;
; Performance-ceiling experiment for a stored-phase ASRC. Unlike STREAM8,
; this kernel receives one already-selected coefficient row and performs no
; per-tap c0/c1 interpolation. Per tap: 9 loads and 8 MACs. The caller is
; responsible for choosing a sufficiently dense stored phase.
; Measured on dsPIC33AK512MPS512, O2, 8ch/direction, load mult=1:
;   L128: pull=99.6/99.0 us, combined TDM load ~=74.5%, no misses
;   L256: pull=99.3..100.0/100.0 us, combined TDM load ~=74.7%, no misses
;
;   f0..f7 = channel accumulators, f8/f9 = window temps, f10 = coefficient
;
; void mchp_stream8_single_f32(const float32_t *wbase0, uint32_t strideBytes,
;                              const float32_t *coeff, uint32_t blockSize,
;                              float32_t *out8);
;
; w0=ch0, w6..w12=ch1..ch7, w2=coeff, w3=blockSize, w4=out8
; blockSize must be even. F8..F10 and W8..W12 are saved/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_stream8_single_f32
_mchp_stream8_single_f32:

    push.l  f8
    push.l  f9
    push.l  f10
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  fcr

    add.l   w0, w1, w6
    add.l   w6, w1, w7
    add.l   w7, w1, w8
    add.l   w8, w1, w9
    add.l   w9, w1, w10
    add.l   w10, w1, w11
    add.l   w11, w1, w12
    floatsetup w1

    movc.s  #22, f0
    movc.s  #22, f1
    movc.s  #22, f2
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5
    movc.s  #22, f6
    movc.s  #22, f7

    lsr.l   w3, w3                   ; two taps per iteration
    cp0.l   w3
    bra     z, _stream8s_store

v_stream8s_loop:
    ; tap n
    mov.l   [w2++], f10
    mov.l   [w0++], f8
    mov.l   [w6++], f9
    mac.s   f8, f10, f0
    mov.l   [w7++], f8
    mac.s   f9, f10, f1
    mov.l   [w8++], f9
    mac.s   f8, f10, f2
    mov.l   [w9++], f8
    mac.s   f9, f10, f3
    mov.l   [w10++], f9
    mac.s   f8, f10, f4
    mov.l   [w11++], f8
    mac.s   f9, f10, f5
    mov.l   [w12++], f9
    mac.s   f8, f10, f6
    mac.s   f9, f10, f7

    ; tap n+1
    mov.l   [w2++], f10
    mov.l   [w0++], f8
    mov.l   [w6++], f9
    mac.s   f8, f10, f0
    mov.l   [w7++], f8
    mac.s   f9, f10, f1
    mov.l   [w8++], f9
    mac.s   f8, f10, f2
    mov.l   [w9++], f8
    mac.s   f9, f10, f3
    mov.l   [w10++], f9
    mac.s   f8, f10, f4
    mov.l   [w11++], f8
    mac.s   f9, f10, f5
    mov.l   [w12++], f9
    mac.s   f8, f10, f6
    mac.s   f9, f10, f7
    DTB     w3, v_stream8s_loop

_stream8s_store:
    mov.l   f0, [w4]
    mov.l   f1, [w4+4]
    mov.l   f2, [w4+8]
    mov.l   f3, [w4+12]
    mov.l   f4, [w4+16]
    mov.l   f5, [w4+20]
    mov.l   f6, [w4+24]
    mov.l   f7, [w4+28]

    pop.l   fcr
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
