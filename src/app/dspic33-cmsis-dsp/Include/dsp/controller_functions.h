/******************************************************************************
 * @file     controller_functions.h
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

#ifndef CONTROLLER_FUNCTIONS_H_
#define CONTROLLER_FUNCTIONS_H_

#include "../mchp_math_types.h"

#ifdef    __cplusplus
extern "C" {
#endif

/**
 * @defgroup groupController Controller Functions
 * @brief Controller Functions for CMSIS-DSP MCHP Library (dsPIC33A)
 *
 * A Proportional Integral Derivative (PID) controller is a generic feedback control
 * loop mechanism widely used in industrial control systems.
 * A PID controller is the most commonly used type of feedback controller.
 *
 * This group of functions implements (PID) controllers
 * for single-precision floating-point and Q31 fixed-point 
 * data types, supporting functionalities which operate on a single sample
 * of data and each call to the function returns a single processed value.
 * <code> S </code> points to an instance of the PID control data structure. <code> in </code>
 * is the input sample value. The functions return the output value.
 *
 * @par Algorithm:
 * <pre>
 *    y[n] = y[n-1] + A0 * x[n] + A1 * x[n-1] + A2 * x[n-2]
 *    A0 = Kp + Ki + Kd
 *    A1 = (-Kp ) - (2 * Kd )
 *    A2 = Kd
 * </pre>
 *
 * @par
 * where @c Kp is proportional constant, @c Ki is Integral constant and @c Kd is Derivative constant
 *
 * @par
 * @image html PID.gif "Proportional Integral Derivative Controller"
 *
 * @par
 * The PID controller calculates an "error" value as the difference between
 * the measured output and the reference input.
 * The controller attempts to minimize the error by adjusting the process control inputs.
 * The proportional value determines the reaction to the current error,
 * the integral value determines the reaction based on the sum of recent errors,
 * and the derivative value determines the reaction based on the rate at which the error has been changing.
 *
 * @par Instance Structure
 * The Gains A0, A1, A2 and state variables for a PID controller are stored together in an instance data structure.
 * A separate instance structure must be defined for each PID Controller.
 * There are separate instance structure declarations for each of the 3 supported data types.
 *
 * @par Reset Functions
 * There is also an associated reset function for each data type which clears the state array.
 *
 * @par Initialization Functions
 * There is also an associated initialization function for each data type.
 * The initialization function performs the following operations:
 * - Initializes the Gains A0, A1, A2 from Kp,Ki, Kd gains.
 * - Zeros out the values in the state buffer.
 *
 * @par
 * Instance structure cannot be placed into a const data section and it is recommended to use the initialization function.
 *
 * @{
 */


/**
 * @brief Instance structure for the single-precision floating-point PID controller.
 *
 * This structure stores:
 * - Derived coefficients (A0, A1, A2)
 * - Controller state history
 * - User-specified PID gains (Kp, Ki, Kd)
 */
typedef struct
{
        float32_t A0;          /**< Derived gain: A0 = Kp + Ki + Kd */
        float32_t A1;          /**< Derived gain: A1 = -Kp - 2*Kd */
        float32_t A2;          /**< Derived gain: A2 = Kd */
        float32_t state[3];    /**< State history: x[n], x[n-1], x[n-2] */
        float32_t Kp;          /**< Proportional gain */
        float32_t Ki;          /**< Integral gain */
        float32_t Kd;          /**< Derivative gain */
} mchp_pid_instance_f32;

/**
 * @brief Initialization function for the single-precision floating-point PID controller.
 *
 * Computes internal filter coefficients from the PID gains (Kp, Ki, Kd).
 * If requested, the function clears the controller state variables,
 * including history terms and output.
 *
 * @param[in,out] S               Pointer to the PID instance structure
 * @param[in]     resetStateFlag  State reset control:
 *                                - 0 : Do not reset internal state
 *                                - 1 : Reset state history and output
 * @par
 */
void mchp_pid_init_f32(
    mchp_pid_instance_f32 * S,
    int32_t resetStateFlag);

/**
 * @brief Reset function for the single-precision floating-point PID controller.
 *
 * Clears the internal state history without modifying gains.
 *
 * @param[in,out] S  Pointer to PID instance structure
 * @par
 */
void mchp_pid_reset_f32(
    mchp_pid_instance_f32 * S);

/**
 * @brief Process function for the single-precision floating-point PID controller.
 *
 * Computes one iteration of the PID control law.
 *
 * @param[in,out] S   Pointer to PID instance structure
 * @param[in]     in  Input error sample
 *
 * @return Control output value
 */
float32_t mchp_pid_f32(
    mchp_pid_instance_f32 * S,
    float32_t in);

/**
 * @ingroup PID
 * @brief Instance structure for the Q31 PID Control.
 */
typedef struct
{
        q31_t A0;              /**< The derived gain, A0 = Kp + Ki + Kd . */
        q31_t A1;              /**< The derived gain, A1 = -Kp - 2Kd. */
        q31_t A2;              /**< The derived gain, A2 = Kd . */
        q31_t state[3];        /**< The state array of length 3. */
        q31_t Kp;              /**< The proportional gain. */
        q31_t Ki;              /**< The integral gain. */
        q31_t Kd;              /**< The derivative gain. */
} mchp_pid_instance_q31;

/**
 * @brief  Initialization function for the Q31 PID Control.
 * @param[in,out] S               points to an instance of the Q31 PID structure.
 * @param[in]     resetStateFlag  flag to reset the state. 0 = no change in state 1 = reset the state.
 */
void mchp_pid_init_q31(
    mchp_pid_instance_q31 * S,
    int32_t resetStateFlag);

/**
 * @brief  Reset function for the Q31 PID Control.
 * @param[in,out] S  is an instance of the Q31 PID Control structure
 */
void mchp_pid_reset_q31(
    mchp_pid_instance_q31 * S);

/**
 * @ingroup PID
 * @brief         Process function for the Q31 PID Control.
 * @param[in,out] S   is an instance of the Q31 PID Control structure
 * @param[in]     in  input sample to process
 * @return        processed output sample.
 */
q31_t mchp_pid_q31(
    mchp_pid_instance_q31 * S,
    q31_t in);

/** 
 * @} 
 */ 
/* end of groupController */

#ifdef   __cplusplus
}
#endif

#endif /* ifndef CONTROLLER_FUNCTIONS_H_ */
