;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod2x2_f32_sched_v1 -- load-scheduling experiment A0 over V3.
;
; IDENTICAL to mchp_dot_prod2x2_f32.s in every count -- same instructions, same
; loads, same MACs, same registers, same DTB count, same accumulator order, same
; FP add order (BIT-EQUIVALENT). The ONLY change: within each tap the four loads
; are issued FIRST, then the four MACs, so every load-use distance is >= 2
; independent instructions (V3 had c0/c1 loaded immediately before their MAC =
; distance 1). Pure test of whether a load->MAC hidden interlock stall exists on
; this 33A FPU: if this is faster, the stall is real; if not, V3 is bound by the
; number of load instructions / issue bandwidth, not by load-use latency.
; V3 (mchp_dot_prod2x2_f32.s) is left untouched.
;
;   f0 = aA0   f3 = aA1   f4 = aB0   f5 = aB1   f1 = wA   f6 = wB   f2 = c0   f7 = c1
;
;   void mchp_dot_prod2x2_f32_sched_v1(const float32_t *wA, const float32_t *wB,
;                                      const float32_t *c0, const float32_t *c1,
;                                      uint32_t blockSize, float32_t *out4);
;   w0=wA w1=wB w2=c0 w3=c1 w4=blockSize(EVEN) w5=out4
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod2x2_f32_sched_v1
_mchp_dot_prod2x2_f32_sched_v1:

    push.l  fcr
    floatsetup w6

    movc.s  #22, f0                  ; aA0 = 0.0
    movc.s  #22, f3                  ; aA1 = 0.0
    movc.s  #22, f4                  ; aB0 = 0.0
    movc.s  #22, f5                  ; aB1 = 0.0

    lsr.l   w4, w4                   ; w4 = blockSize / 2 (2-tap pairs; M even)
    cp0.l   w4
    bra     z, _dot2x2s_store

v_dot2x2s_loop:
    ; --- tap n : all 4 loads first, then 4 MACs (max load-use distance) ---
    mov.l   [w0++], f1               ; wA[n]
    mov.l   [w2++], f2               ; c0[n]
    mov.l   [w1++], f6               ; wB[n]
    mov.l   [w3++], f7               ; c1[n]
    mac.s   f1, f2, f0               ; aA0 += wA*c0
    mac.s   f6, f2, f4               ; aB0 += wB*c0
    mac.s   f1, f7, f3               ; aA1 += wA*c1
    mac.s   f6, f7, f5               ; aB1 += wB*c1
    ; --- tap n+1 ---
    mov.l   [w0++], f1               ; wA[n+1]
    mov.l   [w2++], f2               ; c0[n+1]
    mov.l   [w1++], f6               ; wB[n+1]
    mov.l   [w3++], f7               ; c1[n+1]
    mac.s   f1, f2, f0
    mac.s   f6, f2, f4
    mac.s   f1, f7, f3
    mac.s   f6, f7, f5
    DTB     w4, v_dot2x2s_loop

_dot2x2s_store:
    mov.l   f0, [w5]                 ; out4[0] = aA0
    mov.l   f3, [w5+4]               ; out4[1] = aA1
    mov.l   f4, [w5+8]               ; out4[2] = aB0
    mov.l   f5, [w5+12]              ; out4[3] = aB1

    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
