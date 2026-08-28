#include "nora_nvm.h"

#include <xc.h>

//===========================================================
// NVMCON.NVMOP operation codes (DS70005591C, section 7.2.1 / 7.3.2.2).
//   0b0001 Word program   (data source: NVMDATA0..3)
//   0b0010 Row  program   (data source: RAM buffer at NVMSRCADR)
//   0b0011 Page erase      (8 rows)
//===========================================================
#define NVMOP_WORD_PROGRAM (0x1U)
#define NVMOP_ROW_PROGRAM  (0x2U)
#define NVMOP_PAGE_ERASE   (0x3U)

// WREC captured from the last operation (0 == success). Diagnostics only.
static uint8_t s_last_wrec = 0U;
static uint8_t s_last_crcec = 0U;

//-----------------------------------------------------------
// Run one program/erase operation to completion.
//
// Precondition: NVMADR (+ NVMDATA0..3 or NVMSRCADR) already loaded for `op`.
// Sequence per DS70005591C 7.3.2.2: clear stale WRERR, select NVMOP, WREN=1,
// set WR, wait for hardware to clear WR (the CPU stalls anyway), WREN=0, then
// test WRERR. Returns OK when WRERR == 0.
//-----------------------------------------------------------
static nora_nvm_status_t nvm_execute(uint8_t op)
{
    if (NVMCONbits.LOCK != 0U)
    {
        return NORA_NVM_ERR_LOCKED;
    }

    NVMCONbits.WRERR = 0U;    // clear stale error status before starting
    NVMCONbits.NVMOP = op;
    NVMCONbits.WREN  = 1U;    // enable program/erase

    /* DS80001162E errata 26: an active-space WR with CHECOH=1 can
     * automatically invalidate the PBU cache. A simultaneous Flash IVT fetch
     * may then stall the CPU until a hardware reset. Inactive dual-bank aliases
     * start at 0xC00000 and do not meet the erratum's address condition. Active
     * writers in this repository reset before executing the modified range. */
    if (NVMADR < NORA_NVM_INACTIVE_BASE)
    {
        CHECONbits.CHECOH = 0U;
    }

    NVMCONbits.WR    = 1U;    // start; CPU stalls here until the op completes
    /* DS70005591C 3.4.3.2.1 recommends four NOPs after the instruction that
     * initiates Flash programming so subsequent pipeline instructions have no
     * side effects. */
    __asm__ volatile ("nop\n nop\n nop\n nop" ::: "memory");
    while (NVMCONbits.WR != 0U)
    {
        // Hardware clears WR on completion. The CPU is stalled during the
        // operation, so this loop is a robustness belt-and-braces guard.
    }

    NVMCONbits.WREN = 0U;     // disable further writes

    s_last_wrec = (uint8_t)NVMCONbits.WREC;

    return (NVMCONbits.WRERR != 0U) ? NORA_NVM_ERR_WRERR : NORA_NVM_OK;
}

bool nora_nvm_is_partition2_active(void)
{
    return (NVMCONbits.P2ACTIV != 0U);
}

nora_nvm_status_t nora_nvm_page_erase(uint32_t page_addr)
{
    if (!nora_nvm_is_page_aligned(page_addr))
    {
        return NORA_NVM_ERR_ARG;
    }

    // Any address within the target page selects that page for erase.
    NVMADR = page_addr;

    return nvm_execute(NVMOP_PAGE_ERASE);
}

nora_nvm_status_t nora_nvm_word_program(uint32_t word_addr, const uint32_t data[NORA_NVM_U32_PER_WORD])
{
    if ((data == NULL) || !nora_nvm_is_word_aligned(word_addr))
    {
        return NORA_NVM_ERR_ARG;
    }

    // Load the 128-bit word to program. NVMDATA0 = bits 31:0 ... NVMDATA3 = 127:96.
    NVMDATA0 = data[0];
    NVMDATA1 = data[1];
    NVMDATA2 = data[2];
    NVMDATA3 = data[3];

    NVMADR = word_addr;       // NVMADR[3:0] == 0 forces Flash-word alignment

    return nvm_execute(NVMOP_WORD_PROGRAM);
}

nora_nvm_status_t nora_nvm_row_program(uint32_t row_addr, const uint32_t *ram_src)
{
    if ((ram_src == NULL) || !nora_nvm_is_row_aligned(row_addr))
    {
        return NORA_NVM_ERR_ARG;
    }

    // NVMSRCADR takes the data-RAM byte address of the first word of the row.
    // SRCADR[23:2] is used, so the buffer must be 4-byte aligned.
    if (((uint32_t)(uintptr_t)ram_src & 0x3U) != 0U)
    {
        return NORA_NVM_ERR_ARG;
    }

    NVMSRCADR = (uint32_t)(uintptr_t)ram_src;
    NVMADR    = row_addr;

    return nvm_execute(NVMOP_ROW_PROGRAM);
}

nora_nvm_status_t nora_nvm_read_word(uint32_t word_addr, uint32_t out[NORA_NVM_U32_PER_WORD])
{
    const volatile uint32_t *source;
    uint32_t index;

    if ((out == NULL) || !nora_nvm_is_word_aligned(word_addr))
    {
        return NORA_NVM_ERR_ARG;
    }

    // dsPIC33A has a unified/linear address space, so program memory is read
    // with a plain volatile pointer -- no PSV / table-read setup.
    source = (const volatile uint32_t *)(uintptr_t)word_addr;
    for (index = 0U; index < NORA_NVM_U32_PER_WORD; index++)
    {
        out[index] = source[index];
    }

    return NORA_NVM_OK;
}

// NOTE: `expect` MUST point into data RAM, not program flash. This core takes an
// address-error trap on a runtime data-pointer read of a flash-resident object,
// so comparing flash against another flash location is not supported. In the
// bootloader `expect` is always the received/source block in RAM, which matches.
nora_nvm_status_t nora_nvm_verify(uint32_t flash_addr, const void *expect, uint32_t len_bytes)
{
    const volatile uint32_t *flash;
    const uint32_t *want;
    uint32_t count;
    uint32_t index;

    if ((expect == NULL) || !nora_nvm_is_word_aligned(flash_addr) ||
        ((len_bytes & (NORA_NVM_WORD_BYTES - 1U)) != 0U))
    {
        return NORA_NVM_ERR_ARG;
    }

    flash = (const volatile uint32_t *)(uintptr_t)flash_addr;
    want  = (const uint32_t *)expect;
    count = len_bytes / 4U;

    for (index = 0U; index < count; index++)
    {
        if (flash[index] != want[index])
        {
            return NORA_NVM_ERR_VERIFY;
        }
    }

    return NORA_NVM_OK;
}

nora_nvm_status_t nora_nvm_crc_preflight(uint32_t flash_addr,
                                         uint32_t len_bytes)
{
    uint32_t end_address;
    uint32_t first_page;
    uint32_t last_page_end;

    if ((len_bytes == 0U) || (flash_addr > UINT32_C(0x00FFFFFF)) ||
        ((len_bytes - 1U) > (UINT32_C(0x00FFFFFF) - flash_addr)))
    {
        s_last_crcec = 3U; /* invalid address, matching the hardware code */
        return NORA_NVM_ERR_ARG;
    }

    end_address = flash_addr + len_bytes - 1U;
    first_page = flash_addr & ~(uint32_t)(NORA_NVM_PAGE_BYTES - 1U);
    last_page_end = end_address | (uint32_t)(NORA_NVM_PAGE_BYTES - 1U);

    /* DS70005591C 7.3.4: ON must be cycled before a new calculation. The
     * engine requires inclusive 4 KB page boundaries and reports completion by
     * clearing START and setting NVMCRCIF. */
    NVMCRCCONbits.ON = 0U;
    _NVMCRCIF = 0;
    NVMCRCST = first_page;
    NVMCRCEND = last_page_end;
    NVMCRCSEED = UINT32_MAX;
    NVMCRCCONbits.CRCIDL = 0U; /* run in CPU Run and Idle modes */
    NVMCRCCONbits.DELAY = 0U;  /* foreground preflight, no rate limiting */
    NVMCRCCONbits.ON = 1U;
    NVMCRCCONbits.START = 1U;
    while (NVMCRCCONbits.START != 0U)
    {
        /* Hardware clears START on completion. */
    }

    s_last_crcec = (uint8_t)NVMCRCCONbits.CRCEC;
    NVMCRCCONbits.ON = 0U;
    _NVMCRCIF = 0;

    if (s_last_crcec == 0U)
    {
        return NORA_NVM_OK;
    }
    if (s_last_crcec == 2U)
    {
        return NORA_NVM_ERR_ECC;
    }
    return NORA_NVM_ERR_CRC_ENGINE;
}

uint8_t nora_nvm_last_wrec(void)
{
    return s_last_wrec;
}

uint8_t nora_nvm_last_crc_error(void)
{
    return s_last_crcec;
}
