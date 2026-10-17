;*****************************************************************************
; mchp_asrc_q31_row16.s
;
; NOT a Microchip file.  Written for this project; it sits in the vendored tree
; beside the mchp_stream8_* float kernels it replaces because audio_app_asrc.c
; reaches all of its hot kernels through that one directory.
;
; Q31 generic polyphase ASRC inner loops.  Two entry points:
;
;   _mchp_asrc_q31_blend_row  -- build ONE blended coefficient row from the two
;                                neighbouring polyphase rows.  Cost is paid once
;                                per output frame and amortised over all 16
;                                channels, which is the whole reason the Q31
;                                resampler does not need the float kernels'
;                                "hoisted blend" (ce) / "union window" (ced)
;                                machinery, and therefore has no step != 1
;                                penalty.
;
;   _mchp_asrc_q31_row16      -- the dot products: one Q31 FIR per channel
;                                against that shared blended row.
;
; PLACEMENT REQUIREMENT (DS70005591C 4.3.17).  In _mchp_asrc_q31_row16 the two
; MAC operands must live in DIFFERENT spaces or both reads serialise and the MAC
; silently costs one extra cycle (measured elsewhere in this tree as exactly
; 1.012 -> 2.000 cycles/MAC, with no other symptom).  The caller must therefore
; place the sample history in X and the blended row in Y (or the reverse).  Only
; the ~120-byte blended row needs forcing; the 15,480-byte polyphase table is
; read by ordinary MCU-class loads in the blend, not by the MAC AGU.
;
; MODULO IS BRACKETED OFF, NOT MERELY UNUSED.  MODCON/XMODSRT/YMODSRT are NOT
; part of the per-IPL register context (DS70005591C Table 4-2), so a modulo
; window opened by another kernel is open in EVERY context.  Both kernels here
; run from asrc_pull in interrupt context, and mchp_asrc_q31_row16 addresses Y
; (the blended row) through the MAC AGU, so it can preempt
; fir_ring_q31_ymod_yonly_block while its ring window is open and would inherit
; it.  It therefore saves MODCON,
; disables modulo in both AGUs for the duration of its own use, and restores
; what it found.  This costs 4 instructions per CALL -- once per output frame,
; amortised over all 16 channels -- not per tap.  mchp_asrc_q31_blend_row
; deliberately does NOT bracket: its only indexed operand is a sacr.l store,
; which is not a MAC-class prefetch and so cannot reach the Y AGU, and a
; modulo write with no AGU use of its own is pure risk (the gate rejects it).
;
; The .size directives below are not decoration: tools that recover function
; boundaries from the image (tools/asrc/ymod_safety_gate.py) attribute any
; instruction outside every sized symbol to the nearest preceding label, so an
; unsized hand-written kernel silently absorbs whatever the linker happens to
; place after it -- including another function's retfie, which then reads as
; "this kernel is an interrupt handler".
;
; NO MODULO ADDRESSING.  The history ring is mirrored by asrc_push (the first M samples are
; written twice), so every tap window is a contiguous span.  Both kernels use
; plain post-increment addressing and never touch MODCON/XMODSRT/YMODSRT, which
; sidesteps the non-banked-modulo hazard documented in
; fir_ring_q31_ymod_yonly_dspic33ak.s entirely.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

;-----------------------------------------------------------------------------
;   void mchp_asrc_q31_blend_row(const int32_t *c0,    /* w0  row p     */
;                               const int32_t *c1,    /* w1  row p+1   */
;                               int32_t       *ceff,  /* w2  taps out  */
;                               uint32_t       taps,  /* w3  >= 1      */
;                               int32_t        wbq);  /* w4  Q31 [0,1) */
;
;   ceff[k] = c0[k] + round_q31( wbq * (c1[k] - c0[k]) )
;
;   The subtraction cannot overflow int32: both rows are Q31 values bounded by
;   +-0.93 (max|c| measured 0.929998994), so |delta| < 1.86 in Q31 units, i.e.
;   under 2^31.  The add-back happens inside ACCA, whose guard bits put the sum
;   far above anything Q31 operands can reach, so the only saturation point is
;   sacr.l.  The arithmetic model this must match is the one the FIR kernel
;   bench verified against hardware at 0 LSB error (firb_ref_q31): a fractional
;   MAC of two Q31 values adds the product shifted left by one, and sacr.l
;   rounds at bit 31 -- i.e. (sum + 2^30) >> 31 over UNDOUBLED int64 products,
;   which is exactly what asrc_poly_q31.inc's C reference computes.
;-----------------------------------------------------------------------------
    .global    _mchp_asrc_q31_blend_row
    .type      _mchp_asrc_q31_blend_row, @function
_mchp_asrc_q31_blend_row:

    push.l  w8
    push.l  CORCON
    fractsetup w8                   ; fractional mode: sets sacr.l alignment

_q31_blend_tap:
    mov.l   [w0++], w5              ; c0[k]
    mov.l   [w1++], w6              ; c1[k]
    lac.l   w5, a                   ; a  = c0[k]                (Q31 into ACCA)
    sub.l   w6, w5, w6              ; delta = c1[k] - c0[k]
    mac.l   w4, w6, a               ; a += wbq * delta          (fractional)
    sacr.l  a, [w2++]               ; round + saturate to Q31
    dtb     w3, _q31_blend_tap

    pop.l   CORCON
    pop.l   w8
    return
    .size   _mchp_asrc_q31_blend_row, .-_mchp_asrc_q31_blend_row

;-----------------------------------------------------------------------------
;   void mchp_asrc_q31_blend_row_quad(const int32_t *c0,   /* w0  row p     */
;                                     const int32_t *c1,   /* w1  row p+1   */
;                                     const int32_t *c2,   /* w2  row p+2   */
;                                     int32_t       *ceff, /* w3  taps out  */
;                                     uint32_t       taps, /* w4  >= 1      */
;                                     int32_t        wbq,  /* w5  Q31 w     */
;                                     int32_t        gq);  /* w6  Q31 w(w-1)/2 */
;
;   t = round_q31( c0 + wbq*d1 );  ceff[k] = round_q31( t + gq*d2 )
;   d1 = c1-c0,  d2 = (c2-c1)-d1
;
;   RESEARCH (APP_ASRC_REAR_QUAD_BLEND, Hi-Fi 48 -> 44.1 kHz study).  Two chained copies of
;   _mchp_asrc_q31_blend_row's lac/mac/sacr -- NOT lac/mac/mac/sacr, which the note at
;   _mchp_asrc_q31_blend_row_opt measured 1..2 LSB low.  Oracle asrc_q31_ref_blend_quad().  Own section,
;   so --gc-sections drops it from every image that does not call it.  No modulo, no AGU MAC
;   prefetch (register operands only), so MODCON is not touched.
;-----------------------------------------------------------------------------
    .section .dspic33cmsisdsp_quad_blend_row, code
    .global    _mchp_asrc_q31_blend_row_quad
    .type      _mchp_asrc_q31_blend_row_quad, @function
_mchp_asrc_q31_blend_row_quad:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  CORCON
    fractsetup w8                   ; fractional mode: sets sacr.l alignment
    asr.l   w4, #1, w4              ; taps in PAIRS (M is even; the C side asserts it)

    ; Two taps per pass, the same per-tap arithmetic as before -- only the ORDER changes, so
    ; that no instruction reads the register the one before it wrote (the sub.l -> use latency
    ; the _opt note above measured as a neop).  ACCA only, as the other kernels here.
_q31_quad_pair:
    mov.l   [w0++], w7              ; c0[k]
    mov.l   [w1++], w8              ; c1[k]
    mov.l   [w2++], w9              ; c2[k]
    mov.l   [w0++], w10             ; c0[k+1]
    mov.l   [w1++], w11             ; c1[k+1]
    mov.l   [w2++], w12             ; c2[k+1]
    sub.l   w8, w7, w13             ; d1 k
    sub.l   w11, w10, w14           ; d1 k+1
    sub.l   w9, w8, w9              ; e1 k
    sub.l   w12, w11, w12           ; e1 k+1
    sub.l   w9, w13, w9             ; d2 k   = e1 - d1
    sub.l   w12, w14, w12           ; d2 k+1
    lac.l   w7, a                   ; stage 1, tap k:   a = c0 + wbq*d1
    mac.l   w5, w13, a
    sacr.l  a, w7                   ; t k   (= the shipped linear blend, bit for bit)
    lac.l   w10, a                  ; stage 1, tap k+1
    mac.l   w5, w14, a
    sacr.l  a, w10                  ; t k+1
    lac.l   w7, a                   ; stage 2, tap k:   ceff = t + gq*d2
    mac.l   w6, w9, a
    sacr.l  a, [w3++]
    lac.l   w10, a                  ; stage 2, tap k+1
    mac.l   w6, w12, a
    sacr.l  a, [w3++]
    dtb     w4, _q31_quad_pair

    pop.l   CORCON
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return
    .size   _mchp_asrc_q31_blend_row_quad, .-_mchp_asrc_q31_blend_row_quad

;-----------------------------------------------------------------------------
;   _mchp_asrc_q31_blend_row_quad4(c0, c1, c2, ceff, taps, wbq, gq)   w0..w6
;   _mchp_asrc_q31_blend_row_quad's arithmetic, bit for bit, read FOUR TAPS PER ROW AT A TIME.
;
;   WHY.  The boot bench (2026-10-11, ...184) put the pair kernel above at 924 cycles from flash
;   against 492 from RAM: the cost is the CPU data reads of program flash, not the instructions.
;   Those reads go through the NVM controller's data read buffer, and every switch to another
;   row is a new flash line.  With the table padded to a 16-byte row stride and 16-byte aligned
;   (ASRC_Q31_ROW_STRIDE / ASRC_Q31_TABLE_ALIGN), taps 4j..4j+3 of a row are one line, so this
;   kernel takes one line per row per group of four instead of one per row per pair: 646 cycles.
;   (Probed and dropped the same night: loads grouped per row in pairs, 772; a precomputed d2
;   table, 696; a packed {c0,d1,d2/4} stream, 576 but no longer exact.)
;
;   d1 = c1 - c0 is formed with subr.l straight from c1's stream; d2 = (c2 - c0) - d1 - d1,
;   which is (c2 - c1) - d1 modulo 2^32, i.e. the same 32-bit value _quad forms with sub.l.
;   Two taps run side by side in ACCA and ACCB; each stage is still one lac/mac/sacr.
;   taps must be 4n + 2 (the C side asserts it): n groups of four, then one pair.
;-----------------------------------------------------------------------------
    .section .dspic33cmsisdsp_quad_blend_row4, code
    .global    _mchp_asrc_q31_blend_row_quad4
    .type      _mchp_asrc_q31_blend_row_quad4, @function
_mchp_asrc_q31_blend_row_quad4:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  CORCON
    fractsetup w8
    asr.l   w4, #2, w4              ; groups of four (the trailing pair is done below)

_q31_q4_group:
    mov.l   [w0++], w7              ; c0 k..k+3  (one flash line)
    mov.l   [w0++], w8
    mov.l   [w0++], w9
    mov.l   [w0++], w10
    subr.l  w7, [w1++], w11         ; d1 = c1 - c0, k..k+3  (one flash line)
    subr.l  w8, [w1++], w12
    subr.l  w9, [w1++], w13
    subr.l  w10, [w1++], w14
    ; taps k, k+1
    lac.l   w7, a
    lac.l   w8, b
    mac.l   w5, w11, a
    mac.l   w5, w12, b
    subr.l  w7, [w2++], w7          ; c2 - c0
    subr.l  w8, [w2++], w8
    sub.l   w7, w11, w7
    sub.l   w8, w12, w8
    sub.l   w7, w11, w7             ; d2 k
    sub.l   w8, w12, w8             ; d2 k+1
    sacr.l  a, w11                  ; t k   (= the shipped linear blend)
    sacr.l  b, w12                  ; t k+1
    lac.l   w11, a
    lac.l   w12, b
    mac.l   w6, w7, a
    mac.l   w6, w8, b
    sacr.l  a, [w3++]
    sacr.l  b, [w3++]
    ; taps k+2, k+3
    lac.l   w9, a
    lac.l   w10, b
    mac.l   w5, w13, a
    mac.l   w5, w14, b
    subr.l  w9, [w2++], w9
    subr.l  w10, [w2++], w10
    sub.l   w9, w13, w9
    sub.l   w10, w14, w10
    sub.l   w9, w13, w9
    sub.l   w10, w14, w10
    sacr.l  a, w13
    sacr.l  b, w14
    lac.l   w13, a
    lac.l   w14, b
    mac.l   w6, w9, a
    mac.l   w6, w10, b
    sacr.l  a, [w3++]
    sacr.l  b, [w3++]
    dtb     w4, _q31_q4_group

    ; trailing pair (taps 4n, 4n+1)
    mov.l   [w0++], w7
    mov.l   [w0++], w8
    subr.l  w7, [w1++], w11
    subr.l  w8, [w1++], w12
    lac.l   w7, a
    lac.l   w8, b
    mac.l   w5, w11, a
    mac.l   w5, w12, b
    subr.l  w7, [w2++], w7
    subr.l  w8, [w2++], w8
    sub.l   w7, w11, w7
    sub.l   w8, w12, w8
    sub.l   w7, w11, w7
    sub.l   w8, w12, w8
    sacr.l  a, w11
    sacr.l  b, w12
    lac.l   w11, a
    lac.l   w12, b
    mac.l   w6, w7, a
    mac.l   w6, w8, b
    sacr.l  a, [w3++]
    sacr.l  b, [w3++]

    pop.l   CORCON
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return
    .size   _mchp_asrc_q31_blend_row_quad4, .-_mchp_asrc_q31_blend_row_quad4

;-----------------------------------------------------------------------------
;   _mchp_asrc_q31_blend_row_lin4(c0, c1, ceff, taps, wbq)   w0..w4
;   The shipped linear blend (_mchp_asrc_q31_blend_row) read four taps per row at a time; the
;   same lac/mac/sacr per tap, so bit-exact to it.  RESEARCH probe of what the grouped read
;   would save on the PRODUCT kernel.  taps must be 4n + 2.
;-----------------------------------------------------------------------------
    .section .dspic33cmsisdsp_lin_blend_row4, code
    .global    _mchp_asrc_q31_blend_row_lin4
    .type      _mchp_asrc_q31_blend_row_lin4, @function
_mchp_asrc_q31_blend_row_lin4:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  CORCON
    fractsetup w8
    asr.l   w3, #2, w3
_q31_l4_group:
    mov.l   [w0++], w5
    mov.l   [w0++], w6
    mov.l   [w0++], w7
    mov.l   [w0++], w8
    subr.l  w5, [w1++], w9
    subr.l  w6, [w1++], w10
    subr.l  w7, [w1++], w11
    subr.l  w8, [w1++], w12
    lac.l   w5, a
    lac.l   w6, b
    mac.l   w4, w9, a
    mac.l   w4, w10, b
    sacr.l  a, [w2++]
    sacr.l  b, [w2++]
    lac.l   w7, a
    lac.l   w8, b
    mac.l   w4, w11, a
    mac.l   w4, w12, b
    sacr.l  a, [w2++]
    sacr.l  b, [w2++]
    dtb     w3, _q31_l4_group
    mov.l   [w0++], w5
    mov.l   [w0++], w6
    subr.l  w5, [w1++], w9
    subr.l  w6, [w1++], w10
    lac.l   w5, a
    lac.l   w6, b
    mac.l   w4, w9, a
    mac.l   w4, w10, b
    sacr.l  a, [w2++]
    sacr.l  b, [w2++]
    pop.l   CORCON
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return
    .size   _mchp_asrc_q31_blend_row_lin4, .-_mchp_asrc_q31_blend_row_lin4

    .section .dspic33cmsisdsp, code

;-----------------------------------------------------------------------------
;   void mchp_asrc_q31_row16(const int32_t *hist0,   /* w0  &ch[0][wbase] */
;                           uint32_t stride_bytes,  /* w1  per-channel   */
;                           const int32_t *ceff,    /* w2  blended row   */
;                           uint32_t taps,          /* w3  >= 2          */
;                           int32_t *out,           /* w4  nch entries   */
;                           uint32_t nch);          /* w5  >= 1          */
;
;   out[c] = round_q31( sum(k=0..taps-1) ceff[k] * hist0[c*stride + k] )
;
;   One instruction per MAC.  Per channel the fixed cost is 6 instructions
;   (2 pointer reloads, mpy.l, repeat, sacr.l, add.l, dtb - the repeat itself
;   is not re-issued per tap), i.e. 36 cycles for a 30-tap dot: 1.2 cycles/MAC.
;-----------------------------------------------------------------------------
    .global    _mchp_asrc_q31_row16
    .type      _mchp_asrc_q31_row16, @function
_mchp_asrc_q31_row16:

    push.l  w8
    push.l  w9
    push.l  w10
    push.l  CORCON
    push.l  MODCON
    fractsetup w8                   ; fractional mode: sets sacr.l alignment
    mov.l   #0x00FF, w8             ; XWM=YWM=1111, no EN bit: modulo OFF both AGUs
    mov.l   w8, MODCON

    sub.l   w3, #2, w8              ; w8 = taps-2, the REPEAT count
    mov.l   w0, w9                  ; w9 = this channel's window start

_q31_row16_ch:
    mov.l   w9, w0                  ; restart the sample window
    mov.l   w2, w10                 ; restart the coefficient row
    add.l   w9, w1, w9              ; next channel -- hoisted here on purpose: it
                                    ; fills one slot of the AGU-pointer-to-AGU-read
                                    ; hazard that otherwise costs a second neop
                                    ; (verified in the disassembly: 8 -> 6 fixed
                                    ; instructions per channel, no neop left at all,
                                    ; so 6 + 30 MACs = 1.2 cycles/MAC).
    mpy.l   [w0]+=4, [w10]+=4, a    ; a  = x[0] * ceff[0]
    repeat  w8
    mac.l   [w0]+=4, [w10]+=4, a    ; a += x[k] * ceff[k]   (taps-1 times)
    sacr.l  a, [w4++]               ; round + saturate to Q31
    dtb     w5, _q31_row16_ch

    pop.l   MODCON
    pop.l   CORCON
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return
    .size   _mchp_asrc_q31_row16, .-_mchp_asrc_q31_row16

;=============================================================================
; OPTIMISED VARIANTS -- bit-exact, same coefficients, same generated samples.
;
; These exist so ONE image can run `baseline` and `optimized` back to back and
; be differenced (audio_app_asrc_q31_opt_set()).  Comparing two images cannot
; measure a few us: an untouched stage moves several us on a Y-placement change
; alone, which was measured in this tree on 2026-09-06.
;
; They are assembled UNCONDITIONALLY, unlike the half-band pre-stage, which is
; behind an assembler defsym.  The half-band gate exists to make a
; half-configured build fail to link, because there the ARITHMETIC differs and
; "which kernel ran" is the whole question.  Here both variants compute the
; same samples, so nothing depends on which one is present.  Each variant sits
; in its OWN code section (below) so --gc-sections drops it whole in a build
; that never calls it; the cost of always assembling them is therefore zero
; ROM outside a research image, and in exchange no defsym can be forgotten.
;
; THE TAP AND CHANNEL COUNTS ARE FIXED AT ASSEMBLY TIME.  Removing the two loop
; branches is the optimisation, so the trip counts cannot be arguments any
; more.  They live in the .equ symbols below, deliberately NOT in the exported
; function names: both counts are overridable by --defsym, so a name carrying a
; number goes stale the first time one is overridden, which is exactly the trap
; this file used to set (_blend_row_x30 assembling at 28 taps).
; asrc_poly_q31.inc pairs each .equ with a _Static_assert on ASRC_POLY_M /
; ASRC_CH, so a config that changes either one and forgets the matching -Define
; fails to build; see the .equ blocks below for why --defsym additionally needs a
; second, boot-time check.
;=============================================================================

; ASRC_Q31_OPT_M tracks ASRC_POLY_M, which is a build-time choice: the shipping
; geometry is 30 taps and the 96 kHz profile runs 28 (APP_ASRC_EXPERIMENTAL_M28)
; from the SAME source.  A -Wa,--defsym=ASRC_Q31_OPT_M=<n> overrides the default
; below, and the same drift argument as for ASRC_Q31_OPT_NCH applies: the
; _Static_assert in asrc_poly_q31.inc catches a wrong -Define, and a missing or
; wrong --defsym is caught by the boot selftest, which brackets ceff_opt with
; canaries.  The coefficient table must be regenerated for the new M as well --
; see the table selection at the top of asrc_poly_q31.inc.
    .ifndef ASRC_Q31_OPT_M
    .equ    ASRC_Q31_OPT_M,    30       ; == ASRC_POLY_M   (default: shipping)
    .endif

; ASRC_Q31_OPT_NCH tracks ASRC_CH, which is a build-time choice: the production
; 12-channel image and the 8-channel research image that measures the same
; kernels outside the starve regime need different trip counts from the SAME
; source.  A -Wa,--defsym=ASRC_Q31_OPT_NCH=<n> overrides the default below.
;
; A defsym is invisible to the C preprocessor, so the two sides could drift if
; only one of -Define / -AsDefine were passed.  The compile-time _Static_assert in
; asrc_poly_q31.inc catches a wrong -Define; a missing or wrong --defsym is caught
; instead by the boot selftest, which brackets its output buffer with canaries and
; so detects this kernel writing MORE channels than the C side configured, and
; detects it writing FEWER as an ordinary value mismatch.  Neither check alone is
; sufficient, and no data symbol needs exporting from here.
    .ifndef ASRC_Q31_OPT_NCH
    .equ    ASRC_Q31_OPT_NCH,  12       ; == ASRC_CH   (default: production)
    .endif

;-----------------------------------------------------------------------------
;   void mchp_asrc_q31_blend_row_opt(const int32_t *c0,   /* w0 */
;                                   const int32_t *c1,   /* w1 */
;                                   int32_t       *ceff, /* w2 */
;                                   uint32_t       taps, /* w3  IGNORED */
;                                   int32_t        wbq); /* w4 */
;
;   CANDIDATE 1.  8 -> 6 cycles per tap, ARITHMETIC BYTE-FOR-BYTE THE BASELINE'S.
;
;   THE BASELINE TAP COSTS 8 CYCLES, NOT 7.  The assembler inserts a neop after
;   the `sub.l w6,w5,w6`, so the baseline body is mov,mov,lac,sub,NEOP,mac,sacr,
;   dtb.  That is why simply unrolling the baseline body saves NOTHING worth
;   having: the dtb sits in a slot the neop would occupy anyway, so removing the
;   loop just swaps a dtb for a neop.  The saving has to come from the neop that
;   the ALU subtract forces, not from the branch.
;
;   DO NOT MOVE THE DELTA INTO THE ACCUMULATOR.  That was tried first --
;
;       A = c0[k] ;  A += wbq * c1[k] ;  A -= wbq * c0[k]     (lac/mac/msc.l)
;
;   -- on the argument that a difference of two fractional MACs into a guarded
;   accumulator must equal one MAC of the difference, with sacr.l as the only
;   rounding point.  IT IS NOT BIT-EXACT ON THIS SILICON.  Measured 2026-09-07
;   on 33AK512MPS512 (image sonora_ak512mps512_asrc_96k_a_to_b_202609070151):
;   354 of 882 compared coefficients differed, always LOW, by 1..2 LSB, e.g.
;   got 1555176252 want 1555176254.  So a fractional mac.l does NOT contribute
;   the full 64-bit product to the accumulator: split the product and each half
;   loses its sub-LSB part separately.  The single MAC of the ALU difference is
;   what the hardware-verified model above describes, and it is the only form
;   that reproduces it.  (Mechanism unconfirmed; the measurement is not.)
;
;   So the saving comes from SCHEDULING, with the arithmetic left alone: the neop
;   after a sub.l is a one-slot latency, not a fixed cost of the instruction, and
;   the next tap's own sub.l is allowed to fill it.  Taps are therefore processed
;   in PAIRS -- both operand pairs loaded, both subtracts back to back, then the
;   two lac/mac/sacr triplets -- which is 12 instructions and 12 cycles for two
;   taps with no neop anywhere in the body (verified in the disassembly).
;
;   M must be even for this to tile; the C side static-asserts
;   (ASRC_POLY_M % 2) == 0 and would fail the build rather than drop a tap.
;
;   taps stays in the signature only so the two variants are call-compatible at
;   the C call site; it is not read.  ASRC_Q31_OPT_M is what runs.
;-----------------------------------------------------------------------------
; Own section: the two shipping functions above share .dspic33cmsisdsp because
; they are always called.  --gc-sections collects on SECTIONS, not on symbols,
; so leaving the research variants in that shared section would keep them (and
; their ~1 KB) in every image regardless of APP_ASRC_Q31_OPT_KERNELS.  AK128 is
; ROM-tight; all four MPLAB configurations set isolate-each-function and
; remove-unused-sections, so a section of its own is what makes this droppable.
    .section .dspic33cmsisdsp_opt_blend_row, code
    .global    _mchp_asrc_q31_blend_row_opt
    .type      _mchp_asrc_q31_blend_row_opt, @function
_mchp_asrc_q31_blend_row_opt:

    push.l  w8
    push.l  CORCON
    fractsetup w8                   ; fractional mode: sets sacr.l alignment

    .rept   ASRC_Q31_OPT_M/2
    mov.l   [w0++], w5              ; c0[k]
    mov.l   [w1++], w6              ; c1[k]
    mov.l   [w0++], w7              ; c0[k+1]
    mov.l   [w1++], w8              ; c1[k+1]
    sub.l   w6, w5, w6              ; delta k     (its latency slot is the next sub)
    sub.l   w8, w7, w8              ; delta k+1
    lac.l   w5, a                   ; a  = c0[k]
    mac.l   w4, w6, a               ; a += wbq * delta k
    sacr.l  a, [w2++]               ; round + saturate to Q31 -- the ONLY rounding
    lac.l   w7, a                   ; a  = c0[k+1]
    mac.l   w4, w8, a               ; a += wbq * delta k+1
    sacr.l  a, [w2++]
    .endr

    pop.l   CORCON
    pop.l   w8
    return
    .size   _mchp_asrc_q31_blend_row_opt, .-_mchp_asrc_q31_blend_row_opt

;-----------------------------------------------------------------------------
;   void mchp_asrc_q31_row16_opt(const int32_t *hist0,   /* w0 */
;                               uint32_t stride_bytes,  /* w1 */
;                               const int32_t *ceff,    /* w2 */
;                               uint32_t taps,          /* w3  IGNORED */
;                               int32_t *out,           /* w4 */
;                               uint32_t nch);          /* w5  IGNORED */
;
;   out[c] = round_q31( sum ceff[k]*hist0[c*stride+k] ) & 0xFFFFFF00
;
;   CANDIDATES 2 AND 3 TOGETHER, because they share the store.
;
;   3: the per-channel fixed cost drops from 6 instructions to 4 of its own.
;      The channel loop is unrolled, which removes the dtb, and the two pointer
;      RELOADS become two pointer ADJUSTMENTS: after a dot both pointers have
;      advanced by M*4, so the next window is one add away (stride - M*4) and
;      the row start is one sub away (M*4).  That also frees w9/w10 entirely --
;      w1/w5 hold the two deltas and w2 walks the row itself -- so the prologue
;      pushes one register instead of three.
;
;   2: the s24-left mask that asrc_q31_to_slot() applied in a C loop over all
;      channels is folded into the store, costing one instruction per channel
;      here and removing that loop.  Order matters and is preserved: sacr.l
;      rounds and saturates first, the mask clears the low 8 bits second.
;
;   INSTRUCTION ORDER IS SCHEDULED, NOT INCIDENTAL.  The two pointer adjustments
;   sit between the last mac.l and the sacr.l, so each AGU register is written
;   at least two instructions before the mpy.l that prefetches through it.  That
;   is the same hazard the baseline fills with its hoisted add.l (see the
;   comment there); getting it wrong costs a neop per channel and would show up
;   as the optimisation "not working".
;
;   The final channel's two adjustments are dead -- w0 is left pointing one
;   stride past the last window.  That address is never dereferenced, and
;   keeping the body uniform is worth two instructions per CALL.
;-----------------------------------------------------------------------------
; Own section, for the reason given above _mchp_asrc_q31_blend_row_opt.
    .section .dspic33cmsisdsp_opt_row16, code
    .global    _mchp_asrc_q31_row16_opt
    .type      _mchp_asrc_q31_row16_opt, @function
_mchp_asrc_q31_row16_opt:

    push.l  w8
    push.l  CORCON
    push.l  MODCON
    fractsetup w8                   ; fractional mode: sets sacr.l alignment
    mov.l   #0x00FF, w8             ; XWM=YWM=1111, no EN bit: modulo OFF both AGUs
    mov.l   w8, MODCON

    mov.l   #(ASRC_Q31_OPT_M-2), w8         ; the REPEAT count
    mov.l   #0xFFFFFF00, w3                 ; s24-left slot mask (w3 held taps)
    mov.l   #(ASRC_Q31_OPT_M*4), w5         ; row rewind        (w5 held nch)
    sub.l   w1, w5, w1                      ; w1 = stride - M*4 = next window

    .rept   ASRC_Q31_OPT_NCH
    mpy.l   [w0]+=4, [w2]+=4, a     ; a  = x[0] * ceff[0]
    repeat  w8
    mac.l   [w0]+=4, [w2]+=4, a     ; a += x[k] * ceff[k]   (M-1 times)
    add.l   w0, w1, w0              ; next channel's window start
    sub.l   w2, w5, w2              ; back to ceff[0]
    sacr.l  a, w6                   ; round + saturate to Q31
    and.l   w6, w3, [w4++]          ; drop the 8 sub-LSBs the TDM slot cannot carry
    .endr

    pop.l   MODCON
    pop.l   CORCON
    pop.l   w8
    return
    .size   _mchp_asrc_q31_row16_opt, .-_mchp_asrc_q31_row16_opt

    .end
