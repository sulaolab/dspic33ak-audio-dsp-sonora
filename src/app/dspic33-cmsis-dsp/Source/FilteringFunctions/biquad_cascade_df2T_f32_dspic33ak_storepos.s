;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_storepos.s
;
; opt_v1's DF2T loop, emitted THREE TIMES with the y store - and in one arm the
; old-d2 copy as well - moved to a different position.  Every arm has opt_v1's
; EXACT instruction count (10 per sample) and opt_v1's exact arithmetic.
;
; This is Phase D round 1.  It attacks opt_v1's only remaining stall.
;
; WHAT IS BEING ATTACKED, AND WHY IT IS WORTH ONLY ~1 CYCLE
;   opt_v1 measures 11.484 cyc/sample/section with, per PMU (2026-09-22):
;
;     ev8 FPU register dependency   0.000
;     ev9 FPU read stall            0.999   <- this, and only this
;     ev7 CPU hazard                0.002
;     instructions                 10.433
;
;   One cycle. That is the whole prize here, and it caps this file at about
;   11.484 -> 10.5. The bigger lever is elsewhere (see the x2 file: two
;   independent channels interleaved), and this round is NOT a substitute for it.
;
; THE MECHANISM, AS A PREDICTION THIS FILE IS DESIGNED TO FALSIFY
;   DS70005540C Table 6-2: MAC.s result latency is 3 cycles for single
;   precision, MUL.s 3, MOV.s 1.  opt_v1's y producer and its first y consumer
;   sit exactly 3 instructions apart:
;
;     4  mac.s F0, F7, F8    ; y  = old d1 + b0*x     <- produces F8
;     5  mac.s F1, F7, F5    ; d1 = old d2 + b1*x
;     6  mul.s F2, F7, F6    ; d2 = b2*x
;     7  mov.l F8, [W2++]    ; *pDst++ = y            <- CONSUMES F8, distance 3
;     8  mac.s F3, F8, F5    ; d1 += a1*y
;     9  mac.s F4, F8, F6    ; d2 += a2*y
;
;   If a 3-cycle result needs distance 4 rather than 3, one cycle of stall is
;   exactly what a distance of 3 should cost - and ev9 is exactly 0.999.  So the
;   prediction is: give the y producer one more instruction of distance and the
;   stall goes away.
;
;   That is a PREDICTION, not a conclusion, and two things could falsify it.
;   The consumer at 7 is a STORE (ev9 is the CPU-side counter, "cannot read an
;   F-register"), while the consumers at 8 and 9 are FPU reads (ev8's side).  So
;   moving the store later might simply hand the stall to whichever instruction
;   inherits distance 3 - possibly as ev8 instead of ev9, possibly at the same
;   cost.  And if the store is not the stalling instruction at all, nothing here
;   will move.  Both outcomes are results: they say which consumer position
;   costs, which is the fact needed to schedule anything else on this core.
;
; THE THREE ARMS
;   All three keep opt_v1's arithmetic, operand values and order of operations,
;   so all three must be bit-identical to opt_v1.  All three are 10 instructions
;   per sample, so the comparison is at FIXED instruction count - the same
;   discipline Phase B used to prove instruction count is not cycle count.
;
;   sp_a  store LAST, after both final MACs.
;         Distance from the y producer: mac.s F3 = 3, mac.s F4 = 4, store = 5.
;         The store is fully covered; whether mac.s F3 now pays instead is the
;         question this arm answers.
;
;   sp_b  store BETWEEN the two final MACs.
;         mac.s F3 = 3, store = 4, mac.s F4 = 5.  One FPU consumer at 3, the
;         store at 4.  Sits between sp_a and opt_v1 and separates "the store is
;         expensive at distance 3" from "any consumer is".
;
;   sp_c  store last AND the old-d2 copy moved after the y producer, so that
;         NOTHING reads F8 at distance 3:
;
;           1  mov.l [W1++], F7   ; x
;           2  mov.s F5, F8       ; F8 = old d1   (y accumulator)
;           3  mac.s F0, F7, F8   ; y             <- produces F8
;           4  mov.s F6, F5       ; F5 = old d2   (d1 accumulator)
;           5  mac.s F1, F7, F5   ; d1 = old d2 + b1*x
;           6  mul.s F2, F7, F6   ; d2 = b2*x
;           7  mac.s F3, F8, F5   ; d1 += a1*y    <- first F8 read, distance 4
;           8  mac.s F4, F8, F6   ; d2 += a2*y    distance 5
;           9  mov.l F8, [W2++]   ; *pDst++ = y   distance 6
;
;         THE COPY MOVE IS LEGAL, and the reason is worth stating because it
;         looks unsafe: `mov.s F6, F5` overwrites F5, which held old d1 - but
;         step 2 already copied old d1 into F8, and nothing after step 2 reads
;         old d1 again.  Moving the copy after the y MAC therefore changes no
;         value, only the schedule.  This is the arm the latency table predicts
;         should win, and it is ALSO the cheapest possible test of that table:
;         if the prediction is right, sp_c is opt_v1 minus its one stall.
;
; WHY ONE MACRO WOULD NOT HELP HERE
;   gap.s and d1gap.s used a macro because their arms had to differ ONLY by
;   inserted spacers - a property that had to hold by construction.  These three
;   arms differ by instruction ORDER, which is the variable under test, so a
;   macro would have to be parameterised by the thing being varied and would
;   hide it.  The safety net is instead the bit-exactness gate plus a static
;   check that all three are 10 instructions with the same multiset of opcodes.
;
; C prototypes (identical to opt_v1's):
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_sp_a(
;       const arm_biquad_cascade_df2T_instance_f32 *S,
;       const float32_t *pSrc, float32_t *pDst, uint32_t blockSize );
;   ... likewise _sp_b, _sp_c.
;
;   W0 = S, W1 = pSrc, W2 = pDst, W3 = blockSize
;   [W0+0] = numStages (uint8_t), [W0+4] = coeffs, [W0+8] = state
;   Coefficients per stage: b0, b1, b2, a1, a2.  State per stage: d1, d2.
;
; BLOCKSIZE CONTRACT IS OPT_V1'S, NOT OPT_V3'S
;   These arms are NOT unrolled, so they take any blockSize of 1..512 exactly as
;   opt_v1 does.  No even-only restriction, no dispatch wrapper.  A candidate
;   from this file could therefore replace opt_v1 with no caller change at all,
;   which is half of why this round is worth running even though its ceiling is
;   one cycle.
;
; EACH ARM GETS ITS OWN CODE SECTION
;   --gc-sections then drops the whole set from any image that does not call
;   them, which is every shipping configuration.
;==============================================================================

        .include "dspcommon.inc"

;------------------------------------------------------------------------------
; Shared prologue/epilogue text.
;
; Identical to opt_v1's, including the FCR setup and the stage-loop structure.
; Only the SAMPLE loop differs between arms, so everything around it is emitted
; from these two macros - that way a prologue difference cannot be mistaken for
; a scheduling difference.
;------------------------------------------------------------------------------
.macro  DF2T_SP_PROLOGUE name

        .section .dspic33cmsisdsp_df2t_\name, code
        .global _biquad_cascade_df2T_f32_dspic33ak_\name

_biquad_cascade_df2T_f32_dspic33ak_\name:

        push.l      F8
        push.l      FCR

        mov.l       #0x7F, W7
        floatsetup  W7

        clr         W4
        mov.b       [W0+0], W4        ; numStages
        mov.l       [W0+4], W5        ; coefficients
        mov.l       [W0+8], W6        ; state

        mov.l       W2, [W15++]       ; [W15-8] = original pDst
        mov.l       W3, [W15++]       ; [W15-4] = original blockSize

L_sp_startFilter_\name:
        mov.l       [W5++], F0        ; b0
        mov.l       [W5++], F1        ; b1
        mov.l       [W5++], F2        ; b2
        mov.l       [W5++], F3        ; a1
        mov.l       [W5++], F4        ; a2

        mov.l       [W6],   F5        ; d1
        mov.l       [W6+4], F6        ; d2

L_sp_startSections_\name:
.endm

.macro  DF2T_SP_EPILOGUE name

        dtb         W3, L_sp_startSections_\name

        mov.l       [W15-4], W3       ; restore blockSize

        mov.l       F5, [W6++]        ; pState[0] = d1
        mov.l       F6, [W6++]        ; pState[1] = d2

        mov.l       [W15-8], W1       ; next stage reads this stage's output
        mov.l       [W15-8], W2

        dtb         W4, L_sp_startFilter_\name

        sub.l       #8, W15
        pop.l       FCR
        pop.l       F8
        return
.endm

;==============================================================================
; sp_a - store LAST
;
;   producer at 4; mac.s F3 distance 3, mac.s F4 distance 4, store distance 5.
;==============================================================================
        DF2T_SP_PROLOGUE sp_a

        mov.l       [W1++], F7        ; x = *pSrc++
        mov.s       F5, F8            ; F8 = old d1 (y accumulator)
        mov.s       F6, F5            ; F5 = old d2 (d1 accumulator)
        mac.s       F0, F7, F8        ; y  = old d1 + b0*x
        mac.s       F1, F7, F5        ; d1 = old d2 + b1*x
        mul.s       F2, F7, F6        ; d2 = b2*x
        mac.s       F3, F8, F5        ; d1 += a1*y
        mac.s       F4, F8, F6        ; d2 += a2*y
        mov.l       F8, [W2++]        ; *pDst++ = y   <- moved to last

        DF2T_SP_EPILOGUE sp_a

;==============================================================================
; sp_b - store BETWEEN the two final MACs
;
;   producer at 4; mac.s F3 distance 3, store distance 4, mac.s F4 distance 5.
;==============================================================================
        DF2T_SP_PROLOGUE sp_b

        mov.l       [W1++], F7        ; x = *pSrc++
        mov.s       F5, F8            ; F8 = old d1 (y accumulator)
        mov.s       F6, F5            ; F5 = old d2 (d1 accumulator)
        mac.s       F0, F7, F8        ; y  = old d1 + b0*x
        mac.s       F1, F7, F5        ; d1 = old d2 + b1*x
        mul.s       F2, F7, F6        ; d2 = b2*x
        mac.s       F3, F8, F5        ; d1 += a1*y
        mov.l       F8, [W2++]        ; *pDst++ = y   <- between the finals
        mac.s       F4, F8, F6        ; d2 += a2*y

        DF2T_SP_EPILOGUE sp_b

;==============================================================================
; sp_c - old-d2 copy moved AFTER the y producer, store last
;
;   Nothing reads F8 at distance 3.  producer at 3; first F8 read (mac.s F3) at
;   distance 4, mac.s F4 at 5, store at 6.
;
;   The copy move is value-neutral: F5's old contents (old d1) were already
;   saved into F8 by the instruction before the producer, and nothing later
;   reads old d1.
;==============================================================================
        DF2T_SP_PROLOGUE sp_c

        mov.l       [W1++], F7        ; x = *pSrc++
        mov.s       F5, F8            ; F8 = old d1 (y accumulator)
        mac.s       F0, F7, F8        ; y  = old d1 + b0*x   <- producer
        mov.s       F6, F5            ; F5 = old d2 (d1 accumulator)
        mac.s       F1, F7, F5        ; d1 = old d2 + b1*x
        mul.s       F2, F7, F6        ; d2 = b2*x
        mac.s       F3, F8, F5        ; d1 += a1*y    <- first F8 read, dist 4
        mac.s       F4, F8, F6        ; d2 += a2*y
        mov.l       F8, [W2++]        ; *pDst++ = y

        DF2T_SP_EPILOGUE sp_c

;==============================================================================
; End of file
;==============================================================================
