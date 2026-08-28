;*****************************************************************************
;                       Software License Agreement                           *
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;   SOFTWARE IS "AS IS". NO WARRANTIES.                                      *
;*****************************************************************************
;
; mchp_stream8_f32_p1 -- STREAM-CEFF (8ch single-acc) with SOFTWARE-PIPELINED ce.
;
; Same math and same result as mchp_stream8_f32 (Class B, bit-for-bit identical
; to the non-pipelined STREAM8): per tap ce = c0 + wb*(c1-c0), then acc_c += x*ce.
; The ONLY change is scheduling: the ce for tap k+1 is built (load c0/c1 -> sub ->
; mac) INTERLEAVED with tap k's 8 channel MACs, so the ce dependency chain latency
; hides behind the MAC throughput instead of stalling in front of it (the ~36 us
; the base STREAM8 left on the table vs its static-issue ideal).
;
; ce ping-pongs between two registers so no per-tap copy stalls the MACs:
;   f10 = ce_current (used by this tap's 8 MACs)   f12 = ce_next (being built)
;   f13/f14 = c0[k+1]/c1[k+1] load + blend scratch
;   f8/f9   = window temps        f11 = wb        f0..f7 = 8 accumulators
;
; Prologue builds ce_0. The loop runs taps 0..M-2 (prefetch c0[k+1]/c1[k+1] stays
; in-bounds: k+1 <= M-1). The epilogue does tap M-1 with NO prefetch -> never reads
; c0[M]/c1[M] (avoids the one-past-the-phase-row over-read on the boundary phase).
; blockSize assumed EVEN (M=32).
;
;   void mchp_stream8_f32_p1(const float32_t *wbase0,// &ch[c][wbase]        w0
;                            uint32_t strideBytes,   // row stride bytes      w1
;                            const float32_t *c0,    //                       w2
;                            const float32_t *c1,    //                       w3
;                            uint32_t blockSize,     // EVEN, >=2             w4
;                            float32_t *out8,        // out8[0..7] (blended)  w5
;                            float32_t wb);          // blend weight          f0
;
; System resources: {w0..w12} used (w8..w12 saved/restored); {f0..f14} used
;                   (f8..f14 saved/restored; f0..f7 scratch); FCR saved/used/restored.
;............................................................................

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _mchp_stream8_f32_p1
_mchp_stream8_f32_p1:

    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  fcr

    mov.s   f0, f11                  ; f11 = wb  (before f0 becomes accumulator)

    ; --- derive ch1..ch7 window pointers ---
    add.l   w0, w1, w6               ; ch1
    add.l   w6, w1, w7               ; ch2
    add.l   w7, w1, w8               ; ch3
    add.l   w8, w1, w9               ; ch4
    add.l   w9, w1, w10              ; ch5
    add.l   w10, w1, w11             ; ch6
    add.l   w11, w1, w12             ; ch7
    floatsetup w1                    ; w1 free

    ; --- zero the 8 accumulators ---
    movc.s  #22, f0
    movc.s  #22, f1
    movc.s  #22, f2
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5
    movc.s  #22, f6
    movc.s  #22, f7

    ; --- prologue: build ce_0 into f10 ---
    mov.l   [w2++], f13              ; c0[0]
    mov.l   [w3++], f14              ; c1[0]
    sub.s   f14, f13, f14            ; c1-c0
    mov.s   f13, f10                 ; ce = c0
    mac.s   f14, f11, f10            ; ce_0 = c0 + wb*(c1-c0)

    ; --- loop count = blockSize - 1 taps (taps 0..M-2) ---
    sub.l   w4, #1, w4               ; w4 = M-1
    cp0.l   w4
    bra     z, _p1_epilogue          ; M==1 guard (not expected; M even>=2)

    ; w0..w12 = ch0..ch7   w2=c0(+1) w3=c1(+1)   w4=M-1   w5=out8   f10=ce_k f11=wb
v_p1_loop:
    ; ---- prefetch coeff for tap k+1 ----
    mov.l   [w2++], f13              ; c0[k+1]
    mov.l   [w3++], f14              ; c1[k+1]
    ; ---- tap k: 8 channel MACs (ce=f10), ce_{k+1} build interleaved into f12 ----
    mov.l   [w0++], f8               ; ch0
    mov.l   [w6++], f9               ; ch1
    mac.s   f8, f10, f0              ; acc0
    sub.s   f14, f13, f14            ; (c1-c0)_{k+1}
    mac.s   f9, f10, f1              ; acc1
    mov.l   [w7++], f8               ; ch2
    mov.s   f13, f12                 ; ce_{k+1} = c0
    mac.s   f8, f10, f2              ; acc2
    mov.l   [w8++], f9               ; ch3
    mac.s   f14, f11, f12            ; ce_{k+1} += (c1-c0)*wb  -> ready
    mac.s   f9, f10, f3              ; acc3
    mov.l   [w9++], f8               ; ch4
    mac.s   f8, f10, f4              ; acc4
    mov.l   [w10++], f9              ; ch5
    mac.s   f9, f10, f5              ; acc5
    mov.l   [w11++], f8              ; ch6
    mac.s   f8, f10, f6              ; acc6
    mov.l   [w12++], f9              ; ch7
    mac.s   f9, f10, f7              ; acc7
    mov.s   f12, f10                 ; ce_k <- ce_{k+1} for next tap
    DTB     w4, v_p1_loop

_p1_epilogue:
    ; ---- final tap M-1: 8 MACs with ce (f10); NO prefetch ----
    mov.l   [w0++], f8               ; ch0
    mov.l   [w6++], f9               ; ch1
    mac.s   f8, f10, f0
    mov.l   [w7++], f8               ; ch2
    mac.s   f9, f10, f1
    mov.l   [w8++], f9               ; ch3
    mac.s   f8, f10, f2
    mov.l   [w9++], f8               ; ch4
    mac.s   f9, f10, f3
    mov.l   [w10++], f9              ; ch5
    mac.s   f8, f10, f4
    mov.l   [w11++], f8              ; ch6
    mac.s   f9, f10, f5
    mov.l   [w12++], f9              ; ch7
    mac.s   f8, f10, f6
    mac.s   f9, f10, f7

    mov.l   f0, [w5]
    mov.l   f1, [w5+4]
    mov.l   f2, [w5+8]
    mov.l   f3, [w5+12]
    mov.l   f4, [w5+16]
    mov.l   f5, [w5+20]
    mov.l   f6, [w5+24]
    mov.l   f7, [w5+28]

    pop.l   fcr
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
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
