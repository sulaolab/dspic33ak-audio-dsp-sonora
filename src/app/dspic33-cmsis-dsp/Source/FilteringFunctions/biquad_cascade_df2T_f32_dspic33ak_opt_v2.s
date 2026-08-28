;==============================================================================
; biquad_cascade_df2T_f32_dspic33ak_opt_v2.s
;
; Experimental dsPIC33AK FPU optimized DF2T biquad cascade
;
; Purpose:
;   - Keep the original CMSIS/Microchip function unchanged.
;   - Add a separate test function with the same C-call interface.
;   - Reduce explicit NOP/NEOP slots in the inner sample loop by reordering
;     independent FPU operations.
;
; C prototype:
;
;   extern void biquad_cascade_df2T_f32_dspic33ak_opt_v2(
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
; Notes:
;   - This is v2 for speed testing.
;   - It removes explicit NOP/NEOP from the inner loop.
;   - v2 moves the output store between the a1 and a2 MAC operations.
;   - If the FPU hazard checker inserts stalls internally, the measured gain may
;     be smaller than the assembly listing suggests.
;   - Compare output against the original function before trusting audio results.
;==============================================================================

;------------------------------------------------------------------------------
; Include Microchip/CMSIS DSP assembly helper macros.
; The original library source uses common.inc. In particular, floatsetup appears
; to be provided as a macro, not as a raw assembler mnemonic.
;------------------------------------------------------------------------------
        .include "dspcommon.inc"

        .text
        .global _biquad_cascade_df2T_f32_dspic33ak_opt_v2

_biquad_cascade_df2T_f32_dspic33ak_opt_v2:

;------------------------------------------------------------------------------
; Save FPU registers used by this function and FCR.
; Original library saved F8. This version additionally uses F9.
;------------------------------------------------------------------------------
        push.l      F8
        push.l      F9
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
L_startFilter_v2:

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
; Sample loop, optimized scheduling attempt
;
; Original inner-loop shape had explicit NOP/NEOP after several FPU ops.
; This version computes b0*x and b1*x first, then consumes them later.
;
; Register use inside loop:
;   F7 = x
;   F8 = y
;   F9 = temporary b1*x
;   F5 = d1
;   F6 = d2
;==============================================================================
L_startSections_v2:
        mov.l       [W1++], F7        ; x = *pSrc++

        mul.s       F0, F7, F8        ; F8 = b0*x
        mul.s       F1, F7, F9        ; F9 = b1*x, independent of F8 result

        add.s       F8, F5, F8        ; y = b0*x + old d1
        add.s       F9, F6, F5        ; d1 = b1*x + old d2

        mul.s       F2, F7, F6        ; d2 = b2*x, old d2 no longer needed

        mac.s       F3, F8, F5        ; d1 += a1*y
        mov.l       F8, [W2++]        ; *pDst++ = y
        mac.s       F4, F8, F6        ; d2 += a2*y

        dtb         W3, L_startSections_v2

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

        dtb         W4, L_startFilter_v2

;==============================================================================
; Function epilogue
;==============================================================================
L_completedIIR_v2:
        sub.l       #8, W15           ; discard saved pDst and blockSize

        pop.l       FCR
        pop.l       F9
        pop.l       F8

        return

;==============================================================================
; End of file
;==============================================================================
