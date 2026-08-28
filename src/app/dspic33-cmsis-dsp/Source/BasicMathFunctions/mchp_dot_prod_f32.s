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
    .include    "dspcommon.inc" ; floatsetup
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_dot_prod_f32: Single precision floating-point Vector Dot Product.
;
; Operation:
;    *result = sum (pSrcA[n] * pSrcB[n]), with
;
; n in {0, 1,... , blockSize-1}
;
; Input:
;    w0 = ptr to source one vector (pSrcA)
;    w1 = ptr to source two vector (pSrcB)
;    w2 = number elements in vector(s) (blockSize)
;    w3 = ptr to result (float32_t *result)
;
; Return:
;    none (result stored at *result)
;
; System resources usage:
;    {w0..w3}    used, not restored
;    {f0..f2}    used, not restored
;     FCR        saved, used, restored
;............................................................................

    .global    _mchp_dot_prod_f32
_mchp_dot_prod_f32:

;............................................................................
    ; Save FCR and setup FPU
    push.l    fcr
    floatsetup w4
;............................................................................

    ; If blockSize == 0 ? result = 0.0
    mov.l  w3, w4
    cp0.l   w2
    bra     z, _dot_store_zero

    ; accumulator = 0.0
    movc.s  #22, f0              ; f0 = 0.0

v_dot_loop:
    mov.l   [w0++], f1           ; f1 = pSrcA[n]
    mov.l   [w1++], f2           ; f2 = pSrcB[n]
    mac.s   f1, f2, f0           ; f0 += f1 * f2
    DTB     w2, v_dot_loop
;............................................................................

    ; store accumulated result
    mov.l   f0, [w4]
    bra     _dot_exit

_dot_store_zero:
    movc.s  #22, f0              ; f0 = 0.0
    mov.l   f0, [w4]

_dot_exit:
    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
