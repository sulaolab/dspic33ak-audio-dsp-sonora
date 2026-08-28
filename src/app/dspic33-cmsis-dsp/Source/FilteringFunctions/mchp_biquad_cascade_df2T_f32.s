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
    .include    "dspcommon.inc"       
    .list


    .section .dspic33cmsisdsp, code

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; _mchp_biquad_cascade_df2T_f32: Cascade of second order IIR FORM II filtering.
;
; Datatype: Single precision floating-point.
; 
; Operation: cascade of S sections
;
;             (b2_s*x[n-2] + b1_s*x[n-1] + b0_s*x[n])
;    y_s[n] = ---------------------------------------
;             (a2_s*y[n-2] + a1_s*y[n-1])
;
; Input:        
;    w0 = filter structure (mchp_biquad_cascade_df2T_instance_f32, S)
;    w1 = ptr to input samples (pSrc, x)
;    w2 = ptr to output samples (pDst, y)
;    w3 = number of samples to generate (blockSize, N)
;
; System resources usage:
;    {w0..w7}   used, not restored
;    {f8}	saved, used, restored
;    FCR	saved, used, restored
;
;............................................................................

    .global    _mchp_biquad_cascade_df2T_f32    ; export
_mchp_biquad_cascade_df2T_f32:

;............................................................................
    ; Save working registers.
    push.l    f8                ; {f8} to TOS
    
;............................................................................
    ; Mask all FPU exceptions, set rounding mode to default and clear SAZ/FTZ

    push.l    FCR
    floatsetup    w7

    ; Perpare to filter.
    mov.b    [w0+iirCasNumStage_f32],w4     ; w4 = number of stages
    mov.l    [w0+iirCasPState_f32],w5	; w5 = Coeff array
    mov.l    [w0+iirCasPCoeffs_f32],w6       ; w6 = State
    push.l   w2
    push.l   w3
   
    
startFilter:    

    ;load coeffs
    mov.l    [w5++],f0 ; b0
    mov.l    [w5++],f1 ; b1
    mov.l    [w5++],f2 ; b2
    mov.l    [w5++],f3 ; a1
    mov.l    [w5++],f4 ; a2
 
    ;Reading the state values */
     
     
    mov.l    [w6],f5	; d1 = pState[0]
    mov.l    [w6+4],f6	; d2 = pState[1]

startSections:    
    mov.l    [w1++],f7		; Xn1 = *pSrc++;
    mul.s    f0,f7,f8		; acc1 = b0 * Xn1
    nop
    add.s    f8,f5,f8		; acc1 += d1
    mul.s    f1,f7,f5		; d1 = b1 * Xn1
    nop
    add.s    f5,f6,f5		; d1 += d2
    mac.s    f3,f8,f5           ; d1 += a1 * acc1
    nop
    mul.s    f2,f7,f6           ; d2 = b2 * Xn1
    nop
    mac.s    f4,f8,f6           ; d2 += a2 * acc1
    nop
    mov.l    f8,[w2++]		;*pDst++ = acc1
    
    dtb w3, startSections	;blockSize-- and jump to startSections if not zero;
    mov.l   [W15-4], w3           ;restore num block size
    ;Store the updated state variables back into the state array
    mov.l   f5,[w6++]		;pState[0] = d1
    mov.l   f6,[w6++]		;pState[1] = d2
				;pState += 2U;
    
    ;The current stage output is given as the input to the next stage
    mov.l   [w15-8],w1   ;pIn = pDst
    
    ;Reset the output working pointer
    mov.l   [w15-8],w2   ;pOut = pDst
    
                 
    dtb w4, startFilter
_completedIIR:
    
    sub.l    #8, W15           ; unstacking push.l w2 and w3
;............................................................................

    ; Restore FCR.

    pop.l    FCR
;............................................................................

    ; Restore working registers.
    pop.l    f8                ; {f8} from TOS


    return    

    .end

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; OEF
