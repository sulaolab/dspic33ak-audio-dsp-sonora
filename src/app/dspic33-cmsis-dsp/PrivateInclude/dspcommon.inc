;*********************************************************************
;                                                                    *
;                       Software License Agreement                   *
;                                                                    *
;   The software supplied herewith by Microchip Technology           *
;   Incorporated (the "Company") for its dsPIC controller            *
;   is intended and supplied to you, the Company's customer,         *
;   for use solely and exclusively on Microchip dsPIC                *
;   products. The software is owned by the Company and/or its        *
;   supplier, and is protected under applicable copyright laws. All  *
;   rights are reserved. Any use in violation of the foregoing       *
;   restrictions may subject the user to criminal sanctions under    *
;   applicable laws, as well as to civil liability for the breach of *
;   the terms and conditions of this license.                        *
;                                                                    *
;   THIS SOFTWARE IS PROVIDED IN AN "AS IS" CONDITION.  NO           *
;   WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING,    *
;   BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND    *
;   FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. THE     *
;   COMPANY SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR SPECIAL,  *
;   INCIDENTAL OR CONSEQUENTIAL DAMAGES, FOR ANY REASON WHATSOEVER.  *
;                                                                    *
;   (c) Copyright 2026 Microchip Technology, All rights reserved.    *
;*********************************************************************

        ; Global inclusions.
    .nolist
        ; none...
    .list

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    ;mchp_status
    .equ    MathSuccess,0
        ; Status code indicating successful completion

    .equ    MathArgumentError,-1
        ; One or more input arguments are invalid or NULL

    .equ    MathLengthError,-2
        ; Buffer length does not meet function requirements

    .equ    MathSizeMismatch,-3
        ; Matrix or vector dimensions are incompatible

    .equ    MathNaninf,-4
        ; Intended meaning is NaN or Infinity generated

    .equ    MathSingular,-5
        ; Matrix determinant is zero; inverse not possible

    .equ    MathTestFailure, -6
        ; Internal validation or unit test failure

    .equ    MathDecompositionFailure, -7
        ; LU/QR/Cholesky decomposition did not converge

.ifdef __dsPIC33A
    .equ    kSof,4        
        ; sizeof (fractional), or
        ; sizeof (fractional*) if
        ; addressing data memory

    .equ    kSinPiQ,0x5A82799A
        ; Q31 representation of sin(pi/4) ≈ 0.70710678
.endif
    .equ    kInvSqrt2,kSinPiQ
        ; 1/sqrt(2) equals sin(pi/4), reused constant

;............................................................................

        ; Declare special function registers as of DS70030C page 4-3,4
        ; since IAR does not recognize the symbols CORCON, MODCON, ...
        ; These are absolute SFR addresses for dsPIC devices

.ifdef __dsPIC33A
        .equ    CORCON,0x0010
        .equ    MODCON,0x0014
        .equ    XBREV,0x0028
        .equ    XMODSRT,0x0018
        .equ    XMODEND,0x001C
        .equ    YMODSRT,0x0020
        .equ    YMODEND,0x0024
.else
        .equ    PSVPAG,0x0034
        .equ    CORCON,0x0044
        .equ    MODCON,0x0046
        .equ    XBREV,0x0050
        .equ    XMODSRT,0x0048
        .equ    XMODEND,0x004A
        .equ    YMODSRT,0x004C
        .equ    YMODEND,0x004E
.endif

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

        ; Operational modes for fractional computation:
        .equ    SATA_ON,1<<7
        ; Enables saturation on Accumulator A

        .equ    SATB_ON,1<<6
        ; Enables saturation on Accumulator B

        .equ    SATDW_ON,1<<5
        ; Enables saturation on data writes

        .equ    ACCSAT_ON,1<<4
        ; Enables 9.31 accumulator saturation

        .equ    FRACT_SET,SATA_ON|SATB_ON|SATDW_ON|ACCSAT_ON
        ; Composite mask to enable all saturation features

        .equ    RND_OFF,~(1<<1)
        ; Bit cleared → convergent rounding DISABLED

        .equ    IF_OFF,~(1<<0)
        ; Bit cleared → integer multiply mode disabled,
        ; fractional multiply enabled

        .equ    FRACT_RESET,RND_OFF&IF_OFF
        ; Mask clears rounding and integer/fractional select bits

        .equ    FRACT_MODE,FRACT_SET&FRACT_RESET
        ; Final CORCON mask for fractional DSP mode

        .ifdef  TOOLVERS
        .equ    FRACT_MODE,FRACT_MODE&0x3ff
        ; Ensures 10-bit literal compatibility
        .endif

        ; Operational modes for Program Space Visibility.
    .ifdef __dsPIC33F
        .equ    PSV_ON,1<<2
        ; Enables PSV mapping into data space
    .endif

;............................................................................

        ; Prepare CORCON for fractional computation.
        .macro  fractsetup      wx
        ; Loads CORCON with fractional DSP settings
        mov     #FRACT_MODE,\wx
        mov     \wx,CORCON
        .endm

.ifdef __dsPIC33A
        .equ    FLOAT_MODE,0x7F<<0
        ; Masks all floating-point exceptions, SAZ/FTZ disabled

        .macro  floatsetup      wx
        ; Initializes Floating-Point Control Register (FCR)
        mov.l     #FLOAT_MODE,\wx
        mov.l     \wx,FCR
        .endm
.endif

.ifndef __dsPIC33A
        ; Set higher offset to 1, if found zero
        ; Used to avoid zero-offset addressing in modulo ops
        .macro  offsetsetup      wx, wy
            MOV #0, \wy
            CPSNE \wx, \wy
            MOV #1, \wx
        .endm

        ; Prepare CORCON for Program Space Visibility access.
        .macro  psvaccess       wx
        ; Preserves CORCON and enables PSV bit
        mov     CORCON,\wx
        ior     #PSV_ON,\wx
        mov     \wx,CORCON
        .endm
.endif

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; Matrix function constants
    ; Offsets:
    ;   numRows: 0
    ;   numCols: 2
    ;   pData:   4
    ; Structure assigns 16-bit rows/cols and 32-bit pointer

    .equ    MATRIX_NUMROWS_OFF,   0
    .equ    MATRIX_NUMCOLS_OFF,   2
    .equ    MATRIX_PDATA_OFF,     4

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;Filter function offsets
;............................................................................

    ; All *_f32 structures assume float32 (4-byte) fields

    ; mchp_fir_instance_f32 offset values
    .equ   firNumTaps_f32,0
    .equ   firPState_f32,(firNumTaps_f32+kSof)
    .equ   firPCoeffs_f32,(firPState_f32+kSof)

    ; mchp_fir_instance_q31 offset values
    .equ   firNumTaps_q31,0
    .equ   firPState_q31,(firNumTaps_q31+kSof)
    .equ   firPCoeffs_q31,(firPState_q31+kSof)

    ; mchp_fir_decimate_instance_f32 offset values
    .equ   firDecM_f32,0
    .equ   firDecNumTaps_f32,2
    .equ   firDecPCoeffs_f32,(firDecM_f32+kSof)
    .equ   firDecPState_f32,(firDecPCoeffs_f32+kSof)
    ; firDecNumTaps_f32 is 16-bit, others are 32-bit

    ; mchp_fir_interpolate_instance_f32 offset values
    .equ   firInterL_f32,0
    .equ   firInterPLen_f32,2
    .equ   firInterPCoeffs_f32,(firInterL_f32+kSof)
    .equ   firInterPState_f32,(firInterPCoeffs_f32+kSof)

    ; mchp_fir_lattice_instance_f32 offset values
    .equ   firLatticeNumStage_f32,0
    .equ   firLatticePCoeffs_f32,(firLatticeNumStage_f32+kSof)
    .equ   firLatticePState_f32,(firLatticePCoeffs_f32+kSof)

    ; mchp_lms_instance_f32 offset values
    .equ   lmsNumTaps_f32,0
    .equ   lmsPState_f32,(lmsNumTaps_f32+kSof)
    .equ   lmsPCoeffs_f32,(lmsPState_f32+kSof)
    .equ   lmsMu_f32,(lmsPCoeffs_f32+kSof)
    ; Mu is adaptation step size (learning rate)

    ; mchp_lms_norm_instance_f32 offset values
    .equ   lmsNormNumTaps_f32,0
    .equ   lmsNormPState_f32,(lmsNormNumTaps_f32+kSof)
    .equ   lmsNormPCoeffs_f32,(lmsNormPState_f32+kSof)
    .equ   lmsNormMu_f32,(lmsNormPCoeffs_f32+kSof)
    .equ   lmsNormEnergy_f32,(lmsNormMu_f32+kSof)
    .equ   lmsNormX0_f32,(lmsNormEnergy_f32+kSof)

    ; mchp_iir_lattice_instance_f32 offset values
    .equ   iirLatticeNumStage_f32,0
    .equ   iirLatticePState_f32,(iirLatticeNumStage_f32+kSof)
    .equ   iirLatticePkCoeffs_f32,(iirLatticePState_f32+kSof)
    .equ   iirLatticePvCoeffs_f32,(iirLatticePkCoeffs_f32+kSof)

    ; mchp_biquad_cascade_df2T_instance_f32 offset values
    .equ   iirCasNumStage_f32,0
    .equ   iirCasPState_f32,(iirCasNumStage_f32+kSof)
    .equ   iirCasPCoeffs_f32,(iirCasPState_f32+kSof)

    ; mchp_biquad_cascade_df1_instance_q31 offset values
	.equ   iirCasNumStage_q31,0
	.equ   iirCasPCoeffs_q31,(iirCasNumStage_q31+kSof)
	.equ   iirCasPState_q31,(iirCasPCoeffs_q31+kSof)
	.equ   iirCasPostShift_q31,(iirCasPState_q31+kSof)

    ; mchp_fir_decimate_instance_q31 offset values
    ; C struct: { uint8_t M; uint16_t numTaps; q31_t *pCoeffs; q31_t *pState; }
    ; Same layout as f32: M is byte@0, numTaps is halfword@2, pointers@4,8
	.equ   firDecM_q31,0
	.equ   firDecNumTaps_q31,2
	.equ   firDecPCoeffs_q31,(firDecM_q31+kSof)
	.equ   firDecPState_q31,(firDecPCoeffs_q31+kSof)

    ; mchp_fir_interpolate_instance_q31 offset values
	.equ   firInterL_q31,0
	.equ   firInterPLen_q31,2
	.equ   firInterPCoeffs_q31,(firInterL_q31+kSof)
	.equ   firInterPState_q31,(firInterPCoeffs_q31+kSof)

    ; mchp_fir_lattice_instance_q31 offset values
	.equ   firLatticeNumStages_q31,0
	.equ   firLatticePCoeffs_q31,(firLatticeNumStages_q31+kSof)
	.equ   firLatticePState_q31,(firLatticePCoeffs_q31+kSof)

    ; mchp_lms_instance_q31 offset values
	.equ   lmsNumTaps_q31,0
	.equ   lmsPState_q31,(lmsNumTaps_q31+kSof)
	.equ   lmsPCoeffs_q31,(lmsPState_q31+kSof)
	.equ   lmsMu_q31,(lmsPCoeffs_q31+kSof)

    ; mchp_lms_norm_instance_q31 offset values
	.equ   lmsNormNumTaps_q31,0
	.equ   lmsNormPState_q31,(lmsNormNumTaps_q31+kSof)
	.equ   lmsNormPCoeffs_q31,(lmsNormPState_q31+kSof)
	.equ   lmsNormMu_q31,(lmsNormPCoeffs_q31+kSof)
	.equ   lmsNormEnergy_q31,(lmsNormMu_q31+kSof)

    ; mchp_iir_lattice_instance_q31 offset values
	.equ   iirLatticeNumStage_q31,0
	.equ   iirLatticePState_q31,(iirLatticeNumStage_q31+kSof)
	.equ   iirLatticePkCoeffs_q31,(iirLatticePState_q31+kSof)
	.equ   iirLatticePvCoeffs_q31,(iirLatticePkCoeffs_q31+kSof)

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;PID function offsets
;............................................................................

    ; mchp_pid_instance_f32 offset values
    ; Direct Form PID with stored state variables
    .equ    offsetA0_f32, 0
    .equ    offsetA1_f32, (offsetA0_f32+kSof)
    .equ    offsetA2_f32, (offsetA1_f32+kSof)
    .equ    offsetstate0_f32, (offsetA2_f32+kSof)
    .equ    offsetstate1_f32, (offsetstate0_f32+kSof)
    .equ    offsetstate2_f32, (offsetstate1_f32+kSof)
    .equ    offsetKp_f32, (offsetstate2_f32+kSof)
    .equ    offsetKi_f32, (offsetKp_f32+kSof)
    .equ    offsetKd_f32, (offsetKi_f32+kSof)

    ; mchp_pid_instance_q31 offset values (same layout as f32)
    .equ    offsetA0_q31, 0
    .equ    offsetA1_q31, (offsetA0_q31+kSof)
    .equ    offsetA2_q31, (offsetA1_q31+kSof)
    .equ    offsetstate0_q31, (offsetA2_q31+kSof)
    .equ    offsetstate1_q31, (offsetstate0_q31+kSof)
    .equ    offsetstate2_q31, (offsetstate1_q31+kSof)
    .equ    offsetKp_q31, (offsetstate2_q31+kSof)
    .equ    offsetKi_q31, (offsetKp_q31+kSof)
    .equ    offsetKd_q31, (offsetKi_q31+kSof)

; OEF
	