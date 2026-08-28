#include "nora_adc.h"

#include <stddef.h>

#include <xc.h>

#include "nora_adc_dspic33ak_reg.h"

#define DSPIC33AK_ADC_DEFAULT_READY_TIMEOUT       (1000000UL)
#define DSPIC33AK_ADC_DEFAULT_CALIBRATE_TIMEOUT   (1000000UL)
#define DSPIC33AK_ADC_CHANNEL0                    (0U)
#define DSPIC33AK_ADC_CHANNEL0_TRIGGER_VALUE      (1UL)
#define DSPIC33AK_ADC_CHANNEL0_READY_MASK         (1UL)
#define DSPIC33AK_ADC_CHCON1_SOFTWARE_TRIGGER     (1UL)
#define DSPIC33AK_ADC_CHCON1_DISABLED_TRIGGER     (0UL)
#define DSPIC33AK_ADC_CHCON1_DEFAULT_DISABLED     (DSPIC33AK_ADC_CHCON1_IRQSEL)
#define DSPIC33AK_ADC_CHCON2_COMPARE_DISABLED     (DSPIC33AK_ADC_CHCON2_CMPVAL)
#define DSPIC33AK_ADC_DATA_MASK_12BIT             (0x0FFFUL)

typedef struct {
    nora_adc_instance_t       instance;
    const dspic33ak_adc_regs_t    *regs;
    dspic33ak_adc_channel_regs_t   channel0;
} dspic33ak_adc_device_t;

#if DSPIC33AK_ADC_DEVICE == DSPIC33AK_ADC_DEV_AK512
static const dspic33ak_adc_regs_t adc1_regs = {
    .CON = &AD1CON,
    .STAT = &AD1STAT,
    .SWTRG = &AD1SWTRG,
    .DATAOVR = &AD1DATAOVR,
    .CMPSTAT = &AD1CMPSTAT,
    .RSTAT = &AD1RSTAT,
};

static const dspic33ak_adc_regs_t adc5_regs = {
    .CON = &AD5CON,
    .STAT = &AD5STAT,
    .SWTRG = &AD5SWTRG,
    .DATAOVR = &AD5DATAOVR,
    .CMPSTAT = &AD5CMPSTAT,
    .RSTAT = &AD5RSTAT,
};

static const dspic33ak_adc_device_t adc_devices[] = {
    {
        .instance = NORA_ADC_INSTANCE_1,
        .regs = &adc1_regs,
        .channel0 = {
            .CON1 = &AD1CH0CON1,
            .CON2 = &AD1CH0CON2,
            .DATA = &AD1CH0DATA,
            .CNT = &AD1CH0CNT,
            .RES = &AD1CH0RES,
            .positive_input_mask = DSPIC33AK_ADC_CHCON1_PINSEL_MASK,
            .positive_input_pos = DSPIC33AK_ADC_CHCON1_PINSEL_POS,
        },
    },
    {
        .instance = NORA_ADC_INSTANCE_5,
        .regs = &adc5_regs,
        .channel0 = {
            .CON1 = &AD5CH0CON1,
            .CON2 = &AD5CH0CON2,
            .DATA = &AD5CH0DATA,
            .CNT = &AD5CH0CNT,
            .RES = &AD5CH0RES,
            .positive_input_mask = DSPIC33AK_ADC_CHCON1_PINSEL_MASK,
            .positive_input_pos = DSPIC33AK_ADC_CHCON1_PINSEL_POS,
        },
    },
};
#elif DSPIC33AK_ADC_DEVICE == DSPIC33AK_ADC_DEV_AK128
static const dspic33ak_adc_regs_t adc1_regs = {
    .CON = &AD1CON,
    .STAT = &AD1STAT,
    .SWTRG = &AD1SWTRG,
    .DATAOVR = &AD1DATAOVR,
    .CMPSTAT = &AD1CMPSTAT,
    .RSTAT = NULL,
};

static const dspic33ak_adc_device_t adc_devices[] = {
    {
        .instance = NORA_ADC_INSTANCE_1,
        .regs = &adc1_regs,
        .channel0 = {
            .CON1 = &AD1CH0CON,
            .CON2 = NULL,
            .DATA = &AD1CH0DATA,
            .CNT = &AD1CH0CNT,
            .RES = NULL,
            .positive_input_mask = (0x1FUL << 11),
            .positive_input_pos = 11,
        },
    },
};
#else
static const dspic33ak_adc_device_t adc_devices[] = {
    {
        .instance = (nora_adc_instance_t)0,
        .regs = NULL,
        .channel0 = {
            .CON1 = NULL,
            .CON2 = NULL,
            .DATA = NULL,
            .CNT = NULL,
            .RES = NULL,
            .positive_input_mask = 0UL,
            .positive_input_pos = 0U,
        },
    },
};
#endif

static const dspic33ak_adc_device_t *adc_device_get(nora_adc_instance_t instance)
{
    for (uint16_t index = 0U; index < (uint16_t)(sizeof(adc_devices) / sizeof(adc_devices[0])); index++) {
        if (adc_devices[index].instance == instance) {
            return &adc_devices[index];
        }
    }

    return NULL;
}

static uint32_t adc_timeout_or_default(uint32_t timeout_count, uint32_t default_timeout)
{
    if (timeout_count == 0U) {
        return default_timeout;
    }

    return timeout_count;
}

static bool adc_wait_for_bit(volatile uint32_t *reg, uint32_t mask, uint32_t timeout_count)
{
    uint32_t remaining = timeout_count;

    while ((*reg & mask) == 0U) {
        if (remaining == 0U) {
            return false;
        }
        remaining--;
    }

    return true;
}

static void adc_configure_channel0(const dspic33ak_adc_channel_regs_t *channel, const nora_adc_config_t *config)
{
    uint32_t channel_con1 = DSPIC33AK_ADC_CHCON1_IRQSEL;

    channel_con1 |= (DSPIC33AK_ADC_CHCON1_SOFTWARE_TRIGGER << DSPIC33AK_ADC_CHCON1_TRG1SRC_POS) &
                    DSPIC33AK_ADC_CHCON1_TRG1SRC_MASK;
    channel_con1 |= ((uint32_t)config->positive_input << channel->positive_input_pos) &
                    channel->positive_input_mask;
    channel_con1 |= ((uint32_t)config->sample_time_tad << DSPIC33AK_ADC_CHCON1_SAMC_POS) &
                    DSPIC33AK_ADC_CHCON1_SAMC_MASK;

    *channel->CON1 = channel_con1;
    if (channel->CON2 != NULL) {
        *channel->CON2 = DSPIC33AK_ADC_CHCON2_COMPARE_DISABLED;
    }
    *channel->CNT = 0UL;
}

static void adc_reset_channel0(const dspic33ak_adc_channel_regs_t *channel)
{
    *channel->CON1 = 0UL;
    if (channel->CON2 != NULL) {
        *channel->CON2 = 1UL;
    }
    *channel->CNT = 0UL;
    if (channel->RES != NULL) {
        *channel->RES = 0UL;
    }
}

bool nora_adc_init(nora_adc_handle_t *handle, const nora_adc_config_t *config)
{
    const dspic33ak_adc_device_t *device;
    uint32_t ready_timeout;
    uint32_t calibration_timeout;

    if ((handle == NULL) || (config == NULL)) {
        return false;
    }

    handle->instance = config->instance;
    handle->channel = config->channel;
    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;

    if (config->channel != DSPIC33AK_ADC_CHANNEL0) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return false;
    }

    device = adc_device_get(config->instance);
    if (device == NULL) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return false;
    }

    ready_timeout = adc_timeout_or_default(config->ready_timeout_count, DSPIC33AK_ADC_DEFAULT_READY_TIMEOUT);
    calibration_timeout = adc_timeout_or_default(
        config->calibration_timeout_count,
        DSPIC33AK_ADC_DEFAULT_CALIBRATE_TIMEOUT);

    dspic33ak_adc_reg_clear(device->regs->CON, DSPIC33AK_ADC_CON_ON);
    *device->regs->CON = DSPIC33AK_ADC_CON_RESET_VALUE;
    *device->regs->DATAOVR = 0UL;
    *device->regs->STAT = 0UL;
    if (device->regs->RSTAT != NULL) {
        *device->regs->RSTAT = 0UL;
    }
    *device->regs->CMPSTAT = 0UL;
    *device->regs->SWTRG = 0UL;
    adc_reset_channel0(&device->channel0);
    adc_configure_channel0(&device->channel0, config);

    dspic33ak_adc_reg_set(device->regs->CON, DSPIC33AK_ADC_CON_ON);
    if (!adc_wait_for_bit(device->regs->CON, DSPIC33AK_ADC_CON_ADRDY, ready_timeout)) {
        handle->last_result = NORA_ADC_RESULT_TIMEOUT;
        return false;
    }

    if (config->calibrate) {
        dspic33ak_adc_reg_set(device->regs->CON, DSPIC33AK_ADC_CON_CALREQ);
        if (!adc_wait_for_bit(device->regs->CON, DSPIC33AK_ADC_CON_CALRDY, calibration_timeout)) {
            handle->last_result = NORA_ADC_RESULT_TIMEOUT;
            return false;
        }
    }

    handle->initialized = true;
    handle->last_result = NORA_ADC_RESULT_OK;
    return true;
}

void nora_adc_deinit(nora_adc_handle_t *handle)
{
    const dspic33ak_adc_device_t *device;

    if (handle == NULL) {
        return;
    }

    device = adc_device_get(handle->instance);
    if (device != NULL) {
        dspic33ak_adc_reg_clear(device->regs->CON, DSPIC33AK_ADC_CON_ON);
        *device->regs->CON = DSPIC33AK_ADC_CON_RESET_VALUE;
        *device->regs->DATAOVR = 0UL;
        *device->regs->STAT = 0UL;
        if (device->regs->RSTAT != NULL) {
            *device->regs->RSTAT = 0UL;
        }
        *device->regs->CMPSTAT = 0UL;
        *device->regs->SWTRG = 0UL;
        adc_reset_channel0(&device->channel0);
    }

    handle->initialized = false;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_OK;
}

nora_adc_result_t nora_adc_trigger(nora_adc_handle_t *handle, uint8_t channel)
{
    const dspic33ak_adc_device_t *device;

    if (handle == NULL) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if (channel != DSPIC33AK_ADC_CHANNEL0) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    device = adc_device_get(handle->instance);
    if (device == NULL) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    *device->regs->SWTRG |= DSPIC33AK_ADC_CHANNEL0_TRIGGER_VALUE;
    handle->busy = true;
    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

bool nora_adc_is_conversion_complete(const nora_adc_handle_t *handle, uint8_t channel)
{
    const dspic33ak_adc_device_t *device;

    if ((handle == NULL) || (!handle->initialized) || (channel != DSPIC33AK_ADC_CHANNEL0)) {
        return false;
    }

    device = adc_device_get(handle->instance);
    if (device == NULL) {
        return false;
    }

    return ((*device->regs->STAT & DSPIC33AK_ADC_CHANNEL0_READY_MASK) != 0U);
}

nora_adc_result_t nora_adc_get_result(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result)
{
    const dspic33ak_adc_device_t *device;

    if ((handle == NULL) || (result == NULL)) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }
    if (!handle->initialized) {
        handle->last_result = NORA_ADC_RESULT_NOT_INITIALIZED;
        return handle->last_result;
    }
    if (channel != DSPIC33AK_ADC_CHANNEL0) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    device = adc_device_get(handle->instance);
    if (device == NULL) {
        handle->last_result = NORA_ADC_RESULT_UNSUPPORTED;
        return handle->last_result;
    }

    *result = *device->channel0.DATA & DSPIC33AK_ADC_DATA_MASK_12BIT;
    handle->busy = false;
    handle->last_result = NORA_ADC_RESULT_OK;
    return handle->last_result;
}

nora_adc_result_t nora_adc_read_blocking(
    nora_adc_handle_t *handle,
    uint8_t channel,
    uint32_t *result,
    uint32_t timeout_count)
{
    nora_adc_result_t status;
    uint32_t remaining = adc_timeout_or_default(timeout_count, DSPIC33AK_ADC_DEFAULT_READY_TIMEOUT);

    status = nora_adc_trigger(handle, channel);
    if (status != NORA_ADC_RESULT_OK) {
        return status;
    }

    while (!nora_adc_is_conversion_complete(handle, channel)) {
        if (remaining == 0U) {
            handle->busy = false;
            handle->last_result = NORA_ADC_RESULT_TIMEOUT;
            return handle->last_result;
        }
        remaining--;
    }

    return nora_adc_get_result(handle, channel, result);
}

nora_adc_status_t nora_adc_get_status(const nora_adc_handle_t *handle)
{
    nora_adc_status_t status = {
        .initialized = false,
        .busy = false,
        .last_result = NORA_ADC_RESULT_INVALID_ARG,
    };

    if (handle != NULL) {
        status.initialized = handle->initialized;
        status.busy = handle->busy;
        status.last_result = handle->last_result;
    }

    return status;
}

nora_adc_result_t nora_adc_get_last_result(const nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return NORA_ADC_RESULT_INVALID_ARG;
    }

    return handle->last_result;
}

void nora_adc_clear_error(nora_adc_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->last_result != NORA_ADC_RESULT_OK) {
        handle->last_result = NORA_ADC_RESULT_OK;
    }
}
