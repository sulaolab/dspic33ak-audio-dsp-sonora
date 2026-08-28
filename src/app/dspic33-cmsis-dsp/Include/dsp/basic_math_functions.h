/******************************************************************************
 * @file     basic_math_functions.h
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

#ifndef BASIC_MATH_FUNCTIONS_H_
#define BASIC_MATH_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupMath Basic Math Functions
 * @brief Basic Math Functions for dspic33-cmsis-dsp Library
 *
 * This group of functions provides the APIs for basic mathematical operations
 * in the dspic33-cmsis-dsp Library.
 *
 * It includes functions for single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities like vector addition, subtraction, 
 * multiplication, scaling, negation, and dot product. These APIs
 * are optimized for high-performance.
 *
 * Features:
 * - Vector addition, subtraction, multiplication
 * - Scalar multiplication (scaling)
 * - Vector negation
 * - Dot product calculation
 *
 * Usage Example:
 * @code
 * float32_t a[SIZE], b[SIZE], result[SIZE], dot;
 * mchp_add_f32(a, b, result, SIZE);
 * mchp_dot_prod_f32(a, b, SIZE, &dot);
 * @endcode
 * 
 * @{
 */ 

/**
* @brief Single-precision floating-point vector addition.
*
* Computes:
*     pDst[n] = pSrcA[n] + pSrcB[n]
*
* @param[in]  pSrcA      points to the first input vector
* @param[in]  pSrcB      points to the second input vector
* @param[out] pDst       points to the output vector
* @param[in]  blockSize  number of samples in each vector
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_add_f32(
const float32_t * pSrcA,
const float32_t * pSrcB,
      float32_t * pDst,
      uint32_t blockSize);

/**
* @brief Single-precision floating-point vector subtraction.
*
* Computes:
*     pDst[n] = pSrcA[n] - pSrcB[n]
*
* @param[in]  pSrcA      points to the first input vector
* @param[in]  pSrcB      points to the second input vector
* @param[out] pDst       points to the output vector
* @param[in]  blockSize  number of samples in each vector
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_sub_f32(
const float32_t * pSrcA,
const float32_t * pSrcB,
      float32_t * pDst,
      uint32_t blockSize);

/**
* @brief Single-precision floating-point vector multiplication.
*
* Computes element-wise multiplication:
*     pDst[n] = pSrcA[n] * pSrcB[n]
*
* @param[in]  pSrcA      points to the first input vector
* @param[in]  pSrcB      points to the second input vector
* @param[out] pDst       points to the output vector
* @param[in]  blockSize  number of samples in each vector
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_mult_f32(
const float32_t * pSrcA,
const float32_t * pSrcB,
      float32_t * pDst,
      uint32_t blockSize);

/**
* @brief Multiplies a single-precision floating-point vector by a scalar.
*
* Computes:
*     pDst[n] = pSrc[n] * scale
*
* @param[in]  pSrc       points to the input vector
* @param[in]  scale      scale factor to be applied
* @param[out] pDst       points to the output vector
* @param[in]  blockSize  number of samples in the vector
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_scale_f32(
const float32_t * pSrc,
      float32_t scale,
      float32_t * pDst,
      uint32_t blockSize);

/**
* @brief  Negates the elements of a single-precision floating-point vector.
*
* Computes:
*     pDst[n] = -pSrc[n]
*
* @param[in]  pSrc       points to the input vector
* @param[out] pDst       points to the output vector
* @param[in]  blockSize  number of samples in the vector
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_negate_f32(
const float32_t * pSrc,
      float32_t * pDst,
      uint32_t blockSize);

/**
* @brief Dot product of single-precision floating-point vectors.
*
* Computes:
*     result = sum(pSrcA[n] * pSrcB[n]) for n = 0..blockSize-1
*
* @param[in]  pSrcA      points to the first input vector
* @param[in]  pSrcB      points to the second input vector
* @param[in]  blockSize  number of samples in each vector
* @param[out] result     Pointer to the accumulated dot-product value
*
* @note All input and output arrays must be at least blockSize elements long.
*/
void mchp_dot_prod_f32(
const float32_t * pSrcA,
const float32_t * pSrcB,
      uint32_t blockSize,
      float32_t * result);
	  
/* ===================================================================
 * Q31 Basic Math Functions
 * =================================================================== */

/**
 * @brief Q31 vector addition.
 * @param[in]  pSrcA      points to the first input vector
 * @param[in]  pSrcB      points to the second input vector
 * @param[out] pDst       points to the output vector
 * @param[in]  blockSize  number of samples in each vector
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_add_q31(
    const q31_t * pSrcA,
    const q31_t * pSrcB,
    q31_t * pDst,
    uint32_t blockSize);

/**
 * @brief Q31 vector subtraction.
 * @param[in]  pSrcA      points to the first input vector
 * @param[in]  pSrcB      points to the second input vector
 * @param[out] pDst       points to the output vector
 * @param[in]  blockSize  number of samples in each vector
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_sub_q31(
    const q31_t * pSrcA,
    const q31_t * pSrcB,
    q31_t * pDst,
    uint32_t blockSize);

/**
 * @brief Q31 vector multiplication.
 * @param[in]  pSrcA      points to the first input vector
 * @param[in]  pSrcB      points to the second input vector
 * @param[out] pDst       points to the output vector
 * @param[in]  blockSize  number of samples in each vector
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_mult_q31(
    const q31_t * pSrcA,
    const q31_t * pSrcB,
    q31_t * pDst,
    uint32_t blockSize);

/**
 * @brief Multiplies a Q31 vector by a scalar with post-shift.
 * @param[in]  pSrc        points to the input vector
 * @param[in]  scaleFract  fractional portion of the scale value (Q31)
 * @param[in]  shift       number of bits to shift the result (positive = left)
 * @param[out] pDst        points to the output vector
 * @param[in]  blockSize   number of samples in the vector
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_scale_q31(
    const q31_t * pSrc,
          q31_t   scaleFract,
          int8_t  shift,
          q31_t * pDst,
          uint32_t blockSize);

/**
 * @brief  Negates the elements of a Q31 vector.
 * @param[in]  pSrc       points to the input vector
 * @param[out] pDst       points to the output vector
 * @param[in]  blockSize  number of samples in the vector
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_negate_q31(
    const q31_t * pSrc,
    q31_t * pDst,
    uint32_t blockSize);

/**
 * @brief Dot product of Q31 vectors.
 * @param[in]  pSrcA      points to the first input vector
 * @param[in]  pSrcB      points to the second input vector
 * @param[in]  blockSize  number of samples in each vector
 * @param[out] result     output result returned here (Q63 format)
 *
 * @note All input and output arrays must be at least blockSize elements long.
 */
void mchp_dot_prod_q31(
    const q31_t * pSrcA,
    const q31_t * pSrcB,
    uint32_t blockSize,
    q63_t * result);
/** 
 * @} 
 */ 
/* end of groupMath */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef BASIC_MATH_FUNCTIONS_H_ */
