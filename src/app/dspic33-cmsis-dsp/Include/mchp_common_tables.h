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

#ifndef MCHP_COMMON_TABLES_H
#define MCHP_COMMON_TABLES_H

#include "mchp_math_types.h"

#ifdef   __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------
 * FFT Twiddle Coefficient Tables
 * ------------------------------------------------------------------
 *
 * These tables contain precomputed sine/cosine coefficients
 * (twiddle factors) used by FFT and IFFT routines.
 *
 * Each table corresponds to a specific FFT length (N).
 * Table size is 2 * N elements, storing interleaved
 * real and imaginary components.
 *
 * Conditional compilation allows only required tables
 * to be linked, reducing program memory usage.
 */

// ---------------- FFT length = 16 ----------------
#if USE_FFT_LEN_16_F32
    extern const float32_t twiddleCoef_16[32];
    /* Twiddle factors for 16-point FFT (2 * 16 entries) */
#endif

// ---------------- FFT length = 32 ----------------
#if USE_FFT_LEN_32_F32
    extern const float32_t twiddleCoef_32[64];
    /* Twiddle factors for 32-point FFT */
#endif

// ---------------- FFT length = 64 ----------------
#if USE_FFT_LEN_64_F32
    extern const float32_t twiddleCoef_64[128];
    /* Twiddle factors for 64-point FFT */
#endif

// ---------------- FFT length = 128 ----------------
#if USE_FFT_LEN_128_F32
    extern const float32_t twiddleCoef_128[256];
    /* Twiddle factors for 128-point FFT */
#endif

// ---------------- FFT length = 256 ----------------
#if USE_FFT_LEN_256_F32
    extern const float32_t twiddleCoef_256[512];
    /* Twiddle factors for 256-point FFT */
#endif

// ---------------- FFT length = 512 ----------------
#if USE_FFT_LEN_512_F32
    extern const float32_t twiddleCoef_512[1024];
    /* Twiddle factors for 512-point FFT */
#endif

// ---------------- FFT length = 1024 ----------------
#if USE_FFT_LEN_1024_F32
    extern const float32_t twiddleCoef_1024[2048];
    /* Twiddle factors for 1024-point FFT */
#endif

// ---------------- FFT length = 2048 ----------------
#if USE_FFT_LEN_2048_F32
    extern const float32_t twiddleCoef_2048[4096];
    /* Twiddle factors for 2048-point FFT */
#endif

// ---------------- FFT length = 4096 ----------------
#if USE_FFT_LEN_4096_F32
    extern const float32_t twiddleCoef_4096[8192];
    /* Twiddle factors for 4096-point FFT */
#endif

/* ===================================================================
 *  Q31 Twiddle Factor Tables
 * =================================================================== */

#if USE_FFT_LEN_8_Q31
    extern const q31_t twiddleCoef_q31_8[16];
#endif

#if USE_FFT_LEN_16_Q31
    extern const q31_t twiddleCoef_q31_16[32];
    extern const q31_t twiddleCoef_rfft_q31_16[16];
#endif

#if USE_FFT_LEN_32_Q31
    extern const q31_t twiddleCoef_q31_32[64];
    extern const q31_t twiddleCoef_rfft_q31_32[32];
#endif

#if USE_FFT_LEN_64_Q31
    extern const q31_t twiddleCoef_q31_64[128];
    extern const q31_t twiddleCoef_rfft_q31_64[64];
#endif

#if USE_FFT_LEN_128_Q31
    extern const q31_t twiddleCoef_q31_128[256];
    extern const q31_t twiddleCoef_rfft_q31_128[128];
#endif

#if USE_FFT_LEN_256_Q31
    extern const q31_t twiddleCoef_q31_256[512];
    extern const q31_t twiddleCoef_rfft_q31_256[256];
#endif

#if USE_FFT_LEN_512_Q31
    extern const q31_t twiddleCoef_q31_512[1024];
    extern const q31_t twiddleCoef_rfft_q31_512[512];
#endif

#if USE_FFT_LEN_1024_Q31
    extern const q31_t twiddleCoef_q31_1024[2048];
    extern const q31_t twiddleCoef_rfft_q31_1024[1024];
#endif

#if USE_FFT_LEN_2048_Q31
    extern const q31_t twiddleCoef_q31_2048[4096];
    extern const q31_t twiddleCoef_rfft_q31_2048[2048];
#endif

#if USE_FFT_LEN_4096_Q31
    extern const q31_t twiddleCoef_q31_4096[8192];
    extern const q31_t twiddleCoef_rfft_q31_4096[4096];
#endif

#ifdef   __cplusplus
}
#endif

#endif /* MCHP_COMMON_TABLES_H */
