;*****************************************************************************
; mchp_f32_to_slot8_pair -- batch float to left-justified signed-24 conversion
;
; void mchp_f32_to_slot8_pair(const float *src16, int32_t *dst0, int32_t *dst1);
;
; src16[0..7]  -> dst0[0..7]
; src16[8..15] -> dst1[0..7]
;
; The scalar C fast path reloads both clamp constants and emits two branches for
; every sample. This helper keeps the constants in F registers across all 16
; conversions and amortizes call/loop setup. Truncation matches (int32_t)y.
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .global _mchp_f32_to_slot8_pair
_mchp_f32_to_slot8_pair:
    add.l   w0, #32, w6              ; src1 = &src16[8]
    mov.l   #0xcb000000, f2          ; -8388608.0f
    mov.l   #0x4afffffe, f3          ; +8388607.0f (compiler's exact constant)
    movs.l  #8, w3

v_f32_to_slot8_pair_loop:
    mov.l   [w0++], f0
    mov.l   [w6++], f1

    cpq.s   f0, f2
    fbra    ge, _slot8_y0_min_ok
    mov.s   f2, f0
_slot8_y0_min_ok:
    cpq.s   f0, f3
    fbra    le, _slot8_y0_max_ok
    mov.s   f3, f0
_slot8_y0_max_ok:

    cpq.s   f1, f2
    fbra    ge, _slot8_y1_min_ok
    mov.s   f2, f1
_slot8_y1_min_ok:
    cpq.s   f1, f3
    fbra    le, _slot8_y1_max_ok
    mov.s   f3, f1
_slot8_y1_max_ok:

    f2li.sz f0, f4
    f2li.sz f1, f5
    mov.l   f4, w4
    mov.l   f5, w5
    sl.l    w4, #8, w4
    sl.l    w5, #8, w5
    mov.l   w4, [w1++]
    mov.l   w5, [w2++]
    DTB     w3, v_f32_to_slot8_pair_loop
    return

    .end
