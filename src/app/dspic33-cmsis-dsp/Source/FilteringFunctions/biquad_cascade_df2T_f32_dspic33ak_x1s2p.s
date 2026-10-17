;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_x1s2p.s
;
; ONE CHANNEL x TWO ADJACENT SOS STAGES FUSED.
;
; This is deliberately the no-I/O-pipeline baseline.  The previous six-phase
; pending-output experiment was not safe on target silicon, so stage fusion is
; proved first with the ordinary per-sample input load and output store.  The
; intermediate SOS0 output remains in F15; it is never materialised in RAM.
;
; F map: F0..F4 SOS0 coefficients, F5..F9 SOS1 coefficients,
;        F10..F14 rotate five state/input roles; F15 is unused work space.
;        The state rotation is the natural permutation caused by in-place MAC
;        accumulates.  It avoids copy instructions without delaying external I/O.
;
; Steady body: 10 FPU + one input load + one final-output store = 12
; instructions per sample/pair (6.0 data instructions/sample/SOS).
;
; Contract: blockSize 1..512; nonzero numStages; input/output may alias;
; per-channel coefficients remain independent.  An odd final SOS is processed
; by the scalar stage-major tail.
;==============================================================================
        .include "dspcommon.inc"

        .section .dspic33cmsisdsp_df2t_x1s2p, code
        .global _biquad_cascade_df2T_f32_dspic33ak_x1s2p

; void biquad_cascade_df2T_f32_dspic33ak_x1s2p(
;     const arm_biquad_cascade_df2T_instance_f32 *S,  ; W0
;     const float32_t *pSrc,                            ; W1
;     float32_t *pDst,                                  ; W2
;     uint32_t blockSize);                              ; W3
;
; W3 pair count, W4 sample count, W5 coefficient pointer,
; W6 state pointer, W7 scratch.  Stack words: odd, pDst, blockSize.

_biquad_cascade_df2T_f32_dspic33ak_x1s2p:
        cp.l        W3, #0
        bra         z, L_x1s2p_refuse
        cp.l        W3, #512
        bra         gtu, L_x1s2p_refuse
        clr         W4
        mov.b       [W0+0], W4
        cp.l        W4, #0
        bra         z, L_x1s2p_refuse

        push.l      F8
        push.l      F9
        push.l      F10
        push.l      F11
        push.l      F12
        push.l      F13
        push.l      F14
        push.l      F15
        push.l      FCR
        mov.l       #0x7F, W7         ; FCR mask: defaults, exceptions masked
        floatsetup  W7

        and.l       W4, #1, W7
        mov.l       W7, [W15++]       ; [W15-12] odd final stage
        mov.l       W2, [W15++]       ; [W15-8]  original destination
        mov.l       W3, [W15++]       ; [W15-4]  blockSize
        lsr.l       W4, #1, W3        ; number of fused stage pairs
        mov.l       [W0+4], W5
        mov.l       [W0+8], W6

        cp.l        W3, #0
        bra         z, L_x1s2p_after_pairs

;==============================================================================
; Fused stage-pair loop.  Every output is written only after SOS1's five
; arithmetic operations; hence in-place input/output is safe.
;==============================================================================
L_x1s2p_start_pair:
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
        mov.l       [W6],    F10
        mov.l       [W6+4],  F11
        mov.l       [W6+8],  F12
        mov.l       [W6+12], F13
        ; q=N/5 in W7, r=N%5 in W4.  The subtract loop is outside the
        ; sample body and bounded to 102 iterations by the public N<=512 gate.
        mov.l       [W15-4], W4
        clr         W7
L_x1s2p_div5:
        cp.l        W4, #5
        bra         ltu, L_x1s2p_counts_done
        sub.l       #5, W4
        add.l       #1, W7
        bra         L_x1s2p_div5

L_x1s2p_counts_done:
        cp.l        W7, #0
        bra         z, L_x1s2p_tail

L_x1s2p_groups:
        ; phase 0: d10/d20/d11/d21/x = F10/F11/F12/F13/F14
        mov.l       [W1++], F14
        mac.s       F0, F14, F10
        mac.s       F1, F14, F11
        mul.s       F2, F14, F14
        mac.s       F3, F10, F11
        mac.s       F4, F10, F14
        mac.s       F5, F10, F12
        mac.s       F6, F10, F13
        mul.s       F7, F10, F10
        mov.l       F12, [W2++]
        mac.s       F8, F12, F13
        mac.s       F9, F12, F10

        ; phase 1: F11/F14/F13/F10/F12
        mov.l       [W1++], F12
        mac.s       F0, F12, F11
        mac.s       F1, F12, F14
        mul.s       F2, F12, F12
        mac.s       F3, F11, F14
        mac.s       F4, F11, F12
        mac.s       F5, F11, F13
        mac.s       F6, F11, F10
        mul.s       F7, F11, F11
        mov.l       F13, [W2++]
        mac.s       F8, F13, F10
        mac.s       F9, F13, F11

        ; phase 2: F14/F12/F10/F11/F13
        mov.l       [W1++], F13
        mac.s       F0, F13, F14
        mac.s       F1, F13, F12
        mul.s       F2, F13, F13
        mac.s       F3, F14, F12
        mac.s       F4, F14, F13
        mac.s       F5, F14, F10
        mac.s       F6, F14, F11
        mul.s       F7, F14, F14
        mov.l       F10, [W2++]
        mac.s       F8, F10, F11
        mac.s       F9, F10, F14

        ; phase 3: F12/F13/F11/F14/F10
        mov.l       [W1++], F10
        mac.s       F0, F10, F12
        mac.s       F1, F10, F13
        mul.s       F2, F10, F10
        mac.s       F3, F12, F13
        mac.s       F4, F12, F10
        mac.s       F5, F12, F11
        mac.s       F6, F12, F14
        mul.s       F7, F12, F12
        mov.l       F11, [W2++]
        mac.s       F8, F11, F14
        mac.s       F9, F11, F12

        ; phase 4: F13/F10/F14/F12/F11; returns to phase 0 roles.
        mov.l       [W1++], F11
        mac.s       F0, F11, F13
        mac.s       F1, F11, F10
        mul.s       F2, F11, F11
        mac.s       F3, F13, F10
        mac.s       F4, F13, F11
        mac.s       F5, F13, F14
        mac.s       F6, F13, F12
        mul.s       F7, F13, F13
        mov.l       F14, [W2++]
        mac.s       F8, F14, F12
        mac.s       F9, F14, F13
        dtb         W7, L_x1s2p_groups

L_x1s2p_tail:
        cp.l        W4, #0
        bra         z, L_x1s2p_store_p0
        ; tail phase 0
        mov.l       [W1++], F14
        mac.s       F0, F14, F10
        mac.s       F1, F14, F11
        mul.s       F2, F14, F14
        mac.s       F3, F10, F11
        mac.s       F4, F10, F14
        mac.s       F5, F10, F12
        mac.s       F6, F10, F13
        mul.s       F7, F10, F10
        mov.l       F12, [W2++]
        mac.s       F8, F12, F13
        mac.s       F9, F12, F10
        cp.l        W4, #1
        bra         z, L_x1s2p_store_p1
        ; tail phase 1
        mov.l       [W1++], F12
        mac.s       F0, F12, F11
        mac.s       F1, F12, F14
        mul.s       F2, F12, F12
        mac.s       F3, F11, F14
        mac.s       F4, F11, F12
        mac.s       F5, F11, F13
        mac.s       F6, F11, F10
        mul.s       F7, F11, F11
        mov.l       F13, [W2++]
        mac.s       F8, F13, F10
        mac.s       F9, F13, F11
        cp.l        W4, #2
        bra         z, L_x1s2p_store_p2
        ; tail phase 2
        mov.l       [W1++], F13
        mac.s       F0, F13, F14
        mac.s       F1, F13, F12
        mul.s       F2, F13, F13
        mac.s       F3, F14, F12
        mac.s       F4, F14, F13
        mac.s       F5, F14, F10
        mac.s       F6, F14, F11
        mul.s       F7, F14, F14
        mov.l       F10, [W2++]
        mac.s       F8, F10, F11
        mac.s       F9, F10, F14
        cp.l        W4, #3
        bra         z, L_x1s2p_store_p3
        ; tail phase 3
        mov.l       [W1++], F10
        mac.s       F0, F10, F12
        mac.s       F1, F10, F13
        mul.s       F2, F10, F10
        mac.s       F3, F12, F13
        mac.s       F4, F12, F10
        mac.s       F5, F12, F11
        mac.s       F6, F12, F14
        mul.s       F7, F12, F12
        mov.l       F11, [W2++]
        mac.s       F8, F11, F14
        mac.s       F9, F11, F12

L_x1s2p_store_p4:
        mov.l       F13, [W6++]
        mov.l       F10, [W6++]
        mov.l       F14, [W6++]
        mov.l       F12, [W6++]
        bra         L_x1s2p_next_pair
L_x1s2p_store_p3:
        mov.l       F12, [W6++]
        mov.l       F13, [W6++]
        mov.l       F11, [W6++]
        mov.l       F14, [W6++]
        bra         L_x1s2p_next_pair
L_x1s2p_store_p2:
        mov.l       F14, [W6++]
        mov.l       F12, [W6++]
        mov.l       F10, [W6++]
        mov.l       F11, [W6++]
        bra         L_x1s2p_next_pair
L_x1s2p_store_p1:
        mov.l       F11, [W6++]
        mov.l       F14, [W6++]
        mov.l       F13, [W6++]
        mov.l       F10, [W6++]
        bra         L_x1s2p_next_pair
L_x1s2p_store_p0:
        mov.l       F10, [W6++]
        mov.l       F11, [W6++]
        mov.l       F12, [W6++]
        mov.l       F13, [W6++]

L_x1s2p_next_pair:
        mov.l       [W15-8], W1       ; next pair reads prior pair output
        mov.l       [W15-8], W2
        dtb         W3, L_x1s2p_start_pair

;==============================================================================
; Scalar final SOS for odd stage counts.
;==============================================================================
L_x1s2p_after_pairs:
        mov.l       [W15-12], W7
        cp.l        W7, #0
        bra         z, L_x1s2p_done

        mov.l       [W5++], F0
        mov.l       [W5++], F1
        mov.l       [W5++], F2
        mov.l       [W5++], F3
        mov.l       [W5++], F4
        mov.l       [W6],    F10
        mov.l       [W6+4],  F11
        mov.l       [W15-4], W4
L_x1s2p_odd_sample:
        mov.l       [W1++], F12
        mac.s       F0, F12, F13
        mac.s       F1, F12, F11
        mul.s       F2, F12, F12
        mov.l       F13, [W2++]
        mac.s       F3, F13, F11
        mac.s       F4, F13, F12
        dtb         W4, L_x1s2p_odd_sample

        mov.l       F10, [W6]
        mov.l       F11, [W6+4]

L_x1s2p_done:
        sub.l       #12, W15
        pop.l       FCR
        pop.l       F15
        pop.l       F14
        pop.l       F13
        pop.l       F12
        pop.l       F11
        pop.l       F10
        pop.l       F9
        pop.l       F8
        return

L_x1s2p_refuse:
        return
