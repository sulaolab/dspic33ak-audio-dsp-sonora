#include "app_i2c.h"

#include "resolved_board_config.h"
#include "app_runtime_overrides.h"
#include "timer_app.h"
#include "nora_i2c_master.h"

#if RESOLVED_BOARD_USE_CMSIS_I2C
#include <stdint.h>
#include <stdio.h>
#include "Driver_I2C_dsPIC33AK.h"
#endif


#if RESOLVED_BOARD_USE_CMSIS_I2C
// Millisecond tick source for the CMSIS I2C driver timeout handling.
// Overrides the weak default in Driver_I2C_dsPIC33AK.c.
uint32_t Driver_I2C_dsPIC33AK_GetMs(void)
{
    return GetTicks();
}
#endif // RESOLVED_BOARD_USE_CMSIS_I2C


void app_i2c_cmsis_init(void)
{
#if RESOLVED_BOARD_USE_CMSIS_I2C
    int32_t ret;

    ret = Driver_I2C1.Initialize(NULL);
    printf("Driver_I2C1 Initialize ret=%ld\n", (long)ret);

    ret = Driver_I2C1.PowerControl(ARM_POWER_FULL);
    printf("Driver_I2C1 PowerControl FULL ret=%ld\n", (long)ret);

    ret = Driver_I2C1.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_FAST);
    printf("Driver_I2C1 BUS_SPEED FAST ret=%ld\n", (long)ret);

#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
    ret = Driver_I2C2.Initialize(NULL);
    printf("Driver_I2C2 Initialize ret=%ld\n", (long)ret);

    ret = Driver_I2C2.PowerControl(ARM_POWER_FULL);
    printf("Driver_I2C2 PowerControl FULL ret=%ld\n", (long)ret);

    ret = Driver_I2C2.Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_FAST);
    printf("Driver_I2C2 BUS_SPEED FAST ret=%ld\n", (long)ret);
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
#endif // RESOLVED_BOARD_USE_CMSIS_I2C
}


void app_i2c_hal_init(void)
{
    nora_i2c_config_t i2c_cfg;

    i2c_cfg.fcy_hz             = (uint32_t)(FCY);
    i2c_cfg.bus_hz             = 400000u;
    i2c_cfg.timeout_ms         = 10u;
    i2c_cfg.get_ms             = GetTicks;
    i2c_cfg.pending_timeout_ms = 0u;

    (void)nora_i2c_init( NORA_I2C_INST_2, &i2c_cfg );   /* MikroBUS-A */
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
    (void)nora_i2c_init( NORA_I2C_INST_3, &i2c_cfg );   /* MikroBUS-B */
#elif RESOLVED_BOARD_AK128_J3_TDM_B
    (void)nora_i2c_init( NORA_I2C_INST_1, &i2c_cfg );   /* MikroBUS-B: DIM-P4/P6 */
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
}
