#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_controls.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "timer_app.h"                 // GetTicks()
#include "board/devices/button_led.h"  // BUTTON_GetEvent/LED_On/LED_Off
#include "board/devices/pot_drv.h"     // POT_Read (AVAS pitch trim knob)
#include "board/devices/button_led.h" // TOUCH_GetEvent / TOUCH_IsPressed

#include "app_utils.h"   // biquad_t / biquad_mono_t (needed by tone_ctrl.h / bass_enhancer.h)
#include "gain_ctrl.h"
#include "tone_ctrl.h"
#include "widen_ctrl.h"
#include "bass_enhancer.h"
#include "snd_effect_play.h"
#include "avas_synth.h"
#include "avas_synth_type_ty.h"
#include "avas_synth_type_lb.h"
#include "clickclack_synth.h"
#include "kinkon_synth.h"
#include "pinger_synth.h"

#include "classic_controls.h"

//===========================================================
// classic_controls.c
//
// Classic application: button/touch dispatch and the UsrOperate_* actions they (and the
// app_debug.c raw hotkey handler) invoke. Moved out of main.c per Phase 5.
//===========================================================

#define APP_MUTE_RAMP_MS          (200)

// Classic Audio Demo control state used by the operations below.
extern audiogain_t My_Gain;

extern tone_t  My_ToneTre;
extern tone_t  My_ToneMid;
extern tone_t  My_ToneBas;

#if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
static void   local_avas_pot_release( void );   /* defined next to the POT sampler below */
static void   local_avas_pot_process( void );
#endif

static void   local_button_handler( void );
static void   local_touch_handler( void );
static void   local_touch_led_indicator( BUTTON_EVENT_t ev, uint8_t id );

#if ENA_DRC_DF2T_CASCADE
static void   UsrOperate_temp_test_1( void );
static void   UsrOperate_temp_test_2( void );
static void   UsrOperate_temp_test_3( void );
#endif //ENA_DRC_DF2T_CASCADE


void classic_controls_process( void )
{
    local_button_handler();
    local_touch_handler();
#if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
    local_avas_pot_process();   /* AVAS pitch knob; self-rate-limited to 100 ms */
#endif
}


void UsrOperate_mute( void )
{
    static uint8_t mute_enabled = 0;

    mute_enabled ^= 1;
    if( mute_enabled )
    {
        printf(" MUTE: start mute @%ld\n", GetTicks());
        app_mute_set( true, APP_MUTE_RAMP_MS );   // mute
    }
    else
    {
        printf(" MUTE: start unmute @%ld\n", GetTicks());
        app_mute_set( false, APP_MUTE_RAMP_MS );  // unmute
    }

    audiogain_t* pgain = &My_Gain;

    printf(" cur_gain=%.3f tar_gain=%.3f ramp_ms=%.1f ramp_samples=%lu status=%d\n",
           pgain->prevGain,
           pgain->targetGain,
           pgain->DBG_ramp_ms,
           (unsigned long)pgain->rampTotalSamples,
           pgain->status
          );
}


void UsrOperate_treble( void )
{
    static uint8_t state = 0;
           float   gain  = 0.0f;

    switch( state )
    {
    case 0:
        snd_effect_play_se(SE_TONE_ON);
        gain   = -12.0f;
        state++;
        break;
    case 1:
        snd_effect_play_se(SE_TONE_ON);
        gain   = 12.0f;
        state++;
        break;
    default:
        snd_effect_play_se(SE_TONE_OFF);
        gain   = 0.0f;
        state = 0;
        break;
    }

    app_tone_set_coeffs_tre( gain );

    tone_t* ptone = &My_ToneTre;
    printf(" TREBLE: treb %.2f(Hz) %2.2f(dB) @%ld\n", ptone->DBG_tar_Hz, ptone->DBG_gain_dB, GetTicks());
}


void UsrOperate_bass( void )
{
    static uint8_t state = 0;
           float   gain  = 0.0f;


    switch( state )
    {
    case 0:
        snd_effect_play_se(SE_TONE_ON);
        gain = -24.0f;
        state++;
        break;
    case 1:
        snd_effect_play_se(SE_TONE_ON);
        gain = 20.0f;
        state++;
        break;
    default:
        snd_effect_play_se(SE_TONE_OFF);
        gain  = 0.0f;
        state = 0;
        break;
    }

    app_tone_set_coeffs_bas( gain );

    tone_t* ptone = &My_ToneBas;
    printf(" BASS btn: bass %.2f(Hz) %2.2f(dB) @%ld\n", ptone->DBG_tar_Hz, ptone->DBG_gain_dB, GetTicks());
}




void UsrOperate_clickclack( void )
{
#if defined(ENA_CLICK_CLACK)
    static uint8_t enabled = 0;

    printf(" Click-Clack: ");
//    snd_effect_play_se(SE_TONE_ON);

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        app_clickclack_set_enable(true);
//        app_pinger_set_enable(true);
    }
    else
    {
        printf("disable.\n");
        app_clickclack_set_enable(false);
//        app_pinger_set_enable(false);
    }
#endif //defined(ENA_CLICK_CLACK)
}

void UsrOperate_clickclack_toggle( void )
{
#if defined(ENA_CLICK_CLACK)
    static bool failure_mode = false;

    printf(" Click-Clack: ");

    failure_mode ^= 1;
    if( failure_mode )
    {
        printf(" Bulb failure simulation 200ms\n");
        app_clickclack_set_period_ms(200.0f);
    }
    else
    {
        printf(" Normal simulation 400ms\n");
        app_clickclack_set_period_ms(400.0f);
    }
#endif //defined(ENA_CLICK_CLACK)
}

void UsrOperate_kinkon( void )
{
#if defined(ENA_KINKON)
    static uint8_t enabled = 0;

    printf(" Kin-Kon: ");

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        app_kinkon_set_enable(true);
    }
    else
    {
        printf("disable.\n");
//        app_kinkon_set_enable(false);
        app_kinkon_request_stop();
    }
#endif //defined(ENA_KINKON)
}

void UsrOperate_pinger( void )
{
#if defined(ENA_PINGER_SOUND)
    static uint8_t enabled = 0;

    printf(" Pinger Synth: ");
//    snd_effect_play_se(SE_TONE_ON);

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        app_pinger_set_enable(true);
    }
    else
    {
        printf("disable.\n");
        app_pinger_set_enable(false);
    }
#endif //defined(ENA_PINGER_SOUND)
}


/* The two AVAS sources are STRICTLY exclusive at run time: TYPE_TY alone costs
 * 45.8 % of the block window on hardware, so letting both render would risk the
 * budget.  Enabling one while the other is still sounding -- release fade
 * included -- is therefore refused outright, with the reason printed.  Turning
 * one off is always accepted; there is nothing to protect.
 *
 * The on/off state is a per-engine latch rather than being derived from
 * app_avas_*_is_active(): re-enabling during the release fade is a supported
 * path (set_enable(true) deliberately skips the phase reset there to avoid a
 * click), and deriving the latch from the gate would break it.  The latches are
 * file-static because the Mute-button route below has to know what is playing. */
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
static uint8_t s_avas_type_ty_on = 0;
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
static uint8_t s_avas_type_lb_on = 0;
#endif

void UsrOperate_avas_synth( void )   /* TYPE_TY */
{
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
    if( !s_avas_type_ty_on )
    {
        #if defined(ENA_AVAS_TYPE_LB_SYNTH)
        if( app_avas_type_lb_is_active() )
        {
            printf(" AVAS(TYPE_TY): rejected -- Type_LB is still sounding (wait for it to fade).\n");
            return;
        }
        #endif
        s_avas_type_ty_on = 1;
        printf(" AVAS Synth(TYPE_TY): enable.\n");
        app_avas_type_ty_set_enable(true);

        /* Start at the reference pitch even if the knob is parked at full
         * travel: the POT has to be moved before it may write.  Without this the
         * stop-time reset to 0 cent would be undone within 100 ms of every
         * start, which is exactly what the reset exists to prevent. */
        local_avas_pot_release();
    }
    else
    {
        /* Read the trim BEFORE the stop: app_avas_type_ty_set_enable(false) resets it
         * to 0 cent, so afterwards there is nothing left to report. */
        const float trim_was = app_avas_type_ty_get_pitch_cent();

        s_avas_type_ty_on = 0;
        printf(" AVAS Synth(TYPE_TY): disable.\n");
        app_avas_type_ty_set_enable(false);
        local_avas_pot_release();

        /* Only worth a line when there was something to reset.  The reset itself
         * is deferred until the release fade has finished, so the tail keeps the
         * pitch it was sounding at -- what changes now is what the NEXT start
         * will use. */
        if( (trim_was > 0.05f) || (trim_was < -0.05f) )
        {
            printf(" AVAS(TYPE_TY) pitch reset: %+2.1f -> +0.0 cent (next start)\n",
                   (double)trim_was);
        }
    }
#endif // defined(ENA_AVAS_TYPE_TY_SYNTH)
}


#if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
/* ---------------------------------------------------------------------------
 * The POT is the AVAS pitch knob, for whichever engine is the sounding one.
 *
 * Both AVAS engines take a fine pitch trim in cent (TYPE_TY rebuilds its step
 * tables, TYPE_LB scales its wander factor -- see each engine's header).  They are
 * runtime-exclusive, so ONE knob serves both: this sampler routes to the engine
 * that is currently switched on, and each engine keeps its own trim value and
 * its own clamp.
 *
 * The POT is not free.  Above ENG_SYNTH_POT_ACTIVE_VAL it enables the engine
 * synth, and the engine synth and AVAS are exclusive in fx_domain_48k (their
 * loads would add up), so a knob sweep would otherwise silence AVAS and start
 * the engine sound instead -- and every usable pitch position is above that
 * threshold.  Ownership is therefore decided at run time: classic_demo_app.c
 * refuses to enable the engine synth while either AVAS engine is active, and
 * this sampler only runs while one of them is switched on.
 *
 * Mapping: fully counter-clockwise = the engine's own pitch, clockwise raises it
 * (see local_avas_pot_to_cent).
 *
 * The knob must MOVE past the deadband before it writes anything, and a start or
 * stop re-seeds that reference.  Two reasons:
 *   - ADC jitter must not rebuild step tables or print;
 *   - a start must begin at the engine's own pitch even when the knob is parked
 *     at full travel, which is what makes the stop-time reset to 0 cent mean
 *     anything (otherwise it would be undone within 100 ms of every start).
 *
 * See docs_public/avas_pitch_pot_design.md for the measured numbers behind these.
 */
#define AVAS_POT_MAX            (4095)
#define AVAS_POT_EMA_SHIFT      (4)      /* same idiom/constant as the LED ramp in main.c */
#define AVAS_POT_DEADBAND       (24)     /* counts; ~1.2 cent over a 200 cent travel */
#define AVAS_POT_ZERO_SNAP      (82)     /* counts; the bottom 2 % reads as exactly 0 cent */
#define AVAS_POT_SAMPLE_MS      (100u)   /* knob follow rate */
#define AVAS_POT_SETTLE_MS      (300u)   /* print once the sweep has stopped */

static uint32_t s_pot_ema_acc;
static uint8_t  s_pot_ema_init;
static uint16_t s_pot_ref;         /* position the knob must move away from to write again */
static uint8_t  s_pot_print_due;
static uint32_t s_pot_change_ms;

/* Called on every AVAS start/stop.  There is no stored "who owns it" flag:
 * re-seeding the filter also re-seeds s_pot_ref at wherever the knob is sitting,
 * so the next write costs a real movement.  One mechanism, so nothing can
 * disagree with anything. */
static void local_avas_pot_release( void )
{
    s_pot_print_due = 0u;
    s_pot_ema_init  = 0u;
}

/* Print the trim of whichever engine is switched on (both when neither is, which
 * is what "?cs" wants).  Also the settle-time line after a knob sweep. */
void UsrOperate_avas_pitch_print( void )
{
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
    if( s_avas_type_ty_on ||
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
        !s_avas_type_lb_on
#else
        1   /* TYPE_TY is the only engine in this build -- always show it. */
#endif
      )
    {
        const float cent = app_avas_type_ty_get_pitch_cent();
        printf(" AVAS(TYPE_TY) pitch = %+2.1f cent (x%1.5f)%s\n",
               (double)cent, (double)app_avas_type_ty_get_pitch_ratio(),
               ( (cent >= AVAS_TYPE_TY_PITCH_LIMIT_CENT) || (cent <= -AVAS_TYPE_TY_PITCH_LIMIT_CENT) )
                   ? " [limit]" : "");
    }
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
    if( s_avas_type_lb_on || !s_avas_type_ty_on )
    {
        const float cent = app_avas_type_lb_get_pitch_cent();
        printf(" AVAS(Type_LB) pitch = %+2.1f cent (x%1.5f)%s\n",
               (double)cent, (double)app_avas_type_lb_get_pitch_ratio(),
               ( (cent >= AVAS_TYPE_LB_PITCH_LIMIT_CENT) || (cent <= -AVAS_TYPE_LB_PITCH_LIMIT_CENT) )
                   ? " [limit]" : "");
    }
#endif
}

/* 0..4095 -> 0..top_cent.  Unipolar: fully counter-clockwise is the engine's own
 * pitch and clockwise raises it.  The reference pitch therefore sits at a
 * mechanical end stop -- found by feel, no centre detent needed -- and the whole
 * travel is useful range instead of half of it.
 *
 * The bottom AVAS_POT_ZERO_SNAP counts read as exactly 0 so that "knob at rest"
 * is truly untrimmed rather than a fraction of a cent off.  Nothing is lost: at
 * the deadband those counts cannot be resolved as distinct settings anyway. */
static float local_avas_pot_to_cent( uint16_t pot, float top_cent )
{
    if( pot <= AVAS_POT_ZERO_SNAP )
    {
        return 0.0f;
    }

    return ((float)pot * top_cent) / (float)AVAS_POT_MAX;
}

/* Call once per main-loop pass; does its own 100 ms rate limiting. */
static void local_avas_pot_process( void )
{
    static uint32_t last_ms   = 0u;
    static uint8_t  have_last = 0u;
    const uint32_t  now_ms    = GetTicks();
    uint16_t        pot_f;
    int16_t         moved;

    /* Only while an AVAS engine is the selected source.  Otherwise the POT is
     * the engine synth's throttle and must not be interpreted here at all. */
    if( !( 0
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
           || s_avas_type_ty_on
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
           || s_avas_type_lb_on
#endif
         ) )
    {
        return;
    }

    if( have_last && ((uint32_t)(now_ms - last_ms) < AVAS_POT_SAMPLE_MS) )
    {
        return;
    }
    last_ms   = now_ms;
    have_last = 1u;

    /* Integer EMA:  acc += raw - (acc >> k);  filtered = acc >> k */
    {
        const uint16_t raw = POT_Read();   /* 0..0x0FFF */

        if( !s_pot_ema_init )
        {
            s_pot_ema_acc  = (uint32_t)raw << AVAS_POT_EMA_SHIFT;
            s_pot_ema_init = 1u;
            s_pot_ref      = raw;   /* movement is measured from where it sits now */
            return;                 /* never act on the seeding sample */
        }
        s_pot_ema_acc += (uint32_t)raw - (s_pot_ema_acc >> AVAS_POT_EMA_SHIFT);
        pot_f = (uint16_t)(s_pot_ema_acc >> AVAS_POT_EMA_SHIFT);
    }

    moved = (int16_t)((int32_t)pot_f - (int32_t)s_pot_ref);
    if( moved < 0 ) { moved = (int16_t)-moved; }

    if( moved > AVAS_POT_DEADBAND )
    {
        s_pot_ref       = pot_f;
        s_pot_change_ms = now_ms;
        s_pot_print_due = 1u;

        /* Route to the sounding engine, with ITS span and ITS clamp. */
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
        if( s_avas_type_ty_on )
        {
            app_avas_type_ty_set_pitch_cent(
                local_avas_pot_to_cent(pot_f, AVAS_TYPE_TY_POT_TOP_CENT) );
        }
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
        if( s_avas_type_lb_on )
        {
            app_avas_type_lb_set_pitch_cent(
                local_avas_pot_to_cent(pot_f, AVAS_TYPE_LB_POT_TOP_CENT) );
        }
#endif
    }
    else if( s_pot_print_due
             && ((uint32_t)(now_ms - s_pot_change_ms) >= AVAS_POT_SETTLE_MS) )
    {
        /* One line per sweep, after it stops.  Printing every 100 ms step would
         * flood the console -- and the console is the expensive part here, not
         * the pitch update. */
        s_pot_print_due = 0u;
        UsrOperate_avas_pitch_print();
    }
}
#endif // defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)


void UsrOperate_avas_synth_type_lb( void )
{
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
    if( !s_avas_type_lb_on )
    {
        #if defined(ENA_AVAS_TYPE_TY_SYNTH)
        if( app_avas_type_ty_is_active() )
        {
            printf(" AVAS(Type_LB): rejected -- TYPE_TY is still sounding (wait for it to fade).\n");
            return;
        }
        #endif
        s_avas_type_lb_on = 1;
        printf(" AVAS Synth(Type_LB): enable.\n");
        app_avas_type_lb_set_enable(true);

        /* Start at the engine's own pitch even with the knob parked at full
         * travel: the POT has to be moved before it may write.  Same reason as
         * the TYPE_TY path above. */
        local_avas_pot_release();
    }
    else
    {
        /* Read the trim BEFORE the stop; the stop resets it to 0 cent. */
        const float trim_was = app_avas_type_lb_get_pitch_cent();

        s_avas_type_lb_on = 0;
        printf(" AVAS Synth(Type_LB): disable.\n");
        app_avas_type_lb_set_enable(false);
        local_avas_pot_release();

        if( (trim_was > 0.05f) || (trim_was < -0.05f) )
        {
            printf(" AVAS(Type_LB) pitch reset: %+2.1f -> +0.0 cent (next start)\n",
                   (double)trim_was);
        }
    }
#endif // defined(ENA_AVAS_TYPE_LB_SYNTH)
}


/* Mute (button1) long press.  Long press = start, long press again = stop, as
 * before -- but each START alternates the engine: TYPE_TY, Type_LB, TYPE_TY, ...
 *
 * The alternation only advances when a start actually took effect, so a start
 * refused because the other engine is still fading keeps its turn and the next
 * long press retries the SAME engine instead of silently skipping it. */
void UsrOperate_avas_synth_button( void )
{
#if defined(ENA_AVAS_TYPE_TY_SYNTH) && defined(ENA_AVAS_TYPE_LB_SYNTH)
    static uint8_t next_is_type_lb = 0;

    /* Stop first: whichever is playing is what this press turns off. */
    if( s_avas_type_ty_on )    { UsrOperate_avas_synth();       return; }
    if( s_avas_type_lb_on ) { UsrOperate_avas_synth_type_lb(); return; }

    if( next_is_type_lb )
    {
        UsrOperate_avas_synth_type_lb();
        if( s_avas_type_lb_on ) next_is_type_lb = 0;
    }
    else
    {
        UsrOperate_avas_synth();
        if( s_avas_type_ty_on ) next_is_type_lb = 1;
    }
#elif defined(ENA_AVAS_TYPE_TY_SYNTH)
    UsrOperate_avas_synth();
#elif defined(ENA_AVAS_TYPE_LB_SYNTH)
    UsrOperate_avas_synth_type_lb();
#endif
}


void UsrOperate_surround( void )
{
#if defined(ENA_WIDEN_CTRL)
    static uint8_t state = 0;

    printf(" Surround:");

    state ^= 1;
    if( state )
    {
        snd_effect_play_se(SE_TONE_ON);

        printf(" enabled.\n");
#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( true, ENA_SMOOTH_TRANS_MS );    // mute
        delay_ms(600);
#endif //defined(ENA_SMOOTH_TRANS_MS)

        app_widen_enable();    // surround on

#if defined(ENA_SMOOTH_TRANS_MS)
        delay_ms(100);
        app_mute_set( false, ENA_SMOOTH_TRANS_MS );   // unmute
#endif //defined(ENA_SMOOTH_TRANS_MS)
    }
    else
    {
        snd_effect_play_se(SE_TONE_OFF);

        printf(" disabled.\n");
#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( true, ENA_SMOOTH_TRANS_MS );    // mute
        delay_ms(600);
#endif //defined(ENA_SMOOTH_TRANS_MS)

        app_widen_disable();

#if defined(ENA_SMOOTH_TRANS_MS)
        delay_ms(100);
        app_mute_set( false, ENA_SMOOTH_TRANS_MS );   // unmute
#endif //defined(ENA_SMOOTH_TRANS_MS)
    }

#else  //defined(ENA_WIDEN_CTRL)

    static uint8_t state = 0;

    // tone only
    state ^= 1;
    snd_effect_play_se( state^1 );
#endif //defined(ENA_WIDEN_CTRL)
}


void UsrOperate_Bmode( void )
{
#if defined(ENA_BASS_ENHANCER)
    static uint8_t state = 0;

    printf(" Bass-Enhancer: ");

    state ^= 1;
    if( state )
    {
        snd_effect_play_se(SE_TONE_ON);

        printf("enabled.\n");
#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( true, ENA_SMOOTH_TRANS_MS );    // mute
        delay_ms(600);
#endif //defined(ENA_SMOOTH_TRANS_MS)

        app_bassenh_enable( true );

#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( false, ENA_SMOOTH_TRANS_MS );   // unmute
#endif //defined(ENA_SMOOTH_TRANS_MS)
    }
    else
    {
        snd_effect_play_se(SE_TONE_OFF);

        printf("disabled.\n");
#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( true, ENA_SMOOTH_TRANS_MS );    // mute
        delay_ms(600);
#endif //defined(ENA_SMOOTH_TRANS_MS)

        app_bassenh_enable( false );

#if defined(ENA_SMOOTH_TRANS_MS)
        app_mute_set( false, ENA_SMOOTH_TRANS_MS );   // unmute
#endif //defined(ENA_SMOOTH_TRANS_MS)
    }

#else  //defined(ENA_BASS_ENHANCER)
    static uint8_t state = 0;

    // tone only
    state ^= 1;
    snd_effect_play_se( state^1 );

#endif //defined(ENA_BASS_ENHANCER)
}


#if ENA_DRC_DF2T_CASCADE
static void UsrOperate_temp_test_1( void )
{
    static uint8_t enabled = 0;

    printf(" temporary function test: ");

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        snd_effect_play_se(SE_TONE_ON);

        // add enabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
//        printf("MA8\n");
//        app_fir_filter_set_smoke_test_moving_average_8(4u);
        printf("FIR MA16\n");
        app_fir_filter_set_smoke_test_moving_average_16(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
    else
    {
        printf("disable.\n");
        snd_effect_play_se(SE_TONE_OFF);

        // add disabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
        app_fir_filter_set_smoke_test_bypass(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
}

static void UsrOperate_temp_test_2( void )
{
    static uint8_t enabled = 0;

    printf(" temporary function test: ");

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        snd_effect_play_se(SE_TONE_ON);
        // add enabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
        printf("FIR MA32\n");
        app_fir_filter_set_smoke_test_moving_average_32(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
    else
    {
        printf("disable.\n");
        snd_effect_play_se(SE_TONE_OFF);
        // add disabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
        app_fir_filter_set_smoke_test_bypass(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
}

static void UsrOperate_temp_test_3( void )
{
    static uint8_t enabled = 0;

    printf(" temporary function test: ");

    enabled ^= 1;
    if( enabled )
    {
        printf("enable.\n");
        snd_effect_play_se(SE_TONE_ON);
        // add enabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
        printf("FIR MA64\n");
        app_fir_filter_set_smoke_test_moving_average_64(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
    else
    {
        printf("disable.\n");
        snd_effect_play_se(SE_TONE_ON);
        // add disabling API here
        ///////////////////////////
        #if defined(ENA_FIR_FILTER)
        app_fir_filter_set_smoke_test_bypass(4u);
        #endif //defined(ENA_FIR_FILTER)

        ///////////////////////////
    }
}
#endif //ENA_DRC_DF2T_CASCADE


static void local_button_handler( void )
{
    BUTTON_EVENT_t ev[3];

    ev[0] = BUTTON_GetEvent(1);
    ev[1] = BUTTON_GetEvent(2);
    ev[2] = BUTTON_GetEvent(3);

    switch( ev[0] )
    {
    case BUTTON_EVENT_RELEASED:
#if ENA_DRC_DF2T_CASCADE
        UsrOperate_temp_test_1();
#else
        UsrOperate_mute();
#endif //ENA_DRC_DF2T_CASCADE
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 0);
        snd_effect_play_se(SE_TONE_NOTIF);
        /* The only button route into AVAS.  Each start alternates
         * TYPE_TY -> Type_LB -> TYPE_TY; see UsrOperate_avas_synth_button(). */
        #if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
        UsrOperate_avas_synth_button();
        #endif
        break;
    default:
        break;
    }
    switch( ev[1] )
    {
    case BUTTON_EVENT_RELEASED:
#if ENA_DRC_DF2T_CASCADE
        UsrOperate_temp_test_2();
#else
        UsrOperate_treble();
#endif //ENA_DRC_DF2T_CASCADE
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 1);
        snd_effect_play_se(SE_TONE_NOTIF);
        break;
    default:
        break;
    }
    switch( ev[2] )
    {
    case BUTTON_EVENT_RELEASED:
#if ENA_DRC_DF2T_CASCADE
        UsrOperate_temp_test_3();
#else
        UsrOperate_bass();
#endif //ENA_DRC_DF2T_CASCADE
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 2);
        snd_effect_play_se(SE_TONE_NOTIF);
        /* No AVAS here any more: the Mute button long press is the single button
         * route into AVAS (TYPE_TY). */
        break;
    default:
        break;
    }
}





static void local_touch_handler( void )
{
    BUTTON_EVENT_t ev[3];

    ev[0] = TOUCH_GetEvent(1);
    ev[1] = TOUCH_GetEvent(2);
    ev[2] = TOUCH_GetEvent(3);

    // LED indicator
    local_touch_led_indicator( ev[0], 0 );
    local_touch_led_indicator( ev[1], 1 );
    local_touch_led_indicator( ev[2], 2 );

    switch( ev[0] )
    {
    case BUTTON_EVENT_RELEASED:
        UsrOperate_clickclack();
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 0);
        UsrOperate_clickclack_toggle();
        break;
    default:
        break;
    }
    switch( ev[1] )
    {
    case BUTTON_EVENT_RELEASED:
        UsrOperate_surround();
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 1);
        snd_effect_play_se(SE_TONE_NOTIF);
        UsrOperate_pinger();
        break;
    default:
        break;
    }
    switch( ev[2] )
    {
    case BUTTON_EVENT_RELEASED:
        UsrOperate_Bmode();
        break;
//    case BUTTON_EVENT_LONG_PRESSED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
        printf(" TOUCH(%d): Long press.\n", 2);
        snd_effect_play_se(SE_TONE_NOTIF);
        UsrOperate_kinkon();
        break;
    default:
        break;
    }
}

static void local_touch_led_indicator( BUTTON_EVENT_t ev, uint8_t id )
{
#define LED_OFF   (0)
#define LED_ON    (1)
#define LED_KEEP  (2)

    static uint8_t led_on[3] = { LED_KEEP, LED_KEEP, LED_KEEP };

    if( id >= 3 )
    {
        return;
    }

    switch( ev )
    {
    case BUTTON_EVENT_NONE:
        // keep current state
        break;
    case BUTTON_EVENT_PRESSED:
        led_on[id] = LED_ON;
        break;
    case BUTTON_EVENT_RELEASED:
    case BUTTON_EVENT_LONG_PRESS_REACHED:
    case BUTTON_EVENT_LONG_PRESSED:
        led_on[id] = LED_OFF;
        break;
    default:
        led_on[id] = LED_KEEP;
        break;
    }

    switch( led_on[id] )
    {
    case LED_ON:
        LED_On( 2-id );
        break;
    case LED_OFF:
        LED_Off( 2-id );
        break;
    default:
        // leave LED
        break;
    }
}
