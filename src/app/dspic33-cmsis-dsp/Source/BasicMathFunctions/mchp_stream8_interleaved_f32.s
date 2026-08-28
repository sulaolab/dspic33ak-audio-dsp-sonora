;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_stream8_interleaved_f32 -- STREAM8 BASE math over TILE8 history.
;
; The input window is frame-major within one 8-channel tile:
;
;   xbase = &tile[tile_index][window_start][0]
;   memory = tap0 ch0..ch7, tap1 ch0..ch7, ...
;
; This matches the kernel's tap-major/lane-inner access order. One input
; pointer replaces the eight channel-row pointers used by mchp_stream8_f32.
; Coefficient construction, per-channel MAC order, and outputs are otherwise
; identical to STREAM8 BASE.
;
; HW O2, real 8ch/direction, mult=1 matched A/B:
;   CH_MAJOR: pull=117.2/115.0 us, combined TDM ~=83.8%, miss=0
;   TILE8:    pull=116.4/116.5 us, combined TDM ~=84.6%, miss=0
; TILE8 is neutral in summed pull time and ~0.8 percentage point worse in the
; end-to-end TDM load. It remains opt-in; CH_MAJOR is the safe default.
;
; void mchp_stream8_interleaved_f32(const float32_t *xbase,
;                                   const float32_t *c0,
;                                   const float32_t *c1,
;                                   uint32_t blockSize,
;                                   float32_t *out8,
;                                   float32_t wb);
;
; w0=xbase, w1=c0, w2=c1, w3=blockSize, w4=out8, f0=wb
; blockSize must be even. F8..F11 and FCR are saved/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_stream8_interleaved_f32
_mchp_stream8_interleaved_f32:

    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  fcr

    mov.s   f0, f11                  ; wb before f0 becomes accumulator 0
    floatsetup w5

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
    bra     z, _stream8i_store

v_stream8i_loop:
    ; tap n: ce = c0 + wb*(c1-c0)
    mov.l   [w1++], f8
    mov.l   [w2++], f9
    sub.s   f9, f8, f9
    mov.s   f8, f10
    mac.s   f9, f11, f10
    ; contiguous lanes ch0..ch7
    mov.l   [w0++], f8
    mov.l   [w0++], f9
    mac.s   f8, f10, f0
    mov.l   [w0++], f8
    mac.s   f9, f10, f1
    mov.l   [w0++], f9
    mac.s   f8, f10, f2
    mov.l   [w0++], f8
    mac.s   f9, f10, f3
    mov.l   [w0++], f9
    mac.s   f8, f10, f4
    mov.l   [w0++], f8
    mac.s   f9, f10, f5
    mov.l   [w0++], f9
    mac.s   f8, f10, f6
    mac.s   f9, f10, f7

    ; tap n+1
    mov.l   [w1++], f8
    mov.l   [w2++], f9
    sub.s   f9, f8, f9
    mov.s   f8, f10
    mac.s   f9, f11, f10
    mov.l   [w0++], f8
    mov.l   [w0++], f9
    mac.s   f8, f10, f0
    mov.l   [w0++], f8
    mac.s   f9, f10, f1
    mov.l   [w0++], f9
    mac.s   f8, f10, f2
    mov.l   [w0++], f8
    mac.s   f9, f10, f3
    mov.l   [w0++], f9
    mac.s   f8, f10, f4
    mov.l   [w0++], f8
    mac.s   f9, f10, f5
    mov.l   [w0++], f9
    mac.s   f8, f10, f6
    mac.s   f9, f10, f7
    DTB     w3, v_stream8i_loop

_stream8i_store:
    mov.l   f0, [w4]
    mov.l   f1, [w4+4]
    mov.l   f2, [w4+8]
    mov.l   f3, [w4+12]
    mov.l   f4, [w4+16]
    mov.l   f5, [w4+20]
    mov.l   f6, [w4+24]
    mov.l   f7, [w4+28]

    pop.l   fcr
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
