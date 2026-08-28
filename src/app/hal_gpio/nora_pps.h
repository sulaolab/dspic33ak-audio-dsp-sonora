#ifndef NORA_PPS_H
#define NORA_PPS_H

/*
 * nora_pps.h
 * ---------------
 * Peripheral Pin Select (PPS) routing -- the companion to the NORA GPIO HAL
 * (same hal_gpio family). NORA GPIO owns the pin's ELECTRICAL attributes
 * (TRIS/LAT/ANSEL/pull/OD); this module owns the SIGNAL ROUTING (which RP pin a
 * peripheral input reads from / a peripheral output drives), i.e. the RPINRx
 * (input-select) and RPORx (output _RPnnR) registers.
 *
 * Why a thin PPS layer:
 *   Board code otherwise writes device SFRs directly -- e.g.
 *       _RP90R = 29;        // SCK2 out on RP90 (what is 29?)
 *       _SS2R  = 29;        // SS2 in from RP29
 *   which leaks the RPORx/RPINRx map and the raw function codes into the board.
 *   These two calls hide that:
 *       nora_pps_route_output(NORA_PPS_OUTPUT_SCK2, 90u);
 *       nora_pps_route_input (NORA_PPS_INPUT_SS2,   29u);
 *   leaving only "which signal" + "which RP" at the call site.
 *
 * Processor adaptation: the implementation keys off the XC SFR/constant macros
 * (_RPOUT_xxx, _RPnnR, the input-select registers) with #ifdef, so a peripheral
 * or RP that the selected device does not define is simply unroutable (the call
 * returns false) -- no per-device part-number conditionals here.
 *
 * Coverage: both route_* accept any remappable pin the selected device defines --
 * PHYSICAL (RP1..RP128) or VIRTUAL (RPV0..RPV15 == RP129..RP144, see below). RP is
 * a nora_gpio_rp_t supplied by the board pin map; its numeric range and encoding
 * are processor-specific. Every case is #ifdef-guarded on its _RPnnR, so an RP the
 * selected device does not define is rejected before any register write.
 *
 * This paragraph used to say the RPV outputs were "intentionally NOT routable
 * here -- if they are ever needed, add a separate virtual-output API rather than
 * overloading this GPIO-typed one." That call is reversed, and the reason is that
 * the CK HAL took the other road and the other road works: an RPV endpoint has an
 * _RPnnR that takes the same 6-bit function code as any pad, so routing to one is
 * the SAME operation, and CK's TDM frame-sync generator uses RPV0 to hand a SPI
 * FRMSYNC marker to a CLC input through exactly this call. A separate API would
 * have been a second copy of the RPORx map -- the one thing this file exists to
 * keep in a single place. What "GPIO-typed" really costs is not routing but
 * CONFIGURATION, and that is fenced off where it belongs: nora_gpio_rp_config_*
 * rejects anything above RP_MAX, so nora_pinmux_route_*() (which configures first)
 * refuses an RPV, while nora_pps_route_*() (which does not) accepts it.
 *
 * The peripheral-SIGNAL enums are the AK/CK UNION, not a list of what this
 * codebase happens to call today: for every peripheral family the enums name,
 * they carry every instance either family's device headers define (UART1-3,
 * SPI1-4, CLC1-10, PWM1-8 H/L as available, PWM event A-D, CMP1-8, REFO1/2,
 * REFI1, INT1-4, CAN1/2, ICM1-9). They still do NOT cover every PPS-capable
 * peripheral the silicon has -- OCM, SENT, QEI, BISS and PTGTRG are absent as
 * whole families. Add a new signal by extending the enum and the matching case
 * in nora_pps_dspic33ak.c -- and add it to the CK header too, because the two
 * enums are kept textually identical on purpose. Routing a signal absent on the
 * selected device returns false.
 *
 * Pairing with hal_gpio: PPS routing does NOT configure the pin's direction or
 * analog/digital select. Configure the pin first (nora_gpio_rp_config_*),
 * then route the signal. Order is glitch-aware for outputs: seed the GPIO output
 * (LAT/TRIS) before the PPS output starts driving it.
 *
 * IOLOCK: each route_* call brackets its own write with unlock()/lock() (IOLOCK
 * resets to 0 = unlocked, so this only adds protection). unlock()/lock() are also
 * exposed for code that writes PPS SFRs directly (e.g. the PWM driver) or wants a
 * single window around a batch of direct writes.
 */

#include <stdbool.h>

#include "nora_gpio.h"   /* nora_gpio_rp_t */

/*
 * VIRTUAL RP PINS (RPV0..RPV15 == RP129..RP144).
 *
 * These are remappable PPS endpoints with NO PAD: a peripheral output routed to one
 * is readable by another peripheral's PPS input and by nothing else. They are how a
 * signal is fed from one on-chip block to another without spending a package pin.
 *
 * They are deliberately NOT nora_gpio_pin_t values: they have no TRIS, no LAT and no
 * ANSEL, so nothing in the GPIO half of this family applies to them. They are
 * nora_gpio_rp_t values, because routing is the one thing they DO have -- which is why
 * the two route_*() calls take them and the gpio config calls (and therefore
 * nora_pinmux_route_*()) do not.
 *
 * Availability is per device (checked via _RP129R etc. in the .c); a virtual pin the
 * device does not define is simply unroutable and the call returns false. On
 * dsPIC33AK128MC106 that is ALL of them -- its output registers stop at _RP80R.
 *
 * HOW MANY IS A SILICON COUNT, like NORA_GPIO_RP_MAX. This family has sixteen
 * (RP129..RP144 on dsPIC33AK512MPS512); the dsPIC33CK family has six (RPV0..RPV5 ==
 * RP176..RP181) on every part. So the numbers behind these names are NOT portable and
 * were never going to be -- but the NAMES are, and code that stays within
 * RPV0..RPV5 compiles and routes on both families. Beyond RPV5 is AK-only.
 */
#define NORA_PPS_RP_VIRTUAL_FIRST   ((nora_gpio_rp_t)129u)
#define NORA_PPS_RPV0               ((nora_gpio_rp_t)129u)
#define NORA_PPS_RPV1               ((nora_gpio_rp_t)130u)
#define NORA_PPS_RPV2               ((nora_gpio_rp_t)131u)
#define NORA_PPS_RPV3               ((nora_gpio_rp_t)132u)
#define NORA_PPS_RPV4               ((nora_gpio_rp_t)133u)
#define NORA_PPS_RPV5               ((nora_gpio_rp_t)134u)
/* RPV6..RPV15 have no dsPIC33CK counterpart -- see the silicon-count note above. */
#define NORA_PPS_RPV6               ((nora_gpio_rp_t)135u)
#define NORA_PPS_RPV7               ((nora_gpio_rp_t)136u)
#define NORA_PPS_RPV8               ((nora_gpio_rp_t)137u)
#define NORA_PPS_RPV9               ((nora_gpio_rp_t)138u)
#define NORA_PPS_RPV10              ((nora_gpio_rp_t)139u)
#define NORA_PPS_RPV11              ((nora_gpio_rp_t)140u)
#define NORA_PPS_RPV12              ((nora_gpio_rp_t)141u)
#define NORA_PPS_RPV13              ((nora_gpio_rp_t)142u)
#define NORA_PPS_RPV14              ((nora_gpio_rp_t)143u)
#define NORA_PPS_RPV15              ((nora_gpio_rp_t)144u)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PPS OUTPUT functions (a peripheral output driven onto an RP pin).
 *
 * This enum is the AK/CK source-compatibility union and is kept TEXTUALLY
 * IDENTICAL to the same enum in the dsPIC33CK NORA HAL, so a board or driver
 * that names a signal compiles on both families. A name may therefore be
 * accepted by this header even when the selected device has no matching PPS
 * output; in that case nora_pps_route_output() returns false and writes
 * nothing, which lets code report "not supported on this target" at run time
 * instead of needing a preprocessor split merely to compile.
 *
 * The implementation maps supported names to their _RPOUT_x values under
 * #ifdef, so the device header remains the authority for availability -- and
 * the two families' differing spellings for one function (see PWM_EVENT below)
 * are resolved in the backend, not here.
 */
typedef enum
{
    NORA_PPS_OUTPUT_U1TX,
    NORA_PPS_OUTPUT_U2TX,
    NORA_PPS_OUTPUT_U3TX,

    NORA_PPS_OUTPUT_SS1,
    NORA_PPS_OUTPUT_SCK1,
    NORA_PPS_OUTPUT_SDO1,

    NORA_PPS_OUTPUT_SS2,
    NORA_PPS_OUTPUT_SCK2,
    NORA_PPS_OUTPUT_SDO2,

    NORA_PPS_OUTPUT_SS3,
    NORA_PPS_OUTPUT_SCK3,
    NORA_PPS_OUTPUT_SDO3,

    NORA_PPS_OUTPUT_CLC1,
    NORA_PPS_OUTPUT_CLC2,
    NORA_PPS_OUTPUT_CLC3,
    NORA_PPS_OUTPUT_CLC4,

    NORA_PPS_OUTPUT_PWM4H,
    NORA_PPS_OUTPUT_PWM4L,

    /*
     * PWM EVENT outputs. The two families name this same signal differently in
     * silicon -- CK calls it PWMEA..PWMED (_RPOUT_PWMEA), AK calls it
     * PEVTA..PEVTD (_RPOUT_PEVTA) -- so neither spelling can be the neutral one
     * without leaking one family's vocabulary into a header shared by both. The
     * name here is the FUNCTION; each backend maps it to whatever its device
     * header calls it.
     */
    NORA_PPS_OUTPUT_PWM_EVENT_A,
    NORA_PPS_OUTPUT_PWM_EVENT_B,
    NORA_PPS_OUTPUT_PWM_EVENT_C,
    NORA_PPS_OUTPUT_PWM_EVENT_D,

    NORA_PPS_OUTPUT_CMP1,
    NORA_PPS_OUTPUT_CMP2,
    NORA_PPS_OUTPUT_CMP3,

    NORA_PPS_OUTPUT_REFO1,

    NORA_PPS_OUTPUT_CAN1TX,

    /*
     * The rest of the union: signals present in the OTHER family's silicon (or
     * on a larger part of either) for a peripheral family already named above.
     * Appended rather than interleaved so the values above do not move.
     *
     * The rule that decides membership: for every peripheral family this enum
     * already covers, carry every instance either family's device headers
     * define. It is deliberately not "every PPS-capable signal on both
     * families" -- OCM, SENT, QEI, BISS and PTGTRG are absent from the enum
     * entirely, and stay absent until something needs them.
     */
    NORA_PPS_OUTPUT_SS4,
    NORA_PPS_OUTPUT_SCK4,
    NORA_PPS_OUTPUT_SDO4,
    NORA_PPS_OUTPUT_PWM1H,
    NORA_PPS_OUTPUT_PWM2H,
    NORA_PPS_OUTPUT_PWM3H,
    NORA_PPS_OUTPUT_PWM5H,
    NORA_PPS_OUTPUT_PWM5L,
    NORA_PPS_OUTPUT_PWM6H,
    NORA_PPS_OUTPUT_PWM6L,
    NORA_PPS_OUTPUT_PWM7H,
    NORA_PPS_OUTPUT_PWM7L,
    NORA_PPS_OUTPUT_PWM8H,
    NORA_PPS_OUTPUT_PWM8L,

    /* CLC5..CLC10: dsPIC33AK512MPS512 has ten CLC output selects. */
    NORA_PPS_OUTPUT_CLC5,
    NORA_PPS_OUTPUT_CLC6,
    NORA_PPS_OUTPUT_CLC7,
    NORA_PPS_OUTPUT_CLC8,
    NORA_PPS_OUTPUT_CLC9,
    NORA_PPS_OUTPUT_CLC10,

    /* CMP4..CMP8: dsPIC33AK512MPS512 has eight comparator outputs. */
    NORA_PPS_OUTPUT_CMP4,
    NORA_PPS_OUTPUT_CMP5,
    NORA_PPS_OUTPUT_CMP6,
    NORA_PPS_OUTPUT_CMP7,
    NORA_PPS_OUTPUT_CMP8,

    /* Second reference clock output: both AK parts define _RPOUT_REFO2. */
    NORA_PPS_OUTPUT_REFO2,

    /* Second CAN: dsPIC33AK512MPS512 defines _RPOUT_CAN2TX. */
    NORA_PPS_OUTPUT_CAN2TX
} nora_pps_output_t;

/*
 * PPS INPUT functions (a peripheral input fed from an RP pin). Like the output
 * enum above, this is the AK/CK source-compatibility union and is kept
 * TEXTUALLY IDENTICAL to its dsPIC33CK counterpart: an unsupported target
 * signal is rejected by nora_pps_route_input() with false and no register
 * write. Each supported value maps to its RPINRx input-select register (a
 * _<sig>R bit-field alias; assignment takes the RP number directly).
 *
 * ICM1..ICM9 are the SCCP/MCCP Input Capture inputs (-> RPINR2..6). Route a pin
 * there to feed a CCP channel's Input Capture (see hal_ccp_input_capture).
 */
typedef enum
{
    NORA_PPS_INPUT_U1RX,
    NORA_PPS_INPUT_U2RX,
    NORA_PPS_INPUT_U3RX,

    NORA_PPS_INPUT_SS1,
    NORA_PPS_INPUT_SCK1,
    NORA_PPS_INPUT_SDI1,

    NORA_PPS_INPUT_SS2,
    NORA_PPS_INPUT_SCK2,
    NORA_PPS_INPUT_SDI2,

    NORA_PPS_INPUT_SS3,
    NORA_PPS_INPUT_SCK3,
    NORA_PPS_INPUT_SDI3,

    NORA_PPS_INPUT_CLCINA,
    NORA_PPS_INPUT_CLCINB,
    NORA_PPS_INPUT_CLCINC,
    NORA_PPS_INPUT_CLCIND,

    NORA_PPS_INPUT_INT1,
    NORA_PPS_INPUT_INT2,
    NORA_PPS_INPUT_INT3,

    NORA_PPS_INPUT_CAN1RX,

    /* The rest of the union -- same membership rule as the output enum above;
     * appended so the values before this point do not move. */
    NORA_PPS_INPUT_SS4,
    NORA_PPS_INPUT_SCK4,
    NORA_PPS_INPUT_SDI4,
    NORA_PPS_INPUT_REFI1,
    NORA_PPS_INPUT_ICM1,
    NORA_PPS_INPUT_ICM2,
    NORA_PPS_INPUT_ICM3,
    NORA_PPS_INPUT_ICM4,
    NORA_PPS_INPUT_ICM5,
    NORA_PPS_INPUT_ICM6,
    NORA_PPS_INPUT_ICM7,
    NORA_PPS_INPUT_ICM8,
    NORA_PPS_INPUT_ICM9,

    /* Fourth external interrupt: both AK parts define _INT4R; no CK part does. */
    NORA_PPS_INPUT_INT4,

    /* CLCINE..CLCINJ: dsPIC33AK512MPS512 has ten CLC input selects. */
    NORA_PPS_INPUT_CLCINE,
    NORA_PPS_INPUT_CLCINF,
    NORA_PPS_INPUT_CLCING,
    NORA_PPS_INPUT_CLCINH,
    NORA_PPS_INPUT_CLCINI,
    NORA_PPS_INPUT_CLCINJ,

    /* Second reference clock input: both AK parts define _REFI2R. No CK part
     * defines even _REFI1R, so both REFI values are AK-side here. */
    NORA_PPS_INPUT_REFI2,

    /* Second CAN: dsPIC33AK512MPS512 defines _CAN2RXR. */
    NORA_PPS_INPUT_CAN2RX
} nora_pps_input_t;

/*
 * PPS lock gate (RPCON.IOLOCK). unlock() makes the PPS map writable, lock()
 * protects it. route_*() below bracket their own writes; expose these only for
 * direct-SFR PPS writers or a single window around a batch.
 */
void nora_pps_unlock(void);   /* IOLOCK = 0 : PPS registers writable  */
void nora_pps_lock(void);     /* IOLOCK = 1 : PPS registers protected */

/*
 * Route a peripheral OUTPUT onto an RP pin (writes the RP's _RPnnR with the
 * peripheral's output function code). Self-brackets IOLOCK. Returns false if the
 * peripheral output is not available on this device, or the RP pin has no output
 * PPS register here (range/encoding contract only -- not a board-bonding check;
 * configure the GPIO output first via nora_gpio_rp_config_digital_output()).
 *
 * rp may be a VIRTUAL pin (NORA_PPS_RPV0..15): routing an output to one sends the
 * signal on-chip only, and there is no GPIO configuration step to do first.
 */
bool nora_pps_route_output(nora_pps_output_t output, nora_gpio_rp_t rp);

/*
 * Find the first PHYSICAL RP pin currently carrying a peripheral OUTPUT.
 * Returns false when rp is NULL, the output is unavailable on this device, or
 * no physical pin has that output route. Virtual RPV outputs are not searched:
 * this GPIO-typed API reports only board-visible pins. Read-only; does not
 * change IOLOCK.
 */
bool nora_pps_find_output_rp(nora_pps_output_t output, nora_gpio_rp_t *rp);

/*
 * Route a peripheral INPUT to read from an RP pin (writes the peripheral's RPINRx
 * input-select with the RP number). Self-brackets IOLOCK. Returns false if the
 * peripheral input is not available on this device, OR if rp is neither a physical
 * remappable pin nor a virtual one on this device (rejected before any register
 * write). For a physical pin, configure the GPIO input first via
 * nora_gpio_rp_config_digital_input(); for a virtual pin there is nothing to
 * configure -- the source is whatever output was routed onto it.
 */
bool nora_pps_route_input(nora_pps_input_t input, nora_gpio_rp_t rp);

/*
 * Combined "pinmux" helpers: digital-configure the RP pin AND route the PPS signal in one call.
 * These wrap the two-step sequence every PPS user must perform -- first make the pin a digital
 * input/output (nora_gpio_rp_config_digital_input/output), then route the peripheral signal
 * (nora_pps_route_input/output) -- so the pairing and its glitch-aware order are guaranteed
 * in one place. The GPIO configuration is applied FIRST (for an output, the initial level is
 * seeded before the driver is enabled -- see nora_gpio_rp_config_digital_output). Returns
 * false and routes nothing further if the GPIO configuration fails; otherwise returns the result
 * of the PPS route. No register access is duplicated here -- both existing low-level APIs are
 * reused as-is.
 *
 * Virtual RPV endpoints are rejected here, and that is the point of the split: they are
 * PPS-only and cannot be configured as GPIO, so the GPIO step fails and nothing is routed.
 * nora_pps_route_*() remains the API for virtual routing.
 */
bool nora_pinmux_route_input(nora_pps_input_t function, nora_gpio_rp_t rp);
bool nora_pinmux_route_output(nora_pps_output_t function, nora_gpio_rp_t rp,
                                   bool initial_high);

#ifdef __cplusplus
}
#endif

#endif /* NORA_PPS_H */
