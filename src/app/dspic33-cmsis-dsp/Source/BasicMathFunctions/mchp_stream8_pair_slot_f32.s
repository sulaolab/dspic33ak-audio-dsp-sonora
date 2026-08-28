;*****************************************************************************
; mchp_stream8_pair_slot_f32 -- fused two-output STREAM8 + signed-24 packing.
;
; Arithmetic through the 16 float accumulators is identical to
; mchp_stream8_pair_f32. Instead of storing float results for a second pass,
; the tail clamps, truncates, left-justifies, and writes 16 int32 slots:
; out16[0..7]=frame0 and out16[8..15]=frame1.
;
; void mchp_stream8_pair_slot_f32(const float *wbase0, uint32_t strideBytes,
;                                 const float *c00, const float *c01,
;                                 uint32_t blockSize, int32_t *out16,
;                                 const float *c10, const float *c11,
;                                 float wb0, float wb1);
;*****************************************************************************

    .nolist
    .include    "dspcommon.inc"
    .list

; Stack frame used by the hoisted-blend ("ce") path below: two rows of
; ASRC_CE_MAX_TAPS pre-blended float coefficients, addressed backwards from w15
; (the stack grows upwards, so the frame sits entirely below the current SP and
; is untouched by anything an interrupt may push).
    .equ    ASRC_CE_MAX_TAPS, 32
    .equ    ASRC_CE_FRAME,    256           ; 2 * 32 * 4
    .equ    ASRC_CE0_BACK,    256           ; &ce0[0] = w15 - 256
    .equ    ASRC_CE1_BACK,    128           ; &ce1[0] = w15 - 128

    .section .dspic33cmsisdsp, code

    .global _mchp_stream8_pair_slot_f32
_mchp_stream8_pair_slot_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    mov.s   f0, f20
    mov.s   f1, f21

    rcall   _stream8pair_slot_body
    bra     _stream8pair_slot_epilogue

; Internal leaf body. The caller supplies w0..w7/f20/f21 exactly as documented for
; mchp_stream8_pair_slot_f32. It intentionally preserves none of those working
; registers, allowing the 16-channel wrapper below to share one save/restore frame.
_stream8pair_slot_body:
    add.l   w0, w1, w8
    add.l   w8, w1, w9
    add.l   w9, w1, w10
    add.l   w10, w1, w11
    add.l   w11, w1, w12
    add.l   w12, w1, w13
    add.l   w13, w1, w14
    floatsetup w1

    movc.s  #22, f0
    movc.s  #22, f1
    movc.s  #22, f2
    movc.s  #22, f3
    movc.s  #22, f4
    movc.s  #22, f5
    movc.s  #22, f6
    movc.s  #22, f7
    movc.s  #22, f8
    movc.s  #22, f9
    movc.s  #22, f10
    movc.s  #22, f11
    movc.s  #22, f12
    movc.s  #22, f13
    movc.s  #22, f14
    movc.s  #22, f15

    ; Endpoint base+0: output 0 only.
    mov.l   [w2++], f18
    mov.l   [w3++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f16
    mac.s   f19, f20, f16

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f19, f16, f7

    sub.l   w4, #1, w4
    cp0.l   w4
    bra     z, _stream8pair_slot_tail

v_stream8pair_slot_loop:
    mov.l   [w2++], f18
    mov.l   [w3++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f16
    mac.s   f19, f20, f16
    mov.l   [w6++], f18
    mov.l   [w7++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f17
    mac.s   f19, f21, f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15
    DTB     w4, v_stream8pair_slot_loop

_stream8pair_slot_tail:
    ; Endpoint base+M: output 1 only.
    mov.l   [w6++], f18
    mov.l   [w7++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f17
    mac.s   f19, f21, f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f17, f14
    mac.s   f19, f17, f15

; Shared clamp/convert/pack of the 16 accumulators into out16.  Entered by
; fall-through from _stream8pair_slot_tail and by branch from the hoisted-blend
; tail below.
;
; Clamp constants remain live for all 16 conversions.  min.s/max.s give
; the same result as the former compare-and-branch clamp for every
; non-NaN accumulator, with no conditional branch in the tail at all.
; Lanes are converted in pairs so the f2li result is read one
; instruction later than it is produced (avoids an FPU read stall).
; Only w0 and w4 are used as scratch: w4's tap counter has reached 0, and w0
; is rebuilt by every wrapper.  w1 is deliberately left alone so that the
; hoisted-blend body can keep the channel stride live across a whole pair.
_stream8pair_slot_pack:
    mov.l   #0xcb000000, f16
    mov.l   #0x4afffffe, f17

    max.s   f0, f16, f0
    max.s   f1, f16, f1
    min.s   f0, f17, f0
    min.s   f1, f17, f1
    f2li.sz f0, f18
    f2li.sz f1, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5]
    mov.l   w4, [w5+4]

    max.s   f2, f16, f2
    max.s   f3, f16, f3
    min.s   f2, f17, f2
    min.s   f3, f17, f3
    f2li.sz f2, f18
    f2li.sz f3, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+8]
    mov.l   w4, [w5+12]

    max.s   f4, f16, f4
    max.s   f5, f16, f5
    min.s   f4, f17, f4
    min.s   f5, f17, f5
    f2li.sz f4, f18
    f2li.sz f5, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+16]
    mov.l   w4, [w5+20]

    max.s   f6, f16, f6
    max.s   f7, f16, f7
    min.s   f6, f17, f6
    min.s   f7, f17, f7
    f2li.sz f6, f18
    f2li.sz f7, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+24]
    mov.l   w4, [w5+28]

    max.s   f8, f16, f8
    max.s   f9, f16, f9
    min.s   f8, f17, f8
    min.s   f9, f17, f9
    f2li.sz f8, f18
    f2li.sz f9, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+32]
    mov.l   w4, [w5+36]

    max.s   f10, f16, f10
    max.s   f11, f16, f11
    min.s   f10, f17, f10
    min.s   f11, f17, f11
    f2li.sz f10, f18
    f2li.sz f11, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+40]
    mov.l   w4, [w5+44]

    max.s   f12, f16, f12
    max.s   f13, f16, f13
    min.s   f12, f17, f12
    min.s   f13, f17, f13
    f2li.sz f12, f18
    f2li.sz f13, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+48]
    mov.l   w4, [w5+52]

    max.s   f14, f16, f14
    max.s   f15, f16, f15
    min.s   f14, f17, f14
    min.s   f15, f17, f15
    f2li.sz f14, f18
    f2li.sz f15, f19
    mov.l   f18, w0
    mov.l   f19, w4
    sl.l    w0, #8, w0
    sl.l    w4, #8, w4
    mov.l   w0, [w5+56]
    mov.l   w4, [w5+60]

    return

; Release the blended-coefficient frame, then fall into the shared restore.
_stream8pair_slot_epilogue_ce:
    movs.l  #ASRC_CE_FRAME, w0
    sub.l   w15, w0, w15

_stream8pair_slot_epilogue:
    pop.l   fcr
    pop.l   w14
    pop.l   w13
    pop.l   w12
    pop.l   w11
    pop.l   w10
    pop.l   w9
    pop.l   w8
    pop.l   f21
    pop.l   f20
    pop.l   f19
    pop.l   f18
    pop.l   f17
    pop.l   f16
    pop.l   f15
    pop.l   f14
    pop.l   f13
    pop.l   f12
    pop.l   f11
    pop.l   f10
    pop.l   f9
    pop.l   f8
    return

;*****************************************************************************
; Hoisted phase blend (the "ce" path used by every fixed-M 16-channel wrapper).
;
; _stream8pair_slot_body evaluates the two blended coefficient rows *inside* the
; tap loop, so a 16-channel wrapper -- which calls the body twice with the same
; rows and the same weights -- pays for that blend twice.  Splitting it out costs
; one store plus one reload per coefficient and saves a full blend per tap:
;
;   per tap, 16 channels:  11 (prep) + 2 * 27 (body)  =  65 instructions
;                    was:               2 * 35        =  70 instructions
;
; The arithmetic is bit-identical -- the same sub.s/mac.s pair on the same two
; operands produces each coefficient -- which is exactly what the boot
; selftests check: the 8-channel entry above keeps the in-loop blend, so
; asrc_pair16_slot_selftest compares this path against it lane for lane.
;*****************************************************************************

; _stream8pair_ce_prep -- evaluate both blended coefficient rows once.
;   w2 = c00, w3 = c01, w6 = c10, w7 = c11, w4 = tap count,
;   w8 = &ce0[0], w9 = &ce1[0], f20 = wb0, f21 = wb1.
; FCR must already be set by the caller (this runs before any body call).
; Destroys w2, w3, w4, w6, w7, w8, w9 and f16..f19; preserves w0, w1, w5, w10..w14.
; Unrolled by two: the tap count is always even here (28/30/32), and one loop
; branch per two taps is worth ~1% of the whole pull.
;
; Near ratio 1, adjacent output frames normally select the same two stored
; phase rows and differ only in wb.  Reuse that row pair when both pointers
; match: one c0/c1 load and one subtraction then serve both blends.  The
; subtraction result and both MAC operand orders are bit-identical to the
; general path; only the duplicate load/subtract are removed.
_stream8pair_ce_prep:
    cp.l    w2, w6
    bra     nz, _stream8pair_ce_prep_distinct
    cp.l    w3, w7
    bra     nz, _stream8pair_ce_prep_distinct

    lsr.l   w4, #1, w4
v_stream8pair_ce_prep_same_loop:
    mov.l   [w2++], f18                     ; shared c0[k]
    mov.l   [w3++], f19                     ; shared c1[k]
    sub.s   f19, f18, f19                   ; d = c1 - c0
    mov.s   f18, f16                        ; independent c0 seed for output 1
    mac.s   f19, f20, f18                   ; ce0 = c0 + wb0 * d
    mac.s   f19, f21, f16                   ; ce1 = c0 + wb1 * d
    mov.l   f18, [w8++]
    mov.l   f16, [w9++]

    mov.l   [w2++], f18                     ; shared c0[k+1]
    mov.l   [w3++], f19
    sub.s   f19, f18, f19
    mov.s   f18, f16
    mac.s   f19, f20, f18
    mac.s   f19, f21, f16
    mov.l   f18, [w8++]
    mov.l   f16, [w9++]
    DTB     w4, v_stream8pair_ce_prep_same_loop
    return

_stream8pair_ce_prep_distinct:
    lsr.l   w4, #1, w4
v_stream8pair_ce_prep_loop:
    mov.l   [w2++], f18                     ; c00[k]
    mov.l   [w3++], f19                     ; c01[k]
    mov.l   [w6++], f16                     ; c10[k]
    mov.l   [w7++], f17                     ; c11[k]
    sub.s   f19, f18, f19                   ; d0 = c01 - c00
    sub.s   f17, f16, f17                   ; d1 = c11 - c10
    mac.s   f19, f20, f18                   ; ce0 = c00 + wb0 * d0
    mac.s   f17, f21, f16                   ; ce1 = c10 + wb1 * d1
    mov.l   f18, [w8++]
    mov.l   f16, [w9++]

    mov.l   [w2++], f18                     ; c00[k+1]
    mov.l   [w3++], f19
    mov.l   [w6++], f16
    mov.l   [w7++], f17
    sub.s   f19, f18, f19
    sub.s   f17, f16, f17
    mac.s   f19, f20, f18
    mac.s   f17, f21, f16
    mov.l   f18, [w8++]
    mov.l   f16, [w9++]
    DTB     w4, v_stream8pair_ce_prep_loop
    return

; _stream8pair_slot_body_ce -- the pair body against pre-blended coefficients.
;   w0 = channel-0 window base, w1 = channel stride in bytes, w2 = &ce0[0],
;   w4 = tap count, w5 = out16, w6 = &ce1[0].
; Unlike _stream8pair_slot_body this does NOT run floatsetup (the wrapper sets
; FCR once, before the prep pass), so w1 stays live for the second group; w3 and
; w7 are untouched and carry the wrapper's group-1 pointers across the call.
;
; The accumulators are seeded by the two peeled endpoint/first taps with mul.s
; instead of 16 movc.s zero-inits (C3), and the remaining M-2 taps run unrolled
; by two (C4). w4 must therefore be EVEN and >= 4 -- true for every caller here
; (M = 28/30/32). The generic runtime-M entry keeps using _stream8pair_slot_body.
_stream8pair_slot_body_ce:
    add.l   w0, w1, w8
    add.l   w8, w1, w9
    add.l   w9, w1, w10
    add.l   w10, w1, w11
    add.l   w11, w1, w12
    add.l   w12, w1, w13
    add.l   w13, w1, w14

    ; Endpoint base+0: output 0 only -- and it initialises f0..f7.
    mov.l   [w2++], f16
    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mul.s   f18, f16, f0
    mov.l   [w9++], f18
    mul.s   f19, f16, f1
    mov.l   [w10++], f19
    mul.s   f18, f16, f2
    mov.l   [w11++], f18
    mul.s   f19, f16, f3
    mov.l   [w12++], f19
    mul.s   f18, f16, f4
    mov.l   [w13++], f18
    mul.s   f19, f16, f5
    mov.l   [w14++], f19
    mul.s   f18, f16, f6
    mul.s   f19, f16, f7

    ; First shared tap (window base+1): initialises f8..f15.
    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mul.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mul.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mul.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mul.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mul.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mul.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mul.s   f18, f17, f14
    mac.s   f19, f16, f7
    mul.s   f19, f17, f15

    sub.l   w4, #2, w4
    cp0.l   w4
    bra     z, _stream8pair_slot_tail_ce
    lsr.l   w4, #1, w4                      ; M-2 taps, two per iteration

v_stream8pair_slot_ce_loop:
    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15

    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15
    DTB     w4, v_stream8pair_slot_ce_loop

_stream8pair_slot_tail_ce:
    ; Endpoint base+M: output 1 only.
    mov.l   [w6++], f17
    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f17, f14
    mac.s   f19, f17, f15
    bra     _stream8pair_slot_pack

;*****************************************************************************
; General output-stride pair ("ced") path.
;
; _stream8pair_slot_body_ce above only works when the second output's history
; window starts exactly ONE frame after the first (d = rd1 - rd0 = 1), which is
; true at ratio ~= 1 and false at every other ratio: a 48k->16k pull advances rd
; by 3 per output frame, an up-conversion by 0 or 1.  Those ratios therefore fell
; back to two single-output STREAM8 passes (20 instructions per tap for 8 MACs,
; plus a C-level float->slot conversion per lane) instead of 26 per tap for 16.
;
; The fix is to evaluate both outputs over one UNION window of U = M + d taps and
; pad the two coefficient rows with zeros so that every tap contributes to both
; accumulator banks:
;
;   ce0 = [ blend0(0 .. M-1) , 0 ... ]                    (d + pad zeros at the end)
;   ce1 = [ 0 (d of them) , blend1(0 .. M-1) , 0 ... ]
;
; so out0 = sum h[base+k]*ce0[k] and out1 = sum h[base+k]*ce1[k] = the ordinary
; single-output result at window base+d.  U is rounded UP to an odd value with one
; extra all-zero tap, so one peeled seeding tap plus an even remainder covers every
; d with a single entry point (no parity branch, no tail block).  Multiplying by
; +0.0 instead of skipping a tap can only produce -0.0 instead of +0.0, which
; packs to the same int32 -- the path stays bit-exact against STREAM8.
;
; Cost per 8-channel group: 26*U instead of 40*M, i.e. 858 vs 1200 instructions at
; M=30, d=3 -- and the 16 float->slot conversions move into the kernel's pack block.
;*****************************************************************************

    .equ    ASRC_CED_MAX_TAPS, 44           ; >= M + DMAX + 1 for M = 28/30/32
    .equ    ASRC_CED_FRAME,    360          ; 2 * 44 * 4 + 8 (one spill word)
    .equ    ASRC_CED0_BACK,    360          ; &ce0[0] = w15 - 360
    .equ    ASRC_CED1_BACK,    184          ; &ce1[0] = w15 - 184
    .equ    ASRC_CED_SPU_BACK, 8            ; spilled union tap count = w15 - 8

; _stream8pair_ced_zero -- write `count` +0.0f words at [w8++].
;   w8 = destination, w4 = count (0 is allowed).
; Destroys w4, w8 and f18; preserves every other register.
_stream8pair_ced_zero:
    cp0.l   w4
    bra     z, v_stream8pair_ced_zero_done
    movc.s  #22, f18                        ; +0.0f
v_stream8pair_ced_zero_loop:
    mov.l   f18, [w8++]
    DTB     w4, v_stream8pair_ced_zero_loop
v_stream8pair_ced_zero_done:
    return

; _stream8pair_slot_body_ced -- pair body over the padded union window.
;   w0 = channel-0 window base, w1 = channel stride in bytes, w2 = &ce0[0],
;   w4 = union tap count (ODD, >= 31), w5 = out16, w6 = &ce1[0].
; Identical to _stream8pair_slot_body_ce except that every tap feeds both banks,
; so the peeled first tap seeds all sixteen accumulators and there is no tail.
; Like that body it does not run floatsetup (the wrapper does it once) and leaves
; w3 and w7 alone so the wrapper can carry its group-1 pointers across the call.
_stream8pair_slot_body_ced:
    add.l   w0, w1, w8
    add.l   w8, w1, w9
    add.l   w9, w1, w10
    add.l   w10, w1, w11
    add.l   w11, w1, w12
    add.l   w12, w1, w13
    add.l   w13, w1, w14

    ; Union tap 0: seeds f0..f7 (output 0) and f8..f15 (output 1).
    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mul.s   f18, f16, f0
    mul.s   f18, f17, f8
    mov.l   [w9++], f18
    mul.s   f19, f16, f1
    mul.s   f19, f17, f9
    mov.l   [w10++], f19
    mul.s   f18, f16, f2
    mul.s   f18, f17, f10
    mov.l   [w11++], f18
    mul.s   f19, f16, f3
    mul.s   f19, f17, f11
    mov.l   [w12++], f19
    mul.s   f18, f16, f4
    mul.s   f18, f17, f12
    mov.l   [w13++], f18
    mul.s   f19, f16, f5
    mul.s   f19, f17, f13
    mov.l   [w14++], f19
    mul.s   f18, f16, f6
    mul.s   f18, f17, f14
    mul.s   f19, f16, f7
    mul.s   f19, f17, f15

    sub.l   w4, #1, w4
    lsr.l   w4, #1, w4                      ; U-1 taps, two per iteration

v_stream8pair_slot_ced_loop:
    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15

    mov.l   [w2++], f16
    mov.l   [w6++], f17

    mov.l   [w0++], f18
    mov.l   [w8++], f19
    mac.s   f18, f16, f0
    mac.s   f18, f17, f8
    mov.l   [w9++], f18
    mac.s   f19, f16, f1
    mac.s   f19, f17, f9
    mov.l   [w10++], f19
    mac.s   f18, f16, f2
    mac.s   f18, f17, f10
    mov.l   [w11++], f18
    mac.s   f19, f16, f3
    mac.s   f19, f17, f11
    mov.l   [w12++], f19
    mac.s   f18, f16, f4
    mac.s   f18, f17, f12
    mov.l   [w13++], f18
    mac.s   f19, f16, f5
    mac.s   f19, f17, f13
    mov.l   [w14++], f19
    mac.s   f18, f16, f6
    mac.s   f18, f17, f14
    mac.s   f19, f16, f7
    mac.s   f19, f17, f15
    DTB     w4, v_stream8pair_slot_ced_loop
    bra     _stream8pair_slot_pack

; Release the union-window frame, then fall into the shared restore.
_stream8pair_slot_epilogue_ced:
    movs.l  #ASRC_CED_FRAME, w0
    sub.l   w15, w0, w15
    bra     _stream8pair_slot_epilogue

;*****************************************************************************
; mchp_stream16_paird_f32 -- 16 channels, two outputs, ARBITRARY output stride.
;
; Same result as mchp_stream16_pair_slot{28,30,32}_f32 but the second output's
; window may start d frames after the first instead of exactly one (see the "ced"
; comment block above).  The extra parameters would push the argument list past
; the register-argument limit, so everything constant across the two groups --
; including the tap count, which is why one entry point covers every M -- arrives
; in a descriptor:
;
;   typedef struct {                 // mchp_stream16_paird_desc_t
;       const float *wbase0;         // +0   channel-0 window base (group 0)
;       const float *c00, *c01;      // +4  +8   output-0 phase rows
;       const float *c10, *c11;      // +12 +16  output-1 phase rows
;       float        wb0, wb1;       // +20 +24  blend weights
;       uint32_t     dstep;          // +28  d = rd1 - rd0   (0 .. DMAX)
;       uint32_t     utaps;          // +32  U, odd, M + d <= U <= 44
;       uint32_t     pad0;           // +36  U - M           (ce0 trailing zeros)
;       uint32_t     pad1;           // +40  U - M - d       (ce1 trailing zeros)
;       uint32_t     mbytes;         // +44  M * 4
;   }
;
; void mchp_stream16_paird_f32(const mchp_stream16_paird_desc_t *desc,
;                              uint32_t strideBytes,
;                              int32_t *out_group0, int32_t *out_group1);
;
; The caller must guarantee that the union window base .. base+U-1 is contiguous
; in the mirrored history ring, i.e. that wbase(rd1) == wbase(rd0) + d.
;*****************************************************************************

    .global _mchp_stream16_paird_f32
_mchp_stream16_paird_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    movs.l  #ASRC_CED_FRAME, w10
    add.l   w15, w10, w15
    floatsetup w10

    ; --- descriptor -> registers (w0 stays the descriptor until the prep call) ---
    mov.l   [w0+20], f20                    ; wb0
    mov.l   [w0+24], f21                    ; wb1
    mov.l   w2, w5                          ; out_group0
    mov.l   w3, w10                         ; out_group1
    mov.l   [w0+0], w11                     ; group-0 window base
    sl.l    w1, #3, w12
    add.l   w11, w12, w12                   ; group-1 window base
    mov.l   [w0+28], w13                    ; d
    mov.l   [w0+44], w14                    ; M * 4
    mov.l   [w0+32], w2                     ; U
    movs.l  #ASRC_CED_SPU_BACK, w3
    sub.l   w15, w3, w3
    mov.l   w2, [w3]                        ; spill U (both body calls need it)

    ; --- zero padding: ce0 tail, then ce1 head and tail ---
    movs.l  #ASRC_CED0_BACK, w8
    sub.l   w15, w8, w8
    add.l   w8, w14, w8                     ; &ce0[M]
    mov.l   [w0+36], w4                     ; pad0
    rcall   _stream8pair_ced_zero

    movs.l  #ASRC_CED1_BACK, w8
    sub.l   w15, w8, w8                     ; &ce1[0]
    mov.l   w13, w4                         ; d
    rcall   _stream8pair_ced_zero

    movs.l  #ASRC_CED1_BACK, w8
    sub.l   w15, w8, w8
    sl.l    w13, #2, w9
    add.l   w8, w9, w8
    add.l   w8, w14, w8                     ; &ce1[d + M]
    mov.l   [w0+40], w4                     ; pad1
    rcall   _stream8pair_ced_zero

    ; --- blend both rows once; ce1 lands at offset d ---
    movs.l  #ASRC_CED0_BACK, w8
    sub.l   w15, w8, w8                     ; &ce0[0]
    movs.l  #ASRC_CED1_BACK, w9
    sub.l   w15, w9, w9
    sl.l    w13, #2, w6
    add.l   w9, w6, w9                      ; &ce1[d]
    lsr.l   w14, #2, w4                     ; M
    mov.l   [w0+12], w6                     ; c10
    mov.l   [w0+16], w7                     ; c11
    mov.l   [w0+8], w3                      ; c01
    mov.l   [w0+4], w2                      ; c00
    rcall   _stream8pair_ce_prep

    mov.l   w12, w3                         ; carry group-1 window base
    mov.l   w10, w7                         ; carry out_group1
    mov.l   w11, w0                         ; group-0 window base
    movs.l  #ASRC_CED0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CED1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #ASRC_CED_SPU_BACK, w4
    sub.l   w15, w4, w4
    mov.l   [w4], w4                        ; U
    rcall   _stream8pair_slot_body_ced

    mov.l   w3, w0
    mov.l   w7, w5
    movs.l  #ASRC_CED0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CED1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #ASRC_CED_SPU_BACK, w4
    sub.l   w15, w4, w4
    mov.l   [w4], w4                        ; U
    rcall   _stream8pair_slot_body_ced
    bra     _stream8pair_slot_epilogue_ced

;*****************************************************************************
; mchp_stream16_pair_slot32_f32 -- two 8-channel groups, one register frame.
;
; Fixed at the ASRC hot-path tap count (M=32). The two output pointers each
; receive one group in pair-kernel order: [frame0 lanes 0..7, frame1 lanes 0..7].
;
; void mchp_stream16_pair_slot32_f32(const float *wbase0, uint32_t strideBytes,
;                                    const float *c00, const float *c01,
;                                    int32_t *out_group0, int32_t *out_group1,
;                                    const float *c10, const float *c11,
;                                    float wb0, float wb1);
;*****************************************************************************

    .global _mchp_stream16_pair_slot32_f32
_mchp_stream16_pair_slot32_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    mov.s   f0, f20
    mov.s   f1, f21

    ; Blended-coefficient frame first, then FCR (floatsetup destroys its scratch).
    movs.l  #ASRC_CE_FRAME, w10
    add.l   w15, w10, w15
    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8                     ; &ce0[0]
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9                     ; &ce1[0]
    floatsetup w10

    ; The prep pass consumes the four coefficient rows in w2/w3/w6/w7 and
    ; preserves w0/w1/w5/w10..w14, so stage the two group-1 values around it.
    mov.l   w4, w10                         ; out_group0
    sl.l    w1, #3, w11
    add.l   w0, w11, w11                    ; group-1 base = channel 0 + 8*stride
    movs.l  #32, w4
    rcall   _stream8pair_ce_prep

    ; w3 and w7 are unused by the ce body, so they carry group 1 across it.
    mov.l   w5, w7                          ; out_group1
    mov.l   w11, w3                         ; group-1 window base
    mov.l   w10, w5                         ; out_group0 -> this body's out16
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #32, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   w3, w0
    mov.l   w7, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #32, w4
    rcall   _stream8pair_slot_body_ce
    bra     _stream8pair_slot_epilogue_ce

;*****************************************************************************
; mchp_stream16_block_slot32_f32 -- multiple pair descriptors, one frame.
;
; Descriptor layout (28 bytes):
;   +0 wbase0, +4 c00, +8 c01, +12 c10, +16 c11, +20 wb0, +24 wb1.
; out advances by 16 int32 slots per descriptor. hidden16 is reused because the
; physical transport is still TDM8; all hidden outputs are nevertheless computed.
;
; void mchp_stream16_block_slot32_f32(const desc *d, uint32_t pairCount,
;                                     uint32_t strideBytes, int32_t *out,
;                                     int32_t *hidden16);
;*****************************************************************************

    .global _mchp_stream16_block_slot32_f32
_mchp_stream16_block_slot32_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    ; f22..f26 are untouched by the arithmetic body and retain loop state.
    push.l  f22
    push.l  f23
    push.l  f24
    push.l  f25
    push.l  f26
    mov.l   w0, f22
    mov.l   w1, f23
    mov.l   w2, f24
    mov.l   w3, f25
    mov.l   w4, f26

    ; One blended-coefficient frame and one FCR setup for the whole descriptor loop.
    movs.l  #ASRC_CE_FRAME, w0
    add.l   w15, w0, w15
    floatsetup w0

_stream16_block_loop:
    mov.l   f22, w0
    mov.l   [w0+20], f20
    mov.l   [w0+24], f21
    mov.l   [w0+16], w7
    mov.l   [w0+12], w6
    mov.l   [w0+8], w3
    mov.l   [w0+4], w2
    mov.l   [w0], w0
    mov.l   f24, w1

    ; Blend this descriptor's two rows once, for both channel groups.
    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9
    sl.l    w1, #3, w10
    add.l   w0, w10, w10                    ; group-1 base = channel 0 + 8*stride
    movs.l  #32, w4
    rcall   _stream8pair_ce_prep

    mov.l   w10, w3                         ; group-1 base survives the ce body
    mov.l   f25, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #32, w4
    rcall   _stream8pair_slot_body_ce

    ; Channels 8..15 -> hidden16, same coefficients, stride still live in w1.
    mov.l   w3, w0
    mov.l   f26, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #32, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   f22, w0
    add.l   w0, #28, w0
    mov.l   w0, f22
    mov.l   f25, w0
    movs.l  #64, w1
    add.l   w0, w1, w0
    mov.l   w0, f25
    mov.l   f23, w0
    sub.l   w0, #1, w0
    mov.l   w0, f23
    cp0.l   w0
    bra     nz, _stream16_block_loop

    movs.l  #ASRC_CE_FRAME, w0
    sub.l   w15, w0, w15
    pop.l   f26
    pop.l   f25
    pop.l   f24
    pop.l   f23
    pop.l   f22
    bra     _stream8pair_slot_epilogue

;*****************************************************************************
; M=30 variants used by the production default.  Arithmetic and ABI are the
; same as the proven M=32 wrappers above; only the immediate tap count and the
; body-consumption byte offsets differ:
;   history: (M+1)*4 = 124 bytes, coefficients: M*4 = 120 bytes.
; Keep these wrappers separate from the shared arithmetic body so an
; experimental M28 link can garbage-collect them.
;*****************************************************************************

    .section .dspic33cmsisdsp_m30, code

    .global _mchp_stream16_pair_slot30_f32
_mchp_stream16_pair_slot30_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    mov.s   f0, f20
    mov.s   f1, f21

    movs.l  #ASRC_CE_FRAME, w10
    add.l   w15, w10, w15
    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9
    floatsetup w10

    mov.l   w4, w10                         ; out_group0
    sl.l    w1, #3, w11
    add.l   w0, w11, w11                    ; group-1 base
    movs.l  #30, w4
    rcall   _stream8pair_ce_prep

    mov.l   w5, w7                          ; out_group1
    mov.l   w11, w3                         ; group-1 window base
    mov.l   w10, w5                         ; out_group0
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #30, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   w3, w0
    mov.l   w7, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #30, w4
    rcall   _stream8pair_slot_body_ce
    bra     _stream8pair_slot_epilogue_ce

    .global _mchp_stream16_block_slot30_f32
_mchp_stream16_block_slot30_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    push.l  f22
    push.l  f23
    push.l  f24
    push.l  f25
    push.l  f26
    mov.l   w0, f22
    mov.l   w1, f23
    mov.l   w2, f24
    mov.l   w3, f25
    mov.l   w4, f26

    movs.l  #ASRC_CE_FRAME, w0
    add.l   w15, w0, w15
    floatsetup w0

_stream16_block30_loop:
    mov.l   f22, w0
    mov.l   [w0+20], f20
    mov.l   [w0+24], f21
    mov.l   [w0+16], w7
    mov.l   [w0+12], w6
    mov.l   [w0+8], w3
    mov.l   [w0+4], w2
    mov.l   [w0], w0
    mov.l   f24, w1

    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9
    sl.l    w1, #3, w10
    add.l   w0, w10, w10
    movs.l  #30, w4
    rcall   _stream8pair_ce_prep

    mov.l   w10, w3
    mov.l   f25, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #30, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   w3, w0
    mov.l   f26, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #30, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   f22, w0
    add.l   w0, #28, w0
    mov.l   w0, f22
    mov.l   f25, w0
    movs.l  #64, w1
    add.l   w0, w1, w0
    mov.l   w0, f25
    mov.l   f23, w0
    sub.l   w0, #1, w0
    mov.l   w0, f23
    cp0.l   w0
    bra     nz, _stream16_block30_loop

    movs.l  #ASRC_CE_FRAME, w0
    sub.l   w15, w0, w15
    pop.l   f26
    pop.l   f25
    pop.l   f24
    pop.l   f23
    pop.l   f22
    bra     _stream8pair_slot_epilogue

;*****************************************************************************
; M=28 headroom candidates.  The shared arithmetic body is unchanged; fixed
; immediates remove the generic tap-count setup from the hot C path:
;   history: (M+1)*4 = 116 bytes, coefficients: M*4 = 112 bytes.
; Keep them in a separate input section so the formal M30 image drops them via
; --gc-sections; the experimental M28 compile-time switch retains them.
;*****************************************************************************

    .section .dspic33cmsisdsp_m28, code

    .global _mchp_stream16_pair_slot28_f32
_mchp_stream16_pair_slot28_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    mov.s   f0, f20
    mov.s   f1, f21

    movs.l  #ASRC_CE_FRAME, w10
    add.l   w15, w10, w15
    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9
    floatsetup w10

    mov.l   w4, w10                         ; out_group0
    sl.l    w1, #3, w11
    add.l   w0, w11, w11                    ; group-1 base
    movs.l  #28, w4
    rcall   _stream8pair_ce_prep

    mov.l   w5, w7                          ; out_group1
    mov.l   w11, w3                         ; group-1 window base
    mov.l   w10, w5                         ; out_group0
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #28, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   w3, w0
    mov.l   w7, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #28, w4
    rcall   _stream8pair_slot_body_ce
    bra     _stream8pair_slot_epilogue_ce

    .global _mchp_stream16_block_slot28_f32
_mchp_stream16_block_slot28_f32:
    push.l  f8
    push.l  f9
    push.l  f10
    push.l  f11
    push.l  f12
    push.l  f13
    push.l  f14
    push.l  f15
    push.l  f16
    push.l  f17
    push.l  f18
    push.l  f19
    push.l  f20
    push.l  f21
    push.l  w8
    push.l  w9
    push.l  w10
    push.l  w11
    push.l  w12
    push.l  w13
    push.l  w14
    push.l  fcr

    push.l  f22
    push.l  f23
    push.l  f24
    push.l  f25
    push.l  f26
    mov.l   w0, f22
    mov.l   w1, f23
    mov.l   w2, f24
    mov.l   w3, f25
    mov.l   w4, f26

    movs.l  #ASRC_CE_FRAME, w0
    add.l   w15, w0, w15
    floatsetup w0

_stream16_block28_loop:
    mov.l   f22, w0
    mov.l   [w0+20], f20
    mov.l   [w0+24], f21
    mov.l   [w0+16], w7
    mov.l   [w0+12], w6
    mov.l   [w0+8], w3
    mov.l   [w0+4], w2
    mov.l   [w0], w0
    mov.l   f24, w1

    movs.l  #ASRC_CE0_BACK, w8
    sub.l   w15, w8, w8
    movs.l  #ASRC_CE1_BACK, w9
    sub.l   w15, w9, w9
    sl.l    w1, #3, w10
    add.l   w0, w10, w10
    movs.l  #28, w4
    rcall   _stream8pair_ce_prep

    mov.l   w10, w3
    mov.l   f25, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #28, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   w3, w0
    mov.l   f26, w5
    movs.l  #ASRC_CE0_BACK, w2
    sub.l   w15, w2, w2
    movs.l  #ASRC_CE1_BACK, w6
    sub.l   w15, w6, w6
    movs.l  #28, w4
    rcall   _stream8pair_slot_body_ce

    mov.l   f22, w0
    add.l   w0, #28, w0
    mov.l   w0, f22
    mov.l   f25, w0
    movs.l  #64, w1
    add.l   w0, w1, w0
    mov.l   w0, f25
    mov.l   f23, w0
    sub.l   w0, #1, w0
    mov.l   w0, f23
    cp0.l   w0
    bra     nz, _stream16_block28_loop

    movs.l  #ASRC_CE_FRAME, w0
    sub.l   w15, w0, w15
    pop.l   f26
    pop.l   f25
    pop.l   f24
    pop.l   f23
    pop.l   f22
    bra     _stream8pair_slot_epilogue

    .end
