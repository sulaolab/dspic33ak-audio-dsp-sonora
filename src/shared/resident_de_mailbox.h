#ifndef RESIDENT_BOOT_REQUEST_H
#define RESIDENT_BOOT_REQUEST_H

#include <stdbool.h>
#include <stdint.h>

/* The launch mailbox sits at the BOTTOM of data RAM, which is the same address on
 * every part in the family, so it is stated once. The three diagnostic records sit
 * at the TOP, in the 512-byte reservation, so they move with the size of data RAM:
 * each is "top of RAM minus a fixed offset", the same decision on both devices
 * (see shared/noinit_ram_config.h, which chooses the noinit block that shares the
 * same reservation, exactly this way).
 *
 *   AK512MPS512   data 0x4000..0x13FFF   sentinel 0x13E00  trace 0x13E10  pre-CRT 0x13EF0
 *   AK128MC106    data 0x4000..0x7FFF    sentinel  0x7E00  trace  0x7E10  pre-CRT  0x7EF0
 *
 * A device without an arm is a compile error rather than an AK512 address on a part
 * that has no such RAM: address() outside the data region links silently and only
 * faults with a board in front of you (measured -- see noinit_ram_config.h). */
#define RESIDENT_BOOT_REQUEST_ADDRESS       UINT32_C(0x04050)
#if defined(__dsPIC33AK512MPS512__)
  #define RESIDENT_BOOT_FAR_SENTINEL_ADDRESS  UINT32_C(0x13E00)
  #define RESIDENT_BOOT_SOURCE_TRACE_ADDRESS  UINT32_C(0x13E10)
  #define RESIDENT_BOOT_PRECRT_TRACE_ADDRESS  UINT32_C(0x13EF0)
#elif defined(__dsPIC33AK128MC106__)
  #define RESIDENT_BOOT_FAR_SENTINEL_ADDRESS  UINT32_C(0x07E00)
  #define RESIDENT_BOOT_SOURCE_TRACE_ADDRESS  UINT32_C(0x07E10)
  #define RESIDENT_BOOT_PRECRT_TRACE_ADDRESS  UINT32_C(0x07EF0)
#else
  #error "resident_de_mailbox.h: unsupported device -- expects __dsPIC33AK512MPS512__ or __dsPIC33AK128MC106__."
#endif

#define RESIDENT_BOOT_RESET_SOURCE_APP       UINT32_C(0x41505031) /* "APP1" */
#define RESIDENT_BOOT_RESET_SOURCE_RESIDENT  UINT32_C(0x52455331) /* "RES1" */
#define RESIDENT_BOOT_SOURCE_TRACE_MAGIC      UINT32_C(0x52535453) /* "RSTS" */
#define RESIDENT_BOOT_PRECRT_TRACE_MAGIC      UINT32_C(0x50435231) /* "PCR1" */

typedef struct
{
    uint32_t magic;
    uint32_t source;
    uint32_t rcon;
    uint32_t mbistcon;
    uint32_t ramxecccon;
    uint32_t ramyecccon;
    uint32_t pwbxecccon;
    uint32_t pwbyecccon;
    uint32_t dmacon;
    uint32_t sr;
    uint32_t splim;
    uint32_t w15;
    uint32_t intcon1;
    uint32_t intcon3;
    uint32_t intcon4;
    uint32_t iec[12];
    uint32_t ifs[12];
    uint32_t clkgen[6];
    uint32_t mailbox_a[4];
    uint32_t mailbox_b[4];
    uint32_t check;
} resident_boot_reset_source_trace_t;

typedef struct
{
    uint32_t magic;
    uint32_t mailbox_a[4];
    uint32_t mailbox_b[4];
    uint32_t rcon;
    uint32_t mbistcon;
    uint32_t w15;
    uint32_t splim;
    uint32_t sr;
    uint32_t default_vector;
    uint32_t default_detail;
} resident_boot_precrt_trace_t;

void resident_boot_mailbox_snapshot(uint32_t words[4]);
void resident_boot_far_sentinel_snapshot(uint32_t words[4]);
void resident_boot_mailbox_invalidate(void);
void resident_boot_reset_source_snapshot(resident_boot_reset_source_trace_t *trace);
void resident_boot_precrt_snapshot(resident_boot_precrt_trace_t *trace);
void resident_boot_default_interrupt_capture(uint32_t vector,
                                             uint32_t detail);
bool resident_boot_default_interrupt_take(uint32_t *vector,
                                          uint32_t *detail);
bool resident_boot_reset_source_trace_valid(
    const resident_boot_reset_source_trace_t *trace);
void resident_boot_reset_sync(uint32_t source) __attribute__((noreturn));
void resident_boot_launch_reset_sync(uint32_t ivt_address,
                                     uint32_t entry_address)
    __attribute__((noreturn));
bool resident_boot_launch_take(uint32_t *ivt_address, uint32_t *entry_address);
void resident_boot_probe_set(uint32_t token);
void resident_boot_probe_reset_sync(uint32_t token) __attribute__((noreturn));
bool resident_boot_probe_take(uint32_t *token);

#endif
