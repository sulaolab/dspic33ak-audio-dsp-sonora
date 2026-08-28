
//===========================================================
// INCLUDES
//===========================================================
#include "resolved_board_config.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "hal_touch/nora_touch.h"

#include "nora_gpio.h"   /* RP-first GPIO API (LEDs/buttons are RP pins) */
#include "timer_app.h"

#include "board/devices/button_led.h"


//===========================================================
// Definition
//===========================================================

#if !defined(BUTTON_LONG_PRESS_MS)
// #define BUTTON_LONG_PRESS_MS     (1000UL)
 #define BUTTON_LONG_PRESS_MS     (300UL)
#endif //!defined(BUTTON_LONG_PRESS_MS)

#if !defined(BUTTON_DEBOUNCE_MS)
// Mechanical-switch contact settle time. A raw level must hold this long before
// the edge state machine accepts it, so contact bounce on a quick/sloppy press
// no longer produces spurious release->press->release events.
 #define BUTTON_DEBOUNCE_MS       (15UL)
#endif //!defined(BUTTON_DEBOUNCE_MS)


//===========================================================
// Board pin map (the only board-specific GPIO knowledge here)
//===========================================================
//
// LEDs and buttons are normal board GPIO with RP numbers, so they are addressed
// by RP (the preferred interface) and driven through the RP-first GPIO HAL. RP
// values are the pin's RPn (= GPIO packed pin + 1); the port/bit name is kept in
// the comment for readability.
//
//   LEDs are active-high.
//   Buttons are active-low (pressed == pin reads low).
//
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE

  #define LED_COUNT      (8u)
  #define BUTTON_COUNT   (3u)

  static const nora_gpio_rp_t s_led_rp[LED_COUNT] =
  {
      36u,   // LED0  RP36/RC3
      37u,   // LED1  RP37/RC4
      38u,   // LED2  RP38/RC5
      39u,   // LED3  RP39/RC6
      40u,   // LED4  RP40/RC7
      41u,   // LED5  RP41/RC8
      42u,   // LED6  RP42/RC9
      43u,   // LED7  RP43/RC10
  };

  // index 0 unused (button id is 1..BUTTON_COUNT)
  static const nora_gpio_rp_t s_button_rp[1u + BUTTON_COUNT] =
  {
      0u,
      22u,   // button 1  RP22/RB5
      21u,   // button 2  RP21/RB4
      7u,    // button 3  RP7/RA6
  };

#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE

  #define LED_COUNT      (8u)
  #define BUTTON_COUNT   (3u)

  static const nora_gpio_rp_t s_led_rp[LED_COUNT] =
  {
      41u,   // LED0  RP41/RC8
      42u,   // LED1  RP42/RC9
      43u,   // LED2  RP43/RC10
      44u,   // LED3  RP44/RC11
      45u,   // LED4  RP45/RC12
      46u,   // LED5  RP46/RC13
      47u,   // LED6  RP47/RC14
      48u,   // LED7  RP48/RC15
  };

  // index 0 unused (button id is 1..BUTTON_COUNT)
  static const nora_gpio_rp_t s_button_rp[1u + BUTTON_COUNT] =
  {
      0u,
      84u,   // button 1  RP84/RF3
      81u,   // button 2  RP81/RF0
      19u,   // button 3  RP19/RB2
  };

#else

  #define LED_COUNT      (0u)
  #define BUTTON_COUNT   (0u)

  static const nora_gpio_rp_t s_led_rp[1]    = { 0u };   // unused
  static const nora_gpio_rp_t s_button_rp[1] = { 0u };   // unused

#endif // RESOLVED_BOARD_TARGET


//===========================================================
// Function Prototype
//===========================================================

//===========================================================
// Variables
//===========================================================




//===========================================================
// Global Function
//===========================================================

bool BUTTON_Init(void)
{
    bool ok = true;
    for( uint8_t id = 1u; id <= BUTTON_COUNT; id++ )
    {
        // Digital input, no pull (external pull assumed by the board).
        ok = nora_gpio_rp_config_digital_input( s_button_rp[id] ) && ok;
    }
    return ok;
}


// id: button = 1, 2, 3
bool BUTTON_IsPressed( uint8_t id )
{
    if( (id < 1u) || (id > BUTTON_COUNT) )
    {
        return false;
    }
    // Active low: pressed when the pin reads Low. A read ERROR (-1) or High is
    // treated as not-pressed (the 3-state level must not be used as a plain bool).
    return (nora_gpio_rp_read( s_button_rp[id] ) == NORA_GPIO_LEVEL_LOW);
}


BUTTON_EVENT_t BUTTON_GetEvent( uint8_t id )
{
    static bool     previous_state[4]        = {false/*dummy*/, false, false, false}; // not used of id=0
    static bool     long_press_reached[4]    = {false/*dummy*/, false, false, false}; // not used of id=0
    static uint32_t press_start_tick[4]      = {0, 0, 0, 0};

    // Per-id debounce (stable-confirm): the raw level must hold for
    // BUTTON_DEBOUNCE_MS before it is accepted as the current level.
    static bool     db_level[4]              = {false, false, false, false}; // confirmed level
    static bool     db_cand[4]               = {false, false, false, false}; // candidate raw level
    static uint32_t db_cand_tick[4]          = {0, 0, 0, 0};                  // when candidate first seen

    if( (id == 0)||(id >= 4) )  return BUTTON_EVENT_NONE;

    uint32_t now = GetTicks();
    bool     raw = BUTTON_IsPressed(id);
    if( raw != db_cand[id] )
    {
        db_cand[id]      = raw;
        db_cand_tick[id] = now;   // raw changed -> restart settle window
    }
    else if( (uint32_t)(now - db_cand_tick[id]) >= BUTTON_DEBOUNCE_MS )
    {
        db_level[id]     = raw;   // stable long enough -> accept
    }
    bool current = db_level[id];  // feed edge logic with the debounced level

    if( !previous_state[id] && current )
    {
        press_start_tick[id]   = now;
        long_press_reached[id] = false;
        previous_state[id]     = current;
        return BUTTON_EVENT_PRESSED;
    }

    if( previous_state[id] && current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);

        if( !long_press_reached[id] && (pressed_time >= BUTTON_LONG_PRESS_MS) )
        {
            long_press_reached[id] = true;
            previous_state[id]     = current;
            return BUTTON_EVENT_LONG_PRESS_REACHED;
        }
    }

    if( previous_state[id] && !current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);
        bool     long_pressed = (pressed_time >= BUTTON_LONG_PRESS_MS);

        long_press_reached[id] = false;
        previous_state[id]     = current;

        if( long_pressed )
        {
            return BUTTON_EVENT_LONG_PRESSED;
        }
        return BUTTON_EVENT_RELEASED;
    }

    previous_state[id] = current;

    return BUTTON_EVENT_NONE;
}




// id: touch = 1, 2, 3
bool TOUCH_IsPressed( uint8_t id )
{
    switch( id )
    {
#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE

#elif (RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE) && defined(ENA_OPEN_TOUCH_EXCLUSIVE)
    /* Same three pads, open library instead of the vendor one. Sourcing them
     * here rather than letting the application talk to nora_touch directly is
     * what keeps the two builds comparable: TOUCH_GetEvent() below, and every
     * consumer above it, is then identical code in both images, so a difference
     * in behaviour is a difference between the touch libraries and not between
     * two event layers. It is also the answer to "who owns long press" — this
     * layer already does, for buttons and for the vendor build. */
    case 1:
        return nora_touch_is_pressed(0);
    case 2:
        return nora_touch_is_pressed(1);
    case 3:
        return nora_touch_is_pressed(2);

#else
#endif // RESOLVED_BOARD_TARGET
    default:
        break;
    }
    return false;
}


/* Console trace of the touch events this layer produces, off unless the build
 * asks for it with -Define ENA_TOUCH_EVENT_TRACE=1.
 *
 * It sits here, above TOUCH_IsPressed()'s #if, on purpose: this is the one place
 * both the vendor build and the open build pass through, so the same code prints
 * the same line shape either way and the behavioural comparison in the tuning
 * manual's appendix A can be scored by counting lines instead of by watching
 * LEDs. Scoring it from each library's own logging would compare the logging.
 *
 * The vendor build is untouched by this — nothing here reads its source, and the
 * trace only observes the boolean TOUCH_IsPressed() already returned. */
#if defined(ENA_TOUCH_EVENT_TRACE)
static BUTTON_EVENT_t touch_trace( uint8_t id, BUTTON_EVENT_t ev )
{
    switch( ev )
    {
    case BUTTON_EVENT_PRESSED:            printf(" TOUCH_EV(%u): press\n",      (unsigned)id ); break;
    case BUTTON_EVENT_RELEASED:           printf(" TOUCH_EV(%u): release\n",    (unsigned)id ); break;
    case BUTTON_EVENT_LONG_PRESS_REACHED: printf(" TOUCH_EV(%u): long\n",       (unsigned)id ); break;
    case BUTTON_EVENT_LONG_PRESSED:       printf(" TOUCH_EV(%u): release_long\n", (unsigned)id ); break;
    default: break;
    }
    return ev;
}
#else
 #define touch_trace(id, ev)   (ev)
#endif //defined(ENA_TOUCH_EVENT_TRACE)


BUTTON_EVENT_t TOUCH_GetEvent( uint8_t id )
{
    static bool     previous_state[4]        = {false, false, false, false}; // not used of id=0
    static bool     long_press_reached[4]    = {false, false, false, false}; // not used of id=0
    static uint32_t press_start_tick[4]      = {0, 0, 0, 0};

    if( (id == 0)||(id >= 4) )  return BUTTON_EVENT_NONE;

    bool     current = TOUCH_IsPressed(id);
    uint32_t now     = GetTicks();

    if( !previous_state[id] && current )
    {
        press_start_tick[id]   = now;
        long_press_reached[id] = false;
        previous_state[id]     = current;
        return touch_trace(id, BUTTON_EVENT_PRESSED);
    }

    if( previous_state[id] && current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);

        if( !long_press_reached[id] && (pressed_time >= BUTTON_LONG_PRESS_MS) )
        {
            long_press_reached[id] = true;
            previous_state[id]     = current;
            return touch_trace(id, BUTTON_EVENT_LONG_PRESS_REACHED);
        }
    }

    if( previous_state[id] && !current )
    {
        uint32_t pressed_time = (uint32_t)(now - press_start_tick[id]);
        bool     long_pressed = (pressed_time >= BUTTON_LONG_PRESS_MS);

        long_press_reached[id] = false;
        previous_state[id]     = current;

        if( long_pressed )
        {
            return touch_trace(id, BUTTON_EVENT_LONG_PRESSED);
        }
        return touch_trace(id, BUTTON_EVENT_RELEASED);
    }

    previous_state[id] = current;

    return BUTTON_EVENT_NONE;
}




bool LEDs_Init(void)
{
    // LEDs are active-high standard digital outputs. config_digital_output seeds
    // LAT Low (LED off) BEFORE enabling the driver, so no LED briefly lights
    // during bring-up (no custom config struct needed for this common case).
    bool ok = true;
    for( uint8_t i = 0u; i < LED_COUNT; i++ )
    {
        ok = nora_gpio_rp_config_digital_output( s_led_rp[i], false ) && ok;
    }
    return ok;
}

void LED_On( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_set( s_led_rp[led] );      // active high
    }
    else
    {
        // any out-of-range id (e.g. 0xFF) means "all"
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_set( s_led_rp[i] );
        }
    }
}

void LED_Off( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_clear( s_led_rp[led] );
    }
    else
    {
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_clear( s_led_rp[i] );
        }
    }
}

void LED_Toggle( uint8_t led )
{
    if( led < LED_COUNT )
    {
        (void)nora_gpio_rp_toggle( s_led_rp[led] );
    }
    else
    {
        for( uint8_t i = 0u; i < LED_COUNT; i++ )
        {
            (void)nora_gpio_rp_toggle( s_led_rp[i] );
        }
    }
}


void LED_Set_Mask( uint8_t led )
{
    // bit i -> LEDi (bit0 = first LED), active high
    for( uint8_t i = 0u; i < LED_COUNT; i++ )
    {
        (void)nora_gpio_rp_write( s_led_rp[i], ((led >> i) & 0x01u) != 0u );
    }
}


void LED_fault_indicate_forever( uint8_t code )
{
#if (LED_COUNT == 0u)
    // No LEDs on this board: nothing to show, just halt.
    (void)code;
    for( ;; )
    {
        Nop();
    }
#else
    // Half-period of the LED0 heartbeat, in busy-loop iterations. Deliberately a
    // raw spin count (no timer / clock dependency): at the nominal system clock
    // this is a sub-second blink; at a clock fault the true clock is unknown so
    // the rate only approximates -- acceptable for a visual "alive" indicator.
    static const uint32_t LED_FAULT_HEARTBEAT_LOOPS = 4000000UL;

    // Bring the LED pins up ourselves -- this may run before LEDs_Init().
    // Idempotent: re-seeding the same digital outputs is harmless.
    (void)LEDs_Init();

    // LED1..LEDn: static binary encoding of the fault code (bit0 -> LED1).
    // LED0 is reserved for the heartbeat below.
    for( uint8_t i = 1u; i < LED_COUNT; i++ )
    {
        if( ((code >> (uint8_t)(i - 1u)) & 0x01u) != 0u )
        {
            LED_On( i );
        }
        else
        {
            LED_Off( i );
        }
    }

    // LED0: heartbeat forever.
    for( ;; )
    {
        LED_Toggle( 0u );
        for( volatile uint32_t d = 0u; d < LED_FAULT_HEARTBEAT_LOOPS; d++ )
        {
            Nop();
        }
    }
#endif // LED_COUNT
}




//===========================================================
// Local Function
//===========================================================
