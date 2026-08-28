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

    .end
