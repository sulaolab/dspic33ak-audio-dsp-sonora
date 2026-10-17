;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x2c.s
;
; x2's two-channel interleave with the CPU-side instructions CLUSTERED instead of
; spread: both y stores moved to the end of the loop, so the FPU instructions run
; eight in a row rather than in blocks of four separated by stores.
;
; Same 18 instructions as x2, same arithmetic, same registers, same operand
; values, same order of FPU operations. The ONLY difference is where the two
; stores sit. This is a fixed-instruction-count controlled experiment, the same
; discipline Phase B used to prove instruction count is not cycle count.
;
; THE QUESTION, AND WHOSE IT IS
;   The owner raised two related ideas from outside this measurement series:
;
;     (a) an FPU stall does not stop the CPU, so CPU-side instructions placed
;         after a stalling FPU instruction may proceed while the FPU waits, and
;     (b) a limit on how many FPU instructions can be in flight at once - a
;         four-entry hazard tracker - after which further FPU issue is held.
;
;   Claim (b) IS NOT IN THE PROGRAMMER'S REFERENCE MANUAL for this part
;   (DS70005540C). Searched: no "tracker", no in-flight or queue-depth limit, no
;   CPU/FPU concurrency prose, and none of the MOVCRW / MOVWCR / LDWLOCR /
;   STWLOCR mnemonics it was attributed to. What the PRM does give is the
;   latency table (MAC.s and MUL.s 3 cycles single precision, MOV.s 1) and
;   MOV [Wns+Slit12],Fd, an offset-addressed memory-to-F-register move. So (b) is
;   treated here as an untested hypothesis, not as documentation - this tree has
;   been wrong before by trusting an annotation over a counter.
;
;   But it is CHEAP TO TEST, and this file is that test. x2 already issues four
;   consecutive FPU instructions in three places and measures ev8 = 0.000, which
;   is one data point against a four-entry limit biting at four. x2c doubles the
;   run to eight consecutive FPU instructions by moving the two stores out of the
;   middle. If there is a depth limit anywhere between four and eight, or if the
;   CPU-side stores were doing useful covering work where they sat, x2c is slower
;   than x2 and the counters say which. If x2c matches x2 to the tick, then on
;   this core the placement of the CPU-side work inside this loop does not
;   matter, and eight consecutive FPU instructions cost nothing extra.
;
;   Both outcomes are results. A null result here is worth having: it says the
;   scheduling freedom is larger than assumed, which matters for the next kernel.
;
; THE TWO SCHEDULES, SIDE BY SIDE
;
;   x2 (stores in the middle)          x2c (stores at the end, THIS FILE)
;   -----------------------------      ---------------------------------
;    1 mov.l [W1++],F7   CPU            1 mov.l [W1++],F7   CPU
;    2 mov.l [W8++],F14  CPU            2 mov.l [W8++],F14  CPU
;    3 mov.s F5,F8       FPU            3 mov.s F5,F8       FPU
;    4 mov.s F12,F11     FPU            4 mov.s F12,F11     FPU
;    5 mov.s F6,F5       FPU            5 mov.s F6,F5       FPU
;    6 mov.s F13,F12     FPU            6 mov.s F13,F12     FPU
;    7 mac.s F0,F7,F8    FPU            7 mac.s F0,F7,F8    FPU
;    8 mac.s F9,F14,F11  FPU            8 mac.s F9,F14,F11  FPU
;    9 mac.s F1,F7,F5    FPU            9 mac.s F1,F7,F5    FPU
;   10 mac.s F10,F14,F12 FPU           10 mac.s F10,F14,F12 FPU
;   11 mul.s F2,F7,F6    FPU           11 mul.s F2,F7,F6    FPU
;   12 mul.s F15,F14,F13 FPU           12 mul.s F15,F14,F13 FPU
;   13 mov.l F8,[W2++]   CPU  <--      13 mac.s F3,F8,F5    FPU
;   14 mov.l F11,[W9++]  CPU  <--      14 mac.s F16,F11,F12 FPU
;   15 mac.s F3,F8,F5    FPU           15 mac.s F4,F8,F6    FPU
;   16 mac.s F16,F11,F12 FPU           16 mac.s F17,F11,F13 FPU
;   17 mac.s F4,F8,F6    FPU           17 mov.l F8,[W2++]   CPU  <--
;   18 mac.s F17,F11,F13 FPU           18 mov.l F11,[W9++]  CPU  <--
;
;   x2  : longest run of consecutive FPU instructions = 10 (3..12) then 4 (15..18)
;   x2c : longest run = 14 (3..16)
;
;   Note what that table shows about x2 itself: its FPU run is already TEN
;   instructions long before the stores interrupt it, not four. So if a
;   four-deep limit existed and bit at four, x2 would already be paying for it,
;   and x2 measures ev8 = 0.000. This file pushes the run to fourteen.
;
; WHY MOVING THE STORES IS LEGAL
;   The stores read F8 and F11, which hold y. Nothing after instruction 12 writes
;   F8 or F11 - the final MACs write F5, F12, F6 and F13 - so y is still intact
;   at the end of the loop and the stores read the same values wherever they sit
;   in that range. The output must therefore be bit-identical to x2 and to
;   opt_v1, and the bench proves it on both channels before reporting a cycle.
;
;   Moving them LATER also lengthens the producer-to-consumer distance for the
;   store (from 6 to 10), which the sp_* arms showed can make things worse rather
;   than better on this core. That is part of what is being measured.
;
; PREDICTION, RECORDED BEFORE MEASURING
;   I expect x2c to match x2 to within noise: 9.921 cyc/sample/section, ev8 and
;   ev9 both 0.000, 9.901 instructions. Reasoning: x2 is already issue-bound at
;   CPI 1.004, which leaves no stall for a reschedule to remove, and its FPU run
;   is already ten deep without penalty. If instead x2c is SLOWER, the hypothesis
;   that a deep FPU run costs something gains its first evidence on this part,
;   and the owner's idea (a) - keep CPU-side work interleaved among FPU work -
;   becomes a scheduling rule worth following in the next kernel.
;
; C prototype: identical to x2's.
;
;   void biquad_cascade_df2T_f32_dspic33ak_x2c(
;       const arm_biquad_cascade_df2T_instance_f32 *SA,
;       const float32_t *pSrcA, float32_t *pDstA,
;       const arm_biquad_cascade_df2T_instance_f32 *SB,
;       const float32_t *pSrcB, float32_t *pDstB,
;       uint32_t blockSize );
;
;   W0 = SA, W1 = pSrcA, W2 = pDstA, W3 = SB, W4 = pSrcB, W5 = pDstB,
;   W6 = blockSize.  BOTH INSTANCES MUST HAVE THE SAME numStages, as in x2.
;   Any blockSize 1..512: this arm is not unrolled, so it keeps opt_v1's
;   contract, unlike x2r.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x2c, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x2c

_biquad_cascade_df2T_f32_dspic33ak_x2c:

        cp.l        W6, #0
        bra         z, L_x2c_refuse

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

        mov.l       #0x7F, W7
        floatsetup  W7

        mov.l       W4, W8            ; W8  = pSrcB
        mov.l       W5, W9            ; W9  = pDstB

        clr         W4
        mov.b       [W0+0], W4        ; W4 = numStages (channel A's, drives both)

        mov.l       [W0+4], W10       ; W10 = coeff A
        mov.l       [W0+8], W12       ; W12 = state A

        mov.l       [W3+4], W11       ; W11 = coeff B
        mov.l       [W3+8], W13       ; W13 = state B

        mov.l       W2, [W15++]       ; [W15-16] original pDstA
        mov.l       W9, [W15++]       ; [W15-12] original pDstB
        mov.l       W8, [W15++]       ; [W15-8]  original pSrcB
        mov.l       W6, [W15++]       ; [W15-4]  blockSize

L_x2c_startFilter:

        mov.l       [W10++], F0       ; A: b0
        mov.l       [W10++], F1       ; A: b1
        mov.l       [W10++], F2       ; A: b2
        mov.l       [W10++], F3       ; A: a1
        mov.l       [W10++], F4       ; A: a2

        mov.l       [W11++], F9       ; B: b0
        mov.l       [W11++], F10      ; B: b1
        mov.l       [W11++], F15      ; B: b2
        mov.l       [W11++], F16      ; B: a1
        mov.l       [W11++], F17      ; B: a2

        mov.l       [W12],   F5       ; A: d1
        mov.l       [W12+4], F6       ; A: d2
        mov.l       [W13],   F12      ; B: d1
        mov.l       [W13+4], F13      ; B: d2

        mov.l       [W15-4], W14      ; sample counter for this stage

;==============================================================================
; Sample loop - x2's schedule with both stores moved to the END
;
; Instructions 3..16 are fourteen consecutive FPU operations. x2 breaks the same
; sequence after ten with the two stores. Everything else is identical.
;==============================================================================
L_x2c_startSections:
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

        mac.s       F3, F8, F5        ; A: d1 += a1*y
        mac.s       F16, F11, F12     ; B: d1 += a1*y

        mac.s       F4, F8, F6        ; A: d2 += a2*y
        mac.s       F17, F11, F13     ; B: d2 += a2*y

        ; y is still in F8 / F11: nothing above wrote them after the first MAC.
        mov.l       F8, [W2++]        ; A: *pDstA++ = y   <- moved here from 13
        mov.l       F11, [W9++]       ; B: *pDstB++ = y   <- moved here from 14

        dtb         W14, L_x2c_startSections

        mov.l       F5, [W12++]       ; A: pState[0] = d1
        mov.l       F6, [W12++]       ; A: pState[1] = d2
        mov.l       F12, [W13++]      ; B: pState[0] = d1
        mov.l       F13, [W13++]      ; B: pState[1] = d2

        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB

        dtb         W4, L_x2c_startFilter

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

L_x2c_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
