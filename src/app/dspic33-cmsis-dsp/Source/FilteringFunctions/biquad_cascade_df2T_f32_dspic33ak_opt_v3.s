;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_opt_v3.s
;
; dsPIC33AK FPU DF2T biquad cascade, single-channel block processor.
; Two-sample register ping-pong variant of opt_v1, EVEN blockSize ONLY.
;
; MEASURED ON HARDWARE 2026-09-22: THIS KERNEL IS SLOWER THAN opt_v1. NOT ADOPTED.
;   AK506 Curiosity Nano, Tier 1, ap84, 1 ch x 32 frames x 84 sections:
;       opt_v1   11.484 cyc/sample/section   (154.3 us, N_max 90)
;       opt_v3   13.986 cyc/sample/section   (187.9 us, N_max 74)   +21.8 %
;   The same 1.218 ratio appears at all ten sweep points from 1 to 84 sections, and
;   the 64->84 marginal slope (11.469 vs 13.969) matches the whole-call figure, so
;   the loss is in the steady-state loop, not in per-call overhead.
;
;   So the instruction count below is real and so is its failure: 10 -> 8.5
;   instructions/sample (-15 %) bought +21.8 % cycles. Cycles per instruction went
;   1.148 -> 1.645, i.e. this schedule added roughly 4 cycles/sample of stall to
;   save 1.5 instructions.
;
;   WHY, MEASURED WITH THE PERFORMANCE MONITOR 2026-09-22 (and the answer is NOT
;   the cross-sample interval this header originally blamed):
;
;     Performance Monitor, per sample/section, ap84, 84 sections:
;       opt_v1   ev8 FPU dependency stall 0.000   ev9 FPU read stall 0.999
;       this     ev8 FPU dependency stall 3.999   ev9 FPU read stall 0.000
;
;   The stall IS an FPU register dependency - but widening the A->B gap does not
;   touch it. Four kernels built with 0/1/2/3 extra issue slots at exactly that
;   edge (biquad_cascade_df2T_f32_dspic33ak_gap.s) all measured ev8 = 3.999, and
;   each added slot cost a full cycle with none recovered. The decisive figure:
;   gap3 has the SAME instruction count as opt_v1 (10.439 vs 10.433 instr/sample/
;   section measured) and is still 34.8 % slower (15.486 vs 11.484).
;
;   So the mechanism is the REGISTER ROTATION ITSELF, not the interval. This
;   kernel accumulates y into F5, the register that arrived holding d1, and the
;   next three instructions all read F5. opt_v1 builds y in F8 and keeps d1/d2
;   elsewhere, which is why it shows no ev8 at all - its one stall cycle is ev9,
;   the CPU waiting to read an F-register when it stores y.
;
;   THE ORIGINAL CLAIM IN THIS HEADER WAS WRONG. It said opt_v1's two mov.s are
;   "the interval that hides mac.s result latency on the loop-carried edge". The
;   gap sweep refuted that: the interval is not what opt_v1 gains from. The mov.s
;   are not waste either - but what they buy is a register assignment with no FPU
;   internal dependency, not spacing. Full result and the next step:
;   [internal] iir_df2t_gap_sweep_pmu_2026-09-22.md
;
;   WHERE THE STALL IS, MEASURED 2026-09-22 (Phase C, a second sweep at a
;   different edge - biquad_cascade_df2T_f32_dspic33ak_d1gap.s):
;
;     Inside each sample, d1 is accumulated in two steps through ONE F-register,
;     with only the y store between them:
;
;       mac.s F1,F7,F8   ; d1 partial  (sample B: mac.s F1,F7,F5)
;       mov.l F5,[W2++]  ; store y     (sample B: mov.l F8,[W2++])
;       mac.s F3,F5,F8   ; d1 final    (sample B: mac.s F3,F8,F5)
;
;     Putting 0/1/2/3 independent issue slots in that position, in BOTH samples,
;     made event 8 fall by exactly one cycle per slot - 3.999 / 2.999 / 1.999 /
;     0.999 - and the cycle count did NOT move (18798 / 18800 / 18802 / 18798
;     ticks). Three added instructions bought three stall cycles back, so the
;     issue slots there are real and currently wasted.
;
;     That is the OPPOSITE response from the cross-sample edge above, which was
;     flat in event 8 and cost a full cycle per slot. So the two edges are not
;     interchangeable, and this region is where event 8 lives.
;
;     NOT PROVEN: that the d1 partial -> d1 final edge alone is responsible. A
;     slot in that position also delays the d2 accumulation right behind it
;     (mac.s F4,...), so d1 and d2 are not separated by this experiment. Doing
;     that needs an arm that moves an OPERAND, not one that widens a gap.
;     [internal] iir_df2t_d1gap_sweep_pmu_2026-09-22.md
;
;   Correctness was never the problem: the KAT passed clean on both banks at the
;   existing tolerances with all six negative tests caught, and the dispatch
;   wrapper matched opt_v1 bit-for-bit at n = 1, 2, 6, 7, 31, 32. This is a
;   correct kernel that is slower, which is why it is kept rather than deleted -
;   it is the evidence for the paragraph above.
;
;   KEPT AS A RECORD, NOT AS A CANDIDATE. opt_v1 remains Best-available. Nothing
;   calls this in any shipping configuration, and -ffunction-sections +
;   --gc-sections drops it from any image that does not reference it. Before
;   reviving it or attempting the three-register rotation mentioned at the bottom
;   of this header, read section 6.5 of
;   [internal] iir_df2t_opt_v3_pingpong_status_2026-09-22.md - the
;   measurement argues against simply removing more instructions.
;
; RELATION TO THE OTHER KERNELS IN THIS DIRECTORY
;   opt_v1 is the Best-available cell and is NOT modified by this file.
;   opt_v2 already exists and is a different experiment (mul.s + add.s in place
;   of mac.s); it raised a BUS ERROR on hardware and is recorded as unavailable
;   (see asrc_fir_kernel_bench.c).  This kernel is therefore v3, not v2 - the
;   name was free and reusing v2 would have collided with a symbol that is still
;   referenced.
;
; THIS SYMBOL IS THE EVEN-ONLY FAST PATH, NOT THE PUBLIC ENTRY POINT
;   blockSize MUST be even and non-zero.  Odd block lengths are the wrapper's
;   job: biquad_cascade_df2T_f32_dspic33ak_opt_v3_block() in
;   biquad_df2T_opt_v3_dispatch.c routes them to opt_v1 and keeps opt_v1's full
;   1..512 contract for callers.  Callers should use the wrapper; this symbol is
;   exposed so that a bench can time the fast path with no dispatch in the
;   measured region.
;
;   The split is deliberate and is the whole reason this file has no tail code.
;   An in-kernel odd-sample tail would put a test and a branch on the path of
;   every stage - 84 of them per channel - to serve a case no caller in this tree
;   produces (APP_BLOCK_FRAMES and IIR_BENCH_FRAMES are both 32).  Keeping the
;   decision OUTSIDE the stage loop means the 32-frame production and Tier 1
;   paths pay nothing for odd support beyond one branch per CALL, and the kernel
;   under measurement is exactly the loop being evaluated.
;
;   Guarding rather than trusting: an odd blockSize reaching this symbol directly
;   would drop its last sample and leave that slot of the output buffer unwritten
;   - a silent data defect.  So the prologue REFUSES an odd or zero blockSize by
;   returning without touching pDst or the state, and the wrapper never sends
;   one.  A refusal is recoverable and inspectable; a half-written block is not.
;
; WHAT IS DIFFERENT FROM opt_v1
;   opt_v1's steady-state body spends two instructions per sample copying state
;   between FPU registers:
;
;       mov.s   F5, F8      ; old d1 -> y accumulator
;       mov.s   F6, F5      ; old d2 -> d1 accumulator
;
;   Those copies exist because mac.s accumulates INTO its third operand, so the
;   register that holds a state value has to be the register the next value is
;   accumulated into.  DF2T is a three-stage relay - old d1 becomes y, old d2
;   becomes d1, and d2 is generated fresh from b2*x - so keeping a fixed
;   register per variable forces a copy on every link of the relay.
;
;   This kernel unrolls two samples and lets F5 and F8 SWAP the d1 / scratch
;   roles between them, so the d1 link needs no copy: the value mac.s leaves in
;   F8 at the end of sample A is used directly as sample B's y accumulator.
;   F6 stays d2 throughout.
;
;   ONE copy per sample remains, and cannot be removed in a two-register
;   rotation: d2's old value must reach whichever of F5/F8 is about to become
;   the d1 accumulator, and mac.s cannot write a register other than the one it
;   reads as the addend.  Removing it as well needs a THREE-register rotation
;   (period 3), which is recorded as the next candidate below rather than
;   attempted here.
;
; INSTRUCTION COUNT (CONFIRMED AGAINST THE LINKED IMAGE 2026-09-22)
;   Pair loop body: 16 instructions + 1 dtb = 17 per two samples, i.e.
;   8.5 instructions/sample/section against opt_v1's 10.  Verified with
;   xc-dsc-objdump -d on the linked production ELF, not inferred from this
;   source: the assembler inserts no neop anywhere inside the loop.
;
;   This count is correct and it did not help.  Cycles went the other way - see
;   the measurement at the top.  Quote the count only alongside that result;
;   on its own it reads as an improvement, which it is not.
;
; WHY FEWER INSTRUCTIONS MAY NOT MEAN FEWER CYCLES, AND THE SPECIFIC RISK HERE
;   (WRITTEN BEFORE THE MEASUREMENT. THE RISK BELOW IS THE ONE THAT MATERIALISED -
;   see the hardware result at the top of this header.)
;
;   On this core the FPU hazard interlock stalls whether or not an instruction
;   occupies the slot, so a removed instruction is not automatically a removed
;   cycle.  This project has measured that three times in widen_ctrl.c
;   ([internal] widen_ctrl_load_reduction.md sections 5 and 7.6),
;   where a static -41 % came back as -16 % on hardware for a change of exactly
;   this kind - a 2x unroll that retired all-pass state copies.  Here it did not
;   merely shrink the gain, it reversed the sign.
;
;   The concrete risk this schedule introduces is a TIGHTER cross-sample
;   dependency than opt_v1 had.  Sample A's last two instructions produce
;   d1 (in F8) and d2 (in F6); sample B consumes F8 two instructions later and
;   F6 two instructions after that:
;
;       A7  mac.s F3,F5,F8   -> writes d1
;       A8  mac.s F4,F5,F6   -> writes d2
;       B1  mov.l [W1++],F7
;       B2  mac.s F0,F7,F8   -> READS d1   (2 instructions after A7)
;       B3  mov.s F6,F5      -> READS d2   (2 instructions after A8)
;
;   The same two edges in opt_v1 are THREE instructions apart, because its dtb
;   and its two mov.s sit between the producer and the consumer.  So opt_v1 has
;   one more slot of cover for mac.s result latency on the loop-carried edge
;   than this kernel does.  Whether that costs anything is a measurement, not a
;   deduction: it is the first thing to look at if the hardware A/B comes back
;   flat or worse.
;
;   IT CAME BACK WORSE - AND THEN THE COUNTERS SAID THIS SUSPECT WAS INNOCENT.
;   The arithmetic was consistent with it (about 4 cycles/sample of new stall,
;   against an FPU latency the datasheet caps at 4 cycles for everything except
;   FDIV/FSQRT, DS70005591C 4.6.4), which is exactly why it needed measuring
;   rather than believing.  It was measured on 2026-09-22 with the Performance
;   Monitor plus a four-point spacing sweep, and THE EDGE BELOW IS NOT THE CAUSE:
;   adding 1, 2 or 3 independent issue slots right here changed ev8 not at all
;   (3.999 in every case) and cost a full cycle per slot.
;
;   Keep reading this block as the schedule's DESCRIPTION - the distances stated
;   are correct - but not as its explanation.  See the top of this header for what
;   the counters actually attribute the stall to,
;   [internal] iir_df2t_gap_sweep_pmu_2026-09-22.md for this sweep, and
;   [internal] iir_df2t_d1gap_sweep_pmu_2026-09-22.md for the second
;   one, which found the responsive edge INSIDE each sample instead.
;
; ARITHMETIC IS UNCHANGED, SO THE RESULT SHOULD BE BIT-IDENTICAL TO opt_v1
;   Every sample performs the same five operations on the same operand values in
;   the same order as opt_v1:
;
;       y  = fma(b0, x, d1)
;       t1 = fma(b1, x, d2)
;       t2 = b2 * x
;       d1 = fma(a1, y, t1)
;       d2 = fma(a2, y, t2)
;
;   Only the register NAMES differ between sample A and sample B.  No product is
;   split, fused, reassociated or reordered relative to opt_v1, so the output is
;   expected to match opt_v1 bit for bit and the existing known-answer test
;   tolerances must not be relaxed to accept it.
;
; C prototype:
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_opt_v3(
;       const arm_biquad_cascade_df2T_instance_f32 *S,
;       const float32_t *pSrc,
;       float32_t *pDst,
;       uint32_t blockSize      /* MUST be even and non-zero */
;   );
;
; Calling convention, instance layout, coefficient layout and state layout are
; all opt_v1's, unchanged:
;   W0 = S, W1 = pSrc, W2 = pDst, W3 = blockSize
;   [W0 + 0] = numStages, uint8_t
;   [W0 + 4] = coefficient pointer
;   [W0 + 8] = state pointer
;   Coefficients per stage: b0, b1, b2, a1, a2
;   State per stage:        d1, d2
;
; Difference equation, same as CMSIS DF2T style:
;   y  = b0*x + d1
;   d1 = b1*x + d2 + a1*y
;   d2 = b2*x      + a2*y
;
; Preconditions:
;   - blockSize even and in 2..512.  Odd or zero returns with nothing written.
;   - numStages >= 1.  numStages = 0 is invalid, exactly as in opt_v1: dtb would
;     wrap the counter and run for a very large number of iterations.
;   - pSrc and pDst point at valid contiguous float32 buffers of at least
;     blockSize samples.
;
; STATE REGISTER INVARIANT
;   d1 is in F5 and d2 in F6 at the top of every sample pair, and again after
;   every pair completes, because sample B restores the assignment sample A
;   swapped.  That is what lets the per-stage state store at the end be opt_v1's,
;   unchanged, and it is also why the body needs no fix-up between pairs.
;
; THREE-REGISTER ROTATION: THE MEASUREMENT ARGUES AGAINST IT
;   A three-register rotation (F5 -> F6 -> F8 -> F5) would retire the remaining
;   mov.s as well, reaching 8 + 1/3 instructions/sample in the limit, because
;   the d2 link would also land in a register that is about to be retired.
;   Mechanically it needs a period-3 body, and every current caller uses
;   blockSize = 32 (APP_BLOCK_FRAMES and IIR_BENCH_FRAMES), so 32 mod 3 = 2
;   leaves a two-sample remainder - which the dispatch wrapper could absorb.
;
;   That was written as the next step BEFORE this kernel was measured.  The
;   measurement has now been made, and it argues against a three-way rotation more
;   strongly than the original cycle count did:
;
;     gap3 carries the SAME instruction count as opt_v1 and is 34.8 % slower.
;
;   So instruction count is not the binding constraint at all - a rotation that
;   reaches 8.33 instructions/sample still loses if it keeps the 5 cycles of FPU
;   dependency stall that the two-register rotation introduces.  And opt_v1's
;   measured CPI is 1.102 with 1.033 cycles of total stall per sample/section, of
;   which 0.999 is ev9: there is less than one cycle available to win.
;
;   Read [internal] iir_df2t_gap_sweep_pmu_2026-09-22.md section 4
;   before attempting it.  The direction it recommends is not "fewer
;   instructions" and not "wider spacing": it is to keep opt_v1's property that y
;   is accumulated in a register other than the one that arrived holding d1, which
;   is what the counters distinguish.
;==============================================================================

;------------------------------------------------------------------------------
; Include Microchip/CMSIS DSP assembly helper macros.
; floatsetup is provided as a macro, not as a raw assembler mnemonic.
;------------------------------------------------------------------------------
        .include "dspcommon.inc"

        .text
        .global _biquad_cascade_df2T_f32_dspic33ak_opt_v3

_biquad_cascade_df2T_f32_dspic33ak_opt_v3:

;------------------------------------------------------------------------------
; Refuse an odd or zero blockSize before anything is written.
;
; Done first, and with no FPU or stack state established yet, so the refusal path
; is a plain return: the caller's buffers and the instance state are exactly as
; they were.  See the header - a silently half-written output block would be far
; worse than a call that visibly did nothing.
;------------------------------------------------------------------------------
        and.l       W3, #1, W7        ; W7 = blockSize & 1
        cp.l        W7, #0
        bra         nz, L_refuse_v3   ; odd: the wrapper should have taken opt_v1

        cp.l        W3, #0
        bra         z, L_refuse_v3    ; zero: dtb would wrap the pair counter

;------------------------------------------------------------------------------
; Save the callee-saved FPU register used by this function, and FCR.
; F8 is the only one: F0-F7 are caller-saved, and this kernel uses no F9.
;------------------------------------------------------------------------------
        push.l      F8
        push.l      FCR

;------------------------------------------------------------------------------
; Mask all FPU exceptions, set rounding mode to default, clear SAZ/FTZ.
; Matches opt_v1 and the original library.
;------------------------------------------------------------------------------
        mov.l       #0x7F, W7
        floatsetup  W7

;------------------------------------------------------------------------------
; Prepare cascade pointers.
;------------------------------------------------------------------------------
        clr         W4                ; clear upper bits before loading uint8_t
        mov.b       [W0+0], W4        ; W4 = number of stages
        mov.l       [W0+4], W5        ; W5 = coefficient pointer
        mov.l       [W0+8], W6        ; W6 = state pointer

;------------------------------------------------------------------------------
; blockSize is even, so the pair count is exactly blockSize/2 with no remainder
; and no flag to carry.  Non-zero, because a zero blockSize was refused above.
;
; Written through W0 rather than as the obvious `lsr.l W3, #1, W3`, and NOT
; because the one-instruction form is wrong - it assembles correctly.  It is
; because xc-dsc-objdump MIS-RENDERS the two-byte same-register shift short form:
; `lsr.l W3,#1,W3` encodes as `13 22` and disassembles as `lsr.l w1,#0x1,w3`,
; i.e. it prints the SHIFT AMOUNT in place of the source register.  Verified
; 2026-09-22 by assembling the cases side by side: a different-register
; `lsr.l w1,#1,w3` is a four-byte `48 00 13 a2` and renders correctly, while
; every same-register case renders a bogus source.  (`sl.l` does not have the
; bug, which is what makes it easy to miss.)  The two-byte form has 16 bits
; total, with the register in the low nibble and the shift in the high nibble, so
; it cannot encode a second register field - the semantics are unambiguous and
; the display is simply wrong.
;
; That matters here because disassembly is one of this project's review gates. A
; kernel whose prologue appears to derive its loop count from pSrc would read as
; a serious pointer defect to anyone auditing the listing, and would cost a
; diagnosis session to clear. The long form below costs one extra instruction
; ONCE PER CALL - not per stage, not per sample - so the measured loop is
; unaffected. Do not "simplify" it back.
;------------------------------------------------------------------------------
        lsr.l       W3, #1, W0        ; W0 = blockSize / 2 = pair count
        mov.l       W0, W3            ; W3 = pair count (dtb counts on W3)

;------------------------------------------------------------------------------
; Save original pDst and the pair count.
; Both are restored after each stage, as opt_v1 restores pDst and blockSize.
;------------------------------------------------------------------------------
        mov.l       W2, [W15++]       ; [W15-8] = saved original pDst
        mov.l       W3, [W15++]       ; [W15-4] = saved pair count

;==============================================================================
; Stage loop
;==============================================================================
L_startFilter_v3:

;------------------------------------------------------------------------------
; Load coefficients for this stage.
;------------------------------------------------------------------------------
        mov.l       [W5++], F0        ; b0
        mov.l       [W5++], F1        ; b1
        mov.l       [W5++], F2        ; b2
        mov.l       [W5++], F3        ; a1
        mov.l       [W5++], F4        ; a2

;------------------------------------------------------------------------------
; Load state for this stage.  d1 -> F5, d2 -> F6: the pair loop's entry
; invariant.
;------------------------------------------------------------------------------
        mov.l       [W6],   F5        ; d1
        mov.l       [W6+4], F6        ; d2

;==============================================================================
; Sample pair loop
;
; Register roles, sample A:  F5 = d1 in / y,  F6 = d2 in-out,  F8 = d1 out
; Register roles, sample B:  F8 = d1 in / y,  F6 = d2 in-out,  F5 = d1 out
;
; F7 = x in both.  The single surviving mov.s per sample carries d2's old value
; into whichever register is about to accumulate d1; the d1 link itself needs no
; copy, which is the whole point of the swap.
;
; No test and no branch inside this loop: the even-only precondition is settled
; once per call in the prologue, which is what keeps the 32-frame path free of
; per-stage dispatch cost.
;==============================================================================
L_startPairs_v3:

; ---- sample A: d1 arrives in F5, leaves in F8 -------------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F5        ; F5 = old d1 + b0*x = y   (no copy needed)
        mov.s       F6, F8            ; F8 = old d2              (the one copy)
        mul.s       F2, F7, F6        ; F6 = b2*x, old d2 already saved in F8
        mac.s       F1, F7, F8        ; F8 = old d2 + b1*x
        mov.l       F5, [W2++]        ; *pDst++ = y
        mac.s       F3, F5, F8        ; F8 = new d1
        mac.s       F4, F5, F6        ; F6 = new d2

; ---- sample B: d1 arrives in F8, leaves in F5 -------------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F8        ; F8 = old d1 + b0*x = y
        mov.s       F6, F5            ; F5 = old d2
        mul.s       F2, F7, F6        ; F6 = b2*x
        mac.s       F1, F7, F5        ; F5 = old d2 + b1*x
        mov.l       F8, [W2++]        ; *pDst++ = y
        mac.s       F3, F8, F5        ; F5 = new d1
        mac.s       F4, F8, F6        ; F6 = new d2

; W3 is the pair count and is guaranteed non-zero by the prologue's refusal of a
; zero blockSize, so dtb executes this loop exactly that many times and leaves
; W3 = 0.  Sample B has restored d1 to F5, so the next iteration starts on
; sample A's invariant.
        dtb         W3, L_startPairs_v3

;------------------------------------------------------------------------------
; Restore the pair count for the next stage.
;------------------------------------------------------------------------------
        mov.l       [W15-4], W3

;------------------------------------------------------------------------------
; Store updated state variables.  d1 is in F5 and d2 in F6 after every completed
; pair, which is the invariant documented in the header.
;------------------------------------------------------------------------------
        mov.l       F5, [W6++]        ; pState[0] = d1
        mov.l       F6, [W6++]        ; pState[1] = d2

;------------------------------------------------------------------------------
; For the next stage, the previous output buffer is the input buffer.
; Reset both pSrc and pDst to the original pDst.
;------------------------------------------------------------------------------
        mov.l       [W15-8], W1       ; pIn  = original pDst
        mov.l       [W15-8], W2       ; pOut = original pDst

; numStages is loaded as uint8_t after clearing W4, so W4[31:0] holds a valid
; positive stage count. dtb executes this loop exactly numStages times and
; leaves W4 = 0. numStages = 0 is invalid by design.
        dtb         W4, L_startFilter_v3

;==============================================================================
; Function epilogue
;==============================================================================
L_completedIIR_v3:
        sub.l       #8, W15           ; discard saved pDst and pair count

        pop.l       FCR
        pop.l       F8

        return

;==============================================================================
; Refusal exit.
;
; Reached only from the prologue, before push.l / floatsetup, so there is nothing
; to unwind and nothing has been written.
;==============================================================================
L_refuse_v3:
        return

;==============================================================================
; End of file
;==============================================================================
