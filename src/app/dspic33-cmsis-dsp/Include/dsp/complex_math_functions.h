/******************************************************************************
 * @file     complex_math_functions.h
 * @brief    Public header file for dspic33-cmsis-dsp Library
 * Target Processor: dsPIC33A cores
 ******************************************************************************/
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

#ifndef COMPLEX_MATH_FUNCTIONS_H_
#define COMPLEX_MATH_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupCmplxMath Complex Math Functions
 * @brief  Complex Math Functions for dspic33-cmsis-dsp Library
 * 
 * This group of functions provides the APIs for complex mathematical operations
 * in the dspic33-cmsis-dsp Library.
 * 
 * This group contains functions for single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities that operate on complex-valued vectors.
 * 
 * The data in the complex arrays is stored in an interleaved fashion
 * (real, imag, real, imag, ...).
 * 
 * The numSamples parameter refers to the number of complex samples.
 * Therefore, the input array contains 2 * numSamples float32_t values.
 *
 * @{
 */

/**
 * @brief Single-precision floating-point complex magnitude squared.
 *
 * Computes:
 *     pDst[n] = (real[n] * real[n]) + (imag[n] * imag[n])
 *
 * This function avoids the square-root operation and is typically used
 * when only relative magnitudes or power comparisons are required.
 *
 * @param[in]  pSrc        points to the interleaved complex input vector
 * @param[out] pDst        points to the real-valued output vector
 * @param[in]  numSamples  number of complex samples in the input vector
 *
 * @note Output vector length must be at least numSamples elements.
 */
 void mchp_cmplx_mag_squared_f32(
 const float32_t * pSrc,
    float32_t * pDst,
    uint32_t numSamples);

/**
 * @brief  Q31 complex magnitude squared
 * @param[in]  pSrc        points to the complex input vector (interleaved Re/Im)
 * @param[out] pDst        points to the real output vector
 * @param[in]  numSamples  number of complex samples in the input vector
 *
 * @note Output vector length must be at least numSamples elements.
 */
 void mchp_cmplx_mag_squared_q31(
 const q31_t * pSrc,
    q31_t * pDst,
    uint32_t numSamples);

/**
 * @brief Single-precision floating-point complex magnitude.
 *
 * Computes:
 *     pDst[n] = sqrt( (real[n] * real[n]) + (imag[n] * imag[n]) )
 *
 * This function is commonly used after FFT to obtain amplitude
 * spectra from complex frequency-domain data.
 *
 * @param[in]  pSrc        points to the interleaved complex input vector
 * @param[out] pDst        points to the real-valued output vector
 * @param[in]  numSamples  number of complex samples in the input vector
 *
 * @note Output vector length must be at least numSamples elements.
 */
 void mchp_cmplx_mag_f32(
 const float32_t * pSrc,
    float32_t * pDst,
    uint32_t numSamples);

/** 
 * @} 
 */ 
/* end of groupCmplxMath */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef COMPLEX_MATH_FUNCTIONS_H_ */
