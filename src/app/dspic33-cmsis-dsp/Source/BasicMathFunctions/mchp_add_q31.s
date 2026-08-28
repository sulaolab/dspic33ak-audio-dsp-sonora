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
    .include    "dspcommon.inc"      ; fractsetup macro
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .cmsisdspmchp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_add_q31: Fixed-point (Q31) vector addition.
;
; Description:
;    Element-by-element addition of two Q31 source vectors, storing the
;    saturated result into a destination vector.
;
; Operation:
;    pDst[n] = pSrcA[n] + pSrcB[n], with
;
;    n in {0, 1, ... , blockSize-1}
;
; Input:
;    w0 = ptr to source one vector (pSrcA)
;    w1 = ptr to source two vector (pSrcB)
;    w2 = ptr to destination vector (pDst)
;    w3 = number of elements in vector(s) (blockSize)
;
; Return:
;    no return value
;
; System resources usage:
;    {w0..w4}    used, not restored
;     AccuA      used, not restored
;     CORCON     saved, used, restored
;
;............................................................................

    .global    _mchp_add_q31        ; export

_mchp_add_q31:
    push.l     CORCON               ; Save 32-bit CORCON (will be restored on exit).
    fractsetup w4                   ; Setup CORCON for fractional/saturating
                                    ; arithmetic; w4 used as scratch by macro.
;............................................................................

    cp0.l      w3                   ; blockSize == 0 ? (32-bit compare)
    bra        z, _add_q31_exit     ; Yes => nothing to do, exit.

    mov.l      w2, w4              ; w4 = running destination pointer (pDst).
                                    ; w0 = running source A pointer (pSrcA).
                                    ; w1 = running source B pointer (pSrcB).
                                    ; w3 = loop counter (blockSize).

;............................................................................
; Loop: process one Q31 element per iteration.
;............................................................................

v_add_start:
    lac.l      [w0++], a           ; Load 32-bit pSrcA[n] into Accumulator A.
                                    ; Post-increment pSrcA pointer.
    add.l      [w1++], a           ; Add 32-bit pSrcB[n] to Accumulator A (saturated).
                                    ; Post-increment pSrcB pointer.
    sac.l      a, [w4++]           ; Store 32-bit saturated result to pDst[n].
                                    ; Post-increment pDst pointer.

    DTB        w3, v_add_start     ; Decrement 32-bit blockSize counter (w3);
                                    ; branch to v_add_start if not zero.

;............................................................................
; Exit: restore saved registers and return.
;............................................................................

_add_q31_exit:
    pop.l      CORCON              ; Restore 32-bit CORCON to pre-call state.
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; EOF														 
	 
