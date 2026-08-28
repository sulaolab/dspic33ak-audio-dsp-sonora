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

#ifndef MCHP_DSP_CONFIG_H
#define MCHP_DSP_CONFIG_H

#ifdef   __cplusplus
extern "C"
{
#endif

/**
 * @def MCHP_DSP_ATTRIBUTE
 * @brief Compiler attribute used to mark DSP-related functions or data as weak symbols.
 *
 * Symbols annotated with #MCHP_DSP_ATTRIBUTE may be overridden by
 * user-defined implementations. If an application provides its own
 * definition of a function or object marked with this attribute, the
 * user-defined version takes precedence at link time.
 *
 * @note
 * This attribute is useful for providing default DSP routines while
 * allowing optional application-specific overrides.
 *
 * @par Typical use cases:
 * - FFT twiddle factor tables
 * - Window initialization routines
 * - User-optimized DSP replacements
 */
#define MCHP_DSP_ATTRIBUTE __attribute__ ((weak))


/**
 * @def MCHP_DSP_TABLE_ATTRIBUTE
 * @brief Compiler attribute used to place DSP tables in a dedicated memory section.
 *
 * This attribute forces twiddle-factor tables, bit-reversal tables, or other
 * DSP-related constant data into a custom linker section named `"dsp_table"`.
 *
 * @note 
 * - This is typically mapped to optimized memory regions (e.g., Y-memory or
 * tightly coupled memory) via the linker script to improve FFT execution
 * speed on dsPIC33 or related architectures.
 * - The actual placement depends on the project's linker script configuration.
 *
 * @par Separating DSP tables from generic .rodata allows:
 *  - Predictable memory placement
 *  - Easier linker tuning
 */
#define MCHP_DSP_TABLE_ATTRIBUTE __attribute__((section ("dsp_table")))


/**
 * @brief Configuration macros for enabling FFT length support.
 *
 * Set any of the following macros to 1 to enable support for their
 * corresponding FFT lengths. Set all unused macros to 0.
 *
 * These macros are used for conditional compilation of twiddle-factor
 * tables and internal FFT configuration structures. Selecting only the
 * required FFT length helps reduce code size and memory footprint.
 *
 * The USE_FFT_LEN_xxx macros control F32 twiddle-factor tables and
 * F32 FFT init functions.
 *
 * The USE_FFT_LEN_xxx_Q31 macros independently control Q31 twiddle-factor
 * tables and Q31 FFT init functions.  This allows different FFT lengths
 * to be enabled for F32 and Q31 without pulling in unnecessary tables.
 *
 * @par These macros directly control:
 *  - Which twiddle tables are compiled
 *  - Which FFT initialization paths are enabled
 */

/**
 * @def USE_FFT_LEN_16_F32
 * @brief Enable support for 16-point F32 FFT.
 */
#define USE_FFT_LEN_16_F32     0

/**
 * @def USE_FFT_LEN_32_F32
 * @brief Enable support for 32-point F32 FFT.
 */
#define USE_FFT_LEN_32_F32     0

/**
 * @def USE_FFT_LEN_64_F32
 * @brief Enable support for 64-point F32 FFT.
 */
#define USE_FFT_LEN_64_F32     0

/**
 * @def USE_FFT_LEN_128_F32
 * @brief Enable support for 128-point F32 FFT.
 */
#define USE_FFT_LEN_128_F32    0

/**
 * @def USE_FFT_LEN_256_F32
 * @brief Enable support for 256-point F32 FFT.
 */
#define USE_FFT_LEN_256_F32    0

/**
 * @def USE_FFT_LEN_512_F32
 * @brief Enable support for 512-point F32 FFT.
 */
#define USE_FFT_LEN_512_F32    0

/**
 * @def USE_FFT_LEN_1024_F32
 * @brief Enable support for 1024-point F32 FFT.
 *
 * @note
 * This is a common default choice balancing:
 *  - Frequency resolution
 *  - Execution time
 *  - Memory usage on dsPIC devices
 */
#define USE_FFT_LEN_1024_F32   1

/**
 * @def USE_FFT_LEN_2048_F32
 * @brief Enable support for 2048-point F32 FFT.
 *
 * @note
 * Requires sufficient Y-memory for FFT input buffers and may
 * not be supported on all dsPIC variants.
 */
#define USE_FFT_LEN_2048_F32   0

/**
 * @def USE_FFT_LEN_4096_F32
 * @brief Enable support for 4096-point F32 FFT.
 *
 * @note
 * Typically only feasible on devices with large data memory
 * or when external memory is used.
 */
#define USE_FFT_LEN_4096_F32   0

/* -----------------------------------------------------------------
 *  Q31 FFT length configuration
 *
 *  These macros independently control conditional compilation of Q31
 *  (q31_t) twiddle-factor tables and the Q31 FFT init functions
 *  (mchp_cfft_init_q31, mchp_rfft_fast_init_q31).
 *
 *  The _Q31 suffix separates these from the F32 macros above so that
 *  different FFT lengths can be enabled for each data type without
 *  pulling in unnecessary tables from the other type.
 *
 *  Set to 1 to include support for the corresponding FFT length.
 *  Set to 0 to exclude it and save flash/RAM.
 * ----------------------------------------------------------------- */

/** @def USE_FFT_LEN_8_Q31
 *  @brief Enable 8-point Q31 FFT (debug/test only). */
#define USE_FFT_LEN_8_Q31       1

/** @def USE_FFT_LEN_16_Q31
 *  @brief Enable 16-point Q31 FFT. */
#define USE_FFT_LEN_16_Q31      0

/** @def USE_FFT_LEN_32_Q31
 *  @brief Enable 32-point Q31 FFT. */
#define USE_FFT_LEN_32_Q31      0

/** @def USE_FFT_LEN_64_Q31
 *  @brief Enable 64-point Q31 FFT. */
#define USE_FFT_LEN_64_Q31      0

/** @def USE_FFT_LEN_128_Q31
 *  @brief Enable 128-point Q31 FFT. */
#define USE_FFT_LEN_128_Q31     1

/** @def USE_FFT_LEN_256_Q31
 *  @brief Enable 256-point Q31 FFT. */
#define USE_FFT_LEN_256_Q31     0

/** @def USE_FFT_LEN_512_Q31
 *  @brief Enable 512-point Q31 FFT. */
#define USE_FFT_LEN_512_Q31     0

/** @def USE_FFT_LEN_1024_Q31
 *  @brief Enable 1024-point Q31 FFT. */
#define USE_FFT_LEN_1024_Q31    1

/** @def USE_FFT_LEN_2048_Q31
 *  @brief Enable 2048-point Q31 FFT. */
#define USE_FFT_LEN_2048_Q31    0

/** @def USE_FFT_LEN_4096_Q31
 *  @brief Enable 4096-point Q31 FFT. */
#define USE_FFT_LEN_4096_Q31    0

#ifdef   __cplusplus
}
#endif

#endif /*  MCHP_DSP_CONFIG_H */