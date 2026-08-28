;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod2x2_f32 -- coefficient-shared TWO-channel dual dot (load-reduction V3).
;
; Motivation: V2b showed the 2-accumulator dual-dot is NOT accumulator-stalled -- the
; remaining cost is the load/issue side. Across channels the two sub-filter coefficient
; vectors (c0, c1) are IDENTICAL (same read phase), yet the per-channel dual-dot reloads
; them for every channel. This kernel processes TWO channel windows (wA, wB) together and
; loads c0[k]/c1[k] ONCE per tap, feeding both channels' MACs. Per 2 taps it does 8 loads
; (wA,wB,c0,c1 x2) for 8 MACs, vs 12 loads for two separate dual-dots -- 1/3 fewer loads.
;
; Same 2-tap unroll and 2-accumulator-per-output style as V2a (4 output accumulators here,
; one per channel x sub-filter -- NOT an even/odd split, so no reduce). Add order per output
; is sequential like V1/V2a (bit-equivalent per output). blockSize assumed EVEN (M=32).
; Original / V1 / V2a / V2b untouched.
;
;   f0 = aA0 (chA*c0)   f3 = aA1 (chA*c1)   f4 = aB0 (chB*c0)   f5 = aB1 (chB*c1)
;   f1 = wA sample      f6 = wB sample      f2 = c0 coeff       f7 = c1 coeff
;
;   void mchp_dot_prod2x2_f32(const float32_t *wA,  //                        w0
;                             const float32_t *wB,  //                        w1
;                             const float32_t *c0,  // sub-filter p           w2
;                             const float32_t *c1,  // sub-filter p+1         w3
;                             uint32_t blockSize,   // EVEN                   w4
;                             float32_t *out4);     // out4[0..3]=aA0,aA1,aB0,aB1  w5
;
; System resources: {w0..w6} used not restored; {f0..f7} used not restored (all scratch);
;                    FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"     ; floatsetup
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2x2_f32
_mchp_dot_prod2x2_f32:

    push.l  fcr
    floatsetup w6                    ; w6 = scratch for FCR setup

    movc.s  #22, f0                  ; aA0 = 0.0
    movc.s  #22, f3                  ; aA1 = 0.0
    movc.s  #22, f4                  ; aB0 = 0.0
    movc.s  #22, f5                  ; aB1 = 0.0

    lsr.l   w4, w4                   ; w4 = blockSize / 2 (2-tap pairs; M even, no remainder)
    cp0.l   w4
    bra     z, _dot2x2_store

v_dot2x2_loop:
    ; --- tap n ---
    mov.l   [w0++], f1               ; f1 = wA[n]
    mov.l   [w1++], f6               ; f6 = wB[n]
    mov.l   [w2++], f2               ; f2 = c0[n]     (loaded ONCE, both channels)
    mac.s   f1, f2, f0               ; aA0 += wA*c0
    mac.s   f6, f2, f4               ; aB0 += wB*c0
    mov.l   [w3++], f7               ; f7 = c1[n]     (loaded ONCE, both channels)
    mac.s   f1, f7, f3               ; aA1 += wA*c1
    mac.s   f6, f7, f5               ; aB1 += wB*c1
    ; --- tap n+1 ---
    mov.l   [w0++], f1               ; f1 = wA[n+1]
    mov.l   [w1++], f6               ; f6 = wB[n+1]
    mov.l   [w2++], f2               ; f2 = c0[n+1]
    mac.s   f1, f2, f0               ; aA0 += wA*c0
    mac.s   f6, f2, f4               ; aB0 += wB*c0
    mov.l   [w3++], f7               ; f7 = c1[n+1]
    mac.s   f1, f7, f3               ; aA1 += wA*c1
    mac.s   f6, f7, f5               ; aB1 += wB*c1
    DTB     w4, v_dot2x2_loop        ; one DTB per 2 taps

_dot2x2_store:
    mov.l   f0, [w5]                 ; out4[0] = aA0
    mov.l   f3, [w5+4]               ; out4[1] = aA1
    mov.l   f4, [w5+8]               ; out4[2] = aB0
    mov.l   f5, [w5+12]              ; out4[3] = aB1

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
