#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include "classic_audio_path.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_audio_path.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stddef.h>

#include "apps/shared/float_conversion.h"
#include "apps/shared/LED_level_meter.h"
#include "gain_ctrl.h"
#include "tone_ctrl.h"
#include "deesser.h"
#include "widen_ctrl.h"
#include "bass_enhancer.h"
#include "anc_monitor.h"
#include "ch_expand_2to4.h"
#include "biquad_cascade_4ch.h"
#include "fir_filter.h"
#include "audio_sample_delay.h"
#include "fx_domain_48k.h"
#include "flip4_keepalive.h"
#include "snd_effect_play.h"
#include "classic_audio_pwm.h"

#define CLASSIC_AUDIO_PATH_REGULAR_VOL    (0xFFu)

static float f_a_data[STAGE_1_PROC_CH][APP_BLOCK_FRAMES];
#if ENA_DRC_DF2T_CASCADE
static float f_b_data[STAGE_2_PROC_CH][APP_BLOCK_FRAMES];
#endif

void classic_audio_path_prepare( uint32_t sample_rate_hz )
{
    app_gain_init( sample_rate_hz );
    app_gain_set( CLASSIC_AUDIO_PATH_REGULAR_VOL );
    app_tone_init( sample_rate_hz );

    app_fx_domain_48k_init( sample_rate_hz );

#if defined(ENA_DEESSER)
    app_deesser_init();
#endif
#if defined(ENA_WIDEN_CTRL)
    app_widen_init( sample_rate_hz );
#endif
#if defined(ENA_BASS_ENHANCER)
    app_bassenh_init( sample_rate_hz );
#endif
#if defined(ENA_FLIP4_KEEPALIVE)
    flip4_keepalive_init( sample_rate_hz );
#endif
#if defined(ENA_ANC_MONITOR)
    app_ancmon_init();
#endif
#if ENA_DRC_DF2T_CASCADE
    app_ch_expand_2to4_init();
#endif
#if defined(ENA_BIQUAD_IIR_CASCADE)
    app_biquad_cascade_4ch_init();
#endif
#if defined(ENA_FIR_FILTER)
    app_fir_filter_init();
#endif
#if defined(ENA_SAMPLE_DELAY)
    app_audio_sample_delay_init( sample_rate_hz );
#endif
#if defined(ENA_SND_EFFECT_PLAY)
    // The source assets stay at their own stored rate; retune only the
    // playback Q16 step when codec-A changes rate during a mute-bounded restart.
    snd_effect_set_sample_rate( sample_rate_hz );
#endif
}

static inline void copy_to_codec( const int32_t* src_ptr,
                                  uint8_t        src_ch,
                                  int32_t*       dest_ptr,
                                  uint8_t        offset )
{
    if( dest_ptr == NULL )
    {
        return;
    }

    const int32_t* src_offs = src_ptr + offset;
    int32_t*       dest     = dest_ptr;
    uint16_t       smpl     = APP_BLOCK_FRAMES;

    while( smpl-- > 0 )
    {
        dest[0] = src_offs[0];
        dest[1] = src_offs[1];
        dest     += APP_SLOTS_PER_FS;
        src_offs += src_ch;
    }
}

#if ENA_DRC_DF2T_CASCADE
static void process_drc( const int32_t*    src_ptr,
                         int32_t*          dest_ptr_a,
                         int32_t*          dest_ptr_b,
                         float* __restrict float_in,
                         float* __restrict float_out )
{
    convert_codec_int_to_float(
        src_ptr, APP_SLOTS_PER_FS, float_in, STAGE_1_PROC_CH, APP_BLOCK_FRAMES );

    app_ch_expand_2to4_process( float_in, float_out );

#if defined(ENA_BIQUAD_IIR_CASCADE)
    app_biquad_cascade_4ch_process( float_out, float_out );
#endif
#if defined(ENA_FIR_FILTER)
    app_fir_filter_process( float_out, float_out );
#endif
#if defined(ENA_SAMPLE_DELAY)
    app_audio_sample_delay_process( float_out );
#endif

    level_meter_process( float_out );

#if defined(ENA_FLIP4_KEEPALIVE)
    flip4_keepalive_process( float_out, float_out, STAGE_2_PROC_CH, APP_BLOCK_FRAMES );
#endif

#define CLASSIC_AUDIO_PATH_TMP_TX_CH  (4)
    static int32_t tmp_tx[CLASSIC_AUDIO_PATH_TMP_TX_CH * APP_BLOCK_FRAMES];
    convert_codec_float_to_int(
        float_out, STAGE_2_PROC_CH, tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, APP_BLOCK_FRAMES );

    copy_to_codec( tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_a, 0 );
    copy_to_codec( tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_b, 2 );
}
#else
static void process_regular( const int32_t* src_ptr,
                             int32_t*       dest_ptr_a,
                             int32_t*       dest_ptr_b,
                             float*         in,
                             float*         out )
{
    convert_codec_int_to_float(
        src_ptr, APP_SLOTS_PER_FS, in, STAGE_1_PROC_CH, APP_BLOCK_FRAMES );

    app_tone_process_tre( in, in );
    app_tone_process_bas( in, in );

#if defined(ENA_WIDEN_CTRL)
    app_widen_process( in, in );
#endif
#if defined(ENA_BASS_ENHANCER)
    app_bassenh_process( in, in );
#endif

    app_fx_domain_48k_process( in, in );
    app_gain_process( in, out );

#if defined(ENA_SND_EFFECT_PLAY)
    wav_to_tdm_float_process( out, out, STAGE_1_PROC_CH );
#endif

    level_meter_process( out );

#if defined(ENA_FLIP4_KEEPALIVE)
    flip4_keepalive_process( out, out, STAGE_1_PROC_CH, APP_BLOCK_FRAMES );
#endif

#define CLASSIC_AUDIO_PATH_TMP_TX_CH  (4)
    static int32_t tmp_tx[CLASSIC_AUDIO_PATH_TMP_TX_CH * APP_BLOCK_FRAMES];
    convert_codec_float_to_int(
        out, STAGE_1_PROC_CH, tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, APP_BLOCK_FRAMES );

    copy_to_codec( tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_a, 0 );
    copy_to_codec( tmp_tx, CLASSIC_AUDIO_PATH_TMP_TX_CH, dest_ptr_b, 0 );
}
#endif

static void process_optional_pwm_audio_output( void )
{
#if APP_USE_PWM_AUDIO
#if ENA_DRC_DF2T_CASCADE
    /* The DRC path does not drive the Classic PWM DAC. */
#else
    classic_audio_pwm_process_left_primary( &f_a_data[0][0], STAGE_1_PROC_CH );
    classic_audio_pwm_process_right_primary( &f_a_data[0][0], STAGE_1_PROC_CH );
    classic_audio_pwm_process_left_secondary( &f_a_data[0][0], STAGE_1_PROC_CH );
    classic_audio_pwm_process_right_secondary( &f_a_data[0][0], STAGE_1_PROC_CH );
#endif
#endif
}

void classic_audio_path_process( const int32_t* src_ptr,
                                 int32_t*       dest_ptr_a,
                                 int32_t*       dest_ptr_b )
{
#if defined(ENA_APP_RAW_BYPASS)
    copy_to_codec( src_ptr, APP_SLOTS_PER_FS, dest_ptr_a, 0 );
    copy_to_codec( src_ptr, APP_SLOTS_PER_FS, dest_ptr_b, 0 );
    return;
#endif

#if ENA_DRC_DF2T_CASCADE
    process_drc( src_ptr, dest_ptr_a, dest_ptr_b, &f_a_data[0][0], &f_b_data[0][0] );
#else
    process_regular( src_ptr, dest_ptr_a, dest_ptr_b, &f_a_data[0][0], &f_a_data[0][0] );
#endif

    process_optional_pwm_audio_output();
}

void classic_audio_path_reset( void )
{
    memset( f_a_data, 0, sizeof(f_a_data) );
#if ENA_DRC_DF2T_CASCADE
    memset( f_b_data, 0, sizeof(f_b_data) );
#endif
}
