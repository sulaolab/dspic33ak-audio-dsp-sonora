/**
 * @file    nora_canfd_device.h
 * @brief   dsPIC33AK CAN FD HAL - device/instance map.
 *
 * Maps an instance enum to its register pointer table. This is the only
 * boundary between the instance-agnostic HAL logic and the concrete MCU
 * register set; only nora_canfd_device_dspic33ak.c names raw C1.../C2... symbols.
 */
#ifndef NORA_CANFD_DEVICE_H
#define NORA_CANFD_DEVICE_H

#include "nora_canfd.h"
#include "nora_canfd_dspic33ak_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;
    nora_canfd_regs_t regs;
} nora_canfd_device_t;

/** Return the device entry for an instance, or NULL if absent/out of range. */
const nora_canfd_device_t *nora_canfd_get_device(nora_canfd_instance_t inst);

/** Convenience: true if the instance is present on this device. */
bool nora_canfd_instance_is_present(nora_canfd_instance_t inst);

#ifdef __cplusplus
}
#endif

#endif /* NORA_CANFD_DEVICE_H */
