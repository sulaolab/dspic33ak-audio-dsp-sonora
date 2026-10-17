;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x1s3p.s
;
; ONE CHANNEL x THREE ADJACENT SOS STAGES FUSED, input-pipelined.
;
; F0..F4   SOS0 coefficients       F5..F9   SOS1 coefficients
; F10..F14 SOS2 coefficients
; F15..F17 SOS0 (d1,d2,free) ring  F18..F20 SOS1 ring
; F21..F23 SOS2 ring               F24/F25 current/next input
;
; Every normal body is 15 FPU operations, one next-input load and one output
; store.  The final body omits the load, so blockSize=1..512 never reads past
; pSrc.  The state rings rotate every sample and input registers every two;
; six physical templates return both maps to phase 0.
;
; Contract: nonzero numStages, blockSize 1..512, in-place/out-of-place, and
; independent coefficients.  The 1/2-stage remainder is a correct scalar tail;
; AP84 has 84 stages and takes only the triple path.
;==============================================================================
        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x1s3p, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x1s3p

; void biquad_cascade_df2T_f32_dspic33ak_x1s3p(
;     const arm_biquad_cascade_df2T_instance_f32 *S,  ; W0
;     const float32_t *pSrc,                            ; W1
;     float32_t *pDst,                                  ; W2
;     uint32_t blockSize);                              ; W3
;
; W3 triple count, W4 sample-group count, W5 coefficient pointer,
; W6 state pointer, W7 tail/stage count.  Stack words after saves:
; [W15-20] blockSize, [W15-16] original pDst, [W15-12] stage remainder,
; [W15-8] groups=floor((N-1)/6), [W15-4] tail=(N-1)%6.

_biquad_cascade_df2T_f32_dspic33ak_x1s3p:
        cp.l        W3, #0
        bra         z, L_x1s3p_refuse
        cp.l        W3, #512
        bra         gtu, L_x1s3p_refuse
        clr         W4
        mov.b       [W0+0], W4
        cp.l        W4, #0
        bra         z, L_x1s3p_refuse

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
        push.l      FCR
        mov.l       #0x7F, W7         ; FCR defaults, exceptions masked
        floatsetup  W7

        mov.l       W3, [W15++]       ; blockSize
        mov.l       W2, [W15++]       ; original pDst

        ; W7=floor(numStages/3), W4=numStages%3.  This setup loop is outside
        ; the sample body and bounded by the uint8_t CMSIS stage count.
        clr         W7
L_x1s3p_div3:
        cp.l        W4, #3
        bra         ltu, L_x1s3p_div3_done
        sub.l       #3, W4
        add.l       #1, W7
        bra         L_x1s3p_div3
L_x1s3p_div3_done:
        mov.l       W4, [W15++]       ; stage remainder
        mov.l       W7, W3            ; outer triple count

        ; W4=(N-1)%6, W7=floor((N-1)/6), again outside the sample body.
        mov.l       [W15-12], W4
        sub.l       #1, W4
        clr         W7
L_x1s3p_div6:
        cp.l        W4, #6
        bra         ltu, L_x1s3p_div6_done
        sub.l       #6, W4
        add.l       #1, W7
        bra         L_x1s3p_div6
L_x1s3p_div6_done:
        mov.l       W7, [W15++]       ; complete six-sample groups
        mov.l       W4, [W15++]       ; 0..5 preloading tail samples

        mov.l       [W0+4], W5
        mov.l       [W0+8], W6
        cp.l        W3, #0
        bra         z, L_x1s3p_after_triples

;==============================================================================
; One three-SOS stage group.  X0 is deliberately loaded before coefficient and
; state loads: the first MAC is then 22 issue positions after the CPU write.
;==============================================================================
L_x1s3p_start_triple:
        mov.l       [W1++], F24

        mov.l       [W5++], F0
        mov.l       [W5++], F1
        mov.l       [W5++], F2
        mov.l       [W5++], F3
        mov.l       [W5++], F4
        mov.l       [W5++], F5
        mov.l       [W5++], F6
        mov.l       [W5++], F7
        mov.l       [W5++], F8
        mov.l       [W5++], F9
        mov.l       [W5++], F10
        mov.l       [W5++], F11
        mov.l       [W5++], F12
        mov.l       [W5++], F13
        mov.l       [W5++], F14

        mov.l       [W6],    F15
        mov.l       [W6+4],  F16
        mov.l       [W6+8],  F18
        mov.l       [W6+12], F19
        mov.l       [W6+16], F21
        mov.l       [W6+20], F22

        mov.l       [W15-8], W4
        mov.l       [W15-4], W7
        cp.l        W4, #0
        bra         z, L_x1s3p_tail

L_x1s3p_groups:
L_x1s3p_p0_pre:
        mac.s       F0,  F24, F15
        mac.s       F1,  F24, F16
        mul.s       F2,  F24, F17
        mov.l       [W1++], F25
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23

L_x1s3p_p1_pre:
        mac.s       F0,  F25, F16
        mac.s       F1,  F25, F17
        mul.s       F2,  F25, F15
        mov.l       [W1++], F24
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21

L_x1s3p_p2_pre:
        mac.s       F0,  F24, F17
        mac.s       F1,  F24, F15
        mul.s       F2,  F24, F16
        mov.l       [W1++], F25
        mac.s       F5,  F17, F20
        mac.s       F6,  F17, F18
        mac.s       F3,  F17, F15
        mac.s       F4,  F17, F16
        mul.s       F7,  F17, F19
        mac.s       F10, F20, F23
        mac.s       F8,  F20, F18
        mac.s       F9,  F20, F19
        mac.s       F11, F20, F21
        mul.s       F12, F20, F22
        mov.l       F23, [W2++]
        mac.s       F13, F23, F21
        mac.s       F14, F23, F22

L_x1s3p_p3_pre:
        mac.s       F0,  F25, F15
        mac.s       F1,  F25, F16
        mul.s       F2,  F25, F17
        mov.l       [W1++], F24
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23

L_x1s3p_p4_pre:
        mac.s       F0,  F24, F16
        mac.s       F1,  F24, F17
        mul.s       F2,  F24, F15
        mov.l       [W1++], F25
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21

L_x1s3p_p5_pre:
        mac.s       F0,  F25, F17
        mac.s       F1,  F25, F15
        mul.s       F2,  F25, F16
        mov.l       [W1++], F24
        mac.s       F5,  F17, F20
        mac.s       F6,  F17, F18
        mac.s       F3,  F17, F15
        mac.s       F4,  F17, F16
        mul.s       F7,  F17, F19
        mac.s       F10, F20, F23
        mac.s       F8,  F20, F18
        mac.s       F9,  F20, F19
        mac.s       F11, F20, F21
        mul.s       F12, F20, F22
        mov.l       F23, [W2++]
        mac.s       F13, F23, F21
        mac.s       F14, F23, F22
        dtb         W4, L_x1s3p_groups

;==============================================================================
; 0..5 preloading samples after complete six-phase groups.  The final sample
; branches to an explicit no-preload template, preventing an out-of-range read.
;==============================================================================
L_x1s3p_tail:
        cp.l        W7, #0
        bra         z, L_x1s3p_final_p0

L_x1s3p_tail_p0:
        mac.s       F0,  F24, F15
        mac.s       F1,  F24, F16
        mul.s       F2,  F24, F17
        mov.l       [W1++], F25
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23
        cp.l        W7, #1
        bra         z, L_x1s3p_final_p1

L_x1s3p_tail_p1:
        mac.s       F0,  F25, F16
        mac.s       F1,  F25, F17
        mul.s       F2,  F25, F15
        mov.l       [W1++], F24
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21
        cp.l        W7, #2
        bra         z, L_x1s3p_final_p2

L_x1s3p_tail_p2:
        mac.s       F0,  F24, F17
        mac.s       F1,  F24, F15
        mul.s       F2,  F24, F16
        mov.l       [W1++], F25
        mac.s       F5,  F17, F20
        mac.s       F6,  F17, F18
        mac.s       F3,  F17, F15
        mac.s       F4,  F17, F16
        mul.s       F7,  F17, F19
        mac.s       F10, F20, F23
        mac.s       F8,  F20, F18
        mac.s       F9,  F20, F19
        mac.s       F11, F20, F21
        mul.s       F12, F20, F22
        mov.l       F23, [W2++]
        mac.s       F13, F23, F21
        mac.s       F14, F23, F22
        cp.l        W7, #3
        bra         z, L_x1s3p_final_p3

L_x1s3p_tail_p3:
        mac.s       F0,  F25, F15
        mac.s       F1,  F25, F16
        mul.s       F2,  F25, F17
        mov.l       [W1++], F24
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23
        cp.l        W7, #4
        bra         z, L_x1s3p_final_p4

L_x1s3p_tail_p4:
        mac.s       F0,  F24, F16
        mac.s       F1,  F24, F17
        mul.s       F2,  F24, F15
        mov.l       [W1++], F25
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21
        cp.l        W7, #5
        bra         z, L_x1s3p_final_p5

;==============================================================================
; Final templates: the next-input load is omitted.  Phase 1 is placed directly
; before its state map, which is the N=32 product-point final phase.
;==============================================================================
L_x1s3p_final_p5:
        mac.s       F0,  F25, F17
        mac.s       F1,  F25, F15
        mul.s       F2,  F25, F16
        mac.s       F5,  F17, F20
        mac.s       F6,  F17, F18
        mac.s       F3,  F17, F15
        mac.s       F4,  F17, F16
        mul.s       F7,  F17, F19
        mac.s       F10, F20, F23
        mac.s       F8,  F20, F18
        mac.s       F9,  F20, F19
        mac.s       F11, F20, F21
        mul.s       F12, F20, F22
        mov.l       F23, [W2++]
        mac.s       F13, F23, F21
        mac.s       F14, F23, F22
        bra         L_x1s3p_store_p0

L_x1s3p_final_p0:
        mac.s       F0,  F24, F15
        mac.s       F1,  F24, F16
        mul.s       F2,  F24, F17
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23
        bra         L_x1s3p_store_p1

L_x1s3p_final_p2:
        mac.s       F0,  F24, F17
        mac.s       F1,  F24, F15
        mul.s       F2,  F24, F16
        mac.s       F5,  F17, F20
        mac.s       F6,  F17, F18
        mac.s       F3,  F17, F15
        mac.s       F4,  F17, F16
        mul.s       F7,  F17, F19
        mac.s       F10, F20, F23
        mac.s       F8,  F20, F18
        mac.s       F9,  F20, F19
        mac.s       F11, F20, F21
        mul.s       F12, F20, F22
        mov.l       F23, [W2++]
        mac.s       F13, F23, F21
        mac.s       F14, F23, F22
        bra         L_x1s3p_store_p0

L_x1s3p_final_p3:
        mac.s       F0,  F25, F15
        mac.s       F1,  F25, F16
        mul.s       F2,  F25, F17
        mac.s       F5,  F15, F18
        mac.s       F6,  F15, F19
        mac.s       F3,  F15, F16
        mac.s       F4,  F15, F17
        mul.s       F7,  F15, F20
        mac.s       F10, F18, F21
        mac.s       F8,  F18, F19
        mac.s       F9,  F18, F20
        mac.s       F11, F18, F22
        mul.s       F12, F18, F23
        mov.l       F21, [W2++]
        mac.s       F13, F21, F22
        mac.s       F14, F21, F23
        bra         L_x1s3p_store_p1

L_x1s3p_final_p4:
        mac.s       F0,  F24, F16
        mac.s       F1,  F24, F17
        mul.s       F2,  F24, F15
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21
        bra         L_x1s3p_store_p2

; State maps after final phase 1/4 -> phase 2, 0/3 -> phase 1, 2/5 -> phase 0.
; Store SOS0, SOS1, SOS2 so CPU reads the final stage results only after six
; further issue positions.  Keep final_p1 directly before store_p2: the N=32
; product path then needs neither a branch after its final sample nor one
; after the phase-2 state map.
L_x1s3p_store_p1:
        mov.l       F16, [W6++]
        mov.l       F17, [W6++]
        mov.l       F19, [W6++]
        mov.l       F20, [W6++]
        mov.l       F22, [W6++]
        mov.l       F23, [W6++]
        bra         L_x1s3p_next_triple

L_x1s3p_store_p0:
        mov.l       F15, [W6++]
        mov.l       F16, [W6++]
        mov.l       F18, [W6++]
        mov.l       F19, [W6++]
        mov.l       F21, [W6++]
        mov.l       F22, [W6++]
        bra         L_x1s3p_next_triple

L_x1s3p_final_p1:
        mac.s       F0,  F25, F16
        mac.s       F1,  F25, F17
        mul.s       F2,  F25, F15
        mac.s       F5,  F16, F19
        mac.s       F6,  F16, F20
        mac.s       F3,  F16, F17
        mac.s       F4,  F16, F15
        mul.s       F7,  F16, F18
        mac.s       F10, F19, F22
        mac.s       F8,  F19, F20
        mac.s       F9,  F19, F18
        mac.s       F11, F19, F23
        mul.s       F12, F19, F21
        mov.l       F22, [W2++]
        mac.s       F13, F22, F23
        mac.s       F14, F22, F21

L_x1s3p_store_p2:
        mov.l       F17, [W6++]
        mov.l       F15, [W6++]
        mov.l       F20, [W6++]
        mov.l       F18, [W6++]
        mov.l       F23, [W6++]
        mov.l       F21, [W6++]

L_x1s3p_next_triple:
        mov.l       [W15-16], W1      ; next group reads prior group output
        mov.l       [W15-16], W2
        dtb         W3, L_x1s3p_start_triple
        bra         L_x1s3p_after_triples

;==============================================================================
; Correct scalar remainder for stage counts not divisible by three.  Its cost is
; outside the AP84 product point; it keeps the public general-stage contract.
;==============================================================================
L_x1s3p_after_triples:
        mov.l       [W15-12], W7
        cp.l        W7, #0
        bra         z, L_x1s3p_done

L_x1s3p_tail_stage:
        mov.l       [W5++], F0
        mov.l       [W5++], F1
        mov.l       [W5++], F2
        mov.l       [W5++], F3
        mov.l       [W5++], F4
        mov.l       [W6],    F10
        mov.l       [W6+4],  F11
        mov.l       [W15-20], W4
L_x1s3p_tail_sample:
        mov.l       [W1++], F12
        mov.s       F10, F13
        mac.s       F0, F12, F13
        mac.s       F1, F12, F11
        mul.s       F2, F12, F12
        mov.l       F13, [W2++]
        mac.s       F3, F13, F11
        mac.s       F4, F13, F12
        mov.s       F11, F10
        mov.s       F12, F11
        dtb         W4, L_x1s3p_tail_sample

        mov.l       F10, [W6++]
        mov.l       F11, [W6++]
        mov.l       [W15-16], W1
        mov.l       [W15-16], W2
        dtb         W7, L_x1s3p_tail_stage

L_x1s3p_done:
        sub.l       #20, W15
        pop.l       FCR
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

L_x1s3p_refuse:
        return
