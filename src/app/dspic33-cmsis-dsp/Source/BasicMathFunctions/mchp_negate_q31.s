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

; Local inclusions.

    .nolist
    .include    "dspcommon.inc"      ; fractsetup macro
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .cmsisdspmchp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_negate_q31: Fixed-point (Q31) vector negation.
;
; Description:
;    Negates every element of a Q31 source vector and stores the result
;    into a destination vector.
;
;    For the special case of 0x80000000 (-1.0 in Q31), the negation
;    saturates to 0x7FFFFFFF (+0.999... in Q31).
;
;    Note: The ALU neg.l instruction does NOT obey CORCON saturation
;    bits (those only affect DSP accumulator operations). Therefore
;    this implementation uses the DSP engine fractional multiply by
;    0x80000000 (-1.0 in Q31), which correctly saturates via sacr.l.
;    An unrolled-by-2 pipeline is used for throughput.
;
; Operation:
;    pDst[n] = (-1) * pSrc[n], with
;
;    n in {0, 1, ... , blockSize-1}
;
; Input:
;    w0 = ptr to input vector (pSrc)
;    w1 = ptr to output vector (pDst)
;    w2 = number of elements in vector (blockSize)
;
; Return:
;    no return value (void)
;
; System resources usage:
;    {w0..w4}    used, not restored
;    {w13}       used, restored
;     AccuA      used, not restored
;     AccuB      used, not restored
;     CORCON     saved, used, restored
;
; Notes:
;    - Multiplying by 0x80000000 (-1.0 Q31) in fractional mode:
;      x * (-1) << 1 >> 32 = -x, with saturation for 0x80000000 * 0x80000000.
;    - pSrc and pDst may point to the same buffer (in-place negation).
;    - dsPIC33AK does NOT have REPEAT instruction; DTB is used instead.
;
;............................................................................

    .global    _mchp_negate_q31        ; export

_mchp_negate_q31:

;............................................................................
; Save working registers.
;............................................................................

    push.l     w13                    ; Save w13 (used for accumulator writeback).

;............................................................................
; Prepare CORCON for fractional computation.
;............................................................................

    push.l     CORCON                 ; Save 32-bit CORCON (will be restored on exit).
    fractsetup w3                     ; Setup CORCON for fractional/saturating
                                      ; arithmetic; w3 used as scratch by macro.

;............................................................................
; Setup registers.
;   w0  = running pSrc pointer
;   w3  = negation constant 0x80000000 (-1.0 in Q31)
;   w13 = running pDst pointer (for accumulator writeback)
;   w2  = loop counter (blockSize)
;............................................................................

    mov.l      w1, w13               ; w13 = destination pointer (pDst).
    mov.l      #0x80000000, w3       ; w3 = -1.0 in Q31 (negation multiplier).

;............................................................................
; Early exit check.
;............................................................................

    cp0.l      w2                     ; blockSize == 0 ? (32-bit compare)
    bra        z, _neg_exit           ; Yes => nothing to do, exit.

;............................................................................
; Unrolled-by-2 negation loop using DSP fractional multiply.
;
;   Each element is multiplied by 0x80000000 (-1.0) in fractional mode.
;   The DSP engine's sacr.l provides correct saturation:
;     - Normal values: x * (-1.0) = -x
;     - 0x80000000:    (-1.0) * (-1.0) = +1.0, saturates to 0x7FFFFFFF
;
;   Pipeline: pre-load into AccuB, then alternate AccuA/AccuB with
;   simultaneous store-back for maximum throughput.
;............................................................................

    mov.l      #1, w4                ; Assume trailing element needed (blockSize=1).
    sub.l      #1, w2                ; w2 = blockSize - 1.
    bra        leu, _neg_one_left    ; If blockSize was 0 or 1, handle tail.
                                      ; (w4 == 1 => process the single element.)

    mov.l      w2, w4                ; w4 = blockSize - 1 (save before shift).
    lsr.l      w2, w2                ; w2 = (blockSize-1) / 2 = pairs remaining.
    and.l      #1, w4                ; w4 = (blockSize-1) & 1.
                                      ; w4 == 0 => no trailing element needed
                                      ;            (odd blockSize: 1+2*N covers all).
                                      ; w4 == 1 => trailing element needed
                                      ;            (even blockSize: one left over).

    ; Pre-load: multiply first element into AccuB.
    mpy.l      w3, [w0]+=4, b        ; b = (-1.0) * pSrc[0].

    cp0.l      w2                     ; Only one pair remaining?
    bra        z, _neg_last_pair     ; Yes => skip main loop, store final.

;............................................................................
; Main unrolled loop: process two elements per iteration.
;............................................................................

_neg_loop:
    ; Multiply next element into AccuA, store previous AccuB result.
    mpy.l      w3, [w0]+=4, a, [w13++]

    NOP                               ; Pipeline stall between back-to-back
                                      ; mpy with writeback.

    ; Multiply next element into AccuB, store previous AccuA result.
    mpy.l      w3, [w0]+=4, b, [w13++]

    DTB        w2, _neg_loop         ; Decrement pair counter;
                                      ; branch if not zero.

;............................................................................
; Store last computed result from AccuB.
;............................................................................

_neg_last_pair:
    sacr.l     b, [w13++]            ; Store final b -> pDst[n].

;............................................................................
; Handle odd trailing element (if blockSize was odd).
;............................................................................

_neg_one_left:
    cp0.l      w4                     ; Trailing element needed? (w4 == 0 => no)
    bra        z, _neg_exit           ; No trailing element, exit.

    mulfss.l   w3, [w0], a           ; a = (-1.0) * pSrc[last].
    sacr.l     a, [w13]              ; Store final result -> pDst[last].

;............................................................................
; Exit: restore saved registers and return.
;............................................................................

_neg_exit:
    pop.l      CORCON                 ; Restore 32-bit CORCON to pre-call state.
    pop.l      w13                    ; Restore w13.
    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; EOF