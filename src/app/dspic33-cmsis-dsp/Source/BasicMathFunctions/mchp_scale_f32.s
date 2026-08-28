;*****************************************************************************
;                                                                            *
;                       Software License Agreement                           *
;*****************************************************************************
;*****************************************************************************
;© [2026] Microchip Technology Inc. and its subsidiaries.                    *
;                                                                            *
;   Subject to your compliance with these terms, you may use Microchip       *
;   software and any derivatives exclusively with Microchip products.        *
;    You are responsible for complying with 3rd party license terms          *
;    applicable to your use of 3rd party software (including open source     *
;    software) that may accompany Microchip software. SOFTWARE IS ?AS IS.?   *
;    NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS     *
;    SOFTWARE, INCLUDING ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT,         *
;    MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT       *
;    WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,           *
;    INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY        *
;    KIND WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF        *
;    MICROCHIP HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE        *
;    FORESEEABLE. TO THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP?S          *
;    TOTAL LIABILITY ON ALL CLAIMS RELATED TO THE SOFTWARE WILL NOT          *
;    EXCEED AMOUNT OF FEES, IF ANY, YOU PAID DIRECTLY TO MICROCHIP FOR       *
;   THIS SOFTWARE.                                                           *
;*****************************************************************************

    ; Local inclusions.
        
    .nolist
    .include    "dspcommon.inc"    ; floatsetup
         
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; mchp_scale_f32: Single-precision floating-point vector scaling.
;
; Operation:
;    pDst[n] = scale * pSrc[n]
;    for n = 0 to (blockSize - 1)
;
;
; Input (Register Usage):
;    w0 = pSrc        - Pointer to input vector
;    f0 = scale       - Scale value
;    w1 = pDst        - Pointer to output vector
;    w2 = blockSize   - Number of elements
;
; Return:
;    w0 = pDst (for consistency with dsPIC DSP conventions)
;
; System Resources Used:
;    {w0..w5}  Used, not restored
;    {f0..f2}  Used, not restored
;     FCR      Saved, used, restored
;
; Notes:
;    - Implementation is pipelined: load next input while storing previous output.
;    - FCR is pushed and restored to ensure deterministic FP behavior.
;    - Supports unaligned memory access for both pSrc and pDst.
;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;


    .global _mchp_scale_f32
_mchp_scale_f32:

;............................................................................
    ; Save FCR and setup FPU
    push.l  fcr
    floatsetup w4
;............................................................................

    ; if blockSize == 0 ? just return pDst
    cp0.l   w2
    bra     z, _scale_exit

    mov.l   w1, w4              ; w4 = pDst working pointer

v_scale_loop:
    mov.l   [w0++], f1          ; f1 = pSrc[n]
    mul.s   f0, f1, f2          ; f2 = scale * f1
    mov.l   f2, [w4++]          ; pDst[n] = f2
    DTB     w2, v_scale_loop
;............................................................................

_scale_exit:
    mov.l   w1, w0              ; return pDst in w0
    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
