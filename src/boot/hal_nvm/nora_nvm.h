#ifndef NORA_NVM_H
#define NORA_NVM_H

//===========================================================
// nora_nvm.h / nora_nvm_dspic33ak.c -- Run-Time Self-Programming (RTSP) driver
// for the dsPIC33AK512MPS512 internal Flash program memory.
//
// This is the low-level flash program/erase/read primitive used by the serial
// self-flash LiveUpdate feature. It is deliberately application-agnostic: it
// erases pages and programs words/rows at absolute program-memory addresses and
// verifies by read-back. The A/B (dual partition) policy -- always writing the
// *inactive* partition -- lives one layer up in fw_update/.
//
// Reference: dsPIC33AK512MPS512 Family Data Sheet DS70005591C, sections 7.2/7.3
// (RTSP/NVM CRC) and 6.4 (Flash Dual Partition), plus silicon errata
// DS80001162E item 26 (PBU invalidation versus Flash IVT fetch).
//
// Geometry (dsPIC33A, 128-bit Flash word + ECC; unified/linear address space):
//   * Flash WORD  = 128 bits = 16 bytes  -> occupies 0x10 address units.
//                   Smallest programmable unit. NVMADR[3:0] must be 0.
//   * Flash ROW   = 32 words = 512 bytes -> 0x200 address units.
//                   Bus-mastered from a RAM source buffer (NVMSRCADR).
//   * Flash PAGE  = 8 rows   = 4096 bytes -> 0x1000 address units.
//                   Smallest erasable unit. Erase-before-program is required.
//
// Programming a given Flash word more than once corrupts its ECC parity: always
// erase the containing page first, and never re-program a word.
//
// Unlock: dsPIC33A has NO NVMKEY sequence (the legacy 0xAA/0x55 unlock is replaced
// by the Peripheral Access Controller). At reset the PAC leaves NVMCON writable,
// so no unlock step is needed here as long as firmware never PAC-locks NVMCON.
//
// Concurrency: setting NVMCON.WR stalls the CPU until the operation completes
// (interrupts stay pending, not serviced). The caller must ensure no other code
// path touches the NVM SFRs during a call. During a field update the audio path
// is expected to be stopped.
//===========================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------
// Flash geometry (address units == bytes in the dsPIC33A linear map).
//----------------------------------------------------------------
#define NORA_NVM_WORD_BYTES   (16U)     // 128-bit Flash word
#define NORA_NVM_ROW_BYTES    (512U)    // 32 words
#define NORA_NVM_PAGE_BYTES   (4096U)   // 8 rows

#define NORA_NVM_WORDS_PER_ROW   (NORA_NVM_ROW_BYTES / NORA_NVM_WORD_BYTES)   // 32
#define NORA_NVM_U32_PER_WORD    (NORA_NVM_WORD_BYTES / 4U)                        // 4
#define NORA_NVM_U32_PER_ROW     (NORA_NVM_ROW_BYTES / 4U)                         // 128

//----------------------------------------------------------------
// Dual-partition address map (DS70005591C, section 7.4.1).
//   Active   partition base:  0x800000
//   Inactive partition base:  0xC00000  (== active | 0x400000)
// An address in the active space is aliased into the inactive space by ORing
// 0x400000. The updater programs the inactive partition via this alias.
//----------------------------------------------------------------
#define NORA_NVM_ACTIVE_BASE     UINT32_C(0x800000)
#define NORA_NVM_INACTIVE_BASE   UINT32_C(0xC00000)
#define NORA_NVM_INACTIVE_ALIAS  UINT32_C(0x400000)

// Map an active-space program address to the inactive-partition alias.
#define NORA_NVM_TO_INACTIVE(addr) ((uint32_t)(addr) | NORA_NVM_INACTIVE_ALIAS)

//----------------------------------------------------------------
// Result / status.
//----------------------------------------------------------------
typedef enum
{
    NORA_NVM_OK = 0,          // operation completed, WRERR == 0
    NORA_NVM_ERR_ARG,         // NULL pointer or misaligned address
    NORA_NVM_ERR_LOCKED,      // NVMCON.LOCK is set (one-way, until reset)
    NORA_NVM_ERR_WRERR,       // hardware reported WRERR (see WREC via nora_nvm_last_wrec)
    NORA_NVM_ERR_VERIFY,      // read-back did not match the source data
    NORA_NVM_ERR_ECC,         // NVM CRC preflight found a Flash ECC DED error
    NORA_NVM_ERR_CRC_ENGINE   // NVM CRC address/security/other engine error
} nora_nvm_status_t;

//----------------------------------------------------------------
// Alignment predicates (pure, no hardware access).
//----------------------------------------------------------------
static inline bool nora_nvm_is_word_aligned(uint32_t addr) { return (addr & (NORA_NVM_WORD_BYTES - 1U)) == 0U; }
static inline bool nora_nvm_is_row_aligned (uint32_t addr) { return (addr & (NORA_NVM_ROW_BYTES  - 1U)) == 0U; }
static inline bool nora_nvm_is_page_aligned(uint32_t addr) { return (addr & (NORA_NVM_PAGE_BYTES - 1U)) == 0U; }

//----------------------------------------------------------------
// Which physical partition is currently active (NVMCON.P2ACTIV).
// false -> Partition 1 active; true -> Partition 2 active.
//----------------------------------------------------------------
bool nora_nvm_is_partition2_active(void);

//----------------------------------------------------------------
// Erase the 4 KB page that contains program address `page_addr`.
// `page_addr` must be page-aligned. Blocks until complete (CPU stalls).
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_page_erase(uint32_t page_addr);

//----------------------------------------------------------------
// Program one 128-bit Flash word (4 x uint32_t) at `word_addr`.
// `word_addr` must be word-aligned; `data` points to 4 uint32_t.
// The page must already be erased. Blocks until complete.
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_word_program(uint32_t word_addr, const uint32_t data[NORA_NVM_U32_PER_WORD]);

//----------------------------------------------------------------
// Program one 512-byte row (32 words) at `row_addr` from a RAM source buffer.
// `row_addr` must be row-aligned; `ram_src` points to 128 uint32_t in data RAM.
// The page must already be erased. Blocks until complete.
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_row_program(uint32_t row_addr, const uint32_t *ram_src);

//----------------------------------------------------------------
// Read one 128-bit Flash word (4 x uint32_t) from `word_addr` into `out`.
// Plain linear read; no PSV/table setup on dsPIC33A. `word_addr` word-aligned.
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_read_word(uint32_t word_addr, uint32_t out[NORA_NVM_U32_PER_WORD]);

//----------------------------------------------------------------
// Convenience: verify `len_bytes` (multiple of 16) of program memory at
// `flash_addr` against `expect` in RAM. Returns OK only on an exact match.
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_verify(uint32_t flash_addr, const void *expect, uint32_t len_bytes);

//----------------------------------------------------------------
// Scan every 4 KB Flash page touched by [flash_addr, flash_addr+len_bytes)
// through the NVM CRC engine. The calculated CRC is deliberately ignored: this
// is an ECC/security preflight before the CPU reads Flash directly. Unlike a
// CPU read, the CRC engine reports ECC DED through CRCEC without taking a trap.
//----------------------------------------------------------------
nora_nvm_status_t nora_nvm_crc_preflight(uint32_t flash_addr,
                                         uint32_t len_bytes);

//----------------------------------------------------------------
// The NVMCON.WREC field captured after the most recent program/erase (0 when the
// last operation succeeded). For diagnostics/telemetry after an ERR_WRERR.
//----------------------------------------------------------------
uint8_t nora_nvm_last_wrec(void);

// Raw NVMCRCCON.CRCEC captured by the most recent nora_nvm_crc_preflight call.
uint8_t nora_nvm_last_crc_error(void);

#ifdef __cplusplus
}
#endif

#endif // NORA_NVM_H
