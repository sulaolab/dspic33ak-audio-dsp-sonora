;*****************************************************************************
; asrc_fir_hb_kernel_dspic33ak.s
;
; NOT A PRODUCTION DEFAULT, but it IS on the audio path whenever the 96 -> 48 kHz
; Q31 pre-stage is enabled (APP_ASRC_Q31_PRE_HALFBAND), which the
; APP_BUILD_ASRC_CODEC_96K_12CH_32K preset does.  A build that enables neither
; caller pays nothing for it: the function has its own code section and
; --gc-sections drops it whole.  It used to be wrapped in .ifdef instead, which
; assembled it to nothing but made the C-side switch imply an -AsDefine that no
; header can emit -- so selecting the preset was not enough to build it.
;
; TWO CALLERS ASK FOR IT, AND EITHER ONE IS ENOUGH:
;   ASRC_FIR_KERNEL_BENCH_AVAILABLE  the standalone microbench, which times this
;                                    kernel against the dense one on synthetic
;                                    data (asrc_fir_kernel_bench.c);
;   APP_ASRC_Q31_PRE_HALFBAND        the real 96 -> 48 kHz pre-stage running the
;                                    half-band set, i.e. the same arithmetic
;                                    measured on the actual leg-A path.
; The second exists because the bench cannot answer whether the microbench's
; saving SURVIVES on the audio path -- and it cannot be used to find out, since
; enabling the bench drags in its own xmemory arrays (firb_coeff is
; aligned(1024)) and would move the placement of everything being measured.
; So both callers were served rather than the bench enabled; today they are served
; by the C side alone, since the assembler no longer needs telling.
;
; It deliberately lives beside the bench in src/app/apps/asrc/ and NOT in the
; vendored dspic33-cmsis-dsp tree: that tree is published and synced, and this
; is an experiment.
;
;   q31_t *fir_ring_q31_hb_ymod_yonly_block(
;       const q31_t *coeff,     /* w0  the 4*half-1 filter's NON-ZERO taps only, */
;                               /*     in ascending index order (see LAYOUT)     */
;       const q31_t *hist,      /* w1  Y space, oldest sample of this window     */
;       uint32_t half,          /* w2  >= 3; the filter length is 4*half - 1     */
;       q31_t *out,             /* w3  outputs written here                      */
;       uint32_t outputs,       /* w4  >= 1                                      */
;       uint32_t decim_bytes,   /* w5  decimation factor * 4                     */
;       const q31_t *ring,      /* w6  YMODSRT (ring base)                       */
;       uint32_t ring_bytes);   /* w7  ring length in bytes                      */
;
;   returns the updated history pointer.
;
; WHAT IT IS.  A half-band decimating FIR of length taps = 4*half - 1.  A
; half-band prototype is exactly zero at every EVEN offset from its centre, so
; with the centre at M = (taps-1)/2 = 2*half - 1 (always ODD, which is why the
; length is 4k-1 and not merely odd) the surviving taps are the even ABSOLUTE
; indices 0, 2, ... taps-1 plus the centre M, and nothing else:
;
;       taps = 35, half = 9, M = 17
;       non-zero at 0,2,4,...,16 | 17 | 18,20,...,34   ->  9 + 1 + 9 = 19
;       against 35 MACs for the dense kernel over the same filter.
;
; Skipping a tap that is zero by filter structure is the only arithmetic this
; study is permitted to drop; nothing here approximates the response.
;
; LAYOUT.  coeff[] holds those 19 values in ascending index order and nothing
; else -- h[0], h[2], ... h[16], h[17], h[18], h[20], ... h[34].  The kernel
; walks it with a plain +=4 from start to finish, so the coefficient side is
; identical in cost to the dense kernel's and the packing is the obvious one.
;
; HOW IT DIFFERS FROM fir_ring_q31_ymod_yonly_dspic33ak.s, which it is derived
; from and which owns the full rationale for Y-modulo-only, the X/Y placement
; requirement, and why the coefficient pointer is reloaded per output.  The
; inner loop is split into three pieces so that the Y pointer's stride tracks
; the tap spacing:
;
;       repeat half-3  mac [w0]+=4, [w1]+=8   ; 0,2,...,2*(half-2)  stride 2
;                      mac [w0]+=4, [w1]+=4   ; 2*(half-1)          then step 1
;                      mac [w0]+=4, [w1]+=4   ; the centre, M       then step 1
;       repeat half-1  mac [w0]+=4, [w1]+=8   ; M+1, M+3, ...       stride 2
;
; `mac.l [w0]+=4, [w1]+=8, A` assembles to the same single 4-byte instruction as
; the +=4 form (checked in the object file, not assumed: increments 4, 8, 12, 16
; and -4 are all legal; +=2 is rejected because a long access must be 4-byte
; aligned).  So every MAC above is still one instruction, and the fixed cost per
; output is one extra `repeat` versus the dense kernel -- 1 cycle against the 22
; MACs that were dropped.  Static count per output, no wrap taken:
;
;       dense 41 tap : 2 mov + mpy + repeat + 40 mac + sacr + 3 + dtb = 49
;       half-band 35 : 2 mov + mpy + repeat +  7 mac + 2 mac
;                          + repeat + 9 mac + sacr + 3 + dtb          = 29
;
; i.e. the saving should be very nearly the 22 skipped MACs themselves.  That is
; a prediction about instruction counts; whether the hardware delivers it is the
; question the bench answers, and the judgement is the measured block difference.
;
; The one thing genuinely unknown, and the reason this file had to be flashed
; rather than reasoned about: whether the Y AGU's modulo folds correctly when an
; increment of 8 can step OVER the ring end instead of landing on it.  The
; earlier M6 sweep only ever exercised increment 4.  Note that EVERY Y step here
; is a post-increment, so the AGU is the only thing that ever wraps -- there is
; no hand-folded address to get wrong.  The bench validates against a C
; reference at window offsets that force the wrap, before it times anything, and
; repeats the whole thing on a ring length that IS a multiple of 8 bytes.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

; Own code section, so a build that calls neither of the two callers above drops
; this whole function at link time (--gc-sections, with isolate-each-function and
; remove-unused-sections set in every MPLAB configuration).  That is what replaced
; the .ifdef gate: assembling unconditionally means a C-side switch is sufficient
; on its own, and a mismatch between the C side and the assembler side -- which
; used to be a missing --defsym and a link error -- can no longer exist.
    .section .dspic33cmsisdsp_fir_hb, code

    .global    _fir_ring_q31_hb_ymod_yonly_block
_fir_ring_q31_hb_ymod_yonly_block:

    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  CORCON
    push.l  MODCON
    push.l  YMODSRT
    push.l  YMODEND

    fractsetup w8                   ; fractional mode: sets sacr.l alignment

    mov.l   #0x401F, w8             ; YMODEN, YWM = w1, XWM = 1111 (X modulo OFF)
    mov.l   w8, MODCON

    mov.l   w6, YMODSRT             ; history ring
    add.l   w6, w7, w9
    sub.l   #1, w9
    mov.l   w9, YMODEND

    add.l   w6, w7, w9              ; w9 = one past the ring end, for the wrap test
    sub.l   w2, #3, w8              ; w8 = half-3, REPEAT count for the lower run
    sub.l   w2, #1, w6              ; w6 = half-1, REPEAT count for the upper run.
                                    ; w6 is dead once YMODSRT is loaded, so it is free.
    mov.l   w1, w10                 ; w10 = start of this output's tap window
    mov.l   w0, w11                 ; w11 = coefficient base, reloaded per output

_hb_out:
    mov.l   w11, w0                 ; the whole price of dropping X modulo
    mov.l   w10, w1                 ; restart the window
    mpy.l   [w0]+=4, [w1]+=8, a     ; a  = h[0] * x[0]
    repeat  w8
    mac.l   [w0]+=4, [w1]+=8, a     ; a += h[2k] * x[2k], k = 1 .. half-2
    mac.l   [w0]+=4, [w1]+=4, a     ; a += h[2*(half-1)] * x[...], then step ONE
    mac.l   [w0]+=4, [w1]+=4, a     ; a += h[M] * x[M], the lone odd-index tap
    repeat  w6
    mac.l   [w0]+=4, [w1]+=8, a     ; a += h[M+1+2k] * x[...], k = 0 .. half-1

    sacr.l  a, [w3++]               ; rounded Q31 output

    add.l   w10, w5, w10            ; step the window by the decimation factor
    cp.l    w10, w9
    bra     ltu, _hb_no_wrap
    sub.l   w10, w7, w10
_hb_no_wrap:
    dtb     w4, _hb_out

    mov.l   w10, w0                 ; return the updated window start

    pop.l   YMODEND
    pop.l   YMODSRT
    pop.l   MODCON
    pop.l   CORCON
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return


    .end
