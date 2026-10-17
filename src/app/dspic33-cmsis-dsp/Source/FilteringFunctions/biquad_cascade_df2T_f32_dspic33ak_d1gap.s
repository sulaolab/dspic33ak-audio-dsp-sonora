;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_d1gap.s
;
; opt_v3's two-sample ping-pong DF2T loop, emitted FOUR TIMES with 0, 1, 2 and 3
; independent issue slots inserted INSIDE EACH SAMPLE: between that sample's d1
; partial accumulation and the d1 final accumulation that reads it.
;
; This is Phase C.  It is a different edge from the one
; biquad_cascade_df2T_f32_dspic33ak_gap.s swept, and that file is untouched.
;
; WHAT PHASE B ALREADY SETTLED, SO THAT THIS FILE DOES NOT RE-ASK IT
;   Phase B (gap.s, measured 2026-09-22) widened the CROSS-SAMPLE edge - sample
;   A's producers to sample B's consumers - by 0/1/2/3 slots and measured:
;
;     arm     cyc/sample/sec   instr    ev8 FPUdep
;     opt_v1        11.484    10.433        0.000
;     gap0          13.986     8.937        3.999
;     gap1          14.486     9.437        3.999
;     gap2          14.987     9.938        3.999
;     gap3          15.486    10.439        3.999
;
;   ev8 did not move at all, each slot cost a full cycle, and gap3 - the SAME
;   instruction count as opt_v1 - was still 34.8 % slower.  So the cross-sample
;   interval is not where the four cycles go, and the stall is not a function of
;   instruction count either.  What remains true is the ATTRIBUTION: the stall is
;   an FPU register dependency (event 8), and opt_v1 does not have one.
;
; THE HYPOTHESIS THIS FILE TESTS, AND IT IS ONLY A HYPOTHESIS
;   If the dependency is not across samples, the next place to look is inside
;   one.  Each sample accumulates d1 in two steps through ONE F-register:
;
;     sample A:  mac.s F1,F7,F8   ; F8 = old d2 + b1*x   <- d1 partial, producer
;                mov.l F5,[W2++]  ; store y
;                mac.s F3,F5,F8   ; F8 = new d1          <- d1 final, consumer
;                mac.s F4,F5,F6
;
;     sample B:  mac.s F1,F7,F5   ; F5 = old d2 + b1*x   <- d1 partial, producer
;                mov.l F8,[W2++]  ; store y
;                mac.s F3,F8,F5   ; F5 = new d1          <- d1 final, consumer
;                mac.s F4,F8,F6
;
;   Producer and consumer sit ONE instruction apart, and the instruction between
;   them is a store, not an FPU operation that could cover latency.  opt_v1 has
;   the same two-step accumulation but a different register assignment around it.
;
;   So: insert N independent issue slots between the producer and the consumer,
;   in BOTH samples, and read event 8.  If it falls, this region is in the stall;
;   if it does not, this distance is not the mechanism either and the next
;   experiment has to separate the operands rather than the distance.
;
; WHAT THIS EXPERIMENT CANNOT SAY, EVEN IF ev8 FALLS
;   The slots go in front of `mac.s F3,...`, which means they also delay
;   `mac.s F4,...` behind it.  A drop in event 8 therefore locates the stall in
;   THIS REGION; it does not prove the d1 partial -> d1 final edge is the only
;   contributor, and it does not rule out the d2 edge that shares the position.
;   Separating the two needs a different arm shape (moving one operand, not
;   widening one gap) and is deliberately not attempted here.
;
;   Nor is this file an optimisation attempt.  Every arm is LONGER than opt_v3 by
;   construction; none of them can be a candidate kernel.  The output of the
;   experiment is a counter reading, not a faster loop.
;
; WHY ONE MACRO AND NOT FOUR HAND-WRITTEN KERNELS
;   Same reason gap.s gives: "the only difference between the arms is the
;   spacing" has to be true by construction, not by review.  Four copies of a
;   16-instruction body would let a register name or an operand order drift
;   between arms, and that drift would be read as a spacing effect.  The .rept
;   below is the only text that differs between the four arms.
;
; THE SPACER IS `nopr`, AND THE REASON IS MEASURED, NOT PREFERRED
;   Carried over from gap.s, where all three candidates were assembled on this
;   toolchain (xc-dsc v3.31.01) and checked in the linked image:
;
;     nop   `00 01`        1 word    / 1 cycle   - executes, but only TWO BYTES
;     nopr  `03 00 00 fe`  1 word    / 1 cycle   - executes, FOUR BYTES  <- used
;     neop  `00 00`        0.5 word  / 0 CYCLES  - does NOT execute
;
;   `neop` provides no issue slot, so a sweep built on it measures nothing and
;   looks exactly like a refutation.  `nop` is two bytes, so an ODD number of
;   them makes the assembler insert a two-byte `neop` pad before the next
;   four-byte FPU instruction - benign, but it makes the odd arms structurally
;   unlike the even ones, which puts a second unknown inside the one measurement
;   that exists to remove an unknown.  `nopr` is four bytes: no pad, ever.
;
;   objdump PRINTS `nopr` AS `neop`, in both v3.31.01 and v4.00, so the mnemonic
;   cannot be used to tell them apart.  Use the encoding: `03 00 00 fe` executes,
;   `00 00` is the pad.  A body containing `00 00` inside a gap is not what this
;   file emits and invalidates that arm.
;
;   `nopr` touches no F-register, no W register, no pointer, no loop counter and
;   no status bit, so it cannot perturb the arithmetic or the DF2T state.
;
; ARITHMETIC IS OPT_V3'S, WHICH IS OPT_V1'S
;   Every arm performs the same five operations per sample, on the same operand
;   values, in the same order as opt_v1 - only register NAMES rotate, exactly as
;   in opt_v3.  So all four arms must be bit-identical to opt_v1, and the bench
;   proves that before it reports a single cycle count.  A spacing change that
;   alters an output bit is a defect in this file, not a rounding difference, and
;   no tolerance may be relaxed to accept it.
;
; EVEN-ONLY, SAME AS opt_v3
;   blockSize must be even and non-zero; an odd or zero value returns having
;   written nothing.  The bench measures blockSize = 32 and calls these kernels
;   directly, so no dispatch wrapper is on the timed path.
;
; C prototypes (all four identical to opt_v1's / opt_v3's):
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_d1gap0(
;       const arm_biquad_cascade_df2T_instance_f32 *S,
;       const float32_t *pSrc, float32_t *pDst, uint32_t blockSize );
;   ... likewise _d1gap1, _d1gap2, _d1gap3.
;
;   W0 = S, W1 = pSrc, W2 = pDst, W3 = blockSize
;   [W0+0] = numStages (uint8_t), [W0+4] = coeffs, [W0+8] = state
;   Coefficients per stage: b0, b1, b2, a1, a2.  State per stage: d1, d2.
;
; d1gap0 IS opt_v3
;   Same instruction sequence, same register rotation, same 17 instructions per
;   pair.  It is re-emitted here, rather than measured through the opt_v3 or gap0
;   symbol, so that all five arms of THIS sweep sit in one image built by one
;   link and cannot differ by section placement - a confound this project has
;   measured before.  d1gap0 landing on Phase B's 18798 ticks is the check that
;   this image reproduces the thing being explained; if it does not, the sweep is
;   void and the discrepancy is the finding.
;
; INSTRUCTION COUNTS (per two samples, for the bench's static-vs-measured gate)
;   The slots are per SAMPLE here, not per pair as in gap.s, so the step is two
;   instructions per pair, not one:
;
;     d1gap0  17   (= opt_v3)          8.5 instr/sample
;     d1gap1  19                       9.5
;     d1gap2  21                      10.5
;     d1gap3  23                      11.5
;
; EACH ARM GETS ITS OWN CODE SECTION
;   --gc-sections then drops the whole set from any image that does not call
;   them, which is every shipping configuration.
;==============================================================================

        .include "dspcommon.inc"

;------------------------------------------------------------------------------
; DF2T_D1GAP_KERNEL - emit one arm.
;
;   \n    number of independent issue slots inserted, in EACH sample, between
;         that sample's d1 partial producer and its d1 final consumer
;
; The body is opt_v3's verbatim; see that file's header for the register rotation
; argument.  Comments here are kept to what differs, so that the files cannot
; drift in their explanation of the same schedule.
;------------------------------------------------------------------------------
.macro  DF2T_D1GAP_KERNEL n

        .section .dspic33cmsisdsp_df2t_d1gap\n, code
        .global _biquad_cascade_df2T_f32_dspic33ak_d1gap\n

_biquad_cascade_df2T_f32_dspic33ak_d1gap\n:

        ; Refuse odd or zero blockSize before anything is written or pushed, so
        ; the refusal path is a plain return.  opt_v3's reasoning, unchanged.
        and.l       W3, #1, W7
        cp.l        W7, #0
        bra         nz, L_d1gap_refuse\n

        cp.l        W3, #0
        bra         z, L_d1gap_refuse\n

        push.l      F8
        push.l      FCR

        mov.l       #0x7F, W7
        floatsetup  W7

        clr         W4
        mov.b       [W0+0], W4        ; numStages
        mov.l       [W0+4], W5        ; coefficients
        mov.l       [W0+8], W6        ; state

        ; Pair count = blockSize / 2, via the long form for the reason opt_v3
        ; documents: xc-dsc v3.31.01's objdump mis-renders the same-register
        ; short form's source field as the shift amount, which makes a correct
        ; prologue read like a pointer defect during a disassembly review.
        lsr.l       W3, #1, W0
        mov.l       W0, W3

        mov.l       W2, [W15++]       ; [W15-8] = original pDst
        mov.l       W3, [W15++]       ; [W15-4] = pair count

L_d1gap_startFilter\n:
        mov.l       [W5++], F0        ; b0
        mov.l       [W5++], F1        ; b1
        mov.l       [W5++], F2        ; b2
        mov.l       [W5++], F3        ; a1
        mov.l       [W5++], F4        ; a2

        mov.l       [W6],   F5        ; d1
        mov.l       [W6+4], F6        ; d2

L_d1gap_startPairs\n:

        ; ---- sample A: d1 arrives in F5, leaves in F8 ------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F5        ; F5 = old d1 + b0*x = y
        mov.s       F6, F8            ; F8 = old d2
        mul.s       F2, F7, F6        ; F6 = b2*x
        mac.s       F1, F7, F8        ; F8 = old d2 + b1*x   <- d1 PARTIAL
        mov.l       F5, [W2++]        ; *pDst++ = y

        ; ---- the variable under test, sample A -------------------------------
        ; \n independent issue slots between this sample's d1 partial producer
        ; and the d1 final consumer below.  Together with sample B's identical
        ; insertion, this is the ONLY difference between the four arms.
        ;
        ; `nopr`, not `nop`: four bytes, so the assembler never needs an
        ; alignment pad before the four-byte FPU instruction that follows.  See
        ; the header - objdump prints `nopr` as `neop` too, so check the
        ; encoding (`03 00 00 fe` executes, `00 00` is the pad).
    .if \n > 0
        .rept \n
        nopr
        .endr
    .endif

        mac.s       F3, F5, F8        ; F8 = new d1   <- d1 FINAL, reads F8
        mac.s       F4, F5, F6        ; F6 = new d2

        ; ---- sample B: d1 arrives in F8, leaves in F5 ------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F8        ; F8 = old d1 + b0*x = y
        mov.s       F6, F5            ; F5 = old d2
        mul.s       F2, F7, F6        ; F6 = b2*x
        mac.s       F1, F7, F5        ; F5 = old d2 + b1*x   <- d1 PARTIAL
        mov.l       F8, [W2++]        ; *pDst++ = y

        ; ---- the variable under test, sample B -------------------------------
        ; Same N as sample A.  Both samples are given the gap because the two
        ; halves of the ping-pong are the same schedule with the registers
        ; exchanged; giving only one of them the slots would make the pair
        ; asymmetric and the per-sample average unreadable.
    .if \n > 0
        .rept \n
        nopr
        .endr
    .endif

        mac.s       F3, F8, F5        ; F5 = new d1   <- d1 FINAL, reads F5
        mac.s       F4, F8, F6        ; F6 = new d2

        dtb         W3, L_d1gap_startPairs\n

        mov.l       [W15-4], W3       ; restore pair count

        mov.l       F5, [W6++]        ; pState[0] = d1
        mov.l       F6, [W6++]        ; pState[1] = d2

        mov.l       [W15-8], W1       ; next stage reads this stage's output
        mov.l       [W15-8], W2

        dtb         W4, L_d1gap_startFilter\n

        sub.l       #8, W15
        pop.l       FCR
        pop.l       F8
        return

L_d1gap_refuse\n:
        return

.endm

;------------------------------------------------------------------------------
; The four arms.
;
; d1gap0 = opt_v3's schedule (17 instructions / 2 samples)
; d1gap1 = +1 slot per sample (19)
; d1gap2 = +2 slots per sample (21)
; d1gap3 = +3 slots per sample (23)
;------------------------------------------------------------------------------
        DF2T_D1GAP_KERNEL 0
        DF2T_D1GAP_KERNEL 1
        DF2T_D1GAP_KERNEL 2
        DF2T_D1GAP_KERNEL 3

;==============================================================================
; End of file
;==============================================================================
