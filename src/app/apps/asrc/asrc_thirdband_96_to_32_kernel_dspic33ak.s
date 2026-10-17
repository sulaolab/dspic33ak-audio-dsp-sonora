;*****************************************************************************
; asrc_thirdband_96_to_32_kernel_dspic33ak.s
;
; SHIPPING kernel of the 96 -> 32 kHz third-band 3-path polyphase allpass
; decimator.  The ARITHMETIC BODY is byte-identical to kernel A of the bench
; (asrc_thirdband_kernel_dspic33ak.s, ASRC_TB_BODY_A) -- 1 multiply and 2 state
; words per section, 12 instructions per section per CHANNEL PAIR, measured on
; hardware at 5.995 cycles/section/channel against a static count of 6.000.
; Do not "improve" the body: its instruction ORDER is what keeps the assembler
; from inserting neops, and its numbers are published.
;
; WHAT IS DIFFERENT FROM THE BENCH, and why each difference had to exist
; (the bench's 106.19 cycles/32k output/ch did not pay any of these):
;
;   1 REAL I/O.  The bench re-read one 3-row buffer every frame and overwrote one
;     output row.  Here the input cursor WALKS a gathered 96 kHz block and the
;     output cursor walks the 32 kHz block, so the commutator actually consumes
;     3 input frames per output frame.  Two add.l per frame.
;
;   2 THE BRANCH SUM IS DONE IN THE ACCUMULATOR, and the 1/3 with it.  The bench
;     summed the three branches with 32-bit add.l and left the 1/3 to be folded
;     into the downstream resampler.  Neither survives in the real chain:
;       * the sum reaches 3.0 FULL SCALE and OVERFLOWS a 32-bit Q31 add.  The
;         bench never saw it because its test signal was small.
;       * the resampler coefficient table is ONE build-time table shared by both
;         legs and every rate, so a 1/3 folded there would rescale chains that do
;         not use this front end.
;     So the three branch results go through three MACs against a 17th
;     coefficient word, c = floor(2^31 / 3), and one sacr.l saturates.  The
;     accumulator guard bits carry the 3.0 that a register could not.
;
;   3 THE HEADROOM RESTORE RIDES IN THAT sacr.l.  The caller shifts the input
;     right by ASRC_TB32_HSHIFT before the kernel sees it, because the host gate
;     measured the SECTION OUTPUTS -- which are stored int32 words -- reaching
;     1.01x full scale on a 997 Hz full-scale sine, 2.85x on full-scale noise and
;     4.97x at the L1 bound.  The shift is EXACT on an s24-left input (8 zero
;     sub-LSBs), so it costs no precision, and sacr.l a, #-HSHIFT, Wd puts the
;     factor back with rounding and saturation for no instruction at all.
;     * DIRECTION: a NEGATIVE shift field is the LEFT (x2^n) direction.  The boot
;       check catches a wrong sign at once -- it would be a factor of 64 -- and the
;       sign lives in one .equ below for exactly that reason.
;
;   4 THE s24-LEFT MASK, which the shipping chain applies at the end of its last
;     stage (dec_q31_to_s24_left) and the bench did not.  One and.l against a
;     register-held mask, because and.l's immediate field only reaches 127.
;
; Cost of 2 + 3 + 4 together: FOUR instructions per output per channel.
;
; PLACEMENT IS PART OF THE MEASUREMENT (DS70005591C 4.3.17).  In
; msc.l [w11], [w9], A the two operands must live in DIFFERENT data spaces or
; both reads serialise and the instruction silently costs an extra cycle.  The
; caller must therefore keep the COEFFICIENTS IN X and the STATE IN Y.  It does
; that by carving both out of the front end's existing declared arenas
; (s_q31_coeff is space(xmemory), s_q31_hist is space(ymemory)), which is also
; why this kernel costs no new RAM.
;
; STATE MUST BE A FLAT [ch][16][2] BLOCK with a 128-byte channel stride.  That is
; the stride the body was measured at; addressing the history array by its own
; 264-word row stride instead would change it, and an untouched stage moving on a
; placement change is a trap this tree has already paid for once.
;
; MODULO IS BRACKETED OFF, not merely unused: MODCON/XMODSRT/YMODSRT are not part
; of the per-IPL register context, so a window opened by
; fir_ring_q31_ymod_yonly_block is open in every context, and this kernel
; addresses Y through the MAC AGU.  Four instructions per CALL.
;
; The .size directives are load-bearing: tools/asrc/ymod_safety_gate.py
; attributes any instruction outside every sized symbol to the nearest preceding
; label, so an unsized kernel absorbs whatever the linker places after it.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

; ---- geometry, fixed at assembly time ------------------------------------------------
; K = 16 sections, split 6/5/5 across the three polyphase branches (host design, worst
; alias into the protected 0-15 kHz band -110.33 dBc, -110.11 dBc measured with this
; implementation's exact integer arithmetic).
    .equ    ASRC_TB32_S0,       6
    .equ    ASRC_TB32_S1,       5
    .equ    ASRC_TB32_S2,       5
    .equ    ASRC_TB32_SEC,      (ASRC_TB32_S0 + ASRC_TB32_S1 + ASRC_TB32_S2)

; State is [ch][section][{x1,y1}], so a channel's row is SEC*2 words and w9/w10 walk a
; channel's sections with a constant +8 and never need a multiply.  Independent of the
; channel count on purpose -- see the header.
    .equ    ASRC_TB32_CHSTRIDE, (ASRC_TB32_SEC * 8)
; The coefficient row is SEC words followed by ONE more: c = floor(2^31/3).  The body
; advances w11 by 4 per section, so after the last section w11 points AT that word --
; which is why the 1/3 costs no pointer arithmetic.
    .equ    ASRC_TB32_CREWIND,  (ASRC_TB32_SEC * 4)

; Input headroom, applied by the caller as a right shift and restored here.  See header
; note 3.  A NEGATIVE sacr.l shift field is the left direction.
    .ifndef ASRC_TB32_HSHIFT
    .equ    ASRC_TB32_HSHIFT,   3
    .endif
    .equ    ASRC_TB32_RESTORE,  (0 - ASRC_TB32_HSHIFT)

;=====================================================================================
; ONE SECTION, TWO CHANNELS -- byte-identical to the bench's ASRC_TB_BODY_A.
;
;   A = x1                      lac.l [w9++]      (x1 read for free, w9 -> y1)
;   A = A + a*x                 mac.l [w11], w4   (coefficient read for free)
;   A = A - a*y1                msc.l [w11], [w9] (y1 read for free)
;   x1 = x                      mov.l w4, [w9-4]
;   y  = round(A)               sacr.l A, w4      (y becomes the next section's x)
;   y1 = y                      mov.l w4, [w9++]  (w9 -> next section)
;
; Order is scheduled, not incidental: the y1 READ precedes the y1 WRITE, the x store
; precedes sacr.l overwriting w4, and each channel's dependent pair is separated by the
; other channel's instruction.  The assembler inserts no neop in this body.
;=====================================================================================
    .macro  ASRC_TB32_BODY
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

;=====================================================================================
;   void asrc_thirdband_q31_block_nchN(
;           int32_t       *state,   /* w0  Y space, [N][16][2], zeroed by the caller */
;           const int32_t *coeff,   /* w1  X space, 16 sections then floor(2^31/3)   */
;           const int32_t *in,      /* w2  gathered 96 kHz block, [3*frames][N],
;                                          ALREADY shifted right by ASRC_TB32_HSHIFT  */
;           int32_t       *out,     /* w3  32 kHz block, [frames][N] CONTIGUOUS      */
;           uint32_t       frames); /* w4  >= 1                                      */
;
; `out` is contiguous at the channel count, NOT at a caller-chosen stride.  That is what
; frees the register a stride would have needed: with no stride there is no per-frame
; output fix-up at all and w3 simply walks.  The C side asserts it and stages the output
; itself if a caller ever asks for a wider stride.
;
; Register budget is exactly full (w15 is the stack pointer), so nothing here is
; stylistic: w0 state base, w1/w2 the two channels' branch-1 results (both arguments are
; dead after the entry copies), w3 output cursor, w4/w5 the sample pair, w6/w7 the
; branch-0 results, w8 pair counter, w9/w10 state cursors, w11 coefficient cursor,
; w12 input cursor, w13 frame counter, w14 the s24-left mask.
;=====================================================================================
    .macro  ASRC_TB32_KERNEL name, nch
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

    mov.l   w4, w13                     ; frame counter
    mov.l   w1, w11                     ; coefficient cursor; w1 free from here
    mov.l   w2, w12                     ; input cursor; w2 free from here
    mov.l   #0xFFFFFF00, w14            ; s24-left mask (and.l's immediate stops at 127)

9:                                      ; frame loop
    mov.l   w0, w9                      ; channel 2p   state
    mov.l   w0, w10
    add.l   #ASRC_TB32_CHSTRIDE, w10    ; channel 2p+1 state
    mov.l   #(\nch / 2), w8

8:                                      ; channel-pair loop
    ; THE COMMUTATOR READS THE TRIPLE BACKWARDS IN ADDRESS ORDER.  out[q] is
    ; A0(x[3q]) + A1(x[3q-1]) + A2(x[3q-2]), and the gathered block is in TIME order, so
    ; branch 0 -- the one that takes the NEWEST of the three -- is at the HIGHEST address.
    ; Doing it here rather than reversing each triple in the gather is free; reversing in
    ; the gather would cost an instruction per input sample.
    mov.l   [w12+(\nch*8)], w4          ; branch 0 input = x[3q],   both channels
    mov.l   [w12+(\nch*8)+4], w5
    .rept   ASRC_TB32_S0
    ASRC_TB32_BODY
    .endr
    mov.l   w4, w6                      ; keep b0
    mov.l   w5, w7

    mov.l   [w12+(\nch*4)], w4          ; branch 1 input = x[3q-1]
    mov.l   [w12+(\nch*4)+4], w5
    .rept   ASRC_TB32_S1
    ASRC_TB32_BODY
    .endr
    mov.l   w4, w1                      ; keep b1
    mov.l   w5, w2

    mov.l   [w12], w4                   ; branch 2 input = x[3q-2], the oldest
    mov.l   [w12+4], w5
    .rept   ASRC_TB32_S2
    ASRC_TB32_BODY
    .endr

    ; w11 now points at coeff[SEC] = floor(2^31/3).  Sum the three branches INSIDE the
    ; accumulator -- a 32-bit add of them would overflow at 3.0 full scale -- and take
    ; the headroom factor back out in the same store.
    mpy.l   [w11], w6, a                ; a = c*b0
    mpy.l   [w11], w7, b
    mac.l   [w11], w1, a                ; a += c*b1
    mac.l   [w11], w2, b
    mac.l   [w11], w4, a                ; a += c*b2
    mac.l   [w11], w5, b
    sacr.l  a, #ASRC_TB32_RESTORE, w4   ; x2^HSHIFT, round, saturate
    sacr.l  b, #ASRC_TB32_RESTORE, w5
    and.l   w4, w14, w4                 ; s24-left, as the shipping chain's last stage
    and.l   w5, w14, w5
    mov.l   w4, [w3++]
    mov.l   w5, [w3++]

    add.l   w12, #8, w12                ; next channel pair's commutator column
    mov.l   w10, w9                     ; w10 already points at channel 2p+2
    add.l   #ASRC_TB32_CHSTRIDE, w10
    sub.l   #ASRC_TB32_CREWIND, w11     ; rewind the coefficient row for the next pair
    dtb     w8, 8b

    ; The pair loop advanced w12 by nch*4, i.e. to the END of this output frame's first
    ; input row.  The next output frame's first row is 3*nch*4 from this one's.
    ; Two-operand form: add.l's THREE-operand immediate stops at 31, the two-operand one
    ; does not, and nch*8 is 96 or 128.
    add.l   #(\nch * 8), w12
    dtb     w13, 9b

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

; One section each: --gc-sections collects on sections, and every MPLAB configuration in
; this project sets isolate-each-function and remove-unused-sections, so a 12-channel
; image carries no 16-channel kernel and vice versa.  The C side selects on ASRC_CH and
; #errors on anything else rather than picking a width silently.
    .section .asrc_thirdband_32_nch12, code
    ASRC_TB32_KERNEL _asrc_thirdband_q31_block_nch12, 12

    .section .asrc_thirdband_32_nch14, code
    ASRC_TB32_KERNEL _asrc_thirdband_q31_block_nch14, 14

    .section .asrc_thirdband_32_nch16, code
    ASRC_TB32_KERNEL _asrc_thirdband_q31_block_nch16, 16

    .end
