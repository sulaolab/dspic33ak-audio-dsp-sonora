#ifndef SONORA_TOUCH_CONSOLE_H
#define SONORA_TOUCH_CONSOLE_H

/* Provenance: written from DS70005591 ch.18 (ITC) and the DFP SFR header only.
 * No vendor touch-library source, header or binary was consulted.
 */

#include "app_console.h"
#include "app_specific_config_defs.h"   /* ENA_OPEN_TOUCH_EXCLUSIVE */

/* Open capacitive-touch bring-up console (module 'k'). Raw ITC counts and
 * nothing above them: no baseline, no threshold and no key decision, all of which
 * belong to nora_touch (src/hal_touch) and are read here rather than computed.
 * Every number this prints is what the hardware produced, which is what the
 * tuning manual's procedures need.
 *
 *   *k i <cvdan> [<cvdan> ...]  configure list 0, one record per CVDANx number
 *                               given as payload bytes (no pin list is baked
 *                               in: they come from the board schematic)
 *   *k s                        one software-triggered scan
 *   ?k r                        scan and print the raw signed counts
 *   ?k i                        list state: DRDY/BUSY, converted timer counts,
 *                               CVDCAP code, accumulation depth, scan count
 *   *k t <hi> <lo>              enable test injection with that 16-bit value
 *   *k u                        disable test injection
 *   ?k d                        raw register dump (ITC + the ADC 5 registers it
 *                               depends on). Note it reads ITCRESx, which clears
 *                               ACCDONE — dumping between a scan and ?kr changes
 *                               the answer.
 *   *k c <code>                 set the CVDCAP code and re-apply the list
 *   *k a <n>                    set the accumulation depth (2^n) and re-apply
 *   ?k o                        detection layer (nora_touch) per-key view:
 *                               raw / baseline / delta / peak / trough /
 *                               pressed, plus the scan and rejected-scan counts
 *                               and the thresholds in force. Only present in an
 *                               ENA_OPEN_TOUCH_EXCLUSIVE build; note that *ki
 *                               reprograms the list it is scanning.
 *   *k z                        clear peak/trough on every key. peak/trough are
 *                               tracked whatever the threshold is, which is the
 *                               only way to see a touch that failed to reach it:
 *                               a miss prints no event line at all.
 * *   ?k l                        per-pad calibration: each pad's measured noise
 *                               tail and the thresholds derived from it, or
 *                               PINNED where the integrator set the pair by hand.
 *                               Two boards' individual characteristics are this
 *                               table, side by side.
 *   *k l                        measure it again. Hands off the pads for ~1 s: a
 *                               window with a finger in it is discarded, and
 *                               enough discards in a row keep the old values.
   *k p <hi> <lo>              press threshold, in counts
 *   *k q <hi> <lo>              release threshold, in counts (must stay below
 *                               press: equal values remove the hysteresis and
 *                               the key chatters). Either command clears the
 *                               peaks, since a peak belongs to one setting.
 *   *k g <hi> <lo>              set the charge time in ns and re-apply
 *   *k b <hi> <lo>              set the balance time in ns and re-apply
 *                               (both 16-bit ns; TMRA/TMRB are 8-bit TAD, so a
 *                               value that does not fit is refused with
 *                               "a requested time does not fit its timer" and
 *                               the previous value is put back)
 */
#if defined(ENA_OPEN_TOUCH_EXCLUSIVE)

void touch_console_onmsg( app_console_msg_t* msg );

#else

/* No touch in this build. On the dsPIC33AK128MC106 that is a device fact, not a
 * preference: the part has no ITC and no ADC 5, so nora_itc_dspic33ak.c compiles
 * to its NORA_ITC_ERR_UNSUPPORTED stubs and every verb above would print zeros
 * from a peripheral that is not there. app_specific_config_defs.h (2.1) already
 * decides this -- ENA_OPEN_TOUCH_EXCLUSIVE is defined for the AK512 Classic build
 * only -- and main.c and board/devices/button_led.c already follow it. This stub
 * makes the console dispatcher follow it too, and gives the same answer it gives
 * for any unknown module. Same shape as resident_de_app_console.h, for the same
 * reason: the test lives here, so no caller needs an #if.
 *
 * It is also what keeps the module out of the image. touch_console.c is compiled
 * for every configuration, so the unconditional 'case k' in app_debug.c was the
 * only reference keeping touch_console.c and, through it, nora_touch.c linked --
 * 9.0 KiB of program Flash on a part that cannot use one byte of it. With the
 * reference gone --gc-sections drops both. */
static inline void touch_console_onmsg( app_console_msg_t* msg )
{
    if( msg == NULL ) { return; }
    msg->data_len = 0u;
    msg->status   = APP_CONSOLE_ERR_NOT_FOUND;
}

#endif /* defined(ENA_OPEN_TOUCH_EXCLUSIVE) */

#endif /* SONORA_TOUCH_CONSOLE_H */
