/******************************************************************************
 * @file     support_functions.h
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

#ifndef SUPPORT_FUNCTIONS_H_
#define SUPPORT_FUNCTIONS_H_

#include "../mchp_math_types.h"
#include "../mchp_dsp_config.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupSupport Support Functions
 * @brief Support Functions for dspic33-cmsis-dsp Library
 *
 * This group of functions provides the APIs for support functions in the
 * dspic33-cmsis-dsp Library.
 *
 * Support functions provide common utility operations that are reused. 
 * These functions are typically lightweight and optimized for memory throughput.
 *
 * Features:
 * - Vector copy operation for single-precision floating-point data
 *
 * Usage Example:
 * @code
 * float32_t src[SIZE], dst[SIZE];
 * mchp_copy_f32(src, dst, SIZE);
 * @endcode
 *
 * @{
 */

  /**
   * @brief Copies the elements of a single-precision floating-point vector.
   *
   * Performs:
   *     pDst[n] = pSrc[n]  for n = 0 .. blockSize-1
   *
   * @param[in]  pSrc       input pointer (source buffer)
   * @param[out] pDst       output pointer (destination buffer)
   * @param[in]  blockSize  number of samples to process
   *
   * @note Source and destination buffers must not overlap.
   */
  void mchp_copy_f32(
  const float32_t * pSrc,
        float32_t * pDst,
        uint32_t blockSize);

  /**
   * @brief  Copies the elements of a Q31 vector.
   * @param[in]  pSrc       input pointer
   * @param[out] pDst       output pointer
   * @param[in]  blockSize  number of samples to process
   * @par
   */
  void mchp_copy_q31(
  const q31_t * pSrc,
        q31_t * pDst,
        uint32_t blockSize);

  /**
   * @brief  Fills a constant value into a Q31 vector.
   * @param[in]  value      input value to be filled
   * @param[out] pDst       output pointer
   * @param[in]  blockSize  number of samples to process
   * @par
   */
  void mchp_fill_q31(
        q31_t value,
        q31_t * pDst,
        uint32_t blockSize);

  /**
   * @brief         Q31 square root function.
   * @param[in]     in        input value.  The range of the input value is [0 +1) or 0x00000000 to 0x7FFFFFFF.
   * @param[out]    pOut      points to square root of input value.
   * @return        execution status
   *                  - \ref MCHP_MATH_SUCCESS        : input value is positive
   *                  - \ref MCHP_MATH_ARGUMENT_ERROR  : input value is negative; *pOut is set to 0
   */
  mchp_status mchp_sqrt_q31(
        q31_t in,
        q31_t * pOut);

/** 
 * @} 
 */ 
/* end of groupSupport */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef SUPPORT_FUNCTIONS_H_ */
