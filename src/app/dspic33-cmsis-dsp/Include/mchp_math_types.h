/*********************************************************************
*                                                                    *
*                       Software License Agreement                   *
*                                                                    *
*   The software supplied herewith by Microchip Technology           *
*   Incorporated (the "Company") for its dsPIC controller            *
*   is intended and supplied to you, the Company's customer,         *
*   for use solely and exclusively on Microchip dsPIC                *
*   products. The software is owned by the Company and/or its        *
*   supplier, and is protected under applicable copyright laws. All  *
*   rights are reserved. Any use in violation of the foregoing       *
*   restrictions may subject the user to criminal sanctions under    *
*   applicable laws, as well as to civil liability for the breach of *
*   the terms and conditions of this license.                        *
*                                                                    *
*   THIS SOFTWARE IS PROVIDED IN AN "AS IS" CONDITION.  NO           *
*   WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING,    *
*   BUT NOT LIMITED TO, IMPLIED WARRANTIES OF MERCHANTABILITY AND    *
*   FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE. THE     *
*   COMPANY SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR SPECIAL,  *
*   INCIDENTAL OR CONSEQUENTIAL DAMAGES, FOR ANY REASON WHATSOEVER.  *
*                                                                    *
*   (c) Copyright 2026 Microchip Technology, All rights reserved.    *
*********************************************************************/

#ifndef MCHP_MATH_TYPES_H
#define MCHP_MATH_TYPES_H

#include <stdint.h>
#include "mchp_dsp_config.h"

#ifndef MCHP_DSP_ATTRIBUTE 
#define MCHP_DSP_ATTRIBUTE 
#endif

#ifndef MCHP_DSP_TABLE_ATTRIBUTE 
#define MCHP_DSP_TABLE_ATTRIBUTE 
#endif

#ifdef   __cplusplus
extern "C"
{
#endif

/* Included for intrinsics definitions */
#if defined ( __XC_DSC__ )

#define  __ALIGNED(x) __attribute__((aligned(x))) // cppcheck-suppress misra-c2012-21.1 : Reserved identifiers are intentionally used to match XC-DSC compiler intrinsic naming conventions.
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline)) // cppcheck-suppress misra-c2012-21.1 : Reserved identifiers are intentionally used to match XC-DSC compiler intrinsic naming conventions.
#define __STATIC_INLINE static inline // cppcheck-suppress misra-c2012-21.1 : Reserved identifiers are intentionally used to match XC-DSC compiler intrinsic naming conventions.
#define __WEAK __attribute__((weak)) // cppcheck-suppress misra-c2012-21.1 : Reserved identifiers are intentionally used to match XC-DSC compiler intrinsic naming conventions.
#define SECTION_NOINIT __attribute__((section(".noinit"))) // cppcheck-suppress misra-c2012-21.1 : Reserved identifiers are intentionally used to match XC-DSC compiler intrinsic naming conventions.
#else
  #error Unknown compiler
#endif

/* Some constants. */
#ifndef PI/* [ */
#define PI 3.1415926535897931159979634685441851615905761718750 /* double */
#endif  /* ] */

#define BART_0           2.0f     /* Bartlett 0th factor */

#define HANN_0           0.50f    /* Hanning 0th factor */
#define HANN_1          -0.50f    /* Hanning 1st factor */

#define HAMM_0           0.54f /* Hamming 0th factor */
#define HAMM_1          -0.46f/* Hamming 1st factor */
/**
 * @defgroup genericTypes Generic Types
 * @{
*/

/**
* @brief 32-bit floating-point type definition.
*/
typedef float float32_t;
/**
* @brief 32-bit fixed-point type definition.
*/
typedef int q31_t;
/**
* @brief 64-bit fixed-point type definition.
*/
typedef long long q63_t;
  
/**
 * @} endgroup generic
*/

typedef enum
{
	MCHP_MATH_SUCCESS                 =  0,        /**< No error */
	MCHP_MATH_ARGUMENT_ERROR          = -1,        /**< One or more arguments are incorrect */
	MCHP_MATH_LENGTH_ERROR            = -2,        /**< Length of data buffer is incorrect */
	MCHP_MATH_SIZE_MISMATCH           = -3,        /**< Size of matrices is not compatible with the operation */
	MCHP_MATH_NANINF                  = -4,        /**< Not-a-number (NaN) or infinity is generated */
	MCHP_MATH_SINGULAR                = -5,        /**< Input matrix is singular and cannot be inverted */
	MCHP_MATH_TEST_FAILURE            = -6,        /**< Test Failed */
	MCHP_MATH_DECOMPOSITION_FAILURE   = -7         /**< Decomposition Failed */
} mchp_status;

#ifdef   __cplusplus
}
#endif

#endif /*MCHP_MATH_TYPES_H*/
