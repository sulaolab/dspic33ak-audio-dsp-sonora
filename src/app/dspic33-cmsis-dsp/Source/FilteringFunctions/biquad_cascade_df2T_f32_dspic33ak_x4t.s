;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x4t.s
;
; x3td WITH A FOURTH CHANNEL. THE ARITHMETIC AND THE ROTATION ARE UNCHANGED.
;
; FOUR CHANNELS, THREE SAMPLES PER ITERATION, THREE WORK REGISTERS PER CHANNEL,
; NO COPY INSTRUCTIONS, AND EVERY REGISTER ON THE PART IN USE.
;
; A CANDIDATE. This is the kernel the three-channel work existed to make
; possible, and its value is NOT its per-kernel number.
;
; WHY FOUR CHANNELS IS THE POINT
;   The audio path filters FOUR channels. A three-channel kernel has to put the
;   fourth through something else, and the only other thing is opt_v1 at 11.485 -
;   so the four-channel average is dragged up no matter how fast the three-channel
;   kernel gets:
;
;       x3td x3 + opt_v1 x1   (11.485 + 3 x 7.594) / 4 = 8.567
;       x4t, one kernel                                  ~7.55 predicted
;
;   Against the shipping x2's 9.921 that is -13.7 % versus -23.9 %. The whole
;   reason to spend a fourth channel's registers is to stop paying opt_v1 once.
;
; WHAT WAS UNKNOWN WHEN THIS WAS WRITTEN, AND WHAT WAS NOT
;   x3t established the three-register rotation (7.888) and x3td established that
;   `dtb` loop control costs nothing and opens no stall (7.594, ev8 = 0.000). Both
;   were measured BEFORE this file was written, deliberately and in that order:
;   it means the only untested thing here is the fourth channel itself. If a stall
;   appears in this kernel it is the channel count, because nothing else changed.
;
; THE REGISTER FILE IS NOW FULL, IN BOTH BANKS
;
;   F: 4 channels x (5 coefficients + 3 work) = 32 = F0..F31, exactly.
;
;   W: four channels want 18 - 4 pSrc, 4 pDst, 4 coeff, 4 state, a sample counter
;      and a stage counter - and the part has 15 usable. W15 is the stack pointer
;      and W16 DOES NOT EXIST (the assembler was asked: "Invalid operands").
;      THREE SHORT.
;
;   The 18 are not all live at once, and splitting them by WHEN they are touched
;   is what makes the kernel fit:
;
;       body-live   pSrc, pDst        8    every sample, inside the dtb loop
;       loop        sample counter    1    inside the dtb loop
;       surviving   stage counter     1    across the dtb loop
;       stage-only  coeff, state      8    ONLY at a stage boundary
;
;   Ten must survive the sample loop, leaving five for eight stage-only pointers,
;   so exactly three are spilled to the frame - and WHICH three is a cost
;   decision, not an arbitrary one:
;
;       a COEFF pointer spilled costs 2 instructions per stage
;           (load it, five post-increment reads, store the advanced value back)
;       a STATE pointer spilled costs 3
;           (it is read at the stage top AND written at the stage bottom)
;
;   Three coefficient pointers, then. Six instructions per stage over 128
;   channel-samples is 0.047 each. They are loaded through W4 - the sample
;   counter, which is dead until the triple count is read at the end of the
;   prologue - so the spill consumes no register beyond the ones counted above.
;
;   x3td already proved a frame slot can carry a per-stage quantity at no
;   measurable cost (its triples and remainder live there). This is that
;   mechanism with three more slots.
;
; THE BODY - 7 INSTRUCTIONS PER CHANNEL-SAMPLE, ZERO mov.s, UNCHANGED FROM x3td
;       mov.l  [Wsrc++], C     ; C = x
;       mac.s  b0, C, A        ; A = d1 + b0*x = y          (A was d1)
;       mac.s  b1, C, B        ; B = d2 + b1*x              (B was d2; x now dead)
;       mul.s  b2, C, C        ; C = b2*x                   (self-overwrite)
;       mov.l  A, [Wdst++]     ; store y
;       mac.s  a1, A, B        ; B = new d1
;       mac.s  a2, A, C        ; C = new d2
;
;   84 instructions for twelve channel-samples, plus one `dtb`: 85 per iteration.
;
; GENERATED, NOT HAND-WRITTEN
;   Four channels x three samples is 84 arithmetic instructions whose register
;   names rotate by one position per sample. A hand-written copy would get a phase
;   wrong somewhere and the defect would present as a small numeric difference -
;   which is exactly the kind of thing this bench must not have to argue about.
;   The generator is _scratch/gen_x4t.py; correctness is established by the
;   equivalence gate against opt_v1 on all four channels, not by reading.
;
; STATIC GATES PASSED BEFORE THIS FILE WAS WRITTEN (assembled, then disassembled)
;   1. F24..F31 exist and take every form this schedule needs, including the
;      self-overwriting multiply at the very top of the file:
;         mac.s f24, f31, f29  ->  02 fc 3b 84
;         mul.s f26, f31, f31  ->  06 fd 3f 84
;   2. A NINE-WORD frame assembles, and every displacement [W15-36]..[W15-4] is
;      the 2-byte short form - so the deeper frame costs no code size per access.
;   3. A frame slot can be reloaded straight into a live loop register:
;         mov.l [W15-24], W10   then   mov.l [W10++], F24
;   4. `dtb` reaches over this body. Its displacement is a 16-bit halfword field
;      (+-64 KB): a 1200-byte backward dtb assembles, and the shipping image
;      contains `dtb w3, 0x805edc` jumping +716 bytes. This body is ~336.
;   5. The arithmetic was expanded symbolically over four samples and reproduces
;      DF2T exactly, returning to the phase-0 assignment after three:
;         y = d1 + b0*x ; d1' = d2 + b1*x + a1*y ; d2' = b2*x + a2*y
;
; blockSize CONTRACT: ANY 1..512, INCLUDING ODD
;   Three samples per iteration, so the count splits into triples plus a remainder
;   of 0, 1 or 2, and the remainder runs through a tail of the SAME seven
;   instructions one sample at a time. Unlike x2r/x3r this kernel does not narrow
;   the contract - which is why it is allowed on the audio path at all.
;
;   The triples and the remainder are computed ONCE PER CALL by subtracting three,
;   not by dividing. This ISA does have an integer divide (DIVU.l - `div.l` is
;   simply not the mnemonic on this part), but the quotient is wanted once per
;   CALL against 84 stages of sample loop, so a subtract loop of at most 170
;   iterations is both sufficient and impossible to get wrong on operand size.
;
;   THE TAIL'S ONE REAL SUBTLETY: after a tail sample the roles have rotated, so
;   d1 and d2 are NOT in the registers a single epilogue could name. The ending
;   phase is (blockSize mod 3), a RUNTIME value, so the write-back is selected by
;   branch rather than assumed. The audio path's 32 samples end at phase 2, but
;   this kernel does not get to assume that.
;
; ARITHMETIC IS OPT_V1'S, PER CHANNEL
;   The same five operations on the same operand values in the same order; only
;   which register holds a value changes, and no instruction of one channel names
;   a register of another. So each channel's output must be BIT-IDENTICAL to
;   opt_v1 run on it alone. A difference is a defect in this file, not a rounding
;   effect, and not something a tolerance may be widened to accept.
;
; WHAT IS NOT PREDICTED HERE
;   The instruction count is arithmetic and is stated: 966 per stage over 128
;   channel-samples = 7.547. THE CYCLE COUNT IS NOT PREDICTED IN THIS FILE. This
;   family has mispredicted stall three times out of four. The prediction, its
;   branches, and what each would mean are in a local pre-build prediction record
;   (not part of the shipped archive), written before this file was built and not
;   edited afterwards.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================
        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x4t, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x4t

;------------------------------------------------------------------------------
; void biquad_cascade_df2T_f32_dspic33ak_x4t(
;         const arm_biquad_cascade_df2T_instance_f32 * const *S,   ; W0
;         const float32_t * const *pSrc,                           ; W1
;         float32_t * const *pDst,                                 ; W2
;         uint32_t blockSize );                                    ; W3
;
; Array arguments for the reason x3/x3r/x3td use them: xc-dsc passes only the
; first seven 32-bit arguments in registers, and four instances plus eight
; pointers plus blockSize is thirteen. ALL FOUR INSTANCES MUST HAVE THE SAME
; numStages - S[0]'s count drives all four cascades.
;
; F-register map (coefficients then the three rotating work registers):
;   A ch: b0=F0  b1=F1  b2=F2  a1=F3  a2=F4      work F5  F6  F7
;   B ch: b0=F8  b1=F9  b2=F10 a1=F11 a2=F12     work F13 F14 F15
;   C ch: b0=F16 b1=F17 b2=F18 a1=F19 a2=F20     work F21 F22 F23
;   D ch: b0=F24 b1=F25 b2=F26 a1=F27 a2=F28     work F29 F30 F31
;
; 32 of 32 F-registers. There is no fifth channel in this topology.
;
; W-register map (15 of 15 usable; W15 is SP and W16 does not exist):
;   W1  pSrcA   W2  pDstA     W3  stateA     W0  coeffA
;   W8  pSrcB   W9  pDstB     W6  stateB     coeffB -> [W15-28]
;   W10 pSrcC   W11 pDstC     W7  stateC     coeffC -> [W15-24]
;   W12 pSrcD   W13 pDstD     W14 stateD     coeffD -> [W15-20]
;   W4  sample (dtb) counter, and the prologue's only scratch
;   W5  stage (dtb) counter
;------------------------------------------------------------------------------
_biquad_cascade_df2T_f32_dspic33ak_x4t:

;------------------------------------------------------------------------------
; Refuse a zero blockSize before anything is pushed or written, so the refusal
; path is a plain return. Odd is NOT refused - this kernel takes any length.
;------------------------------------------------------------------------------
        cp.l        W3, #0
        bra         z, L_x4t_refuse

;------------------------------------------------------------------------------
; Callee-saved registers: F8..F31 and W8..W14.
;
; F8..F31 rather than x3td's F8..F23 because the fourth channel reaches the top
; of the register file. The ABI probe in this tree only ever forced a compiler
; prologue to save F8..F19, so F20..F31 are saved here because this kernel USES
; them, not because the probe proved them callee-saved - 48 push/pop per CALL
; over 84 stages x 128 channel-samples is 0.004 per channel-sample.
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
        push.l      F27
        push.l      F28
        push.l      F29
        push.l      F30
        push.l      F31
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
        mov.l       #0x7F, W4
        floatsetup  W4

;------------------------------------------------------------------------------
; THE FRAME - NINE words (W15 grows up)
;
;   [W15-36] triples     blockSize / 3, reloaded into the dtb counter each stage
;   [W15-32] remainder   blockSize % 3, selects the tail
;   [W15-28] coeffB      advanced by 20 bytes per stage, in place
;   [W15-24] coeffC
;   [W15-20] coeffD
;   [W15-16] pDstA original
;   [W15-12] pDstB original
;   [W15-8]  pDstC original
;   [W15-4]  pDstD original
;
; Every one of these displacements assembles as the 2-byte short form (probe13),
; so the deeper frame costs nothing per access over x3td's five words.
;------------------------------------------------------------------------------

;------------------------------------------------------------------------------
; ONCE PER CALL: split blockSize into triples and remainder.
;
; Done FIRST, while W3 still holds blockSize - W3 becomes stateA below.
; W3 is consumed down to the remainder; W4 counts the triples.
;
; At most 170 iterations for blockSize 512, paid once per CALL against 84 stages
; of sample loop, so it vanishes into the per-stage figure.
;
; blockSize 0 was refused at entry, so W4 >= 1 whenever blockSize >= 3, and
; W4 == 0 for blockSize 1 or 2 - which the stage loop must handle, because `dtb`
; decrements BEFORE testing and would wrap 0 to 0xFFFFFFFF.
;------------------------------------------------------------------------------
        mov.l       #0, W4            ; W4 = triples
L_x4t_divloop:
        cp.l        W3, #3
        bra         ltu, L_x4t_divdone
        sub.l       #3, W3            ; consume one triple
        add.l       #1, W4            ; count it
        bra         L_x4t_divloop
L_x4t_divdone:
        mov.l       W4, [W15++]       ; [W15-36] triples
        mov.l       W3, [W15++]       ; [W15-32] remainder

;------------------------------------------------------------------------------
; Resolve the four instances.
;
; Each channel's STATE pointer goes straight to its final register. The
; COEFFICIENT pointers land in temporaries first, because three of them are bound
; for the frame and the fourth's home (W0) still holds the S array at this point.
;
; W4 is the scratch throughout - it is the sample counter, and it is dead until
; the triple count is loaded at the top of each stage.
;------------------------------------------------------------------------------
        mov.l       [W0+0], W4        ; W4 = S[0]
        clr         W5
        mov.b       [W4+0], W5        ; W5 = numStages (S[0]'s, drives all four)
        mov.l       [W4+8], W3        ; W3  = state A   (final home)
        mov.l       [W4+4], W8        ; W8  = coeff A   (temporary)

        mov.l       [W0+4], W4        ; W4 = S[1]
        mov.l       [W4+8], W6        ; W6  = state B   (final home)
        mov.l       [W4+4], W9        ; W9  = coeff B   (temporary)

        mov.l       [W0+8], W4        ; W4 = S[2]
        mov.l       [W4+8], W7        ; W7  = state C   (final home)
        mov.l       [W4+4], W10       ; W10 = coeff C   (temporary)

        mov.l       [W0+12], W4       ; W4 = S[3]   (last use of the S array)
        mov.l       [W4+8], W14       ; W14 = state D   (final home)
        mov.l       [W4+4], W11       ; W11 = coeff D   (temporary)

        mov.l       W9,  [W15++]      ; [W15-28] coeff B
        mov.l       W10, [W15++]      ; [W15-24] coeff C
        mov.l       W11, [W15++]      ; [W15-20] coeff D

        mov.l       W8, W0            ; coeff A -> W0, its resident home

;------------------------------------------------------------------------------
; Destination pointers. Read HIGHEST INDEX FIRST so the array's own register (W2)
; is overwritten last, and keep a copy of each in the frame so every stage can
; restart the cascade from stage 1's output.
;------------------------------------------------------------------------------
        mov.l       [W2+12], W13      ; W13 = pDstD
        mov.l       [W2+8],  W11      ; W11 = pDstC
        mov.l       [W2+4],  W9       ; W9  = pDstB
        mov.l       [W2+0],  W2       ; W2  = pDstA  (last use of the pDst array)

        mov.l       W2,  [W15++]      ; [W15-16] pDstA original
        mov.l       W9,  [W15++]      ; [W15-12] pDstB original
        mov.l       W11, [W15++]      ; [W15-8]  pDstC original
        mov.l       W13, [W15++]      ; [W15-4]  pDstD original

        mov.l       [W1+12], W12      ; W12 = pSrcD
        mov.l       [W1+8],  W10      ; W10 = pSrcC
        mov.l       [W1+4],  W8       ; W8  = pSrcB
        mov.l       [W1+0],  W1       ; W1  = pSrcA  (last use of the pSrc array)

;==============================================================================
; Stage loop
;==============================================================================
L_x4t_startFilter:

        ; Channel A coefficients - its pointer is resident in W0.
        mov.l       [W0++], F0
        mov.l       [W0++], F1
        mov.l       [W0++], F2
        mov.l       [W0++], F3
        mov.l       [W0++], F4

        ; Channel B coefficients - pointer SPILLED to [W15-28], advanced in place.
        mov.l       [W15-28] , W4
        mov.l       [W4++], F8
        mov.l       [W4++], F9
        mov.l       [W4++], F10
        mov.l       [W4++], F11
        mov.l       [W4++], F12
        mov.l       W4, [W15-28]

        ; Channel C coefficients - pointer SPILLED to [W15-24], advanced in place.
        mov.l       [W15-24] , W4
        mov.l       [W4++], F16
        mov.l       [W4++], F17
        mov.l       [W4++], F18
        mov.l       [W4++], F19
        mov.l       [W4++], F20
        mov.l       W4, [W15-24]

        ; Channel D coefficients - pointer SPILLED to [W15-20], advanced in place.
        mov.l       [W15-20] , W4
        mov.l       [W4++], F24
        mov.l       [W4++], F25
        mov.l       [W4++], F26
        mov.l       [W4++], F27
        mov.l       [W4++], F28
        mov.l       W4, [W15-20]

        ; States, into the phase-0 assignment: d1 -> first work reg, d2 -> second.
        mov.l       [W3],   F5        ; A: d1
        mov.l       [W3+4], F6        ; A: d2
        mov.l       [W6],   F13       ; B: d1
        mov.l       [W6+4], F14       ; B: d2
        mov.l       [W7],   F21       ; C: d1
        mov.l       [W7+4], F22       ; C: d2
        mov.l       [W14],   F29       ; D: d1
        mov.l       [W14+4], F30       ; D: d2

        mov.l       [W15-36], W4      ; W4 = FULL TRIPLES remaining in this stage

        ;----------------------------------------------------------------------
        ; `dtb` decrements BEFORE it tests, so a count of zero would wrap to
        ; 0xFFFFFFFF and run the body four billion times. blockSize 1 and 2 are
        ; legal (the contract is 1..512) and produce zero triples, so the zero
        ; case is branched around rather than assumed away.
        ;
        ; This test is per STAGE, not per iteration: one compare and one branch
        ; per 128 channel-samples.
        ;----------------------------------------------------------------------
        cp.l        W4, #0
        bra         z, L_x4t_tail

;==============================================================================
; Body - four channels, three samples, no copies
;
; 84 instructions for TWELVE channel-samples = 7.000 each, plus one dtb. Read as
; quadruples: 1 = A, 2 = B, 3 = C, 4 = D. No instruction of one channel names a
; register of another.
;
; The rotation is x3td's, one more channel wide:
;   sample 0: d1 = first work register of each channel
;   sample 1: roles advance one position
;   sample 2: one more, and the third sample returns to the phase-0 assignment
;==============================================================================
L_x4t_startTriples:
        ; No test here - the trip count was computed once at entry and the `dtb`
        ; at the bottom is the entire loop control, so the body is entered
        ; unconditionally. This is what x3td measured as worth 0.294 cyc.

        ; ---- sample 0 : d1=F5/F13/F21/F29, d2=F6/F14/F22/F30, x->F7/F15/F23/F31 ----
        mov.l       [W1++], F7                            ; A: x
        mov.l       [W8++], F15                           ; B:
        mov.l       [W10++], F23                          ; C:
        mov.l       [W12++], F31                          ; D:

        mac.s       F0, F7, F5                            ; A: d1 + b0*x = y
        mac.s       F8, F15, F13                          ; B:
        mac.s       F16, F23, F21                         ; C:
        mac.s       F24, F31, F29                         ; D:

        mac.s       F1, F7, F6                            ; A: d2 + b1*x   (x dies here)
        mac.s       F9, F15, F14                          ; B:
        mac.s       F17, F23, F22                         ; C:
        mac.s       F25, F31, F30                         ; D:

        mul.s       F2, F7, F7                            ; A: b2*x        (self-overwrite)
        mul.s       F10, F15, F15                         ; B:
        mul.s       F18, F23, F23                         ; C:
        mul.s       F26, F31, F31                         ; D:

        mov.l       F5, [W2++]                            ; A: store y
        mov.l       F13, [W9++]                           ; B:
        mov.l       F21, [W11++]                          ; C:
        mov.l       F29, [W13++]                          ; D:

        mac.s       F3, F5, F6                            ; A: new d1
        mac.s       F11, F13, F14                         ; B:
        mac.s       F19, F21, F22                         ; C:
        mac.s       F27, F29, F30                         ; D:

        mac.s       F4, F5, F7                            ; A: new d2
        mac.s       F12, F13, F15                         ; B:
        mac.s       F20, F21, F23                         ; C:
        mac.s       F28, F29, F31                         ; D:

        ; ---- sample 1 : d1=F6/F14/F22/F30, d2=F7/F15/F23/F31, x->F5/F13/F21/F29 ----
        mov.l       [W1++], F5                            ; A: x
        mov.l       [W8++], F13                           ; B:
        mov.l       [W10++], F21                          ; C:
        mov.l       [W12++], F29                          ; D:

        mac.s       F0, F5, F6                            ; A: d1 + b0*x = y
        mac.s       F8, F13, F14                          ; B:
        mac.s       F16, F21, F22                         ; C:
        mac.s       F24, F29, F30                         ; D:

        mac.s       F1, F5, F7                            ; A: d2 + b1*x   (x dies here)
        mac.s       F9, F13, F15                          ; B:
        mac.s       F17, F21, F23                         ; C:
        mac.s       F25, F29, F31                         ; D:

        mul.s       F2, F5, F5                            ; A: b2*x        (self-overwrite)
        mul.s       F10, F13, F13                         ; B:
        mul.s       F18, F21, F21                         ; C:
        mul.s       F26, F29, F29                         ; D:

        mov.l       F6, [W2++]                            ; A: store y
        mov.l       F14, [W9++]                           ; B:
        mov.l       F22, [W11++]                          ; C:
        mov.l       F30, [W13++]                          ; D:

        mac.s       F3, F6, F7                            ; A: new d1
        mac.s       F11, F14, F15                         ; B:
        mac.s       F19, F22, F23                         ; C:
        mac.s       F27, F30, F31                         ; D:

        mac.s       F4, F6, F5                            ; A: new d2
        mac.s       F12, F14, F13                         ; B:
        mac.s       F20, F22, F21                         ; C:
        mac.s       F28, F30, F29                         ; D:

        ; ---- sample 2 : d1=F7/F15/F23/F31, d2=F5/F13/F21/F29, x->F6/F14/F22/F30 ----
        mov.l       [W1++], F6                            ; A: x
        mov.l       [W8++], F14                           ; B:
        mov.l       [W10++], F22                          ; C:
        mov.l       [W12++], F30                          ; D:

        mac.s       F0, F6, F7                            ; A: d1 + b0*x = y
        mac.s       F8, F14, F15                          ; B:
        mac.s       F16, F22, F23                         ; C:
        mac.s       F24, F30, F31                         ; D:

        mac.s       F1, F6, F5                            ; A: d2 + b1*x   (x dies here)
        mac.s       F9, F14, F13                          ; B:
        mac.s       F17, F22, F21                         ; C:
        mac.s       F25, F30, F29                         ; D:

        mul.s       F2, F6, F6                            ; A: b2*x        (self-overwrite)
        mul.s       F10, F14, F14                         ; B:
        mul.s       F18, F22, F22                         ; C:
        mul.s       F26, F30, F30                         ; D:

        mov.l       F7, [W2++]                            ; A: store y
        mov.l       F15, [W9++]                           ; B:
        mov.l       F23, [W11++]                          ; C:
        mov.l       F31, [W13++]                          ; D:

        mac.s       F3, F7, F5                            ; A: new d1
        mac.s       F11, F15, F13                         ; B:
        mac.s       F19, F23, F21                         ; C:
        mac.s       F27, F31, F29                         ; D:

        mac.s       F4, F7, F6                            ; A: new d2
        mac.s       F12, F15, F14                         ; B:
        mac.s       F20, F23, F22                         ; C:
        mac.s       F28, F31, F30                         ; D:

        dtb         W4, L_x4t_startTriples   ; the whole loop control, one instruction

;==============================================================================
; Tail - 0, 1 or 2 samples, one at a time, the SAME seven instructions per
; channel-sample. This is what keeps the 1..512 contract that x2r and x3r gave up.
;
; Each tail sample rotates the roles by one, so after the tail d1 and d2 are NOT
; in the registers a single epilogue could name. The ending phase is
; (blockSize mod 3), a RUNTIME value, so the write-back is selected by branch.
;==============================================================================
L_x4t_tail:
        ;----------------------------------------------------------------------
        ; The remainder is NOT left in W4 by the loop: `dtb` runs it down to zero
        ; regardless of blockSize. It is reloaded from the frame, where the entry
        ; split stored it - one instruction per stage.
        ;----------------------------------------------------------------------
        mov.l       [W15-32], W4      ; W4 = remainder (0, 1 or 2)

        cp.l        W4, #0
        bra         z, L_x4t_store_p0

        ; ---- tail sample 1 (phase 0 roles) : d1=F5/F13/F21/F29, d2=F6/F14/F22/F30, x->F7/F15/F23/F31 ----
        mov.l       [W1++], F7                            ; A:
        mov.l       [W8++], F15                           ; B:
        mov.l       [W10++], F23                          ; C:
        mov.l       [W12++], F31                          ; D:

        mac.s       F0, F7, F5                            ; A:
        mac.s       F8, F15, F13                          ; B:
        mac.s       F16, F23, F21                         ; C:
        mac.s       F24, F31, F29                         ; D:

        mac.s       F1, F7, F6                            ; A:
        mac.s       F9, F15, F14                          ; B:
        mac.s       F17, F23, F22                         ; C:
        mac.s       F25, F31, F30                         ; D:

        mul.s       F2, F7, F7                            ; A:
        mul.s       F10, F15, F15                         ; B:
        mul.s       F18, F23, F23                         ; C:
        mul.s       F26, F31, F31                         ; D:

        mov.l       F5, [W2++]                            ; A:
        mov.l       F13, [W9++]                           ; B:
        mov.l       F21, [W11++]                          ; C:
        mov.l       F29, [W13++]                          ; D:

        mac.s       F3, F5, F6                            ; A:
        mac.s       F11, F13, F14                         ; B:
        mac.s       F19, F21, F22                         ; C:
        mac.s       F27, F29, F30                         ; D:

        mac.s       F4, F5, F7                            ; A:
        mac.s       F12, F13, F15                         ; B:
        mac.s       F20, F21, F23                         ; C:
        mac.s       F28, F29, F31                         ; D:

        cp.l        W4, #1
        bra         z, L_x4t_store_p1

        ; ---- tail sample 2 (phase 1 roles) : d1=F6/F14/F22/F30, d2=F7/F15/F23/F31, x->F5/F13/F21/F29 ----
        mov.l       [W1++], F5                            ; A:
        mov.l       [W8++], F13                           ; B:
        mov.l       [W10++], F21                          ; C:
        mov.l       [W12++], F29                          ; D:

        mac.s       F0, F5, F6                            ; A:
        mac.s       F8, F13, F14                          ; B:
        mac.s       F16, F21, F22                         ; C:
        mac.s       F24, F29, F30                         ; D:

        mac.s       F1, F5, F7                            ; A:
        mac.s       F9, F13, F15                          ; B:
        mac.s       F17, F21, F23                         ; C:
        mac.s       F25, F29, F31                         ; D:

        mul.s       F2, F5, F5                            ; A:
        mul.s       F10, F13, F13                         ; B:
        mul.s       F18, F21, F21                         ; C:
        mul.s       F26, F29, F29                         ; D:

        mov.l       F6, [W2++]                            ; A:
        mov.l       F14, [W9++]                           ; B:
        mov.l       F22, [W11++]                          ; C:
        mov.l       F30, [W13++]                          ; D:

        mac.s       F3, F6, F7                            ; A:
        mac.s       F11, F14, F15                         ; B:
        mac.s       F19, F22, F23                         ; C:
        mac.s       F27, F30, F31                         ; D:

        mac.s       F4, F6, F5                            ; A:
        mac.s       F12, F14, F13                         ; B:
        mac.s       F20, F22, F21                         ; C:
        mac.s       F28, F30, F29                         ; D:

        bra         L_x4t_store_p2

;------------------------------------------------------------------------------
; State write-back, one variant per ending phase. Only the register NAMES
; differ; the addresses and the order are identical, so a reader can check the
; three against each other.
;------------------------------------------------------------------------------
L_x4t_store_p0:                    ; remainder 0: d1 in F5 / F13 / F21 / F29
        mov.l       F5  , [W3++]
        mov.l       F6  , [W3++]
        mov.l       F13 , [W6++]
        mov.l       F14 , [W6++]
        mov.l       F21 , [W7++]
        mov.l       F22 , [W7++]
        mov.l       F29 , [W14++]
        mov.l       F30 , [W14++]
        bra         L_x4t_nextStage

L_x4t_store_p1:                    ; remainder 1: d1 in F6 / F14 / F22 / F30
        mov.l       F6  , [W3++]
        mov.l       F7  , [W3++]
        mov.l       F14 , [W6++]
        mov.l       F15 , [W6++]
        mov.l       F22 , [W7++]
        mov.l       F23 , [W7++]
        mov.l       F30 , [W14++]
        mov.l       F31 , [W14++]
        bra         L_x4t_nextStage

L_x4t_store_p2:                    ; remainder 2: d1 in F7 / F15 / F23 / F31
        mov.l       F7  , [W3++]
        mov.l       F5  , [W3++]
        mov.l       F15 , [W6++]
        mov.l       F13 , [W6++]
        mov.l       F23 , [W7++]
        mov.l       F21 , [W7++]
        mov.l       F31 , [W14++]
        mov.l       F29 , [W14++]

;------------------------------------------------------------------------------
; Next stage reads this stage's output, for all four channels - opt_v1's in-place
; cascade arrangement, done four times.
;
; NOTE THE DISPLACEMENTS: the frame is NINE words here, not x3td's five. [W15-36]
; is the triple count and [W15-28..-20] are coefficient pointers - reading a pDst
; from any of them would use an integer or a coefficient address as an output
; pointer, and the bit-exactness gate is what would catch it.
;------------------------------------------------------------------------------
L_x4t_nextStage:
        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB
        mov.l       [W15-8],  W10     ; C: pSrc = original pDstC
        mov.l       [W15-8],  W11     ; C: pDst = original pDstC
        mov.l       [W15-4],  W12     ; D: pSrc = original pDstD
        mov.l       [W15-4],  W13     ; D: pDst = original pDstD

        dtb         W5, L_x4t_startFilter

;------------------------------------------------------------------------------
; Restore and return.
;------------------------------------------------------------------------------
        sub.l       #36, W15          ; NINE frame words (x3td popped 20)

        pop.l       W14
        pop.l       W13
        pop.l       W12
        pop.l       W11
        pop.l       W10
        pop.l       W9
        pop.l       W8

        pop.l       FCR
        pop.l       F31
        pop.l       F30
        pop.l       F29
        pop.l       F28
        pop.l       F27
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

L_x4t_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
