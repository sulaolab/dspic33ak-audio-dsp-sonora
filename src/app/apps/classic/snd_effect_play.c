
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>   // for fmaxf
#include <assert.h>
#include "timer_app.h"
#if APP_SND_EFFECT_EXTERNAL_SST26
#include "board/devices/SST26_drv.h"
#endif
#include "apps/shared/float_conversion.h"


#include "snd_effect_play.h"

#if defined(ENA_SND_EFFECT_PLAY) && APP_SND_EFFECT_INTERNAL_ADPCM
#include "tone_data_ima_adpcm.h"
#endif



#if !SONORA_APP_IS_CLASSIC
#  error "snd_effect_play.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#if defined(ENA_SND_EFFECT_PLAY)
//===========================================================
// Definition
//===========================================================

#define CLAMPF(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/*
 * Embedded sound-effect source data decodes to mono int16 at a per-tone
 * sample rate (Tone_*_rate; 12 kHz for the button clicks, 24 kHz for the
 * notification, because the tones are narrow-band and storing them all at
 * 48 kHz wasted program flash). AK512 stores that PCM in external SST26
 * flash; AK128 has no independent SST26 bus while MikroBUS-A carries the
 * continuous audio stream, so it decodes immutable IMA-ADPCM blocks from
 * internal Program Flash instead. Both backends feed the same runtime SRC.
 * The processing sample rate may be 44.1 kHz, 48 kHz or 96 kHz.
 *
 * Runtime SRC policy:
 * - source position is tracked in Q16 fixed-point format.
 * - source step = source_sample_rate / output_sample_rate, per tone.
 * - Linear interpolation is used between adjacent source samples.
 */
#define SND_EFFECT_SOURCE_SAMPLE_RATE_HZ  (48000u)   // fallback only

#define SE_PHASE_FRAC_BITS                (16u)
#define SE_PHASE_ONE_Q16                  (1UL << SE_PHASE_FRAC_BITS)
#define SE_PHASE_FRAC_MASK                (SE_PHASE_ONE_Q16 - 1UL)

/*
 * Worst case is one source sample per output sample plus one for interpolation,
 * which happens when the stored rate reaches the output rate:
 *  - 48k source -> 44.1k output needs about 35 source samples per 32 output samples.
 *  - 32k source -> 48k output needs about 22 source samples per 32 output samples.
 *
 * Keep APP_BLOCK_FRAMES*2 to cover 44.1/48/96k safely with interpolation, with
 * headroom for any higher processing rate a future build might use.
 * A stored rate above the output rate stays bounded by the
 * SND_EFFECT_WAV_READ_SAMPLES clamp in wav_to_tdm_float_process().
 */
#define SND_EFFECT_WAV_READ_SAMPLES       (APP_BLOCK_FRAMES * 2)





//===========================================================
// Enum & Struct typedef
//===========================================================

typedef enum se_play_state
{
    SE_SLEEP = 0,
    SE_START,
    SE_PLAY,

    SE_STATE_NUM    // must be bottom for the count

} ENUM_SE_PLAY;


typedef struct se_tone_info
{
    const void* pDat;
    uint32_t    size;       // stored byte size (PCM or ADPCM)
    uint32_t    arraysize;  // decoded number of int16 samples
    uint32_t    flash_addr; // byte address in external flash (AK512 only)
    uint32_t    rate_Hz;    // stored sample rate of this tone

} ST_SE_TONE_INFO;

#if APP_SND_EFFECT_INTERNAL_ADPCM
typedef struct se_adpcm_decoder
{
    const snd_effect_adpcm_asset_t* asset;
    uint32_t next_sample;
    uint32_t block_start;
    uint16_t block_samples;
    uint16_t nibble_index;
    int16_t  predictor;
    uint8_t  step_index;
    uint8_t  tone_id;
    uint8_t  valid;
    uint8_t  have_last;
    uint32_t last_sample_index;
    int16_t  last_sample;
} ST_SE_ADPCM_DECODER;
#endif




//===========================================================
// Function Prototype
//===========================================================

#if APP_SND_EFFECT_EXTERNAL_SST26
static void     snd_effect_flash_probe( void );
static bool     snd_effect_verify_tone_data( uint8_t id );
#endif
static void     snd_effect_init_tone_info( void );
static uint32_t snd_effect_get_tone_size( uint8_t id );
static bool     snd_effect_read_tone_samples( uint8_t id,
                                              uint32_t first_sample,
                                              int16_t* buf,
                                              uint32_t sample_count );

#if APP_SND_EFFECT_INTERNAL_ADPCM
static void     snd_effect_adpcm_reset( void );
static bool     snd_effect_adpcm_load_block( uint8_t id, uint32_t block_index );
static bool     snd_effect_adpcm_decode_next( int16_t* sample );
static bool     snd_effect_adpcm_seek( uint8_t id, uint32_t sample_index );
#endif

static inline uint32_t local_get_valid_sample_rate( uint32_t sample_rate_Hz );
static inline uint32_t local_tone_source_rate( uint8_t id );
static inline uint32_t local_calc_phase_step_q16( uint32_t source_sample_rate_Hz,
                                                  uint32_t output_sample_rate_Hz );
static inline void     local_copy_pass_through( const float* in_buf,
                                                float*       out_buf,
                                                int          frameSize,
                                                int          num_proc_ch );
static inline int      local_calc_src_frames_to_read( uint32_t phase_q16,
                                                      uint32_t phase_step_q16,
                                                      uint32_t totalFrames,
                                                      int      out_frames );


//===========================================================
// Variables
//===========================================================

#if APP_SND_EFFECT_EXTERNAL_SST26
#include "tone_data_int16.h"
#elif APP_SND_EFFECT_INTERNAL_ADPCM
_Static_assert( SND_EFFECT_ADPCM_ASSET_COUNT == SE_TONE_NUM,
                "ADPCM asset order/count must match ENUM_SE_TONE_ID" );
#endif

static ENUM_SE_PLAY    Ply_Status   = SE_SLEEP;
static uint8_t         Req_Tone_Id  = SE_TONE_ON;
static uint8_t         Play_Tone_Id = SE_TONE_ON;
static int16_t         WavData[ SND_EFFECT_WAV_READ_SAMPLES ];

static ST_SE_TONE_INFO Tone_Info[ SE_TONE_NUM ];
static bool            Tone_Info_Initialized = false;

static uint32_t        Snd_Effect_Sample_Rate_Hz = (uint32_t)SAMPLE_RATE;
static uint32_t        Wave_Phase_Q16            = 0u;
static uint32_t        Wave_Phase_Step_Q16       = SE_PHASE_ONE_Q16;

#if APP_SND_EFFECT_INTERNAL_ADPCM
static ST_SE_ADPCM_DECODER Adpcm_Decoder;

static const int16_t Adpcm_Step_Table[89] =
{
       7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
      19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
      50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
     130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
     337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
     876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
   15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t Adpcm_Index_Table[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
#endif




//===========================================================
// Global Function
//===========================================================

void snd_effect_int( uint32_t sample_rate_Hz )
{
#if APP_SND_EFFECT_EXTERNAL_SST26
    // Bring-up probe: confirm the external flash device answers before we rely
    // on it to store the tone data.  This absorbs the former driver-side
    // "SST26_test" self-test so the SST26 driver stays a pure device driver and
    // the flash sound-effect feature owns its own bring-up diagnostics.
    snd_effect_flash_probe();
#endif

    snd_effect_init_tone_info();

    snd_effect_set_sample_rate( sample_rate_Hz );

    snd_effect_prepare_flash_data();
}


/*
 * snd_effect_set_sample_rate
 * ---------------------------
 * Re-derive the SRC step for a processing-rate change (e.g. codec-A restart
 * with a new output rate) without touching the tone data itself. Safe to call
 * whether or not a tone is currently playing: stops any in-flight playback and
 * resets the ADPCM decoder so the next snd_effect_play_se() starts clean.
 */
void snd_effect_set_sample_rate( uint32_t sample_rate_Hz )
{
    Snd_Effect_Sample_Rate_Hz = local_get_valid_sample_rate( sample_rate_Hz );
    Wave_Phase_Step_Q16       = local_calc_phase_step_q16( local_tone_source_rate( Req_Tone_Id ),
                                                           Snd_Effect_Sample_Rate_Hz );
    Wave_Phase_Q16            = 0u;
    Ply_Status                = SE_SLEEP;
#if APP_SND_EFFECT_INTERNAL_ADPCM
    snd_effect_adpcm_reset();
#endif
}


bool snd_effect_verify( void )
{
#if APP_SND_EFFECT_EXTERNAL_SST26
    if( !snd_effect_verify_tone_data( SE_TONE_ON ) )
    {
        return false;
    }

    if( !snd_effect_verify_tone_data( SE_TONE_OFF ) )
    {
        return false;
    }

    if( !snd_effect_verify_tone_data( SE_TONE_NOTIF ) )
    {
        return false;
    }

    return true; // good
#else
    // Internal assets are immutable linker data. The generator's --check mode
    // verifies their byte stream and the normal device programmer supplies ECC.
    return Tone_Info_Initialized;
#endif
}


/*
 * snd_effect_prepare_flash_data
 * -----------------------------
 * Purpose : Verify and program sound effect WAV data in external flash.
 * Notes   : This function owns the tone data layout. SST26_drv only provides
 *           low-level flash access such as read/program/erase/verify.
 */
bool snd_effect_prepare_flash_data( void )
{
#if APP_SND_EFFECT_EXTERNAL_SST26
    if( snd_effect_verify() )
    {
        return true; // good
    }

    // need to write the external flash
    printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf(" External Flash: Verify NOT OK!!\n");
    printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");

    sst26_unprotect_all();

    sst26_chip_erase();
    printf(" sst26_chip_erase\n");
//    printf("sst26_sector_erase_4k\n");
//    sst26_sector_erase_4k(0x00000000);


    uint32_t wr = 0;
    uint8_t  id;
    for( id = 0; id < SE_TONE_NUM; id++ )
    {
        Tone_Info[id].flash_addr = wr;
        wr = sst26_write_next(wr, (const uint8_t *)Tone_Info[id].pDat, Tone_Info[id].size);
    }

    sst26_protect_all();

    if( snd_effect_verify() )
    {
        printf("--------------------\n");
        printf(" Verify OK.\n");
        printf("--------------------\n");
        return true; // good
    }

    printf("@@@@@@@@@@@@@@@@@@@@\n");
    printf(" Verify NOT OK!!\n");
    printf("@@@@@@@@@@@@@@@@@@@@\n");
    return false;
#else
    return snd_effect_verify();
#endif
}


void snd_effect_play_se( uint8_t id )
//void wav_to_tdm_play_se_id( uint8_t id )
{
    if( id >= SE_TONE_NUM )
    {
        return;
    }

    Req_Tone_Id = id;
    Ply_Status  = SE_START;
}


/**
 * wav_to_tdm_float_process
 * ----------------------------
 * Mix a mono notification (decoded int16 samples) into channel-major float I/O buffers.
 * - I/O are normalized floats in [-1.0, +1.0].
 * - Embedded WAV source is mono int16, stored at each tone's own rate.
 * - Source sample position is SRC-converted to the processing sample rate using Q16 phase.
 * - Each int16 sample is normalized by 1/32768.0f and then scaled by Pre_Gain_WAV (linear).
 * - Mixed to all processing channels. The mono WAV sample is added equally to every channel.
 * - frameSize is fixed to APP_BLOCK_FRAMES inside this module.
 */
void wav_to_tdm_float_process(const float* in_buf,
                                        float* out_buf,
                                        int    num_proc_ch )
{
    const int frameSize = APP_BLOCK_FRAMES;

    int      framesToProcess;
    int      srcFramesToRead;
    uint32_t totalFrames;
    uint32_t base_idx;
    uint32_t local_phase_q16;

    // Playback state handling (pass-through when stopped)
    if (Ply_Status == SE_SLEEP)
    {
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        return;
    }

    if (Ply_Status == SE_START)
    {
        Wave_Phase_Q16 = 0u;
        Play_Tone_Id   = Req_Tone_Id;
        Ply_Status     = SE_PLAY;

        // Each tone is stored at its own sample rate -> re-derive the SRC step.
        Wave_Phase_Step_Q16 = local_calc_phase_step_q16( local_tone_source_rate( Play_Tone_Id ),
                                                         Snd_Effect_Sample_Rate_Hz );
#if APP_SND_EFFECT_INTERNAL_ADPCM
        snd_effect_adpcm_reset();
#endif
    }
    else if (Ply_Status != SE_PLAY)
    {
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        Ply_Status     = SE_SLEEP;
        return;
    }

    totalFrames = snd_effect_get_tone_size( Play_Tone_Id );

    if( totalFrames == 0u )
    {
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        Ply_Status     = SE_SLEEP;
        return;
    }

    base_idx = (Wave_Phase_Q16 >> SE_PHASE_FRAC_BITS);

    if( base_idx >= totalFrames )
    {
        // Source exhausted -> pass-through
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        Ply_Status     = SE_SLEEP;
        return;
    }

    /*
     * Determine how many output frames can be produced in this call.
     * We stop before the source position reaches totalFrames.
     */
    framesToProcess = 0;
    local_phase_q16 = Wave_Phase_Q16;

    while( framesToProcess < frameSize )
    {
        uint32_t src_idx = (local_phase_q16 >> SE_PHASE_FRAC_BITS);

        if( src_idx >= totalFrames )
        {
            break;
        }

        framesToProcess++;
        local_phase_q16 += Wave_Phase_Step_Q16;
    }

    if( framesToProcess <= 0 )
    {
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        Ply_Status     = SE_SLEEP;
        return;
    }

    srcFramesToRead = local_calc_src_frames_to_read( Wave_Phase_Q16,
                                                     Wave_Phase_Step_Q16,
                                                     totalFrames,
                                                     framesToProcess );

    if( srcFramesToRead > SND_EFFECT_WAV_READ_SAMPLES )
    {
        /*
         * This should not happen for 44.1/48/96 kHz.
         * If an unexpected very low output sample rate is used, process a safe partial frame.
         */
        framesToProcess = frameSize / 2;
        if( framesToProcess <= 0 )
        {
            framesToProcess = 1;
        }

        srcFramesToRead = local_calc_src_frames_to_read( Wave_Phase_Q16,
                                                         Wave_Phase_Step_Q16,
                                                         totalFrames,
                                                         framesToProcess );

        if( srcFramesToRead > SND_EFFECT_WAV_READ_SAMPLES )
        {
            srcFramesToRead = SND_EFFECT_WAV_READ_SAMPLES;
        }
    }


    /*
     * Read/decode source samples through the selected storage backend.
     * +1 sample is included by local_calc_src_frames_to_read() when possible
     * so linear interpolation can read x0 and x1 safely.
     */
    if( !snd_effect_read_tone_samples( Play_Tone_Id,
                                       base_idx,
                                       WavData,
                                       (uint32_t)srcFramesToRead ) )
    {
        local_copy_pass_through( in_buf, out_buf, frameSize, num_proc_ch );
        Wave_Phase_Q16 = 0u;
        Ply_Status     = SE_SLEEP;
        return;
    }


    // Main processing
    local_phase_q16 = Wave_Phase_Q16;

    for (int n = 0; n < framesToProcess; n++)
    {
        uint32_t src_idx_abs;
        uint32_t src_idx_rel;
        uint32_t frac_q16;
        float    s0;
        float    s1;
        float    frac;
        float    notif_norm;
        float    notif;

        src_idx_abs = (local_phase_q16 >> SE_PHASE_FRAC_BITS);
        src_idx_rel = src_idx_abs - base_idx;
        frac_q16    = local_phase_q16 & SE_PHASE_FRAC_MASK;

        if( src_idx_rel >= (uint32_t)srcFramesToRead )
        {
            break;
        }

        s0 = (float)WavData[src_idx_rel];

        if( (src_idx_rel + 1u) < (uint32_t)srcFramesToRead )
        {
            s1 = (float)WavData[src_idx_rel + 1u];
        }
        else
        {
            s1 = s0;
        }

        frac       = (float)frac_q16 * (1.0f / (float)SE_PHASE_ONE_Q16);
        notif_norm = (s0 + ((s1 - s0) * frac)) * (1.0f / 32768.0f);

        // Pre_Gain_WAV is assumed to be precomputed at system init (linear gain)
        notif = notif_norm * Pre_Gain_WAV;

        // Mix into all processing channels
        for (int ch = 0; ch < num_proc_ch; ch++)
        {
            const int idx = (ch * frameSize) + n;
            const float mixed = in_buf[idx] + notif;
            out_buf[idx] = CLAMPF(mixed, -1.0f,  1.0f);
        }

        local_phase_q16 += Wave_Phase_Step_Q16;
    }

    // Pass-through for the tail of the buffer after the source is exhausted
    for (int n = framesToProcess; n < frameSize; n++)
    {
        for (int ch = 0; ch < num_proc_ch; ch++)
        {
            const int idx = (ch * frameSize) + n;
            out_buf[idx] = in_buf[idx];
        }
    }

    // Advance phase and handle end-of-source
    Wave_Phase_Q16 = local_phase_q16;

    if( (Wave_Phase_Q16 >> SE_PHASE_FRAC_BITS) >= totalFrames )
    {
        Ply_Status = SE_SLEEP;
    }
}





//===========================================================
// Local Function
//===========================================================

#if APP_SND_EFFECT_EXTERNAL_SST26
/*
 * snd_effect_flash_probe
 * ----------------------
 * Purpose : Report the external-flash device identity/state at bring-up.
 * Flow    : Show JEDEC ID and the current Status / Configuration registers,
 *           using only the generic SST26 driver accessors.
 * Notes   : Diagnostics for the flash sound-effect feature; the SST26 driver
 *           itself no longer carries a self-test entry point.
 */
static void snd_effect_flash_probe( void )
{
    sst26_read_jedec_id();
    printf(" SR = 0x%02X\n", sst26_read_status());
    printf(" CR = 0x%02X\n", sst26_read_config());
}
#endif //APP_SND_EFFECT_EXTERNAL_SST26


static inline uint32_t local_get_valid_sample_rate( uint32_t sample_rate_Hz )
{
    if( sample_rate_Hz != 0u )
    {
        return sample_rate_Hz;
    }

    /* Fallback only. Normal operation should pass sample_rate_Hz via init. */
    return (uint32_t)SAMPLE_RATE;
}


/*
 * Stored sample rate of one tone.  Falls back to the build-time default if the
 * generated table left it at zero, so a stale table degrades to the previous
 * behaviour instead of dividing by zero.
 */
static inline uint32_t local_tone_source_rate( uint8_t id )
{
    if( id >= SE_TONE_NUM )
    {
        return SND_EFFECT_SOURCE_SAMPLE_RATE_HZ;
    }

    if( Tone_Info[id].rate_Hz == 0u )
    {
        return SND_EFFECT_SOURCE_SAMPLE_RATE_HZ;
    }

    return Tone_Info[id].rate_Hz;
}


static inline uint32_t local_calc_phase_step_q16( uint32_t source_sample_rate_Hz,
                                                  uint32_t output_sample_rate_Hz )
{
    uint64_t step_q16;

    output_sample_rate_Hz = local_get_valid_sample_rate( output_sample_rate_Hz );

    if( source_sample_rate_Hz == 0u )
    {
        source_sample_rate_Hz = SND_EFFECT_SOURCE_SAMPLE_RATE_HZ;
    }

    /*
     * phase_step = round(source_rate / output_rate * 65536)
     */
    step_q16  = ((uint64_t)source_sample_rate_Hz << SE_PHASE_FRAC_BITS);
    step_q16 += ((uint64_t)output_sample_rate_Hz / 2ULL);
    step_q16 /= (uint64_t)output_sample_rate_Hz;

    if( step_q16 == 0ULL )
    {
        step_q16 = 1ULL;
    }

    if( step_q16 > 0xFFFFFFFFULL )
    {
        step_q16 = 0xFFFFFFFFULL;
    }

    return (uint32_t)step_q16;
}


static inline void local_copy_pass_through( const float* in_buf,
                                            float*       out_buf,
                                            int          frameSize,
                                            int          num_proc_ch )
{
    for (int i = 0; i < frameSize * num_proc_ch; i++)
    {
        out_buf[i] = in_buf[i];
    }
}


static inline int local_calc_src_frames_to_read( uint32_t phase_q16,
                                                 uint32_t phase_step_q16,
                                                 uint32_t totalFrames,
                                                 int      out_frames )
{
    uint32_t first_idx;
    uint32_t last_phase_q16;
    uint32_t last_idx;
    uint32_t read_frames;

    if( out_frames <= 0 )
    {
        return 0;
    }

    first_idx = (phase_q16 >> SE_PHASE_FRAC_BITS);

    last_phase_q16 = phase_q16 + ((uint32_t)(out_frames - 1) * phase_step_q16);
    last_idx       = (last_phase_q16 >> SE_PHASE_FRAC_BITS);

    if( first_idx >= totalFrames )
    {
        return 0;
    }

    if( last_idx >= totalFrames )
    {
        last_idx = totalFrames - 1u;
    }

    /*
     * +1 for the inclusive range.
     * +1 extra sample for linear interpolation if the next source sample exists.
     */
    read_frames = (last_idx - first_idx) + 1u;

    if( (last_idx + 1u) < totalFrames )
    {
        read_frames++;
    }

    if( read_frames > 0x7FFFFFFFUL )
    {
        read_frames = 0x7FFFFFFFUL;
    }

    return (int)read_frames;
}


static void snd_effect_init_tone_info( void )
{
    if( Tone_Info_Initialized )
    {
        return;
    }

#if APP_SND_EFFECT_EXTERNAL_SST26
    Tone_Info[ SE_TONE_ON ].pDat          = Button_Tone_i16.Tone_ON;
    Tone_Info[ SE_TONE_ON ].size          = Button_Tone_i16.Tone_ON_size;
    Tone_Info[ SE_TONE_ON ].arraysize     = Button_Tone_i16.Tone_ON_array_s;
    Tone_Info[ SE_TONE_ON ].rate_Hz       = Button_Tone_i16.Tone_ON_rate;
    Tone_Info[ SE_TONE_ON ].flash_addr    = 0x00000000;

    Tone_Info[ SE_TONE_OFF ].pDat         = Button_Tone_i16.Tone_OFF;
    Tone_Info[ SE_TONE_OFF ].size         = Button_Tone_i16.Tone_OFF_size;
    Tone_Info[ SE_TONE_OFF ].arraysize    = Button_Tone_i16.Tone_OFF_array_s;
    Tone_Info[ SE_TONE_OFF ].rate_Hz      = Button_Tone_i16.Tone_OFF_rate;
    Tone_Info[ SE_TONE_OFF ].flash_addr   = Tone_Info[ SE_TONE_ON ].flash_addr + Tone_Info[ SE_TONE_ON ].size;

    Tone_Info[ SE_TONE_NOTIF ].pDat       = Button_Tone_i16.Tone_Notif;
    Tone_Info[ SE_TONE_NOTIF ].size       = Button_Tone_i16.Tone_Notif_size;
    Tone_Info[ SE_TONE_NOTIF ].arraysize  = Button_Tone_i16.Tone_Notif_array_s;
    Tone_Info[ SE_TONE_NOTIF ].rate_Hz    = Button_Tone_i16.Tone_Notif_rate;
    Tone_Info[ SE_TONE_NOTIF ].flash_addr = Tone_Info[ SE_TONE_OFF ].flash_addr + Tone_Info[ SE_TONE_OFF ].size;
#elif APP_SND_EFFECT_INTERNAL_ADPCM
    for( uint8_t id = 0u; id < (uint8_t)SE_TONE_NUM; id++ )
    {
        const snd_effect_adpcm_asset_t* asset = &Snd_Effect_Adpcm_Assets[id];
        Tone_Info[id].pDat       = asset->data;
        Tone_Info[id].size       = asset->encoded_size;
        Tone_Info[id].arraysize  = asset->sample_count;
        Tone_Info[id].rate_Hz    = asset->rate_Hz;
        Tone_Info[id].flash_addr = 0u;
    }
#endif

    Tone_Info_Initialized = true;
}


static uint32_t snd_effect_get_tone_size( uint8_t id )
{
    if( id >= SE_TONE_NUM )
    {
        return 0;
    }

    return Tone_Info[id].arraysize;
}


/*
 * snd_effect_read_tone_samples
 * -----------------------------
 * Purpose : Fetch `sample_count` decoded int16 samples of tone `id`, starting
 *           at `first_sample`, through whichever storage backend is active.
 * Notes   : Tone offset/address layout (SST26) or ADPCM block/decoder state
 *           is owned entirely by this module.
 */
static bool snd_effect_read_tone_samples( uint8_t id,
                                          uint32_t first_sample,
                                          int16_t* buf,
                                          uint32_t sample_count )
{
    if( sample_count == 0u )
    {
        return true;
    }
    if( (buf == NULL) || (id >= SE_TONE_NUM) )
    {
        return false;
    }
    if( (first_sample >= Tone_Info[id].arraysize) ||
        (sample_count > (Tone_Info[id].arraysize - first_sample)) )
    {
        return false;
    }

#if APP_SND_EFFECT_EXTERNAL_SST26
    const uint32_t addr = Tone_Info[id].flash_addr +
                          (first_sample * (uint32_t)sizeof(int16_t));
    sst26_read_fast( addr, (uint8_t*)buf, sample_count * (uint32_t)sizeof(int16_t) );
    return true;
#elif APP_SND_EFFECT_INTERNAL_ADPCM
    uint32_t produced = 0u;

    // Consecutive SRC windows overlap by the interpolation look-ahead sample.
    // Reuse that one decoded sample instead of rewinding the ADPCM stream.
    if( Adpcm_Decoder.valid &&
        (Adpcm_Decoder.tone_id == id) &&
        Adpcm_Decoder.have_last &&
        (Adpcm_Decoder.last_sample_index == first_sample) &&
        (Adpcm_Decoder.next_sample == (first_sample + 1u)) )
    {
        buf[produced++] = Adpcm_Decoder.last_sample;
    }

    if( produced < sample_count )
    {
        const uint32_t wanted = first_sample + produced;
        if( !Adpcm_Decoder.valid ||
            (Adpcm_Decoder.tone_id != id) ||
            (Adpcm_Decoder.next_sample != wanted) )
        {
            if( !snd_effect_adpcm_seek( id, wanted ) )
            {
                return false;
            }
        }

        while( produced < sample_count )
        {
            if( !snd_effect_adpcm_decode_next( &buf[produced] ) )
            {
                return false;
            }
            produced++;
        }
    }
    return true;
#else
    return false;
#endif
}


#if APP_SND_EFFECT_INTERNAL_ADPCM
static void snd_effect_adpcm_reset( void )
{
    Adpcm_Decoder.asset             = NULL;
    Adpcm_Decoder.next_sample       = 0u;
    Adpcm_Decoder.block_start       = 0u;
    Adpcm_Decoder.block_samples     = 0u;
    Adpcm_Decoder.nibble_index      = 0u;
    Adpcm_Decoder.predictor         = 0;
    Adpcm_Decoder.step_index        = 0u;
    Adpcm_Decoder.tone_id           = 0u;
    Adpcm_Decoder.valid             = 0u;
    Adpcm_Decoder.have_last         = 0u;
    Adpcm_Decoder.last_sample_index = 0u;
    Adpcm_Decoder.last_sample       = 0;
}


static bool snd_effect_adpcm_load_block( uint8_t id, uint32_t block_index )
{
    if( id >= SE_TONE_NUM )
    {
        return false;
    }

    const snd_effect_adpcm_asset_t* asset = &Snd_Effect_Adpcm_Assets[id];
    if( (asset->data == NULL) || (block_index >= asset->block_count) )
    {
        return false;
    }

    const uint32_t block_start = block_index * SND_EFFECT_ADPCM_BLOCK_SAMPLES;
    if( block_start >= asset->sample_count )
    {
        return false;
    }

    const uint32_t remaining = asset->sample_count - block_start;
    const uint16_t block_samples = (remaining < SND_EFFECT_ADPCM_BLOCK_SAMPLES)
                                 ? (uint16_t)remaining
                                 : (uint16_t)SND_EFFECT_ADPCM_BLOCK_SAMPLES;
    const uint32_t block_offset = block_index * SND_EFFECT_ADPCM_FULL_BLOCK_BYTES;
    const uint32_t block_bytes  = SND_EFFECT_ADPCM_BLOCK_HEADER_BYTES +
                                  ((uint32_t)block_samples / 2u);
    if( (block_offset > asset->encoded_size) ||
        (block_bytes > (asset->encoded_size - block_offset)) )
    {
        return false;
    }

    const uint8_t* header = asset->data + block_offset;
    const uint16_t raw_predictor = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
    if( (header[2] > 88u) || (header[3] != 0u) )
    {
        return false;
    }

    Adpcm_Decoder.asset         = asset;
    Adpcm_Decoder.next_sample   = block_start;
    Adpcm_Decoder.block_start   = block_start;
    Adpcm_Decoder.block_samples = block_samples;
    Adpcm_Decoder.nibble_index  = 0u;
    Adpcm_Decoder.predictor     = (int16_t)raw_predictor;
    Adpcm_Decoder.step_index    = header[2];
    Adpcm_Decoder.tone_id       = id;
    Adpcm_Decoder.valid         = 1u;
    return true;
}


static bool snd_effect_adpcm_decode_next( int16_t* sample )
{
    if( (sample == NULL) || !Adpcm_Decoder.valid || (Adpcm_Decoder.asset == NULL) )
    {
        return false;
    }
    if( Adpcm_Decoder.next_sample >= Adpcm_Decoder.asset->sample_count )
    {
        return false;
    }

    if( Adpcm_Decoder.next_sample >=
        (Adpcm_Decoder.block_start + (uint32_t)Adpcm_Decoder.block_samples) )
    {
        const uint32_t next_block = Adpcm_Decoder.next_sample /
                                    SND_EFFECT_ADPCM_BLOCK_SAMPLES;
        if( !snd_effect_adpcm_load_block( Adpcm_Decoder.tone_id, next_block ) )
        {
            return false;
        }
    }

    int16_t decoded;
    if( Adpcm_Decoder.next_sample == Adpcm_Decoder.block_start )
    {
        decoded = Adpcm_Decoder.predictor;
    }
    else
    {
        if( Adpcm_Decoder.nibble_index >= (uint16_t)(Adpcm_Decoder.block_samples - 1u) )
        {
            return false;
        }

        const uint32_t block_index  = Adpcm_Decoder.block_start /
                                      SND_EFFECT_ADPCM_BLOCK_SAMPLES;
        const uint32_t block_offset = block_index * SND_EFFECT_ADPCM_FULL_BLOCK_BYTES;
        const uint8_t* payload = Adpcm_Decoder.asset->data + block_offset +
                                 SND_EFFECT_ADPCM_BLOCK_HEADER_BYTES;
        const uint8_t packed = payload[ Adpcm_Decoder.nibble_index >> 1 ];
        const uint8_t code = (Adpcm_Decoder.nibble_index & 1u)
                           ? (uint8_t)((packed >> 4) & 0x0Fu)
                           : (uint8_t)(packed & 0x0Fu);
        const int32_t step = Adpcm_Step_Table[ Adpcm_Decoder.step_index ];
        int32_t delta = step >> 3;
        if( code & 4u ) delta += step;
        if( code & 2u ) delta += step >> 1;
        if( code & 1u ) delta += step >> 2;

        int32_t predictor = Adpcm_Decoder.predictor;
        predictor = (code & 8u) ? (predictor - delta) : (predictor + delta);
        if( predictor > 32767 ) predictor = 32767;
        if( predictor < -32768 ) predictor = -32768;

        int32_t step_index = (int32_t)Adpcm_Decoder.step_index +
                             (int32_t)Adpcm_Index_Table[code & 7u];
        if( step_index > 88 ) step_index = 88;
        if( step_index < 0 )  step_index = 0;

        Adpcm_Decoder.predictor  = (int16_t)predictor;
        Adpcm_Decoder.step_index = (uint8_t)step_index;
        Adpcm_Decoder.nibble_index++;
        decoded = (int16_t)predictor;
    }

    Adpcm_Decoder.last_sample_index = Adpcm_Decoder.next_sample;
    Adpcm_Decoder.last_sample       = decoded;
    Adpcm_Decoder.have_last         = 1u;
    Adpcm_Decoder.next_sample++;
    *sample = decoded;
    return true;
}


static bool snd_effect_adpcm_seek( uint8_t id, uint32_t sample_index )
{
    if( (id >= SE_TONE_NUM) ||
        (sample_index >= Snd_Effect_Adpcm_Assets[id].sample_count) )
    {
        return false;
    }

    Adpcm_Decoder.valid     = 0u;
    Adpcm_Decoder.have_last = 0u;
    if( !snd_effect_adpcm_load_block( id,
                                      sample_index / SND_EFFECT_ADPCM_BLOCK_SAMPLES ) )
    {
        return false;
    }

    while( Adpcm_Decoder.next_sample < sample_index )
    {
        int16_t discarded;
        if( !snd_effect_adpcm_decode_next( &discarded ) )
        {
            return false;
        }
    }
    return true;
}
#endif //APP_SND_EFFECT_INTERNAL_ADPCM

#if APP_SND_EFFECT_EXTERNAL_SST26
static bool snd_effect_verify_tone_data( uint8_t id )
{
    printf(" snd_effect_verify_tone_data:\n");

    if( id >= SE_TONE_NUM )
    {
        return false;
    }

    // check current external Flash contents
    if( sst26_verify(Tone_Info[id].flash_addr, (const uint8_t*)Tone_Info[id].pDat, Tone_Info[id].size) )
    {
        printf(" Ext Flash Verify OK. id=%d addr=%ld size=%ld(Byte)\n",
                                                                        id,
                                                                        Tone_Info[id].flash_addr,
                                                                        Tone_Info[id].size);
        return true;
    }
    else
    {
        printf(" Ext Flash Verify NOT OK!! id=%d addr=%ld size=%ld(Byte)\n",
                                                                        id,
                                                                        Tone_Info[id].flash_addr,
                                                                        Tone_Info[id].size);
        return false;
    }
}
#endif //APP_SND_EFFECT_EXTERNAL_SST26

#endif //defined(ENA_SND_EFFECT_PLAY)
