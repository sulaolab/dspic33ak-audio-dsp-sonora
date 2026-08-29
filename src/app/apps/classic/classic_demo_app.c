#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_demo_app.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include "apps/sonora_app.h"
#include "audio_transport.h"
#include "apps/audio_application_telemetry.h"
#include "audio_transport_client.h"
#include "audio_transport_sai_drytest.h"
#include "classic_audio_path.h"
#include "apps/shared/float_conversion.h"
#include "apps/shared/LED_level_meter.h"
#include "biquad_cascade_4ch.h"
#include "fir_filter.h"
#include "nora_high_res_timer.h"
#include "classic_audio_pwm.h"
#include "classic_controls.h"
#include "timer_app.h"
#include "board/devices/pot_drv.h"
#include "app_utils.h"   // biquad_t / biquad_mono_t (needed by engine_synth.h / bass_enhancer.h)
#include "engine_synth.h"
#include "avas_synth_type_ty.h"    // app_avas_type_ty_is_active    (POT ownership vs the engine synth)
#include "avas_synth_type_lb.h"   // app_avas_type_lb_is_active (same)
#include "bass_enhancer.h"
#include "anc_monitor.h"
#include "board/devices/SST26_drv.h"   // external serial flash (sound-effect storage)
#include "snd_effect_play.h"           // flash-backed button sound effects (Classic-only)

// Bass-enhancer monitor ("Lv=..." debug line) visibility. This is a Classic-app concern and its
// only consumer is below, so the derivation lives here rather than in the shared config header.
// In the Classic profile the former ASRC-only suppression terms (APP_ASRC_MEAS / APP_B_INDEP_DOMAIN
// / the ASRC load-test route) are all structurally 0, so showing the monitor reduces to "is the
// bass-enhancer feature compiled in": AK512 regular / 96K define it (=> 1); AK128 and DRC do not
// (=> 0).
#if defined(ENA_BASS_ENHANCER)
#define APP_SHOW_BASSH_MONITOR (1)
#else
#define APP_SHOW_BASSH_MONITOR (0)
#endif

static void classic_transport_process( const int32_t* src,
                                       int32_t*       dst_a,
                                       int32_t*       dst_b,
                                       void*          user )
{
    (void)user;
    classic_audio_path_process( src, dst_a, dst_b );
}

static void classic_transport_prepare( uint32_t sample_rate_hz, void* user )
{
    (void)user;
    /* Safety net only: the gains are built in sonora_app_prepare at boot and the call
     * is idempotent, so this cannot recompute or reprint them on a rate change. It stays
     * so that a build that forgets the boot call still gets non-zero gains before
     * level_meter_init reads Pre_Gain_CODEC below. */
    audio_gains_init();
    level_meter_init( sample_rate_hz,
                      LEVEL_METER_DEFAULT_UPDATE_MS,
                      LEVEL_METER_DEFAULT_ATTACK_MS,
                      LEVEL_METER_DEFAULT_RELEASE_MS );
    classic_audio_path_prepare( sample_rate_hz );
}

static void classic_transport_reset( void* user )
{
    (void)user;
    classic_audio_path_reset();
}

static const audio_transport_client_t s_classic_transport_client =
{
    .capabilities = 0u,
    .co_clock_process = classic_transport_process,
    .leg_a_process = NULL,
    .leg_b_process = NULL,
    .prepare = classic_transport_prepare,
    .reset_stream_state = classic_transport_reset,
    .clock_progress = NULL,
    .user = NULL,
};

/*
 * The classic (co-clocked) profile has no rate measurement: both codecs run off one clock the MCU
 * itself derives, and there is no CCP capture pair. 0 means "omit the field" -- see the contract
 * in apps/audio_application_telemetry.h.
 */
uint32_t audio_application_leg_measured_fs_hz( uint8_t leg )
{
    (void)leg;
    return 0u;
}

void audio_application_telemetry_print(
    const audio_transport_snapshot_t* transport,
    uint32_t                          now_ms,
    uint32_t                          recovery_count )
{
    (void)transport;
    (void)now_ms;
    (void)recovery_count;

#if ENA_DRC_DF2T_CASCADE
#if defined(ENA_BIQUAD_IIR_CASCADE) || defined(ENA_FIR_FILTER)
    printf("DSP [%dch]", STAGE_2_PROC_CH);
#endif
#if defined(ENA_BIQUAD_IIR_CASCADE)
    {
        extern uint32_t dt;   // biquad cascade process time (audio/biquad_cascade_4ch.c)
        const uint32_t iir_us10 = nora_high_res_timer_count_to_us_x10( dt );
        printf("[stage=%d total=%d]", BIQUAD_CASCADE_4CH_NUM_STAGE,
               BIQUAD_CASCADE_4CH_NUM_STAGE * STAGE_2_PROC_CH);
#if defined(USE_CMSIS_IIR_DSP_PROCESS)
        printf("CMSIS-IIR:%lu.%luus", (unsigned long)(iir_us10 / 10u),
               (unsigned long)(iir_us10 % 10u));
#else
        printf("C-IIR:%lu.%luus", (unsigned long)(iir_us10 / 10u),
               (unsigned long)(iir_us10 % 10u));
#endif
    }
#endif //ENA_BIQUAD_IIR_CASCADE
#if defined(ENA_FIR_FILTER)
    {
        const uint32_t fir_us10 =
            nora_high_res_timer_count_to_us_x10( g_fir_filter_process_dt );
        printf("[tap=%u]", (unsigned)g_fir_filter_process_num_taps);
#if defined(USE_CMSIS_FIR_DSP_PROCESS)
        printf("CMSIS-FIR:%lu.%luus", (unsigned long)(fir_us10 / 10u),
               (unsigned long)(fir_us10 % 10u));
#else
        printf("C-FIR:%lu.%luus", (unsigned long)(fir_us10 / 10u),
               (unsigned long)(fir_us10 % 10u));
#endif
    }
#endif //ENA_FIR_FILTER
#if defined(ENA_BIQUAD_IIR_CASCADE) || defined(ENA_FIR_FILTER)
    printf("\n");
#endif
#endif //ENA_DRC_DF2T_CASCADE
}

const char* sonora_app_name( void )
{
    return "Classic Audio Demo App";
}

void sonora_app_print_banner( void )
{
    printf(" Mode: "
#if defined(ENA_USB_AUDIO_IN)
           "USB-AUDIO-IN (Pico2 bridge -> codecs, I2S)"
#elif defined(ENA_96K_RATE)
           "96K (co-clocked dual-codec split, non-USB)"
#elif ENA_DRC_DF2T_CASCADE
           "DRC-DEMO (co-clocked dual codec; DF2T biquad cascade)"
#elif APP_USE_SPI_TDM_CLK_MASTER
           "DEMO2 (co-clocked dual codec; dsPIC drives BCLK/FS)"
#else
           "DEMO1 (co-clocked dual codec; WM8904-A drives BCLK/FS, B slave)"
#endif
           "\n");
}

void sonora_app_prepare( void )
{
    /* Rate-independent gain table: built once here, before audio_transport_start_route
     * calls the fs-parameterized prepare hook (main.c calls this before start_audio). */
    audio_gains_init();
    audio_gains_print();

#if defined(ENA_SAI_WRAPPER_DRYTEST)
    audio_transport_sai_drytest_run();
#endif

#if defined(ENA_SND_EFFECT_PLAY)
    // AK512 uses the SST26 external flash solely to store and play the button
    // sound effects; bring the flash SPI up (formerly inlined in main()) before
    // snd_effect_int() provisions + verifies the tone data (its own bring-up
    // probe -- JEDEC/SR/CR -- runs from inside snd_effect_int() too).
    // AK128 has no independent SST26 bus (shared with the audio TDM pins) and
    // decodes immutable IMA-ADPCM assets from internal Program Flash instead,
    // so there is no flash device to bring up.
  #if APP_SND_EFFECT_EXTERNAL_SST26
    sst26_init();
  #endif
    snd_effect_int( SAMPLE_RATE );
#endif
}

void sonora_app_start_audio( void )
{
    const audio_transport_client_bind_result_t bind_result =
        audio_transport_client_bind( &s_classic_transport_client );
    if( bind_result != AUDIO_TRANSPORT_CLIENT_BIND_OK )
    {
        printf(" Classic transport client bind failed: %s\n",
               audio_transport_client_bind_result_name( bind_result ) );
        return;
    }

#if defined(ENA_SAI_WRAPPER_LIVE)
    audio_transport_cmsis_sai_start();
#else
    audio_transport_hal_start();
#endif

#if defined(ENA_BIQUAD_IIR_CASCADE)
    app_biquad_cascade_4ch_request_normal();
#endif
}

void sonora_app_start_aux_output( void )
{
    classic_audio_pwm_init();
}

bool sonora_app_manage_audio( void )
{
#if defined(ENA_SAI_WRAPPER_LIVE)
    return audio_transport_cmsis_sai_manage();
#else
    return audio_transport_hal_manage();
#endif
}

void sonora_app_process_controls( void )
{
    classic_controls_process();   /* button + touch dispatch */
}

bool sonora_app_service( void )
{
    return false;
}

/* POT reading above which the engine-synth monitor treats the pot as active.
 * (Distinct from main.c's LED_POT_OFF_VAL RGB-off threshold; same magnitude.) */
#define ENG_SYNTH_POT_ACTIVE_VAL   (50)

void sonora_app_debug_print( void )
{
    /* Classic-owned periodic monitors (moved from main.c dbg_print). The
     * 200/400 ms cadences are preserved; these run ahead of the shared transport
     * telemetry line to keep the original print order. */
    static uint32_t last_prt_2 = UINT32_MAX;   /* 200 ms */
    static uint32_t last_prt_3 = UINT32_MAX;   /* 400 ms */
    uint32_t        cur        = GetTicks();

    /* "*tq" is the parent switch for EVERY periodic monitor, not only the transport's own line.
     * These monitors used to have no runtime gate at all, so "*tq0000" left the Lv= line printing
     * every 400 ms and the console never actually went quiet.
     *
     * That mattered beyond the annoyance: *feaa55 / *fu5A disable telemetry and then drain the UART so
     * the update status is the last thing on the wire, and an ungated monitor interleaves with it --
     * and with the XMODEM protocol during a transfer.
     *
     * NOT a blanket early return. The 400 ms block below also drives app_engine_synth_enable() from
     * the pot, which is audio CONTROL, not output -- silencing the console must not disable a
     * feature. So the gate is applied to each printf, and the control path runs either way. */
    const bool dbg_on = audio_transport_dbg_enabled();

    if( (uint32_t)(cur - last_prt_2) >= 200 )
    {
#if defined(ENA_ANC_TEST)
        if( dbg_on ) { app_ancmon_dbg_prt(); }
#endif
        last_prt_2 = cur;
    }

    if( (uint32_t)(cur - last_prt_3) >= 400 )
    {
        /* The PRINTED monitors in this block run every other tick -- 800 ms, half the rate
         * they used to -- because the "rpm= | Lv=..." readout is followed by eye while
         * listening and at 2.5 Hz it scrolls faster than it can be read.
         *
         * The skip is one flag covering the whole line on purpose: " rpm=%4d | " is printed
         * WITHOUT a newline as a prefix to the Lv= line, so gating the two independently
         * would leave a dangling prefix on every skipped tick. Everything else in this block
         * -- the pot read and app_engine_synth_enable() -- is audio CONTROL and still runs at
         * 400 ms, unchanged. */
        static bool mon_tick = false;
        mon_tick = !mon_tick;
        const bool mon_on = dbg_on && mon_tick;
        (void)mon_on;   /* both users below are behind compile-time gates */

#if defined(ENA_ENGINE_SYNTH)
        static uint8_t cnt_adc_up = 0;

        uint16_t adc = POT_Read();   // 0x0000..0x0FFF

        /* While either AVAS engine is sounding, the POT is ITS pitch knob, not the
         * engine throttle. Without this the two fight over one control and AVAS
         * always loses: the engine synth takes priority in fx_domain_48k (they
         * are exclusive because their loads would add up), so any knob movement
         * past ENG_SYNTH_POT_ACTIVE_VAL would silence AVAS and start the engine
         * sound instead -- and every usable pitch position is above that
         * threshold.
         *
         * Gated on is_active() rather than the UI on-flag so the release fade is
         * covered too: handing the POT back mid-fade would cut the tail off.
         * Nothing is lost by refusing here -- the engine could not be heard
         * during AVAS anyway. See docs_public/avas_pitch_pot_design.md. */
#if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
        if( 0
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
            || app_avas_type_ty_is_active()
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
            || app_avas_type_lb_is_active()
#endif
          )
        {
            cnt_adc_up = 0;
            app_engine_synth_enable(false);
        }
        else
#endif //defined(ENA_AVAS_TYPE_TY_SYNTH)
        if( adc > ENG_SYNTH_POT_ACTIVE_VAL )
        {
            float    adc_f = (float)adc * ENG_SYNTH_POT_SCALE_FACTOR;
            uint16_t rpm   = (uint16_t)adc_f;

            if( cnt_adc_up < 255 ) cnt_adc_up++;

            if( cnt_adc_up > 3 )
            {
                app_engine_synth_enable(true);       /* control: runs even when output is off */
                if( mon_on ) { printf(" rpm=%4d | ", rpm); }
            }
        }
        else
        {
            cnt_adc_up = 0;
            app_engine_synth_enable(false);
        }
#endif //defined(ENA_ENGINE_SYNTH)

#if APP_SHOW_BASSH_MONITOR
        // Bass-enhancer monitor (Lv= line): shown whenever the feature is compiled in AND its
        // processing is actually in the active audio path -- see APP_SHOW_BASSH_MONITOR. Kept
        // showing even while the enhancer is runtime-disabled (no runtime is_enabled() gate);
        // only the ASRC load-test route (which bypasses the A-DSP) hides it (stale zeros).
        // Runtime gate: "*tq0000" silences this too (see the parent-switch note above);
        // mon_on adds the every-other-tick skip, so this line now appears every 800 ms.
        // Also silenced by a latched codec-B-missing stop (see audio_transport.h) -- this
        // line's own "everything looks fine" chatter is exactly what buries that one-time
        // guidance message.
        if( mon_on && !audio_transport_codec_b_missing_stop_active() ) { app_bassenh_dbg_prt(); }
#endif //APP_SHOW_BASSH_MONITOR

        last_prt_3 = cur;
    }

    audio_transport_dbg_print();
}
