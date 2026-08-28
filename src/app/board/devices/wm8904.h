// Sonora board WM8904 codec device support.
#ifndef _WM8904_H_
#define	_WM8904_H_


//===========================================================
// INCLUDES
//===========================================================

#include <stdbool.h>
#include <stdint.h>
//===========================================================
// Definition
//===========================================================

/*
 * Configuration is one path: the rate table (whose rows are 8, 11.025, 12, 16,
 * 22.05, 24, 32, 44.1, 48 and 96 kHz -- 96 kHz is a row like any other, and there
 * is no 88.2 kHz row) plus an explicit wm8904_role_t. Rate and converter role are
 * independent axes, which is what makes a rate-only change -- e.g. a runtime
 * 96k -> 48k switch on leg B -- expressible at all.
 *
 * This replaced a split of wm8904_config() (48 kHz family) plus dedicated
 * wm8904_config_96k_{adc,dac}_only() / wm8904_init_96k_* entry points. That split
 * was carried alongside this path behind a temporary WM8904_USE_UNIFIED_CONFIG
 * A/B switch until the unified path had been hardware-verified on every
 * configuration that exercises the codec (Classic 96 K, ASRC 96 kHz A->B,
 * ASRC 48 kHz BIDIR, and the runtime `*ar 1 8` rate change), then deleted.
 * [internal] wm8904_unified_rate_role.md records the evidence, the two
 * silent-revert defects the split caused, and the one behavioural difference
 * unification introduces (§7).
 */


//===========================================================
// Enum & Struct typedef
//===========================================================

/*
 * Declick research one-shot restart-strategy bitmask.
 *
 * See [internal] manual_wm8904_mute_restart_declick.md. A mask is armed with
 * wm8904_set_pending_declick() immediately before a mute-bounded restart (see
 * audio_transport_restart_declick()); the codec (re)configure path reads it while it
 * runs. 0 == baseline == the unchanged shipping behavior, so shipping and the auto-
 * recovery restart path (which never arm a mask) are unaffected. Bits may be OR-combined.
 */
typedef enum {
    WM8904_DECLICK_NONE            = 0x00u,  // shipping default: WSEQ shutdown + manual startup (see below)
    WM8904_DECLICK_ORDERED_SHUTDN  = 0x01u,  // C: Table 42-ordered HP disable during shutdown
    WM8904_DECLICK_WARM_SERVO      = 0x02u,  // B: skip R0 SW-reset + DCS_TRIG_DAC_WR retained-servo restore
    WM8904_DECLICK_SOFT_UNMUTE     = 0x04u,  // D: ramped HPOUT analog unmute
    WM8904_DECLICK_WRITE_SEQUENCER = 0x08u,  // A: force vendor WSEQ shutdown (now also the default; redundant)
    WM8904_DECLICK_SOFT_SHUTDOWN   = 0x10u,  // E: ramp HPOUT gain DOWN before mute/shutdown (measured no-op)
    WM8904_DECLICK_WSEQ_STARTUP    = 0x20u,  // F: vendor WSEQ startup for the analog bring-up (startup pop)
    WM8904_DECLICK_LEGACY_QUENCH   = 0x40u,  // regression: force the OLD quench shutdown (pre-declick default)
} wm8904_declick_mask_t;

/*
 * SHUTDOWN discharge policy (decided by measurement -- see [internal] manual_wm8904_mute_restart_declick.md):
 *   default (mask NONE) = vendor Control Write Sequencer shutdown (Table 89), which does the full ordered
 *   VMID/charge-pump/bias power-down and suppresses the shutdown pop (~47 dB vs the old quench) and, by
 *   leaving the chip fully discharged, also cuts the following startup pop. WSEQ needs SYSCLK; it falls back
 *   to the quench on timeout. LEGACY_QUENCH forces the old behavior for A/B regression.
 */


//===========================================================
// Variables
//===========================================================


//===========================================================
// Function Prototype
//===========================================================

/*
 * Converter role -- which halves of the codec are powered and clocked.
 *
 * This is an axis of its own, independent of the sample rate.  It reads as a
 * "96 kHz thing" only because the hardware FORCES a choice at fs >= 88.2 kHz (the
 * WM8904 cannot run its ADC and DAC simultaneously there), and 96 kHz is the only
 * rate this driver offers on that side of the boundary.  ADC-only / DAC-only are
 * equally meaningful at any rate, though -- e.g. a
 * one-way path that has no use for the unused converter's power or noise.
 *
 * Treating role and rate as one axis is what previously required separate
 * wm8904_init_96k_* entry points, and why a rate-only change could not be
 * expressed without also re-deciding the role.
 */
typedef enum
{
    WM8904_ROLE_ADC_DAC  = 0,   /* capture + playback (the part forbids it >= 88.2 kHz,
                                 * i.e. at 96 kHz, the only such rate offered)   */
    WM8904_ROLE_ADC_ONLY = 1,   /* capture only; DAC/HP/charge-pump left down     */
    WM8904_ROLE_DAC_ONLY = 2,   /* playback only; input PGAs/ADC left down        */
} wm8904_role_t;

/*
 * Configure the codec and verify all I2C writes/readbacks.
 *
 * wm8904_init() keeps its historical signature and means ROLE_ADC_DAC at the
 * instance's currently selected rate, so existing call sites are unchanged.
 * wm8904_init_role() is the full form.  Requesting ADC_DAC at or above the part's
 * 88.2 kHz boundary -- in practice at 96 kHz -- is rejected (returns false), so
 * that hardware constraint is enforced in one place instead of being implied by
 * which function the caller picked.
 */
extern bool wm8904_init( uint8_t inst, bool master_cfg );
extern bool wm8904_init_role( uint8_t inst, bool master_cfg, wm8904_role_t role );
extern void wm8904_shutdown( uint8_t inst );
extern void wm8904_dump_reg( uint8_t inst );
extern void wm8904_set_analog_output_mute( uint8_t inst, bool mute );
/* Apply the HPOUT analog mute and read both channel registers back.  The
 * normal lifecycle helper is best effort; pre-flash code needs a verdict. */
extern bool wm8904_set_analog_output_mute_verified( uint8_t inst, bool mute );

/* Raw register access for interactive experiments (declick research console *aw/?aw). `inst` is the
 * I2C instance (2=A, 3=B). Thin public wrappers over the internal verified write / read. */
extern void     wm8904_reg_write( uint8_t inst, uint8_t reg, uint16_t data );
extern uint16_t wm8904_reg_read( uint8_t inst, uint8_t reg );

/*
 * (Phase B) Select the sample rate applied to `inst` on its NEXT (re)configuration.
 * Stores the request only; the caller must re-init the codec (e.g. audio_transport_restart())
 * for it to take effect. Returns false for an instance out of range or an fs outside the
 * supported standard menu: 8/11.025/12/16/22.05/24/32/44.1/48 kHz. The 44.1 kHz family
 * uses the codec FLL; the 48 kHz family is FLL-less. Default (never called) is 48 kHz.
 */
extern bool wm8904_set_rate_hz( uint8_t inst, uint32_t fs_hz );

/* Currently-selected sample rate for `inst` (default 48000 until wm8904_set_rate_hz changes it). */
extern uint32_t wm8904_get_rate_hz( uint8_t inst );

/*
 * Declick research (one-shot): arm/read the restart-strategy bitmask consumed by the NEXT codec
 * (re)configure. `mask` is a bitwise-OR of wm8904_declick_mask_t. Set to WM8904_DECLICK_NONE (0)
 * for baseline. The mask persists until explicitly changed; audio_transport_restart_declick()
 * arms it, runs the restart, then re-arms NONE so recovery/shipping paths stay on baseline.
 */
extern void    wm8904_set_pending_declick( uint8_t mask );
extern uint8_t wm8904_get_pending_declick( void );

/*
 * Is the declick A/B research code present in this build? The three build-policy switches are fixed
 * inside wm8904.c on purpose (one behaviour for the whole fleet), so consumers ask at run time with
 * a compile-time-constant answer instead of testing a macro they cannot see. False => only the
 * shipping default exists and a non-zero mask is not honoured.
 */
extern bool    wm8904_declick_research_available( void );

/* Print the *td<NN> strategy legend for the console -- or, when the research code is compiled out,
 * a single line saying so. Kept in wm8904.c so the legend is compiled out with what it describes. */
extern void    wm8904_declick_print_strategy_help( void );

/* True once a full STARTUP DC-servo run on `inst` has captured offset values usable by WARM_SERVO. */
extern bool    wm8904_declick_servo_captured( uint8_t inst );

#endif //!_WM8904_H_

