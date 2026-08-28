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

#ifndef MCHP_MATH_H
#define MCHP_MATH_H

#ifdef   __cplusplus
extern "C"
{
#endif

#include "mchp_math_types.h"

/* ------------------------------------------------------------------
 * DSP Library Functions Includes
 * ------------------------------------------------------------------
 *
 * This header aggregates all DSP function groups into a single
 * include point. Applications typically include only this file
 * to gain access to the complete DSP math API.
 *
 * Each sub-header contains a logically grouped set of DSP routines
 * optimized for dsPIC33 Digital Signal Controllers.
 */

#include "dsp/basic_math_functions.h"
    /* NOTE: Vector arithmetic (add, subtract, scale, dot product) */

#include "dsp/transform_functions.h"
    /* NOTE: FFT, IFFT, DCT, and related spectral transforms */

#include "dsp/filtering_functions.h"
    /* NOTE: FIR, IIR, LMS, and biquad filter implementations */

#include "dsp/window_functions.h"
    /* NOTE: Windowing functions used in spectral analysis */

#include "dsp/matrix_functions.h"
    /* NOTE: Matrix operations such as add, multiply, transpose, inverse */

#include "dsp/complex_math_functions.h"
    /* NOTE: Complex vector operations (magnitude, magnitude squared, etc.) */

#include "dsp/controller_functions.h"
    /* NOTE: Control algorithms such as PID controllers */

#include "dsp/statistics_functions.h"
    /* NOTE: Statistical functions (mean, variance, RMS, min/max) */

#ifdef   __cplusplus
}
#endif

#endif /* MCHP_MATH_H */
