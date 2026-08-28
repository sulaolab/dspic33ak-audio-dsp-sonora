;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod2_f32_v2b -- fused DUAL dot, 2-tap unrolled, 4-ACCUMULATOR (V2b).
;
; Diagnostic fork of V2a (mchp_dot_prod2_f32_v2a.s). The ONLY change from V2a is the
; accumulator split: even taps and odd taps accumulate into SEPARATE registers, so
; each of the two sub-filters has two independent accumulator chains. Purpose: test
; whether the 2-accumulator V2a still leaves an FPU accumulator-dependency stall.
; Everything else is identical to V2a -- 2-tap unroll, same DTB count, same load
; order, same MAC count. Only the final reduce adds 2 pairs.
;
; NOTE: even/odd split changes the summation ORDER vs V1/V2a, so the result is NOT
; bit-equivalent (float rounding differs at ~1e-7). Original / V1 / V2a untouched.
;
;   f0 = phase0 (c0) even taps      f4 = phase0 (c0) odd taps
;   f3 = phase1 (c1) even taps      f5 = phase1 (c1) odd taps
;   f1 = window sample              f2 = coefficient
;
;   void mchp_dot_prod2_f32_v2b(const float32_t *w,   // window (contiguous)   w0
;                               const float32_t *c0,  // sub-filter p          w1
;                               const float32_t *c1,  // sub-filter p+1        w2
;                               uint32_t blockSize,   // EVEN                  w3
;                               float32_t *r0,        // *r0 = sum w*c0        w4
;                               float32_t *r1);       // *r1 = sum w*c1        w5
;
; System resources: {w0..w6} used not restored; {f0..f5} used not restored (f4/f5
;                    are caller-saved scratch); FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"     ; floatsetup
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2_f32_v2b
_mchp_dot_prod2_f32_v2b:

    push.l  fcr
    floatsetup w6                    ; w6 = scratch for FCR setup

    movc.s  #22, f0                  ; acc0_even = 0.0
    movc.s  #22, f3                  ; acc1_even = 0.0
    movc.s  #22, f4                  ; acc0_odd  = 0.0
    movc.s  #22, f5                  ; acc1_odd  = 0.0

    lsr.l   w3, w3                   ; w3 = blockSize / 2 (2-tap pairs; M even, no remainder)
    cp0.l   w3
    bra     z, _dot2v2b_store

v_dot2v2b_loop:
    ; --- tap n (EVEN) -> f0, f3 ---
    mov.l   [w0++], f1               ; f1 = w[n]      (load once, both MACs)
    mov.l   [w1++], f2               ; f2 = c0[n]
    mac.s   f1, f2, f0               ; acc0_even += w[n]*c0[n]
    mov.l   [w2++], f2               ; f2 = c1[n]
    mac.s   f1, f2, f3               ; acc1_even += w[n]*c1[n]
    ; --- tap n+1 (ODD) -> f4, f5 ---
    mov.l   [w0++], f1               ; f1 = w[n+1]
    mov.l   [w1++], f2               ; f2 = c0[n+1]
    mac.s   f1, f2, f4               ; acc0_odd += w[n+1]*c0[n+1]   (independent chain)
    mov.l   [w2++], f2               ; f2 = c1[n+1]
    mac.s   f1, f2, f5               ; acc1_odd += w[n+1]*c1[n+1]   (independent chain)
    DTB     w3, v_dot2v2b_loop

_dot2v2b_store:
    add.s   f0, f4, f0               ; phase0 = even + odd
    add.s   f3, f5, f3               ; phase1 = even + odd
    mov.l   f0, [w4]                 ; *r0
    mov.l   f3, [w5]                 ; *r1

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
