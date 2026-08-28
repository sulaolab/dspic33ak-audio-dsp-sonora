
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
#include "board/devices/SST26_drv.h"
#include "apps/shared/float_conversion.h"


#include "snd_effect_play.h"



#if !SONORA_APP_IS_CLASSIC
#  error "snd_effect_play.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#if defined(ENA_SND_EFFECT_PLAY)
//===========================================================
// Definition
//===========================================================

#define CLAMPF(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/*
 * Embedded sound-effect source data is generated as mono int16 at a per-tone
 * sample rate (Button_Tone_i16.Tone_*_rate; 16 kHz for the button clicks,
 * 32 kHz for the notification, because the tones are narrow-band and storing
 * them all at 48 kHz wasted program flash).
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
    const int16_t* pDat;
    uint32_t       size;       // byte size
    uint32_t       arraysize;  // number of int16 samples
    uint32_t       flash_addr; // byte address in external flash
    uint32_t       rate_Hz;    // stored sample rate of this tone

} ST_SE_TONE_INFO;




//===========================================================
// Function Prototype
//===========================================================

static void     snd_effect_flash_probe( void );
static void     snd_effect_init_tone_info( void );
static uint32_t snd_effect_get_tone_size( uint8_t id );
static void     snd_effect_read_tone_dat(uint8_t id, uint32_t addr, uint8_t *buf, size_t len);
static bool     snd_effect_verify_tone_data( uint8_t id );

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

#include "tone_data_int16.h"

static ENUM_SE_PLAY    Ply_Status   = SE_SLEEP;
static uint8_t         Req_Tone_Id  = SE_TONE_ON;
static uint8_t         Play_Tone_Id = SE_TONE_ON;
static int16_t         WavData[ SND_EFFECT_WAV_READ_SAMPLES ];

static ST_SE_TONE_INFO Tone_Info[ SE_TONE_NUM ];
static bool            Tone_Info_Initialized = false;

static uint32_t        Snd_Effect_Sample_Rate_Hz = (uint32_t)SAMPLE_RATE;
static uint32_t        Wave_Phase_Q16            = 0u;
static uint32_t        Wave_Phase_Step_Q16       = SE_PHASE_ONE_Q16;





//===========================================================
// Global Function
//===========================================================

void snd_effect_int( uint32_t sample_rate_Hz )
{
    // Bring-up probe: confirm the external flash device answers before we rely
    // on it to store the tone data.  This absorbs the former driver-side
    // "SST26_test" self-test so the SST26 driver stays a pure device driver and
    // the flash sound-effect feature owns its own bring-up diagnostics.
    snd_effect_flash_probe();

    Snd_Effect_Sample_Rate_Hz = local_get_valid_sample_rate( sample_rate_Hz );
    Wave_Phase_Q16            = 0u;

    snd_effect_init_tone_info();

    // The step is per tone; this is only the value used until the first play.
    Wave_Phase_Step_Q16       = local_calc_phase_step_q16( local_tone_source_rate( Req_Tone_Id ),
                                                           Snd_Effect_Sample_Rate_Hz );

    snd_effect_prepare_flash_data();
}


bool snd_effect_verify( void )
{
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
 * Mix a mono notification WAV (int16 samples in external flash) into channel-major float I/O buffers.
 * - I/O are normalized floats in [-1.0, +1.0].
 * - Embedded WAV source is 48 kHz mono int16.
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


#if APP_TARGET == APP_TARGET_AK512
#if defined(ENA_SND_EFFECT_PLAY)
    /*
     * Read source samples from external flash.
     * +1 sample is included by local_calc_src_frames_to_read() when possible
     * so linear interpolation can read x0 and x1 safely.
     */
    snd_effect_read_tone_dat( Play_Tone_Id,
                              base_idx * (uint32_t)sizeof(int16_t),
                              (uint8_t*)WavData,
                              (uint32_t)srcFramesToRead * (uint32_t)sizeof(int16_t));
#endif //defined(ENA_SND_EFFECT_PLAY)
#endif //APP_TARGET == APP_TARGET_AK512


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
 * snd_effect_read_tone_dat
 * ---------------
 * Purpose : Read one tone from external flash.
 * Notes   : Tone offset/address layout is owned by snd_effect_play.
 */
static void snd_effect_read_tone_dat(uint8_t id, uint32_t addr, uint8_t *buf, size_t len)
{
    if (!len)
    {
        return;
    }

    if( id >= SE_TONE_NUM )
    {
        return;
    }

    // adjust the offset address
    addr += Tone_Info[id].flash_addr;

    sst26_read_fast( addr, buf, len );
}


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

#endif //defined(ENA_SND_EFFECT_PLAY)
