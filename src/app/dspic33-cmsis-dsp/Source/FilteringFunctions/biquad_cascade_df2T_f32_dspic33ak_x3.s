;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x3.s
;
; THREE INDEPENDENT CHANNELS through one DF2T cascade loop.
;
; This is Phase E, and it is an EXPERIMENT WITH A NEGATIVE PREDICTION, not a
; candidate. It is written to settle a question, and the honest expectation
; recorded before measuring is that it LOSES to x2.
;
; WHY THE PREDICTION IS NEGATIVE
;   x2 measured 9.900 instructions and 9.922 cycles per sample per section:
;   ev8 = ev9 = 0.000, CPI 1.004. That is ISSUE-BOUND. Interleaving buys exactly
;   one thing - coverage for a latency stall - and after x2 there is no stall
;   left to cover.
;
;   Per channel per sample the arithmetic is irreducible:
;     mac (y), mac (d1 partial), mul (d2), mac (d1 final), mac (d2 final)
;   plus one load (x) and one store (y) = 7 instructions that must exist.
;   x2 spends 9, the extra two being the mov.s copies that shuffle d1 -> y and
;   d2 -> d1. A THIRD CHANNEL DOES NOT REMOVE EITHER OF THEM: it is the same 9
;   instructions per channel-sample, three times over.
;
;   So x3 cannot reduce work per sample. What it does add is a bigger per-stage
;   prologue - 15 coefficient loads and 6 state loads against x2's 10 and 4 -
;   amortised over the same 32 samples, plus two frame reloads. That is why the
;   prediction is a small LOSS, around 0.3 to 1 %.
;
;   What would falsify the premise: if this measures materially FASTER than
;   9.922, then "one instruction per cycle with no stall" is wrong - there is
;   dual-issue capacity that three chains reach and two do not. That would be a
;   real finding about the core, which is the only reason this file exists.
;   (The PRM, DS70005540C, has no instruction-timing table to settle it from;
;   see the Phase D report section 2 on why arguing it is not an option here.)
;
; THE ARGUMENT WALL, AND HOW THIS SIDESTEPS IT
;   Three instances + three sources + three destinations + blockSize = ten
;   arguments, and xc-dsc passes only the first seven 32-bit arguments in W0..W6
;   (measured, not assumed: _scratch/abiprobe*.c). Passing the tail on the stack
;   would work but adds prologue cost to a kernel whose whole predicted deficit
;   IS prologue cost.
;
;   So the signature takes three ARRAYS instead:
;
;     void biquad_cascade_df2T_f32_dspic33ak_x3(
;         const arm_biquad_cascade_df2T_instance_f32 * const *S,   ; W0: S[0..2]
;         const float32_t * const *pSrc,                           ; W1: pSrc[0..2]
;         float32_t * const *pDst,                                 ; W2: pDst[0..2]
;         uint32_t blockSize );                                    ; W3
;
;   Four arguments, all in registers. The indirection is paid ONCE per call,
;   outside both loops, not per sample.
;
;   ALL THREE INSTANCES MUST HAVE THE SAME numStages. The kernel reads S[0]'s
;   count and drives all three cascades with it; a mismatch silently filters the
;   other channels with the wrong number of sections. This is the one way to
;   misuse the kernel and still get plausible-looking audio, which is why the
;   bench gives channels B and C their own opt_v1 references and sums every
;   mismatch into one counter.
;
; THE SCHEDULE
;   Three independent chains, round-robin. Channel A keeps opt_v1's assignment
;   exactly, so channel A alone is opt_v1:
;     A: F5=d1 F6=d2 F7=x F8=y        coefficients F0..F4
;     B: F12=d1 F13=d2 F14=x F11=y    coefficients F9,F10,F15,F16,F17
;     C: F20=d1 F21=d2 F22=x F19=y    coefficients F18,F23,F24,F25,F26
;
;   27 instructions for THREE samples = 9.000 per sample, the same as x2 - which
;   is the point being tested. Every producer-to-consumer distance grows from
;   x2's 5-6 to 8-9, so if any latency remained hidden at distance 5 this would
;   expose it as a further gain; the prediction says there is none.
;
; ARITHMETIC IS OPT_V1'S, PER CHANNEL, UNCHANGED
;   No instruction of one channel names a register of another, so each channel's
;   result must be BIT-IDENTICAL to opt_v1 run on it alone. A difference is a
;   defect in this file, not a rounding effect, and not something a tolerance may
;   be widened to accept.
;
; REGISTER BUDGET
;   F0..F26 of F0..F31 (DS70005540C Figure 1-2). F8 and above are callee-saved in
;   this tree, so F8..F26 are pushed - 19 pushes against x2's 10. At ap84 x 32
;   samples that is 0.014 instr/sample/section; at one stage and one sample it is
;   not, the same caveat x2 carries.
;
;   W registers are the tight resource, not F. The sample loop needs SIX data
;   pointers live at once (three src, three dst), three coefficient pointers,
;   three state pointers and two counters = 14, and W1..W14 is exactly 14. So
;   nothing has to be spilled and reloaded per stage: the frame holds only the
;   three original destination pointers and blockSize, which is the same
;   four-word frame x2 uses. W0 is free after the prologue.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x3, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x3

_biquad_cascade_df2T_f32_dspic33ak_x3:

;------------------------------------------------------------------------------
; Refuse a zero blockSize before anything is written or pushed, so the refusal
; path is a plain return.
;------------------------------------------------------------------------------
        cp.l        W3, #0
        bra         z, L_x3_refuse

;------------------------------------------------------------------------------
; Callee-saved registers: F8..F26 and W8..W14.
;------------------------------------------------------------------------------
        push.l      F8
        push.l      F9
        push.l      F10
        push.l      F11
        push.l      F12
        push.l      F13
        push.l      F14
        push.l      F15
        push.l      F16
        push.l      F17
        push.l      F18
        push.l      F19
        push.l      F20
        push.l      F21
        push.l      F22
        push.l      F23
        push.l      F24
        push.l      F25
        push.l      F26
        push.l      FCR

        push.l      W8
        push.l      W9
        push.l      W10
        push.l      W11
        push.l      W12
        push.l      W13
        push.l      W14

;------------------------------------------------------------------------------
; opt_v1's FPU setup, so a bit-exactness comparison against it is meaningful.
;------------------------------------------------------------------------------
        mov.l       #0x7F, W7
        floatsetup  W7

;------------------------------------------------------------------------------
; Resolve the three instances and the six data pointers ONCE, here, outside both
; loops.
;
; Order matters: W0/W1/W2 are the three argument ARRAYS, and each is finished
; with before its register is reused. Nothing reads an array after its register
; has been overwritten.
;
; Loop register roles, established by the end of this block:
;   W1  = pSrcA (post-increment)   W2  = pDstA
;   W8  = pSrcB                    W9  = pDstB
;   W14 = pSrcC                    W3  = pDstC
;   W10 = coeff A                  W12 = state A
;   W11 = coeff B                  W13 = state B
;   W6  = coeff C                  W7  = state C
;   W4  = sample counter           W5  = stage counter
;
; W0 holds the S array and is read three times, before anything reuses it.
;------------------------------------------------------------------------------
        mov.l       [W0+0], W4        ; W4 = S[0]
        clr         W5
        mov.b       [W4+0], W5        ; W5 = numStages (S[0]'s, drives all three)
        mov.l       [W4+4], W10       ; W10 = coeff A
        mov.l       [W4+8], W12       ; W12 = state A

        mov.l       [W0+4], W4        ; W4 = S[1]
        mov.l       [W4+4], W11       ; W11 = coeff B
        mov.l       [W4+8], W13       ; W13 = state B

        mov.l       [W0+8], W4        ; W4 = S[2]   (last use of the S array)
        mov.l       [W4+4], W6        ; W6 = coeff C
        mov.l       [W4+8], W7        ; W7 = state C

;------------------------------------------------------------------------------
; Frame (W15 grows up), four words - the same shape x2 uses:
;   [W15-16] = pDstA original
;   [W15-12] = pDstB original
;   [W15-8]  = pDstC original
;   [W15-4]  = blockSize
;
; W0 is dead from here on and serves as the one scratch register this shuffle
; needs. The destinations go to the frame FIRST, because their registers
; (W2/W3/W9) are also where the loop wants them, and the sources are read out of
; the pSrc array while W1 still points at it.
;------------------------------------------------------------------------------
        mov.l       W3, W0            ; W0 = blockSize (W3 becomes pDstC)

        mov.l       [W2+8], W3        ; W3 = pDstC
        mov.l       [W2+4], W9        ; W9 = pDstB
        mov.l       [W2+0], W2        ; W2 = pDstA   (last use of the pDst array)

        mov.l       W2, [W15++]       ; [W15-16] pDstA original
        mov.l       W9, [W15++]       ; [W15-12] pDstB original
        mov.l       W3, [W15++]       ; [W15-8]  pDstC original
        mov.l       W0, [W15++]       ; [W15-4]  blockSize

        mov.l       [W1+8], W14       ; W14 = pSrcC
        mov.l       [W1+4], W8        ; W8  = pSrcB
        mov.l       [W1+0], W1        ; W1  = pSrcA  (last use of the pSrc array)

        ; The pSrc pointers are needed only for stage 1: from stage 2 on the
        ; cascade is in-place and every stage reads the DESTINATION buffer, which
        ; is what the reset at the bottom of the stage loop restores. So no
        ; source pointer has to be kept in the frame.

;==============================================================================
; Stage loop
;==============================================================================
L_x3_startFilter:

        ; Channel A coefficients: F0=b0 F1=b1 F2=b2 F3=a1 F4=a2 (opt_v1's).
        mov.l       [W10++], F0
        mov.l       [W10++], F1
        mov.l       [W10++], F2
        mov.l       [W10++], F3
        mov.l       [W10++], F4

        ; Channel B coefficients: F9=b0 F10=b1 F15=b2 F16=a1 F17=a2.
        mov.l       [W11++], F9
        mov.l       [W11++], F10
        mov.l       [W11++], F15
        mov.l       [W11++], F16
        mov.l       [W11++], F17

        ; Channel C coefficients: F18=b0 F23=b1 F24=b2 F25=a1 F26=a2.
        mov.l       [W6++], F18
        mov.l       [W6++], F23
        mov.l       [W6++], F24
        mov.l       [W6++], F25
        mov.l       [W6++], F26

        ; States.
        mov.l       [W12],   F5       ; A: d1
        mov.l       [W12+4], F6       ; A: d2
        mov.l       [W13],   F12      ; B: d1
        mov.l       [W13+4], F13      ; B: d2
        mov.l       [W7],    F20      ; C: d1
        mov.l       [W7+4],  F21      ; C: d2

        mov.l       [W15-4], W4       ; W4 = sample counter for this stage

;==============================================================================
; Sample loop - three independent chains, round-robin
;
; 27 instructions for three channel-samples = 9.000 per sample, the same as x2.
; No instruction of one channel names a register of another.
;
;   A: F5=d1 F6=d2 F7=x F8=y      coefficients F0..F4
;   B: F12=d1 F13=d2 F14=x F11=y  coefficients F9,F10,F15,F16,F17
;   C: F20=d1 F21=d2 F22=x F19=y  coefficients F18,F23,F24,F25,F26
;==============================================================================
L_x3_startSections:
        mov.l       [W1++], F7        ; A: x = *pSrcA++
        mov.l       [W8++], F14       ; B: x = *pSrcB++
        mov.l       [W14++], F22      ; C: x = *pSrcC++

        mov.s       F5, F8            ; A: F8  = old d1 (y accumulator)
        mov.s       F12, F11          ; B: F11 = old d1
        mov.s       F20, F19          ; C: F19 = old d1

        mov.s       F6, F5            ; A: F5  = old d2 (d1 accumulator)
        mov.s       F13, F12          ; B: F12 = old d2
        mov.s       F21, F20          ; C: F20 = old d2

        mac.s       F0, F7, F8        ; A: y  = old d1 + b0*x
        mac.s       F9, F14, F11      ; B: y
        mac.s       F18, F22, F19     ; C: y

        mac.s       F1, F7, F5        ; A: d1 = old d2 + b1*x
        mac.s       F10, F14, F12     ; B: d1
        mac.s       F23, F22, F20     ; C: d1

        mul.s       F2, F7, F6        ; A: d2 = b2*x
        mul.s       F15, F14, F13     ; B: d2
        mul.s       F24, F22, F21     ; C: d2

        mov.l       F8, [W2++]        ; A: *pDstA++ = y   (F8 read, distance 9)
        mov.l       F11, [W9++]       ; B: *pDstB++ = y
        mov.l       F19, [W3++]       ; C: *pDstC++ = y

        mac.s       F3, F8, F5        ; A: d1 += a1*y
        mac.s       F16, F11, F12     ; B: d1 += a1*y
        mac.s       F25, F19, F20     ; C: d1 += a1*y

        mac.s       F4, F8, F6        ; A: d2 += a2*y
        mac.s       F17, F11, F13     ; B: d2 += a2*y
        mac.s       F26, F19, F21     ; C: d2 += a2*y

        dtb         W4, L_x3_startSections

;------------------------------------------------------------------------------
; Write all three channels' state back.
;------------------------------------------------------------------------------
        mov.l       F5, [W12++]       ; A: pState[0] = d1
        mov.l       F6, [W12++]       ; A: pState[1] = d2
        mov.l       F12, [W13++]      ; B: pState[0] = d1
        mov.l       F13, [W13++]      ; B: pState[1] = d2
        mov.l       F20, [W7++]       ; C: pState[0] = d1
        mov.l       F21, [W7++]       ; C: pState[1] = d2

;------------------------------------------------------------------------------
; Next stage reads this stage's output, for all three channels - opt_v1's
; in-place cascade arrangement, done three times.
;------------------------------------------------------------------------------
        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB
        mov.l       [W15-8],  W14     ; C: pSrc = original pDstC
        mov.l       [W15-8],  W3      ; C: pDst = original pDstC

        dtb         W5, L_x3_startFilter

;------------------------------------------------------------------------------
; Restore and return.
;------------------------------------------------------------------------------
        sub.l       #16, W15

        pop.l       W14
        pop.l       W13
        pop.l       W12
        pop.l       W11
        pop.l       W10
        pop.l       W9
        pop.l       W8

        pop.l       FCR
        pop.l       F26
        pop.l       F25
        pop.l       F24
        pop.l       F23
        pop.l       F22
        pop.l       F21
        pop.l       F20
        pop.l       F19
        pop.l       F18
        pop.l       F17
        pop.l       F16
        pop.l       F15
        pop.l       F14
        pop.l       F13
        pop.l       F12
        pop.l       F11
        pop.l       F10
        pop.l       F9
        pop.l       F8
        return

L_x3_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
