#include "app_specific_config_defs.h"   // APP_TARGET (self-contained: independent of include order)
#if APP_TARGET == APP_TARGET_AK512



/*
 * Each tone carries its own stored sample rate.  The tones are narrow-band, so
 * storing every one of them at the 48 kHz processing rate wasted program flash;
 * the runtime SRC in snd_effect_play.c already converts an arbitrary source rate
 * to the processing rate, so *_rate is what it converts from.
 *
 * The table is generated -- see tools/classic/gen_tone_data_int16.py.
 */
typedef struct {

    const int16_t*  Tone_ON;
          uint16_t  Tone_ON_size;
          uint16_t  Tone_ON_array_s;
          uint32_t  Tone_ON_rate;       // Hz, stored sample rate

    const int16_t*  Tone_OFF;
          uint16_t  Tone_OFF_size;
          uint16_t  Tone_OFF_array_s;
          uint32_t  Tone_OFF_rate;      // Hz, stored sample rate

    const int16_t*  Tone_Notif;
          uint16_t  Tone_Notif_size;
          uint16_t  Tone_Notif_array_s;
          uint32_t  Tone_Notif_rate;    // Hz, stored sample rate

} Button_Tone_i16_t;


extern const Button_Tone_i16_t Button_Tone_i16;







#endif //APP_TARGET == APP_TARGET_AK512