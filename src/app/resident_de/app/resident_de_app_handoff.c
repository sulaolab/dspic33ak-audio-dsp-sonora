/* Compiled only in a serial-update image -- the three standalone MPLAB
 * configurations exclude src/resident_de/ wholesale, and the header supplies empty
 * inlines in place of everything here. Nothing in this file needs to ask which
 * delivery mode it is in.
 *
 * This code used to sit inline in src/main.c behind fourteen
 * #if defined(SONORA_DELIVERY_SERIAL_UPDATE_APP) blocks, eight of which were
 * bring-up scaffolding rather than delivery logic. Collecting it here is what
 * reduced main.c's share of the gate to zero. */
#include "resident_de/app/resident_de_app_handoff.h"

#include <stdbool.h>
#include <stdint.h>
#include <xc.h>

#include "hal_nvm/nora_nvm.h"
#include "hal_reset/nora_reset.h"
#include "nora_tick_timer.h"
#include "resident_de_abi.h"
#include "resident_de_manifest.h"
#include "resident_de_pipe.h"

/* Bring-up markers ('M','P','C','T','U','D') emitted by raw U1TXB writes, so a
 * handoff that dies before printf still says how far it got. They cost Flash and
 * console noise in a shipping image, and the handoff they were written to debug is
 * now settled -- so they are switchable. Default 1: this commit is a pure
 * restructure, and flipping observable output at the same time would destroy the
 * one property that makes the restructure checkable (a disassembly diff that
 * explains every hunk). Set to 0 once the current hardware verification round is
 * signed off. */
#if !defined(RESIDENT_DE_ENA_HANDOFF_TRACE)
#define RESIDENT_DE_ENA_HANDOFF_TRACE 1
#endif

/* How long the main loop must have been running before this boot is declared a
 * success to the resident engine. Long enough that a start-up crash loop cannot
 * slip under it, short enough that a genuine boot is acknowledged well within the
 * engine's limit (RESIDENT_BOOT_PIPE_LAUNCH_LIMIT attempts). */
#define APP_LAUNCH_SUCCESS_MS 5000u

#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
/* Latched once the engine's launch counter has been zeroed for this boot. */
static bool g_launch_acknowledged = false;
#endif

/* True if the resident engine forwarded a reset cause to this boot -- i.e. an engine
 * demonstrably ran ahead of us. Latched because cause_take() is one-shot: the record is
 * gone by the time the banner prints, so it cannot be re-read later.
 *
 * Diagnostic wording only. It must NOT gate anything: the cause record was added to the
 * pipe on 2026-08-12 without a layout-version bump, so a compatible resident that predates
 * it forwards nothing and would be misjudged absent. resident_boot_pipe_ready() is the
 * authority on whether an engine is there. */
static bool s_resident_cause_seen = false;

/* CPU state as handed over, captured before this image touches anything. Kept as
 * globals rather than locals so a debugger can read them after a later fault. */
volatile uint32_t g_dbg_xramecc_count = 0;
volatile uint32_t g_dbg_xramecc_stat = 0;
volatile uint32_t g_dbg_xramecc_faddr = 0;
volatile uint32_t g_dbg_app_entry_intcon1 = 0;
volatile uint32_t g_dbg_app_entry_intcon3 = 0;
volatile uint32_t g_dbg_app_entry_intcon4 = 0;
volatile uint32_t g_dbg_app_entry_intcon5 = 0;
volatile uint32_t g_dbg_app_entry_corcon = 0;
volatile uint32_t g_dbg_app_entry_modcon = 0;
volatile uint32_t g_dbg_app_entry_xbrev = 0;
volatile uint32_t g_dbg_app_entry_splim = 0;
volatile uint32_t g_dbg_app_entry_ivtbase = 0;

void __attribute__((interrupt, context)) _XRAMECCInterrupt(void)
{
    g_dbg_xramecc_count++;
    g_dbg_xramecc_stat = RAMXECCSTAT;
    g_dbg_xramecc_faddr = RAMXECCFADDR;
    _XRAMECCIF = 0;
}

void resident_de_app_handoff_mark(char marker)
{
#if RESIDENT_DE_ENA_HANDOFF_TRACE
    while (U1STATbits.TXBF != 0u) {
    }
    U1TXB = (uint8_t)marker;
    while (U1STATbits.TXBF != 0u) {
    }
    U1TXB = (uint8_t)'\r';
    while (U1STATbits.TXBF != 0u) {
    }
    U1TXB = (uint8_t)'\n';
#else
    (void)marker;
#endif
}

void resident_de_app_handoff_entry(void)
{
    g_dbg_app_entry_intcon1 = INTCON1;
    g_dbg_app_entry_intcon3 = INTCON3;
    g_dbg_app_entry_intcon4 = INTCON4;
    g_dbg_app_entry_intcon5 = INTCON5;
    g_dbg_app_entry_corcon = CORCON;
    g_dbg_app_entry_modcon = MODCON;
    g_dbg_app_entry_xbrev = XBREV;
    g_dbg_app_entry_splim = SPLIM;
    g_dbg_app_entry_ivtbase = IVTBASE;

    /* A direct resident handoff does not apply the CPU interrupt-controller
     * reset values. Start the relocated image with every source disabled and
     * every stale flag clear; each application HAL then enables only the
     * vectors it owns before global interrupts are restored. */
    __builtin_disable_interrupts();
    IEC0 = 0u; IEC1 = 0u; IEC2 = 0u; IEC3 = 0u;
    IEC4 = 0u; IEC5 = 0u; IEC6 = 0u; IEC7 = 0u;
    IEC8 = 0u;
    IFS0 = 0u; IFS1 = 0u; IFS2 = 0u; IFS3 = 0u;
    IFS4 = 0u; IFS5 = 0u; IFS6 = 0u; IFS7 = 0u;
    IFS8 = 0u;
    /* How many IEC/IFS banks exist is a device property, not a choice: the
     * AK128MC106 implements 0-8 only and the compiler rejects IEC9 outright
     * ("did you mean 'IPC9'?" -- IPC9 is the register that follows). Same split,
     * and same reason, as src/shared/resident_de_mailbox.c:143-152 and
     * src/boot/resident_de_boot_platform.c:142-151. */
#if defined(__dsPIC33AK512MPS512__)
    IEC9 = 0u; IEC10 = 0u; IEC11 = 0u;
    IFS9 = 0u; IFS10 = 0u; IFS11 = 0u;
#elif !defined(__dsPIC33AK128MC106__)
#error "Unknown target: state how many IEC/IFS banks this part implements."
#endif
    /* Trap status is sticky across a direct branch just like IEC/IFS. The
     * indirect resident-to-CRT transfer can leave ADDRERR pending while GIE is
     * masked; discard all reset-default CPU trap status before restoring GIE. */
    INTCON1bits.BADOPERR = 0u;
    INTCON1bits.ADDRERR = 0u;
    INTCON1bits.STKERR = 0u;
    INTCON3 = 0u;
    INTCON4 = 0u;

    resident_de_app_handoff_mark('M');
}

void resident_de_app_handoff_interrupts_resume(void)
{
    resident_de_app_handoff_mark('U');
    /* A resident handoff is not a hardware reset: the engine masks global
     * interrupts while replacing IVTBASE. Re-enable only after this image has
     * installed its timer and UART state, before the first tick-based delay. */
    __asm__ volatile("mov #0, w0\n\tmov w0, SR" ::: "w0");
    __builtin_enable_interrupts();
}

void resident_de_app_handoff_report(void)
{
    resident_de_app_handoff_mark('D');
    printf(" APP CPU: I1=%08lx I3=%08lx I4=%08lx I5=%08lx COR=%08lx MOD=%08lx XBR=%08lx SPL=%08lx IVT=%08lx\n",
           (unsigned long)g_dbg_app_entry_intcon1,
           (unsigned long)g_dbg_app_entry_intcon3,
           (unsigned long)g_dbg_app_entry_intcon4,
           (unsigned long)g_dbg_app_entry_intcon5,
           (unsigned long)g_dbg_app_entry_corcon,
           (unsigned long)g_dbg_app_entry_modcon,
           (unsigned long)g_dbg_app_entry_xbrev,
           (unsigned long)g_dbg_app_entry_splim,
           (unsigned long)g_dbg_app_entry_ivtbase);
    if (g_dbg_xramecc_count != 0u) {
        printf(" Resident handoff XRAMECC: count=%lu stat=0x%lx faddr=0x%lx\n",
               (unsigned long)g_dbg_xramecc_count,
               (unsigned long)g_dbg_xramecc_stat,
               (unsigned long)g_dbg_xramecc_faddr);
    }
}

bool resident_de_app_latch_forwarded_reset_cause(void)
{
    uint32_t rcon = 0u;

    /* One-shot on both sides: cause_take() clears the record, and the HAL latch refuses a
     * second capture. So this is safe to call unconditionally and exactly once. */
    if (!resident_boot_pipe_cause_take(&rcon))
    {
        return false;
    }
    s_resident_cause_seen = true;
    return nora_reset_snapshot_capture_forwarded(rcon);
}


void resident_de_app_launch_ack_tick(void)
{
#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
    /* Tell the resident engine this boot succeeded, by zeroing its launch-attempt
     * counter. Until this happens the engine keeps counting, and past its limit it
     * stops handing over and waits for an update instead -- which is what breaks a
     * crash-on-startup loop.
     *
     * Deliberately NOT at the end of initialization. An application that
     * initializes cleanly and then dies in the main loop would clear the counter on
     * every boot, so the count would never climb and the guard would never trip.
     * Waiting until the loop has actually been running is the whole point. */
    if (!g_launch_acknowledged &&
        (nora_tick_timer_get_ms() >= APP_LAUNCH_SUCCESS_MS))
    {
        resident_boot_pipe_launch_set(0u);
        g_launch_acknowledged = true;
    }
#endif
}

void resident_de_app_launch_banner(void)
{
#if RESIDENT_BOOT_ENA_LAUNCH_GUARD
    /* How many times the engine has handed over without this application
     * confirming a successful start. Normally 1. A climbing value means earlier
     * boots died before the acknowledgement in the main loop, and past the engine's
     * limit it stops handing over -- so this line is the visible warning before
     * that happens.
     *
     * The crash record is deliberately NOT reported here yet, because nothing
     * writes one yet -- wiring the trap handlers is a separate step. */
    printf(" LaunchAttempts: %u\n", (unsigned)resident_boot_pipe_launch_count());
#endif
}

bool resident_de_app_resident_is_present(void)
{
    uint32_t address;

    /* THE SRAM CONTAINER ALONE IS NOT ENOUGH, measured 2026-09-02. The container lives in
     * no-init RAM and only the resident's boot pass establishes it; nothing else ever wipes
     * it. So after the resident is erased from Flash and the board is merely RESET rather
     * than power-cycled, the established bytes are still sitting there and
     * resident_boot_pipe_ready() answers true with no engine in Flash at all -- which is
     * exactly the state an IDE program-and-run leaves behind, and exactly the state the
     * operator needs the truth about. (It answers false after a power cycle, because the
     * version and its complement do not both survive by chance -- but the check may not
     * depend on someone having pulled the power.)
     *
     * So look at Flash. An application-only image writes ONE thing into the boot region:
     * the reset word at RESIDENT_BOOT_BASE_ADDRESS, pointing at its own entry -- verified
     * against the linked hex, whose next record is at RESIDENT_APP_BASE_ADDRESS. A resident
     * image continues straight on with its interrupt vector table. So a programmed byte
     * ANYWHERE in the boot region past that first reset word means an engine is installed.
     *
     * Scanned rather than spot-checked at one address, so this does not silently become
     * wrong if a future engine's first page is laid out differently -- one page is the
     * smallest thing the NVM HAL can erase, so nothing smaller can be half-present. */
    for (address = RESIDENT_BOOT_BASE_ADDRESS;
         address < (RESIDENT_BOOT_BASE_ADDRESS + NORA_NVM_PAGE_BYTES);
         address += NORA_NVM_WORD_BYTES)
    {
        uint32_t words[NORA_NVM_U32_PER_WORD];
        uint32_t index;

        if (nora_nvm_read_word(address, words) != NORA_NVM_OK)
        {
            return false;
        }
        for (index = 0u; index < NORA_NVM_U32_PER_WORD; index++)
        {
            /* Skip the reset word itself: an application-only image writes it too. */
            if ((address == RESIDENT_BOOT_BASE_ADDRESS) && (index == 0u))
            {
                continue;
            }
            if (words[index] != UINT32_MAX)
            {
                /* An engine is installed. It is only usable, though, if it establishes the
                 * container this build understands -- a resident from another pipe
                 * generation writes its own version and is correctly reported absent. */
                return resident_boot_pipe_ready();
            }
        }
    }
    return false;
}

void resident_de_app_delivery_banner(void)
{
    /* This line used to ASSERT the delivery mode from a compile-time constant, which made
     * it a lie in exactly the case an operator needs the truth: build this configuration in
     * MPLAB X and program it from the IDE, and only the application reaches Flash -- the
     * resident engine is never rebuilt and never paired into a FACTORY_IMAGE (see
     * buildtools/README.md "Support scope"). The board then runs an application that claims
     * "resident bootloader + application" with no engine behind it, and *fu5A appears to be
     * ignored. Two sessions were spent on that. So the line now REPORTS what was observed.
     *
     * The ABI version rides on the same line, so a field log can be matched against the
     * engine's own banner without costing another printf. */
    if (!resident_de_app_resident_is_present())
    {
        printf(" Delivery: application ONLY -- resident bootloader ABSENT"
               " (boot region empty, or a cross-reset layout this build cannot use),"
               " ABI=%u\n",
               (unsigned)RESIDENT_DE_ABI_VERSION);
        printf(" Delivery: WARNING -- serial update (*fu5A) will NOT work on this board."
               " An image built and programmed from MPLAB X contains no resident"
               " bootloader; build and flash with buildtools instead"
               " (switch_config.ps1 -> build.ps1 -> *.factory.production.hex)\n");
        return;
    }
    printf(" Delivery: resident bootloader + application (single panel), ABI=%u"
           " (boot=present, pipe=ok, cause=%s)\n",
           (unsigned)RESIDENT_DE_ABI_VERSION,
           s_resident_cause_seen ? "ok" : "n/a");
}
