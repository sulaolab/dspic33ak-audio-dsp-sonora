;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_stream8_f32 -- coefficient-blended EIGHT-channel SINGLE-accumulator dot
;                     (load-reduction "STREAM-CEFF", per-channel MAC halved).
;
; Unlike wide4 (which keeps TWO sub-filter accumulators per channel, c0-dot and
; c1-dot, and blends them in C afterwards -> 2 MAC / channel / tap), this kernel
; blends the two sub-filter coefficients into ONE effective coefficient PER TAP,
; in registers, and fans it out to all EIGHT channels with a SINGLE MAC each:
;
;       ce[k] = c0[k] + wb*(c1[k] - c0[k])           (computed once per tap)
;       acc_c += x_c[k] * ce[k]                       (one MAC per channel)
;
; This HALVES the per-channel MAC count vs wide4 (8 MAC/tap for 8ch, not 16) at
; the cost of ~3 extra FP ops/tap for the blend -- amortised across 8 channels.
; No c_eff[] array is materialised (that was the earlier CEFF variant that lost
; to memory round-trips); c0[k]/c1[k] are loaded ONCE per tap straight into the
; blend. The output is ALREADY blended: out8[c] is the final channel-c sample,
; so the caller does asrc_to_slot(out8[c]) directly (no post-blend).
;
; Add order per output is sequential (tap 0,1,2,...) into a fixed accumulator,
; identical to wide4's per-accumulator order; the ONLY numerical difference vs
; wide4 is that the c0/c1 blend happens per-tap instead of once at the end. That
; makes this Class B (~1e-7 vs wide4) -> SFDR spot check required, NOT bit-exact.
;
; Load order follows the A0 lesson: window operands are loaded >= 2 instructions
; before their MAC. blockSize assumed EVEN (M=32). 2-tap unrolled. Uses only the
; f0..f11 range proven by wide4 (no dependence on f12+ existence).
;
;   f0..f7 = 8 channel accumulators   f8/f9 = c0[k]/c1[k] then window temps
;   f10    = ce (effective coeff)      f11   = wb (blend weight, whole call)
;
;   void mchp_stream8_f32(const float32_t *wbase0,// &ch[c][wbase]        w0
;                         uint32_t strideBytes,   // &ch[c+1]-&ch[c] bytes w1
;                         const float32_t *c0,    //                       w2
;                         const float32_t *c1,    //                       w3
;                         uint32_t blockSize,     // EVEN                  w4
;                         float32_t *out8,        // out8[0..7] (blended)  w5
;                         float32_t wb);          // blend weight          f0
;
; System resources: {w0..w12} used (w8..w12 saved/restored; w6/w7 scratch);
;                   {f0..f11} used (f8..f11 saved/restored; f0..f7 scratch);
;                   FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_stream8_f32
_mchp_stream8_f32:

    ; --- save callee-saved registers used here ---
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  fcr

    mov.s   f0, f11                  ; f11 = wb  (BEFORE f0 is reused as accumulator)

    ; --- derive ch1..ch7 window pointers from base + stride (once) ---
    add.l   w0, w1, w6               ; w6  = ch1
    add.l   w6, w1, w7               ; w7  = ch2
    add.l   w7, w1, w8               ; w8  = ch3
    add.l   w8, w1, w9               ; w9  = ch4
    add.l   w9, w1, w10              ; w10 = ch5
    add.l   w10, w1, w11             ; w11 = ch6
    add.l   w11, w1, w12             ; w12 = ch7
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
    bra     z, _stream8_store

    ; w0..w12 = ch0..ch7 windows   w2=c0 w3=c1   w4=pairs   w5=out8   f11=wb
v_stream8_loop:
    ; ---- tap n : build ce = c0 + wb*(c1-c0) in f10 ----
    mov.l   [w2++], f8               ; c0[k]   (ONCE for all 8 channels)
    mov.l   [w3++], f9               ; c1[k]   (ONCE for all 8 channels)
    sub.s   f9, f8, f9               ; f9 = c1 - c0
    mov.s   f8, f10                  ; f10 = c0
    mac.s   f9, f11, f10             ; f10 = c0 + wb*(c1-c0) = ce
    ; ---- fan ce out to 8 channels (single MAC each; load >=2 ahead) ----
    mov.l   [w0++], f8               ; ch0 x
    mov.l   [w6++], f9               ; ch1 x
    mac.s   f8, f10, f0              ; acc0 += ch0*ce
    mov.l   [w7++], f8               ; ch2 x (f8 free: acc0 consumed it)
    mac.s   f9, f10, f1              ; acc1
    mov.l   [w8++], f9               ; ch3 x
    mac.s   f8, f10, f2              ; acc2
    mov.l   [w9++], f8               ; ch4 x
    mac.s   f9, f10, f3              ; acc3
    mov.l   [w10++], f9              ; ch5 x
    mac.s   f8, f10, f4              ; acc4
    mov.l   [w11++], f8              ; ch6 x
    mac.s   f9, f10, f5              ; acc5
    mov.l   [w12++], f9              ; ch7 x
    mac.s   f8, f10, f6              ; acc6
    mac.s   f9, f10, f7              ; acc7
    ; ---- tap n+1 ----
    mov.l   [w2++], f8
    mov.l   [w3++], f9
    sub.s   f9, f8, f9
    mov.s   f8, f10
    mac.s   f9, f11, f10
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
    DTB     w4, v_stream8_loop

_stream8_store:
    mov.l   f0, [w5]                 ; out8[0] = ch0 (already blended)
    mov.l   f1, [w5+4]               ; out8[1] = ch1
    mov.l   f2, [w5+8]               ; out8[2] = ch2
    mov.l   f3, [w5+12]              ; out8[3] = ch3
    mov.l   f4, [w5+16]              ; out8[4] = ch4
    mov.l   f5, [w5+20]              ; out8[5] = ch5
    mov.l   f6, [w5+24]              ; out8[6] = ch6
    mov.l   f7, [w5+28]              ; out8[7] = ch7

    pop.l   fcr
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    .end
