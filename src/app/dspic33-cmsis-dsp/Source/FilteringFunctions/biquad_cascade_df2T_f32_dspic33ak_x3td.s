;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x3td.s
;
; x3t WITH THE LOOP CONTROL REPLACED BY DTB. NOTHING ELSE DIFFERS.
;
; THREE CHANNELS, THREE SAMPLES PER ITERATION, THREE WORK REGISTERS PER CHANNEL,
; AND NO COPY INSTRUCTIONS AT ALL.
;
; A CANDIDATE. This is not the previous rotation with one more sample bolted on -
; it is a different register topology, and it is the first thing in this family
; that removes an instruction without adding one somewhere else.
;
; WHAT THIS FILE CHANGES, AND WHY IT IS A SEPARATE FILE
;   x3t pays FIVE instructions of loop control per iteration, measured in its own
;   disassembly, not counted from the source:
;
;       cp.l  W4, #3        ; still three left?
;       bra   NC, tail      ; no -> tail
;       ...body...
;       sub.l #3, W4        ; consume three
;       neop                ; assembler's branch-hazard filler after sub.l
;       bra   startTriples
;
;   Five instructions over nine channel-samples is 0.556 per channel-sample - 64 %
;   of x3t's entire 0.874 of overhead above the 7.000 arithmetic floor.
;
;   DTB does the whole job in one instruction: it decrements Wn and branches while
;   the result is non-zero. So the count of FULL TRIPLES is computed ONCE at entry
;   and the sample loop becomes straight-line body plus `dtb`:
;
;       1 instruction / 9 channel-samples = 0.111    (against 0.556)
;
;   MEASURED 2026-09-23 on dsPIC33AK512MPS506 Curiosity Nano, ap84 x 32 frames,
;   same image as x3t so the two are directly comparable:
;
;       x3t   7.888 cyc/sample/section   instr 7.874   CPI 1.003   ev8 0.000
;       x3td  7.594 cyc/sample/section   instr 7.579   CPI 1.003   ev8 0.000
;               -2.94 %                    -3.75 %
;
;   Bit-identical on all three channels (0/96), drift check 0.000 % STABLE.
;   The cycle saving tracked the instruction saving to within 0.001 - this kernel
;   is issue-bound, so removing an instruction removes a cycle. ev8 stayed at zero,
;   which was the open question: `dtb` lands immediately after the last mac.s of
;   the body, two instructions closer than x3t's sub.l/neop put it, and that did
;   NOT open a dependency stall.
;
;   WHY IT IS A SEPARATE FILE RATHER THAN AN EDIT: x3t's 7.874 is a measured,
;   published number and the next kernel (four channels) will be built on top of
;   this loop shape. Keeping both lets the sweep measure them IN THE SAME IMAGE,
;   so the dtb delta is isolated from the four-channel change that follows it.
;
;   NO DIVIDE INSTRUCTION IS USED. This ISA does have integer divide (DIVU.l),
;   but blockSize is at most 512, the quotient is needed once per CALL rather than
;   once per stage, and a subtract loop of at most 171 iterations at entry is both
;   cheaper to reason about and impossible to get wrong on operand size. The
;   quotient is then reloaded per stage from the frame, which is one instruction -
;   exactly what x3t already did with blockSize.
;
; WHY THE UNDERLYING SHAPE EXISTS (unchanged from x3t)
;   x3r spends 8 instructions per channel-sample, and one of them is a copy:
;
;       mov.s   d2 -> partner        ; so that b1*x can accumulate into it
;
;   The copy is there because the schedule treats d2 as a value that must be
;   preserved while a new d1 is built somewhere else. It does not have to be:
;   OLD d2 IS EXACTLY THE ACCUMULATOR b1*x WANTS. Accumulate into d2 itself and
;   the copy disappears.
;
;   That frees the fourth work register, and then x's register can be recycled
;   too: x dies the instant b1*x retires, so `mul.s b2, C, C` overwrites x with
;   b2*x - the d2 partial - in place. Three work registers per channel, and the
;   roles rotate one position per sample:
;
;       sample 0 : A=d1 B=d2 C=x  ->  exit  B=d1 C=d2 A=free (holds y)
;       sample 1 : B=d1 C=d2 A=x  ->  exit  C=d1 A=d2 B=free
;       sample 2 : C=d1 A=d2 B=x  ->  exit  A=d1 B=d2 C=free   <- back to entry
;
;   THREE samples close the cycle, which is why the loop unrolls by three.
;
; THE BODY - 7 INSTRUCTIONS PER CHANNEL-SAMPLE, ZERO mov.s
;       mov.l  [Wsrc++], C     ; C = x
;       mac.s  b0, C, A        ; A = d1 + b0*x = y          (A was d1)
;       mac.s  b1, C, B        ; B = d2 + b1*x              (B was d2; x now dead)
;       mul.s  b2, C, C        ; C = b2*x                   (self-overwrite)
;       mov.l  A, [Wdst++]     ; store y
;       mac.s  a1, A, B        ; B = new d1
;       mac.s  a2, A, C        ; C = new d2
;
;   against x3r's 8, and against opt_v1's 10.
;
; REGISTER BUDGET - THE POINT OF THE WHOLE EXERCISE
;   coefficients 5 + work 3 = 8 per channel. THREE channels is 24, which is what
;   this file uses. FOUR channels would be 32, which is exactly F0..F31
;   (DS70005540C Figure 1-2) - so this topology is what makes a four-channel
;   kernel representable at all. x4 is deliberately NOT attempted here: the point
;   of doing three first is that the harness already exists and the register file
;   has slack, so a defect shows up as a defect rather than as a register clash.
;
; TWO GATES PASSED BEFORE THIS FILE WAS WRITTEN
;   1. ARITHMETIC. The seven instructions were expanded symbolically over four
;      samples and reproduce DF2T exactly:
;         y = d1 + b0*x ; d1' = d2 + b1*x + a1*y ; d2' = b2*x + a2*y
;      and the phase returns to its starting assignment after three samples.
;   2. ISA. `mul.s` writing its own source is the one form this schedule needs
;      and the one thing that could have killed it. It assembles, and the
;      ENCODING confirms it is not a short form being mis-rendered:
;         mul.s f2, f7, f7  ->  06 71 0e 84   (dest field 0x0e = f7*2)
;         mul.s f2, f7, f6  ->  06 71 0c 84   (dest field 0x0c = f6*2)
;      Only the destination field differs, exactly as the register number
;      predicts. (This tree has a standing trap where xc-dsc v3.31.01's objdump
;      mis-renders a same-register short form - that is a different encoding and
;      these bytes distinguish themselves.)
;
; blockSize CONTRACT: ANY 1..512, INCLUDING ODD
;   Three samples per iteration, so the sample count is split into triples plus a
;   remainder of 0, 1 or 2 - and the remainder is handled by a tail loop that
;   runs the SAME seven instructions one sample at a time. So unlike x2r/x3r this
;   kernel does NOT narrow the contract: it takes every length opt_v1 takes.
;   That matters more than it sounds - the even-only contract is precisely why
;   x2r was kept off the audio path.
;
;   The triple count and the remainder are computed ONCE AT ENTRY by subtracting
;   three until fewer than three remain, and both are stored in the frame. The
;   sample loop is then driven by `dtb` on the triple count, and the remainder
;   selects the tail exactly as it did in x3t.
;
;   THE TAIL'S ONE REAL SUBTLETY: after a tail sample the roles have rotated, so
;   d1 and d2 are NOT in the registers the epilogue would naively store from. The
;   phase after the tail is (blockSize mod 3), which is a RUNTIME value, so the
;   write-back is selected by a two-way branch on the remainder rather than
;   assumed. For the audio path's 32 samples the remainder is 2, but this kernel
;   does not get to assume that.
;
; ARITHMETIC IS OPT_V1'S, PER CHANNEL
;   Same five operations on the same operand values in the same order; only which
;   register holds a value changes, and no instruction of one channel names a
;   register of another. So each channel's output must be BIT-IDENTICAL to opt_v1
;   run on it alone. A difference is a defect in this file, not a rounding
;   effect, and not something a tolerance may be widened to accept.
;
; WHAT IS NOT PREDICTED HERE
;   The instruction count is arithmetic and is stated: 7 per channel-sample, plus
;   two of loop control per iteration (cp + bra rather than one dtb), so
;   (7*3*3 + 2)/9 = 7.222 in the loop and about 7.6 with the per-stage prologue,
;   against x3r's measured 8.554. THE CYCLE COUNT IS NOT PREDICTED IN THIS FILE.
;   This family has mispredicted stall three times out of four, and the one thing
;   measured today is that a stall window cannot be filled with CPU work. The
;   prediction, with its branches and what each would mean, is in a local
;   pre-build prediction record (not part of the shipped archive), and is not
;   edited afterwards.
;
; ITS OWN CODE SECTION
;   --gc-sections drops it from any image that does not call it.
;==============================================================================

        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x3td, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x3td

;------------------------------------------------------------------------------
; void biquad_cascade_df2T_f32_dspic33ak_x3td(
;         const arm_biquad_cascade_df2T_instance_f32 * const *S,   ; W0
;         const float32_t * const *pSrc,                           ; W1
;         float32_t * const *pDst,                                 ; W2
;         uint32_t blockSize );                                    ; W3
;
; Array arguments for the reason x3/x3r use them: xc-dsc passes only the first
; seven 32-bit arguments in registers, and three instances plus six pointers plus
; blockSize is ten. ALL THREE INSTANCES MUST HAVE THE SAME numStages - S[0]'s
; count drives all three cascades.
;
; Channel register map (coefficients then the three rotating work registers):
;   A ch: b0=F0  b1=F1  b2=F2  a1=F3  a2=F4      work F5  F6  F7
;   B ch: b0=F8  b1=F9  b2=F10 a1=F11 a2=F12     work F13 F14 F15
;   C ch: b0=F16 b1=F17 b2=F18 a1=F19 a2=F20     work F21 F22 F23
;
; 24 F-registers. Four channels in this topology would need 32, which is the
; whole register file - that is the door this kernel opens, and it is not walked
; through here.
;------------------------------------------------------------------------------
_biquad_cascade_df2T_f32_dspic33ak_x3td:

;------------------------------------------------------------------------------
; Refuse a zero blockSize before anything is pushed or written, so the refusal
; path is a plain return. Odd is NOT refused - this kernel takes any length.
;------------------------------------------------------------------------------
        cp.l        W3, #0
        bra         z, L_x3td_refuse

;------------------------------------------------------------------------------
; Callee-saved registers: F8..F23 and W8..W14.
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
; Resolve the three instances. W0/W1/W2 are the argument arrays and each is read
; out before its register is reused.
;
; Loop register roles:
;   W1  = pSrcA (post-increment)   W2  = pDstA
;   W8  = pSrcB                    W9  = pDstB
;   W14 = pSrcC                    W3  = pDstC
;   W10 = coeff A                  W12 = state A
;   W11 = coeff B                  W13 = state B
;   W6  = coeff C                  W7  = state C
;   W4  = triple counter           W5  = stage counter
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
; THE TRIP COUNT IS COMPUTED ONCE PER CALL - this is the difference from x3t.
;
; x3t drove its sample loop by counting down by three and testing every iteration,
; which supplied the remainder for free but cost five instructions per iteration
; (cp, bra, sub, the assembler's neop, bra). Here the split is done once at entry,
; below, and the loop is driven by `dtb` - one instruction, no test in the body.
;
; NO DIVIDE INSTRUCTION IS USED. This ISA does have integer divide (DIVU.l), but
; the quotient is wanted once per CALL, blockSize is at most 512, and a subtract
; loop at entry cannot be got wrong on operand size. `div.l` is NOT the mnemonic
; on this part - an earlier note in this tree recorded "no integer divide" on the
; strength of that spelling failing to assemble, which was the wrong probe.
;------------------------------------------------------------------------------

;------------------------------------------------------------------------------
; Frame (W15 grows up), FIVE words - x3t's four, with blockSize replaced by the
; triple count and the remainder that are now computed once at entry:
;   [W15-20] = pDstA original
;   [W15-16] = pDstB original
;   [W15-12] = pDstC original
;   [W15-8]  = triples        (blockSize / 3, reloaded into the dtb counter each stage)
;   [W15-4]  = remainder      (blockSize % 3, selects the tail)
;
; Why both live in the frame rather than in registers: the sample loop needs one W
; for the dtb counter and x3t already used 14 of the 15 usable W registers (W15 is
; the stack pointer, and W16 DOES NOT EXIST on this part - checked with the
; assembler). Reloading two words per STAGE costs two instructions per 96
; channel-samples; keeping them resident would cost a register that the
; four-channel kernel is going to need.
;
; W0 is dead after the instance block and is the scratch this shuffle needs. W4 is
; free here (it becomes the dtb counter per stage) and is used to build the
; quotient.
;------------------------------------------------------------------------------
        mov.l       W3, W0            ; W0 = blockSize (W3 becomes pDstC)

        mov.l       [W2+8], W3        ; W3 = pDstC
        mov.l       [W2+4], W9        ; W9 = pDstB
        mov.l       [W2+0], W2        ; W2 = pDstA   (last use of the pDst array)

        mov.l       W2, [W15++]       ; [W15-20] pDstA original
        mov.l       W9, [W15++]       ; [W15-16] pDstB original
        mov.l       W3, [W15++]       ; [W15-12] pDstC original

        ;----------------------------------------------------------------------
        ; ONCE PER CALL: split blockSize into triples and remainder.
        ;
        ; W0 = blockSize on entry, consumed down to the remainder.
        ; W4 = triple count, built up from zero.
        ;
        ; At most 170 iterations for blockSize 512, paid once per CALL against 84
        ; or 99 stages x 10 iterations of sample loop, so it vanishes into the
        ; per-stage figure. This ISA does have an integer divide (DIVU.l), but it
        ; would save wall-clock that nothing is waiting on and add an operand-size
        ; question to a kernel whose whole value is being exactly right.
        ;
        ; blockSize 0 was already refused at entry, so W4 >= 1 whenever blockSize
        ; >= 3, and W4 == 0 for blockSize 1 or 2 - which the stage loop must handle
        ; because `dtb` decrements BEFORE testing and would wrap 0 to 0xFFFFFFFF.
        ;----------------------------------------------------------------------
        mov.l       #0, W4            ; W4 = triples
L_x3td_divloop:
        cp.l        W0, #3
        bra         ltu, L_x3td_divdone
        sub.l       #3, W0            ; consume one triple
        add.l       #1, W4            ; count it
        bra         L_x3td_divloop
L_x3td_divdone:
        mov.l       W4, [W15++]       ; [W15-8]  triples
        mov.l       W0, [W15++]       ; [W15-4]  remainder

        mov.l       [W1+8], W14       ; W14 = pSrcC
        mov.l       [W1+4], W8        ; W8  = pSrcB
        mov.l       [W1+0], W1        ; W1  = pSrcA  (last use of the pSrc array)

;==============================================================================
; Stage loop
;==============================================================================
L_x3td_startFilter:

        ; Channel A coefficients: F0=b0 F1=b1 F2=b2 F3=a1 F4=a2.
        mov.l       [W10++], F0
        mov.l       [W10++], F1
        mov.l       [W10++], F2
        mov.l       [W10++], F3
        mov.l       [W10++], F4

        ; Channel B coefficients: F8=b0 F9=b1 F10=b2 F11=a1 F12=a2.
        mov.l       [W11++], F8
        mov.l       [W11++], F9
        mov.l       [W11++], F10
        mov.l       [W11++], F11
        mov.l       [W11++], F12

        ; Channel C coefficients: F16=b0 F17=b1 F18=b2 F19=a1 F20=a2.
        mov.l       [W6++], F16
        mov.l       [W6++], F17
        mov.l       [W6++], F18
        mov.l       [W6++], F19
        mov.l       [W6++], F20

        ; States, into the phase-0 assignment: d1 -> first work reg, d2 -> second.
        mov.l       [W12],   F5       ; A: d1
        mov.l       [W12+4], F6       ; A: d2
        mov.l       [W13],   F13      ; B: d1
        mov.l       [W13+4], F14      ; B: d2
        mov.l       [W7],    F21      ; C: d1
        mov.l       [W7+4],  F22      ; C: d2

        mov.l       [W15-8], W4       ; W4 = FULL TRIPLES remaining in this stage

        ;----------------------------------------------------------------------
        ; `dtb` decrements BEFORE it tests, so a count of zero would wrap to
        ; 0xFFFFFFFF and run the body four billion times. blockSize 1 and 2 are
        ; legal (the contract is 1..512) and produce zero triples, so the zero
        ; case is branched around rather than assumed away.
        ;
        ; This test is per STAGE, not per iteration: one instruction per 96
        ; channel-samples against the five per nine that it replaces.
        ;----------------------------------------------------------------------
        cp.l        W4, #0
        bra         z, L_x3td_tail

;==============================================================================
; Body - three channels, three samples, no copies
;
; 63 instructions for NINE channel-samples = 7.000 per sample, plus one dtb.
; Read as triples: 1 = A, 2 = B, 3 = C. No instruction of one channel names a
; register of another.
;
; Phase per sample (A/B/C here means the ROLE, not the channel):
;   sample 0: d1=F5  d2=F6  x=F7      (A ch)   d1=F13 d2=F14 x=F15   (B ch)
;                                              d1=F21 d2=F22 x=F23   (C ch)
;   sample 1: d1=F6  d2=F7  x=F5      etc, rotating one position
;   sample 2: d1=F7  d2=F5  x=F6
;==============================================================================
L_x3td_startTriples:
        ; No test here - this is the whole point of the file. The trip count was
        ; computed once at entry and `dtb` at the bottom is the entire loop
        ; control, so the body is entered unconditionally.

        ; ---- sample 0 : d1=F5/F13/F21, d2=F6/F14/F22, x->F7/F15/F23 ----------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F15       ; B: x
        mov.l       [W14++], F23      ; C: x

        mac.s       F0, F7, F5        ; A: F5  = d1 + b0*x = y
        mac.s       F8, F15, F13      ; B: F13 = y
        mac.s       F16, F23, F21     ; C: F21 = y

        mac.s       F1, F7, F6        ; A: F6  = d2 + b1*x   (x dies here)
        mac.s       F9, F15, F14      ; B: F14
        mac.s       F17, F23, F22     ; C: F22

        mul.s       F2, F7, F7        ; A: F7  = b2*x        (self-overwrite)
        mul.s       F10, F15, F15     ; B: F15
        mul.s       F18, F23, F23     ; C: F23

        mov.l       F5, [W2++]        ; A: *pDstA++ = y
        mov.l       F13, [W9++]       ; B: *pDstB++ = y
        mov.l       F21, [W3++]       ; C: *pDstC++ = y

        mac.s       F3, F5, F6        ; A: F6  = new d1
        mac.s       F11, F13, F14     ; B: F14 = new d1
        mac.s       F19, F21, F22     ; C: F22 = new d1

        mac.s       F4, F5, F7        ; A: F7  = new d2
        mac.s       F12, F13, F15     ; B: F15 = new d2
        mac.s       F20, F21, F23     ; C: F23 = new d2

        ; ---- sample 1 : d1=F6/F14/F22, d2=F7/F15/F23, x->F5/F13/F21 ----------
        mov.l       [W1++], F5        ; A: x   (into the register y vacated)
        mov.l       [W8++], F13       ; B: x
        mov.l       [W14++], F21      ; C: x

        mac.s       F0, F5, F6        ; A: F6  = d1 + b0*x = y
        mac.s       F8, F13, F14      ; B: F14 = y
        mac.s       F16, F21, F22     ; C: F22 = y

        mac.s       F1, F5, F7        ; A: F7  = d2 + b1*x
        mac.s       F9, F13, F15      ; B: F15
        mac.s       F17, F21, F23     ; C: F23

        mul.s       F2, F5, F5        ; A: F5  = b2*x
        mul.s       F10, F13, F13     ; B: F13
        mul.s       F18, F21, F21     ; C: F21

        mov.l       F6, [W2++]        ; A: *pDstA++ = y
        mov.l       F14, [W9++]       ; B
        mov.l       F22, [W3++]       ; C

        mac.s       F3, F6, F7        ; A: F7  = new d1
        mac.s       F11, F14, F15     ; B: F15 = new d1
        mac.s       F19, F22, F23     ; C: F23 = new d1

        mac.s       F4, F6, F5        ; A: F5  = new d2
        mac.s       F12, F14, F13     ; B: F13 = new d2
        mac.s       F20, F22, F21     ; C: F21 = new d2

        ; ---- sample 2 : d1=F7/F15/F23, d2=F5/F13/F21, x->F6/F14/F22 ----------
        mov.l       [W1++], F6        ; A: x
        mov.l       [W8++], F14       ; B: x
        mov.l       [W14++], F22      ; C: x

        mac.s       F0, F6, F7        ; A: F7  = d1 + b0*x = y
        mac.s       F8, F14, F15      ; B: F15 = y
        mac.s       F16, F22, F23     ; C: F23 = y

        mac.s       F1, F6, F5        ; A: F5  = d2 + b1*x
        mac.s       F9, F14, F13      ; B: F13
        mac.s       F17, F22, F21     ; C: F21

        mul.s       F2, F6, F6        ; A: F6  = b2*x
        mul.s       F10, F14, F14     ; B: F14
        mul.s       F18, F22, F22     ; C: F22

        mov.l       F7, [W2++]        ; A: *pDstA++ = y
        mov.l       F15, [W9++]       ; B
        mov.l       F23, [W3++]       ; C

        mac.s       F3, F7, F5        ; A: F5  = new d1  (back to phase 0)
        mac.s       F11, F15, F13     ; B: F13 = new d1
        mac.s       F19, F23, F21     ; C: F21 = new d1

        mac.s       F4, F7, F6        ; A: F6  = new d2
        mac.s       F12, F15, F14     ; B: F14 = new d2
        mac.s       F20, F23, F22     ; C: F22 = new d2

        dtb         W4, L_x3td_startTriples   ; the whole loop control, one instruction

;==============================================================================
; Tail - 0, 1 or 2 samples, one at a time, the SAME seven instructions.
;
; This is what keeps the 1..512 contract that x2r and x3r gave up. Each tail
; sample rotates the roles by one, so after the tail d1 and d2 are NOT in F5/F6 -
; the write-back below selects by remainder.
;==============================================================================
L_x3td_tail:
        ;----------------------------------------------------------------------
        ; UNLIKE x3t, the remainder is NOT left in W4 by the loop: `dtb` runs W4
        ; down to zero regardless of blockSize. It is reloaded from the frame,
        ; where the entry split stored it - one instruction per stage.
        ;----------------------------------------------------------------------
        mov.l       [W15-4], W4       ; W4 = remainder (0, 1 or 2)

        cp.l        W4, #0
        bra         z, L_x3td_store_p0

        ; ---- tail sample 1: phase 0 roles (d1=F5/F13/F21) --------------------
        mov.l       [W1++], F7        ; A: x
        mov.l       [W8++], F15       ; B: x
        mov.l       [W14++], F23      ; C: x

        mac.s       F0, F7, F5        ; A: y
        mac.s       F8, F15, F13      ; B: y
        mac.s       F16, F23, F21     ; C: y

        mac.s       F1, F7, F6        ; A
        mac.s       F9, F15, F14      ; B
        mac.s       F17, F23, F22     ; C

        mul.s       F2, F7, F7        ; A
        mul.s       F10, F15, F15     ; B
        mul.s       F18, F23, F23     ; C

        mov.l       F5, [W2++]        ; A: store y
        mov.l       F13, [W9++]       ; B
        mov.l       F21, [W3++]       ; C

        mac.s       F3, F5, F6        ; A: new d1 -> F6
        mac.s       F11, F13, F14     ; B: -> F14
        mac.s       F19, F21, F22     ; C: -> F22

        mac.s       F4, F5, F7        ; A: new d2 -> F7
        mac.s       F12, F13, F15     ; B: -> F15
        mac.s       F20, F21, F23     ; C: -> F23

        cp.l        W4, #1
        bra         z, L_x3td_store_p1

        ; ---- tail sample 2: phase 1 roles (d1=F6/F14/F22) --------------------
        mov.l       [W1++], F5        ; A: x
        mov.l       [W8++], F13       ; B: x
        mov.l       [W14++], F21      ; C: x

        mac.s       F0, F5, F6        ; A: y
        mac.s       F8, F13, F14      ; B: y
        mac.s       F16, F21, F22     ; C: y

        mac.s       F1, F5, F7        ; A
        mac.s       F9, F13, F15      ; B
        mac.s       F17, F21, F23     ; C

        mul.s       F2, F5, F5        ; A
        mul.s       F10, F13, F13     ; B
        mul.s       F18, F21, F21     ; C

        mov.l       F6, [W2++]        ; A: store y
        mov.l       F14, [W9++]       ; B
        mov.l       F22, [W3++]       ; C

        mac.s       F3, F6, F7        ; A: new d1 -> F7
        mac.s       F11, F14, F15     ; B: -> F15
        mac.s       F19, F22, F23     ; C: -> F23

        mac.s       F4, F6, F5        ; A: new d2 -> F5
        mac.s       F12, F14, F13     ; B: -> F13
        mac.s       F20, F22, F21     ; C: -> F21

        bra         L_x3td_store_p2

;------------------------------------------------------------------------------
; State write-back, one variant per ending phase. Only the register NAMES differ;
; the addresses and the order are identical, so a reader can check the three
; against each other.
;------------------------------------------------------------------------------
L_x3td_store_p0:                       ; remainder 0: d1 in F5 / F13 / F21
        mov.l       F5, [W12++]
        mov.l       F6, [W12++]
        mov.l       F13, [W13++]
        mov.l       F14, [W13++]
        mov.l       F21, [W7++]
        mov.l       F22, [W7++]
        bra         L_x3td_nextStage

L_x3td_store_p1:                       ; remainder 1: d1 in F6 / F14 / F22
        mov.l       F6, [W12++]
        mov.l       F7, [W12++]
        mov.l       F14, [W13++]
        mov.l       F15, [W13++]
        mov.l       F22, [W7++]
        mov.l       F23, [W7++]
        bra         L_x3td_nextStage

L_x3td_store_p2:                       ; remainder 2: d1 in F7 / F15 / F23
        mov.l       F7, [W12++]
        mov.l       F5, [W12++]
        mov.l       F15, [W13++]
        mov.l       F13, [W13++]
        mov.l       F23, [W7++]
        mov.l       F21, [W7++]

;------------------------------------------------------------------------------
; Next stage reads this stage's output, for all three channels - opt_v1's
; in-place cascade arrangement, done three times.
;------------------------------------------------------------------------------
L_x3td_nextStage:
        ; NOTE THE DISPLACEMENTS: the frame is FIVE words here, not x3t's four, so
        ; every pDst slot sits one word deeper than in x3t. [W15-8] is the triple
        ; count in this kernel - reading a pDst from it would use an integer as a
        ; pointer, which is why these three lines differ from x3t's.
        mov.l       [W15-20], W1      ; A: pSrc = original pDstA
        mov.l       [W15-20], W2      ; A: pDst = original pDstA
        mov.l       [W15-16], W8      ; B: pSrc = original pDstB
        mov.l       [W15-16], W9      ; B: pDst = original pDstB
        mov.l       [W15-12], W14     ; C: pSrc = original pDstC
        mov.l       [W15-12], W3      ; C: pDst = original pDstC

        dtb         W5, L_x3td_startFilter

;------------------------------------------------------------------------------
; Restore and return.
;------------------------------------------------------------------------------
        sub.l       #20, W15          ; FIVE frame words (x3t popped 16)

        pop.l       W14
        pop.l       W13
        pop.l       W12
        pop.l       W11
        pop.l       W10
        pop.l       W9
        pop.l       W8

        pop.l       FCR
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

L_x3td_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
