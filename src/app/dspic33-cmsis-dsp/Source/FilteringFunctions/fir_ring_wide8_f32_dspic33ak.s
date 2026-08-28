;*****************************************************************************
; fir_ring_wide8_f32_dspic33ak.s
;
; NOT a Microchip file.  Written for this project.  See the note in
; fir_ring_q31_dspic33ak.s about why it lives in the vendored tree and why the
; "mchp_" prefix is not used.
;
; _fir_ring_wide8_f32 -- float32 FIR tap loop, eight channels per pass.
;
;   void fir_ring_wide8_f32(const float32_t *hist,   /* w0 */
;                           const float32_t *coeff,  /* w1 */
;                           uint32_t taps,           /* w2, >= 2 */
;                           float32_t *out8);        /* w3 */
;
; The point of this kernel is the width, not the schedule.  The FPU mac.s
; takes register operands only, so a float MAC always costs at least one
; mov.l plus one mac.s.  The single-channel float FIRs in this tree therefore
; sit at 3-4 instructions per MAC (two loads + MAC, plus the dummy mpy.l the
; vendor needs just to bump a Y-modulo pointer).  Sharing ONE coefficient load
; across eight channels drives that down to its asymptote:
;
;   per tap: 1 coefficient load + 8 sample loads + 8 mac.s = 17 instructions
;            for 8 MACs                                    = 2.125 instr/MAC
;
; plus one taken DTB (2/3 cycles, DS70005591C Table 39-2 instr #39) per tap,
; so ~2.4 cycles/MAC as written and ~2.2 if the tap body is unrolled.  Two
; instructions per MAC is the floor for float on this core; only the Q31 DSP
; engine goes below it (see fir_ring_q31_dspic33ak.s).
;
; hist is FRAME-major: hist[frame*8 + ch], `taps` frames contiguous and
; ascending from the oldest, i.e. the mirrored ring the C front end already
; builds.  Unlike the Q31 kernel this needs no X/Y split and no channel-major
; rewrite -- the eight channels of one frame are eight consecutive loads.
;
; The coefficient run must be symmetric (linear phase); the loop walks
; coefficients and history in the same direction.
;
; f9..f12 rotate over the sample loads so that no load has to wait on the
; mac.s that read the same register; f0..f7 hold the eight accumulators and
; are caller-saved, f8..f12 are callee-saved and pushed here.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global    _fir_ring_wide8_f32
_fir_ring_wide8_f32:

    push.l  fcr
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    floatsetup w4

    ; ---- tap 0: seed the eight accumulators (no zeroing pass needed) ------
    mov.l   [w1++], f8              ; h[0]
    mov.l   [w0++], f9              ; frame0 ch0
    mov.l   [w0++], f10             ;        ch1
    mov.l   [w0++], f11             ;        ch2
    mov.l   [w0++], f12             ;        ch3
    mul.s   f9,  f8, f0
    mul.s   f10, f8, f1
    mul.s   f11, f8, f2
    mul.s   f12, f8, f3
    mov.l   [w0++], f9              ;        ch4
    mov.l   [w0++], f10             ;        ch5
    mov.l   [w0++], f11             ;        ch6
    mov.l   [w0++], f12             ;        ch7
    mul.s   f9,  f8, f4
    mul.s   f10, f8, f5
    mul.s   f11, f8, f6
    mul.s   f12, f8, f7

    sub.l   #1, w2                  ; taps-1 taps left
    cp0.l   w2
    bra     z, _wide8_store

    ; ---- taps 1 .. taps-1 -------------------------------------------------
_wide8_tap:
    mov.l   [w1++], f8              ; h[m], shared by all eight channels
    mov.l   [w0++], f9
    mov.l   [w0++], f10
    mov.l   [w0++], f11
    mov.l   [w0++], f12
    mac.s   f9,  f8, f0
    mac.s   f10, f8, f1
    mac.s   f11, f8, f2
    mac.s   f12, f8, f3
    mov.l   [w0++], f9
    mov.l   [w0++], f10
    mov.l   [w0++], f11
    mov.l   [w0++], f12
    mac.s   f9,  f8, f4
    mac.s   f10, f8, f5
    mac.s   f11, f8, f6
    mac.s   f12, f8, f7
    dtb     w2, _wide8_tap

_wide8_store:
    mov.l   f0, [w3++]
    mov.l   f1, [w3++]
    mov.l   f2, [w3++]
    mov.l   f3, [w3++]
    mov.l   f4, [w3++]
    mov.l   f5, [w3++]
    mov.l   f6, [w3++]
    mov.l   f7, [w3++]

    pop.l   f12
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    pop.l   fcr
    return

    .end
