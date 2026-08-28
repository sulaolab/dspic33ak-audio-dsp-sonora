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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_fir_f32: Single precision floating-point FIR block filtering.
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
    .extern    _firPStateStart_f32
    .global    _mchp_fir_f32    ; export
_mchp_fir_f32:
;............................................................................

    ; Save working registers.
    push.l    w8         ; {w8 } to TOS
    push.l    w9         ; {w9 } to TOS
    push.l    w10        ; {w10} to TOS
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
    sl.l     w7, #2, w9                ; w9 = numCoeffs*sizeof(coeffs)
    sub.l    #1, w9                    ; w9 =  numCoeffs*sizeof(coeffs)-1
    
    
    mov.l    [w0+firPCoeffs_f32],w6       ; w6 -> h[0]
    mov.l    w6,XMODSRT               ; init'ed to coeffs base address
    add.l    w6,w9,w10
    mov.l    w10,XMODEND               ; init'ed to coeffs end address
    
    
    mov.l    _firPStateStart_f32,w7      ; w7 -> d[0]
    mov.l    w7,YMODSRT               ; init'ed to delay base address
    add.l    w7,w9,w10                 ; w7-> last byte of d[M-1]
    mov.l    w10,YMODEND               ; init'ed to delay end address
    
    

;............................................................................
    push.l    w2                    ; save return value (y)
;............................................................................

;_START:
;............................................................................

    ; Prepare to filter.
    mov.l    [w0+firPState_f32],w7            ; w7 points at current delay
                                           ; sample d[m], 0 <= m < M
    mov.l    [w0+firNumTaps_f32],w4        ; w4 = M
    sub.l    #2, w4                        ; w4 = M-2

    push.l    w4

    ; Perform filtering of all samples.
; {                                        ; do (N-1)+1 times
startFilter:
    mov.l    [w1++],[w7]                    ; store new sample into delay
                                            ; w7 = d[init]
    
    mov.l     [w6++], f0                    ; f0 = h[0]
    mov.l     [w7], f1                      ; f1 = delay[init]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mul.s     f0, f1, f2                    ; f2 = h[0]*d[current]

; do (M-2) times
start_multiply:
    ; Filter each sample. (Perform all but last MAC.)
    mov.l     [w6++], f0                    ; f0 = h[m]
    mov.l     [w7], f1                      ; f1 = delay[m]
    mpy.l     w3, [w7]+=4, a                ; A dummy DSP op to use Y Modulo addressing
    mac.s     f0, f1, f2                    ; f2 += h[m]*d[current]
    dtb       w4, start_multiply
; }

    ; (Perform  last MAC.)
    mov.l     [w6++], f0                    ; f0 = h{M-1]
    mov.l     [w7], f1                      ; f1 = d[current-1]
    ;NOP                                    ; stall cycle.
    mac.s     f0, f1, f2                    ; f2 += h[M-1]*d[current-1]
    mov.l     [w15-4], w4                   ; restore w4 = M-2
    ;NOP                                    ; stall cycle.
    ;NOP                                    ; stall cycle.
    ; Save filtered result.
    mov.l      f2, [w2++]                   ; y[n] = sum_{m=0:M-1}(h[m]*x[n-m])
                                            ; w2-> y[n+1]
    dtb     w3, startFilter
; }
    pop.l     w4
;............................................................................

    ; Update delay pointer.
    mov.l    w7,[w0+firPState_f32]     ; note that the delay pointer
                                    ; may wrap several times around
                                    ; d[m], 0 <= m < M, depending
                                    ; on the value of N

;............................................................................
_completedFIR:
    pop.l    w0                ; restore return value

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

    pop.l    w10                ; {w10} from TOS
    pop.l    w9                 ; {w9 } from TOS
    pop.l    w8                 ; {w8 } from TOS
    
;............................................................................
    return    

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
    