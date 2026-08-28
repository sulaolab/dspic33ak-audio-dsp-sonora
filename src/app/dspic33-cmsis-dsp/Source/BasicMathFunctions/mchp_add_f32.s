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
    .include    "dspcommon.inc"  ; floatsetup
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_add_f32: Floating-point vector addition.
;
; Operation:
;    pDst[n] = pSrcA[n] + pSrcB[n], with
;
;    n in {0, 1,... , blockSize-1}
;
; Input:
;    w0 = ptr to source one vector (pSrcA)
;    w1 = ptr to source two vector (pSrcB)
;    w2 = ptr to destination vector (pDst)
;    w3 = number elements in vector(s) (blockSize)
;
; Return:
;    no return value
;
; System resources usage:
;    {w0..w4}    used, not restored
;    {f0..f2}    used, not restored
;     FCR        saved, used, restored
;............................................................................

    .global    _mchp_add_f32        ; export
_mchp_add_f32:
    push.l  fcr
    floatsetup w4                   ; Setup FCR - default rounding, mask all exceptions.
;............................................................................

    cp0.l   w3
    bra     z, _exit
    mov.l   w2, w4                  ; w4 = destination pointer

v_add_start:
    mov.l   [w0++], f0
    mov.l   [w1++], f1
    add.s   f0, f1, f2
    mov.l   f2, [w4++]
    DTB w3, v_add_start
;............................................................................
_exit:
    pop.l   fcr
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
