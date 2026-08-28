;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_dot_prod4x2_f32 -- coefficient-shared FOUR-channel dual dot (load-reduction B0/wide4).
;
; Extends V3/A0 (2-channel) to FOUR channels: one call dots four channel windows
; (A,B,C,D) against the two shared sub-filter coefficients (c0,c1), loading c0[k]/c1[k]
; ONCE per tap for all four channels. Per tap: 6 loads (wA,c0,wB,c1,wC,wD) for 8 MACs,
; vs 8 loads for two 2x2 calls -> 2 fewer coefficient loads/tap, and HALF the call /
; FCR-setup / accumulator-init / store fixed cost at 4ch. Load order follows the A0
; lesson (operands are loaded >=2 instructions before their MAC). 2-tap unrolled.
;
; The four channel windows are contiguous rows of ch[ASRC_CH][PHYS]; the caller passes
; the base (=&ch[c][wbase]) and the per-channel byte stride, so only ONE window pointer
; arg is needed and B/C/D are derived once at entry.
;
; Add order per output is sequential (tap 0,1,2,...) into a fixed accumulator, exactly
; like V1/V2a/V3/A0 -> BIT-EQUIVALENT (channel MACs are merely interleaved). blockSize
; assumed EVEN (M=32). Original / V1..V3 / A0 untouched.
;
;   f0=A*c0 f1=A*c1 f2=B*c0 f3=B*c1 f4=C*c0 f5=C*c1 f6=D*c0 f7=D*c1   (8 accumulators)
;   f8=c0   f9=c1   f10=window tmp (A then C)   f11=window tmp (B then D)
;
;   void mchp_dot_prod4x2_f32(const float32_t *wA,  // &ch[c][wbase]         w0
;                             uint32_t strideBytes, // &ch[c+1]-&ch[c] bytes w1
;                             const float32_t *c0,  //                       w2
;                             const float32_t *c1,  //                       w3
;                             uint32_t blockSize,   // EVEN                  w4
;                             float32_t *out8);     // out8[0..7]            w5
;
; System resources: {w0..w8} used (w8 saved/restored); {f0..f11} used (f8..f11
;                   saved/restored; f0..f7 scratch); FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_dot_prod4x2_f32
_mchp_dot_prod4x2_f32:

    ; --- save callee-saved registers used here ---
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  w8
    push.l  fcr

    ; --- derive wB/wC/wD from base + stride (once) ---
    add.l   w0, w1, w6               ; w6 = wB = base + stride
    add.l   w6, w1, w7               ; w7 = wC
    add.l   w7, w1, w8               ; w8 = wD
    floatsetup w1                    ; w1 now free -> FCR setup scratch

    ; --- zero the 8 accumulators ---
    movc.s  #22, f0
    movc.s  #22, f1
    movc.s  #22, f2
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5
    movc.s  #22, f6
    movc.s  #22, f7

    lsr.l   w4, w4                   ; w4 = blockSize / 2 (2-tap pairs; M even)
    cp0.l   w4
    bra     z, _dot4x2_store

    ; w0=wA w6=wB w7=wC w8=wD   w2=c0 w3=c1   w4=pairs   w5=out8
v_dot4x2_loop:
    ; ---- tap n ----
    mov.l   [w0++], f10              ; wA
    mov.l   [w2++], f8               ; c0   (loaded ONCE for all 4 channels)
    mov.l   [w6++], f11              ; wB
    mov.l   [w3++], f9               ; c1   (loaded ONCE for all 4 channels)
    mac.s   f10, f8, f0              ; A*c0
    mac.s   f11, f8, f2              ; B*c0
    mac.s   f10, f9, f1              ; A*c1
    mov.l   [w7++], f10              ; wC  (reuse f10; A done)
    mac.s   f11, f9, f3              ; B*c1
    mov.l   [w8++], f11              ; wD  (reuse f11; B done)
    mac.s   f10, f8, f4              ; C*c0
    mac.s   f10, f9, f5              ; C*c1
    mac.s   f11, f8, f6              ; D*c0
    mac.s   f11, f9, f7              ; D*c1
    ; ---- tap n+1 ----
    mov.l   [w0++], f10
    mov.l   [w2++], f8
    mov.l   [w6++], f11
    mov.l   [w3++], f9
    mac.s   f10, f8, f0
    mac.s   f11, f8, f2
    mac.s   f10, f9, f1
    mov.l   [w7++], f10
    mac.s   f11, f9, f3
    mov.l   [w8++], f11
    mac.s   f10, f8, f4
    mac.s   f10, f9, f5
    mac.s   f11, f8, f6
    mac.s   f11, f9, f7
    DTB     w4, v_dot4x2_loop

_dot4x2_store:
    mov.l   f0, [w5]                 ; out8[0] = A*c0
    mov.l   f1, [w5+4]               ; out8[1] = A*c1
    mov.l   f2, [w5+8]               ; out8[2] = B*c0
    mov.l   f3, [w5+12]              ; out8[3] = B*c1
    mov.l   f4, [w5+16]              ; out8[4] = C*c0
    mov.l   f5, [w5+20]              ; out8[5] = C*c1
    mov.l   f6, [w5+24]              ; out8[6] = D*c0
    mov.l   f7, [w5+28]              ; out8[7] = D*c1

    pop.l   fcr
    pop.l   w8
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
