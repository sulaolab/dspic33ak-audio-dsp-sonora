// Sonora board WM8904 codec driver.
#include "resolved_board_config.h"
#include "resolved_transport_config.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "timer_app.h"
#include "nora_i2c_master.h"
#if RESOLVED_BOARD_USE_CMSIS_I2C
#include "Driver_I2C_dsPIC33AK.h"
#endif
#include "board/devices/wm8904_def.h"


#include "board/devices/wm8904.h"




//===========================================================
// Definition
//===========================================================

//----------------------------------------------------------
// Build policy -- STATIC. One setting for the whole fleet (AK128 == AK512).
//----------------------------------------------------------
/*
 * Decided 2026-08-19 (ROM diet, see
 * [internal] report_wm8904_rom_diet_2026-08-19.md).
 *
 * These three are deliberately NOT project-level macros. Putting them in
 * configurations.xml would let one configuration diverge from another, and a
 * codec bring-up that talks and self-checks differently on AK128 than on AK512
 * is a debugging trap worth more than the bytes it saves. The driver fixes them
 * here instead, so every configuration -- AK128 serial update, AK512 Classic,
 * AK512 ASRC -- builds the same behaviour.
 *
 * A -D of any of the three from the build config is a build ERROR, not a silent
 * override: to measure an alternative, edit the three lines below (and put them
 * back). The per-setting commentary further down says what each one removes.
 */
#if defined(WM8904_TRACE_LEVEL) || defined(WM8904_ENA_DECLICK_RESEARCH) || defined(WM8904_ENA_WRITE_VERIFY)
#error "WM8904 build policy is fixed in wm8904.c -- do not -D these from configurations.xml / build.ps1"
#endif

#define WM8904_TRACE_LEVEL              1   // compact: one mode line per bring-up + hex failure codes
#define WM8904_ENA_DECLICK_RESEARCH     0   // console-only declick A/B strategies compiled out
#define WM8904_ENA_WRITE_VERIFY         1   // per-write read-back self-check kept: it is what makes
                                             // "apply=verified" / OK1 true, and what audio_transport's
                                             // CODEC_APPLY_FAILED path relies on. Costs ~300 B on AK128 --
                                             // not worth trading away the self-check for (see the report).

#define WM8904_SLV_ADDR                 (0x34)
#define WM8904_HPOUT_VOL_DEFAULT        (57u - 6u)    // -6dB, same as previous startup setting


/*
 * Startup policy:
 *
 * - The manual headphone/DC-servo start-up sequence is used whenever the
 *   headphone path is brought up.
 * - The WM8904 internal/default start-up sequence experiment is kept below
 *   as a reference, but it is not called from config.
 * - The integrated shutdown sequence is still used before init to bring a
 *   codec that survived dsPIC reset closer to a known state.
 * - Before manual start-up, wm8904_hpout_quench_before_startup() is used for
 *   HPOUT residual-state discharge control.
 */


/*
 * HIGH RATES: WHAT THE HARDWARE SAYS, AND WHAT THIS DRIVER IMPLEMENTS.
 *
 * Hardware (datasheet): at fs >= 88.2 kHz the WM8904 cannot run its ADC and DAC
 * simultaneously.  That is a property of the part, stated as >= 88.2 kHz because
 * that is where the datasheet draws the line.
 *
 * Implemented here: the rate table below offers 8, 11.025, 12, 16, 22.05, 24, 32,
 * 44.1, 48 and 96 kHz.  THERE IS NO 88.2 kHz ROW -- 96 kHz is the only rate in the
 * menu on the far side of that hardware boundary, so it is the only rate the
 * ADC+DAC rule can ever refuse.
 *
 * 96 kHz needs no dedicated register sequence: it is a table row like the others.
 * The register facts that used to live in dedicated constants -- SAMPLE_RATE still
 * selects the 48 kHz setting, CLK_SYS_RATE selects SYSCLK/fs = 128, BCLK_DIV
 * = 00010, LRCLK_RATE = 040h -- are simply that row's columns.  It is offered on a
 * 2-slot frame only (its TDM8 columns are WM8904_I2S_RATE_UNSUPPORTED).  The one
 * thing the table cannot express is enforced in wm8904_init_role(): no
 * simultaneous ADC + DAC at or above the hardware boundary.
 */


//----------------------------------------------------------
// Configurable sample rate -- FULL standard menu, TDM8 path (8 slots x 32 bit): BCLK = fs x 256,
// LRCLK_RATE = 256. So per fs: CLK_SYS_RATE = SYSCLK/fs, BCLK_DIV = SYSCLK/(fs x 256).
//   (Phase B) 48k FAMILY (8/12/16/24/32/48k): FLL-less, SYSCLK = MCLK = 12.288 MHz.
//   (Phase C) 44.1k FAMILY (11.025/22.05/44.1k): SYSCLK = FLL output = 11.2896 MHz (12.288 cannot
//             divide to these). use_fll=true rows share the one FLL setting below.
// Codes verified vs WM8904 datasheet Rev4.1: R21 CLK_SYS_RATE/SAMPLE_RATE (p.100), R26 BCLK_DIV (p.94,
// note ÷6 = 0b00111 = 0x07, NOT 0x06 = ÷5.5), FLL R116-R120 (pp.104-108).
// NOTE the 44.1k-family rows reuse the SAME SAMPLE_RATE/CLK_SYS_RATE/BCLK_DIV codes as the
// 48/24/12k rows; only the SYSCLK source differs (FLL 11.2896M vs MCLK 12.288M), which is what
// turns 48k->44.1k, 24k->22.05k, 12k->11.025k.
//----------------------------------------------------------

// (Phase C) One shared FLL setting for the whole 44.1k family: MCLK 12.288 MHz -> SYSCLK 11.2896 MHz.
// FVCO = FREF(12.288M) x N.K(7.35) x FRATIO(1) = 90.3168 MHz (in the required 90-100 MHz window);
// FOUT = FVCO / OUTDIV(8) = 11.2896 MHz. FRACN_ENA=1, GAIN/CTRL_RATE at datasheet defaults.
#define WM8904_FLL_R117_VAL   ( WM8904_FLL_OUTDIV(7u) | WM8904_FLL_FRATIO(0u) )        // OUTDIV field=div-1: 7=÷8; FRATIO ÷1
#define WM8904_FLL_R118_VAL   ( WM8904_FLL_K(0x599Au) )                                // K = round(0.35*65536) = 22938
#define WM8904_FLL_R119_VAL   ( WM8904_FLL_N(7u) | WM8904_FLL_GAIN(0u) )               // N = 7, GAIN x1
#define WM8904_FLL_R120_VAL   ( WM8904_FLL_CLK_REF_DIV(0u) | WM8904_FLL_CLK_REF_SRC(0u) ) // REF = MCLK / 1
/*
 * OVERRIDABLE settle time between FLL_ENA and selecting the FLL as SYSCLK.  There is no lock
 * check anywhere in this driver -- the datasheet's FLL lock time depends on FVCO/FREF/FRATIO and
 * no status bit is read here -- so this delay is the whole guarantee that SYSCLK is stable before
 * the switch.  Only the 44.1 kHz family takes this path (use_fll rows), which is why a defect that
 * appears at 44.1/22.05/11.025 kHz and vanishes at 48 kHz points here first.  Left at the shipping
 * 10 ms; raise it from a build (-D WM8904_FLL_LOCK_MS=30) to test that hypothesis without touching
 * this file.  See [internal] report_ak128_crackle_and_fifo128_2026-08-20.md.
 */
#ifndef WM8904_FLL_LOCK_MS
#define WM8904_FLL_LOCK_MS    (10u)   // fixed settle after FLL_ENA before selecting FLL as SYSCLK
#endif

typedef struct {
    uint32_t fs_hz;              // target sample rate
    uint8_t  sample_rate_code;   // R21[2:0]   SAMPLE_RATE
    uint8_t  clk_sys_rate_code;  // R21[13:10] CLK_SYS_RATE (SYSCLK/fs)
    uint8_t  bclk_div_code;      // R26[4:0]   BCLK_DIV (SYSCLK/BCLK; BCLK = fs x 256 for TDM8)
    bool     use_fll;            // false: SYSCLK = MCLK 12.288M; true: SYSCLK = FLL 11.2896M (44.1k family)
    /*
     * 2-slot (I2S, 64-BCLK frame) variants of the two fields above.  The TDM8
     * columns cannot serve a 2-slot frame: there BCLK = fs x 64, not fs x 256.
     * Keeping both in the table is what lets one code path drive either frame
     * width -- the previous code hardcoded the 2-slot values, which is why a
     * runtime rate change silently failed to move BCLK (see the report).
     * clk_sys_rate_code_i2s == 0xFF marks "this rate is not offered on a 2-slot
     * frame", so an unsupported request is rejected rather than mis-programmed.
     */
    uint8_t  clk_sys_rate_code_i2s;
    uint8_t  bclk_div_code_i2s;
    bool     high_rate;          // true: at/above the part's 88.2 kHz boundary (here: 96 kHz)
                                 //       -- ADC and DAC cannot run simultaneously
} wm8904_rate_cfg_t;

#define WM8904_I2S_RATE_UNSUPPORTED  (0xFFu)

/*
 * The two right-hand columns are the 2-slot (I2S, BCLK = fs x 64) equivalents of
 * CLK_SYS_RATE / BCLK_DIV.  CLK_SYS_RATE does not depend on the frame width
 * (it is SYSCLK/fs), so those two columns repeat the TDM8 value; only BCLK_DIV
 * changes, because a 64-BCLK frame needs 4x less BCLK than a 256-BCLK one.
 * Verified: every rate divides exactly on a 64-BCLK frame from 12.288 MHz
 * (or the 11.2896 MHz FLL for the 44.1k family), e.g. 48k -> 12.288M/3.072M = /4
 * and 96k -> 12.288M/6.144M = /2.
 *
 * 96 kHz is a normal table row, not a special case.  It differs from 48 kHz in
 * exactly two fields (CLK_SYS_RATE 128fs vs 256fs, BCLK_DIV /2 vs /4) and reuses
 * the 48 kHz SAMPLE_RATE code, exactly as the datasheet note above requires.  It
 * is offered on a 2-slot frame only: a TDM8 frame at 96 kHz would need
 * 8 x 32 x 96k = 24.576 MHz BCLK, which this SYSCLK cannot produce.
 */
static const wm8904_rate_cfg_t s_wm8904_rates[] = {
    //   fs        SAMPLE_RATE   CLK_SYS_RATE        BCLK_DIV          use_fll  CLK_SYS(I2S)      BCLK_DIV(I2S)     high_rate
    // 8k on a 64-BCLK frame needs /24. MEASURED: code 0x0E gives /22, not /24
    // (fsA came out 8727.27 = 12.288M/22/64 instead of 8000), so /24 is 0x0F.
    // Every other 2-slot BCLK_DIV code in this column was confirmed exact on
    // hardware by the leg-A rate sweep; this row was the only one wrong.
    {  8000u,      0x0u,         0x9u /* 1536 */,    0x07u /* ÷6   */, false,   0x9u /* 1536 */,  0x0Fu /* ÷24  */, false },
    { 11025u,      0x1u,         0x7u /* 1024 */,    0x04u /* ÷4   */, true,    0x7u /* 1024 */,  0x0Cu /* ÷16  */, false },  // FLL 11.2896M/1024
    { 12000u,      0x1u,         0x7u /* 1024 */,    0x04u /* ÷4   */, false,   0x7u /* 1024 */,  0x0Cu /* ÷16  */, false },
    { 16000u,      0x2u,         0x6u /*  768 */,    0x03u /* ÷3   */, false,   0x6u /*  768 */,  0x0Bu /* ÷12  */, false },
    { 22050u,      0x3u,         0x5u /*  512 */,    0x02u /* ÷2   */, true,    0x5u /*  512 */,  0x08u /* ÷8   */, false },  // FLL 11.2896M/512
    { 24000u,      0x3u,         0x5u /*  512 */,    0x02u /* ÷2   */, false,   0x5u /*  512 */,  0x08u /* ÷8   */, false },
    { 32000u,      0x4u,         0x4u /*  384 */,    0x01u /* ÷1.5 */, false,   0x4u /*  384 */,  0x07u /* ÷6   */, false },
    { 44100u,      0x5u,         0x3u /*  256 */,    0x00u /* ÷1   */, true,    0x3u /*  256 */,  0x04u /* ÷4   */, false },  // FLL 11.2896M/256
    { 48000u,      0x5u,         0x3u /*  256 */,    0x00u /* ÷1   */, false,   0x3u /*  256 */,  0x04u /* ÷4   */, false },
    // 96 kHz: 2-slot frame only (TDM8 columns unsupported), and -- being at or above
    // the datasheet's 88.2 kHz boundary -- ADC and DAC cannot run at the same time.
    { 96000u,      0x5u,         WM8904_I2S_RATE_UNSUPPORTED,
                                                     WM8904_I2S_RATE_UNSUPPORTED,
                                                                       false,   0x1u /*  128 */,  0x02u /* ÷2   */, true  },
};
#define WM8904_RATE_COUNT   (sizeof(s_wm8904_rates)/sizeof(s_wm8904_rates[0]))

// Per-I2C-instance selected sample rate (default 48 kHz). Indexed by the I2C instance number
// (I2C_INST_A=2, I2C_INST_B=3). Set per codec via wm8904_set_rate_hz(); default 48k until changed.
#define WM8904_INST_MAX     (4u)
static uint32_t s_fs_hz[WM8904_INST_MAX] = { 48000u, 48000u, 48000u, 48000u };
static bool s_io_ok[WM8904_INST_MAX] = { true, true, true, true };

// --- Declick research state (see [internal] manual_wm8904_mute_restart_declick.md) ---
// One-shot restart-strategy bitmask consumed by the next (re)configure. 0 == baseline.
/*
 * Declick A/B research (see [internal] manual_wm8904_mute_restart_declick.md).
 * Every strategy other than the shipping default is selected ONLY by the one-shot
 * console command audio_transport_restart_declick(mask) -- nothing in a normal
 * boot ever arms a bit. So the whole set is compile-time removable, and with the
 * mask a constant 0 the compiler drops the alternative paths on its own.
 */
/*
 * Every register write is read back and compared (wm8904_verify_write_readback),
 * and the verdict is what wm8904_init_role() reports as apply=verified/FAILED.
 * It is a bring-up self-check, not something the audio path needs: turning it off
 * also halves the I2C traffic of a codec bring-up.
 */
/* Both are fixed at the top of this file (build policy): no -D, no per-config value. */

#if WM8904_ENA_DECLICK_RESEARCH
static uint8_t s_declick_pending = (uint8_t)WM8904_DECLICK_NONE;
#define WM8904_DECLICK_MASK()   (s_declick_pending)
#else
#define WM8904_DECLICK_MASK()   ((uint8_t)WM8904_DECLICK_NONE)
#endif
// Retained DC-servo offset values captured after a full STARTUP servo run, per instance
// (R73..R76 = LINEOUTR, LINEOUTL, HPOUTR, HPOUTL), for the WARM_SERVO (DCS_TRIG_DAC_WR) restore.
static uint8_t s_dcs_val[WM8904_INST_MAX][4] = { { 0 } };
static bool    s_dcs_valid[WM8904_INST_MAX]  = { false, false, false, false };

static const wm8904_rate_cfg_t* wm8904_find_rate( uint32_t fs_hz );

/*
 * Record the rate an instance has actually been programmed to.
 *
 * This is deliberately NOT wm8904_set_rate_hz(): that one is the public
 * runtime-rate REQUEST, which validates its argument and programs the codec. This
 * one only publishes what a bring-up has already applied, so a caller that
 * configured the codec through another path does not leave s_fs_hz[] lying.
 *
 * Every rate in s_wm8904_rates[], 96 kHz included, is a real table entry with both
 * frame widths' columns, and 96 kHz IS runtime-selectable (`*ar CC 9`) on a build
 * whose frame is 2 slots and whose leg is one-way. What is NOT runtime-selectable
 * is the frame width itself: moving between a TDM8 (8-slot) and an I2S (2-slot)
 * frame would change APP_SLOTS_PER_FS, a compile-time transport fact, which is why
 * a TDM8 build refuses 96 kHz rather than reconfiguring itself.
 */
static void wm8904_note_configured_rate( uint8_t inst, uint32_t fs_hz )
{
    if( inst < WM8904_INST_MAX ) { s_fs_hz[inst] = fs_hz; }
}


// compatible
/*
 * Console verbosity, compile-time. The step-by-step narration of a codec bring-up
 * is a bring-up tool, not a shipping feature, and it is not free: on the AK128
 * serial-update image the full text costs 3,276 B of the module's 6,824 B.
 *
 *   3 = every step (the pre-2026-08-19 behaviour)
 *   2 = failure messages only, full text
 *   1 = COMPACT (SHIPPING): one line per bring-up saying which mode was applied, plus a hex
 *       code per failure. The code identifies the source site exactly (see the
 *       wm8904_err_t enum below, and the table in
 *       [internal] report_wm8904_rom_diet_2026-08-19.md), so a report
 *       stays actionable without carrying a sentence of prose per site in ROM.
 *   0 = silent
 *
 * Levels 2 and 3 print the prose and do NOT print the codes -- they are the same
 * information at different cost, never both.
 */
/* Fixed at the top of this file (build policy): no -D, no per-config value. */

#if WM8904_TRACE_LEVEL >= 2
#define TRACE                  printf
#else
#define TRACE(...)             do { } while(0)
#endif

#if WM8904_TRACE_LEVEL >= 3
#define TRACE_V                printf
#else
#define TRACE_V(...)           do { } while(0)
#endif

/*
 * Failure codes for the compact level. Grouped so the first hex digit already says
 * where to look: 1x bring-up entry / 2x register write / 3x register read /
 * 4x analog-mute verify / 5x DC servo + write sequencer.
 */
typedef enum {
    WM8904_E_BAD_INSTANCE       = 0x10,  // init_role: inst >= WM8904_INST_MAX
    WM8904_E_ID_CANCELLED       = 0x11,  // init_role: device ID not confirmed, bring-up cancelled
    WM8904_E_HIGH_RATE_ADC_DAC  = 0x12,  // init_role: >= 88.2 kHz cannot run ADC and DAC together
    WM8904_E_RATE_NOT_ON_FRAME  = 0x13,  // config: this rate is not offered on this frame width
    WM8904_E_ID_READ_FAILED     = 0x14,  // confirm_device_id: device ID read failed
    WM8904_E_WR_CMSIS_TX        = 0x20,  // write_reg: CMSIS MasterTransmit failed
    WM8904_E_WR_HAL             = 0x21,  // write_reg: I2C HAL write failed
    WM8904_E_WR_UNMATCH_VOL     = 0x22,  // readback mismatch, DAC/ADC digital volume
    WM8904_E_WR_UNMATCH_OUT1    = 0x23,  // readback mismatch, ANALOGUE_OUT1 L/R
    WM8904_E_WR_UNMATCH         = 0x24,  // readback mismatch, every other register
    WM8904_E_RD_CMSIS_TX        = 0x30,  // read_reg: CMSIS MasterTransmit failed
    WM8904_E_RD_CMSIS_RX        = 0x31,  // read_reg: CMSIS MasterReceive failed
    WM8904_E_RD_HAL             = 0x32,  // read_reg: I2C HAL read failed
    WM8904_E_MUTE_READ_FAILED   = 0x40,  // set_analog_output_mute_verified: I2C read failed
    WM8904_E_MUTE_UNVERIFIED    = 0x41,  // set_analog_output_mute_verified: mute bits wrong
    WM8904_E_WSEQ_SHDN_FELLBACK = 0x50,  // shutdown: write sequencer did not run, quenched instead
    WM8904_E_WSEQ_HPEN_FELLBACK = 0x51,  // config: WSEQ HP-enable fell back to the manual sequence
    WM8904_E_DCS_TIMEOUT        = 0x52,  // DC servo STARTUP did not complete
    WM8904_E_WSEQ_READ_FAILED   = 0x53,  // write sequencer wait: I2C read failed
    WM8904_E_WSEQ_NEVER_STARTED = 0x54,  // write sequencer never started (no SYSCLK on this codec?)
    WM8904_E_WSEQ_TIMEOUT       = 0x55,  // write sequencer did not finish
} wm8904_err_t;

#if WM8904_TRACE_LEVEL == 1
/*
 * One shared format string and one call for every failure site:
 * " wm8904 E<code> i<inst> d<detail>". `detail` is the register address for the
 * 2x/3x codes and the last register value read for the 5x codes -- whichever
 * single number the site's prose carried that a reader actually needs.
 */
static void wm8904_report_err( uint8_t code, uint8_t inst, uint16_t detail )
{
    printf(" wm8904 E%02X i%u d%04X\n", (unsigned)code, (unsigned)inst, (unsigned)detail);
}
#define WM8904_FAIL(code, inst, detail) \
        wm8904_report_err( (uint8_t)(code), (uint8_t)(inst), (uint16_t)(detail) )
#else
/* Levels 2/3 print the prose instead; level 0 prints nothing. */
#define WM8904_FAIL(code, inst, detail)  do { (void)(inst); (void)(detail); } while(0)
#endif

/* Console command output (wm8904_dump_reg): the reply IS the command. */
#define TRACE_OUT              printf






//===========================================================
// Enum & Struct typedef
//===========================================================





//===========================================================
// Function Prototype
//===========================================================

/*
 * Kept out-of-line on purpose. With the console narration compiled out
 * (WM8904_TRACE_LEVEL 1) the body was small enough for -O3 to inline it into
 * all 97 call sites, and wm8904_config() alone grew by ~12 KiB -- costing the
 * two AK512 configurations ~7.5 KiB of ROM each. Bring-up is a one-shot I2C
 * sequence, so the call overhead is free; this holds regardless of the current
 * WM8904_ENA_WRITE_VERIFY value.
 */
static void     wm8904_write_reg(uint8_t inst, uint8_t uc_register_address, uint16_t us_data)
    __attribute__((noinline));
static uint16_t wm8904_read_reg(uint8_t inst, uint8_t uc_register_address);
#if WM8904_ENA_WRITE_VERIFY
static void     wm8904_verify_write_readback(uint8_t inst, uint8_t uc_register_address, uint16_t us_data, uint16_t read_dat);
#endif
static bool     wm8904_confirm_device_id(uint8_t inst);

static void     wm8904_config(uint8_t inst, bool master_cfg, wm8904_role_t role);

static void     wm8904_write_dac_digital_mute(uint8_t inst, bool mute, bool ena96k);
static void     wm8904_write_hpout_level_mute(uint8_t inst, bool mute);

static bool     wm8904_wait_dc_servo_startup_done(uint8_t inst, uint16_t mask, uint32_t timeout_ms);
static bool     wm8904_wait_write_sequencer_done(uint8_t inst, uint32_t timeout_ms);

//backup static bool     wm8904_integrated_startup_sequence(uint8_t inst);
//backup static bool     wm8904_integrated_shutdown_sequence(uint8_t inst);
static void     wm8904_hpout_quench_before_startup(uint8_t inst);

// --- Declick research helpers (see [internal] manual_wm8904_mute_restart_declick.md) ---
static void     wm8904_hpout_ordered_disable(uint8_t inst);      // C: Table 42-ordered HP disable
static void     wm8904_capture_dc_servo(uint8_t inst);           // B: store R73..R76 after STARTUP
static bool     wm8904_apply_dc_servo_warm(uint8_t inst);        // B: restore via DCS_TRIG_DAC_WR
static void     wm8904_hpout_ramp_unmute(uint8_t inst);          // D: stepped HPOUT analog unmute (ramp up)
static void     wm8904_hpout_ramp_mute(uint8_t inst);            // E: stepped HPOUT gain ramp-down + mute
static bool     wm8904_wseq_shutdown(uint8_t inst);              // A: vendor Write Sequencer shutdown
static bool     wm8904_wseq_hp_enable(uint8_t inst);             // F: vendor WSEQ HP-enable block (idx 12)

/* Map legacy 1-based I2C instance to the HAL instance enum / CMSIS driver. */
#if RESOLVED_BOARD_USE_CMSIS_I2C
static ARM_DRIVER_I2C *wm8904_i2c_cmsis_driver(uint8_t inst);
#else
static nora_i2c_instance_t wm8904_i2c_hal_inst(uint8_t inst);
#endif




//===========================================================
// Variables
//===========================================================





//===========================================================
// Global Function
//===========================================================


#if WM8904_TRACE_LEVEL == 1
/*
 * The compact level's one line per bring-up: which mode the codec was actually left
 * in, and whether every register write verified. It stands in for the level-3
 * config summary plus the "apply=verified/FAILED" line.
 *   fs = applied sample rate                  R  = role (0 ADC+DAC / 1 ADC / 2 DAC)
 *   M  = 1 when this codec drives BCLK/LRCLK  S  = slots per frame
 *   F  = 1 when SYSCLK comes from the FLL (44.1 kHz family)
 *   OK = 1 when every register write read back as written
 */
static void wm8904_report_mode( uint8_t inst, bool master_cfg, wm8904_role_t role )
{
    const uint32_t fs = ( inst < WM8904_INST_MAX ) ? s_fs_hz[inst] : 0u;
    const wm8904_rate_cfg_t* const rate = wm8904_find_rate( fs );

    printf(" wm8904 i%u fs=%lu R%u M%u S%u F%u OK%u\n",
           (unsigned)inst,
           (unsigned long)fs,
           (unsigned)role,
           (unsigned)( master_cfg ? 1u : 0u ),
           (unsigned)RESOLVED_TRANSPORT_SLOTS_PER_FRAME,
           (unsigned)( ( ( rate != NULL ) && rate->use_fll ) ? 1u : 0u ),
           (unsigned)( ( inst < WM8904_INST_MAX ) && s_io_ok[inst] ) );
}
#define WM8904_REPORT_MODE(inst, master_cfg, role)  wm8904_report_mode((inst), (master_cfg), (role))
#else
#define WM8904_REPORT_MODE(inst, master_cfg, role)  do { } while(0)
#endif

bool wm8904_init_role( uint8_t inst, bool master_cfg, wm8904_role_t role )
{
    TRACE_V(" wm8904_init_role(I2C-%d): start. TDM master=%s role=%d @%ld\n",
          inst, (master_cfg)?"on":"off", (int)role, GetTicks());

    if( inst >= WM8904_INST_MAX )
    {
        WM8904_FAIL(WM8904_E_BAD_INSTANCE, inst, 0u);
        TRACE(" wm8904_init_role(%d): invalid instance\n", inst);
        return false;
    }

    /*
     * The one place the "no simultaneous ADC+DAC above 48 kHz" hardware limit is
     * enforced. Previously it was implied by which entry point the caller chose,
     * so nothing stopped a caller from asking for something the chip cannot do --
     * it just silently got whatever that function happened to program.
     */
    const wm8904_rate_cfg_t* const rate = wm8904_find_rate( s_fs_hz[inst] );
    if( ( rate != NULL ) && rate->high_rate && ( role == WM8904_ROLE_ADC_DAC ) )
    {
        WM8904_FAIL(WM8904_E_HIGH_RATE_ADC_DAC, inst, 0u);
        TRACE(" wm8904_init_role(%d): fs=%lu Hz cannot run ADC and DAC together;"
               " pick ROLE_ADC_ONLY or ROLE_DAC_ONLY\n",
               inst, (unsigned long)rate->fs_hz );
        s_io_ok[inst] = false;
        return false;
    }

    s_io_ok[inst] = true;

    if( wm8904_confirm_device_id(inst) )
    {
        /*
         * CPU reset does not reset the WM8904, so discharge the HPOUT residual state before the
         * software reset inside the configure below.
         *
         * THIS is the one place it happens, for every caller and every topology. Two facts hold
         * here and nowhere else: the codec has just answered on I2C, and its SYSCLK is running --
         * because a leg is only ever initialised at a point where the clock it will use is present
         * (that is why a controller-clocked leg is initialised after the transport start). The
         * vendor write sequencer requires exactly that clock. Callers used to decide whether and
         * where to pre-shut-down from the PROCESSOR's reset cause and the leg's clock ownership;
         * both are proxies for a codec-side fact, and both got it wrong -- leg B was quiesced
         * before its clock existed, on a decision made from RCON (T2.8, 2026-08-11).
         *
         * Unconditional on purpose. Discharging a codec that happens to sit at its reset defaults
         * costs the ~294 ms of the write sequencer and changes nothing else, which is cheaper than
         * a condition that can be wrong -- and cheaper than depending on state that a power cycle
         * silently changes. wm8904_shutdown() is the ordered vendor WSEQ shutdown and falls back to
         * the quench if the sequencer does not run. The quench below is kept as well because the
         * measured pop suppression was measured WITH it in place
         * ([internal] manual_wm8904_mute_restart_declick.md, §8 and §13 #2).
         *
         * Skipped for ADC-only, which does not use the headphone path at all -- this matches what
         * the dedicated 96 kHz ADC-only path did, and its comment explaining why.
         */
        if( role != WM8904_ROLE_ADC_ONLY )
        {
            wm8904_shutdown(inst);
            wm8904_hpout_quench_before_startup(inst);
        }

        wm8904_config(inst, master_cfg, role);
    }
    else
    {
        WM8904_FAIL(WM8904_E_ID_CANCELLED, inst, master_cfg);
        TRACE(" Cancel starting up WM8904 I2C inst=%d master_cfg=%d\n", inst, master_cfg);
    }

    TRACE_V(" wm8904_init_role(%d): end. @%ld\n", inst, GetTicks());
    TRACE(" wm8904_init_role(%d): apply=%s\n", inst, s_io_ok[inst] ? "verified" : "FAILED" );
    WM8904_REPORT_MODE(inst, master_cfg, role);
    TRACE_V("\n");
    return s_io_ok[inst];
}

/* Historical signature: ADC+DAC at the instance's currently selected rate. */
bool wm8904_init( uint8_t inst, bool master_cfg )
{
    return wm8904_init_role( inst, master_cfg, WM8904_ROLE_ADC_DAC );
}




void wm8904_shutdown( uint8_t inst )
{
    // SHUTDOWN discharge policy. Measurement (research doc) showed the vendor Control Write Sequencer
    // shutdown is the only strategy that suppresses the pop, so it is now the DEFAULT (mask NONE). The
    // declick mask can still force the alternatives for A/B regression.
    const uint8_t declick = WM8904_DECLICK_MASK();

    if( (declick & WM8904_DECLICK_LEGACY_QUENCH) != 0u )
    {
        // Regression: the pre-declick quench (RMV_SHORT=1 -> 0), for comparison vs the new default.
        wm8904_hpout_quench_before_startup(inst);
        return;
    }

    if( (declick & WM8904_DECLICK_ORDERED_SHUTDN) != 0u )
    {
        // C: Table 42-ordered headphone disable (RMV_SHORT=0 first, then all ENA bits off).
        wm8904_hpout_ordered_disable(inst);
        return;
    }

    // Default (NONE) and explicit WSEQ (A): vendor Control Write Sequencer shutdown (Table 89, ordered
    // RMV_SHORT->ENA->DCS->CP->DAC->CLK->PGA->BIAS->VMID with datasheet timing). Needs SYSCLK present;
    // falls back to the quench if the sequencer does not complete (e.g. no clock at cold entry).
    if( wm8904_wseq_shutdown(inst) )
    {
        return;
    }
    WM8904_FAIL(WM8904_E_WSEQ_SHDN_FELLBACK, inst, 0u);
    TRACE(" WM8904 WSEQ shutdown fell back to quench inst=%d\n", inst);
    wm8904_hpout_quench_before_startup(inst);
}




void wm8904_reg_write( uint8_t inst, uint8_t reg, uint16_t data )
{
    if( inst >= WM8904_INST_MAX ) { return; }
    wm8904_write_reg( inst, reg, data );
}

uint16_t wm8904_reg_read( uint8_t inst, uint8_t reg )
{
    if( inst >= WM8904_INST_MAX ) { return 0xFFFFu; }
    return wm8904_read_reg( inst, reg );
}


void wm8904_dump_reg( uint8_t inst )
{
    uint16_t reg_val = 0;

    TRACE_OUT("wm8904_dump_reg: inst=%d\n", inst);

    for( uint8_t reg=0x00; reg<=0xF8; reg++ )
    {
        reg_val = wm8904_read_reg(inst, reg);
        TRACE_OUT(" addr=0x%02x val=0x%04x\n", reg, reg_val);
    }
}


void wm8904_set_analog_output_mute( uint8_t inst, bool mute )
{
    // Declick research (D / SOFT_UNMUTE): when the one-shot mask is armed, unmute by ramping the HPOUT
    // volume in steps instead of the immediate mute-bit release. Mute (and every non-declick caller,
    // mask NONE) is unchanged.
    if( !mute && (WM8904_DECLICK_MASK() & WM8904_DECLICK_SOFT_UNMUTE) != 0u )
    {
        wm8904_hpout_ramp_unmute(inst);
        TRACE_V(" WM8904 analog output unmute(soft-ramp) inst=%d @%ld\n", inst, GetTicks());
        return;
    }
    if( mute && (WM8904_DECLICK_MASK() & WM8904_DECLICK_SOFT_SHUTDOWN) != 0u )
    {
        wm8904_hpout_ramp_mute(inst);   // E: ramp HPOUT gain down BEFORE the hard mute/shutdown
        TRACE_V(" WM8904 analog output mute(soft-ramp-down) inst=%d @%ld\n", inst, GetTicks());
        return;
    }

    wm8904_write_hpout_level_mute(inst, mute);

    TRACE_V(" WM8904 analog output %s inst=%d @%ld\n",
          (mute) ? "mute" : "unmute",
          inst,
           GetTicks());
}


bool wm8904_set_analog_output_mute_verified( uint8_t inst, bool mute )
{
    uint16_t left;
    uint16_t right;
    const uint16_t expected = mute ? WM8904_HPOUTL_MUTE : 0u;

    if( inst >= WM8904_INST_MAX )
    {
        return false;
    }

    wm8904_set_analog_output_mute( inst, mute );

    /* wm8904_read_reg() returns 0xCECE on a transport failure.  Test it
     * explicitly so an unmute cannot mistake the sentinel for a clear bit. */
    left  = wm8904_read_reg( inst, WM8904_ANALOGUE_OUT1_LEFT );
    right = wm8904_read_reg( inst, WM8904_ANALOGUE_OUT1_RIGHT );
    if( ( left == 0xCECEu ) || ( right == 0xCECEu ) )
    {
        WM8904_FAIL(WM8904_E_MUTE_READ_FAILED, inst, mute ? 1u : 0u);
        TRACE(" WM8904 analog output %s NOT verified inst=%d (I2C read failed)\n",
              mute ? "mute" : "unmute", inst);
        return false;
    }

    if( ( ( left & WM8904_HPOUTL_MUTE ) != expected ) ||
        ( ( right & WM8904_HPOUTR_MUTE ) != expected ) )
    {
        WM8904_FAIL(WM8904_E_MUTE_UNVERIFIED, inst, left);
        TRACE(" WM8904 analog output %s NOT verified inst=%d L=0x%04x R=0x%04x\n",
              mute ? "mute" : "unmute", inst, left, right);
        return false;
    }

    TRACE_V(" WM8904 analog output %s verified inst=%d\n",
          mute ? "mute" : "unmute", inst);
    return true;
}


// --- Declick research one-shot strategy accessors (see wm8904.h / research doc) ---
void wm8904_set_pending_declick( uint8_t mask )
{
#if WM8904_ENA_DECLICK_RESEARCH
    s_declick_pending = mask;
#else
    (void)mask;   /* research paths compiled out; the mask has nowhere to go */
#endif
}

uint8_t wm8904_get_pending_declick( void )
{
    return WM8904_DECLICK_MASK();
}

bool wm8904_declick_servo_captured( uint8_t inst )
{
    return ( inst < WM8904_INST_MAX ) ? s_dcs_valid[inst] : false;
}

/*
 * Whether the one-shot declick A/B strategies exist in THIS build. The policy that decides it lives
 * at the top of this file, so a consumer cannot test the macro -- it asks instead. When this is false
 * the only reachable behaviour is the shipping default, and the console says so rather than offering
 * a strategy menu that has been compiled out.
 */
bool wm8904_declick_research_available( void )
{
    return ( WM8904_ENA_DECLICK_RESEARCH != 0 );
}

/*
 * Print the *td<NN> strategy legend. It lives here, next to the policy, so that when the research
 * code is compiled out the legend strings go with it instead of describing code that is not there.
 */
void wm8904_declick_print_strategy_help( void )
{
#if WM8904_ENA_DECLICK_RESEARCH
    printf("   DEFAULT(0x00) shutdown = vendor WSEQ (proven best); startup = manual\n");
    printf("   bit0 0x01 C: Table42-ordered HP disable (measured no-op)\n");
    printf("   bit1 0x02 B: skip R0 SW-reset + DCS_TRIG_DAC_WR servo restore (no-op)\n");
    printf("   bit2 0x04 D: ramped HPOUT analog unmute (no-op)\n");
    printf("   bit3 0x08 A: force WSEQ shutdown (== default; redundant)\n");
    printf("   bit4 0x10 E: ramp HPOUT gain DOWN before shutdown (no-op)\n");
    printf("   bit5 0x20 F: vendor WSEQ startup (HP-enable block) -- attacks startup pop\n");
    printf("   bit6 0x40 : force LEGACY quench shutdown (regression vs default)\n");
    printf("   e.g. *td00(default) *td20(+WSEQ startup) *td40(old quench)\n");
#else
    printf("   compiled out: WM8904_ENA_DECLICK_RESEARCH 0 in wm8904.c -- only the shipping\n");
    printf("   default is built, the mask is ignored, so \"*td\" == \"*tr\"\n");
#endif
}






//===========================================================
// Local Function
//===========================================================

/*
 * \brief Write data to WM8904 Register.
 *
 * \param uc_register_address Register address to write
 * \param us_data Data to write.
 */
/* noinline -- see the prototype for why. */
static void wm8904_write_reg(uint8_t inst, uint8_t uc_register_address, uint16_t us_data)
{
    uint16_t read_dat = 0;
    uint8_t  tx[3];


    tx[0] = uc_register_address;
    tx[1] = (uint8_t)((us_data >> 8) & 0xFFu);
    tx[2] = (uint8_t)(us_data & 0xFFu);

#if RESOLVED_BOARD_USE_CMSIS_I2C
    {
        ARM_DRIVER_I2C *drv = wm8904_i2c_cmsis_driver(inst);

        if( drv == NULL )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }

        if( drv->MasterTransmit((uint32_t)(WM8904_SLV_ADDR >> 1),
                                tx,
                                sizeof(tx),
                                false) != ARM_DRIVER_OK )
        {
            WM8904_FAIL(WM8904_E_WR_CMSIS_TX, inst, uc_register_address);
            TRACE(" wm8904_write_reg(): CMSIS MasterTransmit failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }
    }
#else
    {
        nora_i2c_instance_t hal_inst = wm8904_i2c_hal_inst(inst);

        if( hal_inst == NORA_I2C_INST_COUNT )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }

        if( nora_i2c_write(hal_inst,
                                 (uint8_t)(WM8904_SLV_ADDR >> 1),
                                 tx,
                                 sizeof(tx)) != NORA_I2C_OK )
        {
            WM8904_FAIL(WM8904_E_WR_HAL, inst, uc_register_address);
            TRACE(" wm8904_write_reg(): I2C HAL write failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return;
        }
    }
#endif

    delay_us(100);  // keep same post-write settling as legacy path


#if WM8904_ENA_WRITE_VERIFY
    read_dat = wm8904_read_reg( inst, uc_register_address );

    wm8904_verify_write_readback(inst, uc_register_address, us_data, read_dat);
#else
    (void)read_dat;
#endif
}




/*
 * \brief Verify WM8904 register readback after write.
 *
 * Some WM8904 registers contain trigger/update bits that do not read back as
 * the written command value. Keep those register-specific exceptions here so
 * wm8904_write_reg() stays focused on the I2C write sequence.
 */
#if WM8904_ENA_WRITE_VERIFY
static void wm8904_verify_write_readback(uint8_t inst, uint8_t uc_register_address, uint16_t us_data, uint16_t read_dat)
{
    switch( uc_register_address )
    {
    case 0x00:
        // R0 always reads back as the device ID 0x8904, not the written reset command.
        break;

    case WM8904_WRITE_SEQUENCER_3:
        // WSEQ_START is a trigger bit. Readback may not match the written value.
        break;

    case WM8904_DC_SERVO_1:
        /*
         * R68 / DC Servo 1 contains DC-servo trigger bits.
         *
         * DCS_TRIG_STARTUP_x and DCS_TRIG_DAC_WR_x are command/status bits:
         * writing 1 starts a correction, while readback 1 means the correction
         * is still in progress. After completion, readback can return 0 even
         * though the command write was accepted.
         *
         * Therefore full write/readback equality is not a valid check for this
         * register. Completion is checked separately by reading
         * WM8904_DC_SERVO_READBACK_0.
         */
        break;

    case WM8904_DAC_DIGITAL_VOLUME_LEFT:
    case WM8904_DAC_DIGITAL_VOLUME_RIGHT:
    case WM8904_ADC_DIGITAL_VOLUME_LEFT:
    case WM8904_ADC_DIGITAL_VOLUME_RIGHT:
        // WM8904_DAC_VU and WM8904_ADC_VU are same 0x0100.
        if( (us_data & ~(0x0100)) != read_dat )
        {
            WM8904_FAIL(WM8904_E_WR_UNMATCH_VOL, inst, uc_register_address);
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;

    case WM8904_ANALOGUE_OUT1_LEFT:
    case WM8904_ANALOGUE_OUT1_RIGHT:
        if( (us_data & ~WM8904_HPOUT_VU) != read_dat )
        {
            WM8904_FAIL(WM8904_E_WR_UNMATCH_OUT1, inst, uc_register_address);
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;

    default:
        if( us_data != read_dat )
        {
            WM8904_FAIL(WM8904_E_WR_UNMATCH, inst, uc_register_address);
            TRACE(" wm8904_write_reg(): unmatch!! [reg:0x%x] w_data=0x%04x r_dat=0x%04x\n",
                                                   uc_register_address, us_data, read_dat);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        }
        break;
    }
}
#endif // WM8904_ENA_WRITE_VERIFY




/*
 * \brief Read data from WM8904 Register.
 *
 * \param uc_register_address Register address to write
 * \return Register value.
 */
static uint16_t wm8904_read_reg(uint8_t inst, uint8_t uc_register_address)
{
#define RET_INVALID     (0xCECE)

    uint8_t tx[1];
    uint8_t rx[2];

    tx[0] = uc_register_address;

#if RESOLVED_BOARD_USE_CMSIS_I2C
    {
        ARM_DRIVER_I2C *drv = wm8904_i2c_cmsis_driver(inst);
        uint32_t addr7 = (uint32_t)(WM8904_SLV_ADDR >> 1);

        if( drv == NULL )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( drv->MasterTransmit(addr7, tx, sizeof(tx), true) != ARM_DRIVER_OK )
        {
            WM8904_FAIL(WM8904_E_RD_CMSIS_TX, inst, uc_register_address);
            TRACE(" wm8904_read_reg(): CMSIS MasterTransmit failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( drv->MasterReceive(addr7, rx, sizeof(rx), false) != ARM_DRIVER_OK )
        {
            WM8904_FAIL(WM8904_E_RD_CMSIS_RX, inst, uc_register_address);
            TRACE(" wm8904_read_reg(): CMSIS MasterReceive failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }
    }
#else
    {
        nora_i2c_instance_t hal_inst = wm8904_i2c_hal_inst(inst);

        if( hal_inst == NORA_I2C_INST_COUNT )
        {
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }

        if( nora_i2c_write_read(hal_inst,
                                     (uint8_t)(WM8904_SLV_ADDR >> 1),
                                     tx,
                                     sizeof(tx),
                                     rx,
                                     sizeof(rx)) != NORA_I2C_OK )
        {
            WM8904_FAIL(WM8904_E_RD_HAL, inst, uc_register_address);
            TRACE(" wm8904_read_reg(): I2C HAL read failed inst=%d reg=0x%02x\n",
                  inst,
                  uc_register_address);
            if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
            return RET_INVALID;
        }
    }
#endif

    return ((((uint16_t)rx[0]) << 8) | rx[1]);
}


#if RESOLVED_BOARD_USE_CMSIS_I2C
/* Map legacy 1-based I2C instance to the CMSIS driver handle.
 * I2C peripheral init is done from main via Initialize()/PowerControl(). */
static ARM_DRIVER_I2C *wm8904_i2c_cmsis_driver(uint8_t inst)
{
    switch( inst )
    {
    case 1:
        return &Driver_I2C0;
    case 2:
        return &Driver_I2C1;
    case 3:
        return &Driver_I2C2;
    case 4:
        return &Driver_I2C3;
    default:
        return NULL;
    }
}
#else
/* Map legacy 1-based I2C instance to the I2C HAL instance enum.
 * I2C peripheral init is done from main via nora_i2c_init(). */
static nora_i2c_instance_t wm8904_i2c_hal_inst(uint8_t inst)
{
    switch( inst )
    {
    case 1:
        return NORA_I2C_INST_1;
    case 2:
        return NORA_I2C_INST_2;
    case 3:
        return NORA_I2C_INST_3;
    default:
        return NORA_I2C_INST_COUNT;
    }
}
#endif



static bool wm8904_confirm_device_id( uint8_t inst )
{
    uint16_t data = 0;

    data = wm8904_read_reg(inst, WM8904_SW_RESET_AND_ID);
    if(data != 0x8904)
    {
        if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        WM8904_FAIL(WM8904_E_ID_READ_FAILED, inst, 0u);
        TRACE(" wm8904_confirm_device_id(%d): Error!! Failed to read WM8904 device ID\n", inst);
        return false;
    }
    else
    {
        TRACE_V(" wm8904_confirm_device_id(%d): WM8904 dev ID is 0x8904(good)\n", inst);
        return true;
    }
}





// Look up one supported standard-menu rate row; NULL for values outside the
// 8/11.025/12/16/22.05/24/32/44.1/48/96 kHz menu. The row records whether the FLL is required.
static const wm8904_rate_cfg_t* wm8904_find_rate( uint32_t fs_hz )
{
    for( unsigned i = 0u; i < WM8904_RATE_COUNT; i++ )
    {
        if( s_wm8904_rates[i].fs_hz == fs_hz )
        {
            return &s_wm8904_rates[i];
        }
    }
    return NULL;
}

// (Phase B) Select the sample rate applied to `inst` on its NEXT (re)configuration. Stores only;
// the caller must re-init the codec (e.g. audio_transport_restart()) for it to take effect. Rejects any
// rate not in the standard menu (8/11.025/12/16/22.05/24/32/44.1/48k). In the independent
// codec-master topology either endpoint may be selected before a mute-bounded restart.
bool wm8904_set_rate_hz( uint8_t inst, uint32_t fs_hz )
{
    if( inst >= WM8904_INST_MAX )            { return false; }
    const wm8904_rate_cfg_t* const rate = wm8904_find_rate( fs_hz );
    if( rate == NULL )                       { return false; }   // must be a table rate
    /*
     * Reject a rate this build's frame width cannot carry, rather than accepting
     * it here and mis-programming the clock later. 96 kHz exists in the table but
     * is 2-slot-only: on TDM8 it would need 24.576 MHz BCLK.
     */
#if RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u
    if( rate->clk_sys_rate_code_i2s == WM8904_I2S_RATE_UNSUPPORTED ) { return false; }
#else
    if( rate->clk_sys_rate_code == WM8904_I2S_RATE_UNSUPPORTED )     { return false; }
#endif
    s_fs_hz[inst] = fs_hz;
    return true;
}

// Currently-selected sample rate for `inst` (the value the next wm8904_config applies). Default 48k.
// Lets the app read the codec-domain rate (e.g. to re-tune the A-side DSP to A's rate).
uint32_t wm8904_get_rate_hz( uint8_t inst )
{
    return ( inst < WM8904_INST_MAX ) ? s_fs_hz[inst] : 48000u;
}


static void wm8904_config( uint8_t inst, bool master_cfg, wm8904_role_t role )
{
    uint16_t data = 0;

    // (Phase B) Resolve the target sample rate for this instance (default 48 kHz). Fail-safe to 48k
    // if somehow unset/out of range, so a bad value never leaves the codec unclocked.
    const wm8904_rate_cfg_t* rate =
        wm8904_find_rate( (inst < WM8904_INST_MAX) ? s_fs_hz[inst] : 48000u );
    if( rate == NULL )
    {
        if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        rate = wm8904_find_rate( 48000u );
    }
    /*
     * Pick the CLK_SYS_RATE / BCLK_DIV pair that matches the PHYSICAL frame width.
     * A 2-slot (I2S) frame carries BCLK = fs x 64; a TDM8 frame carries fs x 256.
     * The code this replaced hardcoded the 2-slot pair, which is precisely why a
     * runtime rate change on a 2-slot bus reported success while BCLK never moved.
     */
#if RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u
    const uint8_t clk_sys_rate_code = rate->clk_sys_rate_code_i2s;
    const uint8_t bclk_div_code     = rate->bclk_div_code_i2s;
#else
    const uint8_t clk_sys_rate_code = rate->clk_sys_rate_code;
    const uint8_t bclk_div_code     = rate->bclk_div_code;
#endif
    if( ( clk_sys_rate_code == WM8904_I2S_RATE_UNSUPPORTED ) ||
        ( bclk_div_code == WM8904_I2S_RATE_UNSUPPORTED ) )
    {
        // This rate is not offered on this frame width (e.g. 96 kHz on TDM8, which
        // would need 24.576 MHz BCLK). Report and leave the codec alone rather than
        // programming an incoherent clock.
        if( inst < WM8904_INST_MAX ) { s_io_ok[inst] = false; }
        WM8904_FAIL(WM8904_E_RATE_NOT_ON_FRAME, inst, RESOLVED_TRANSPORT_SLOTS_PER_FRAME);
        TRACE(" wm8904_config(%d): fs=%lu Hz is not supported on a %u-slot frame\n",
               inst, (unsigned long)rate->fs_hz,
               (unsigned)RESOLVED_TRANSPORT_SLOTS_PER_FRAME );
        return;
    }
    TRACE_V(" wm8904_config(%d): fs=%luHz role=%d FLL=%s (SAMPLE_RATE=0x%x CLK_SYS_RATE=0x%x BCLK_DIV=0x%x slots=%u)\n",
          inst, (unsigned long)rate->fs_hz, (int)role,
          rate->use_fll ? "on(SYSCLK=11.2896M)" : "off(SYSCLK=MCLK)",
          (unsigned)rate->sample_rate_code, (unsigned)clk_sys_rate_code,
          (unsigned)bclk_div_code, (unsigned)RESOLVED_TRANSPORT_SLOTS_PER_FRAME );

    // Declick research (B / WARM_SERVO): a warm restart skips the R0 software reset and restores the DC
    // servo from captured offsets via DCS_TRIG_DAC_WR (below), instead of collapsing VMID/charge-pump and
    // re-running the full STARTUP servo. Only taken when the one-shot mask asks AND a prior STARTUP run has
    // captured offsets for this instance; otherwise we fall through to the unchanged cold path. Baseline
    // (mask NONE / shipping / recovery) always resets.
    const bool warm = ( (WM8904_DECLICK_MASK() & WM8904_DECLICK_WARM_SERVO) != 0u )
                      && ( inst < WM8904_INST_MAX ) && s_dcs_valid[inst];

    // --- Step 1: Software Reset ---
    if( !warm )
    {
        wm8904_write_reg( inst, WM8904_SW_RESET_AND_ID,     0x0000 ); // R0 - SW Reset and ID
        delay_ms(50);
    }
    else
    {
        TRACE_V(" wm8904_config(%d): WARM restart -- skip SW-reset, DCS_TRIG_DAC_WR servo restore\n", inst);
    }

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS );
    wm8904_write_reg( inst, WM8904_VMID_CONTROL_0,      WM8904_VMID_BUF_ENA | WM8904_VMID_RES_LP | WM8904_VMID_ENA );
    delay_ms(5);

    wm8904_write_reg( inst, WM8904_BIAS_CONTROL_0,      WM8904_ISEL_HP_BIAS | WM8904_BIAS_ENA | 0x10 ); //0x10: secret? datasheet recommendation=0x19 (Low Power Playback Mode Disable)

    /*
     * Role-dependent analog/converter power. These are the same register values the
     * three former entry points (wm8904_config() and
     * wm8904_config_96k_{adc,dac}_only()) programmed; only the selection moved from
     * "which function did the caller call" to "which role did the caller ask for".
     *
     * ADC_OSR128 / DAC_OSR128 must be 0 at fs >= 88.2 kHz (datasheet), which is why
     * ANALOGUE_ADC_0 is rate-dependent rather than role-dependent.
     */
    const bool want_adc = ( role != WM8904_ROLE_DAC_ONLY );
    const bool want_dac = ( role != WM8904_ROLE_ADC_ONLY );

    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_0,
                      want_adc ? ( WM8904_INL_ENA | WM8904_INR_ENA ) : 0x0000 );
    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_2,
                      want_dac ? ( WM8904_HPL_PGA_ENA | WM8904_HPR_PGA_ENA ) : 0x0000 );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT12_ZC,   0x0000 );
    wm8904_write_reg( inst, WM8904_CHARGE_PUMP_0,       want_dac ? WM8904_CP_ENA : 0x0000 );

    // The WM8904 datasheet states "The input to the ADC is phase inverted with respect
    // to the selected input pin" -- the single-ended input PGA is internally inverting
    // (p.38). ADCL/ADCR_DATINV restores the source phase at the ADC output stage.
    // WM8904_ADC_HPF is just the datasheet default; it carries no special meaning here.
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_0,
                      want_adc ? ( WM8904_ADC_HPF | WM8904_ADCL_DATINV | WM8904_ADCR_DATINV )
                               : 0x0000 );
    // ADC_OSR128 is the 48 kHz-family default and MUST be clear at fs >= 88.2 kHz.
    wm8904_write_reg( inst, WM8904_ANALOGUE_ADC_0,
                      ( want_adc && !rate->high_rate ) ? WM8904_ADC_OSR128 : 0x0000 );

    wm8904_write_dac_digital_mute( inst, true, !want_dac );

    wm8904_write_reg( inst, WM8904_POWER_MANAGEMENT_6,
                      (uint16_t)( ( want_dac ? ( WM8904_DACL_ENA | WM8904_DACR_ENA ) : 0u ) |
                                  ( want_adc ? ( WM8904_ADCL_ENA | WM8904_ADCR_ENA ) : 0u ) ) );

// test class G(regular)   wm8904_write_reg( inst, WM8904_CLASS_W_0,           WM8904_CP_DYN_PWR );


    // (Phase C) Clocking: 48k family runs SYSCLK = MCLK (FLL off); 44.1k family runs SYSCLK from the
    // FLL (12.288M -> 11.2896M). CLK_SYS is still disabled here (post SW-reset), so it is safe to set
    // up the source. FLL order (datasheet pp.104-108): program R117-R120, enable FLL (FLL_ENA after
    // FRACN_ENA), wait for lock, THEN select FLL as SYSCLK (SYSCLK_SRC) and enable CLK_SYS/DSP.
    if( rate->use_fll )
    {
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_2, WM8904_FLL_R117_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_3, WM8904_FLL_R118_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_4, WM8904_FLL_R119_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_5, WM8904_FLL_R120_VAL );
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, WM8904_FLL_FRACN_ENA );                     // configure, FLL_ENA=0
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, WM8904_FLL_FRACN_ENA | WM8904_FLL_ENA );    // enable FLL
        delay_ms( WM8904_FLL_LOCK_MS );                                                           // let the FLL lock
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_1, WM8904_CLK_SYS_RATE(clk_sys_rate_code) | WM8904_SAMPLE_RATE(rate->sample_rate_code) );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_0, 0x0000 );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_2, WM8904_SYSCLK_SRC | WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );  // SYSCLK = FLL
    }
    else
    {
        wm8904_write_reg( inst, WM8904_FLL_CONTROL_1, 0x0000 );  // FLL disabled
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_1, WM8904_CLK_SYS_RATE(clk_sys_rate_code) | WM8904_SAMPLE_RATE(rate->sample_rate_code) );
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_0, 0x0000 );  // 0: SYSCLK = MCLK
        wm8904_write_reg( inst, WM8904_CLOCK_RATES_2, WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA );  // SYSCLK_SRC=0 (MCLK)
    }


    data  = 0x0050;
#if defined(WM8904_SWAP_ADC_LR)
    data &= ~(WM8904_AIFADCR_SRC);
    data |= WM8904_AIFADCL_SRC;   // Left digital audio channel source = Right ADC
#endif //defined(WM8904_SWAP_ADC_LR)
#if defined(WM8904_SWAP_DAC_LR)
    data &= ~(WM8904_AIFDACR_SRC);
    data |= WM8904_AIFDACL_SRC;   // Left DAC source = Right digital channel
#endif //defined(WM8904_SWAP_DAC_LR)
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_0,   data );

// note!! regarding WM8904_AIF_LRCLK_INV
//
// I2S modes:
//  LRC polarity
//   0 = Not Inverted
//   1 = Inverted
//
//  DSP Mode Mode A-B:
//   0 = [   1-bit delay] MSB is available on 2nd BCLK rising edge after LRC rising edge (mode A)
//   1 = [no 1-bit delay] MSB is available on 1st BCLK rising edge after LRC rising edge (mode B)
//
    // WM8904_AUDIO_INTERFACE_1 settings
    data =  WM8904_AIF_WL_32BIT;
#if RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u
    data |= WM8904_AIF_FMT_I2S;
#else
    data |= WM8904_AIF_FMT_DSP;
  #if RESOLVED_TRANSPORT_DATA_DELAY_BITS == 0u
    data |= WM8904_AIF_LRCLK_INV;   // set 1 means "no 1bit delay". it's tricky. see above comment.
  #endif // RESOLVED_TRANSPORT_DATA_DELAY_BITS == 0u
#endif // RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u

    if( master_cfg )
    {
        data |= WM8904_BCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_1,   data );


    // WM8904_AUDIO_INTERFACE_3 settings
#if RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u
    COMPILEASSERT(RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u);
           // 3) LRCLK output and rate: 64Fs (= 48 kHz at BCLK 3.072 MHz).
    data = WM8904_LRCLK_RATE(64);
#else
    COMPILEASSERT(RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 8u);

    data = WM8904_LRCLK_RATE(256);
#endif // RESOLVED_TRANSPORT_SLOTS_PER_FRAME == 2u
    if( master_cfg )
    {
        data |= WM8904_LRCLK_DIR;
    }
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_3,   data );

    // BCLK divider from the rate table, for whichever frame width this build uses:
    // 2-slot -> BCLK = fs x 64 (48k: 12.288M/3.072M = /4; 96k: /2),
    // TDM8   -> BCLK = fs x 256.
    // Driving this from the table (instead of a hardcoded /4) is what makes a
    // runtime rate change on a 2-slot bus actually move BCLK.
    wm8904_write_reg( inst, WM8904_AUDIO_INTERFACE_2,   WM8904_BCLK_DIV(bclk_div_code) );


//
// Microchip WM8904 X32 Eval PCB equips RED and BLUE connectors
//
#if RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK
//////////////
// IN1(RED)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN1L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN1R ); // in single-ended mode, use N only
#else
//////////////
// IN2(BLUE)
//////////////
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_1,  WM8904_L_MODE_SINGLE_ENDED | WM8904_L_IP_SEL_N_IN2L ); // in single-ended mode, use N only
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_1, WM8904_R_MODE_SINGLE_ENDED | WM8904_R_IP_SEL_N_IN2R ); // in single-ended mode, use N only
#endif // RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK

//
// enabling / disabling MIC Bias voltage from WM8904
//
#if RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED
    // MIC related settings (BIAS voltage ON)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     WM8904_MICBIAS_ENA );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     WM8904_MICBIAS_SEL(0x1) ); // 001 = 10/9 x AVDD (2.0V)
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );                  // disabled MICDET / Hook switch detection
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );                  // disabled PDM mic
#else
    // MIC related settings (BIAS voltage OFF)
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_0,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_BIAS_CONTROL_1,     0x0000 );
    wm8904_write_reg( inst, WM8904_MIC_FILTER_CONTROL,     0x0000 );
    wm8904_write_reg( inst, WM8904_DIGITAL_MICROPHONE_0,   0x0000 );
#endif // RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED


    /////////////////////////////////////////////////
    // DC servo manual startup sequence (start)
    ////////////////////////////////////////////////
    // Safety: preload OUT1 volume with analog MUTE before enabling the HP output path.
    // The application must explicitly call wm8904_set_analog_output_mute(inst, false) to output sound.
    wm8904_write_hpout_level_mute(inst, true);

    #define WM8904_DCS_STARTUP_COMPLETE_ALL_MASK    (0x000F)
    #define WM8904_DCS_STARTUP_TIMEOUT_MS           (500)

    // Declick research (F / WSEQ_STARTUP): run the pop-critical HP output-enable via the vendor Write
    // Sequencer (Table 88 idx 12..22) with datasheet timing, instead of the manual sequence below. Clocks/
    // interface/BIAS/VMID/CP/PGA were already brought up manually, so TDM/ADC config is preserved. On WSEQ
    // timeout we fall back to the manual sequence.
    bool hp_enable_done = false;
    if( (WM8904_DECLICK_MASK() & WM8904_DECLICK_WSEQ_STARTUP) != 0u )
    {
        if( wm8904_wseq_hp_enable(inst) )
        {
            wm8904_capture_dc_servo(inst);   // WSEQ ran the STARTUP servo -> capture for WARM_SERVO
            hp_enable_done = true;
        }
        else
        {
            WM8904_FAIL(WM8904_E_WSEQ_HPEN_FELLBACK, inst, 0u);
            TRACE(" WM8904 WSEQ HP-enable fell back to manual inst=%d\n", inst);
        }
    }

    if( !hp_enable_done )
    {
        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,                               WM8904_HPL_ENA |
                                                                                    WM8904_HPR_ENA );
        delay_us(20);

        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                               WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
        wm8904_write_reg( inst, WM8904_DC_SERVO_0,             WM8904_DCS_ENA_CHAN_3 | WM8904_DCS_ENA_CHAN_2 | WM8904_DCS_ENA_CHAN_1 | WM8904_DCS_ENA_CHAN_0 );

        // Declick research (B): a warm restart restores the retained DC-servo offsets with DCS_TRIG_DAC_WR
        // (~2ms/ch) instead of a full STARTUP measurement (~86ms/ch). The cold path additionally CAPTURES the
        // measured offsets so a subsequent warm restart has values to restore. wm8904_apply_dc_servo_warm()
        // returns false only if the (guarded) offsets went stale, in which case we run the full STARTUP.
        if( !warm || !wm8904_apply_dc_servo_warm(inst) )
        {
            wm8904_write_reg( inst, WM8904_DC_SERVO_1,         WM8904_DCS_TRIG_STARTUP_3 | WM8904_DCS_TRIG_STARTUP_2 | WM8904_DCS_TRIG_STARTUP_1 | WM8904_DCS_TRIG_STARTUP_0 );
            if( !wm8904_wait_dc_servo_startup_done(inst,
                                                   WM8904_DCS_STARTUP_COMPLETE_ALL_MASK,
                                                   WM8904_DCS_STARTUP_TIMEOUT_MS) )
            {
                // For debug phase, continue the startup sequence even if timeout occurs.
                // The final register dump can show whether R4D is still incomplete.
            }
            else
            {
                wm8904_capture_dc_servo(inst);   // store measured offsets for future WARM_SERVO restores
            }
        }

        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,                                 WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                                                      WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
        wm8904_write_reg( inst, WM8904_ANALOGUE_HP_0,          WM8904_HPL_RMV_SHORT | WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                                                               WM8904_HPR_RMV_SHORT | WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA );
    }
    /////////////////////////////////////////////////
    // DC servo manual startup sequence (end)
    ////////////////////////////////////////////////


    ///////////////////////////
    // input gain settings
    ///////////////////////////
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
//org    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x5) ); // 00101 = +0.0 dB (default)
    // to reduce the hissing noise at A/D process
    wm8904_write_reg( inst, WM8904_ANALOGUE_LEFT_INPUT_0,    WM8904_LIN_VOL(0x0) ); // 00000 = -1.5 dB
    wm8904_write_reg( inst, WM8904_ANALOGUE_RIGHT_INPUT_0,   WM8904_RIN_VOL(0x0) ); // 00000 = -1.5 dB

    // to reduce hissing noise on input side
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_LEFT,  WM8904_ADC_VU | WM8904_ADCL_VOL(0xA5) );     // 0xA5 = -10.125 dB
    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_RIGHT, WM8904_ADC_VU | WM8904_ADCR_VOL(0xA5) );     // 0xA5 = -10.125 dB
//    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_LEFT,  WM8904_ADC_VU | WM8904_ADCL_VOL(0xC0) );     // 0xC0 = 0 dB
//    wm8904_write_reg( inst, WM8904_ADC_DIGITAL_VOLUME_RIGHT, WM8904_ADC_VU | WM8904_ADCR_VOL(0xC0) );     // 0xC0 = 0 dB


    ///////////////////////////
    // output gain settings
    ///////////////////////////
    // -6dB to reduce the hissing noise on output side.
//    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_LEFT,       WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(57-6) ); // -6dB
//    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_RIGHT,      WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(57-6) ); // -6dB
    // Startup default is analog MUTE. Keep muted until the application explicitly unmutes.
    wm8904_write_hpout_level_mute(inst, true);
    //  Gain[dB]=(VOL_CODE - 0xC0) * 0.375
    //  Gain[dB] / 0.375 + 0xC0(192) = VOL_CODE
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_LEFT,  WM8904_DAC_VU | WM8904_DACL_VOL(0xC0) ); // 0dB
    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_VOLUME_RIGHT, WM8904_DAC_VU | WM8904_DACR_VOL(0xC0) ); // 0dB


    delay_ms(20);


    wm8904_write_dac_digital_mute( inst, false, false );  // digital unmute
}


static void wm8904_write_dac_digital_mute(uint8_t inst, bool mute, bool ena96k)
{
    uint16_t data = 0;

    data = WM8904_DAC_MUTERATE    |
           WM8904_DAC_UNMUTE_RAMP |
           WM8904_DEEMPH(0);
    if( !ena96k )
    {
        data |= WM8904_DAC_OSR128;   // 96K config must be disabled.
    }

    if( mute )
    {
        data |= WM8904_DAC_MUTE;
    }

    wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1, data );
}


static void wm8904_write_hpout_level_mute(uint8_t inst, bool mute)
{
    uint16_t data_l = 0;
    uint16_t data_r = 0;

    data_l = WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(WM8904_HPOUT_VOL_DEFAULT);
    data_r = WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(WM8904_HPOUT_VOL_DEFAULT);

    if( mute )
    {
        data_l |= WM8904_HPOUTL_MUTE;
        data_r |= WM8904_HPOUTR_MUTE;
    }

    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_LEFT,  data_l );
    wm8904_write_reg( inst, WM8904_ANALOGUE_OUT1_RIGHT, data_r );
}


static bool wm8904_wait_dc_servo_startup_done(uint8_t inst, uint16_t mask, uint32_t timeout_ms)
{
    uint16_t data  = 0;
#if WM8904_TRACE_LEVEL >= 3
    uint32_t start = GetTicks();
#endif

    while( timeout_ms-- )
    {
        data = wm8904_read_reg(inst, WM8904_DC_SERVO_READBACK_0);

        if( (data & mask) == mask )
        {
            TRACE_V(" WM8904 DC servo startup done inst=%d R4D=0x%04x %ld(ms)\n", inst, data, GetTicks()-start);
            return true;
        }

        delay_ms(1);
    }

    WM8904_FAIL(WM8904_E_DCS_TIMEOUT, inst, data);
    TRACE(" WM8904 DC servo startup timeout!! inst=%d R4D=0x%04x\n", inst, data);

    return false;
}


//backup static bool wm8904_integrated_startup_sequence(uint8_t inst)
//backup {
//backup #define WM8904_WSEQ_STARTUP_INDEX       (0u)
//backup #define WM8904_WSEQ_STARTUP_TIMEOUT_MS  (500u)
//backup 
//backup     bool result = false;
//backup 
//backup     TRACE_V(" WM8904 default startup sequence start inst=%d @%ld\n",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     /*
//backup      * Default Start-Up sequence:
//backup      * - Requires MCLK/SYSCLK to be available.
//backup      * - Intended for DAC playback via headphone/line output.
//backup      * - Datasheet default assumes 12.288 MHz MCLK and configures 48 kHz playback.
//backup      * - Runs DC servo sequence for pop/click reduction.
//backup      */
//backup 
//backup     // Enable the Control Write Sequencer.
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_0,
//backup                       WM8904_WSEQ_ENA );
//backup 
//backup     // Start default startup sequence from index 0.
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_3,
//backup                       WM8904_WSEQ_START |
//backup                       WM8904_WSEQ_START_INDEX(WM8904_WSEQ_STARTUP_INDEX) );
//backup 
//backup     result = wm8904_wait_write_sequencer_done(inst,
//backup                                               WM8904_WSEQ_STARTUP_TIMEOUT_MS);
//backup 
//backup     // Datasheet quick start-up sequence unmutes DAC after WSEQ completion.
//backup     // For your current analog-mute policy, I would NOT unmute here automatically.
//backup     // Keep analog mute and let the application call wm8904_set_analog_output_mute().
//backup     //
//backup     // wm8904_write_reg( inst, WM8904_DAC_DIGITAL_1, 0x0000 );
//backup 
//backup     TRACE(" WM8904 default startup sequence %s inst=%d @%ld\n",
//backup           (result) ? "done" : "timeout",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     return result;
//backup }

//backup static bool wm8904_integrated_shutdown_sequence(uint8_t inst)
//backup {
//backup #define WM8904_WSEQ_SHUTDOWN_INDEX      (25u)
//backup #define WM8904_WSEQ_SHUTDOWN_TIMEOUT_MS (500u)
//backup 
//backup     bool result = false;
//backup 
//backup     TRACE_V(" WM8904 pre-reset shutdown sequence start inst=%d @%ld\n", inst, GetTicks());
//backup 
//backup     /*
//backup      * CPU reset does not reset WM8904.
//backup      * The codec may still be alive from the previous run.
//backup      *
//backup      * Do not directly touch OUT1 volume/mute registers here.
//backup      * Use the WM8904 default shutdown sequence instead, so the codec can
//backup      * shut down HP/charge pump/DC servo blocks in its intended order.
//backup      *
//backup      * Note:
//backup      * The write sequencer requires WM8904 SYSCLK/MCLK to be available.
//backup      * If it times out, continue to the normal software reset/config flow.
//backup      */
//backup 
//backup     // Enable the Control Write Sequencer clock, then start the default shutdown sequence.
//backup     wm8904_write_reg( inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA );
//backup 
//backup     wm8904_write_reg( inst,
//backup                       WM8904_WRITE_SEQUENCER_3,
//backup                       WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(WM8904_WSEQ_SHUTDOWN_INDEX) );
//backup 
//backup     result = wm8904_wait_write_sequencer_done(inst, WM8904_WSEQ_SHUTDOWN_TIMEOUT_MS);
//backup 
//backup     TRACE(" WM8904 pre-reset shutdown sequence %s inst=%d @%ld\n",
//backup           (result) ? "done" : "timeout",
//backup           inst,
//backup           GetTicks());
//backup 
//backup     return result;
//backup }


static bool wm8904_wait_write_sequencer_done(uint8_t inst, uint32_t timeout_ms)
{
    uint16_t data      = 0;
#if WM8904_TRACE_LEVEL >= 2
    uint32_t start     = GetTicks();
#endif
    bool     busy_seen = false;

    while( timeout_ms-- )
    {
        data = wm8904_read_reg(inst, WM8904_WRITE_SEQUENCER_4);

        // A failed I2C read returns RET_INVALID (0xCECE). Its bit0 is 0, so the WSEQ_BUSY test
        // below would otherwise misread an unreachable codec as "sequencer idle / done" -- a
        // bogus success. Treat a failed read as a sequencer failure so the caller
        // (wm8904_shutdown) falls back to the quench discharge instead of trusting it.
        //
        // Only THIS read's own outcome may decide that. s_io_ok[] used to be tested here as well,
        // but it is LATCHED: any earlier mismatch anywhere in the boot leaves it false, and it is
        // only re-armed inside wm8904_init_role() -- which this pre-shutdown runs BEFORE. That
        // aborted a wait on a sequencer that was demonstrably running: R70=0x01c1 (WSEQ_BUSY=1)
        // reported as "I2C read failed" on the retry after a leg-B apply failure (T2.8, 2026-08-11).
        if( data == RET_INVALID )
        {
            WM8904_FAIL(WM8904_E_WSEQ_READ_FAILED, inst, data);
            TRACE(" WM8904 WSEQ wait: I2C read failed inst=%d data=0x%04x\n", inst, data);
            return false;
        }

        if( (data & WM8904_WSEQ_BUSY) == 0u )
        {
            // BUSY==0 means "finished" only once the sequencer has been seen RUNNING. Without
            // SYSCLK the WSEQ cannot start at all, so BUSY never asserts and this read is the
            // START state, not the DONE state. Reporting success there skips the quench fallback
            // and leaves the analog HP block undischarged -- which is what then made every
            // ANALOGUE_HP_0 write fail its read-back verify (T2.8: "done inst=3 R70=0x0000 0(ms)"
            // against instance 2's 294 ms on the same boot). Fail, so the caller falls back.
            if( !busy_seen )
            {
                WM8904_FAIL(WM8904_E_WSEQ_NEVER_STARTED, inst, data);
                TRACE(" WM8904 WSEQ never started inst=%d R70=0x%04x %ld(ms)"
                      " -- no SYSCLK on this codec?\n",
                      inst,
                      data,
                      GetTicks()-start);
                return false;
            }

            TRACE_V(" WM8904 write sequencer done inst=%d R70=0x%04x %ld(ms)\n",
                  inst,
                  data,
                  GetTicks()-start);
            return true;
        }
        busy_seen = true;

        delay_ms(1);
    }

    WM8904_FAIL(WM8904_E_WSEQ_TIMEOUT, inst, data);
    TRACE(" WM8904 write sequencer timeout!! inst=%d R70=0x%04x\n", inst, data);

    return false;
}


static void wm8904_hpout_quench_before_startup(uint8_t inst)
{
    TRACE_V(" WM8904 HPOUT quench before startup inst=%d @%ld\n",
          inst,
          GetTicks());

    wm8904_write_reg(inst,
                     WM8904_ANALOGUE_HP_0,
                     WM8904_HPL_RMV_SHORT |
                     WM8904_HPR_RMV_SHORT);   // 0x0088

    delay_ms(100);

    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0, 0x0000);

    delay_ms(100);

    TRACE_V(" WM8904 HPOUT quench before startup done inst=%d @%ld\n",
          inst,
          GetTicks());
}


//===========================================================
// Declick research helpers (one-shot; see [internal] manual_wm8904_mute_restart_declick.md)
//===========================================================

// C: datasheet Table 42-ordered headphone disable. Step 1 clears RMV_SHORT (re-shorts the outputs to
// ground) while the output stages stay enabled; Step 2 clears all ENA/ENA_DLY/ENA_OUTP bits. This is the
// vendor pop-suppressed disable, versus the baseline quench which sets RMV_SHORT=1 first. VMID and the
// charge pump are intentionally left up here (same as the quench) -- the following config re-uses them.
static void wm8904_hpout_ordered_disable(uint8_t inst)
{
    TRACE_V(" WM8904 ordered HP disable (Table42) inst=%d @%ld\n", inst, GetTicks());

    // Step 1: RMV_SHORT = 0, keep ENA / ENA_DLY / ENA_OUTP set (assumes a running/enabled output).
    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0,
                     WM8904_HPL_ENA_OUTP | WM8904_HPL_ENA_DLY | WM8904_HPL_ENA |
                     WM8904_HPR_ENA_OUTP | WM8904_HPR_ENA_DLY | WM8904_HPR_ENA);   // 0x0077
    delay_ms(1);

    // Step 2: all ENA bits off.
    wm8904_write_reg(inst, WM8904_ANALOGUE_HP_0, 0x0000);
    delay_ms(1);
}

// B (capture): store the measured DC-servo offsets (R73..R76 readback = current offset) so a later
// WARM_SERVO restart can reload them with DCS_TRIG_DAC_WR. Called after a completed STARTUP servo.
static void wm8904_capture_dc_servo(uint8_t inst)
{
    if( inst >= WM8904_INST_MAX ) { return; }

    s_dcs_val[inst][0] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_6) & 0x00FFu );  // LINEOUTR
    s_dcs_val[inst][1] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_7) & 0x00FFu );  // LINEOUTL
    s_dcs_val[inst][2] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_8) & 0x00FFu );  // HPOUTR
    s_dcs_val[inst][3] = (uint8_t)( wm8904_read_reg(inst, WM8904_DC_SERVO_9) & 0x00FFu );  // HPOUTL
    s_dcs_valid[inst]  = true;

    TRACE_V(" WM8904 DC servo captured inst=%d [%02x %02x %02x %02x]\n",
          inst, s_dcs_val[inst][0], s_dcs_val[inst][1], s_dcs_val[inst][2], s_dcs_val[inst][3]);
}

// B (restore): reload the captured offsets and trigger the fast DAC-write servo mode (~2ms/ch) instead
// of a full STARTUP measurement (~86ms/ch). Returns false if no capture is available (caller then runs
// the STARTUP path); true once applied, even if the completion poll times out (still on the warm path).
static bool wm8904_apply_dc_servo_warm(uint8_t inst)
{
    if( inst >= WM8904_INST_MAX || !s_dcs_valid[inst] ) { return false; }

    wm8904_write_reg(inst, WM8904_DC_SERVO_6, s_dcs_val[inst][0]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_7, s_dcs_val[inst][1]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_8, s_dcs_val[inst][2]);
    wm8904_write_reg(inst, WM8904_DC_SERVO_9, s_dcs_val[inst][3]);

    wm8904_write_reg(inst, WM8904_DC_SERVO_1,
                     WM8904_DCS_TRIG_DAC_WR_3 | WM8904_DCS_TRIG_DAC_WR_2 |
                     WM8904_DCS_TRIG_DAC_WR_1 | WM8904_DCS_TRIG_DAC_WR_0);

    // DAC-write completion is reported in R77[7:4] (DCS_DAC_WR_COMPLETE); reuse the readback waiter.
#if WM8904_TRACE_LEVEL >= 3
    const bool done = wm8904_wait_dc_servo_startup_done(inst, WM8904_DCS_DAC_WR_COMPLETE_Msk, 100u);
    TRACE_V(" WM8904 DC servo WARM restore inst=%d done=%d\n", inst, (int)done);
#else
    (void)wm8904_wait_dc_servo_startup_done(inst, WM8904_DCS_DAC_WR_COMPLETE_Msk, 100u);
#endif
    return true;
}

// D: stepped (soft) HPOUT analog unmute. Clears the mute by climbing the OUT1 volume from a lower level
// to the default in small steps, so the mute release is a short ramp instead of an instantaneous jump.
// HPOUT_VU commits the L/R volume update. The DAC digital path is already soft-unmuted (DAC_UNMUTE_RAMP).
static void wm8904_hpout_ramp_unmute(uint8_t inst)
{
    const uint8_t target = (uint8_t)WM8904_HPOUT_VOL_DEFAULT;                 // -6 dB (51)
    const uint8_t start  = ( target > 24u ) ? (uint8_t)( target - 24u ) : 0u; // ~ -15 dB below target
    const uint8_t step   = 3u;

    for( uint8_t v = start; v < target; v = (uint8_t)( v + step ) )
    {
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(v));
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(v));
        delay_ms(6);
    }
    wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(target));
    wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(target));
}

// E: stepped HPOUT gain ramp-DOWN before the hard mute/shutdown. Lowers OUT1 volume from the default
// operating level toward minimum in small steps, then latches the HPOUT mute bit, so the output is
// already near-silent when the teardown/discharge follows. Mirror of wm8904_hpout_ramp_unmute().
static void wm8904_hpout_ramp_mute(uint8_t inst)
{
    const uint8_t target = (uint8_t)WM8904_HPOUT_VOL_DEFAULT;                 // -6 dB (51) operating level
    const uint8_t floor   = ( target > 24u ) ? (uint8_t)( target - 24u ) : 0u; // ~ -15 dB below target
    const uint8_t step    = 3u;

    for( uint8_t v = target; v > floor; v = ( v > step ) ? (uint8_t)( v - step ) : 0u )
    {
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_LEFT,  WM8904_HPOUT_VU | WM8904_HPOUTL_VOL(v));
        wm8904_write_reg(inst, WM8904_ANALOGUE_OUT1_RIGHT, WM8904_HPOUT_VU | WM8904_HPOUTR_VOL(v));
        delay_ms(6);
    }
    // Final hard mute at the floor level (sets HPOUTx_MUTE via the shared writer).
    wm8904_write_hpout_level_mute(inst, true);
}

// A: run the WM8904 vendor Control Write Sequencer default shutdown (index 0x19, Table 89) -- the ordered
// RMV_SHORT -> ENA -> DCS -> CP -> DAC -> CLK -> PGA -> BIAS -> VMID power-down with datasheet timing.
// Requires SYSCLK to be present. Returns false on sequencer timeout so the caller can fall back to quench.
// NOTE: only the WSEQ *shutdown* is wired; startup stays on the manual Table 41 sequence (which already
// matches the vendor enable order and, unlike the fixed-48k WSEQ startup, honours the TDM/rate/ADC config).
static bool wm8904_wseq_shutdown(uint8_t inst)
{
    TRACE_V(" WM8904 WSEQ shutdown start inst=%d @%ld\n", inst, GetTicks());

    // The vendor shutdown sequence steps VMID/CP/CLK/PGA/BIAS power-down under SYSCLK. If SYSCLK is
    // sourced from the FLL (44.1k family) the sequence disturbs/powers down the FLL and kills its own
    // clock mid-run -> WSEQ_BUSY never clears -> 600 ms timeout, and the analog HP block is left wedged
    // (subsequent ANALOGUE_HP_0 writes then fail their read-back verify). Coming from 48k the source is
    // MCLK, which is always present on the codec-master XTAL, so the sequence completes (~290 ms) -- that
    // is the observed 44.1k->48k-only failure. Point SYSCLK at MCLK before starting the sequencer so it
    // has a stable clock for the whole power-down regardless of the current rate. Datasheet p.100: set
    // CLK_SYS_ENA=0 before changing SYSCLK_SRC. R22 (CLOCK_RATES_2) is always accessible; no readback
    // needed. wm8904_config reprograms the clock source fully on the following (re)configure.
    wm8904_write_reg(inst, WM8904_CLOCK_RATES_2, 0x0000);                              // CLK_SYS_ENA=0 (allow source switch)
    wm8904_write_reg(inst, WM8904_CLOCK_RATES_2,
                     WM8904_CLK_SYS_ENA | WM8904_CLK_DSP_ENA);                         // SYSCLK_SRC=0 (MCLK), re-enable

    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA);                 // enable sequencer clock
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_3,
                     WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(0x19u));              // start at shutdown index
    delay_ms(1);                                                                       // let WSEQ_BUSY assert
    return wm8904_wait_write_sequencer_done(inst, 600u);
}

// F: run only the HP/LINEOUT output-enable portion of the vendor startup sequence (Table 88 indices
// 12..22): HP_ENA -> ENA_DLY -> DCS_ENA -> DCS_TRIG_STARTUP -> ENA_OUTP -> RMV_SHORT, with the datasheet
// inter-step timing and the ~256ms DC-servo wait. The caller has already brought up clocks/interface/
// BIAS/VMID/CP/PGA manually (so TDM/ADC config is preserved); this only replaces the pop-critical output
// bring-up. Needs SYSCLK. Returns false on sequencer timeout so the caller can fall back to the manual seq.
static bool wm8904_wseq_hp_enable(uint8_t inst)
{
    TRACE_V(" WM8904 WSEQ HP-enable (idx12) start inst=%d @%ld\n", inst, GetTicks());
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_0, WM8904_WSEQ_ENA);
    wm8904_write_reg(inst, WM8904_WRITE_SEQUENCER_3,
                     WM8904_WSEQ_START | WM8904_WSEQ_START_INDEX(0x0Cu));   // start at HP_ENA (index 12)
    delay_ms(1);
    return wm8904_wait_write_sequencer_done(inst, 600u);
}
