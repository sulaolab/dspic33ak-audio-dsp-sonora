;*****************************************************************************
; Fixed-geometry ASRC producer history writers.
;
; Three entry points consume one 16-frame physical TDM8 block and write the
; CH-major ASRC history used by the headroom M=30 build:
;   FIFO frames       = 128
;   mirror frames     = 30
;   channel stride    = (128 + 30) * 4 = 632 bytes
;   source stride     = 8 * 4 = 32 bytes
;
; void mchp_asrc_push16_stereo30_f32(float *history0,
;                                    const int32_t *src, uint32_t startIdx);
;   slot 0/1 are replicated L/R over history channels 0..15.
;
; void mchp_asrc_push16_stereo30_aligned_f32(float *history0,
;                                            const int32_t *src, uint32_t startIdx);
;   M=30 fast path for 16-frame-aligned starts; non-aligned starts tail-branch
;   to mchp_asrc_push16_stereo30_f32.
;
; void mchp_asrc_push8_tdm30_f32(float *history0,
;                                const int32_t *src, uint32_t startIdx);
;   slots 0..7 map one-to-one to history channels 0..7.  This is the reusable
;   producer primitive for a future physical 8-channel input path.
;
; Input slots are left-justified signed 24-bit.  ASR #8 followed by LI2F.S is
; bit-identical to the established C expression (float)(slot >> 8).
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

    .section .dspic33cmsisdsp, code

    .equ ASRC_PUSH_FRAMES,          16
    .equ ASRC_FIFO_FRAMES,          128
    .equ ASRC_MIRROR_FRAMES,        30
    .equ ASRC_FIFO_BYTES,           512
    .equ ASRC_HISTORY_STRIDE,       632
    .equ ASRC_GROUP8_BYTES,         5056
    .equ ASRC_GROUP8_MIRROR_BYTES,  5568
    .equ ASRC28_HISTORY_STRIDE,     624
    .equ ASRC28_GROUP8_BYTES,       4992

; M=30 hot entry for the normal producer invariant: startIdx is 16-frame
; aligned.  Keep the generic entry below intact for variable-frame producers,
; tests and any future caller that breaks that invariant.  The guard is before
; the prologue so fallback is a tail branch with the three arguments unchanged.
;
; Aligned blocks never wrap within the 128-frame main ring.  Only start 0 has
; sixteen mirrored frames, start 16 has fourteen, and starts 32..112 have none.
; Splitting those cases removes the per-frame mirror/ring comparisons and the
; final pointer rewind while preserving every conversion and store operation.
    .global _mchp_asrc_push16_stereo30_aligned_f32
_mchp_asrc_push16_stereo30_aligned_f32:
    and.l   w2, #(ASRC_PUSH_FRAMES-1), w3
    bra     z, _asrc_push16_stereo30_aligned_fast
    bra     _mchp_asrc_push16_stereo30_f32

_asrc_push16_stereo30_aligned_fast:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14

    ; Set w0/w8..w14 to channel 0..7 at startIdx.
    sl.l    w2, #2, w4
    add.l   w0, w4, w0
    movs.l  #ASRC_HISTORY_STRIDE, w4
    add.l   w0, w4, w8
    add.l   w8, w4, w9
    add.l   w9, w4, w10
    add.l   w10, w4, w11
    add.l   w11, w4, w12
    add.l   w12, w4, w13
    add.l   w13, w4, w14
    movs.l  #ASRC_GROUP8_BYTES, w6
    movs.l  #ASRC_FIFO_BYTES, w7

    cp0.l   w2
    bra     z, _asrc_push16_stereo30_aligned_start0
    cp.l    w2, #ASRC_PUSH_FRAMES
    bra     z, _asrc_push16_stereo30_aligned_start16

    ; Every other aligned start (32..112): main ring only.
    movs.l  #ASRC_PUSH_FRAMES, w3
    bra     _asrc_push16_stereo30_aligned_main_loop

_asrc_push16_stereo30_aligned_start0:
    movs.l  #ASRC_PUSH_FRAMES, w3            ; 16 mirror + main frames
    bra     _asrc_push16_stereo30_aligned_mirror_loop

_asrc_push16_stereo30_aligned_start16:
    movs.l  #(ASRC_MIRROR_FRAMES-ASRC_PUSH_FRAMES), w3 ; 14 mirror + main
    movs.l  #2, w2                           ; two final main-only frames

_asrc_push16_stereo30_aligned_mirror_loop:
    mov.l   [w1++], w4
    mov.l   [w1++], w5
    asr.l   w4, #8, w4
    asr.l   w5, #8, w5
    mov.l   w4, f0
    mov.l   w5, f1
    li2f.s  f0, f0
    li2f.s  f1, f1

    ; Main-ring channels 0..15, in the same order as the generic entry.
    mov.l   f0, [w0]
    add.l   w0, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w8]
    add.l   w8, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w9]
    add.l   w9, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w10]
    add.l   w10, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w11]
    add.l   w11, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w12]
    add.l   w12, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w13]
    add.l   w13, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w14]
    add.l   w14, w6, w4
    mov.l   f1, [w4]

    ; The dispatch guarantees that every iteration here is in mirror 0..29.
    add.l   w0, w7, w4
    mov.l   f0, [w4]
    add.l   w0, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w8, w7, w4
    mov.l   f1, [w4]
    add.l   w8, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w9, w7, w4
    mov.l   f0, [w4]
    add.l   w9, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w10, w7, w4
    mov.l   f1, [w4]
    add.l   w10, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w11, w7, w4
    mov.l   f0, [w4]
    add.l   w11, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w12, w7, w4
    mov.l   f1, [w4]
    add.l   w12, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w13, w7, w4
    mov.l   f0, [w4]
    add.l   w13, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w14, w7, w4
    mov.l   f1, [w4]
    add.l   w14, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]

    add.l   #24, w1
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    DTB     w3, _asrc_push16_stereo30_aligned_mirror_loop

    ; start 0 leaves w2==0; start 16 staged its two main-only frames in w2.
    mov.l   w2, w3
    cp0.l   w3
    bra     z, _asrc_push16_stereo30_aligned_epilogue

_asrc_push16_stereo30_aligned_main_loop:
    mov.l   [w1++], w4
    mov.l   [w1++], w5
    asr.l   w4, #8, w4
    asr.l   w5, #8, w5
    mov.l   w4, f0
    mov.l   w5, f1
    li2f.s  f0, f0
    li2f.s  f1, f1

    mov.l   f0, [w0]
    add.l   w0, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w8]
    add.l   w8, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w9]
    add.l   w9, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w10]
    add.l   w10, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w11]
    add.l   w11, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w12]
    add.l   w12, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w13]
    add.l   w13, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w14]
    add.l   w14, w6, w4
    mov.l   f1, [w4]

    add.l   #24, w1
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    DTB     w3, _asrc_push16_stereo30_aligned_main_loop

_asrc_push16_stereo30_aligned_epilogue:
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .global _mchp_asrc_push16_stereo30_f32
_mchp_asrc_push16_stereo30_f32:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14

    ; Set w0/w8..w14 to channel 0..7 at startIdx.
    sl.l    w2, #2, w4
    add.l   w0, w4, w0
    movs.l  #ASRC_HISTORY_STRIDE, w4
    add.l   w0, w4, w8
    add.l   w8, w4, w9
    add.l   w9, w4, w10
    add.l   w10, w4, w11
    add.l   w11, w4, w12
    add.l   w12, w4, w13
    add.l   w13, w4, w14
    movs.l  #ASRC_PUSH_FRAMES, w3
    movs.l  #ASRC_GROUP8_BYTES, w6
    movs.l  #ASRC_FIFO_BYTES, w7

_asrc_push16_stereo_loop:
    mov.l   [w1++], w4
    mov.l   [w1++], w5
    asr.l   w4, #8, w4
    asr.l   w5, #8, w5
    mov.l   w4, f0
    mov.l   w5, f1
    li2f.s  f0, f0
    li2f.s  f1, f1

    ; Channels 0..7 plus their corresponding channels 8..15.  The latter are
    ; reached by one fixed add, avoiding a second conversion pass.
    mov.l   f0, [w0]
    add.l   w0, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w8]
    add.l   w8, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w9]
    add.l   w9, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w10]
    add.l   w10, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w11]
    add.l   w11, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w12]
    add.l   w12, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w13]
    add.l   w13, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w14]
    add.l   w14, w6, w4
    mov.l   f1, [w4]

    cp.l    w2, #(ASRC_MIRROR_FRAMES-1)
    bra     gtu, _asrc_push16_stereo_no_mirror

    add.l   w0, w7, w4
    mov.l   f0, [w4]
    add.l   w0, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w8, w7, w4
    mov.l   f1, [w4]
    add.l   w8, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w9, w7, w4
    mov.l   f0, [w4]
    add.l   w9, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w10, w7, w4
    mov.l   f1, [w4]
    add.l   w10, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w11, w7, w4
    mov.l   f0, [w4]
    add.l   w11, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w12, w7, w4
    mov.l   f1, [w4]
    add.l   w12, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w13, w7, w4
    mov.l   f0, [w4]
    add.l   w13, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w14, w7, w4
    mov.l   f1, [w4]
    add.l   w14, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]

_asrc_push16_stereo_no_mirror:
    add.l   #24, w1
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    add.l   w2, #1, w2
    cp.l    w2, #ASRC_FIFO_FRAMES
    bra     nz, _asrc_push16_stereo_advanced
    clr.l   w2
    sub.l   w0, w7, w0
    sub.l   w8, w7, w8
    sub.l   w9, w7, w9
    sub.l   w10, w7, w10
    sub.l   w11, w7, w11
    sub.l   w12, w7, w12
    sub.l   w13, w7, w13
    sub.l   w14, w7, w14
_asrc_push16_stereo_advanced:
    DTB     w3, _asrc_push16_stereo_loop

    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .global _mchp_asrc_push8_tdm30_f32
_mchp_asrc_push8_tdm30_f32:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14

    ; Set w0/w8..w14 to channel 0..7 at startIdx.
    sl.l    w2, #2, w4
    add.l   w0, w4, w0
    movs.l  #ASRC_HISTORY_STRIDE, w4
    add.l   w0, w4, w8
    add.l   w8, w4, w9
    add.l   w9, w4, w10
    add.l   w10, w4, w11
    add.l   w11, w4, w12
    add.l   w12, w4, w13
    add.l   w13, w4, w14
    movs.l  #ASRC_PUSH_FRAMES, w3
    movs.l  #ASRC_FIFO_BYTES, w7

_asrc_push8_tdm_loop:
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f0
    li2f.s  f0, f0
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f1
    li2f.s  f1, f1
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f2
    li2f.s  f2, f2
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f3
    li2f.s  f3, f3
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f4
    li2f.s  f4, f4
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f5
    li2f.s  f5, f5
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f6
    li2f.s  f6, f6
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f7
    li2f.s  f7, f7

    mov.l   f0, [w0]
    mov.l   f1, [w8]
    mov.l   f2, [w9]
    mov.l   f3, [w10]
    mov.l   f4, [w11]
    mov.l   f5, [w12]
    mov.l   f6, [w13]
    mov.l   f7, [w14]

    cp.l    w2, #(ASRC_MIRROR_FRAMES-1)
    bra     gtu, _asrc_push8_tdm_no_mirror
    add.l   w0, w7, w4
    mov.l   f0, [w4]
    add.l   w8, w7, w4
    mov.l   f1, [w4]
    add.l   w9, w7, w4
    mov.l   f2, [w4]
    add.l   w10, w7, w4
    mov.l   f3, [w4]
    add.l   w11, w7, w4
    mov.l   f4, [w4]
    add.l   w12, w7, w4
    mov.l   f5, [w4]
    add.l   w13, w7, w4
    mov.l   f6, [w4]
    add.l   w14, w7, w4
    mov.l   f7, [w4]

_asrc_push8_tdm_no_mirror:
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    add.l   w2, #1, w2
    cp.l    w2, #ASRC_FIFO_FRAMES
    bra     nz, _asrc_push8_tdm_advanced
    clr.l   w2
    sub.l   w0, w7, w0
    sub.l   w8, w7, w8
    sub.l   w9, w7, w9
    sub.l   w10, w7, w10
    sub.l   w11, w7, w11
    sub.l   w12, w7, w12
    sub.l   w13, w7, w13
    sub.l   w14, w7, w14
_asrc_push8_tdm_advanced:
    DTB     w3, _asrc_push8_tdm_loop

    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

; M=28 variants live in a separate input section so --gc-sections removes them
; from the formal M30 image.  The experimental M28 build references these
; symbols and therefore retains the section automatically.
    .section .dspic33cmsisdsp_m28, code

; FIFO depth and source geometry stay unchanged; only the CH-major row stride
; and mirror threshold differ from M=30.
    .global _mchp_asrc_push16_stereo28_f32
_mchp_asrc_push16_stereo28_f32:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14

    sl.l    w2, #2, w4
    add.l   w0, w4, w0
    movs.l  #ASRC28_HISTORY_STRIDE, w4
    add.l   w0, w4, w8
    add.l   w8, w4, w9
    add.l   w9, w4, w10
    add.l   w10, w4, w11
    add.l   w11, w4, w12
    add.l   w12, w4, w13
    add.l   w13, w4, w14
    movs.l  #ASRC_PUSH_FRAMES, w3
    movs.l  #ASRC28_GROUP8_BYTES, w6
    movs.l  #ASRC_FIFO_BYTES, w7

_asrc_push16_stereo28_loop:
    mov.l   [w1++], w4
    mov.l   [w1++], w5
    asr.l   w4, #8, w4
    asr.l   w5, #8, w5
    mov.l   w4, f0
    mov.l   w5, f1
    li2f.s  f0, f0
    li2f.s  f1, f1

    mov.l   f0, [w0]
    add.l   w0, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w8]
    add.l   w8, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w9]
    add.l   w9, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w10]
    add.l   w10, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w11]
    add.l   w11, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w12]
    add.l   w12, w6, w4
    mov.l   f1, [w4]
    mov.l   f0, [w13]
    add.l   w13, w6, w4
    mov.l   f0, [w4]
    mov.l   f1, [w14]
    add.l   w14, w6, w4
    mov.l   f1, [w4]

    cp.l    w2, #27
    bra     gtu, _asrc_push16_stereo28_no_mirror

    add.l   w0, w7, w4
    mov.l   f0, [w4]
    add.l   w0, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w8, w7, w4
    mov.l   f1, [w4]
    add.l   w8, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w9, w7, w4
    mov.l   f0, [w4]
    add.l   w9, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w10, w7, w4
    mov.l   f1, [w4]
    add.l   w10, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w11, w7, w4
    mov.l   f0, [w4]
    add.l   w11, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w12, w7, w4
    mov.l   f1, [w4]
    add.l   w12, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]
    add.l   w13, w7, w4
    mov.l   f0, [w4]
    add.l   w13, w6, w4
    add.l   w4, w7, w4
    mov.l   f0, [w4]
    add.l   w14, w7, w4
    mov.l   f1, [w4]
    add.l   w14, w6, w4
    add.l   w4, w7, w4
    mov.l   f1, [w4]

_asrc_push16_stereo28_no_mirror:
    add.l   #24, w1
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    add.l   w2, #1, w2
    cp.l    w2, #ASRC_FIFO_FRAMES
    bra     nz, _asrc_push16_stereo28_advanced
    clr.l   w2
    sub.l   w0, w7, w0
    sub.l   w8, w7, w8
    sub.l   w9, w7, w9
    sub.l   w10, w7, w10
    sub.l   w11, w7, w11
    sub.l   w12, w7, w12
    sub.l   w13, w7, w13
    sub.l   w14, w7, w14
_asrc_push16_stereo28_advanced:
    DTB     w3, _asrc_push16_stereo28_loop

    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .global _mchp_asrc_push8_tdm28_f32
_mchp_asrc_push8_tdm28_f32:
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14

    sl.l    w2, #2, w4
    add.l   w0, w4, w0
    movs.l  #ASRC28_HISTORY_STRIDE, w4
    add.l   w0, w4, w8
    add.l   w8, w4, w9
    add.l   w9, w4, w10
    add.l   w10, w4, w11
    add.l   w11, w4, w12
    add.l   w12, w4, w13
    add.l   w13, w4, w14
    movs.l  #ASRC_PUSH_FRAMES, w3
    movs.l  #ASRC_FIFO_BYTES, w7

_asrc_push8_tdm28_loop:
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f0
    li2f.s  f0, f0
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f1
    li2f.s  f1, f1
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f2
    li2f.s  f2, f2
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f3
    li2f.s  f3, f3
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f4
    li2f.s  f4, f4
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f5
    li2f.s  f5, f5
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f6
    li2f.s  f6, f6
    mov.l   [w1++], w4
    asr.l   w4, #8, w4
    mov.l   w4, f7
    li2f.s  f7, f7

    mov.l   f0, [w0]
    mov.l   f1, [w8]
    mov.l   f2, [w9]
    mov.l   f3, [w10]
    mov.l   f4, [w11]
    mov.l   f5, [w12]
    mov.l   f6, [w13]
    mov.l   f7, [w14]

    cp.l    w2, #27
    bra     gtu, _asrc_push8_tdm28_no_mirror
    add.l   w0, w7, w4
    mov.l   f0, [w4]
    add.l   w8, w7, w4
    mov.l   f1, [w4]
    add.l   w9, w7, w4
    mov.l   f2, [w4]
    add.l   w10, w7, w4
    mov.l   f3, [w4]
    add.l   w11, w7, w4
    mov.l   f4, [w4]
    add.l   w12, w7, w4
    mov.l   f5, [w4]
    add.l   w13, w7, w4
    mov.l   f6, [w4]
    add.l   w14, w7, w4
    mov.l   f7, [w4]

_asrc_push8_tdm28_no_mirror:
    add.l   w0, #4, w0
    add.l   w8, #4, w8
    add.l   w9, #4, w9
    add.l   w10, #4, w10
    add.l   w11, #4, w11
    add.l   w12, #4, w12
    add.l   w13, #4, w13
    add.l   w14, #4, w14
    add.l   w2, #1, w2
    cp.l    w2, #ASRC_FIFO_FRAMES
    bra     nz, _asrc_push8_tdm28_advanced
    clr.l   w2
    sub.l   w0, w7, w0
    sub.l   w8, w7, w8
    sub.l   w9, w7, w9
    sub.l   w10, w7, w10
    sub.l   w11, w7, w11
    sub.l   w12, w7, w12
    sub.l   w13, w7, w13
    sub.l   w14, w7, w14
_asrc_push8_tdm28_advanced:
    DTB     w3, _asrc_push8_tdm28_loop

    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    return

    .end
