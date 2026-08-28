#ifndef NORA_GPIO_H
#define NORA_GPIO_H

/*
 * nora_gpio.h
 * ----------------
 * NORA GPIO HAL public API.
 *
 * Portability scope:
 *   This interface minimizes application changes between NORA-supported
 *   dsPIC33AK and dsPIC33CK ports. It is not a universal, arbitrary-processor
 *   GPIO HAL: electrical capabilities, available ports, RP numbering, and the
 *   packed-pin representation remain properties of the selected NORA port and
 *   board. Keep physical identifiers in the board pin map rather than
 *   scattering them through feature code.
 *
 * API orientation (two layers, both declared in this header):
 *   1. RP-first API (PREFERRED for normal NORA board / application code) --
 *      address a pin by its Remappable-Pin number, the same RPn used by the PPS
 *      map (see the RP-first section near the bottom), e.g.:
 *          nora_gpio_rp_config_digital_output(101u, false); // RP101
 *          nora_gpio_rp_set(101u);
 *   2. Packed-pin core API -- the core HAL and the fixed-function interface.
 *      The RP API is a thin adapter over it. Use the packed-pin API for
 *      non-RP pins, when a packed handle is more convenient, or for HAL
 *      internals.
 *
 * Scope (intentionally small):
 *   - per-pin direction (input / output)
 *   - per-pin pull-up / pull-down / none
 *   - per-pin analog / digital select (ANSEL)
 *   - per-pin open-drain enable
 *   - simple one-call digital input / output configuration
 *   - output write / set / clear / toggle; input read (3-state level result)
 *
 * Out of scope:
 *   - change-notification (CN) / edge interrupts
 *   - PPS (peripheral pin select) routing          (separate concern)
 *
 * Packed pin addressing:
 *   A pin is a single packed number: (port << 4) | bit. Do NOT write the raw
 *   number; build it with NORA_GPIO_PIN(port, bit). Prefer an RP number
 *   (RP-first API) when the pin has one; use a packed pin for non-RP pins.
 *
 * Interrupt safety:
 *   The accessors are plain read-modify-write and do NOT disable interrupts. If
 *   the same LAT port is updated from both main-line code and an ISR, the caller
 *   must provide the mutual exclusion.
 *
 * The register table adapts to the device automatically: a port row is emitted
 * only when the device header defines that port's SFRs.
 *
 * Limitations (all layers): this HAL configures the GPIO electrical attributes
 * only. It does NOT validate (a) that the device implements the pin, (b) that the
 * selected package bonds it out, or (c) that the board exposes it -- those are
 * device / package / board concerns.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Port codes. Values are the high nibble of a packed pin number. */
typedef enum
{
    NORA_GPIO_PORT_A = 0,
    NORA_GPIO_PORT_B = 1,
    NORA_GPIO_PORT_C = 2,
    NORA_GPIO_PORT_D = 3,
    NORA_GPIO_PORT_E = 4,
    NORA_GPIO_PORT_F = 5,
    NORA_GPIO_PORT_G = 6,
    NORA_GPIO_PORT_H = 7
} nora_gpio_port_t;

/* Packed pin handle: (port << 4) | bit. Build only via NORA_GPIO_PIN(). */
typedef uint16_t nora_gpio_pin_t;

/*
 * NORA_GPIO_PIN(port, bit)
 * - port : a NORA_GPIO_PORT_* code
 * - bit  : 0..15
 * bit must be 0..15. Values outside this range are masked by the macro and
 * should not be used.
 * Use this (and a board-level name) instead of a raw pin number.
 */
#define NORA_GPIO_PIN(port, bit) \
    ((nora_gpio_pin_t)((((uint16_t)(port)) << 4) | ((uint16_t)(bit) & 0x0Fu)))

/*
 * Extract the port code / bit from a packed pin. Inverse of NORA_GPIO_PIN().
 *   NORA_GPIO_PIN_PORT(pin) -> NORA_GPIO_PORT_* value (0..7)
 *   NORA_GPIO_PIN_BIT(pin)  -> 0..15
 */
#define NORA_GPIO_PIN_PORT(pin) ((uint8_t)(((uint16_t)(pin) >> 4) & 0x07u))
#define NORA_GPIO_PIN_BIT(pin)  ((uint8_t)((uint16_t)(pin) & 0x0Fu))

typedef enum
{
    NORA_GPIO_DIR_INPUT  = 0,
    NORA_GPIO_DIR_OUTPUT = 1
} nora_gpio_dir_t;

typedef enum
{
    NORA_GPIO_PULL_NONE = 0,
    NORA_GPIO_PULL_UP,
    NORA_GPIO_PULL_DOWN
} nora_gpio_pull_t;

/*
 * Optional one-shot configuration. Applied in a transition-aware order:
 *   input : TRIS=input -> analog/digital -> pull -> open-drain
 *   output: analog/digital -> pull -> open-drain -> initial output level
 *           -> TRIS=output
 *
 * Therefore a live output stops driving before its input attributes change,
 * while a new output receives its desired LAT value before its driver enables.
 */
typedef struct
{
    nora_gpio_dir_t  dir;          /* input or output                    */
    nora_gpio_pull_t pull;         /* none / up / down                   */
    bool                  analog;       /* true = analog (ANSEL=1)            */
    bool                  open_drain;   /* true = open-drain output           */
    bool                  initial_high; /* output only: level set before dir  */
} nora_gpio_config_t;

/*
 * Pin level result for the read functions. A small SIGNED type so a read can
 * report an error distinctly from a valid Low/High -- the result is NOT a plain
 * bool. Callers must check for the error value before treating it as a level.
 */
typedef int8_t nora_gpio_level_t;

#define NORA_GPIO_LEVEL_ERROR ((nora_gpio_level_t)-1)   /* invalid / unavailable GPIO */
#define NORA_GPIO_LEVEL_LOW   ((nora_gpio_level_t)0)    /* pin / latch is Low         */
#define NORA_GPIO_LEVEL_HIGH  ((nora_gpio_level_t)1)    /* pin / latch is High        */

/*
 * All functions take a packed pin from NORA_GPIO_PIN(). Setter / config
 * functions return bool (false if the pin's port is not present; otherwise true).
 * The read functions return a 3-state nora_gpio_level_t (see below), NOT a
 * bool -- NORA_GPIO_LEVEL_ERROR for a pin whose port is not present.
 *
 * Return-value contract:
 *   - The setter / config functions return false only when the port is not
 *     present in the selected device header, or (for nora_gpio_config())
 *     when config is NULL.
 *   - false does NOT indicate an electrical pin fault; it means "this device
 *     header has no register for that port".
 *   - This HAL does not check whether a given package actually exposes the pin.
 *     Package-level and board-level pin validity are the board layer's
 *     responsibility.
 */
bool nora_gpio_set_direction(nora_gpio_pin_t pin, nora_gpio_dir_t dir);
bool nora_gpio_set_pull(nora_gpio_pin_t pin, nora_gpio_pull_t pull);
bool nora_gpio_set_analog(nora_gpio_pin_t pin, bool analog);
bool nora_gpio_set_open_drain(nora_gpio_pin_t pin, bool enable);
bool nora_gpio_config(nora_gpio_pin_t pin, const nora_gpio_config_t *config);

/*
 * Simple one-call configuration for the common cases, so callers do not build a
 * config struct. Both go through nora_gpio_config() (no duplicated register
 * logic).
 *   digital_input : direction=input,  analog=off, pull=none, open-drain=off.
 *   digital_output: direction=output, analog=off, pull=none, open-drain=off,
 *                   LAT seeded to initial_high BEFORE the pin is driven (the
 *                   glitch-aware order: attributes -> LAT -> TRIS=output).
 * Return false only when the pin's port is not present on the device.
 */
bool nora_gpio_config_digital_input(nora_gpio_pin_t pin);
bool nora_gpio_config_digital_output(nora_gpio_pin_t pin, bool initial_high);

bool nora_gpio_write(nora_gpio_pin_t pin, bool high);
bool nora_gpio_set(nora_gpio_pin_t pin);
bool nora_gpio_clear(nora_gpio_pin_t pin);
bool nora_gpio_toggle(nora_gpio_pin_t pin);

/*
 * nora_gpio_read       : read the pin's actual level   (PORT register)
 * nora_gpio_read_output: read the driven output latch  (LAT register)
 * read() reflects the physical pin; read_output() reflects the last value
 * written, regardless of the pin's electrical state.
 *
 * Return a 3-state nora_gpio_level_t -- NOT a bool:
 *   NORA_GPIO_LEVEL_ERROR (-1) : port not present / invalid pin
 *   NORA_GPIO_LEVEL_LOW   ( 0) : Low
 *   NORA_GPIO_LEVEL_HIGH  ( 1) : High
 * Do NOT use the result directly as a boolean (ERROR is non-zero, i.e. truthy);
 * compare against the named constants and handle ERROR first.
 */
nora_gpio_level_t nora_gpio_read(nora_gpio_pin_t pin);
nora_gpio_level_t nora_gpio_read_output(nora_gpio_pin_t pin);


/*===========================================================================
 * RP-first API -- address a pin by its Remappable-Pin number (the same RPn
 * the PPS map uses), rather than by a packed (port, bit) pair.
 *
 * The dsPIC33AK backend encodes RPn as packed_pin + 1 = port*16 + bit + 1,
 * with 0 meaning "no RP".  The public nora_gpio_rp_t contract is a processor's
 * physical remappable-pin identifier, not this encoding; portable application
 * code must obtain its values from the board layer.
 * The functions below are thin wrappers over the packed-pin API (convert RPn
 * -> packed, then call it) and share its return contract. This is the
 * ENCODING/range layer only -- it does NOT check package bonding or board
 * availability. Package-level pin availability may be validated separately
 * using device metadata such as Microchip ATDF files.
 *
 * Coverage: the RP-first API provides the same GPIO operations as the
 * packed-pin API: full one-shot configuration (nora_gpio_rp_config),
 * individual attribute setters (pull, analog, open-drain), simple digital
 * input/output shortcuts, output write/set/clear/toggle, and 3-state read.
 * Use packed-pin API only for non-RP pins or when a packed pin handle is
 * more convenient.
 *===========================================================================*/

/* Remappable-pin number. RPn is 1-based; 0 means "no RP". */
typedef uint8_t nora_gpio_rp_t;

/*
 * The remappable band, as a pair. Both ends are stated even though AK's low end
 * is the trivial RP1, because the PAIR is what portable code needs: a range
 * check or a sweep written as MIN..MAX compiles on both families, where one
 * written as 1..MAX silently means "every RP" on AK and "wrong" on CK, whose
 * band starts at RP32 (RB0 -- ports A and E have no RPn at all).
 */
#define NORA_GPIO_RP_MIN  (1u)     /* RP1 = RA0 (RPn = packed_pin + 1), contiguous from 1 */
/* 8 ports x 16 = 128 encodable RP numbers. */
#define NORA_GPIO_RP_MAX  (128u)

/*
 * Validated RPn <-> packed-pin conversion. Return false (leaving *out untouched)
 * for a NULL pointer or an out-of-range argument: rp == 0 / rp > RP_MAX for
 * pin_from_rp; pin >= RP_MAX for rp_from_pin. Range only (see note above).
 */
bool nora_gpio_pin_from_rp(nora_gpio_rp_t rp, nora_gpio_pin_t *pin);
bool nora_gpio_rp_from_pin(nora_gpio_pin_t pin, nora_gpio_rp_t *rp);

/*
 * Full one-shot configuration via a config struct (thin wrapper over
 * nora_gpio_config). Returns false on RP conversion failure, NULL config,
 * or the underlying GPIO failure.
 */
bool nora_gpio_rp_config(nora_gpio_rp_t rp,
                               const nora_gpio_config_t *config);

/* Individual attribute setters (thin wrappers over the packed-pin setters). */
bool nora_gpio_rp_set_direction(nora_gpio_rp_t rp, nora_gpio_dir_t dir);
bool nora_gpio_rp_set_pull(nora_gpio_rp_t rp, nora_gpio_pull_t pull);
bool nora_gpio_rp_set_analog(nora_gpio_rp_t rp, bool analog);
bool nora_gpio_rp_set_open_drain(nora_gpio_rp_t rp, bool enable);

/*
 * Simple digital input/output shortcuts. config_digital_input sets direction
 * input, analog off, no pull, no open-drain. config_digital_output sets
 * direction output, analog off, no pull, no open-drain, and seeds LAT to
 * initial_high before enabling the output driver (glitch-free).
 * Return false on RP conversion failure or the underlying GPIO failure.
 */
bool nora_gpio_rp_config_digital_input(nora_gpio_rp_t rp);
bool nora_gpio_rp_config_digital_output(nora_gpio_rp_t rp, bool initial_high);

bool nora_gpio_rp_set(nora_gpio_rp_t rp);
bool nora_gpio_rp_clear(nora_gpio_rp_t rp);
bool nora_gpio_rp_toggle(nora_gpio_rp_t rp);
bool nora_gpio_rp_write(nora_gpio_rp_t rp, bool high);

nora_gpio_level_t nora_gpio_rp_read(nora_gpio_rp_t rp);
nora_gpio_level_t nora_gpio_rp_read_output(nora_gpio_rp_t rp);

#ifdef __cplusplus
}
#endif

#endif /* NORA_GPIO_H */
