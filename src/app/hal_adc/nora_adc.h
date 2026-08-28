#ifndef NORA_ADC_H
#define NORA_ADC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Small dsPIC33AK ADC peripheral HAL.
 *
 * Scope for the first migration phase:
 *   - ADC peripheral instance abstraction
 *   - polling / software-triggered conversion path
 *   - no board pin/PPS ownership
 *   - no DMA ownership
 *   - no audio/TDM buffer policy
 *   - no CMSIS types
 *
 * Existing ADC3/ADC4 audio input files remain the active audio implementation
 * until that path is split and hardware-validated separately.
 */

typedef enum {
    NORA_ADC_INSTANCE_1 = 1,
    NORA_ADC_INSTANCE_3 = 3,
    NORA_ADC_INSTANCE_4 = 4,
    NORA_ADC_INSTANCE_5 = 5,
} nora_adc_instance_t;

typedef enum {
    NORA_ADC_RESULT_OK = 0,
    NORA_ADC_RESULT_ERROR,
    NORA_ADC_RESULT_INVALID_ARG,
    NORA_ADC_RESULT_UNSUPPORTED,
    NORA_ADC_RESULT_NOT_INITIALIZED,
    NORA_ADC_RESULT_BUSY,
    NORA_ADC_RESULT_TIMEOUT,
} nora_adc_result_t;

typedef struct {
    nora_adc_instance_t instance;
    uint8_t                  channel;
    uint8_t                  positive_input;
    uint8_t                  sample_time_tad;
    bool                     calibrate;
    uint32_t                 ready_timeout_count;
    uint32_t                 calibration_timeout_count;
} nora_adc_config_t;

typedef struct {
    nora_adc_instance_t instance;
    uint8_t                  channel;
    bool                     initialized;
    bool                     busy;
    nora_adc_result_t   last_result;
} nora_adc_handle_t;

typedef struct {
    bool                   initialized;
    bool                   busy;
    nora_adc_result_t last_result;
} nora_adc_status_t;

bool nora_adc_init(nora_adc_handle_t *handle, const nora_adc_config_t *config);
void nora_adc_deinit(nora_adc_handle_t *handle);

nora_adc_result_t nora_adc_trigger(nora_adc_handle_t *handle, uint8_t channel);
bool nora_adc_is_conversion_complete(const nora_adc_handle_t *handle, uint8_t channel);
nora_adc_result_t nora_adc_get_result(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result);

nora_adc_result_t nora_adc_read_blocking(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result,
    uint32_t timeout_count);

nora_adc_status_t nora_adc_get_status(const nora_adc_handle_t *handle);
nora_adc_result_t nora_adc_get_last_result(const nora_adc_handle_t *handle);
void nora_adc_clear_error(nora_adc_handle_t *handle);

#endif /* NORA_ADC_H */
