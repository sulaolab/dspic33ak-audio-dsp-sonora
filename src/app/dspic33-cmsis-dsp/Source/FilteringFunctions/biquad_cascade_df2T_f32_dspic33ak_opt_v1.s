;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_opt_v1_dtb_stage_sample_y_d1_mac_v7.s
;
; dsPIC33AK FPU DF2T biquad cascade, single-channel block processor
;
; Purpose:
;   - Preserve the original CMSIS/Microchip library implementation.
;   - Provide a separate, drop-in test kernel with the same C-call interface.
;   - Process one contiguous channel buffer through all DF2T stages.
;   - Reduce avoidable stalls in the inner sample loop by scheduling
;     independent FPU operations between dependent operations.
;   - Final measured result in current ch-major pipeline:
;       stage=70, blockSize=32, CMSIS-IIR ~= 139.8us,
;       DMA0 max ~= 620-624us, margin ~= 42-46us.
;
; C prototype:
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_opt_v1(
;       const arm_biquad_cascade_df2T_instance_f32 *S,
;       const float32_t *pSrc,
;       float32_t *pDst,
;       uint32_t blockSize
;   );
;
; Calling convention observed from existing disassembly:
;   W0 = S
;   W1 = pSrc
;   W2 = pDst
;   W3 = blockSize
;
; Preconditions:
;   - blockSize must be in the range 1..512.
;   - blockSize = 0 is invalid and must be rejected by the caller.
;   - pSrc and pDst must point to valid contiguous float32 buffers of
;     at least blockSize samples.
;
; Instance layout intentionally follows the working library disassembly:
;   [W0 + 0] = numStages, uint8_t
;   [W0 + 4] = coefficient pointer
;   [W0 + 8] = state pointer
;
; Coefficients per stage:
;   b0, b1, b2, a1, a2
;
; State per stage:
;   d1, d2
;
; Difference equation, same as CMSIS DF2T style:
;   y  = b0*x + d1
;   d1 = b1*x + d2 + a1*y
;   d2 = b2*x      + a2*y
;
; Loop counter note:
;   - DTB decrements Wn[31:0] and branches while the result is non-zero.
;   - With DTB placed at the end of the loop, an initial value of N
;     executes the loop N times and exits with the counter register = 0.
;   - This function uses DTB for both the sample loop and the stage loop.
;   - This variant computes both y = b0*x + d1 and
;     d1_partial = b1*x + old d2 using MAC form for timing test.
;   - Do not call this function with blockSize = 0. DTB would wrap the
;     counter and run for a very large number of iterations.
;   - numStages = 0 is also invalid and must not be used by instance setup.
;
; Validation notes:
;   - Compare against the original CMSIS/MCHP function using a fixed self-test.
;   - MAC-form calculations change rounding order slightly; tiny differences
;     are expected and acceptable if they remain in the self-test noise floor.
;   - Validate this d1-MAC variant against the fixed self-test before use.
;   - Keep this kernel separate from the library implementation so it can be
;     reverted or swapped without affecting the reference path.
;   - Measured speed must be checked on target hardware, because assembler or
;     FPU hazard handling may insert hidden stalls.
;==============================================================================

;------------------------------------------------------------------------------
; Include Microchip/CMSIS DSP assembly helper macros.
; The original library source uses common.inc. In particular, floatsetup appears
; to be provided as a macro, not as a raw assembler mnemonic.
;------------------------------------------------------------------------------
        .include "dspcommon.inc"

        .text
        .global _biquad_cascade_df2T_f32_dspic33ak_opt_v1

_biquad_cascade_df2T_f32_dspic33ak_opt_v1:

;------------------------------------------------------------------------------
; Save FPU registers used by this function and FCR.
; Original library saved F8. This variant does not use F9.
;------------------------------------------------------------------------------
        push.l      F8
        push.l      FCR

;------------------------------------------------------------------------------
; Mask all FPU exceptions, set rounding mode to default, clear SAZ/FTZ.
; This matches the behavior seen in the original library disassembly.
;------------------------------------------------------------------------------
        mov.l       #0x7F, W7
        floatsetup  W7

;------------------------------------------------------------------------------
; Prepare cascade pointers.
;------------------------------------------------------------------------------
        clr         W4                ; clear upper bits before loading uint8_t numStages
        mov.b       [W0+0], W4        ; W4 = number of stages
        mov.l       [W0+4], W5        ; W5 = coefficient pointer
        mov.l       [W0+8], W6        ; W6 = state pointer

;------------------------------------------------------------------------------
; Save original pDst and blockSize.
; These are restored after each stage, exactly like the original implementation.
;------------------------------------------------------------------------------
        mov.l       W2, [W15++]       ; saved original pDst
        mov.l       W3, [W15++]       ; saved original blockSize

;==============================================================================
; Stage loop
;==============================================================================
L_startFilter_v1:

;------------------------------------------------------------------------------
; Load coefficients for this stage.
;------------------------------------------------------------------------------
        mov.l       [W5++], F0        ; b0
        mov.l       [W5++], F1        ; b1
        mov.l       [W5++], F2        ; b2
        mov.l       [W5++], F3        ; a1
        mov.l       [W5++], F4        ; a2

;------------------------------------------------------------------------------
; Load state for this stage.
;------------------------------------------------------------------------------
        mov.l       [W6],   F5        ; d1
        mov.l       [W6+4], F6        ; d2

;==============================================================================
; Sample loop
;
; Scheduling intent:
;   - Use MOV.s for FPU-register copy. MOV.l F5,F8 is not accepted
;     by this assembler for FPU-to-FPU transfer.
;   - Compute y = b0*x + old d1 using MAC form.
;   - Compute d1_partial = b1*x + old d2 using MAC form.
;   - This removes the F9 temporary used by the previous y-MAC variant.
;   - Store y before the final d1/d2 updates, because the output does not
;     depend on the new state values.
;
; Register use inside loop:
;   F7 = x
;   F8 = y accumulator / y
;   F5 = d1 accumulator / d1
;   F6 = d2 accumulator / d2
;==============================================================================
L_startSections_v1:
        mov.l       [W1++], F7        ; x = *pSrc++

        mov.s       F5, F8            ; F8 = old d1, used as y accumulator
        mov.s       F6, F5            ; F5 = old d2, used as d1 accumulator

        mac.s       F0, F7, F8        ; y  = old d1 + b0*x
        mac.s       F1, F7, F5        ; d1 = old d2 + b1*x

        mul.s       F2, F7, F6        ; d2 = b2*x, old d2 already copied to F5

        mov.l       F8, [W2++]        ; *pDst++ = y

        mac.s       F3, F8, F5        ; d1 += a1*y
        mac.s       F4, F8, F6        ; d2 += a2*y

; blockSize is guaranteed by the caller to be 1..512.
; DTB executes this loop exactly blockSize times and leaves W3 = 0.
        dtb         W3, L_startSections_v1

;------------------------------------------------------------------------------
; Restore blockSize for next stage.
;------------------------------------------------------------------------------
        mov.l       [W15-4], W3

;------------------------------------------------------------------------------
; Store updated state variables.
;------------------------------------------------------------------------------
        mov.l       F5, [W6++]        ; pState[0] = d1
        mov.l       F6, [W6++]        ; pState[1] = d2

;------------------------------------------------------------------------------
; For the next stage, the previous output buffer is the input buffer.
; Reset both pSrc and pDst to original pDst.
;------------------------------------------------------------------------------
        mov.l       [W15-8], W1       ; pIn  = original pDst
        mov.l       [W15-8], W2       ; pOut = original pDst

; numStages is loaded as uint8_t after clearing W4, so W4[31:0] holds
; a valid positive stage count. DTB executes this loop exactly numStages
; times and leaves W4 = 0. numStages = 0 is invalid by design.
        dtb         W4, L_startFilter_v1

;==============================================================================
; Function epilogue
;==============================================================================
L_completedIIR_v1:
        sub.l       #8, W15           ; discard saved pDst and blockSize

        pop.l       FCR
        pop.l       F8

        return

;==============================================================================
; End of file
;==============================================================================
