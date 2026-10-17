;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x3t.s
;
; THREE CHANNELS, THREE SAMPLES PER ITERATION, THREE WORK REGISTERS PER CHANNEL,
; AND NO COPY INSTRUCTIONS AT ALL.
;
; A CANDIDATE. This is not the previous rotation with one more sample bolted on -
; it is a different register topology, and it is the first thing in this family
; that removes an instruction without adding one somewhere else.
;
; WHY THIS SHAPE EXISTS
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
;   No division is computed anywhere: this ISA has no integer divide (`div.l` is
;   an invalid mnemonic here - checked with the assembler, not assumed). The
;   sample loop counts DOWN by three while at least three remain, so the
;   remainder is simply what is left in the counter when it falls through.
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

        .section .dspic33cmsisdsp_df2t_x3t, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x3t

;------------------------------------------------------------------------------
; void biquad_cascade_df2T_f32_dspic33ak_x3t(
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
_biquad_cascade_df2T_f32_dspic33ak_x3t:

;------------------------------------------------------------------------------
; Refuse a zero blockSize before anything is pushed or written, so the refusal
; path is a plain return. Odd is NOT refused - this kernel takes any length.
;------------------------------------------------------------------------------
        cp.l        W3, #0
        bra         z, L_x3t_refuse

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
; NO DIVISION IS COMPUTED, on purpose.
;
; The obvious way to unroll by three is to precompute blockSize/3 and drive a dtb
; with it. This ISA has no integer divide (`div.l` is an invalid mnemonic here -
; checked with the assembler, not assumed), so that would need a reciprocal
; multiply and a 64-bit product register pair.
;
; The sample loop below instead counts DOWN by three while at least three remain,
; which needs no division AND leaves the remainder sitting in the counter when it
; falls through - and the remainder is exactly what selects the state write-back.
; One construct supplies both. It costs two instructions per iteration against
; dtb's one, i.e. 0.111 per channel-sample, which is a better trade than an
; untested multiply form inside a kernel this exact.
;------------------------------------------------------------------------------

;------------------------------------------------------------------------------
; Frame (W15 grows up), four words - the same shape x3/x3r use:
;   [W15-16] = pDstA original
;   [W15-12] = pDstB original
;   [W15-8]  = pDstC original
;   [W15-4]  = blockSize      (reloaded into the sample counter each stage)
;
; W0 is dead after the instance block and is the only scratch this shuffle needs.
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

;==============================================================================
; Stage loop
;==============================================================================
L_x3t_startFilter:

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

        mov.l       [W15-4], W4       ; W4 = samples remaining in this stage

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
L_x3t_startTriples:
        cp.l        W4, #3
        bra         ltu, L_x3t_tail   ; fewer than three left -> tail

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

        sub.l       #3, W4
        bra         L_x3t_startTriples

;==============================================================================
; Tail - 0, 1 or 2 samples, one at a time, the SAME seven instructions.
;
; This is what keeps the 1..512 contract that x2r and x3r gave up. Each tail
; sample rotates the roles by one, so after the tail d1 and d2 are NOT in F5/F6 -
; the write-back below selects by remainder.
;==============================================================================
L_x3t_tail:
        ; W4 holds the remainder (0, 1 or 2): the loop above fell through on it,
        ; so nothing has to be recomputed or reloaded.
        cp.l        W4, #0
        bra         z, L_x3t_store_p0

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
        bra         z, L_x3t_store_p1

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

        bra         L_x3t_store_p2

;------------------------------------------------------------------------------
; State write-back, one variant per ending phase. Only the register NAMES differ;
; the addresses and the order are identical, so a reader can check the three
; against each other.
;------------------------------------------------------------------------------
L_x3t_store_p0:                       ; remainder 0: d1 in F5 / F13 / F21
        mov.l       F5, [W12++]
        mov.l       F6, [W12++]
        mov.l       F13, [W13++]
        mov.l       F14, [W13++]
        mov.l       F21, [W7++]
        mov.l       F22, [W7++]
        bra         L_x3t_nextStage

L_x3t_store_p1:                       ; remainder 1: d1 in F6 / F14 / F22
        mov.l       F6, [W12++]
        mov.l       F7, [W12++]
        mov.l       F14, [W13++]
        mov.l       F15, [W13++]
        mov.l       F22, [W7++]
        mov.l       F23, [W7++]
        bra         L_x3t_nextStage

L_x3t_store_p2:                       ; remainder 2: d1 in F7 / F15 / F23
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
L_x3t_nextStage:
        mov.l       [W15-16], W1      ; A: pSrc = original pDstA
        mov.l       [W15-16], W2      ; A: pDst = original pDstA
        mov.l       [W15-12], W8      ; B: pSrc = original pDstB
        mov.l       [W15-12], W9      ; B: pDst = original pDstB
        mov.l       [W15-8],  W14     ; C: pSrc = original pDstC
        mov.l       [W15-8],  W3      ; C: pDst = original pDstC

        dtb         W5, L_x3t_startFilter

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

L_x3t_refuse:
        return

;==============================================================================
; End of file
;==============================================================================
