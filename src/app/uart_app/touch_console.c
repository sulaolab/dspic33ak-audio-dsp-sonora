/* Provenance: written from DS70005591 ch.18 (ITC) and the DFP SFR header only.
 * No vendor touch-library source, header or binary was consulted.
 */

#include <xc.h>          /* ANSELA/TRISA/LATA for the ?kd electrode-state dump */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "app_console.h"
#include "touch_console.h"
/* One header. The acquisition peripheral is private to the touch HAL, and the
 * hardware this console pokes at is reached through its nora_touch_hw_* entry
 * points -- see the diagnostics section of nora_touch.h. */
#include "hal_touch/nora_touch.h"

/* Nothing below is compiled without touch -- touch_console.h supplies the
 * "unknown module" stub in that case, and it explains why. Guarding the body
 * here rather than excluding the file in configurations.xml keeps the device
 * fact in one place and keeps MPLAB X out of it. */
#if defined(ENA_OPEN_TOUCH_EXCLUSIVE)

/*===========================================================================
 * touch_console.c
 *
 * Bring-up console for the open capacitive-touch HAL (module 'k').
 *
 * This is deliberately a raw-count console. Bring-up's whole job is to show that
 * the ITC produces plausible, repeatable, finger-responsive numbers, and every
 * layer of interpretation added on top of that hides the evidence. The tuning
 * manual's opening procedure is driven entirely from here.
 *=========================================================================== */

/* CLKGEN6 is raised to 200 MHz by the project's clock boot code and the ITC
 * inherits it; this HAL owns no clock, so the frequency is stated rather than
 * queried.
 * If the boot clock ever changes, this constant is the one place to follow it —
 * and getting it wrong shows up as wrong timer counts in "?ki", not as silence. */
#define TOUCH_CONSOLE_CLOCK_HZ     (200000000UL)

/* Starting points, not tuned values. The bench procedure moves them and watches
 * the counts; the manual records what each does. */
#define TOUCH_CONSOLE_CHARGE_NS    (2000UL)
#define TOUCH_CONSOLE_BALANCE_NS   (1000UL)
#define TOUCH_CONSOLE_CVDCAP       (4u)
#define TOUCH_CONSOLE_ACC_COUNT    (4u)   /* 16 acquisitions summed per record   */
#define TOUCH_CONSOLE_MAX_RECORDS  (8u)

/* CVDANx analog-input numbers, as the board's pin table names them. Typed in at
 * the console: they are a fact about the board, not about this file. */
static uint8_t                  s_cvdan[TOUCH_CONSOLE_MAX_RECORDS];
static uint8_t                  s_record_count;
static uint8_t                  s_cvdcap = TOUCH_CONSOLE_CVDCAP;
static uint8_t                  s_acc_count = TOUCH_CONSOLE_ACC_COUNT;
/* Charge and balance time are runtime state, not compile-time constants, because
 * the CVDCAP sweep of 2026-08-13 came out monotonic and saturating with no peak:
 * capacitance matching is not the sensitivity lever on this board, so the lever
 * that is left is acquisition time — and a reflash per data point makes a sweep
 * cost an afternoon instead of a minute. */
static uint32_t                 s_charge_ns = TOUCH_CONSOLE_CHARGE_NS;
static uint32_t                 s_balance_ns = TOUCH_CONSOLE_BALANCE_NS;
static bool                     s_configured;

/* Is the detection layer running and therefore the owner of the list? There is
 * one scan list underneath both, and in an
 * ENA_OPEN_TOUCH_EXCLUSIVE build nora_touch is scanning it continuously. Nothing
 * here may reprogram it behind that layer's back -- that is the same "two owners
 * on one peripheral" mistake the vendor library made visible, and it would be
 * just as silent. */
static bool touch_console_nora_touch_owns_list( void )
{
    nora_touch_status_t st;

    nora_touch_get_status( &st );
    return st.initialized;
}

/* Take the acquisition settings from whoever currently owns the list, so a
 * sweep command modifies what is actually running rather than this file's stale
 * copy of it (nora_touch acquires at 2^8, this console's own default is 2^4). */
static void touch_console_sync_from_owner( void )
{
    if( touch_console_nora_touch_owns_list() )
    {
        nora_touch_get_acquisition( &s_charge_ns, &s_balance_ns,
                                    &s_cvdcap, &s_acc_count );
    }
}

/* Apply the record list currently held here. Kept separate so *kc and *ka can
 * re-apply without the caller re-typing the pin list. */
/* Why the last apply failed. The two paths below report through different
 * functions -- only the diagnostics one keeps text -- so the reason is captured
 * here rather than left to nora_touch_hw_last_error(), which on the owned-list
 * path would still be holding the previous command's answer. */
static const char *s_apply_error = "ok";

static bool touch_console_apply( void )
{
    /* Delegated rather than duplicated: nora_touch re-inits its own list with its
     * own electrodes and then re-seeds baselines and peaks, which is exactly what
     * a sweep point needs and what this function cannot do from outside. */
    if( touch_console_nora_touch_owns_list() )
    {
        if( nora_touch_set_acquisition( s_charge_ns, s_balance_ns,
                                        s_cvdcap, s_acc_count ) )
        {
            s_apply_error = "ok";
            return true;
        }
        /* set_acquisition() refuses one combination for one reason: a requested
         * time that does not fit its 8-bit timer. */
        s_apply_error = "refused (a requested time does not fit its timer)";
        return false;
    }

    if( nora_touch_hw_configure( TOUCH_CONSOLE_CLOCK_HZ,
                                 s_cvdan, s_record_count,
                                 s_charge_ns, s_balance_ns,
                                 s_cvdcap, s_acc_count ) )
    {
        s_apply_error = "ok";
        return true;
    }
    s_apply_error = nora_touch_hw_last_error();
    return false;
}

static void touch_console_print_counts( void )
{
    int32_t results[TOUCH_CONSOLE_MAX_RECORDS];
    uint8_t i;

    if( !nora_touch_hw_read_raw( results, TOUCH_CONSOLE_MAX_RECORDS ) )
    {
        printf( " ?kr read failed: %s\n", nora_touch_hw_last_error() );
        return;
    }

    for( i = 0u; i < s_record_count; i++ )
    {
        printf( " rec %u  CVDAN%-3u  %ld\n",
                (unsigned)i,
                (unsigned)s_cvdan[i],
                (long)results[i] );
    }
}

static void touch_console_print_info( void )
{
    nora_touch_hw_info_t info;

    if( !nora_touch_hw_get_info( &info ) )
    {
        printf( " ?ki failed: %s\n", nora_touch_hw_last_error() );
        return;
    }

    printf( " ITC list 0: %s, hardware %s, %s\n",
            info.configured ? "configured" : "not configured",
            info.hardware_ready ? "ready (DRDY)" : "NOT ready",
            info.list_busy ? "busy" : "idle" );
    printf( "   records %u, next %u, scans %lu\n",
            (unsigned)info.record_count,
            (unsigned)info.next_record,
            (unsigned long)info.scans_completed );
    /* The converted counts matter more than the requested times: this is where
     * a request that rounded badly becomes visible. */
    printf( "   clock %lu Hz, charge %u TAD (%lu ns asked), balance %u TAD (%lu ns asked)\n",
            (unsigned long)info.clock_hz,
            (unsigned)info.charge_counts,
            (unsigned long)s_charge_ns,
            (unsigned)info.balance_counts,
            (unsigned long)s_balance_ns );
    printf( "   CVDCAP code %u, accumulation 2^%u, test injection %s\n",
            (unsigned)info.cvdcap,
            (unsigned)info.acc_count,
            info.test_inject_active ? "ON" : "off" );
    printf( "   last status: %s\n", info.last_status );
}

/* Raw register dump. The one command that distinguishes "the peripheral did not
 * accept the configuration" from "the electrode is dead", which is the fork every
 * bring-up problem so far has turned on. */
static void touch_console_print_regs( void )
{
    const char *name;
    uint32_t    value;
    uint8_t     i;

    for( i = 0u; i < 255u; i++ )
    {
        if( !nora_touch_hw_debug_reg( i, &name, &value ) )
        {
            break;
        }
        printf( " %-11s %08lX\n", name, (unsigned long)value );
    }

    /* Port A as well, because the electrode pins' idle state is part of the
     * measurement condition and NOT visible in any ITC register. DS70005591C
     * p.1487: a CVD pin must be ANSELx = 1 / TRISx = 0, and the ITC returns pin
     * control to TRIS/LAT whenever the pin is idle between scans -- so a pad left
     * at the POR default TRISx = 1 floats, and the data sheet warns that
     * robustness then "degrade[s] significantly even with a light noise". That is
     * exactly the bug found on 2026-08-15 ([internal] itc_hardware_reference.md §12.2), and
     * it was invisible for months because nothing printed these three words.
     *
     * The three pads live on port A: RA1 (CVDAN1), RA8 (CVDAN8), RA10 (CVDAN10),
     * i.e. check bits 1, 8 and 10 -> mask 0x00000502. Want:
     *   ANSELA & 0x502 == 0x502   (analog)
     *   TRISA  & 0x502 == 0        (outputs)
     *   LATA   & 0x502 == 0        (driven low when idle)
     * Bits 11, 9 and 2 are the redundant channels and must stay TRIS = 1 -- see
     * the hazard note in §12.2 before "fixing" them. */
    printf( " ANSELA      %08lX\n", (unsigned long)ANSELA );
    printf( " TRISA       %08lX\n", (unsigned long)TRISA  );
    printf( " LATA        %08lX\n", (unsigned long)LATA   );
    printf( " (pads RA1/RA8/RA10 = mask 00000502: want ANSEL 502, TRIS 0, LAT 0)\n" );
}

void touch_console_onmsg( app_console_msg_t* msg )
{
    bool     ok = false;
    uint16_t payload_len;
    uint8_t  i;

    if( !msg ) { return; }

    /* The incoming payload length has to be read before the reply length is
     * written into the same field. */
    payload_len   = msg->data_len;
    msg->data_len = 0u;

    switch( msg->name )
    {
    case 'i':   /* ?ki : print state.  *ki <cvdan>... : configure list 0 */
        if( msg->kind == '?' )
        {
            touch_console_print_info();
            msg->status = APP_CONSOLE_OK;
            break;
        }
        if( touch_console_nora_touch_owns_list() )
        {
            printf( " *ki refused: nora_touch is scanning list 0."
                    " Use *kc/*ka/*kg/*kb to sweep it, ?ko to read it\n" );
            msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
            break;
        }
        if( (payload_len == 0u) || (payload_len > TOUCH_CONSOLE_MAX_RECORDS) )
        {
            printf( " *ki wants 1..%u CVDANx numbers, one payload byte each\n",
                    (unsigned)TOUCH_CONSOLE_MAX_RECORDS );
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        /* No electrode map is baked in. The CVDANx numbers are a fact about the
         * board and come from its schematic, so they are typed in here until a
         * board description exists to hold them. */
        for( i = 0u; i < (uint8_t)payload_len; i++ )
        {
            s_cvdan[i] = msg->data[i];
        }
        s_record_count = (uint8_t)payload_len;

        ok = touch_console_apply();
        s_configured = ok;
        printf( " *ki %u record(s): %s\n", (unsigned)s_record_count,
                ok ? "ok" : s_apply_error );
        msg->status = s_configured ? APP_CONSOLE_OK
                                   : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'd':   /* ?kd : raw register dump */
        touch_console_print_regs();
        msg->status = APP_CONSOLE_OK;
        break;

    case 's':   /* *ks : one scan, no printing */
    case 'r':   /* ?kr : scan and print raw counts */
        if( touch_console_nora_touch_owns_list() )
        {
            /* A scan started here would race the detection layer's pump for
             * ACCDONE, and whichever read arrived first would take results the
             * other one is about to use as data. */
            printf( " *k%c refused: nora_touch is scanning list 0, use ?ko\n",
                    (char)msg->name );
            msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
            break;
        }
        if( msg->name == 'r' )
        {
            if( !s_configured )
            {
                printf( " ?kr: run *ki <cvdan>... first\n" );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }
            if( !nora_touch_hw_scan_once() )
            {
                printf( " ?kr scan %s\n", nora_touch_hw_last_error() );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }
            touch_console_print_counts();
            msg->status = APP_CONSOLE_OK;
            break;
        }
        ok = nora_touch_hw_scan_once();
        printf( " *ks %s\n", ok ? "ok" : nora_touch_hw_last_error() );
        msg->status = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 't':   /* *kt <hi> <lo> : test injection on */
        if( payload_len != 2u )
        {
            printf( " *kt wants a 16-bit value, e.g. *kt0800\n" );
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        {
            uint16_t value = (uint16_t)(((uint16_t)msg->data[0] << 8u) |
                                         (uint16_t)msg->data[1]);
            ok = nora_touch_hw_test_inject( true, value );
            printf( " *kt inject %u: %s\n", (unsigned)value,
                    ok ? "ok" : nora_touch_hw_last_error() );
        }
        msg->status = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'u':   /* *ku : test injection off */
        ok = nora_touch_hw_test_inject( false, 0u );
        printf( " *ku inject off: %s\n", ok ? "ok" : nora_touch_hw_last_error() );
        msg->status = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'c':   /* *kc <code> : CVDCAP code, then re-apply */
    case 'a':   /* *ka <n>    : accumulation depth 2^n, then re-apply */
        if( payload_len != 1u )
        {
            printf( " *k%c wants one value\n", (char)msg->name );
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        if( !s_configured && !touch_console_nora_touch_owns_list() )
        {
            printf( " *k%c: run *ki <cvdan>... first\n", (char)msg->name );
            msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
            break;
        }
        touch_console_sync_from_owner();
        if( msg->name == 'c' )
        {
            if( msg->data[0] > NORA_TOUCH_HW_CVDCAP_MAX )
            {
                printf( " *kc: CVDCAP code is 0..%u\n",
                        (unsigned)NORA_TOUCH_HW_CVDCAP_MAX );
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            s_cvdcap = msg->data[0];
        }
        else
        {
            if( msg->data[0] > NORA_TOUCH_HW_ACC_MAX )
            {
                printf( " *ka: depth exponent is 0..%u\n",
                        (unsigned)NORA_TOUCH_HW_ACC_MAX );
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            s_acc_count = msg->data[0];
        }
        ok = touch_console_apply();
        printf( " *k%c applied (CVDCAP %u, acc 2^%u): %s\n",
                (char)msg->name, (unsigned)s_cvdcap, (unsigned)s_acc_count,
                ok ? "ok" : s_apply_error );
        msg->status = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'o':   /* ?ko : the detection layer's per-key view */
        {
            nora_touch_status_t    st;
            nora_touch_key_state_t ks;
            uint8_t                key;
            int32_t                press_thr;
            int32_t                release_thr;

            nora_touch_get_status( &st );
            if( !st.initialized )
            {
                /* Not a failure to explain away: the detection layer only exists
                 * in a build with touch detection enabled (sonora:
                 * -Define ENA_OPEN_TOUCH_EXCLUSIVE=1), and
                 * *ki reprogramming the same list is what would break it. */
                printf( " ?ko: nora_touch not running"
                        " (touch detection is not running in this build)\n" );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }
            nora_touch_get_thresholds( &press_thr, &release_thr );
            /* implausible is reported next to rejected rather than folded into it:
             * a rejected scan means the ITC would not run, an implausible sample
             * means it ran and gave an impossible answer, and the two point at
             * different things to go and look at. */
            printf( " nora_touch: %u key(s), %lu scans, %lu rejected,"
                    " %lu implausible, press %ld / release %ld\n",
                    (unsigned)st.key_count,
                    (unsigned long)st.scans,
                    (unsigned long)st.rejected_scans,
                    (unsigned long)st.implausible_samples,
                    (long)press_thr, (long)release_thr );
            for( key = 0u; key < st.key_count; key++ )
            {
                if( !nora_touch_get_key_state( key, &ks ) ) { continue; }
                /* mag is printed right after delta because mag is the number the
                 * thresholds are compared against; delta is kept only so the raw
                 * waveform stays visible. peak/trough stay signed -- trough is
                 * still the best single view of the wander with nobody there. */
                printf( "   key %u  CVDAN%-3u raw %ld  base %ld  delta %ld  mag %ld"
                        "  peak %ld  trough %ld  n %u  %s\n",
                        (unsigned)key, (unsigned)ks.cvdan,
                        (long)ks.raw, (long)ks.baseline, (long)ks.delta,
                        (long)ks.mag,
                        (long)ks.peak, (long)ks.trough,
                        (unsigned)ks.presses,
                        ks.pressed ? "PRESSED" : "-" );
            }
        }
        msg->status = APP_CONSOLE_OK;
        break;

    case 'g':   /* *kg <hi> <lo> : charge time in ns, then re-apply  */
    case 'b':   /* *kb <hi> <lo> : balance time in ns, then re-apply */
        if( payload_len != 2u )
        {
            printf( " *k%c wants a 16-bit nanosecond value, e.g. *k%c07D0 = 2000 ns\n",
                    (char)msg->name, (char)msg->name );
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        if( !s_configured && !touch_console_nora_touch_owns_list() )
        {
            printf( " *k%c: run *ki <cvdan>... first\n", (char)msg->name );
            msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
            break;
        }
        touch_console_sync_from_owner();
        {
            uint32_t saved;
            uint32_t ns = (uint32_t)(((uint16_t)msg->data[0] << 8u) |
                                      (uint16_t)msg->data[1]);
            /* TMRA/TMRB are 8-bit, so most of the 16-bit range does not fit and
             * the acquisition layer refuses it. Keep the old value on refusal: a
             * sweep that silently left the list configured for something other
             * than what was typed would attribute the next reading to the wrong
             * time. */
            saved = (msg->name == 'g') ? s_charge_ns : s_balance_ns;
            if( msg->name == 'g' ) { s_charge_ns = ns; } else { s_balance_ns = ns; }

            ok = touch_console_apply();
            if( !ok )
            {
                const char *why = s_apply_error;

                if( msg->name == 'g' ) { s_charge_ns = saved; } else { s_balance_ns = saved; }
                /* The restore re-applies, which overwrites s_apply_error with the
                 * restore's own (successful) outcome -- so the reason for the
                 * refusal is kept before that happens. */
                (void)touch_console_apply();
                s_apply_error = why;
            }
            printf( " *k%c charge %lu ns / balance %lu ns: %s\n",
                    (char)msg->name,
                    (unsigned long)s_charge_ns,
                    (unsigned long)s_balance_ns,
                    ok ? "ok" : s_apply_error );
        }
        msg->status = ok ? APP_CONSOLE_OK : APP_CONSOLE_ERR_OPERATION_FAILED;
        break;

    case 'l':   /* ?kl : what each pad has learned.  *kl : forget it and relearn */
        {
            nora_touch_status_t      st;
            nora_touch_calibration_t cal;
            uint8_t                  key;

            nora_touch_get_status( &st );
            if( !st.initialized )
            {
                printf( " ?kl: nora_touch not running"
                        " (touch detection is not running in this build)\n" );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }

            if( msg->kind != '?' )
            {
                if( !nora_touch_calibrate() )
                {
                    printf( " *kl refused: learning is switched off"
                            " (learn_presses = 0)\n" );
                    msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                    break;
                }
                /* Say what the operator has to do, not just what was cleared.
                 * Nothing is measured by this command -- the pads relearn from
                 * being touched, so the taps are the operator's part of it. */
                printf( " *kl cleared -- tap each pad a few times, then ?kl\n" );
                msg->status = APP_CONSOLE_OK;
                break;
            }

            for( key = 0u; key < st.key_count; key++ )
            {
                if( !nora_touch_get_calibration( key, &cal ) ) { continue; }
                /* samples/needed before the thresholds, because a pad still short
                 * of its quota is not misbehaving, it is unfinished -- and that is
                 * the difference between "this pad is insensitive" and "tap it
                 * three more times". idle sits beside press as the margin. */
                printf( "   key %u  %u/%u presses  idle %ld"
                        "  press %ld  release %ld  %s\n",
                        (unsigned)key, (unsigned)cal.samples, (unsigned)cal.needed,
                        (long)cal.idle_ref,
                        (long)cal.press_threshold, (long)cal.release_threshold,
                        cal.pinned     ? "PINNED (set by the integrator)" :
                        cal.cold_gate  ? "COLD (strict until the first press)" :
                        cal.calibrated ? "learned" : "learning" );
            }
        }
        msg->status = APP_CONSOLE_OK;
        break;

    case 'v':   /* *kv [<hi> <lo>] : arm the scan trace.  ?kv : dump it */
        {
            nora_touch_status_t st;
            uint16_t            n;
            uint16_t            i;
            uint8_t             key;

            nora_touch_get_status( &st );
            if( !st.initialized )
            {
                printf( " ?kv: nora_touch not running"
                        " (touch detection is not running in this build)\n" );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }

            if( msg->kind != '?' )
            {
                int32_t trigger = 0;

                if( payload_len == 2u )
                {
                    trigger = (int32_t)(((uint32_t)msg->data[0] << 8) |
                                         (uint32_t)msg->data[1]);
                }
                nora_touch_trace_arm( trigger );
                /* The instruction is the command: an armed trace records nothing
                 * until a pad moves, so the operator's touch is the trigger and
                 * there is no window to hit. */
                printf( " *kv armed (trigger %ld counts) -- touch a pad, then"
                        " ?kv\n", (long)((trigger > 0) ? trigger : 800) );
                msg->status = APP_CONSOLE_OK;
                break;
            }

            n = nora_touch_trace_count();
            if( n == 0u )
            {
                printf( " ?kv: nothing captured (arm with *kv, then touch a"
                        " pad)\n" );
                msg->status = APP_CONSOLE_OK;
                break;
            }

            printf( " nora_touch trace: %u scan(s), %s, delta per key\n",
                    (unsigned)n,
                    nora_touch_trace_ready() ? "full" : "still filling" );
            for( i = 0u; i < n; i++ )
            {
                printf( "   %3u", (unsigned)i );
                for( key = 0u; key < st.key_count; key++ )
                {
                    int32_t d = 0;
                    (void)nora_touch_trace_get( key, i, &d );
                    printf( "  %6ld", (long)d );
                }
                printf( "\n" );
            }
        }
        msg->status = APP_CONSOLE_OK;
        break;

    case 'z':   /* *kz : clear peak/trough and the press count on every key */
        {
            nora_touch_status_t st;

            nora_touch_get_status( &st );
            if( !st.initialized )
            {
                printf( " *kz: nora_touch not running"
                        " (touch detection is not running in this build)\n" );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }
            nora_touch_reset_peaks();
            printf( " *kz peaks and press counts cleared -- tap a counted number of times, then ?ko\n" );
        }
        msg->status = APP_CONSOLE_OK;
        break;

    /* The value is a *magnitude* -- the mean of |raw - baseline| over the last
     * few scans -- not a signed delta. A number from before that change is
     * roughly 3x too large; see nora_touch.h. */
    case 'p':   /* *kp <hi> <lo> : press threshold, magnitude counts   */
    case 'q':   /* *kq <hi> <lo> : release threshold, magnitude counts  */
        if( payload_len != 2u )
        {
            printf( " *k%c wants a 16-bit magnitude count, e.g. *k%c02BC = 700\n",
                    (char)msg->name, (char)msg->name );
            msg->status = APP_CONSOLE_ERR_BAD_PARM_LEN;
            break;
        }
        {
            nora_touch_status_t st;
            int32_t             press_thr;
            int32_t             release_thr;
            int32_t             value = (int32_t)(uint32_t)
                                        (((uint16_t)msg->data[0] << 8u) |
                                          (uint16_t)msg->data[1]);

            nora_touch_get_status( &st );
            if( !st.initialized )
            {
                printf( " *k%c: nora_touch not running"
                        " (touch detection is not running in this build)\n",
                        (char)msg->name );
                msg->status = APP_CONSOLE_ERR_OPERATION_FAILED;
                break;
            }
            /* Read the pair, change one, and hand both back, so the hysteresis
             * check in nora_touch_set_thresholds() sees the combination that
             * would actually be in force rather than one number in isolation. */
            nora_touch_get_thresholds( &press_thr, &release_thr );
            if( msg->name == 'p' ) { press_thr = value; }
            else                   { release_thr = value; }

            if( !nora_touch_set_thresholds( press_thr, release_thr ) )
            {
                printf( " *k%c refused: need 0 < release < press"
                        " (asked press %ld / release %ld)\n",
                        (char)msg->name, (long)press_thr, (long)release_thr );
                msg->status = APP_CONSOLE_ERR_BAD_DATA;
                break;
            }
            /* Peaks belong to a threshold setting: leaving them would let a
             * measurement taken before the change look like evidence for it. */
            nora_touch_reset_peaks();
            printf( " *k%c press %ld / release %ld, peaks cleared\n",
                    (char)msg->name, (long)press_thr, (long)release_thr );
        }
        msg->status = APP_CONSOLE_OK;
        break;

    default:
        msg->status = APP_CONSOLE_ERR_NOT_FOUND;
        break;
    }
}

#endif /* defined(ENA_OPEN_TOUCH_EXCLUSIVE) */
