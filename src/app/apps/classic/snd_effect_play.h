#ifndef _SND_EFFECT_PLAY_H
#define	_SND_EFFECT_PLAY_H

#include <stdbool.h>
#include <stdint.h>

#if defined(ENA_SND_EFFECT_PLAY)
//===========================================================
// INCLUDES
//===========================================================


//===========================================================
// Definition
//===========================================================


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum se_tone_id
{
    SE_TONE_ON    = 0,
    SE_TONE_OFF   = 1,

    SE_TONE_NOTIF = 2,

    SE_TONE_NUM    // must be bottom for the count

} ENUM_SE_TONE_ID;



//===========================================================
// Variables
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

extern void  snd_effect_int( uint32_t sample_rate_Hz );
extern void  snd_effect_set_sample_rate( uint32_t sample_rate_Hz );
extern bool  snd_effect_verify( void );
extern bool  snd_effect_prepare_flash_data( void );

extern void  snd_effect_play_se( uint8_t id );
extern void  wav_to_tdm_float_process(const float*  in_buf,
                                                float*  out_buf,
                                                int     num_proc_ch);

#else

#define snd_effect_play_se( ... )           // do nothing



#endif //defined(ENA_SND_EFFECT_PLAY)
#endif //!_SND_EFFECT_PLAY_H

