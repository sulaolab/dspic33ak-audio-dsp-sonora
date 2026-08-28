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
; _mchp_mult_q31: Fixed-point (Q31) vector element-wise multiplication.
;
; Description:
;    Element-by-element multiplication of two Q31 source vectors, storing
;    the saturated result into a destination vector.
;
;    Uses an unrolled-by-2 loop with AccuA and AccuB for pipelining,
;    matching the original _VectorMultiply implementation for maximum
;    DSP engine throughput. Handles both even and odd block sizes.
;
; Operation:
;    pDst[n] = pSrcA[n] * pSrcB[n], with
;
;    n in {0, 1, ... , blockSize-1}
;
; Input:
;    w0 = ptr to first source vector (pSrcA)
;    w1 = ptr to second source vector (pSrcB)
;    w2 = ptr to destination vector (pDst)
;    w3 = number of elements in vector(s) (blockSize)
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
;............................................................................

    .global    _mchp_mult_q31        ; export

_mchp_mult_q31:

;............................................................................
; Save working registers.
;............................................................................

    push.l     w13                    ; Save w13 (used for accumulator writeback).

;............................................................................
; Prepare CORCON for fractional computation.
;............................................................................

    push.l     CORCON                 ; Save 32-bit CORCON.
    fractsetup w4                     ; Setup CORCON for fractional/saturating
                                      ; arithmetic; w4 used as scratch by macro.

;............................................................................
; Setup destination pointer for accumulator store-back.
;   w0  = running pSrcA pointer
;   w1  = running pSrcB pointer
;   w13 = running pDst pointer (used for accumulator writeback)
;   w3  = loop counter (blockSize)
;............................................................................

    mov.l      w2, w13               ; w13 = destination pointer (pDst).
                                      ; w0 = pSrcA, w1 = pSrcB (already set).

;............................................................................
; Early exit check.
;............................................................................

    cp0.l      w3                     ; blockSize == 0 ? (32-bit compare)
    bra        z, _mult_exit          ; Yes => nothing to do, exit.

;............................................................................
; Unrolled-by-2 multiply loop.
;
;   The loop processes two elements per iteration using AccuA and AccuB
;   in a pipelined fashion. The first multiply is pre-loaded into AccuB,
;   then each iteration computes the next multiply into AccuA/AccuB while
;   storing the previous result.
;
;   This matches the original vmul_aa.s _VectorMultiply implementation.
;
;   Odd element count:
;     - If blockSize is odd, the last element is handled separately
;       after the main loop exits.
;
;   Even element count:
;     - The loop handles all element pairs; the final store is done
;       after the loop.
;............................................................................

    sub.l      #1, w3                ; w3 = blockSize - 1 (pre-check).
    bra        leu, one_left          ; If blockSize was 0 or 1, handle tail.

    lsr.l      w3, w3                ; w3 = (blockSize-1) / 2 = pairs remaining.
                                      ; Carry flag = odd indicator.

    ; Pre-load first multiply into AccuB.
    mpy.l      [w0]+=4, [w1]+=4, b  ; b = pSrcA[0] * pSrcB[0].
                                      ; Post-increment both source pointers.

    cp0.l      w3                     ; Only one pair left?
    bra        z, last_pair           ; Yes => skip main loop, store final.

;............................................................................
; Main unrolled loop: process two elements per iteration.
;............................................................................

v_mult_start:
    ; Multiply next element into AccuA, store previous AccuB result.
    mpy.l      [w0]+=4, [w1]+=4, a, [w13++]
                                      ; a = pSrcA[n] * pSrcB[n].
                                      ; Store previous b -> pDst[n-1].
                                      ; Post-increment all pointers.
    NOP                               ; Pipeline stall (required by DSP engine).

    ; Multiply next element into AccuB, store previous AccuA result.
    mpy.l      [w0]+=4, [w1]+=4, b, [w13++]
                                      ; b = pSrcA[n+1] * pSrcB[n+1].
                                      ; Store previous a -> pDst[n].
                                      ; Post-increment all pointers.

    DTB        w3, v_mult_start      ; Decrement pair counter (w3);
                                      ; branch to v_mult_start if not zero.

;............................................................................
; Store last computed result from AccuB.
;............................................................................

last_pair:
    sacr.l     b, [w13++]            ; Store final b -> pDst[n].

;............................................................................
; Handle odd trailing element (if blockSize was odd).
;............................................................................

one_left:
    bra        NC, _mult_exit         ; If original count was even (or 0), skip.
                                      ; (Carry flag set by lsr.l indicates odd.)

    ; Compute final element: a = pSrcA[last] * pSrcB[last].
    mpy.l      [w0], [w1], a         ; a = pSrcA[last] * pSrcB[last].
    sacr.l     a, [w13]              ; Store final result -> pDst[last].

;............................................................................
; Exit: restore saved registers and return.
;............................................................................

_mult_exit:
    pop.l      CORCON                 ; Restore 32-bit CORCON.
    pop.l      w13                    ; Restore w13.

    return

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; EOF
