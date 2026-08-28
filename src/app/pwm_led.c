
//===========================================================
// INCLUDES
//===========================================================
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"

#include <xc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_pwm.h"   /* hal_pwm: PG generator register HAL */
#include "board/clock/sonora_clock.h"
#include "pwm_audio_dma_buffer.h"   /* PWM_AUDIO_PERIOD_COUNT_Q4: module-wide
                                     * period shared with the audio-DAC path,
                                     * see pwm_audio_dma_buffer.h. */

#include "pwm_led.h"


//===========================================================
// Definition
//===========================================================
// Unknown-device guard is centralized: app_specific_config_defs.h (2.0) APP_TARGET
// derivation #error's on any device this build does not recognize.

#define PG_PER_COUNT         PWM_AUDIO_PERIOD_COUNT_Q4
#define PG_DC_INIT           (PG_PER_COUNT/2)           // duty 50%

#define LED_DEAD_TIME        (8)

//#define BRIGHTNESS_VAL      (0.25)
#define BRIGHTNESS_VAL       (0.3)
#define BRIGHTNESS_VAL_RED   (0.25)

//===========================================================
// Function Prototype
//===========================================================

static bool     pwm_prepare_clock_or_stop(void);
static void     pwm_clock_fail_stop(void);
static bool     led_generator_init(nora_pwm_generator_id_t id, nora_gpio_rp_t rp_h);


//===========================================================
// Global Function
//===========================================================

void pwm_led_init(void)
{
#if APP_USE_I2S_FORMAT && !defined(WM8904_PCB_REV4)
    // NOTE:
    // I2S is 64FS(3.0MHz) which BCLK. PWM is using PLL2 which needs above 6MHz.
    // As a result, We cannot use PLL2 at I2S mode.
    printf("\n\n\n\n\n\n\n");
    printf(" pwm_led_init:\n");
    printf(" APP_USE_I2S_FORMAT is enabled and WM8904_PCB_REV4 isn't enabled.\n");
    printf(" This configuration cannot use PWM functionality due to the clock connection.\n");
    printf("\n\n\n\n\n\n\n");
    return;
#endif //APP_USE_I2S_FORMAT && !defined(WM8904_PCB_REV4)

    // Prepare the upstream PWM clock path only when PWM audio needs CLKGEN5.
    if( !pwm_prepare_clock_or_stop() )
    {
        return;
    }

    // Module-wide clock-source select: every PG instance on this device
    // (LED here, the Classic audio PWM DAC later) shares this one bit --
    // see hal_pwm/nora_pwm.h. Must run before any nora_pwm_generator_init().
    if( !nora_pwm_module_init(
            APP_USE_PWM_AUDIO ? NORA_PWM_MCLK_HIGH_FREQ : NORA_PWM_MCLK_STANDARD ) )
    {
        printf(" WARNING: pwm_led_init: nora_pwm_module_init failed\n");
        return;
    }

    // 3-colors LED: board pin map (DIM-P72/70/68 -> BLUE/GREEN/RED). Blue/green
    // share the same RP on both targets; red's pin differs (RP58 is U2TX on the
    // AK128 DIM, so red is rerouted to RP35 there). This is board/target wiring,
    // not chip-generic PWM behaviour, so it stays here rather than in hal_pwm.
#if APP_TARGET == APP_TARGET_AK128
    #define PWM_LED_RED_RP   35u
#elif APP_TARGET == APP_TARGET_AK512
    #define PWM_LED_RED_RP   58u
#else
    #error "Unhandled APP_TARGET in pwm_led.c pin map -- add an arm for the new device"
#endif

    bool ok = true;
    ok = led_generator_init(NORA_PWM_GEN_1, (nora_gpio_rp_t)51u) && ok;   // blue
    ok = led_generator_init(NORA_PWM_GEN_2, (nora_gpio_rp_t)49u) && ok;   // green
    ok = led_generator_init(NORA_PWM_GEN_3, (nora_gpio_rp_t)PWM_LED_RED_RP) && ok;  // red

    if( !ok )
    {
        printf(" WARNING: pwm_led_init: one or more LED PWM generators failed to init\n");
    }
}

// blue
void pwm1_set_duty(uint8_t duty)
{
    // duty==0 -> DC=0 -> channel fully off. No minimum-glow floor: the RGB colour
    // ramp needs a real zero so a channel truly extinguishes (e.g. blue at the OFF
    // end) instead of leaving a residual glow.
    nora_pwm_generator_set_duty( NORA_PWM_GEN_1, (PG_PER_COUNT*duty/100) * BRIGHTNESS_VAL );
}
// green
void pwm2_set_duty(uint8_t duty)
{
    // no minimum-glow floor (see pwm1_set_duty): duty==0 is a true off.
    nora_pwm_generator_set_duty( NORA_PWM_GEN_2, (PG_PER_COUNT*duty/100) * BRIGHTNESS_VAL );
}
// red
void pwm3_set_duty(uint8_t duty)
{
    // no minimum-glow floor (see pwm1_set_duty): duty==0 is a true off.
    nora_pwm_generator_set_duty( NORA_PWM_GEN_3, (PG_PER_COUNT*duty/100) * BRIGHTNESS_VAL_RED );
}


//===========================================================
// Local Function (file scope)
//===========================================================

static bool led_generator_init(nora_pwm_generator_id_t id, nora_gpio_rp_t rp_h)
{
    nora_pwm_generator_cfg_t cfg = { 0 };

    cfg.period          = PG_PER_COUNT;
    cfg.duty_init        = 1;   // LED off
    cfg.dead_time_high   = LED_DEAD_TIME;
    cfg.dead_time_low    = LED_DEAD_TIME;
    cfg.rp_h             = rp_h;
    cfg.rp_l             = (nora_gpio_rp_t)0;   // PWMxL never routed for the LED generators
    cfg.pen_h            = true;
    cfg.pen_l            = true;   // matches the previously-shipped register state
    cfg.output_mode      = NORA_PWM_MODE_COMPLEMENTARY;
    cfg.trigger_follows  = false;
    cfg.is_module_master = true;

    return nora_pwm_generator_init(id, &cfg);
}

static bool pwm_prepare_clock_or_stop(void)
{
#if !APP_USE_PWM_AUDIO
    return true;
#else
    nora_clock_status_t detail = NORA_CLOCK_OK;
    sonora_clock_pwm_status_t status = sonora_clock_pwm_prepare(&detail);

    if( status == SONORA_CLOCK_PWM_OK )
    {
        return true;
    }

    if( status == SONORA_CLOCK_PWM_ERR_REFI1_ROUTE )
    {
        printf(" WARNING: pwm_led_init: GPIO/PPS configuration failed -- aborting PWM init\n");
        return false;
    }

    if( status == SONORA_CLOCK_PWM_ERR_PLL2 )
    {
        /* One status, plus the backend's phase code. The four per-phase timeout
         * status values this used to test are gone from the Clock HAL: which phase
         * of a silicon sequence stalled is not portable, so it is now a
         * backend-defined detail printed alongside rather than five status values
         * every consumer had to enumerate. */
        printf( "PWM PLL2 clock configuration failed. status=%d diag=%u\n",
                (int)detail, (unsigned)nora_clock_last_diag() );
        if( detail == NORA_CLOCK_ERR_TIMEOUT )
        {
            printf( "Check REFI1 / MCLK source.\n" );
        }
    }
    else if( status == SONORA_CLOCK_PWM_ERR_CLKGEN5 )
    {
        printf( "PWM CLKGEN5 clock configuration failed. status=%d\n", (int)detail );
    }
    pwm_clock_fail_stop();
    return false;
#endif
}

static void pwm_clock_fail_stop(void)
{
    while( 1 )
    {
        Nop();
    }
}
