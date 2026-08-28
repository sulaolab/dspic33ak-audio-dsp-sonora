;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod2_f32_v2a -- fused DUAL dot, 2-TAP UNROLLED (load-reduction V2a).
;
; Fork of mchp_dot_prod2_f32.s (V1). Identical result and accumulation ORDER
; (acc0/acc1 still summed n = 0,1,2,... sequentially -- bit-equivalent to V1), but
; the loop body processes TWO taps per iteration so the DTB / loop overhead is
; halved. Still exactly TWO accumulators (f0, f3). No 4-accumulator even/odd split
; (that is V2b, which would change the add order). No remainder handling: blockSize
; is assumed EVEN (ASRC_POLY_M = 32). Original mchp_dot_prod_f32.s and V1 untouched.
;
;   void mchp_dot_prod2_f32_v2a(const float32_t *w,   // window (contiguous)   w0
;                               const float32_t *c0,  // sub-filter p          w1
;                               const float32_t *c1,  // sub-filter p+1        w2
;                               uint32_t blockSize,   // EVEN                  w3
;                               float32_t *r0,        // *r0 = sum w*c0        w4
;                               float32_t *r1);       // *r1 = sum w*c1        w5
;
; System resources: {w0..w6} used not restored; {f0..f3} used not restored;
;                    FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"     ; floatsetup
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2_f32_v2a
_mchp_dot_prod2_f32_v2a:

    push.l  fcr
    floatsetup w6                    ; w6 = scratch for FCR setup

    movc.s  #22, f0                  ; acc0 = 0.0
    movc.s  #22, f3                  ; acc1 = 0.0

    lsr.l   w3, w3                   ; w3 = blockSize / 2 (2-tap pairs; M even, no remainder)
    cp0.l   w3
    bra     z, _dot2v2a_store

v_dot2v2a_loop:
    ; --- tap n ---
    mov.l   [w0++], f1               ; f1 = w[n]      (window: load once, feeds both MACs)
    mov.l   [w1++], f2               ; f2 = c0[n]
    mac.s   f1, f2, f0               ; acc0 += w[n]*c0[n]
    mov.l   [w2++], f2               ; f2 = c1[n]
    mac.s   f1, f2, f3               ; acc1 += w[n]*c1[n]
    ; --- tap n+1 ---
    mov.l   [w0++], f1               ; f1 = w[n+1]
    mov.l   [w1++], f2               ; f2 = c0[n+1]
    mac.s   f1, f2, f0               ; acc0 += w[n+1]*c0[n+1]
    mov.l   [w2++], f2               ; f2 = c1[n+1]
    mac.s   f1, f2, f3               ; acc1 += w[n+1]*c1[n+1]
    DTB     w3, v_dot2v2a_loop       ; one DTB per 2 taps (halved loop overhead)

_dot2v2a_store:
    mov.l   f0, [w4]                 ; *r0 = acc0
    mov.l   f3, [w5]                 ; *r1 = acc1

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
