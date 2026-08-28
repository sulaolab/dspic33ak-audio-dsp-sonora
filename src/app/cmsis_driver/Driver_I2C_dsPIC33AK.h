#ifndef DRIVER_I2C_DSPIC33AK_H
#define DRIVER_I2C_DSPIC33AK_H

#include <stdint.h>
#include "Driver_I2C.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CMSIS-Driver instance mapping:
 *   Driver_I2C0 -> NORA_I2C_INST_1
 *   Driver_I2C1 -> NORA_I2C_INST_2
 *   Driver_I2C2 -> NORA_I2C_INST_3
 *   Driver_I2C3 -> NORA_I2C_INST_4
 */

extern ARM_DRIVER_I2C Driver_I2C0;
extern ARM_DRIVER_I2C Driver_I2C1;
extern ARM_DRIVER_I2C Driver_I2C2;
extern ARM_DRIVER_I2C Driver_I2C3;

uint32_t Driver_I2C_dsPIC33AK_GetMs(void);

#ifdef __cplusplus
}
#endif

#endif /* DRIVER_I2C_DSPIC33AK_H */
