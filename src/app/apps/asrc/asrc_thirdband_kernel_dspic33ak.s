;*****************************************************************************
; asrc_thirdband_kernel_dspic33ak.s
;
; MEASUREMENT ONLY.  Four hand-written Q31 kernels for the 3-path polyphase
; allpass 96 -> 32 kHz decimator studied in
; [internal] report_96to32_thirdband_kernel_bench_2026-09-09.md.  Nothing on the audio
; path calls them; the only caller is asrc_fir_kernel_bench.c behind
; ASRC_FIR_KERNEL_BENCH_AVAILABLE.  Each entry point has its own code section so
; --gc-sections drops the lot from any image that does not enable the bench.
;
; WHY THEY EXIST.  The C form of this filter measured 18.641 cycles/section
; (Q31) and 11.323 (float32) on 33AK512MPS512, against a shipping front end of
; 111.3 cycles per 32 kHz output per channel -- i.e. the multiply count said the
; structure should win 6.9x and the hardware said it loses.  The diagnosis was
; that an allpass section pays FOUR memory accesses per multiply (load x1, load
; y1, store x1, store y1) where an FIR pays two AGU-incremented reads with the
; accumulator resident in a register.  These kernels test that diagnosis against
; the instruction set instead of against a C compiler.
;
; * THE ISA FACT THAT REFRAMES THE WHOLE QUESTION.  dsPIC33A has MIXED
; register x memory MAC forms -- `mac.l [w11], w4, A` and `msc.l [w11], [w9], A`
; both assemble to ONE 4-byte instruction (verified in the object file, not
; assumed; the probe log is in the report).  So a memory operand of a multiply
; costs NO separate load instruction.  The consequence is that "four memory
; accesses per multiply" is NOT four instructions: only the two STORES cost an
; instruction, and the two loads ride inside `lac.l [w9++], A` and
; `msc.l [w11], [w9], A` for free.  That is why kernel B below -- the deliberate
; "halve the memory traffic, double the multiplies" counter-bet -- comes out
; SLOWER than A in static instruction count rather than faster.
;
; THE TWO STRUCTURES
;
;   A   1 multiply / 2 state words.   y = a*(x - y1) + x1
;       Written as a = x1 + a*x - a*y1 so that both products can take their
;       memory operand in place.  12 instructions per section per CHANNEL PAIR
;       = 6.0 per section per channel.
;
;   B   2 multiplies / 1 state word.  v = x - a*s ;  y = a*v + s ;  s = v
;       Y/X = (a + z^-1)/(1 + a z^-1), the same allpass.  Needs two sacr.l and
;       two accumulator loads where A needs one of each, so it is 14 instructions
;       per section per channel pair = 7.0 per section per channel.  Measured
;       anyway, because instruction count is not cycle count when the load/store
;       unit is the contended resource -- which is the entire hypothesis.
;
; * SPLIT PRODUCTS ARE NOT BIT-EXACT WITH ONE PRODUCT OF THE DIFFERENCE.
; mchp_asrc_q31_row16.s records the measurement: on this silicon a fractional
; mac.l does not contribute the full 64-bit product, so `a*x - a*y1` lands 1..2
; LSB low against `a*(x - y1)`.  Kernel A takes that deliberately -- at Q31 two
; LSB is -180 dBFS and the alias floor being bought is -110 dBc -- and the bench
; gates on a double reference with a tolerance far above it.  A shipping version
; of this filter would have to re-qualify the rounding, which is noted in the
; report rather than papered over here.
;
; PLACEMENT IS PART OF THE MEASUREMENT (DS70005591C 4.3.17).  In
; `msc.l [w11], [w9], A` the two operands must live in DIFFERENT data spaces or
; both reads serialise and the instruction silently costs an extra cycle -- the
; same 1.012 -> 2.000 cycles/MAC trap the FIR kernels document.  The caller must
; therefore put the COEFFICIENTS IN X and the STATE IN Y.  The bench prints both
; addresses and refuses to believe its own numbers if they are not split.
;
; MODULO IS BRACKETED OFF, not merely unused.  MODCON/XMODSRT/YMODSRT are not
; part of the per-IPL register context, so a window opened by
; fir_ring_q31_ymod_yonly_block is open in every context.  These kernels address
; Y through the MAC AGU and so would inherit it.  They save MODCON, disable
; modulo in both AGUs, and restore what they found -- four instructions per CALL,
; i.e. per measured block, not per section.
;
; The .size directives are load-bearing: tools/asrc/ymod_safety_gate.py
; attributes any instruction outside every sized symbol to the nearest preceding
; label, so an unsized kernel absorbs whatever the linker places after it.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

; ---- geometry, fixed at assembly time ------------------------------------------------
; The channel count is the shipping 16 and the interleave IS the point: two channels are
; processed together so that every accumulator and every AGU register is written at least
; one instruction before it is read, which is what keeps the body free of assembler-
; inserted neops (verified in the disassembly).  A single-channel version of the same
; arithmetic would stall on its own serial dependency.
    .ifndef ASRC_TB_NCH
    .equ    ASRC_TB_NCH, 16
    .endif

    .equ    ASRC_TB_PAIRS,      (ASRC_TB_NCH / 2)
    .equ    ASRC_TB_BRANCH_OFF, (ASRC_TB_NCH * 4)     ; bytes between in[b] rows
    .equ    ASRC_TB_BRANCH_OF2, (ASRC_TB_NCH * 8)
    .equ    ASRC_TB_OUT_OFF,    (ASRC_TB_NCH * 12)    ; out[] follows in[3][NCH]

; The K=16 design splits 6/5/5 across the three polyphase branches (host design, worst
; alias -110.33 dBc).  K=8 is the second point the slope is taken from; 3/3/2 is the same
; (sections+2)/3 rule the C bench uses, so both forms cascade identically.
    .equ    ASRC_TB_K16_S0, 6
    .equ    ASRC_TB_K16_S1, 5
    .equ    ASRC_TB_K16_S2, 5
    .equ    ASRC_TB_K8_S0,  3
    .equ    ASRC_TB_K8_S1,  3
    .equ    ASRC_TB_K8_S2,  2

;=====================================================================================
; BODY MACROS -- one section, two channels.
;=====================================================================================

; A: 1 multiply, 2 state words.  State is [ch][section][{x1, y1}], so w9/w10 walk a
; channel's sections with a constant +8 and never need a multiply.
;
;   A = x1                      lac.l [w9++]      (x1 read for free, w9 -> y1)
;   A = A + a*x                 mac.l [w11], w4   (coefficient read for free)
;   A = A - a*y1                msc.l [w11], [w9] (y1 read for free)
;   x1 = x                      mov.l w4, [w9-4]
;   y  = round(A)               sacr.l A, w4      (y becomes the next section's x)
;   y1 = y                      mov.l w4, [w9++]  (w9 -> next section)
;
; Order is scheduled, not incidental: the y1 READ (msc.l) precedes the y1 WRITE, the x
; store precedes sacr.l overwriting w4, and each channel's dependent pair is separated by
; the other channel's instruction.
    .macro  ASRC_TB_BODY_A
    lac.l   [w9++], a
    lac.l   [w10++], b
    mac.l   [w11], w4, a
    mac.l   [w11], w5, b
    msc.l   [w11], [w9], a
    msc.l   [w11]+=4, [w10], b
    mov.l   w4, [w9-4]
    mov.l   w5, [w10-4]
    sacr.l  a, w4
    sacr.l  b, w5
    mov.l   w4, [w9++]
    mov.l   w5, [w10++]
    .endm

; B: 2 multiplies, 1 state word.  State is [ch][section], so w9/w10 walk with +4 and the
; per-channel arena is half A's.
;
;   A = x                       lac.l w4
;   A = x - a*s   (= v)         msc.l [w11], [w9]   (s read for free)
;   v = round(A)                sacr.l A, w3
;   A = s                       lac.l [w9]          (s still the OLD value here)
;   A = s + a*v   (= y)         mac.l [w11], w3
;   s = v                       mov.l w3, [w9++]
;   y = round(A)                sacr.l A, w4
;
; The extra cost against A is exactly one sacr.l and one lac.l per section per channel:
; B saves one STORE and pays two accumulator round-trips for it.  w3/w13 hold v for the
; two channels -- free because w11 rewinds itself and the coefficient base is never
; needed again after entry.
    .macro  ASRC_TB_BODY_B
    lac.l   w4, a
    lac.l   w5, b
    msc.l   [w11], [w9], a
    msc.l   [w11], [w10], b
    sacr.l  a, w3
    sacr.l  b, w13
    lac.l   [w9], a
    lac.l   [w10], b
    mac.l   [w11], w3, a
    mac.l   [w11]+=4, w13, b
    mov.l   w3, [w9++]
    mov.l   w13, [w10++]
    sacr.l  a, w4
    sacr.l  b, w5
    .endm

;=====================================================================================
; SKELETON -- the frame loop, the 3-branch commutator, the branch sum and the output
; store.  This is inside the measured window on purpose: the C form's 24.4 cycles of
; per-output fixed cost was the SECOND of the two levers the study identified, and
; leaving it in C would have measured only the first.
;
;   void <name>(int32_t *state,        /* w0  Y space  */
;               const int32_t *coeff,  /* w1  X space  */
;               int32_t *io,           /* w2  in[3][NCH] then out[NCH], one buffer */
;               uint32_t frames);      /* w3  >= 1     */
;
; `io` is ONE buffer rather than two pointers so that a single register addresses all six
; commutator reads and both output writes by displacement -- which is what frees the two
; scratch registers kernel B needs.  Register budget is exactly full (w15 is the stack
; pointer), so nothing here is stylistic.
;
; The input is re-read every frame and the output overwritten every frame, which is what
; the C bench does: the cost being measured is the kernel's, and feeding it fresh samples
; would add a generator to the window.  The FILTER STATE does advance frame to frame, so
; the arithmetic is a real 16-frame run and not the same section repeated.
;=====================================================================================
    .macro  ASRC_TB_KERNEL name, body, s0, s1, s2, chstride, rewind
    .global \name
    .type   \name, @function
\name:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  CORCON
    push.l  MODCON

    fractsetup w8                       ; fractional mode: sets sacr.l alignment
    mov.l   #0x00FF, w8                 ; XWM=YWM=1111, no EN bit: modulo OFF both AGUs
    mov.l   w8, MODCON

    mov.l   w3, w14                     ; frame counter
    mov.l   w1, w11                     ; coefficient pointer; rewound per pair below, so
                                        ; w1 is dead from here and is free scratch

9:                                      ; frame loop
    mov.l   w0, w9                      ; channel 2p   state
    mov.l   w0, w10
    add.l   #\chstride, w10             ; channel 2p+1 state
    mov.l   w2, w12                     ; commutator / output cursor
    mov.l   #ASRC_TB_PAIRS, w8

8:                                      ; channel-pair loop
    mov.l   [w12], w4                   ; branch 0 input, both channels
    mov.l   [w12+4], w5
    .rept   \s0
    \body
    .endr
    mov.l   w4, w6                      ; branch 0 result starts the sum
    mov.l   w5, w7

    mov.l   [w12+ASRC_TB_BRANCH_OFF], w4
    mov.l   [w12+ASRC_TB_BRANCH_OFF+4], w5
    .rept   \s1
    \body
    .endr
    add.l   w6, w4, w6
    add.l   w7, w5, w7

    mov.l   [w12+ASRC_TB_BRANCH_OF2], w4
    mov.l   [w12+ASRC_TB_BRANCH_OF2+4], w5
    .rept   \s2
    \body
    .endr
    add.l   w6, w4, w6
    add.l   w7, w5, w7

    ; The 1/3 of the 3-path average is folded into the downstream resampler's
    ; coefficients in the real chain, exactly as the C bench does, so no shift is
    ; charged here.  Charging one would measure a choice nobody would make.
    mov.l   w6, [w12+ASRC_TB_OUT_OFF]
    mov.l   w7, [w12+ASRC_TB_OUT_OFF+4]

    add.l   w12, #8, w12                ; next channel pair's commutator column
    mov.l   w10, w9                     ; w10 already points at channel 2p+2
    add.l   #\chstride, w10
    sub.l   #\rewind, w11               ; rewind the coefficient row for the next pair
    dtb     w8, 8b

    dtb     w14, 9b

    pop.l   MODCON
    pop.l   CORCON
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return
    .size   \name, .-\name
    .endm

; Each in its own section: --gc-sections collects on sections, and every MPLAB
; configuration in this project sets isolate-each-function and remove-unused-sections.
    .section .asrc_thirdband_a_k16, code
    ASRC_TB_KERNEL _asrc_tb_a_q31_k16, ASRC_TB_BODY_A, ASRC_TB_K16_S0, ASRC_TB_K16_S1, ASRC_TB_K16_S2, 128, 64

    .section .asrc_thirdband_a_k8, code
    ASRC_TB_KERNEL _asrc_tb_a_q31_k8, ASRC_TB_BODY_A, ASRC_TB_K8_S0, ASRC_TB_K8_S1, ASRC_TB_K8_S2, 64, 32

    .section .asrc_thirdband_b_k16, code
    ASRC_TB_KERNEL _asrc_tb_b_q31_k16, ASRC_TB_BODY_B, ASRC_TB_K16_S0, ASRC_TB_K16_S1, ASRC_TB_K16_S2, 64, 64

    .section .asrc_thirdband_b_k8, code
    ASRC_TB_KERNEL _asrc_tb_b_q31_k8, ASRC_TB_BODY_B, ASRC_TB_K8_S0, ASRC_TB_K8_S1, ASRC_TB_K8_S2, 32, 32

    .end
