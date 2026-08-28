/******************************************************************************
 * @file     filtering_functions.h
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

/**
 * @defgroup groupFilters Filtering Functions
 */

#ifndef FILTERING_FUNCTIONS_H_
#define FILTERING_FUNCTIONS_H_


#include "../mchp_math_types.h"

#ifdef    __cplusplus
extern "C" {
#endif

/**
 * @defgroup groupFilters Filtering Functions
 * @brief Filtering Functions for dspic33-cmsis-dsp Library
 *
 * This group of functions defines the APIs for filtering functions in the
 * dspic33-cmsis-dsp Library.
 *
 * The function provides data structures and functions for implementing a wide range
 * of single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities like digital filters, including FIR, FIR decimator,
 * FIR interpolator, FIR lattice, LMS, normalized LMS, IIR lattice, and biquad cascade filters.
 *
 * Features:
 * - FIR filter, FIR decimator, FIR interpolator, FIR lattice filter
 * - LMS and normalized LMS adaptive filters
 * - IIR lattice filter
 * - Transposed direct form II biquad cascade filter
 * - Initialization and processing functions for each filter type
 *
 * Usage Example:
 * @code
 * mchp_fir_instance_f32 fir;
 * mchp_fir_init_f32(&fir, numTaps, coeffs, state, blockSize);
 * mchp_fir_f32(&fir, input, output, blockSize);
 * @endcode
 *
 * @{
 */


/**
 * @brief Instance structure for the single-precision floating-point FIR filter.
 *
 * The FIR filter operates using a circular delay buffer and
 * performs block-based convolution with fixed coefficients.
 */
typedef struct  
{
    uint16_t numTaps;           /** Number of filter coefficients in the filter */
    float32_t *pState;          /** Points to the state variable array (length = numTaps + blockSize - 1) */
    const float32_t *pCoeffs;   /** Points to the coefficient array (length = numTaps) */
    float32_t *pStateStart;     /** Points to the start of delay buffer */
} mchp_fir_instance_f32 __ALIGNED(4);

/**
 * @brief Initialization function for the single-precision floating-point FIR filter.
 * 
 * Initializes state and coefficient pointers and clears the delay line.
 * 
 * @param[in,out] S          Points to an instance of the single-precision floating-point FIR filter structure
 * @param[in]     numTaps    Number of filter coefficients in the filter
 * @param[in]     pCoeffs    Points to the filter coefficients array
 * @param[in]     pState     Points to the state variable array
 * @param[in]     blockSize  Number of samples to process
 * @par
 * 
 */
void mchp_fir_init_f32(
        mchp_fir_instance_f32 * S,
        uint16_t numTaps,
const   float32_t * pCoeffs,
        float32_t * pState,
        uint32_t blockSize);

/**
 * @brief Processing function for the single-precision floating-point FIR filter.
 *
 * Applies FIR filtering to a block of input samples.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point FIR structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[out] pDst       Points to the block of output data
 * @param[in]  blockSize  Number of samples to process
 * @par
 * 
 */
void mchp_fir_f32(
const mchp_fir_instance_f32 * S,
const float32_t * pSrc,
      float32_t * pDst,
      uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point FIR decimator.
 *
 * Performs low-pass filtering followed by downsampling.
 */
typedef struct 
{
          uint8_t M;                  /** Decimation factor */
          uint16_t numTaps;           /** Number of coefficients */
    const float32_t *pCoeffs;         /** Points to the coefficient array (length = numTaps) */
          float32_t *pState;          /** Points to the state variable array (length = numTaps + blockSize - 1) */
} mchp_fir_decimate_instance_f32 __ALIGNED(4);

/**
 * @brief         Initialization function for the single-precision floating-point FIR decimator.
 * 
 * @param[in,out] S          Points to an instance of the single-precision floating-point FIR decimator structure
 * @param[in]     numTaps    Number of coefficients in the filter
 * @param[in]     M          Decimation factor
 * @param[in]     pCoeffs    Points to the filter coefficients
 * @param[in]     pState     Points to the state variable array
 * @param[in]     blockSize  Number of samples to process
 * @par
 * 
 * @return
 * - @ref MCHP_MATH_SUCCESS if success
 * - @ref MCHP_MATH_LENGTH_ERROR if <code>blockSize</code> is not a multiple of <code>M</code>
 */
mchp_status mchp_fir_decimate_init_f32(
        mchp_fir_decimate_instance_f32 * S,
        uint16_t numTaps,
        uint8_t M,
  const float32_t * pCoeffs,
        float32_t * pState,
        uint32_t blockSize);

/**
 * @brief         Processing function for single-precision floating-point FIR decimator.
 * 
 * @param[in]     S         Points to an instance of the single-precision floating-point FIR decimator structure
 * @param[in]     pSrc      Points to the block of input data
 * @param[out]    pDst      Points to the block of output data
 * @param[in]     blockSize Number of samples to process
 * @par
 */
void mchp_fir_decimate_f32(
  const mchp_fir_decimate_instance_f32 * S,
  const float32_t * pSrc,
        float32_t * pDst,
        uint32_t blockSize);

/**
  @brief Instance structure for Q31 FIR decimator.
 */
typedef struct
  {
          uint8_t M;                  /**< decimation factor. */
          uint16_t numTaps;           /**< number of coefficients in the filter. */
    const q31_t *pCoeffs;             /**< points to the coefficient array. The array is of length numTaps.*/
          q31_t *pState;              /**< points to the state variable array. The array is of length numTaps+blockSize-1. */
  } mchp_fir_decimate_instance_q31 __ALIGNED(4);

/**
  @brief         Initialization function for the Q31 FIR decimator.
  @param[in,out] S          points to an instance of the Q31 FIR decimator structure
  @param[in]     numTaps    number of coefficients in the filter
  @param[in]     M          decimation factor
  @param[in]     pCoeffs    points to the filter coefficients
  @param[in]     pState     points to the state buffer
  @param[in]     blockSize  number of input samples to process per call
  @return        execution status
                   - \ref MCHP_MATH_SUCCESS      : Operation successful
                   - \ref MCHP_MATH_LENGTH_ERROR : <code>blockSize</code> is not a multiple of <code>M</code>
 */
mchp_status mchp_fir_decimate_init_q31(
        mchp_fir_decimate_instance_q31 * S,
        uint16_t numTaps,
        uint8_t M,
  const q31_t * pCoeffs,
        q31_t * pState,
        uint32_t blockSize);

/**
 * @brief         Processing function for Q31 FIR decimator.
 * @param[in]     S         points to an instance of the Q31 FIR decimator structure
 * @param[in]     pSrc      points to the block of input data
 * @param[out]    pDst      points to the block of output data
 * @param[in]     blockSize number of samples to process
 * @par
 */
void mchp_fir_decimate_q31(
  const mchp_fir_decimate_instance_q31 * S,
  const q31_t * pSrc,
        q31_t * pDst,
        uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point FIR interpolator.
 *
 * Performs upsampling followed by polyphase FIR filtering.
 */
typedef struct 
{
        uint8_t L;                     /** Upsample factor */
        uint16_t phaseLength;          /** Length of each polyphase filter component */
    const float32_t *pCoeffs;          /** Coefficient array (length = L * phaseLength)  */
        float32_t *pState;             /** State varriable array (length = phaseLength + numTaps - 1)  */
} mchp_fir_interpolate_instance_f32 __ALIGNED(4);

/**
 * @brief  Initialization function for the single-precision floating-point FIR interpolator.
 * 
 * @param[in,out] S          Points to an instance of the single-precision floating-point FIR interpolator structure
 * @param[in]     L          Upsample factor
 * @param[in]     numTaps    Number of filter coefficients in the filter
 * @param[in]     pCoeffs    Points to the filter coefficient array
 * @param[in]     pState     Points to the state variable array
 * @param[in]     blockSize  Number of input samples to process per call
 * @par
 * @return
 * - @ref MCHP_MATH_SUCCESS if initialization is successful 
 * - @ref MCHP_MATH_LENGTH_ERROR if the filter length <code>numTaps</code> is not a 
 * multiple of the interpolation factor <code>L</code>.
 */
mchp_status mchp_fir_interpolate_init_f32(
        mchp_fir_interpolate_instance_f32 * S,
        uint8_t L,
        uint16_t numTaps,
  const float32_t * pCoeffs,
        float32_t * pState,
        uint32_t blockSize);

/**
 * @brief Processing function for the single-precision floating-point FIR interpolator.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point FIR interpolator structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[out] pDst       Points to the block of output data
 * @param[in]  blockSize  Number of input samples to process
 * @par
 */
void mchp_fir_interpolate_f32(
    const mchp_fir_interpolate_instance_f32 * S,
    const float32_t * pSrc,
        float32_t * pDst,
        uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point FIR lattice filter.
 */
typedef struct 
{
        uint16_t numStages;                  /** Number of filter stages */
        float32_t *pState;                   /** State variable array (length = numStages) */
    const float32_t *pCoeffs;                /** Coefficient array (length = numStages) */
} mchp_fir_lattice_instance_f32 __ALIGNED(4);

/**
 * @brief Initialization function for the single-precision floating-point FIR lattice filter.
 * 
 * @param[in] S          Points to an instance of the single-precision floating-point FIR lattice structure
 * @param[in] numStages  Number of filter stages
 * @param[in] pCoeffs    Points to the coefficient array (length = numStages)
 * @param[in] pState     Points to the state variable array (length = numStages)
 * @par
 */
void mchp_fir_lattice_init_f32(
        mchp_fir_lattice_instance_f32 * S,
        uint16_t numStages,
  const float32_t * pCoeffs,
        float32_t * pState);

/**
 * @brief Processing function for the single-precision floating-point FIR lattice filter.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point FIR lattice structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[out] pDst       Points to the block of output data
 * @param[in]  blockSize  Number of samples to process
 * @par
 */
void mchp_fir_lattice_f32(
    const mchp_fir_lattice_instance_f32 * S,
    const float32_t * pSrc,
        float32_t * pDst,
        uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point LMS filter.
 */
typedef struct 
{
        uint16_t numTaps;        /** Number of coefficients */
        float32_t *pState;       /** Points to the state variable array (length = numTaps + blockSize - 1) */
        float32_t *pCoeffs;      /** Points to the coefficient array (length =  numTaps) */
        float32_t mu;            /** Step size that controls filter coefficient updates */
        float32_t *pStateStart;  /** Points to the start of delay buffer */
} mchp_lms_instance_f32 __ALIGNED(4);

/**
 * @brief Initialization function for single-precision floating-point LMS filter.
 * 
 * @param[in] S          Points to an instance of the single-precision floating-point LMS filter structure
 * @param[in] numTaps    Number of filter coefficients
 * @param[in] pCoeffs    Points to the coefficient array
 * @param[in] pState     Points to state variable array
 * @param[in] mu         Step size that controls filter coefficient updates
 * @param[in] blockSize  Number of samples to process
 * @par
 */
void mchp_lms_init_f32(
        mchp_lms_instance_f32 * S,
        uint16_t numTaps,
        float32_t * pCoeffs,
        float32_t * pState,
        float32_t mu,
        uint32_t blockSize);

/**
 * @brief Processing function for single-precision floating-point LMS filter.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point LMS filter structure.
 * @param[in]  pSrc       Points to the block of input data
 * @param[in]  pRef       Points to the block of reference data
 * @param[out] pOut       Points to the block of output data
 * @param[out] pErr       Points to the block of error data
 * @param[in]  blockSize  Number of samples to process
 * @par
 */
void mchp_lms_f32(
  const mchp_lms_instance_f32 * S,
  const float32_t * pSrc,
        float32_t * pRef,
        float32_t * pOut,
        float32_t * pErr,
        uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point normalized LMS filter.
 */
typedef struct
{
        uint16_t numTaps;       /** Number of coefficients in the filter. */
        float32_t *pState;      /** Points to the state variable array (length = numTaps + blockSize - 1) */
        float32_t *pCoeffs;     /** Points to the coefficient array (length = numTaps) */
        float32_t mu;           /** Step size that control filter coefficient updates */
        float32_t energy;       /** Saves previous frame energy */
        float32_t x0;           /** Saves previous input sample */
        float32_t *pStateStart; /** Points to the start of delay buffer */
} mchp_lms_norm_instance_f32 __ALIGNED(4);

/**
 * @brief Initialization function for single-precision floating-point normalized LMS filter.
 * 
 * @param[in] S          Points to an instance of the single-precision floating-point LMS filter structure
 * @param[in] numTaps    Number of filter coefficients
 * @param[in] pCoeffs    Points to coefficient array
 * @param[in] pState     Points to state variable array
 * @param[in] mu         Step size that controls filter coefficient updates
 * @param[in] blockSize  Number of samples to process
 * @par
 */
void mchp_lms_norm_init_f32(
        mchp_lms_norm_instance_f32 * S,
        uint16_t numTaps,
        float32_t * pCoeffs,
        float32_t * pState,
        float32_t mu,
        uint32_t blockSize);

/**
 * @brief Processing function for single-precision floating-point normalized LMS filter.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point normalized LMS filter structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[in]  pRef       Points to the block of reference data
 * @param[out] pOut       Points to the block of output data
 * @param[out] pErr       Points to the block of error data
 * @param[in]  blockSize  Number of samples to process
 * @par
 */
void mchp_lms_norm_f32(
        mchp_lms_norm_instance_f32 * S,
  const float32_t * pSrc,
        float32_t * pRef,
        float32_t * pOut,
        float32_t * pErr,
        uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point IIR lattice filter.
 */
typedef struct 
{
        uint16_t numStages;                  /** Number of filter stages */
        float32_t *pState;                   /** Points to the state variable array (length = numStages + blockSize) */
        float32_t *pkCoeffs;                 /** Points to the reflection coefficient array (length = numStages) */
        float32_t *pvCoeffs;                 /** Points to the ladder coefficient array(length = numStages + 1) */
} mchp_iir_lattice_instance_f32 __ALIGNED(4);

/**
 * @brief Initialization function for the single-precision floating-point IIR lattice filter.
 * 
 * @param[in] S          Points to an instance of the single-precision floating-point IIR lattice structure
 * @param[in] numStages  Number of filter stages
 * @param[in] pkCoeffs   Points to the reflection coefficient array (length = numStages)
 * @param[in] pvCoeffs   Points to the ladder coefficient array (length = numStages + 1)
 * @param[in] pState     Points to the state variable array (length = numStages + blockSize - 1)
 * @param[in] blockSize  Number of samples to process
 * @par
 */
void mchp_iir_lattice_init_f32(
      mchp_iir_lattice_instance_f32 * S,
      uint16_t numStages,
      float32_t * pkCoeffs,
      float32_t * pvCoeffs,
      float32_t * pState,
      uint32_t blockSize);

/**
 * @brief Processing function for the single-precision floating-point IIR lattice filter.
 * 
 * @param[in]  S          Points to an instance of the single-precision floating-point IIR lattice structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[out] pDst       Points to the block of output data
 * @param[in]  blockSize  Number of samples to process
 * @par
 */
void mchp_iir_lattice_f32(
const mchp_iir_lattice_instance_f32 * S,
const float32_t * pSrc,
      float32_t * pDst,
      uint32_t blockSize);

/**
 * @brief Instance structure for the single-precision floating-point transposed direct form II (DF2T) Biquad cascade filter.
 */
typedef struct
{
        uint8_t numStages;           /** Number of second-order stages in the filter (overall order = 2 * numStages) */
        float32_t *pState;           /** Points to State variable array (length = 2 * numStages) */
  const float32_t *pCoeffs;          /** Points to Coefficient array (length = 5 * numStages) */
} mchp_biquad_cascade_df2T_instance_f32;

/**
 * @brief  Initialization function for the single-precision floating-point transposed direct form II (DF2T) Biquad cascade filter.
 * 
 * @param[in,out] S          Points to an instance of the filter data structure
 * @param[in]     numStages  Number of 2nd order stages in the filter
 * @param[in]     pCoeffs    Points to coefficient array
 * @param[in]     pState     Points to state variable array
 * @par
 */
void mchp_biquad_cascade_df2T_init_f32(
      mchp_biquad_cascade_df2T_instance_f32 * S,
      uint8_t numStages,
const float32_t * pCoeffs,
      float32_t * pState);

/**
 * @brief Processing function for the single-precision floating-point transposed direct form II Biquad cascade filter.
 * 
 * @param[in]  S          Points to an instance of the filter data structure
 * @param[in]  pSrc       Points to the block of input data
 * @param[out] pDst       Points to the block of output data
 * @param[in]  blockSize  Number of samples to process.
 * @par
 */
void mchp_biquad_cascade_df2T_f32(
const mchp_biquad_cascade_df2T_instance_f32 * S,
const float32_t * pSrc,
      float32_t * pDst,
      uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 direct form I Biquad cascade filter.
 */
typedef struct
{
        uint8_t numStages;         /**< number of 2nd order stages in the filter.  Overall order is 2*numStages. */
  const q31_t *pCoeffs;            /**< points to the array of coefficients.  The array is of length 5*numStages. Layout per stage: [b0,b1,b2,a1,a2]. */
        q31_t *pState;             /**< points to the array of state coefficients.  The array is of length 4*numStages. Layout per stage: [x[n-1],x[n-2],y[n-1],y[n-2]]. */
        int8_t postShift;          /**< additional shift applied to output of each stage. */
} mchp_biquad_cascade_df1_instance_q31;

/**
 * @brief  Initialization function for the Q31 direct form I Biquad cascade filter.
 * @param[in,out] S          points to an instance of the filter data structure.
 * @param[in]     numStages  number of 2nd order stages in the filter.
 * @param[in]     pCoeffs    points to the filter coefficients.
 * @param[in]     pState     points to the state buffer.
 * @param[in]     postShift  bit shift applied to output of each stage.
 * @par
 */
void mchp_biquad_cascade_df1_init_q31(
      mchp_biquad_cascade_df1_instance_q31 * S,
      uint8_t numStages,
const q31_t * pCoeffs,
      q31_t * pState,
      int8_t postShift);

/**
 * @brief Processing function for the Q31 direct form I Biquad cascade filter.
 * @param[in]  S          points to an instance of the filter data structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[out] pDst       points to the block of output data.
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_biquad_cascade_df1_q31(
const mchp_biquad_cascade_df1_instance_q31 * S,
const q31_t * pSrc,
      q31_t * pDst,
      uint32_t blockSize);

/**
 * @brief Correlation of single-precision floating-point sequences.
 * 
 * @param[in]  pSrcA    Points to the first input sequence
 * @param[in]  srcALen  Length of the first input sequence
 * @param[in]  pSrcB    Points to the second input sequence
 * @param[in]  srcBLen  Length of the second input sequence
 * @param[out] pDst     Points to the block of output data (Length = 2 * max(srcALen, srcBLen) - 1)
 * @par
 */
 void mchp_correlate_f32(
 const float32_t * pSrcA,
       uint32_t srcALen,
 const float32_t * pSrcB,
       uint32_t srcBLen,
       float32_t * pDst);

/**
 * @brief Convolution of single-precision floating-point sequences.
 * 
 * @param[in]  pSrcA    Points to the first input sequence
 * @param[in]  srcALen  Length of the first input sequence
 * @param[in]  pSrcB    Points to the second input sequence
 * @param[in]  srcBLen  Length of the second input sequence
 * @param[out] pDst     Points to the block of output data (Length =  srcALen + srcBLen - 1)
 * @par
 */
  void mchp_conv_f32(
  const float32_t * pSrcA,
        uint32_t srcALen,
  const float32_t * pSrcB,
        uint32_t srcBLen,
        float32_t * pDst);

/**
 * @brief Correlation of Q31 sequences.
 * @param[in]  pSrcA    points to the first input sequence.
 * @param[in]  srcALen  length of the first input sequence.
 * @param[in]  pSrcB    points to the second input sequence.
 * @param[in]  srcBLen  length of the second input sequence.
 * @param[out] pDst     points to the block of output data  Length 2 * max(srcALen, srcBLen) - 1.
 *
 * @note Implementation reverses pSrcB in-place before calling convolution.
 *       The const qualifier matches the ARM CMSIS-DSP API contract; callers
 *       should not rely on pSrcB being unmodified.
 */
  void mchp_correlate_q31(
  const q31_t * pSrcA,
        uint32_t srcALen,
  const q31_t * pSrcB,
        uint32_t srcBLen,
        q31_t * pDst);

/**
 * @brief Convolution of Q31 sequences.
 * @param[in]  pSrcA    points to the first input sequence.
 * @param[in]  srcALen  length of the first input sequence.
 * @param[in]  pSrcB    points to the second input sequence.
 * @param[in]  srcBLen  length of the second input sequence.
 * @param[out] pDst     points to the location where the output result is written.  Length srcALen+srcBLen-1.
 * @par
 */
  void mchp_conv_q31(
  const q31_t * pSrcA,
        uint32_t srcALen,
  const q31_t * pSrcB,
        uint32_t srcBLen,
        q31_t * pDst);

/**
 * @brief Instance structure for the Q31 FIR filter.
 */
typedef struct
{
    uint16_t numTaps;     /**< number of filter coefficients in the filter. */
    q31_t *pState;        /**< points to the state variable array. The array is of length numTaps+blockSize-1. */
    const q31_t *pCoeffs; /**< points to the coefficient array. The array is of length numTaps. */
    q31_t *pStateStart;   /**< points to the start of delay buffer */
} mchp_fir_instance_q31 __ALIGNED(4);

/**
 * @brief  Initialization function for the Q31 FIR filter.
 * @param[in,out] S          points to an instance of the Q31 FIR filter structure.
 * @param[in]     numTaps    Number of filter coefficients in the filter.
 * @param[in]     pCoeffs    points to the filter coefficients.
 * @param[in]     pState     points to the state buffer.
 * @param[in]     blockSize  number of samples that are processed at a time.
 * @par
 */
void mchp_fir_init_q31(
        mchp_fir_instance_q31 * S,
        uint16_t numTaps,
const   q31_t * pCoeffs,
        q31_t * pState,
        uint32_t blockSize);

/**
 * @brief Processing function for the Q31 FIR filter.
 * @param[in]  S          points to an instance of the Q31 FIR structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[out] pDst       points to the block of output data.
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_fir_q31(
const mchp_fir_instance_q31 * S,
const q31_t * pSrc,
      q31_t * pDst,
      uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 FIR interpolator.
 */
typedef struct
{
        uint8_t L;                     /**< upsample factor. */
        uint16_t phaseLength;          /**< length of each polyphase filter component. */
    const q31_t *pCoeffs;              /**< points to the coefficient array. The array is of length L*phaseLength. */
        q31_t *pState;                 /**< points to the state variable array. The array is of length phaseLength+numTaps-1. */
} mchp_fir_interpolate_instance_q31 __ALIGNED(4);

/**
 * @brief  Initialization function for the Q31 FIR interpolator.
 * @param[in,out] S          points to an instance of the Q31 FIR interpolator structure.
 * @param[in]     L          upsample factor.
 * @param[in]     numTaps    number of filter coefficients in the filter.
 * @param[in]     pCoeffs    points to the filter coefficient buffer.
 * @param[in]     pState     points to the state buffer.
 * @param[in]     blockSize  number of input samples to process per call.
 * @return        execution status
 */
mchp_status mchp_fir_interpolate_init_q31(
        mchp_fir_interpolate_instance_q31 * S,
        uint8_t L,
        uint16_t numTaps,
  const q31_t * pCoeffs,
        q31_t * pState,
        uint32_t blockSize);

/**
 * @brief Processing function for the Q31 FIR interpolator.
 * @param[in]  S          points to an instance of the Q31 FIR interpolator structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[out] pDst       points to the block of output data.
 * @param[in]  blockSize  number of input samples to process per call.
 * @par
 */
void mchp_fir_interpolate_q31(
    const mchp_fir_interpolate_instance_q31 * S,
    const q31_t * pSrc,
        q31_t * pDst,
        uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 FIR lattice filter.
 */
typedef struct
{
        uint16_t numStages;            /**< number of filter stages. */
        q31_t *pState;                 /**< points to the state variable array. The array is of length numStages. */
    const q31_t *pCoeffs;              /**< points to the coefficient array. The array is of length numStages. */
} mchp_fir_lattice_instance_q31 __ALIGNED(4);

/**
 * @brief Initialization function for the Q31 FIR lattice filter.
 * @param[in] S          points to an instance of the Q31 FIR lattice structure.
 * @param[in] numStages  number of filter stages.
 * @param[in] pCoeffs    points to the coefficient buffer.  The array is of length numStages.
 * @param[in] pState     points to the state buffer.  The array is of length numStages.
 * @par
 */
void mchp_fir_lattice_init_q31(
        mchp_fir_lattice_instance_q31 * S,
        uint16_t numStages,
  const q31_t * pCoeffs,
        q31_t * pState);

/**
 * @brief Processing function for the Q31 FIR lattice filter.
 * @param[in]  S          points to an instance of the Q31 FIR lattice structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[out] pDst       points to the block of output data
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_fir_lattice_q31(
    const mchp_fir_lattice_instance_q31 * S,
    const q31_t * pSrc,
        q31_t * pDst,
        uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 LMS filter.
 */
typedef struct
{
        uint16_t numTaps;    /**< number of coefficients in the filter. */
        q31_t *pState;       /**< points to the state variable array. The array is of length numTaps+blockSize-1. */
        q31_t *pCoeffs;      /**< points to the coefficient array. The array is of length numTaps. */
        q31_t mu;            /**< step size that controls filter coefficient updates. */
        uint32_t postShift;  /**< bit shift applied to coefficients. */
        q31_t *pStateStart;  /**< points to the start of delay buffer */
} mchp_lms_instance_q31 __ALIGNED(4);

/**
 * @brief Initialization function for Q31 LMS filter.
 * @param[in] S          points to an instance of the Q31 LMS filter structure.
 * @param[in] numTaps    number of filter coefficients.
 * @param[in] pCoeffs    points to the coefficient buffer.
 * @param[in] pState     points to state buffer.
 * @param[in] mu         step size that controls filter coefficient updates.
 * @param[in] blockSize  number of samples to process.
 * @param[in] postShift  bit shift applied to coefficients.
 * @par
 */
void mchp_lms_init_q31(
        mchp_lms_instance_q31 * S,
        uint16_t numTaps,
        q31_t * pCoeffs,
        q31_t * pState,
        q31_t mu,
        uint32_t blockSize,
        uint32_t postShift);

/**
 * @brief Processing function for Q31 LMS filter.
 * @param[in]  S          points to an instance of the Q31 LMS filter structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[in]  pRef       points to the block of reference data.
 * @param[out] pOut       points to the block of output data.
 * @param[out] pErr       points to the block of error data.
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_lms_q31(
  const mchp_lms_instance_q31 * S,
  const q31_t * pSrc,
        q31_t * pRef,
        q31_t * pOut,
        q31_t * pErr,
        uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 normalized LMS filter.
 */
typedef struct
{
        uint16_t numTaps;     /**< number of coefficients in the filter. */
        q31_t *pState;        /**< points to the state variable array. The array is of length numTaps+blockSize-1. */
        q31_t *pCoeffs;       /**< points to the coefficient array. The array is of length numTaps. */
        q31_t mu;             /**< step size that control filter coefficient updates. */
        uint8_t postShift;    /**< bit shift applied to coefficients. */
        q31_t *pRecipTable;   /**< points to the reciprocal initial value table. */
        q31_t energy;         /**< saves previous frame energy. */
        q31_t x0;             /**< saves previous input sample. */
        q31_t *pStateStart;   /**< points to the start of delay buffer */
} mchp_lms_norm_instance_q31 __ALIGNED(4);

/**
 * @brief Initialization function for Q31 normalized LMS filter.
 * @param[in] S          points to an instance of the Q31 LMS filter structure.
 * @param[in] numTaps    number of filter coefficients.
 * @param[in] pCoeffs    points to coefficient buffer.
 * @param[in] pState     points to state buffer.
 * @param[in] mu         step size that controls filter coefficient updates.
 * @param[in] blockSize  number of samples to process.
 * @param[in] postShift  bit shift applied to coefficients.
 * @par
 */
void mchp_lms_norm_init_q31(
        mchp_lms_norm_instance_q31 * S,
        uint16_t numTaps,
        q31_t * pCoeffs,
        q31_t * pState,
        q31_t mu,
        uint32_t blockSize,
        uint8_t postShift);

/**
 * @brief Processing function for Q31 normalized LMS filter.
 * @param[in]  S          points to an instance of the Q31 normalized LMS filter structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[in]  pRef       points to the block of reference data.
 * @param[out] pOut       points to the block of output data.
 * @param[out] pErr       points to the block of error data.
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_lms_norm_q31(
        mchp_lms_norm_instance_q31 * S,
  const q31_t * pSrc,
        q31_t * pRef,
        q31_t * pOut,
        q31_t * pErr,
        uint32_t blockSize);

/**
 * @brief Instance structure for the Q31 IIR lattice filter.
 */
typedef struct
{
        uint16_t numStages;            /**< number of stages in the filter. */
        q31_t *pState;                 /**< points to the state variable array. The array is of length numStages+blockSize. */
        q31_t *pkCoeffs;               /**< points to the reflection coefficient array. The array is of length numStages. */
        q31_t *pvCoeffs;               /**< points to the ladder coefficient array. The array is of length numStages+1. */
} mchp_iir_lattice_instance_q31 __ALIGNED(4);

/**
 * @brief Initialization function for the Q31 IIR lattice filter.
 * @param[in] S          points to an instance of the Q31 IIR lattice structure.
 * @param[in] numStages  number of stages in the filter.
 * @param[in] pkCoeffs   points to the reflection coefficient buffer.  The array is of length numStages.
 * @param[in] pvCoeffs   points to the ladder coefficient buffer.  The array is of length numStages+1.
 * @param[in] pState     points to the state buffer.  The array is of length numStages+blockSize-1.
 * @param[in] blockSize  number of samples to process.
 * @par
 */
void mchp_iir_lattice_init_q31(
      mchp_iir_lattice_instance_q31 * S,
      uint16_t numStages,
      q31_t * pkCoeffs,
      q31_t * pvCoeffs,
      q31_t * pState,
      uint32_t blockSize);

/**
 * @brief Processing function for the Q31 IIR lattice filter.
 * @param[in]  S          points to an instance of the Q31 IIR lattice structure.
 * @param[in]  pSrc       points to the block of input data.
 * @param[out] pDst       points to the block of output data.
 * @param[in]  blockSize  number of samples to process.
 * @par
 */
void mchp_iir_lattice_q31(
const mchp_iir_lattice_instance_q31 * S,
const q31_t * pSrc,
      q31_t * pDst,
      uint32_t blockSize);

/** 
 * @} 
 */ 
/* end of groupFilters */

#ifdef __cplusplus
}
#endif

#endif /* FILTERING_FUNCTIONS_H_ */
