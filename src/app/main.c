
//===========================================================
// INCLUDES
//===========================================================
#include "app_specific_config_defs.h"
#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include "board/clock/sonora_clock.h"
#include "nora_clock.h"
#include "nora_gpio.h"
#include "nora_pps.h"
#include "nora_uart.h"
#include "timer_app.h"
#include "nora_high_res_timer.h"
#include "uart_platform_uart1_usb_serial_port.h"
#include "uart_platform_uart2_usb_serial_device.h"
#include "board/devices/app_i2c.h"
#include "board/devices/wm8904.h"

#include "app_console.h"
#include "app_debug.h"
#include "audio_transport/audio_transport.h"
#include "board/devices/button_led.h"
#include "hal_touch/nora_touch.h"
#include "board/devices/pot_drv.h"
#include "apps/sonora_app.h"
#include "board/board_dbg_pins.h"
#include "pwm_led.h"
#include "nora_dma.h"
#if defined(ENA_DMA_SELFTEST)
#include "dma_selftest.h"     // NORA hal_dma conformance gate; see the call site below
#endif
#include "nora_udid.h"        // board-individual ID (UDID) for the boot banner / *nt04
#include "hal_reset/nora_reset.h"       // core reset cause (RCON) latched at boot; drives cold/hot start
#include "diagnostics/app_traps.h"      // trap latch in surviving RAM; reported after UART init
#include "diagnostics/app_silicon.h"    // DEVID/DEVREV in the banner; optional revision gate
// Everything this image owes the resident download engine that launched it. Empty
// inlines in a standalone build, so no delivery-mode #if is needed below.
#include "resident_de/app/resident_de_app_handoff.h"
#if defined(ENA_CMSIS_USART)
#include "Driver_USART_dsPIC33AK.h"
#endif


#include "main.h"


//===========================================================
// Definition
//===========================================================

// #define ENA_SMOOTH_TRANS_MS       (200)

// Boot-fault codes shown on the LED bank by LED_fault_indicate_forever():
// LED0 = heartbeat, LED1..LEDn = this value in binary (bit0 -> LED1).
enum
{
    BOOT_FAULT_CLOCK       = 1u,  // sonora_clock_boot_init() failed
    BOOT_FAULT_USART_INIT  = 2u,  // Driver_USART1.Initialize() failed
    BOOT_FAULT_USART_POWER = 3u,  // Driver_USART1.PowerControl() failed
};


//-----------------------------------------------------------------------
// DIMpin  CPUpin  I2C#    other function
//-----------------------------------------------------------------------
// I2C-1
// DIM-P5  57      SCL1    PGC1/AD4AN0/CVDAN20/CVDTX4/CMP4B/RP21/SCL1/RB4
// DIM-P7  55      SDA1    PGD1/AD4AN3/CVDAN19/CVDTX3/CMP4A/RP20/SDA1/RB3
// I2C-2
// DIM-P77 4       SCL2    PGD2/AD3AN5/CVDAN10/CMP6A/RP1/SCL2/IOMAF2/RA0
// DIM-P66 5       SDA2    AD5ANN1/AD5AN0/CVDAN7/CMP6B/RP8/SDA2/IOMAF1/RA7
// I2C-2(alternative)
// DIM-P67 124     ASCL2   CVDTX17/RP56/ASCL2/IOMAF4/RD7     (Curiosity PCB: MikroBus A)
// DIM-P65 125     ASDA2   CVDTX18/RP57/ASDA2/IOMAF3/RD8     (Curiosity PCB: MikroBus A)

// Curiosity PCB
//  DIM-P67 SCL  (MikroBus A)
//  DIM-P65 SDA  (MikroBus A)
//
// dsPIC33AK512 mini PCB
//  DIM-P67 124pin  ASCL2    CVDTX17/RP56/ASCL2/IOMAF4/RD7
//  DIM-P65 125pin  ASDA2    CVDTX18/RP57/ASDA2/IOMAF3/RD8
//
// dsPIC33AK128 mini PCB
//  DIM-P67 57pin   ASCL2    RP56/ASCL2/IOMD7/IOMF4/RD7
//  DIM-P65 58pin   ASDA2    RP57/ASDA2/IOMD6/IOMF3/RD8
//
//
// I2C-1 (alternative)
// DIM-P6   55pin   ASCL1   RP54/ASCL1/RD5   (Curiosity: mikroBUS B SCL)
// DIM-P4   56pin   ASDA1   RP55/ASDA1/RD6   (Curiosity: mikroBUS B SDA)
//
// The former DIM-P19/P17 mikroBUS-B comment was incorrect for the AK128 DIM.
// B control uses I2C1 alternate on DIM-P4/P6.
//
// The RP and RDx values above were also wrong until 2026-08-17: this comment
// had RP19/RD6 for P6 and RP17/RD5 for P4, i.e. both the RP numbers and the
// RD5/RD6 pairing swapped and wrong.  The device pin numbers (55/56) were
// right.  Corrected against the official dsPIC33AK128MC106 General Purpose DIM
// Information Sheet (DS70005556B), Table 1 and Table 2.  No functional effect
// -- I2C1's alternate pads are selected by the FDEVOPT_ALTI2C1 fuse below, not
// by PPS, so nothing read these numbers -- but RP17/RP19 belong to DIM-P2/P3
// and the wrong text here did send one pin-map audit down a false trail.
//
// Codec B's TDM pins remain DIM-NC.  They are reached only by the dedicated
// Curiosity J3 jumper capability, which lands them on the four mikroBUS-A pins
// a WM8904 mikroBUS board leaves unused; see config_pins_SPI_2_AK128() in
// board/audio/audio.c for that map and its hardware precondition.
//
// How to configure the alternative pins
// 1. See MPLAB X [MENU Bar] > Production > Set Configuration Bits
// 2. Set "ON" at FDEVOPT::ALTI2C2
// 3. Click button of Generate Source code to Output
// 4. Copy and paste the necessary part into my source like below.
//
#if APP_AK128_J3_TDM_B
#pragma config FDEVOPT_ALTI2C1 = ON      // Alternate I2C1 pins: DIM-P4 / DIM-P6 (MikroBUS B).
#endif
#pragma config FDEVOPT_ALTI2C2 = ON     // Alternate I2C2 pins selection bit.
//#pragma config FDEVOPT_ALTI2C3 = ON     // Alternate I2C3 pins selection bit.

// --- Firmware delivery layout ---
// Single Boot for both delivery modes: a serial-update build runs a resident
// bootloader + one relocated application in the single panel; standalone builds occupy the
// whole panel as one ordinary image. Flash Dual Partition / BTMODE=DUAL was an earlier
// A/B LiveUpdate plan ([internal] resident_serial_bootloader_research.md appendix,
// superseded by the resident bootloader) -- its firmware (fw_update/, fw_console.c,
// the UCA/BTSEQ fail-closed audio gate) has been retired along with it; do not
// reintroduce BTMODE=DUAL without rebuilding that support.
// dsPIC33AK128MC106 has no Flash Dual Partition feature and rejects these config
// settings outright, so they are AK512-only.
#if defined(__dsPIC33AK512MPS512__)
#pragma config BTMODE  = SINGLE         // Device Boot Mode: Single Boot
#pragma config NOBTSWP = OFF            // BOOTSWP is not used in single-panel mode
#endif


//===========================================================
// Enum & Struct typedef
//===========================================================




//===========================================================
// Function Prototype
//===========================================================

// Audio transport lifecycle (HAL-direct or CMSIS-SAI wrapper) lives in audio_transport/.
// main.c only selects the route and pumps the selected top-level manage function.

static void   high_res_timer_boot_test( nora_high_res_timer_status_t init_status );
static void   term_init_safe( void );
static void   clearTerminalScreen( void );
// static void   moveCursor( int row );
// static void   moveCursorToLineStart( void );
// static void   moveCursorUp( void );
// static void   hideCursor( void );
static void   reset_console( void );
static void   printMenu( void );
static void   boot_banner_hold_if_requested( void );

static void   init_ports( void );
static nora_high_res_timer_status_t init_timers( void );

#if SONORA_APP_IS_CLASSIC
static void   dbg_RGB_pot( void );
#else
static void   set_rgb_idle_color( void );
#endif
static void   dbg_print( void );
// static void   dbg_print_pot( void );
// static int    adc_to_step(uint16_t adc);


//===========================================================
// Variables
//===========================================================



// debug purpose
volatile uint16_t g_dbg_default_INTCON1 = 0;
volatile uint16_t g_dbg_default_INTCON2 = 0;
volatile uint32_t g_dbg_default_INTTREG = 0;
volatile uint32_t g_dbg_default_IEC0    = 0;
volatile uint32_t g_dbg_default_IFS0_32 = 0;
volatile uint32_t g_dbg_default_RAMXECCSTAT = 0;
volatile uint32_t g_dbg_default_RAMXECCFADDR = 0;
volatile uint32_t g_dbg_default_PCTRAP = 0;
volatile uint32_t g_dbg_default_PCHOLD = 0;
volatile uint32_t g_dbg_default_VFA = 0;
volatile uint32_t g_dbg_default_count   = 0;
volatile uint16_t g_dbg_default_IFS0    = 0;
volatile uint16_t g_dbg_default_IFS1    = 0;
volatile uint16_t g_dbg_default_IFS2    = 0;
volatile uint16_t g_dbg_default_IFS3    = 0;
volatile uint16_t g_dbg_default_IFS4    = 0;
volatile uint16_t g_dbg_default_IFS5    = 0;
volatile uint16_t g_dbg_default_IFS6    = 0;
volatile uint16_t g_dbg_default_IFS7    = 0;






//===========================================================
// Global Function
//===========================================================

/*
 * ONE CALL DEEP, so the VECTOR itself carries no prologue push.
 *
 * `context` banks W0-W7 only (DS70005591D: seven alternate W0-W7 arrays, each inherently
 * tied to an IPL). This body latches ~20 SFRs and then printf's, which needs far more than
 * W0-W7, so inlined into the vector it opened with 12 `mov.l wN,[w15++]` -- measured the
 * most of any ISR in every configuration. A prologue push at an ISR's first instruction is
 * the documented trigger of the A1 silicon STACK ERROR; see the DO-NOT-REVERT note above
 * _CCP1Interrupt in apps/asrc/asrc_clock_control.c, and the per-vector bodies in
 * hal_spi_i2s_tdm/nora_spi_i2s_tdm_dspic33ak.c for the same remedy applied to W8 saves
 * that `context` cannot remove.
 *
 * The pushes still happen, inside an ordinary function, which is the point: this vector is
 * the catch-all that has to survive long enough to NAME an uninstalled vector. Taking a
 * STACK ERROR on the way in would replace that diagnosis with a misleading one.
 *
 * (The printf-and-spin shape is itself questionable for the reasons given about
 * _AddressErrorTrap below, but that is a separate change and is left alone here.)
 */
/*
 * A1-SILICON GATE for the ISR-prologue workaround below.
 *
 * `context` is NOT gated and must not be: the alternate W0-W7 array is inherently tied to
 * the IPL on this core (DS70005591D), so declaring it is a statement of hardware fact that
 * is correct on every revision. What IS revision-specific is the extra indirection that
 * moves the W8+ saves OUT of the vector prologue, because its only purpose is to avoid the
 * A1 STACK ERROR trigger and it costs one rcall + return per interrupt.
 *
 * Default 1 = keep the workaround. Set to 0 (-D APP_A1_ISR_STACK_WORKAROUND=0) to build the
 * direct form and A/B it -- which is how this should be re-evaluated on A2 silicon, where
 * the exit condition for the whole A1 STACK ERROR investigation already lives. The die
 * revision is a RUNTIME fact (app_silicon.c prints it in the boot banner), so this cannot
 * key off it automatically; it is a deliberate build choice.
 *
 * Rationale and measurements: [internal] isr_w15_audit_2026-08-27.md and the
 * DO-NOT-REVERT note above _CCP1Interrupt in src/app/apps/asrc/asrc_clock_control.c.
 */
#ifndef APP_A1_ISR_STACK_WORKAROUND
#define APP_A1_ISR_STACK_WORKAROUND (1)
#endif

#if APP_A1_ISR_STACK_WORKAROUND
static void __attribute__((noinline)) app_default_interrupt_body(void);

void __attribute__( ( interrupt, context ) ) _DefaultInterrupt(void)
{
    app_default_interrupt_body();
}

static void __attribute__((noinline)) app_default_interrupt_body(void)
#else
void __attribute__( ( interrupt, context ) ) _DefaultInterrupt(void)
#endif
{
    g_dbg_default_count++;

    g_dbg_default_INTCON1 = INTCON1;
    g_dbg_default_INTCON2 = INTCON2;
    g_dbg_default_INTTREG = INTTREG;
    g_dbg_default_IEC0 = IEC0;
    g_dbg_default_IFS0_32 = IFS0;
    g_dbg_default_RAMXECCSTAT = RAMXECCSTAT;
    g_dbg_default_RAMXECCFADDR = RAMXECCFADDR;
    g_dbg_default_PCTRAP = PCTRAP;
    g_dbg_default_PCHOLD = PCHOLD;
    g_dbg_default_VFA = VFA;

    g_dbg_default_IFS0 = IFS0;
    g_dbg_default_IFS1 = IFS1;
    g_dbg_default_IFS2 = IFS2;
    g_dbg_default_IFS3 = IFS3;
    g_dbg_default_IFS4 = IFS4;
    g_dbg_default_IFS5 = IFS5;
    g_dbg_default_IFS6 = IFS6;
    g_dbg_default_IFS7 = IFS7;

    

    // unexpected
    printf(" _DefaultInterrupt: vector=%lu INTTREG=0x%lx INTCON1=0x%x INTCON2=0x%x\n",
              (unsigned long)(g_dbg_default_INTTREG & 0x1ffu),
              (unsigned long)g_dbg_default_INTTREG,
              (unsigned int)g_dbg_default_INTCON1,
              (unsigned int)g_dbg_default_INTCON2);
    printf(" IEC0=0x%lx IFS0=0x%lx RAMXECCSTAT=0x%lx FADDR=0x%lx\n",
              (unsigned long)g_dbg_default_IEC0,
              (unsigned long)g_dbg_default_IFS0_32,
              (unsigned long)g_dbg_default_RAMXECCSTAT,
              (unsigned long)g_dbg_default_RAMXECCFADDR);
    printf(" PCTRAP=0x%lx PCHOLD=0x%lx VFA=0x%lx\n",
              (unsigned long)g_dbg_default_PCTRAP,
              (unsigned long)g_dbg_default_PCHOLD,
              (unsigned long)g_dbg_default_VFA);
    while(1)
    {
        Nop();
    }

}

/*
 * _AddressErrorTrap USED TO BE HERE. It read INTCON1/2 into globals, printf'd them, and
 * then spun forever. Both halves of that were wrong in the same way: printf from a trap
 * calls the UART with an unknown stack and a possibly-corrupt W set -- if it faults, the
 * evidence is gone -- and the spin left a board with no reset button looking like a dead
 * COM port. It now lives with the other five vectors in diagnostics/app_traps.c, which
 * latches the fault registers to surviving RAM and resets so the next boot can print them.
 */

/* Length of the rate cross-check window, in milliseconds, and the tolerance
 * applied to it, in percent.
 *
 * "The counter advances" is not enough to trust a load figure: a wrong prescale
 * or clock source still advances, just 4x, 16x or 64x off, and then every
 * microsecond and percentage derived from it is wrong by that factor with
 * nothing on the console to say so.  So the count is also compared against the
 * Timer1 millisecond tick, which is already running by the time this test is
 * reached.  50 ms keeps the tick's +0..+1 ms quantisation under 2 %, so a 5 %
 * window passes a healthy timer and still rejects the smallest factor error. */
#define HIGH_RES_TIMER_RATE_WINDOW_MS     (50u)
#define HIGH_RES_TIMER_RATE_TOLERANCE_PCT (5u)

static void high_res_timer_boot_test( nora_high_res_timer_status_t init_status )
{
    const uint32_t rate_expect =
        (uint32_t)( ( (uint64_t)(FCY) * HIGH_RES_TIMER_RATE_WINDOW_MS ) / 1000u );
    const uint32_t rate_tolerance =
        (uint32_t)( ( (uint64_t)rate_expect * HIGH_RES_TIMER_RATE_TOLERANCE_PCT ) / 100u );
    const uint32_t count_to_us_100 = nora_high_res_timer_count_to_us( 100u );
    const uint32_t count_to_us_x10_10 = nora_high_res_timer_count_to_us_x10( 10u );
    const uint32_t count_to_us_x10_100 = nora_high_res_timer_count_to_us_x10( 100u );
    uint32_t count0;
    uint32_t count1;
    uint32_t count2;
    uint32_t count3;
    uint32_t delta1;
    uint32_t delta10;
    uint32_t delta_rate;
    bool conversion_ok;
    bool counter_ok;
    bool rate_ok;
    bool status_ok;

    count0 = nora_high_res_timer_get_count();
    delay_ms( 1 );
    count1 = nora_high_res_timer_get_count();
    delay_ms( 10 );
    count2 = nora_high_res_timer_get_count();
    delay_ms( (uint16_t)HIGH_RES_TIMER_RATE_WINDOW_MS );
    count3 = nora_high_res_timer_get_count();

    delta1 = count1 - count0;
    delta10 = count2 - count1;
    delta_rate = count3 - count2;
    conversion_ok = (count_to_us_100 == 1u) &&
                    (count_to_us_x10_10 == 1u) &&
                    (count_to_us_x10_100 == 10u);
    counter_ok = (count1 != count0) && (count2 != count1);
    rate_ok = (delta_rate >= (rate_expect - rate_tolerance)) &&
              (delta_rate <= (rate_expect + rate_tolerance));
    status_ok = (init_status == NORA_HIGH_RES_TIMER_OK) &&
                nora_high_res_timer_is_present() &&
                nora_high_res_timer_is_initialized();

    printf("[HRT] init=%d present=%d initialized=%d clk=%luHz\n",
           (int)init_status,
           (int)nora_high_res_timer_is_present(),
           (int)nora_high_res_timer_is_initialized(),
           (unsigned long)(uint32_t)(FCY));
    printf("[HRT] count0=%lu count1=%lu count2=%lu d1=%lu d10=%lu\n",
           (unsigned long)count0,
           (unsigned long)count1,
           (unsigned long)count2,
           (unsigned long)delta1,
           (unsigned long)delta10);
    printf("[HRT] conv:100cnt=%luus 10cnt=%lu(x0.1us) 100cnt=%lu(x0.1us)\n",
           (unsigned long)count_to_us_100,
           (unsigned long)count_to_us_x10_10,
           (unsigned long)count_to_us_x10_100);
    printf("[HRT] rate:%ums=%lu expect=%lu tol=%lu %s\n",
           (unsigned)HIGH_RES_TIMER_RATE_WINDOW_MS,
           (unsigned long)delta_rate,
           (unsigned long)rate_expect,
           (unsigned long)rate_tolerance,
           rate_ok ? "OK" : "BAD");
    printf("[HRT] self-check: %s\n",
           (status_ok && counter_ok && conversion_ok && rate_ok)
               ? "PASS"
               : "FAIL");
}

static nora_high_res_timer_status_t init_timers( void )
{
    const nora_high_res_timer_config_t high_res_timer_config = {
        .timer_clk_hz = (uint32_t)(FCY),
        .run_in_idle = true,
    };

    timer_app_init();

    return nora_high_res_timer_init( &high_res_timer_config );
}




/*
 * UART init status, captured so a failed bring-up is observable from the
 * debugger. Deliberately NOT reported via printf(): stdio retargets to UART1,
 * so if UART1 init failed there is no console to print to (this was the AK128
 * "no output" root cause). Inspect these instead of trusting the port.
 *
 * Scope: g_uart2_init_status is always written (UART2_Initialize runs on every
 * build). g_uart1_init_status reflects the legacy UART1_Initialize() path only;
 * under ENA_CMSIS_USART, UART1 is brought up via Driver_USART1 (which halts on
 * error, see below) and this variable stays at its NORA_UART_OK default.
 */
volatile nora_uart_status_t g_uart1_init_status = NORA_UART_OK;
volatile nora_uart_status_t g_uart2_init_status = NORA_UART_OK;

#if APP_RAMFUNC_C_PROBE
static volatile uint32_t s_ramfunc_probe_result;

static uint32_t __attribute__((ramfunc, noinline)) ramfunc_probe( uint32_t x )
{
    return x * 3u + 1u;
}
#endif

int main(void)
{
    nora_high_res_timer_status_t high_res_timer_status =
        NORA_HIGH_RES_TIMER_OK;

    // A resident handoff is a branch, not a reset: finish the interrupt-controller
    // reset the branch skipped, before anything else runs. No-op when this image was
    // not launched by the resident engine.
    resident_de_app_handoff_entry();

    // FIRST thing: snapshot + clear the core reset cause (RCON) while it is still
    // pristine, before any peripheral bring-up. The audio boot path reads this to
    // choose a cold vs hot codec start (see hal_reset/nora_reset.h). Only
    // touches the RCON SFR, so it is safe before the clock is configured.
    // This board latches and clears, so it is allowed to name a single cause; the
    // return value is ignored on purpose -- capture() refusing means someone already
    // captured, which is exactly the state this call wants.
    // In a delivery image the resident engine saw the real cause and RCON no longer holds
    // it, so take the forwarded word first. An empty inline returning false in a
    // standalone image, and false as well on a resident too old to forward one -- either
    // way the capture below is then the answer, exactly as before.
    (void)resident_de_app_latch_forwarded_reset_cause();
    (void)nora_reset_snapshot_capture( NORA_RESET_LATCH_AND_CLEAR_RCON );

    // Immediately after the snapshot, and before anything that could trap: decide whether
    // the trap latch in surviving RAM is evidence or garbage. Only a power-on/brown-out
    // makes it garbage, and only the snapshot above can tell -- which is why this call is
    // here and not inside app_traps.c. The report itself waits for the console, further
    // down; a trap vector must never print.
    app_traps_boot_prepare( nora_reset_snapshot_is_power_on_class() );

    init_ports();

    resident_de_app_handoff_mark('P');

    if( !sonora_clock_boot_init() )
    {
        /*
         * No fallback, by design: a second PLL1 configuration is exactly what
         * makes this failure unrecoverable (see the clock design contract). So
         * report and stop -- never limp on an unintended clock.
         *
         * Report best-effort, printf first: the CPU is still alive on the
         * reset-default FRC here, so UART1 can be brought up against the FRC and
         * say what happened. If even that fails, the LED code is the backstop and
         * names WHICH step died -- PLL1 itself versus a specific CLKGEN. See
         * SONORA_CLOCK_FAIL_* in sonora_clock.h.
         */
        unsigned stage = sonora_clock_boot_fail_stage();

        if( UART1_InitializeBootFaultConsole() == NORA_UART_OK )
        {
            printf("\n\n*** BOOT FAULT: clock bring-up failed, stage=%u ***\n", stage);
            printf("PLL1<-FRC did not come up. This is NOT recovered by a reset:\n");
            printf("a PLL only re-locks to the configuration it was already locked\n");
            printf("to, and the failed request latches. POWER-CYCLE the board.\n");
            printf("(console is %u baud here, not %u -- PLL1 is unavailable)\n",
                   (unsigned)UART_PLATFORM_BOOT_FAULT_BAUD, (unsigned)UART_BRG);
            printf("See the clock bring-up notes in src/board/clock/.\n");
        }

        LED_fault_indicate_forever( (stage != SONORA_CLOCK_FAIL_NONE)
                                    ? stage
                                    : (unsigned)BOOT_FAULT_CLOCK );   // never returns
    }

    resident_de_app_handoff_mark('C');


    ////////////////////////
    // Init Basic functions
    ////////////////////////
    high_res_timer_status = init_timers();

    resident_de_app_handoff_mark('T');

#if defined(ENA_CMSIS_USART)
    if( Driver_USART1.Initialize(NULL) != ARM_DRIVER_OK )
    {
        LED_fault_indicate_forever( BOOT_FAULT_USART_INIT );   // never returns
    }
    if( Driver_USART1.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK )
    {
        (void)Driver_USART1.Uninitialize();
        LED_fault_indicate_forever( BOOT_FAULT_USART_POWER );   // never returns
    }
#else
    g_uart1_init_status = UART1_Initialize();
#endif
    g_uart2_init_status = UART2_Initialize();

    // Timer and UART state are installed, so global interrupts can come back --
    // the resident engine handed over with GIE masked. No-op on a hardware reset.
    resident_de_app_handoff_interrupts_resume();
    delay_ms(10);

    resident_de_app_handoff_report();

    app_console_init();

    reset_console();
    // The console is up, so the latch from before this boot can finally be printed. Quiet
    // when there is nothing latched, so a clean boot looks exactly as it did.
    app_traps_report_previous();
    printMenu();

#if APP_RAMFUNC_C_PROBE
    s_ramfunc_probe_result = ramfunc_probe( 7u );
    printf(" RAMFUNC C probe: result=%lu expected=22 %s\n",
           (unsigned long)s_ramfunc_probe_result,
           (s_ramfunc_probe_result == 22u) ? "PASS" : "FAIL" );
#endif

    if( !BUTTON_Init() )
    {
        printf(" WARNING: BUTTON_Init: one or more button pin configs failed\n");
    }
    if( !LEDs_Init() )
    {
        printf(" WARNING: LEDs_Init: one or more LED pin configs failed\n");
    }
    LED_Off( 0xFF );    // all off

    // Boot-banner visibility: on a power-on-class reset the USB-CDC terminal is
    // usually still enumerating when the early printMenu() above prints, so hold
    // the boot button at power-on to have the banner re-printed for a short window.
    // (No-op / single early banner = fast boot when the button is not held. Warm
    // resets are never delayed.) Placed after BUTTON_Init()/LEDs_Init().
    boot_banner_hold_if_requested();


    high_res_timer_boot_test( high_res_timer_status );

    if( high_res_timer_status != NORA_HIGH_RES_TIMER_OK )
    {
        printf("High-resolution timer init failed: %d\n", (int)high_res_timer_status);
    }

    // DMA controller global init (DMA ON + allowed address window).
    // Must run before any SPI-TDM / PWM DMA channel config.
    nora_dma_global_init();

#if defined(ENA_DMA_SELFTEST)
    // NORA hal_dma conformance gate -- section 4 of
    // [internal] nora_dma_tdm_contract_draft_2026-08-10.md. This is the CK
    // application src/app/dma_selftest.c running here; what it proves is not "the DMA
    // works" (the audio path already proves that) but that the application MOVED.
    //
    // Position is deliberate: after nora_dma_global_init() because the test asserts
    // nora_dma_global_is_ready(), and before any transport/PWM channel is configured,
    // so borrowing a channel is safe -- the test leaves it disabled with status and
    // IRQ flag cleared. Channel 3 is named because it exists in BOTH families'
    // channel sets (AK 0-7, CK 0-3); the transport reconfigures it later either way.
    (void)dma_selftest_run(NORA_DMA_CHANNEL_3);
#endif

    // P2/P5: the board/clock port registration moved into audio_app (the orchestrator
    // binds it once before the first start/is_active) -- main stays an entry point.


#if SONORA_APP_IS_CLASSIC
    POT_Initialize();   // init potentiometer ADC path
#endif


#if defined(ENA_OPEN_TOUCH_EXCLUSIVE)
    {
        /* CVDANx numbers for the three Curiosity Platform pads, in pad order.
         * A fact about the board, so it is stated at the integration point and
         * not inside the detection layer -- see [internal] itc_hardware_reference.md §9.
         * Sole owner of the ITC now that the vendor library is gone; when both
         * were linked, measuring with two owners cost 3,000 counts of baseline
         * offset before it was noticed ([internal] itc_hardware_reference.md 10). */
        static const uint8_t nora_touch_pads[3] = { 1u, 8u, 10u };

        /* The same three electrodes as port pins, spelled out from the schematic
         * ([internal] itc_hardware_reference.md §9: P48/CVDAN1=RA1, P44/CVDAN8=RA8,
         * P40/CVDAN10=RA10) and NOT derived from the CVDANx number -- that mapping
         * is a lookup on this device, not arithmetic.
         *
         * Why they need configuring at all: DS70005591C p.1487 requires TRISx = 0
         * on every pin used for CVD, because the ITC hands pin control back to
         * TRIS/LAT as soon as the pin is idle between scans, and warns verbatim
         * that "if the sensor CVDANx pin floats (TRISx = 1) when it is idle
         * between CVD scans, then the robustness will degrade significantly even
         * with a light noise". LATx = 0 grounds the idle electrode, which also
         * makes each pad a static ground guard for its neighbours while they are
         * being scanned. nora_itc owns no pins by contract (nora_itc.h), and
         * nothing else was doing this: until 2026-08-15 all three pads ran at the
         * POR default TRISx = 1, i.e. exactly the warned condition -- which is
         * also a reason to distrust the 2026-08-14 sensitivity sweeps that were
         * taken in that state ([internal] itc_hardware_reference.md §12.2).
         *
         * ANSELx stays 1 (analog): ADC input pins come out of Reset that way and
         * this project no longer bulk-clears ANSELx, so this asserts the state
         * rather than changing it. nora_gpio_config() applies an output in the
         * order analog -> pull -> open-drain -> LAT -> TRIS, so the latch is at 0
         * before the driver ever enables.
         *
         * Deliberately NOT applied to the redundant channels CVDAN11/9/2
         * (RA11/RA9/RA2). Those are jumpered onto the same electrode nets and are
         * not records in the List, so the ITC never takes their pin control -- a
         * static Low output there would short the CVD waveform to ground during
         * the primary channel's own scan. High-Z input is their correct state. */
        static const nora_gpio_pin_t nora_touch_pad_pins[3] =
        {
            NORA_GPIO_PIN( NORA_GPIO_PORT_A,  1u ),   /* P48  CVDAN1  = RA1  */
            NORA_GPIO_PIN( NORA_GPIO_PORT_A,  8u ),   /* P44  CVDAN8  = RA8  */
            NORA_GPIO_PIN( NORA_GPIO_PORT_A, 10u )    /* P40  CVDAN10 = RA10 */
        };
        static const nora_gpio_config_t nora_touch_pad_cfg =
        {
            .dir          = NORA_GPIO_DIR_OUTPUT,
            .pull         = NORA_GPIO_PULL_NONE,
            .analog       = true,     /* CVD needs ANSELx = 1 */
            .open_drain   = false,
            .initial_high = false     /* LATx = 0: idle electrode grounded */
        };
        nora_touch_config_t  nora_touch_cfg;
        unsigned int         nora_touch_pad_i;

        _Static_assert( sizeof(nora_touch_pad_pins) / sizeof(nora_touch_pad_pins[0])
                        == sizeof(nora_touch_pads) / sizeof(nora_touch_pads[0]),
                        "touch pad CVDANx list and port-pin list must stay in step" );

        /* Before nora_touch_init(): the list starts scanning inside it. */
        for( nora_touch_pad_i = 0u;
             nora_touch_pad_i < sizeof(nora_touch_pad_pins) / sizeof(nora_touch_pad_pins[0]);
             nora_touch_pad_i++ )
        {
            if( !nora_gpio_config( nora_touch_pad_pins[nora_touch_pad_i],
                                   &nora_touch_pad_cfg ) )
            {
                printf(" NORA_TOUCH: pad %u pin config failed\n", nora_touch_pad_i);
            }
        }

        nora_touch_default_config( &nora_touch_cfg );
        /* CLKGEN6 is raised to 200 MHz by sonora_clock_boot_init(). The clock tree
         * belongs to the board, so the board states it, exactly as the CVDAN numbers
         * above are stated here rather than inside the detection layer. */
        nora_touch_cfg.clock_hz = 200000000uL;
        if( !nora_touch_init( nora_touch_pads, 3u, &nora_touch_cfg ) )
        {
            printf(" NORA_TOUCH: init failed (ITC refused the configuration)\n");
        }
        else
        {
            /* No per-pad override. Pad 1 used to need one (1,500/800 against the
             * 2,000 default, tuning manual appendix A A.1), but that was a patch
             * on signed-level detection: pad 1 is not less sensitive, its touch
             * waveform simply crosses zero more often, so a lower level threshold
             * only bought a few more accidental crossings. Detection now works on
             * the mean of |delta| (A.7), where the old numbers are in the wrong
             * units anyway. Reinstate an override here -- not in the library -- if
             * a board turns out to need one. */
        }
    }
#endif //defined(ENA_OPEN_TOUCH_EXCLUSIVE)
    ////////////////////////


    //
    // I2C init (main-driven). I2C2=MikroBUS-A, I2C3=MikroBUS-B.
    //   ENA_CMSIS_I2C defined  : via CMSIS-Driver wrapper
    //   ENA_CMSIS_I2C undefined: via I2C HAL directly
    //
#if defined(ENA_CMSIS_I2C)
    app_i2c_cmsis_init();
#else
    app_i2c_hal_init();
#endif //defined(ENA_CMSIS_I2C)




    // The selected app owns its audio route and app-specific pre-start work.
    // (SST26 external-flash bring-up and its sound-effect provisioning are now
    //  owned by the Classic app's sonora_app_prepare(); ASRC does not use them.)
    // Platform/board initialization above remains common and keeps its original order.
    // IS THIS THE DIE THIS IMAGE WAS BUILT FOR? The identity itself is already in the boot
    // banner (printMenu() -> app_silicon_print_identity()), unconditionally, so no log can be
    // silent about which board produced it. This call is the policy half and prints nothing
    // unless it refuses: an image that DECLARES a revision (build define
    // APP_SILICON_EXPECTED_REV, default 0 = announce only) will not start the signal path on
    // any other die, because a result attributed to the wrong board is worse than no result.
    // See app_silicon.h.
    if( !app_silicon_check() )
    {
        // Console only, deliberately: nothing in the audio or ASRC path has been initialised,
        // so this state cannot produce the fault under investigation, and the board still
        // answers "wrong die, here is what I am" instead of sitting dark. The resident engine
        // is still told the launch succeeded -- this image IS running correctly; it is
        // declining to run the engine, which is not a failed image to roll back.
        while(1)
        {
            resident_de_app_launch_ack_tick();
            app_uart_process();
        }
    }

    sonora_app_prepare();
    sonora_app_start_audio();


    ////////////////////////////////////
    // RGB LED PWM (hal_pwm) + the module-wide PWM clock-source select every
    // generator on the device shares. The Classic audio PWM DAC is app-owned --
    // brought up by the app contract's start_aux_output() (no-op for ASRC).
    ////////////////////////////////////
    pwm_led_init();
#if SONORA_APP_IS_ASRC
    // ASRC does not use the potentiometer. Keep the RGB status LED, but let
    // hardware PWM hold the existing dim-green idle color without ADC polling.
    set_rgb_idle_color();
#endif
    sonora_app_start_aux_output();


    while(1)
    {
        /* Confirm a successful start to the resident engine, once this loop has
         * genuinely been running. At the top of the loop body because paths below it
         * `continue`. No-op on a hardware reset. */
        resident_de_app_launch_ack_tick();

        (void)sonora_app_manage_audio();

        sonora_app_process_controls();   /* app-owned per-loop control input */

#if defined(ENA_OPEN_TOUCH_EXCLUSIVE)
        /* Non-blocking: polls the scan in flight and starts the next one. A scan
         * at 2^8 takes ~5 ms, which is why it is not waited for here. */
        nora_touch_process();
#endif //defined(ENA_OPEN_TOUCH_EXCLUSIVE)
        app_uart_process();   /* drains UART1 + UART2 command input (shared parser) */

        // App-specific control work runs at the same point as before. ASRC measurement
        // streaming may own this iteration so debug/pot work cannot perturb the capture.
        if( sonora_app_service() ) { continue; }
        dbg_print();
        sonora_app_debug_print();
#if SONORA_APP_IS_CLASSIC
        dbg_RGB_pot();
        POT_Process();
#endif
    }

    return 0;
}










//===========================================================
// File-scope Local Function
//===========================================================



static void term_init_safe( void )
{
    printf("\x1b(B\x0F");    // set terminal mode to ASCII + select G0
    printf("\x1b[0m");       // set attribution to default
}
static void clearTerminalScreen( void )
{
//    printf("\033[2J"); 
    printf("\x1b[2J\x1b[H");     // clear+home
}
// Kept commented out with the other cursor helpers: no caller today, but the
// terminal-control set belongs together for when a monitor line needs it.
// static void moveCursor( int row )
// {
//     if (row < 1) row = 1;
//     printf("\x1b[%d;1H", row);   // line=row then row=requested val
// }
// static void moveCursorToLineStart( void )
// {
// //    // Option 1: simplest
// //    printf("\r");
//     // Option 2: explicit ANSI escape (same effect)
//     printf("\033[G");
// }
// static void moveCursorUp( void )
// {
//     printf("\033[1A");
// }
// static void hideCursor( void )
// {
//     printf("\033[?25l");
// }
static void reset_console( void )
{
    term_init_safe();
    clearTerminalScreen();
}
// Build-identity tokens injected by buildtools/build.ps1 as bare tokens and stringified here.
// Both print on the boot banner so the log self-identifies the build. Each falls back to
// "(unknown)" for IDE-direct builds that bypass build.ps1.
//   -DAPP_SRC_DIRNAME=<leaf-of-repo-root>    : which clone folder was flashed (guards against
//                                              flashing a look-alike sibling folder by mistake)
//   -DSONORA_GIT_COMMIT=<short-hash>[_dirty] : which Git revision was built
#define APP_SRC_DIRNAME_STR2(x) #x
#define APP_SRC_DIRNAME_STR(x)  APP_SRC_DIRNAME_STR2(x)
#ifndef APP_SRC_DIRNAME
#define APP_SRC_DIRNAME (unknown)
#endif
#ifndef SONORA_GIT_COMMIT
#define SONORA_GIT_COMMIT (unknown)
#endif

// Boot-banner visibility hold. See app_specific_config_defs.h (APP_BOOT_BANNER_HOLD_*).
// On a power-on-class reset (POR/BOR) only -- the case where the USB-CDC console has
// not finished enumerating in time to catch the early banner -- if the configured
// boot button is held, re-print the full banner every REPEAT_MS for SECONDS so a
// terminal connecting anywhere within the window still receives it. Any warm reset,
// or a boot where the button is not held, returns immediately and leaves the single
// early banner and boot timing unchanged (= fast boot).
#if APP_BOOT_BANNER_HOLD_ENABLE
#if (APP_BOOT_BANNER_HOLD_REPEAT_MS <= 0)
#error "APP_BOOT_BANNER_HOLD_REPEAT_MS must be > 0"
#endif
#if (APP_BOOT_BANNER_HOLD_BUTTON < 1) || (APP_BOOT_BANNER_HOLD_BUTTON > 3)
#error "APP_BOOT_BANNER_HOLD_BUTTON must be a board button id in 1..3"
#endif
#endif
static void boot_banner_hold_if_requested(void)
{
#if APP_BOOT_BANNER_HOLD_ENABLE
    // Only power-on-class resets lose the early banner; warm resets keep the
    // terminal open, so never delay them.
    if( !nora_reset_snapshot_is_power_on_class() )
    {
        return;
    }
    // The button is the mode selector: held at power-on => banner-hold mode; not
    // held => fast boot (the early printMenu() already ran once for warm/connected).
    if( !BUTTON_IsPressed( APP_BOOT_BANNER_HOLD_BUTTON ) )
    {
        return;
    }

    const uint32_t repeats =
        ( (uint32_t)APP_BOOT_BANNER_HOLD_SECONDS * 1000u ) /
        (uint32_t)APP_BOOT_BANNER_HOLD_REPEAT_MS;

    printf(" [boot-banner hold: button %u held at power-on -- re-printing banner for %u s]\n",
           (unsigned)APP_BOOT_BANNER_HOLD_BUTTON,
           (unsigned)APP_BOOT_BANNER_HOLD_SECONDS);

    for( uint32_t i = 0u; i < repeats; ++i )
    {
        LED_Toggle( 0u );   // heartbeat so a held board is visibly in banner-hold mode
        printMenu();
        printf(" [boot-banner hold %lu/%lu]\n",
               (unsigned long)( i + 1u ), (unsigned long)repeats);
        delay_ms( APP_BOOT_BANNER_HOLD_REPEAT_MS );
    }
    LED_Off( 0xFFu );
#endif // APP_BOOT_BANNER_HOLD_ENABLE
}

static void printMenu(void)
{
    printf("===============================================================\n");
    printf(" dsPIC33AK Audio DSP Demo (Build: %s %s)\n", __DATE__, __TIME__);
    printf(" Commit: %s\n", APP_SRC_DIRNAME_STR(SONORA_GIT_COMMIT));
    printf(" Source: %s\n", APP_SRC_DIRNAME_STR(APP_SRC_DIRNAME));
    printf(" App: %s\n", sonora_app_name());
    printf(" Config: target=%s rate=%luHz format=%s slots=%u block=%u role=%s\n",
           (APP_TARGET == APP_TARGET_AK512) ? "AK512" : "AK128",
           (unsigned long)APP_SAMPLE_RATE_HZ,
           APP_USE_I2S_FORMAT ? "I2S" : "TDM8",
           (unsigned)APP_SLOTS_PER_FS,
           (unsigned)APP_BLOCK_FRAMES,
           APP_USE_SPI_TDM_CLK_MASTER ? "dsPIC-master" : "external-master");
    printf(" Preset: %s\n", APP_BUILD_NAME);
    printf(" Detail: %s\n", APP_BUILD_DETAIL);
    printf(" Reset: %s (RCON=0x%08lX)\n",
           nora_reset_snapshot_cause_str(),
           (unsigned long)nora_reset_snapshot_raw());
    resident_de_app_launch_banner();
    sonora_app_print_banner();
    /* One fixed clock structure, so this is a statement of fact rather than a
     * report of which arm won -- there are no arms. See the clock design contract. */
    /* Fosc/Fcy come from the clock HAL, which reports what it actually programmed,
     * next to the compile-time PLL1_CLK_HZ the build asserted. Printing both means a
     * board running at a different speed than the build believes says so in the
     * banner instead of only in a garbled console -- the failure mode that cost a
     * bring-up on the CK side when an undocumented divide-by-2 halved the real Fcy. */
    printf(" SysClock: PLL1<-FRC (%lu Hz)  Fosc=%lu Fcy=%lu  SPI2-transport-clock=%s\n",
           (unsigned long)PLL1_CLK_HZ,
           (unsigned long)nora_clock_get_fosc_hz(),
           (unsigned long)nora_clock_get_fcy_hz(),
           RESOLVED_BOARD_SPI_CLOCK_FROM_PLL2 ? "PLL2<-REFI1" : "PLL1");
    printf(" Feature: WM8904-B-REQ=%u TDM-PHYS=SPI%u/%u SPI2-AUDIO=%u PWM=%u USB-IN=%u\n",
           (unsigned)APP_REQ_MIKROB_WM8904,
           (unsigned)APP_TDM_PHYS_A_NUM,
           (unsigned)APP_TDM_PHYS_B_NUM,
           (unsigned)APP_USE_SPI2_AUDIO,
           (unsigned)APP_USE_PWM_AUDIO,
           (unsigned)APP_USE_USB_AUDIO_IN);
    // Which DIE, before which BOARD. These two lines are the hardware identity of the run:
    // the silicon revision decides whether a revision-specific workaround applies (see the
    // DO NOT REVERT note on the CCP ISR attributes in asrc_clock_control.c, which is a rev A1
    // workaround), and the UDID decides which physical unit it was. Both belong in the banner
    // and not in some later init's output: a log pasted into a report carries the banner, and
    // a trap record that cannot be attributed to a die is a run that has to be repeated.
    app_silicon_print_identity();
    // Board-individual ID: print the target UDID so each board's log self-identifies
    // which physical unit produced it. UDID128 is concatenated UDID4..UDID1 (high
    // word first), matching the *nt04 console command.
    {
        nora_udid_t udid;
        if (nora_udid_read(&udid))
        {
            printf(" UDID=%08lX%08lX%08lX%08lX\n",
                   (unsigned long)udid.word[3], (unsigned long)udid.word[2],
                   (unsigned long)udid.word[1], (unsigned long)udid.word[0]);
        }
        else
        {
            printf(" UDID read failed or invalid\n");
        }
    }
    resident_de_app_delivery_banner();
    printf("===============================================================\n");
}


static void init_ports( void )
{
    // Each pin owner configures analog/digital mode explicitly. Digital GPIO
    // and pinmux helpers clear ANSEL; analog consumers set it.

#if APP_TARGET == APP_TARGET_AK512

// DIM28  RP41/APWM1L/IOMAD8/IOMBF3/RC8                 [LED0]
// DIM30  RP42/SDO2/IOMBF2/RC9                          [LED1]
// DIM32  RP43/PWM7H/IOMBD5/IOMBF1/RC10                 [LED2]
// DIM36  RP45/PWM8H/IOMBD7/RC12                        [LED4]
// DIM40  AD1ANN1/AD1AN4/CVDAN10/CMPEN/CMP5C/RP11/RA10
// DIM43  AD3AN4/CVDTX29/RP81/RF0                       [BUTTON1]
// DIM45  RP84/RF3
// DIM41  OA2IN+/AD2AN1/CVDAN18/CVDTX2/CMP2B/RP19/RB2
// DIM56  CVDTX23/RP69/RE4
// DIM74  CVDAN12/RP13/RA12                             [QSPI-Flash MOSI]
// DIM102 RP113/RH0

// Debug/scope pins DIM102(RH0) and DIM56(RE4): digital output. LAT is set
// before the driver is enabled (nora_gpio_config). analog is set false here
// explicitly, so the pin owns its ANSEL -- the boot-time bulk ANSELx=0 clear
// was removed and each pin sets its own analog/digital mode.
//
// RH0 idles High (plain scope pin, drives nothing on the board).
//
// RE4 idles *Low*, and that is deliberate. DIM56 is not a free debug pin: on
// this board (DS70005562 p.36/p.38) it is the driven-shield net
//   RE4 --[R10 = 100R, populated]-- SHIELD copper --[R11 = 0R, DNP]-- GND
// so the shield guarding the touch electrodes is terminated only by whatever
// RE4 drives. Held High it was a static 3.3 V rail capacitively coupled to
// three CVD electrodes through the shield -- the opposite of a guard, and any
// supply ripple on it lands straight in the measurement. Low ties the shield
// to ground, which is the conventional static-guard connection and the control
// experiment against the High case (never Hi-Z: a floating shield plate is
// strictly worse than either).
//
// This is still a *static* guard. The proper fix is to let the ITC drive the
// shield in phase with the CVD sample, which the silicon supports because RE4
// is CVDTX23 (ITCTXA bit 23) -- see [internal] itc_hardware_reference.md
// sections 9 and 12.5. Do that after this control experiment, not at the same
// time.
//
// If DIM56 is ever needed as a scope pin again, remember what it is wired to:
// nora_spi_i2s_tdm_dspic33ak_diag.c toggles BOARD_DBG_PIN_E4 under ENA_TDM_DBG,
// which injects a square wave onto the shield. That build option is currently
// not enabled anywhere.
    static const nora_gpio_config_t dbg_cfg =
    {
        .dir          = NORA_GPIO_DIR_OUTPUT,
        .pull         = NORA_GPIO_PULL_NONE,
        .analog       = false,
        .open_drain   = false,
        .initial_high = true,
    };
    static const nora_gpio_config_t shield_cfg =
    {
        .dir          = NORA_GPIO_DIR_OUTPUT,
        .pull         = NORA_GPIO_PULL_NONE,
        .analog       = false,
        .open_drain   = false,
        .initial_high = false,
    };
    (void)nora_gpio_config(BOARD_DBG_PIN_H0, &dbg_cfg);
    (void)nora_gpio_config(BOARD_DBG_PIN_E4, &shield_cfg);

#endif //APP_TARGET == APP_TARGET_AK512
}


#if SONORA_APP_IS_CLASSIC
static void dbg_RGB_pot( void )
{
#define LED_COLOR_GREEN( a )    pwm2_set_duty( a )  // green
#define LED_COLOR_RED( a )      pwm3_set_duty( a )  // red
#define LED_COLOR_BLUE( a )     pwm1_set_duty( a )  // blue

    // POT -> RGB mapping:  OFF(dim green) --(crisp boundary)--> BLUE ---- WHITE ---- RED.
    // Full-CCW shows a faint green "idle" glow (not a hard off); crossing the ON
    // boundary snaps straight to blue, then a smooth ramp runs blue -> white -> red
    // toward full-CW. The ramp is EMA-smoothed for a jitter-free gradient, while the
    // OFF<->ON edge uses hysteresis so the dim-green<->blue boundary is both sharp
    // AND flicker-free.
    enum { POT_MAX = 4095 };
    const uint8_t  POT_EMA_SHIFT  = 4;    // ramp-smoothing time constant (larger = smoother)
    const uint16_t POT_ON_THRESH  = 80;   // OFF->ON boundary: light up above this
    const uint16_t POT_OFF_THRESH = 40;   // ON->OFF boundary (hysteresis; must be < ON)
    const uint8_t  POT_OFF_GREEN  = 12;   // faint green shown in the OFF (full-CCW) zone

    // (1) integer EMA smoothing:  acc += raw - (acc >> k);  filtered = acc >> k
    static uint32_t pot_ema_acc;
    static bool     pot_ema_init;
    uint16_t raw = (uint16_t)POT_Read();
    if( !pot_ema_init ) { pot_ema_acc = (uint32_t)raw << POT_EMA_SHIFT; pot_ema_init = true; }
    pot_ema_acc += (uint32_t)raw - (pot_ema_acc >> POT_EMA_SHIFT);
    uint16_t pot_f = (uint16_t)(pot_ema_acc >> POT_EMA_SHIFT);

    // (2) crisp OFF<->ON boundary with hysteresis (full-CCW = faint green idle)
    static bool led_on;
    if( led_on ) { if( pot_f < POT_OFF_THRESH ) led_on = false; }
    else         { if( pot_f > POT_ON_THRESH  ) led_on = true;  }

    if( !led_on )
    {
        LED_COLOR_RED  ( 0 );
        LED_COLOR_GREEN( POT_OFF_GREEN );
        LED_COLOR_BLUE ( 0 );
        return;
    }

    // (3) ON: map [ON_THRESH .. MAX] -> 0..1000, then a blue->white->red ramp.
    //     Position 0 is pure blue so the LED lands straight on blue at the edge.
    int32_t pos;
    if( pot_f >= POT_MAX ) pos = 1000;
    else                   pos = (int32_t)(pot_f - POT_ON_THRESH) * 1000 / (POT_MAX - POT_ON_THRESH);
    if( pos < 0 ) pos = 0;

    static const struct { int16_t pos; uint8_t r, g, b; } ramp[] = {
        {    0,  0,  0, 90 },   // blue  (just past the ON boundary)
        {  500, 60, 60, 60 },   // white (mid travel)
        { 1000, 90,  0,  0 },   // red   (full CW)
    };
    const int n = (int)(sizeof(ramp) / sizeof(ramp[0]));
    int i = 0;
    while( i < n - 2 && pos > ramp[i + 1].pos ) i++;   // bracket [i, i+1]
    int32_t span = ramp[i + 1].pos - ramp[i].pos;
    int32_t t    = (pos - ramp[i].pos) * 256 / span;   // 0..256
    uint8_t r = (uint8_t)(ramp[i].r + ((int32_t)ramp[i + 1].r - ramp[i].r) * t / 256);
    uint8_t g = (uint8_t)(ramp[i].g + ((int32_t)ramp[i + 1].g - ramp[i].g) * t / 256);
    uint8_t b = (uint8_t)(ramp[i].b + ((int32_t)ramp[i + 1].b - ramp[i].b) * t / 256);

    LED_COLOR_RED  ( r );
    LED_COLOR_GREEN( g );
    LED_COLOR_BLUE ( b );
}
#else
static void set_rgb_idle_color( void )
{
    pwm3_set_duty( 0u );   // red
    pwm2_set_duty( 12u );  // green: match the POT-off idle indication
    pwm1_set_duty( 0u );   // blue
}
#endif


static void dbg_print( void )
{
    static uint32_t last_prt_1 = UINT32_MAX;
           uint32_t cur = GetTicks();

    // every 3000ms
    if ((uint32_t)(cur - last_prt_1) >= 3000) // Actively use overflow
    {
        // Reset tera term to ASCII standard mode (just in case) -- but only while periodic
        // output is enabled. This is 7 bytes of ESC sequence on the wire, not a printed line,
        // so it is the most damaging thing here to leave running: it lands mid-XMODEM-block
        // during an update, and it defeats the UART drain that *feaa55 / *fu5A do before resetting.
        // "*tq" is the parent switch for all periodic output (see audio_transport_dbg_enabled).
        if( audio_transport_dbg_enabled() )
        {
            term_init_safe();
        }

        last_prt_1 = cur;
    }

    // NOTE: app-owned periodic monitors (engine-synth / bass-enhancer / anc) moved
    // into the selected app's sonora_app_debug_print(); the app telemetry line
    // (TDM load / ASRC / CCP) lives in audio_transport_dbg_print(). main() stays
    // app-blind and only keeps common terminal housekeeping here.
}


// POT -> 11-step (-5..+5) mapping. No caller since the tone/gain demo controls
// moved into the Classic app's own control module; kept here as the reference
// mapping only.
// static int adc_to_step(uint16_t adc)
// {
//     // assign to 11 steps (-5 ~ +5)
//     int step = (adc * 11) / 4096 - 5;
//
//     // range clipper
//     if (step < -5) step = -5;
//     if (step >  5) step =  5;
//
//     return step;
// }
