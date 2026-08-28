;*****************************************************************************
;                                                                            *
;                       Software License Agreement                           *
;*****************************************************************************
;*****************************************************************************
; © [2026] Microchip Technology Inc. and its subsidiaries.                    *
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

    .include		"dspcommon.inc"       
    .list

    .equ    firPStateStart_f32_ex, 12

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_fir_f32_ex_opt_v5: Single precision floating-point FIR block filtering.
;
; Operation:
;    y[n] = sum_(m=0:M-1){h[m]*x[n-m]}, 0 <= n < N.
;    x[n] defined for 0 <= n < N,
;    y[n] defined for 0 <= n < N,
;    h[m] defined for 0 <= m < M as an increasing circular buffer,
;    NOTE: delay defined for 0 <= m < M as an increasing circular buffer.
;
; Input:
;    w0 = filter structure (mchp_fir_instance_f32, h)
;    w1 = ptr to input samples (pSrc, x)
;    w2 = ptr to output samples (pDst, y)
;    w3 = number of samples to generate (blockSize, N)
;    
;
; Return:
;    (void)
;
; System resources usage:
;    {w0..w7}    used, not restored
;    {f0..f3}    used, not restored
;     FCR        saved, used, restored
;     MODCON     saved, used, restored
;     XMODSRT    saved, used, restored
;     XMODEND    saved, used, restored
;     YMODSRT    saved, used, restored
;     YMODEND    saved, used, restored
;
; Note: In order to make use of Y-modulo addressing, this requires delay samples to be placed in Y-Data space aligned to M*4.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
        .global    _mchp_fir_f32_ex_opt_v5    ; export
_mchp_fir_f32_ex_opt_v5:
;
;
;
;
; opt_v5 changes from mchp_fir_f32_ex_opt_v4:
;   - Keep opt_v1/v2 changes.
;   - Experimental 4-tap unroll for the inner MAC loop.
;   - Assumes numTaps is a multiple of 4 and >= 8.
;   - Intended first for MA8/16/32/64 timing experiments.
;
;............................................................................

    ; Save working registers.
    push.l    w8         ; {w8 } to TOS
;............................................................................

    push.l    FCR         ; save FCR
    floatsetup    w8      ; setup FCR for default rounding mode, disabled SAZ/FTZ, with all exceptions masked.

;............................................................................

    ; Prepare core registers for modulo addressing.
    push.l    MODCON
    push.l    XMODSRT
    push.l    XMODEND
    push.l    YMODSRT
    push.l    YMODEND

;............................................................................

    ; Setup registers for modulo addressing.
    mov.l    #0xC076,w7               ; XWM = w6, YWM = w7
                                      ; set XMODEND and YMODEND bits
    mov.l    w7,MODCON                ; enable X,Y modulo addressing
    

    mov.l    [w0+firNumTaps_f32],w7
    sl.l     w7, #2, w5                ; w5 = numCoeffs*sizeof(coeffs)
    sub.l    #1, w5                    ; w5 = numCoeffs*sizeof(coeffs)-1
    
    
    mov.l    [w0+firPCoeffs_f32],w6       ; w6 -> h[0]
    mov.l    w6,XMODSRT               ; init'ed to coeffs base address
    add.l    w6,w5,w4
    mov.l    w4,XMODEND               ; init'ed to coeffs end address
    
    
    mov.l    [w0+firPStateStart_f32_ex],w7 ; w7 -> d[0]
    mov.l    w7,YMODSRT               ; init'ed to delay base address
    add.l    w7,w5,w4                 ; w7-> last byte of d[M-1]
    mov.l    w4,YMODEND               ; init'ed to delay end address
    
    

;_START:
;............................................................................

    ; Prepare to filter.
    mov.l    [w0+firPState_f32],w7            ; w7 points at current delay
                                           ; sample d[m], 0 <= m < M
    mov.l    [w0+firNumTaps_f32],w4        ; w4 = M
    sub.l    #2, w4                        ; w4 = M-2
    lsr.l    w4, #2, w4                    ; w4 = (M-2)/4, multiple-of-4 tap assumption
    mov.l    w4,w8                         ; w8 = fixed quad-loop reload value

    ; Perform filtering of all samples.
; {                                        ; do (N-1)+1 times
startFilter:
    mov.l    [w1++],[w7]                    ; store new sample into delay
                                            ; w7 = d[init]
    
    mov.l     [w6++], f0                    ; f0 = h[0]
    mov.l     [w7], f1                      ; f1 = delay[init]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mul.s     f0, f1, f2                    ; f2 = h[0]*d[current]

; do ((M-2)/4) times, four middle taps per loop
start_multiply:
    ; Middle tap 0 in this group.
    mov.l     [w6++], f0                    ; f0 = h[m]
    mov.l     [w7], f1                      ; f1 = delay[m]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mac.s     f0, f1, f2                    ; f2 += h[m]*d[current]

    ; Middle tap 1 in this group.
    mov.l     [w6++], f0                    ; f0 = h[m+1]
    mov.l     [w7], f1                      ; f1 = delay[m+1]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mac.s     f0, f1, f2                    ; f2 += h[m+1]*d[current]

    ; Middle tap 2 in this group.
    mov.l     [w6++], f0                    ; f0 = h[m+2]
    mov.l     [w7], f1                      ; f1 = delay[m+2]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mac.s     f0, f1, f2                    ; f2 += h[m+2]*d[current]

    ; Middle tap 3 in this group.
    mov.l     [w6++], f0                    ; f0 = h[m+3]
    mov.l     [w7], f1                      ; f1 = delay[m+3]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mac.s     f0, f1, f2                    ; f2 += h[m+3]*d[current]

    dtb       w4, start_multiply
; }

    ; (Perform  last MAC.)
    mov.l     [w6++], f0                    ; f0 = h{M-1]
    mov.l     [w7], f1                      ; f1 = d[current-1]
    ;NOP                                    ; stall cycle.
    mac.s     f0, f1, f2                    ; f2 += h[M-1]*d[current-1]
    mov.l     w8, w4                        ; restore w4 = (M-2)/4
    ;NOP                                    ; stall cycle.
    ;NOP                                    ; stall cycle.
    ; Save filtered result.
    mov.l      f2, [w2++]                   ; y[n] = sum_{m=0:M-1}(h[m]*x[n-m])
                                            ; w2-> y[n+1]
    dtb     w3, startFilter
; }
;............................................................................

    ; Update delay pointer.
    mov.l    w7,[w0+firPState_f32]     ; note that the delay pointer
                                    ; may wrap several times around
                                    ; d[m], 0 <= m < M, depending
                                    ; on the value of N

;............................................................................
_completedFIR:

;............................................................................

    ; Restore core registers for modulo addressing.
    pop.l    YMODEND
    pop.l    YMODSRT
    pop.l    XMODEND
    pop.l    XMODSRT
    pop.l    MODCON

;............................................................................
    ; Restore FCR.
    pop.l    FCR
    
;............................................................................

    ; Restore working registers.

    pop.l    w8                 ; {w8 } from TOS
    
;............................................................................
    return    

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
    