;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_gap.s
;
; opt_v3's two-sample ping-pong DF2T loop, emitted FOUR TIMES with 0, 1, 2 and 3
; independent issue slots inserted at ONE place: between sample A's last state
; producer and sample B's first consumer.
;
; WHY THIS FILE EXISTS
;   opt_v3 removed 15 % of the inner-loop instructions and measured 21.8 % SLOWER
;   than opt_v1 on hardware (2026-09-22).  The leading explanation was that
;   opt_v1's two mov.s are not waste but the interval that hides mac.s result
;   latency across the loop-carried edge - opt_v3 puts sample A's producers only
;   two instructions ahead of sample B's consumers where opt_v1 has three.
;
;   That was an argument from instruction distance, not a measurement.  These
;   four kernels turn it into one: if the cross-sample dependency is what costs
;   the cycles, then ADDING independent slots at exactly that edge must buy them
;   back, and the cycle count must fall as the gap widens until the producer's
;   latency is covered.  If widening the gap changes nothing, the hypothesis is
;   wrong and the stall is somewhere else - which is a result, not a failure.
;
;   This is deliberately the opposite direction from every previous attempt in
;   this tree: it makes the loop LONGER on purpose.  gap3 is 20 instructions per
;   two samples, exactly opt_v1's 10/sample, with none of opt_v1's copies doing
;   useful work - so if gap3 lands at opt_v1's cycle count, the interval is the
;   whole story and the copies never mattered as copies.
;
; WHY ONE MACRO AND NOT FOUR HAND-WRITTEN KERNELS
;   "The only difference between the arms is the spacing" has to be TRUE, not
;   reviewed.  Four copies of a 16-instruction body would let a register name or
;   an operand order drift between arms, and the drift would show up as a cycle
;   difference attributed to spacing.  Emitting all four from ONE body makes that
;   impossible by construction: the .rept below is the only text that differs,
;   and the disassembly gate in the bench checks the instruction counts came out
;   17/18/19/20 as intended.
;
; WHERE THE SLOTS GO, AND WHY ONLY THERE
;   Inside sample A, between A8 (the last mac.s, which writes d2) and B1:
;
;       A7  mac.s F3,F5,F8   -> writes d1 (F8), consumed by B2
;       A8  mac.s F4,F5,F6   -> writes d2 (F6), consumed by B3
;       --- N independent slots inserted HERE ---
;       B1  mov.l [W1++],F7
;       B2  mac.s F0,F7,F8   -> READS d1
;       B3  mov.s F6,F5      -> READS d2
;
;   Nothing is inserted between sample B's tail and the next iteration's sample
;   A, because that edge already carries the dtb and is therefore not the tight
;   one.  Inserting at both edges would double the added instructions and make
;   the sweep unable to say WHICH edge paid.
;
; THE SPACER IS `nopr`, AND THE CHOICE WAS FORCED BY MEASUREMENT, NOT PREFERENCE
;   Three mnemonics are candidates and only one is right here.  DS70005591D
;   Table 38-2 rows 67-68, cross-checked by assembling each one on this exact
;   toolchain (xc-dsc v3.31.01) before this file was written:
;
;     nop   `00 01`        1 word    / 1 cycle   - executes, but only TWO BYTES
;     nopr  `03 00 00 fe`  1 word    / 1 cycle   - executes, FOUR BYTES  <- used
;     neop  `00 00`        0.5 word  / 0 CYCLES  - does NOT execute
;
;   DO NOT USE `neop`.  The datasheet calls it a "None executable NOP (16-bit
;   instruction pad)" at zero cycles.  An instruction that does not execute
;   cannot provide an issue slot, so a `neop` sweep would measure nothing and
;   would look exactly like a refutation of the hypothesis.
;
;   `nop` LOOKED right and is the trap.  It is two bytes, and the FPU
;   instructions on either side of the gap are four-byte and four-byte-aligned,
;   so after an ODD number of `nop`s the assembler inserts a two-byte `neop`
;   pad to realign.  Measured: gap1 and gap3 came out with `nop ... neop` in the
;   body while gap0 and gap2 did not.  That is benign - it is alignment, not a
;   schedule change, and the pad is documented at zero cycles - but it makes the
;   odd arms structurally different from the even ones, and "did the pad cost a
;   cycle?" would become a second unknown inside the one measurement that exists
;   to remove an unknown.  `nopr` is four bytes, so no pad is ever inserted and
;   every arm differs from its neighbour by exactly one issue slot.
;
;   NOTE FOR ANYONE READING THE LISTING: xc-dsc objdump prints `nopr` AS `neop`.
;   Both v3.31.01 and v4.00 do it, so it is not the v3.31.01-only shift-decode
;   bug this kernel family already documents.  Tell them apart by the ENCODING,
;   not the mnemonic: the executing four-byte spacer is `03 00 00 fe`, the
;   non-executing two-byte pad is `00 00`.  A body showing `03 00 00 fe` is
;   correct; one showing `00 00` inside the gap is not what this file emits.
;
;   Whether the spacer retires is not left to the datasheet either: the bench
;   reads PMU event 2 (instructions completed), so the measured dynamic
;   instruction count per arm settles it on hardware.  17/18/19/20 per pair
;   confirms one slot per step; anything else invalidates the sweep.
;
;   `nopr` touches no F-register, no W register, no pointer, no loop counter and
;   no status bit, so it cannot perturb the arithmetic or the DF2T state.  That
;   is the whole requirement for a spacer: occupy an issue slot, change nothing.
;
; ARITHMETIC IS OPT_V3'S, WHICH IS OPT_V1'S
;   Every arm performs the same five operations per sample, on the same operand
;   values, in the same order as opt_v1 - only register NAMES rotate, exactly as
;   in opt_v3.  So all four arms must be bit-identical to opt_v1 and to each
;   other, and the bench checks that before it reports a single cycle count.  A
;   spacing change that alters an output bit is a defect in this file, not a
;   rounding difference, and no tolerance may be relaxed to accept it.
;
; EVEN-ONLY, SAME AS opt_v3
;   blockSize must be even and non-zero; an odd or zero value returns having
;   written nothing.  There is no odd-sample tail here for opt_v3's reason: the
;   test belongs outside the stage loop, and these kernels exist to be measured
;   at blockSize = 32, which every caller in this tree uses.  The bench calls
;   them directly rather than through a dispatch wrapper, so the timed region is
;   the loop and nothing else.
;
; C prototypes (all four identical to opt_v1's / opt_v3's):
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_gap0(
;       const arm_biquad_cascade_df2T_instance_f32 *S,
;       const float32_t *pSrc, float32_t *pDst, uint32_t blockSize );
;   ... likewise _gap1, _gap2, _gap3.
;
;   W0 = S, W1 = pSrc, W2 = pDst, W3 = blockSize
;   [W0+0] = numStages (uint8_t), [W0+4] = coeffs, [W0+8] = state
;   Coefficients per stage: b0, b1, b2, a1, a2.  State per stage: d1, d2.
;
; gap0 IS opt_v3
;   Same instruction sequence, same register rotation, same 17 instructions per
;   pair.  It is re-emitted here rather than measured through the existing
;   opt_v3 symbol so that all five arms in the sweep sit in ONE image, built by
;   ONE link, and cannot differ by section placement - a confound this project
;   has measured before (a stage cost moved 9.2 us on an 83->84 layout change).
;   opt_v3 itself is untouched and remains the record of the original result;
;   gap0 landing on its 13.986 is the cross-check that this file reproduces it.
;
; EACH ARM GETS ITS OWN CODE SECTION
;   --gc-sections then drops the whole set from any image that does not call
;   them, which is every shipping configuration.  Same arrangement as the ASRC
;   candidate kernels in BasicMathFunctions/.
;==============================================================================

        .include "dspcommon.inc"

;------------------------------------------------------------------------------
; DF2T_GAP_KERNEL - emit one arm.
;
;   \n    number of independent issue slots inserted at the A->B edge
;
; The body is opt_v3's verbatim; see that file's header for the register rotation
; argument.  Comments here are kept to what differs, so that the two files
; cannot drift in their explanation of the same schedule.
;------------------------------------------------------------------------------
.macro  DF2T_GAP_KERNEL n

        .section .dspic33cmsisdsp_df2t_gap\n, code
        .global _biquad_cascade_df2T_f32_dspic33ak_gap\n

_biquad_cascade_df2T_f32_dspic33ak_gap\n:

        ; Refuse odd or zero blockSize before anything is written or pushed, so
        ; the refusal path is a plain return.  opt_v3's reasoning, unchanged.
        and.l       W3, #1, W7
        cp.l        W7, #0
        bra         nz, L_gap_refuse\n

        cp.l        W3, #0
        bra         z, L_gap_refuse\n

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

L_gap_startFilter\n:
        mov.l       [W5++], F0        ; b0
        mov.l       [W5++], F1        ; b1
        mov.l       [W5++], F2        ; b2
        mov.l       [W5++], F3        ; a1
        mov.l       [W5++], F4        ; a2

        mov.l       [W6],   F5        ; d1
        mov.l       [W6+4], F6        ; d2

L_gap_startPairs\n:

        ; ---- sample A: d1 arrives in F5, leaves in F8 ------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F5        ; F5 = old d1 + b0*x = y
        mov.s       F6, F8            ; F8 = old d2
        mul.s       F2, F7, F6        ; F6 = b2*x
        mac.s       F1, F7, F8        ; F8 = old d2 + b1*x
        mov.l       F5, [W2++]        ; *pDst++ = y
        mac.s       F3, F5, F8        ; F8 = new d1   <- consumed by B2
        mac.s       F4, F5, F6        ; F6 = new d2   <- consumed by B3

        ; ---- the variable under test ----------------------------------------
        ; \n independent issue slots between sample A's producers and sample B's
        ; consumers.  This is the ONLY difference between the four arms.
        ;
        ; `nopr`, not `nop`: four bytes, so the assembler never needs an
        ; alignment pad before the four-byte FPU instruction that follows.  See
        ; the header - `nop` is two bytes and the odd arms picked up a `neop`
        ; pad, and objdump prints `nopr` as `neop` too (encoding
        ; `03 00 00 fe`, versus `00 00` for the real pad).
    .if \n > 0
        .rept \n
        nopr
        .endr
    .endif

        ; ---- sample B: d1 arrives in F8, leaves in F5 ------------------------
        mov.l       [W1++], F7        ; x = *pSrc++
        mac.s       F0, F7, F8        ; F8 = old d1 + b0*x = y
        mov.s       F6, F5            ; F5 = old d2
        mul.s       F2, F7, F6        ; F6 = b2*x
        mac.s       F1, F7, F5        ; F5 = old d2 + b1*x
        mov.l       F8, [W2++]        ; *pDst++ = y
        mac.s       F3, F8, F5        ; F5 = new d1
        mac.s       F4, F8, F6        ; F6 = new d2

        dtb         W3, L_gap_startPairs\n

        mov.l       [W15-4], W3       ; restore pair count

        mov.l       F5, [W6++]        ; pState[0] = d1
        mov.l       F6, [W6++]        ; pState[1] = d2

        mov.l       [W15-8], W1       ; next stage reads this stage's output
        mov.l       [W15-8], W2

        dtb         W4, L_gap_startFilter\n

        sub.l       #8, W15
        pop.l       FCR
        pop.l       F8
        return

L_gap_refuse\n:
        return

.endm

;------------------------------------------------------------------------------
; The four arms.
;
; gap0 = opt_v3's schedule (17 instructions / 2 samples)
; gap1 = +1 slot            (18)
; gap2 = +2 slots           (19)
; gap3 = +3 slots           (20) = opt_v1's instruction count per sample
;------------------------------------------------------------------------------
        DF2T_GAP_KERNEL 0
        DF2T_GAP_KERNEL 1
        DF2T_GAP_KERNEL 2
        DF2T_GAP_KERNEL 3

;==============================================================================
; End of file
;==============================================================================
