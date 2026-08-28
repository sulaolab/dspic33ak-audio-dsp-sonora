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
;    software) that may accompany Microchip software. SOFTWARE IS "AS IS."   *
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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; Local inclusions.

    .nolist
    .include    "dspcommon.inc"      ; fractsetup macro
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .cmsisdspmchp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_scale_q31: Fixed-point (Q31) vector scaling with post-shift.
;
; Description:
;    Multiplies every element of a Q31 source vector by a Q31 scale value,
;    applies a post-shift, and stores the saturated result into a
;    destination vector.  Matches the ARM CMSIS arm_scale_q31 semantics:
;
;      pDst[n] = (pSrc[n] * scaleFract) >> (32 - shift)
;
;    The dsPIC fractional multiply (mpy.l) computes (a * b) << 1 into
;    the accumulator.  To match the ARM formula, the accumulator is
;    then shifted right by (1 - shift) additional bits via sftac before
;    sacr.l extracts the upper 32 bits.
;
;    dsPIC33AK does NOT have the REPEAT instruction; DTB is used for
;    all loop control.
;
; Input:
;    w0 = ptr to input vector (pSrc)
;    w1 = scale value (Q31 format, scaleFract)
;    w2 = shift amount (int8_t, sign-extended to 32-bit)
;    w3 = ptr to output vector (pDst)
;    w4 = number of elements in vector (blockSize)
;
; Return:
;    no return value (void)
;
; System resources usage:
;    {w0..w5}    used, not restored
;     AccuA      used, not restored
;     CORCON     saved, used, restored
;
; Notes:
;    - pSrc and pDst may point to the same buffer (in-place scaling).
;    - shift = 1 gives the same result as a plain fractional multiply.
;    - shift = 0 divides the fractional result by 2.
;    - Positive shift values increase gain; negative values decrease.
;
;............................................................................

    .global    _mchp_scale_q31        ; export

_mchp_scale_q31:

;............................................................................
; Prepare CORCON for fractional computation.
;............................................................................

    push.l     CORCON                 ; Save 32-bit CORCON.
    fractsetup w5                     ; Setup CORCON for fractional/saturating
                                      ; arithmetic; w5 used as scratch by macro.

;............................................................................
; Compute sftac shift amount:  w2 = 1 - shift.
;
;   mpy.l in fractional mode computes (a * b) << 1.
;   ARM wants (a * b) >> (32 - shift).
;   sacr.l extracts upper 32 bits, i.e. >> 32 from accumulator.
;   So after mpy.l we have (a*b)<<1 in Acc, and sacr.l gives
;   ((a*b)<<1) >> 32 = (a*b) >> 31.
;   ARM wants (a*b) >> (32 - shift) = (a*b) >> 31 >> (1-shift).
;   So we need sftac a, #(1-shift) (positive = right shift).
;............................................................................

    mov.l      #1, w5                ; w5 = 1.
    sub.l      w5, w2, w2            ; w2 = 1 - shift (sftac shift amount).

;............................................................................
; Early exit check.
;............................................................................

    cp0.l      w4                     ; blockSize == 0 ? (32-bit compare)
    bra        z, _scale_exit         ; Yes => nothing to do, exit.

;............................................................................
; Simple scale loop: one element per iteration.
;
;   For each element:
;     1. mpy.l w1, [w0]+=4, a   -- fractional multiply: a = scaleFract * pSrc[n] << 1.
;     2. sftac a, w2             -- shift accumulator right by (1 - shift).
;     3. sacr.l a, [w3++]       -- store saturated Q31 result to pDst[n].
;............................................................................

_scale_loop:
    mpy.l      w1, [w0]+=4, a        ; a = scaleFract * pSrc[n] (fractional, <<1).
    sftac      a, w2                  ; a >>= (1 - shift)  [positive w2 = right shift].
    sacr.l     a, [w3++]             ; pDst[n] = saturated Q31 result.

    DTB        w4, _scale_loop       ; Decrement blockSize (w4);
                                      ; branch to _scale_loop if not zero.

;............................................................................
; Exit: restore saved registers and return.
;............................................................................

_scale_exit:
    pop.l      CORCON                 ; Restore 32-bit CORCON.

    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; EOF
