#ifndef SONORA_CLASSIC_AUDIO_PATH_H
#define SONORA_CLASSIC_AUDIO_PATH_H

#include <stdint.h>

/*
 * Classic/DRC-owned audio processing path.
 *
 * The caller owns transport lifecycle, DMA-half validation and co-clock safety.
 * prepare() is called while muted when the A-domain rate changes. process()
 * interprets the PCM slots and writes the supplied transport output halves.
 */
void classic_audio_path_prepare( uint32_t sample_rate_hz );
void classic_audio_path_reset( void );
void classic_audio_path_process( const int32_t* src_ptr,
                                 int32_t*       dest_ptr_a,
                                 int32_t*       dest_ptr_b );

#endif /* SONORA_CLASSIC_AUDIO_PATH_H */
