// Sonora board button and LED device support.
#ifndef BUTTON_LED_H
#define BUTTON_LED_H

//===========================================================
// INCLUDES
//===========================================================
#include <stdbool.h>
#include <stdint.h>


//===========================================================
// Definition
//===========================================================

#ifndef BUTTON_LONG_PRESS_MS
#define BUTTON_LONG_PRESS_MS    (1000UL)
#endif

//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum
{
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_PRESSED,
    BUTTON_EVENT_RELEASED,
    BUTTON_EVENT_LONG_PRESS_REACHED,
    BUTTON_EVENT_LONG_PRESSED,
} BUTTON_EVENT_t;


//===========================================================
// Variables
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

extern bool           BUTTON_Init( void );   /* false if any pin config failed */
extern bool           BUTTON_IsPressed( uint8_t id );
extern BUTTON_EVENT_t BUTTON_GetEvent( uint8_t id );

extern bool           TOUCH_IsPressed( uint8_t id );
extern BUTTON_EVENT_t TOUCH_GetEvent( uint8_t id );

extern bool   LEDs_Init( void );   /* false if any pin config failed */
extern void   LED_On( uint8_t led );
extern void   LED_Off( uint8_t led );
extern void   LED_Toggle( uint8_t led );

extern void   LED_Set_Mask( uint8_t led );

// Boot-fault indicator: show an error code on the LED bank and never return.
//   LED0        : heartbeat -- toggled forever so a lit-but-frozen board is
//                 distinguishable from a live one.
//   LED1..LEDn  : static binary encoding of `code` (bit0 -> LED1, bit1 -> LED2, ...).
// Self-contained and dependency-free: it configures the LED pins itself
// (idempotent with LEDs_Init) and paces the heartbeat with a busy-wait loop, so
// it is safe to call from the earliest boot faults -- before LEDs_Init(), the
// timers, or even a confirmed system clock (at a clock fault the heartbeat rate
// is only approximate, which is fine for a visual fault signal).
// On a board with no LEDs (LED_COUNT == 0) it degrades to an idle spin.
extern void   LED_fault_indicate_forever( uint8_t code );



#endif	//!_BUTTON_LED_H

