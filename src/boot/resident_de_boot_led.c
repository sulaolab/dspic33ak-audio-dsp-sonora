#include "resident_de_boot_led.h"

#if RESIDENT_BOOT_ENA_LED_PROGRESS

#include "nora_gpio.h"

#define RESIDENT_LED_COUNT (8u)

/* Same RP numbers as src/app/board/devices/button_led.c's s_led_rp[]. Vendored
 * rather than shared: button_led.c is application-only (boot_image.psd1), and
 * a boot source may not include an application header. */
#if defined(__dsPIC33AK512MPS512__)
static const nora_gpio_rp_t s_led_rp[RESIDENT_LED_COUNT] = {
    41u, 42u, 43u, 44u, 45u, 46u, 47u, 48u, /* LED0..LED7 = RP41..48 / RC8..RC15 */
};
#elif defined(__dsPIC33AK128MC106__)
static const nora_gpio_rp_t s_led_rp[RESIDENT_LED_COUNT] = {
    36u, 37u, 38u, 39u, 40u, 41u, 42u, 43u, /* LED0..LED7 = RP36..43 / RC3..RC10 */
};
#else
#error "Unknown target: give the resident LED progress bar its RP pins for this board."
#endif

void resident_boot_led_init(void)
{
    uint8_t i;
    for (i = 0u; i < RESIDENT_LED_COUNT; i++) {
        (void)nora_gpio_rp_config_digital_output(s_led_rp[i], false);
    }
}

void resident_boot_led_progress_reset(void)
{
    uint8_t i;
    for (i = 0u; i < RESIDENT_LED_COUNT; i++) {
        (void)nora_gpio_rp_clear(s_led_rp[i]);
    }
}

void resident_boot_led_progress(uint32_t done, uint32_t total)
{
    uint8_t lit;
    uint8_t i;

    if (total == 0u) {
        return;
    }
    /* done <= total <= RESIDENT_APP_CAPACITY_BYTES, well under 2^24; *8 cannot overflow. */
    lit = (uint8_t)((done * (uint32_t)RESIDENT_LED_COUNT) / total);
    /* Fill from the LED7 end, matching the board's physical left-to-right
     * orientation -- unverified until the first HW test; flip the comparison
     * below if it runs backward on the bench. */
    for (i = 0u; i < RESIDENT_LED_COUNT; i++) {
        (void)nora_gpio_rp_write(s_led_rp[i], (i >= (uint8_t)(RESIDENT_LED_COUNT - lit)));
    }
}

#endif /* RESIDENT_BOOT_ENA_LED_PROGRESS */
