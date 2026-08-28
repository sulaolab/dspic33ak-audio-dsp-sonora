#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_CLASSIC
#  error "classic_console.c is Classic-app-owned; build it only in a Classic manifest (SONORA_APP_IS_CLASSIC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stdio.h>
#include <stdint.h>

#include "apps/sonora_app_console.h"
#include "classic_controls.h"   // UsrOperate_* (raw hotkey actions)

#if defined(ENA_BIQUAD_IIR_CASCADE) || defined(ENA_BASS_ENHANCER)
#include "app_utils.h"   // biquad_mono_t (needed by biquad_cascade_4ch.h / bass_enhancer.h)
#endif
#if defined(ENA_BIQUAD_IIR_CASCADE)
#include "biquad_cascade_4ch.h"
#endif
#if defined(ENA_BASS_ENHANCER)
#include "bass_enhancer.h"
#endif
#if defined(ENA_ENGINE_SYNTH)
#include "engine_synth.h"   // app_engine_synth_blip_start (raw 'b' hotkey)
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
#include "avas_synth_type_lb.h"   // wind-level trim for "*cn" / "?cn"
#endif

//===========================================================
// classic_console.c
//
// Classic application console module (module 'c'). Owns Classic console commands; the shared
// parser routes non-common modules here via the sonora_app_console_onmsg() contract.
//===========================================================

#if defined(ENA_BIQUAD_IIR_CASCADE)
// *cf VV (write only) : biquad cascade bypass(1)/normal(0) (was *nt02).
static void classic_console_biquad_bypass( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;
    const uint8_t  bypass = ( in_len > 0u ) ? msg->data[0] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 1u )
    {
        printf(" \"*cf VV\" bad args VV=bypass (0=normal 1=bypass)\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    if( bypass )
    {
        app_biquad_cascade_4ch_request_bypass();
    }
    else
    {
        app_biquad_cascade_4ch_request_normal();
    }
    msg->status = APP_CONSOLE_OK;
}
#endif /* defined(ENA_BIQUAD_IIR_CASCADE) */

#if defined(ENA_BASS_ENHANCER)
// *cb VV (write only) : bass enhancer LPF cap, VV = dB value (was *na00).
static void classic_console_bass_lpf_cap( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  cap_db  = ( in_len > 0u ) ? msg->data[0] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 1u )
    {
        printf(" \"*cb VV\" bad args VV=LPF cap dB\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    app_bassenh_dbg_set_lpf_cap_db( (float)cap_db );
    msg->status = APP_CONSOLE_OK;
}
#endif /* defined(ENA_BASS_ENHANCER) */

#if defined(ENA_AVAS_TYPE_LB_SYNTH)
// *cn VV (write) / ?cn (read) : Type_LB AVAS wind (noise) level trim.
//
// VV is a SIGNED byte in HALF-dB steps, so 04 = +2.0 dB and FD = -1.5 dB.  Signed
// because the point of the knob is an A/B in both directions, and half-dB because
// the question being asked ("a little more wind") is finer than a dB.
//
// The engine clamps the trim to +-12 dB: 0 dB is the level the analysis measured,
// not a starting point for a new balance.  NOTE THE DEFAULT IS NOT 0 dB -- the
// audition settled on -8.0 dB AT THE FINAL AMPLIFIER VOLUME and that is what the
// image boots with (see AVAS_TYPE_LB_NOISE_GAIN_DB, which records why an earlier
// audition at a lower level said +4.0), so "*cn00" is how the analysed level is
// heard and "*cnF0" is how the shipped level is restored after A/B'ing.  The
// print includes the headroom figure because raising the wind spends peak, and the
// output clamp does not sound like distortion -- it sounds like the wind stopping
// getting louder, which is the one failure mode that would be misread as "the
// gain change did not work".
static void classic_console_type_lb_noise_trim( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( msg->kind == '*' )
    {
        if( in_len != 1u )
        {
            printf(" \"*cn VV\" bad args VV=signed half-dB wind trim (04=+2.0dB FD=-1.5dB 00=measured)\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        app_avas_type_lb_set_noise_gain_db( 0.5f * (float)(int8_t)msg->data[0] );
    }
    else if( in_len != 0u )
    {
        printf(" \"?cn\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    printf(" Type_LB wind trim = %+.1f dB (0 dB = measured, -8.0 = default), clamp at %+.1f dB\n",
           (double)app_avas_type_lb_get_noise_gain_db(),
           (double)app_avas_type_lb_noise_headroom_db());
    msg->status = APP_CONSOLE_OK;
}
// *cg VV (write) / ?cg (read) : Type_LB AVAS wind gust RATE.
//
// VV is UNSIGNED in TENTHS of a Hz, so 0C = 1.2 Hz (the analysed value and the
// default) and 06 = 0.6 Hz.  Unsigned because a rate has no sign, and tenths
// because the audible range of "how slowly the wind breathes" is under 2 Hz.
//
// The depth stays at the model's 1.5 dB sd: the engine rescales the walk's drive
// for the new pole (see avas_synth_type_lb_set_gust_hz).  So this key moves ONE axis,
// which is the point -- the water/wind mistake in this design's history was a
// bandwidth change being read as a rate change.
static void classic_console_type_lb_gust_rate( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( msg->kind == '*' )
    {
        if( in_len != 1u )
        {
            printf(" \"*cg VV\" bad args VV=gust rate in 0.1 Hz (0C=1.2Hz analysed, 06=0.6Hz)\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        app_avas_type_lb_set_gust_hz( 0.1f * (float)msg->data[0] );
    }
    else if( in_len != 0u )
    {
        printf(" \"?cg\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    printf(" Type_LB gust rate = %.2f Hz (1.20 Hz = analysed), depth/corr unchanged\n",
           (double)app_avas_type_lb_get_gust_hz());
    msg->status = APP_CONSOLE_OK;
}
// *cd VV (write) / ?cd (read) : Type_LB AVAS wind gust DEPTH.
//
// VV is UNSIGNED in TENTHS of a dB sd per band, so 0F = 1.5 dB (the model's value
// and the default) and 1E = 3.0 dB.
//
// The print carries the warning threshold because the gain law is the first-order
// 10^(x/20): past ~3 dB it breaks asymmetrically and the negative side reaches
// zero, so the deep end of this knob sounds like PUMPING rather than like a deeper
// gust.  That is a property of the approximation, not a verdict about the depth --
// see the header on replacing the law if a deep value wins.
static void classic_console_type_lb_gust_depth( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;
    float depth;

    msg->data_len = 0u;

    if( msg->kind == '*' )
    {
        if( in_len != 1u )
        {
            printf(" \"*cd VV\" bad args VV=gust depth in 0.1 dB sd (0F=1.5dB model, 1E=3.0dB)\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        app_avas_type_lb_set_gust_depth_db( 0.1f * (float)msg->data[0] );
    }
    else if( in_len != 0u )
    {
        printf(" \"?cd\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    /* The "%s" suffix is free here: this build already links the s-carrying
     * __printf_* variant (__sio_printf_dfosux, pulled for this file's conversion
     * set), so the conditional warning costs no new instantiation -- checked in the
     * map rather than assumed, because at 86 % ROM one extra printf variant would
     * be a real cost for one line of text. */
    depth = app_avas_type_lb_get_gust_depth_db();
    printf(" Type_LB gust depth = %.1f dB sd/band (1.5 dB = model)%s\n",
           (double)depth,
           (depth > app_avas_type_lb_gust_depth_warn_db())
               ? "  [1st-order gain law: expect pumping, not depth]" : "");
    msg->status = APP_CONSOLE_OK;
}
// *cw VV (write) / ?cw (read) : Type_LB AVAS wind gust band-to-band CORRELATION.
//
// VV is UNSIGNED in PERCENT, so 00 = independent (the model, and bit-identical to
// an image without this knob) and 64 = every band breathing as one.
//
// THIS IS THE AXIS THE "the original's wind breathes more slowly" VERDICT NEEDS.
// With 12 independent walks the bands partly cancel, so the TOTAL level hardly
// moves however deep or slow each band's own gust is -- which is why the rate knob
// was only faintly audible even at 25.5 Hz on hardware.  The depth does not move
// with this: the mix sqrt(1-c^2)*w_ind + c*w_common is unit sd at every c.
static void classic_console_type_lb_gust_corr( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( msg->kind == '*' )
    {
        if( in_len != 1u )
        {
            printf(" \"*cw VV\" bad args VV=gust band correlation in %% (00=independent model, 64=100%%)\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        app_avas_type_lb_set_gust_corr( 0.01f * (float)msg->data[0] );
    }
    else if( in_len != 0u )
    {
        printf(" \"?cw\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    printf(" Type_LB gust band correlation = %.0f %% (0 %% = independent model), depth unchanged\n",
           (double)(100.0f * app_avas_type_lb_get_gust_corr()));
    msg->status = APP_CONSOLE_OK;
}
// *cl CCVV (write) / ?cl (read) : Type_LB AVAS per-cluster TONE level.
//
// CC is the cluster index 00..06, or FF for all seven at once.
// VV is SIGNED in HALF-dB steps (two's complement), so 00 = the analysed level,
// 08 = +4.0 dB, F8 = -4.0 dB.  Range -24.0 .. +12.0 dB, clamped.
//
// WHY THIS IS SEPARATE FROM THE WIND TRIM.  The wind ("gogogo") and cluster 0
// ("fuuuu", 50 lines inside +-48 Hz around 66.8 Hz, 85 % of the line energy)
// OVERLAP: the noise bands at 24/35/51/73/106 Hz sit on cluster 0's span.  So a
// wind trim chosen on its own can bury the tone component underneath it, and
// telling those two apart by ear needs them moved one at a time.
//
// NOTE THE DIRECTION: "?cl" prints the headroom, and it is only about +1 dB --
// the tone already owns 84 % of the measured peak.  Making one cluster stand out
// is done by DUCKING the other six, which is why the range goes 24 dB down.
static void classic_console_type_lb_cluster_gain( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( msg->kind == '*' )
    {
        if( in_len != 2u )
        {
            printf(" \"*cl CCVV\" bad args CC=cluster 00..06 (FF=all), VV=signed 0.5 dB (00=analysed)\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
        app_avas_type_lb_set_cluster_gain_db( msg->data[0],
                                           0.5f * (float)(int8_t)msg->data[1] );
    }
    else if( in_len != 0u )
    {
        printf(" \"?cl\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    /* All seven every time: the point of the knob is the BALANCE between them,
     * and a single line cannot show a balance. */
    for( uint8_t k = 0u; k < AVAS_TYPE_LB_L3_CLUSTERS; k++ )
    {
        printf("  Type_LB cluster %u: %7.2f Hz  %+.1f dB\n",
               (unsigned)k,
               (double)app_avas_type_lb_get_cluster_carrier_hz(k),
               (double)app_avas_type_lb_get_cluster_gain_db(k));
    }
    printf("  (0 dB = analysed; headroom for raising the tone %+.1f dB)\n",
           (double)app_avas_type_lb_cluster_headroom_db());
    msg->status = APP_CONSOLE_OK;
}
#endif /* defined(ENA_AVAS_TYPE_LB_SYNTH) */

// ?cs (read, no payload) : Classic status print.
static void classic_console_status( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( in_len != 0u )
    {
        printf(" \"?cs\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

#if defined(ENA_AVAS_TYPE_TY_SYNTH) || defined(ENA_AVAS_TYPE_LB_SYNTH)
    UsrOperate_avas_pitch_print();
#endif
#if defined(ENA_BASS_ENHANCER)
    app_bassenh_dbg_prt_lpf_cap_db();
#endif
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
    /* Listed here and not only under "?cn" because a trim left off 0 dB is
     * exactly the kind of state that gets attributed to the synth itself. */
    printf(" Type_LB wind trim = %+.1f dB, gust %.2f Hz / %.1f dB sd / corr %.0f %%\n",
           (double)app_avas_type_lb_get_noise_gain_db(),
           (double)app_avas_type_lb_get_gust_hz(),
           (double)app_avas_type_lb_get_gust_depth_db(),
           (double)(100.0f * app_avas_type_lb_get_gust_corr()));
    /* The per-cluster trims are 7 numbers, too many for a status line, so the
     * status only says WHETHER the tone is off its analysed balance -- which is
     * the part that would otherwise be blamed on the synth.  "?cl" prints them. */
    {
        bool cl_trimmed = false;
        for( uint8_t k = 0u; k < AVAS_TYPE_LB_L3_CLUSTERS; k++ )
        {
            if( app_avas_type_lb_get_cluster_gain_db(k) != 0.0f ) { cl_trimmed = true; }
        }
        if( cl_trimmed )
        {
            printf(" Type_LB cluster levels are TRIMMED off the analysed balance (see \"?cl\")\n");
        }
    }
#endif
#if !defined(ENA_AVAS_TYPE_TY_SYNTH) && !defined(ENA_AVAS_TYPE_LB_SYNTH) && !defined(ENA_BASS_ENHANCER)
    printf(" \"?cs\" no Classic status items built into this config\n");
#endif
    msg->status = APP_CONSOLE_OK;
}

// *cy SS (write only) : synth group toggle. SS selects which synth; the action
// mirrors the corresponding single-key hotkey (same UsrOperate_* / engine blip),
// so hotkey and command share one enable-state source (no desync). No value byte.
// A known synth not built into this target returns UNSUPPORTED (vs unknown subcode
// which is BAD_DATA), so callers can tell "not on this board" from "no such synth".
static void classic_console_synth( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 1u )
    {
        printf(" \"*cy SS\" SS=synth (00 avas(TYPE_TY) 01 pinger 02 kinkon 03 clickclack 04 engine)\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    switch( subcode )
    {
    case 0x00u:   // avas (TYPE_TY; Type_LB is the 'A' hotkey only)
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
        UsrOperate_avas_synth();
        msg->status = APP_CONSOLE_OK;
#else
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
#endif
        break;
    case 0x01u:   // pinger
#if defined(ENA_PINGER_SOUND)
        UsrOperate_pinger();
        msg->status = APP_CONSOLE_OK;
#else
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
#endif
        break;
    case 0x02u:   // kinkon
#if defined(ENA_KINKON)
        UsrOperate_kinkon();
        msg->status = APP_CONSOLE_OK;
#else
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
#endif
        break;
    case 0x03u:   // clickclack (on/off)
#if defined(ENA_CLICK_CLACK)
        UsrOperate_clickclack();
        msg->status = APP_CONSOLE_OK;
#else
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
#endif
        break;
    case 0x04u:   // engine blip (one-shot trigger)
#if defined(ENA_ENGINE_SYNTH)
        app_engine_synth_blip_start();
        msg->status = APP_CONSOLE_OK;
#else
        msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
#endif
        break;
    default:
        printf(" \"*cy\" unknown synth subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

void sonora_app_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    // This build's selected application owns module 'c' (Classic). Anything else is not ours.
    if( msg->module != 'c' )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    switch( msg->name )
    {
#if defined(ENA_BIQUAD_IIR_CASCADE)
    case 'f':   // biquad cascade bypass/normal: "*cf VV" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        classic_console_biquad_bypass( msg );
        break;
#endif /* defined(ENA_BIQUAD_IIR_CASCADE) */

#if defined(ENA_BASS_ENHANCER)
    case 'b':   // bass enhancer LPF cap: "*cb VV" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        classic_console_bass_lpf_cap( msg );
        break;
#endif /* defined(ENA_BASS_ENHANCER) */

#if defined(ENA_AVAS_TYPE_LB_SYNTH)
    case 'n':   // Type_LB wind trim: "*cn VV" (write) / "?cn" (read)
        classic_console_type_lb_noise_trim( msg );
        break;

    case 'g':   // Type_LB gust rate: "*cg VV" (write) / "?cg" (read)
        classic_console_type_lb_gust_rate( msg );
        break;

    case 'd':   // Type_LB gust depth: "*cd VV" (write) / "?cd" (read)
        classic_console_type_lb_gust_depth( msg );
        break;

    case 'w':   // Type_LB gust band correlation: "*cw VV" (write) / "?cw" (read)
        classic_console_type_lb_gust_corr( msg );
        break;

    case 'l':   // Type_LB per-cluster tone level: "*cl CCVV" (write) / "?cl" (read)
        classic_console_type_lb_cluster_gain( msg );
        break;
#endif /* defined(ENA_AVAS_TYPE_LB_SYNTH) */

    case 's':   // Classic status: "?cs" (read only, no payload)
        if( msg->kind != '?' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        classic_console_status( msg );
        break;

    case 'y':   // synth group toggle: "*cy SS" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        classic_console_synth( msg );
        break;

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}


// Raw single-key hotkeys (not part of a '*'/'?' console command line). Relocated verbatim from the
// UART layer (app_debug.c) so the console/UART infrastructure stays app-blind. The mapping and its
// per-feature #if gates are preserved 1:1. Keys whose action may block (mute/settle with delay_ms,
// or a synth blip) return HANDLED_FLUSH so the UART layer drops keystrokes that queued during the
// operation, exactly as the original code did with its inline local_flush_uart_data().
sonora_hotkey_result_t sonora_app_handle_hotkey( char c )
{
    switch( c )
    {
#if defined(ENA_BIQUAD_IIR_CASCADE)
    case 'B':
        printf("\n Bypass IIR block process.\n");
        app_biquad_cascade_4ch_request_bypass();
        return SONORA_HOTKEY_HANDLED;
    case 'b':
        printf("\n Activate IIR block process.\n");
        app_biquad_cascade_4ch_request_normal();
        return SONORA_HOTKEY_HANDLED;
#endif /* defined(ENA_BIQUAD_IIR_CASCADE) */

    /* The two AVAS sources are exclusive at run time (their loads would add up),
     * so each key only ever asks; the toggle refuses if the other is sounding. */
#if defined(ENA_AVAS_TYPE_TY_SYNTH)
    case 'a':
        UsrOperate_avas_synth();
        return SONORA_HOTKEY_HANDLED;
#endif /* defined(ENA_AVAS_TYPE_TY_SYNTH) */
#if defined(ENA_AVAS_TYPE_LB_SYNTH)
    case 'A':
        UsrOperate_avas_synth_type_lb();
        return SONORA_HOTKEY_HANDLED;
#endif /* defined(ENA_AVAS_TYPE_LB_SYNTH) */

    /* No hotkey for the AVAS pitch trim: the POT is its only control (see
     * classic_controls.c).  '[' / ']' briefly held it and were removed -- keys
     * would need a soft-takeover arbitration against an absolute knob, for a
     * second way to do the same thing.  '?cs' reports the current value. */

    case 'k':
        UsrOperate_kinkon();
        return SONORA_HOTKEY_HANDLED;
    case 'p':
        UsrOperate_pinger();
        return SONORA_HOTKEY_HANDLED;

#if defined(ENA_CLICK_CLACK)
    case 'C':
        UsrOperate_clickclack();
        return SONORA_HOTKEY_HANDLED;
    case 'c':
        UsrOperate_clickclack_toggle();
        return SONORA_HOTKEY_HANDLED;
#endif /* defined(ENA_CLICK_CLACK) */

#if defined(ENA_ENGINE_SYNTH)
    case 'b':
        app_engine_synth_blip_start();
        return SONORA_HOTKEY_HANDLED_FLUSH;
#endif /* defined(ENA_ENGINE_SYNTH) */

#if defined(ENA_BASS_ENHANCER)
    case '-':
        app_bassenh_dbg_minus_key_hdr();
        return SONORA_HOTKEY_HANDLED_FLUSH;
    case '=':   // +key
        app_bassenh_dbg_plus_key_hdr();
        return SONORA_HOTKEY_HANDLED_FLUSH;
#endif /* defined(ENA_BASS_ENHANCER) */

    case 'e':
        UsrOperate_Bmode();
        return SONORA_HOTKEY_HANDLED_FLUSH;
    case 's':
        UsrOperate_surround();
        return SONORA_HOTKEY_HANDLED_FLUSH;

    default:
        return SONORA_HOTKEY_IGNORED;
    }
}
