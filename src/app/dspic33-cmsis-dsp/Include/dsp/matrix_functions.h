/******************************************************************************
 * @file     matrix_functions.h
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

#ifndef MATRIX_FUNCTIONS_H_
#define MATRIX_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupMatrix Matrix Functions
 * @brief Matrix Functions for dspic33-cmsis-dsp Library
 *
 * This group provides single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities for matrix operations used in
 * linear algebra, control systems, estimation, and DSP algorithms.
 *
 * Matrices are stored in row-major order:
 *   element(i,j) = pData[i * numCols + j]
 *
 * Size checking is performed by default on all operations.
 *
 * @{
 */

  /**
   * @brief Instance structure for the single-precision floating-point matrix structure.
   *
   * This structure describes the dimensions of a matrix and
   * points to its contiguous data buffer.
   */
   typedef struct
   {
     uint16_t numRows;     /**< Number of rows in the matrix */
     uint16_t numCols;     /**< Number of columns in the matrix */
     float32_t *pData;     /**< Pointer to row-major data array */
   } mchp_matrix_instance_f32;

  /**
   * @brief Single-precision floating-point matrix initialization.
   *
   * Initializes the matrix instance structure with dimensions
   * and a pointer to user-allocated data.
   *
   * @param[in,out] S         Matrix instance to initialize
   * @param[in]     nRows     Number of rows in the matrix
   * @param[in]     nColumns  Number of columns in the matrix
   * @param[in]     pData     Pointer to matrix data array
   *
   * @note
   * - If the instance is declared const, do not use this function.
   * - Initialize the structure statically instead.
   */
   void mchp_mat_init_f32(
        mchp_matrix_instance_f32 * S,
        uint16_t nRows,
        uint16_t nColumns,
        float32_t * pData);

  /**
   * @brief Single-precision floating-point matrix addition.
   * @param[in]  pSrcA  Points to the first input matrix structure
   * @param[in]  pSrcB  Points to the second input matrix structure
   * @param[out] pDst   Points to output matrix structure
   * @return     The function returns either @ref MCHP_MATH_SIZE_MISMATCH or @ref MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
   mchp_status mchp_mat_add_f32(
    const mchp_matrix_instance_f32 * pSrcA,
    const mchp_matrix_instance_f32 * pSrcB,
            mchp_matrix_instance_f32 * pDst);

  /**
   * @brief Single-precision floating-point matrix subtraction
   * @param[in]  pSrcA  Points to the first input matrix structure
   * @param[in]  pSrcB  Points to the second input matrix structure
   * @param[out] pDst   Points to output matrix structure
   * @return     The function returns either @ref MCHP_MATH_SIZE_MISMATCH or @ref MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_sub_f32(
    const mchp_matrix_instance_f32 * pSrcA,
    const mchp_matrix_instance_f32 * pSrcB,
          mchp_matrix_instance_f32 * pDst);

  /**
   * @brief Single-precision floating-point matrix multiplication
   * @param[in]  pSrcA  Points to the first input matrix structure
   * @param[in]  pSrcB  Points to the second input matrix structure
   * @param[out] pDst   Points to output matrix structure
   * @return     The function returns either @ref MCHP_MATH_SIZE_MISMATCH or @ref MCHP_MATH_SUCCESS based on the outcome of size checking.
   * 
   * @note  
   * - pSrcA columns must match pSrcB rows.
   */
  mchp_status mchp_mat_mult_f32(
    const mchp_matrix_instance_f32 * pSrcA,
    const mchp_matrix_instance_f32 * pSrcB,
          mchp_matrix_instance_f32 * pDst);

  /**
   * @brief Single-precision floating-point matrix transpose.
   * @param[in]  pSrc  Points to the input matrix
   * @param[out] pDst  Points to the output matrix
   * @return     The function returns either @ref MCHP_MATH_SIZE_MISMATCH or @ref MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_trans_f32(
    const mchp_matrix_instance_f32 * pSrc,
          mchp_matrix_instance_f32 * pDst);

  /**
   * @brief Single-precision floating-point matrix scaling.
   * @param[in]  pSrc   Points to the input matrix
   * @param[in]  scale  Scale factor
   * @param[out] pDst   Points to the output matrix
   * @return     The function returns either @ref MCHP_MATH_SIZE_MISMATCH or @ref MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_scale_f32(
    const mchp_matrix_instance_f32 * pSrc,
          float32_t scale,
          mchp_matrix_instance_f32 * pDst);

  /**
   * @brief Single-precision floating-point matrix inverse.
   * 
   * Computes the inverse of a square single-precision floating-point matrix.
   * 
   * @param[in]  src   Points to the instance of the input single-precision floating-point matrix structure.
   * @param[out] dst   Points to the instance of the output single-precision floating-point matrix structure.
   * @return 
   * - @ref MCHP_MATH_SUCCESS        Matrix inversion successful.
   * - @ref MCHP_MATH_SIZE_MISMATCH  Dimensions do not match.
   * - @ref MCHP_MATH_SINGULAR       Input matrix is singular (non-invertible).
   *
   * @note
   * - Input matrix must be square
   */
  mchp_status mchp_mat_inverse_f32(
    const mchp_matrix_instance_f32 *src,
    mchp_matrix_instance_f32 *dst);

  /**
   * @brief Instance structure for the Q31 matrix structure.
   */
   typedef struct
   {
     uint16_t numRows;     /**< number of rows of the matrix.     */
     uint16_t numCols;     /**< number of columns of the matrix.  */
     q31_t *pData;         /**< points to the data of the matrix. */
   } mchp_matrix_instance_q31;

  /**
   * @brief  Q31 matrix initialization.
   *
   * @note The init function is shared with f32 since the struct layout is identical
   *       (both have {uint16_t numRows, uint16_t numCols, pointer pData}).
   *       Use mchp_mat_init_q31() which maps to mchp_mat_init_f32() via macro.
   */
   #define mchp_mat_init_q31(S, nRows, nColumns, pData) \
       mchp_mat_init_f32((mchp_matrix_instance_f32 *)(void *)(S), \
                          (nRows), (nColumns), (float32_t *)(void *)(pData))

  /**
   * @brief Q31 matrix addition.
   * @param[in]  pSrcA  points to the first input matrix structure
   * @param[in]  pSrcB  points to the second input matrix structure
   * @param[out] pDst   points to output matrix structure
   * @return     The function returns either MCHP_MATH_SIZE_MISMATCH or MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
   mchp_status mchp_mat_add_q31(
    const mchp_matrix_instance_q31 * pSrcA,
    const mchp_matrix_instance_q31 * pSrcB,
            mchp_matrix_instance_q31 * pDst);

  /**
   * @brief Q31 matrix subtraction
   * @param[in]  pSrcA  points to the first input matrix structure
   * @param[in]  pSrcB  points to the second input matrix structure
   * @param[out] pDst   points to output matrix structure
   * @return     The function returns either MCHP_MATH_SIZE_MISMATCH or MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_sub_q31(
    const mchp_matrix_instance_q31 * pSrcA,
    const mchp_matrix_instance_q31 * pSrcB,
          mchp_matrix_instance_q31 * pDst);

  /**
   * @brief Q31 matrix multiplication
   * @param[in]  pSrcA  points to the first input matrix structure
   * @param[in]  pSrcB  points to the second input matrix structure
   * @param[out] pDst   points to output matrix structure
   * @return     The function returns either MCHP_MATH_SIZE_MISMATCH or MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_mult_q31(
    const mchp_matrix_instance_q31 * pSrcA,
    const mchp_matrix_instance_q31 * pSrcB,
          mchp_matrix_instance_q31 * pDst);

  /**
   * @brief Q31 matrix transpose.
   * @param[in]  pSrc  points to the input matrix
   * @param[out] pDst  points to the output matrix
   * @return     The function returns either MCHP_MATH_SIZE_MISMATCH or MCHP_MATH_SUCCESS based on the outcome of size checking.
   */
  mchp_status mchp_mat_trans_q31(
    const mchp_matrix_instance_q31 * pSrc,
          mchp_matrix_instance_q31 * pDst);

  /**
   * @brief Q31 matrix scaling.
   * @param[in]  pSrc        points to the input matrix
   * @param[in]  scaleFract  fractional portion of the scale factor (Q31)
   * @param[in]  shift       number of bits to shift the result by
   * @param[out] pDst        points to the output matrix structure
   * @return     The function returns either MCHP_MATH_SIZE_MISMATCH or MCHP_MATH_SUCCESS based on the outcome of size checking.
   *
   * @par Scaling and Overflow Behavior
   *      The input data and scaleFract are in 1.31 format.
   *      These are multiplied to yield a 2.62 intermediate result
   *      which is shifted with saturation to 1.31 format.
   *      pDst[n] = (pSrc[n] * scaleFract) << (shift + 1) >> 32
   */
  mchp_status mchp_mat_scale_q31(
    const mchp_matrix_instance_q31 * pSrc,
          q31_t scaleFract,
          int32_t shift,
          mchp_matrix_instance_q31 * pDst);

  /**
   * @brief Q31 matrix inverse.
   * @param[in]  src   points to the instance of the input Q31 matrix structure.
   * @param[out] dst   points to the instance of the output Q31 matrix structure.
   * @return The function returns MCHP_MATH_SIZE_MISMATCH, if the dimensions do not match.
   * If the input matrix is singular (does not have an inverse), then the algorithm terminates and returns error status MCHP_MATH_SINGULAR.
   */
  mchp_status mchp_mat_inverse_q31(
    const mchp_matrix_instance_q31 * src,
    mchp_matrix_instance_q31 * dst);

/** 
 * @} 
 */ 
/* end of groupMatrix */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef MATRIX_FUNCTIONS_H_ */
