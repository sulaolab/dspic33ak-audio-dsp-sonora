#include "resident_de_boot_platform.h"

#include <stddef.h>
#include <xc.h>

#include "nora_clock.h"
#include "nora_clock_dspic33ak.h"   /* AK-only: the resident engine re-programs CLKGEN6/8 */
#include "nora_gpio.h"
#include "nora_pps.h"
#include "nora_tick_timer.h"
#include "nora_uart.h"
#include "resident_de_mailbox.h"
#include "resident_de_bootloader.h"

#define RESIDENT_FRC_HZ       UINT32_C(8000000)
#define RESIDENT_FCY_HZ       (RESIDENT_FRC_HZ / 2u)
#define RESIDENT_UART_BAUD    UINT32_C(230400)
#define RESIDENT_UART         NORA_UART_INST_1

/* The console is UART1 at 230400 on every board, and the clock above is the FRC --
 * so what differs per DIM is only which pins the console and the recovery button
 * are wired to. Both sets are taken from the application's own board code rather
 * than from a datasheet: src/app/uart_platform/uart_platform_board.c (console) and
 * src/app/board/devices/button_led.c (button). Keep them in step with those. */
#if defined(__dsPIC33AK512MPS512__)
#define RESIDENT_UART_RX_RP   ((nora_gpio_rp_t)50u)
#define RESIDENT_UART_TX_RP   ((nora_gpio_rp_t)114u)
#define RESIDENT_RECOVERY_BUTTON_RP ((nora_gpio_rp_t)19u) /* Button 3 / RB2 */
#elif defined(__dsPIC33AK128MC106__)
#define RESIDENT_UART_RX_RP   ((nora_gpio_rp_t)52u)       /* RD3 */
#define RESIDENT_UART_TX_RP   ((nora_gpio_rp_t)50u)       /* RD1 */
#define RESIDENT_RECOVERY_BUTTON_RP ((nora_gpio_rp_t)7u)  /* Button / RA6 */
#else
#error "Unknown target: give the resident console its UART RX/TX RP and the recovery button RP for this board."
#endif

#define RESIDENT_RECOVERY_BUTTON_SAMPLES (8u)
#define RESIDENT_RECOVERY_BUTTON_SETTLE_LOOPS (1000u)

static __attribute__((always_inline)) inline void service_tick_timer(void)
{
    /* Timer1's IF bit is a latch, not an event counter. Service it while UART
     * bytes are flowing as well as while waiting, otherwise a long frame can
     * collapse many elapsed milliseconds into one software tick. */
    if (_T1IF != 0u) {
        nora_tick_timer_irq_handler();
    }
}

static bool configure_clock(void)
{
    const nora_clock_dspic33ak_clkgen_config_t clkgen = {
        .source = NORA_CLOCK_SOURCE_FRC,
        .divide_by = 1u,
    };

    /* Every device reset reloads the configuration words, but the configured
     * reset clock need not be the clock wanted by this resident image.
     * Normalize CPU CLKGEN1, peripheral CLKGEN6, and UART CLKGEN8 explicitly.
     * Timer1 uses the standard-speed peripheral clock derived from CLKGEN1 / 2,
     * so XMODEM timing is independent of the factory configuration. 8 MHz /
     * round(8 MHz / 230400) gives about -0.8% baud error.
     *
     * CLKGEN1 is the clock currently executing this code, so it is switched
     * through nora_clock_switch_source() and not through the CLKGEN call used
     * for the two peripheral generators below: the system-clock path keeps the
     * generator enabled while it changes source (DS70005591C 13.4.2), whereas
     * clearing ON first -- correct for a peripheral generator -- hangs the CPU.
     * This engine used to hand-roll that sequence here because the HAL had no
     * safe system-clock entry point; it now has one, with the same polling
     * budget and a CLKRDY + COSC confirmation, so the copy is gone.
     *
     * CLKGEN1 is normalized in two explicit steps, because the portable switch
     * changes the source ONLY -- it no longer forces this divider to /1 as a side
     * effect, since that divider is an AK concept and CK has no writable
     * equivalent. The source goes first and the divider second: FRC is the
     * lowest-frequency selection available here, so /1 is applied to a clock that
     * is already slow. The reverse order would raise the frequency while the
     * configuration-word source is still unknown to this image.
     *
     * When the configuration words already selected FRC -- the usual case -- the
     * switch call finds the part on the requested source and only confirms it: no
     * clock-switch sequence runs, so this normalization costs nothing on the common
     * path and still corrects the uncommon one. */
    if (nora_clock_switch_source(NORA_CLOCK_SOURCE_FRC, 0u) != NORA_CLOCK_OK) {
        return false;
    }
    if (nora_clock_dspic33ak_system_divider_set(1u) != NORA_CLOCK_OK) {
        return false;
    }

    return (nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_6,
                                              &clkgen) == NORA_CLOCK_OK) &&
           (nora_clock_dspic33ak_clkgen_configure(NORA_CLOCK_DSPIC33AK_CLKGEN_8,
                                              &clkgen) == NORA_CLOCK_OK);
}

static bool configure_uart(void)
{
    const nora_uart_config_t config = {
        .uart_clk_hz = RESIDENT_FRC_HZ,
        .baudrate = RESIDENT_UART_BAUD,
        .timeout_ms = 0u,
        .get_ms = NULL,
        .data_bits = 8u,
        .stop_bits = 1u,
        .parity = NORA_UART_PARITY_NONE,
        .enable_tx = true,
        .enable_rx = true,
        .rx_mode = NORA_UART_RX_MODE_POLLING,
        .rx_ring_buffer = NULL,
        .rx_ring_buffer_size = 0u,
        .rx_irq_priority = 0u,
        .tx_irq_priority = 0u,
    };

    return nora_pinmux_route_output(NORA_PPS_OUTPUT_U1TX,
                                          RESIDENT_UART_TX_RP,
                                          true) &&
           nora_pinmux_route_input(NORA_PPS_INPUT_U1RX,
                                         RESIDENT_UART_RX_RP) &&
           (nora_uart_init(RESIDENT_UART, &config) == NORA_UART_OK);
}

static void normalize_interrupt_state(void)
{
    /* Device reset specifies reset values for the interrupt controller and the
     * peripherals used here. Repeat the resident-owned baseline explicitly so
     * startup remains deterministic and does not depend on future CRT changes. */
    __builtin_disable_interrupts();
    /* The resident image does not use PTG. Keep it disabled before clearing
     * interrupt flags, even though PTGCON.ON itself has a device-reset value of
     * zero. */
    PTGCONbits.ON = 0u;
    PTGCON = 0u;
    IEC0 = 0u; IEC1 = 0u; IEC2 = 0u; IEC3 = 0u;
    IEC4 = 0u; IEC5 = 0u; IEC6 = 0u; IEC7 = 0u;
    IEC8 = 0u;
    IFS0 = 0u; IFS1 = 0u; IFS2 = 0u; IFS3 = 0u;
    IFS4 = 0u; IFS5 = 0u; IFS6 = 0u; IFS7 = 0u;
    IFS8 = 0u;
    /* How many IEC/IFS banks exist is a device property, not a choice: the
     * AK128MC106 implements 0-8 only and the compiler rejects IEC9 outright
     * ("did you mean 'IPC9'?" -- IPC9 is the register that follows). Same split,
     * and same reason, as src/shared/resident_de_mailbox.c:143-152. */
#if defined(__dsPIC33AK512MPS512__)
    IEC9 = 0u; IEC10 = 0u; IEC11 = 0u;
    IFS9 = 0u; IFS10 = 0u; IFS11 = 0u;
#elif !defined(__dsPIC33AK128MC106__)
#error "Unknown target: state how many IEC/IFS banks this part implements."
#endif
    INTCON1bits.BADOPERR = 0u;
    INTCON1bits.ADDRERR = 0u;
    INTCON1bits.STKERR = 0u;
    INTCON3 = 0u;
    INTCON4 = 0u;
    __asm__ volatile("mov #0, w0\n\tmov w0, SR" ::: "w0");
}

bool resident_boot_platform_init(void)
{
    const nora_tick_timer_config_t timer = {
        .timer_clk_hz = RESIDENT_FCY_HZ,
        .irq_priority = NORA_TICK_TIMER_DEFAULT_IRQ_PRIORITY,
        .run_in_idle = false,
    };

    normalize_interrupt_state();
    if (!configure_clock() || !configure_uart()) {
        return false;
    }
    if (nora_tick_timer_init(&timer) != NORA_TICK_TIMER_OK) {
        return false;
    }
    /* Keep the resident image fully polled. It needs no asynchronous work:
     * UART is polling and millis() services the Timer1 flag below. Leaving GIE
     * clear reduces the number of execution paths during Flash programming;
     * it is not a workaround for App peripheral state surviving device reset. */
    _T1IE = 0u;
    __builtin_disable_interrupts();
    return true;
}

bool resident_boot_platform_recovery_button_pressed(void)
{
    uint8_t sample;

    /* Button 3 is active-low and has an external pull-up on the Sonora board.
     * This check intentionally needs no App clock, timer, UART, or interrupt
     * state, so it can run before a valid launch record transfers control to
     * the App. Requiring several consecutive low samples rejects a transient
     * level at reset without adding another resident timer dependency. */
    if (!nora_gpio_rp_config_digital_input(RESIDENT_RECOVERY_BUTTON_RP)) {
        return false;
    }
    for (sample = 0u; sample < RESIDENT_RECOVERY_BUTTON_SAMPLES; sample++) {
        volatile uint16_t settle;

        if (nora_gpio_rp_read(RESIDENT_RECOVERY_BUTTON_RP) !=
            NORA_GPIO_LEVEL_LOW) {
            return false;
        }
        for (settle = 0u; settle < RESIDENT_RECOVERY_BUTTON_SETTLE_LOOPS;
             settle++) {
            __asm__ volatile("nop");
        }
    }
    return true;
}

void __attribute__((interrupt, context)) _T1Interrupt(void)
{
    nora_tick_timer_irq_handler();
}

/*
 * ONE CALL DEEP, so the VECTOR carries no prologue push.
 *
 * `context` banks W0-W7 only; the capture below needs more, and inlined here it made this
 * vector open with 12 `mov.l wN,[w15++]`. A prologue push at an ISR's first instruction is
 * the documented trigger of the A1 silicon STACK ERROR (DO-NOT-REVERT note above
 * _CCP1Interrupt in src/app/apps/asrc/asrc_clock_control.c), and this vector is exactly the
 * one that must not fail: it is the catch-all that names an uninstalled vector before
 * resetting. Taking a STACK ERROR here would replace that diagnosis with a misleading one.
 *
 * The pushes still happen -- they just happen inside an ordinary function now. Same remedy
 * as the per-vector bodies in nora_spi_i2s_tdm_dspic33ak.c. Cheap: this path runs once and
 * then resets.
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
static void __attribute__((noinline, noreturn)) resident_default_interrupt_body(void)
#else
void __attribute__((interrupt, context)) _DefaultInterrupt(void)
#endif
{
    const uint32_t vector = INTTREG;
    /* PCTRAP is the primary-source diagnostic for CPU-originated traps. Keep
     * the low INTCON1 trap flags in the high byte so Address/Stack/Bad-Opcode
     * provenance survives the RESET executed below. */
    const uint32_t detail = ((INTCON1 & UINT32_C(0xff)) << 24) |
                            (PCTRAP & UINT32_C(0x00ffffff));
    resident_boot_default_interrupt_capture(vector, detail);
    __asm__ volatile("reset");
    __builtin_unreachable();
}

#if APP_A1_ISR_STACK_WORKAROUND
void __attribute__((interrupt, context)) _DefaultInterrupt(void)
{
    resident_default_interrupt_body();
}
#endif

uint32_t resident_boot_platform_millis(void)
{
    service_tick_timer();
    return nora_tick_timer_get_ms();
}

bool resident_boot_platform_read(uint8_t *data, uint32_t timeout_ms)
{
    const uint32_t start = resident_boot_platform_millis();

    for (;;) {
        /* XMODEM sends each frame without inter-byte gaps.  At the resident
         * 8 MHz FRC clock, routing every byte through the generic HAL's
         * instance/mode checks and reading the millisecond timer first can let
         * the small hardware FIFO overflow at 230400 baud.  This platform is
        * permanently bound to UART1 in polling mode, so consume a queued byte
        * first. Service the lightweight Timer1 latch after the read so a
        * continuous frame does not lose elapsed ticks. */
        if (U1STATbits.RXBE == 0u) {
            *data = (uint8_t)U1RXB;
            service_tick_timer();
            return true;
        }
        if ((uint32_t)(resident_boot_platform_millis() - start) >= timeout_ms) {
            return false;
        }
    }
}

void resident_boot_platform_write_byte(uint8_t data)
{
    (void)nora_uart_write_byte(RESIDENT_UART, data);
    service_tick_timer();
}

void resident_boot_platform_write(const char *text)
{
    if (text != NULL) {
        while (*text != '\0') {
            resident_boot_platform_write_byte((uint8_t)*text++);
        }
    }
}

void resident_boot_platform_flush(void)
{
    nora_uart_rx_flush(RESIDENT_UART);
}

#if RESIDENT_BOOT_ENA_BOOT_TRACE
static void write_hex_digit(uint8_t nibble)
{
    static const char hex[] = "0123456789ABCDEF";
    resident_boot_platform_write_byte((uint8_t)hex[nibble & 0x0fu]);
}

static void write_hex32(uint32_t value)
{
    write_hex_digit((uint8_t)(value >> 28));
    write_hex_digit((uint8_t)(value >> 24));
    write_hex_digit((uint8_t)(value >> 20));
    write_hex_digit((uint8_t)(value >> 16));
    write_hex_digit((uint8_t)(value >> 12));
    write_hex_digit((uint8_t)(value >> 8));
    write_hex_digit((uint8_t)(value >> 4));
    write_hex_digit((uint8_t)value);
}

static void trace_cpu_state(void)
{
    resident_boot_platform_write("BL CPU: I1="); write_hex32(INTCON1);
    resident_boot_platform_write(" I3="); write_hex32(INTCON3);
    resident_boot_platform_write(" I4="); write_hex32(INTCON4);
    resident_boot_platform_write(" I5="); write_hex32(INTCON5);
    resident_boot_platform_write(" COR="); write_hex32(CORCON);
    resident_boot_platform_write(" MOD="); write_hex32(MODCON);
    resident_boot_platform_write(" XBR="); write_hex32(XBREV);
    resident_boot_platform_write(" SPL="); write_hex32(SPLIM);
    resident_boot_platform_write(" IVT="); write_hex32(IVTBASE);
    resident_boot_platform_write("\r\n");
}
#else
#define trace_cpu_state() ((void)0)
#endif

void resident_boot_platform_jump(uint32_t ivt_address, uint32_t entry_address)
{
    register uint32_t target __asm__("w8") = entry_address;

    while (!nora_uart_tx_done(RESIDENT_UART)) {
        /* allow the final diagnostic line to leave the FIFO */
    }
    trace_cpu_state();
    while (!nora_uart_tx_done(RESIDENT_UART)) {
    }
    __builtin_disable_interrupts();
    (void)nora_tick_timer_deinit();
    /* Keep UART1 alive through the first instructions of the relocated app.
     * The SERIAL_UPDATE_APP's temporary handoff trace uses the existing reset-clock baud
     * to identify the exact initialization stage reached. UART1_Initialize()
     * later replaces the hardware configuration normally. */
    __builtin_setIVTBASE((void *)(uintptr_t)ivt_address);
    /* This is a reset-entry handoff, not a C function call. A CALL would push a
     * return PC onto the resident stack immediately before the application CRT
     * replaces W15. Transfer without a link/return record instead. */
    __asm__ volatile("goto w8" : : "r"(target));
    __builtin_unreachable();
}

void resident_boot_platform_jump_early(uint32_t ivt_address, uint32_t entry_address)
{
    register uint32_t target __asm__("w8") = entry_address;

    /* Called before platform initialization on the reset following validation.
     * Only the resident CRT has run, so no UART/timer/NVM state needs teardown. */
    __builtin_disable_interrupts();
    __builtin_setIVTBASE((void *)(uintptr_t)ivt_address);
    __asm__ volatile("goto w8" : : "r"(target));
    __builtin_unreachable();
}

void resident_boot_platform_reset(void)
{
    while (!nora_uart_tx_done(RESIDENT_UART)) {
        /* drain the final status line */
    }
    resident_boot_reset_sync(RESIDENT_BOOT_RESET_SOURCE_RESIDENT);
}

void resident_boot_platform_launch_reset(uint32_t ivt_address,
                                         uint32_t entry_address)
{
    while (!nora_uart_tx_done(RESIDENT_UART)) {
        /* drain the final status line */
    }
    resident_boot_launch_reset_sync(ivt_address, entry_address);
}
