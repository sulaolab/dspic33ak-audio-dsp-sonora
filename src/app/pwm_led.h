#ifndef _PWM_LED_H
#define	_PWM_LED_H


//===========================================================
// Function Prototype
//===========================================================

// RGB LED bring-up (PG1/2/3 via hal_pwm) + the module-wide PWM clock-source
// selection every generator on the device shares (see hal_pwm/nora_pwm.h).
// Call before any other PWM generator (e.g. the Classic audio PWM DAC) is
// initialized.
extern void   pwm_led_init(void);

extern void   pwm1_set_duty( uint8_t duty );
extern void   pwm2_set_duty( uint8_t duty );
extern void   pwm3_set_duty( uint8_t duty );


#endif //!_PWM_LED_H
