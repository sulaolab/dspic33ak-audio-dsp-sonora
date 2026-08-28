/******************************************************************************
 * @file     arm_math.h
 * @brief    Public header file for CMSIS DSP Library
 * @version  V1.10.0
 * @date     08 July 2021
 * Target Processor: Cortex-M and Cortex-A cores
 ******************************************************************************/
/*
 * Copyright (c) 2010-2021 Arm Limited or its affiliates. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * MODIFICATIONS:
 * This file has been substantially modified by Microchip Technology Inc.
 * (2026) to provide compatibility with the Microchip CMSIS-DSP
 * implementation for dsPIC33 devices. Original ARM CMSIS-DSP API
 * signatures are preserved; internal implementations differ entirely.
 * All mchp_* functions are original Microchip implementations. This file 
 * modified for mapping public header file for mapping CMSIS DSP APIs to 
 * dspic33-cmsis-dsp APIs.
 */

#ifndef ARM_MATH_H
#define ARM_MATH_H

#include "mchp_math.h"

#ifdef   __cplusplus
extern "C"
{
#endif

// ------------------------------------------------------------------
// Macro type mapping
// ------------------------------------------------------------------

/*
 * Maps ARM CMSIS DSP attribute macros to Microchip equivalents.
 * This allows CMSIS-DSP based application source code to compile unmodified.
 */
#define ARM_DSP_ATTRIBUTE                MCHP_DSP_ATTRIBUTE

// ------------------------------------------------------------------
// Enum type mapping
// ------------------------------------------------------------------

/*
 * CMSIS-DSP uses arm_status as the standard return type.
 * This maps it directly to Microchip's status enum.
 */
#define arm_status           mchp_status

// ------------------------------------------------------------------
// Enum member mapping
// ------------------------------------------------------------------

/*
 * These macros translate CMSIS-DSP status codes
 * to the Microchip DSP library equivalents.
 */
#define ARM_MATH_SUCCESS                 MCHP_MATH_SUCCESS
#define ARM_MATH_ARGUMENT_ERROR          MCHP_MATH_ARGUMENT_ERROR
#define ARM_MATH_LENGTH_ERROR            MCHP_MATH_LENGTH_ERROR
#define ARM_MATH_SIZE_MISMATCH           MCHP_MATH_SIZE_MISMATCH
#define ARM_MATH_NANINF                  MCHP_MATH_NANINF
#define ARM_MATH_SINGULAR                MCHP_MATH_SINGULAR
#define ARM_MATH_TEST_FAILURE            MCHP_MATH_TEST_FAILURE
#define ARM_MATH_DECOMPOSITION_FAILURE   MCHP_MATH_DECOMPOSITION_FAILURE

// ------------------------------------------------------------------
// CMSIS-DSP API → Microchip API mapping
// ------------------------------------------------------------------

/*
 * The following macros provide API compatibility with
 * ARM CMSIS-DSP function names.
 *
 * Existing CMSIS-DSP based projects can include this header
 * and link against Microchip DSP libraries without
 * changing application source code.
 */

// ---------------- Basic Math Functions (F32)----------------

#define arm_add_f32                   mchp_add_f32 
#define arm_sub_f32                   mchp_sub_f32
#define arm_mult_f32                  mchp_mult_f32
#define arm_negate_f32                mchp_negate_f32
#define arm_scale_f32                 mchp_scale_f32
#define arm_dot_prod_f32              mchp_dot_prod_f32

// ---------------- Transform Functions (CFFT) (F32)----------------

/* Complex FFT support*/
#define arm_cfft_init_f32             mchp_cfft_init_f32
#define arm_cfft_f32                  mchp_cfft_f32
#define arm_cfft_instance_f32         mchp_cfft_instance_f32  

// ---------------- Transform Functions (RFFT) (F32)----------------

/* Optimized real FFT using fast algorithm */
#define arm_rfft_fast_init_f32        mchp_rfft_fast_init_f32
#define arm_rfft_fast_f32             mchp_rfft_fast_f32
#define arm_rfft_fast_instance_f32    mchp_rfft_fast_instance_f32

// ---------------- Window Functions (F32)----------------

/* Windowing functions for spectral analysis */
#define arm_hamming_f32               mchp_hamming_f32
#define arm_bartlett_f32              mchp_bartlett_f32
#define arm_hanning_f32               mchp_hanning_f32

// ---------------- Filter Functions (F32)----------------

/* FIR, IIR, LMS, and biquad filters */
#define arm_fir_instance_f32                    mchp_fir_instance_f32
#define arm_fir_init_f32                        mchp_fir_init_f32
#define arm_fir_f32                             mchp_fir_f32

#define arm_fir_decimate_instance_f32           mchp_fir_decimate_instance_f32
#define arm_fir_decimate_init_f32               mchp_fir_decimate_init_f32
#define arm_fir_decimate_f32                    mchp_fir_decimate_f32

#define arm_fir_interpolate_instance_f32        mchp_fir_interpolate_instance_f32
#define arm_fir_interpolate_init_f32            mchp_fir_interpolate_init_f32
#define arm_fir_interpolate_f32                 mchp_fir_interpolate_f32

#define arm_fir_lattice_instance_f32            mchp_fir_lattice_instance_f32
#define arm_fir_lattice_init_f32                mchp_fir_lattice_init_f32
#define arm_fir_lattice_f32                     mchp_fir_lattice_f32

#define arm_lms_instance_f32                    mchp_lms_instance_f32
#define arm_lms_init_f32                        mchp_lms_init_f32
#define arm_lms_f32                             mchp_lms_f32

#define arm_lms_norm_instance_f32               mchp_lms_norm_instance_f32
#define arm_lms_norm_init_f32                   mchp_lms_norm_init_f32
#define arm_lms_norm_f32                        mchp_lms_norm_f32

#define arm_iir_lattice_instance_f32            mchp_iir_lattice_instance_f32
#define arm_iir_lattice_init_f32                mchp_iir_lattice_init_f32
#define arm_iir_lattice_f32                     mchp_iir_lattice_f32

#define arm_biquad_cascade_df2T_instance_f32    mchp_biquad_cascade_df2T_instance_f32
#define arm_biquad_cascade_df2T_init_f32        mchp_biquad_cascade_df2T_init_f32
#define arm_biquad_cascade_df2T_f32             mchp_biquad_cascade_df2T_f32

#define arm_conv_f32                            mchp_conv_f32
#define arm_correlate_f32                       mchp_correlate_f32

// ---------------- Controller Functions (F32)----------------

/* PID controller implementation */
#define arm_pid_instance_f32          mchp_pid_instance_f32
#define arm_pid_init_f32              mchp_pid_init_f32
#define arm_pid_f32                   mchp_pid_f32  
#define arm_pid_reset_f32             mchp_pid_reset_f32

// ---------------- Matrix Functions (F32)----------------

/* Matrix arithmetic and linear algebra */
#define arm_mat_init_f32              mchp_mat_init_f32
#define arm_matrix_instance_f32       mchp_matrix_instance_f32
#define arm_mat_add_f32               mchp_mat_add_f32
#define arm_mat_sub_f32               mchp_mat_sub_f32
#define arm_mat_mult_f32              mchp_mat_mult_f32
#define arm_mat_trans_f32             mchp_mat_trans_f32
#define arm_mat_scale_f32             mchp_mat_scale_f32
#define arm_mat_inverse_f32           mchp_mat_inverse_f32

// ---------------- Complex Math Functions (F32)----------------

/* Operations on complex-valued vectors */
#define arm_cmplx_mag_squared_f32     mchp_cmplx_mag_squared_f32
#define arm_cmplx_mag_f32             mchp_cmplx_mag_f32

// ---------------- Statistics Functions (F32)----------------

/* Statistical analysis on floating-point vectors */
#define arm_mean_f32                  mchp_mean_f32
#define arm_max_f32                   mchp_max_f32
#define arm_min_f32                   mchp_min_f32
#define arm_power_f32                 mchp_power_f32
#define arm_std_f32                   mchp_std_f32
#define arm_var_f32                   mchp_var_f32

// ---------------- Support Functions (F32)----------------

/* Utility operations */
#define arm_copy_f32                  mchp_copy_f32

// =========================================================================
// Q31 Function Mappings
// =========================================================================

// Basic Math Functions (Q31)
#define arm_add_q31                   mchp_add_q31
#define arm_sub_q31                   mchp_sub_q31
#define arm_mult_q31                  mchp_mult_q31
#define arm_negate_q31                mchp_negate_q31
#define arm_scale_q31                 mchp_scale_q31
#define arm_dot_prod_q31              mchp_dot_prod_q31

// Transform Functions - CFFT (Q31)
#define arm_cfft_init_q31             mchp_cfft_init_q31
#define arm_cfft_q31                  mchp_cfft_q31
#define arm_cfft_instance_q31         mchp_cfft_instance_q31

// Transform Functions - RFFT (Q31) — ARM-compatible names
#define arm_rfft_init_q31             mchp_rfft_init_q31
#define arm_rfft_q31                  mchp_rfft_q31
#define arm_rfft_instance_q31         mchp_rfft_instance_q31
// Legacy aliases (map old "fast" names to the same functions)
#define arm_rfft_fast_init_q31        mchp_rfft_init_q31
#define arm_rfft_fast_q31             mchp_rfft_q31

// Window Functions (Q31)
#define arm_hamming_q31               mchp_hamming_q31
#define arm_bartlett_q31              mchp_bartlett_q31
#define arm_hanning_q31               mchp_hanning_q31

// Filter Functions (Q31)
#define arm_fir_instance_q31                    mchp_fir_instance_q31
#define arm_fir_init_q31                        mchp_fir_init_q31
#define arm_fir_q31                             mchp_fir_q31
#define arm_fir_decimate_instance_q31           mchp_fir_decimate_instance_q31
#define arm_fir_decimate_init_q31               mchp_fir_decimate_init_q31
#define arm_fir_decimate_q31                    mchp_fir_decimate_q31
#define arm_fir_interpolate_instance_q31        mchp_fir_interpolate_instance_q31
#define arm_fir_interpolate_init_q31            mchp_fir_interpolate_init_q31
#define arm_fir_interpolate_q31                 mchp_fir_interpolate_q31
#define arm_fir_lattice_instance_q31            mchp_fir_lattice_instance_q31
#define arm_fir_lattice_init_q31                mchp_fir_lattice_init_q31
#define arm_fir_lattice_q31                     mchp_fir_lattice_q31
#define arm_lms_instance_q31                    mchp_lms_instance_q31
#define arm_lms_init_q31                        mchp_lms_init_q31
#define arm_lms_q31                             mchp_lms_q31
#define arm_lms_norm_instance_q31               mchp_lms_norm_instance_q31
#define arm_lms_norm_init_q31                   mchp_lms_norm_init_q31
#define arm_lms_norm_q31                        mchp_lms_norm_q31
#define arm_iir_lattice_instance_q31            mchp_iir_lattice_instance_q31
#define arm_iir_lattice_init_q31                mchp_iir_lattice_init_q31
#define arm_iir_lattice_q31                     mchp_iir_lattice_q31
#define arm_biquad_cascade_df1_instance_q31     mchp_biquad_cascade_df1_instance_q31
#define arm_biquad_cascade_df1_init_q31         mchp_biquad_cascade_df1_init_q31
#define arm_biquad_cascade_df1_q31              mchp_biquad_cascade_df1_q31
#define arm_conv_q31                            mchp_conv_q31
#define arm_correlate_q31                       mchp_correlate_q31

// Controller Functions (Q31)
#define arm_pid_instance_q31          mchp_pid_instance_q31
#define arm_pid_init_q31              mchp_pid_init_q31
#define arm_pid_q31                   mchp_pid_q31
#define arm_pid_reset_q31             mchp_pid_reset_q31

// Matrix Functions (Q31)
#define arm_mat_init_q31              mchp_mat_init_q31
#define arm_matrix_instance_q31       mchp_matrix_instance_q31
#define arm_mat_add_q31               mchp_mat_add_q31
#define arm_mat_sub_q31               mchp_mat_sub_q31
#define arm_mat_mult_q31              mchp_mat_mult_q31
#define arm_mat_trans_q31             mchp_mat_trans_q31
#define arm_mat_scale_q31             mchp_mat_scale_q31
#define arm_mat_inverse_q31           mchp_mat_inverse_q31

// Complex Math Functions (Q31)
#define arm_cmplx_mag_squared_q31     mchp_cmplx_mag_squared_q31
#define arm_cmplx_mag_q31             mchp_cmplx_mag_q31

// Statistics Functions (Q31)
#define arm_mean_q31                  mchp_mean_q31
#define arm_max_q31                   mchp_max_q31
#define arm_min_q31                   mchp_min_q31
#define arm_power_q31                 mchp_power_q31
#define arm_std_q31                   mchp_std_q31
#define arm_var_q31                   mchp_var_q31

// Support Functions (Q31)
#define arm_copy_q31                  mchp_copy_q31
#define arm_fill_q31                  mchp_fill_q31
#define arm_sqrt_q31                  mchp_sqrt_q31

#ifdef   __cplusplus
}
#endif

#endif /* ARM_MATH_H */