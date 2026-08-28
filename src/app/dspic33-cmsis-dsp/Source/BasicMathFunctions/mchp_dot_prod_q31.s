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
;    FORESEEABLE. TO THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIPS           *
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
; _mchp_dot_prod_q31: Fixed-point (Q31) vector dot product.
;
; Description:
;    Computes the dot product of two Q31 source vectors, storing the
;    saturated result at the location pointed to by the result pointer.
;
; Operation:
;    *result = sum( pSrcA[n] * pSrcB[n] ), with
;
;    n in {0, 1, ... , blockSize-1}
;
; Input:
;    w0 = ptr to source one vector (pSrcA)
;    w1 = ptr to source two vector (pSrcB)
;    w2 = number of elements in vector(s) (blockSize)
;    w3 = ptr to result (q63_t *result, 64-bit little-endian)
;
; Return:
;    none (result stored at *result as q63_t)
;
; System resources usage:
;    {w0..w5}    used, not restored
;     AccuA      used, not restored
;     CORCON     saved, used, restored
;
;............................................................................

    .global    _mchp_dot_prod_q31        ; export

_mchp_dot_prod_q31:

;............................................................................
; Save CORCON and setup fractional mode.
;............................................................................

    push.l     CORCON                    ; Save 32-bit CORCON (will be restored on exit).
    fractsetup w4                        ; Setup CORCON for fractional/saturating
                                         ; arithmetic; w4 used as scratch by macro.

;............................................................................
; Save result pointer and check for zero blockSize.
;............................................................................

    mov.l      w3, w4                    ; w4 = result pointer (saved for later store).

    cp0.l      w2                        ; blockSize == 0 ? (32-bit compare)
    bra        z, _dot_store_zero        ; Yes => store zero result and exit.

;............................................................................
; Initialize accumulator and perform first multiply.
;............................................................................

    mpy.l      [w0]+=4, [w1]+=4, a      ; a  = pSrcA[0] * pSrcB[0].
                                         ; Post-increment pSrcA pointer.
                                         ; Post-increment pSrcB pointer.

    sub.l      w2, #2, w2               ; w2 = blockSize - 2 (remaining MAC iterations).
    bra        n, _dot_store             ; If blockSize == 1, skip MAC loop, store result.

    cp0.l      w2                        ; blockSize - 2 == 0 ? (only 2 elements total)
    bra        z, _dot_last_mac          ; If exactly 2 elements, do one final MAC.

;............................................................................
; Loop: multiply-accumulate remaining Q31 elements.
;............................................................................

v_dot_loop:
    mac.l      [w0]+=4, [w1]+=4, a      ; a += pSrcA[n] * pSrcB[n] (saturated).
                                         ; Post-increment pSrcA pointer.
                                         ; Post-increment pSrcB pointer.

    DTB        w2, v_dot_loop            ; Decrement 32-bit blockSize counter (w2);
                                         ; branch to v_dot_loop if not zero.

;............................................................................
; Final MAC for last element.
;............................................................................

_dot_last_mac:
    mac.l      [w0]+=4, [w1]+=4, a      ; a += pSrcA[last] * pSrcB[last] (saturated).

;............................................................................
; Store accumulated dot product result as q63_t (64-bit).
;
;   q63_t is little-endian: low 32 bits at [w4+0], high 32 bits at [w4+4].
;   Must disable SATDW before extraction to get raw accumulator bits.
;   slac.l extracts accumulator bits[31:0] (lower 32 bits).
;   sac.l  extracts accumulator bits[63:32] (upper 32 bits, no saturation).
;............................................................................

_dot_store:
    bclr       CORCON, #5               ; Clear SATDW_ON to disable write saturation.
    slac.l     a, w3                    ; w3 = AccA bits[31:0] (lower word).
    sac.l      a, w5                    ; w5 = AccA bits[63:32] (upper word, unsaturated).
    mov.l      w3, [w4]                 ; Store low 32 bits at result+0.
    mov.l      w5, [w4+4]              ; Store high 32 bits at result+4.
    bra        _dot_exit                 ; Branch to exit.

;............................................................................
; Zero result: blockSize was 0, store zero q63_t to *result.
;............................................................................

_dot_store_zero:
    mov.l      #0, w3                   ; Zero value.
    mov.l      w3, [w4]                 ; Store 0 at result+0 (low word).
    mov.l      w3, [w4+4]              ; Store 0 at result+4 (high word).

;............................................................................
; Exit: restore saved registers and return.
;............................................................................

_dot_exit:
    pop.l      CORCON                    ; Restore 32-bit CORCON to pre-call state.
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; EOF
