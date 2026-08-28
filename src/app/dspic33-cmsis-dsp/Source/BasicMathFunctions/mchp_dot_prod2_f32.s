;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR          *
;   STATUTORY, APPLY TO THIS SOFTWARE.                                       *
;*****************************************************************************
;
; mchp_dot_prod2_f32 -- DUAL single-precision dot product (load-reduction V1).
;
; Local fork of mchp_dot_prod_f32.s for the polyphase ASRC: it dots ONE window
; against TWO coefficient vectors (the adjacent sub-filters c0, c1) in a single
; call. Vs calling mchp_dot_prod_f32 twice this:
;   - loads each window sample ONCE and feeds both MACs (one fewer load/tap),
;   - uses TWO INDEPENDENT accumulators (f0, f3) so the two MACs are not on one
;     dependency chain (fills the FPU pipeline better),
;   - pays the FCR save/setup + call/return ONCE instead of twice.
; V1 does NOT unroll -- that is a later variant. Original mchp_dot_prod_f32.s is
; left untouched (see [internal] asrc_load_reduction.md).
;
;   void mchp_dot_prod2_f32(const float32_t *w,    // window (contiguous)   w0
;                           const float32_t *c0,   // sub-filter p          w1
;                           const float32_t *c1,   // sub-filter p+1        w2
;                           uint32_t blockSize,    //                       w3
;                           float32_t *r0,         // *r0 = sum w*c0        w4
;                           float32_t *r1);        // *r1 = sum w*c1        w5
;
; System resources: {w0..w6} used not restored; {f0..f3} used not restored;
;                    FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"     ; floatsetup
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2_f32
_mchp_dot_prod2_f32:

    push.l  fcr
    floatsetup w6                    ; w6 = scratch for FCR setup

    movc.s  #22, f0                  ; acc0 = 0.0
    movc.s  #22, f3                  ; acc1 = 0.0

    cp0.l   w3                       ; blockSize == 0 ? -> store zeros
    bra     z, _dot2_store

v_dot2_loop:
    mov.l   [w0++], f1               ; f1 = w[n]      (window: load ONCE)
    mov.l   [w1++], f2               ; f2 = c0[n]
    mac.s   f1, f2, f0               ; acc0 += w[n]*c0[n]
    mov.l   [w2++], f2               ; f2 = c1[n]
    mac.s   f1, f2, f3               ; acc1 += w[n]*c1[n]   (independent accumulator)
    DTB     w3, v_dot2_loop

_dot2_store:
    mov.l   f0, [w4]                 ; *r0 = acc0
    mov.l   f3, [w5]                 ; *r1 = acc1

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
