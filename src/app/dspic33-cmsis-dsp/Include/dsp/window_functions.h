/******************************************************************************
 * @file     window_functions.h
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

#ifndef WINDOW_FUNCTIONS_H_
#define WINDOW_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @defgroup groupWindow Window Functions
 * @brief Window Functions for dspic33-cmsis-dsp Library
 *
 * This group of functions provides the APIs for windowing functions in the
 * dspic33-cmsis-dsp Library.
 *
 * It includes functions for single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities like window sequences, including Bartlett, 
 * Hamming, and Hanning windows. These window functions are essential for spectral 
 * analysis, filter design, and other digital signal processing applications.
 *
 * Features:
 * - Bartlett window generation
 * - Hamming window generation
 * - Hanning window generation
 * - Includes key window parameters for reference
 *
 * Usage Example:
 * @code
 * float32_t window[SIZE];
 * mchp_hamming_f32(window, SIZE);
 * @endcode
 * 
 * @{
 */

 /**
   * @brief Bartlett window (Single-Precision Floating-Point).
   *
   * Generates a triangular (Bartlett) window.
   *
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Bartlett windows provide moderate spectral leakage reduction
   *       with minimal computational complexity.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           26.5 dB  |
   * | Normalized equivalent noise bandwidth |       1.3333 bins  |
   * | Flatness                              |        -1.8242 dB  |
   * | Recommended overlap                   |            50.0 %  |
   * @endparblock
   */
  void mchp_bartlett_f32(
        float32_t * pDst,
        uint32_t blockSize);


 /**
   * @brief Hamming window (Single-Precision Floating-Point).
   *
   * Generates a Hamming window optimized for sidelobe suppression.
   *
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Hamming windows are commonly used for FFT-based
   *       spectral estimation with good sidelobe attenuation.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           42.7 dB  |
   * | Normalized equivalent noise bandwidth |       1.3628 bins  |
   * | Flatness                              |        -1.7514 dB  |
   * | Recommended overlap                   |              50 %  |
   * @endparblock
   *
   */
  void mchp_hamming_f32(
        float32_t * pDst,
        uint32_t blockSize);


 /**
   * @brief Hanning window (Single-Precision Floating-Point).
   *
   * Generates a Hann (Hanning) window.
   *
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Hann windows provide a good compromise between
   *       frequency resolution and leakage reduction.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           31.5 dB  |
   * | Normalized equivalent noise bandwidth |          1.5 bins  |
   * | Flatness                              |        -1.4236 dB  |
   * | Recommended overlap                   |              50 %  |
   * @endparblock
   * 
   */
  void mchp_hanning_f32(
        float32_t * pDst,
        uint32_t blockSize);

 /**
   * @brief Bartlett window (Q31).
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Bartlett windows provide moderate spectral leakage reduction
   *       with minimal computational complexity.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           26.5 dB  |
   * | Normalized equivalent noise bandwidth |       1.3333 bins  |
   * | Flatness                              |        -1.8242 dB  |
   * | Recommended overlap                   |            50.0 %  |
   * @endparblock
   */
  void mchp_bartlett_q31(
        q31_t * pDst,
        uint32_t blockSize);

 /**
   * @brief Hamming window (Q31).
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Hamming windows are commonly used for FFT-based
   *       spectral estimation with good sidelobe attenuation.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           42.7 dB  |
   * | Normalized equivalent noise bandwidth |       1.3628 bins  |
   * | Flatness                              |        -1.7514 dB  |
   * | Recommended overlap                   |              50 %  |
   * @endparblock
   *
   */
  void mchp_hamming_q31(
        q31_t * pDst,
        uint32_t blockSize);

 /**
   * @brief Hanning window (Q31).
   * @param[out] pDst       points to the output generated window
   * @param[in]  blockSize  number of samples in the window
   *
   * @note Hann windows provide a good compromise between
   *       frequency resolution and leakage reduction.
   *
   * @parblock 
   * Parameters of the window
   * 
   * | Parameter                             | Value              |
   * | ------------------------------------: | -----------------: |
   * | Peak sidelobe level                   |           31.5 dB  |
   * | Normalized equivalent noise bandwidth |          1.5 bins  |
   * | Flatness                              |        -1.4236 dB  |
   * | Recommended overlap                   |              50 %  |
   * @endparblock
   * 
   */
  void mchp_hanning_q31(
        q31_t * pDst,
        uint32_t blockSize);

/** 
 * @} 
 */ 
/* end of groupWindow */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef WINDOW_FUNCTIONS_H_ */
