#include "resident_de_mailbox.h"

#include <stddef.h>
#include <stdint.h>
#include <xc.h>

#define RESIDENT_BOOT_LAUNCH_MAGIC     UINT32_C(0x53424C41) /* "SBLA" */
#define RESIDENT_BOOT_PROBE_MAGIC      UINT32_C(0x53424D50) /* "SBMP" */
#define RESIDENT_BOOT_REQUEST_COOKIE   UINT32_C(0xA53C19E7)
#define RESIDENT_BOOT_DEFAULT_TRACE_TAG  UINT32_C(0xD17F0000)
#define RESIDENT_BOOT_DEFAULT_TRACE_MASK UINT32_C(0xFFFF0000)

typedef struct
{
    uint32_t magic;
    uint32_t argument0;
    uint32_t argument1;
    uint32_t check;
} resident_boot_request_mailbox_t;

static volatile resident_boot_request_mailbox_t s_mailbox
    __attribute__((persistent, space(xmemory),
                   section(".resident_launch_mailbox"),
                   address(RESIDENT_BOOT_REQUEST_ADDRESS), aligned(16)));

/* A second physical-RAM sentinel distinguishes a local overwrite near the
 * reset stack from destructive MBIST/all-RAM behavior.  The ASRC image has one
 * contiguous 37 KiB BSS object spanning the remaining X RAM, so the only
 * non-disruptive far reservation is at the top of unified/Y data RAM. */
static volatile resident_boot_request_mailbox_t s_far_sentinel
    __attribute__((persistent, space(ymemory),
                   section(".resident_far_sentinel"),
                   address(RESIDENT_BOOT_FAR_SENTINEL_ADDRESS), aligned(16)));

static volatile resident_boot_reset_source_trace_t s_source_trace
    __attribute__((persistent, space(ymemory),
                   section(".resident_reset_source_trace"),
                   address(RESIDENT_BOOT_SOURCE_TRACE_ADDRESS), aligned(16)));

static volatile resident_boot_precrt_trace_t s_precrt_trace
    __attribute__((persistent, space(ymemory),
                   section(".resident_precrt_trace"),
                   address(RESIDENT_BOOT_PRECRT_TRACE_ADDRESS), aligned(16)));

/* The two size budgets are the gaps between the addresses above, so they are the
 * same on both devices even though the addresses are not: 0xE0 from the trace to
 * the pre-CRT record, 0x40 from there to the noinit block. */
_Static_assert(sizeof(resident_boot_reset_source_trace_t) <= 0xE0u,
               "reset source trace overruns the pre-CRT record 0xE0 bytes above it");
_Static_assert(sizeof(resident_boot_precrt_trace_t) <= 0x40u,
               "pre-CRT trace overruns the noinit_ram block 0x40 bytes above it");
_Static_assert((RESIDENT_BOOT_PRECRT_TRACE_ADDRESS -
                RESIDENT_BOOT_SOURCE_TRACE_ADDRESS) == 0xE0u,
               "the diagnostic reservation must keep its shape on every device");
_Static_assert((sizeof(resident_boot_reset_source_trace_t) % sizeof(uint32_t)) == 0u,
               "reset source trace must contain whole 32-bit words");

static uint32_t read_sr(void)
{
    uint16_t value;
    __asm__ volatile("mov SR, %0" : "=r"(value));
    return (uint32_t)value;
}

static uint32_t read_w15(void)
{
    uint32_t value;
    __asm__ volatile("mov.l w15, %0" : "=r"(value));
    return value;
}

static __attribute__((always_inline)) inline void
mailbox_write(volatile resident_boot_request_mailbox_t *mailbox,
              uint32_t magic, uint32_t argument0, uint32_t argument1)
{
    mailbox->magic = 0u;
    mailbox->argument0 = argument0;
    mailbox->argument1 = argument1;
    mailbox->check = magic ^ argument0 ^ argument1 ^
                     RESIDENT_BOOT_REQUEST_COOKIE;
    __asm__ volatile ("" ::: "memory");
    mailbox->magic = magic;
}

static void mailbox_clear(void)
{
    s_mailbox.magic = 0u;
    s_mailbox.argument0 = 0u;
    s_mailbox.argument1 = 0u;
    s_mailbox.check = 0u;
    s_far_sentinel.magic = 0u;
    s_far_sentinel.argument0 = 0u;
    s_far_sentinel.argument1 = 0u;
    s_far_sentinel.check = 0u;
}

void resident_boot_mailbox_invalidate(void)
{
    mailbox_clear();
}

static __attribute__((always_inline)) inline void
mailbox_publish(uint32_t magic, uint32_t argument0, uint32_t argument1)
{
    /* Publish magic last so reset or an observer cannot accept a torn record. */
    mailbox_write(&s_mailbox, magic, argument0, argument1);
    mailbox_write(&s_far_sentinel, magic, argument0, argument1);
}

static __attribute__((always_inline)) inline void
reset_source_capture_impl(uint32_t source)
{
    const volatile uint32_t *words =
        (const volatile uint32_t *)&s_source_trace;
    uint32_t check;
    size_t index;

    s_source_trace.magic = 0u;
    s_source_trace.source = source;
    s_source_trace.rcon = RCON;
    s_source_trace.mbistcon = MBISTCON;
    s_source_trace.ramxecccon = RAMXECCCON;
    s_source_trace.ramyecccon = RAMYECCCON;
    s_source_trace.pwbxecccon = PWBXECCCON;
    s_source_trace.pwbyecccon = PWBYECCCON;
    s_source_trace.dmacon = DMACON;
    s_source_trace.sr = read_sr();
    s_source_trace.splim = SPLIM;
    s_source_trace.w15 = read_w15();
    s_source_trace.intcon1 = INTCON1;
    s_source_trace.intcon3 = INTCON3;
    s_source_trace.intcon4 = INTCON4;
    s_source_trace.iec[0] = IEC0; s_source_trace.iec[1] = IEC1;
    s_source_trace.iec[2] = IEC2; s_source_trace.iec[3] = IEC3;
    s_source_trace.iec[4] = IEC4; s_source_trace.iec[5] = IEC5;
    s_source_trace.iec[6] = IEC6; s_source_trace.iec[7] = IEC7;
    s_source_trace.iec[8] = IEC8;
    s_source_trace.ifs[0] = IFS0; s_source_trace.ifs[1] = IFS1;
    s_source_trace.ifs[2] = IFS2; s_source_trace.ifs[3] = IFS3;
    s_source_trace.ifs[4] = IFS4; s_source_trace.ifs[5] = IFS5;
    s_source_trace.ifs[6] = IFS6; s_source_trace.ifs[7] = IFS7;
    s_source_trace.ifs[8] = IFS8;
#if defined(__dsPIC33AK512MPS512__)
    s_source_trace.iec[9] = IEC9;
    s_source_trace.iec[10] = IEC10; s_source_trace.iec[11] = IEC11;
    s_source_trace.ifs[9] = IFS9;
    s_source_trace.ifs[10] = IFS10; s_source_trace.ifs[11] = IFS11;
#elif defined(__dsPIC33AK128MC106__)
    /* AK128MC106 only implements IEC0-8/IFS0-8; the struct keeps 12 slots so
     * the trace record layout stays identical across devices. */
    s_source_trace.iec[9] = 0u; s_source_trace.iec[10] = 0u; s_source_trace.iec[11] = 0u;
    s_source_trace.ifs[9] = 0u; s_source_trace.ifs[10] = 0u; s_source_trace.ifs[11] = 0u;
#else
#error "resident_boot_request.c: unsupported device -- expects __dsPIC33AK512MPS512__ or __dsPIC33AK128MC106__."
#endif
    s_source_trace.clkgen[0] = CLK1CON; s_source_trace.clkgen[1] = CLK1DIV;
    s_source_trace.clkgen[2] = CLK6CON; s_source_trace.clkgen[3] = CLK6DIV;
    s_source_trace.clkgen[4] = CLK8CON; s_source_trace.clkgen[5] = CLK8DIV;
    s_source_trace.mailbox_a[0] = s_mailbox.magic;
    s_source_trace.mailbox_a[1] = s_mailbox.argument0;
    s_source_trace.mailbox_a[2] = s_mailbox.argument1;
    s_source_trace.mailbox_a[3] = s_mailbox.check;
    s_source_trace.mailbox_b[0] = s_far_sentinel.magic;
    s_source_trace.mailbox_b[1] = s_far_sentinel.argument0;
    s_source_trace.mailbox_b[2] = s_far_sentinel.argument1;
    s_source_trace.mailbox_b[3] = s_far_sentinel.check;
    check = RESIDENT_BOOT_SOURCE_TRACE_MAGIC ^ RESIDENT_BOOT_REQUEST_COOKIE;
    for (index = 1u;
         index < (sizeof(s_source_trace) / sizeof(uint32_t)) - 1u;
         index++) {
        check ^= words[index];
    }
    s_source_trace.check = check;
    __asm__ volatile ("" ::: "memory");
    s_source_trace.magic = RESIDENT_BOOT_SOURCE_TRACE_MAGIC;
}

void resident_boot_mailbox_snapshot(uint32_t words[4])
{
    if (words == NULL) {
        return;
    }
    words[0] = s_mailbox.magic;
    words[1] = s_mailbox.argument0;
    words[2] = s_mailbox.argument1;
    words[3] = s_mailbox.check;
}

void resident_boot_far_sentinel_snapshot(uint32_t words[4])
{
    if (words == NULL) {
        return;
    }
    words[0] = s_far_sentinel.magic;
    words[1] = s_far_sentinel.argument0;
    words[2] = s_far_sentinel.argument1;
    words[3] = s_far_sentinel.check;
}

void resident_boot_reset_source_snapshot(resident_boot_reset_source_trace_t *trace)
{
    const volatile uint32_t *source = (const volatile uint32_t *)&s_source_trace;
    uint32_t *destination = (uint32_t *)trace;
    size_t index;

    if (trace == NULL) {
        return;
    }
    for (index = 0u; index < sizeof(*trace) / sizeof(uint32_t); index++) {
        destination[index] = source[index];
    }
}

void resident_boot_precrt_snapshot(resident_boot_precrt_trace_t *trace)
{
    const volatile uint32_t *source = (const volatile uint32_t *)&s_precrt_trace;
    uint32_t *destination = (uint32_t *)trace;
    size_t index;

    if (trace == NULL) {
        return;
    }
    for (index = 0u; index < sizeof(*trace) / sizeof(uint32_t); index++) {
        destination[index] = source[index];
    }
}

void resident_boot_default_interrupt_capture(uint32_t vector,
                                             uint32_t detail)
{
    /* These words are intentionally left untouched by the stackless pre-CRT
     * shim. Publish a tagged, one-shot record so a later unrelated reset cannot
     * print an old vector as if it belonged to the current reset. */
    s_precrt_trace.default_vector = 0u;
    s_precrt_trace.default_detail = detail;
    __asm__ volatile ("" ::: "memory");
    s_precrt_trace.default_vector = RESIDENT_BOOT_DEFAULT_TRACE_TAG |
                                     (vector & ~RESIDENT_BOOT_DEFAULT_TRACE_MASK);
}

bool resident_boot_default_interrupt_take(uint32_t *vector,
                                          uint32_t *detail)
{
    const uint32_t tagged_vector = s_precrt_trace.default_vector;
    const uint32_t captured_detail = s_precrt_trace.default_detail;
    const bool valid = (tagged_vector & RESIDENT_BOOT_DEFAULT_TRACE_MASK) ==
                       RESIDENT_BOOT_DEFAULT_TRACE_TAG;

    s_precrt_trace.default_vector = 0u;
    s_precrt_trace.default_detail = 0u;
    if (valid && (vector != NULL)) {
        *vector = tagged_vector & ~RESIDENT_BOOT_DEFAULT_TRACE_MASK;
    }
    if (valid && (detail != NULL)) {
        *detail = captured_detail;
    }
    return valid;
}

bool resident_boot_reset_source_trace_valid(
    const resident_boot_reset_source_trace_t *trace)
{
    const uint32_t *words = (const uint32_t *)trace;
    uint32_t check;
    size_t index;

    if ((trace == NULL) ||
        (trace->magic != RESIDENT_BOOT_SOURCE_TRACE_MAGIC)) {
        return false;
    }
    check = RESIDENT_BOOT_SOURCE_TRACE_MAGIC ^ RESIDENT_BOOT_REQUEST_COOKIE;
    for (index = 1u; index < (sizeof(*trace) / sizeof(uint32_t)) - 1u;
         index++) {
        check ^= words[index];
    }
    return trace->check == check;
}

void resident_boot_reset_sync(uint32_t source)
{
    /* UART must already be drained by the caller. */
    __builtin_disable_interrupts();
    DMACONbits.ON = 0u;
    reset_source_capture_impl(source);
    __asm__ volatile("reset" ::: "memory");
    for (;;) {
    }
}

void resident_boot_launch_reset_sync(uint32_t ivt_address,
                                     uint32_t entry_address)
{
    uint32_t settle;

    /* UART must already be drained by the caller.  Match the App probe tail:
     * stop asynchronous writers before publishing either sentinel. */
    __builtin_disable_interrupts();
    DMACONbits.ON = 0u;

    /* Resident runs CLKGEN1 directly from FRC. Quiesce both PLL analog blocks
     * here, where doing so cannot remove the CPU clock, and give the next App
     * one uniform launch baseline regardless of which App requested recovery. */
    PLL1CONbits.ON = 0u;
    PLL2CONbits.ON = 0u;
    for (settle = 0u; settle < 10000u; settle++) {
        Nop();
    }

    mailbox_publish(RESIDENT_BOOT_LAUNCH_MAGIC, ivt_address, entry_address);
    reset_source_capture_impl(RESIDENT_BOOT_RESET_SOURCE_RESIDENT);
    __asm__ volatile("reset" ::: "memory");
    for (;;) {
    }
}

bool resident_boot_launch_take(uint32_t *ivt_address, uint32_t *entry_address)
{
    const uint32_t magic = s_mailbox.magic;
    const uint32_t ivt = s_mailbox.argument0;
    const uint32_t entry = s_mailbox.argument1;
    const uint32_t check = magic ^ ivt ^ entry ^ RESIDENT_BOOT_REQUEST_COOKIE;

    if ((magic != RESIDENT_BOOT_LAUNCH_MAGIC) ||
        (s_mailbox.check != check)) {
        return false;
    }
    mailbox_clear();
    if (ivt_address != NULL) {
        *ivt_address = ivt;
    }
    if (entry_address != NULL) {
        *entry_address = entry;
    }
    return true;
}

void resident_boot_probe_set(uint32_t token)
{
    mailbox_publish(RESIDENT_BOOT_PROBE_MAGIC, token, ~token);
}

void resident_boot_probe_reset_sync(uint32_t token)
{
    /* UART must already be drained by the caller.  From this point onward no
     * interrupt, DMA transfer, delay, printf, or out-of-line helper may run. */
    __builtin_disable_interrupts();
    DMACONbits.ON = 0u;
    mailbox_publish(RESIDENT_BOOT_PROBE_MAGIC, token, ~token);
    reset_source_capture_impl(RESIDENT_BOOT_RESET_SOURCE_APP);
    __asm__ volatile("reset" ::: "memory");
    for (;;) {
    }
}

bool resident_boot_probe_take(uint32_t *token)
{
    const uint32_t magic = s_mailbox.magic;
    const uint32_t value = s_mailbox.argument0;
    const uint32_t inverse = s_mailbox.argument1;
    const uint32_t check = magic ^ value ^ inverse ^
                           RESIDENT_BOOT_REQUEST_COOKIE;

    if ((magic != RESIDENT_BOOT_PROBE_MAGIC) || (inverse != ~value) ||
        (s_mailbox.check != check)) {
        return false;
    }
    mailbox_clear();
    if (token != NULL) {
        *token = value;
    }
    return true;
}
