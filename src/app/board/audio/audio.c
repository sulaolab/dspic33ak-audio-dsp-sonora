//===========================================================
// board/audio/audio.c
//
// This file is a board adapter example for the Sonora hardware.
// It is not part of the generic SPI/I2S/TDM HAL core.
//
// Phase D5a: board adapter physically split out of nora_spi_i2s_tdm_dspic33ak.c.
//
// PPS/GPIO pin maps and CLC pass-through for the current board(s), gated by
// resolved device and audio-input wiring facts. GPIO attributes use the RP-first GPIO
// HAL; PPS is routed alongside. The individual config_pins_* helpers are private
// (static) here; CLC pass-through is built by the CLC_PASSTHROUGH() macro. The
// driver calls audio_transport_board_config_pins()/audio_transport_board_clc_passthrough().
// NOT a public driver API.
//===========================================================

#include "resolved_board_config.h"
#include "resolved_transport_config.h"

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

#include "nora_gpio.h"              /* packed-pin core + RP-first GPIO API */
#include "nora_gpio_event.h"        /* CN event dispatcher + RP-first IRQ helpers */
#include "nora_pps.h"               /* PPS signal routing (route_output/input) */

#include "board/audio/audio.h"
#include "board/audio/tdm.h"   // TDM topology table (leg B clock role for pin direction)
#include "Driver_SAI_dsPIC33AK.h"        /* CMSIS-SAI wrapper integration hooks (overridden below) */
#include "board/clock/sonora_clock.h"


//--------------------------------------------
// PPS signal routing for these SPI/TDM pins is done through the PPS HAL
// (nora_pps_route_output / nora_pps_route_input). The former
// DEF_*_OUT_ID output function codes and DEF_*_IN_REG input-select aliases are
// gone: the HAL maps each peripheral signal to its _RPOUT_* code / RPINRx
// register, so the AK128/AK512 code differences live in the device header.
//--------------------------------------------


#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
//
// AK512
//
// TDM/CLC pins are configured through the RP-first GPIO HAL: PPS inputs use
// nora_gpio_rp_config_digital_input(rp); PPS-driven outputs use
// nora_gpio_rp_config_digital_output(rp, false) (seeded Low before the
// peripheral PPS takes over). No local default-config structs are needed.

// SPI34_TEST keeps the physical WM8904 wiring unchanged and switches only the PPS
// peripheral function routed to each MikroBUS pin. Codec A/B remain dense logical
// legs 0/1, while the HAL's physical spi3()/spi4() accessors expose their descriptor
// rows. Application code uses audio_transport_tdm_leg_a()/leg_b().
#if RESOLVED_TRANSPORT_BASE_ON_SPI34
#define TDM_A_PPS_OUT_SS   NORA_PPS_OUTPUT_SS3
#define TDM_A_PPS_OUT_SCK  NORA_PPS_OUTPUT_SCK3
#define TDM_A_PPS_OUT_SDO  NORA_PPS_OUTPUT_SDO3
#define TDM_A_PPS_IN_SS    NORA_PPS_INPUT_SS3
#define TDM_A_PPS_IN_SCK   NORA_PPS_INPUT_SCK3
#define TDM_A_PPS_IN_SDI   NORA_PPS_INPUT_SDI3
#define TDM_B_PPS_OUT_SS   NORA_PPS_OUTPUT_SS4
#define TDM_B_PPS_OUT_SCK  NORA_PPS_OUTPUT_SCK4
#define TDM_B_PPS_OUT_SDO  NORA_PPS_OUTPUT_SDO4
#define TDM_B_PPS_IN_SS    NORA_PPS_INPUT_SS4
#define TDM_B_PPS_IN_SCK   NORA_PPS_INPUT_SCK4
#define TDM_B_PPS_IN_SDI   NORA_PPS_INPUT_SDI4
#else
#define TDM_A_PPS_OUT_SS   NORA_PPS_OUTPUT_SS1
#define TDM_A_PPS_OUT_SCK  NORA_PPS_OUTPUT_SCK1
#define TDM_A_PPS_OUT_SDO  NORA_PPS_OUTPUT_SDO1
#define TDM_A_PPS_IN_SS    NORA_PPS_INPUT_SS1
#define TDM_A_PPS_IN_SCK   NORA_PPS_INPUT_SCK1
#define TDM_A_PPS_IN_SDI   NORA_PPS_INPUT_SDI1
#define TDM_B_PPS_OUT_SS   NORA_PPS_OUTPUT_SS2
#define TDM_B_PPS_OUT_SCK  NORA_PPS_OUTPUT_SCK2
#define TDM_B_PPS_OUT_SDO  NORA_PPS_OUTPUT_SDO2
#define TDM_B_PPS_IN_SS    NORA_PPS_INPUT_SS2
#define TDM_B_PPS_IN_SCK   NORA_PPS_INPUT_SCK2
#define TDM_B_PPS_IN_SDI   NORA_PPS_INPUT_SDI2
#endif

#if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
// MCLK    DIM-P11    RP26 / RB9
// BCLK    DIM-P15    RP99 / RG2
// FS      DIM-P13    RP25 / RB8
// SDOUT   DIM-P3     RP23 / RB6
static bool config_pins_SPI_1_MikroA_AK512( uint8_t tdm_master )
{
    if( tdm_master )
    {
    //
    // SPI is TDM master -- NOT implemented for the USB-audio board variant.
    // Return false so start() aborts cleanly (was an infinite while(1) trap).
    //
        return false;
    }
    //
    // SPI is TDM slave
    //
    // BCLK    DIM-P15    RP99 / RG2
    // FS      DIM-P13    RP25 / RB8
    // Per-pin: GPIO attribute first, then PPS route. Fail-fast.
    if( !nora_pinmux_route_input(TDM_A_PPS_IN_SS,  25u) ) return false;  // FS   RP25/RB8
    if( !nora_pinmux_route_input(TDM_A_PPS_IN_SCK, 99u) ) return false;  // BCLK RP99/RG2
    if( !nora_pinmux_route_input(TDM_A_PPS_IN_SDI, 23u) ) return false;  // SDOUT/DAT-in RP23/RB6
    return true;
}
#else
// MikroBUS A
// CS/P81     DIM-P81   RP70    E5   CVDTX24/RP70/RE5
// SCK/P83    DIM-P83   RP75    E10  RP75/RE10
// MISO/P85   DIM-P85   RP106   G9   RP106/RG9
// MOSI/P87   DIM-P87   RP101   G4   RP101/RG4
static bool config_pins_SPI_1_MikroA_AK512( uint8_t tdm_master )
{
    // Per-pin: set GPIO attribute first, then route the PPS signal.
    // GPIO config before PPS keeps the pin in a known electrical state before the
    // peripheral output starts driving it (glitch-aware). Fail-fast: if GPIO config
    // fails, skip the PPS route so no partial config is left on the pin.
    if( tdm_master )
    {
        // master: dsPIC drives FS(SS1)/BCLK(SCK1) as clock outputs.
        if( !nora_pinmux_route_output(TDM_A_PPS_OUT_SS,  70u, false) ) return false;  // FS   RP70/RE5
        if( !nora_pinmux_route_output(TDM_A_PPS_OUT_SCK, 75u, false) ) return false;  // BCLK RP75/RE10
    }
    else
    {
        // slave: FS(SS1)/BCLK(SCK1) are PPS inputs.
        if( !nora_pinmux_route_input(TDM_A_PPS_IN_SS,   70u) ) return false;  // FS   RP70/RE5
        if( !nora_pinmux_route_input(TDM_A_PPS_IN_SCK,  75u) ) return false;  // BCLK RP75/RE10
    }
    if( !nora_pinmux_route_output(TDM_A_PPS_OUT_SDO, 101u, false) ) return false;  // SDO MOSI/P87 RP101/RG4
    if( !nora_pinmux_route_input(TDM_A_PPS_IN_SDI, 106u) ) return false;  // DAT-IN MISO/P85 RP106/RG9

    return true;
}
#endif // RESOLVED_BOARD_AUDIO_INPUT_IS_USB


// MikroBUS B
// CS/P33     DIM-P33   RP29	B12  CVDAN28/CVDTX12/CMP3D/RP29/RB12
// SCK/P35    DIM-P35   RP90	F9   RP90/RF9
// MISO/P37   DIM-P37   RP89	F8   RP89/RF8
// MOSI/P39   DIM-P39   RP92	F11  RP92/RF11
static bool config_pins_SPI_2_MikroB_AK512( uint8_t tdm_master )
{
    if( tdm_master )
    {
        // Independent leg-B controller clock: dsPIC drives
        // FS(SS2)/BCLK(SCK2) as clock OUTPUTS (seed Low), so SPI2 no longer rides SPI1's
        // clocks. SDI2 stays an input (WM8904-B ADC data), SDO2 stays an output. The
        // caller must NOT also fan SPI1's FS/BCLK onto RP29/RP90 (see
        // board_passthrough_frame_clocks) -- that would collide with these outputs.
        if( !nora_pinmux_route_output(TDM_B_PPS_OUT_SS,  29u, false) ) return false;  // FS   RP29/RB12
        if( !nora_pinmux_route_output(TDM_B_PPS_OUT_SCK, 90u, false) ) return false;  // BCLK RP90/RF9
        if( !nora_pinmux_route_input(TDM_B_PPS_IN_SDI, 89u) ) return false;  // SDI  RP89/RF8
        if( !nora_pinmux_route_output(TDM_B_PPS_OUT_SDO, 92u, false) ) return false;  // SDO  RP92/RF11
    }
    else
    {
        // SPI2 follows SPI1 clocks: FS/BCLK are PPS inputs. Per-pin: GPIO first, then PPS.
        if( !nora_pinmux_route_input(TDM_B_PPS_IN_SS,  29u) ) return false;  // FS   RP29/RB12
        if( !nora_pinmux_route_input(TDM_B_PPS_IN_SCK, 90u) ) return false;  // BCLK RP90/RF9
        if( !nora_pinmux_route_input(TDM_B_PPS_IN_SDI, 89u) ) return false;  // SDI  RP89/RF8
        if( !nora_pinmux_route_output(TDM_B_PPS_OUT_SDO, 92u, false) ) return false;  // SDO  RP92/RF11
    }

    return true;
}




// MikroBUS A
// AN/P77       DIM-P77 |   RP1    A0   PGD2/AD3AN5/CVDAN10/CMP6A/RP1/SCL2/IOMAF2/RA0
//
// CS/P81       DIM-P81	|	RP70   E5   CVDTX24/RP70/RE5
// SCK/P83      DIM-P83	|	RP75   E10  RP75/RE10
// MISO/P85     DIM-P85	|	RP106  G9   RP106/RG9
// MOSI/P87     DIM-P87	|	RP101  G4   RP101/RG4

// MikroBUS B
// AN/P29       DIM-P29 |   RP4    A3   OA1IN-/AD1ANN2/AD1AN2/CVDAN3/CMPCN/CMP1C/RP4/RA3
//                      |
// CS           DIM-P33	|	RP29   B12  CVDAN28/CVDTX12/CMP3D/RP29/RB12
// SCK          DIM-P35	|	RP90   F9   RP90/RF9
// MISO	        DIM-P37	|	RP89   F8   RP89/RF8
// MOSI         DIM-P39	|	RP92   F11  RP92/RF11

// PWM          DIM-P27	|	RP98   G1   RP98/APWM4H/IOMBD11/RG1
// INT          DIM-P25 |   RP82   F1   CVDTX30/RP82/RF1

//===========================================================
// CLC signal pass-through builder (route any CLC input pin -> any CLCnOUT pin).
//
// CLC pass-through GPIO setup uses the RP-first GPIO HAL (nora_gpio.h).
// PPS routing uses nora_pps_route_input/output(), so the board code does
// not write RPINRx/RPORx aliases directly.
//===========================================================

// CLC1..3 are used by this board adapter. For each CLCn this gives the PPS input
// signal, PPS output signal, and the CLCxSEL DS slot/code that selects that input.
// DS slot/code values are from the device CLCxSEL table; they are not a formula.
#define CLC1_PPS_INPUT  NORA_PPS_INPUT_CLCINA
#define CLC1_PPS_OUTPUT NORA_PPS_OUTPUT_CLC1
#define CLC1_DSSLOT     1u
#define CLC1_DSCODE     0x4u

#define CLC2_PPS_INPUT  NORA_PPS_INPUT_CLCINB
#define CLC2_PPS_OUTPUT NORA_PPS_OUTPUT_CLC2
#define CLC2_DSSLOT     1u
#define CLC2_DSCODE     0x5u

#define CLC3_PPS_INPUT  NORA_PPS_INPUT_CLCINC
#define CLC3_PPS_OUTPUT NORA_PPS_OUTPUT_CLC3
#define CLC3_DSSLOT     2u
#define CLC3_DSCODE     0x3u

#define BOARD_CLC_CON_MODE_MASK  (0x00000007u)
#define BOARD_CLC_CON_MODE_OR    (0x00000001u)
#define BOARD_CLC_CON_LCPOL_MASK (0x00000020u)
#define BOARD_CLC_CON_LCOE_MASK  (0x00000080u)
#define BOARD_CLC_CON_ON_MASK    (0x00008000u)

static void board_clc_configure_passthrough(volatile uint32_t *con,
                                            volatile uint32_t *sel,
                                            volatile uint32_t *gls,
                                            uint8_t ds_slot,
                                            uint8_t ds_code)
{
    uint32_t ds_shift = ((uint32_t)ds_slot - 1u) * 4u;
    uint32_t g1d_true_shift = (((uint32_t)ds_slot - 1u) * 2u) + 1u;
    uint32_t con_value = *con;

    con_value &= ~(BOARD_CLC_CON_ON_MASK |
                   BOARD_CLC_CON_MODE_MASK |
                   BOARD_CLC_CON_LCPOL_MASK |
                   BOARD_CLC_CON_LCOE_MASK);
    *con = con_value;

    *sel = (*sel & ~(0x7u << ds_shift)) | (((uint32_t)ds_code & 0x7u) << ds_shift);
    *gls = (1u << g1d_true_shift);  // Gate1: pass selected D<slot> true only.

    *con = con_value | BOARD_CLC_CON_MODE_OR | BOARD_CLC_CON_LCOE_MASK | BOARD_CLC_CON_ON_MASK;
}

// CLC_PASSTHROUGH(n, in_rp, out_rp): wire CLCIN<n> (fed from RP in_rp)
// straight through to CLC<n>OUT (driven onto RP out_rp), Gate1 in OR mode.
// n must be a literal CLC instance number so the macro can bind that instance's
// local register set. Use only inside a function returning bool.
#define CLC_PASSTHROUGH(n, in_rp, out_rp) \
    do {                                                                                \
        /* input+output: digital config + PPS route folded via the pinmux helper */     \
        if( !nora_pinmux_route_input(CLC##n##_PPS_INPUT, in_rp) ) { return false; } \
        if( !nora_pinmux_route_output(CLC##n##_PPS_OUTPUT, out_rp, false) ){ return false; } \
        _CLC##n##MD                  = 0;                                               \
        board_clc_configure_passthrough(&CLC##n##CON, &CLC##n##SEL, &CLC##n##GLS,       \
                                        CLC##n##_DSSLOT, CLC##n##_DSCODE);              \
    } while( 0 )


#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE




#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE
//
// AK128
//
// CS/P81       DIM-P81  RP24   B7   AD2ANN2/AD2AN8/RP24/IOMF0/RB7
// SCK/P83      DIM-P83  RP33   C0   OSCO/CLKO/RP33/IOMF5/RC0
// MISO/P85     DIM-P85  RP60   D11  RP60/RD11
// MOSI/P87     DIM-P87  RP44   C11  RP44/IOMD8/IOMF8/RC11
static bool config_pins_SPI_1_AK128( uint8_t tdm_master )
{
    // Per-pin: GPIO attribute first, then PPS route. Fail-fast.
    if( tdm_master )
    {
        // master: dsPIC drives FS(SS1)/BCLK(SCK1) as clock outputs; seed Low.
        if( !nora_pinmux_route_output(NORA_PPS_OUTPUT_SS1,  24u, false) ) return false;  // FS   RP24/RB7
        if( !nora_pinmux_route_output(NORA_PPS_OUTPUT_SCK1, 33u, false) ) return false;  // BCLK RP33/RC0
    }
    else
    {
        // slave: FS(SS1)/BCLK(SCK1) are PPS inputs (digital, no pull).
        if( !nora_pinmux_route_input(NORA_PPS_INPUT_SS1,  24u) ) return false;  // FS   RP24/RB7
        if( !nora_pinmux_route_input(NORA_PPS_INPUT_SCK1, 33u) ) return false;  // BCLK RP33/RC0
    }
    if( !nora_pinmux_route_input(NORA_PPS_INPUT_SDI1,  60u) ) return false;  // MISO/P85 RP60/RD11
    if( !nora_pinmux_route_output(NORA_PPS_OUTPUT_SDO1, 44u, false) ) return false;  // MOSI/P87 RP44/RC11

    return true;
}

#if RESOLVED_BOARD_AK128_J3_TDM_B
/*
 * Curiosity J3 jumper route for the otherwise NC MikroBUS-B TDM pins.
 *
 * The AK128 DIM leaves all four MikroBUS-B TDM nets DIM-NC, so codec B's frame
 * clock and data have no MCU pin of their own.  Both ends of the jumper are
 * brought out on the SAME J3 breakout, so the route is four wires within one
 * connector -- no XPRO/J17 header and no J10 involved:
 *
 *   signal    source (mikroBUS-B)      destination (mikroBUS-A, unused by
 *                                      the WM8904-A board)
 *   FS/LRCLK  DIM-P33  mkB_B_CS    ->  DIM-P77  mkB_A_AN    RP12 / SS2  in
 *   BCLK      DIM-P35  mkB_B_SCK   ->  DIM-P73  mkB_A_INT   RP6  / SCK2 in
 *   ADCDAT    DIM-P37  mkB_B_MISO  ->  DIM-P69  mkB_A_TX    RP27 / SDI2 in
 *   DACDAT    DIM-P39  mkB_B_MOSI  <-  DIM-P71  mkB_A_RX    RP28 / SDO2 out
 *
 * The destinations are mikroBUS-A socket pins (AN/RST/TX/INT) that a WM8904
 * mikroBUS board does not use -- it needs only I2C control plus the I2S/TDM
 * group.  THAT IS A HARDWARE PRECONDITION, not something firmware can check:
 * a different mikroBUS board on mikroBUS-A would contend with these four
 * wires.  Confirmed for this bench on 2026-08-17: the WM8904 boards in use are
 * an in-house design (sulaolab/EasyEDA-WM8904-mikroBUS) whose author states
 * AN/RST/TX/INT are not connected.
 *
 * Per the DIM Information Sheet (DS70005556B, Table 1) none of the four
 * destinations carries any other Curiosity circuit -- no LED, touch pad,
 * button or pull-up.  DIM-P75 (mkB_A_PWM) is deliberately NOT used even
 * though it is equally free on MikroBUS-A: it shares device pin 41 with
 * DIM-P68 = LED_R, so the RGB LED's red channel would load a TDM data line.
 * DIM-P71 (mkB_A_RX) is likewise free and unused here.
 *
 * All four are PPS-routed, so the silicon's own dedicated names on these pads
 * (RP27's is SCK2, and DIM-P71's RP28 is SDI2) do not have to line up with the
 * function assigned here.  Three of the four are analogue-capable
 * (RP12/RA11 = AD1AN10, RP23/RB6 = AD1AN8, RP6/RA5 = AD1AN3); the
 * nora_pinmux_route_* helpers configure the pad digital before routing, the
 * same way SPI1's SS1 already runs on the analogue-capable RP24/RB7 above.
 *
 * Codec B owns its BCLK/FS from B-XTAL -> B-MCLK, hence SPI2 is deliberately
 * a slave.  Refuse a master request rather than accidentally driving the J3
 * wires or reviving the A-to-B CLC clock-copy topology.
 */
static bool config_pins_SPI_2_AK128( uint8_t tdm_master )
{
    if( tdm_master )
    {
        return false;
    }

    // FS     DIM-P77  RP12  A11  AD1AN10/RP12/RA11
    if( !nora_pinmux_route_input(NORA_PPS_INPUT_SS2,  12u) ) return false;
    // BCLK   DIM-P73  RP6   A5   OA3OUT/AD1AN3/CMP3A/RP6/RA5
    if( !nora_pinmux_route_input(NORA_PPS_INPUT_SCK2,  6u) ) return false;
    // ADCDAT DIM-P69  RP27  B10  RP27/SCK2/RB10
    if( !nora_pinmux_route_input(NORA_PPS_INPUT_SDI2, 27u) ) return false;
    /* DACDAT DIM-P71  RP28  B11  RP28/SDI2/RB11
     *
     * NOT DIM-P79 (RP23/RB6, mkB_A_RST) -- that was the first choice and it
     * silently killed leg A.  Bisected on hardware 2026-08-17:
     *
     *   all four pins routed, DACDAT on RP23  -> leg A blk=0, ds=0x00000000
     *                                            (SPI1 sees no clock at all,
     *                                             no framing/overrun errors),
     *                                            leg B fine
     *   three inputs only, DACDAT not routed  -> leg A clocks, leg B dead
     *   all four routed, DACDAT on RP28       -> BOTH legs clock, miss=0
     *
     * So it is not "enabling SDO2", it is specifically DRIVING DIM-P79.  That
     * net is mikroBUS-A's RST, and codec A stops producing BCLK/FS while audio
     * data toggles on it -- while still answering I2C and completing its
     * write-sequencer and DC-servo startup, which is what made this look like a
     * firmware fault for so long.  Do not move DACDAT back to DIM-P79 on a
     * board whose mikroBUS-A socket is populated. */
    if( !nora_pinmux_route_output(NORA_PPS_OUTPUT_SDO2, 28u, false) ) return false;

    return true;
}
#endif // RESOLVED_BOARD_AK128_J3_TDM_B
#endif // RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE



//===========================================================
// Board entry points (Phase D5a)
//
// These mirror exactly what nora_spi_i2s_tdm_start() used to call inline,
// preserving the per-device selection and call order (pins, then CLC pass-through).
//===========================================================

bool audio_transport_board_config_pins( nora_spi_i2s_tdm_clock_role_t role )
{
    // `role` is the COMMITTED primary leg's clock role -- open() derives it from the HAL's
    // committed config and passes it here (a caller can no longer contradict the config).
    const uint8_t tdm_master = ( role == NORA_SPI_I2S_TDM_CLOCK_MASTER ) ? 1u : 0u;
    bool ok = true;

#if RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK512_VALUE
    // SPI1 (primary): USB/non-USB difference is handled inside config_pins_SPI_1_MikroA_AK512()
    // (two static definitions selected by the resolved board input fact).
    ok = config_pins_SPI_1_MikroA_AK512( tdm_master ) && ok;
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    // SPI2 pin direction from the HAL's COMMITTED state (single source), NOT a board table.
    // inst_get_setup() is a pure query (no last-error clobber).
    {
        nora_spi_i2s_tdm_leg_setup_t spi2;
        uint8_t spi2_tdm_master;
        if( nora_spi_i2s_tdm_inst_get_setup( audio_transport_tdm_leg_b(), &spi2 ) )
        {
            // Configured leg -> route pins for its COMMITTED clock role.
            spi2_tdm_master = ( spi2.stream.clock_role == NORA_SPI_I2S_TDM_CLOCK_MASTER ) ? 1u : 0u;
        }
        else
        {
            // SPI2 is NOT a configured leg for this run (e.g. a single-instance CMSIS run that
            // configured only the primary). BOARD-SPECIFIC: this board physically co-clocks A+B
            // on a shared FS/BCLK bus and still inits + unmutes codec B, so B's SPI2 pins must be
            // PARKED as slave INPUTS (BCLK/FS/SDI in, SDO idle) -- leaving them unconfigured lets
            // codec B float and play garbage. This is pin-parking on the shared bus, NOT assuming
            // B is an active slave leg. (A truly independent secondary on a generic board could be
            // skipped instead; that belongs in the standalone board example, not this co-clock one.)
            spi2_tdm_master = 0u;   // slave: inputs
        }
        ok = config_pins_SPI_2_MikroB_AK512( spi2_tdm_master ) && ok;
    }
#endif // RESOLVED_TRANSPORT_LEG_B_PRESENT
#elif RESOLVED_BOARD_TARGET == RESOLVED_BOARD_TARGET_AK128_VALUE
    ok = config_pins_SPI_1_AK128( tdm_master ) && ok;
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
    {
        nora_spi_i2s_tdm_leg_setup_t spi2;
        uint8_t spi2_tdm_master;

        if( !nora_spi_i2s_tdm_inst_get_setup( audio_transport_tdm_leg_b(), &spi2 ) )
        {
            return false;
        }
        spi2_tdm_master =
            ( spi2.stream.clock_role == NORA_SPI_I2S_TDM_CLOCK_MASTER ) ? 1u : 0u;
        ok = config_pins_SPI_2_AK128( spi2_tdm_master ) && ok;
    }
#endif // RESOLVED_TRANSPORT_LEG_B_PRESENT
#else
    #error "Unhandled resolved board target in audio pin map."
#endif // RESOLVED_BOARD_TARGET
    return ok;   // false => an unsupported role (e.g. USB-audio MASTER) or a pin-config failure
}


// FS + BCLK are leg-0's FRAME clocks, fanned out UNCONDITIONALLY to MikroBUS-B. The
// follower codec always rides leg-0's frame timing, so there is no per-leg / per-mode
// decision here -- it is pure passthrough (the "follow leg-0" wiring, axis A on the
// follower side). Each path is one line: CLC_PASSTHROUGH(CLC<n>, in RP, out RP) routes
// the pins (GPIO via the RP-first HAL + PPS) and the CLC logic. Add a path = add a line.
static bool board_passthrough_frame_clocks( void )
{
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
    // Independent-domain: SPI2 GENERATES its own FS/BCLK on RP29/RP90 (see
    // config_pins_SPI_2_MikroB_AK512 master branch). Do NOT fan SPI1's FS/BCLK onto
    // those pins -- that would collide with SPI2's own clock outputs. No frame-clock
    // passthrough here; only MCLK is routed (board_route_mclk, from SPI2's own BCLK).
#elif RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    // B-codec-master: WM8904-B DRIVES FS/BCLK onto RP29/RP90 (SPI2 is a SLAVE input, its own
    // independent domain clocked by B's XTAL). Do NOT fan SPI1's (leg A's) FS/BCLK onto those
    // pins -- that would collide with the codec-B-driven clocks. No frame-clock passthrough here.
#elif RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    // USB-audio bridge -> MikroBUS-B  (CLCn input is CLCIN<n>: 1=A, 2=B, 3=C)
    CLC_PASSTHROUGH( 1, 25, 29 );   // FS  : CLCINA RP25/RB8  -> CLC1OUT RP29/RB12
    CLC_PASSTHROUGH( 2, 99, 90 );   // BCLK: CLCINB RP99/RG2  -> CLC2OUT RP90/RF9
#else
    // MikroBUS-A -> MikroBUS-B  (CLCn input is CLCIN<n>: 1=A, 2=B, 3=C)
    CLC_PASSTHROUGH( 1, 70, 29 );   // FS  : CLCINA RP70/RE5  -> CLC1OUT RP29/RB12
    CLC_PASSTHROUGH( 2, 75, 90 );   // BCLK: CLCINB RP75/RE10 -> CLC2OUT RP90/RF9
#endif // resolved leg-B clock source / board input
#endif // RESOLVED_TRANSPORT_LEG_B_PRESENT
    return true;
}

// MCLK is the ONE clock whose SOURCE varies (axis B), and it varies by a BOARD/compile
// fact, NOT by any leg's master/slave role (axis A):
//   - USB-audio bridge      : dedicated MCLK net (CLCINC RP26).
//   - controller-clocked    : WM8904-B reuses the owning leg's BCLK as its MCLK.
//   - codec-master (default): the board's MCLK net (CLCINC RP16).
// Kept separate from the frame-clock passthrough so the one path that needs a decision
// does not entangle the two that don't (was the old role-conditional mess).
static bool board_route_mclk( void )
{
#if RESOLVED_TRANSPORT_LEG_B_PRESENT
#if RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_ENDPOINT_VALUE
    // B-codec-master: WM8904-B self-clocks its MCLK from its OWN 12.288 MHz XTAL (board jumper
    // B-XTAL -> B-MCLK). The dsPIC must NOT drive B's MCLK net -- route NOTHING to CLC3OUT/RP97,
    // leaving RP97/RG0 a non-driving input (reset default, no CLC3 output assigned) so it cannot
    // contend with the jumper-supplied XTAL on B's MCLK. (This is the "stop the CLC MCLK" step;
    // the jumper is the physical half. Verify no stale CLC3OUT holds RP97 before first run.)
#elif RESOLVED_TRANSPORT_LEG_B_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
    // Independent-domain: WM8904-B reuses SPI2's OWN BCLK as its MCLK. SPI2 BCLK (SCK2)
    // is on RP90; read it back through CLCINx -> CLC3OUT RP97 -> WM8904-B MCLK.
    // (Same "MCLK = BCLK" trick as the self-clocked A-master path, but sourced from SPI2.)
    CLC_PASSTHROUGH( 3, 90, 97 );   // MCLK = SPI2 BCLK RP90/RF9 -> CLC3OUT RP97/RG0
#elif RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    CLC_PASSTHROUGH( 3, 26, 97 );   // MCLK: CLCINC RP26/RB9  -> CLC3OUT RP97/RG0 (MikroBUS pin3 -> WM8904-B MCLK; modified board)
#elif RESOLVED_TRANSPORT_LEG_A_CLOCK_SOURCE == TRANSPORT_CLOCK_SOURCE_CONTROLLER_VALUE
    CLC_PASSTHROUGH( 3, 75, 97 );   // MCLK = BCLK   RP75/RE10 -> CLC3OUT RP97/RG0
#else
    CLC_PASSTHROUGH( 3, 16, 97 );   // MCLK: CLCINC RP16/RA15 -> CLC3OUT RP97/RG0
#endif
#endif // RESOLVED_TRANSPORT_LEG_B_PRESENT
    return true;
}

bool audio_transport_board_clc_passthrough( nora_spi_i2s_tdm_clock_role_t role )
{
    // role selects axis A (BCLK/FS master vs slave) and is consumed by config_pins();
    // the CLC clock fan-out to the follower codec does NOT depend on it -- FS/BCLK always
    // ride leg-0, and MCLK's source is a board/compile fact (board_route_mclk). So role
    // is intentionally unused here. (When the port goes per-leg, this hook takes the leg
    // and these two helpers become that leg's frame + MCLK routing.)
    (void)role;
    return board_passthrough_frame_clocks() && board_route_mclk();
}



//===========================================================
// USB audio clock detection (Phase D5b: moved verbatim from nora_spi_i2s_tdm_dspic33ak.c)
//
// Board/input-specific: detects the USB audio clock presence on RB15 via the
// PORTB change-notification (CN) interrupt. Gated by the resolved USB input fact as
// before. The driver queries readiness through audio_transport_board_usb_clock_ready().
//===========================================================

#if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
#define USB_AUDIO_CLOCK_RP          ((nora_gpio_rp_t)32u)  /* RP32 / RB15 */
#define USB_AUDIO_CLOCK_CN_PRIORITY (4u)

static volatile uint8_t g_rb15_level = 0;
static volatile uint8_t g_rb15_rise_event = 0;
static volatile uint8_t g_rb15_fall_event = 0;

static void audio_transport_board_usb_clock_event_callback(nora_gpio_pin_t pin,
                                                     nora_gpio_event_edge_t edge,
                                                     void *user_data)
{
    (void)pin;
    (void)user_data;

    if( edge == NORA_GPIO_EVENT_EDGE_RISING )
    {
        g_rb15_rise_event = 1u;
        g_rb15_level = 1u;
    }
    else if( edge == NORA_GPIO_EVENT_EDGE_FALLING )
    {
        g_rb15_fall_event = 1u;
        g_rb15_level = 0u;
    }
}

/*
 * `context` not `no_auto_psv`: the alternate W0-W7 array is inherently tied to the IPL
 * on dsPIC33A, so each nesting level gets its own bank and this thunk needs no prologue
 * push -- and a prologue push at an ISR's first instruction is the documented trigger of
 * the A1 silicon STACK ERROR. Rationale in full above the vectors in
 * src/app/hal_i2c/nora_i2c_dspic33ak_device.c; the DO-NOT-REVERT case is above
 * _CCP1Interrupt in src/app/apps/asrc/asrc_clock_control.c.
 */
void __attribute__((interrupt, context)) _CNBInterrupt(void)
{
    nora_gpio_event_process_isr();
}
#endif // RESOLVED_BOARD_AUDIO_INPUT_IS_USB

// USB audio clock init entry: arms RB15 CN detection (no-op when disabled).
// Returns false if the GPIO/CN HAL cannot arm the detect path. role is unused
// (the external-clock detect is a slave-side concern, inert when
// the resolved USB input fact is false).
bool audio_transport_board_usb_clock_init( nora_spi_i2s_tdm_clock_role_t role )
{
    (void)role;
#if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    nora_gpio_level_t level;

    // Arm RP32/RB15 change-notification edge detection for the external
    // USB-audio clock presence. GPIO electrical setup and CN/IRQ arming are
    // routed through the RP-first GPIO HAL.
    if( !nora_gpio_rp_config_digital_input(USB_AUDIO_CLOCK_RP) )
    {
        return false;
    }

    level = nora_gpio_rp_read(USB_AUDIO_CLOCK_RP);
    if( level == NORA_GPIO_LEVEL_ERROR )
    {
        return false;
    }

    g_rb15_level = (level == NORA_GPIO_LEVEL_HIGH) ? 1u : 0u;
    g_rb15_rise_event = 0u;
    g_rb15_fall_event = 0u;

    if( !nora_gpio_event_rp_attach(USB_AUDIO_CLOCK_RP,
                                        NORA_GPIO_EVENT_EDGE_EITHER,
                                        audio_transport_board_usb_clock_event_callback,
                                        0) )
    {
        return false;
    }
    if( !nora_gpio_event_rp_irq_enable(USB_AUDIO_CLOCK_RP, USB_AUDIO_CLOCK_CN_PRIORITY) )
    {
        return false;
    }
#endif // RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    return true;
}


// USB audio clock readiness query. Implements the is_active() policy:
// when USB audio input is enabled, ready == (RB15 level high); otherwise the
// stream is always considered active.
bool audio_transport_board_usb_clock_ready( nora_spi_i2s_tdm_clock_role_t role )
{
    (void)role;
#if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    return (g_rb15_level == 1);
#else
    return true;
#endif // RESOLVED_BOARD_AUDIO_INPUT_IS_USB
}


// Phase H8a: read-and-clear the next external-clock stop/resume edge.
// STOPPED (RB15 fall) takes priority over RESUMED (rise) so a fall+rise that both
// arrived since the last call are reported STOPPED first (the rise stays pending
// for the next call) -- the app always mutes+stops before restarting. The flag
// read/clear is briefly masked against _CNBInterrupt through the GPIO event HAL.
// When the resolved USB input fact is false there is no detect, so this is always NONE.
nora_spi_i2s_tdm_clock_event_t audio_transport_board_consume_clock_event( void )
{
#if RESOLVED_BOARD_AUDIO_INPUT_IS_USB
    nora_spi_i2s_tdm_clock_event_t ev = NORA_SPI_I2S_TDM_CLOCK_EVENT_NONE;
    bool irq_was_enabled = false;
    bool have_irq_state;

    have_irq_state = nora_gpio_event_rp_irq_is_enabled(USB_AUDIO_CLOCK_RP, &irq_was_enabled);
    if( have_irq_state )
    {
        (void)nora_gpio_event_rp_irq_set_enabled(USB_AUDIO_CLOCK_RP, false);
    }

    if( g_rb15_fall_event )
    {
        g_rb15_fall_event = 0u;
        ev = NORA_SPI_I2S_TDM_CLOCK_EVENT_STOPPED;
    }
    else if( g_rb15_rise_event )
    {
        g_rb15_rise_event = 0u;
        ev = NORA_SPI_I2S_TDM_CLOCK_EVENT_RESUMED;
    }

    if( have_irq_state )
    {
        (void)nora_gpio_event_rp_irq_set_enabled(USB_AUDIO_CLOCK_RP, irq_was_enabled);
    }
    return ev;
#else
    return NORA_SPI_I2S_TDM_CLOCK_EVENT_NONE;
#endif // RESOLVED_BOARD_AUDIO_INPUT_IS_USB
}


//===========================================================
// CMSIS-SAI wrapper integration hooks (Sonora strong overrides).
//
// The standalone Driver_SAI_dsPIC33AK wrapper ships weak defaults for these; Sonora
// binds them to its board adapter + app rate policy here (same pattern as the I2C
// wrapper's Driver_I2C_dsPIC33AK_GetMs() override in board/devices/app_i2c.c).
//
// NOTE: there is no board-side "default config builder" anymore -- the complete TDM
// configuration is the system table in board/audio/tdm.c. The former
// audio_transport_board_get_default_config() (and its SPI2-master variant) were removed; this
// file no longer derives any TDM stream config.
//===========================================================

// GetDefaultConfig is a CMSIS wrapper API NAME only -- it does NOT generate a new default.
// It is a thin compatibility shim: hand back leg A (AUDIO_TDM_LEG_CODEC_A) of the same
// complete system table the native path uses, so the wrapper and the native transport read
// ONE source. There is no separate CMSIS default table / builder. Returns false (fail
// closed) if cfg is NULL or the system table is unavailable.
bool Driver_SAI_dsPIC33AK_GetDefaultConfig( nora_spi_i2s_tdm_config_t* cfg )
{
    const nora_spi_i2s_tdm_leg_setup_t* system = audio_transport_board_tdm_system();
    if( ( cfg == NULL ) || ( system == NULL ) || ( audio_transport_board_tdm_leg_count() == 0u ) )
    {
        return false;
    }
    *cfg = system[AUDIO_TDM_LEG_CODEC_A].stream;
    return true;
}

// Wrapper AUDIO_FREQ allow-list = this product's supported-rate set (48 k / 96 k).
bool Driver_SAI_dsPIC33AK_IsSampleRateSupported( uint32_t hz )
{
    return RESOLVED_BOARD_SAI_RATE_IS_SUPPORTED( hz );
}

// Board/clock PORT for the HAL core (nora_spi_i2s_tdm_set_port). Each hook points
// at a board function above; those self-gate on the resolved USB input fact, so wiring
// them unconditionally is behaviour-identical. Exposed as const data (not behind a
// register-wrapper): the caller binds it directly with set_port(&audio_transport_board_port).
const nora_spi_i2s_tdm_port_t audio_transport_board_port =
{
    .configure_pins      = audio_transport_board_config_pins,
    .clc_passthrough     = audio_transport_board_clc_passthrough,
    .clock_source_init   = audio_transport_board_usb_clock_init,
    .clock_source_ready  = audio_transport_board_usb_clock_ready,
    .consume_clock_event = audio_transport_board_consume_clock_event,
};
