;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x2r.s
;
; x2's two-channel interleave PLUS opt_v3's two-sample register rotation: two
; independent channels, each unrolled over two samples so that the d1/d2 copies
; disappear instead of being executed.
;
; This is Phase D round 3, and it is the combination that neither earlier result
; could justify on its own.
;
; THE ARGUMENT FOR TRYING THE THING THAT ALREADY FAILED ONCE
;   opt_v3 was exactly this rotation, on ONE channel, and it lost badly: 8.5
;   instructions per sample against opt_v1's 10, and 13.986 cycles against
;   11.484, because the instructions it saved came back as 4 cycles of ev8. It
;   was rejected, and Phase B and C then spent two sweeps establishing WHY:
;
;     - the stall is an FPU register dependency (ev8), not an interval problem
;     - the cycles it costs are genuinely EMPTY ISSUE SLOTS - Phase C put three
;       nopr into them and the cycle count did not move at all
;
;   So opt_v3's rotation did not fail because rotation is wrong. It failed
;   because a single channel has no independent work to put in the slots the
;   rotation opens, and nopr proved those slots are real and fillable.
;
;   x2 then measured 9.921 cyc/sample/section with ev8 = ev9 = 0 and CPI 1.004:
;   the interleave supplies exactly that independent work, and the loop became
;   ISSUE-bound. When a loop is issue-bound, the only remaining lever is
;   instruction count - and four of x2's eighteen loop instructions are the
;   mov.s copies that rotation exists to remove.
;
;   That is the whole hypothesis: rotation's cost was latency it could not hide,
;   x2 has latency headroom to spare, so combining them should pay the
;   instruction saving without the stall.
;
;   WHAT THE ROTATION ACTUALLY SAVES, COUNTED HONESTLY
;     The rotation removes ONE of the two copies per sample, not both. opt_v1
;     copies twice: old d1 -> the y accumulator, and old d2 -> the d1
;     accumulator. Rotation makes the first one unnecessary, because y is built
;     in the register that already holds old d1. The old-d2 copy stays. That is
;     why opt_v3 measured 8.5 instructions per sample and not 7.5.
;
;     Counted from the loop as written below - 8 instructions per channel-sample,
;     32 per iteration for four channel-samples, plus one dtb:
;
;       opt_v1   10 per sample                         (2 copies, no unroll)
;       x2        9.0 per sample  (18 / 2)             (2 copies, interleaved)
;       x2r       8.25 per sample (33 / 4)             (1 copy, interleaved)
;
;     So the saving over x2 is 0.75 instructions per sample, not 2. Modest - but
;     x2 is issue-bound at CPI 1.004, which means instructions convert into
;     cycles almost one for one, and 0.75 of them is worth about 8 %.
;
;   PREDICTION, RECORDED BEFORE MEASURING (do not edit afterwards - amend in the
;   report, as this tree requires):
;     instructions/sample/section   ~8.7  (8.25 + the per-stage load overhead,
;                                   which measured ~0.55 on x2: 9.901 vs 9.359
;                                   counted)
;     ev8                            0.000 hoped; if the rotation's dependency
;                                    survives even with a second channel's work
;                                    interleaved, this is where it shows
;     cyc/sample/section            ~8.7-9.2
;     versus opt_v1's 11.484         -20 to -24 %
;     versus x2's 9.921              -7 to -12 %
;   FALSIFIABLE OUTCOME: if ev8 comes back at ~4 and cycles land near x2's or
;   worse, then rotation carries a cost that independent work cannot cover, and
;   x2 at 9.0 instructions is the floor for this family. That is a real result
;   and it retires the rotation idea for good.
;
; HOW THE ROTATION REMOVES THE COPIES
;   opt_v1 must restore the invariant "d1 lives in F5, d2 in F6" every sample,
;   which costs two mov.s. Unrolling two samples lets the registers swap roles
;   instead: sample A leaves its new d1 where sample B expects to find it, and
;   sample B leaves its new d1 back where sample A started. Two samples restore
;   the original assignment, so the loop still closes.
;
;   Channel A, sample 1: d1 arrives in F5, d2 in F6, y is built in F8.
;   Channel A, sample 2: d1 arrives in F8, d2 in F6, y is built in F5.
;     ... and after sample 2, d1 is in F5 again. No copies.
;
;   This is opt_v3's schedule verbatim, per channel. Its arithmetic is opt_v1's -
;   the same five operations on the same operand values in the same order - which
;   is why opt_v3 was bit-identical to opt_v1 and why this must be too.
;
; THE COST THIS INHERITS FROM opt_v3: EVEN-ONLY
;   Unrolling two samples means blockSize must be even and non-zero. The bench
;   measures 32 and the Classic path uses 32, so no wrapper is needed here; a
;   caller with an odd block would need the dispatch shape opt_v3 has.
;
; REGISTERS
;   Channel A: F5/F8 alternate as d1 and y, F6 = d2, F7 = x, coefficients F0..F4.
;   Channel B: F12/F11 alternate as d1 and y, F13 = d2, F14 = x, coefficients
;              F9, F10, F15, F16, F17.
;   Identical to x2's assignment, so a difference between x2 and x2r is the
;   rotation and nothing else.
;
; C prototype (identical to x2's):
;
;   void biquad_cascade_df2T_f32_dspic33ak_x2r(
;       const arm_biquad_cascade_df2T_instance_f32 *SA,
;       const float32_t *pSrcA, float32_t *pDstA,
;       const arm_biquad_cascade_df2T_instance_f32 *SB,
;       const float32_t *pSrcB, float32_t *pDstB,
;       uint32_t blockSize );
;
;   W0 = SA, W1 = pSrcA, W2 = pDstA, W3 = SB, W4 = pSrcB, W5 = pDstB,
;   W6 = blockSize.  Confirmed from the compiler, not assumed: seven 32-bit
;   arguments land in W0..W6, and F8..F19 / W8..W14 are callee-saved.
;
;   BOTH INSTANCES MUST HAVE THE SAME numStages, as in x2.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x2r, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x2r

_biquad_cascade_df2T_f32_dspic33ak_x2r:

;------------------------------------------------------------------------------
; Refuse odd or zero blockSize before anything is written or pushed. The loop
; below consumes two samples per iteration, so an odd count would run one sample
; past the caller's buffers.
;------------------------------------------------------------------------------
        and.l       W7, #0, W7        ; (W7 is scratch below; clear it first)
        and.l       W6, #1, W7
        cp.l        W7, #0
        bra         nz, L_x2r_refuse

        cp.l        W6, #0
        bra         z, L_x2r_refuse

;------------------------------------------------------------------------------
; Callee-saved registers: F8..F17 and W8..W14.
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
; opt_v1's FPU setup, so a bit-exactness comparison against it is meaningful.
;------------------------------------------------------------------------------
        mov.l       #0x7F, W7
        floatsetup  W7

;------------------------------------------------------------------------------
; Unpack both instances.
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
; Pair count = blockSize / 2. Long form of the shift, for the reason opt_v3
; documents: xc-dsc v3.31.01's objdump mis-renders the same-register short form's
; source field as the shift amount, which makes a correct prologue read like a
; pointer defect during a disassembly review.
;------------------------------------------------------------------------------
        lsr.l       W6, #1, W7
        mov.l       W7, W6            ; W6 = pair count

;------------------------------------------------------------------------------
; Saved across the stage loop:
;   [W15-16] = original pDstA
;   [W15-12] = original pDstB
;   [W15-8]  = original pSrcB
;   [W15-4]  = pair count
;------------------------------------------------------------------------------
        mov.l       W2, [W15++]       ; original pDstA
        mov.l       W9, [W15++]       ; original pDstB
        mov.l       W8, [W15++]       ; original pSrcB
        mov.l       W6, [W15++]       ; pair count

;==============================================================================
; Stage loop
;==============================================================================
L_x2r_startFilter:

        ; Channel A coefficients: F0=b0 F1=b1 F2=b2 F3=a1 F4=a2.
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

        mov.l       [W15-4], W14      ; W14 = pair counter for this stage

;==============================================================================
; Sample loop - two samples per channel per iteration, no copies
;
; Sample 1: A's d1 arrives in F5 and leaves in F8; B's arrives in F12, leaves
;           in F11.  y is built in the register that held old d1.
; Sample 2: the roles swap back, so the loop closes with d1 in F5 / F12 again.
;
; Read the interleave as pairs: odd instruction = channel A, even = channel B,
; and no instruction of one channel names a register of the other.
;==============================================================================
L_x2r_startPairs:

        ; ---- sample 1 --------------------------------------------------------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F14       ; B: x

        mac.s       F0, F7, F5        ; A: F5  = old d1 + b0*x = y
        mac.s       F9, F14, F12      ; B: F12 = old d1 + b0*x = y

        mov.s       F6, F8            ; A: F8  = old d2   (becomes new d1)
        mov.s       F13, F11          ; B: F11 = old d2   (becomes new d1)

        mul.s       F2, F7, F6        ; A: F6  = b2*x
        mul.s       F15, F14, F13     ; B: F13 = b2*x

        mac.s       F1, F7, F8        ; A: F8  = old d2 + b1*x
        mac.s       F10, F14, F11     ; B: F11 = old d2 + b1*x

        mov.l       F5, [W2++]        ; A: *pDstA++ = y
        mov.l       F12, [W9++]       ; B: *pDstB++ = y

        mac.s       F3, F5, F8        ; A: F8  = new d1
        mac.s       F16, F12, F11     ; B: F11 = new d1

        mac.s       F4, F5, F6        ; A: F6  = new d2
        mac.s       F17, F12, F13     ; B: F13 = new d2

        ; ---- sample 2: d1 now in F8 (A) and F11 (B) --------------------------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F14       ; B: x

        mac.s       F0, F7, F8        ; A: F8  = old d1 + b0*x = y
        mac.s       F9, F14, F11      ; B: F11 = old d1 + b0*x = y

        mov.s       F6, F5            ; A: F5  = old d2   (becomes new d1)
        mov.s       F13, F12          ; B: F12 = old d2   (becomes new d1)

        mul.s       F2, F7, F6        ; A: F6  = b2*x
        mul.s       F15, F14, F13     ; B: F13 = b2*x

        mac.s       F1, F7, F5        ; A: F5  = old d2 + b1*x
        mac.s       F10, F14, F12     ; B: F12 = old d2 + b1*x

        mov.l       F8, [W2++]        ; A: *pDstA++ = y
        mov.l       F11, [W9++]       ; B: *pDstB++ = y

        mac.s       F3, F8, F5        ; A: F5  = new d1  (back where it started)
        mac.s       F16, F11, F12     ; B: F12 = new d1

        mac.s       F4, F8, F6        ; A: F6  = new d2
        mac.s       F17, F11, F13     ; B: F13 = new d2

        dtb         W14, L_x2r_startPairs

;------------------------------------------------------------------------------
; Write both channels' state back. d1 is in F5 / F12 here, as at loop entry.
;------------------------------------------------------------------------------
        mov.l       F5, [W12++]       ; A: pState[0] = d1
        mov.l       F6, [W12++]       ; A: pState[1] = d2
        mov.l       F12, [W13++]      ; B: pState[0] = d1
        mov.l       F13, [W13++]      ; B: pState[1] = d2

;------------------------------------------------------------------------------
; Next stage reads this stage's output, both channels.
;------------------------------------------------------------------------------
        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB

        dtb         W4, L_x2r_startFilter

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

L_x2r_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
