
//===========================================================
// INCLUDES
//===========================================================
#include "resolved_board_config.h"
#if RESOLVED_BOARD_USE_SST26

#include "app_runtime_overrides.h"
#include <xc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "timer_app.h"
#include "nora_spi.h"
#include "nora_gpio.h"
#include "nora_pps.h"


#include "board/devices/SST26_drv.h"


//===========================================================
// Definition
//===========================================================
// Unknown-device guard is centralized in the resolved board target adapter.


// SST26 JEDEC ID
#define MANUFACTURER_ID            (0xBF)
#define DEVICE_TYP_ID              (0x26)
#define DEVICE_ID                  (0x12)

#define SST26_PAGE_SIZE            (256)


//-----------------------------------------------------------
// SPI bus binding (via the instance-capable SPI HAL)
//-----------------------------------------------------------
// SST26 uses SPI4. The SPI bus transfer is provided by src/hal_spi (nora_spi_*),
// which does NOT touch PPS/GPIO. This board component driver owns the Curiosity
// board's SST26 SPI4 pin assignment (PPS + GPIO pre-config) and CS/WP/RST setup.
#define SST26_SPI_INSTANCE         (NORA_SPI_INST_4)
#define SST26_SPI_MODE             (NORA_SPI_MODE_0)
#define SST26_SPI_PBCLK_HZ         (100000000UL)   // SPI peripheral clock (Hz)
#define SST26_SPI_SCK_HZ           (12500000UL)    // target SCK (Hz)

// SST26 external flash pin map on the AK512 Curiosity board.
// Remappable SPI pins: SDO4=RA12/RP13, SDI4=RA13/RP14, SCK4=RE1/RP66.
// Control pins: CS=RD15/RP64, RST=RE0/RP65, WP=RE3/RP68.
#define SST26_RP_SDO               ((nora_gpio_rp_t)13u)   // SDO4 -> RA12
#define SST26_RP_SDI               ((nora_gpio_rp_t)14u)   // SDI4 <- RA13
#define SST26_RP_SCK               ((nora_gpio_rp_t)66u)   // SCK4 -> RE1
#define SST26_RP_CS                ((nora_gpio_rp_t)64u)   // CS   = RD15, active low
#define SST26_RP_RST               ((nora_gpio_rp_t)65u)   // RST  = RE0, active low
#define SST26_RP_WP                ((nora_gpio_rp_t)68u)   // WP   = RE3, active low

// CS is active-low: assert = drive low, deassert = drive high.
#define SST26_CS_ASSERT()          nora_gpio_rp_clear(SST26_RP_CS)
#define SST26_CS_DEASSERT()        nora_gpio_rp_set(SST26_RP_CS)

// SPI bus operations used by the flash command layer below.
#define SPI_CS_ASSERT()            SST26_CS_ASSERT()
#define SPI_CS_DEASSERT()          SST26_CS_DEASSERT()
#define SPI_XFER8(a)               nora_spi_transfer8(&s_sst26_spi, (uint8_t)(a))
#define SPI_WAIT_DONE()            nora_spi_wait_done(&s_sst26_spi)


//===========================================================
// Function Prototype
//===========================================================
static uint8_t sst26_rdcr(void);
static uint8_t sst26_rdsr(void);
static void    sst26_wait_wip_clear(void);

static bool    local_sst26_pins_init(void);


//===========================================================
// Variables
//===========================================================

// SPI HAL handle bound to SST26's SPI instance (SPI4).
static nora_spi_handle_t s_sst26_spi;


//===========================================================
// Global Function
//===========================================================

/*
 * sst26_init
 * ----------
 * Purpose : Bring up the SST26 SPI bus.
 * Flow    : 1) Configure PPS routing and CS/WP/RST GPIO; pulse RST.
 *           2) Initialize the SPI HAL on the SST26 instance (SPI4),
 *              master, 8-bit, mode 0, target SCK.
 * Notes   : Replaces the former spi4_init(); the SPI HAL owns the SPI
 *           peripheral registers, while pin routing and CS/WP/RST stay here.
 */
void sst26_init(void)
{
    nora_spi_config_t cfg;

    // Board-component pin routing + control lines (PPS / GPIO), then reset the flash.
    if (!local_sst26_pins_init())
    {
        printf(" sst26_init: SST26 SPI4 pin setup failed\n");
        return;
    }

    // Hold CS de-asserted before enabling the bus.
    SST26_CS_DEASSERT();

    cfg.instance          = SST26_SPI_INSTANCE;
    cfg.peripheralClockHz = SST26_SPI_PBCLK_HZ;
    cfg.targetSckHz       = SST26_SPI_SCK_HZ;
    cfg.mode              = SST26_SPI_MODE;

    if (!nora_spi_init(&s_sst26_spi, &cfg))
    {
        printf(" sst26_init: nora_spi_init failed (instance=%d)\n",
               (int)cfg.instance);
    }
}

/*
 * sst26_read_status
 * -----------------
 * Purpose : Public accessor for the Status Register (0x05).
 * Notes   : Thin wrapper over the internal read; lets callers (e.g. a
 *           sound-effect flash bring-up) probe the device without owning
 *           the SPI command details.
 */
uint8_t sst26_read_status(void)
{
    return sst26_rdsr();
}

/*
 * sst26_read_config
 * -----------------
 * Purpose : Public accessor for the Configuration Register (0x35).
 */
uint8_t sst26_read_config(void)
{
    return sst26_rdcr();
}


/*
 * sst26_read_jedec_id
 * -------------------
 * Purpose : Read JEDEC ID (0x9F).
 * Return  : true if ID matches expected values.
 */
bool sst26_read_jedec_id(void)
{
    uint8_t mfr=0, dev_typ=0, dev_id=0;
    SPI_CS_ASSERT();
    SPI_XFER8(0x9F);
    mfr     = SPI_XFER8(0x00);
    dev_typ = SPI_XFER8(0x00);
    dev_id  = SPI_XFER8(0x00);
    SPI_CS_DEASSERT();

    printf(" sst26_read_jedec_id: MFR=0x%02X DEV_TYP=0x%02X DEV_ID=0x%02X ",
           mfr, dev_typ, dev_id);

    if(mfr==MANUFACTURER_ID && dev_typ==DEVICE_TYP_ID && dev_id==DEVICE_ID)
    {
        printf("(good)\n");
        return true;
    }
    else
    {
        printf("(NOT good!!)\n");
        return false;
    }
}

/*
 * sst26_read_fast
 * ---------------
 * Purpose : Fast Read command (0x0B).
 * Notes   : 24-bit addr + dummy byte.
 */
void sst26_read_fast(uint32_t addr, uint8_t *buf, size_t len)
{
    if (!len)
    {
        return;
    }

    SPI_CS_ASSERT();
    SPI_XFER8(0x0B);
    SPI_XFER8((addr >> 16) & 0xFF);
    SPI_XFER8((addr >>  8) & 0xFF);
    SPI_XFER8((addr >>  0) & 0xFF);
    SPI_XFER8(0x00);
    for (size_t i=0; i<len; i++)
    {
        buf[i] = SPI_XFER8(0x00);
    }
    SPI_CS_DEASSERT();
}

/*
 * sst26_write_enable
 * ------------------
 * Purpose : Send WREN (0x06).
 */
void sst26_write_enable(void)
{
    SPI_CS_ASSERT();
    SPI_XFER8(0x06);
    SPI_CS_DEASSERT();
}

/*
 * sst26_sector_erase_4k
 * ---------------------
 * Purpose : Erase one 4KB sector at aligned address.
 */
void sst26_sector_erase_4k(uint32_t addr)
{
    sst26_write_enable();
    SPI_CS_ASSERT();
    SPI_XFER8(0x20);
    SPI_XFER8((addr >> 16) & 0xFF);
    SPI_XFER8((addr >>  8) & 0xFF);
    SPI_XFER8((addr >>  0) & 0xFF);
    SPI_CS_DEASSERT();
    sst26_wait_wip_clear();
}

/*
 * sst26_chip_erase
 * ----------------
 * Purpose : Erase entire chip (takes time).
 */
void sst26_chip_erase(void)
{
    sst26_write_enable();
    SPI_CS_ASSERT();
    SPI_XFER8(0xC7); // or 0x60
    SPI_CS_DEASSERT();
    sst26_wait_wip_clear();
}

/*
 * sst26_verify
 * ------------
 * Purpose : Verify data by reading back and comparing.
 */
bool sst26_verify(uint32_t addr, const uint8_t *data, size_t len)
{
    uint8_t r;
    for (size_t i=0; i<len; i++)
    {
        SPI_CS_ASSERT();
        SPI_XFER8(0x0B);
        SPI_XFER8(((addr+i) >> 16) & 0xFF);
        SPI_XFER8(((addr+i) >>  8) & 0xFF);
        SPI_XFER8(((addr+i) >>  0) & 0xFF);
        SPI_XFER8(0x00);
        r = SPI_XFER8(0x00);
        SPI_CS_DEASSERT();
        if (r != data[i]) return false;
    }
    return true;
}

/*
 * sst26_page_program
 * ------------------
 * Purpose : Program up to 256 bytes (one page).
 * Notes   : Caller ensures not to cross page boundary.
 */
void sst26_page_program(uint32_t addr, const uint8_t *data, size_t nbytes)
{
    if (!nbytes)
    {
        return;    // nothing to do
    }
    if (nbytes>256)
    {
        printf(" sst26_page_program: nbytes=%d must be below 256!!.\n", nbytes);
        return;
    }

    sst26_write_enable();
    SPI_CS_ASSERT();
    SPI_XFER8(0x02);
    SPI_XFER8((addr >> 16) & 0xFF);
    SPI_XFER8((addr >>  8) & 0xFF);
    SPI_XFER8((addr >>  0) & 0xFF);
    for (size_t i=0; i<nbytes; i++)
    {
        SPI_XFER8(data[i]);
    }

//test
    SPI_WAIT_DONE();
//test

    SPI_CS_DEASSERT();
    sst26_wait_wip_clear();
}


uint32_t sst26_write_next(uint32_t addr, const uint8_t *data, uint32_t size)
{
    uint32_t cur  = addr;
    uint32_t done = 0;

    if ((data == NULL) || (size == 0u))
    {
        return addr;
    }

    printf(" sst26_write_next: addr=%ld size=%ld\n", addr, size);

    while (done < size)
    {
        uint32_t page_remain = SST26_PAGE_SIZE - (cur & (SST26_PAGE_SIZE - 1u));
        uint32_t remain      = size - done;
        uint32_t chunk       = (remain < page_remain) ? remain : page_remain;

        sst26_page_program(cur, &data[done], chunk);

        printf(" cur=0x%08lx(%8ld) chunk=%ld\n", cur, cur, chunk);

        cur  += chunk;
        done += chunk;
    }

    return cur;
}


/*
 * sst26_unprotect_all
 * -------------------
 * Purpose : Clear BP bits (BP1:BP0=00) via WRSR.
 */
void sst26_unprotect_all(void)
{
    printf(" sst26_unprotect_all\n");

    uint8_t cr = sst26_rdcr();
    sst26_write_enable();
    SPI_CS_ASSERT();
    SPI_XFER8(0x01);
    SPI_XFER8(0x00); // STATUS: BPL=0, BP=00
    SPI_XFER8(cr);   // CONFIG: unchanged
    SPI_CS_DEASSERT();
    sst26_wait_wip_clear();
}

/*
 * sst26_protect_all
 * -----------------
 * Purpose : Set BP bits (BP1:BP0=11) via WRSR to protect all blocks.
 * Notes   : Requires WP#=High and BPL=0. CONFIG is preserved.
 */
void sst26_protect_all(void)
{
    printf(" sst26_protect_all\n");

    uint8_t cr = sst26_rdcr();   // keep current CONFIG
    sst26_write_enable();        // WREN required before WRSR

    SPI_CS_ASSERT();
    SPI_XFER8(0x01);            // WRSR
    SPI_XFER8(0x0C);            // STATUS: BPL=0, BP1:BP0=11 (all protected)
    SPI_XFER8(cr);              // CONFIG: unchanged
    SPI_CS_DEASSERT();

    sst26_wait_wip_clear();      // wait for internal write complete
}







//===========================================================
// Local Function
//===========================================================

/*
 * sst26_rdcr
 * ----------
 * Purpose : Read Configuration Register (0x35).
 */
static uint8_t sst26_rdcr(void)
{
    uint8_t cr;
    SPI_CS_ASSERT();
    SPI_XFER8(0x35);
    cr = SPI_XFER8(0x00);
    SPI_CS_DEASSERT();
    return cr;
}

/*
 * sst26_rdsr
 * ----------
 * Purpose : Read Status Register (0x05).
 */
static uint8_t sst26_rdsr(void)
{
    uint8_t sr;
    SPI_CS_ASSERT();
    SPI_XFER8(0x05);
    sr = SPI_XFER8(0x00);
    SPI_CS_DEASSERT();
    return sr;
}

/*
 * sst26_wait_wip_clear
 * --------------------
 * Purpose : Wait until WIP=0 (not busy).
 */
static void sst26_wait_wip_clear(void)
{
    while (sst26_rdsr() & 0x01) { }
}


/*
 * local_sst26_pins_init
 * ---------------------
 * Purpose : Bring up the SST26 SPI4 pins and reset the flash.
 * Notes   : This board component owns the Curiosity SST26 wiring: SPI4 PPS,
 *           GPIO pre-config, and CS/WP/RST idle levels. RST is first asserted
 *           Low, then released after the device reset pulse delay.
 */
static bool local_sst26_pins_init(void)
{
    bool ok = true;

    /*
     * GPIO pre-config via the RP-first hal_gpio (analog off, no pull), with each
     * output pin's LAT initial value seeded before its driver is enabled:
     *   SDO/SCK : digital output, idle low
     *   SDI     : digital input
     *   CS / WP : active-low control, idle High (deasserted / WP released)
     *   RST     : active-low, asserted Low until the reset pulse is complete.
     */
    // SPI signal pins: digital config + PPS route folded into the pinmux helper.
    ok = nora_pinmux_route_output(NORA_PPS_OUTPUT_SDO4, SST26_RP_SDO, false) && ok;  // SDO4 idle low
    ok = nora_pinmux_route_output(NORA_PPS_OUTPUT_SCK4, SST26_RP_SCK, false) && ok;  // SCK4 idle low
    ok = nora_pinmux_route_input (NORA_PPS_INPUT_SDI4,  SST26_RP_SDI) && ok;         // SDI4
    // Control lines are plain GPIO (no PPS peripheral): keep direct digital-output config.
    ok = nora_gpio_rp_config_digital_output(SST26_RP_CS,  true)  && ok;   // CS idle high
    ok = nora_gpio_rp_config_digital_output(SST26_RP_WP,  true)  && ok;   // WP idle high
    ok = nora_gpio_rp_config_digital_output(SST26_RP_RST, false) && ok;  // RST asserted low
    if (!ok)
    {
        return false;
    }

    // Complete the reset pulse, then release RST (WP is already idle high).
    delay_ms(1);
    return nora_gpio_rp_set(SST26_RP_RST);   // RST high
}

#endif // RESOLVED_BOARD_USE_SST26
