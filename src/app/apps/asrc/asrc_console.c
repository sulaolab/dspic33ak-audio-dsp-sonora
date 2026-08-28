#include "app_specific_config_defs.h"

#if !SONORA_APP_IS_ASRC
#  error "asrc_console.c is ASRC-app-owned; build it only in an ASRC manifest (SONORA_APP_IS_ASRC). Check nbproject/configurations.xml source exclusions."
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <math.h>

#include "apps/sonora_app_console.h"
#include "audio_transport_control.h"   // audio_transport_reconfigure_leg_rate_hz
#include "audio_transport.h"           // *ap: audio_transport_restart_codec_b_only_declick
#include "asrc_audio_path.h"           // *ap: A-loopback pop metric (asrc_audio_path_pop_meas_*)
#include "board/devices/wm8904.h"      // *aw/?aw: raw codec register poke
#include "transport_static_config.h"   // transport_leg_t, TRANSPORT_LEG_A/B
#include "timer_app.h"                 // *ap: delay_ms for the measurement windows
#include "audio_app_meas.h"             // *ac/?ac/*at: capture + tone/level measurement harness
#include "audio_app_meas_tones.h"       // *at 04: MEAS_TABLE_N_TONES / MEAS_TONE_ROW_* row ids
#include "audio_transport_snapshot.h"  // *ar: the legs' currently configured rates
#include "audio_transport_client.h"    // *ar: AUDIO_TRANSPORT_LEG_A/B snapshot indices
#include "audio_app_asrc.h"             // *al: interp load multiplier
#include "asrc_fir_kernel_bench.h"   // *aq: front-stage FIR kernel cycles/MAC bench
                                       // *ar: audio_app_asrc_rate_pair_is_supported

//===========================================================
// asrc_console.c
//
// ASRC application console module (module 'a'). Owns ASRC console commands; the shared parser
// routes non-common modules here via the sonora_app_console_onmsg() contract. App-agnostic
// common code never sees ASRC command policy. Depends only on the public audio-transport
// control API, not on ASRC engine internals.
//===========================================================

static bool asrc_console_request_sample_rate_hz( uint8_t target, uint32_t sample_rate_hz )
{
    transport_leg_t leg;
    switch( target )
    {
        case 0u: leg = TRANSPORT_LEG_A; break;
        case 1u: leg = TRANSPORT_LEG_B; break;
        default: return false;
    }
    return audio_transport_reconfigure_leg_rate_hz( leg, sample_rate_hz );
}

// *ar CC RR : set a codec leg's sample rate. CC = 0(A)/1(B), RR = rate index (0..9).
//
// RR=9 is 96 kHz, the highest rate the driver offers (there is no 88.2 kHz entry).
// It is listed on every build so the index numbering is stable across images (a
// build-dependent menu makes bench notes and scripts ambiguous), but it is only
// REACHABLE where the transport is a 2-slot I2S frame and the leg is not
// full-duplex: the WM8904 cannot run ADC and DAC together at or above its 88.2 kHz
// boundary, and a TDM8 frame at 96 kHz would need 24.576 MHz BCLK. Where it cannot
// be reached the request is rejected up front with the reason, before any stream
// teardown.
static void asrc_console_rate( app_console_msg_t* msg )
{
    static const uint32_t rates_hz[] =
    {
        8000u, 11025u, 12000u, 16000u, 22050u,
        24000u, 32000u, 44100u, 48000u, 96000u,
    };
    const uint16_t in_len    = msg->data_len;   // input payload length (data_len is reused for the response)
    const uint8_t target     = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t rate_index = ( in_len > 1u ) ? msg->data[1] : 0xFFu;

    msg->data_len = 0u;   // response carries no data bytes; text goes via printf

    if( ( in_len != 2u ) || ( target > 1u ) ||
        ( rate_index >= (uint8_t)( sizeof(rates_hz) / sizeof(rates_hz[0]) ) ) )
    {
        printf(" \"*ar CC RR\" bad args CC=%u(0=A 1=B) RR=%u (0=8k 1=11k025 2=12k 3=16k 4=22k05 5=24k 6=32k 7=44k1 8=48k 9=96k)\n",
               (unsigned)target, (unsigned)rate_index);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

#if defined(APP_ASRC_AK128_BASELINE_RATE_ONLY) && APP_ASRC_AK128_BASELINE_RATE_ONLY
    if( ( rates_hz[rate_index] != 32000u ) &&
        ( rates_hz[rate_index] != 44100u ) &&
        ( rates_hz[rate_index] != 48000u ) )
    {
        printf(" \"*ar\" AK128 baseline supports 32 / 44.1 / 48 kHz only\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }
#endif

    /*
     * Ask first, act second. The capability answer is a build fact, so a request
     * that cannot succeed is refused here with its reason and the running stream is
     * left completely untouched -- no mute, no restart, no audible gap.
     */
    {
        const char* reason = NULL;
        const transport_leg_t leg =
            ( target == 0u ) ? TRANSPORT_LEG_A : TRANSPORT_LEG_B;
        if( !audio_transport_leg_rate_is_supported( leg, rates_hz[rate_index], &reason ) )
        {
            printf(" \"*ar\" WM8904-%c -> %lu Hz not available in this build: %s\n",
                   ( target == 0u ) ? 'A' : 'B',
                   (unsigned long)rates_hz[rate_index],
                   ( reason != NULL ) ? reason : "unsupported" );
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            return;
        }
    }

    /*
     * Second gate, and it judges the PAIR rather than the leg: what some rate pairs fail on is
     * the ASRC ring geometry, not the codec.  Past the ratio the ring can look ahead, the
     * resampler zero-fills the tail of every block with rd held, FOREVER -- the fill setpoint is
     * already clamped and the fill is already at the overflow guard, so there is nothing left for
     * the servo to converge to.  Refuse it here, stream still untouched, rather than leave
     * audibly broken audio running and no counter moving (miss and sat both stay 0).
     *
     * A build with a decimating front end answers true throughout; the predicate knows that.
     */
#if APP_B_INDEP_DOMAIN && APP_B_ROUTE_IS_ASRC
    {
        audio_transport_snapshot_t snap;
        if( audio_transport_snapshot_get( &snap ) &&
            ( snap.leg_count > (uint8_t)AUDIO_TRANSPORT_LEG_B ) )
        {
            uint32_t    rate_a = snap.legs[AUDIO_TRANSPORT_LEG_A].configured_rate_hz;
            uint32_t    rate_b = snap.legs[AUDIO_TRANSPORT_LEG_B].configured_rate_hz;
            const char* reason = NULL;

            if( target == 0u ) { rate_a = rates_hz[rate_index]; }
            else               { rate_b = rates_hz[rate_index]; }

            if( !audio_app_asrc_rate_pair_is_supported( rate_a, rate_b, &reason ) )
            {
                printf(" \"*ar\" A=%lu Hz + B=%lu Hz not supported by this image: %s\n",
                       (unsigned long)rate_a, (unsigned long)rate_b,
                       ( reason != NULL ) ? reason : "unsupported rate pair" );
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                return;
            }
        }
    }
#endif

    if( !asrc_console_request_sample_rate_hz( target, rates_hz[rate_index] ) )
    {
        printf(" \"*ar\" rate switch rejected (unsupported fs or not a codec-master build)\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    printf(" \"*ar\" WM8904-%c rate -> %lu Hz (restarting)\n",
           ( target == 0u ) ? 'A' : 'B',
           (unsigned long)rates_hz[rate_index] );
    msg->status = APP_CONSOLE_OK;
}

#if APP_ASRC_MEAS
// *ac (write, no payload) : arm a one-shot A->B capture (was *nt20).
// ?ac (read, no payload)  : dump the captured buffer for offline FFT (was *nt21).
static void asrc_console_capture( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( in_len != 0u )
    {
        printf(" \"*ac\"/\"?ac\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    if( msg->kind == '*' )
    {
        audio_app_meas_arm();
    }
    else
    {
        audio_app_meas_dump();
    }
    msg->status = APP_CONSOLE_OK;
}

// *at SS [VV] (write only) : tone/level select, subcode SS selects the sub-command.
//   SS=0 : select the low (mid) test tone (was *nt22)
//   SS=1 : select the high (band-edge) test tone (was *nt23).  18 kHz REAL: the table row is
//          resolved from the live source-leg rate, so this works on a 48 kHz or 96 kHz leg.
//   SS=2 VV : set tone level by index 0..5 (was *nt2A)
//   SS=3 VV : set fixed-decimator tone index 0..8 (was *nt45; APP_ASRC_48K_TO_8_DECIMATOR only)
//   SS=4 VV : select the tone table ROW explicitly, VV = MEAS_TONE_ROW_* (0=low, 1=high@48k,
//          2=high@96k).  Bench override for SS=1's rate matching -- e.g. to show that the
//          48 kHz HF row really does alias when played on a 96 kHz leg.
static void asrc_console_tone( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t  value   = ( in_len > 1u ) ? msg->data[1] : 0xFFu;

    msg->data_len = 0u;

    switch( subcode )
    {
    case 0u:
        if( in_len != 1u )
        {
            printf(" \"*at 00\" takes no extra payload\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_set_tone_low();
        msg->status = APP_CONSOLE_OK;
        break;

    case 1u:
        if( in_len != 1u )
        {
            printf(" \"*at 01\" takes no extra payload\n");
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_set_tone_high();
        msg->status = APP_CONSOLE_OK;
        break;

    case 2u:
        if( ( in_len != 2u ) || ( value > 5u ) )
        {
            printf(" \"*at 02 VV\" bad level index VV=%u (0=-1 1=-60 2=-20 3=-40 4=-80 5=-6 dBFS)\n",
                   (unsigned)value);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_set_level_idx( value );
        msg->status = APP_CONSOLE_OK;
        break;

#if APP_ASRC_48K_TO_8_DECIMATOR
    case 3u:
        if( ( in_len != 2u ) || ( value > 8u ) )
        {
            printf(" \"*at 03 VV\" bad decimator tone index VV=%u (0..8)\n", (unsigned)value);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_set_decimator_tone_idx( value );
        msg->status = APP_CONSOLE_OK;
        break;
#endif /* APP_ASRC_48K_TO_8_DECIMATOR */

    case 4u:
        if( ( in_len != 2u ) || ( value >= (uint8_t)MEAS_TABLE_N_TONES ) )
        {
            printf(" \"*at 04 VV\" bad tone row VV=%u (0=low 1=high@48k 2=high@96k)\n",
                   (unsigned)value);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_set_tone_row( value );
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        printf(" \"*at\" unknown subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

// *as SS [..] (write only) : servo control, subcode SS selects the sub-command. ?as (read, no
// payload) : Q50/Q51/Q52 status readout (was *nt3A). data[1..] payload matches the old
// pdata[1..] layout verbatim; only the subcode byte (data[0]) is new.
//   SS=0x00       : freeze the A->B step, open the control loop (was *nt24)
//   SS=0x01       : restore normal fill control + feed-forward (was *nt25)
//   SS=0x02 VV    : feed-forward ratio freeze, VV: 1=freeze 0=unfreeze (was *nt2E)
//   SS=0x03 VV    : hold/release corr_lpf, VV: 1=hold 0=release (was *nt2F)
//   SS=0x04 VV    : corr_lpf motion selector VV=0..3 (was *nt30)
//   SS=0x05 VV WW : applied-step smoothing, VV=on/off, WW=beta index 0..3 (was *nt32)
//   SS=0x06 VV    : Q50 fast-acquisition enable, VV=1/0 (was *nt39)
//   SS=0x07       : warm servo restart / reset_all (was *nt3B)
//   SS=0x08 VV    : Q51 feed-forward rate seed enable, VV=1/0 (was *nt3C)
//   SS=0x09 VV    : Q52 differential fill-drift rate seed enable, VV=1/0 (was *nt3D)
//   SS=0x0A       : freeze at live feed-forward ratio + centre FIFO (was *nt46)
static void asrc_console_servo_write( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t  v1      = ( in_len > 1u ) ? msg->data[1] : 0xFFu;
    const uint8_t  v2      = ( in_len > 2u ) ? msg->data[2] : 0xFFu;

    msg->data_len = 0u;

    switch( subcode )
    {
    case 0x00u:
        if( in_len != 1u ) { printf(" \"*as 00\" takes no extra payload\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_freeze();
        /* Says "A->B step" historically, but audio_app_asrc_freeze() freezes EVERY live engine --
         * B->A too where APP_B_ROUTE_USES_BA. The old wording made a correct B->A capture look
         * like it had been taken with a running servo. Keep the "*MEAS freeze" prefix: the capture
         * script matches on it. */
        printf(" *MEAS freeze step, all live legs (loop open)\n");
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x01u:
        if( in_len != 1u ) { printf(" \"*as 01\" takes no extra payload\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_unfreeze();
        printf(" *MEAS unfreeze (normal control)\n");
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x02u:
        if( in_len != 2u ) { printf(" \"*as 02 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        if( v1 != 0u ) { audio_app_asrc_ff_freeze(); } else { audio_app_asrc_ff_unfreeze(); }
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x03u:
        if( in_len != 2u ) { printf(" \"*as 03 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        if( v1 != 0u ) { audio_app_asrc_corr_hold(); } else { audio_app_asrc_corr_release(); }
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x04u:
        if( ( in_len != 2u ) || ( v1 > 3u ) )
        {
            printf(" \"*as 04 VV\" bad corr_lpf mode VV=%u (0=FULL 1=SLOW 2=FAST 3=HOLD)\n", (unsigned)v1);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_asrc_corr_mode( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x05u:
        if( ( in_len != 3u ) || ( v2 > 3u ) )
        {
            printf(" \"*as 05 VV WW\" bad args, beta index WW=%u (0..3)\n", (unsigned)v2);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_asrc_step_smooth( v1, v2 );
        printf(" *MEAS step_smooth=%u beta=%.4f (LPF on applied step; DC fill authority kept)\n",
               (unsigned)audio_app_asrc_get_step_smooth(),
               (double)audio_app_asrc_get_step_smooth_beta() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x06u:
        if( in_len != 2u ) { printf(" \"*as 06 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q50_enable( v1 );
        printf(" *MEAS q50_fast_acq=%u (re-arms on next ratio lock)\n", (unsigned)audio_app_asrc_q50_get_en() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x07u:
        if( in_len != 1u ) { printf(" \"*as 07\" takes no extra payload\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_reset_all();
        printf(" *MEAS q50 servo restart (re-arm on next ratio lock; q50_en=%u)\n", (unsigned)audio_app_asrc_q50_get_en() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x08u:
        if( in_len != 2u ) { printf(" \"*as 08 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q51_enable( v1 );
        printf(" *MEAS q51_rate_seed=%u (re-arms on next ratio lock)\n", (unsigned)audio_app_asrc_q51_get_en() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x09u:
        if( in_len != 2u ) { printf(" \"*as 09 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q52_enable( v1 );
        printf(" *MEAS q52_drift_seed=%u (re-arms on next ratio lock)\n", (unsigned)audio_app_asrc_q52_get_en() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x0Au:
        if( in_len != 1u ) { printf(" \"*as 0A\" takes no extra payload\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_freeze_ratio_ab();
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        printf(" \"*as\" unknown subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

// ?as (read, no payload) : Q50/Q51/Q52 status readout (was *nt3A).
static void asrc_console_servo_read( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( in_len != 0u )
    {
        printf(" \"?as\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    {
        const uint32_t lp = audio_app_asrc_q50_lock_pulls();
        const float    ph = audio_app_asrc_q50_pull_hz();
        printf(" *MEAS q50 en=%u state=%u lock_pulls=%lu lock_s=%.3f pull_hz=%.2f ho_stepdiff=%.3e strend=%.3e\n",
               (unsigned)audio_app_asrc_q50_get_en(),
               (unsigned)audio_app_asrc_q50_state(),
               (unsigned long)lp, (double)( ph > 0.0f ? (float)lp / ph : 0.0f ),
               (double)ph, (double)audio_app_asrc_q50_ho_stepdiff(),
               (double)audio_app_asrc_q50_strend() );
        printf(" *MEAS q51 en=%u applied=%u est_step=%.9f corr_seed=%.3e\n",
               (unsigned)audio_app_asrc_q51_get_en(),
               (unsigned)audio_app_asrc_q51_applied(),
               (double)audio_app_asrc_q51_est_step(),
               (double)audio_app_asrc_q51_corr_seed() );
        printf(" *MEAS q52 en=%u est_step=%.9f dfill=%.3f ratio0=%.9f fill0=%.3f\n",
               (unsigned)audio_app_asrc_q52_get_en(),
               (double)audio_app_asrc_q52_est_step(),
               (double)audio_app_asrc_q52_dfill(),
               (double)audio_app_asrc_q52_ratio0(),
               (double)audio_app_asrc_q52_fill0() );
    }
    msg->status = APP_CONSOLE_OK;
}

// *ax SS [..] (write only) : experiment/perturbation, subcode SS selects the sub-command.
//   SS=0x00 VV       : frozen-step perturbation, VV=letter-free index 0..8 (was *nt29)
//   SS=0x01 VV       : observer-interference isolation probe, VV=mode 0..2 (was *nt2D)
//   SS=0x02 VV WW XX : diagnostic servo-error injection, VV=en, WW=freq_hz, XX=amp index 0..3 (was *nt33)
//   SS=0x03 VV WW    : servo sensitivity screen, VV=which 0..1, WW=code 0..2 (was *nt34)
static void asrc_console_experiment( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t  v1      = ( in_len > 1u ) ? msg->data[1] : 0xFFu;
    const uint8_t  v2      = ( in_len > 2u ) ? msg->data[2] : 0xFFu;
    const uint8_t  v3      = ( in_len > 3u ) ? msg->data[3] : 0xFFu;

    msg->data_len = 0u;

    switch( subcode )
    {
    case 0x00u:
        // index -> (mode, req): 0=base, ULP +-1/+-8, ppm +-100/+-1000. All derived from base.
        {
            static const struct { int mode; int32_t req; } q3pts[9] =
            {
                {0,0}, {1,+1}, {1,-1}, {1,+8}, {1,-8}, {2,+100}, {2,-100}, {2,+1000}, {2,-1000}
            };
            if( ( in_len != 2u ) || ( v1 > 8u ) )
            {
                printf(" \"*ax 00 VV\" bad step-delta index VV=%u (0..8)\n", (unsigned)v1);
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            audio_app_asrc_set_step_delta( q3pts[v1].mode, q3pts[v1].req );
        }
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x01u:
        if( ( in_len != 2u ) || ( v1 > 2u ) )
        {
            printf(" \"*ax 01 VV\" bad isolation mode VV=%u (0..2)\n", (unsigned)v1);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_meas_q11_isolate( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x02u:
        if( ( in_len != 4u ) || ( v3 > 3u ) )
        {
            printf(" \"*ax 02 VV WW XX\" bad args, amp index XX=%u (0..3)\n", (unsigned)v3);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_asrc_inject( v1, v2, v3 );
        printf(" *MEAS inject en=%u freq=%uHz amp=%.2e (added to servo error raw_corr; trace sel=9)\n",
               (unsigned)audio_app_asrc_get_inject(), (unsigned)v2,
               (double)audio_app_asrc_get_inject_amp() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x03u:
        if( ( in_len != 3u ) || ( v1 > 1u ) || ( v2 > 2u ) )
        {
            printf(" \"*ax 03 VV WW\" bad args, which VV=%u (0=KP 1=ALPHA) code WW=%u (0..2)\n",
                   (unsigned)v1, (unsigned)v2);
            msg->status = APP_CONSOLE_ERR_BAD_DATA;
            break;
        }
        audio_app_asrc_set_servo_mult( v1, v2 );
        printf(" *MEAS servo_mult KP=%.3f ALPHA=%.3f (which=%u code=%u)\n",
               (double)audio_app_asrc_get_kp_mult(), (double)audio_app_asrc_get_alpha_mult(),
               (unsigned)v1, (unsigned)v2 );
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        printf(" \"*ax\" unknown subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

// *af SS [..] (write only) : fill/phase servo config, subcode SS selects the sub-command.
//   SS=0x00 VV    : servo fill observation source, VV: 1=MA64 0=RAW (was *nt2C)
//   SS=0x01 VV    : continuous-fill estimator toggle, VV=1/0 (was *nt35)
//   SS=0x02 VV    : automatic phase-centering toggle, VV=1/0 (was *nt36)
//   SS=0x03 VV    : measured-producer-period phase toggle, VV=1/0 (was *nt38)
//   SS=0x04 VV    : silent-startup FIFO prime toggle, VV=1/0 (was *nt3F)
//   SS=0x05 VV    : ratio-lock fill pre-bias, VV=signed offset (was *nt41)
//   SS=0x06 VV WW : block-phase-aware lock offset, VV=enable, WW=center frames, 0=keep (was *nt42)
static void asrc_console_fill_write( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t  v1      = ( in_len > 1u ) ? msg->data[1] : 0xFFu;
    const uint8_t  v2      = ( in_len > 2u ) ? msg->data[2] : 0xFFu;

    msg->data_len = 0u;

    switch( subcode )
    {
    case 0x00u:
        if( in_len != 2u ) { printf(" \"*af 00 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_fill_use_ma( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x01u:
        if( in_len != 2u ) { printf(" \"*af 01 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_cfill_en( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x02u:
        if( in_len != 2u ) { printf(" \"*af 02 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_pc_auto( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x03u:
        if( in_len != 2u ) { printf(" \"*af 03 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_hf_en( v1 );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x04u:
        if( in_len != 2u ) { printf(" \"*af 04 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q55_prime_en( v1 );
        printf(" *MEAS q55_startup_prime=%u (effective next boot/restart)\n",
               (unsigned)audio_app_asrc_q55_get_prime_en() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x05u:
        if( in_len != 2u ) { printf(" \"*af 05 VV\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q57_lock_off( (int16_t)(int8_t)v1 );
        printf(" *MEAS q57_lock_off=%d (fill_start=%d; effective next ratio lock)\n",
               (int)audio_app_asrc_q57_get_lock_off(), (int)(256 - audio_app_asrc_q57_get_lock_off()) );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x06u:
        if( in_len != 3u ) { printf(" \"*af 06 VV WW\" bad args\n"); msg->status = APP_CONSOLE_ERR_BAD_DATA; break; }
        audio_app_asrc_q58_cfg( v1, (float)v2 );
        printf(" *MEAS q58_phase_aware=%u center=%.0f last_phase=%.1f last_off=%d\n",
               (unsigned)audio_app_asrc_q58_get_en(), (double)v2,
               (double)audio_app_asrc_q58_last_phase(), (int)audio_app_asrc_q58_last_off() );
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        printf(" \"*af\" unknown subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

// ?af (read, no payload) : cfill/phase-center/producer-period status print (was *nt37).
static void asrc_console_fill_read( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( in_len != 0u )
    {
        printf(" \"?af\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    audio_app_asrc_pc_status();
    msg->status = APP_CONSOLE_OK;
}

// *ag VV WW (write only) : arm the measurement-only control-variable trace (was *nt27).
//   VV = sel (0=applied step,1=corr_lpf,2=fill,3=feed-forward ratio,10=servo internal,11=frac-wrap;
//        other values accepted but ignored internally by the trace collector -- no range check here,
//        matches the old dispatcher's behaviour)
//   WW = decim (store every Nth A->B pull; 0 is treated as 1)
static void asrc_console_trace_arm( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  sel     = ( in_len > 0u ) ? msg->data[0] : 0xFFu;
    const uint8_t  decim_b = ( in_len > 1u ) ? msg->data[1] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 2u )
    {
        printf(" \"*ag VV WW\" bad args\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    {
        const uint16_t decim = ( decim_b == 0u ) ? 1u : (uint16_t)decim_b;
        audio_app_meas_trace_arm( sel, decim );
    }
    msg->status = APP_CONSOLE_OK;
}

// ?ag SS (read) : dump trace/log/probe, subcode SS selects the sub-command.
//   SS=0x00 : dump the control-variable trace, reuses *MEAS_BEGIN/_END framing (was *nt28)
//   SS=0x01 : raw wr/rd + elapsed-us probe (was *nt3E)
//   SS=0x02 : dump the early-boot fill/step/ratio log, t<5s (was *nt40)
static void asrc_console_trace_read( app_console_msg_t* msg )
{
    const uint16_t in_len  = msg->data_len;
    const uint8_t  subcode = ( in_len > 0u ) ? msg->data[0] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 1u )
    {
        printf(" \"?ag SS\" bad args\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    switch( subcode )
    {
    case 0x00u:
        audio_app_meas_trace_dump();
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x01u:
        printf(" *MEAS wrt wr=%lu rd=%lu us10=%lu\n",
               (unsigned long)audio_app_asrc_get_wr(),
               (unsigned long)audio_app_asrc_get_rd(),
               (unsigned long)audio_app_asrc_wrt_elapsed_us10() );
        msg->status = APP_CONSOLE_OK;
        break;

    case 0x02u:
        audio_app_asrc_q55log_dump();
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        printf(" \"?ag\" unknown subcode %u\n", (unsigned)subcode);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        break;
    }
}

// ?ak (read, no payload) : dump exact polyphase coefficient bits + CRC32 (was *nt2B).
static void asrc_console_coeff_read( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;

    msg->data_len = 0u;

    if( in_len != 0u )
    {
        printf(" \"?ak\" takes no payload\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    audio_app_asrc_dump_poly_bits();
    msg->status = APP_CONSOLE_OK;
}
#endif /* APP_ASRC_MEAS */

#if APP_ASRC_LOAD_TEST
// *al VV (write only) : set the interp load multiplier (was *nt26).
static void asrc_console_load( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;
    const uint8_t  mult   = ( in_len > 0u ) ? msg->data[0] : 0u;

    msg->data_len = 0u;

    if( ( in_len != 1u ) || ( mult < 1u ) || ( mult > ASRC_LOAD_MULT_MAX ) )
    {
        printf(" \"*al VV\" bad args VV=mult (1..%u)\n", (unsigned)ASRC_LOAD_MULT_MAX);
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    audio_app_asrc_set_load_mult( mult );
    printf(" \"*al\" interp mult=%u (~%u ch/dir)\n",
           (unsigned)audio_app_asrc_get_load_mult(),
           (unsigned)(audio_app_asrc_get_load_mult() * ASRC_CH) );
    msg->status = APP_CONSOLE_OK;
}
#endif /* APP_ASRC_LOAD_TEST */

#if APP_ASRC_MEAS_UART2_STREAM
// *ab VV (write only) : arm the long-coherent binary A->B stream on the UART2 DATA port (was *nt31).
//   VV = duration seconds in BCD digits-only 00..99 (never collides with a console hotkey).
static void asrc_console_stream_arm( app_console_msg_t* msg )
{
    const uint16_t in_len = msg->data_len;
    const uint8_t  bcd    = ( in_len > 0u ) ? msg->data[0] : 0xFFu;

    msg->data_len = 0u;

    if( in_len != 1u )
    {
        printf(" \"*ab VV\" bad args\n");
        msg->status = APP_CONSOLE_ERR_BAD_DATA;
        return;
    }

    {
        const uint8_t sec = (uint8_t)( ( bcd >> 4 ) * 10u + ( bcd & 0x0Fu ) );
        audio_app_meas_stream_arm( sec );
    }
    msg->status = APP_CONSOLE_OK;
}
#endif /* APP_ASRC_MEAS_UART2_STREAM */

// *ap<NN> (write) : declick pop test. Restart CODEC-B ONLY with declick strategy bitmask NN, while A
// keeps running as the loopback ADC recorder (patch B HPOUT -> A LINE-IN). Prints an objective pop
// metric of A's ADC: a quiescent baseline window, then the window spanning B's restart. Enables fully
// automated A/B comparison without a listener. Requires the independent B-codec-master topology.
// Measure one phase: reset+arm the pop meter, run `phase`, settle, disarm, return peak (24-bit) + dBFS.
static int32_t asrc_pop_measure_phase( bool (*phase)( uint8_t ), uint8_t mask,
                                       uint32_t settle_ms, float* out_db )
{
    asrc_audio_path_pop_meas_reset();
    asrc_audio_path_pop_meas_set_active( true );
    if( phase != NULL ) { (void)phase( mask ); }
    delay_ms( settle_ms );
    asrc_audio_path_pop_meas_set_active( false );
    int32_t peak = 0; uint64_t ss = 0u; uint32_t n = 0u;
    asrc_audio_path_pop_meas_read( &peak, &ss, &n );
    if( out_db ) { *out_db = ( peak > 0 ) ? 20.0f * log10f( (float)peak / 8388607.0f ) : -200.0f; }
    return peak;
}

// *aw II RR DDDD (write) : write WM8904 register RR = DDDD (16-bit) on I2C instance II (2=A,3=B).
// ?aw II RR       (read)  : read WM8904 register RR on instance II and print it.
// Raw poke for free interactive experiments (e.g. attacking the residual startup pop) without a rebuild.
static void asrc_console_reg( app_console_msg_t* msg )
{
    if( msg->kind == '*' )
    {
        if( msg->data_len < 4u )   // inst, reg, data_hi, data_lo
        {
            printf(" \"*aw\" needs II RR DDDD (inst, reg, 16-bit data)\n");
            msg->data_len = 0u; msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN; return;
        }
        const uint8_t  inst = msg->data[0];
        const uint8_t  reg  = msg->data[1];
        const uint16_t data = (uint16_t)( ( (uint16_t)msg->data[2] << 8 ) | msg->data[3] );
        wm8904_reg_write( inst, reg, data );
        printf(" *aw inst=%u reg=0x%02x <= 0x%04x (rb=0x%04x)\n",
               (unsigned)inst, (unsigned)reg, (unsigned)data, (unsigned)wm8904_reg_read( inst, reg ) );
        msg->data_len = 0u; msg->status = APP_CONSOLE_OK;
    }
    else if( msg->kind == '?' )
    {
        if( msg->data_len < 2u )   // inst, reg
        {
            printf(" \"?aw\" needs II RR (inst, reg)\n");
            msg->data_len = 0u; msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN; return;
        }
        const uint8_t inst = msg->data[0];
        const uint8_t reg  = msg->data[1];
        printf(" ?aw inst=%u reg=0x%02x => 0x%04x\n",
               (unsigned)inst, (unsigned)reg, (unsigned)wm8904_reg_read( inst, reg ) );
        msg->data_len = 0u; msg->status = APP_CONSOLE_OK;
    }
    else
    {
        msg->data_len = 0u; msg->status = APP_CONSOLE_ERR_UNSUPPORTED;
    }
}

#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_CH == 16u) && \
    ((ASRC_POLY_M == 28u) || (ASRC_POLY_M == 30u) || (ASRC_POLY_M == 32u))
// *az [VV] (write) : run the resampler kernel micro-benchmark VV*100 times (VV omitted or 00 = 200)
// and print the minimum per-call time. This is the DSP-load metric to compare across kernel edits:
// the telemetry pull=/cbA= peaks include ISR nesting and drift ~6% window to window.
static void asrc_console_kernel_bench( app_console_msg_t* msg )
{
    const uint32_t hundreds = ( msg->data_len >= 1u ) ? (uint32_t)msg->data[0] : 0u;
    msg->data_len = 0u;
    audio_app_asrc_kernel_bench( hundreds * 100u );   // 0 -> the callee's 200-trial default
    msg->status = APP_CONSOLE_OK;
}
#endif

// *aq [VV] (write) : measure the candidate front-stage FIR kernels in CPU cycles per MAC, VV*100
// trials (VV omitted or 00 = the callee's default).  Reports a slope, not a division, so the call
// and setup overhead cancel; also sweeps the modulo ring start address (M6).  Reads its own buffers
// only, so it is safe to run while audio is streaming -- and running it there is the check that the
// per-IPL DSP context really does keep the foreground and the IPL4 TDM ISRs out of each other.
#if ASRC_FIR_KERNEL_BENCH_AVAILABLE
static void asrc_console_fir_kernel_bench( app_console_msg_t* msg )
{
    const uint32_t hundreds = ( msg->data_len >= 1u ) ? (uint32_t)msg->data[0] : 0u;
    msg->data_len = 0u;
    asrc_fir_kernel_bench_run( hundreds * 100u );
    msg->status = APP_CONSOLE_OK;
}
#endif /* ASRC_FIR_KERNEL_BENCH_AVAILABLE */

static void asrc_console_pop_test( app_console_msg_t* msg )
{
    const uint8_t mask  = ( msg->data_len >= 1u ) ? msg->data[0] : 0x00u;
    if( ( mask != 0x00u ) && !audio_transport_declick_research_available() )
    {
        // A/B against the compiled-out strategies would silently measure the default twice.
        printf(" *ap mask=0x%02x not available -- declick research compiled out; use \"*ap00\"\n",
               (unsigned)mask );
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        return;
    }
    const uint8_t flags = ( msg->data_len >= 2u ) ? msg->data[1] : 0x00u;
    const bool    skip_shutdown = ( flags & 0x01u ) != 0u;   // *ap<mask>01: measure startup on the RUNNING
                                                             // B with NO explicit shutdown-first (does the
                                                             // pre-init discharge in wm8904_init_role()
                                                             // actually matter?)

    // Phase-split pop measurement (isolate SHUTDOWN vs STARTUP). Each phase runs in this blocking
    // main-loop context, so the manage/recovery tick can't restart the whole transport mid-window while
    // A's block ISR keeps observing the looped B output. Baseline first, then shutdown, then startup.
    float base_db = 0.0f, sd_db = -200.0f, su_db = 0.0f;
    const int32_t base = asrc_pop_measure_phase( NULL, mask, 200u, &base_db );   // quiescent floor
    const int32_t sd   = skip_shutdown ? 0
                       : asrc_pop_measure_phase( audio_transport_declick_b_shutdown_only, mask, 150u, &sd_db );
    const int32_t su   = asrc_pop_measure_phase( audio_transport_declick_b_startup_only,  mask, 150u, &su_db );

    // The B-only restart briefly stopped B's clock; B is now re-inited and clean. Clear the transient
    // frmerr/liveness so the manage loop does NOT fire a log-flooding full-transport auto-recovery
    // (which would also disrupt the quiet-log state during a measurement session).
    delay_ms( 100 );
    audio_transport_frmerr_reset();

    if( skip_shutdown )
    {
        printf(" *ap POP mask=0x%02x [no-shutdown-first] | base=%ld(%.1fdB) | STARTUP peak=%ld(%.1fdB)\n",
               (unsigned)mask, (long)base, (double)base_db, (long)su, (double)su_db );
    }
    else
    {
        printf(" *ap POP mask=0x%02x | base=%ld(%.1fdB) | SHUTDOWN peak=%ld(%.1fdB) | STARTUP peak=%ld(%.1fdB)\n",
               (unsigned)mask, (long)base, (double)base_db,
               (long)sd, (double)sd_db, (long)su, (double)su_db );
    }

    msg->data_len = 0u;
    msg->status   = APP_CONSOLE_OK;
}

void sonora_app_console_onmsg( app_console_msg_t* msg )
{
    if( !msg ) { return; }

    // This build's selected application owns module 'a' (ASRC). Anything else is not ours.
    if( msg->module != 'a' )
    {
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        return;
    }

    switch( msg->name )
    {
    case 'r':   // rate: "*ar CC RR" (write only). asrc_console_rate() reads the input payload
                // length from msg->data_len, then clears it for the response.
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_rate( msg );
        break;

    case 'p':   // declick pop test: "*ap<NN>" B-only restart(mask NN) + A-loopback pop metric
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_pop_test( msg );
        break;

    case 'q':   // front-stage FIR kernel bench: "*aq [VV]" (write only), VV*100 trials.
                // AK512 only -- see asrc_fir_kernel_bench.h for why the measurement cannot be made
                // on the other device, and note that configurations.xml excludes the .c there, so
                // this guard and that exclusion have to stay in step.
#if ASRC_FIR_KERNEL_BENCH_AVAILABLE
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_fir_kernel_bench( msg );
        break;
#else
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        break;
#endif

    case 'w':   // raw codec register: "*aw II RR DDDD" write / "?aw II RR" read
        asrc_console_reg( msg );
        break;

    case 'z':   // kernel micro-benchmark: "*az [VV]" (write only), VV*100 trials
#if (APP_ASRC_INTERP == ASRC_INTERP_POLY) && \
    (ASRC_POLY_METHOD == ASRC_POLY_STREAM8_PAIR) && \
    (ASRC_CH == 16u) && \
    ((ASRC_POLY_M == 28u) || (ASRC_POLY_M == 30u) || (ASRC_POLY_M == 32u))
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_kernel_bench( msg );
        break;
#else
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        break;
#endif

#if APP_ASRC_MEAS
    case 'c':   // capture: "*ac" arm / "?ac" dump
        if( ( msg->kind != '*' ) && ( msg->kind != '?' ) )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_capture( msg );
        break;

    case 't':   // tone/level: "*at SS [VV]" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_tone( msg );
        break;

    case 's':   // servo: "*as SS [..]" (write) / "?as" (read, no payload)
        if( msg->kind == '*' )      { asrc_console_servo_write( msg ); }
        else if( msg->kind == '?' ) { asrc_console_servo_read( msg ); }
        else
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        }
        break;

    case 'x':   // experiment/perturbation: "*ax SS [..]" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_experiment( msg );
        break;

    case 'f':   // fill/phase servo config: "*af SS [..]" (write) / "?af" (read, no payload)
        if( msg->kind == '*' )      { asrc_console_fill_write( msg ); }
        else if( msg->kind == '?' ) { asrc_console_fill_read( msg ); }
        else
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        }
        break;

    case 'g':   // trace/log: "*ag VV WW" (write, arm) / "?ag SS" (read)
        if( msg->kind == '*' )      { asrc_console_trace_arm( msg ); }
        else if( msg->kind == '?' ) { asrc_console_trace_read( msg ); }
        else
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
        }
        break;

    case 'k':   // coefficient dump: "?ak" (read only, no payload)
        if( msg->kind != '?' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_coeff_read( msg );
        break;
#endif /* APP_ASRC_MEAS */

#if APP_ASRC_LOAD_TEST
    case 'l':   // interp load multiplier: "*al VV" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_load( msg );
        break;
#endif /* APP_ASRC_LOAD_TEST */

#if APP_ASRC_MEAS_UART2_STREAM
    case 'b':   // binary A->B stream arm: "*ab VV" (write only)
        if( msg->kind != '*' )
        {
            msg->data_len = 0u;
            msg->status   = APP_CONSOLE_ERR_UNSUPPORTED;
            break;
        }
        asrc_console_stream_arm( msg );
        break;
#endif /* APP_ASRC_MEAS_UART2_STREAM */

    default:
        msg->data_len = 0u;
        msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}


// ASRC owns no raw single-key hotkeys: every keystroke is left to the shared console parser.
sonora_hotkey_result_t sonora_app_handle_hotkey( char c )
{
    (void)c;
    return SONORA_HOTKEY_IGNORED;
}
