#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_utils.h"

#include "audio_sample_delay.h"


#if defined(ENA_SAMPLE_DELAY)
//===========================================================
// Definition
//===========================================================

// This module applies per-channel pure sample delay to channel-major float audio.
//
// Buffer layout:
//   buf[0 * samples + n] = ch0
//   buf[1 * samples + n] = ch1
//   buf[2 * samples + n] = ch2
//   buf[3 * samples + n] = ch3
//
// Delay rule:
//   delay_samples[ch] == 0 : bypass equivalent
//   delay_samples[ch] >  0 : output delayed sample
//
// Delay memory:
//   - One shared pool is used by all channels.
//   - Each channel obtains a contiguous area from the pool.
//   - Fragmentation is not managed.  When a delay size changes, all channel
//     areas are re-laid out from the start of the pool.
//   - If a requested layout does not fit in the pool, the request is rejected
//     and the current setting/state is kept.
//
// Sample-rate policy:
//   - The actual processing path is sample-based and does not depend on Fs.
//   - The sample_rate_hz value is used for ms <-> samples conversion and status
//     printout.
//   - For the same delay time in ms, 96 kHz needs twice as many delay samples
//     as 48 kHz.
//
// Notes:
//   - In-place processing is supported.
//   - Changing delay_samples clears all delay states before applying the new layout.
//   - delay_samples values larger than the available pool are rejected, not clipped.


//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    uint16_t delay_samples;
    uint16_t buffer_samples;
    uint16_t write_index;
    uint16_t offset;
    float*   buffer;

} audio_sample_delay_ch_state_t;


typedef struct
{
    uint16_t num_ch;
    uint32_t sample_rate_hz;

    float delay_pool[AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT];

    audio_sample_delay_ch_state_t ch[AUDIO_SAMPLE_DELAY_MAX_CH];

    uint16_t used_samples;

} audio_sample_delay_t;


//===========================================================
// Function Prototype
//===========================================================

static void     audio_sample_delay_init( audio_sample_delay_t* pdelay,
                                         uint32_t              sample_rate_hz );
static void     audio_sample_delay_clear_state( audio_sample_delay_t* pdelay );
static bool     audio_sample_delay_is_layout_available( const audio_sample_delay_t* pdelay,
                                                        const uint16_t              delay_samples[],
                                                              uint16_t*             used_samples );
static void     audio_sample_delay_apply_layout( audio_sample_delay_t* pdelay,
                                                 const uint16_t        delay_samples[] );
static bool     audio_sample_delay_set_delay_samples( audio_sample_delay_t* pdelay,
                                                      uint16_t              channel,
                                                      uint16_t              delay_samples );
static bool     audio_sample_delay_get_delay_samples( const audio_sample_delay_t* pdelay,
                                                            uint16_t              channel,
                                                            uint16_t*             delay_samples );
static float    audio_sample_delay_samples_to_ms( const audio_sample_delay_t* pdelay,
                                                        uint16_t              samples );
static uint32_t audio_sample_delay_samples_to_ms_x10( const audio_sample_delay_t* pdelay,
                                                            uint16_t              samples );
static void     audio_sample_delay_process( audio_sample_delay_t* pdelay,
                                            float*                buf,
                                            uint16_t              samples );
static void     audio_sample_delay_debug_print_status( const audio_sample_delay_t* pdelay );


//===========================================================
// Variables
//===========================================================

static __attribute__((far)) audio_sample_delay_t My_AudioSampleDelay;


//===========================================================
// Global Function
//===========================================================


//===========================================================
// Local Function
//===========================================================

static void audio_sample_delay_init( audio_sample_delay_t* pdelay,
                                     uint32_t              sample_rate_hz )
{
    if (pdelay == NULL)
    {
        return;
    }

    if (sample_rate_hz == 0u)
    {
        sample_rate_hz = (uint32_t)SAMPLE_RATE;
    }

    app_memset(pdelay, 0x00, sizeof(audio_sample_delay_t));

    pdelay->sample_rate_hz = sample_rate_hz;
    pdelay->num_ch         = AUDIO_SAMPLE_DELAY_NUM_CH;

    if (pdelay->num_ch > AUDIO_SAMPLE_DELAY_MAX_CH)
    {
        pdelay->num_ch = AUDIO_SAMPLE_DELAY_MAX_CH;
    }
}


static void audio_sample_delay_clear_state( audio_sample_delay_t* pdelay )
{
    if (pdelay == NULL)
    {
        return;
    }

    app_memset(pdelay->delay_pool, 0x00, sizeof(pdelay->delay_pool));

    for( uint16_t ch=0; ch<AUDIO_SAMPLE_DELAY_MAX_CH; ch++ )
    {
        pdelay->ch[ch].write_index = 0u;
    }
}


static bool audio_sample_delay_is_layout_available( const audio_sample_delay_t* pdelay,
                                                    const uint16_t              delay_samples[],
                                                          uint16_t*             used_samples )
{
    uint32_t total_samples = 0u;

    if ((pdelay == NULL) || (delay_samples == NULL) || (used_samples == NULL))
    {
        return false;
    }

    for( uint16_t ch=0; ch<pdelay->num_ch; ch++ )
    {
        total_samples += (uint32_t)delay_samples[ch];

        if (total_samples > (uint32_t)AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT)
        {
            return false;
        }
    }

    *used_samples = (uint16_t)total_samples;

    return true;
}


static void audio_sample_delay_apply_layout( audio_sample_delay_t* pdelay,
                                             const uint16_t        delay_samples[] )
{
    uint16_t offset = 0u;

    if ((pdelay == NULL) || (delay_samples == NULL))
    {
        return;
    }

    for( uint16_t ch=0; ch<AUDIO_SAMPLE_DELAY_MAX_CH; ch++ )
    {
        pdelay->ch[ch].delay_samples  = 0u;
        pdelay->ch[ch].buffer_samples = 0u;
        pdelay->ch[ch].write_index    = 0u;
        pdelay->ch[ch].offset         = 0u;
        pdelay->ch[ch].buffer         = NULL;
    }

    for( uint16_t ch=0; ch<pdelay->num_ch; ch++ )
    {
        const uint16_t samples = delay_samples[ch];

        pdelay->ch[ch].delay_samples  = samples;
        pdelay->ch[ch].buffer_samples = samples;
        pdelay->ch[ch].write_index    = 0u;
        pdelay->ch[ch].offset         = offset;

        if (samples > 0u)
        {
            pdelay->ch[ch].buffer = &pdelay->delay_pool[offset];
            offset = (uint16_t)(offset + samples);
        }
        else
        {
            pdelay->ch[ch].buffer = NULL;
        }
    }

    pdelay->used_samples = offset;
}


static bool audio_sample_delay_set_delay_samples( audio_sample_delay_t* pdelay,
                                                  uint16_t              channel,
                                                  uint16_t              delay_samples )
{
    uint16_t new_delay_samples[AUDIO_SAMPLE_DELAY_MAX_CH];
    uint16_t used_samples;

    if (pdelay == NULL)
    {
        return false;
    }

    if (channel >= pdelay->num_ch)
    {
        return false;
    }

    for( uint16_t ch=0; ch<pdelay->num_ch; ch++ )
    {
        new_delay_samples[ch] = pdelay->ch[ch].delay_samples;
    }

    new_delay_samples[channel] = delay_samples;

    if (!audio_sample_delay_is_layout_available(pdelay,
                                                new_delay_samples,
                                                &used_samples))
    {
        return false;
    }

    (void)used_samples;

    if (pdelay->ch[channel].delay_samples != delay_samples)
    {
        audio_sample_delay_clear_state(pdelay);
        audio_sample_delay_apply_layout(pdelay, new_delay_samples);
    }

    return true;
}


static bool audio_sample_delay_get_delay_samples( const audio_sample_delay_t* pdelay,
                                                        uint16_t              channel,
                                                        uint16_t*             delay_samples )
{
    if ((pdelay == NULL) || (delay_samples == NULL))
    {
        return false;
    }

    if (channel >= pdelay->num_ch)
    {
        return false;
    }

    *delay_samples = pdelay->ch[channel].delay_samples;

    return true;
}


static float audio_sample_delay_samples_to_ms( const audio_sample_delay_t* pdelay,
                                                     uint16_t              samples )
{
    if ((pdelay == NULL) || (pdelay->sample_rate_hz == 0u))
    {
        return 0.0f;
    }

    return ((float)samples * 1000.0f) / (float)pdelay->sample_rate_hz;
}


static void audio_sample_delay_process( audio_sample_delay_t* pdelay,
                                        float*                buf,
                                        uint16_t              samples )
{
    if ((pdelay == NULL) || (buf == NULL) || (samples == 0u))
    {
        return;
    }

    /*
     * Chunk process path.
     *
     * - Skip channels with no allocated delay buffer.
     * - Process contiguous ring-buffer region without wrap check
     *   inside the innermost sample loop.
     */
    for( uint16_t ch=0u; ch<pdelay->num_ch; ch++ )
    {
        audio_sample_delay_ch_state_t* pst = &pdelay->ch[ch];

        uint16_t write_index;
        uint16_t buffer_samples;
        uint16_t remain;

        float* p_delay_base;
        float* p_delay;
        float* p_ch_buf;

        if ((pst->delay_samples == 0u) ||
            (pst->buffer_samples == 0u) ||
            (pst->buffer == NULL))
        {
            continue;
        }

        write_index    = pst->write_index;
        buffer_samples = pst->buffer_samples;
        p_delay_base   = pst->buffer;
        p_ch_buf       = &buf[ch * samples];
        remain         = samples;

        while( remain > 0u )
        {
            uint16_t chunk;
            uint16_t i;

            chunk = buffer_samples - write_index;

            if (chunk > remain)
            {
                chunk = remain;
            }

            p_delay = &p_delay_base[write_index];

            for( i=0u; i<chunk; i++ )
            {
                const float x       = *p_ch_buf;
                const float delayed = *p_delay;

                *p_delay++ = x;
                *p_ch_buf++ = delayed;
            }

            write_index += chunk;

            if (write_index >= buffer_samples)
            {
                write_index = 0u;
            }

            remain -= chunk;
        }

        pst->write_index = write_index;
    }
}


static uint32_t audio_sample_delay_samples_to_ms_x10( const audio_sample_delay_t* pdelay,
                                                            uint16_t              samples )
{
    if ((pdelay == NULL) || (pdelay->sample_rate_hz == 0u))
    {
        return 0u;
    }

    return ((uint32_t)samples * 10000u) / pdelay->sample_rate_hz;
}


static void audio_sample_delay_debug_print_status( const audio_sample_delay_t* pdelay )
{
    uint32_t used_bytes;
    uint32_t usage_x10;

    if (pdelay == NULL)
    {
        return;
    }

    used_bytes = (uint32_t)pdelay->used_samples * 4u;

    if (AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT > 0u)
    {
        usage_x10 = ((uint32_t)pdelay->used_samples * 1000u) /
                    (uint32_t)AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT;
    }
    else
    {
        usage_x10 = 0u;
    }

    printf("\n");
    printf("--------------------------------------------\n");
    printf("AUDIO SAMPLE DELAY STATUS\n");
    printf("sample_rate  = %lu Hz\n", (unsigned long)pdelay->sample_rate_hz);
    printf("num_ch       = %u\n", (unsigned)pdelay->num_ch);
    printf("used_bytes   = %lu/%lu\n", (unsigned long)used_bytes, (unsigned long)AUDIO_SAMPLE_DELAY_POOL_BYTES);
    printf("used_samples = %u/%u(0x%04x)\n", (unsigned)pdelay->used_samples, (unsigned)AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT, (unsigned)AUDIO_SAMPLE_DELAY_POOL_FLOAT_COUNT);
    printf("usage        = %lu.%lu%%\n",
           (unsigned long)(usage_x10 / 10u),
           (unsigned long)(usage_x10 % 10u));
    printf("\n");
    printf("ch   delay                offset     write\n");

    for( uint16_t ch=0; ch<pdelay->num_ch; ch++ )
    {
        uint32_t samples_10ms = audio_sample_delay_samples_to_ms_x10(pdelay,
                                                                     pdelay->ch[ch].delay_samples);

        printf("%2u  %5u(0x%04x)%3u.%ums   %5u     %5u\n",
               (unsigned)ch,
               (unsigned)pdelay->ch[ch].delay_samples,
               (unsigned)pdelay->ch[ch].delay_samples,
               (unsigned)samples_10ms / 10,
               (unsigned)samples_10ms % 10,
               (unsigned)pdelay->ch[ch].offset,
               (unsigned)pdelay->ch[ch].write_index);
    }

    printf("--------------------------------------------\n");
    printf("\n");
}


//===========================================================
// API
//===========================================================

void app_audio_sample_delay_init( uint32_t sample_rate_hz )
{
    audio_sample_delay_init(&My_AudioSampleDelay,
                            sample_rate_hz);
}


void app_audio_sample_delay_clear_state(void)
{
    audio_sample_delay_clear_state(&My_AudioSampleDelay);
}


bool app_audio_sample_delay_set_delay_samples( uint16_t channel,
                                               uint16_t delay_samples )
{
    return audio_sample_delay_set_delay_samples(&My_AudioSampleDelay,
                                                channel,
                                                delay_samples);
}


bool app_audio_sample_delay_get_delay_samples( uint16_t  channel,
                                               uint16_t* delay_samples )
{
    return audio_sample_delay_get_delay_samples(&My_AudioSampleDelay,
                                                channel,
                                                delay_samples);
}


void app_audio_sample_delay_process( float* buf )
{
    audio_sample_delay_process(&My_AudioSampleDelay,
                               buf,
                               APP_BLOCK_FRAMES);
}


void app_audio_sample_delay_debug_print_status(void)
{
    audio_sample_delay_debug_print_status(&My_AudioSampleDelay);
}
#endif //defined(ENA_SAMPLE_DELAY)
