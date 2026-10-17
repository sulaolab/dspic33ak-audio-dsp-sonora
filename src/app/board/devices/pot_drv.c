/*
(c) [2024] Microchip Technology Inc. and its subsidiaries.

    Subject to your compliance with these terms, you may use Microchip 
    software and any derivatives exclusively with Microchip products. 
    You are responsible for complying with 3rd party license terms  
    applicable to your use of 3rd party software (including open source  
    software) that may accompany Microchip software. SOFTWARE IS ?AS IS.? 
    NO WARRANTIES, WHETHER EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS 
    SOFTWARE, INCLUDING ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT,  
    MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT 
    WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE, 
    INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY 
    KIND WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF 
    MICROCHIP HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE 
    FORESEEABLE. TO THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP?S 
    TOTAL LIABILITY ON ALL CLAIMS RELATED TO THE SOFTWARE WILL NOT 
    EXCEED AMOUNT OF FEES, IF ANY, YOU PAID DIRECTLY TO MICROCHIP FOR 
    THIS SOFTWARE.
*/
#include "resolved_board_config.h"
#include "board/devices/pot_drv.h"
#include <xc.h>
#include "nora_adc.h"
#include "nora_gpio.h"
#include <stdio.h>

static nora_adc_handle_t pot_adc;
static bool pot_adc_ready;
static uint16_t pot_last_data;

#define POT_ADC_CHANNEL        (0U)
#if (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE) && ENA_NANO_POT
#define POT_ADC_POSITIVE_INPUT (2U) // AD2AN2, not PPS RP18
#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
#define POT_ADC_POSITIVE_INPUT (0U)
#else
#define POT_ADC_POSITIVE_INPUT (6U)
#endif
#define POT_ADC_SAMPLE_TIME    (0U)
#define POT_ADC_TIMEOUT_COUNT  (1000000UL)

void POT_Initialize(void)
{
    const nora_adc_config_t config = {
#if (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE) && ENA_NANO_POT
        .instance = NORA_ADC_INSTANCE_2,
#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
        .instance = NORA_ADC_INSTANCE_5,
#else
        .instance = NORA_ADC_INSTANCE_1,
#endif
        .channel = POT_ADC_CHANNEL,
        .positive_input = POT_ADC_POSITIVE_INPUT,
        .sample_time_tad = POT_ADC_SAMPLE_TIME,
        .calibrate = true,
        .ready_timeout_count = POT_ADC_TIMEOUT_COUNT,
        .calibration_timeout_count = POT_ADC_TIMEOUT_COUNT,
    };

#if (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE) && ENA_NANO_POT
    const nora_gpio_config_t pin_cfg = {
        .dir = NORA_GPIO_DIR_INPUT, .pull = NORA_GPIO_PULL_NONE,
        .analog = true, .open_drain = false, .initial_high = false,
    };
    pot_adc_ready = false;
    if (!nora_gpio_rp_config(18u, &pin_cfg)) return;
#endif
    pot_last_data = 0U;
    pot_adc_ready = nora_adc_init(&pot_adc, &config);
}

static uint16_t pot_read_adc(void)
{
    uint32_t result = 0U;

    if (!pot_adc_ready) {
        return pot_last_data;
    }

    if (nora_adc_read_blocking(&pot_adc, POT_ADC_CHANNEL, &result, POT_ADC_TIMEOUT_COUNT) !=
        NORA_ADC_RESULT_OK) {
        return pot_last_data;
    }

    pot_last_data = (uint16_t)result;
    return pot_last_data;
}


bool POT_IsReady(void) { return pot_adc_ready; }
void POT_PrintStatus(void)
{
    uint16_t raw = POT_Read();
    printf(" POT ready=%u raw=%u", (unsigned)pot_adc_ready, (unsigned)raw);
#if (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK506_VALUE) && ENA_NANO_POT
    printf(" slot=2 rp=18 adc=2 input=2 span=%u invert=%u", (unsigned)NANO_POT_SPAN_COUNTS, (unsigned)NANO_POT_INVERT);
#endif
    printf("\n");
}

#if RESOLVED_BOARD_USE_REGULAR_ADC_API

uint16_t POT_Read(void)
{
    return pot_read_adc();
}

#else


uint16_t POT_Read(void)
{
    return pot_last_data;
}

void POT_Process(void)
{
    (void)pot_read_adc();
}

#endif // RESOLVED_BOARD_USE_REGULAR_ADC_API
