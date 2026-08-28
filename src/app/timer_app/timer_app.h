#ifndef TIMER_APP_H
#define TIMER_APP_H

/*
 * timer_app.h
 * -----------
 * Sonora application timer compatibility layer.
 *
 * The reusable Timer1 HAL lives in hal_timer/nora_tick_timer.*. This layer
 * keeps the legacy application entry points that existing Sonora modules use.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void timer_app_init(void);
uint32_t GetTicks(void);
void delay_ms(uint16_t time_ms);
void delay_us(uint16_t time_us);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_APP_H */
