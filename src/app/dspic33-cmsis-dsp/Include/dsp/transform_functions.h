/******************************************************************************
 * @file     transform_functions.h
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

#ifndef TRANSFORM_FUNCTIONS_H_
#define TRANSFORM_FUNCTIONS_H_

#include "../mchp_math_types.h"
#include "../mchp_dsp_config.h"

#ifdef   __cplusplus
extern "C"
{
#endif


/**
 * @defgroup groupTransforms Transform Functions
 * @brief FFT and related transform functions for dspic33-cmsis-dsp.
 *
 * This group of functions provides the APIs for transform operations
 * in the dspic33-cmsis-dsp Library.
 * 
 * The implementations of complex FFT (CFFT) and real FFT (RFFT)
 * optimized for Microchip architectures. Depending on the device and instruction
 * set capabilities, different algorithmic variants may be used to achieve optimal
 * performance.
 *
 * Although the implementations may differ internally across architectures,
 * all variants produce results that meet the same verification requirements,
 * including identical SNR thresholds and error tolerances.
 *
 * @par Initialization
 * Transform instances must be initialized before use. Refer to the specific
 * initialization functions such as ::mchp_cfft_init_f32 for details.
 *
 * @par Buffer Requirements
 * The required buffer sizes depend on both the FFT length and the target
 * architecture. Detailed information is provided on the documentation page
 * @ref transformbuffers "Transform Buffers".
 *
 * @{
 */

/**
* @brief Instance structure for the single-precision floating-point CFFT/CIFFT function.
*
* This structure holds all configuration information required to
* execute a complex FFT or inverse FFT.
*
* @note The pBitRevTable and bitRevLength members are retained for
*       CMSIS-DSP compatibility but are not used internally.
*/
typedef struct
{
      uint32_t fftLen;                   /**< FFT length in complex samples */
const float32_t *pTwiddle;         /**< Pointer to twiddle-factor table */
const uint16_t *pBitRevTable;      /**< points to the bit reversal table */
      uint16_t bitRevLength;             /**< bit reversal table length */
} mchp_cfft_instance_f32;

/**
* @brief Initialize a single-precision floating-point complex FFT instance.
*
* This function selects the appropriate twiddle-factor table and
* initializes the FFT instance based on fftLen.
*
* @param[out] S          Pointer to instance structure.
* @param[in]  fftLen     FFT length (e.g., 128, 256, 2048).
*
* @note
* Twiddle table availability is controlled entirely by build-time
* macros such as:
*   USE_FFT_LEN_1024
*
* Example:
* @code
*    mchp_cfft_instance_f32 S1, S2;
*    mchp_cfft_init_f32(&S1, 128);
*    mchp_cfft_init_f32(&S2, 256);
* @endcode
*
* @return 
* - @ref MCHP_MATH_SUCCESS        Initialization successful
* - @ref MCHP_MATH_ARGUMENT_ERROR Unsupported FFT length
*/
mchp_status mchp_cfft_init_f32(
mchp_cfft_instance_f32 * S,
uint16_t fftLen);

/**
 * @brief Processing function for the single-precision floating-point complex FFT.
 *
 * Performs an in-place forward or inverse FFT on complex data.
 *
 * @param[in]     S              FFT instance structure
 * @param[in,out] p1             Pointer to interleaved complex buffer
 * @param[in]     ifftFlag       Transform direction:
 *                               0 = forward FFT
 *                               1 = inverse FFT
 * @param[in]     bitReverseFlag Enables or disables output bit reversal
 *
 * @note
 * - No bit-reversal table is used internally.
 * - bitReverseFlag is retained for CMSIS-DSP compatibility.
 */
void mchp_cfft_f32(
const mchp_cfft_instance_f32 * S,
      float32_t * p1,
      uint8_t ifftFlag,
      uint8_t bitReverseFlag);

/**
* @brief Instance structure for the single-precision floating-point RFFT/RIFFT function.
*
* This structure wraps an internal complex FFT instance and
* configuration information specific to real-valued FFTs.
*
* @note pTwiddleRFFT is retained for compatibility but not used.
*/
typedef struct
{
    mchp_cfft_instance_f32 Sint;      /**< Internal complex FFT instance */
    uint16_t fftLenRFFT;             /**< Length of real-valued sequence */
    const float32_t * pTwiddleRFFT;  /**< Unused (API compatibility) */
} mchp_rfft_fast_instance_f32 ;

/**
 * @brief Initialize a single-precision floating-point real FFT instance.
 *
 * @param[in,out] S       RFFT instance structure
 * @param[in]     fftLen  Length of the real input sequence
 *
 * @return 
 * - @ref MCHP_MATH_SUCCESS        Initialization successful
 * - @ref MCHP_MATH_ARGUMENT_ERROR Unsupported FFT length
 *
 * @note
 * Supported FFT lengths:
 * 32, 64, 128, 256, 512, 1024, 2048, 4096
 *
 * Only the twiddle data for the selected FFT length is compiled in.
 *
 * When FFT sizes are known at build time, it is recommended to enable
 * only the required `USE_FFT_LEN_xxx` macro so the compiler pulls in
 * the correct twiddle-factor table and eliminates unused data.
 */
mchp_status mchp_rfft_fast_init_f32 (
         mchp_rfft_fast_instance_f32 * S,
         uint16_t fftLen);

/**
 * @brief Processing function for the single-precision floating-point real FFT.
 *
 * Performs a real FFT or inverse real FFT using the
 * configuration stored in the `mchp_rfft_fast_instance_f32`.
 *
 * @param[in]     S         Points to an mchp_rfft_fast_instance_f32 structure
 * @param[in,out] p         Points to the input buffer (modified during processing)
 * @param[out]    pOut      Points to the output buffer
 * @param[in]     ifftFlag  Transform direction:
 *                          - 0 : Forward Real FFT (RFFT)
 *                          - 1 : Inverse Real FFT (RIFFT)
 *
 * @note
 * Internally uses a complex FFT of length fftLen / 2 followed by a split 
 * or merge operation depending on the transform direction.
 */
void mchp_rfft_fast_f32(
    const mchp_rfft_fast_instance_f32 * S,
    float32_t * p, float32_t * pOut,
    uint8_t ifftFlag);  


/**
 * @brief         In-place single-precision floating-point bit reversal function.
 * @param[in,out] pSrc         points to in-place single-precision floating-point data buffer
 * @param[in]     fftSize      length of FFT
 * @param[in]     bitRevFactor bit reversal modifier that supports different size FFTs with the same bit reversal table.
 * @param[in]     pBitRevTab   points to the bit reversal table.
 *
 * @note
 * The parameters @p bitRevFactor and @p pBitRevTab are ignored.
 */
MCHP_DSP_ATTRIBUTE void mchp_bitreversal_f32(
        float32_t * pSrc,
        uint16_t fftSize,
        uint16_t bitRevFactor,
  const uint16_t * pBitRevTab);

/* ===================================================================
 *  Q31 (fixed-point) Transform Functions
 * =================================================================== */

/**
 * @brief Instance structure for the Q31 CFFT/CIFFT function.
 *
 * Layout expected by the Q31 CFFT assembly (mchp_cfft_q31.s):
 *   offset 0 : fftLen   (uint32_t)
 *   offset 4 : pTwiddle (const q31_t *)
 */
typedef struct
{
      uint32_t fftLen;                   /**< length of the FFT. */
const q31_t *pTwiddle;                   /**< points to the Twiddle factor table. */
} mchp_cfft_instance_q31;

/**
 * @brief Initialize a Q31 complex FFT instance.
 *
 * @param[out] S          Pointer to instance structure.
 * @param[in]  fftLen     FFT length (e.g., 128, 256, 1024).
 *
 * @return MCHP_MATH_SUCCESS          Operation successful.
 * @return MCHP_MATH_ARGUMENT_ERROR   fftLen is not a supported length.
 */
mchp_status mchp_cfft_init_q31(
    mchp_cfft_instance_q31 * S,
    uint16_t fftLen);

/**
 * @brief Processing function for the Q31 complex FFT.
 *
 * @param[in]     S              Points to an instance of the Q31 CFFT structure.
 * @param[in,out] pSrc           Points to the complex data buffer (in-place).
 * @param[in]     ifftFlag       0 = forward FFT, 1 = inverse FFT.
 * @param[in]     bitReverseFlag 0 = no bit reversal, 1 = enable bit reversal.
 *
 * @note The CFFT applies 1/2 scaling per stage, so the output is scaled
 *       by 1/N for the forward transform.
 */
void mchp_cfft_q31(
    const mchp_cfft_instance_q31 * S,
          q31_t * pSrc,
          uint8_t ifftFlag,
          uint8_t bitReverseFlag);

/**
 * @brief Instance structure for the Q31 RFFT/RIFFT function.
 *
 * Layout expected by the Q31 RFFT assembly (mchp_rfft_fast_q31.s):
 *   offset  0 : fftLenRFFT     (uint32_t)
 *   offset  4 : ifftFlagR      (uint32_t)
 *   offset  8 : pCfft          (mchp_cfft_instance_q31 *)
 *   offset 12 : pTwiddle       (const q31_t *)
 *   offset 16 : pTwiddleRFFT   (const q31_t *)
 */
typedef struct
{
      uint32_t fftLenRFFT;                        /**< length of the real sequence. */
      uint32_t ifftFlagR;                         /**< flag: 0 = forward RFFT, 1 = inverse RFFT. */
      mchp_cfft_instance_q31 *pCfft;              /**< pointer to internal CFFT instance. */
const q31_t *pTwiddle;                             /**< points to the CFFT twiddle factor table. */
const q31_t *pTwiddleRFFT;                         /**< points to the real FFT split twiddle factors. */
} mchp_rfft_instance_q31;

/**
 * @brief Initialize a Q31 real FFT instance (ARM-compatible signature).
 *
 * @param[in,out] S              Points to an mchp_rfft_instance_q31 structure.
 * @param[in]     fftLenReal     Length of the real sequence.
 * @param[in]     ifftFlagR      0 = forward RFFT, 1 = inverse RFFT.
 * @param[in]     bitReverseFlag 0 = disable bit reversal, 1 = enable (always enabled).
 *
 * @return MCHP_MATH_SUCCESS          Operation successful.
 * @return MCHP_MATH_ARGUMENT_ERROR   fftLenReal is not a supported length.
 */
mchp_status mchp_rfft_init_q31(
    mchp_rfft_instance_q31 * S,
    uint32_t fftLenReal,
    uint32_t ifftFlagR,
    uint32_t bitReverseFlag);

/**
 * @brief Processing function for the Q31 real FFT (ARM-compatible signature).
 *
 * @param[in]     S         Points to an mchp_rfft_instance_q31 structure.
 * @param[in,out] pSrc      Points to the input buffer (modified by this function).
 * @param[out]    pDst      Points to the output buffer.
 *
 * @note The transform direction (forward/inverse) is determined by the
 *       ifftFlagR field set during initialization.
 */
void mchp_rfft_q31(
    const mchp_rfft_instance_q31 * S,
    q31_t * pSrc,
    q31_t * pDst);

/**
 * @brief In-place Q31 bit reversal function.
 *
 * @param[in,out] pSrc     Points to in-place Q31 data buffer.
 * @param[in]     fftSize  Length of FFT.
 * @param[in]     bitRevFactor Not used (API compatibility).
 * @param[in]     pBitRevTab   Not used (API compatibility).
 * @par
 */
MCHP_DSP_ATTRIBUTE void mchp_bitreversal_q31(
        q31_t * pSrc,
        uint16_t fftSize,
        uint16_t bitRevFactor,
  const uint16_t * pBitRevTab);

/** 
 * @} 
 */ /* end of groupTransforms */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef TRANSFORM_FUNCTIONS_H_ */
