;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x2.s
;
; TWO INDEPENDENT CHANNELS through one DF2T cascade loop, their instructions
; interleaved so that each channel's MAC latency is covered by the other
; channel's work instead of by a stall.
;
; This is Phase D round 2, and unlike every previous round in this family it is
; a CANDIDATE, not an experiment: if it measures faster it is meant to ship.
;
; WHY THIS IS THE ONE WITH HEADROOM
;   Everything measured so far says the single-channel loop is latency-bound,
;   not issue-bound:
;
;     opt_v1   10.433 instr, 11.484 cyc, CPI 1.102, ev9 0.999, ev8 0.000
;     opt_v3    8.937 instr, 13.986 cyc, CPI 1.568, ev8 3.999  (REJECTED)
;
;   opt_v3 proved that squeezing instructions out of ONE channel's chain just
;   converts them into stall: -1.5 instructions bought +4 cycles of ev8, and
;   Phase C showed those cycles are real issue slots sitting empty inside the
;   sample (filling them with nopr cost nothing at all - 18798 ticks with three
;   spacers added per pair, the same as with none).
;
;   Empty issue slots plus a 3-cycle MAC latency (DS70005540C Table 6-2) is
;   exactly the shape that interleaving fixes. A second channel's MACs are
;   INDEPENDENT of the first's - different coefficients, different state,
;   different registers - so they can occupy those slots while channel A's
;   result is in flight, and vice versa. No arithmetic changes, no rounding
;   changes, no rotation, none of what made opt_v3 fail.
;
;   The registers are available: the FPU has F0..F31 (DS70005540C Figure 1-2)
;   and opt_v1 uses only F0..F8. This kernel uses F0..F14, still under half.
;
; WHAT THIS COSTS, STATED UP FRONT
;   The coefficient loads double: each stage now loads five coefficients per
;   channel, ten in all, plus two states per channel. That is 14 loads per stage
;   against opt_v1's 7, and at 84 stages x 32 frames it is amortised over 32
;   samples - about +0.22 instr/sample/section. The prize is up to ~1.9
;   instructions and ~2.8 cycles per sample, so the trade is worth making, but
;   the per-stage overhead is why this kernel is measured at ap84 rather than
;   argued about: at ONE stage per channel it might well lose.
;
;   It also needs both channels to have the SAME number of stages, which the
;   4-channel Classic path satisfies by construction (one NUM_STAGE for all).
;
; THE SCHEDULE, AND THE DISTANCES IT BUYS
;   Channel A uses F5 (d1), F6 (d2), F7 (x), F8 (y) and coefficients F0..F4 -
;   exactly opt_v1's assignment, so channel A alone is opt_v1.
;   Channel B uses F12 (d1), F13 (d2), F14 (x), F11 (y) and coefficients
;   F9, F10, F15, F16, F17.
;
;     1   mov.l [W1++], F7     ; A: x
;     2   mov.l [W8++], F14    ; B: x
;     3   mov.s F5, F8         ; A: F8 = old d1   (y accumulator)
;     4   mov.s F12, F11       ; B: F11 = old d1
;     5   mov.s F6, F5         ; A: F5 = old d2   (d1 accumulator)
;     6   mov.s F13, F12       ; B: F12 = old d2
;     7   mac.s F0, F7, F8     ; A: y   <- produces F8
;     8   mac.s F9, F14, F11   ; B: y   <- produces F11
;     9   mac.s F1, F7, F5     ; A: d1 = old d2 + b1*x
;    10   mac.s F10, F14, F12  ; B: d1
;    11   mul.s F2, F7, F6     ; A: d2 = b2*x
;    12   mul.s F15, F14, F13  ; B: d2
;    13   mov.l F8, [W2++]     ; A: store y      <- F8 read, distance 6
;    14   mov.l F11, [W9++]    ; B: store y      <- F11 read, distance 6
;    15   mac.s F3, F8, F5     ; A: d1 += a1*y
;    16   mac.s F16, F11, F12  ; B: d1 += a1*y
;    17   mac.s F4, F8, F6     ; A: d2 += a2*y
;    18   mac.s F17, F11, F13  ; B: d2 += a2*y
;
;   18 instructions for TWO samples = 9.0 per sample, against opt_v1's 10.0.
;   Every producer-to-consumer distance is at least 5, where opt_v1 has 3 and
;   pays one cycle of ev9 for it. So the prediction is ev9 = 0 and ev8 = 0, at
;   9.0 instructions - and if the core issues one instruction per cycle with no
;   stall, that lands near 9.2 cyc/sample/section against opt_v1's 11.484.
;
;   PREDICTION, RECORDED BEFORE MEASURING (do not edit this afterwards - amend
;   it in the report instead, the way this tree already requires):
;     instructions/sample/section  ~9.2   (9.0 + the per-stage load overhead)
;     ev8, ev9                      0.000 both
;     cyc/sample/section           ~9.3-9.6
;     versus opt_v1's 11.484        -17 to -19 %
;   If the measured cycle count lands near the instruction count, the loop has
;   become issue-bound and THAT is the new floor - no further scheduling of this
;   shape can help, and the next lever would be 3 channels or fewer
;   instructions per sample.
;
; THE INTERFACE, AND WHY IT IS A NEW SYMBOL RATHER THAN A DROP-IN
;   Two channels means two input pointers, two output pointers and two instance
;   structures, so this cannot have opt_v1's signature. It takes two instances:
;
;     void biquad_cascade_df2T_f32_dspic33ak_x2(
;         const arm_biquad_cascade_df2T_instance_f32 *SA,
;         const float32_t *pSrcA, float32_t *pDstA,
;         const arm_biquad_cascade_df2T_instance_f32 *SB,
;         const float32_t *pSrcB, float32_t *pDstB,
;         uint32_t blockSize );
;
;   W0 = SA, W1 = pSrcA, W2 = pDstA, W3 = SB, W4 = pSrcB, W5 = pDstB,
;   W6 = blockSize   (per the xc-dsc calling convention: first seven 32-bit
;   arguments in W0..W6).
;
;   BOTH INSTANCES MUST HAVE THE SAME numStages. The kernel reads channel A's
;   count and drives both cascades with it; a mismatch would silently filter
;   channel B with the wrong number of sections. The caller in this tree
;   guarantees it (one NUM_STAGE for all four channels), and the bench asserts
;   it. This is stated here because it is the one way to misuse this kernel and
;   get plausible-looking audio out.
;
; REGISTER BUDGET AND WHAT MUST BE SAVED
;   F8 and above are callee-saved in this tree's convention (opt_v1 pushes F8).
;   This kernel uses F8..F17, so it pushes all ten. Ten pushes and ten pops per
;   CALL, amortised over blockSize x numStages samples: at ap84 x 32 that is
;   0.007 instr/sample/section, negligible. At one stage and one sample it is
;   not, which is the same caveat as the coefficient loads.
;
;   W registers: W0..W6 arrive as arguments, W7 is the FCR scratch, and the loop
;   needs pointers for both channels plus two counters. W8/W9 carry channel B's
;   src/dst, W10/W11 the two coefficient pointers, W12/W13 the two state
;   pointers, W14 the sample counter and W4 the stage counter. W15 is the stack.
;   Everything W8 and above is callee-saved, so they are pushed too.
;
; ARITHMETIC IS OPT_V1'S, PER CHANNEL, UNCHANGED
;   Each channel performs exactly opt_v1's five operations per sample on exactly
;   opt_v1's operand values in exactly opt_v1's order. Interleaving two
;   independent chains cannot change either channel's result: no instruction of
;   channel A reads or writes any register of channel B. So the output must be
;   BIT-IDENTICAL to opt_v1 run separately on each channel, and the bench proves
;   that on both channels before reporting a cycle count. A difference is a
;   defect in this file - not a rounding effect, and not something a tolerance
;   may be widened to accept.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x2, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x2

_biquad_cascade_df2T_f32_dspic33ak_x2:

;------------------------------------------------------------------------------
; Refuse a zero blockSize before anything is written or pushed, so the refusal
; path is a plain return. opt_v1 relies on its caller for 1..512; this kernel is
; new, so it checks the one value that would make DTB wrap.
;------------------------------------------------------------------------------
        cp.l        W6, #0
        bra         z, L_x2_refuse

;------------------------------------------------------------------------------
; Callee-saved registers. F8..F17 and W8..W14.
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
        push.l      FCR

        push.l      W8
        push.l      W9
        push.l      W10
        push.l      W11
        push.l      W12
        push.l      W13
        push.l      W14

;------------------------------------------------------------------------------
; Mask all FPU exceptions, default rounding, SAZ/FTZ clear - opt_v1's setup, so
; that a bit-exactness comparison against opt_v1 is meaningful.
;------------------------------------------------------------------------------
        mov.l       #0x7F, W7
        floatsetup  W7

;------------------------------------------------------------------------------
; Unpack both instances.
;
; Channel A: W1 = pSrc, W2 = pDst (already in place).
; Channel B: move its pointers out of the argument registers into W8/W9 before
; anything reuses them.
;------------------------------------------------------------------------------
        mov.l       W4, W8            ; W8  = pSrcB
        mov.l       W5, W9            ; W9  = pDstB

        clr         W4
        mov.b       [W0+0], W4        ; W4 = numStages (channel A's, drives both)

        mov.l       [W0+4], W10       ; W10 = coeff A
        mov.l       [W0+8], W12       ; W12 = state A

        mov.l       [W3+4], W11       ; W11 = coeff B
        mov.l       [W3+8], W13       ; W13 = state B

;------------------------------------------------------------------------------
; Saved on the stack across the stage loop:
;   [W15-16] = original pDstA
;   [W15-12] = original pDstB
;   [W15-8]  = original pSrcB       (channel A's pSrc comes back via pDstA)
;   [W15-4]  = blockSize
;------------------------------------------------------------------------------
        mov.l       W2, [W15++]       ; original pDstA
        mov.l       W9, [W15++]       ; original pDstB
        mov.l       W8, [W15++]       ; original pSrcB
        mov.l       W6, [W15++]       ; blockSize

;==============================================================================
; Stage loop
;==============================================================================
L_x2_startFilter:

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

        ; States.
        mov.l       [W12],   F5       ; A: d1
        mov.l       [W12+4], F6       ; A: d2
        mov.l       [W13],   F12      ; B: d1
        mov.l       [W13+4], F13      ; B: d2

        mov.l       [W15-4], W14      ; W14 = sample counter for this stage

;==============================================================================
; Sample loop - the point of this file
;
; Two independent chains, interleaved. Read the pairs: every odd instruction is
; channel A, every even one channel B, and no instruction of one channel names a
; register of the other. Each producer is followed by FIVE instructions before
; its first consumer, where opt_v1 has three.
;
;   A: F5=d1 F6=d2 F7=x F8=y    coefficients F0..F4
;   B: F12=d1 F13=d2 F14=x F11=y  coefficients F9,F10,F15,F16,F17
;==============================================================================
L_x2_startSections:
        mov.l       [W1++], F7        ; A: x = *pSrcA++
        mov.l       [W8++], F14       ; B: x = *pSrcB++

        mov.s       F5, F8            ; A: F8  = old d1 (y accumulator)
        mov.s       F12, F11          ; B: F11 = old d1 (y accumulator)

        mov.s       F6, F5            ; A: F5  = old d2 (d1 accumulator)
        mov.s       F13, F12          ; B: F12 = old d2 (d1 accumulator)

        mac.s       F0, F7, F8        ; A: y  = old d1 + b0*x
        mac.s       F9, F14, F11      ; B: y  = old d1 + b0*x

        mac.s       F1, F7, F5        ; A: d1 = old d2 + b1*x
        mac.s       F10, F14, F12     ; B: d1 = old d2 + b1*x

        mul.s       F2, F7, F6        ; A: d2 = b2*x
        mul.s       F15, F14, F13     ; B: d2 = b2*x

        mov.l       F8, [W2++]        ; A: *pDstA++ = y   (F8 read, distance 6)
        mov.l       F11, [W9++]       ; B: *pDstB++ = y   (F11 read, distance 6)

        mac.s       F3, F8, F5        ; A: d1 += a1*y
        mac.s       F16, F11, F12     ; B: d1 += a1*y

        mac.s       F4, F8, F6        ; A: d2 += a2*y
        mac.s       F17, F11, F13     ; B: d2 += a2*y

        dtb         W14, L_x2_startSections

;------------------------------------------------------------------------------
; Write both channels' state back.
;------------------------------------------------------------------------------
        mov.l       F5, [W12++]       ; A: pState[0] = d1
        mov.l       F6, [W12++]       ; A: pState[1] = d2
        mov.l       F12, [W13++]      ; B: pState[0] = d1
        mov.l       F13, [W13++]      ; B: pState[1] = d2

;------------------------------------------------------------------------------
; Next stage reads this stage's output, for both channels - opt_v1's
; in-place cascade arrangement, done twice.
;------------------------------------------------------------------------------
        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB

        dtb         W4, L_x2_startFilter

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

L_x2_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
