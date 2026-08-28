/******************************************************************************
 * @file     statistics_functions.h
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

#ifndef STATISTICS_FUNCTIONS_H_
#define STATISTICS_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupStats Statistics Functions
 * @brief Statistics Functions for dspic33-cmsis-dsp Library
 *
 * This group of functions provides the APIs for statistical analysis functions
 * in the dspic33-cmsis-dsp Library.
 *
 * It includes functions for single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities like computing mean, maximum, minimum, sum of squares (power),
 * variance, and standard deviation.
 *
 * Features:
 * - Mean value calculation
 * - Maximum and minimum value detection (with index)
 * - Sum of squares (power) computation
 * - Variance and standard deviation calculation
 *
 * Usage Example:
 * @code
 * float32_t data[SIZE], mean, max, min, power, var, std;
 * uint32_t maxIdx, minIdx;
 * mchp_mean_f32(data, SIZE, &mean);
 * mchp_max_f32(data, SIZE, &max, &maxIdx);
 * mchp_min_f32(data, SIZE, &min, &minIdx);
 * mchp_power_f32(data, SIZE, &power);
 * mchp_var_f32(data, SIZE, &var);
 * mchp_std_f32(data, SIZE, &std);
 * @endcode
 *
 * @{
 */

/**
 * @brief Mean value of a single-precision floating-point vector.
 *
 * Computes:
 *     mean = (1/N) * sum(pSrc[n])
 *
 * @param[in]  pSrc       Pointer to input vector
 * @param[in]  blockSize  Number of samples to process
 * @param[out] pResult    Computed mean value
 *
 * @note blockSize must be greater than zero.
 */
  void mchp_mean_f32(
    const float32_t * pSrc,
            uint32_t blockSize,
            float32_t * pResult);

/**
 * @brief Maximum value of a single-precision floating-point vector.
 *
 * Scans the input vector and returns the maximum value along with the index
 * of its first occurrence. The index is zero-based.
 *
 * @param[in]  pSrc       Pointer to input vector
 * @param[in]  blockSize  Length of the input vector
 * @param[out] pResult    Computed maximum value
 * @param[out] pIndex     Index of maximum value
 * 
 * @note 
 * - If multiple elements have the same maximum value, the lowest index
 * is returned.
 * - NaN values are ignored during comparison.
 * - Behavior is undefined if blockSize is zero or pSrc is NULL.
 */
  void mchp_max_f32(
    const float32_t * pSrc,
            uint32_t blockSize,
            float32_t * pResult,
            uint32_t * pIndex);

  /**
   * @brief  Minimum value of a single-precision floating-point vector.
   * 
   * Scans the input vector and returns the minimum value and the index
   * of its first occurrence. The index is zero-based.
   * 
   * @param[in]  pSrc       Pointer to input vector
   * @param[in]  blockSize  Number of samples to process
   * @param[out] pResult    Computed minimum value
   * @param[out] pIndex     Index of minimum value
   * 
   * @note 
   * - If multiple elements have the same minimum value, the lowest
   * index is returned.
   * - NaN values are ignored during comparison.
   * - Behavior is undefined if blockSize is zero.
   */
  void mchp_min_f32(
    const float32_t * pSrc,
            uint32_t blockSize,
            float32_t * pResult,
            uint32_t * pIndex);

  /**
   * @brief  Sum of the squares of the elements of a single-precision floating-point vector.
   * 
   * Computes:
   *   result = sum_{i=0..blockSize-1} (pSrc[i] * pSrc[i])
   * 
   * @param[in]  pSrc       Pointer to input vector
   * @param[in]  blockSize  Number of samples to process
   * @param[out] pResult    Computed value
   * 
   * @note 
   * - NaN values propagate to the result.
   * - Overflow may result in +Inf.
   * - Behavior is undefined if blockSize is zero.
   */
  void mchp_power_f32(
    const float32_t * pSrc,
          uint32_t blockSize,
          float32_t * pResult);

  /**
   * @brief  Variance of the elements of a single-precision floating-point vector.
   * @param[in]  pSrc       Pointer to input vector
   * @param[in]  blockSize  Number of samples to process
   * @param[out] pResult    Computed variance value
   * @note Uses population variance definition.
   */
  void mchp_var_f32(
    const float32_t * pSrc,
        uint32_t blockSize,
        float32_t * pResult);

  /**
   * @brief  Standard deviation of the elements of a single-precision floating-point vector.
   * @param[in]  pSrc       Pointer to input vector
   * @param[in]  blockSize  Number of samples to process
   * @param[out] pResult    Computed standard deviation value
   * @note This function internally computes variance.
   */
  void mchp_std_f32(
    const float32_t * pSrc,
        uint32_t blockSize,
        float32_t * pResult);

/**
 * @brief  Mean value of a Q31 vector.
 * @param[in]  pSrc       is input pointer
 * @param[in]  blockSize  is the number of samples to process
 * @param[out] pResult    is output value.
 * @par
 */
  void mchp_mean_q31(
    const q31_t * pSrc,
            uint32_t blockSize,
            q31_t * pResult);

/**
 * @brief Maximum value of a Q31 vector.
 * @param[in]  pSrc       points to the input buffer
 * @param[in]  blockSize  length of the input vector
 * @param[out] pResult    maximum value returned here
 * @param[out] pIndex     index of maximum value returned here
 * @par
 */
  void mchp_max_q31(
    const q31_t * pSrc,
            uint32_t blockSize,
            q31_t * pResult,
            uint32_t * pIndex);

  /**
   * @brief  Minimum value of a Q31 vector.
   * @param[in]  pSrc       is input pointer
   * @param[in]  blockSize  is the number of samples to process
   * @param[out] pResult    is output pointer
   * @param[out] pIndex     is the array index of the minimum value in the input buffer.
   * @par
   */
  void mchp_min_q31(
    const q31_t * pSrc,
            uint32_t blockSize,
            q31_t * pResult,
            uint32_t * pIndex);

  /**
   * @brief  Sum of the squares of the elements of a Q31 vector.
   * @param[in]  pSrc       is input pointer
   * @param[in]  blockSize  is the number of samples to process
   * @param[out] pResult    is output value.
   * @par
   */
  void mchp_power_q31(
    const q31_t * pSrc,
          uint32_t blockSize,
          q63_t * pResult);

  /**
   * @brief  Variance of the elements of a Q31 vector.
   * @param[in]  pSrc       is input pointer
   * @param[in]  blockSize  is the number of samples to process
   * @param[out] pResult    is output value.
   * @par
   */
  void mchp_var_q31(
    const q31_t * pSrc,
        uint32_t blockSize,
        q31_t * pResult);

  /**
   * @brief  Standard deviation of the elements of a Q31 vector.
   * @param[in]  pSrc       is input pointer
   * @param[in]  blockSize  is the number of samples to process
   * @param[out] pResult    is output value.
   * @par
   */
  void mchp_std_q31(
    const q31_t * pSrc,
        uint32_t blockSize,
        q31_t * pResult);

/** 
 * @} 
 */ 
/* end of groupStats */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef STATISTICS_FUNCTIONS_H_ */
