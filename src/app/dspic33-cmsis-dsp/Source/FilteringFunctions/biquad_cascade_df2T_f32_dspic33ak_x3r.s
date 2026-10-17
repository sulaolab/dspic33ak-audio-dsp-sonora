;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x3r.s
;
; THREE CHANNELS, TWO SAMPLES PER ITERATION, NO COPIES.
;
; This is the combination of the two independent levers Phase D and Phase E each
; found separately, and it is a CANDIDATE: if it measures faster it is meant to
; ship.
;
; THE TWO LEVERS, AND WHY THEY DO NOT CONFLICT
;   x2r (rotation) removed one of the two mov.s copies per sample by unrolling
;   two samples and letting d1 and d2 swap registers instead of being copied:
;   8.651 instructions against x2's 9.900. But it handed one cycle back to ev8
;   (0.999) - with only two chains, the shortened dependency distances no longer
;   covered each other, so it netted 9.672 instead of the ~8.7 the instruction
;   count implied.
;
;   x3 (three channels) removed 0.167 instructions per sample of LOOP CONTROL -
;   one dtb amortised over three channel-samples instead of two - and kept
;   ev8 = ev9 = 0.000 with CPI 1.002. Measured 9.732.
;
;   These are different levers. Rotation attacks the arithmetic; the third
;   channel attacks the loop overhead AND supplies a third independent chain.
;   x3r applies both:
;
;     sample loop: 8 instr per channel-sample x 3 channels x 2 samples = 48
;                  plus one dtb                                        = 49
;                  49 / 6 channel-samples                          = 8.167
;     per stage:   35 / 96 channel-samples                         = 0.365
;                                                                    -------
;     instructions per sample per section                            ~8.53
;
;   PREDICTION, RECORDED BEFORE MEASURING (do not edit this afterwards - amend it
;   in the report, the way this tree already requires):
;     instr/sample/section   ~8.53
;     ev8                     0.000 expected - THREE independent chains should
;                             cover the rotation's stall where two could not.
;                             This is the part that can go either way.
;     cyc/sample/section      8.55 - 8.75
;     vs opt_v1 (11.485)      -24 to -26 %
;     vs x2r   (9.672)        -10 to -12 %
;   If ev8 comes back at 0.999 as it did in x2r, this lands near 9.5 and still
;   wins. The downside case is register pressure forcing a spill, which would
;   show up as a measured instruction count above 8.7.
;
; HOW THE ROTATION WORKS, PER CHANNEL
;   Sample 1 builds y in the register that held old d1, and the new d1 in the
;   register that held old d2. Sample 2 does the same thing with the roles
;   swapped, so the loop closes with d1 and d2 back in the registers they
;   started in. Two samples per iteration, and no mov.s at all:
;
;     sample 1:  d1 in Fa, d2 in Fb  ->  y in Fa, new d1 in Fb, new d2 in Fa'
;     sample 2:  d1 in Fb, d2 in Fa' ->  y in Fb, new d1 in Fa', new d2 in Fb'
;
;   Channel A uses F5 (d1 at entry), F6 (d2), F7 (x), F8 (the rotation partner) -
;   exactly x2r's assignment for channel A.
;   Channel B uses F12, F13, F14, F11 - x2r's channel B.
;   Channel C uses F20, F21, F22, F19 - x3's channel C.
;
; EVEN blockSize ONLY
;   Two samples per iteration, so an odd count would run one sample past the
;   caller's buffers. Refused up front with nothing written, the same as x2r.
;   APP_BLOCK_FRAMES is 32, so the Classic 4ch path can take this kernel, but it
;   is a NARROWER contract than x2/x3's 1..512 and that is a real cost, not a
;   footnote: it is the reason x2 rather than x2r went into the audio path.
;
; ARITHMETIC IS OPT_V1'S, PER CHANNEL, UNCHANGED
;   Each channel performs opt_v1's five operations per sample on opt_v1's operand
;   values in opt_v1's order; only the register a value lives in changes. No
;   instruction of one channel names a register of another. So every channel's
;   output must be BIT-IDENTICAL to opt_v1 run on it alone - a difference is a
;   defect in this file, not a rounding effect, and not something a tolerance may
;   be widened to accept.
;
; REGISTER BUDGET
;   F0..F26, as x3. F8 and above are callee-saved in this tree, so F8..F26 are
;   pushed. W registers are the tight resource: six data pointers, three
;   coefficient pointers, three state pointers and two counters = 14, and W1..W14
;   is exactly 14, so nothing spills.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x3r, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x3r

_biquad_cascade_df2T_f32_dspic33ak_x3r:

;------------------------------------------------------------------------------
; Refuse odd or zero blockSize before anything is written or pushed. The loop
; consumes two samples per channel per iteration, so an odd count would run one
; sample past the caller's buffers.
;------------------------------------------------------------------------------
        and.l       W7, #0, W7        ; (W7 is scratch below; clear it first)
        and.l       W3, #1, W7
        cp.l        W7, #0
        bra         nz, L_x3r_refuse

        cp.l        W3, #0
        bra         z, L_x3r_refuse

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
; Resolve the three instances and the six data pointers ONCE, outside both loops.
; Same order and same frame as x3: W0/W1/W2 are the argument arrays and each is
; finished with before its register is reused.
;
; Loop register roles:
;   W1  = pSrcA (post-increment)   W2  = pDstA
;   W8  = pSrcB                    W9  = pDstB
;   W14 = pSrcC                    W3  = pDstC
;   W10 = coeff A                  W12 = state A
;   W11 = coeff B                  W13 = state B
;   W6  = coeff C                  W7  = state C
;   W4  = pair counter             W5  = stage counter
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
; Pair count = blockSize / 2. Long form of the shift, for the reason opt_v3
; documents: xc-dsc v3.31.01's objdump mis-renders the same-register short form's
; source field as the shift amount, which makes a correct prologue read like a
; pointer defect during a disassembly review.
;------------------------------------------------------------------------------
        lsr.l       W3, #1, W0        ; W0 = pair count; W3 is now free for pDstC

;------------------------------------------------------------------------------
; Frame (W15 grows up), four words, as x3:
;   [W15-16] = pDstA original
;   [W15-12] = pDstB original
;   [W15-8]  = pDstC original
;   [W15-4]  = pair count
;
; W0 already holds the pair count from the shift above and is dead afterwards.
;------------------------------------------------------------------------------
        mov.l       [W2+8], W3        ; W3 = pDstC
        mov.l       [W2+4], W9        ; W9 = pDstB
        mov.l       [W2+0], W2        ; W2 = pDstA   (last use of the pDst array)

        mov.l       W2, [W15++]       ; [W15-16] pDstA original
        mov.l       W9, [W15++]       ; [W15-12] pDstB original
        mov.l       W3, [W15++]       ; [W15-8]  pDstC original
        mov.l       W0, [W15++]       ; [W15-4]  pair count

        mov.l       [W1+8], W14       ; W14 = pSrcC
        mov.l       [W1+4], W8        ; W8  = pSrcB
        mov.l       [W1+0], W1        ; W1  = pSrcA  (last use of the pSrc array)

;==============================================================================
; Stage loop
;==============================================================================
L_x3r_startFilter:

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

        mov.l       [W15-4], W4       ; W4 = pair counter for this stage

;==============================================================================
; Sample loop - three channels, two samples each, no copies
;
; 48 instructions for SIX channel-samples = 8.000 per sample, plus one dtb.
; Read it as triples: instruction 1 = A, 2 = B, 3 = C, and no instruction of one
; channel names a register of another.
;
; Sample 1: each channel's d1 arrives in its "low" register and y is built there;
;           the new d1 lands in the rotation partner.
; Sample 2: the roles swap back, so the loop closes with d1 where it started.
;
;   A: F5/F8 rotate, F6 = d2, F7 = x     coefficients F0..F4
;   B: F12/F11 rotate, F13 = d2, F14 = x coefficients F9,F10,F15,F16,F17
;   C: F20/F19 rotate, F21 = d2, F22 = x coefficients F18,F23,F24,F25,F26
;==============================================================================
L_x3r_startPairs:

        ; ---- sample 1 --------------------------------------------------------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F14       ; B: x
        mov.l       [W14++], F22      ; C: x

        mac.s       F0, F7, F5        ; A: F5  = old d1 + b0*x = y
        mac.s       F9, F14, F12      ; B: F12 = old d1 + b0*x = y
        mac.s       F18, F22, F20     ; C: F20 = old d1 + b0*x = y

        mov.s       F6, F8            ; A: F8  = old d2  (becomes new d1)
        mov.s       F13, F11          ; B: F11 = old d2
        mov.s       F21, F19          ; C: F19 = old d2

        mul.s       F2, F7, F6        ; A: F6  = b2*x
        mul.s       F15, F14, F13     ; B: F13 = b2*x
        mul.s       F24, F22, F21     ; C: F21 = b2*x

        mac.s       F1, F7, F8        ; A: F8  = old d2 + b1*x
        mac.s       F10, F14, F11     ; B: F11 = old d2 + b1*x
        mac.s       F23, F22, F19     ; C: F19 = old d2 + b1*x

        mov.l       F5, [W2++]        ; A: *pDstA++ = y
        mov.l       F12, [W9++]       ; B: *pDstB++ = y
        mov.l       F20, [W3++]       ; C: *pDstC++ = y

        mac.s       F3, F5, F8        ; A: F8  = new d1
        mac.s       F16, F12, F11     ; B: F11 = new d1
        mac.s       F25, F20, F19     ; C: F19 = new d1

        mac.s       F4, F5, F6        ; A: F6  = new d2
        mac.s       F17, F12, F13     ; B: F13 = new d2
        mac.s       F26, F20, F21     ; C: F21 = new d2

        ; ---- sample 2: d1 now in F8 (A), F11 (B), F19 (C) --------------------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F14       ; B: x
        mov.l       [W14++], F22      ; C: x

        mac.s       F0, F7, F8        ; A: F8  = old d1 + b0*x = y
        mac.s       F9, F14, F11      ; B: F11 = old d1 + b0*x = y
        mac.s       F18, F22, F19     ; C: F19 = old d1 + b0*x = y

        mov.s       F6, F5            ; A: F5  = old d2  (becomes new d1)
        mov.s       F13, F12          ; B: F12 = old d2
        mov.s       F21, F20          ; C: F20 = old d2

        mul.s       F2, F7, F6        ; A: F6  = b2*x
        mul.s       F15, F14, F13     ; B: F13 = b2*x
        mul.s       F24, F22, F21     ; C: F21 = b2*x

        mac.s       F1, F7, F5        ; A: F5  = old d2 + b1*x
        mac.s       F10, F14, F12     ; B: F12 = old d2 + b1*x
        mac.s       F23, F22, F20     ; C: F20 = old d2 + b1*x

        mov.l       F8, [W2++]        ; A: *pDstA++ = y
        mov.l       F11, [W9++]       ; B: *pDstB++ = y
        mov.l       F19, [W3++]       ; C: *pDstC++ = y

        mac.s       F3, F8, F5        ; A: F5  = new d1 (back where it started)
        mac.s       F16, F11, F12     ; B: F12 = new d1
        mac.s       F25, F19, F20     ; C: F20 = new d1

        mac.s       F4, F8, F6        ; A: F6  = new d2
        mac.s       F17, F11, F13     ; B: F13 = new d2
        mac.s       F26, F19, F21     ; C: F21 = new d2

        dtb         W4, L_x3r_startPairs

;------------------------------------------------------------------------------
; Write all three channels' state back. d1 is in F5 / F12 / F20 here, as at
; loop entry.
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

        dtb         W5, L_x3r_startFilter

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

L_x3r_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
