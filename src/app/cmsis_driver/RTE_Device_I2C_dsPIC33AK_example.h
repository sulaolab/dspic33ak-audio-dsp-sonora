#ifndef RTE_DEVICE_I2C_DSPIC33AK_EXAMPLE_H
#define RTE_DEVICE_I2C_DSPIC33AK_EXAMPLE_H

/*
 * Example RTE configuration for the dsPIC33AK I2C CMSIS-Driver wrapper.
 *
 * - This file is an example RTE configuration for the dsPIC33AK I2C CMSIS-Driver
 *   wrapper.
 * - It is not intended to be a shared application-level RTE_Device.h.
 * - In an integrated application, copy the required I2C definitions into that
 *   application's RTE_Device.h or equivalent configuration header.
 * - Do not add USART/SPI/etc. settings to this I2C example file.
 */

#define RTE_I2C0 0
#define RTE_I2C1 1
#define RTE_I2C2 1
#define RTE_I2C3 0

#define RTE_I2C0_FCY_HZ             100000000u
#define RTE_I2C0_BUS_HZ             400000u
#define RTE_I2C0_TIMEOUT_MS         10u
#define RTE_I2C0_PENDING_TIMEOUT_MS 0u

#define RTE_I2C1_FCY_HZ             100000000u
#define RTE_I2C1_BUS_HZ             400000u
#define RTE_I2C1_TIMEOUT_MS         10u
#define RTE_I2C1_PENDING_TIMEOUT_MS 0u

#define RTE_I2C2_FCY_HZ             100000000u
#define RTE_I2C2_BUS_HZ             400000u
#define RTE_I2C2_TIMEOUT_MS         10u
#define RTE_I2C2_PENDING_TIMEOUT_MS 0u

#define RTE_I2C3_FCY_HZ             100000000u
#define RTE_I2C3_BUS_HZ             400000u
#define RTE_I2C3_TIMEOUT_MS         10u
#define RTE_I2C3_PENDING_TIMEOUT_MS 0u

#endif /* RTE_DEVICE_I2C_DSPIC33AK_EXAMPLE_H */
