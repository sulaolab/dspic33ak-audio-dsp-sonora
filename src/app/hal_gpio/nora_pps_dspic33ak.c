/*
 * nora_pps_dspic33ak.c
 * ---------------
 * NORA PPS routing backend. See nora_pps.h for the layering and contract.
 *
 * Device adaptation is by #ifdef on the XC macros themselves:
 *   - output function code : _RPOUT_<sig>   (e.g. _RPOUT_SCK2)
 *   - output pin register  : _RP<nn>R       (RPORx bit-field alias)
 *   - input-select register: _<sig>R        (RPINRx bit-field alias)
 * A peripheral / RP the selected device header does not define is left out of the
 * relevant switch and the call returns false -- no per-device part-number conditionals.
 */

#include "nora_pps.h"
#include "nora_gpio.h"   /* nora_gpio_rp_config_digital_input/output (pinmux helpers) */

#include <xc.h>

/* Virtual RP range (RPV0..RPV15). Padless PPS endpoints; see the header. The bounds
 * are only a fast reject -- which of them the selected device actually has is decided
 * per register by nora_pps_rpv_is_defined(), because dsPIC33AK128MC106 has none of
 * them (its output registers stop at _RP80R). */
#define NORA_PPS_RPV_MIN 129u
#define NORA_PPS_RPV_MAX 144u


//===========================================================
// PPS lock gate (RPCON.IOLOCK)
//===========================================================
// dsPIC33A guards RPCON via the PAC, which leaves RPCON writable out of reset
// (RPCONLK=0, RPCONWR=1), so IOLOCK is set/cleared by a direct write
// (__builtin_write_RPCON() is unsupported on this XC-DSC target).
void nora_pps_unlock(void)
{
    RPCONbits.IOLOCK = 0u;   /* PPS writable  */
}

void nora_pps_lock(void)
{
    RPCONbits.IOLOCK = 1u;   /* PPS protected */
}


//===========================================================
// Local: peripheral output -> RPORx function code (_RPOUT_*)
//===========================================================
static bool nora_pps_get_output_code(nora_pps_output_t output, uint8_t *code)
{
    if (code == NULL)
    {
        return false;
    }

    switch (output)
    {
#ifdef _RPOUT_U1TX
    case NORA_PPS_OUTPUT_U1TX: *code = (uint8_t)_RPOUT_U1TX; return true;
#endif
#ifdef _RPOUT_U2TX
    case NORA_PPS_OUTPUT_U2TX: *code = (uint8_t)_RPOUT_U2TX; return true;
#endif
#ifdef _RPOUT_SS1
    case NORA_PPS_OUTPUT_SS1:  *code = (uint8_t)_RPOUT_SS1;  return true;
#endif
#ifdef _RPOUT_SCK1
    case NORA_PPS_OUTPUT_SCK1: *code = (uint8_t)_RPOUT_SCK1; return true;
#endif
#ifdef _RPOUT_SDO1
    case NORA_PPS_OUTPUT_SDO1: *code = (uint8_t)_RPOUT_SDO1; return true;
#endif
#ifdef _RPOUT_SS2
    case NORA_PPS_OUTPUT_SS2:  *code = (uint8_t)_RPOUT_SS2;  return true;
#endif
#ifdef _RPOUT_SCK2
    case NORA_PPS_OUTPUT_SCK2: *code = (uint8_t)_RPOUT_SCK2; return true;
#endif
#ifdef _RPOUT_SDO2
    case NORA_PPS_OUTPUT_SDO2: *code = (uint8_t)_RPOUT_SDO2; return true;
#endif
#ifdef _RPOUT_SS3
    case NORA_PPS_OUTPUT_SS3:  *code = (uint8_t)_RPOUT_SS3;  return true;
#endif
#ifdef _RPOUT_SCK3
    case NORA_PPS_OUTPUT_SCK3: *code = (uint8_t)_RPOUT_SCK3; return true;
#endif
#ifdef _RPOUT_SDO3
    case NORA_PPS_OUTPUT_SDO3: *code = (uint8_t)_RPOUT_SDO3; return true;
#endif
#ifdef _RPOUT_SS4
    case NORA_PPS_OUTPUT_SS4:  *code = (uint8_t)_RPOUT_SS4;  return true;
#endif
#ifdef _RPOUT_SCK4
    case NORA_PPS_OUTPUT_SCK4: *code = (uint8_t)_RPOUT_SCK4; return true;
#endif
#ifdef _RPOUT_SDO4
    case NORA_PPS_OUTPUT_SDO4: *code = (uint8_t)_RPOUT_SDO4; return true;
#endif
#ifdef _RPOUT_CLC1OUT
    case NORA_PPS_OUTPUT_CLC1: *code = (uint8_t)_RPOUT_CLC1OUT; return true;
#endif
#ifdef _RPOUT_CLC2OUT
    case NORA_PPS_OUTPUT_CLC2: *code = (uint8_t)_RPOUT_CLC2OUT; return true;
#endif
#ifdef _RPOUT_CLC3OUT
    case NORA_PPS_OUTPUT_CLC3: *code = (uint8_t)_RPOUT_CLC3OUT; return true;
#endif
#ifdef _RPOUT_PWM1H
    case NORA_PPS_OUTPUT_PWM1H: *code = (uint8_t)_RPOUT_PWM1H; return true;
#endif
#ifdef _RPOUT_PWM2H
    case NORA_PPS_OUTPUT_PWM2H: *code = (uint8_t)_RPOUT_PWM2H; return true;
#endif
#ifdef _RPOUT_PWM3H
    case NORA_PPS_OUTPUT_PWM3H: *code = (uint8_t)_RPOUT_PWM3H; return true;
#endif
#ifdef _RPOUT_PWM5H
    case NORA_PPS_OUTPUT_PWM5H: *code = (uint8_t)_RPOUT_PWM5H; return true;
#endif
#ifdef _RPOUT_PWM5L
    case NORA_PPS_OUTPUT_PWM5L: *code = (uint8_t)_RPOUT_PWM5L; return true;
#endif
#ifdef _RPOUT_PWM6H
    case NORA_PPS_OUTPUT_PWM6H: *code = (uint8_t)_RPOUT_PWM6H; return true;
#endif
#ifdef _RPOUT_PWM6L
    case NORA_PPS_OUTPUT_PWM6L: *code = (uint8_t)_RPOUT_PWM6L; return true;
#endif
#ifdef _RPOUT_PWM7H
    case NORA_PPS_OUTPUT_PWM7H: *code = (uint8_t)_RPOUT_PWM7H; return true;
#endif
#ifdef _RPOUT_PWM7L
    case NORA_PPS_OUTPUT_PWM7L: *code = (uint8_t)_RPOUT_PWM7L; return true;
#endif
#ifdef _RPOUT_PWM8H
    case NORA_PPS_OUTPUT_PWM8H: *code = (uint8_t)_RPOUT_PWM8H; return true;
#endif
#ifdef _RPOUT_PWM8L
    case NORA_PPS_OUTPUT_PWM8L: *code = (uint8_t)_RPOUT_PWM8L; return true;
#endif
#ifdef _RPOUT_CAN1TX
    case NORA_PPS_OUTPUT_CAN1TX: *code = (uint8_t)_RPOUT_CAN1TX; return true;
#endif
#ifdef _RPOUT_CAN2TX
    case NORA_PPS_OUTPUT_CAN2TX: *code = (uint8_t)_RPOUT_CAN2TX; return true;
#endif
#ifdef _RPOUT_U3TX
    case NORA_PPS_OUTPUT_U3TX: *code = (uint8_t)_RPOUT_U3TX; return true;
#endif
#ifdef _RPOUT_CLC4OUT
    case NORA_PPS_OUTPUT_CLC4:  *code = (uint8_t)_RPOUT_CLC4OUT;  return true;
#endif
#ifdef _RPOUT_CLC5OUT
    case NORA_PPS_OUTPUT_CLC5:  *code = (uint8_t)_RPOUT_CLC5OUT;  return true;
#endif
#ifdef _RPOUT_CLC6OUT
    case NORA_PPS_OUTPUT_CLC6:  *code = (uint8_t)_RPOUT_CLC6OUT;  return true;
#endif
#ifdef _RPOUT_CLC7OUT
    case NORA_PPS_OUTPUT_CLC7:  *code = (uint8_t)_RPOUT_CLC7OUT;  return true;
#endif
#ifdef _RPOUT_CLC8OUT
    case NORA_PPS_OUTPUT_CLC8:  *code = (uint8_t)_RPOUT_CLC8OUT;  return true;
#endif
#ifdef _RPOUT_CLC9OUT
    case NORA_PPS_OUTPUT_CLC9:  *code = (uint8_t)_RPOUT_CLC9OUT;  return true;
#endif
#ifdef _RPOUT_CLC10OUT
    case NORA_PPS_OUTPUT_CLC10: *code = (uint8_t)_RPOUT_CLC10OUT; return true;
#endif
#ifdef _RPOUT_PWM4H
    case NORA_PPS_OUTPUT_PWM4H: *code = (uint8_t)_RPOUT_PWM4H; return true;
#endif
#ifdef _RPOUT_PWM4L
    case NORA_PPS_OUTPUT_PWM4L: *code = (uint8_t)_RPOUT_PWM4L; return true;
#endif
    /* PWM event outputs. AK silicon spells these PEVTA..PEVTD; CK silicon calls
     * the same signal PWMEA..PWMED. The enum name in nora_pps.h is the FUNCTION,
     * so the family-specific spelling is resolved here and nowhere else. */
#ifdef _RPOUT_PEVTA
    case NORA_PPS_OUTPUT_PWM_EVENT_A: *code = (uint8_t)_RPOUT_PEVTA; return true;
#endif
#ifdef _RPOUT_PEVTB
    case NORA_PPS_OUTPUT_PWM_EVENT_B: *code = (uint8_t)_RPOUT_PEVTB; return true;
#endif
#ifdef _RPOUT_PEVTC
    case NORA_PPS_OUTPUT_PWM_EVENT_C: *code = (uint8_t)_RPOUT_PEVTC; return true;
#endif
#ifdef _RPOUT_PEVTD
    case NORA_PPS_OUTPUT_PWM_EVENT_D: *code = (uint8_t)_RPOUT_PEVTD; return true;
#endif
#ifdef _RPOUT_CMP1
    case NORA_PPS_OUTPUT_CMP1: *code = (uint8_t)_RPOUT_CMP1; return true;
#endif
#ifdef _RPOUT_CMP2
    case NORA_PPS_OUTPUT_CMP2: *code = (uint8_t)_RPOUT_CMP2; return true;
#endif
#ifdef _RPOUT_CMP3
    case NORA_PPS_OUTPUT_CMP3: *code = (uint8_t)_RPOUT_CMP3; return true;
#endif
#ifdef _RPOUT_CMP4
    case NORA_PPS_OUTPUT_CMP4: *code = (uint8_t)_RPOUT_CMP4; return true;
#endif
#ifdef _RPOUT_CMP5
    case NORA_PPS_OUTPUT_CMP5: *code = (uint8_t)_RPOUT_CMP5; return true;
#endif
#ifdef _RPOUT_CMP6
    case NORA_PPS_OUTPUT_CMP6: *code = (uint8_t)_RPOUT_CMP6; return true;
#endif
#ifdef _RPOUT_CMP7
    case NORA_PPS_OUTPUT_CMP7: *code = (uint8_t)_RPOUT_CMP7; return true;
#endif
#ifdef _RPOUT_CMP8
    case NORA_PPS_OUTPUT_CMP8: *code = (uint8_t)_RPOUT_CMP8; return true;
#endif
    /* Reference clock outputs. Both AK parts spell these with the digit; the CK
     * backend additionally accepts the digitless _RPOUT_REFO of single-REFO
     * parts, which is why REFO1 is not simply guarded on one macro there. */
#ifdef _RPOUT_REFO1
    case NORA_PPS_OUTPUT_REFO1: *code = (uint8_t)_RPOUT_REFO1; return true;
#endif
#ifdef _RPOUT_REFO2
    case NORA_PPS_OUTPUT_REFO2: *code = (uint8_t)_RPOUT_REFO2; return true;
#endif
    default:
        break;
    }
    return false;   /* peripheral output not available on this device */
}


//===========================================================
// Local: write the output function code onto an RP pin's _RPnnR
//===========================================================
// _RPnnR is a bit-field alias (assignment only; no address / no formula). Every
// RP for which the selected device defines an output register (_RPnnR) is
// listed -- physical RP1..RP128 AND the virtual RPV0..RPV15 (RP129..RP144) at the
// end, each #ifdef-guarded so only the device's real registers compile. An RP with
// no output PPS register on this device returns false.
// Flat register table by design -- RPORx numbering has gaps and is
// device-specific, so a switch (not a formula) is the safe canonical form.
//
// The virtual pins belong in THIS switch rather than a parallel one because the
// operation is identical: same _RPnnR alias, same 6-bit function code. The only
// difference is that no GPIO configuration precedes them, and that difference lives
// in nora_pinmux_route_*() -- which configures, and therefore refuses them.
static bool nora_pps_write_output_rp(nora_gpio_rp_t rp, uint8_t code)
{
    switch (rp)
    {
#ifdef _RP1R
    case 1u: _RP1R = code; return true;
#endif
#ifdef _RP2R
    case 2u: _RP2R = code; return true;
#endif
#ifdef _RP3R
    case 3u: _RP3R = code; return true;
#endif
#ifdef _RP4R
    case 4u: _RP4R = code; return true;
#endif
#ifdef _RP5R
    case 5u: _RP5R = code; return true;
#endif
#ifdef _RP6R
    case 6u: _RP6R = code; return true;
#endif
#ifdef _RP7R
    case 7u: _RP7R = code; return true;
#endif
#ifdef _RP8R
    case 8u: _RP8R = code; return true;
#endif
#ifdef _RP9R
    case 9u: _RP9R = code; return true;
#endif
#ifdef _RP10R
    case 10u: _RP10R = code; return true;
#endif
#ifdef _RP11R
    case 11u: _RP11R = code; return true;
#endif
#ifdef _RP12R
    case 12u: _RP12R = code; return true;
#endif
#ifdef _RP13R
    case 13u: _RP13R = code; return true;
#endif
#ifdef _RP14R
    case 14u: _RP14R = code; return true;
#endif
#ifdef _RP15R
    case 15u: _RP15R = code; return true;
#endif
#ifdef _RP16R
    case 16u: _RP16R = code; return true;
#endif
#ifdef _RP17R
    case 17u: _RP17R = code; return true;
#endif
#ifdef _RP18R
    case 18u: _RP18R = code; return true;
#endif
#ifdef _RP19R
    case 19u: _RP19R = code; return true;
#endif
#ifdef _RP20R
    case 20u: _RP20R = code; return true;
#endif
#ifdef _RP21R
    case 21u: _RP21R = code; return true;
#endif
#ifdef _RP22R
    case 22u: _RP22R = code; return true;
#endif
#ifdef _RP23R
    case 23u: _RP23R = code; return true;
#endif
#ifdef _RP24R
    case 24u: _RP24R = code; return true;
#endif
#ifdef _RP25R
    case 25u: _RP25R = code; return true;
#endif
#ifdef _RP26R
    case 26u: _RP26R = code; return true;
#endif
#ifdef _RP27R
    case 27u: _RP27R = code; return true;
#endif
#ifdef _RP28R
    case 28u: _RP28R = code; return true;
#endif
#ifdef _RP29R
    case 29u: _RP29R = code; return true;
#endif
#ifdef _RP30R
    case 30u: _RP30R = code; return true;
#endif
#ifdef _RP31R
    case 31u: _RP31R = code; return true;
#endif
#ifdef _RP32R
    case 32u: _RP32R = code; return true;
#endif
#ifdef _RP33R
    case 33u: _RP33R = code; return true;
#endif
#ifdef _RP34R
    case 34u: _RP34R = code; return true;
#endif
#ifdef _RP35R
    case 35u: _RP35R = code; return true;
#endif
#ifdef _RP36R
    case 36u: _RP36R = code; return true;
#endif
#ifdef _RP37R
    case 37u: _RP37R = code; return true;
#endif
#ifdef _RP38R
    case 38u: _RP38R = code; return true;
#endif
#ifdef _RP39R
    case 39u: _RP39R = code; return true;
#endif
#ifdef _RP40R
    case 40u: _RP40R = code; return true;
#endif
#ifdef _RP41R
    case 41u: _RP41R = code; return true;
#endif
#ifdef _RP42R
    case 42u: _RP42R = code; return true;
#endif
#ifdef _RP43R
    case 43u: _RP43R = code; return true;
#endif
#ifdef _RP44R
    case 44u: _RP44R = code; return true;
#endif
#ifdef _RP45R
    case 45u: _RP45R = code; return true;
#endif
#ifdef _RP46R
    case 46u: _RP46R = code; return true;
#endif
#ifdef _RP47R
    case 47u: _RP47R = code; return true;
#endif
#ifdef _RP48R
    case 48u: _RP48R = code; return true;
#endif
#ifdef _RP49R
    case 49u: _RP49R = code; return true;
#endif
#ifdef _RP50R
    case 50u: _RP50R = code; return true;
#endif
#ifdef _RP51R
    case 51u: _RP51R = code; return true;
#endif
#ifdef _RP52R
    case 52u: _RP52R = code; return true;
#endif
#ifdef _RP53R
    case 53u: _RP53R = code; return true;
#endif
#ifdef _RP54R
    case 54u: _RP54R = code; return true;
#endif
#ifdef _RP55R
    case 55u: _RP55R = code; return true;
#endif
#ifdef _RP56R
    case 56u: _RP56R = code; return true;
#endif
#ifdef _RP57R
    case 57u: _RP57R = code; return true;
#endif
#ifdef _RP58R
    case 58u: _RP58R = code; return true;
#endif
#ifdef _RP59R
    case 59u: _RP59R = code; return true;
#endif
#ifdef _RP60R
    case 60u: _RP60R = code; return true;
#endif
#ifdef _RP61R
    case 61u: _RP61R = code; return true;
#endif
#ifdef _RP62R
    case 62u: _RP62R = code; return true;
#endif
#ifdef _RP63R
    case 63u: _RP63R = code; return true;
#endif
#ifdef _RP64R
    case 64u: _RP64R = code; return true;
#endif
#ifdef _RP65R
    case 65u: _RP65R = code; return true;
#endif
#ifdef _RP66R
    case 66u: _RP66R = code; return true;
#endif
#ifdef _RP67R
    case 67u: _RP67R = code; return true;
#endif
#ifdef _RP68R
    case 68u: _RP68R = code; return true;
#endif
#ifdef _RP69R
    case 69u: _RP69R = code; return true;
#endif
#ifdef _RP70R
    case 70u: _RP70R = code; return true;
#endif
#ifdef _RP71R
    case 71u: _RP71R = code; return true;
#endif
#ifdef _RP72R
    case 72u: _RP72R = code; return true;
#endif
#ifdef _RP73R
    case 73u: _RP73R = code; return true;
#endif
#ifdef _RP74R
    case 74u: _RP74R = code; return true;
#endif
#ifdef _RP75R
    case 75u: _RP75R = code; return true;
#endif
#ifdef _RP76R
    case 76u: _RP76R = code; return true;
#endif
#ifdef _RP77R
    case 77u: _RP77R = code; return true;
#endif
#ifdef _RP78R
    case 78u: _RP78R = code; return true;
#endif
#ifdef _RP79R
    case 79u: _RP79R = code; return true;
#endif
#ifdef _RP80R
    case 80u: _RP80R = code; return true;
#endif
#ifdef _RP81R
    case 81u: _RP81R = code; return true;
#endif
#ifdef _RP82R
    case 82u: _RP82R = code; return true;
#endif
#ifdef _RP83R
    case 83u: _RP83R = code; return true;
#endif
#ifdef _RP84R
    case 84u: _RP84R = code; return true;
#endif
#ifdef _RP85R
    case 85u: _RP85R = code; return true;
#endif
#ifdef _RP86R
    case 86u: _RP86R = code; return true;
#endif
#ifdef _RP87R
    case 87u: _RP87R = code; return true;
#endif
#ifdef _RP88R
    case 88u: _RP88R = code; return true;
#endif
#ifdef _RP89R
    case 89u: _RP89R = code; return true;
#endif
#ifdef _RP90R
    case 90u: _RP90R = code; return true;
#endif
#ifdef _RP91R
    case 91u: _RP91R = code; return true;
#endif
#ifdef _RP92R
    case 92u: _RP92R = code; return true;
#endif
#ifdef _RP93R
    case 93u: _RP93R = code; return true;
#endif
#ifdef _RP94R
    case 94u: _RP94R = code; return true;
#endif
#ifdef _RP95R
    case 95u: _RP95R = code; return true;
#endif
#ifdef _RP96R
    case 96u: _RP96R = code; return true;
#endif
#ifdef _RP97R
    case 97u: _RP97R = code; return true;
#endif
#ifdef _RP98R
    case 98u: _RP98R = code; return true;
#endif
#ifdef _RP99R
    case 99u: _RP99R = code; return true;
#endif
#ifdef _RP100R
    case 100u: _RP100R = code; return true;
#endif
#ifdef _RP101R
    case 101u: _RP101R = code; return true;
#endif
#ifdef _RP102R
    case 102u: _RP102R = code; return true;
#endif
#ifdef _RP103R
    case 103u: _RP103R = code; return true;
#endif
#ifdef _RP104R
    case 104u: _RP104R = code; return true;
#endif
#ifdef _RP105R
    case 105u: _RP105R = code; return true;
#endif
#ifdef _RP106R
    case 106u: _RP106R = code; return true;
#endif
#ifdef _RP107R
    case 107u: _RP107R = code; return true;
#endif
#ifdef _RP108R
    case 108u: _RP108R = code; return true;
#endif
#ifdef _RP109R
    case 109u: _RP109R = code; return true;
#endif
#ifdef _RP110R
    case 110u: _RP110R = code; return true;
#endif
#ifdef _RP111R
    case 111u: _RP111R = code; return true;
#endif
#ifdef _RP112R
    case 112u: _RP112R = code; return true;
#endif
#ifdef _RP113R
    case 113u: _RP113R = code; return true;
#endif
#ifdef _RP114R
    case 114u: _RP114R = code; return true;
#endif
#ifdef _RP115R
    case 115u: _RP115R = code; return true;
#endif
#ifdef _RP116R
    case 116u: _RP116R = code; return true;
#endif
#ifdef _RP117R
    case 117u: _RP117R = code; return true;
#endif
#ifdef _RP118R
    case 118u: _RP118R = code; return true;
#endif
#ifdef _RP119R
    case 119u: _RP119R = code; return true;
#endif
#ifdef _RP120R
    case 120u: _RP120R = code; return true;
#endif
#ifdef _RP121R
    case 121u: _RP121R = code; return true;
#endif
#ifdef _RP122R
    case 122u: _RP122R = code; return true;
#endif
#ifdef _RP123R
    case 123u: _RP123R = code; return true;
#endif
#ifdef _RP124R
    case 124u: _RP124R = code; return true;
#endif
#ifdef _RP125R
    case 125u: _RP125R = code; return true;
#endif
#ifdef _RP126R
    case 126u: _RP126R = code; return true;
#endif
#ifdef _RP127R
    case 127u: _RP127R = code; return true;
#endif
#ifdef _RP128R
    case 128u: _RP128R = code; return true;
#endif
    /* Virtual pins RPV0..RPV15 (RP129..RP144): padless on-chip routing endpoints.
     * Absent entirely on dsPIC33AK128MC106, so these all compile out there. */
#ifdef _RP129R
    case 129u: _RP129R = code; return true;
#endif
#ifdef _RP130R
    case 130u: _RP130R = code; return true;
#endif
#ifdef _RP131R
    case 131u: _RP131R = code; return true;
#endif
#ifdef _RP132R
    case 132u: _RP132R = code; return true;
#endif
#ifdef _RP133R
    case 133u: _RP133R = code; return true;
#endif
#ifdef _RP134R
    case 134u: _RP134R = code; return true;
#endif
#ifdef _RP135R
    case 135u: _RP135R = code; return true;
#endif
#ifdef _RP136R
    case 136u: _RP136R = code; return true;
#endif
#ifdef _RP137R
    case 137u: _RP137R = code; return true;
#endif
#ifdef _RP138R
    case 138u: _RP138R = code; return true;
#endif
#ifdef _RP139R
    case 139u: _RP139R = code; return true;
#endif
#ifdef _RP140R
    case 140u: _RP140R = code; return true;
#endif
#ifdef _RP141R
    case 141u: _RP141R = code; return true;
#endif
#ifdef _RP142R
    case 142u: _RP142R = code; return true;
#endif
#ifdef _RP143R
    case 143u: _RP143R = code; return true;
#endif
#ifdef _RP144R
    case 144u: _RP144R = code; return true;
#endif
    default:
        break;
    }
    return false;   /* RP has no output PPS register on this device */
}


//===========================================================
// Local: is this RP a VIRTUAL endpoint the selected device defines?
//===========================================================
// Built from the same per-register #ifdef list as the write switch above, for the
// same reason nora_pps_rp_is_defined() is: a range check would claim RPV0..RPV15
// exist on dsPIC33AK128MC106, which stops at _RP80R and has no virtual band at all.
// Fall-through cases, no bodies -- the switch IS the answer.
static bool nora_pps_rpv_is_defined(nora_gpio_rp_t rp)
{
    if ((rp < NORA_PPS_RPV_MIN) || (rp > NORA_PPS_RPV_MAX))
    {
        return false;               /* fast reject; not in the virtual band */
    }

    switch (rp)
    {
#ifdef _RP129R
    case 129u:
#endif
#ifdef _RP130R
    case 130u:
#endif
#ifdef _RP131R
    case 131u:
#endif
#ifdef _RP132R
    case 132u:
#endif
#ifdef _RP133R
    case 133u:
#endif
#ifdef _RP134R
    case 134u:
#endif
#ifdef _RP135R
    case 135u:
#endif
#ifdef _RP136R
    case 136u:
#endif
#ifdef _RP137R
    case 137u:
#endif
#ifdef _RP138R
    case 138u:
#endif
#ifdef _RP139R
    case 139u:
#endif
#ifdef _RP140R
    case 140u:
#endif
#ifdef _RP141R
    case 141u:
#endif
#ifdef _RP142R
    case 142u:
#endif
#ifdef _RP143R
    case 143u:
#endif
#ifdef _RP144R
    case 144u:
#endif
        return true;
    default:
        break;
    }
    return false;
}


//===========================================================
// Local: read the output function code from a physical RP pin's _RPnnR
//===========================================================
// dsPIC33AK RPORx registers pack four RP output codes into 8-bit slots, with
// RP1 in the low slot of RPOR0. Callers first validate the RP against the
// _RPnnR switch below, so this only reads a physical output PPS register.
static uint8_t nora_pps_read_output_rp(nora_gpio_rp_t rp)
{
    volatile uint32_t *reg = (&RPOR0) + ((rp - 1u) / 4u);
    const uint32_t pos = ((rp - 1u) % 4u) * 8u;

    return (uint8_t)((*reg >> pos) & 0x7Fu);
}


//===========================================================
// Local: is rp a physical remappable pin on this device?
//===========================================================
// PPS input sources and output pins are the same physical RP set, so an RP that
// the device defines an output register (_RPnnR) for is a valid physical RP.
// Used to validate the RP handed to route_input(); route_output()'s write switch
// validates inherently. RPV virtual pins (RP129..) have no _RPnnR and are rejected.
static bool nora_pps_rp_is_defined(nora_gpio_rp_t rp)
{
    switch (rp)
    {
#ifdef _RP1R
    case 1u:
#endif
#ifdef _RP2R
    case 2u:
#endif
#ifdef _RP3R
    case 3u:
#endif
#ifdef _RP4R
    case 4u:
#endif
#ifdef _RP5R
    case 5u:
#endif
#ifdef _RP6R
    case 6u:
#endif
#ifdef _RP7R
    case 7u:
#endif
#ifdef _RP8R
    case 8u:
#endif
#ifdef _RP9R
    case 9u:
#endif
#ifdef _RP10R
    case 10u:
#endif
#ifdef _RP11R
    case 11u:
#endif
#ifdef _RP12R
    case 12u:
#endif
#ifdef _RP13R
    case 13u:
#endif
#ifdef _RP14R
    case 14u:
#endif
#ifdef _RP15R
    case 15u:
#endif
#ifdef _RP16R
    case 16u:
#endif
#ifdef _RP17R
    case 17u:
#endif
#ifdef _RP18R
    case 18u:
#endif
#ifdef _RP19R
    case 19u:
#endif
#ifdef _RP20R
    case 20u:
#endif
#ifdef _RP21R
    case 21u:
#endif
#ifdef _RP22R
    case 22u:
#endif
#ifdef _RP23R
    case 23u:
#endif
#ifdef _RP24R
    case 24u:
#endif
#ifdef _RP25R
    case 25u:
#endif
#ifdef _RP26R
    case 26u:
#endif
#ifdef _RP27R
    case 27u:
#endif
#ifdef _RP28R
    case 28u:
#endif
#ifdef _RP29R
    case 29u:
#endif
#ifdef _RP30R
    case 30u:
#endif
#ifdef _RP31R
    case 31u:
#endif
#ifdef _RP32R
    case 32u:
#endif
#ifdef _RP33R
    case 33u:
#endif
#ifdef _RP34R
    case 34u:
#endif
#ifdef _RP35R
    case 35u:
#endif
#ifdef _RP36R
    case 36u:
#endif
#ifdef _RP37R
    case 37u:
#endif
#ifdef _RP38R
    case 38u:
#endif
#ifdef _RP39R
    case 39u:
#endif
#ifdef _RP40R
    case 40u:
#endif
#ifdef _RP41R
    case 41u:
#endif
#ifdef _RP42R
    case 42u:
#endif
#ifdef _RP43R
    case 43u:
#endif
#ifdef _RP44R
    case 44u:
#endif
#ifdef _RP45R
    case 45u:
#endif
#ifdef _RP46R
    case 46u:
#endif
#ifdef _RP47R
    case 47u:
#endif
#ifdef _RP48R
    case 48u:
#endif
#ifdef _RP49R
    case 49u:
#endif
#ifdef _RP50R
    case 50u:
#endif
#ifdef _RP51R
    case 51u:
#endif
#ifdef _RP52R
    case 52u:
#endif
#ifdef _RP53R
    case 53u:
#endif
#ifdef _RP54R
    case 54u:
#endif
#ifdef _RP55R
    case 55u:
#endif
#ifdef _RP56R
    case 56u:
#endif
#ifdef _RP57R
    case 57u:
#endif
#ifdef _RP58R
    case 58u:
#endif
#ifdef _RP59R
    case 59u:
#endif
#ifdef _RP60R
    case 60u:
#endif
#ifdef _RP61R
    case 61u:
#endif
#ifdef _RP62R
    case 62u:
#endif
#ifdef _RP63R
    case 63u:
#endif
#ifdef _RP64R
    case 64u:
#endif
#ifdef _RP65R
    case 65u:
#endif
#ifdef _RP66R
    case 66u:
#endif
#ifdef _RP67R
    case 67u:
#endif
#ifdef _RP68R
    case 68u:
#endif
#ifdef _RP69R
    case 69u:
#endif
#ifdef _RP70R
    case 70u:
#endif
#ifdef _RP71R
    case 71u:
#endif
#ifdef _RP72R
    case 72u:
#endif
#ifdef _RP73R
    case 73u:
#endif
#ifdef _RP74R
    case 74u:
#endif
#ifdef _RP75R
    case 75u:
#endif
#ifdef _RP76R
    case 76u:
#endif
#ifdef _RP77R
    case 77u:
#endif
#ifdef _RP78R
    case 78u:
#endif
#ifdef _RP79R
    case 79u:
#endif
#ifdef _RP80R
    case 80u:
#endif
#ifdef _RP81R
    case 81u:
#endif
#ifdef _RP82R
    case 82u:
#endif
#ifdef _RP83R
    case 83u:
#endif
#ifdef _RP84R
    case 84u:
#endif
#ifdef _RP85R
    case 85u:
#endif
#ifdef _RP86R
    case 86u:
#endif
#ifdef _RP87R
    case 87u:
#endif
#ifdef _RP88R
    case 88u:
#endif
#ifdef _RP89R
    case 89u:
#endif
#ifdef _RP90R
    case 90u:
#endif
#ifdef _RP91R
    case 91u:
#endif
#ifdef _RP92R
    case 92u:
#endif
#ifdef _RP93R
    case 93u:
#endif
#ifdef _RP94R
    case 94u:
#endif
#ifdef _RP95R
    case 95u:
#endif
#ifdef _RP96R
    case 96u:
#endif
#ifdef _RP97R
    case 97u:
#endif
#ifdef _RP98R
    case 98u:
#endif
#ifdef _RP99R
    case 99u:
#endif
#ifdef _RP100R
    case 100u:
#endif
#ifdef _RP101R
    case 101u:
#endif
#ifdef _RP102R
    case 102u:
#endif
#ifdef _RP103R
    case 103u:
#endif
#ifdef _RP104R
    case 104u:
#endif
#ifdef _RP105R
    case 105u:
#endif
#ifdef _RP106R
    case 106u:
#endif
#ifdef _RP107R
    case 107u:
#endif
#ifdef _RP108R
    case 108u:
#endif
#ifdef _RP109R
    case 109u:
#endif
#ifdef _RP110R
    case 110u:
#endif
#ifdef _RP111R
    case 111u:
#endif
#ifdef _RP112R
    case 112u:
#endif
#ifdef _RP113R
    case 113u:
#endif
#ifdef _RP114R
    case 114u:
#endif
#ifdef _RP115R
    case 115u:
#endif
#ifdef _RP116R
    case 116u:
#endif
#ifdef _RP117R
    case 117u:
#endif
#ifdef _RP118R
    case 118u:
#endif
#ifdef _RP119R
    case 119u:
#endif
#ifdef _RP120R
    case 120u:
#endif
#ifdef _RP121R
    case 121u:
#endif
#ifdef _RP122R
    case 122u:
#endif
#ifdef _RP123R
    case 123u:
#endif
#ifdef _RP124R
    case 124u:
#endif
#ifdef _RP125R
    case 125u:
#endif
#ifdef _RP126R
    case 126u:
#endif
#ifdef _RP127R
    case 127u:
#endif
#ifdef _RP128R
    case 128u:
#endif
        return true;
    default:
        return false;
    }
}


//===========================================================
// Global
//===========================================================
bool nora_pps_route_output(nora_pps_output_t output, nora_gpio_rp_t rp)
{
    uint8_t code;
    if (!nora_pps_get_output_code(output, &code))
    {
        return false;
    }

    nora_pps_unlock();
    bool ok = nora_pps_write_output_rp(rp, code);
    nora_pps_lock();
    return ok;
}

bool nora_pps_find_output_rp(nora_pps_output_t output, nora_gpio_rp_t *rp)
{
    uint8_t want;

    if (rp == NULL)
    {
        return false;
    }
    if (!nora_pps_get_output_code(output, &want))
    {
        return false;   /* peripheral output not available on this device */
    }

    /* PHYSICAL pins only, and deliberately so even though route_output() now accepts
     * the virtual band: a caller asking "which pad is this signal on" does not want
     * RPV0, and a caller that put the signal on a virtual endpoint did so itself and
     * therefore already knows. Same rule as the CK backend.
     * nora_pps_rp_is_defined() also skips any RP number without an output PPS
     * register on the selected device. */
    for (nora_gpio_rp_t candidate = 1u; candidate <= 128u; ++candidate)
    {
        if (nora_pps_rp_is_defined(candidate) &&
            (nora_pps_read_output_rp(candidate) == want))
        {
            *rp = candidate;
            return true;
        }
    }

    return false;   /* no physical pin currently carries this output */
}

bool nora_pps_route_input(nora_pps_input_t input, nora_gpio_rp_t rp)
{
    bool ok = true;

    /* Reject any RP that is neither a physical remappable pin nor a virtual one
     * before writing. RPINRx input-selects take the RP NUMBER, and the virtual
     * numbers are legal values there -- that is how a padless output reaches
     * another peripheral's input -- so the check admits both bands and nothing
     * between or beyond them. */
    if (!nora_pps_rp_is_defined(rp) && !nora_pps_rpv_is_defined(rp))
    {
        return false;   /* rp is not a remappable pin, physical or virtual, here */
    }

    nora_pps_unlock();
    switch (input)
    {
#ifdef _U1RXR
    case NORA_PPS_INPUT_U1RX: _U1RXR = rp; break;
#endif
#ifdef _U2RXR
    case NORA_PPS_INPUT_U2RX: _U2RXR = rp; break;
#endif
#ifdef _SS1R
    case NORA_PPS_INPUT_SS1:  _SS1R  = rp; break;
#endif
#ifdef _SCK1R
    case NORA_PPS_INPUT_SCK1: _SCK1R = rp; break;
#endif
#ifdef _SDI1R
    case NORA_PPS_INPUT_SDI1: _SDI1R = rp; break;
#endif
#ifdef _SS2R
    case NORA_PPS_INPUT_SS2:  _SS2R  = rp; break;
#endif
#ifdef _SCK2R
    case NORA_PPS_INPUT_SCK2: _SCK2R = rp; break;
#endif
#ifdef _SDI2R
    case NORA_PPS_INPUT_SDI2: _SDI2R = rp; break;
#endif
#ifdef _SS3R
    case NORA_PPS_INPUT_SS3:  _SS3R  = rp; break;
#endif
#ifdef _SCK3R
    case NORA_PPS_INPUT_SCK3: _SCK3R = rp; break;
#endif
#ifdef _SDI3R
    case NORA_PPS_INPUT_SDI3: _SDI3R = rp; break;
#endif
#ifdef _SS4R
    case NORA_PPS_INPUT_SS4:  _SS4R  = rp; break;
#endif
#ifdef _SCK4R
    case NORA_PPS_INPUT_SCK4: _SCK4R = rp; break;
#endif
#ifdef _SDI4R
    case NORA_PPS_INPUT_SDI4: _SDI4R = rp; break;
#endif
#ifdef _CLCINAR
    case NORA_PPS_INPUT_CLCINA: _CLCINAR = rp; break;
#endif
#ifdef _CLCINBR
    case NORA_PPS_INPUT_CLCINB: _CLCINBR = rp; break;
#endif
#ifdef _CLCINCR
    case NORA_PPS_INPUT_CLCINC: _CLCINCR = rp; break;
#endif
#ifdef _REFI1R
    case NORA_PPS_INPUT_REFI1: _REFI1R = rp; break;
#endif
#ifdef _CAN1RXR
    case NORA_PPS_INPUT_CAN1RX: _CAN1RXR = rp; break;
#endif
#ifdef _ICM1R
    case NORA_PPS_INPUT_ICM1: _ICM1R = rp; break;
#endif
#ifdef _ICM2R
    case NORA_PPS_INPUT_ICM2: _ICM2R = rp; break;
#endif
#ifdef _ICM3R
    case NORA_PPS_INPUT_ICM3: _ICM3R = rp; break;
#endif
#ifdef _ICM4R
    case NORA_PPS_INPUT_ICM4: _ICM4R = rp; break;
#endif
#ifdef _ICM5R
    case NORA_PPS_INPUT_ICM5: _ICM5R = rp; break;
#endif
#ifdef _ICM6R
    case NORA_PPS_INPUT_ICM6: _ICM6R = rp; break;
#endif
#ifdef _ICM7R
    case NORA_PPS_INPUT_ICM7: _ICM7R = rp; break;
#endif
#ifdef _ICM8R
    case NORA_PPS_INPUT_ICM8: _ICM8R = rp; break;
#endif
#ifdef _ICM9R
    case NORA_PPS_INPUT_ICM9: _ICM9R = rp; break;
#endif
#ifdef _U3RXR
    case NORA_PPS_INPUT_U3RX: _U3RXR = rp; break;
#endif
#ifdef _CLCINDR
    case NORA_PPS_INPUT_CLCIND: _CLCINDR = rp; break;
#endif
#ifdef _CLCINER
    case NORA_PPS_INPUT_CLCINE: _CLCINER = rp; break;
#endif
#ifdef _CLCINFR
    case NORA_PPS_INPUT_CLCINF: _CLCINFR = rp; break;
#endif
#ifdef _CLCINGR
    case NORA_PPS_INPUT_CLCING: _CLCINGR = rp; break;
#endif
#ifdef _CLCINHR
    case NORA_PPS_INPUT_CLCINH: _CLCINHR = rp; break;
#endif
#ifdef _CLCINIR
    case NORA_PPS_INPUT_CLCINI: _CLCINIR = rp; break;
#endif
#ifdef _CLCINJR
    case NORA_PPS_INPUT_CLCINJ: _CLCINJR = rp; break;
#endif
#ifdef _REFI2R
    case NORA_PPS_INPUT_REFI2: _REFI2R = rp; break;
#endif
#ifdef _CAN2RXR
    case NORA_PPS_INPUT_CAN2RX: _CAN2RXR = rp; break;
#endif
    /* External interrupts. INT1..INT3 exist on both families; _INT4R is AK-only. */
#ifdef _INT1R
    case NORA_PPS_INPUT_INT1: _INT1R = rp; break;
#endif
#ifdef _INT2R
    case NORA_PPS_INPUT_INT2: _INT2R = rp; break;
#endif
#ifdef _INT3R
    case NORA_PPS_INPUT_INT3: _INT3R = rp; break;
#endif
#ifdef _INT4R
    case NORA_PPS_INPUT_INT4: _INT4R = rp; break;
#endif
    default:
        ok = false;   /* peripheral input not available on this device */
        break;
    }
    nora_pps_lock();
    return ok;
}


/*
 * Combined pinmux helpers: digital-configure the pin, then route the PPS signal. See the header
 * for the contract. These reuse the low-level GPIO and PPS APIs as-is (no register duplication);
 * the GPIO configuration runs first so the glitch-aware order is preserved for outputs.
 */
bool nora_pinmux_route_input(nora_pps_input_t function, nora_gpio_rp_t rp)
{
    if (!nora_gpio_rp_config_digital_input(rp))
    {
        return false;
    }
    return nora_pps_route_input(function, rp);
}

bool nora_pinmux_route_output(nora_pps_output_t function, nora_gpio_rp_t rp,
                                   bool initial_high)
{
    if (!nora_gpio_rp_config_digital_output(rp, initial_high))
    {
        return false;
    }
    return nora_pps_route_output(function, rp);
}
